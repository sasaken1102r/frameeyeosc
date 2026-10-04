// Tests for the eye capture tab's logic (eyecam.cpp): reading eyecam-rec's status.json (every state, missing and odd
// fields), when the tab shows (stale, stopped, missing), the step texts in both languages, when the full-view light
// shows and how it fades (and when it goes at once), the light warning before a start and the commands it sends, the
// calibrations (their commands, what ran last), the setup checklist (its step for every combination, how the setup's
// calibration ends, the screens, the Konsole command lines it starts), and the control socket against a stand-in
// recorder in a temporary folder (never the real one; nothing is started). Built with the panel as eyecam-test; exits non-zero
// on failure.
#include "eyecam.h"
#include "setup_tools.h"

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
#include <iterator>
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
                   {"calibrating", State::Calibrating}, {"paused", State::Unknown}};
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
    // (no calibration fields: an eyecam-rec before them)
    CHECK(s.calibState == 0 && !s.recalibSuggested && !s.live && std::isnan(s.liveMs));

    // The calibration and live fields
    {
        const Status c = eyecam::parseStatus(
            "{\"state\": \"calibrating\", \"locked\": true, \"step_index\": 2, \"step_count\": 6, "
            "\"step_label\": \"normal\", \"step_remaining_s\": 4.5, \"calib_state\": 2, "
            "\"recalib_suggested\": true, \"live\": true, \"live_ms\": 3.75}",
            kNow);
        CHECK(c.present && c.state == State::Calibrating);
        CHECK(c.locked && c.stepIndex == 2 && c.stepCount == 6);
        SAME(c.stepLabel, "normal");
        CHECK(c.calibState == 2 && c.recalibSuggested && c.live);
        CHECK(std::fabs(c.liveMs - 3.75) < 1e-9);
        // Odd types are missing; a negative calib_state is none
        const Status odd = eyecam::parseStatus(
            "{\"state\": \"idle\", \"calib_state\": \"3\", \"recalib_suggested\": 1, \"live\": \"on\", "
            "\"live_ms\": null}",
            kNow);
        CHECK(odd.calibState == 0 && !odd.recalibSuggested && !odd.live && std::isnan(odd.liveMs));
        CHECK(eyecam::parseStatus("{\"state\": \"idle\", \"calib_state\": -1}", kNow).calibState == 0);
        CHECK(eyecam::parseStatus("{\"state\": \"idle\", \"calib_state\": 3}", kNow).calibState == 3);
        CHECK(eyecam::parseStatus("{\"state\": \"idle\", \"calib_state\": 7}", kNow).calibState == 7);
    }
    // The baseline learned by itself, and whether a wear calibration was ever saved (a newer eyecam-rec)
    {
        const Status warming = eyecam::parseStatus(
            "{\"state\": \"idle\", \"calib_state\": 0, \"baseline\": \"warming\", \"warmup_remaining_s\": 21.3, "
            "\"calib_saved\": false}",
            kNow);
        CHECK(warming.hasBaseline && warming.baseline == "warming" && eyecam::baselineWarming(warming));
        CHECK(std::fabs(warming.warmupRemainingS - 21.3) < 1e-9);
        CHECK(warming.hasCalibSaved && !warming.calibSaved);
        const Status ready = eyecam::parseStatus(
            "{\"state\": \"idle\", \"calib_state\": 4, \"baseline\": \"ready\", \"calib_saved\": true}", kNow);
        CHECK(ready.hasBaseline && ready.baseline == "ready" && !eyecam::baselineWarming(ready));
        CHECK(ready.calibState == eyecam::kCalibAutoBit);
        CHECK(std::isnan(ready.warmupRemainingS));
        CHECK(ready.hasCalibSaved && ready.calibSaved);
        // An older eyecam-rec: none of them
        const Status old = eyecam::parseStatus("{\"state\": \"idle\", \"calib_state\": 1}", kNow);
        CHECK(!old.hasBaseline && old.baseline.empty() && !eyecam::baselineWarming(old));
        CHECK(std::isnan(old.warmupRemainingS) && !old.hasCalibSaved && !old.calibSaved);
        // Odd types are missing
        const Status odd = eyecam::parseStatus(
            "{\"state\": \"idle\", \"baseline\": 1, \"warmup_remaining_s\": \"5\", \"calib_saved\": \"yes\"}",
            kNow);
        CHECK(!odd.hasBaseline && std::isnan(odd.warmupRemainingS) && !odd.hasCalibSaved && !odd.calibSaved);
    }

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
    for (const char* state : {"waiting_fds", "searching", "recording", "calibrating", "error", "something_new"}) {
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
    // Not recording (a label left over from the last run); a calibration has no light at all
    for (const char* state : {"idle", "searching", "error", "stopped", "waiting_fds", "calibrating"}) {
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
    // Recording no longer running: stopped, failed, back to idle, calibrating, the recorder gone
    for (const char* state : {"idle", "error", "stopped", "searching", "waiting_fds", "calibrating", "something_new"}) {
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

/**
 * A status as eyecam-rec writes it while ready (or in another state).
 * @param state the state
 * @param calibState calib_state
 * @param extra more members, each starting with ", "
 * @return the status
 */
Status calibStatus(const std::string& state, int calibState, const std::string& extra = "") {
    return eyecam::parseStatus("{\"state\": \"" + state + "\", \"locked\": true, \"live\": true, \"calib_state\": " +
                                   std::to_string(calibState) + extra + "}",
                               kNow);
}

void testCalib() {
    using eyecam::Calib;
    using eyecam::Run;
    // The commands
    SAME(eyecam::calibCommand(Calib::Wear), "calib wear");
    SAME(eyecam::calibCommand(Calib::User), "calib user");
    // The user's needs this wear's first (bit 0); the user's own bit alone isn't enough
    for (int bits = 0; bits <= 3; ++bits) {
        CHECK(eyecam::userCalibAllowed(calibStatus("idle", bits)) == ((bits & 1) != 0));
    }

    // The run a command starts
    CHECK(eyecam::runOfCommand("calib wear") == Run::CalibWear);
    CHECK(eyecam::runOfCommand("calib user") == Run::CalibUser);
    CHECK(eyecam::runOfCommand("start") == Run::Recording);
    CHECK(eyecam::runOfCommand("start widen_nolight") == Run::Recording);
    for (const char* other : {"stop", "live on", "live off", "calib", "calib other", "started", ""}) {
        CHECK(eyecam::runOfCommand(other) == Run::None);
    }
    CHECK(eyecam::isCalib(Run::CalibWear) && eyecam::isCalib(Run::CalibUser));
    CHECK(!eyecam::isCalib(Run::Recording) && !eyecam::isCalib(Run::None));

    // What ran last, following the status
    const auto step = [](const std::string& state, const std::string& label) {
        return eyecam::parseStatus("{\"state\": \"" + state + "\", \"step_label\": \"" + label + "\"}", kNow);
    };
    for (const char* label : {"squint", "look_up", "look_down"}) {
        CHECK(eyecam::followRun(Run::None, step("calibrating", label)) == Run::CalibUser);
        CHECK(eyecam::followRun(Run::CalibWear, step("calibrating", label)) == Run::CalibUser);
    }
    for (const char* label : {"close", "normal", "widen"}) {
        CHECK(eyecam::followRun(Run::CalibUser, step("calibrating", label)) == Run::CalibWear);
    }
    // The countdown and the wait for the video: the calibration known, else this wear's
    for (const char* label : {"lead_in", ""}) {
        CHECK(eyecam::followRun(Run::CalibUser, step("calibrating", label)) == Run::CalibUser);
        CHECK(eyecam::followRun(Run::CalibWear, step("calibrating", label)) == Run::CalibWear);
        CHECK(eyecam::followRun(Run::None, step("calibrating", label)) == Run::CalibWear);
        CHECK(eyecam::followRun(Run::Recording, step("calibrating", label)) == Run::CalibWear);
    }
    CHECK(eyecam::followRun(Run::CalibUser, step("recording", "normal")) == Run::Recording);
    CHECK(eyecam::followRun(Run::CalibWear, step("searching", "")) == Run::Recording);
    // Other states keep it: an error after a calibration is the calibration's
    for (const char* state : {"idle", "error", "waiting_fds", "stopped"}) {
        for (const Run run : {Run::None, Run::Recording, Run::CalibWear, Run::CalibUser}) {
            CHECK(eyecam::followRun(run, step(state, "")) == run);
        }
    }
    // Along a user calibration that fails: sent -> countdown -> its steps -> error
    {
        Run run = eyecam::runOfCommand("calib user");
        run = eyecam::followRun(run, step("calibrating", "lead_in"));
        CHECK(run == Run::CalibUser);
        run = eyecam::followRun(run, step("calibrating", "look_down"));
        run = eyecam::followRun(run, step("error", ""));
        CHECK(run == Run::CalibUser);
        // ...then a recording
        run = eyecam::followRun(run, step("searching", ""));
        run = eyecam::followRun(run, step("error", ""));
        CHECK(run == Run::Recording);
    }

    // The light warning closes when a calibration starts (from the eye cameras tab, or elsewhere)
    {
        eyecam::StartConfirm confirm;
        confirm.open(State::Idle);
        CHECK(confirm.sync(State::Calibrating, true));
        CHECK(!confirm.isOpen());
        CHECK(!confirm.open(State::Calibrating));
    }

    // The tab redraws when the calibration's state, the drift, live or the last run change
    {
        eyecam::View a;
        a.status = calibStatus("idle", 0);
        a.visible = true;
        const std::string base = eyecam::signature(a);
        eyecam::View b = a;
        b.status.calibState = 1;
        CHECK(eyecam::signature(b) != base);
        b = a;
        b.status.recalibSuggested = true;
        CHECK(eyecam::signature(b) != base);
        b = a;
        b.status.live = false;
        CHECK(eyecam::signature(b) != base);
        b = a;
        b.lastRun = Run::CalibUser;
        CHECK(eyecam::signature(b) != base);
        // ...the baseline, its seconds left (whole ones, as shown), calib_saved
        b = a;
        b.status.hasBaseline = true;
        b.status.baseline = "warming";
        CHECK(eyecam::signature(b) != base);
        eyecam::View c = b;
        b.status.warmupRemainingS = 20.4;
        c.status.warmupRemainingS = 20.1;
        SAME(eyecam::signature(b), eyecam::signature(c));
        c.status.warmupRemainingS = 19.9;
        CHECK(eyecam::signature(b) != eyecam::signature(c));
        b = a;
        b.status.hasCalibSaved = b.status.calibSaved = true;
        CHECK(eyecam::signature(b) != base);
        // (live_ms alone doesn't redraw: it isn't shown)
        b = a;
        b.status.liveMs = 9.0;
        SAME(eyecam::signature(b), base);
    }
}

void testCalibText() {
    // Every text the eye cameras add is there in both languages (the table has no check of its own)
    for (const Language language : {Language::Ja, Language::En}) {
        const UiText& t = uiText(language);
        for (const char* text :
             {t.rowCameraLids, t.cameraUseBoth, t.cameraUseLeft, t.cameraUseRight, t.cameraUseValve,
              t.cameraUseValveFormat, t.cameraWhyNotCalibrated, t.cameraWhyNoCamera, t.cameraPupilSuffix,
              t.cameraWhyWarming, t.eyecamWarmingFormat, t.eyecamLiveOff, t.eyecamCalibWearTitle,
              t.eyecamCalibUserTitle, t.eyecamCalibWaiting, t.eyecamCalibErrorTitle, t.eyecamCalibRetry,
              t.eyecamSensitivity, t.eyecamSensitivityDull, t.eyecamSensitivitySharp, t.setupTitle, t.setupOptional,
              t.setupOneLeft, t.setupAllDone, t.setupStepPassword, t.setupStepTool, t.setupStepLearn, t.setupStepDone,
              t.setupLaterTool, t.setupLaterLearn, t.setupLaterDone, t.setupPasswordLabel, t.setupPasswordSet,
              t.setupAutoChecked, t.setupToolLabel, t.setupToolDone, t.setupLearnLabel, t.setupLearnDone,
              t.setupPassPill, t.setupPassBody, t.setupPassWhere, t.setupPassPath1, t.setupPassPath2,
              t.setupPassPath3, t.setupPassKonsole, t.setupPassKonsoleHow, t.setupPassMemo, t.setupPassMemoText,
              t.setupPassButton, t.setupVideo, t.setupVideoNote, t.setupCheckPill, t.setupCheckFlow,
              t.setupCheckTyped, t.setupCheckWhat, t.setupCheckWhatText, t.setupCheckPassword,
              t.setupCheckPasswordText, t.setupCheckSsh, t.setupCheckSshText, t.setupCheckButton, t.setupWaitPill,
              t.setupWaitTitle, t.setupWaitVideo, t.setupWaitVideoOk, t.setupWaitVideoNo, t.setupWaitEyeOk,
              t.setupWaitEyeNo, t.setupWaitButton, t.setupWaitHint1, t.setupWaitHint2, t.setupWaitFoot,
              t.setupLearnPill, t.setupLearnWidenHint, t.setupChipClose, t.setupChipNormal, t.setupChipWiden,
              t.setupLeftAfter, t.setupLearnStepFormat, t.setupLearnFoot, t.setupStop, t.setupErrorPill,
              t.setupAgain, t.setupFailPill, t.setupFailTitle, t.setupFailBody, t.setupFailClosed, t.setupFailNormal,
              t.setupFailWiden, t.setupFailWidenValue, t.setupFailProceed, t.setupFailLater, t.setupDoneTitle,
              t.setupDoneBody, t.setupDoneHelp, t.setupDoneHelp1, t.setupDoneHelp1Do, t.setupDoneHelp2,
              t.setupDoneHelp2Do, t.setupDoneButton, t.setupDoneNote, t.nextTitle, t.nextPass, t.nextWait,
              t.nextLearnFormat, t.nextFail, t.nextError, t.readyTitle, t.readyNote, t.lidsFromValve,
              t.lidsFromCamera, t.lidsFromCameraLeft, t.lidsFromCameraRight, t.camRowState, t.camRowStateHint,
              t.camLearned, t.camRowLidsHint, t.camRowSensitivityHint, t.camRowCalib, t.camRowCalibHint,
              t.camCalibButton, t.camCalibSide1, t.camCalibSide2, t.camRowUser, t.camRowUserHint, t.camUserButton,
              t.camUserSide1, t.camUserSide2, t.camUserNeedsCalib, t.camHelpTitle, t.camHelp1, t.camHelp1Do,
              t.camHelp2, t.camHelp2Do, t.camHelp3, t.camHelp3Do, t.camHelp4, t.camHelp4Do, t.lidsCamRow,
              t.lidsCamRowHint, t.lidsCamText1, t.lidsCamText2, t.lidsCamButton, t.lidsCamNote, t.lidsCamMarks,
              t.lidsCamMarksOpen, t.camLineBothVrchat, t.camLineBoth, t.camLineLeft, t.camLineRight,
              t.camLineWarmingFormat, t.camLineWarming, t.camLinePutOn, t.camLineOff, t.devTitle,
              t.devRecord, t.devRecordHint, t.tabEyecam}) {
            CHECK(text != nullptr && text[0] != '\0');
        }
        CHECK(t.setupLeftBefore != nullptr);  // (empty in English: "7 s left")
        // The formats: one %s, or the numbers they are given
        CHECK(std::string(t.cameraUseValveFormat).find("%s") != std::string::npos);
        CHECK(std::string(t.eyecamWarmingFormat).find("%d") != std::string::npos);
        CHECK(std::string(t.eyecamWarmingFormat).find("%s") == std::string::npos);
        CHECK(std::string(t.nextLearnFormat).find("%d") != std::string::npos);
        char line[200];
        std::snprintf(line, sizeof(line), t.setupLearnStepFormat, 3, 5, 18);
        CHECK(std::string(line).find('3') != std::string::npos && std::string(line).find("18") != std::string::npos);
    }
    // The mocks' wording (案D, as the user wrote it)
    const UiText& ja = uiText(Language::Ja);
    SAME(ja.tabEyecam, "目のカメラ");
    SAME(ja.setupTitle, "目のカメラの準備（最初の一度だけ）");
    SAME(ja.camCalibButton, "目のカメラの校正（18秒）");
    SAME(ja.camUserButton, "ユーザー校正（最初に 1 回）");
    SAME(ja.setupWaitButton, "覚えはじめる");
    SAME(ja.setupFailProceed, "このまま進む");
    SAME(ja.setupAgain, "もう一度（18秒）");
    SAME(ja.nextTitle, "次にやること・目のカメラ");
    SAME(ja.readyTitle, "目のカメラ：準備できたよ");
    // "かぶりの校正" is gone from every text
    CHECK(std::string(ja.eyecamCalibWearTitle).find("かぶり") == std::string::npos);
}

void testAutoGrab() {
    // The sudo command shows only while eyecam-rec can't take the buffers by itself
    Status s;
    for (const char* grab : {"", "missing", "no_cap", "unsafe: group-writable"}) {
        s.autoGrab = grab;
        CHECK(eyecam::needsManualGrab(s));
    }
    for (const char* grab : {"waiting_tracker", "trying", "ok", "failed: no eye tracker"}) {
        s.autoGrab = grab;
        CHECK(!eyecam::needsManualGrab(s));
    }
    // Read from the file; an older eyecam-rec writes none
    const std::string json = R"({"version":1,"state":"waiting_fds","auto_grab":"waiting_tracker","updated_unix":)" +
                             std::to_string(kNow) + "}";
    SAME(eyecam::parseStatus(json, kNow).autoGrab, "waiting_tracker");
    SAME(eyecam::parseStatus(fullStatus("idle", ""), kNow).autoGrab, "");
}

void testSensitivity() {
    // Read from status.json; missing (an older eyecam-rec: no slider) or not a number is none
    {
        const Status s = eyecam::parseStatus("{\"state\": \"idle\", \"widen_sensitivity\": 0.6}", kNow);
        CHECK(s.hasWidenSensitivity && std::fabs(s.widenSensitivity - 0.6) < 1e-9);
        for (const char* bad : {"{\"state\": \"idle\"}", "{\"state\": \"idle\", \"widen_sensitivity\": null}",
                                "{\"state\": \"idle\", \"widen_sensitivity\": \"0.6\"}"}) {
            const Status none = eyecam::parseStatus(bad, kNow);
            CHECK(!none.hasWidenSensitivity && std::isnan(none.widenSensitivity));
        }
        // In every state (it is shown wherever the camera section is)
        CHECK(eyecam::parseStatus("{\"state\": \"calibrating\", \"widen_sensitivity\": 0}", kNow).hasWidenSensitivity);
    }
    // The command: two decimals, kept within 0..1, nothing for a value that isn't a number
    SAME(eyecam::sensitivityCommand(0.6), "set widen_sensitivity 0.60");
    SAME(eyecam::sensitivityCommand(0.456), "set widen_sensitivity 0.46");
    SAME(eyecam::sensitivityCommand(0.0), "set widen_sensitivity 0.00");
    SAME(eyecam::sensitivityCommand(1.0), "set widen_sensitivity 1.00");
    SAME(eyecam::sensitivityCommand(1.7), "set widen_sensitivity 1.00");
    SAME(eyecam::sensitivityCommand(-0.3), "set widen_sensitivity 0.00");
    SAME(eyecam::sensitivityCommand(std::nan("")), "");
    CHECK(eyecam::isSensitivityCommand("set widen_sensitivity 0.60"));
    CHECK(!eyecam::isSensitivityCommand("calib wear") && !eyecam::isSensitivityCommand("stop"));
    // It starts no run
    CHECK(eyecam::runOfCommand("set widen_sensitivity 0.60") == eyecam::Run::None);
    // The redraw follows it (to 0.01)
    {
        eyecam::View a;
        a.status = eyecam::parseStatus("{\"state\": \"idle\", \"widen_sensitivity\": 0.6}", kNow);
        eyecam::View b = a;
        b.status.widenSensitivity = 0.61;
        CHECK(eyecam::signature(a) != eyecam::signature(b));
        b.status.widenSensitivity = 0.6004;
        SAME(eyecam::signature(a), eyecam::signature(b));
    }

    // Sending: on release always; while dragged at most every kSensitivitySendSec and only when it moved; never
    // while another command is out (the newest value waits)
    {
        eyecam::SensitivitySender sender;
        std::string command;
        CHECK(!sender.next(false, 0.0, command));
        sender.dragged(0.4, 10.0);
        CHECK(sender.next(false, 10.0, command));
        SAME(command, "set widen_sensitivity 0.40");
        CHECK(!sender.next(false, 10.01, command));  // once
        sender.dragged(0.5, 10.1);                  // too soon
        CHECK(!sender.next(false, 10.1, command));
        sender.dragged(0.41, 10.0 + eyecam::kSensitivitySendSec + 0.01);  // hardly moved... 0.01 is a move
        CHECK(sender.next(false, 10.4, command));
        SAME(command, "set widen_sensitivity 0.41");
        sender.dragged(0.412, 11.0);  // less than half a step: no
        CHECK(!sender.next(false, 11.0, command));
        // Many moves in a second: at most a few commands
        int sent = 0;
        for (int i = 0; i < 90; ++i) {
            const double now = 20.0 + i / 90.0;
            sender.dragged(0.1 + i * 0.008, now);
            if (sender.next(false, now, command)) ++sent;
        }
        CHECK(sent >= 2 && sent <= 4);
        // Busy: it waits, and the newest value goes
        sender.released(0.7);
        CHECK(!sender.next(true, 30.0, command));
        sender.released(0.75);
        CHECK(sender.next(false, 30.1, command));
        SAME(command, "set widen_sensitivity 0.75");
        // A release goes even right after a drag send of the same value
        sender.dragged(0.9, 40.0);
        CHECK(sender.next(false, 40.0, command));
        sender.released(0.9);
        CHECK(sender.next(false, 40.01, command));
        SAME(command, "set widen_sensitivity 0.90");
    }
}

/**
 * A status as a newer eyecam-rec writes it during the setup.
 * @param state the state
 * @param extra more members, each starting with ", "
 * @return the status
 */
Status setupStatus(const std::string& state, const std::string& extra = "") {
    return eyecam::parseStatus("{\"state\": \"" + state + "\", \"locked\": true, \"live\": true" + extra + "}", kNow);
}

void testMessageEn() {
    // In English the recorder's own English line, when it gives one; else (and in Japanese) its message
    const Status both = eyecam::parseStatus(
        "{\"state\": \"idle\", \"message\": \"校正できた（かぶり）\", \"message_en\": \"Calibrated (this wear)\"}", kNow);
    SAME(both.messageEn, "Calibrated (this wear)");
    SAME(eyecam::shownMessage(both, Language::En), "Calibrated (this wear)");
    SAME(eyecam::shownMessage(both, Language::Ja), "校正できた（かぶり）");
    const Status jaOnly = eyecam::parseStatus("{\"state\": \"error\", \"message\": \"左目: 下を見ても\"}", kNow);
    CHECK(jaOnly.messageEn.empty());
    SAME(eyecam::shownMessage(jaOnly, Language::En), "左目: 下を見ても");
    const Status emptyEn = eyecam::parseStatus(
        "{\"state\": \"error\", \"message\": \"失敗\", \"message_en\": \"\"}", kNow);
    SAME(eyecam::shownMessage(emptyEn, Language::En), "失敗");
    const Status odd = eyecam::parseStatus("{\"state\": \"error\", \"message\": \"失敗\", \"message_en\": 3}", kNow);
    SAME(eyecam::shownMessage(odd, Language::En), "失敗");
    SAME(eyecam::shownMessage(Status(), Language::En), "");
    // ...and it redraws
    eyecam::View a;
    a.status = both;
    eyecam::View b = a;
    b.status.messageEn = "Calibrated";
    CHECK(eyecam::signature(a) != eyecam::signature(b));
}

void testSetupParse() {
    const Status s = setupStatus("idle", ", \"has_buffers\": true, \"setup_done\": false, \"last_calib_widen\": \"default\"");
    CHECK(s.hasBuffers && s.hasSetupDone && !s.setupDone);
    SAME(s.lastCalibWiden, "default");
    const Status done = setupStatus("idle", ", \"setup_done\": true, \"last_calib_widen\": \"measured\"");
    CHECK(done.hasSetupDone && done.setupDone);
    SAME(done.lastCalibWiden, "measured");
    // An older eyecam-rec: none of them; odd types are missing
    const Status old = setupStatus("idle");
    CHECK(!old.hasBuffers && !old.hasSetupDone && !old.setupDone && old.lastCalibWiden.empty());
    const Status odd = setupStatus("idle", ", \"has_buffers\": 1, \"setup_done\": \"yes\", \"last_calib_widen\": 3");
    CHECK(!odd.hasBuffers && !odd.hasSetupDone && !odd.setupDone && odd.lastCalibWiden.empty());
    // ...and they redraw the tab
    eyecam::View a;
    a.status = setupStatus("idle", ", \"setup_done\": false");
    eyecam::View b = a;
    b.status.setupDone = true;
    CHECK(eyecam::signature(a) != eyecam::signature(b));
    b = a;
    b.password = eyecam::PasswordState::NotSet;
    CHECK(eyecam::signature(a) != eyecam::signature(b));
    b = a;
    b.readyNotice = true;
    CHECK(eyecam::signature(a) != eyecam::signature(b));
}

void testSetupStep() {
    using eyecam::PasswordState;
    using eyecam::SetupStep;
    // The tool: auto_grab saying it is there, has_buffers, or a state past the buffers
    for (const char* grab : {"ok", "trying", "waiting_tracker", "failed: x", "failed: timeout"}) {
        const Status st = setupStatus("waiting_fds", std::string(", \"auto_grab\": \"") + grab + "\"");
        CHECK(eyecam::toolInstalled(st));
        CHECK(!eyecam::needsManualGrab(st));
        // ...also in an error (taking the buffers failed)
        CHECK(eyecam::toolInstalled(setupStatus("error", std::string(", \"auto_grab\": \"") + grab + "\"")));
    }
    CHECK(eyecam::toolInstalled(setupStatus("waiting_fds", ", \"has_buffers\": true")));
    for (const char* state : {"idle", "searching", "recording", "calibrating"}) {
        CHECK(eyecam::toolInstalled(setupStatus(state)));
    }
    for (const char* grab : {"missing", "no_cap", "unsafe: group-writable", ""}) {
        const Status st = setupStatus("waiting_fds", std::string(", \"auto_grab\": \"") + grab + "\"");
        CHECK(!eyecam::toolInstalled(st));
        CHECK(eyecam::needsManualGrab(st));
        CHECK(!eyecam::toolInstalled(setupStatus("error", std::string(", \"auto_grab\": \"") + grab + "\"")));
    }
    // A failed calibration's error: eyecam-rec still writes has_buffers and auto_grab
    CHECK(eyecam::toolInstalled(
        setupStatus("error", ", \"has_buffers\": true, \"auto_grab\": \"ok\", \"setup_done\": false")));
    CHECK(eyecam::toolInstalled(setupStatus("error", ", \"has_buffers\": true, \"auto_grab\": \"missing\"")));
    // An older eyecam-rec (no auto_grab): only the buffers say so
    CHECK(eyecam::toolInstalled(setupStatus("error", ", \"has_buffers\": true")));
    CHECK(!eyecam::toolInstalled(setupStatus("error")));
    CHECK(!eyecam::toolInstalled(setupStatus("waiting_fds")));

    // Complete: setup_done when it is there, whatever calib_state says
    CHECK(eyecam::setupComplete(setupStatus("idle", ", \"setup_done\": true")));
    CHECK(!eyecam::setupComplete(setupStatus("idle", ", \"setup_done\": false, \"calib_state\": 7, \"calib_saved\": true")));
    // ...an older eyecam-rec: any baseline (bit 0 or 2) or a saved wear calibration
    for (int bits = 0; bits <= 7; ++bits) {
        CHECK(eyecam::setupComplete(setupStatus("idle", ", \"calib_state\": " + std::to_string(bits))) ==
              ((bits & 5) != 0));
    }
    CHECK(eyecam::setupComplete(setupStatus("idle", ", \"calib_saved\": true")));
    CHECK(!eyecam::setupComplete(setupStatus("idle", ", \"calib_saved\": false")));

    // The step for every combination of password, tool and setup done
    const PasswordState passwords[3] = {PasswordState::Unknown, PasswordState::Set, PasswordState::NotSet};
    for (const PasswordState password : passwords) {
        for (int tool = 0; tool < 2; ++tool) {
            for (int done = 0; done < 2; ++done) {
                const Status st = setupStatus(tool ? "idle" : "waiting_fds",
                                              std::string(", \"auto_grab\": \"") + (tool ? "ok" : "missing") +
                                                  "\", \"setup_done\": " + (done ? "true" : "false"));
                const SetupStep step = eyecam::setupStep(st, password);
                const SetupStep want = done ? SetupStep::Done
                                       : tool ? SetupStep::Learn
                                       : password == PasswordState::NotSet ? SetupStep::Password
                                                                           : SetupStep::Tool;
                CHECK(step == want);
            }
        }
    }
    // A failed setup calibration (error, the buffers still held) stays at (3); an error that says nothing is (2)
    CHECK(eyecam::setupStep(setupStatus("error", ", \"has_buffers\": true, \"auto_grab\": \"ok\""),
                            PasswordState::Set) == SetupStep::Learn);
    CHECK(eyecam::setupStep(setupStatus("error"), PasswordState::Set) == SetupStep::Tool);
    // An older eyecam-rec in use (bits) is never sent back, whatever the password says
    CHECK(eyecam::setupStep(setupStatus("waiting_fds", ", \"calib_state\": 4"), PasswordState::NotSet) ==
          SetupStep::Done);
    CHECK(eyecam::setupStep(setupStatus("waiting_fds", ", \"calib_state\": 1"), PasswordState::NotSet) ==
          SetupStep::Done);
    CHECK(eyecam::setupStep(setupStatus("waiting_fds", ", \"calib_saved\": true"), PasswordState::NotSet) ==
          SetupStep::Done);
    // ...one that isn't yet goes through it like a new one, the tool told by its buffers
    CHECK(eyecam::setupStep(setupStatus("idle", ", \"calib_state\": 2"), PasswordState::Set) == SetupStep::Learn);
    CHECK(eyecam::setupStep(setupStatus("waiting_fds", ", \"has_buffers\": false"), PasswordState::Set) ==
          SetupStep::Tool);
    CHECK(eyecam::setupStep(setupStatus("waiting_fds", ", \"has_buffers\": false"), PasswordState::NotSet) ==
          SetupStep::Password);
}

void testSetupFlow() {
    using eyecam::SetupResult;
    using eyecam::SetupScreen;
    const std::string notDone = ", \"auto_grab\": \"ok\", \"setup_done\": false";
    const std::string done = ", \"auto_grab\": \"ok\", \"setup_done\": true";
    /**
     * The view as the loop has it after following these statuses (at 100 s).
     */
    const auto follow = [](std::initializer_list<Status> statuses, eyecam::Run lastRun = eyecam::Run::CalibWear) {
        eyecam::View view;
        view.password = eyecam::PasswordState::Set;
        view.lastRun = lastRun;
        for (const Status& st : statuses) {
            view.status = st;
            view.flow.follow(st, 100.0);
        }
        return view;
    };
    // Measured: the checklist's done screen once, the ready notice a while, then the page
    {
        eyecam::View view = follow({setupStatus("idle", notDone), setupStatus("calibrating", notDone),
                                    setupStatus("idle", done + ", \"last_calib_widen\": \"measured\"")});
        CHECK(view.flow.result() == SetupResult::Done);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Done);
        CHECK(view.flow.readyNotice(100.0) && view.flow.readyNotice(111.0) && !view.flow.readyNotice(113.0));
        view.flow.proceed();
        CHECK(eyecam::setupScreen(view) == SetupScreen::Camera);
        // ...also when the dashboard closes instead
        eyecam::View closed = follow({setupStatus("idle", notDone), setupStatus("calibrating", notDone),
                                      setupStatus("idle", done + ", \"last_calib_widen\": \"measured\"")});
        closed.flow.closed();
        CHECK(eyecam::setupScreen(closed) == SetupScreen::Camera);
    }
    // Standard widening (eyecam-rec says setup_done with it): the question, until "continue" (the page) or once more
    {
        eyecam::View view = follow({setupStatus("idle", notDone), setupStatus("calibrating", notDone),
                                    setupStatus("idle", done + ", \"last_calib_widen\": \"default\"")});
        CHECK(view.flow.result() == SetupResult::Fail);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Fail);
        CHECK(!view.flow.readyNotice(100.0));
        view.flow.closed();  // a closed dashboard doesn't answer it
        CHECK(eyecam::setupScreen(view) == SetupScreen::Fail);
        eyecam::View again = view;
        view.flow.proceed();
        CHECK(eyecam::setupScreen(view) == SetupScreen::Camera);
        // once more: calibrating again clears it (now a calibration after the setup: no screen after it)
        again.status = setupStatus("calibrating", done);
        again.flow.follow(again.status, 200.0);
        CHECK(again.flow.result() == SetupResult::None);
        again.status = setupStatus("idle", done + ", \"last_calib_widen\": \"measured\"");
        again.flow.follow(again.status, 220.0);
        CHECK(eyecam::setupScreen(again) == SetupScreen::Camera);
        // An eyecam-rec that says the standard widening without setup_done: not set up, back to the button
        eyecam::View notSaved = follow({setupStatus("idle", notDone), setupStatus("calibrating", notDone),
                                        setupStatus("idle", notDone + ", \"last_calib_widen\": \"default\"")});
        CHECK(notSaved.flow.result() == SetupResult::None);
        CHECK(eyecam::setupScreen(notSaved) == SetupScreen::Wait);
    }
    // Failed: the checklist's error (and again), never a result. An older eyecam-rec that writes neither has_buffers
    // nor auto_grab says nothing of the tool in an error: (2), as nothing is remembered from before it
    {
        eyecam::View old = follow({setupStatus("idle"), setupStatus("calibrating"), setupStatus("error")});
        CHECK(eyecam::setupScreen(old) == SetupScreen::Check);
        old.status = setupStatus("error", ", \"has_buffers\": true");
        CHECK(eyecam::setupScreen(old) == SetupScreen::Error);
    }
    {
        const std::string held = notDone + ", \"has_buffers\": true";
        eyecam::View view = follow({setupStatus("idle", held), setupStatus("calibrating", held),
                                    setupStatus("error", held)});
        CHECK(view.flow.result() == SetupResult::None);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Error);
        // (an error after a recording isn't the calibration's: back to the button)
        view.lastRun = eyecam::Run::Recording;
        CHECK(eyecam::setupScreen(view) == SetupScreen::Wait);
    }
    // Stopped ("やめる"): back to the button, nothing to show
    {
        const eyecam::View view = follow({setupStatus("idle", notDone), setupStatus("calibrating", notDone),
                                          setupStatus("idle", notDone)});
        CHECK(view.flow.result() == SetupResult::None);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Wait);
    }
    // A calibration after the setup ("when something feels off") shows no checklist screen at all
    {
        const eyecam::View view = follow({setupStatus("idle", done), setupStatus("calibrating", done),
                                          setupStatus("idle", done + ", \"last_calib_widen\": \"default\"")});
        CHECK(view.flow.result() == SetupResult::None);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Camera);
    }
    // An older eyecam-rec (no setup_done, no last_calib_widen): done once a baseline is there after it
    {
        const eyecam::View view = follow({setupStatus("idle", ", \"calib_state\": 0"),
                                          setupStatus("calibrating", ", \"calib_state\": 0"),
                                          setupStatus("idle", ", \"calib_state\": 1")});
        CHECK(eyecam::setupScreen(view) == SetupScreen::Done);
    }
    // Joined midway (the panel started while it calibrated)
    {
        const eyecam::View view = follow({setupStatus("calibrating", notDone),
                                          setupStatus("idle", done + ", \"last_calib_widen\": \"measured\"")});
        CHECK(eyecam::setupScreen(view) == SetupScreen::Done);
    }
    // The screens before (3)
    {
        eyecam::View view;
        view.status = setupStatus("waiting_fds", ", \"auto_grab\": \"missing\", \"setup_done\": false");
        view.password = eyecam::PasswordState::NotSet;
        CHECK(eyecam::setupScreen(view) == SetupScreen::Pass);
        view.password = eyecam::PasswordState::Set;
        CHECK(eyecam::setupScreen(view) == SetupScreen::Check);
        view.password = eyecam::PasswordState::Unknown;
        CHECK(eyecam::setupScreen(view) == SetupScreen::Check);
        view.status = setupStatus("idle", notDone);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Wait);
        view.status = setupStatus("calibrating", notDone);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Learn);
        view.status = setupStatus("recording", done);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Camera);
    }
    // Nothing sticks: each status decides the step (the rehearsal: set up, then the tool removed and eyecam-rec
    // restarted without calib.json)
    {
        const std::string removed =
            ", \"has_buffers\": false, \"auto_grab\": \"missing\", \"setup_done\": false, \"calib_state\": 0";
        eyecam::View view = follow({setupStatus("idle", notDone + ", \"has_buffers\": true"),
                                    setupStatus("calibrating", notDone + ", \"has_buffers\": true"),
                                    setupStatus("idle", done + ", \"has_buffers\": true, \"last_calib_widen\": "
                                                               "\"measured\", \"calib_state\": 5")});
        CHECK(eyecam::setupScreen(view) == SetupScreen::Done);
        view.flow.proceed();
        CHECK(eyecam::setupScreen(view) == SetupScreen::Camera);
        // eyecam-rec restarted without the tool: (2), not the page nor (3)
        view.status = setupStatus("waiting_fds", removed);
        view.flow.follow(view.status, 300.0);
        view.lastRun = eyecam::Run::None;
        CHECK(eyecam::setupScreen(view) == SetupScreen::Check);
        view.password = eyecam::PasswordState::NotSet;
        CHECK(eyecam::setupScreen(view) == SetupScreen::Pass);
        view.password = eyecam::PasswordState::Set;
        // ...the tool installed again: (3) while it waits for the tracker, tries, or after a failed try
        for (const char* grab : {"waiting_tracker", "trying", "failed: x", "ok"}) {
            view.status = setupStatus("waiting_fds", ", \"has_buffers\": false, \"setup_done\": false, "
                                                     "\"auto_grab\": \"" + std::string(grab) + "\"");
            view.flow.follow(view.status, 310.0);
            CHECK(eyecam::setupScreen(view) == SetupScreen::Wait);
        }
        // ...and gone again
        view.status = setupStatus("waiting_fds", removed);
        view.flow.follow(view.status, 320.0);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Check);
    }
    // setup_done back to false (calib.json removed) while the done screen or the page shows: the checklist again
    {
        eyecam::View view = follow({setupStatus("idle", notDone), setupStatus("calibrating", notDone),
                                    setupStatus("idle", done + ", \"last_calib_widen\": \"measured\"")});
        CHECK(eyecam::setupScreen(view) == SetupScreen::Done);
        CHECK(view.flow.readyNotice(100.0));
        view.status = setupStatus("idle", notDone);
        view.flow.follow(view.status, 101.0);
        CHECK(view.flow.result() == SetupResult::None);
        CHECK(!view.flow.readyNotice(101.0));
        CHECK(eyecam::setupScreen(view) == SetupScreen::Wait);
        // ...done again by eyecam-rec alone (no calibration seen): the page, without a done screen
        view.status = setupStatus("idle", done);
        view.flow.follow(view.status, 102.0);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Camera);
        // ...the standard widening answered with "continue", then setup_done gone: the checklist, not the page
        eyecam::View fail = follow({setupStatus("idle", notDone), setupStatus("calibrating", notDone),
                                    setupStatus("idle", done + ", \"last_calib_widen\": \"default\"")});
        CHECK(eyecam::setupScreen(fail) == SetupScreen::Fail);
        fail.flow.proceed();
        CHECK(eyecam::setupScreen(fail) == SetupScreen::Camera);
        fail.status = setupStatus("idle", notDone + ", \"last_calib_widen\": \"default\"");
        fail.flow.follow(fail.status, 130.0);
        CHECK(eyecam::setupScreen(fail) == SetupScreen::Wait);
        // ...and a Fail still on screen goes too
        eyecam::View failShown = follow({setupStatus("idle", notDone), setupStatus("calibrating", notDone),
                                         setupStatus("idle", done + ", \"last_calib_widen\": \"default\"")});
        failShown.status = setupStatus("idle", notDone);
        failShown.flow.follow(failShown.status, 140.0);
        CHECK(eyecam::setupScreen(failShown) == SetupScreen::Wait);
    }
    // An older eyecam-rec: its fallback decides each time too (the baseline lost: the checklist again)
    {
        eyecam::View view;
        view.password = eyecam::PasswordState::Set;
        view.status = setupStatus("idle", ", \"calib_state\": 4");
        view.flow.follow(view.status, 100.0);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Camera);
        view.status = setupStatus("idle", ", \"calib_state\": 0");
        view.flow.follow(view.status, 101.0);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Wait);
        view.status = setupStatus("waiting_fds", ", \"calib_state\": 0");
        view.flow.follow(view.status, 102.0);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Check);
        view.status = setupStatus("waiting_fds", ", \"calib_saved\": true");
        view.flow.follow(view.status, 103.0);
        CHECK(eyecam::setupScreen(view) == SetupScreen::Camera);
    }
    // The protocol the chips show: a countdown, then these five
    CHECK(std::size(eyecam::kCalibWearSteps) == 5);
    SAME(eyecam::kCalibWearSteps[2], "widen");
}

