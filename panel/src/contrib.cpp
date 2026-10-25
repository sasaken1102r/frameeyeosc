// "Help train the AI": the contribution's progress (info.txt), which step comes next and whether it fits, following its
// recordings and the re-wear between them in eyecam-rec's status, moving the recordings into its folder, and packing
// or deleting it on a thread of its own.
#include "contrib.h"

#include "file_io.h"
#include "recorder.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace contrib {

namespace {

/** Bytes read from a recording's file at a time while packing. */
constexpr size_t kChunk = 1 << 20;
/** The packing thread's and zstd's niceness (the panel and VR come first). */
constexpr int kNice = 10;

/**
 * Whether text from a position is "YYYY-MM-DD_HH-MM-SS", and nothing after it.
 * @param text the text
 * @param at where it starts
 * @return true if it is
 */
bool stampAt(const std::string& text, size_t at) {
    static const char* const shape = "dddd-dd-dd_dd-dd-dd";
    if (text.size() != at + 19) return false;
    for (size_t i = 0; i < 19; ++i) {
        const char c = text[at + i];
        if (shape[i] == 'd' ? !std::isdigit(static_cast<unsigned char>(c)) : c != shape[i]) return false;
    }
    return true;
}

/**
 * The names in a folder.
 * @param dir the folder
 * @return them ("." and ".." left out), sorted
 */
std::vector<std::string> names(const std::string& dir) {
    std::vector<std::string> found;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return found;
    while (const dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") found.push_back(name);
    }
    ::closedir(d);
    std::sort(found.begin(), found.end());
    return found;
}

/**
 * Whether a path is a folder itself (not a link to one).
 * @param path the path
 * @return true if it is
 */
bool isRealDir(const std::string& path) {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * The last part of a path.
 * @param path the path (without a trailing "/")
 * @return its name
 */
std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/**
 * The folder a path is in.
 * @param path the path (without a trailing "/")
 * @return the folder ("" for a bare name)
 */
std::string parentOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

/**
 * A whole number from info.txt.
 * @param text the value
 * @param fallback for anything else
 * @return the number
 */
long long readInteger(const std::string& text, long long fallback) {
    if (text.empty()) return fallback;
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    return errno == 0 && end != nullptr && *end == '\0' ? value : fallback;
}

/**
 * Write all of a buffer, to a file or (without SIGPIPE) to a socket.
 * @param fd where
 * @param data the bytes
 * @param size how many
 * @param socket fd is a socket
 * @return false on an error (errno says it)
 */
bool writeAll(int fd, const char* data, size_t size, bool socket) {
    size_t done = 0;
    while (done < size) {
        const ssize_t n = socket ? ::send(fd, data + done, size - done, MSG_NOSIGNAL)
                                 : ::write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

/**
 * Put an octal number into a tar header field: digits, then a NUL.
 * @param field the field
 * @param width its width
 * @param value the number (fits in width - 1 octal digits)
 */
void putOctal(char* field, size_t width, unsigned long long value) {
    field[width - 1] = '\0';
    for (size_t i = width - 1; i > 0; --i) {
        field[i - 1] = static_cast<char>('0' + (value & 7));
        value >>= 3;
    }
}

}  // namespace

const char* protocolOf(int step) {
    return isFree(step) ? kFreeProtocol : kGuidedProtocol;
}

bool isFree(int step) {
    return step >= 3;
}

bool needsRewear(int step) {
    return step > 0;
}

long long estimateBytes(int step, double fps) {
    const double rate = std::isfinite(fps) && fps > 0 ? fps : kDefaultFps;
    const double seconds = (isFree(step) ? kFreeSec : kGuidedSec) + kTailSec;
    return static_cast<long long>(seconds * 2 * rate * kFrameBytes);
}

int nextStep(const Info& info, bool extra) {
    for (int step = 0; step < kRequiredSteps; ++step) {
        if (info.steps[step].rec.empty()) return step;
    }
    return extra && info.steps[kRequiredSteps].rec.empty() ? kRequiredSteps : kStepCount;
}

int requiredDone(const Info& info) {
    int done = 0;
    for (int step = 0; step < kRequiredSteps; ++step) done += info.steps[step].rec.empty() ? 0 : 1;
    return done;
}

long long recordedBytes(const Info& info) {
    long long total = 0;
    for (const Step& step : info.steps) total += step.rec.empty() ? 0 : step.bytes;
    return total;
}

long long neededBytes(const Info& info, double fps) {
    long long total = kMarginBytes;
    for (int step = 0; step < kRequiredSteps; ++step) {
        if (info.steps[step].rec.empty()) total += estimateBytes(step, fps);
    }
    return total;
}

bool fits(int step, long long freeBytes, double fps) {
    return freeBytes < 0 || freeBytes >= estimateBytes(step, fps) + kMarginBytes;
}

long long packNeedBytes(const Info& info, bool compressed) {
    const double share = compressed ? kPackShare : 1.0;
    return static_cast<long long>(recordedBytes(info) * share) + kMarginBytes;
}

std::string formatInfo(const Info& info) {
    std::ostringstream out;
    out << "# frameeyeosc: eye-camera recordings for the half-shut check (AI), made with the panel's \"Help train the "
           "AI\".\n"
           "# Each step's recording is its folder here, as eyecam-rec writes it (cues.csv, valve.csv, frames.csv, "
           "meta.txt,\n"
           "# eye_L.raw, eye_R.raw). Steps 1-3 follow the protocol without light, 4 (and the optional 5) are free "
           "use, each in\n"
           "# a wear of its own (wear: first / detected = seen off and on again / button = the user said so). "
           "aborted/ holds\n"
           "# tries that didn't finish (never packed). No names or other personal data are kept.\n";
    out << "format=1\n";
    out << "app_version=" << info.appVersion << "\n";
    out << "consent=" << info.consent << "\n";
    for (int i = 0; i < kStepCount; ++i) {
        const Step& step = info.steps[i];
        const std::string key = "step" + std::to_string(i + 1) + "_";
        out << key << "protocol=" << (step.protocol.empty() ? protocolOf(i) : step.protocol.c_str()) << "\n";
        if (step.rec.empty()) continue;
        out << key << "rec=" << step.rec << "\n";
        out << key << "bytes=" << step.bytes << "\n";
        out << key << "wear=" << step.wear << "\n";
        out << key << "done=" << step.done << "\n";
    }
    if (!info.nextWear.empty()) out << "next_wear=" << info.nextWear << "\n";
    if (info.running >= 0) {
        char since[64];
        std::snprintf(since, sizeof(since), "%.3f", info.runningSince);
        out << "running=" << info.running + 1 << "\n";
        out << "running_since=" << since << "\n";
    }
    for (const std::string& aborted : info.aborted) out << "aborted=" << aborted << "\n";
    if (!info.packed.empty()) {
        out << "packed=" << info.packed << "\n";
        out << "packed_bytes=" << info.packedBytes << "\n";
    }
    return out.str();
}

bool parseInfo(const std::string& text, Info& info) {
    info = Info();
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "app_version") {
            info.appVersion = value;
        } else if (key == "consent") {
            info.consent = value;
        } else if (key == "next_wear") {
            info.nextWear = value;
        } else if (key == "running") {
            const long long step = readInteger(value, 0);
            info.running = step >= 1 && step <= kStepCount ? static_cast<int>(step - 1) : -1;
        } else if (key == "running_since") {
            info.runningSince = std::strtod(value.c_str(), nullptr);
        } else if (key == "aborted") {
            info.aborted.push_back(value);
        } else if (key == "packed") {
            info.packed = value;
        } else if (key == "packed_bytes") {
            info.packedBytes = readInteger(value, 0);
        } else if (key.size() > 6 && key.compare(0, 4, "step") == 0 && key[5] == '_' && key[4] >= '1' &&
                   key[4] < '1' + kStepCount) {
            Step& step = info.steps[key[4] - '1'];
            const std::string field = key.substr(6);
            if (field == "protocol") step.protocol = value;
            if (field == "rec") step.rec = value;
            if (field == "bytes") step.bytes = readInteger(value, 0);
            if (field == "wear") step.wear = value;
            if (field == "done") step.done = value;
        }
    }
    // A recording's name that isn't one (written by hand) doesn't count: that step is done again
    for (Step& step : info.steps) {
        const std::string name = step.rec;
        if (!name.empty() && !isRecordingName(name)) step.rec.clear();
    }
    return !info.consent.empty();
}

bool isFolderName(const std::string& name) {
    return name.compare(0, 8, "contrib_") == 0 && stampAt(name, 8);
}

bool isRecordingName(const std::string& name) {
    return name.compare(0, 4, "rec_") == 0 && stampAt(name, 4);
}

std::string folderName(std::time_t when) {
    return "contrib_" + recorder::fileStamp(when);
}

std::string localTime(std::time_t when, bool zone) {
    std::tm local {};
    localtime_r(&when, &local);
    char text[64];
    std::strftime(text, sizeof(text), zone ? "%Y-%m-%d %H:%M:%S %z" : "%Y-%m-%d %H:%M:%S", &local);
    return text;
}

std::string defaultHome() {
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : "") + "/eyecam";
}

