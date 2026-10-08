// Tests for the eye fit's sound cues (sounds.cpp): the WAV files and which cue goes with which moment of a fit,
// without playing anything. Built with the panel as sounds-test; exits non-zero on failure.
#include "sounds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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

using namespace sounds;
using gaze_fit::Actions;
using gaze_fit::Measured;
using gaze_fit::Point;
using gaze_fit::Session;

/**
 * A little-endian number in a byte buffer.
 * @param data the buffer
 * @param at where
 * @param bytes its size
 * @return the number
 */
uint32_t read(const std::vector<uint8_t>& data, size_t at, int bytes) {
    uint32_t value = 0;
    for (int i = 0; i < bytes; ++i) value |= static_cast<uint32_t>(data[at + i]) << (8 * i);
    return value;
}

void testWav() {
    const double peak = std::pow(10.0, kPeakDbfs / 20.0) * 32767.0;
    for (Cue cue : kAllCues) {
        const std::vector<int16_t> samples = synthesize(cue);
        const std::vector<uint8_t> file = wav(samples);
        CHECK(file.size() == 44 + samples.size() * 2);
        CHECK(std::memcmp(file.data(), "RIFF", 4) == 0 && std::memcmp(file.data() + 8, "WAVEfmt ", 8) == 0);
        CHECK(read(file, 4, 4) == file.size() - 8 && read(file, 16, 4) == 16);
        CHECK(read(file, 20, 2) == 1 && read(file, 22, 2) == 1);  // PCM, mono
        CHECK(read(file, 24, 4) == 48000 && read(file, 28, 4) == 96000);
        CHECK(read(file, 32, 2) == 2 && read(file, 34, 2) == 16);
        CHECK(std::memcmp(file.data() + 36, "data", 4) == 0 && read(file, 40, 4) == samples.size() * 2);
        // Peaks at -14 dBFS, and starts and ends at silence (no clicks)
        int loudest = 0;
        for (int16_t sample : samples) loudest = std::max(loudest, std::abs(static_cast<int>(sample)));
        CHECK(std::abs(loudest - peak) <= 1.0);
        CHECK(std::abs(samples.front()) < 50 && std::abs(samples.back()) < 50);
        Cue parsed = Cue::Pop;
        CHECK(parse(name(cue), parsed) && parsed == cue);
    }
    // Short cues stay short, the chimes a little longer
    CHECK(synthesize(Cue::Tick).size() < 48000 / 20);
    CHECK(synthesize(Cue::Pop).size() < 48000 / 10);
    CHECK(synthesize(Cue::Done).size() > 48000 / 2);
    Cue cue = Cue::Pop;
    CHECK(!parse("beep", cue));
}

/**
 * status.json as seen while frameeyeosc runs, with a capture in it.
 * @param id the capture id (0 for none)
 * @param m its result
 * @param closed whether it is the eyes-shut step (no gaze average)
 * @return the status
 */
EyeStatus answer(long long id, const Measured& m, bool closed) {
    EyeStatus status;
    status.present = status.running = status.tracking = true;
    if (id != 0) {
        GazeCaptureStatus& c = status.capture;
        c.present = true;
        c.id = id;
        c.done = true;
        c.samples = m.samples;
        c.hasAverage = !closed;
        c.x = m.x;
        c.y = m.y;
        c.spread = m.spread;
        c.hasOpenness = true;
        c.openness[0] = m.openness[0];
        c.openness[1] = m.openness[1];
    }
    return status;
}

/**
 * status.json while frameeyeosc runs, with no capture.
 * @return the status
 */
EyeStatus idle() {
    EyeStatus status;
    status.present = status.running = status.tracking = true;
    return status;
}

/**
 * A steady capture.
 * @param x x
 * @param y y
 * @param spread spread
 * @return the capture
 */
Measured steady(double x, double y, double spread = 0.01) {
    Measured m;
    m.x = x;
    m.y = y;
    m.spread = spread;
    m.samples = 120;
    m.hasOpenness = true;
    m.openness[0] = 0.9;
    m.openness[1] = 0.8;
    return m;
}

/** Drives a session frame by frame (every 1/90 s) and collects the cues with the time they sound. */
struct Run {
    Session session;
    FitCues cues;
    double now = 0.0;
    long long id = 0;
    std::vector<std::pair<double, Cue>> heard;
    gaze_fit::Dashboard dashboard {true, true};  ///< started from the panel: the dashboard open on it

    /**
     * Tick one frame.
     * @param status the status seen
     * @return the actions
     */
    Actions frame(const EyeStatus& status) {
        const Actions a = session.tick(now, dashboard, status);
        for (Cue cue : cues.update(session.view(), a)) heard.emplace_back(now, cue);
        now += 1.0 / 90;
        return a;
    }

