// Tests for the eye capture tab's logic (eyecam.cpp): reading eyecam-rec's status.json (every state, missing and odd
// fields), when the tab shows (stale, stopped, missing), the step texts in both languages, when the full-view light
// shows and how it fades (and when it goes at once), the light warning before a start and the commands it sends, and
// the control socket against a stand-in recorder in a temporary folder (never the real one). Built with the panel as
// eyecam-test; exits non-zero on failure.
#include "eyecam.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

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

using eyecam::Fill;
using eyecam::State;
using eyecam::Status;

/** Unix time the tests pretend it is. */
constexpr double kNow = 1'790'000'000.0;

/**
 * A status.json the way eyecam-rec writes it while recording.
 * @param state the state
 * @param label the step label
 * @return the JSON text
 */
std::string fullStatus(const std::string& state, const std::string& label) {
    return "{\"state\": \"" + state + "\", \"message\": \"右目が暗いです\", \"locked\": true, \"fps_l\": 30.2, "
           "\"fps_r\": 29.5, \"step_index\": 6, \"step_count\": 9, \"step_label\": \"" + label + "\", "
           "\"step_remaining_s\": 3.25, \"elapsed_s\": 81.5, \"total_s\": 120, "
           "\"session_dir\": \"/home/steamos/eyecam/s1\", \"protocol\": \"default\"}";
}

void testParse() {
    // Every state the recorder writes
    const struct {
        const char* text;
        State state;
    } kStates[] = {{"waiting_fds", State::WaitingFds}, {"idle", State::Idle},   {"searching", State::Searching},
                   {"recording", State::Recording},    {"error", State::Error}, {"stopped", State::Stopped},
                   {"paused", State::Unknown}};
    for (const auto& item : kStates) {
        const Status s = eyecam::parseStatus(fullStatus(item.text, "normal"), kNow);
        CHECK(s.present);
        CHECK(s.state == item.state);
        SAME(s.stateText, item.text);
    }

    // All fields
    const Status s = eyecam::parseStatus(fullStatus("recording", "bright"), kNow - 0.2);
    CHECK(s.present);
    CHECK(s.readError.empty());
    CHECK(s.mtime == kNow - 0.2);
    SAME(s.message, "右目が暗いです");
    CHECK(s.locked);
    CHECK(std::fabs(s.fpsL - 30.2) < 1e-9 && std::fabs(s.fpsR - 29.5) < 1e-9);
    CHECK(s.stepIndex == 6 && s.stepCount == 9);
    SAME(s.stepLabel, "bright");
    CHECK(std::fabs(s.stepRemainingS - 3.25) < 1e-9);
    CHECK(std::fabs(s.elapsedS - 81.5) < 1e-9 && std::fabs(s.totalS - 120) < 1e-9);
    SAME(s.sessionDir, "/home/steamos/eyecam/s1");
    SAME(s.protocol, "default");

    // Only a state: the rest missing (NaN numbers, no step)
    {
        const Status bare = eyecam::parseStatus("{\"state\": \"idle\"}", kNow);
        CHECK(bare.present && bare.state == State::Idle);
        CHECK(bare.message.empty() && !bare.locked);
        CHECK(std::isnan(bare.fpsL) && std::isnan(bare.fpsR));
        CHECK(bare.stepIndex == -1 && bare.stepCount == -1 && bare.stepLabel.empty());
        CHECK(std::isnan(bare.stepRemainingS) && std::isnan(bare.elapsedS) && std::isnan(bare.totalS));
    }
    // Odd types are taken as missing; nulls too; extra fields are ignored
    {
        const Status odd = eyecam::parseStatus(
            "{\"state\": \"recording\", \"message\": 5, \"locked\": \"yes\", \"fps_l\": \"30\", \"fps_r\": null, "
            "\"step_index\": 2.6, \"step_count\": 1e300, \"step_label\": [\"widen\"], \"elapsed_s\": true, "
            "\"extra\": {\"a\": 1}}",
            kNow);
        CHECK(odd.present && odd.state == State::Recording);
        CHECK(odd.message.empty() && !odd.locked);
        CHECK(std::isnan(odd.fpsL) && std::isnan(odd.fpsR));
        CHECK(odd.stepIndex == 3);  // rounded
        CHECK(odd.stepCount == -1);  // out of range
        CHECK(odd.stepLabel.empty());
        CHECK(std::isnan(odd.elapsedS));
    }
    // No state, or a state that isn't text: still the recorder's file, in a state the panel doesn't know
    CHECK(eyecam::parseStatus("{}", kNow).state == State::Unknown);
    CHECK(eyecam::parseStatus("{\"state\": 3}", kNow).state == State::Unknown);
    // Not JSON, or not an object: not present
    for (const char* bad : {"", "{", "[1, 2]", "\"idle\"", "{\"state\": \"idle\""}) {
        const Status broken = eyecam::parseStatus(bad, kNow);
        CHECK(!broken.present);
        CHECK(broken.state == State::Missing);
        CHECK(!broken.readError.empty());
    }
}