std::string findCurrent(const std::string& home) {
    std::vector<std::string> found = names(home);
    for (auto name = found.rbegin(); name != found.rend(); ++name) {
        if (!isFolderName(*name) || !isRealDir(home + "/" + *name)) continue;
        Info info;
        std::string error;
        if (readInfo(home + "/" + *name, info, error)) return home + "/" + *name;
    }
    return "";
}

bool readInfo(const std::string& dir, Info& info, std::string& error) {
    std::string text;
    const std::string path = dir + "/" + kInfoFile;
    if (!file_io::read(path, text, 64 * 1024)) {
        error = "can't read " + recorder::shortPath(path) + ": " + std::strerror(errno);
        return false;
    }
    if (!parseInfo(text, info)) {
        error = recorder::shortPath(path) + " has no consent";
        return false;
    }
    return true;
}

bool writeInfo(const std::string& dir, const Info& info, std::string& error) {
    const std::string path = dir + "/" + kInfoFile;
    const std::string temp = path + ".tmp";
    const std::string text = formatInfo(info);
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "can't write " + recorder::shortPath(temp) + ": " + std::strerror(errno);
        return false;
    }
    // (on the disk before the rename, so a crash never leaves an empty one)
    if (!writeAll(fd, text.data(), text.size(), false) || ::fsync(fd) != 0) {
        error = "can't write " + recorder::shortPath(temp) + ": " + std::strerror(errno);
        ::close(fd);
        ::unlink(temp.c_str());
        return false;
    }
    ::close(fd);
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        error = "can't replace " + recorder::shortPath(path) + ": " + std::strerror(errno);
        ::unlink(temp.c_str());
        return false;
    }
    return true;
}

