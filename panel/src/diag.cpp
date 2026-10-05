// The diagnostics page's texts and code (see diag.h).
#include "diag.h"

#include "config.h"
#include "eyecam.h"
#include "model.h"

#include <sys/stat.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

namespace diag {

namespace {

/** The separator between the code's parts and between the values in a row. */
constexpr const char* kDot = "·";

/**
 * A printf format with its arguments, as long as it comes out (never cut, so never inside a UTF-8 character).
 * @param format the format
 * @param args its arguments
 * @return the text
 */
template <typename... Args>
std::string format(const char* format, Args... args) {
    const int n = std::snprintf(nullptr, 0, format, args...);
    if (n <= 0) return "";
    std::string text(static_cast<size_t>(n) + 1, '\0');
    std::snprintf(text.data(), text.size(), format, args...);
    text.resize(static_cast<size_t>(n));
    return text;
}

/**
 * A number with no decimals, or "—" when it isn't one.
 * @param value the number
 * @return the text
 */
std::string whole(double value) {
    return std::isfinite(value) ? format("%.0f", value) : std::string("—");
}

/**
 * A count a second as shown: no decimals, never "-0".
 * @param value the count
 * @return the number to format
 */
double count(double value) {
    return std::max(0.0, value);
}

/**
 * A share 0..1 as a percentage ("87%"), or "—".
 * @param share the share
 * @return the text
 */
std::string percent(double share) {
    return std::isfinite(share) ? format("%.0f%%", share * 100) : std::string("—");
}

/**
 * Local "MM/DD HH:MM" of a Unix time.
 * @param seconds Unix seconds
 * @return the text ("" for 0)
 */
std::string localTime(double seconds) {
    if (!(seconds > 0)) return "";
    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm tm {};
    localtime_r(&t, &tm);
    char text[32];
    std::strftime(text, sizeof(text), "%m/%d %H:%M", &tm);
    return text;
}

/**
 * "MM/DD HH:MM" of a local "YYYY-MM-DD HH:MM:SS".
 * @param stamp the time as eyecam-rec writes it
 * @return the text (the stamp as it is if it isn't one)
 */
std::string shortStamp(const std::string& stamp) {
    if (stamp.size() < 16 || stamp[4] != '-' || stamp[7] != '-' || stamp[13] != ':') return stamp;
    return stamp.substr(5, 2) + "/" + stamp.substr(8, 2) + " " + stamp.substr(11, 5);
}

/**
 * When something happened and what: "MM/DD HH:MM · text" (the time first, so a long text cut short keeps it).
 * @param t the texts
 * @param text the text
 * @param seconds when (Unix seconds)
 * @return the line
 */
std::string withTime(const UiText& t, const std::string& text, double seconds) {
    const std::string when = localTime(seconds);
    return when.empty() ? text : format(t.diagWithTimeFormat, when.c_str(), text.c_str());
}

/**
 * The parts joined with " · ".
 * @param parts the parts
 * @return the text
 */
std::string joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
        if (part.empty()) continue;
        if (!out.empty()) out += std::string(" ") + kDot + " ";
        out += part;
    }
    return out;
}

/** The eye video's state, as the code's first part says it. */
enum class Video { Flowing, NotWorn, NoVideo, OneEye, Searching, LiveOff, WaitingTool, NoRecorder };

/**
 * The eye video's state.
 * @param view eyecam-rec
 * @return the state
 */
Video videoState(const eyecam::View& view) {
    const eyecam::Status& s = view.status;
    if (!view.visible || !s.present) return Video::NoRecorder;
    if (s.state == eyecam::State::WaitingFds) return Video::WaitingTool;
    if (eyecam::videoFlowing(s)) return Video::Flowing;
    switch (eyecam::searchReason(s)) {
        case eyecam::Search::NotWorn: return Video::NotWorn;
        case eyecam::Search::NoVideo: return Video::NoVideo;
        case eyecam::Search::OneEye: return Video::OneEye;
        case eyecam::Search::None: break;
    }
    if (!s.live && s.state == eyecam::State::Idle) return Video::LiveOff;
    return Video::Searching;
}

/** frameeyeosc's state, as the code's last part says it. */
enum class Core { Sending, WaitingData, NoTarget, Paused, Error, NotRunning };

/**
 * frameeyeosc's state.
 * @param s its status
 * @return the state
 */
