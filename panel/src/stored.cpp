// "Saved data": the eye logs' count and delete, and following eyecam-rec's.
#include "stored.h"

#include "eyecam.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace stored {

namespace {

/** One file of an eye log, as found. */
struct Found {
    std::string path;
    long long bytes = 0;
    std::time_t mtime = 0;
};

/** The eye logs in a folder: each recording's files, by its stem. */
using Logs = std::map<std::string, std::vector<Found>>;

/**
 * Whether a name follows a pattern of digits ('9') and fixed characters.
 * @param name the name
 * @param pattern e.g. "eyes_9999-99-99_99-99-99"
 * @return true if it does
 */
bool matches(const std::string& name, const char* pattern) {
    const size_t n = std::strlen(pattern);
    if (name.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        const bool ok = pattern[i] == '9' ? (name[i] >= '0' && name[i] <= '9') : name[i] == pattern[i];
        if (!ok) return false;
    }
    return true;
}

/**
 * The eye logs' regular files in a folder (links and anything else are never one).
 * @param dir the folder
 * @return them, by recording
 */
Logs listLogs(const std::string& dir) {
    Logs logs;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return logs;
    while (const dirent* entry = ::readdir(d)) {
        const std::string name = entry->d_name;
        const std::string stem = eyeLogStem(name);
        if (stem.empty()) continue;
        const std::string path = dir + "/" + name;
        struct stat info {};
        if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
        logs[stem].push_back({path, static_cast<long long>(info.st_size), info.st_mtime});
    }
    ::closedir(d);
    return logs;
}

/**
 * Whether a recording is kept: the one being made now, or one written to within kBusySec.
 * @param dir the folder
 * @param stem the recording
 * @param files its files
 * @param keepBase the one being made now ("" = none)
 * @param now the time
 * @return true to keep it
 */
bool kept(const std::string& dir, const std::string& stem, const std::vector<Found>& files,
          const std::string& keepBase, std::time_t now) {
    if (!keepBase.empty() && dir + "/" + stem == keepBase) return true;
    for (const Found& f : files) {
        if (std::fabs(std::difftime(now, f.mtime)) < kBusySec) return true;
    }
    return false;
}

}  // namespace

const char* eyecamName(Kind kind) {
    switch (kind) {
        case Kind::EyecamRecordings: return "rec";
        case Kind::CalibImages: return "calib_images";
        case Kind::EyeLogs: break;
    }
    return "";
}

std::string eyeLogStem(const std::string& name) {
    std::string base;
    for (const char* extension : {".config.json", ".csv", ".log"}) {
        const size_t n = std::strlen(extension);
        if (name.size() > n && name.compare(name.size() - n, n, extension) == 0) {
            base = name.substr(0, name.size() - n);
            break;
        }
    }
    if (base.size() < 24 || !matches(base.substr(0, 24), "eyes_9999-99-99_99-99-99")) return "";
    // (uniqueBase's _2, _3...)
    if (base.size() > 24) {
        const std::string rest = base.substr(24);
        if (rest.size() < 2 || rest[0] != '_' || rest.find_first_not_of("0123456789", 1) != std::string::npos) return "";
    }
    return base;
}

Usage eyeLogUsage(const std::string& dir, const std::string& keepBase, std::time_t now) {
    Usage usage;
    for (const auto& [stem, files] : listLogs(dir)) {
        if (kept(dir, stem, files, keepBase, now)) continue;
        ++usage.count;
        for (const Found& f : files) usage.bytes += f.bytes;
    }
    return usage;
}

Usage deleteEyeLogs(const std::string& dir, const std::string& keepBase, std::time_t now, std::string& error) {
    error.clear();
    Usage gone;
    for (const auto& [stem, files] : listLogs(dir)) {
        if (kept(dir, stem, files, keepBase, now)) continue;
        bool any = false;
        for (const Found& f : files) {
            if (::unlink(f.path.c_str()) == 0) {
                any = true;
                gone.bytes += f.bytes;
            } else if (error.empty()) {
                error = f.path + ": " + std::strerror(errno);
            }
        }
        if (any) ++gone.count;
    }
    return gone;
}

