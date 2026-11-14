// Tests for the headset slip test (slip_check.cpp): the steps' timeline, a whole test through the session at 90 samples
// a second with made-up slips, the per-direction scoring (the settling part of each step left out), a stop, no data,
// no pupil map, and the record's files. Built with the panel as slip-test; exits non-zero on failure. All numbers are
// made up.
#include "slip_check.h"
#include "json.h"

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

/**
 * Whether two numbers are within a tolerance.
 * @param a one
 * @param b the other
 * @param tolerance how close
 * @return true if close enough
 */
bool near(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) < tolerance;
}

using namespace slip_check;

/** How each source moves while the headset is pushed each way (degrees: dx, dy), and while widening / narrowing. */
constexpr double kValveSlip[kDirCount][2] = {{0.0, 0.3}, {0.0, -0.2}, {-0.4, 0.0}, {0.6, 0.0}};
constexpr double kPupilSlip[kDirCount][2] = {{0.0, 3.0}, {0.0, -2.5}, {2.0, 0.0}, {-3.3, 0.0}};
constexpr double kPupilWiden[2] = {0.0, -1.5};

/** A made-up test: samples at 90 a second, each a frame after the tick, with the slips above. */
struct Simulation {
    Session session;
    double now = 10.0;
    bool mapped = true;
    bool spikes = true;     ///< a large jump in the first second of each hold (the hands still moving): not counted
    double stopAt = -1.0;   ///< stop this many seconds in (-1: run to the end)
    double silentFrom = -1.0;  ///< no samples from this many seconds in

    /**
     * The sample for the step shown now.
     * @param step the step
     * @param into seconds into it
     * @return the sample
     */
    gaze_measure::Sample sampleAt(int step, double into) const {
        gaze_measure::Sample s;
        s.time = 5000.0 + now;
        s.mapped = mapped;
        s.usable = true;
        s.beforeSmoothing = true;
        const Step& st = kSteps[step];
        double g[2] = {0, 0};
        gaze_measure::targetGaze({st.yawDeg, st.pitchDeg}, g);
        double v[2] = {g[0] - 2.0, g[1] + 1.0};
        double p[2] = {g[0] + 0.5, g[1] - 0.3};
        const int d = dirIndex(st.dir);
        if (st.kind == StepKind::Push) {
            for (int i = 0; i < 2; ++i) {
                v[i] += kValveSlip[d][i];
                p[i] += kPupilSlip[d][i];
            }
        }
        if (st.kind == StepKind::Widen) {
            for (int i = 0; i < 2; ++i) p[i] += kPupilWiden[i];
        }
        if (spikes && (st.kind == StepKind::Push || st.kind == StepKind::Release) && into < 1.0) {
            v[1] += 20.0;
            p[1] += 20.0;
        }
        s.valve[0] = static_cast<float>(v[0] / 45);
        s.valve[1] = static_cast<float>(v[1] / 45);
        if (mapped) {
            s.pupilState = gaze_measure::PupilState::Pupil;
            s.pupil[0] = static_cast<float>(p[0] / 45);
            s.pupil[1] = static_cast<float>(p[1] / 45);
        }
        return s;
    }

    /** Run it (from start to its end, or to stopAt). */
    void run() {
        const double start = now;
        session.start(now, 1.79e9 + now);
        const double dt = 1.0 / 90;
        for (int guard = 0; guard < 20000 && session.active(); ++guard) {
            if (stopAt >= 0 && now - start >= stopAt) {
                session.stop(1.79e9 + now);
                break;
            }
            const Tick tick = session.tick(now, 1.79e9 + now);
            if (!session.active()) break;
            const bool silent = silentFrom >= 0 && now - start >= silentFrom;
            if (!silent && tick.step >= 0) {
                const double into = now - session.shownAt()[tick.step];
                session.add(now + 0.002, 1.79e9 + now, sampleAt(tick.step, into));
            }
            now += dt;
        }
    }
};

