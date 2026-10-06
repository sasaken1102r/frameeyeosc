// Records of calibrations and eye fits (see report.h).
#include "report.h"

#include "command.h"
#include "icons.h"
#include "json.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace report {

namespace {

/** The files of a record, in the order the record view lists them. */
constexpr const char* kFileNames[] = {"report.txt", "logs.txt", "status.jsonl", "calib_result.json", "summary.json"};

/**
 * printf into a string.
 * @param format the format
 * @param args its values
 * @return the text
 */
template <typename... Args>
std::string format(const char* format, Args... args) {
    char text[512];
    std::snprintf(text, sizeof(text), format, args...);
    return text;
}

/**
 * @param text the text
 * @param prefix the start
 * @return true if text starts with prefix
 */
bool startsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

/**
 * @param text the text
 * @param part what to look for
 * @return true if text has it
 */
bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

/**
 * A local time as text.
 * @param when Unix seconds
 * @param pattern strftime's
 * @return the text
 */
std::string localText(double when, const char* pattern) {
    const std::time_t whole = static_cast<std::time_t>(std::floor(when));
    std::tm local {};
    localtime_r(&whole, &local);
    char text[64];
    std::strftime(text, sizeof(text), pattern, &local);
    return text;
}

/**
 * How many terminal columns a text takes (ASCII one, anything else two: the labels are Japanese).
 * @param text UTF-8
 * @return columns
 */
size_t columns(const std::string& text) {
    size_t n = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
        // The code point: from U+3000 on (Japanese, full-width forms) two columns, anything before it one
        unsigned long code = len == 1 ? lead : lead & (0xFF >> (len + 1));
        for (size_t k = 1; k < len && i + k < text.size(); ++k) code = (code << 6) | (text[i + k] & 0x3F);
        n += code >= 0x3000 ? 2 : 1;
        i += len;
    }
    return n;
}

/**
 * A text padded with spaces to a width (at least one space after it).
 * @param text the text
 * @param width columns
 * @return the padded text
 */
std::string pad(const std::string& text, size_t width) {
    const size_t used = columns(text);
    return text + std::string(used < width ? width - used : 1, ' ');
}

/**
 * Read a whole file (up to a size).
 * @param path the file
 * @param text where to write it
 * @param maxBytes the most read
 * @return true if it was read
 */
bool readFile(const std::string& path, std::string& text, size_t maxBytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    text.assign(maxBytes, '\0');
    in.read(&text[0], static_cast<std::streamsize>(maxBytes));
    text.resize(static_cast<size_t>(in.gcount()));
    return true;
}

/**
 * The end of a file, from the start of a line.
 * @param path the file
 * @param maxBytes the most read
 * @return the text ("" if it can't be read)
 */
std::string readTail(const std::string& path, size_t maxBytes) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return "";
    const std::streamoff size = in.tellg();
    const std::streamoff from = size > static_cast<std::streamoff>(maxBytes) ? size - static_cast<std::streamoff>(maxBytes) : 0;
    in.seekg(from);
    std::string text(static_cast<size_t>(size - from), '\0');
    in.read(&text[0], static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(in.gcount()));
    if (from > 0) {
        const size_t newline = text.find('\n');
        text = newline == std::string::npos ? std::string() : text.substr(newline + 1);
    }
    return text;
}

/**
 * Write a file.
 * @param path where
 * @param text what
 * @return true if written
 */
bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    return static_cast<bool>(out);
}

/**
 * Make a folder and the ones above it.
 * @param path the folder
 * @return true if it is there now
 */
bool makeDirs(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !makeDirs(path.substr(0, slash))) return false;
    return ::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
}

/**
 * @param path a path
 * @return true if it is a folder
 */
