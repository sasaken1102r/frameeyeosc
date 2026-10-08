// Tests for the records of calibrations and eye fits (report.cpp) and the remembered sub-tabs (ui_state.cpp): the
// journal's and Valve's lines taken apart, the key lines and their words, summary.json, report.txt as printed, the
// record written from made-up logs (what goes in, what never does), keeping 10 (only real record folders, only their
// files, never the one just written), the size caps, status.jsonl a second (its start and end kept), following a
// calibration, the writer's thread and `--report`. Files go to a folder made under the current one, removed at the
// end. Exits non-zero on failure. Runs in Japan's time zone (JST-9), so the local times are fixed.
#include "icons.h"
#include "report.h"
#include "ui_state.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

/**
 * Record a failed check.
 * @param ok the check
 * @param what what was checked
 * @param line where
 */
void check(bool ok, const char* what, int line) {
    if (ok) return;
    ++gFailures;
    std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
}

#define CHECK(condition) check((condition), #condition, __LINE__)

using report::Kind;
using report::LogLine;
using report::Result;
using report::Source;

/** The test's folder (made under the current one). */
const std::string kRoot = "report-test-files";

/**
 * Unix seconds of a time in Japan (JST, UTC+9).
 * @param y year
 * @param mo month
 * @param d day
 * @param h hour
 * @param mi minute
 * @param s second
 * @return the time
 */
double jst(int y, int mo, int d, int h, int mi, double s) {
    std::tm tm {};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = 0;
    return static_cast<double>(timegm(&tm)) - 9 * 3600 + s;
}

/**
 * Write a file.
 * @param path where
 * @param text what
 */