bool create(const std::string& home, const std::string& version, std::time_t now, std::string& dir, Info& info,
            std::string& error) {
    if (::mkdir(home.c_str(), 0755) != 0 && errno != EEXIST) {
        error = "can't create " + recorder::shortPath(home) + ": " + std::strerror(errno);
        return false;
    }
    // (a second one in the same second gets the next second's name)
    std::string path;
    for (int later = 0;; ++later) {
        path = home + "/" + folderName(now + later);
        if (::mkdir(path.c_str(), 0755) == 0) break;
        if (errno != EEXIST || later >= 60) {
            error = "can't create " + recorder::shortPath(path) + ": " + std::strerror(errno);
            return false;
        }
    }
    Info made;
    made.appVersion = version;
    made.consent = localTime(now, true);
    for (int step = 0; step < kStepCount; ++step) made.steps[step].protocol = protocolOf(step);
    if (!writeInfo(path, made, error)) {
        ::rmdir(path.c_str());
        return false;
    }
    dir = path;
    info = made;
    return true;
}

long long freeBytes(const std::string& path) {
    struct statvfs st {};
    if (::statvfs(path.c_str(), &st) != 0) return -1;
    return static_cast<long long>(st.f_bavail) * static_cast<long long>(st.f_frsize);
}

long long folderBytes(const std::string& path) {
    long long total = 0;
    for (const std::string& name : names(path)) {
        const std::string child = path + "/" + name;
        struct stat st {};
        if (::lstat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            total += folderBytes(child);
        } else if (S_ISREG(st.st_mode)) {
            total += st.st_size;
        }
    }
    return total;
}

bool takeRecording(const std::string& home, const std::string& dir, const std::string& sessionDir, bool aborted,
                   std::string& name, std::string& error) {
    const std::string base = baseName(sessionDir);
    if (parentOf(sessionDir) != home || !isRecordingName(base) || !isRealDir(sessionDir)) {
        error = "not one of eyecam-rec's recordings in " + recorder::shortPath(home) + ": " + sessionDir;
        return false;
    }
    std::string into = dir;
    if (aborted) {
        into = dir + "/" + kAbortedDir;
        if (::mkdir(into.c_str(), 0755) != 0 && errno != EEXIST) {
            error = "can't create " + recorder::shortPath(into) + ": " + std::strerror(errno);
            return false;
        }
    }
    const std::string target = into + "/" + base;
    if (::access(target.c_str(), F_OK) == 0) {
        error = recorder::shortPath(target) + " is there already";
        return false;
    }
    if (std::rename(sessionDir.c_str(), target.c_str()) != 0) {
        error = "can't move " + recorder::shortPath(sessionDir) + ": " + std::strerror(errno);
        return false;
    }
    name = aborted ? std::string(kAbortedDir) + "/" + base : base;
    return true;
}

void RunWatch::begin(int step, const std::string& protocol, double sentUnix) {
    step_ = step;
    protocol_ = protocol;
    sent_ = sentUnix;
    seen_ = false;
    staleSince_ = -1.0;
}