bool isDir(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * The names in a folder.
 * @param dir the folder
 * @return them ("." and ".." left out)
 */
std::vector<std::string> entries(const std::string& dir) {
    std::vector<std::string> names;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return names;
    while (const dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(d);
    return names;
}

/**
 * Whether text from a position is "YYYY-MM-DD_HH-MM-SS".
 * @param text the text
 * @param at where it starts
 * @return true if it is
 */
bool isStamp(const std::string& text, size_t at) {
    static const char* const shape = "dddd-dd-dd_dd-dd-dd";
    if (text.size() < at + 19) return false;
    for (size_t i = 0; i < 19; ++i) {
        const char c = text[at + i];
        if (shape[i] == 'd' ? !std::isdigit(static_cast<unsigned char>(c)) : c != shape[i]) return false;
    }
    return true;
}

/**
 * A stamp's local time.
 * @param stamp "YYYY-MM-DD_HH-MM-SS"
 * @return Unix seconds (NaN if it isn't one)
 */
double stampTime(const std::string& stamp) {
    std::tm tm {};
    if (std::sscanf(stamp.c_str(), "%4d-%2d-%2d_%2d-%2d-%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                    &tm.tm_min, &tm.tm_sec) != 6) {
        return NAN;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    return static_cast<double>(std::mktime(&tm));
}

/**
 * A number or null.
 * @param value the number (NaN = null)
 * @param decimals rounded to this many
 * @return the JSON value
 */
JsonValue numberOrNull(double value, int decimals) {
    if (!std::isfinite(value)) return JsonValue::makeNull();
    const double scale = std::pow(10.0, decimals);
    return JsonValue::makeNumber(std::round(value * scale) / scale, decimals == 0);
}

/**
 * A member's number.
 * @param object the object
 * @param name the member
 * @return the number (NaN when missing or null)
 */
double numberOf(const JsonValue& object, const char* name) {
    const JsonValue* v = object.get(name);
    return v != nullptr && v->isNumber() ? v->number : NAN;
}

/**
 * A member's text.
 * @param object the object
 * @param name the member
 * @return the text ("" when missing)
 */
std::string textOf(const JsonValue& object, const char* name) {
    const JsonValue* v = object.get(name);
    return v != nullptr && v->isString() ? v->text : std::string();
}

/**
 * A wear or user calibration's folder from eyecam (calib_YYYY-MM-DD_HH-MM-SS) that began within a run.
 * @param home eyecam's folder
 * @param start the run's start (Unix seconds)
 * @param end its end
 * @return the newest such folder ("" if none)
 */
std::string findCalibDir(const std::string& home, double start, double end) {
    if (home.empty()) return "";
    std::string best;
    for (const std::string& name : entries(home)) {
        if (!startsWith(name, "calib_") || !isStamp(name, 6)) continue;
        const double at = stampTime(name.substr(6, 19));
        if (!std::isfinite(at) || at < start - 5 || at > end + 5) continue;
        if (name > best) best = name;
    }
    return best.empty() ? std::string() : home + "/" + best;
}

/**
 * A point's name as the eye fit's log writes it ("center", "up", ...) in the panel's words.
 * @param t texts
 * @param name the log's name
 * @return the panel's ("" if unknown)
 */
std::string pointWord(const UiText& t, const std::string& name) {
    if (name == "center") return t.pointCenter;
    if (name == "up") return t.pointUp;
    if (name == "down") return t.pointDown;
    if (name == "left") return t.pointLeft;
    if (name == "right") return t.pointRight;
    if (name == "closed") return t.pointClosed;
    return "";
}

/**
 * @param t texts
 * @return true for the English table
 */
bool english(const UiText& t) {
    return &t == &uiText(Language::En);
}

}  // namespace

const Unit kUnits[3] = {
    {"frameeyeosc", Source::Core},
    {"frameeyeosc-panel", Source::Panel},
    {"eyecam", Source::Eyecam},
};

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::CalibWear: return "calib-wear";
        case Kind::CalibUser: return "calib-user";
        case Kind::Fit: return "fit";
        case Kind::Recenter: return "recenter";
    }
    return "fit";
}

bool parseKind(const std::string& name, Kind& kind) {
    for (const Kind k : {Kind::CalibWear, Kind::CalibUser, Kind::Fit, Kind::Recenter}) {
        if (name == kindName(k)) {
            kind = k;
            return true;
        }
    }
    return false;
}

const char* resultName(Result result) {
    switch (result) {
        case Result::Ok: return "ok";
        case Result::Failed: return "failed";
        case Result::Partial: return "partial";
    }
    return "failed";
}

bool parseResult(const std::string& name, Result& result) {
    for (const Result r : {Result::Ok, Result::Failed, Result::Partial}) {
        if (name == resultName(r)) {
            result = r;
            return true;
        }
    }
    return false;
}

const char* sourceName(Source source) {
    switch (source) {
        case Source::Core: return "frameeyeosc";
        case Source::Panel: return "panel";
        case Source::Eyecam: return "eyecam";
        case Source::Valve: return "valve";
    }
    return "panel";
}

bool parseSource(const std::string& name, Source& source) {
    for (const Source s : {Source::Core, Source::Panel, Source::Eyecam, Source::Valve}) {
        if (name == sourceName(s)) {
            source = s;
            return true;
        }
    }
    return false;
}

std::string defaultDir() {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    std::string base;
    if (xdg != nullptr && xdg[0] == '/') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home != nullptr ? home : ".") + "/.local/state";
    }
    return base + "/frameeyeosc/reports";
}

std::string folderName(Kind kind, double start) {
    return std::string(kindName(kind)) + "_" + localText(start, "%Y-%m-%d_%H-%M-%S");
}