void writeText(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

/**
 * Read a file.
 * @param path the file
 * @return its text ("" if missing)
 */
std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/**
 * @param path a path
 * @return true if it is there
 */
bool exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

/**
 * @param path a file
 * @return its size (-1 if missing)
 */
long long sizeOf(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<long long>(st.st_size) : -1;
}

/**
 * Remove a folder and everything in it.
 * @param path the folder
 */
void removeAll(const std::string& path) {
    DIR* d = ::opendir(path.c_str());
    if (d != nullptr) {
        while (const dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string child = path + "/" + name;
            struct stat st {};
            if (::lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                removeAll(child);
            } else {
                ::unlink(child.c_str());
            }
        }
        ::closedir(d);
    }
    ::rmdir(path.c_str());
}

/**
 * The names in a folder.
 * @param path the folder
 * @return them
 */
std::vector<std::string> names(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = ::opendir(path.c_str());
    if (d == nullptr) return out;
    while (const dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") out.push_back(name);
    }
    ::closedir(d);
    return out;
}

/**
 * @param text a text
 * @param part what to look for
 * @return true if it is in it
 */
bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

/**
 * printf into a string.
 * @param pattern the format
 * @param value its number
 * @return the text
 */
std::string format(const char* pattern, int value) {
    char text[64];
    std::snprintf(text, sizeof(text), pattern, value);
    return text;
}

/**
 * A rule of so many "─".
 * @param n how many
 * @return the rule
 */
std::string rule(int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += "─";
    return out;
}

/**
 * The 20:45 fit on 2026-10-06 as the panel saw it (the summary without its logs).
 * @return it
 */
report::Summary fitSummary() {
    report::Summary s;
    s.kind = Kind::Fit;
    s.mode = "full";
    s.start = jst(2026, 10, 6, 20, 45, 9.28);
    s.end = s.start + 7.9;
    s.result = Result::Failed;
    s.reason = "真ん中の点で使えるサンプルが 0/153（3 回とも）";
    s.reasonEn = "Center dot: 0 of 153 samples usable (3 tries)";
    s.brief = "正面の点で視線が落ち着きませんでした";
    s.briefEn = "The gaze wasn't steady at the center dot";
    s.version = "0.7.5";
    s.steamos = "0.4.3 (20260930.6234839)";
    s.code = "OK·P24·B128·G1·C1·FW";
    s.conditions.dashboardOpen = 1;
    s.conditions.cameraFps = 90;
    s.conditions.trackerRate = 90;
    s.conditions.trackerMissed = 0;
    s.conditions.sincePutOnSec = 10;
    s.conditions.offBeforeSec = 202.4;
    s.conditions.ipdMm = 69.6;
    s.folder = report::folderName(s.kind, s.start);
    return s;
}

/** The journals of that window, as journalctl -o short-unix prints them (and lines outside it). */
const char* const kCoreJournal =
    "1791287080.000000 frame frameeyeosc[28560]: Way before the window\n"
    "1791287099.117852 frame frameeyeosc[28560]: Eye tracking resumed after 202.4 s\n"
    "1791287109.794899 frame frameeyeosc[28560]: Loaded /home/steamos/.config/frameeyeosc/config.json\n"
    "1791287109.795152 frame frameeyeosc[28560]: Gaze capture 610 (center) asked for\n"
    "1791287111.816643 frame frameeyeosc[28560]: Gaze capture 610 (center): no usable samples (153 received at 90 Hz)\n";
const char* const kPanelJournal =
    "1791287099.162643 frame frameeyeosc-panel[28590]: [fit] put on (tracking was off 201.3 s): re-centering once the "
    "eyes settle\n"
    "1791287109.280724 frame frameeyeosc-panel[28590]: [fit] eye fit: started from the panel\n"
    "1791287109.280724 frame frameeyeosc-panel[28590]: [fit] eye fit, IPD 69.6 mm, dashboard open\n"
    "1791287109.280724 frame frameeyeosc-panel[28590]: [fit] auto re-center: skipped (an eye fit was started)\n"
    "1791287109.287151 frame frameeyeosc-panel[28590]: [fit] phase 1, point 1/6 (center), try 1, failure 0\n"
    "1791287109.786746 frame frameeyeosc-panel[28590]: [config] wrote /home/steamos/.config/frameeyeosc/config.json\n"
    "1791287109.786746 frame frameeyeosc-panel[28590]: [fit] asked for gaze capture 610 (center, 2.0 s)\n"
    "1791287111.852174 frame frameeyeosc-panel[28590]: [fit] center try 1: 0 of 153 samples usable at 90 Hz (needs "
    "45), no gaze average -> again\n"
    "1791287117.193177 frame frameeyeosc-panel[28590]: [fit] center try 3: 0 of 153 samples usable at 90 Hz (needs "
    "45), no gaze average -> failed\n"
    "1791287117.193603 frame frameeyeosc-panel[28590]: [fit] target up 7.9 s: 944 frames (119 per second), 902 "
    "pictures drawn\n";
const char* const kEyecamJournal =
    "1791287101.137847 frame eyecam-rec[28566]:   search: 8 candidate frame(s) in changed memory\n"
    "1791287102.752789 frame eyecam-rec[28566]: locked: 8 slots at 0x234100, spacing 262272\n"
    "1791287140.000000 frame eyecam-rec[28566]: calib wear: long after the window\n";
const char* const kValveLog =
    "Tue Oct 06 2026 20:40:00.000000 [Info] - before the window\n"
    "Tue Oct 06 2026 20:44:59.045811 [Info] - HMD on, starting eye tracking\n"
    "Tue Oct 06 2026 20:44:59.046687 [Info] - CEyePoseUKF L: Initialized\n"
    "  a line of its own without a time\n"
    "Tue Oct 06 2026 20:45:23.048590 [Info] - CEyePoseUKF L: Large dt 0.522194 > 0.400000\n";

/**
 * Made-up sources: the journals above, Valve's log in a file, eyecam's folder, no waiting.
 * @param dir where the files go
 * @param huge a frameeyeosc journal far over logs.txt's cap
 * @return the sources
 */
report::Sources fakeSources(const std::string& dir, bool huge = false) {
    report::Sources s;
    s.journal = [huge](const std::string& unit, double, double) {
        if (unit == "frameeyeosc") {
            if (!huge) return std::string(kCoreJournal);
            std::string text;
            for (int i = 0; i < 20000; ++i) {
                char line[160];
                std::snprintf(line, sizeof(line), "%.6f frame frameeyeosc[1]: line %05d of a very chatty journal\n",
                              1791287100.0 + i * 0.0005, i);
                text += line;
            }
            return text;
        }
        if (unit == "frameeyeosc-panel") return std::string(kPanelJournal);
        if (unit == "eyecam") return std::string(kEyecamJournal);
        return std::string();
    };
    s.valveLog = dir + "/eyetracking.txt";
    writeText(s.valveLog, kValveLog);
    s.eyecamHome = dir + "/eyecam";
    s.waitUntil = [](double) {};
    return s;
}

void testNames() {
    for (const Kind kind : {Kind::CalibWear, Kind::CalibUser, Kind::Fit, Kind::Recenter}) {
        Kind back = Kind::Fit;
        CHECK(report::parseKind(report::kindName(kind), back) && back == kind);
    }
    Kind k;
    CHECK(!report::parseKind("calib", k));
    for (const Result result : {Result::Ok, Result::Failed, Result::Partial, Result::Stopped}) {
        Result back = Result::Ok;
        CHECK(report::parseResult(report::resultName(result), back) && back == result);
    }
    for (const Source source : {Source::Core, Source::Panel, Source::Eyecam, Source::Valve}) {
        Source back = Source::Panel;
        CHECK(report::parseSource(report::sourceName(source), back) && back == source);
    }
    // Folder names: <kind>_YYYY-MM-DD_HH-MM-SS in local time, maybe _2 after it
    const double start = jst(2026, 10, 6, 20, 45, 9.6);
    CHECK(report::folderName(Kind::Fit, start) == "fit_2026-10-06_20-45-09");
    CHECK(report::folderName(Kind::CalibWear, start) == "calib-wear_2026-10-06_20-45-09");
    CHECK(report::isFolderName("fit_2026-10-06_20-45-09"));
    CHECK(report::isFolderName("recenter_2026-10-06_20-45-09_2"));
    CHECK(!report::isFolderName("fit_2026-10-06_20-45-0"));
    CHECK(!report::isFolderName("rec_2026-10-06_20-45-09"));
    CHECK(!report::isFolderName("fit_2026-10-06_20-45-09_"));
    CHECK(!report::isFolderName("fit_2026-10-06_20-45-09x"));
    CHECK(!report::isFolderName("../fit_2026-10-06_20-45-09"));
    CHECK(report::folderStamp("calib-user_2026-10-05_22-30-15") == "2026-10-05_22-30-15");
    // Sizes as the record view shows them
    CHECK(report::sizeText(512) == "512 B");
    CHECK(report::sizeText(4000) == "4 KB");
    CHECK(report::sizeText(86 * 1024) == "86 KB");
    CHECK(report::sizeText(1258291) == "1.2 MB");
}

void testParsing() {
    // The journal: the time, the message after "name[pid]: ", a message's next line keeping the time before it
    const std::vector<LogLine> lines = report::parseJournal(
        "1791287099.117852 frame frameeyeosc[28560]: Eye tracking resumed after 202.4 s\n"
        "    the same message's next line\n"
        "\n"
        "1791287109.794899 frame frameeyeosc[28560]: Loaded: a: b\n",
        Source::Core);
    CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        CHECK(std::fabs(lines[0].at - 1791287099.117852) < 1e-6);
        CHECK(lines[0].text == "Eye tracking resumed after 202.4 s");
        CHECK(lines[0].source == Source::Core);
        CHECK(lines[1].at == lines[0].at && lines[1].text == "the same message's next line");
        CHECK(lines[2].text == "Loaded: a: b");
    }
    CHECK(report::parseJournal("-- No entries --\n", Source::Panel).empty());

    // Valve's log: local time with microseconds, "[Info] - " kept in the line (left out where it is shown)
    double at = 0;
    std::string rest;
    CHECK(report::parseValveLine("Tue Oct 06 2026 20:44:59.045811 [Info] - HMD on, starting eye tracking", at, rest));
    CHECK(std::fabs(at - jst(2026, 10, 6, 20, 44, 59.045811)) < 1e-5);
    CHECK(rest == "[Info] - HMD on, starting eye tracking");
    CHECK(!report::parseValveLine("HMD on", at, rest));
    CHECK(!report::parseValveLine("Tue Foo 06 2026 20:44:59.045811 x", at, rest));
    const std::vector<LogLine> valve =
        report::parseValveLog(kValveLog, jst(2026, 10, 6, 20, 44, 50), jst(2026, 10, 6, 20, 45, 20));
    CHECK(valve.size() == 3);
    if (valve.size() == 3) {
        CHECK(valve[0].source == Source::Valve && valve[0].text == "[Info] - HMD on, starting eye tracking");
        CHECK(valve[2].text == "  a line of its own without a time" && valve[2].at == valve[1].at);
    }

    // Merged in time order; the same time keeps its order
    const std::vector<LogLine> merged = report::merge({{{2.0, Source::Core, "b"}, {3.0, Source::Core, "d"}},
                                                       {{1.0, Source::Valve, "a"}, {2.0, Source::Panel, "c"}}});
    CHECK(merged.size() == 4);
    if (merged.size() == 4) CHECK(merged[0].text == "a" && merged[1].text == "b" && merged[2].text == "c");

    // logs.txt: date, time to the millisecond, the source in brackets
    const std::string text = report::logsText({{jst(2026, 10, 6, 20, 44, 59.1178), Source::Core, "Eye tracking resumed"},
                                               {jst(2026, 10, 6, 20, 44, 59.5), Source::Valve, "HMD on"}});
    CHECK(text ==
          "2026-10-06 20:44:59.117 [frameeyeosc] Eye tracking resumed\n2026-10-06 20:44:59.500 [valve] HMD on\n");
    // ...cut to its cap: the start and the end, and how many lines were left out
    std::vector<LogLine> many;
    for (int i = 0; i < 5000; ++i) many.push_back({1791287100.0 + i, Source::Eyecam, "line " + std::to_string(i)});
    const std::string capped = report::logsText(many, 20 * 1024);
    CHECK(capped.size() <= 20 * 1024);
    CHECK(has(capped, "] line 0\n") && has(capped, "] line 4999\n") && !has(capped, "] line 2500\n"));
    CHECK(has(capped, "lines left out (logs.txt keeps at most 20 KB)"));

    // compact JSON: no white space between the tokens, the strings as they are; nothing for a broken one
    CHECK(report::compactJson("{\n  \"a\": [1, 2],\n  \"b\": \"x  y\\\" z\"\n}\n") == "{\"a\":[1,2],\"b\":\"x  y\\\" z\"}");
    CHECK(report::compactJson("{\"a\": ").empty());
    CHECK(report::compactJson("[1, 2]").empty());
    CHECK(report::plainJson("{\"ok\": true}"));
    CHECK(!report::plainJson(std::string("{\"ok\": true}\0", 13)));
    CHECK(!report::plainJson("eye image"));
}