void testVisible() {
    const Status idle = eyecam::parseStatus("{\"state\": \"idle\"}", kNow);
    CHECK(eyecam::tabVisible(idle, kNow));
    CHECK(eyecam::tabVisible(idle, kNow + 4.9));
    CHECK(!eyecam::tabVisible(idle, kNow + 5.1));
    // A clock that went back a lot counts as stale too, a little as fresh
    CHECK(eyecam::tabVisible(idle, kNow - 1.0));
    CHECK(!eyecam::tabVisible(idle, kNow - 60.0));
    // Every running state shows it; "stopped" and a missing or broken file don't
    for (const char* state : {"waiting_fds", "searching", "recording", "error", "something_new"}) {
        CHECK(eyecam::tabVisible(eyecam::parseStatus(std::string("{\"state\": \"") + state + "\"}", kNow), kNow));
    }
    CHECK(!eyecam::tabVisible(eyecam::parseStatus("{\"state\": \"stopped\"}", kNow), kNow));
    CHECK(!eyecam::tabVisible(eyecam::parseStatus("not json", kNow), kNow));
    CHECK(!eyecam::tabVisible(Status(), kNow));
}

void testText() {
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    const struct {
        const char* label;
        const char* ja;
        const char* en;
    } kSteps[] = {
        {"normal", "普通に開けて", "Open normally"},       {"widen", "見開いて！", "Open wide!"},
        {"close", "目を閉じて", "Close your eyes"},         {"squint", "目を細めて", "Squint"},
        {"look_up", "上を見て", "Look up"},                 {"look_down", "下を見て", "Look down"},
        {"bright", "明るい画面を見て", "Look at the bright screen"},
        {"dark", "暗い画面を見て", "Look at the dark screen"}, {"end", "おわり", "Done"}, {"lead_in", "もうすぐ始まるよ", "Get ready"},
    };
    for (const auto& step : kSteps) {
        CHECK(eyecam::parseStep(step.label) != eyecam::Step::Unknown);
        SAME(eyecam::instruction(ja, step.label), step.ja);
        SAME(eyecam::instruction(en, step.label), step.en);
    }
    // An unknown label is shown as written
    CHECK(eyecam::parseStep("blink") == eyecam::Step::Unknown);
    SAME(eyecam::instruction(ja, "blink"), "blink");
    SAME(eyecam::instruction(en, ""), "");
}

void testFill() {
    const auto at = [](const std::string& state, const std::string& label, double age) {
        return eyecam::fillFor(eyecam::parseStatus(fullStatus(state, label), kNow - age), kNow);
    };
    CHECK(at("recording", "bright", 0.1) == Fill::Bright);
    CHECK(at("recording", "dark", 0.1) == Fill::Dark);
    CHECK(at("recording", "dark", 0.99) == Fill::Dark);
    // The other steps
    for (const char* label : {"normal", "widen", "close", "squint", "look_up", "look_down", "end", "", "white"}) {
        CHECK(at("recording", label, 0.1) == Fill::None);
    }
    // Not recording (a label left over from the last run)
    for (const char* state : {"idle", "searching", "error", "stopped", "waiting_fds"}) {
        CHECK(at(state, "bright", 0.1) == Fill::None);
        CHECK(at(state, "dark", 0.1) == Fill::None);
    }
    // Stale for more than a second: off, long before the tab goes
    CHECK(at("recording", "bright", 1.1) == Fill::None);
    CHECK(at("recording", "dark", 3.0) == Fill::None);
    CHECK(at("recording", "bright", 60.0) == Fill::None);
    // A file that isn't there or can't be read
    CHECK(eyecam::fillFor(Status(), kNow) == Fill::None);
    CHECK(eyecam::fillFor(eyecam::parseStatus("{\"state\": \"recording\", \"step_label\": \"bright\"", kNow), kNow) ==
          Fill::None);
}