void testTimeline() {
    double sum = 0.0;
    for (const Step& s : kSteps) sum += s.seconds;
    CHECK(near(sum, kRunSec) && near(kRunSec, 132.0));
    CHECK(near(stepStart(0), 0.0) && near(stepStart(1), 10.0) && near(stepStart(kStepCount), kRunSec));
    // Still 10 s, then up, down, left, right twice, each pushed 5 s and let go of 5 s
    CHECK(kSteps[0].kind == StepKind::Still && near(kSteps[0].seconds, 10.0));
    const Dir order[kDirCount] = {Dir::Up, Dir::Down, Dir::Left, Dir::Right};
    for (int i = 0; i < 16; ++i) {
        const Step& s = kSteps[1 + i];
        if (i % 2 == 0) {
            CHECK(s.kind == StepKind::Push && s.dir == order[(i / 2) % kDirCount] && near(s.seconds, 5.0));
        } else {
            CHECK(s.kind == StepKind::Release && near(s.seconds, 5.0));
        }
        CHECK(s.yawDeg == 0.0 && s.pitchDeg == 0.0);
    }
    // Wide, normal, narrow, normal; the dot at the gaze error check's four sides, 3 s each; still 10 s
    CHECK(kSteps[17].kind == StepKind::Widen && kSteps[18].kind == StepKind::Normal);
    CHECK(kSteps[19].kind == StepKind::Squint && kSteps[20].kind == StepKind::Normal);
    const int places[4] = {3, 4, 1, 2};
    for (int i = 0; i < 4; ++i) {
        const Step& s = kSteps[21 + i];
        CHECK(s.kind == StepKind::Look && near(s.seconds, 3.0));
        CHECK(s.yawDeg == gaze_measure::kPositions[places[i]].yawDeg &&
              s.pitchDeg == gaze_measure::kPositions[places[i]].pitchDeg);
    }
    CHECK(kSteps[25].kind == StepKind::Still && near(kSteps[25].seconds, 10.0));
    // From when they count
    CHECK(near(settleSec(kSteps[0]), 2.0) && near(settleSec(kSteps[1]), 1.5) && near(settleSec(kSteps[21]), 1.0));
    // Names and lines
    CHECK(stepName(kSteps[0]) == "still" && stepName(kSteps[1]) == "push_up" && stepName(kSteps[2]) == "release");
    CHECK(stepName(kSteps[7]) == "push_right" && stepName(kSteps[17]) == "widen" && stepName(kSteps[19]) == "squint");
    CHECK(stepName(kSteps[23]) == "look_left" && stepName(kSteps[24]) == "look_right");
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    CHECK(instruction(ja, kSteps[1]) == "ヘッドセットを上に 3〜5 mm ずらして持ってて");
    CHECK(instruction(ja, kSteps[5]) == "ヘッドセットを左に 3〜5 mm ずらして持ってて");
    CHECK(instruction(en, kSteps[3]) == "Push the headset down 3-5 mm and hold it");
    CHECK(instruction(ja, kSteps[0]) == "じっと見ててね" && instruction(ja, kSteps[2]) == "はなしてね");
    CHECK(instruction(ja, kSteps[21]) == "点を見てね" && instruction(en, kSteps[19]) == "Narrow your eyes");
}

void testTicks() {
    Session session;
    session.start(100.0, 0);
    CHECK(session.active() && session.view().phase == Phase::Running);
    Tick t = session.tick(100.0, 0);
    CHECK(t.showTarget && t.step == 0 && t.changed && near(t.progress, 1.0) && t.seconds == 10);
    CHECK(t.yawDeg == 0.0 && t.pitchDeg == 0.0);
    // (a sample every second keeps it going; the first two in the still step's first 2 s don't count)
    for (int i = 0; i < 10; ++i) session.add(100.01 + i, 0, gaze_measure::Sample());
    t = session.tick(104.0, 0);
    CHECK(t.step == 0 && !t.changed && near(t.progress, 0.6) && t.seconds == 6);
    // The first push at 10 s, the dot still straight ahead; a sample in its first 1.5 s doesn't count
    t = session.tick(110.0, 0);
    CHECK(t.step == 1 && t.changed && t.seconds == 5 && t.yawDeg == 0.0);
    session.add(110.5, 0, gaze_measure::Sample());
    session.add(111.6, 0, gaze_measure::Sample());
    const std::vector<Row>& rows = session.rows();
    CHECK(rows.size() == 12 && rows[0].step == 0 && !rows[0].counted && !rows[1].counted && rows[2].counted);
    CHECK(rows[10].step == 1 && !rows[10].counted && rows[11].counted);
}