void testKeyLines() {
    const auto key = [](Source source, const char* text) { return report::keyLine({0.0, source, text}); };
    CHECK(key(Source::Core, "Eye tracking resumed after 202.4 s"));
    CHECK(!key(Source::Core, "Loaded /home/steamos/.config/frameeyeosc/config.json"));
    CHECK(!key(Source::Core, "Gaze capture 610 (center) asked for"));
    CHECK(key(Source::Panel, "[fit] center try 1: 0 of 153 samples usable at 90 Hz (needs 45), no gaze average -> again"));
    CHECK(key(Source::Panel, "[fit] eye fit, IPD 69.6 mm, dashboard open"));
    CHECK(!key(Source::Panel, "[fit] phase 1, point 1/6 (center), try 1, failure 0"));
    CHECK(!key(Source::Panel, "[fit] asked for gaze capture 610 (center, 2.0 s)"));
    CHECK(!key(Source::Panel, "[fit] eye fit: started from the panel"));
    CHECK(!key(Source::Panel, "[config] wrote /home/steamos/.config/frameeyeosc/config.json"));
    CHECK(!key(Source::Panel, "[dots] off"));
    CHECK(key(Source::Panel, "[eyecam] calibrating, step 2/6 close"));
    CHECK(!key(Source::Panel, "[eyecam] tab shown (idle)"));
    CHECK(key(Source::Eyecam, "locked: 8 slots at 0x234100"));
    CHECK(key(Source::Eyecam, "calib wear: L step 0.22"));
    CHECK(!key(Source::Eyecam, "  search: 8 candidate frame(s) in changed memory"));
    CHECK(!key(Source::Eyecam, "live: 3295 frames in the last 30 s"));
    CHECK(key(Source::Valve, "[Info] - HMD on, starting eye tracking"));
    CHECK(key(Source::Valve, "[Info] - CStereoAdspCams: Set framerate 90"));
    CHECK(!key(Source::Valve, "[Info] - CEyePoseUKF L: Initialized"));

    // At most so many: the first three and the last ones
    std::vector<LogLine> lines;
    for (int i = 0; i < 30; ++i) lines.push_back({static_cast<double>(i), Source::Panel, "[fit] line " + std::to_string(i)});
    lines.push_back({30.0, Source::Panel, "[fit] phase 5"});
    const std::vector<LogLine> flow = report::flowLines(lines, 10);
    CHECK(flow.size() == 10);
    if (flow.size() == 10) {
        CHECK(flow[0].text == "[fit] line 0" && flow[2].text == "[fit] line 2" && flow[3].text == "[fit] line 23");
        CHECK(flow[9].text == "[fit] line 29");
    }
    CHECK(report::flowLines(lines, 100).size() == 30);

    // In the panel's words
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    const LogLine tryLine {0.0, Source::Panel,
                           "[fit] center try 1: 0 of 153 samples usable at 90 Hz (needs 45), no gaze average -> again"};
    // (the arrow is an icon marker the panel draws with lines; as plain text it is "→")
    CHECK(report::flowText(ja, tryLine) == "正面 1 回目 0/153 " ICON_ARROW_RIGHT " もう一度");
    CHECK(report::flowText(en, tryLine) == "center try 1: 0/153 " ICON_ARROW_RIGHT " again");
    CHECK(icon::plain(report::flowText(ja, tryLine)) == "正面 1 回目 0/153 → もう一度");
    CHECK(icon::plain(ICON_CHEVRON_LEFT " Back · " ICON_TRIANGLE_DOWN ICON_NOTE "x") == "‹ Back · ▼♪x");
    CHECK(icon::plain("no icons → here") == "no icons → here");
    CHECK(!icon::any("正面 → OK") && icon::any("a" ICON_DOT));
    CHECK(report::flowText(ja, {0.0, Source::Panel, "[fit] up try 3: 12 of 40 samples usable at 15 Hz (needs 24) -> failed"}) ==
          "上 3 回目 12/40 " ICON_ARROW_RIGHT " 失敗");
    CHECK(report::flowText(ja, {0.0, Source::Panel, "[fit] center try 2: 128 samples (min 45), spread 3.4° -> ok"}) ==
          "正面 2 回目 128 " ICON_ARROW_RIGHT " OK");
    CHECK(report::flowText(ja, {0.0, Source::Panel, "[fit] eye fit, IPD 69.6 mm, dashboard open"}) ==
          "目合わせ 開始（開いたまま）");
    CHECK(report::flowText(en, {0.0, Source::Panel, "[fit] re-center, IPD 63.0 mm, dashboard closed"}) ==
          "re-center started (dashboard closed)");
    CHECK(report::flowText(ja, {0.0, Source::Panel, "[fit] target up 7.9 s: 944 frames (119 per second)"}) ==
          "点を消した（7.9 秒）");
    CHECK(report::flowText(ja, {0.0, Source::Panel, "[fit] put on (tracking was off 201.3 s): re-centering"}) ==
          "かぶった（201 秒ぶり）");
    CHECK(report::flowText(ja, {0.0, Source::Core, "Eye tracking resumed after 202.4 s"}) == "目のデータ 再開（202 秒ぶり）");
    CHECK(report::flowText(ja, {0.0, Source::Valve, "[Info] - HMD on, starting eye tracking"}) ==
          "HMD on, starting eye tracking");
    CHECK(report::flowText(ja, {0.0, Source::Eyecam, "locked: 8 slots"}) == "locked: 8 slots");
    CHECK(report::flowText(ja, {0.0, Source::Panel, "[eyecam] calibrating, step 2/6 close"}) ==
          "[eyecam] calibrating, step 2/6 close");
}

