// "Record for squint training": its info.txt (sections added for the consent, each recording, each try that didn't
// finish and each pack), whether a recording fits, following it in eyecam-rec's status, checking its files and moving
// it into the folder, and packing or deleting the folder on a thread of its own.
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
/** The most of info.txt that is read (a section is under 1 KB). */
constexpr size_t kMaxInfoBytes = 4 << 20;
/** The most of a calibration result (or eyecam's calib.json, settings.json) that is copied. */
constexpr size_t kMaxCalibBytes = 1 << 20;
/** The most of a recording's CSV file that is read to check it (frames.csv of 3 minutes is about 2 MB). */
constexpr size_t kMaxCsvBytes = 64 << 20;

/**
 * Whether text from a position is "YYYY-MM-DD_HH-MM-SS".
 * @param text the text
 * @param at where it starts
 * @return true if it is
 */
bool stampAt(const std::string& text, size_t at) {
    static const char* const shape = "dddd-dd-dd_dd-dd-dd";
    if (text.size() < at + 19) return false;
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
 * Whether a path is a regular file itself (not a link to one).
 * @param path the path
 * @return true if it is
 */
bool isRealFile(const std::string& path) {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
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
 * A number from info.txt.
 * @param text the value
 * @param fallback for anything else
 * @return the number
 */
double readNumber(const std::string& text, double fallback) {
    if (text.empty()) return fallback;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    return end != nullptr && *end == '\0' ? value : fallback;
}

/**
 * A number as info.txt writes it.
 * @param value the number (NaN: "unknown")
 * @param decimals how many decimals
 * @return the text
 */
std::string number(double value, int decimals) {
    if (!std::isfinite(value)) return "unknown";
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
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
 * Write a whole small file (a temporary one, then renamed over it).
 * @param path where
 * @param text what
 * @return true if written
 */
bool writeFile(const std::string& path, const std::string& text) {
    const std::string temp = path + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const bool ok = writeAll(fd, text.data(), text.size(), false) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || std::rename(temp.c_str(), path.c_str()) != 0) {
        ::unlink(temp.c_str());
        return false;
    }
    return true;
}

/**
 * Split a CSV line (eyecam-rec's: plain fields, no quotes).
 * @param line the line
 * @return its fields
 */
std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> out;
    size_t from = 0;
    for (;;) {
        const size_t comma = line.find(',', from);
        out.push_back(line.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
        if (comma == std::string::npos) break;
        from = comma + 1;
    }
    if (!out.empty() && !out.back().empty() && out.back().back() == '\r') out.back().pop_back();
    return out;
}

/**
 * A column's place in a CSV header.
 * @param header the header's fields
 * @param name the column
 * @return its index, or -1
 */
int column(const std::vector<std::string>& header, const char* name) {
    const auto at = std::find(header.begin(), header.end(), name);
    return at == header.end() ? -1 : static_cast<int>(at - header.begin());
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

const char* eyewearName(Eyewear eyewear) {
    switch (eyewear) {
        case Eyewear::None: return "none";
        case Eyewear::Glasses: return "glasses";
        case Eyewear::Inserts: return "inserts";
        case Eyewear::NoAnswer: return "no_answer";
        case Eyewear::Unasked: break;
    }
    return "";
}

Eyewear parseEyewear(const std::string& name) {
    for (const Eyewear e : {Eyewear::None, Eyewear::Glasses, Eyewear::Inserts, Eyewear::NoAnswer}) {
        if (name == eyewearName(e)) return e;
    }
    return Eyewear::Unasked;
}

bool consentNeeded(const Info& info) {
    return info.consentVersion < kConsentVersion || info.eyewear == Eyewear::Unasked;
}

long long estimateBytes(double fps) {
    const double rate = std::isfinite(fps) && fps > 0 ? fps : kDefaultFps;
    return static_cast<long long>((kProtocolSec + kTailSec) * 2 * rate * kFrameBytes);
}

bool fits(long long freeBytes, double fps) {
    return freeBytes < 0 || freeBytes >= estimateBytes(fps) + kMarginBytes;
}

long long recordedBytes(const Info& info) {
    long long total = 0;
    for (const Recording& r : info.recordings) total += r.bytes;
    return total;
}

long long packNeedBytes(const Info& info, bool compressed) {
    const double share = compressed ? kPackShare : 1.0;
    return static_cast<long long>(recordedBytes(info) * share) + kMarginBytes;
}

std::string headerText() {
    return "# frameeyeosc: eye-camera recordings for training eyecam's squint and half-shut check, made with the "
           "panel's\n"
           "# \"Record for squint training\" (protocol_contrib.txt). Each recording is its folder here, as eyecam-rec "
           "writes it\n"
           "# (cues.csv, valve.csv, frames.csv, meta.txt, eye_L.raw, eye_R.raw), with the calibration result at its "
           "start beside\n"
           "# it. A section is added for the consent, each recording, each try that didn't finish (in aborted/, never "
           "packed)\n"
           "# and each pack. headset_off: whether the headset was taken off since the recording before, from "
           "eyecam-rec's\n"
           "# headset_off_count (the eye video stopped or the proximity sensor read off for 10 s or more, then worn "
           "again; only\n"
           "# within one run of eyecam-rec, else unknown). No names or other personal data are kept.\n"
           "format=2\n";
}

std::string consentSection(const std::string& time, int version, Eyewear eyewear) {
    return "\n[consent]\ntime=" + time + "\nversion=" + std::to_string(version) + "\neyewear=" + eyewearName(eyewear) +
           "\n";
}

std::string recordingSection(const Recording& r) {
    std::ostringstream out;
    out << "\n[recording]\n";
    out << "rec=" << r.rec << "\n";
    out << "protocol=" << r.protocol << "\n";
    out << "start=" << r.start << "\n";
    out << "start_unix=" << number(r.startUnix, 3) << "\n";
    out << "end_unix=" << number(r.endUnix, 3) << "\n";
    out << "bytes=" << r.bytes << "\n";
    out << "app_version=" << r.appVersion << "\n";
    out << "eyecam_rec_sha256=" << (r.eyecamRec.empty() ? std::string("unknown") : r.eyecamRec) << "\n";
    out << "lid_veto=" << r.lidVeto << "\n";
    out << "widen=" << r.widen << "\n";
    out << "widen_sensitivity=" << r.widenSensitivity << "\n";
    out << "camera_lids=" << r.cameraLids << "\n";
    out << "eyewear=" << r.eyewear << "\n";
    out << "camera_fps=" << number(r.fps, 1) << "\n";
    out << "minutes_since_previous=" << r.sincePrevious << "\n";
    out << "headset_off=" << r.headsetOff << "\n";
    out << "headset_off_count_start=" << r.offCountStart << "\n";
    out << "headset_off_count_end=" << r.offCountEnd << "\n";
    out << "eyecam_started_unix=" << number(r.eyecamStarted > 0 ? r.eyecamStarted : NAN, 3) << "\n";
    out << "calib_result=" << r.calib << "\n";
    out << "user_calib=" << r.userCalib << "\n";
    out << "settings=" << r.settings << "\n";
    return out.str();
}

std::string abortedSection(const std::string& name, const std::string& why) {
    return "\n[aborted]\nrec=" + name + "\nwhy=" + why + "\n";
}

std::string packedSection(const std::string& file, int count, long long bytes) {
    return "\n[packed]\nfile=" + file + "\nrecordings=" + std::to_string(count) + "\nbytes=" + std::to_string(bytes) +
           "\n";
}

void parseInfo(const std::string& text, Info& info) {
    info = Info();
    std::istringstream lines(text);
    std::string line;
    std::string section;
    std::string abortedRec;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            if (section == "recording") info.recordings.push_back(Recording());
            if (section == "consent") {
                info.consentVersion = 0;
                info.consent.clear();
                info.eyewear = Eyewear::Unasked;
            }
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (section == "consent") {
            if (key == "time") info.consent = value;
            if (key == "version") info.consentVersion = static_cast<int>(readInteger(value, 0));
            if (key == "eyewear") info.eyewear = parseEyewear(value);
        } else if (section == "recording" && !info.recordings.empty()) {
            Recording& r = info.recordings.back();
            if (key == "rec") r.rec = value;
            if (key == "protocol") r.protocol = value;
            if (key == "start") r.start = value;
            if (key == "start_unix") r.startUnix = readNumber(value, 0.0);
            if (key == "end_unix") r.endUnix = readNumber(value, 0.0);
            if (key == "bytes") r.bytes = readInteger(value, 0);
            if (key == "app_version") r.appVersion = value;
            if (key == "eyecam_rec_sha256") r.eyecamRec = value == "unknown" ? std::string() : value;
            if (key == "lid_veto") r.lidVeto = value;
            if (key == "widen") r.widen = value;
            if (key == "widen_sensitivity") r.widenSensitivity = value;
            if (key == "camera_lids") r.cameraLids = value;
            if (key == "eyewear") r.eyewear = value;
            if (key == "camera_fps") r.fps = readNumber(value, NAN);
            if (key == "minutes_since_previous") r.sincePrevious = value;
            if (key == "headset_off") r.headsetOff = value;
            if (key == "headset_off_count_start") r.offCountStart = readInteger(value, -1);
            if (key == "headset_off_count_end") r.offCountEnd = readInteger(value, -1);
            if (key == "eyecam_started_unix") r.eyecamStarted = readNumber(value, 0.0);
            if (key == "calib_result") r.calib = value;
            if (key == "user_calib") r.userCalib = value;
            if (key == "settings") r.settings = value;
        } else if (section == "aborted") {
            if (key == "rec") abortedRec = value;
            if (key == "why") info.aborted.push_back(abortedRec + " " + value);
        } else if (section == "packed") {
            if (key == "file") info.packed = value;
            if (key == "recordings") info.packedCount = static_cast<int>(readInteger(value, 0));
            if (key == "bytes") info.packedBytes = readInteger(value, 0);
        }
    }
    // A recording's name that isn't one (written by hand) doesn't count
    info.recordings.erase(std::remove_if(info.recordings.begin(), info.recordings.end(),
                                         [](const Recording& r) { return !isRecordingName(r.rec); }),
                          info.recordings.end());
    if (!isPackName(info.packed)) info.packed.clear();
}

bool isRecordingName(const std::string& name) {
    return name.size() == 23 && name.compare(0, 4, "rec_") == 0 && stampAt(name, 4);
}

bool isPackName(const std::string& name) {
    if (name.compare(0, 8, "contrib_") != 0 || !stampAt(name, 8)) return false;
    const std::string rest = name.substr(27);
    return rest == ".tar.zst" || rest == ".tar";
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

std::string defaultConfigDir() {
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : "") + "/.config/eyecam";
}

bool appendInfo(const std::string& dir, const std::string& text, std::string& error) {
    const std::string path = dir + "/" + kInfoFile;
    const bool fresh = ::access(path.c_str(), F_OK) != 0;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "can't write " + recorder::shortPath(path) + ": " + std::strerror(errno);
        return false;
    }
    const std::string all = fresh ? headerText() + text : text;
    if (!writeAll(fd, all.data(), all.size(), false) || ::fsync(fd) != 0) {
        error = "can't write " + recorder::shortPath(path) + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::close(fd);
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

std::vector<std::string> missingFiles(const std::string& dir) {
    std::vector<std::string> missing;
    for (const char* file : kRequiredFiles) {
        if (!isRealFile(dir + "/" + file)) missing.push_back(file);
    }
    return missing;
}

CheckResult checkRecording(const std::string& dir) {
    CheckResult result;
    const std::vector<std::string> missing = missingFiles(dir);
    if (!missing.empty()) {
        result.check = Check::Missing;
        for (const std::string& file : missing) result.detail += (result.detail.empty() ? "" : ", ") + file;
        return result;
    }
    // frames.csv: each eye's frames, and their camera times
    std::string text;
    file_io::read(dir + "/frames.csv", text, kMaxCsvBytes);
    std::istringstream frames(text);
    std::string line;
    std::getline(frames, line);
    const std::vector<std::string> head = fields(line);
    const int eyeAt = column(head, "eye");
    const int camAt = column(head, "t_cam");
    long long count[2] = {0, 0};
    std::vector<double> times;
    while (eyeAt >= 0 && camAt >= 0 && std::getline(frames, line)) {
        const std::vector<std::string> f = fields(line);
        if (static_cast<int>(f.size()) <= std::max(eyeAt, camAt)) continue;
        if (f[eyeAt] != "L" && f[eyeAt] != "R") continue;
        ++count[f[eyeAt] == "R" ? 1 : 0];
        times.push_back(readNumber(f[camAt], NAN));
    }
    // Each raw file exactly that many frames (frame_bytes from meta.txt): one cut short when the disk ran full
    long long frameBytes = kRawFrameBytes;
    file_io::read(dir + "/meta.txt", text, kMaxCsvBytes);
    std::istringstream meta(text);
    while (std::getline(meta, line)) {
        if (line.rfind("frame_bytes=", 0) == 0) frameBytes = readInteger(line.substr(12), kRawFrameBytes);
    }
    // An eye without a single frame: its camera's video didn't come (both eyes are needed)
    if (count[0] == 0 || count[1] == 0) {
        result.check = Check::NoVideo;
        result.detail = std::string(count[0] == 0 ? "L" : "") + (count[1] == 0 ? "R" : "");
        return result;
    }
    // One eye's camera far slower than the other's: its video came only in part (a ring locked on some of its slots)
    const double fewer = static_cast<double>(std::min(count[0], count[1]));
    if (fewer < kEvenShare * static_cast<double>(std::max(count[0], count[1]))) {
        result.check = Check::Uneven;
        result.detail = std::to_string(count[0]) + "/" + std::to_string(count[1]);
        return result;
    }
    for (int eye = 0; eye < 2; ++eye) {
        const std::string raw = eye == 0 ? "eye_L.raw" : "eye_R.raw";
        struct stat st {};
        if (::stat((dir + "/" + raw).c_str(), &st) != 0 ||
            static_cast<long long>(st.st_size) != count[eye] * frameBytes) {
            result.check = Check::Truncated;
            result.detail += (result.detail.empty() ? "" : ", ") + raw;
        }
    }
    if (result.check != Check::Ok) return result;
    // valve.csv: the usable samples' times, and how many frames have one near
    file_io::read(dir + "/valve.csv", text, kMaxCsvBytes);
    std::istringstream valve(text);
    std::getline(valve, line);
    const std::vector<std::string> vhead = fields(line);
    const int timeAt = column(vhead, "sample_time");
    const int stateAt = column(vhead, "producer_state");
    const int openL = column(vhead, "open_l");
    const int openR = column(vhead, "open_r");
    const int last = std::max({timeAt, stateAt, openL, openR});
    std::vector<double> samples;
    while (std::min({timeAt, stateAt, openL, openR}) >= 0 && std::getline(valve, line)) {
        const std::vector<std::string> f = fields(line);
        if (static_cast<int>(f.size()) <= last || f[stateAt] != "1") continue;
        const double t = readNumber(f[timeAt], NAN);
        if (std::isfinite(t) && std::isfinite(readNumber(f[openL], NAN)) && std::isfinite(readNumber(f[openR], NAN))) {
            samples.push_back(t);
        }
    }
    std::sort(samples.begin(), samples.end());
    long long paired = 0;
    for (const double t : times) {
        if (!std::isfinite(t)) continue;
        const auto next = std::lower_bound(samples.begin(), samples.end(), t);
        const bool after = next != samples.end() && *next - t <= kValvePairSec;
        const bool before = next != samples.begin() && t - *(next - 1) <= kValvePairSec;
        paired += after || before ? 1 : 0;
    }
    const double share = times.empty() ? 0.0 : static_cast<double>(paired) / static_cast<double>(times.size());
    if (share < kValveShare) {
        result.check = Check::Valve;
        result.detail = std::to_string(static_cast<int>(std::floor(share * 100))) + "%";
    }
    return result;
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

std::string newestCalibResult(const std::string& home) {
    std::vector<std::string> found = names(home);
    for (auto name = found.rbegin(); name != found.rend(); ++name) {
        if (name->compare(0, 6, "calib_") != 0 || !stampAt(*name, 6)) continue;
        std::string text;
        if (file_io::read(home + "/" + *name + "/calib_result.json", text, kMaxCalibBytes) && !text.empty()) {
            return text;
        }
    }
    return "";
}

std::string headsetOffBetween(const Recording& before, long long offCountStart, double eyecamStarted) {
    if (before.offCountEnd < 0 || offCountStart < 0 || before.eyecamStarted <= 0 || eyecamStarted <= 0 ||
        std::fabs(before.eyecamStarted - eyecamStarted) > 1.0) {
        return "unknown";
    }
    return offCountStart > before.offCountEnd ? "yes" : "no";
}

void RunWatch::begin(const std::string& protocol, double sentUnix) {
    active_ = true;
    protocol_ = protocol;
    sent_ = sentUnix;
    seen_ = false;
    staleSince_ = -1.0;
}

RunEnd RunWatch::follow(const eyecam::Status& s, bool fresh, double nowUnix, double sessionMtime) {
    using eyecam::State;
    RunEnd end;
    if (!active_) return end;
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
        // (something else runs: a calibration, or another recording before this one started)
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

Screen screen(const View& view, Page page, bool agreed) {
    if (view.progress.job == Job::Delete && view.progress.running) return Screen::Deleting;
    if (page == Page::Manage) return Screen::Done;
    if (view.running) return Screen::Running;
    if (consentNeeded(view.info)) return agreed ? Screen::Eyewear : Screen::Consent;
    if (view.failure.shown) return Screen::Failed;
    return view.justDone ? Screen::Done : Screen::Ready;
}

bool startAllowed(const View& view, const eyecam::View& eyecam, bool eyeData) {
    const eyecam::State state = eyecam.status.state;
    return view.exists && !consentNeeded(view.info) && eyecam::setUp(eyecam) && !eyecam.busy &&
           (state == eyecam::State::Idle || state == eyecam::State::Error) && !view.running &&
           !view.progress.running && fits(view.freeBytes, view.fps) && eyeData;
}

bool packEntries(const std::string& dir, const Info& info, std::vector<Entry>& entries,
                 std::vector<std::string>& incomplete, std::string& error) {
    entries.clear();
    incomplete.clear();
    const std::string top = kFolderName;
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0) {
        error = "can't read " + recorder::shortPath(dir) + ": " + std::strerror(errno);
        return false;
    }
    entries.push_back({dir, top + "/", 0, st.st_mtime, true});
    const auto file = [&](const std::string& name) {
        const std::string path = dir + "/" + name;
        if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
        entries.push_back({path, top + "/" + name, static_cast<long long>(st.st_size), st.st_mtime, false});
        return true;
    };
    if (!file(kInfoFile)) {
        error = "can't read " + recorder::shortPath(dir + "/" + kInfoFile);
        return false;
    }
    for (const Recording& r : info.recordings) {
        const std::string rec = dir + "/" + r.rec;
        // (one that can't be used, or gone, is left out and named: it is to be recorded again)
        if (!isRealDir(rec) || checkRecording(rec).check != Check::Ok || ::stat(rec.c_str(), &st) != 0) {
            incomplete.push_back(r.rec);
            continue;
        }
        entries.push_back({rec, top + "/" + r.rec + "/", 0, st.st_mtime, true});
        for (const std::string& name : names(rec)) file(r.rec + "/" + name);
        for (const std::string& copy : {r.calib, r.userCalib, r.settings}) {
            if (!copy.empty() && copy != "none") file(copy);
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
    putOctal(header + 148, 7, sum);
    header[155] = ' ';
    return std::string(header, sizeof(header));
}

std::string packName(std::time_t when, bool compressed) {
    return "contrib_" + recorder::fileStamp(when) + (compressed ? ".tar.zst" : ".tar");
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

bool Worker::pack(const std::string& dir, const Info& info, const std::string& name, bool compressed) {
    if (progress().running) return false;
    reap();
    Progress start;
    start.job = Job::Pack;
    start.running = true;
    start.output = name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_ = start;
        reported_ = true;
    }
    thread_ = std::thread([this, dir, info, name, compressed]() { runPack(dir, info, name, compressed); });
    return true;
}

bool Worker::remove(const std::string& home, const std::string& dir) {
    if (progress().running) return false;
    reap();
    if (dir != home + "/" + kFolderName || !isRealDir(dir)) return false;
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

void Worker::runPack(const std::string& dir, const Info& info, const std::string& name, bool compressed) {
    // Below the panel's own loop and VR (Linux sets a thread's niceness on its own)
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), kNice);
    // Each recording checked again (its CSV files read): what can't be used stays out
    std::vector<Entry> entries;
    std::vector<std::string> incomplete;
    std::string listError;
    const bool listed = packEntries(dir, info, entries, incomplete, listError);
    const int count = static_cast<int>(info.recordings.size() - incomplete.size());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.incomplete = incomplete;
        progress_.count = count;
        for (const Entry& entry : entries) progress_.total += entry.size;
    }
    if (!listed || count == 0) {
        finish(false, listed ? std::string("no recording can be used") : listError);
        return;
    }
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
        std::vector<std::string> args = {"zstd", "-" + std::to_string(kZstdLevel), "-T2", "-q", "-c"};
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

void Session::load(View& view, const std::string& home, const std::string& configDir) {
    const Progress progress = view.progress;
    const bool deleted = view.deleted;
    view = View();
    view.loaded = true;
    view.home = home;
    view.dir = home + "/" + kFolderName;
    view.configDir = configDir;
    view.progress = progress;
    view.deleted = deleted;
    view.compress = zstdAvailable();
    view.freeBytes = freeBytes(home);
    view.exists = isRealDir(view.dir);
    run_.end();
    command_.clear();
    if (!view.exists) return;
    std::string text;
    file_io::read(view.dir + "/" + kInfoFile, text, kMaxInfoBytes);
    parseInfo(text, view.info);
    view.totalBytes = folderBytes(view.dir);
}

void Session::refresh(View& view) {
    if (view.home.empty()) return;
    // (an earlier write's failure isn't shown again: the next one says so if it fails too)
    view.error.clear();
    view.exists = isRealDir(view.dir);
    view.freeBytes = freeBytes(view.home);
    view.info = Info();
    view.totalBytes = 0;
    if (!view.exists) return;
    std::string text;
    file_io::read(view.dir + "/" + kInfoFile, text, kMaxInfoBytes);
    parseInfo(text, view.info);
    view.totalBytes = folderBytes(view.dir);
}

bool Session::consent(View& view, Eyewear eyewear, std::time_t now) {
    if (eyewear == Eyewear::Unasked || view.home.empty()) return false;
    for (const std::string& dir : {view.home, view.dir}) {
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
            view.error = "can't create " + recorder::shortPath(dir) + ": " + std::strerror(errno);
            return false;
        }
    }
    view.exists = isRealDir(view.dir);
    if (!append(view, consentSection(localTime(now, true), kConsentVersion, eyewear))) return false;
    view.deleted = false;
    view.failure = Failure();
    return true;
}

std::string Session::startCommand(const View& view, const eyecam::View& eyecam, bool eyeData) const {
    if (!startAllowed(view, eyecam, eyeData) || !command_.empty()) return "";
    return std::string(eyecam::kStartCommand) + " " + kProtocol;
}

void Session::started(View& view, const eyecam::View& eyecam, const Snapshot& snapshot, const std::string& command,
                      double sentUnix) {
    const eyecam::Status& s = eyecam.status;
    command_ = command;
    run_.begin(kProtocol, sentUnix);
    view.running = true;
    view.justDone = false;
    view.failure = Failure();
    // What its section says about the start: the settings, the time since the one before, the count of taking-offs
    Recording r;
    r.protocol = kProtocol;
    r.startUnix = sentUnix;
    r.start = localTime(static_cast<std::time_t>(sentUnix), true);
    r.appVersion = snapshot.appVersion;
    r.eyecamRec = snapshot.eyecamRec;
    r.lidVeto = s.hasLidVeto ? (s.lidVeto ? "on" : "off") : "unknown";
    r.widen = snapshot.widen ? "on" : "off";
    r.widenSensitivity = s.hasWidenSensitivity ? number(s.widenSensitivity, 2) : "unknown";
    r.cameraLids = snapshot.cameraLids ? "on" : "off";
    r.eyewear = eyewearName(view.info.eyewear);
    r.fps = eyecam::cameraFps(s);
    r.offCountStart = s.hasHeadsetOff ? s.headsetOffCount : -1;
    r.eyecamStarted = s.hasHeadsetOff ? s.startedUnix : 0.0;
    if (view.info.recordings.empty()) {
        r.sincePrevious = "none";
        r.headsetOff = "none";
    } else {
        const Recording& before = view.info.recordings.back();
        r.sincePrevious = before.endUnix > 0 ? number((sentUnix - before.endUnix) / 60.0, 1) : "unknown";
        r.headsetOff = headsetOffBetween(before, r.offCountStart, r.eyecamStarted);
    }
    pending_ = r;
    // The calibrations and settings then, copied beside it once it is recorded
    calibText_ = newestCalibResult(view.home);
    userCalibText_.clear();
    settingsText_.clear();
    file_io::read(view.configDir + "/calib.json", userCalibText_, kMaxCalibBytes);
    file_io::read(view.configDir + "/settings.json", settingsText_, kMaxCalibBytes);
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
    ended(view, end, eyecam::View(), static_cast<double>(std::time(nullptr)), log);
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
    // The recording running (only once its start is answered)
    if (run_.active() && command_.empty()) {
        double mtime = NAN;
        struct stat st {};
        if (!eyecam.status.sessionDir.empty() && ::stat(eyecam.status.sessionDir.c_str(), &st) == 0) {
            mtime = static_cast<double>(st.st_mtime);
        }
        const RunEnd end = run_.follow(eyecam.status, eyecam::tabVisible(eyecam.status, nowUnix), nowUnix, mtime);
        if (end.outcome != Outcome::Running) {
            ended(view, end, eyecam, nowUnix, log);
            changed = true;
        }
    }
    // The pack or the delete
    const Progress progress = worker_.progress();
    changed |= progress.running != view.progress.running || progress.done != view.progress.done ||
               progress.finished != view.progress.finished;
    view.progress = progress;
    Progress finished;
    if (worker_.takeFinished(finished)) {
        changed = true;
        if (finished.job == Job::Pack) view.incomplete = finished.incomplete;
        if (finished.job == Job::Pack && finished.ok) {
            // (the pack before it holds fewer: it goes; the recordings stay)
            const std::string before = view.info.packed;
            if (append(view, packedSection(finished.output, finished.count, finished.outputBytes)) &&
                isPackName(before) && before != finished.output) {
                ::unlink((view.dir + "/" + before).c_str());
            }
            view.totalBytes = folderBytes(view.dir);
            log += "packed " + std::to_string(finished.count) + " recording(s) into " + finished.output + " (" +
                   sizeText(finished.outputBytes) + ")\n";
        } else if (finished.job == Job::Delete && finished.ok) {
            log += "deleted " + recorder::shortPath(view.dir) + "\n";
            const std::string home = view.home;
            view.deleted = true;
            load(view, home);
        } else {
            log += std::string(finished.job == Job::Pack ? "pack" : "delete") + " failed: " + finished.error + "\n";
        }
    }
    return changed;
}

bool Session::pack(View& view, std::time_t now) {
    if (!view.exists || view.running || view.info.recordings.empty()) return false;
    // (the free space read now: the panel's button follows the one read every 2 s)
    view.freeBytes = freeBytes(view.home);
    const long long need = packNeedBytes(view.info, view.compress);
    if (view.freeBytes >= 0 && view.freeBytes < need) {
        view.progress = Progress();
        view.progress.job = Job::Pack;
        view.progress.finished = true;
        view.progress.error = "not enough space (" + sizeText(need) + " needed, " + sizeText(view.freeBytes) + " free)";
        return false;
    }
    const bool ok = worker_.pack(view.dir, view.info, packName(now, view.compress), view.compress);
    view.progress = worker_.progress();
    return ok;
}

bool Session::remove(View& view) {
    if (!view.exists || view.running) return false;
    if (!worker_.remove(view.home, view.dir)) return false;
    view.progress = worker_.progress();
    return true;
}

bool Session::append(View& view, const std::string& text) {
    std::string error;
    if (!appendInfo(view.dir, text, error)) {
        view.error = error;
        return false;
    }
    std::string all;
    file_io::read(view.dir + "/" + kInfoFile, all, kMaxInfoBytes);
    parseInfo(all, view.info);
    view.error.clear();
    return true;
}

void Session::ended(View& view, const RunEnd& end, const eyecam::View& eyecam, double nowUnix, std::string& log) {
    run_.end();
    command_.clear();
    view.running = false;
    // Done, and usable: into the folder, with its section and the copies beside it
    CheckResult check;
    if (end.outcome == Outcome::Done) check = checkRecording(end.dir);
    if (end.outcome == Outcome::Done && check.check == Check::Ok) {
        std::string name;
        std::string error;
        if (takeRecording(view.home, view.dir, end.dir, false, name, error)) {
            Recording r = pending_;
            r.rec = name;
            r.endUnix = nowUnix;
            r.bytes = folderBytes(view.dir + "/" + name);
            const eyecam::Status& s = eyecam.status;
            r.offCountEnd = s.hasHeadsetOff && std::fabs(s.startedUnix - r.eyecamStarted) <= 1.0 ? s.headsetOffCount
                                                                                                   : -1;
            const auto copy = [&](const std::string& text, const char* suffix) {
                const std::string file = name + suffix;
                return !text.empty() && writeFile(view.dir + "/" + file, text) ? file : std::string("none");
            };
            r.calib = copy(calibText_, ".calib_result.json");
            r.userCalib = copy(userCalibText_, ".user_calib.json");
            r.settings = copy(settingsText_, ".settings.json");
            append(view, recordingSection(r));
            view.totalBytes = folderBytes(view.dir);
            view.justDone = true;
            log += "recorded " + name + " (" + sizeText(r.bytes) + "; headset off since the one before: " +
                   r.headsetOff + ")\n";
            return;
        }
        view.failure = {true, Why::Move, error, error, ""};
        log += "recorded, but " + error + "\n";
        return;
    }
    // Didn't finish, or can't be used: its part (if any) goes to aborted/, and it can be recorded again
    view.failure = {true, end.why, end.message, end.messageEn, ""};
    std::string why = end.outcome == Outcome::Stopped ? "stopped" : "failed";
    if (end.outcome == Outcome::Done) {
        const Why reason = check.check == Check::Missing     ? Why::Incomplete
                           : check.check == Check::NoVideo   ? Why::NoVideo
                           : check.check == Check::Uneven    ? Why::Uneven
                           : check.check == Check::Truncated ? Why::Truncated
                                                             : Why::Valve;
        view.failure = {true, reason, check.detail, check.detail, ""};
        why = check.check == Check::Missing     ? "missing " + check.detail
              : check.check == Check::NoVideo   ? "no frames of eye " + check.detail
              : check.check == Check::Uneven    ? "uneven frames (left/right): " + check.detail
              : check.check == Check::Truncated ? "cut short: " + check.detail
                                                : "Valve's samples for only " + check.detail + " of the frames";
    }
    if (!end.dir.empty()) {
        std::string name;
        std::string error;
        if (takeRecording(view.home, view.dir, end.dir, true, name, error)) {
            view.failure.kept = name;
            append(view, abortedSection(name, why));
        } else {
            why += " (" + error + ")";
        }
    }
    view.totalBytes = folderBytes(view.dir);
    log += "the recording " + why + (end.message.empty() ? "" : ": " + end.message) + "\n";
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