RunEnd RunWatch::follow(const eyecam::Status& s, bool fresh, double nowUnix, double sessionMtime) {
    using eyecam::State;
    RunEnd end;
    if (step_ < 0) return end;
    // eyecam-rec gone (or stalled) for long: nothing will come of it
    if (!fresh) {
        if (staleSince_ < 0) staleSince_ = nowUnix;
        if (nowUnix - staleSince_ >= kGoneSec) {
            end.outcome = Outcome::Failed;
            end.why = Why::Gone;
        }
        return end;
    }
    staleSince_ = -1.0;
    const bool mine = s.protocol == protocol_;
    if ((s.state == State::Searching || s.state == State::Recording) && mine) {
        seen_ = true;
        return end;
    }
    if (s.state != State::Idle && s.state != State::Error) {
        // (something else runs: a calibration, or another recording before ours started)
        if (!seen_ && nowUnix - sent_ >= kStartGraceSec) {
            end.outcome = Outcome::Failed;
            end.why = Why::NoStart;
        }
        return end;
    }
    // A folder made after the start is this one's (eyecam-rec clears session_dir when it starts)
    const bool folder = !s.sessionDir.empty() && std::isfinite(sessionMtime) && sessionMtime >= sent_ - 2.0;
    if (!seen_ && !(mine && folder)) {
        // Not shown running yet: an older status, or a start that went wrong at once
        if (nowUnix - sent_ >= kStartGraceSec) {
            end.outcome = Outcome::Failed;
            end.why = s.state == State::Error ? Why::Error : Why::NoStart;
            end.message = s.state == State::Error ? s.message : std::string();
            end.messageEn = s.state == State::Error ? s.messageEn : std::string();
        }
        return end;
    }
    end.dir = folder ? s.sessionDir : std::string();
    end.message = s.message;
    end.messageEn = s.messageEn;
    if (s.state == State::Error) {
        end.outcome = Outcome::Failed;
        end.why = Why::Error;
    } else if (!folder) {
        // Stopped while it searched (no folder), or its folder already gone
        end.outcome = s.sessionDir.empty() ? Outcome::Stopped : Outcome::Failed;
        end.why = s.sessionDir.empty() ? Why::Stopped : Why::Missing;
    } else if (s.lastSessionAborted) {
        end.outcome = Outcome::Stopped;
        end.why = Why::Stopped;
    } else {
        end.outcome = Outcome::Done;
    }
    return end;
}

void WearWatch::start(double now) {
    *this = WearWatch();
    watching_ = true;
    start_ = now;
}

void WearWatch::follow(const eyecam::Status& s, bool fresh, double now) {
    if (!watching_ || detected_) return;
    // The loop stood still that long: the Frame slept (the headset off)
    if (last_ >= 0 && now - last_ >= kOffSec) off_ = true;
    last_ = now;
    if (!fresh) {
        proxOffSince_ = videoOffSince_ = onSince_ = -1.0;
        return;
    }
    // The sensor: off only after it read worn (a face it never reaches the threshold on reads "off" all the time)
    const bool proxKnown = std::isfinite(s.prox) && s.prox >= 0 && std::isfinite(s.proxMin);
    const bool proxWorn = proxKnown && s.prox > s.proxMin;
    proxWorn_ |= proxWorn;
    if (proxKnown && !proxWorn && proxWorn_) {
        if (proxOffSince_ < 0) proxOffSince_ = now;
        if (now - proxOffSince_ >= kOffSec) off_ = true;
    } else {
        proxOffSince_ = -1.0;
    }
    // The eye video, while eyecam-rec looks at it (idle with live processing)
    const bool videoKnown = s.live && s.state == eyecam::State::Idle;
    if (videoKnown && !s.locked) {
        if (videoOffSince_ < 0) videoOffSince_ = now;
        if (now - videoOffSince_ >= kOffSec) off_ = true;
    } else {
        videoOffSince_ = -1.0;
    }
    // On again: the video back (or the sensor, without the video to go by)
    const bool on = videoKnown ? s.locked : proxWorn;
    if (off_ && on) {
        if (onSince_ < 0) onSince_ = now;
        if (now - onSince_ >= kOnSec) detected_ = true;
    } else {
        onSince_ = -1.0;
    }
}

Screen screen(const View& view, bool extra) {
    if (view.progress.job == Job::Delete && view.progress.running) return Screen::Deleting;
    if (view.dir.empty()) return Screen::Consent;
    if (view.running >= 0) return Screen::Running;
    const int step = nextStep(view.info, extra);
    if (step >= kStepCount) return Screen::Done;
    if (view.failure.step == step) return Screen::Failed;
    return needsRewear(step) && view.info.nextWear.empty() ? Screen::Rewear : Screen::Ready;
}

bool startAllowed(const View& view, const eyecam::View& eyecam, int step) {
    const eyecam::State state = eyecam.status.state;
    return !view.dir.empty() && step >= 0 && step < kStepCount && eyecam::setUp(eyecam) && !eyecam.busy &&
           (state == eyecam::State::Idle || state == eyecam::State::Error) && view.running < 0 &&
           !view.progress.running && fits(step, view.freeBytes, view.fps);
}