void testWholeRun() {
    Simulation sim;
    sim.run();
    const View& view = sim.session.view();
    CHECK(view.phase == Phase::Done && view.stepsDone == kStepCount && view.mapped && view.beforeSmoothing);
    CHECK(view.samples > 11800 && view.samples < 11950);
    const Result& r = view.result;
    CHECK(r.mapped && r.valveDirs == kDirCount && r.pupilDirs == kDirCount);
    // Each direction's move, the spikes in the first second of each hold and let-go left out
    for (int d = 0; d < kDirCount; ++d) {
        const MoveScore& m = r.pushes[d];
        CHECK(m.valve.rounds == 2 && m.pupil.rounds == 2);
        CHECK(near(m.valve.dx, kValveSlip[d][0], 1e-4) && near(m.valve.dy, kValveSlip[d][1], 1e-4));
        CHECK(near(m.pupil.dx, kPupilSlip[d][0], 1e-4) && near(m.pupil.dy, kPupilSlip[d][1], 1e-4));
    }
    CHECK(near(r.valveMean, (0.3 + 0.2 + 0.4 + 0.6) / 4, 1e-4) && near(r.pupilMean, (3.0 + 2.5 + 2.0 + 3.3) / 4, 1e-4));
    CHECK(near(r.widen.pupil.dy, kPupilWiden[1], 1e-4) && near(r.widen.valve.size(), 0.0, 1e-4));
    CHECK(r.widen.pupil.rounds == 1 && near(r.squint.pupil.size(), 0.0, 1e-4));
    // The steps' own medians: the still part's offsets, a look step's dot
    CHECK(near(r.steps[0].valve.x, -2.0, 1e-4) && near(r.steps[0].pupil.y, -0.3, 1e-4));
    double g[2];
    gaze_measure::targetGaze({0, 15}, g);
    CHECK(near(r.steps[21].valve.y, g[1] + 1.0, 1e-4) && r.steps[21].valve.counts());
    CHECK(briefText(uiText(Language::Ja), r) == "Valve 0.4°・瞳 2.7°");
    CHECK(briefText(uiText(Language::En), r) == "Valve 0.4°, pupil 2.7°");
    // The spikes (in the first second of each hold and let-go) all fell in the settling part, so none counted
    int spikesCounted = 0;
    int spikesSeen = 0;
    for (const Row& row : sim.session.rows()) {
        if (row.sample.pupil[1] * 45.0 < 17.0) continue;  // (the dot up 15° is 14.7°)
        ++spikesSeen;
        spikesCounted += row.counted ? 1 : 0;
    }
    CHECK(spikesSeen > 1300 && spikesCounted == 0);

    // The record's files
    sim.session.setVideo(VideoState::On);
    sim.session.setVideoDir("/home/steamos/eyecam/rec_2026-11-14_12-00-00");
    sim.session.setVideoDir("");
    const std::string json = recordJson(sim.session.view(), sim.session.rows(), sim.session.shownAt(),
                                        sim.session.shownUnix(), sim.session.startedAt(), r);
    JsonValue root;
    std::string error;
    CHECK(parseJson(json, root, error) && root.isObject());
    const JsonValue* steps = root.get("steps");
    CHECK(steps != nullptr && steps->isArray() && steps->items.size() == static_cast<size_t>(kStepCount));
    if (steps != nullptr && steps->items.size() == static_cast<size_t>(kStepCount)) {
        const JsonValue& first = steps->items[1];
        CHECK(first.get("name") != nullptr && first.get("name")->text == "push_up");
        CHECK(first.get("planned_s") != nullptr && near(first.get("planned_s")->number, 10.0));
        CHECK(first.get("shown_s") != nullptr && near(first.get("shown_s")->number, 10.0, 0.02));
        const JsonValue* sampleTime = first.get("first_sample_time");
        CHECK(sampleTime != nullptr && sampleTime->isNumber() && near(sampleTime->number, 5020.0, 0.02));
        CHECK(first.get("shown_ms") != nullptr && first.get("shown_ms")->isNumber());
    }
    const JsonValue* moves = root.get("moves");
    const JsonValue* up = moves != nullptr ? moves->get("push_up") : nullptr;
    const JsonValue* upPupil = up != nullptr ? up->get("pupil") : nullptr;
    CHECK(upPupil != nullptr && upPupil->get("dy_deg") != nullptr && near(upPupil->get("dy_deg")->number, 3.0, 1e-3));
    CHECK(upPupil != nullptr && upPupil->get("rounds") != nullptr && near(upPupil->get("rounds")->number, 2));
    const JsonValue* video = root.get("video");
    CHECK(video != nullptr && video->get("state") != nullptr && video->get("state")->text == "on");
    CHECK(video != nullptr && video->get("session_dir")->text == "/home/steamos/eyecam/rec_2026-11-14_12-00-00");
    CHECK(video != nullptr && video->get("protocol")->text == "free");
    CHECK(root.get("result") != nullptr && root.get("result")->text == "done");
    const std::string csv = samplesCsv(sim.session.rows(), sim.session.shownAt());
    CHECK(csv.rfind("panel_s,unix,step,step_name,step_s,counted,sample_time,", 0) == 0);
    CHECK(csv.find(",1,push_up,0.0") != std::string::npos && csv.find(",25,still,") != std::string::npos);
    size_t lines = 0;
    for (char c : csv) lines += c == '\n' ? 1 : 0;
    CHECK(lines == sim.session.rows().size() + 1);
}