void testSummary() {
    report::Summary s = fitSummary();
    s.flow = {{s.start - 10.2, Source::Core, "Eye tracking resumed after 202.4 s"},
              {s.start, Source::Panel, "[fit] eye fit, IPD 69.6 mm, dashboard open"}};
    const std::string json = report::summaryJson(s);
    CHECK(has(json, "\"kind\": \"fit\"") && has(json, "\"result\": \"failed\"") && has(json, "\"start\": \"2026-10-06 20:45:09\""));
    CHECK(has(json, "\"dashboard_open\": true") && has(json, "\"camera_fps\": 90"));
    report::Summary back;
    CHECK(report::parseSummary(json, back));
    CHECK(back.kind == Kind::Fit && back.mode == "full" && back.trigger == "panel" && back.result == Result::Failed);
    CHECK(std::fabs(back.start - s.start) < 0.001 && std::fabs(back.end - s.end) < 0.001);
    CHECK(back.reason == s.reason && back.reasonEn == s.reasonEn && back.brief == s.brief && back.briefEn == s.briefEn);
    CHECK(back.version == "0.7.5" && back.steamos == s.steamos && back.code == s.code);
    CHECK(back.conditions.dashboardOpen == 1 && back.conditions.cameraFps == 90 && back.conditions.trackerMissed == 0);
    CHECK(std::fabs(back.conditions.offBeforeSec - 202.4) < 1e-9 && std::fabs(back.conditions.ipdMm - 69.6) < 1e-9);
    CHECK(back.flow.size() == 2);
    if (back.flow.size() == 2) CHECK(back.flow[1].source == Source::Panel && back.flow[1].text == s.flow[1].text);
    // Unknown numbers as null, and back as NaN
    report::Summary bare;
    bare.start = bare.end = 1791287109.0;
    report::Summary bareBack;
    CHECK(report::parseSummary(report::summaryJson(bare), bareBack));
    CHECK(std::isnan(bareBack.conditions.cameraFps) && bareBack.conditions.dashboardOpen == -1);
    CHECK(!report::parseSummary("{\"kind\": \"fit\"}", bareBack));
    CHECK(!report::parseSummary("not json", bareBack));

    // The lines shown with it
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    CHECK(report::conditionsText(ja, s) == "ダッシュボードを開いたまま・カメラ 90 枚/秒・かぶってから 10 秒");
    CHECK(report::conditionsText(en, s) == "dashboard open, cameras 90 fps, 10 s after putting it on");
    report::Summary rewear = s;
    rewear.kind = Kind::Recenter;
    rewear.mode = "tilt";
    rewear.trigger = "auto";
    rewear.conditions = report::Conditions();
    rewear.conditions.dashboardOpen = 0;
    CHECK(report::kindLabel(ja, rewear) == "正面と傾きを合わせ直す");
    CHECK(report::conditionsText(ja, rewear) == "かぶったとき・ダッシュボードを閉じて");
    CHECK(std::string(report::resultLabel(ja, Result::Partial)) == "片目だけ");
    // Stopped is not a failure: its own word, and its own name in summary.json
    CHECK(std::string(report::resultLabel(ja, Result::Stopped)) == "止めた");
    CHECK(std::string(report::resultLabel(en, Result::Stopped)) == "Stopped");
    {
        report::Summary stopped = s;
        stopped.result = Result::Stopped;
        report::Summary stoppedBack;
        CHECK(has(report::summaryJson(stopped), "\"result\": \"stopped\""));
        CHECK(report::parseSummary(report::summaryJson(stopped), stoppedBack) && stoppedBack.result == Result::Stopped);
    }

    // report.txt, as `--report latest` prints it
    const std::string text = report::reportText(ja, s, "~/.local/state/frameeyeosc/reports/fit_2026-10-06_20-45-09/");
    const std::string expected =
        "frameeyeosc 記録  fit_2026-10-06_20-45-09\n"
        "種類      目を合わせる（ダッシュボードを開いたまま）\n"
        "時刻      2026-10-06 20:45:09 – 20:45:17（7.9 秒）\n"
        "結果      失敗: 真ん中の点で使えるサンプルが 0/153（3 回とも）\n"
        "版        frameeyeosc 0.7.5 / SteamOS 0.4.3 (20260930.6234839)\n"
        "診断      OK·P24·B128·G1·C1·FW\n"
        "目のデータ Valve 90 回/秒（取りこぼし 0）・カメラ 90 枚/秒\n"
        "直前      かぶってから 10 秒（目のデータ 202 秒ぶりに再開）\n"
        "\n"
        "── 流れ " + rule(44) + "\n"
        "20:44:59 本体    Eye tracking resumed after 202.4 s\n"
        "20:45:09 パネル  [fit] eye fit, IPD 69.6 mm, dashboard open\n"
        "\n"
        "ファイル  ~/.local/state/frameeyeosc/reports/fit_2026-10-06_20-45-09/\n";
    CHECK(text == expected);
    if (text != expected) std::fprintf(stderr, "--- got:\n%s--- expected:\n%s", text.c_str(), expected.c_str());
    const std::string english = report::reportText(en, s, "~/x/");
    CHECK(has(english, "frameeyeosc record  fit_2026-10-06_20-45-09\n"));
    CHECK(has(english, "Kind      Eye fit (dashboard open)\n"));
    CHECK(has(english, "Result    Failed: Center dot: 0 of 153 samples usable (3 tries)\n"));
    CHECK(has(english, "Eye data  Valve 90/s (missed 0), cameras 90 fps\n"));
    CHECK(has(english, "Before    10 s after putting it on (eye data back after 202 s)\n"));
    CHECK(has(english, "20:45:09 panel   [fit] eye fit"));
    // Without what isn't known: no rates, no "before", no flow
    report::Summary plain = fitSummary();
    plain.conditions = report::Conditions();
    const std::string short_ = report::reportText(ja, plain, "~/x/");
    CHECK(!has(short_, "目のデータ") && !has(short_, "直前") && has(short_, "ログが見つからなかったよ"));
}