bool packEntries(const std::string& dir, const Info& info, std::vector<Entry>& entries, std::string& error) {
    entries.clear();
    const std::string top = baseName(dir);
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0) {
        error = "can't read " + recorder::shortPath(dir) + ": " + std::strerror(errno);
        return false;
    }
    entries.push_back({dir, top + "/", 0, st.st_mtime, true});
    const std::string infoPath = dir + "/" + kInfoFile;
    if (::stat(infoPath.c_str(), &st) != 0) {
        error = "can't read " + recorder::shortPath(infoPath) + ": " + std::strerror(errno);
        return false;
    }
    entries.push_back({infoPath, top + "/" + kInfoFile, static_cast<long long>(st.st_size), st.st_mtime, false});
    for (const Step& step : info.steps) {
        if (step.rec.empty()) continue;
        const std::string rec = dir + "/" + step.rec;
        if (!isRealDir(rec) || ::stat(rec.c_str(), &st) != 0) {
            error = "the recording " + step.rec + " isn't there";
            return false;
        }
        entries.push_back({rec, top + "/" + step.rec + "/", 0, st.st_mtime, true});
        for (const std::string& name : names(rec)) {
            const std::string path = rec + "/" + name;
            if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            entries.push_back({path, top + "/" + step.rec + "/" + name, static_cast<long long>(st.st_size),
                               st.st_mtime, false});
        }
    }
    for (const Entry& entry : entries) {
        if (entry.name.size() >= 100) {
            error = "a name is too long for the pack: " + entry.name;
            return false;
        }
    }
    return true;
}

std::string tarHeader(const Entry& entry) {
    if (entry.name.empty() || entry.name.size() >= 100) return "";
    char header[512] = {};
    std::memcpy(header, entry.name.data(), entry.name.size());
    putOctal(header + 100, 8, entry.folder ? 0755 : 0644);
    putOctal(header + 108, 8, 0);  // (no owner: the files are the user's wherever they are unpacked)
    putOctal(header + 116, 8, 0);
    const unsigned long long size = entry.folder ? 0 : static_cast<unsigned long long>(std::max(0LL, entry.size));
    if (size < (1ULL << 33)) {
        putOctal(header + 124, 12, size);
    } else {
        // GNU's base-256: the top bit of the first byte, then the size big-endian
        header[124] = static_cast<char>(0x80);
        for (int i = 0; i < 8; ++i) header[135 - i] = static_cast<char>((size >> (8 * i)) & 0xff);
    }
    putOctal(header + 136, 12, static_cast<unsigned long long>(std::max<std::time_t>(0, entry.mtime)));
    header[156] = entry.folder ? '5' : '0';
    std::memcpy(header + 257, "ustar", 6);
    std::memcpy(header + 263, "00", 2);
    // The checksum: the header's bytes with its own field as spaces
    std::memset(header + 148, ' ', 8);
    unsigned int sum = 0;
    for (unsigned char c : header) sum += c;
    std::snprintf(header + 148, 8, "%06o", sum);
    header[155] = ' ';
    return std::string(header, sizeof(header));
}

std::string packName(const std::string& dir, bool compressed) {
    return baseName(dir) + (compressed ? ".tar.zst" : ".tar");
}

bool zstdAvailable() {
    const char* path = std::getenv("PATH");
    const std::string dirs = path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t from = 0;
    while (from <= dirs.size()) {
        const size_t colon = std::min(dirs.find(':', from), dirs.size());
        const std::string dir = dirs.substr(from, colon - from);
        if (!dir.empty() && ::access((dir + "/zstd").c_str(), X_OK) == 0) return true;
        from = colon + 1;
    }
    return false;
}

Worker::~Worker() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

void Worker::reap() {
    if (thread_.joinable()) thread_.join();
    cancel_ = false;
}

bool Worker::pack(const std::string& dir, const Info& info, bool compressed) {
    if (progress().running) return false;
    reap();
    std::vector<Entry> entries;
    std::string error;
    Progress start;
    start.job = Job::Pack;
    start.output = packName(dir, compressed);
    if (!packEntries(dir, info, entries, error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_ = start;
        progress_.finished = true;
        progress_.error = error;
        reported_ = false;
        return false;
    }
    for (const Entry& entry : entries) start.total += entry.size;
    start.running = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_ = start;
        reported_ = true;
    }
    thread_ = std::thread([this, dir, entries, compressed]() { runPack(dir, entries, compressed); });
    return true;
}

bool Worker::remove(const std::string& home, const std::string& dir) {
    if (progress().running) return false;
    reap();
    if (parentOf(dir) != home || !isFolderName(baseName(dir)) || !isRealDir(dir)) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_ = Progress();
        progress_.job = Job::Delete;
        progress_.running = true;
        reported_ = true;
    }
    thread_ = std::thread([this, dir]() {
        std::error_code error;
        // (links in it are removed, never followed)
        std::filesystem::remove_all(dir, error);
        finish(!error, error ? "can't delete " + recorder::shortPath(dir) + ": " + error.message() : "");
    });
    return true;
}