bool isFolderName(const std::string& name) {
    const size_t underscore = name.find('_');
    Kind kind;
    if (underscore == std::string::npos || !parseKind(name.substr(0, underscore), kind)) return false;
    const size_t at = underscore + 1;
    if (!isStamp(name, at)) return false;
    const std::string rest = name.substr(at + 19);
    if (rest.empty()) return true;
    // A second one in the same second: _2, _3...
    if (rest.size() < 2 || rest[0] != '_') return false;
    return std::all_of(rest.begin() + 1, rest.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

std::string folderStamp(const std::string& name) {
    if (!isFolderName(name)) return "";
    return name.substr(name.find('_') + 1);
}

std::vector<LogLine> parseJournal(const std::string& text, Source source) {
    std::vector<LogLine> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        // "1791287099.117852 host name[pid]: text"
        const char* begin = line.c_str();
        char* after = nullptr;
        const double when = std::strtod(begin, &after);
        const bool timed = after != begin && *after == ' ' && std::isdigit(static_cast<unsigned char>(line[0])) &&
                           std::isfinite(when) && when > 0;
        if (!timed) {
            // A message's next line (journalctl indents it): the time of the line before
            if (lines.empty()) continue;
            size_t first = line.find_first_not_of(' ');
            if (first == std::string::npos) continue;
            lines.push_back({lines.back().at, source, line.substr(first)});
            continue;
        }
        std::string rest = after + 1;
        const size_t host = rest.find(' ');
        rest = host == std::string::npos ? std::string() : rest.substr(host + 1);
        const size_t colon = rest.find(": ");
        lines.push_back({when, source, colon == std::string::npos ? rest : rest.substr(colon + 2)});
    }
    return lines;
}

bool parseValveLine(const std::string& line, double& when, std::string& rest) {
    // "Tue Oct 06 2026 20:44:59.045811 [Info] - HMD on, starting eye tracking"
    static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char weekday[4] = {};
    char month[4] = {};
    std::tm tm {};
    int used = 0;
    if (std::sscanf(line.c_str(), "%3s %3s %d %d %d:%d:%d%n", weekday, month, &tm.tm_mday, &tm.tm_year, &tm.tm_hour,
                    &tm.tm_min, &tm.tm_sec, &used) != 7) {
        return false;
    }
    int m = -1;
    for (int i = 0; i < 12; ++i) {
        if (std::strcmp(month, months[i]) == 0) m = i;
    }
    if (m < 0 || tm.tm_year < 2000 || tm.tm_mday < 1 || tm.tm_mday > 31) return false;
    tm.tm_mon = m;
    tm.tm_year -= 1900;
    tm.tm_isdst = -1;
    double fraction = 0.0;
    size_t at = static_cast<size_t>(used);
    if (at < line.size() && line[at] == '.') {
        double scale = 0.1;
        for (++at; at < line.size() && std::isdigit(static_cast<unsigned char>(line[at])); ++at) {
            fraction += (line[at] - '0') * scale;
            scale /= 10;
        }
    }
    const std::time_t whole = std::mktime(&tm);
    if (whole == static_cast<std::time_t>(-1)) return false;
    when = static_cast<double>(whole) + fraction;
    while (at < line.size() && line[at] == ' ') ++at;
    rest = line.substr(at);
    if (!rest.empty() && rest.back() == '\r') rest.pop_back();
    return true;
}

std::vector<LogLine> parseValveLog(const std::string& text, double from, double to) {
    std::vector<LogLine> lines;
    std::istringstream in(text);
    std::string line;
    double last = NAN;
    while (std::getline(in, line)) {
        double when = 0.0;
        std::string rest;
        if (parseValveLine(line, when, rest)) {
            last = when;
        } else {
            // A line of its own without a time: the time before it
            if (!std::isfinite(last)) continue;
            when = last;
            rest = line;
            if (!rest.empty() && rest.back() == '\r') rest.pop_back();
            if (rest.empty()) continue;
        }
        if (when >= from && when <= to) lines.push_back({when, Source::Valve, rest});
    }
    return lines;
}

std::vector<LogLine> merge(const std::vector<std::vector<LogLine>>& logs) {
    std::vector<LogLine> all;
    for (const auto& log : logs) all.insert(all.end(), log.begin(), log.end());
    std::stable_sort(all.begin(), all.end(), [](const LogLine& a, const LogLine& b) { return a.at < b.at; });
    return all;
}

std::string logsText(const std::vector<LogLine>& lines, size_t maxBytes) {
    std::vector<std::string> texts;
    size_t total = 0;
    for (const LogLine& line : lines) {
        const int ms = static_cast<int>(std::floor((line.at - std::floor(line.at)) * 1000));
        std::string text = localText(line.at, "%Y-%m-%d %H:%M:%S") + format(".%03d", std::min(999, std::max(0, ms))) +
                           " [" + sourceName(line.source) + "] " + validUtf8(line.text) + "\n";
        total += text.size();
        texts.push_back(std::move(text));
    }
    std::string out;
    if (total <= maxBytes) {
        for (const std::string& text : texts) out += text;
        return out;
    }
    // Too much: the start and the end, half each, and how many lines are left out between them
    const size_t half = maxBytes / 2 > 120 ? maxBytes / 2 - 120 : 0;
    size_t head = 0;
    size_t used = 0;
    while (head < texts.size() && used + texts[head].size() <= half) used += texts[head++].size();
    size_t tail = texts.size();
    used = 0;
    while (tail > head && used + texts[tail - 1].size() <= half) used += texts[--tail].size();
    for (size_t i = 0; i < head; ++i) out += texts[i];
    out += format("... %zu lines left out (logs.txt keeps at most %zu KB) ...\n", tail - head, maxBytes / 1024);
    for (size_t i = tail; i < texts.size(); ++i) out += texts[i];
    return out;
}

bool keyLine(const LogLine& line) {
    const std::string& s = line.text;
    if (s.empty()) return false;
    switch (line.source) {
        case Source::Core:
            // The capture requests and their answers say the same as the panel's tries; reloads say nothing
            return !startsWith(s, "Loaded ") && !contains(s, "Gaze capture") && !contains(s, "is gone; using the defaults");
        case Source::Panel:
            if (!startsWith(s, "[fit] ") && !startsWith(s, "[eyecam] ") && !startsWith(s, "[setup] ")) return false;
            return !startsWith(s, "[fit] phase ") && !startsWith(s, "[fit] asked for gaze capture") &&
                   !startsWith(s, "[fit] eye fit: started") && !startsWith(s, "[fit] re-wear fit: started") &&
                   !contains(s, "auto re-center: skipped") && !startsWith(s, "[eyecam] tab ") &&
                   !startsWith(s, "[eyecam] light") && !contains(s, "widen_sensitivity") &&
                   !startsWith(s, "[setup] password");
        case Source::Eyecam:
            if (std::isspace(static_cast<unsigned char>(s[0]))) return false;
            return contains(s, "calib") || startsWith(s, "locked") || contains(s, "lost") || contains(s, "rror") ||
                   contains(s, "stale") || contains(s, "stopped") || contains(s, "unlock");
        case Source::Valve:
            return contains(s, "HMD on") || contains(s, "HMD off") || contains(s, "ramerate") || contains(s, "[Error]") ||
                   contains(s, "[Warning]");
    }
    return false;
}

std::vector<LogLine> flowLines(const std::vector<LogLine>& lines, size_t max) {
    std::vector<LogLine> key;
    for (const LogLine& line : lines) {
        if (keyLine(line)) key.push_back(line);
    }
    if (key.size() <= max || max == 0) return max == 0 ? std::vector<LogLine>() : key;
    // The first few (what came before), and the last ones (how it ended)
    const size_t head = std::min<size_t>(3, max / 3);
    std::vector<LogLine> kept(key.begin(), key.begin() + head);
    kept.insert(kept.end(), key.end() - (max - head), key.end());
    return kept;
}

std::string flowText(const UiText& t, const LogLine& line) {
    const std::string& s = line.text;
    if (line.source == Source::Valve) {
        for (const char* level : {"[Info] - ", "[Info] "}) {
            if (startsWith(s, level)) return s.substr(std::strlen(level));
        }
        return s;
    }
    if (line.source == Source::Core) {
        double seconds = 0;
        if (std::sscanf(s.c_str(), "Eye tracking resumed after %lf s", &seconds) == 1) {
            return format(t.flowResumedFormat, seconds);
        }
        return s;
    }
    if (line.source != Source::Panel || !startsWith(s, "[fit] ")) return s;
    const std::string body = s.substr(6);
    // "[fit] eye fit, IPD 69.6 mm, dashboard open"
    for (const auto& start : {std::make_pair("eye fit, IPD ", t.flowFit), std::make_pair("re-center, IPD ", t.flowRecenter),
                              std::make_pair("re-center and tilt, IPD ", t.flowTilt)}) {
        if (!startsWith(body, start.first)) continue;
        const bool open = contains(body, "dashboard open");
        return format(t.flowStartFormat, start.second, open ? t.flowDashOpen : t.flowDashClosed);
    }
    // "[fit] put on (tracking was off 201.3 s): re-centering once the eyes settle"
    double off = 0;
    if (std::sscanf(body.c_str(), "put on (tracking was off %lf s)", &off) == 1) return format(t.flowPutOnFormat, off);
    // "[fit] target up 7.9 s: 944 frames ..."
    double up = 0;
    if (std::sscanf(body.c_str(), "target up %lf s", &up) == 1) return format(t.flowDotHiddenFormat, up);
    // "[fit] center try 1: 0 of 153 samples usable at 90 Hz (needs 45), no gaze average -> again"
    char point[16] = {};
    int attempt = 0;
    int used = 0;
    if (std::sscanf(body.c_str(), "%15s try %d: %n", point, &attempt, &used) >= 2 && used > 0) {
        const std::string word = pointWord(t, point);
        const size_t arrow = body.rfind("-> ");
        int samples = 0;
        int received = 0;
        const std::string numbers = body.substr(static_cast<size_t>(used));
        int got = std::sscanf(numbers.c_str(), "%d of %d samples", &samples, &received);
        if (got < 2) {
            got = std::sscanf(numbers.c_str(), "%d samples", &samples);
            received = -1;
        }
        if (!word.empty() && arrow != std::string::npos && got >= 1) {
            const std::string outcome = body.substr(arrow + 3);
            const char* how = startsWith(outcome, "again") ? t.flowAgain
                              : startsWith(outcome, "failed") ? t.flowFailed
                                                              : t.flowOk;
            if (received < 0) {
                // An older line without the count that came in: "128 samples"
                std::string text = format(t.flowTryFormat, word.c_str(), attempt, samples, 0, how);
                const size_t slash = text.find("/0");
                if (slash != std::string::npos) text.erase(slash, 2);
                return text;
            }
            return format(t.flowTryFormat, word.c_str(), attempt, samples, received, how);
        }
    }
    return s;
}

const char* sourceLabel(const UiText& t, Source source) {
    switch (source) {
        case Source::Core: return t.sourceCore;
        case Source::Panel: return t.sourcePanel;
        case Source::Eyecam: return t.sourceEyecam;
        case Source::Valve: return t.sourceValve;
    }
    return t.sourcePanel;
}

std::string kindLabel(const UiText& t, const Summary& summary) {
    switch (summary.kind) {
        case Kind::Fit: return t.kindFit;
        case Kind::Recenter: return summary.mode == "tilt" ? t.kindRecenterTilt : t.kindRecenter;
        case Kind::CalibWear: return t.kindCalibWear;
        case Kind::CalibUser: return t.kindCalibUser;
    }
    return t.kindFit;
}

const char* resultLabel(const UiText& t, Result result) {
    switch (result) {
        case Result::Ok: return t.resultOk;
        case Result::Failed: return t.resultFailed;
        case Result::Partial: return t.resultPartial;
    }
    return t.resultFailed;
}

std::string conditionsText(const UiText& t, const Summary& summary) {
    const Conditions& c = summary.conditions;
    std::vector<std::string> parts;
    if (summary.trigger == "auto") parts.push_back(t.condAuto);
    if (c.dashboardOpen >= 0) parts.push_back(c.dashboardOpen > 0 ? t.condDashOpen : t.condDashClosed);
    if (std::isfinite(c.cameraFps)) parts.push_back(format(t.condCameraFormat, c.cameraFps));
    if (std::isfinite(c.sincePutOnSec)) parts.push_back(format(t.condPutOnFormat, c.sincePutOnSec));
    std::string out;
    for (const std::string& part : parts) out += (out.empty() ? "" : t.condSeparator) + part;
    return out;
}

std::string summaryJson(const Summary& s) {
    const auto object = []() {
        JsonValue v;
        v.type = JsonValue::Type::Object;
        return v;
    };
    const auto ms = [](double when) { return JsonValue::makeNumber(std::round(when * 1000), true); };
    JsonValue root = object();
    root.set("format", JsonValue::makeNumber(1, true));
    root.set("kind", JsonValue::makeString(kindName(s.kind)));
    root.set("trigger", JsonValue::makeString(s.trigger));
    root.set("mode", s.mode.empty() ? JsonValue::makeNull() : JsonValue::makeString(s.mode));
    root.set("start", JsonValue::makeString(localText(s.start, "%Y-%m-%d %H:%M:%S")));
    root.set("start_ms", ms(s.start));
    root.set("end", JsonValue::makeString(localText(s.end, "%Y-%m-%d %H:%M:%S")));
    root.set("end_ms", ms(s.end));
    root.set("seconds", numberOrNull(s.end - s.start, 1));
    root.set("result", JsonValue::makeString(resultName(s.result)));
    root.set("reason", JsonValue::makeString(validUtf8(s.reason)));
    root.set("reason_en", JsonValue::makeString(validUtf8(s.reasonEn)));
    root.set("brief", JsonValue::makeString(validUtf8(s.brief)));
    root.set("brief_en", JsonValue::makeString(validUtf8(s.briefEn)));
    JsonValue versions = object();
    versions.set("frameeyeosc", JsonValue::makeString(s.version));
    versions.set("steamos", s.steamos.empty() ? JsonValue::makeNull() : JsonValue::makeString(s.steamos));
    root.set("versions", versions);
    root.set("code", JsonValue::makeString(s.code));
    const Conditions& c = s.conditions;
    JsonValue conditions = object();
    conditions.set("dashboard_open", c.dashboardOpen < 0 ? JsonValue::makeNull() : JsonValue::makeBool(c.dashboardOpen > 0));
    conditions.set("camera_fps", numberOrNull(c.cameraFps, 0));
    conditions.set("tracker_rate", numberOrNull(c.trackerRate, 0));
    conditions.set("tracker_missed", numberOrNull(c.trackerMissed, 0));
    conditions.set("seconds_since_put_on", numberOrNull(c.sincePutOnSec, 1));
    conditions.set("tracking_off_before_s", numberOrNull(c.offBeforeSec, 1));
    conditions.set("ipd_mm", numberOrNull(c.ipdMm, 1));
    root.set("conditions", conditions);
    JsonValue flow;
    flow.type = JsonValue::Type::Array;
    for (const LogLine& line : s.flow) {
        JsonValue item = object();
        item.set("time", JsonValue::makeString(localText(line.at, "%H:%M:%S")));
        item.set("unix_ms", ms(line.at));
        item.set("source", JsonValue::makeString(sourceName(line.source)));
        item.set("text", JsonValue::makeString(validUtf8(line.text)));
        flow.items.push_back(item);
    }
    root.set("flow", flow);
    return writeJson(root);
}

bool parseSummary(const std::string& text, Summary& s) {
    JsonValue root;
    std::string error;
    if (!parseJson(text, root, error) || !root.isObject()) return false;
    if (!parseKind(textOf(root, "kind"), s.kind) || !parseResult(textOf(root, "result"), s.result)) return false;
    s.trigger = textOf(root, "trigger");
    if (s.trigger.empty()) s.trigger = "panel";
    s.mode = textOf(root, "mode");
    s.start = numberOf(root, "start_ms") / 1000;
    s.end = numberOf(root, "end_ms") / 1000;
    if (!std::isfinite(s.start)) return false;
    if (!std::isfinite(s.end)) s.end = s.start;
    s.reason = textOf(root, "reason");
    s.reasonEn = textOf(root, "reason_en");
    s.brief = textOf(root, "brief");
    s.briefEn = textOf(root, "brief_en");
    if (const JsonValue* v = root.get("versions"); v != nullptr && v->isObject()) {
        s.version = textOf(*v, "frameeyeosc");
        s.steamos = textOf(*v, "steamos");
    }
    s.code = textOf(root, "code");
    s.conditions = Conditions();
    if (const JsonValue* c = root.get("conditions"); c != nullptr && c->isObject()) {
        const JsonValue* open = c->get("dashboard_open");
        s.conditions.dashboardOpen = open != nullptr && open->isBool() ? (open->boolean ? 1 : 0) : -1;
        s.conditions.cameraFps = numberOf(*c, "camera_fps");
        s.conditions.trackerRate = numberOf(*c, "tracker_rate");
        s.conditions.trackerMissed = numberOf(*c, "tracker_missed");
        s.conditions.sincePutOnSec = numberOf(*c, "seconds_since_put_on");
        s.conditions.offBeforeSec = numberOf(*c, "tracking_off_before_s");
        s.conditions.ipdMm = numberOf(*c, "ipd_mm");
    }
    s.flow.clear();
    if (const JsonValue* flow = root.get("flow"); flow != nullptr && flow->isArray()) {
        for (const JsonValue& item : flow->items) {
            if (!item.isObject()) continue;
            LogLine line;
            line.at = numberOf(item, "unix_ms") / 1000;
            if (!std::isfinite(line.at) || !parseSource(textOf(item, "source"), line.source)) continue;
            line.text = textOf(item, "text");
            s.flow.push_back(line);
        }
    }
    return true;
}

std::string reportText(const UiText& t, const Summary& s, const std::string& folderShown) {
    const bool en = english(t);
    const size_t labelW = 10;
    std::string out;
    const auto row = [&](const char* label, const std::string& value) { out += pad(label, labelW) + value + "\n"; };
    out += std::string(t.reportTitle) + "  " + s.folder + "\n";
    // What ran, and how ("目を合わせる（ダッシュボードを開いたまま）")
    {
        std::vector<std::string> parts;
        if (s.trigger == "auto") parts.push_back(t.condAuto);
        if (s.conditions.dashboardOpen >= 0) parts.push_back(s.conditions.dashboardOpen > 0 ? t.condDashOpen : t.condDashClosed);
        std::string aside;
        for (const std::string& part : parts) aside += (aside.empty() ? "" : t.condSeparator) + part;
        row(t.reportKind, kindLabel(t, s) + (aside.empty() ? std::string() : format(t.reportParenFormat, aside.c_str())));
    }
    {
        const bool sameDay = localText(s.start, "%Y-%m-%d") == localText(s.end, "%Y-%m-%d");
        row(t.reportTime, localText(std::round(s.start), "%Y-%m-%d %H:%M:%S") + " – " +
                              localText(std::round(s.end), sameDay ? "%H:%M:%S" : "%Y-%m-%d %H:%M:%S") +
                              format(t.reportSecondsFormat, std::max(0.0, s.end - s.start)));
    }
    {
        const std::string& reason = en && !s.reasonEn.empty() ? s.reasonEn : s.reason;
        row(t.reportResult, std::string(resultLabel(t, s.result)) + (reason.empty() ? "" : ": " + reason));
    }
    row(t.reportVersions, "frameeyeosc " + (s.version.empty() ? std::string("?") : s.version) +
                              (s.steamos.empty() ? std::string() : " / SteamOS " + s.steamos));
    row(t.reportCode, s.code.empty() ? std::string("—") : s.code);
    {
        const Conditions& c = s.conditions;
        std::vector<std::string> parts;
        if (std::isfinite(c.trackerRate)) {
            std::string tracker = format(t.reportTrackerFormat, c.trackerRate);
            if (std::isfinite(c.trackerMissed)) tracker += format(t.reportMissedFormat, c.trackerMissed);
            parts.push_back(tracker);
        }
        if (std::isfinite(c.cameraFps)) parts.push_back(format(t.condCameraFormat, c.cameraFps));
        std::string line;
        for (const std::string& part : parts) line += (line.empty() ? "" : t.condSeparator) + part;
        if (!line.empty()) row(t.reportEyeData, line);
        std::string before;
        if (std::isfinite(c.sincePutOnSec)) before = format(t.condPutOnFormat, c.sincePutOnSec);
        if (std::isfinite(c.offBeforeSec) && c.offBeforeSec > 0) before += format(t.reportOffFormat, c.offBeforeSec);
        if (!before.empty()) row(t.reportBefore, before);
    }
    out += "\n";
    {
        const std::string head = std::string("── ") + t.reportFlow + " ";
        const size_t used = columns(head);
        std::string rule;
        for (size_t i = used; i < 52; ++i) rule += "─";
        out += head + rule + "\n";
    }
    if (s.flow.empty()) out += std::string(t.recordFlowNone) + "\n";
    for (const LogLine& line : s.flow) {
        out += localText(line.at, "%H:%M:%S") + " " + pad(sourceLabel(t, line.source), 8) + line.text + "\n";
    }
    out += "\n";
    row(t.reportFiles, folderShown);
    // A text file: any icon marker from the panel's texts as a plain character
    return icon::plain(out);
}

std::string compactJson(const std::string& text) {
    JsonValue root;
    std::string error;
    if (!parseJson(text, root, error) || !root.isObject()) return "";
    std::string out;
    out.reserve(text.size());
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            out += c;
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '"') inString = true;
        out += c;
    }
    return out;
}