void testWithoutMap() {
    Simulation sim;
    sim.mapped = false;
    sim.run();
    const Result& r = sim.session.view().result;
    CHECK(sim.session.view().phase == Phase::Done && !r.mapped && r.valveDirs == kDirCount && r.pupilDirs == 0);
    CHECK(!std::isfinite(r.pupilMean) && briefText(uiText(Language::Ja), r) == "Valve 0.4°");
}

void testStopAndNoData() {
    // Stopped while letting go after the second push: stopped, four steps done, the two pushes scored
    Simulation sim;
    sim.stopAt = 27.0;
    sim.run();
    const View& view = sim.session.view();
    CHECK(view.phase == Phase::Stopped && view.stepsDone == 4 && !sim.session.active());
    CHECK(view.result.pushes[0].pupil.rounds == 1 && view.result.pushes[1].pupil.rounds == 1);
    CHECK(view.result.pushes[2].pupil.rounds == 0 && view.result.pupilDirs == 2);
    // Its one end
    Tick t = sim.session.tick(sim.now, 0);
    CHECK(t.ended && !t.showTarget);
    t = sim.session.tick(sim.now + 0.1, 0);
    CHECK(!t.ended);
    // No samples for kNoDataSec: failed
    Simulation silent;
    silent.silentFrom = 20.0;
    silent.run();
    CHECK(silent.session.view().phase == Phase::Failed && silent.session.view().samples > 1700);
    // Closed: back to idle
    silent.session.dismiss();
    CHECK(silent.session.view().phase == Phase::Idle && silent.session.rows().empty());
    // The eye video's state and why it is missing
    Session s;
    s.start(0, 0);
    s.setVideo(VideoState::Off, "eyecam-rec not running");
    CHECK(s.view().video == VideoState::Off && s.view().videoReason == "eyecam-rec not running");
    s.setVideo(VideoState::Asked);
    CHECK(s.view().video == VideoState::Asked && s.view().videoReason == "eyecam-rec not running");
    s.setVideo(VideoState::Refused, "err 録画中");
    CHECK(s.view().videoReason == "err 録画中");
}

}  // namespace

int main() {
    testTimeline();
    testTicks();
    testWholeRun();
    testWithoutMap();
    testStopAndNoData();
    if (gFailures == 0) std::printf("slip-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