Progress Worker::progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

bool Worker::takeFinished(Progress& ended) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reported_ || !progress_.finished) return false;
    reported_ = true;
    ended = progress_;
    return true;
}

void Worker::finish(bool ok, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.running = false;
    progress_.finished = true;
    progress_.ok = ok;
    progress_.error = error;
    reported_ = false;
}

void Worker::runPack(const std::string& dir, const std::vector<Entry>& entries, bool compressed) {
    // Below the panel's own loop and VR (Linux sets a thread's niceness on its own)
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), kNice);
    const std::string name = packName(dir, compressed);
    const std::string final = dir + "/" + name;
    const std::string part = final + ".part";
    const int out = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        finish(false, "can't write " + recorder::shortPath(part) + ": " + std::strerror(errno));
        return;
    }
    // Through zstd: it reads a socket (so a zstd that died is an error here, never SIGPIPE) and writes the file
    int sink = out;
    pid_t child = -1;
    if (compressed) {
        int pair[2] = {-1, -1};
        const int devNull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devNull < 0 || ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
            const std::string why = std::strerror(errno);
            if (devNull >= 0) ::close(devNull);
            ::close(out);
            ::unlink(part.c_str());
            finish(false, "can't start zstd: " + why);
            return;
        }
        std::vector<std::string> args = {"zstd", "-3", "-T2", "-q", "-c"};
        std::vector<char*> argv;
        for (std::string& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        child = ::fork();
        if (child == 0) {
            ::setpriority(PRIO_PROCESS, 0, kNice);
            sigset_t none;
            sigemptyset(&none);
            ::sigprocmask(SIG_SETMASK, &none, nullptr);
            ::dup2(pair[1], 0);
            ::dup2(out, 1);
            ::dup2(devNull, 2);
            recorder::cloexecFrom3();
            ::execvp(argv[0], argv.data());
            ::_exit(127);
        }
        const std::string why = std::strerror(errno);
        ::close(pair[1]);
        ::close(devNull);
        if (child < 0) {
            ::close(pair[0]);
            ::close(out);
            ::unlink(part.c_str());
            finish(false, "can't start zstd: " + why);
            return;
        }
        sink = pair[0];
    }
    std::string error;
    std::vector<char> buffer(kChunk);
    long long done = 0;
    const auto put = [&](const char* data, size_t size) {
        if (writeAll(sink, data, size, compressed)) return true;
        error = compressed ? std::string("zstd stopped taking the data: ") + std::strerror(errno)
                           : "can't write " + recorder::shortPath(part) + ": " + std::strerror(errno);
        return false;
    };
    for (const Entry& entry : entries) {
        if (cancel_) {
            error = "stopped";
            break;
        }
        const std::string header = tarHeader(entry);
        if (header.empty()) {
            error = "a name is too long for the pack: " + entry.name;
            break;
        }
        if (!put(header.data(), header.size())) break;
        if (entry.folder) continue;
        const int in = ::open(entry.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) {
            error = "can't read " + recorder::shortPath(entry.path) + ": " + std::strerror(errno);
            break;
        }
        // As long as the header says: a file that shrank meanwhile is padded with zeros, one that grew is cut
        long long left = entry.size;
        while (left > 0 && error.empty() && !cancel_) {
            const size_t want = static_cast<size_t>(std::min<long long>(left, static_cast<long long>(kChunk)));
            ssize_t n = ::read(in, buffer.data(), want);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) {
                error = "can't read " + recorder::shortPath(entry.path) + ": " + std::strerror(errno);
                break;
            }
            if (n == 0) {
                std::fill(buffer.begin(), buffer.begin() + static_cast<long>(want), 0);
                n = static_cast<ssize_t>(want);
            }
            if (!put(buffer.data(), static_cast<size_t>(n))) break;
            left -= n;
            done += n;
            std::lock_guard<std::mutex> lock(mutex_);
            progress_.done = done;
        }
        ::close(in);
        if (!error.empty()) break;
        if (cancel_) {
            error = "stopped";
            break;
        }
        const size_t pad = static_cast<size_t>((512 - entry.size % 512) % 512);
        if (pad > 0) {
            std::fill(buffer.begin(), buffer.begin() + static_cast<long>(pad), 0);
            if (!put(buffer.data(), pad)) break;
        }
    }
    if (error.empty()) {
        // The end: two empty blocks
        std::fill(buffer.begin(), buffer.begin() + 1024, 0);
        put(buffer.data(), 1024);
    }
    if (compressed) {
        ::close(sink);
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        if (error.empty() && !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
            error = WIFEXITED(status) && WEXITSTATUS(status) == 127
                        ? std::string("zstd couldn't be run")
                        : "zstd failed (exit " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) +
                              "; is the disk full?)";
        }
    }
    if (error.empty() && ::fsync(out) != 0) {
        error = "can't write " + recorder::shortPath(part) + ": " + std::strerror(errno);
    }
    struct stat st {};
    if (error.empty() && ::fstat(out, &st) != 0) error = std::strerror(errno);
    ::close(out);
    if (error.empty() && std::rename(part.c_str(), final.c_str()) != 0) {
        error = "can't rename " + recorder::shortPath(part) + ": " + std::strerror(errno);
    }
    if (!error.empty()) {
        ::unlink(part.c_str());
        finish(false, error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.outputBytes = st.st_size;
    }
    finish(true, "");
}