bool plainJson(const std::string& text) {
    if (text.find('\0') != std::string::npos) return false;
    JsonValue root;
    std::string error;
    return parseJson(text, root, error) && root.isObject();
}

Sources systemSources() {
    Sources s;
    s.journal = [](const std::string& unit, double since, double until) {
        const std::vector<std::string> argv = {
            "journalctl", "--user",     "-u",       unit,        "--since", "@" + std::to_string(static_cast<long long>(std::floor(since))),
            "--until",    "@" + std::to_string(static_cast<long long>(std::ceil(until))),
            "-o",         "short-unix", "--no-pager", "-q"};
        const CommandResult result = runCommand(argv, 5000);
        if (!result.ok() && result.out.empty()) {
            std::fprintf(stderr, "[report] %s\n", describeCommand(argv, result).c_str());
        }
        std::string out = result.out;
        if (out.size() > kMaxJournalBytes) out.resize(kMaxJournalBytes);
        return out;
    };
    const char* home = std::getenv("HOME");
    const std::string h = home != nullptr ? home : "";
    if (!h.empty()) {
        s.valveLog = h + "/.local/share/Steam/logs/eyetracking.txt";
        s.eyecamHome = h + "/eyecam";
    }
    s.waitUntil = [](double when) {
        for (;;) {
            const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            if (now >= when) return;
            std::this_thread::sleep_for(std::chrono::duration<double>(std::min(0.5, when - now)));
        }
    };
    return s;
}