bool followEyecam(View& view, const eyecam::Status& status, double nowUnix) {
    bool changed = false;
    const eyecam::Stored& s = status.stored;
    for (const Kind kind : {Kind::EyecamRecordings, Kind::CalibImages}) {
        Delete& d = view.of(kind);
        const std::string name = eyecamName(kind);
        const bool underWay = s.present && s.deleting == name;
        const bool ended = s.present && s.hasDeleted && s.deletedKind == name && s.deletedUnix >= d.sentUnix - 1.0;
        if (d.running && !underWay && d.sentUnix > 0 && ended) {
            d.running = false;
            d.done = true;
            d.ok = s.deletedError.empty();
            d.usage = {static_cast<int>(s.deletedCount), s.deletedBytes};
            d.error = s.deletedError;
            changed = true;
        } else if (d.running && !underWay && nowUnix - d.sentUnix >= kEyecamDoneSec) {
            d.running = false;
            d.done = true;
            d.ok = false;
            d.usage = Usage();
            d.error = "eyecam-rec didn't say it was done";
            changed = true;
        } else if (!d.running && underWay) {
            // (one asked for elsewhere: shown under way, and done when it shows done)
            d.running = true;
            d.sentUnix = nowUnix;
            changed = true;
        }
    }
    return changed;
}

EyeLogs::~EyeLogs() {
    if (thread_.joinable()) thread_.join();
}

void EyeLogs::reap() {
    if (thread_.joinable()) thread_.join();
}

bool EyeLogs::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ || pending_;
}

bool EyeLogs::count(const std::string& dir, const std::string& keepBase) {
    if (busy()) return false;
    reap();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
    }
    thread_ = std::thread([this, dir, keepBase]() {
        const Usage usage = eyeLogUsage(dir, keepBase, std::time(nullptr));
        std::lock_guard<std::mutex> lock(mutex_);
        usage_ = usage;
        counted_ = true;
        running_ = false;
    });
    return true;
}

bool EyeLogs::remove(const std::string& dir, const std::string& keepBase) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deleting_ || pending_) return false;
        if (running_) {
            // (after the count: take starts it)
            pending_ = true;
            pendingDir_ = dir;
            pendingKeep_ = keepBase;
            return true;
        }
    }
    startRemove(dir, keepBase);
    return true;
}

void EyeLogs::startRemove(const std::string& dir, const std::string& keepBase) {
    reap();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
        deleting_ = true;
    }
    thread_ = std::thread([this, dir, keepBase]() {
        std::string error;
        const Usage gone = deleteEyeLogs(dir, keepBase, std::time(nullptr), error);
        const Usage left = eyeLogUsage(dir, keepBase, std::time(nullptr));
        std::fprintf(stderr, "[data] deleted %d eye log(s), %.1f MB, from %s%s%s\n", gone.count, gone.bytes / 1e6,
                     dir.c_str(), error.empty() ? "" : "; not all: ", error.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        gone_ = gone;
        error_ = error;
        deleted_ = true;
        usage_ = left;
        counted_ = true;
        running_ = false;
        deleting_ = false;
    });
}

bool EyeLogs::take(View& view) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (pending_ && !running_) {
            pending_ = false;
            const std::string dir = pendingDir_;
            const std::string keep = pendingKeep_;
            lock.unlock();
            startRemove(dir, keep);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;
    if (deleted_) {
        deleted_ = false;
        Delete& d = view.of(Kind::EyeLogs);
        d.running = false;
        d.done = true;
        d.ok = error_.empty();
        d.usage = gone_;
        d.error = error_;
        changed = true;
    }
    if (counted_) {
        counted_ = false;
        changed |= !view.eyeLogsCounted || usage_.count != view.eyeLogs.count || usage_.bytes != view.eyeLogs.bytes;
        view.eyeLogsCounted = true;
        view.eyeLogs = usage_;
    }
    return changed;
}

}  // namespace stored