void testSetupTools() {
    // Konsole with the command typed in: the spec's line, exactly
    const std::vector<std::string> install = setup_tools::konsoleArgv(setup_tools::kInstallCommand, Language::Ja);
    CHECK(install.size() == 5);
    SAME(install[0], "konsole");
    SAME(install[1], "-e");
    SAME(install[2], "bash");
    SAME(install[3], "-c");
    SAME(install[4],
         "echo \"Enter を押すと実行するよ（パスワードを聞かれるよ）\"; read -e -p \"$ \" -i \"sudo "
         "$HOME/.local/lib/eyecam/install_grab.sh\" c && eval \"$c\"; echo; read -p \"Enter で閉じるよ\" _");
    // ...passwd the same way
    const std::vector<std::string> passwd = setup_tools::konsoleArgv(setup_tools::kPasswdCommand, Language::Ja);
    SAME(passwd[4], "echo \"Enter を押すと実行するよ（パスワードを聞かれるよ）\"; read -e -p \"$ \" -i \"passwd\" c && "
                    "eval \"$c\"; echo; read -p \"Enter で閉じるよ\" _");
    // ...in English too, with the same command
    const std::vector<std::string> english = setup_tools::konsoleArgv(setup_tools::kInstallCommand, Language::En);
    CHECK(english[4].find("-i \"sudo $HOME/.local/lib/eyecam/install_grab.sh\" c && eval \"$c\"") != std::string::npos);
    CHECK(english[4].find("Press Enter") != std::string::npos);
    // The buttons: each one's command, with the panel's language in what Konsole says
    for (const Language language : {Language::Ja, Language::En}) {
        const bool ja = language == Language::Ja;
        for (int button = 0; button < 2; ++button) {
            const std::vector<std::string> argv = setup_tools::setupKonsoleArgv(button, language);
            CHECK(argv == setup_tools::konsoleArgv(button == 1 ? "passwd" : setup_tools::kInstallCommand, language));
            const std::string& script = argv[4];
            CHECK(script.find(button == 1 ? "-i \"passwd\"" : "-i \"sudo $HOME/.local/lib/eyecam/install_grab.sh\"") !=
                  std::string::npos);
            CHECK((script.find("Enter を押すと実行するよ（パスワードを聞かれるよ）") != std::string::npos) == ja);
            CHECK((script.find("Enter で閉じるよ") != std::string::npos) == ja);
            CHECK((script.find("Press Enter to run it (it asks for your password)") != std::string::npos) == !ja);
            CHECK((script.find("Press Enter to close") != std::string::npos) == !ja);
        }
    }
    // Never sudo or passwd run by the panel itself: they are only typed in
    for (const auto& argv : {install, passwd, english}) CHECK(argv[0] == "konsole");
    SAME(setup_tools::kShownInstallCommand, "sudo ~/.local/lib/eyecam/install_grab.sh");
    // The video: Chromium through flatpak, never xdg-open; and none yet (its button stays hidden)
    const std::vector<std::string> video = setup_tools::videoArgv("https://example.com/v");
    CHECK(video.size() == 4);
    SAME(video[0], "flatpak");
    SAME(video[2], "org.chromium.Chromium");
    SAME(video[3], "https://example.com/v");
    SAME(setup_tools::kVideoUrl, "");
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
    // A calibration: the command as one line
    {
        FakeRecorder recorder(socket, "ok\n");
        eyecam::Control control;
        CHECK(control.send(socket, eyecam::calibCommand(eyecam::Calib::Wear), monotonic()));
        waitReply(control);
        CHECK(control.reply().ok);
        SAME(recorder.received(), "calib wear");
        CHECK(eyecam::runOfCommand(control.reply().command) == eyecam::Run::CalibWear);
    }
    // ...and the recorder's Japanese reason when it says no
    {
        FakeRecorder recorder(socket, "err 先に calib wear をしてね\n");
        eyecam::Control control;
        CHECK(control.send(socket, eyecam::calibCommand(eyecam::Calib::User), monotonic()));
        waitReply(control);
        CHECK(!control.reply().ok);
        SAME(control.reply().command, "calib user");
        SAME(control.reply().error, "先に calib wear をしてね");
        SAME(recorder.received(), "calib user");
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
    testCalib();
    testCalibText();
    testAutoGrab();
    testSensitivity();
    testMessageEn();
    testSetupParse();
    testSetupStep();
    testSetupFlow();
    testSetupTools();
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