void testRecord() {
    const std::string dir = kRoot + "/record";
    ::mkdir(dir.c_str(), 0700);
    const std::string records = dir + "/reports";
    report::Sources sources = fakeSources(dir);
    // eyecam's folder: this calibration's (with its samples and images, which never go in), and an older one
    const std::string calib = sources.eyecamHome + "/calib_2026-10-06_20-45-10";
    ::mkdir(sources.eyecamHome.c_str(), 0700);
    ::mkdir(calib.c_str(), 0700);
    writeText(calib + "/calib_result.json", "{\n  \"kind\": \"wear\",\n  \"ok\": false,\n  \"values\": {\"r_px\": [41.2, 40.8]}\n}\n");
    writeText(calib + "/calib_samples.csv", "eye,t,step,pupil\n0,0.1,normal,1\n");
    writeText(calib + "/eye_L.raw", std::string(4096, '\x7f'));
    ::mkdir((sources.eyecamHome + "/calib_2026-10-06_19-00-00").c_str(), 0700);
    writeText(sources.eyecamHome + "/calib_2026-10-06_19-00-00/calib_result.json", "{\"kind\": \"old\"}");

    // An eye fit: summary, status a second, the logs, report.txt
    {
        report::Pending pending;
        pending.summary = fitSummary();
        pending.statusLines = "{\"t\":1.0,\"frameeyeosc\":{},\"eyecam\":null}\n";
        std::string folder;
        std::string error;
        CHECK(report::writeRecord(records, pending, sources, folder, error));
        CHECK(folder == "fit_2026-10-06_20-45-09");
        const std::string path = records + "/" + folder;
        CHECK(exists(path + "/summary.json") && exists(path + "/status.jsonl") && exists(path + "/logs.txt") &&
              exists(path + "/report.txt"));
        CHECK(!exists(path + "/calib_result.json"));  // not a calibration
        const std::string logs = readText(path + "/logs.txt");
        // The window (10 s before to 2 s after), all four sources, nothing outside it
        CHECK(has(logs, "2026-10-06 20:44:59.117 [frameeyeosc] Eye tracking resumed after 202.4 s\n"));
        CHECK(has(logs, "[panel] [fit] center try 3: 0 of 153"));
        CHECK(has(logs, "[eyecam] locked: 8 slots"));
        CHECK(has(logs, "[valve] [Info] - HMD on, starting eye tracking"));
        CHECK(!has(logs, "Way before the window") && !has(logs, "long after the window") && !has(logs, "before the window\n"));
        CHECK(!has(logs, "Large dt"));  // 20:45:23, after the window
        // Its key lines in summary.json and report.txt
        report::Summary back;
        std::vector<report::FileInfo> files;
        CHECK(report::load(records, folder, back, &files));
        CHECK(back.folder == folder && back.result == Result::Failed);
        CHECK(back.flow.size() >= 5);
        bool resumed = false;
        bool valve = false;
        bool phase = false;
        for (const LogLine& line : back.flow) {
            resumed |= line.text == "Eye tracking resumed after 202.4 s";
            valve |= line.source == Source::Valve && has(line.text, "HMD on");
            phase |= has(line.text, "phase 1");
        }
        CHECK(resumed && valve && !phase);
        CHECK(files.size() == 4);
        if (files.size() == 4) CHECK(files[0].name == "report.txt" && files[3].name == "summary.json" && files[1].bytes > 0);
        const std::string reportTxt = readText(path + "/report.txt");
        CHECK(has(reportTxt, "frameeyeosc 記録  fit_2026-10-06_20-45-09\n") && has(reportTxt, "パネル  [fit] center try 3"));
        // The same second again: a folder of its own
        CHECK(report::writeRecord(records, pending, sources, folder, error));
        CHECK(folder == "fit_2026-10-06_20-45-09_2");
    }
    // A calibration: its calib_result.json from the folder status.json named (a rec_ folder is never read), never its
    // samples or images
    {
        report::Pending pending;
        pending.summary = fitSummary();
        pending.summary.kind = Kind::CalibWear;
        pending.calibDir = calib;
        std::string folder;
        std::string error;
        CHECK(report::writeRecord(records, pending, sources, folder, error));
        const std::string path = records + "/" + folder;
        CHECK(readText(path + "/calib_result.json") == readText(calib + "/calib_result.json"));
        for (const std::string& name : names(path)) {
            CHECK(name == "summary.json" || name == "logs.txt" || name == "calib_result.json" || name == "report.txt");
        }
        // ...found by its time when status.json didn't say
        pending.calibDir = sources.eyecamHome + "/rec_2026-10-06_20-45-10";
        CHECK(report::writeRecord(records, pending, sources, folder, error));
        CHECK(readText(records + "/" + folder + "/calib_result.json") == readText(calib + "/calib_result.json"));
        // ...and not at all when it isn't plain JSON, or too big
        writeText(calib + "/calib_result.json", std::string("{\"a\": 1}\0\0", 10));
        pending.calibDir = calib;
        CHECK(report::writeRecord(records, pending, sources, folder, error));
        CHECK(!exists(records + "/" + folder + "/calib_result.json"));
        writeText(calib + "/calib_result.json", "{\"pad\": \"" + std::string(report::kMaxCalibResultBytes, 'x') + "\"}");
        CHECK(report::writeRecord(records, pending, sources, folder, error));
        CHECK(!exists(records + "/" + folder + "/calib_result.json"));
    }
    // logs.txt within its cap with a chatty journal
    {
        report::Pending pending;
        pending.summary = fitSummary();
        pending.summary.start = jst(2026, 10, 6, 20, 45, 9.0);
        pending.summary.end = pending.summary.start + 20;
        std::string folder;
        std::string error;
        CHECK(report::writeRecord(records, pending, fakeSources(dir, true), folder, error));
        const long long size = sizeOf(records + "/" + folder + "/logs.txt");
        CHECK(size > 100 * 1024 && size <= static_cast<long long>(report::kMaxLogBytes));
        CHECK(has(readText(records + "/" + folder + "/logs.txt"), "lines left out"));
    }
}