void Session::load(View& view, const std::string& home) {
    const Progress progress = view.progress;
    const bool deleted = view.deleted;
    view = View();
    view.loaded = true;
    view.home = home;
    view.progress = progress;
    view.deleted = deleted;
    view.compress = zstdAvailable();
    view.freeBytes = freeBytes(home);
    run_.end();
    wear_.stop();
    command_.clear();
    view.dir = findCurrent(home);
    if (view.dir.empty()) return;
    view.deleted = false;
    if (!readInfo(view.dir, view.info, view.error)) {
        view.dir.clear();
        return;
    }
    view.totalBytes = folderBytes(view.dir);
    // A recording it had started (the panel restarted meanwhile): followed on from when it was sent
    if (view.info.running >= 0) {
        run_.begin(view.info.running, protocolOf(view.info.running), view.info.runningSince);
        view.running = view.info.running;
    }
}

bool Session::consent(View& view, const std::string& version, std::time_t now) {
    if (!view.dir.empty()) return false;
    std::string dir;
    Info info;
    if (!create(view.home, version, now, dir, info, view.error)) return false;
    view.dir = dir;
    view.info = info;
    view.error.clear();
    view.failure = Failure();
    view.deleted = false;
    view.progress = Progress();
    return true;
}

bool Session::rewore(View& view) {
    const int step = nextStep(view.info, true);
    if (view.dir.empty() || view.running >= 0 || step >= kStepCount || !needsRewear(step) ||
        !view.info.nextWear.empty()) {
        return false;
    }
    view.info.nextWear = kWearButton;
    wear_.stop();
    save(view);
    return true;
}

std::string Session::startCommand(const View& view, const eyecam::View& eyecam, int step) const {
    // Only the step whose turn it is (the optional one only after the others), with its re-wear done
    if (step != nextStep(view.info, step >= kRequiredSteps) || !startAllowed(view, eyecam, step) ||
        !command_.empty() || (needsRewear(step) && view.info.nextWear.empty())) {
        return "";
    }
    return std::string(eyecam::kStartCommand) + " " + protocolOf(step);
}

void Session::started(View& view, int step, const std::string& command, double sentUnix) {
    command_ = command;
    run_.begin(step, protocolOf(step), sentUnix);
    view.running = step;
    view.failure = Failure();
    view.info.running = step;
    view.info.runningSince = sentUnix;
    save(view);
}

bool Session::replied(View& view, const eyecam::Reply& reply) {
    if (command_.empty() || reply.command != command_) return false;
    command_.clear();
    if (reply.ok) return false;
    RunEnd end;
    end.outcome = Outcome::Failed;
    end.why = Why::Refused;
    end.message = reply.error;
    std::string log;
    ended(view, end, log);
    return true;
}

