// Tests for "Help train the AI" (contrib.cpp): the steps and their sizes (against eyecam's own protocol files),
// info.txt (written, read back, odd lines), the folder names, following a step's recording in eyecam-rec's status
// (done, stopped while searching or recording, failed, refused, never started, eyecam-rec gone, a panel restarted in
// the middle), the re-wear between steps (the eye video, the proximity sensor, a sensor that never reads worn, the
// Frame asleep, the fallback's delay), which screen shows and when a start can be pressed, the tar headers, and on real
// folders in a temporary one (never ~/eyecam): making a contribution, moving recordings into it, packing it (tar, and
// through zstd when it is there: unpacked again with the system's tar and compared) and deleting it, and a whole
// session from the consent to the end. Built with the panel as contrib-test; exits non-zero on failure.
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
 * eyecam-rec set up and idle, as the panel would have it.
 * @param now when its status was written
 * @return the view
 */
eyecam::View eyecamReady(double now) {
    eyecam::View v;
    v.status = status(State::Idle, "", now);
    v.status.hasSetupDone = v.status.setupDone = true;
    v.status.hasBuffers = true;
    v.status.autoGrab = "ok";
    v.status.calibState = eyecam::kCalibWearBit;
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

void testSteps() {
    using namespace contrib;
    SAME(protocolOf(0), kGuidedProtocol);
    SAME(protocolOf(2), kGuidedProtocol);
    SAME(protocolOf(3), kFreeProtocol);
    SAME(protocolOf(4), kFreeProtocol);
    CHECK(!isFree(2) && isFree(3) && isFree(4));
    CHECK(!needsRewear(0) && needsRewear(1) && needsRewear(3) && needsRewear(4));
    // eyecam-rec's estimate: 60.5 s and 181.5 s of two 90 fps cameras at 160300 bytes a frame
    CHECK(estimateBytes(0, 90) == static_cast<long long>(60.5 * 2 * 90 * 160300.0));
    CHECK(estimateBytes(3, 90) == static_cast<long long>(181.5 * 2 * 90 * 160300.0));
    CHECK(estimateBytes(0, NAN) == estimateBytes(0, 90));
    CHECK(estimateBytes(0, 0) == estimateBytes(0, 90));
    CHECK(estimateBytes(0, 120) > estimateBytes(0, 90));
    // About 1.75 GB a guided one, 5.2 GB the free use: 10.5 GB for the four
    Info info;
    const long long all = neededBytes(info, 90) - kMarginBytes;
    CHECK(all > 10'400'000'000LL && all < 10'600'000'000LL);
    SAME(sizeText(all), "10.5 GB");
    SAME(sizeText(523'000'000), "523 MB");
    // The order: the first not done; the optional one only when asked for, after the others
    CHECK(nextStep(info, false) == 0);
    CHECK(nextStep(info, true) == 0);
    info.steps[0].rec = "rec_2026-10-25_14-02-41";
    info.steps[1].rec = "rec_2026-10-25_14-05-12";
    CHECK(nextStep(info, false) == 2);
    CHECK(requiredDone(info) == 2);
    CHECK(neededBytes(info, 90) == kMarginBytes + estimateBytes(2, 90) + estimateBytes(3, 90));
    info.steps[2].rec = "rec_2026-10-25_14-08-03";
    info.steps[3].rec = "rec_2026-10-25_14-11-40";
    CHECK(nextStep(info, false) == kStepCount);
    CHECK(nextStep(info, true) == 4);
    CHECK(requiredDone(info) == kRequiredSteps);
    CHECK(neededBytes(info, 90) == kMarginBytes);
    info.steps[4].rec = "rec_2026-10-25_14-20-00";
    CHECK(nextStep(info, true) == kStepCount);
    // Sizes: what was recorded, what packing needs
    for (Step& step : info.steps) step.bytes = 1'000'000'000;
    CHECK(recordedBytes(info) == 5'000'000'000LL);
    CHECK(packNeedBytes(info, true) == 3'750'000'000LL + kMarginBytes);
    CHECK(packNeedBytes(info, false) == 5'000'000'000LL + kMarginBytes);
    // A step fits with its estimate and the margin free (or the free space unknown)
    CHECK(fits(0, estimateBytes(0, 90) + kMarginBytes, 90));
    CHECK(!fits(0, estimateBytes(0, 90) + kMarginBytes - 1, 90));
    CHECK(!fits(3, estimateBytes(0, 90) + kMarginBytes, 90));
    CHECK(fits(3, -1, 90));
}

void testProtocolFiles() {
    using namespace contrib;
    // The protocols as eyecam ships them (install.sh puts every protocol_*.txt beside eyecam-rec)
    const std::string guided = readFile(std::string(EYECAM_SOURCE_DIR) + "/protocol_" + kGuidedProtocol + ".txt");
    const std::string free = readFile(std::string(EYECAM_SOURCE_DIR) + "/protocol_" + kFreeProtocol + ".txt");
    CHECK(!guided.empty() && !free.empty());
    CHECK(protocolSeconds(guided) == kGuidedSec);
    CHECK(protocolSeconds(free) == kFreeSec);
    // Its steps are what the panel says: close / normal / widen, then squint, up, down; no light
    const std::vector<std::string> steps = eyecam::parseProtocol(guided);
    CHECK(steps.size() == 14);
    for (const char* label : {"close", "normal", "widen", "squint", "look_up", "look_down"}) {
        CHECK(std::find(steps.begin(), steps.end(), std::string(label)) != steps.end());
    }
    CHECK(std::find(steps.begin(), steps.end(), "bright") == steps.end());
    CHECK(std::find(steps.begin(), steps.end(), "dark") == steps.end());
    CHECK(eyecam::parseProtocol(free) == std::vector<std::string> {"normal"});
}

void testInfo() {
    using namespace contrib;
    Info info;
    info.appVersion = "0.8.2-beta.2";
    info.consent = "2026-10-25 14:02:10 +0900";
    for (int step = 0; step < kStepCount; ++step) info.steps[step].protocol = protocolOf(step);
    info.steps[0] = {kGuidedProtocol, "rec_2026-10-25_14-02-41", 1'745'000'000, kWearFirst, "2026-10-25 14:03:44"};
    info.steps[1] = {kGuidedProtocol, "rec_2026-10-25_14-05-12", 1'744'000'000, kWearButton, "2026-10-25 14:06:30"};
    info.nextWear = kWearDetected;
    info.running = 2;
    info.runningSince = 1'790'000'123.25;
    info.aborted = {"aborted/rec_2026-10-25_14-04-00 stopped"};
    const std::string text = formatInfo(info);
    // No personal data: the comment, the version, times, protocols, folders and sizes only
    CHECK(text.rfind("# frameeyeosc", 0) == 0);
    CHECK(text.find("app_version=0.8.2-beta.2\n") != std::string::npos);
    CHECK(text.find("step1_rec=rec_2026-10-25_14-02-41\n") != std::string::npos);
    CHECK(text.find("step2_wear=button\n") != std::string::npos);
    CHECK(text.find("step4_protocol=free\n") != std::string::npos);
    CHECK(text.find("step3_rec=") == std::string::npos);
    CHECK(text.find("running=3\n") != std::string::npos);
    Info back;
    CHECK(parseInfo(text, back));
    SAME(back.appVersion, info.appVersion);
    SAME(back.consent, info.consent);
    for (int step = 0; step < kStepCount; ++step) {
        SAME(back.steps[step].protocol, protocolOf(step));
        SAME(back.steps[step].rec, info.steps[step].rec);
        CHECK(back.steps[step].bytes == info.steps[step].bytes);
        SAME(back.steps[step].wear, info.steps[step].wear);
        SAME(back.steps[step].done, info.steps[step].done);
    }
    SAME(back.nextWear, kWearDetected);
    CHECK(back.running == 2);
    CHECK(std::fabs(back.runningSince - info.runningSince) < 1e-3);
    CHECK(back.aborted == info.aborted);
    CHECK(back.packed.empty());
    // Packed, nothing running
    info.running = -1;
    info.packed = "contrib_2026-10-25_14-02-10.tar.zst";
    info.packedBytes = 2'100'000'000;
    CHECK(parseInfo(formatInfo(info), back));
    CHECK(back.running == -1);
    SAME(back.packed, info.packed);
    CHECK(back.packedBytes == info.packedBytes);
    // Odd lines are skipped, CRLF read, a recording's name that isn't one dropped (that step is done again), steps
    // past the fifth ignored
    CHECK(parseInfo("consent=x\r\nnonsense\nstep1_rec=../../etc\nstep2_rec=rec_2026-10-25_14-05-12\r\n"
                    "step9_rec=rec_2026-10-25_14-05-12\nrunning=9\nstep2_bytes=12x\n",
                    back));
    CHECK(back.steps[0].rec.empty());
    SAME(back.steps[1].rec, "rec_2026-10-25_14-05-12");
    CHECK(back.steps[1].bytes == 0);
    CHECK(back.running == -1);
    // Without the consent it isn't a contribution's
    CHECK(!parseInfo("app_version=1\n", back));
    CHECK(!parseInfo("", back));
}

void testNames() {
    using namespace contrib;
    CHECK(isFolderName("contrib_2026-10-25_14-02-10"));
    CHECK(!isFolderName("contrib_2026-10-25_14-02-10.tar.zst"));
    CHECK(!isFolderName("contrib_2026-10-25"));
    CHECK(!isFolderName("rec_2026-10-25_14-02-10"));
    CHECK(!isFolderName("contrib_../../x"));
    CHECK(isRecordingName("rec_2026-10-25_14-02-10"));
    CHECK(!isRecordingName("calib_2026-10-25_14-02-10"));
    CHECK(!isRecordingName("rec_fake"));
    CHECK(!isRecordingName("aborted/rec_2026-10-25_14-02-10"));
    const std::string name = folderName(static_cast<std::time_t>(kNow));
    CHECK(isFolderName(name));
    const std::string local = localTime(static_cast<std::time_t>(kNow), true);
    CHECK(local.size() == 25 && local[10] == ' ' && (local[20] == '+' || local[20] == '-'));
    CHECK(localTime(static_cast<std::time_t>(kNow), false).size() == 19);
}

void testRunWatch() {
    using contrib::RunWatch;
    const std::string p = contrib::kGuidedProtocol;
    const std::string dir = "/home/steamos/eyecam/rec_2026-10-25_14-05-12";
    // Searching, recording, then idle with its folder: done
    {
        RunWatch w;
        w.begin(1, p, kNow);
        CHECK(w.active() && w.step() == 1);
        CHECK(w.follow(status(State::Searching, p, kNow + 0.1), true, kNow + 0.1, NAN).outcome == Outcome::Running);
        CHECK(w.follow(status(State::Recording, p, kNow + 30), true, kNow + 30, NAN).outcome == Outcome::Running);
        eyecam::Status s = status(State::Idle, p, kNow + 63);
        s.sessionDir = dir;
        s.message = "保存した: rec_2026-10-25_14-05-12";
        const contrib::RunEnd end = w.follow(s, true, kNow + 63, kNow + 2);
        CHECK(end.outcome == Outcome::Done);
        SAME(end.dir, dir);
    }
    // ...ended early (Stop, or the eyes lost): stopped, with the part's folder
    {
        RunWatch w;
        w.begin(1, p, kNow);
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
        w.begin(1, p, kNow);
        w.follow(status(State::Searching, p, kNow + 1), true, kNow + 1, NAN);
        const contrib::RunEnd end = w.follow(status(State::Idle, p, kNow + 5), true, kNow + 5, NAN);
        CHECK(end.outcome == Outcome::Stopped);
        CHECK(end.dir.empty());
    }
    // ...failed: eyecam-rec's error, with what it saved
    {
        RunWatch w;
        w.begin(1, p, kNow);
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
        w.begin(2, p, kNow);
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
        w.begin(0, p, kNow);
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
        w.begin(0, p, kNow);
        CHECK(w.follow(status(State::Recording, "widen", kNow + 1), true, kNow + 1, NAN).outcome == Outcome::Running);
        CHECK(w.follow(status(State::Calibrating, "calib wear", kNow + 9), true, kNow + 9, NAN).outcome ==
              Outcome::Failed);
    }
    // The panel restarted in the middle: it ended meanwhile, its folder made after the start: done
    {
        RunWatch w;
        w.begin(3, contrib::kFreeProtocol, kNow);
        eyecam::Status s = status(State::Idle, contrib::kFreeProtocol, kNow + 400);
        s.sessionDir = dir;
        CHECK(w.follow(s, true, kNow + 400, kNow + 3).outcome == Outcome::Done);
        // ...but another protocol's folder isn't it
        RunWatch other;
        other.begin(3, contrib::kFreeProtocol, kNow);
        eyecam::Status s2 = status(State::Idle, p, kNow + 400);
        s2.sessionDir = dir;
        CHECK(other.follow(s2, true, kNow + 400, kNow + 3).outcome == Outcome::Failed);
    }
    // ...its folder gone (moved by hand): missing
    {
        RunWatch w;
        w.begin(1, p, kNow);
        w.follow(status(State::Recording, p, kNow + 10), true, kNow + 10, NAN);
        eyecam::Status s = status(State::Idle, p, kNow + 63);
        s.sessionDir = dir;
        const contrib::RunEnd end = w.follow(s, true, kNow + 63, NAN);
        CHECK(end.outcome == Outcome::Failed);
        CHECK(end.why == contrib::Why::Missing);
    }
    // eyecam-rec gone (its status stale) for kGoneSec: failed, not before
    {
        RunWatch w;
        w.begin(1, p, kNow);
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

void testWearWatch() {
    using contrib::WearWatch;
    const auto idle = [](bool locked, double prox = -1.0) {
        eyecam::Status s = status(State::Idle, "", kNow);
        s.locked = locked;
        s.prox = prox;
        return s;
    };
    // The eye video stops for kOffSec, then comes back for kOnSec
    {
        WearWatch w;
        w.start(100);
        double t = 100;
        for (; t < 101; t += 0.1) w.follow(idle(true), true, t);
        for (double end = t + 2.9; t < end; t += 0.1) w.follow(idle(false), true, t);
        CHECK(!w.offSeen());
        for (double end = t + 0.3; t < end; t += 0.1) w.follow(idle(false), true, t);
        CHECK(w.offSeen() && !w.detected());
        for (double end = t + 1.8; t < end; t += 0.1) w.follow(idle(true), true, t);
        CHECK(!w.detected());
        for (double end = t + 0.5; t < end; t += 0.1) w.follow(idle(true), true, t);
        CHECK(w.detected());
    }
    // A short gap in the video (a blink of the tracker) is no re-wear
    {
        WearWatch w;
        w.start(0);
        double t = 0;
        for (; t < 20; t += 0.1) w.follow(idle(t < 5 || t > 6.5), true, t);
        CHECK(!w.offSeen() && !w.detected());
    }
    // The sensor: off after it read worn, then worn again (live processing off: the video doesn't say)
    {
        WearWatch w;
        w.start(0);
        double t = 0;
        const auto noLive = [&](double prox) {
            eyecam::Status s = idle(false, prox);
            s.live = false;
            return s;
        };
        for (; t < 2; t += 0.1) w.follow(noLive(31), true, t);
        for (; t < 6; t += 0.1) w.follow(noLive(9), true, t);
        CHECK(w.offSeen());
        for (; t < 9; t += 0.1) w.follow(noLive(31), true, t);
        CHECK(w.detected());
    }
    // A face the sensor never reads worn on: its low reading alone is no "off"
    {
        WearWatch w;
        w.start(0);
        for (double t = 0; t < 30; t += 0.1) w.follow(idle(true, 12), true, t);
        CHECK(!w.offSeen());
    }
    // The Frame slept (the loop stood still kOffSec or more): off; on again with the video
    {
        WearWatch w;
        w.start(0);
        w.follow(idle(true), true, 1);
        w.follow(idle(true), true, 1 + contrib::kOffSec + 30);
        CHECK(w.offSeen());
        w.follow(idle(true), true, 40);
        w.follow(idle(true), true, 42.5);
        CHECK(w.detected());
    }
    // Back on without being off first: nothing
    {
        WearWatch w;
        w.start(0);
        for (double t = 0; t < 30; t += 0.1) w.follow(idle(true, 30), true, t);
        CHECK(!w.offSeen() && !w.detected());
    }
    // eyecam-rec not fresh: its numbers don't count
    {
        WearWatch w;
        w.start(0);
        for (double t = 0; t < 10; t += 0.1) w.follow(idle(false), false, t);
        CHECK(!w.offSeen());
    }
    // "I put it back on": kFallbackSec after the start, and only while watching
    {
        WearWatch w;
        CHECK(!w.fallbackReady(1000));
        w.start(100);
        CHECK(!w.fallbackReady(100 + contrib::kFallbackSec - 0.5));
        CHECK(w.fallbackReady(100 + contrib::kFallbackSec));
        w.stop();
        CHECK(!w.fallbackReady(1000));
        // Started again: the time again, and what it saw forgotten
        w.start(500);
        CHECK(!w.fallbackReady(505) && !w.offSeen());
    }
}

void testScreen() {
    using namespace contrib;
    View v;
    const eyecam::View ready = eyecamReady(kNow);
    CHECK(screen(v, false) == Screen::Consent);
    CHECK(!startAllowed(v, ready, 0));
    v.dir = "/home/steamos/eyecam/contrib_2026-10-25_14-02-10";
    v.freeBytes = 20'000'000'000LL;
    CHECK(screen(v, false) == Screen::Ready);
    CHECK(startAllowed(v, ready, 0));
    // eyecam-rec busy, not set up, a command waiting, no room: no start
    {
        eyecam::View busy = ready;
        busy.status.state = State::Calibrating;
        CHECK(!startAllowed(v, busy, 0));
        eyecam::View waiting = ready;
        waiting.busy = true;
        CHECK(!startAllowed(v, waiting, 0));
        eyecam::View notSetUp = ready;
        notSetUp.status.setupDone = false;
        notSetUp.status.calibState = 0;
        CHECK(!startAllowed(v, notSetUp, 0));
        eyecam::View error = ready;
        error.status.state = State::Error;
        CHECK(startAllowed(v, error, 0));
        View full = v;
        full.freeBytes = 2'000'000'000LL;
        CHECK(!startAllowed(full, ready, 0));
        View packing = v;
        packing.progress.running = true;
        CHECK(!startAllowed(packing, ready, 0));
    }
    // After the first: the re-wear, then the start
    v.info.steps[0].rec = "rec_2026-10-25_14-02-41";
    CHECK(screen(v, false) == Screen::Rewear);
    v.info.nextWear = kWearDetected;
    CHECK(screen(v, false) == Screen::Ready);
    // Running
    v.running = 1;
    CHECK(screen(v, false) == Screen::Running);
    CHECK(!startAllowed(v, ready, 1));
    v.running = -1;
    // A try that didn't finish: its screen, for its step only
    v.failure.step = 1;
    CHECK(screen(v, false) == Screen::Failed);
    v.failure.step = 0;
    CHECK(screen(v, false) == Screen::Ready);
    // All four: the end; the optional one when asked for (its re-wear first)
    for (int step = 0; step < kRequiredSteps; ++step) v.info.steps[step].rec = "rec_2026-10-25_14-02-41";
    v.info.nextWear.clear();
    CHECK(screen(v, false) == Screen::Done);
    CHECK(screen(v, true) == Screen::Rewear);
    v.info.nextWear = kWearButton;
    CHECK(screen(v, true) == Screen::Ready);
    // Being deleted: that, whatever else
    v.progress.job = Job::Delete;
    v.progress.running = true;
    CHECK(screen(v, false) == Screen::Deleting);
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
    Entry file {"/x/eye_L.raw", "contrib_2026-10-25_14-02-10/rec_2026-10-25_14-02-41/eye_L.raw", 1'000'320'000,
                1'790'000'000, false};
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
    const Entry folder {"/x", "contrib_2026-10-25_14-02-10/", 0, 1'790'000'000, true};
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
 * @return its path
 */
std::string makeRecording(const std::string& home, const std::string& name, size_t bytes, double made = kNow) {
    const std::string dir = home + "/" + name;
    ::mkdir(dir.c_str(), 0755);
    std::string video(bytes, '\0');
    for (size_t i = 0; i < bytes; ++i) video[i] = static_cast<char>((i * 7 + i / 400) & 0xff);
    writeFile(dir + "/eye_L.raw", video);
    writeFile(dir + "/eye_R.raw", video.substr(0, bytes - 13));
    writeFile(dir + "/cues.csv", "t,label\n0.0,lead_in\n");
    writeFile(dir + "/valve.csv", "t,openness_l\n");
    writeFile(dir + "/frames.csv", "eye,t_cam\n");
    writeFile(dir + "/meta.txt", "protocol=widen_nolight\n");
    const timespec times[2] = {{static_cast<time_t>(made), 0}, {static_cast<time_t>(made), 0}};
    ::utimensat(AT_FDCWD, dir.c_str(), times, 0);
    return dir;
}

void testFiles() {
    using namespace contrib;
    const RemoveDir temp {tempDir()};
    const std::string home = temp.path + "/eyecam";
    // None yet; a folder that isn't one is never taken
    ::mkdir(home.c_str(), 0755);
    CHECK(findCurrent(home).empty());
    ::mkdir((home + "/contrib_2026-01-01_00-00-00").c_str(), 0755);
    CHECK(findCurrent(home).empty());  // (no info.txt)
    // Made with the consent
    std::string dir;
    Info info;
    std::string error;
    CHECK(create(home, "0.8.2-test", static_cast<std::time_t>(kNow), dir, info, error));
    SAME(dir, home + "/" + folderName(static_cast<std::time_t>(kNow)));
    SAME(info.appVersion, "0.8.2-test");
    CHECK(!info.consent.empty());
    SAME(findCurrent(home), dir);
    Info read;
    CHECK(readInfo(dir, read, error));
    SAME(read.consent, info.consent);
    SAME(read.steps[3].protocol, kFreeProtocol);
    // A second in the same second: the next second's name; the newest is the current one
    std::string second;
    Info secondInfo;
    CHECK(create(home, "0.8.2-test", static_cast<std::time_t>(kNow), second, secondInfo, error));
    CHECK(second != dir && isFolderName(second.substr(home.size() + 1)));
    SAME(findCurrent(home), second);
    std::filesystem::remove_all(second);
    // A recording moved in, and one that didn't finish into aborted/
    const std::string rec = makeRecording(home, "rec_2026-10-25_14-02-41", 4096);
    std::string name;
    CHECK(takeRecording(home, dir, rec, false, name, error));
    SAME(name, "rec_2026-10-25_14-02-41");
    CHECK(::access((dir + "/" + name + "/eye_L.raw").c_str(), F_OK) == 0);
    CHECK(::access(rec.c_str(), F_OK) != 0);
    const std::string part = makeRecording(home, "rec_2026-10-25_14-04-00", 1000);
    CHECK(takeRecording(home, dir, part, true, name, error));
    SAME(name, "aborted/rec_2026-10-25_14-04-00");
    // Only eyecam-rec's own folders right in home
    const std::string elsewhere = temp.path + "/rec_2026-10-25_14-09-00";
    ::mkdir(elsewhere.c_str(), 0755);
    CHECK(!takeRecording(home, dir, elsewhere, false, name, error));
    ::mkdir((home + "/calib_2026-10-25_14-09-00").c_str(), 0755);
    CHECK(!takeRecording(home, dir, home + "/calib_2026-10-25_14-09-00", false, name, error));
    CHECK(!takeRecording(home, dir, home + "/rec_2026-10-25_14-59-59", false, name, error));  // (not there)
    // Sizes: the files in it and its folders
    CHECK(folderBytes(dir + "/rec_2026-10-25_14-02-41") ==
          4096 + 4096 - 13 + 20 + 13 + 10 + 23);
    CHECK(folderBytes(dir) > folderBytes(dir + "/rec_2026-10-25_14-02-41"));
    CHECK(freeBytes(home) > 0);
    CHECK(freeBytes(temp.path + "/missing") == -1);
    // info.txt written over (no temporary left)
    info.steps[0].rec = "rec_2026-10-25_14-02-41";
    CHECK(writeInfo(dir, info, error));
    CHECK(readInfo(dir, read, error));
    SAME(read.steps[0].rec, "rec_2026-10-25_14-02-41");
    CHECK(::access((dir + "/info.txt.tmp").c_str(), F_OK) != 0);
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
    ::mkdir(home.c_str(), 0755);
    std::string dir;
    Info info;
    std::string error;
    CHECK(create(home, "0.8.2-test", static_cast<std::time_t>(kNow), dir, info, error));
    const std::string names[2] = {"rec_2026-10-25_14-02-41", "rec_2026-10-25_14-05-12"};
    std::string name;
    takeRecording(home, dir, makeRecording(home, names[0], 300'000), false, name, error);
    takeRecording(home, dir, makeRecording(home, names[1], 1'500'000), false, name, error);
    takeRecording(home, dir, makeRecording(home, "rec_2026-10-25_14-04-00", 1000), true, name, error);
    info.steps[0].rec = names[0];
    info.steps[1].rec = names[1];
    CHECK(writeInfo(dir, info, error));
    // What goes in: the folder, info.txt, the finished recordings (sorted), not aborted/
    std::vector<Entry> entries;
    CHECK(packEntries(dir, info, entries, error));
    CHECK(entries.size() == 2 + 2 * 7);
    const std::string top = dir.substr(home.size() + 1);
    SAME(entries[0].name, top + "/");
    SAME(entries[1].name, top + "/info.txt");
    SAME(entries[2].name, top + "/" + names[0] + "/");
    SAME(entries[3].name, top + "/" + names[0] + "/cues.csv");
    for (const Entry& entry : entries) CHECK(entry.name.find("aborted") == std::string::npos);
    // A finished one gone missing: nothing is packed
    Info missing = info;
    missing.steps[2].rec = "rec_2026-10-25_15-00-00";
    CHECK(!packEntries(dir, missing, entries, error));
    // Packed as tar: unpacks to the same files
    Worker worker;
    CHECK(worker.pack(dir, info, false));
    Progress ended = waitFor(worker);
    CHECK(ended.finished && ended.ok);
    SAME(ended.output, top + ".tar");
    CHECK(ended.done == ended.total && ended.total > 0);
    const std::string tar = dir + "/" + top + ".tar";
    CHECK(::access(tar.c_str(), F_OK) == 0);
    CHECK(::access((tar + ".part").c_str(), F_OK) != 0);
    CHECK(ended.outputBytes == static_cast<long long>(std::filesystem::file_size(tar)));
    const std::string out = temp.path + "/out";
    ::mkdir(out.c_str(), 0755);
    CHECK(run("tar -xf '" + tar + "' -C '" + out + "'"));
    for (const std::string& rec : names) {
        for (const char* file : {"eye_L.raw", "eye_R.raw", "cues.csv", "valve.csv", "frames.csv", "meta.txt"}) {
            const std::string a = readFile(dir + "/" + rec + "/" + file);
            const std::string b = readFile(out + "/" + top + "/" + rec + "/" + file);
            CHECK(!a.empty() && a == b);
        }
    }
    SAME(readFile(out + "/" + top + "/info.txt"), readFile(dir + "/info.txt"));
    CHECK(::access((out + "/" + top + "/aborted").c_str(), F_OK) != 0);
    // ...and through zstd, when it is there
    if (zstdAvailable()) {
        CHECK(worker.pack(dir, info, true));
        ended = waitFor(worker);
        CHECK(ended.finished && ended.ok);
        SAME(ended.output, top + ".tar.zst");
        const std::string zst = dir + "/" + top + ".tar.zst";
        const std::string out2 = temp.path + "/out2";
        ::mkdir(out2.c_str(), 0755);
        CHECK(run("zstd -dc '" + zst + "' | tar -xf - -C '" + out2 + "'"));
        SAME(readFile(out2 + "/" + top + "/" + names[1] + "/eye_L.raw"),
             readFile(dir + "/" + names[1] + "/eye_L.raw"));
        // (the earlier pack and aborted/ aren't in it)
        CHECK(::access((out2 + "/" + top + "/" + top + ".tar").c_str(), F_OK) != 0);
    } else {
        std::printf("contrib-test: no zstd here, its pack not tried\n");
    }
    // Deleting: only a contribution's folder right in home
    CHECK(!worker.remove(home, dir + "/" + names[0]));
    CHECK(!worker.remove(temp.path, dir));
    CHECK(!worker.remove(home, home + "/contrib_2026-10-25_99-99-99"));
    CHECK(worker.remove(home, dir));
    ended = waitFor(worker);
    CHECK(ended.finished && ended.ok && ended.job == Job::Delete);
    CHECK(::access(dir.c_str(), F_OK) != 0);
    CHECK(::access(home.c_str(), F_OK) == 0);
}

void testSession() {
    using namespace contrib;
    const RemoveDir temp {tempDir()};
    const std::string home = temp.path + "/eyecam";
    ::mkdir(home.c_str(), 0755);
    View view;
    Session session;
    std::string log;
    session.load(view, home);
    CHECK(view.loaded && view.dir.empty() && view.freeBytes > 0);
    CHECK(screen(view, false) == Screen::Consent);
    const double t0 = static_cast<double>(std::time(nullptr));
    double now = t0;
    double boot = 1000;
    eyecam::View eyecam = eyecamReady(now);
    // No start before the consent
    CHECK(session.startCommand(view, eyecam, 0).empty());
    CHECK(session.consent(view, "0.8.2-test", static_cast<std::time_t>(now)));
    CHECK(!view.dir.empty());
    CHECK(!session.consent(view, "0.8.2-test", static_cast<std::time_t>(now)));
    // Step 1: no re-wear before it
    SAME(session.startCommand(view, eyecam, 0), "start widen_nolight");
    CHECK(session.startCommand(view, eyecam, 1).empty());
    session.started(view, 0, "start widen_nolight", now);
    CHECK(view.running == 0 && screen(view, false) == Screen::Running);
    // (the start went out: nothing more until its reply)
    CHECK(session.startCommand(view, eyecam, 0).empty());
    eyecam::Reply ok;
    ok.ok = true;
    ok.command = "start widen_nolight";
    CHECK(!session.replied(view, ok));
    // Recording, then idle with its folder: moved into the contribution
    now += 1;
    eyecam.status = status(State::Recording, kGuidedProtocol, now);
    session.follow(view, eyecam, now, boot, log);
    CHECK(view.running == 0);
    const std::string rec1 = makeRecording(home, "rec_2026-10-25_14-02-41", 2000, now);
    now += 61;
    eyecam.status = status(State::Idle, kGuidedProtocol, now);
    eyecam.status.sessionDir = rec1;
    CHECK(session.follow(view, eyecam, now, boot, log));
    CHECK(view.running == -1);
    SAME(view.info.steps[0].rec, "rec_2026-10-25_14-02-41");
    SAME(view.info.steps[0].wear, kWearFirst);
    CHECK(view.info.steps[0].bytes > 4000);
    CHECK(::access((view.dir + "/rec_2026-10-25_14-02-41/cues.csv").c_str(), F_OK) == 0);
    CHECK(log.find("step 1 recorded") != std::string::npos);
    // Step 2 waits for the re-wear: the video stops, then comes back
    CHECK(screen(view, false) == Screen::Rewear);
    CHECK(session.startCommand(view, eyecam, 1).empty());
    for (int i = 0; i < 40; ++i) {
        now += 0.1;
        boot += 0.1;
        eyecam.status = status(State::Idle, kGuidedProtocol, now);
        eyecam.status.locked = false;
        session.follow(view, eyecam, now, boot, log);
    }
    CHECK(view.offSeen && view.info.nextWear.empty());
    for (int i = 0; i < 25; ++i) {
        now += 0.1;
        boot += 0.1;
        eyecam.status = status(State::Idle, kGuidedProtocol, now);
        session.follow(view, eyecam, now, boot, log);
    }
    SAME(view.info.nextWear, kWearDetected);
    CHECK(screen(view, false) == Screen::Ready);
    // Step 2 stopped part way: its part in aborted/, the step again (its re-wear kept)
    eyecam = eyecamReady(now);
    SAME(session.startCommand(view, eyecam, 1), "start widen_nolight");
    session.started(view, 1, "start widen_nolight", now);
    session.replied(view, ok);
    now += 1;
    eyecam.status = status(State::Recording, kGuidedProtocol, now);
    session.follow(view, eyecam, now, boot, log);
    const std::string part = makeRecording(home, "rec_2026-10-25_14-05-12", 500, now);
    now += 20;
    eyecam.status = status(State::Idle, kGuidedProtocol, now);
    eyecam.status.sessionDir = part;
    eyecam.status.lastSessionAborted = true;
    CHECK(session.follow(view, eyecam, now, boot, log));
    CHECK(view.failure.step == 1 && view.failure.why == Why::Stopped);
    SAME(view.failure.kept, "aborted/rec_2026-10-25_14-05-12");
    CHECK(view.info.steps[1].rec.empty());
    CHECK(view.info.aborted.size() == 1);
    CHECK(screen(view, false) == Screen::Failed);
    SAME(view.info.nextWear, kWearDetected);
    // Refused at once (a reply with an error): failed with its reason
    eyecam = eyecamReady(now);
    session.started(view, 1, "start widen_nolight", now);
    eyecam::Reply refused;
    refused.command = "start widen_nolight";
    refused.error = "プロトコルが見つからない: protocol_widen_nolight.txt";
    CHECK(session.replied(view, refused));
    CHECK(view.running == -1 && view.failure.why == Why::Refused);
    SAME(view.failure.message, refused.error);
    // The panel restarts while step 2 records: followed on after the load, done
    session.started(view, 1, "start widen_nolight", now);
    session.replied(view, ok);
    {
        View again;
        Session restarted;
        restarted.load(again, home);
        SAME(again.dir, view.dir);
        CHECK(again.running == 1);
        const std::string rec2 = makeRecording(home, "rec_2026-10-25_14-08-03", 800, now);
        now += 70;
        eyecam.status = status(State::Idle, kGuidedProtocol, now);
        eyecam.status.sessionDir = rec2;
        CHECK(restarted.follow(again, eyecam, now, boot, log));
        SAME(again.info.steps[1].rec, "rec_2026-10-25_14-08-03");
        SAME(again.info.steps[1].wear, kWearDetected);
        CHECK(again.info.nextWear.empty());
        // "I put it back on" for step 3 (only once a step waits for one)
        CHECK(restarted.rewore(again));
        CHECK(!restarted.rewore(again));
        Info onDisk;
        std::string error;
        CHECK(readInfo(again.dir, onDisk, error));
        SAME(onDisk.nextWear, kWearButton);
        CHECK(onDisk.running == -1);
        view = again;
        session.load(view, home);
    }
    // Steps 3 and 4 (the free use) done: the end, and its pack
    for (int step = 2; step < kRequiredSteps; ++step) {
        if (view.info.nextWear.empty()) CHECK(session.rewore(view));
        eyecam = eyecamReady(now);
        SAME(session.startCommand(view, eyecam, step), std::string("start ") + protocolOf(step));
        session.started(view, step, std::string("start ") + protocolOf(step), now);
        // (followed only once its start is answered)
        eyecam::Reply started = ok;
        started.command = std::string("start ") + protocolOf(step);
        CHECK(!session.replied(view, started));
        now += 1;
        eyecam.status = status(State::Recording, protocolOf(step), now);
        session.follow(view, eyecam, now, boot, log);
        const std::string rec =
            makeRecording(home, step == 2 ? "rec_2026-10-25_14-11-40" : "rec_2026-10-25_14-15-00", 600, now);
        now += 60;
        eyecam.status = status(State::Idle, protocolOf(step), now);
        eyecam.status.sessionDir = rec;
        CHECK(session.follow(view, eyecam, now, boot, log));
        CHECK(!view.info.steps[step].rec.empty());
    }
    CHECK(requiredDone(view.info) == kRequiredSteps);
    CHECK(screen(view, false) == Screen::Done);
    SAME(view.info.steps[3].protocol, kFreeProtocol);
    view.compress = false;
    CHECK(session.pack(view));
    for (int i = 0; i < 3000 && view.info.packed.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        session.follow(view, eyecam, now, boot, log);
    }
    SAME(view.info.packed, view.dir.substr(home.size() + 1) + ".tar");
    CHECK(view.info.packedBytes > 0);
    CHECK(view.totalBytes > view.info.packedBytes);
    // Deleted: the first screen again, saying so
    CHECK(session.remove(view));
    for (int i = 0; i < 3000 && !view.dir.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        session.follow(view, eyecam, now, boot, log);
    }
    CHECK(view.dir.empty() && view.deleted);
    CHECK(screen(view, false) == Screen::Consent);
    CHECK(findCurrent(home).empty());
}

}  // namespace

int main() {
    testSteps();
    testProtocolFiles();
    testInfo();
    testNames();
    testRunWatch();
    testWearWatch();
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
