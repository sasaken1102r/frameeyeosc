// Tests for "Record for squint training" (contrib.cpp): a recording's size and when it fits, eyecam's
// protocol_contrib.txt (its length, and a text for each of its steps in both languages), info.txt (its sections
// written and read back, the last consent counting, odd lines), whether the headset was taken off between two
// recordings, following a recording in eyecam-rec's status (done, stopped while searching or recording, failed,
// refused, never started, eyecam-rec gone), which screen shows and when "Start" can be pressed, the tar headers, and on
// real folders in a temporary one (never ~/eyecam): the files a recording needs, moving it, the calibration result,
// packing (tar, and through zstd when it is there: unpacked again with the system's tar and compared; a recording
// lacking a file left out) and deleting, and a whole session from the consent on. Built with the panel as
// contrib-test; exits non-zero on failure.
#include "contrib.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
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

/**
 * Compare two texts, printing both when they differ.
 * @param got the text made
 * @param expected the text wanted
 * @param line where
 */
void same(const std::string& got, const std::string& expected, int line) {
    if (got == expected) return;
    ++gFailures;
    std::fprintf(stderr, "FAILED line %d:\n  got      \"%s\"\n  expected \"%s\"\n", line, got.c_str(), expected.c_str());
}

#define SAME(got, expected) same((got), (expected), __LINE__)

using contrib::Outcome;
using contrib::Page;
using contrib::Screen;
using eyecam::State;

/** Unix time the tests pretend it is. */
constexpr double kNow = 1'790'000'000.0;

/**
 * A folder for the test's own files, in $TMPDIR (else /tmp).
 * @return its path
 */
std::string tempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "contrib-test-XXXXXX").string();
    const char* dir = ::mkdtemp(pattern.data());
    return dir != nullptr ? dir : "/tmp";
}