Core coreState(const EyeStatus& s) {
    if (!s.running) return Core::NotRunning;
    if (!s.sourceError.empty() || !s.configError.empty()) return Core::Error;
    if (!s.sending) return Core::Paused;
    if (s.target.empty()) return Core::NoTarget;
    if (!s.tracking) return Core::WaitingData;
    return Core::Sending;
}

/**
 * The proximity reading as shown ("2.9"), or "" when unknown.
 * @param s eyecam-rec's status
 * @return the text
 */
std::string proxText(const eyecam::Status& s) {
    if (!s.present || !std::isfinite(s.prox) || s.prox < 0) return "";
    return format("%.1f", s.prox);
}

/**
 * The last look's row: candidates, refreshes a second, slots, eyes, where it stopped, and the changed blocks.
 * @param t the texts
 * @param d the look
 * @return the text
 */
std::string searchText(const UiText& t, const eyecam::SearchDetail& d) {
    std::vector<std::string> parts {format(t.diagCandidatesFormat, d.candidates)};
    const std::string& stop = d.stoppedAt;
    // (it stops before measuring the refreshes there)
    const bool measured = stop != "no_candidates" && stop != "split_buffers";
    if (measured && std::isfinite(d.refreshHz)) parts.push_back(format(t.diagHzFormat, d.refreshHz));
    if (stop.empty() || stop == "one_eye") {
        parts.push_back(format(t.diagSlotsFormat, d.slots));
        parts.push_back(d.bothEyes ? t.diagBothEyes : t.diagOneEye);
    } else if (stop == "no_candidates") {
        parts.push_back(t.diagStopNoCandidates);
    } else if (stop == "split_buffers") {
        parts.push_back(t.diagStopSplitBuffers);
    } else if (stop == "not_refreshing") {
        parts.push_back(t.diagStopNotRefreshing);
    } else if (stop == "few_slots") {
        parts.push_back(t.diagStopFewSlots);
    } else {
        parts.push_back(stop);
    }
    return joined(parts);
}

/**
 * The camera tool's checksum as shown: the copy eyecam-rec runs when it can be read, with install.sh's beside it when
 * that differs; otherwise install.sh's, "unreadable" or "no file".
 * @param t the texts
 * @param s what the panel read
 * @return the text
 */
std::string toolHash(const UiText& t, const System& s) {
    if (!s.installedHash.empty()) {
        return s.grabHash.empty() || s.grabHash == s.installedHash
                   ? s.installedHash
                   : format(t.diagToolBundledFormat, s.installedHash.c_str(), s.grabHash.c_str());
    }
    if (!s.grabHash.empty()) return s.grabHash;
    return s.grabUnreadable ? t.diagUnreadable : t.diagToolMissing;
}

/**
 * A message from eyecam-rec in the panel's language.
 * @param ja the Japanese
 * @param en the English ("" when it has none)
 * @param language the panel's
 * @return the text
 */
const std::string& inLanguage(const std::string& ja, const std::string& en, Language language) {
    return language == Language::En && !en.empty() ? en : ja;
}

}  // namespace

std::string steamosVersion(const std::string& text) {
    std::string version;
    std::string build;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        if (!value.empty() && value.back() == '\r') value.pop_back();
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        if (key == "VERSION_ID") version = value;
        if (key == "BUILD_ID") build = value;
    }
    if (version.empty()) return "";
    return build.empty() ? version : version + " (" + build + ")";
}

std::string readSteamos(const std::string& path) {
    std::ifstream file(path);
    if (!file) return "";
    return steamosVersion(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
}

std::string sha256Hex(const std::string& bytes) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    // The message with its padding: 0x80, zeros up to 56 mod 64, then the bit length (big-endian)
    std::string data = bytes;
    const uint64_t bits = static_cast<uint64_t>(bytes.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) data.push_back('\0');
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
    uint32_t w[64];
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        for (int i = 0; i < 16; ++i) {
            const auto byte = [&](int j) { return static_cast<uint32_t>(static_cast<unsigned char>(data[chunk + i * 4 + j])); };
            w[i] = byte(0) << 24 | byte(1) << 16 | byte(2) << 8 | byte(3);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    std::string hex;
    for (uint32_t word : h) hex += format("%08x", word);
    return hex;
}

std::string defaultGrabPath() {
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : "") + "/.local/lib/eyecam/eyecam-grab";
}