void testRetention() {
    const std::string dir = kRoot + "/keep";
    ::mkdir(dir.c_str(), 0700);
    const std::string records = dir + "/reports";
    report::Sources sources = fakeSources(dir);
    sources.journal = nullptr;
    sources.valveLog.clear();
    ::mkdir(records.c_str(), 0700);
    ::mkdir((records + "/notes").c_str(), 0700);  // not a record: left alone
    writeText(records + "/notes/x.txt", "mine");
    writeText(records + "/readme.txt", "mine");
    // Twelve, an hour apart, written oldest first
    for (int i = 0; i < 12; ++i) {
        report::Pending pending;
        pending.summary = fitSummary();
        pending.summary.kind = i % 2 == 0 ? Kind::Fit : Kind::CalibWear;
        pending.summary.start = jst(2026, 10, 5, 8 + i, 0, 0);
        pending.summary.end = pending.summary.start + 5;
        std::string folder;
        std::string error;
        CHECK(report::writeRecord(records, pending, sources, folder, error));
    }
    const std::vector<std::string> kept = report::folders(records);
    CHECK(kept.size() == static_cast<size_t>(report::kKeep));
    if (kept.size() == 10) {
        CHECK(kept.front() == "calib-wear_2026-10-05_19-00-00");  // the newest first
        CHECK(kept.back() == "fit_2026-10-05_10-00-00");          // the two oldest gone
    }
    CHECK(!exists(records + "/fit_2026-10-05_08-00-00") && !exists(records + "/calib-wear_2026-10-05_09-00-00"));
    CHECK(exists(records + "/notes/x.txt") && exists(records + "/readme.txt"));
    // A folder with something else in it: only the record's files go, the rest (and so the folder) stays
    ::mkdir((records + "/" + kept.back() + "/inner").c_str(), 0700);
    writeText(records + "/" + kept.back() + "/mine.txt", "mine");
    CHECK(report::prune(records, 9) == 0);
    CHECK(exists(records + "/" + kept.back() + "/inner") && exists(records + "/" + kept.back() + "/mine.txt"));
    CHECK(!exists(records + "/" + kept.back() + "/summary.json"));
    CHECK(!exists(records + "/" + kept.back() + "/report.txt"));
    CHECK(report::list(records).size() == 9);
    removeAll(records + "/" + kept.back());

    // Newest by summary.json's start, not by the local time in the names: written after the time zone moved 21 hours
    // west, the newest record's name looks the oldest, and yet the oldest by its start is the one deleted
    const auto writeAt = [&](Kind kind, double start) {
        report::Pending pending;
        pending.summary = fitSummary();
        pending.summary.kind = kind;
        pending.summary.start = start;
        pending.summary.end = start + 5;
        std::string folder;
        std::string error;
        CHECK(report::writeRecord(records, pending, sources, folder, error));
        return folder;
    };
    writeAt(Kind::Recenter, jst(2026, 10, 5, 20, 0, 0));
    CHECK(report::folders(records).size() == 10);
    ::setenv("TZ", "AOE12", 1);
    ::tzset();
    const std::string west = writeAt(Kind::Fit, jst(2026, 10, 5, 21, 0, 0));
    ::setenv("TZ", "JST-9", 1);
    ::tzset();
    CHECK(west == "fit_2026-10-05_00-00-00");
    std::vector<std::string> now = report::folders(records);
    CHECK(now.size() == 10 && !now.empty() && now.front() == west);
    CHECK(exists(records + "/" + west + "/summary.json"));
    CHECK(!exists(records + "/calib-wear_2026-10-05_11-00-00"));  // the oldest by its start
    // The clock stepped back: the record just written is the oldest by its start, and still never deleted
    const std::string back = writeAt(Kind::Recenter, jst(2026, 10, 1, 9, 0, 0));
    CHECK(exists(records + "/" + back + "/summary.json"));
    now = report::folders(records);
    CHECK(now.size() == 11 && !now.empty() && now.back() == back);

    // A link named like a record (to a folder elsewhere) is never one: not listed, and nothing in it deleted
    const std::string elsewhere = dir + "/elsewhere";
    ::mkdir(elsewhere.c_str(), 0700);
    writeText(elsewhere + "/summary.json", report::summaryJson(fitSummary()));
    writeText(elsewhere + "/report.txt", "mine");
    const std::string link = records + "/fit_2026-10-07_00-00-00";
    CHECK(::symlink("../elsewhere", link.c_str()) == 0);
    for (const std::string& name : report::folders(records)) CHECK(name != "fit_2026-10-07_00-00-00");
    // A record with a file of someone else's in it: its own files go, the folder stays
    writeText(records + "/" + now.front() + "/notes.txt", "mine");
    // Everything pruned
    CHECK(report::prune(records, 0) == 10);
    struct stat st {};
    CHECK(::lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(exists(elsewhere + "/summary.json") && exists(elsewhere + "/report.txt"));
    CHECK(exists(records + "/" + now.front() + "/notes.txt") && !exists(records + "/" + now.front() + "/summary.json"));
    CHECK(exists(records + "/notes/x.txt") && exists(records + "/readme.txt"));
    std::vector<std::string> left = names(records);
    std::sort(left.begin(), left.end());
    CHECK(left == std::vector<std::string>({"fit_2026-10-07_00-00-00", now.front(), "notes", "readme.txt"}) ||
          left == std::vector<std::string>({now.front(), "fit_2026-10-07_00-00-00", "notes", "readme.txt"}));
}

void testRun() {
    report::Run run;
    CHECK(!run.active());
    CHECK(!run.sample(100.0, "{}", "{}"));  // not running
    run.begin(Kind::CalibUser, 100.0);
    CHECK(run.active() && run.summary().kind == Kind::CalibUser && run.summary().start == 100.0);
    // A line a second at most: compact status files, null for one missing or broken
    CHECK(run.sample(100.0, "{\n  \"pid\": 7,\n  \"rate\": 90.5\n}\n", ""));
    CHECK(!run.sample(100.5, "{}", "{}"));
    CHECK(run.sample(101.0, "{\"pid\": 7}", "{\"state\": \"calibrating\"}"));
    CHECK(run.sample(102.1, "{broken", "{}"));
    run.calibDir("");
    run.calibDir("/home/steamos/eyecam/calib_2026-10-06_20-45-10");
    report::Pending pending = run.finish(99.0);  // never before its start
    CHECK(!run.active());
    CHECK(pending.summary.end == 100.0);
    CHECK(pending.calibDir == "/home/steamos/eyecam/calib_2026-10-06_20-45-10");
    CHECK(pending.statusLines ==
          "{\"t\":100.000,\"frameeyeosc\":{\"pid\":7,\"rate\":90.5},\"eyecam\":null}\n"
          "{\"t\":101.000,\"frameeyeosc\":{\"pid\":7},\"eyecam\":{\"state\":\"calibrating\"}}\n"
          "{\"t\":102.100,\"frameeyeosc\":null,\"eyecam\":{}}\n");
    // A long run: status.jsonl keeps its start (kStatusHeadBytes) and its newest lines, with a line between them
    // saying how many were left out
    run.begin(Kind::Fit, 0.0);
    const std::string big = "{\"pad\": \"" + std::string(3000, 'x') + "\"}";
    const int count = 200;
    for (int i = 0; i < count; ++i) CHECK(run.sample(i, big, big));
    const report::Pending capped = run.finish(200.0);
    CHECK(capped.statusLines.size() <= report::kMaxStatusBytes &&
          capped.statusLines.size() > report::kMaxStatusBytes - 16 * 1024);
    {
        std::vector<std::string> lines;
        std::istringstream in(capped.statusLines);
        for (std::string line; std::getline(in, line);) lines.push_back(line);
        size_t marker = lines.size();
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].rfind("{\"left_out\":", 0) == 0) marker = i;
        }
        CHECK(marker < lines.size());
        if (marker < lines.size()) {
            const int skipped = std::atoi(lines[marker].c_str() + std::strlen("{\"left_out\":"));
            const int head = static_cast<int>(marker);
            const int tail = static_cast<int>(lines.size() - marker - 1);
            CHECK(head > 0 && tail > head && head + skipped + tail == count);
            CHECK(has(lines[marker], "lines left out") && report::compactJson(lines[marker]) == lines[marker]);
            // The start, then (after the gap) the newest, each a second apart, up to the last
            for (int i = 0; i < head; ++i) CHECK(lines[i].rfind(format("{\"t\":%d.000,", i), 0) == 0);
            for (int i = 0; i < tail; ++i) {
                CHECK(lines[marker + 1 + i].rfind(format("{\"t\":%d.000,", head + skipped + i), 0) == 0);
            }
            CHECK(lines.back().rfind(format("{\"t\":%d.000,", count - 1), 0) == 0);
            // The start within its share
            size_t headBytes = 0;
            for (int i = 0; i < head; ++i) headBytes += lines[i].size() + 1;
            CHECK(headBytes <= report::kStatusHeadBytes);
        }
    }
    // A new run starts afresh
    run.begin(Kind::Recenter, 300.0);
    CHECK(run.finish(301.0).statusLines.empty());
}