void testConfirm() {
    using eyecam::StartChoice;
    using eyecam::StartConfirm;
    // The commands each button sends
    SAME(eyecam::startCommand(StartChoice::WithLight), "start");
    SAME(eyecam::startCommand(StartChoice::WithoutLight), "start widen_nolight");
    SAME(eyecam::startCommand(StartChoice::Cancel), "");

    // idle -> the warning -> each button
    const struct {
        StartChoice choice;
        const char* command;
    } kChoices[] = {{StartChoice::WithLight, "start"},
                    {StartChoice::WithoutLight, "start widen_nolight"},
                    {StartChoice::Cancel, ""}};
    for (const auto& item : kChoices) {
        StartConfirm confirm;
        CHECK(!confirm.isOpen());
        CHECK(confirm.open(State::Idle));
        CHECK(confirm.isOpen());
        CHECK(!confirm.sync(State::Idle, true));  // stays while idle and shown
        CHECK(confirm.isOpen());
        SAME(confirm.choose(item.choice), item.command);
        CHECK(!confirm.isOpen());  // every button closes it: back to idle's view
    }
    // The retry in error goes through it too
    {
        StartConfirm confirm;
        CHECK(confirm.open(State::Error));
        CHECK(!confirm.sync(State::Error, true));
        SAME(confirm.choose(StartChoice::WithoutLight), "start widen_nolight");
    }
    // Only idle and error open it
    for (const State state : {State::Missing, State::WaitingFds, State::Searching, State::Recording, State::Stopped,
                              State::Unknown}) {
        StartConfirm confirm;
        CHECK(!confirm.open(state));
        CHECK(!confirm.isOpen());
    }
    // It closes by itself once the state leaves the one it was opened in (another start from elsewhere, the
    // recorder failing, ...), even if it comes back
    for (const State state : {State::Searching, State::Recording, State::Error, State::WaitingFds, State::Stopped,
                              State::Missing}) {
        StartConfirm confirm;
        confirm.open(State::Idle);
        CHECK(confirm.sync(state, true));
        CHECK(!confirm.isOpen());
        CHECK(!confirm.sync(State::Idle, true));  // closed already: nothing more
        CHECK(!confirm.isOpen());
    }
    {
        StartConfirm confirm;
        confirm.open(State::Error);
        CHECK(confirm.sync(State::Idle, true));  // error -> idle is a change too
        CHECK(!confirm.isOpen());
    }
    // ...and when the tab hides (eyecam-rec's tab gone, another tab chosen, the dashboard closed)
    {
        StartConfirm confirm;
        confirm.open(State::Idle);
        CHECK(confirm.sync(State::Idle, false));
        CHECK(!confirm.isOpen());
        confirm.open(State::Idle);
        confirm.close();
        CHECK(!confirm.isOpen());
    }
}

void testNoLight() {
    CHECK(!eyecam::withoutLight(eyecam::parseStatus(fullStatus("recording", "widen"), kNow)));  // "default"
    const std::string noLight = "{\"state\": \"recording\", \"step_label\": \"widen\", \"protocol\": \"widen_nolight\"}";
    CHECK(eyecam::withoutLight(eyecam::parseStatus(noLight, kNow)));
    CHECK(!eyecam::withoutLight(eyecam::parseStatus("{\"state\": \"recording\", \"protocol\": \"widen\"}", kNow)));
    CHECK(!eyecam::withoutLight(eyecam::parseStatus("{\"state\": \"recording\"}", kNow)));
    // The note redraws the panel when the protocol changes
    eyecam::View a;
    eyecam::View b;
    a.status = eyecam::parseStatus(fullStatus("recording", "widen"), kNow);
    b.status = a.status;
    b.status.protocol = eyecam::kNoLightProtocol;
    CHECK(eyecam::withoutLight(b.status));
    CHECK(eyecam::signature(a) != eyecam::signature(b));
}