bool Session::follow(View& view, const eyecam::View& eyecam, double nowUnix, double bootNow, std::string& log) {
    bool changed = false;
    if (bootNow - freeAt_ >= 2.0 && !view.home.empty()) {
        freeAt_ = bootNow;
        const long long free = freeBytes(view.home);
        changed |= free != view.freeBytes;
        view.freeBytes = free;
    }
    const double fps = eyecam::cameraFps(eyecam.status);
    if (std::isfinite(fps) && fps > 0) view.fps = fps;
    const bool fresh = eyecam::tabVisible(eyecam.status, nowUnix);
    // The recording running
    if (run_.active() && command_.empty()) {
        double mtime = NAN;
        struct stat st {};
        if (!eyecam.status.sessionDir.empty() && ::stat(eyecam.status.sessionDir.c_str(), &st) == 0) {
            mtime = static_cast<double>(st.st_mtime);
        }
        const RunEnd end = run_.follow(eyecam.status, fresh, nowUnix, mtime);
        if (end.outcome != Outcome::Running) {
            ended(view, end, log);
            changed = true;
        }
    }
    // The re-wear the next step waits for (the optional one's too, once the others are done)
    const int next = nextStep(view.info, true);
    if (!view.dir.empty() && view.running < 0 && next < kStepCount && needsRewear(next) && view.info.nextWear.empty()) {
        if (!wear_.watching()) wear_.start(bootNow);
        wear_.follow(eyecam.status, fresh, bootNow);
        if (wear_.detected()) {
            view.info.nextWear = kWearDetected;
            wear_.stop();
            save(view);
            log += "re-wear before step " + std::to_string(next + 1) + " seen\n";
            changed = true;
        }
    } else {
        wear_.stop();
    }
    const bool off = wear_.watching() && wear_.offSeen();
    const bool ready = wear_.fallbackReady(bootNow);
    changed |= off != view.offSeen || ready != view.fallbackReady;
    view.offSeen = off;
    view.fallbackReady = ready;
    // The pack or the delete
    const Progress progress = worker_.progress();
    changed |= progress.running != view.progress.running || progress.done != view.progress.done ||
               progress.finished != view.progress.finished;
    view.progress = progress;
    Progress finished;
    if (worker_.takeFinished(finished)) {
        changed = true;
        if (finished.job == Job::Pack && finished.ok) {
            view.info.packed = finished.output;
            view.info.packedBytes = finished.outputBytes;
            view.totalBytes = folderBytes(view.dir);
            save(view);
            log += "packed into " + finished.output + " (" + sizeText(finished.outputBytes) + ")\n";
        } else if (finished.job == Job::Delete && finished.ok) {
            log += "deleted " + recorder::shortPath(view.dir) + "\n";
            const std::string home = view.home;
            view.deleted = true;
            load(view, home);
            view.deleted = view.dir.empty();
        } else {
            log += std::string(finished.job == Job::Pack ? "pack" : "delete") + " failed: " + finished.error + "\n";
        }
    }
    return changed;
}

bool Session::pack(View& view) {
    if (view.dir.empty() || view.running >= 0 || requiredDone(view.info) < kRequiredSteps) return false;
    const bool ok = worker_.pack(view.dir, view.info, view.compress);
    view.progress = worker_.progress();
    return ok;
}

bool Session::remove(View& view) {
    if (view.dir.empty() || view.running >= 0) return false;
    if (!worker_.remove(view.home, view.dir)) return false;
    run_.end();
    wear_.stop();
    view.progress = worker_.progress();
    return true;
}

bool Session::save(View& view) {
    std::string error;
    if (writeInfo(view.dir, view.info, error)) return true;
    view.error = error;
    return false;
}

void Session::ended(View& view, const RunEnd& end, std::string& log) {
    const int step = run_.step() >= 0 ? run_.step() : view.running;
    run_.end();
    command_.clear();
    view.running = -1;
    view.info.running = -1;
    view.info.runningSince = 0;
    if (step < 0 || step >= kStepCount) return;
    const std::string label = "step " + std::to_string(step + 1);
    if (end.outcome == Outcome::Done) {
        std::string name;
        std::string error;
        if (takeRecording(view.home, view.dir, end.dir, false, name, error)) {
            Step& done = view.info.steps[step];
            done.protocol = protocolOf(step);
            done.rec = name;
            done.bytes = folderBytes(view.dir + "/" + name);
            done.wear = needsRewear(step) ? view.info.nextWear : std::string(kWearFirst);
            done.done = localTime(std::time(nullptr), false);
            view.info.nextWear.clear();
            view.failure = Failure();
            log += label + " recorded: " + name + " (" + sizeText(done.bytes) + ")\n";
            view.totalBytes = folderBytes(view.dir);
            save(view);
            return;
        }
        view.failure = {step, Why::Move, error, error, ""};
        view.info.aborted.push_back(recorder::shortPath(end.dir) + " not moved: " + error);
        log += label + " recorded, but " + error + "\n";
        save(view);
        return;
    }
    // Didn't finish: its part (if any) goes to aborted/, and the step can be tried again
    view.failure = {step, end.why, end.message, end.messageEn, ""};
    std::string kept;
    if (!end.dir.empty()) {
        std::string name;
        std::string error;
        if (takeRecording(view.home, view.dir, end.dir, true, name, error)) {
            kept = name;
            view.failure.kept = name;
        } else {
            kept = recorder::shortPath(end.dir) + " (" + error + ")";
        }
    }
    const char* how = end.outcome == Outcome::Stopped ? "stopped" : "failed";
    if (!kept.empty()) view.info.aborted.push_back(kept + " " + how);
    log += label + " " + how + (end.message.empty() ? "" : ": " + end.message) +
           (kept.empty() ? "" : "; kept " + kept) + "\n";
    view.totalBytes = folderBytes(view.dir);
    save(view);
}

std::string sizeText(long long bytes) {
    char text[32];
    if (bytes >= 1000LL * 1000 * 1000) {
        std::snprintf(text, sizeof(text), "%.1f GB", bytes / 1e9);
    } else {
        std::snprintf(text, sizeof(text), "%.0f MB", std::max(0LL, bytes) / 1e6);
    }
    return text;
}

}  // namespace contrib