void testCalibWatch() {
    using report::CalibWatch;
    // Seen calibrating, then idle (or error): over
    {
        CalibWatch w;
        CHECK(!w.follow(false, true, false, 0.0));  // nothing followed
        w.start(100.0);
        CHECK(w.active());
        CHECK(!w.follow(false, true, true, 100.1));   // the reply on its way, eyecam-rec still idle
        CHECK(!w.follow(false, true, false, 100.3));  // took it, not calibrating yet
        CHECK(!w.follow(true, true, false, 101.0));
        CHECK(!w.replied(false));                     // an error after it began isn't a refusal
        CHECK(!w.follow(true, true, false, 119.0));
        CHECK(w.follow(false, true, false, 119.1));
        CHECK(!w.active() && !w.stopped() && !w.silent());
        CHECK(!w.follow(false, true, false, 119.2));  // once
    }
    // Refused: over at once
    {
        CalibWatch w;
        w.start(0.0);
        CHECK(!w.replied(true));
        CHECK(w.active());
        w.start(0.0);
        CHECK(w.replied(false) && !w.active());
    }
    // Taken but never calibrating: over after kStartSec, not while the reply is awaited
    {
        CalibWatch w;
        w.start(0.0);
        CHECK(!w.follow(false, true, true, CalibWatch::kStartSec + 5));
        CHECK(!w.follow(false, true, false, CalibWatch::kStartSec - 1));
        CHECK(w.follow(false, true, false, CalibWatch::kStartSec + 1));
    }
    // eyecam-rec gone mid-calibration (its status.json no longer fresh): a stale "calibrating" or "idle" says nothing;
    // kSilentSec after it last said "calibrating", over, as silent
    {
        CalibWatch w;
        w.start(0.0);
        CHECK(!w.follow(true, true, false, 2.0));
        CHECK(!w.follow(true, false, false, 3.0));    // stale "calibrating"
        CHECK(!w.follow(false, false, false, 60.0));  // stale anything
        CHECK(!w.follow(true, false, false, 2.0 + CalibWatch::kSilentSec - 0.1));
        CHECK(w.active());
        CHECK(w.follow(true, false, false, 2.0 + CalibWatch::kSilentSec + 0.1));
        CHECK(!w.active() && w.silent() && !w.stopped());
        // Fresh again before then: it goes on as before
        w.start(0.0);
        CHECK(!w.silent());
        CHECK(!w.follow(true, true, false, 2.0));
        CHECK(!w.follow(true, false, false, 100.0));
        CHECK(!w.follow(true, true, false, 110.0));
        CHECK(!w.follow(true, false, false, 200.0));  // 90 s since the last fresh "calibrating"
        CHECK(w.follow(false, true, false, 201.0) && !w.silent());
        // Never seen calibrating, and nothing fresh after the command: over after kSilentSec, not kStartSec
        w.start(0.0);
        CHECK(!w.follow(false, false, false, CalibWatch::kStartSec + 1));
        CHECK(w.follow(false, false, false, CalibWatch::kSilentSec + 1) && w.silent());
    }
    // "Stop" during it
    {
        CalibWatch w;
        w.stop();
        CHECK(!w.stopped());  // nothing followed
        w.start(0.0);
        w.follow(true, true, false, 1.0);
        w.stop();
        CHECK(w.follow(false, true, false, 2.0) && w.stopped());
        w.start(5.0);
        CHECK(!w.stopped());
        w.end();
        CHECK(!w.active());
    }
}