/**
 * Step a light along at a fixed rate.
 * @param light the light (changed)
 * @param wanted what fillFor says
 * @param from monotonic seconds to start at (light.at should be there)
 * @param seconds how long
 * @param dt the loop's period
 * @return the time it got to
 */
double run(eyecam::Light& light, Fill wanted, double from, double seconds, double dt) {
    double t = from;
    const int steps = static_cast<int>(std::lround(seconds / dt));
    for (int i = 0; i < steps; ++i) {
        t += dt;
        light = eyecam::stepLight(light, wanted, false, t);
    }
    return t;
}

void testFade() {
    using eyecam::Light;
    const double dt = 1.0 / 90;
    CHECK(std::fabs(eyecam::kFadeInSec - 0.7) < 1e-9);
    CHECK(std::fabs(eyecam::kFadeOutSec - 0.5) < 1e-9);

    // Nothing wanted, nothing shown
    Light light;
    light = eyecam::stepLight(light, Fill::None, false, 100.0);
    CHECK(light.fill == Fill::None && light.alpha == 0.0 && light.at == 100.0);
    CHECK(!eyecam::lightFading(light, Fill::None));

    // Bright wanted: up at once, but clear; then 0 -> 1 over 0.7 s, never jumping
    light = eyecam::stepLight(light, Fill::Bright, false, 100.0);
    CHECK(light.fill == Fill::Bright && light.alpha == 0.0);
    CHECK(eyecam::lightFading(light, Fill::Bright));
    double t = 100.0;
    double last = 0.0;
    double biggestStep = 0.0;
    while (t < 100.0 + 0.35 - 1e-9) {
        t += dt;
        light = eyecam::stepLight(light, Fill::Bright, false, t);
        biggestStep = std::max(biggestStep, light.alpha - last);
        CHECK(light.alpha >= last);
        last = light.alpha;
    }
    CHECK(std::fabs(light.alpha - 0.5) < 0.02);  // halfway at 0.35 s
    CHECK(biggestStep < 0.02);
    t = run(light, Fill::Bright, t, 0.3, dt);
    CHECK(light.alpha > 0.9 && light.alpha < 1.0);  // not yet at 0.65 s
    t = run(light, Fill::Bright, t, 0.06, dt);
    CHECK(light.alpha == 1.0);  // full at 0.7 s, and it stays there
    CHECK(!eyecam::lightFading(light, Fill::Bright));
    t = run(light, Fill::Bright, t, 5.0, dt);
    CHECK(light.fill == Fill::Bright && light.alpha == 1.0);

    // The same however often it is called (up to kMaxFadeInStepSec apart)
    for (const double period : {0.035, 0.07}) {
        Light slow = eyecam::stepLight(Light(), Fill::Dark, false, 0.0);
        run(slow, Fill::Dark, 0.0, 0.35, period);
        CHECK(std::fabs(slow.alpha - 0.5) < 0.01);
    }
    // A stalled loop (2 s between two calls) never makes it jump to full
    {
        Light stalled = eyecam::stepLight(Light(), Fill::Bright, false, 0.0);
        stalled = eyecam::stepLight(stalled, Fill::Bright, false, 2.0);
        CHECK(stalled.alpha <= eyecam::kMaxFadeInStepSec / eyecam::kFadeInSec + 1e-9);
        // nor a clock going backwards
        const Light back = eyecam::stepLight(stalled, Fill::Bright, false, 1.0);
        CHECK(back.alpha == stalled.alpha);
    }

    // The step ends while recording goes on: 1 -> 0 over 0.5 s, then gone
    {
        Light out = light;
        double u = run(out, Fill::None, t, 0.25, dt);
        CHECK(out.fill == Fill::Bright && std::fabs(out.alpha - 0.5) < 0.03);
        CHECK(eyecam::lightFading(out, Fill::None));
        u = run(out, Fill::None, u, 0.2, dt);
        CHECK(out.fill == Fill::Bright && out.alpha > 0.0 && out.alpha < 0.15);
        run(out, Fill::None, u, 0.07, dt);
        CHECK(out.fill == Fill::None && out.alpha == 0.0);
        CHECK(!eyecam::lightFading(out, Fill::None));
    }
    // Bright straight to dark: never white to black at once; bright fades out, then dark fades in from clear
    {
        Light swap = light;
        double u = t;
        bool sawDark = false;
        double darkFrom = 0.0;
        double brightGone = 0.0;
        for (int i = 0; i < 200; ++i) {
            u += dt;
            const Light before = swap;
            swap = eyecam::stepLight(swap, Fill::Dark, false, u);
            if (swap.fill == Fill::Dark && !sawDark) {
                sawDark = true;
                darkFrom = swap.alpha;
                brightGone = u - t;
                CHECK(before.fill == Fill::Bright && before.alpha < 0.05);  // bright was nearly clear
            }
        }
        CHECK(sawDark);
        CHECK(darkFrom == 0.0);
        CHECK(brightGone > 0.45 && brightGone < 0.55);
        CHECK(swap.fill == Fill::Dark && swap.alpha == 1.0);  // and dark is full 0.7 s later
        // Dark back to bright mid fade-in: dark fades out from where it got to
        Light back = eyecam::stepLight(Light(), Fill::Dark, false, 0.0);
        double v = run(back, Fill::Dark, 0.0, 0.35, dt);
        const double reached = back.alpha;
        v = run(back, Fill::Bright, v, 0.1, dt);
        CHECK(back.fill == Fill::Dark && back.alpha < reached);
        // (0.31 left of 0.5 s: out in about 0.15 s, then bright from clear for the rest)
        run(back, Fill::Bright, v, 0.3, dt);
        CHECK(back.fill == Fill::Bright);
        CHECK(back.alpha > 0.0 && back.alpha < 0.25);
    }
    // Hidden at once, from full or mid-fade, whatever is wanted
    for (const Fill wanted : {Fill::None, Fill::Bright, Fill::Dark}) {
        const Light gone = eyecam::stepLight(light, wanted, true, t + dt);
        CHECK(gone.fill == Fill::None && gone.alpha == 0.0);
    }
}