bool writeRecord(const std::string& dir, Pending pending, const Sources& sources, std::string& folder,
                 std::string& error) {
    Summary& s = pending.summary;
    if (sources.waitUntil) sources.waitUntil(s.end + kSettleSec);
    // The logs of the window, merged
    const double from = s.start - kBeforeSec;
    const double to = s.end + kAfterSec;
    std::vector<std::vector<LogLine>> logs;
    if (sources.journal) {
        for (const Unit& unit : kUnits) {
            std::vector<LogLine> lines = parseJournal(sources.journal(unit.name, from, to), unit.source);
            lines.erase(std::remove_if(lines.begin(), lines.end(),
                                       [&](const LogLine& l) { return l.at < from || l.at > to; }),
                        lines.end());
            logs.push_back(std::move(lines));
        }
    }
    if (!sources.valveLog.empty()) logs.push_back(parseValveLog(readTail(sources.valveLog, kMaxValveBytes), from, to));
    const std::vector<LogLine> merged = merge(logs);
    s.flow = flowLines(merged);

    // Its folder (a new name if one of that second is there)
    if (!makeDirs(dir)) {
        error = "can't create " + dir + ": " + std::strerror(errno);
        return false;
    }
    folder = folderName(s.kind, s.start);
    for (int n = 2; isDir(dir + "/" + folder); ++n) folder = folderName(s.kind, s.start) + "_" + std::to_string(n);
    const std::string path = dir + "/" + folder;
    if (::mkdir(path.c_str(), 0700) != 0) {
        error = "can't create " + path + ": " + std::strerror(errno);
        return false;
    }
    s.folder = folder;

    bool ok = writeFile(path + "/summary.json", summaryJson(s));
    if (!pending.statusLines.empty()) ok &= writeFile(path + "/status.jsonl", pending.statusLines);
    ok &= writeFile(path + "/logs.txt", logsText(merged));
    // A calibration's numbers: calib_result.json from eyecam's folder of it (a calib_* folder only; never its images
    // or calib_samples.csv)
    if (s.kind == Kind::CalibWear || s.kind == Kind::CalibUser) {
        std::string calib = pending.calibDir;
        const size_t slash = calib.find_last_of('/');
        if (calib.empty() || !startsWith(calib.substr(slash == std::string::npos ? 0 : slash + 1), "calib_")) {
            calib = findCalibDir(sources.eyecamHome, s.start, s.end);
        }
        std::string text;
        if (!calib.empty() && readFile(calib + "/calib_result.json", text, kMaxCalibResultBytes + 1) &&
            text.size() <= kMaxCalibResultBytes && plainJson(text)) {
            ok &= writeFile(path + "/calib_result.json", text);
        }
    }
    ok &= writeFile(path + "/report.txt",
                    reportText(uiText(pending.language), s, shortPathOf(path) + "/"));
    if (!ok) error = "couldn't write every file in " + path;
    prune(dir, kKeep);
    return ok;
}