const std::string& FileHash::get(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        path_ = path;
        size_ = mtimeNs_ = ctimeNs_ = -1;
        hash_.clear();
        unreadable_ = false;
        return hash_;
    }
    const auto ns = [](const timespec& time) {
        return static_cast<long long>(time.tv_sec) * 1000000000LL + time.tv_nsec;
    };
    const long long mtimeNs = ns(st.st_mtim);
    const long long ctimeNs = ns(st.st_ctim);
    if (path == path_ && st.st_size == size_ && mtimeNs == mtimeNs_ && ctimeNs == ctimeNs_) return hash_;
    path_ = path;
    std::ifstream file(path, std::ios::binary);
    std::string bytes;
    if (file.is_open()) bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    // Not opened (permissions) or failed while reading: nothing kept, so it is tried again next time
    unreadable_ = !file.is_open() || file.bad();
    if (unreadable_) {
        size_ = mtimeNs_ = ctimeNs_ = -1;
        hash_.clear();
        return hash_;
    }
    size_ = st.st_size;
    mtimeNs_ = mtimeNs;
    ctimeNs_ = ctimeNs;
    hash_ = sha256Hex(bytes).substr(0, 8);
    return hash_;
}

std::string code(const PanelModel& m) {
    const eyecam::Status& s = m.eyecam.status;
    const bool recorder = m.eyecam.visible && s.present;
    std::string out;
    switch (videoState(m.eyecam)) {
        case Video::Flowing: out = "OK"; break;
        case Video::NotWorn: out = "NW"; break;
        case Video::NoVideo: out = "NV"; break;
        case Video::OneEye: out = "OE"; break;
        case Video::Searching: out = "SR"; break;
        case Video::LiveOff: out = "LO"; break;
        case Video::WaitingTool: out = "WF"; break;
        case Video::NoRecorder: out = "NR"; break;
    }
    // (a reading rounds to a whole number; -0 shows as 0)
    const std::string prox = recorder ? proxText(s) : "";
    out += std::string(kDot) + "P" + (prox.empty() ? "-" : format("%.0f", std::fabs(std::round(s.prox))));
    const bool looked = recorder && s.searchDetail.present && s.searchDetail.changedBlocks >= 0;
    out += std::string(kDot) + "B" + (looked ? std::to_string(s.searchDetail.changedBlocks) : "-");
    out += std::string(kDot) + "G" + (recorder && (s.hasBuffers || s.autoGrab == "ok") ? "1" : "0");
    const eyecam::LastCalib& c = s.lastCalib;
    std::string calib = "-";
    if (recorder && c.present) {
        calib = c.ok ? "1" : "0";
        if (c.ok && (c.failedEye == "L" || c.failedEye == "R")) calib += c.failedEye;
    }
    out += std::string(kDot) + "C" + calib;
    switch (coreState(m.status)) {
        case Core::Sending: out += std::string(kDot) + "F1"; break;
        case Core::WaitingData: out += std::string(kDot) + "FW"; break;
        case Core::NoTarget: out += std::string(kDot) + "FT"; break;
        case Core::Paused: out += std::string(kDot) + "FP"; break;
        case Core::Error: out += std::string(kDot) + "FE"; break;
        case Core::NotRunning: out += std::string(kDot) + "F0"; break;
    }
    return out;
}