void testWriterAndCli() {
    const std::string dir = kRoot + "/writer";
    ::mkdir(dir.c_str(), 0700);
    const std::string records = dir + "/reports";
    const UiText& ja = uiText(Language::Ja);
    std::string out;
    // None yet
    CHECK(report::cliReport(ja, records, "latest", out) == 1 && has(out, "まだ記録はないよ"));
    CHECK(report::cliReport(ja, records, "list", out) == 0 && has(out, "まだ記録はないよ"));
    {
        report::Writer writer(records, fakeSources(dir));
        report::Pending first;
        first.summary = fitSummary();
        writer.submit(first);
        report::Pending second;
        second.summary = fitSummary();
        second.summary.kind = Kind::CalibWear;
        second.summary.result = Result::Partial;
        second.summary.brief = "右目は前の値を使うよ";
        second.summary.start += 60;
        second.summary.end += 60;
        writer.submit(second);
        std::vector<std::string> done;
        std::vector<Kind> kinds;
        for (int i = 0; i < 500 && done.size() < 2; ++i) {
            std::string folder;
            Kind kind = Kind::Fit;
            if (writer.poll(folder, kind)) {
                done.push_back(folder);
                kinds.push_back(kind);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        CHECK(done.size() == 2);
        if (done.size() == 2) {
            CHECK(done[0] == "fit_2026-10-06_20-45-09" && kinds[0] == Kind::Fit);
            CHECK(done[1] == "calib-wear_2026-10-06_20-46-09" && kinds[1] == Kind::CalibWear);
        }
        CHECK(!writer.busy());
    }
    // --report latest: the newest one's report.txt; NAME: that one; list: a line each
    CHECK(report::cliReport(ja, records, "latest", out) == 0);
    CHECK(out == readText(records + "/calib-wear_2026-10-06_20-46-09/report.txt") && has(out, "目のカメラの校正"));
    CHECK(report::cliReport(ja, records, "fit_2026-10-06_20-45-09", out) == 0 && has(out, "失敗: 真ん中の点"));
    CHECK(report::cliReport(ja, records, records + "/fit_2026-10-06_20-45-09/", out) == 0 && has(out, "失敗"));
    CHECK(report::cliReport(ja, records, "fit_2026-10-06_20-45-10", out) == 1);
    CHECK(report::cliReport(ja, records, "../writer", out) == 1);
    CHECK(report::cliReport(ja, records, "list", out) == 0);
    CHECK(out ==
          "calib-wear_2026-10-06_20-46-09  片目だけ  目のカメラの校正  右目は前の値を使うよ\n"
          "fit_2026-10-06_20-45-09         失敗      目を合わせる      正面の点で視線が落ち着きませんでした\n");
    if (!has(out, "calib-wear")) std::fprintf(stderr, "%s", out.c_str());
    // report.txt gone: made again from summary.json
    ::unlink((records + "/fit_2026-10-06_20-45-09/report.txt").c_str());
    CHECK(report::cliReport(uiText(Language::En), records, "fit_2026-10-06_20-45-09", out) == 0 &&
          has(out, "frameeyeosc record  fit_2026-10-06_20-45-09"));
}

void testUiState() {
    const std::string path = kRoot + "/state/frameeyeosc/panel.json";
    // The first time: Version
    CHECK(ui_state::readPage(path) == AdvPage::Version);
    std::string error;
    for (const AdvPage page : {AdvPage::Trouble, AdvPage::Tools, AdvPage::Files, AdvPage::Version, AdvPage::Tools}) {
        CHECK(ui_state::writePage(path, page, error));
        CHECK(ui_state::readPage(path) == page);
    }
    CHECK(has(readText(path), "\"advanced_page\": \"tools\""));
    // Other members are kept
    writeText(path, "{\"other\": 1, \"advanced_page\": \"files\"}");
    CHECK(ui_state::readPage(path) == AdvPage::Files);
    CHECK(ui_state::writePage(path, AdvPage::Trouble, error));
    CHECK(has(readText(path), "\"other\": 1") && ui_state::readPage(path) == AdvPage::Trouble);
    // Broken or unknown: Version
    writeText(path, "{\"advanced_page\": ");
    CHECK(ui_state::readPage(path) == AdvPage::Version);
    writeText(path, "{\"advanced_page\": \"debug\"}");
    CHECK(ui_state::readPage(path) == AdvPage::Version);
    CHECK(ui_state::writePage(path, AdvPage::Files, error) && ui_state::readPage(path) == AdvPage::Files);
    CHECK(!exists(path + ".tmp"));
    AdvPage page = AdvPage::Version;
    CHECK(ui_state::parsePage("trouble", page) && page == AdvPage::Trouble);
    CHECK(!ui_state::parsePage("Trouble", page));

    // The Eyelids tab's: the first time Look, kept beside the Advanced tab's (each write keeps the other)
    ::unlink(path.c_str());
    CHECK(ui_state::readLidsPage(path) == LidsPage::Look);
    for (const LidsPage lids : {LidsPage::Fine, LidsPage::Blinks, LidsPage::Look, LidsPage::Fine}) {
        CHECK(ui_state::writeLidsPage(path, lids, error));
        CHECK(ui_state::readLidsPage(path) == lids);
    }
    CHECK(has(readText(path), "\"lids_page\": \"fine\""));
    CHECK(ui_state::writePage(path, AdvPage::Tools, error));
    CHECK(ui_state::readLidsPage(path) == LidsPage::Fine && ui_state::readPage(path) == AdvPage::Tools);
    CHECK(ui_state::writeLidsPage(path, LidsPage::Blinks, error));
    CHECK(ui_state::readLidsPage(path) == LidsPage::Blinks && ui_state::readPage(path) == AdvPage::Tools);
    // Broken or unknown: Look
    writeText(path, "{\"lids_page\": \"marks\", \"advanced_page\": \"files\"}");
    CHECK(ui_state::readLidsPage(path) == LidsPage::Look && ui_state::readPage(path) == AdvPage::Files);
    writeText(path, "{\"lids_page\": 2}");
    CHECK(ui_state::readLidsPage(path) == LidsPage::Look);
    writeText(path, "{\"lids_page\": ");
    CHECK(ui_state::readLidsPage(path) == LidsPage::Look);
    CHECK(!exists(path + ".tmp"));
    LidsPage lids = LidsPage::Look;
    CHECK(ui_state::parseLidsPage("blinks", lids) && lids == LidsPage::Blinks);
    CHECK(!ui_state::parseLidsPage("Fine", lids) && lids == LidsPage::Blinks);
}

}  // namespace

int main() {
    ::setenv("TZ", "JST-9", 1);
    ::tzset();
    removeAll(kRoot);
    ::mkdir(kRoot.c_str(), 0700);
    testNames();
    testParsing();
    testKeyLines();
    testSummary();
    testRecord();
    testRetention();
    testRun();
    testCalibWatch();
    testWriterAndCli();
    testUiState();
    removeAll(kRoot);
    if (gFailures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("report-test: all passed\n");
    return 0;
}