std::string shortPathOf(const std::string& path) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return path;
    const std::string prefix = std::string(home) + "/";
    return path.rfind(prefix, 0) == 0 ? "~/" + path.substr(prefix.size()) : path;
}

int prune(const std::string& dir, int keep) {
    const std::vector<std::string> names = folders(dir);
    int deleted = 0;
    for (size_t i = static_cast<size_t>(std::max(0, keep)); i < names.size(); ++i) {
        const std::string path = dir + "/" + names[i];
        // Only files in it (as written): a folder with anything else is left alone
        const std::vector<std::string> files = entries(path);
        const bool plain = std::all_of(files.begin(), files.end(), [&](const std::string& name) {
            struct stat st {};
            return ::lstat((path + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode);
        });
        if (!plain) continue;
        for (const std::string& name : files) ::unlink((path + "/" + name).c_str());
        if (::rmdir(path.c_str()) == 0) ++deleted;
    }
    return deleted;
}

std::vector<std::string> folders(const std::string& dir) {
    std::vector<std::string> names;
    for (const std::string& name : entries(dir)) {
        if (isFolderName(name) && isDir(dir + "/" + name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        const std::string sa = folderStamp(a);
        const std::string sb = folderStamp(b);
        // Newest first; "_2" after the same second's first
        if (sa.substr(0, 19) != sb.substr(0, 19)) return sa.substr(0, 19) > sb.substr(0, 19);
        if (sa.size() != sb.size()) return sa.size() > sb.size();
        return a > b;
    });
    return names;
}

std::vector<Summary> list(const std::string& dir) {
    std::vector<Summary> out;
    for (const std::string& name : folders(dir)) {
        Summary s;
        if (load(dir, name, s)) out.push_back(s);
    }
    return out;
}

bool load(const std::string& dir, const std::string& folder, Summary& summary, std::vector<FileInfo>* files) {
    if (!isFolderName(folder)) return false;
    const std::string path = dir + "/" + folder;
    if (files != nullptr) {
        files->clear();
        for (const char* name : kFileNames) {
            struct stat st {};
            if (::stat((path + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                files->push_back({name, static_cast<long long>(st.st_size)});
            }
        }
    }
    std::string text;
    if (!readFile(path + "/summary.json", text, 1024 * 1024)) return false;
    if (!parseSummary(text, summary)) return false;
    summary.folder = folder;
    return true;
}

std::string sizeText(long long bytes) {
    if (bytes < 1024) return format("%lld B", bytes);
    if (bytes < 1024 * 1024) return format("%lld KB", (bytes + 1023) / 1024);
    return format("%.1f MB", bytes / (1024.0 * 1024.0));
}

std::string listText(const UiText& t, const std::vector<Summary>& records) {
    const bool en = english(t);
    size_t folderW = 0;
    size_t resultW = 0;
    size_t kindW = 0;
    for (const Summary& s : records) {
        folderW = std::max(folderW, columns(s.folder));
        resultW = std::max(resultW, columns(resultLabel(t, s.result)));
        kindW = std::max(kindW, columns(kindLabel(t, s)));
    }
    std::string out;
    for (const Summary& s : records) {
        const std::string& brief = en && !s.briefEn.empty() ? s.briefEn : s.brief;
        const std::string& reason = en && !s.reasonEn.empty() ? s.reasonEn : s.reason;
        std::string line = pad(s.folder, folderW + 2) + pad(resultLabel(t, s.result), resultW + 2) +
                           pad(kindLabel(t, s), kindW + 2) + (brief.empty() ? reason : brief);
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line + "\n";
    }
    return icon::plain(out);
}

int cliReport(const UiText& t, const std::string& dir, const std::string& what, std::string& out) {
    const std::string shown = shortPathOf(dir);
    if (what == "list") {
        const std::vector<Summary> records = list(dir);
        out = records.empty() ? std::string(t.recordsNone) + " (" + shown + ")\n" : listText(t, records);
        return 0;
    }
    std::string folder = what;
    if (what == "latest") {
        const std::vector<std::string> names = folders(dir);
        if (names.empty()) {
            out = std::string(t.recordsNone) + " (" + shown + ")\n";
            return 1;
        }
        folder = names.front();
    }
    while (!folder.empty() && folder.back() == '/') folder.pop_back();
    const size_t slash = folder.find_last_of('/');
    if (slash != std::string::npos) folder = folder.substr(slash + 1);
    Summary summary;
    if (!isFolderName(folder) || !isDir(dir + "/" + folder)) {
        out = std::string(t.recordMissing) + ": " + what + "\n";
        return 1;
    }
    std::string text;
    if (readFile(dir + "/" + folder + "/report.txt", text, 1024 * 1024) && !text.empty()) {
        out = text;
        return 0;
    }
    // No report.txt (removed by hand): made again from summary.json
    if (!load(dir, folder, summary)) {
        out = std::string(t.recordMissing) + ": " + what + "\n";
        return 1;
    }
    out = reportText(t, summary, shown + "/" + folder + "/");
    return 0;
}

void Run::begin(Kind kind, double start) {
    pending_ = Pending();
    pending_.summary.kind = kind;
    pending_.summary.start = start;
    pending_.summary.end = start;
    lastSample_ = -1e18;
    active_ = true;
}

bool Run::sample(double when, const std::string& coreStatus, const std::string& eyecamStatus) {
    if (!active_ || when < lastSample_ + 0.95) return false;
    lastSample_ = when;
    const std::string core = compactJson(coreStatus);
    const std::string eyecam = compactJson(eyecamStatus);
    const std::string line = format("{\"t\":%.3f,\"frameeyeosc\":", when) + (core.empty() ? "null" : core) +
                             ",\"eyecam\":" + (eyecam.empty() ? "null" : eyecam) + "}\n";
    if (pending_.statusLines.size() + line.size() > kMaxStatusBytes) return false;
    pending_.statusLines += line;
    return true;
}

Pending Run::finish(double end) {
    active_ = false;
    pending_.summary.end = std::max(end, pending_.summary.start);
    Pending out = std::move(pending_);
    pending_ = Pending();
    return out;
}

Writer::Writer(std::string dir, Sources sources) : dir_(std::move(dir)), sources_(std::move(sources)) {
    thread_ = std::thread([this]() { loop(); });
}

Writer::~Writer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Writer::submit(Pending pending) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(pending));
    }
    wake_.notify_all();
}

bool Writer::poll(std::string& folder, Kind& kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (done_.empty()) return false;
    folder = done_.front().folder;
    kind = done_.front().kind;
    done_.pop_front();
    return true;
}

bool Writer::busy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return working_ || !queue_.empty();
}

void Writer::loop() {
    for (;;) {
        Pending pending;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stopped, nothing left
            pending = std::move(queue_.front());
            queue_.pop_front();
            working_ = true;
        }
        const Kind kind = pending.summary.kind;
        std::string folder;
        std::string error;
        const bool ok = writeRecord(dir_, std::move(pending), sources_, folder, error);
        if (ok) {
            std::fprintf(stderr, "[report] saved %s/%s\n", shortPathOf(dir_).c_str(), folder.c_str());
        } else {
            std::fprintf(stderr, "[report] %s\n", error.c_str());
        }
        std::lock_guard<std::mutex> lock(mutex_);
        working_ = false;
        if (!folder.empty()) done_.push_back({folder, kind});
    }
}

}  // namespace report