std::vector<Card> cards(const UiText& t, const PanelModel& m) {
    const EyeStatus& s = m.status;
    const eyecam::View& view = m.eyecam;
    const eyecam::Status& e = view.status;
    const bool recorder = view.visible && e.present;
    const std::string unknown = "—";
    std::vector<Card> out;

    // Versions: frameeyeosc, SteamOS, the camera tool (as eyecam-rec runs it, and its checksum), the output
    {
        Card card {t.diagCardVersions, {}};
        std::string version = m.update.current;
        if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) version = version.substr(1);
        card.rows.push_back({"frameeyeosc", version.empty() ? unknown : version});
        card.rows.push_back({"SteamOS", m.system.steamos.empty() ? unknown : m.system.steamos});
        std::string grab = recorder && !e.autoGrab.empty() ? e.autoGrab : unknown;
        if (recorder && e.grabOutdated) grab += std::string(" ") + kDot + " " + t.diagToolOutdated;
        grab += std::string(" ") + kDot + " " + toolHash(t, m.system);
        const bool grabBad = recorder && (e.autoGrab.rfind("failed", 0) == 0 || e.autoGrab == "too_old" ||
                                          e.autoGrab == "missing" || e.autoGrab.rfind("unsafe", 0) == 0);
        card.rows.push_back({t.diagRowTool, grab, grabBad});
        const SettingsView settings(m);
        const std::string output = settings.text(key::kOutput);
        const std::string name = output == kOutputLivelink ? t.outputLivelink
                                 : output == kOutputEtvr   ? t.outputEtvr
                                                           : t.outputVrchat;
        const std::string host = settings.text(key::kHost);
        const std::string mode = host == "auto" || host.empty() ? t.diagModeAuto : std::string(t.diagModeFixed) + " " + host;
        card.rows.push_back({t.diagRowOutput, name + " " + kDot + " " + mode});
        out.push_back(card);
    }

    // Eye data: frameeyeosc's rates, the openness cap, where the eyelids come from, and its error
    {
        Card card {t.diagCardEyeData, {}};
        if (s.running) {
            card.rows.push_back({t.diagRowGaze,
                                 joined({std::isfinite(s.trackerRate) ? format(t.diagRateFormat, count(s.trackerRate)) : unknown,
                                         std::isfinite(s.missedRate) ? format(t.diagMissedFormat, count(s.missedRate))
                                                                     : ""}),
                                 std::isfinite(s.trackerRate) && s.tracking && s.trackerRate < kLowTrackerRate});
            card.rows.push_back(
                {t.diagRowSend,
                 s.sending ? joined({format(t.diagRateFormat, count(s.rate)),
                                     std::isfinite(s.droppedRate) ? format(t.diagDroppedFormat, count(s.droppedRate))
                                                                  : ""})
                           : std::string(t.diagPaused)});
            card.rows.push_back({t.diagRowCap, s.opennessSaturated ? t.diagCapOn : t.diagCapOff});
            const bool left = s.camera.used[0];
            const bool right = s.camera.used[1];
            card.rows.push_back({t.diagRowLids, left && right ? t.diagLidsBoth
                                                : left        ? t.diagLidsLeft
                                                : right       ? t.diagLidsRight
                                                              : t.diagLidsValve});
        } else {
            for (const char* label : {t.diagRowGaze, t.diagRowSend, t.diagRowCap, t.diagRowLids}) {
                card.rows.push_back({label, unknown});
            }
        }
        // frameeyeosc's error: what is wrong now first, then the last one it logged (with its time)
        Row error {t.diagRowCoreError, t.diagNone};
        switch (coreState(s)) {
            case Core::NotRunning: error = {t.diagRowCoreError, t.diagCoreNotRunning, true}; break;
            case Core::Error:
                error = {t.diagRowCoreError,
                         !s.sourceError.empty() ? t.sourceErrorPrefix + s.sourceError : t.errorPrefix + s.configError,
                         true};
                break;
            case Core::NoTarget: error = {t.diagRowCoreError, t.diagCoreNoTarget, true}; break;
            default:
                if (!s.lastError.empty()) error.value = withTime(t, s.lastError, s.lastErrorTime);
                break;
        }
        card.rows.push_back(error);
        out.push_back(card);
    }

    // Eye cameras: the video, the last look, the proximity sensor, the pupils and the processing time
    {
        Card card {t.diagCardCameras, {}};
        Row state {t.diagRowState, ""};
        switch (videoState(view)) {
            case Video::Flowing:
                state.value = e.searchDetail.present && e.searchDetail.slots > 0
                                  ? format(t.diagVideoSlotsFormat, e.searchDetail.slots)
                                  : std::string(t.diagVideo);
                break;
            case Video::NotWorn: state = {t.diagRowState, t.diagNotWorn, true}; break;
            case Video::NoVideo: state = {t.diagRowState, t.diagNoVideo, true}; break;
            case Video::OneEye: state = {t.diagRowState, t.diagOneEyeOnly, true}; break;
            case Video::Searching: state.value = t.diagSearching; break;
            case Video::LiveOff: state.value = t.diagLiveOff; break;
            case Video::WaitingTool: state = {t.diagRowState, t.diagWaitingTool, true}; break;
            case Video::NoRecorder: state.value = t.diagNoRecorder; break;
        }
        if (recorder && e.state == eyecam::State::Error) state.value += std::string(" ") + kDot + " " + t.diagError;
        card.rows.push_back(state);
        if (recorder) {
            const eyecam::SearchDetail& look = e.searchDetail;
            // (an older eyecam-rec doesn't say)
            card.rows.push_back({t.diagRowSearch, look.present ? searchText(t, look)
                                                  : look.known ? std::string(t.diagNotLooked)
                                                               : unknown});
            // (0: nothing is being written into the buffers at all)
            card.rows.push_back({t.diagRowBlocks, look.present && look.changedBlocks >= 0
                                                      ? format(t.diagChangedFormat, look.changedBlocks)
                                                      : unknown,
                                 look.present && look.changedBlocks == 0});
            const std::string prox = proxText(e);
            const std::string proxMin = std::isfinite(e.proxMin) ? format("%.0f", e.proxMin) : unknown;
            card.rows.push_back({t.diagRowProx, (prox.empty() ? std::string(t.diagUnreadable) : prox) + " / " + proxMin});
            card.rows.push_back({t.diagRowPupil, e.hasPupil ? format(t.diagEyesFormat, percent(e.pupil[0]).c_str(),
                                                                     percent(e.pupil[1]).c_str())
                                                            : unknown});
            card.rows.push_back({t.diagRowLoad, e.live && std::isfinite(e.liveMs) && e.liveMs > 0
                                                    ? format(t.diagMsFormat, e.liveMs)
                                                    : unknown});
        } else {
            for (const char* label : {t.diagRowSearch, t.diagRowBlocks, t.diagRowProx, t.diagRowPupil, t.diagRowLoad}) {
                card.rows.push_back({label, unknown});
            }
        }
        out.push_back(card);
    }

    // The last calibration (this wear's): when, how it ended, the pupils it saw and where, and eyecam-rec's last error
    {
        Card card {t.diagCardCalib, {}};
        const eyecam::LastCalib& c = e.lastCalib;
        if (recorder && c.present) {
            card.rows.push_back({t.diagRowWhen, shortStamp(c.time)});
            Row result {t.diagRowResult, t.diagOk};
            if (!c.ok) {
                result = {t.diagRowResult,
                          format(t.diagFailedFormat, inLanguage(c.message, c.messageEn, m.language).c_str()), true};
            } else if (c.failedEye == "L" || c.failedEye == "R") {
                result.value = c.failedEye == "L" ? t.diagPreviousLeft : t.diagPreviousRight;
            }
            card.rows.push_back(result);
            const auto frames = [&](int eye) { return whole(c.pupilFrames[eye]) + "/" + whole(c.normalFrames[eye]); };
            card.rows.push_back({t.diagRowPupilFrames, format(t.diagEyesFormat, frames(0).c_str(), frames(1).c_str())});
            const auto at = [&](int eye) {
                return std::isfinite(c.pupilX[eye]) && std::isfinite(c.pupilY[eye])
                           ? format("(%.0f, %.0f)", c.pupilX[eye], c.pupilY[eye])
                           : unknown;
            };
            const auto window = [&](int eye) { return whole(c.window[eye][0]) + "–" + whole(c.window[eye][1]); };
            card.rows.push_back({t.diagRowPupilAt, format(t.diagEyesFormat, at(0).c_str(), at(1).c_str())});
            card.rows.push_back({t.diagRowWindow, format(t.diagEyesFormat, window(0).c_str(), window(1).c_str())});
        } else {
            card.rows.push_back({t.diagRowWhen, recorder && c.known ? t.diagNoCalib : unknown.c_str()});
            for (const char* label : {t.diagRowResult, t.diagRowPupilFrames, t.diagRowPupilAt, t.diagRowWindow}) {
                card.rows.push_back({label, unknown});
            }
        }
        // (the failed calibration's own message is already in its result: only when it came)
        const bool same = c.present && !c.ok && e.lastError == c.message;
        const std::string error =
            same ? std::string(t.diagSameAsResult) : inLanguage(e.lastError, e.lastErrorEn, m.language);
        card.rows.push_back({t.diagRowLastError,
                             recorder ? (error.empty() ? std::string(t.diagNone) : withTime(t, error, e.lastErrorUnix))
                                      : unknown});
        out.push_back(card);
    }
    return out;
}

std::string signature(const UiText& t, const PanelModel& m) {
    std::string out = code(m);
    for (const Card& card : cards(t, m)) {
        for (const Row& row : card.rows) out += "|" + row.value + (row.bad ? "!" : "");
    }
    return out;
}

}  // namespace diag