void testHideAtOnce() {
    const auto view = [](const std::string& state, const std::string& label, double age) {
        eyecam::View v;
        v.status = eyecam::parseStatus(fullStatus(state, label), kNow - age);
        v.visible = eyecam::tabVisible(v.status, kNow);
        return v;
    };
    // Recording, fresh: a step change fades (bright, dark, and the steps after them)
    for (const char* label : {"bright", "dark", "normal", "end", "lead_in"}) {
        CHECK(!eyecam::hideLightAtOnce(view("recording", label, 0.1), kNow));
    }
    // The status file more than a second old, or long gone stale (the tab gone)
    CHECK(!eyecam::hideLightAtOnce(view("recording", "bright", 0.99), kNow));
    CHECK(eyecam::hideLightAtOnce(view("recording", "bright", 1.1), kNow));
    CHECK(eyecam::hideLightAtOnce(view("recording", "dark", 10.0), kNow));
    // Recording no longer running: stopped, failed, back to idle, the recorder gone
    for (const char* state : {"idle", "error", "stopped", "searching", "waiting_fds", "something_new"}) {
        CHECK(eyecam::hideLightAtOnce(view(state, "bright", 0.1), kNow));
    }
    CHECK(eyecam::hideLightAtOnce(eyecam::View(), kNow));
    // "Stop" pressed: gone before the recorder even answers
    {
        eyecam::View v = view("recording", "bright", 0.1);
        v.busy = true;
        v.busyCommand = "stop";
        CHECK(eyecam::hideLightAtOnce(v, kNow));
        v.busyCommand = "start";
        CHECK(!eyecam::hideLightAtOnce(v, kNow));
    }
    // So, along a run: dark fading in, then the status goes stale -> off in one step
    eyecam::Light light = eyecam::stepLight(eyecam::Light(), Fill::Dark, false, 0.0);
    run(light, Fill::Dark, 0.0, 0.3, 0.01);
    CHECK(light.fill == Fill::Dark && light.alpha > 0.3);
    const eyecam::View stale = view("recording", "dark", 1.5);
    light = eyecam::stepLight(light, eyecam::fillFor(stale.status, kNow), eyecam::hideLightAtOnce(stale, kNow), 0.31);
    CHECK(light.fill == Fill::None && light.alpha == 0.0);
}