/** A folder from tempDir, removed with what is in it when this goes (also when a check fails on the way). */
struct RemoveDir {
    std::string path;  ///< the folder
    ~RemoveDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

/**
 * Write a file.
 * @param path where
 * @param text what
 */
void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/**
 * Read a file.
 * @param path where
 * @return its text ("" if missing)
 */
std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/**
 * Whether a path is there.
 * @param path the path
 * @return true if it is
 */
bool there(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

/**
 * A status of eyecam-rec, fresh at a time.
 * @param state its state
 * @param protocol its protocol
 * @param now when it was written
 * @return the status
 */
eyecam::Status status(State state, const std::string& protocol, double now) {
    eyecam::Status s;
    s.present = true;
    s.state = state;
    s.stateText = state == State::Idle        ? "idle"
                  : state == State::Searching ? "searching"
                  : state == State::Recording ? "recording"
                  : state == State::Error     ? "error"
                                              : "calibrating";
    s.protocol = protocol;
    s.mtime = now;
    s.prox = -1.0;
    s.proxMin = 20.0;
    s.live = true;
    s.locked = true;
    s.camFps[0] = s.camFps[1] = NAN;
    return s;
}

/**
 * eyecam-rec set up and idle, as the panel would have it, with its count of taking-offs.
 * @param now when its status was written
 * @param offCount headset_off_count
 * @param started started_unix
 * @return the view
 */
eyecam::View eyecamReady(double now, long long offCount = 0, double started = kNow - 3600) {
    eyecam::View v;
    v.status = status(State::Idle, "", now);
    v.status.hasSetupDone = v.status.setupDone = true;
    v.status.hasBuffers = true;
    v.status.autoGrab = "ok";
    v.status.calibState = eyecam::kCalibWearBit;
    v.status.hasLidVeto = true;
    v.status.lidVeto = true;
    v.status.hasWidenSensitivity = true;
    v.status.widenSensitivity = 0.6;
    v.status.hasHeadsetOff = true;
    v.status.headsetOffCount = offCount;
    v.status.startedUnix = started;
    v.status.hasCamFps = true;
    v.status.camFps[0] = 90.0;
    v.status.camFps[1] = 89.9;
    v.visible = true;
    v.password = eyecam::PasswordState::Set;
    return v;
}

/**
 * The steps' sum of seconds in a protocol file, as eyecam reads it ("seconds label" lines, "#" comments).
 * @param text the file
 * @return the seconds (-1 for a line it doesn't read)
 */
double protocolSeconds(const std::string& text) {
    std::istringstream lines(text);
    std::string line;
    double total = 0.0;
    while (std::getline(lines, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream words(line);
        double seconds = 0.0;
        std::string label;
        if (!(words >> seconds)) {
            if (line.find_first_not_of(" \t\r") != std::string::npos) return -1.0;
            continue;
        }
        if (!(words >> label) || seconds <= 0) return -1.0;
        total += seconds;
    }
    return total;
}

void testSizes() {
    using namespace contrib;
    // eyecam-rec's estimate: 94.5 s of two 90 fps cameras at 160300 bytes a frame, about 2.7 GB
    CHECK(estimateBytes(90) == static_cast<long long>(94.5 * 2 * 90 * 160300.0));
    SAME(sizeText(estimateBytes(90)), "2.7 GB");
    SAME(sizeText(523'000'000), "523 MB");
    CHECK(estimateBytes(NAN) == estimateBytes(90));
    CHECK(estimateBytes(0) == estimateBytes(90));
    CHECK(estimateBytes(120) > estimateBytes(90));
    // It fits with its estimate and the margin free (or the free space unknown)
    CHECK(fits(estimateBytes(90) + kMarginBytes, 90));
    CHECK(!fits(estimateBytes(90) + kMarginBytes - 1, 90));
    CHECK(!fits(estimateBytes(90) + kMarginBytes, 120));
    CHECK(fits(-1, 90));
    // What packing needs: 0.7 of the recordings through zstd, all of them without, and the margin
    Info info;
    info.recordings.resize(2);
    info.recordings[0].bytes = info.recordings[1].bytes = 1'000'000'000;
    CHECK(recordedBytes(info) == 2'000'000'000LL);
    CHECK(packNeedBytes(info, true) == 1'400'000'000LL + kMarginBytes);
    CHECK(packNeedBytes(info, false) == 2'000'000'000LL + kMarginBytes);
    // The consent: none, an older text's, or no answer about glasses yet asks again
    Info consent;
    CHECK(consentNeeded(consent));
    consent.consentVersion = kConsentVersion;
    CHECK(consentNeeded(consent));
    consent.eyewear = Eyewear::NoAnswer;
    CHECK(!consentNeeded(consent));
    consent.consentVersion = kConsentVersion - 1;
    CHECK(consentNeeded(consent));
    for (const Eyewear e : {Eyewear::None, Eyewear::Glasses, Eyewear::Inserts, Eyewear::NoAnswer}) {
        CHECK(parseEyewear(eyewearName(e)) == e);
    }
    CHECK(parseEyewear("contacts") == Eyewear::Unasked);
}

void testProtocolFile() {
    using namespace contrib;
    // The protocol as eyecam ships it (install.sh puts every protocol_*.txt beside eyecam-rec)
    const std::string text = readFile(std::string(EYECAM_SOURCE_DIR) + "/protocol_" + kProtocol + ".txt");
    CHECK(!text.empty());
    CHECK(protocolSeconds(text) == kProtocolSec);
    const std::vector<std::string> steps = eyecam::parseProtocol(text);
    CHECK(steps.size() == 23);
    CHECK(std::count(steps.begin(), steps.end(), "close") == 3);
    CHECK(std::count(steps.begin(), steps.end(), "squint") == 3);
    CHECK(std::find(steps.begin(), steps.end(), "bright") == steps.end());
    // Every step is one the panel knows, with its own text in both languages (left and right as the wearer sees)
    for (const Language language : {Language::Ja, Language::En}) {
        const UiText& t = uiText(language);
        for (const std::string& label : steps) {
            CHECK(eyecam::parseStep(label) != eyecam::Step::Unknown);
            CHECK(eyecam::instruction(t, label) != label);
            if (label != "lead_in") CHECK(eyecam::chipLabel(t, label) != label);
        }
    }
    SAME(eyecam::instruction(uiText(Language::Ja), "look_left"), "左の耳の方を見て");
    SAME(eyecam::instruction(uiText(Language::En), "look_right"), "Look toward your right ear");
}

void testInfo() {
    using namespace contrib;
    contrib::Recording r;
    r.rec = "rec_2026-10-25_14-05-12";
    r.protocol = kProtocol;
    r.start = "2026-10-25 14:03:40 +0900";
    r.startUnix = kNow;
    r.endUnix = kNow + 97.25;
    r.bytes = 2'712'000'000;
    r.appVersion = "0.8.2-beta.2";
    r.eyecamRec = "294d06c6";
    r.lidVeto = "on";
    r.widen = "on";
    r.widenSensitivity = "0.60";
    r.cameraLids = "on";
    r.eyewear = "inserts";
    r.fps = 89.9;
    r.sincePrevious = "12.5";
    r.headsetOff = "yes";
    r.offCountStart = 3;
    r.offCountEnd = 3;
    r.eyecamStarted = kNow - 3600;
    r.calib = "rec_2026-10-25_14-05-12.calib_result.json";
    const std::string text = headerText() + consentSection("2026-10-25 14:02:10 +0900", 1, Eyewear::Inserts) +
                             recordingSection(r) + abortedSection("aborted/rec_2026-10-25_14-09-00", "stopped") +
                             packedSection("contrib_2026-10-25_14-25-02.tar.zst", 1, 1'600'000'000);
    // No personal data: what it is, the format, times, settings, folder names and sizes only
    CHECK(text.rfind("# frameeyeosc", 0) == 0);
    CHECK(text.find("format=2\n") != std::string::npos);
    CHECK(text.find("\n[consent]\ntime=2026-10-25 14:02:10 +0900\nversion=1\neyewear=inserts\n") != std::string::npos);
    CHECK(text.find("\nheadset_off=yes\n") != std::string::npos);
    CHECK(text.find("\nminutes_since_previous=12.5\n") != std::string::npos);
    CHECK(text.find("\ncamera_fps=89.9\n") != std::string::npos);
    CHECK(text.find("\neyecam_rec_sha256=294d06c6\n") != std::string::npos);
    Info info;
    parseInfo(text, info);
    CHECK(info.consentVersion == 1 && info.eyewear == Eyewear::Inserts);
    SAME(info.consent, "2026-10-25 14:02:10 +0900");
    CHECK(info.recordings.size() == 1);
    if (!info.recordings.empty()) {
        const contrib::Recording& back = info.recordings[0];
        SAME(back.rec, r.rec);
        SAME(back.protocol, r.protocol);
        SAME(back.start, r.start);
        CHECK(std::fabs(back.startUnix - r.startUnix) < 1e-3 && std::fabs(back.endUnix - r.endUnix) < 1e-3);
        CHECK(back.bytes == r.bytes);
        SAME(back.appVersion, r.appVersion);
        SAME(back.eyecamRec, r.eyecamRec);
        SAME(back.lidVeto, "on");
        SAME(back.widen, "on");
        SAME(back.widenSensitivity, "0.60");
        SAME(back.cameraLids, "on");
        SAME(back.eyewear, "inserts");
        CHECK(std::fabs(back.fps - 89.9) < 1e-9);
        SAME(back.sincePrevious, "12.5");
        SAME(back.headsetOff, "yes");
        CHECK(back.offCountStart == 3 && back.offCountEnd == 3);
        CHECK(std::fabs(back.eyecamStarted - r.eyecamStarted) < 1e-3);
        SAME(back.calib, r.calib);
    }
    CHECK(info.aborted == std::vector<std::string> {"aborted/rec_2026-10-25_14-09-00 stopped"});
    SAME(info.packed, "contrib_2026-10-25_14-25-02.tar.zst");
    CHECK(info.packedCount == 1 && info.packedBytes == 1'600'000'000);
    // Unknowns as they are written
    contrib::Recording unknown;
    unknown.rec = "rec_2026-10-25_14-20-55";
    const std::string second = recordingSection(unknown);
    CHECK(second.find("\neyecam_rec_sha256=unknown\n") != std::string::npos);
    CHECK(second.find("\ncamera_fps=unknown\n") != std::string::npos);
    CHECK(second.find("\neyecam_started_unix=unknown\n") != std::string::npos);
    CHECK(second.find("\nheadset_off_count_start=-1\n") != std::string::npos);
    // Sections added later: the last consent counts (an older text's asks again), recordings add up
    parseInfo(text + recordingSection(unknown) + consentSection("2026-11-01 10:00:00 +0900", 0, Eyewear::None), info);
    CHECK(info.recordings.size() == 2 && info.consentVersion == 0 && info.eyewear == Eyewear::None);
    CHECK(consentNeeded(info));
    if (info.recordings.size() == 2) CHECK(info.recordings[1].offCountStart == -1 && info.recordings[1].eyecamRec.empty());
    // Odd lines are skipped, CRLF read, a recording's name that isn't one dropped, a pack's that isn't one forgotten
    parseInfo("format=2\r\n[consent]\r\nversion=1\r\neyewear=glasses\r\nnonsense\r\n[recording]\r\nrec=../../etc\r\n"
              "[recording]\nrec=rec_2026-10-25_14-05-12\nbytes=12x\n[packed]\nfile=/etc/passwd\n[mystery]\nrec=x\n",
              info);
    CHECK(info.consentVersion == 1 && info.eyewear == Eyewear::Glasses);
    CHECK(info.recordings.size() == 1);
    if (!info.recordings.empty()) CHECK(info.recordings[0].bytes == 0);
    CHECK(info.packed.empty());
    parseInfo("", info);
    CHECK(consentNeeded(info) && info.recordings.empty());
}

void testNames() {
    using namespace contrib;
    CHECK(isRecordingName("rec_2026-10-25_14-02-10"));
    CHECK(!isRecordingName("calib_2026-10-25_14-02-10"));
    CHECK(!isRecordingName("rec_fake"));
    CHECK(!isRecordingName("rec_2026-10-25_14-02-10x"));
    CHECK(!isRecordingName("aborted/rec_2026-10-25_14-02-10"));
    CHECK(isPackName("contrib_2026-10-25_14-25-02.tar.zst"));
    CHECK(isPackName("contrib_2026-10-25_14-25-02.tar"));
    CHECK(!isPackName("contrib_2026-10-25_14-25-02.zip"));
    CHECK(!isPackName("contrib_2026-10-25.tar"));
    CHECK(!isPackName("../contrib_2026-10-25_14-25-02.tar"));
    CHECK(isPackName(packName(static_cast<std::time_t>(kNow), true)));
    CHECK(packName(static_cast<std::time_t>(kNow), false).size() == 31);
    const std::string local = localTime(static_cast<std::time_t>(kNow), true);
    CHECK(local.size() == 25 && local[10] == ' ' && (local[20] == '+' || local[20] == '-'));
    CHECK(localTime(static_cast<std::time_t>(kNow), false).size() == 19);
}

void testHeadsetOff() {
    contrib::Recording before;
    before.offCountEnd = 2;
    before.eyecamStarted = kNow - 3600;
    // Within one run of eyecam-rec: the count went up, or it didn't
    SAME(contrib::headsetOffBetween(before, 3, kNow - 3600), "yes");
    SAME(contrib::headsetOffBetween(before, 2, kNow - 3600.4), "no");
    // eyecam-rec restarted between (its count starts again), or a count not known: unknown
    SAME(contrib::headsetOffBetween(before, 5, kNow - 60), "unknown");
    SAME(contrib::headsetOffBetween(before, -1, kNow - 3600), "unknown");
    before.offCountEnd = -1;
    SAME(contrib::headsetOffBetween(before, 2, kNow - 3600), "unknown");
    before.offCountEnd = 2;
    before.eyecamStarted = 0;
    SAME(contrib::headsetOffBetween(before, 2, kNow - 3600), "unknown");
}

void testRunWatch() {
    using contrib::RunWatch;
    const std::string p = contrib::kProtocol;
    const std::string dir = "/home/steamos/eyecam/rec_2026-10-25_14-05-12";
    // Searching, recording, then idle with its folder: done
    {
        RunWatch w;
        w.begin(p, kNow);
        CHECK(w.active());
        CHECK(w.follow(status(State::Searching, p, kNow + 0.1), true, kNow + 0.1, NAN).outcome == Outcome::Running);
        CHECK(w.follow(status(State::Recording, p, kNow + 30), true, kNow + 30, NAN).outcome == Outcome::Running);
        eyecam::Status s = status(State::Idle, p, kNow + 97);
        s.sessionDir = dir;
        s.message = "保存した: rec_2026-10-25_14-05-12";
        const contrib::RunEnd end = w.follow(s, true, kNow + 97, kNow + 2);
        CHECK(end.outcome == Outcome::Done);
        SAME(end.dir, dir);
    }
    // ...ended early (Stop, or the eyes lost): stopped, with the part's folder
    {
        RunWatch w;
        w.begin(p, kNow);
        w.follow(status(State::Recording, p, kNow + 10), true, kNow + 10, NAN);
        eyecam::Status s = status(State::Idle, p, kNow + 20);
        s.sessionDir = dir;
        s.lastSessionAborted = true;
        const contrib::RunEnd end = w.follow(s, true, kNow + 20, kNow + 2);
        CHECK(end.outcome == Outcome::Stopped);
        CHECK(end.why == contrib::Why::Stopped);
        SAME(end.dir, dir);
    }
    // ...stopped while it searched: no folder (eyecam-rec cleared session_dir when it started)
    {
        RunWatch w;
        w.begin(p, kNow);
        w.follow(status(State::Searching, p, kNow + 1), true, kNow + 1, NAN);
        const contrib::RunEnd end = w.follow(status(State::Idle, p, kNow + 5), true, kNow + 5, NAN);
        CHECK(end.outcome == Outcome::Stopped);
        CHECK(end.dir.empty());
    }
    // ...failed: eyecam-rec's error, with what it saved
    {
        RunWatch w;
        w.begin(p, kNow);
        w.follow(status(State::Recording, p, kNow + 10), true, kNow + 10, NAN);
        eyecam::Status s = status(State::Error, p, kNow + 20);
        s.sessionDir = dir;
        s.message = "ディスクの空きが 1 GB 未満（途中まで保存: rec_2026-10-25_14-05-12）";
        s.messageEn = "Less than 1 GB of disk space left (saved so far: rec_2026-10-25_14-05-12)";
        const contrib::RunEnd end = w.follow(s, true, kNow + 20, kNow + 2);
        CHECK(end.outcome == Outcome::Failed);
        CHECK(end.why == contrib::Why::Error);
        SAME(end.dir, dir);
        SAME(end.message, s.message);
        SAME(end.messageEn, s.messageEn);
    }
    // An older status (idle, the previous recording's folder) right after the start: not this one's end
    {
        RunWatch w;
        w.begin(p, kNow);
        eyecam::Status s = status(State::Idle, p, kNow + 0.05);
        s.sessionDir = "/home/steamos/eyecam/rec_2026-10-25_14-02-41";
        CHECK(w.follow(s, true, kNow + 0.05, kNow - 120).outcome == Outcome::Running);
        // ...and never running at all: it didn't start
        const contrib::RunEnd end = w.follow(s, true, kNow + contrib::kStartGraceSec, kNow - 120);
        CHECK(end.outcome == Outcome::Failed);
        CHECK(end.why == contrib::Why::NoStart);
        CHECK(end.dir.empty());
    }
    // A start that failed at once (error before the panel saw it searching): its message, after the grace
    {
        RunWatch w;
        w.begin(p, kNow);
        eyecam::Status s = status(State::Error, p, kNow + 0.2);
        s.message = "ディスクの空きが 1 GB 未満";
        CHECK(w.follow(s, true, kNow + 0.2, NAN).outcome == Outcome::Running);
        const contrib::RunEnd end = w.follow(s, true, kNow + 9, NAN);
        CHECK(end.outcome == Outcome::Failed);
        CHECK(end.why == contrib::Why::Error);
        SAME(end.message, s.message);
    }
    // A calibration or another recording running: not this one (it never started)
    {
        RunWatch w;
        w.begin(p, kNow);
        CHECK(w.follow(status(State::Recording, "widen", kNow + 1), true, kNow + 1, NAN).outcome == Outcome::Running);
        CHECK(w.follow(status(State::Calibrating, "calib wear", kNow + 9), true, kNow + 9, NAN).outcome ==
              Outcome::Failed);
    }
    // Missed while it ran (the loop slow): its folder made after the start says it ended; another protocol's doesn't
    {
        RunWatch w;
        w.begin(p, kNow);
        eyecam::Status s = status(State::Idle, p, kNow + 400);
        s.sessionDir = dir;
        CHECK(w.follow(s, true, kNow + 400, kNow + 3).outcome == Outcome::Done);
        RunWatch other;
        other.begin(p, kNow);
        eyecam::Status s2 = status(State::Idle, "widen", kNow + 400);
        s2.sessionDir = dir;
        CHECK(other.follow(s2, true, kNow + 400, kNow + 3).outcome == Outcome::Failed);
    }
    // ...its folder gone (moved by hand): missing
    {
        RunWatch w;
        w.begin(p, kNow);
        w.follow(status(State::Recording, p, kNow + 10), true, kNow + 10, NAN);
        eyecam::Status s = status(State::Idle, p, kNow + 97);
        s.sessionDir = dir;
        const contrib::RunEnd end = w.follow(s, true, kNow + 97, NAN);
        CHECK(end.outcome == Outcome::Failed);
        CHECK(end.why == contrib::Why::Missing);
    }
    // eyecam-rec gone (its status stale) for kGoneSec: failed, not before
    {
        RunWatch w;
        w.begin(p, kNow);
        w.follow(status(State::Recording, p, kNow + 10), true, kNow + 10, NAN);
        const eyecam::Status stale = status(State::Recording, p, kNow + 10);
        CHECK(w.follow(stale, false, kNow + 16, NAN).outcome == Outcome::Running);
        CHECK(w.follow(stale, false, kNow + 16 + contrib::kGoneSec - 1, NAN).outcome == Outcome::Running);
        const contrib::RunEnd end = w.follow(stale, false, kNow + 16 + contrib::kGoneSec, NAN);
        CHECK(end.outcome == Outcome::Failed);
        CHECK(end.why == contrib::Why::Gone);
    }
    // Not following one: nothing
    {
        RunWatch w;
        CHECK(!w.active());
        CHECK(w.follow(status(State::Idle, p, kNow), true, kNow, NAN).outcome == Outcome::Running);
    }
}

void testScreen() {
    using namespace contrib;
    View v;
    const eyecam::View ready = eyecamReady(kNow);
    // The consent first (then the question about glasses), from "Record"; "Recordings" goes to the end's screen
    CHECK(screen(v, Page::Record, false) == Screen::Consent);
    CHECK(screen(v, Page::Record, true) == Screen::Eyewear);
    CHECK(screen(v, Page::Manage, false) == Screen::Done);
    CHECK(!startAllowed(v, ready));
    v.exists = true;
    v.info.consentVersion = kConsentVersion;
    v.info.eyewear = Eyewear::None;
    v.freeBytes = 20'000'000'000LL;
    CHECK(screen(v, Page::Record, false) == Screen::Ready);
    CHECK(screen(v, Page::Record, true) == Screen::Ready);
    CHECK(startAllowed(v, ready));
    // eyecam-rec busy, not set up, a command waiting, no room, packing: no start
    {
        eyecam::View busy = ready;
        busy.status.state = State::Calibrating;
        CHECK(!startAllowed(v, busy));
        eyecam::View waiting = ready;
        waiting.busy = true;
        CHECK(!startAllowed(v, waiting));
        eyecam::View notSetUp = ready;
        notSetUp.status.setupDone = false;
        notSetUp.status.calibState = 0;
        CHECK(!startAllowed(v, notSetUp));
        eyecam::View error = ready;
        error.status.state = State::Error;
        CHECK(startAllowed(v, error));
        View full = v;
        full.freeBytes = 3'000'000'000LL;
        CHECK(!startAllowed(full, ready));
        View packing = v;
        packing.progress.running = true;
        CHECK(!startAllowed(packing, ready));
        View older = v;
        older.info.consentVersion = kConsentVersion - 1;
        CHECK(!startAllowed(older, ready));
        CHECK(screen(older, Page::Record, false) == Screen::Consent);
    }
    // Running, then how it ended
    v.running = true;
    CHECK(screen(v, Page::Record, false) == Screen::Running);
    CHECK(!startAllowed(v, ready));
    v.running = false;
    v.failure.shown = true;
    CHECK(screen(v, Page::Record, false) == Screen::Failed);
    v.failure = Failure();
    v.justDone = true;
    CHECK(screen(v, Page::Record, false) == Screen::Done);
    // Being deleted: that, whatever else
    v.progress.job = Job::Delete;
    v.progress.running = true;
    CHECK(screen(v, Page::Manage, false) == Screen::Deleting);
}

/**
 * Whether a tar header's checksum adds up.
 * @param header the 512 bytes
 * @return true if it does
 */
bool checksumOk(const std::string& header) {
    unsigned int sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += i >= 148 && i < 156 ? ' ' : static_cast<unsigned char>(header[i]);
    return std::strtoul(header.substr(148, 7).c_str(), nullptr, 8) == sum;
}

void testTarHeader() {
    using contrib::Entry;
    Entry file {"/x/eye_L.raw", "contrib/rec_2026-10-25_14-02-41/eye_L.raw", 1'000'320'000, 1'790'000'000, false};
    const std::string header = contrib::tarHeader(file);
    CHECK(header.size() == 512);
    SAME(header.substr(0, file.name.size()), file.name);
    CHECK(header[file.name.size()] == '\0');
    SAME(header.substr(124, 11), "07347726000");  // 1000320000 in octal
    CHECK(header[156] == '0');
    SAME(header.substr(257, 6), std::string("ustar\0", 6));
    SAME(header.substr(263, 2), "00");
    SAME(header.substr(100, 7), "0000644");
    CHECK(checksumOk(header));
    // A folder
    const Entry folder {"/x", "contrib/", 0, 1'790'000'000, true};
    const std::string dir = contrib::tarHeader(folder);
    CHECK(dir[156] == '5');
    SAME(dir.substr(100, 7), "0000755");
    SAME(dir.substr(124, 11), "00000000000");
    CHECK(checksumOk(dir));
    // 8 GiB and more: GNU's base-256
    Entry big = file;
    big.size = 9'000'000'000LL;
    const std::string large = contrib::tarHeader(big);
    CHECK(static_cast<unsigned char>(large[124]) == 0x80);
    unsigned long long size = 0;
    for (int i = 125; i < 136; ++i) size = size << 8 | static_cast<unsigned char>(large[i]);
    CHECK(size == 9'000'000'000ULL);
    CHECK(checksumOk(large));
    // A name of 100 bytes or more doesn't fit
    Entry long_ = file;
    long_.name = std::string(100, 'a');
    CHECK(contrib::tarHeader(long_).empty());
    long_.name = std::string(99, 'a');
    CHECK(contrib::tarHeader(long_).size() == 512);
}

/**
 * Make a recording's folder as eyecam-rec leaves one.
 * @param home where
 * @param name its name
 * @param bytes the size of its eye_L.raw (eye_R.raw is a little smaller)
 * @param made when it was made (its modification time; the tests' clock runs ahead of the real one)
 * @param without a file it lacks ("" = none)
 * @return its path
 */
std::string makeRecording(const std::string& home, const std::string& name, size_t bytes, double made = kNow,
                          const std::string& without = "") {
    const std::string dir = home + "/" + name;
    ::mkdir(dir.c_str(), 0755);
    std::string video(bytes, '\0');
    for (size_t i = 0; i < bytes; ++i) video[i] = static_cast<char>((i * 7 + i / 400) & 0xff);
    writeFile(dir + "/eye_L.raw", video);
    writeFile(dir + "/eye_R.raw", video.substr(0, bytes - 13));
    for (const auto& [file, text] : {std::pair<const char*, const char*> {"cues.csv", "t,label\n0.0,lead_in\n"},
                                     {"valve.csv", "t,openness_l\n"}, {"frames.csv", "eye,t_cam\n"},
                                     {"meta.txt", "protocol=contrib\n"}}) {
        if (without != file) writeFile(dir + "/" + file, text);
    }
    const timespec times[2] = {{static_cast<time_t>(made), 0}, {static_cast<time_t>(made), 0}};
    ::utimensat(AT_FDCWD, dir.c_str(), times, 0);
    return dir;
}

void testFiles() {
    using namespace contrib;
    const RemoveDir temp {tempDir()};
    const std::string home = temp.path + "/eyecam";
    const std::string dir = home + "/" + kFolderName;
    ::mkdir(home.c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    // info.txt: its header once, then the sections in order, on the disk
    std::string error;
    CHECK(appendInfo(dir, consentSection("2026-10-25 14:02:10 +0900", 1, Eyewear::None), error));
    CHECK(appendInfo(dir, abortedSection("aborted/rec_2026-10-25_14-04-00", "stopped"), error));
    const std::string info = readFile(dir + "/" + kInfoFile);
    CHECK(info.rfind(headerText(), 0) == 0);
    CHECK(info.find("# frameeyeosc", 1) == std::string::npos);
    CHECK(info.find("[consent]") < info.find("[aborted]"));
    CHECK(!appendInfo(temp.path + "/missing", "x", error) && !error.empty());
    // The files a recording needs
    const std::string full = makeRecording(home, "rec_2026-10-25_14-02-41", 4096);
    CHECK(missingFiles(full).empty());
    const std::string lacking = makeRecording(home, "rec_2026-10-25_14-03-00", 100, kNow, "cues.csv");
    CHECK(missingFiles(lacking) == std::vector<std::string> {"cues.csv"});
    CHECK(missingFiles(temp.path + "/none").size() == 4);
    // Moved into the folder, and one that didn't finish into aborted/
    std::string name;
    CHECK(takeRecording(home, dir, full, false, name, error));
    SAME(name, "rec_2026-10-25_14-02-41");
    CHECK(there(dir + "/" + name + "/eye_L.raw") && !there(full));
    CHECK(takeRecording(home, dir, lacking, true, name, error));
    SAME(name, "aborted/rec_2026-10-25_14-03-00");
    // Only eyecam-rec's own folders right in home
    const std::string elsewhere = temp.path + "/rec_2026-10-25_14-09-00";
    ::mkdir(elsewhere.c_str(), 0755);
    CHECK(!takeRecording(home, dir, elsewhere, false, name, error));
    ::mkdir((home + "/calib_2026-10-25_14-09-00").c_str(), 0755);
    CHECK(!takeRecording(home, dir, home + "/calib_2026-10-25_14-09-00", false, name, error));
    CHECK(!takeRecording(home, dir, home + "/rec_2026-10-25_14-59-59", false, name, error));  // (not there)
    // Sizes: the files in it and its folders
    CHECK(folderBytes(dir + "/rec_2026-10-25_14-02-41") == 4096 + 4096 - 13 + 20 + 13 + 10 + 17);
    CHECK(folderBytes(dir) > folderBytes(dir + "/rec_2026-10-25_14-02-41"));
    CHECK(freeBytes(home) > 0);
    CHECK(freeBytes(temp.path + "/missing") == -1);
    // The newest calibration result (by name), skipping a folder without one
    CHECK(newestCalibResult(home).empty());
    ::mkdir((home + "/calib_2026-10-24_10-00-00").c_str(), 0755);
    writeFile(home + "/calib_2026-10-24_10-00-00/calib_result.json", "{\"old\":1}");
    ::mkdir((home + "/calib_2026-10-25_09-00-00").c_str(), 0755);
    writeFile(home + "/calib_2026-10-25_09-00-00/calib_result.json", "{\"new\":1}");
    SAME(newestCalibResult(home), "{\"new\":1}");
}

/**
 * Wait for the worker's job to end.
 * @param worker the worker
 * @return how it ended
 */
contrib::Progress waitFor(contrib::Worker& worker) {
    contrib::Progress ended;
    for (int i = 0; i < 3000 && !worker.takeFinished(ended); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ended;
}

/**
 * Run a shell command.
 * @param command the command
 * @return true if it exited 0
 */
bool run(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

void testPack() {
    using namespace contrib;
    const RemoveDir temp {tempDir()};
    const std::string home = temp.path + "/eyecam";
    const std::string dir = home + "/" + kFolderName;
    ::mkdir(home.c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    std::string error;
    std::string name;
    const std::string names[3] = {"rec_2026-10-25_14-02-41", "rec_2026-10-25_14-05-12", "rec_2026-10-25_14-07-30"};
    Info info;
    for (int i = 0; i < 3; ++i) {
        takeRecording(home, dir, makeRecording(home, names[i], i == 1 ? 1'500'000 : 300'000, kNow), false, name, error);
        contrib::Recording r;
        r.rec = names[i];
        r.calib = i == 0 ? names[i] + ".calib_result.json" : "none";
        info.recordings.push_back(r);
        appendInfo(dir, recordingSection(r), error);
    }
    writeFile(dir + "/" + names[0] + ".calib_result.json", "{\"calib\":1}");
    // (the third lost a file since: left out and named)
    ::unlink((dir + "/" + names[2] + "/valve.csv").c_str());
    takeRecording(home, dir, makeRecording(home, "rec_2026-10-25_14-04-00", 1000), true, name, error);
    writeFile(dir + "/contrib_2026-10-24_09-00-00.tar.zst", "an older pack");
    // What goes in: the folder, info.txt, the complete recordings (sorted) and the calibration result; not aborted/,
    // not a pack
    std::vector<Entry> entries;
    std::vector<std::string> incomplete;
    CHECK(packEntries(dir, info, entries, incomplete, error));
    CHECK(incomplete == std::vector<std::string> {names[2]});
    CHECK(entries.size() == 2 + 7 + 1 + 7);
    SAME(entries[0].name, "contrib/");
    SAME(entries[1].name, "contrib/info.txt");
    SAME(entries[2].name, "contrib/" + names[0] + "/");
    SAME(entries[3].name, "contrib/" + names[0] + "/cues.csv");
    SAME(entries[9].name, "contrib/" + names[0] + ".calib_result.json");
    for (const Entry& entry : entries) {
        CHECK(entry.name.find("aborted") == std::string::npos && entry.name.find(".tar") == std::string::npos);
        CHECK(entry.name.find(names[2]) == std::string::npos);
    }
    // Packed as tar: unpacks to the same files
    Worker worker;
    const std::string tarName = packName(static_cast<std::time_t>(kNow), false);
    CHECK(worker.pack(dir, entries, tarName, 2, false));
    Progress ended = waitFor(worker);
    CHECK(ended.finished && ended.ok && ended.count == 2);
    SAME(ended.output, tarName);
    CHECK(ended.done == ended.total && ended.total > 0);
    const std::string tar = dir + "/" + tarName;
    CHECK(there(tar) && !there(tar + ".part"));
    CHECK(ended.outputBytes == static_cast<long long>(std::filesystem::file_size(tar)));
    const std::string out = temp.path + "/out";
    ::mkdir(out.c_str(), 0755);
    CHECK(run("tar -xf '" + tar + "' -C '" + out + "'"));
    for (int i = 0; i < 2; ++i) {
        for (const char* file : {"eye_L.raw", "eye_R.raw", "cues.csv", "valve.csv", "frames.csv", "meta.txt"}) {
            const std::string a = readFile(dir + "/" + names[i] + "/" + file);
            CHECK(!a.empty() && a == readFile(out + "/contrib/" + names[i] + "/" + file));
        }
    }
    SAME(readFile(out + "/contrib/info.txt"), readFile(dir + "/info.txt"));
    SAME(readFile(out + "/contrib/" + names[0] + ".calib_result.json"), "{\"calib\":1}");
    CHECK(!there(out + "/contrib/aborted") && !there(out + "/contrib/" + names[2]));
    // ...and through zstd, when it is there
    if (zstdAvailable()) {
        const std::string zstName = packName(static_cast<std::time_t>(kNow) + 1, true);
        CHECK(worker.pack(dir, entries, zstName, 2, true));
        ended = waitFor(worker);
        CHECK(ended.finished && ended.ok);
        const std::string out2 = temp.path + "/out2";
        ::mkdir(out2.c_str(), 0755);
        CHECK(run("zstd -dc '" + dir + "/" + zstName + "' | tar -xf - -C '" + out2 + "'"));
        SAME(readFile(out2 + "/contrib/" + names[1] + "/eye_L.raw"), readFile(dir + "/" + names[1] + "/eye_L.raw"));
    } else {
        std::printf("contrib-test: no zstd here, its pack not tried\n");
    }
    // Deleting: only home/contrib itself
    CHECK(!worker.remove(home, dir + "/" + names[0]));
    CHECK(!worker.remove(temp.path, dir));
    CHECK(worker.remove(home, dir));
    ended = waitFor(worker);
    CHECK(ended.finished && ended.ok && ended.job == Job::Delete);
    CHECK(!there(dir) && there(home));
}

/**
 * Run one recording through a session: started, answered, recording, then idle with its folder.
 * @param session the session
 * @param view its view
 * @param home eyecam-rec's folder
 * @param now the tests' clock (moved on)
 * @param eyecam eyecam-rec as it is before (its count and run)
 * @param name the recording's folder
 * @param without a file it lacks
 * @param aborted it ended early
 */
void record(contrib::Session& session, contrib::View& view, const std::string& home, double& now, eyecam::View eyecam,
            const std::string& name, const std::string& without = "", bool aborted = false) {
    std::string log;
    contrib::Snapshot snapshot;
    snapshot.appVersion = "0.8.2-test";
    snapshot.eyecamRec = "294d06c6";
    eyecam.status.mtime = now;
    const std::string command = session.startCommand(view, eyecam);
    SAME(command, "start contrib");
    session.started(view, eyecam, snapshot, command, now);
    eyecam::Reply ok;
    ok.ok = true;
    ok.command = command;
    CHECK(!session.replied(view, ok));
    now += 1;
    eyecam::View running = eyecam;
    running.status = status(State::Recording, contrib::kProtocol, now);
    session.follow(view, running, now, now, log);
    CHECK(view.running);
    const std::string rec = makeRecording(home, name, 600, now, without);
    now += aborted ? 20 : 96;
    eyecam::View after = eyecam;
    after.status.mtime = now;
    after.status.sessionDir = rec;
    after.status.protocol = contrib::kProtocol;
    after.status.lastSessionAborted = aborted;
    CHECK(session.follow(view, after, now, now, log));
    CHECK(!view.running);
}

void testSession() {
    using namespace contrib;
    const RemoveDir temp {tempDir()};
    const std::string home = temp.path + "/eyecam";
    ::mkdir(home.c_str(), 0755);
    ::mkdir((home + "/calib_2026-10-25_09-00-00").c_str(), 0755);
    writeFile(home + "/calib_2026-10-25_09-00-00/calib_result.json", "{\"ok\":true}");
    View view;
    Session session;
    std::string log;
    session.load(view, home);
    CHECK(view.loaded && !view.exists && view.freeBytes > 0);
    CHECK(screen(view, Page::Record, false) == Screen::Consent);
    double now = static_cast<double>(std::time(nullptr));
    const double started = now - 3600;
    // No start before the consent, nor without an answer
    CHECK(session.startCommand(view, eyecamReady(now)).empty());
    CHECK(!session.consent(view, Eyewear::Unasked, static_cast<std::time_t>(now)));
    CHECK(session.consent(view, Eyewear::Glasses, static_cast<std::time_t>(now)));
    CHECK(view.exists && view.info.eyewear == Eyewear::Glasses && !consentNeeded(view.info));
    CHECK(screen(view, Page::Record, false) == Screen::Ready);
    // The first: moved in, its section with the settings and the calibration result beside it
    record(session, view, home, now, eyecamReady(now, 2, started), "rec_2026-10-25_14-02-41");
    CHECK(view.justDone && screen(view, Page::Record, false) == Screen::Done);
    CHECK(view.info.recordings.size() == 1);
    if (view.info.recordings.size() == 1) {
        const Recording& r = view.info.recordings[0];
        SAME(r.rec, "rec_2026-10-25_14-02-41");
        SAME(r.protocol, kProtocol);
        SAME(r.appVersion, "0.8.2-test");
        SAME(r.eyecamRec, "294d06c6");
        SAME(r.lidVeto, "on");
        SAME(r.widenSensitivity, "0.60");
        SAME(r.eyewear, "glasses");
        SAME(r.sincePrevious, "none");
        SAME(r.headsetOff, "none");
        CHECK(r.offCountStart == 2 && r.offCountEnd == 2);
        CHECK(std::fabs(r.fps - 89.9) < 1e-6);
        SAME(r.calib, "rec_2026-10-25_14-02-41.calib_result.json");
        CHECK(r.bytes > 1000);
    }
    SAME(readFile(view.dir + "/rec_2026-10-25_14-02-41.calib_result.json"), "{\"ok\":true}");
    CHECK(there(view.dir + "/rec_2026-10-25_14-02-41/cues.csv") && !there(home + "/rec_2026-10-25_14-02-41"));
    // The second, 10 minutes later with the headset taken off between (the count went up)
    now += 600;
    record(session, view, home, now, eyecamReady(now, 3, started), "rec_2026-10-25_14-12-41");
    CHECK(view.info.recordings.size() == 2);
    if (view.info.recordings.size() == 2) {
        SAME(view.info.recordings[1].headsetOff, "yes");
        SAME(view.info.recordings[1].sincePrevious, "10.0");
    }
    // The third right after it (no taking-off), the fourth after eyecam-rec restarted (unknown)
    record(session, view, home, now, eyecamReady(now, 3, started), "rec_2026-10-25_14-14-30");
    record(session, view, home, now, eyecamReady(now, 0, now - 5), "rec_2026-10-25_14-16-20");
    CHECK(view.info.recordings.size() == 4);
    if (view.info.recordings.size() == 4) {
        SAME(view.info.recordings[2].headsetOff, "no");
        SAME(view.info.recordings[2].sincePrevious, "0.0");
        SAME(view.info.recordings[3].headsetOff, "unknown");
    }
    // One lacking a file: into aborted/, the screen says which, to record again
    record(session, view, home, now, eyecamReady(now, 0, now - 5), "rec_2026-10-25_14-18-00", "frames.csv");
    CHECK(view.info.recordings.size() == 4 && view.failure.shown && view.failure.why == Why::Incomplete);
    SAME(view.failure.message, "frames.csv");
    SAME(view.failure.kept, "aborted/rec_2026-10-25_14-18-00");
    CHECK(screen(view, Page::Record, false) == Screen::Failed);
    CHECK(there(view.dir + "/aborted/rec_2026-10-25_14-18-00"));
    // One stopped part way: into aborted/ too
    record(session, view, home, now, eyecamReady(now, 0, now - 5), "rec_2026-10-25_14-19-00", "", true);
    CHECK(view.failure.why == Why::Stopped && view.info.aborted.size() == 2);
    // Refused at once (a reply with an error): failed with its reason
    {
        const eyecam::View eyecam = eyecamReady(now);
        session.started(view, eyecam, Snapshot(), "start contrib", now);
        eyecam::Reply refused;
        refused.command = "start contrib";
        refused.error = "プロトコルが見つからない: protocol_contrib.txt";
        CHECK(session.replied(view, refused));
        CHECK(!view.running && view.failure.why == Why::Refused);
        SAME(view.failure.message, refused.error);
    }
    // Read again from the disk: the same
    {
        View again;
        Session restarted;
        restarted.load(again, home);
        CHECK(again.exists && again.info.recordings.size() == 4 && again.info.aborted.size() == 2);
        CHECK(again.info.eyewear == Eyewear::Glasses && !consentNeeded(again.info));
    }
    // Packed (tar); another recording, packed again: the first pack goes, the recordings stay
    view.compress = false;
    CHECK(session.pack(view, static_cast<std::time_t>(now)));
    for (int i = 0; i < 3000 && view.info.packed.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        session.follow(view, eyecamReady(now), now, now, log);
    }
    const std::string first = view.info.packed;
    CHECK(isPackName(first) && view.info.packedCount == 4 && view.info.packedBytes > 0);
    CHECK(there(view.dir + "/" + first));
    record(session, view, home, now, eyecamReady(now), "rec_2026-10-25_14-30-00");
    CHECK(session.pack(view, static_cast<std::time_t>(now) + 60));
    for (int i = 0; i < 3000 && view.info.packed == first; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        session.follow(view, eyecamReady(now), now, now, log);
    }
    CHECK(view.info.packed != first && view.info.packedCount == 5);
    CHECK(!there(view.dir + "/" + first) && there(view.dir + "/" + view.info.packed));
    CHECK(there(view.dir + "/rec_2026-10-25_14-02-41/eye_L.raw"));
    // Deleted: the consent again, saying so
    CHECK(session.remove(view));
    for (int i = 0; i < 3000 && view.exists; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        session.follow(view, eyecamReady(now), now, now, log);
    }
    CHECK(!view.exists && view.deleted && !there(home + "/" + kFolderName));
    CHECK(screen(view, Page::Record, false) == Screen::Consent);
    CHECK(there(home + "/calib_2026-10-25_09-00-00/calib_result.json"));
}

}  // namespace

int main() {
    testSizes();
    testProtocolFile();
    testInfo();
    testNames();
    testHeadsetOff();
    testRunWatch();
    testScreen();
    testTarHeader();
    testFiles();
    testPack();
    testSession();
    if (gFailures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("contrib-test: all passed\n");
    return 0;
}