    /**
     * Run frames until a capture is asked for, then answer it.
     * @param m the answer
     * @param closed the eyes-shut step
     */
    void step(const Measured& m, bool closed = false) {
        for (int i = 0; i < 90 * 5; ++i) {
            const Actions a = frame(idle());
            if (a.writeCapture) {
                session.captureSent(++id, now);
                break;
            }
        }
        for (int i = 0; i < 90; ++i) frame(answer(0, m, closed));  // frameeyeosc takes a moment
        frame(answer(id, m, closed));
    }

    /**
     * How many of a cue were heard.
     * @param cue the cue
     * @return the count
     */
    int count(Cue cue) const {
        return static_cast<int>(std::count_if(heard.begin(), heard.end(), [cue](const auto& h) { return h.second == cue; }));
    }
};

void testFullFitCues() {
    Run run;
    run.session.start(gaze_fit::Mode::Full, gaze_fit::Values(), 0.0);
    // Started with the dashboard open; it is closed halfway through, and the fit goes on
    const double side = gaze_fit::kSideDeg / gaze_fit::kFullScaleDeg;
    const double upDown = gaze_fit::kUpDownDeg / gaze_fit::kFullScaleDeg;
    run.step(steady(0, 0));
    run.step(steady(0, upDown, 0.2));  // unsteady: measured again
    run.step(steady(0, upDown));
    run.step(steady(0, -upDown));
    run.dashboard = {};
    run.step(steady(-side, 0));
    run.step(steady(side, 0));
    Measured shut = steady(0, 0);
    shut.openness[0] = 0.15;
    shut.openness[1] = 0.26;
    run.step(shut, true);
    for (int i = 0; i < 90 * 2; ++i) run.frame(idle());
    CHECK(run.session.view().phase == gaze_fit::Phase::Done);

    // A pop for each of the 5 dots, a pip for each measured dot, a buzz for the retry
    CHECK(run.count(Cue::Pop) == 5);
    CHECK(run.count(Cue::Pip) == 5);
    CHECK(run.count(Cue::Buzz) == 1);
    // "Close your eyes" 3, 2, 1, then the chime to open them, then done; nothing failed
    CHECK(run.count(Cue::Tick) == 3);
    CHECK(run.count(Cue::Open) == 1 && run.count(Cue::Done) == 1 && run.count(Cue::Fail) == 0);
    // In order: the ticks come before the open chime, which comes kReopenSec before done
    double tick = -1;
    double open = -1;
    double done = -1;
    for (const auto& h : run.heard) {
        if (h.second == Cue::Tick) tick = h.first;
        if (h.second == Cue::Open) open = h.first;
        if (h.second == Cue::Done) done = h.first;
    }
    CHECK(tick < open && open < done && std::fabs(done - open - gaze_fit::kReopenSec) < 0.05);
    // Ticks are about a second apart, as the countdown on the target
    std::vector<double> ticks;
    for (const auto& h : run.heard) {
        if (h.second == Cue::Tick) ticks.push_back(h.first);
    }
    CHECK(ticks.size() == 3 && std::fabs(ticks[2] - ticks[1] - 1.0) < 0.05);
    // The first dot pops as soon as it shows; later ones when they arrive after gliding
    CHECK(run.heard.front().second == Cue::Pop);
}

void testStopCues() {
    // Opening the dashboard during a run started without it (re-centering when the headset is put on): the falling
    // tones
    Run run;
    run.dashboard = {};
    run.session.start(gaze_fit::Mode::Center, gaze_fit::Values(), 0.0);
    run.frame(idle());
    run.session.tick(run.now, {true, true}, idle());
    std::vector<Cue> cues = run.cues.update(run.session.view(), Actions());
    CHECK(cues.size() == 1 && cues[0] == Cue::Fail);
    CHECK(run.count(Cue::Pop) == 1);
    // "Stop" pressed on the panel: the same
    Run stopped;
    stopped.session.start(gaze_fit::Mode::Full, gaze_fit::Values(), 0.0);
    stopped.frame(idle());
    stopped.session.cancel();
    cues = stopped.cues.update(stopped.session.view(), Actions());
    CHECK(cues.size() == 1 && cues[0] == Cue::Fail);
    // Re-centering done: a pip is not needed next to the chime
    Run center;
    center.session.start(gaze_fit::Mode::Center, gaze_fit::Values(), 0.0);
    center.step(steady(0.02, -0.02));
    CHECK(center.count(Cue::Done) == 1 && center.count(Cue::Pip) == 0 && center.count(Cue::Pop) == 1);
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testWav();
    testFullFitCues();
    testStopCues();
    if (gFailures == 0) std::printf("sounds-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