void testReply() {
    eyecam::Reply r = eyecam::parseReply("ok\n", "start");
    CHECK(r.ok && r.error.empty());
    SAME(r.command, "start");
    CHECK(eyecam::parseReply("ok", "stop").ok);
    CHECK(eyecam::parseReply("ok\r\n", "stop").ok);
    r = eyecam::parseReply("err not idle\n", "start");
    CHECK(!r.ok);
    SAME(r.error, "not idle");
    r = eyecam::parseReply("err", "start");
    CHECK(!r.ok);
    SAME(r.error, "(no reason)");
    r = eyecam::parseReply("", "start");
    CHECK(!r.ok);
    SAME(r.error, "empty reply");
    r = eyecam::parseReply("okay", "start");
    CHECK(!r.ok);
    SAME(r.error, "unexpected reply: okay");
    CHECK(!eyecam::parseReply("error x", "start").ok);
}

/**
 * Make a folder for the test's own files.
 * @return its path
 */
std::string tempDir() {
    char pattern[] = "/tmp/eyecam-test-XXXXXX";
    const char* dir = ::mkdtemp(pattern);
    return dir != nullptr ? dir : "/tmp";
}

void testReadFile() {
    const std::string dir = tempDir();
    CHECK(!eyecam::readStatus(dir).present);
    SAME(eyecam::readStatus(dir).readError, "no status file");
    const std::string path = dir + "/status.json";
    {
        std::ofstream file(path);
        file << fullStatus("recording", "dark");
    }
    // Its time is the file's: written just now, and then made 2 s and 10 s old
    const double now = static_cast<double>(std::time(nullptr));
    Status s = eyecam::readStatus(dir);
    CHECK(s.present && s.state == State::Recording);
    CHECK(eyecam::tabVisible(s, now) && eyecam::fillFor(s, now) == Fill::Dark);
    for (const double old : {2.0, 10.0}) {
        timeval times[2] = {{static_cast<time_t>(now - old), 0}, {static_cast<time_t>(now - old), 0}};
        CHECK(::utimes(path.c_str(), times) == 0);
        s = eyecam::readStatus(dir);
        CHECK(std::fabs(eyecam::age(s, now) - old) < 0.01);
        CHECK(eyecam::fillFor(s, now) == Fill::None);
        CHECK(eyecam::tabVisible(s, now) == (old < eyecam::kVisibleSec));
    }
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

/** A stand-in recorder: listens on a socket and answers each connection once. */
class FakeRecorder {
public:
    /**
     * @param path the socket
     * @param reply what to answer ("" = never answer, keep the connection open until stopped)
     */
    FakeRecorder(const std::string& path, const std::string& reply) : path_(path), reply_(reply) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        ::unlink(path.c_str());
        ok_ = ::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 && ::listen(fd_, 4) == 0;
        thread_ = std::thread([this]() { serve(); });
    }

    ~FakeRecorder() {
        stop_ = true;
        ::shutdown(fd_, SHUT_RDWR);
        thread_.join();
        ::close(fd_);
        ::unlink(path_.c_str());
    }

    /** @return true if it listens */
    bool ok() const { return ok_; }

    /** @return the last line it was sent (after it was answered) */
    std::string received() const {
        while (!done_ && !stop_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return received_;
    }

private:
    std::string path_;
    std::string reply_;
    int fd_ = -1;
    bool ok_ = false;
    std::atomic<bool> stop_ {false};
    std::atomic<bool> done_ {false};
    std::string received_;
    std::thread thread_;

    /** Answer connections until stopped. */
    void serve() {
        while (!stop_) {
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) return;
            std::string line;
            char c = 0;
            while (::read(client, &c, 1) == 1 && c != '\n') line += c;
            received_ = line;
            done_ = true;
            if (!reply_.empty()) {
                if (::write(client, reply_.data(), reply_.size()) < 0) std::perror("write");
            } else {
                // Never answer: hold the connection until the test is over
                while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ::close(client);
        }
    }
};

/**
 * Seconds on a monotonic clock.
 * @return seconds
 */
double monotonic() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/**
 * Poll a control until its command finishes (or 3 s pass).
 * @param control the control
 * @return how long it took (s)
 */
double waitReply(eyecam::Control& control) {
    const double start = monotonic();
    while (control.busy() && monotonic() - start < 3.0) {
        control.poll(monotonic());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return monotonic() - start;
}

void testControl() {
    const std::string dir = tempDir();
    const std::string socket = dir + "/ctl.sock";

    // No recorder: fails at once, without waiting
    {
        eyecam::Control control;
        const double start = monotonic();
        CHECK(!control.send(socket, "start", monotonic()));
        CHECK(monotonic() - start < 0.1);
        CHECK(!control.busy() && control.hasReply());
        CHECK(!control.reply().ok);
        SAME(control.reply().command, "start");
        CHECK(control.reply().error.find("connect") == 0);
    }
    // "ok"
    {
        FakeRecorder recorder(socket, "ok\n");
        CHECK(recorder.ok());
        eyecam::Control control;
        const double start = monotonic();
        CHECK(control.send(socket, "start", monotonic()));
        CHECK(monotonic() - start < 0.1);  // sending never waits for the reply
        CHECK(control.busy());
        CHECK(!control.send(socket, "stop", monotonic()));  // one at a time
        waitReply(control);
        CHECK(!control.busy());
        CHECK(control.reply().ok);
        SAME(recorder.received(), "start");
    }
    // The light warning's "Start without light": the protocol goes with the command, on the same line
    {
        FakeRecorder recorder(socket, "ok\n");
        eyecam::Control control;
        CHECK(control.send(socket, eyecam::startCommand(eyecam::StartChoice::WithoutLight), monotonic()));
        waitReply(control);
        CHECK(control.reply().ok);
        SAME(control.reply().command, "start widen_nolight");
        SAME(recorder.received(), "start widen_nolight");
    }
    // "err <reason>"
    {
        FakeRecorder recorder(socket, "err not idle\n");
        eyecam::Control control;
        CHECK(control.send(socket, "stop", monotonic()));
        waitReply(control);
        CHECK(!control.reply().ok);
        SAME(control.reply().command, "stop");
        SAME(control.reply().error, "not idle");
        SAME(recorder.received(), "stop");
    }
    // No reply: gives up after the timeout, never blocking a poll
    {
        FakeRecorder recorder(socket, "");
        eyecam::Control control;
        CHECK(control.send(socket, "start", monotonic(), 0.3));
        double longestPoll = 0;
        const double start = monotonic();
        while (control.busy() && monotonic() - start < 3.0) {
            const double before = monotonic();
            control.poll(monotonic());
            longestPoll = std::max(longestPoll, monotonic() - before);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const double took = monotonic() - start;
        CHECK(!control.busy());
        CHECK(took > 0.25 && took < 1.0);
        CHECK(longestPoll < 0.05);
        SAME(control.reply().error, "no reply in time");
    }
    // A reply without its newline before the recorder closes still counts
    {
        FakeRecorder recorder(socket, "ok");
        eyecam::Control control;
        CHECK(control.send(socket, "stop", monotonic()));
        waitReply(control);
        CHECK(control.reply().ok);
    }
    ::rmdir(dir.c_str());
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testParse();
    testVisible();
    testText();
    testFill();
    testConfirm();
    testNoLight();
    testFade();
    testHideAtOnce();
    testReply();
    testReadFile();
    testControl();
    if (gFailures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("eyecam-test: all passed\n");
    return 0;
}
