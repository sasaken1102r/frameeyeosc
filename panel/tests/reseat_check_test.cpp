// Tests for the headset re-seat test (reseat_check.cpp): the step plan, a whole test through the session at 90 samples
// a second with a made-up headset taken off and put back on (the samples stopping) and a different seat each round,
// the per-round scores (against the dots and against the still start's centre), the re-seat's waiting (the wearer's
// time, the eyes settled, the headset never seen off, the eyes never back), a stop, no data, no pupil map, and the
// record's files. Built with the panel as reseat-test; exits non-zero on failure. All numbers are made up.
#include "reseat_check.h"
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

using namespace reseat_check;

/** Valve's and the pupils' offset from every dot (degrees), the same in every seat... */
constexpr double kValveOffset[2] = {-2.0, 1.0};
constexpr double kPupilOffset[2] = {0.5, -0.3};

/**
 * ...and how each round's seat moves the pupils' gaze (Valve's a tenth of it).
 * @param round 1..kRounds (0: the still start's seat)
 * @param axis 0 x, 1 y
 * @return degrees
 */
double seatShift(int round, int axis) {
    if (round == 0) return 0.0;
    return axis == 0 ? (round % 3 - 1) * 2.0 : (round % 2) * 1.5;
}

/** A made-up test: samples at 90 a second, each a frame after the tick. */
struct Simulation {
    Session session;
    double now = 10.0;
    bool mapped = true;
    double offFrom = 2.0;     ///< in each re-seat step, no samples from this many seconds in...
    double offFor = 4.0;      ///< ...for this long (0: the headset never seen off; -1: never back)
    double stopAt = -1.0;     ///< stop this many seconds in (-1: run to the end)
    double silentFrom = -1.0; ///< no samples from this many seconds in
    bool raw = true;          ///< the samples come with their raw values

    /**
     * The sample for the step shown now.
     * @param step the step
     * @return the sample
     */
    gaze_measure::Sample sampleAt(int step) const {
        gaze_measure::Sample s;
        s.time = 5000.0 + now;
        s.mapped = mapped;
        s.usable = true;
        s.beforeSmoothing = true;
        const Step& st = steps()[step];
        double g[2] = {0, 0};
        gaze_measure::targetGaze({st.yawDeg, st.pitchDeg}, g);
        double v[2];
        double p[2];
        for (int i = 0; i < 2; ++i) {
            v[i] = g[i] + kValveOffset[i] + 0.1 * seatShift(st.round, i);
            p[i] = g[i] + kPupilOffset[i] + seatShift(st.round, i);
        }
        s.valve[0] = static_cast<float>(v[0] / 45);
        s.valve[1] = static_cast<float>(v[1] / 45);
        if (mapped) {
            s.pupilState = gaze_measure::PupilState::Pupil;
            s.pupil[0] = static_cast<float>(p[0] / 45);
            s.pupil[1] = static_cast<float>(p[1] / 45);
        }
        if (raw) {
            s.raw.has = true;
            s.raw.pair[0] = 0.2f + 0.001f * st.round;
        }
        return s;
    }

    /** Run it (from start to its end, or to stopAt). */
    void run() {
        const double start = now;
        session.start(now, 1.79e9 + now);
        const double dt = 1.0 / 90;
        for (int guard = 0; guard < 200000 && session.active(); ++guard) {
            if (stopAt >= 0 && now - start >= stopAt) {
                session.stop(1.79e9 + now);
                break;
            }
            const Tick tick = session.tick(now, 1.79e9 + now);
            if (!session.active()) break;
            bool silent = silentFrom >= 0 && now - start >= silentFrom;
            if (tick.step >= 0 && steps()[tick.step].kind == StepKind::Reseat) {
                const double into = now - session.shownAt()[tick.step];
                const bool off = into >= offFrom && (offFor < 0 || into < offFrom + offFor);
                if (offFor != 0 && off) silent = true;
            }
            if (!silent && tick.step >= 0) session.add(now + 0.002, 1.79e9 + now, sampleAt(tick.step));
            now += dt;
        }
    }
};

void testPlan() {
    const std::vector<Step>& all = steps();
    CHECK(static_cast<int>(all.size()) == kStepCount && kStepCount == 101 && kRounds == 10 && kPlaces == 9);
    CHECK(near(kMinRunSec, 270.0) && near(kMaxRunSec, 790.0) && kRecordVersion == 1);
    // The still start, 10 s at the centre
    CHECK(all[0].kind == StepKind::Still && all[0].round == 0 && near(all[0].seconds, 10.0));
    CHECK(all[0].yawDeg == 0.0 && all[0].pitchDeg == 0.0);
    // Ten rounds: the re-seat (at the centre), then the gaze error check's first nine places, 2 s each (the corners
    // among them)
    int corners = 0;
    for (int r = 1; r <= kRounds; ++r) {
        const int first = 1 + (r - 1) * 10;
        const Step& reseat = all[first];
        CHECK(reseat.kind == StepKind::Reseat && reseat.round == r && near(reseat.seconds, kReseatMinSec));
        CHECK(reseat.yawDeg == 0.0 && reseat.pitchDeg == 0.0 && reseat.place == -1);
        for (int p = 0; p < kPlaces; ++p) {
            const Step& d = all[first + 1 + p];
            CHECK(d.kind == StepKind::Dot && d.round == r && d.place == p && near(d.seconds, 2.0));
            CHECK(d.yawDeg == gaze_measure::kPositions[p].yawDeg && d.pitchDeg == gaze_measure::kPositions[p].pitchDeg);
            if (d.yawDeg != 0 && d.pitchDeg != 0) ++corners;
        }
    }
    CHECK(corners == 4 * kRounds);
    // From when they count: the still start from 2 s, a dot's last second, a re-seat never
    CHECK(near(settleSec(all[0]), 2.0) && near(settleSec(all[2]), 1.0) && std::isinf(settleSec(all[1])));
    // Names and lines
    CHECK(stepName(all[0]) == "still" && stepName(all[1]) == "reseat_1" && stepName(all[2]) == "dot_0_0");
    CHECK(stepName(all[7]) == "dot_-20_15" && stepName(all[91]) == "reseat_10" && stepName(all[100]) == "dot_-20_-15");
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    CHECK(instruction(ja, all[1]) == "ヘッドセットを外して、かぶり直してね");
    CHECK(instruction(ja, all[1], true) == "かぶったら、目を開けて点を見てね");
    CHECK(instruction(en, all[1]) == "Take the headset off and put it back on");
    CHECK(instruction(ja, all[0]) == "じっと見ててね" && instruction(ja, all[5]) == "点を見てね");
    CHECK(instruction(en, all[5]) == "Look at the dot");
}

void testWholeRun() {
    Simulation sim;
    sim.run();
    const View& view = sim.session.view();
    CHECK(view.phase == Phase::Done && view.stepsDone == kStepCount && view.roundsDone == kRounds && !view.timedOut);
    CHECK(view.mapped && view.beforeSmoothing && view.raw);
    // Each re-seat: off from 2 s to 6 s in, settled 3 s after: on at 9 s (a frame or two either way)
    const double* reseat = sim.session.reseatSec();
    const double* off = sim.session.offSec();
    for (int r = 0; r < kRounds; ++r) {
        CHECK(near(reseat[r], 9.0, 0.05) && near(off[r], 4.0, 0.05));
    }
    // 10 s still + 10 x (9 s + 18 s): about 280 s of samples but the 4 s off in each round
    CHECK(near(sim.now - 10.0, 280.0, 0.2));
    CHECK(view.samples > (280 - 40) * 90 - 100 && view.samples < (280 - 40) * 90 + 100);
    const Result& r = view.result;
    CHECK(r.mapped && r.valve.rounds == kRounds && r.pupil.rounds == kRounds);
    double mx = 0.0;
    double my = 0.0;
    for (int round = 1; round <= kRounds; ++round) {
        const RoundScore& rs = r.rounds[round - 1];
        // Every dot counted; the round's offset is its seat's, its centre moved by the seat
        CHECK(rs.pupil.dots == kPlaces && rs.valve.dots == kPlaces);
        CHECK(near(rs.pupil.dx, kPupilOffset[0] + seatShift(round, 0), 1e-3));
        CHECK(near(rs.pupil.dy, kPupilOffset[1] + seatShift(round, 1), 1e-3));
        CHECK(near(rs.valve.dx, kValveOffset[0] + 0.1 * seatShift(round, 0), 1e-3));
        CHECK(near(rs.pupil.centreDx, seatShift(round, 0), 1e-3) && near(rs.pupil.centreDy, seatShift(round, 1), 1e-3));
        // The median angle to the dots: about the offset's size (the angles aren't quite flat away from the centre)
        const double size = std::hypot(rs.pupil.dx, rs.pupil.dy);
        CHECK(near(rs.pupil.error, size, 0.05 + 0.05 * size));
        CHECK(near(rs.reseatSec, 9.0, 0.05) && near(rs.offSec, 4.0, 0.05));
        mx += rs.pupil.dx;
        my += rs.pupil.dy;
    }
    // Over the rounds: the mean error, how far the seats' offsets sit from their mean, the centre's mean move
    mx /= kRounds;
    my /= kRounds;
    double spread = 0.0;
    double centre = 0.0;
    double error = 0.0;
    for (int round = 1; round <= kRounds; ++round) {
        const RoundScore& rs = r.rounds[round - 1];
        spread += std::hypot(rs.pupil.dx - mx, rs.pupil.dy - my);
        centre += std::hypot(seatShift(round, 0), seatShift(round, 1));
        error += rs.pupil.error;
    }
    CHECK(near(r.pupil.spread, spread / kRounds, 1e-3) && near(r.pupil.centre, centre / kRounds, 1e-3));
    CHECK(near(r.pupil.error, error / kRounds, 1e-9) && near(r.valve.spread, spread / kRounds / 10, 1e-3));
    char brief[64];
    std::snprintf(brief, sizeof(brief), "Valve %.1f°・瞳 %.1f°", r.valve.error, r.pupil.error);
    CHECK(briefText(uiText(Language::Ja), r) == brief);
    std::snprintf(brief, sizeof(brief), "Valve %.1f°, pupil %.1f°", r.valve.spread, r.pupil.spread);
    CHECK(spreadText(uiText(Language::En), r) == brief);
    // The re-seat steps' samples never count; the dots' only in their last second
    for (const Row& row : sim.session.rows()) {
        if (row.step < 0) continue;
        const Step& st = steps()[row.step];
        const double into = row.at - sim.session.shownAt()[row.step];
        if (st.kind == StepKind::Reseat) CHECK(!row.counted);
        if (st.kind == StepKind::Dot) CHECK(row.counted == (into >= 1.0));
    }

    // The record's files
    sim.session.setVideo(VideoState::On);
    sim.session.setVideoDir("/home/steamos/eyecam/rec_2026-11-14_19-00-00");
    const std::string json = recordJson(sim.session.view(), sim.session.rows(), sim.session.shownAt(),
                                        sim.session.shownUnix(), sim.session.startedAt(), r);
    JsonValue root;
    std::string error2;
    CHECK(parseJson(json, root, error2) && root.isObject());
    const JsonValue* stepsJson = root.get("steps");
    CHECK(stepsJson != nullptr && stepsJson->isArray() && stepsJson->items.size() == static_cast<size_t>(kStepCount));
    if (stepsJson != nullptr && stepsJson->items.size() == static_cast<size_t>(kStepCount)) {
        const JsonValue& dot = stepsJson->items[2];
        CHECK(dot.get("name")->text == "dot_0_0" && near(dot.get("round")->number, 1) && near(dot.get("place")->number, 0));
        // Shown at 10 s + the first re-seat's 9 s; its first sample the eye server's time then
        CHECK(near(dot.get("shown_s")->number, 19.0, 0.05) && near(dot.get("first_sample_time")->number, 5029.0, 0.05));
        CHECK(dot.get("pupil") != nullptr && dot.get("pupil")->get("counts")->boolean);
        CHECK(stepsJson->items[1].get("pupil") == nullptr && stepsJson->items[1].get("name")->text == "reseat_1");
    }
    const JsonValue* rounds = root.get("rounds");
    CHECK(rounds != nullptr && rounds->isArray() && rounds->items.size() == static_cast<size_t>(kRounds));
    if (rounds != nullptr && rounds->items.size() == static_cast<size_t>(kRounds)) {
        const JsonValue& third = rounds->items[2];
        CHECK(near(third.get("round")->number, 3) && near(third.get("reseat_s")->number, 9.0, 0.05));
        CHECK(near(third.get("pupil")->get("dx_deg")->number, kPupilOffset[0] + seatShift(3, 0), 1e-3));
        CHECK(near(third.get("pupil")->get("centre_dy_deg")->number, seatShift(3, 1), 1e-3));
    }
    const JsonValue* summary = root.get("summary");
    CHECK(summary != nullptr && near(summary->get("pupil")->get("rounds")->number, kRounds));
    const JsonValue* video = root.get("video");
    CHECK(video != nullptr && video->get("protocol")->text == "reseat" && video->get("state")->text == "on");
    CHECK(video->get("session_dir")->text == "/home/steamos/eyecam/rec_2026-11-14_19-00-00");
    CHECK(root.get("result")->text == "done" && near(root.get("rounds_done")->number, kRounds));
    CHECK(root.get("raw")->boolean && !root.get("timed_out")->boolean);
    const std::string csv = samplesCsv(sim.session.rows(), sim.session.shownAt());
    CHECK(csv.rfind("panel_s,unix,step,step_name,round,step_s,counted,sample_time,", 0) == 0);
    CHECK(csv.find(",raw,pair_lx,pair_ly,pair_rx,pair_ry,cam_l_fresh,cam_l_frame_ns,") != std::string::npos);
    CHECK(csv.find(",2,dot_0_0,1,0.0") != std::string::npos && csv.find(",91,reseat_10,10,") != std::string::npos);
    CHECK(csv.find(",1,0.203000,") != std::string::npos);
    size_t lines = 0;
    for (char c : csv) lines += c == '\n' ? 1 : 0;
    CHECK(lines == sim.session.rows().size() + 1);
}

void testWaiting() {
    // The wearer's time: off for only 1.5 s from 1 s in, back at 2.5 s and settled at 5.5 s: the dots still wait until
    // 8 s
    {
        Simulation sim;
        sim.offFrom = 1.0;
        sim.offFor = 1.5;
        sim.stopAt = 10.0 + 9.0;
        sim.run();
        CHECK(near(sim.session.reseatSec()[0], kReseatMinSec, 0.05) && near(sim.session.offSec()[0], 1.5, 0.05));
    }
    // The headset never seen off (the samples go on): only from 20 s on
    {
        Simulation sim;
        sim.offFor = 0;
        sim.run();
        CHECK(sim.session.view().phase == Phase::Done && near(sim.session.reseatSec()[4], 20.0, 0.05));
        CHECK(near(sim.session.offSec()[4], 1.0 / 90, 0.01));
    }
    // Off, then not settled: the line changes past the wearer's time (only once the headset was seen off)
    {
        Session s;
        s.start(0, 0);
        double now = 0;
        for (; now < 10.0; now += 0.01) {
            s.tick(now, 0);
            s.add(now, 0, Simulation().sampleAt(0));
        }
        Tick t = s.tick(10.0, 0);
        CHECK(t.step == 1 && t.changed && !t.waiting && t.seconds == 8 && near(t.progress, 1.0));
        // No samples for 9 s: not failed (the headset is off), the wearer's time over, the eyes not back
        t = s.tick(19.0, 0);
        CHECK(s.active() && t.step == 1 && t.seconds == 0 && !t.waiting);
        // Back (unusable at first: the eyes still shut), then open: waiting until settled
        gaze_measure::Sample shut = Simulation().sampleAt(1);
        shut.usable = false;
        s.add(19.1, 0, shut);
        s.add(19.2, 0, Simulation().sampleAt(1));
        t = s.tick(19.3, 0);
        CHECK(t.step == 1 && t.waiting && s.view().waiting);
        for (now = 19.3; now < 22.3; now += 0.01) {
            s.add(now, 0, Simulation().sampleAt(1));
            t = s.tick(now, 0);
        }
        CHECK(t.step == 2 && !t.waiting && near(s.reseatSec()[0], 12.2, 0.02) && near(s.offSec()[0], 9.2, 0.02));
    }
    // Never back: failed at 60 s, said why
    {
        Simulation sim;
        sim.offFor = -1;
        sim.run();
        const View& v = sim.session.view();
        CHECK(v.phase == Phase::Failed && v.timedOut && v.roundsDone == 0 && v.stepsDone == 1);
        CHECK(near(sim.session.reseatSec()[0], kReseatMaxSec, 0.05));
    }
}

void testWithoutMap() {
    Simulation sim;
    sim.mapped = false;
    sim.raw = false;
    sim.run();
    const Result& r = sim.session.view().result;
    CHECK(sim.session.view().phase == Phase::Done && !r.mapped && r.valve.rounds == kRounds && r.pupil.rounds == 0);
    CHECK(!sim.session.view().raw && !std::isfinite(r.pupil.error) && briefText(uiText(Language::Ja), r).rfind("Valve ", 0) == 0);
    CHECK(briefText(uiText(Language::Ja), r).find("瞳") == std::string::npos);
    const std::string csv = samplesCsv(sim.session.rows(), sim.session.shownAt());
    CHECK(csv.find(",0,,,,,") != std::string::npos);
}

void testStopAndNoData() {
    // Stopped in the third round's dots: stopped, the first two rounds scored, the third with too few dots
    Simulation sim;
    sim.stopAt = 10.0 + 2 * 27.0 + 9.0 + 4 * 2.0 + 0.5;
    sim.run();
    const View& view = sim.session.view();
    CHECK(view.phase == Phase::Stopped && view.roundsDone == 2 && view.stepsDone == 1 + 2 * 10 + 1 + 4);
    CHECK(view.result.rounds[1].pupil.counts() && !view.result.rounds[2].pupil.counts() && view.result.pupil.rounds == 2);
    // Its one end
    Tick t = sim.session.tick(sim.now, 0);
    CHECK(t.ended && !t.showTarget);
    t = sim.session.tick(sim.now + 0.1, 0);
    CHECK(!t.ended);
    // No samples for kNoDataSec in the dots: failed (not timed out)
    Simulation silent;
    silent.silentFrom = 25.0;
    silent.run();
    CHECK(silent.session.view().phase == Phase::Failed && !silent.session.view().timedOut);
    CHECK(near(silent.now - 10.0, 25.0 + kNoDataSec, 0.05));
    // Closed: back to idle
    silent.session.dismiss();
    CHECK(silent.session.view().phase == Phase::Idle && silent.session.rows().empty());
    // The eye video's state and why it is missing
    Session s;
    s.start(0, 0);
    s.setVideo(VideoState::Off, "eyecam-rec not running");
    CHECK(s.view().video == VideoState::Off && s.view().videoReason == "eyecam-rec not running");
    s.setVideo(VideoState::Refused, "err 録画中");
    CHECK(s.view().videoReason == "err 録画中");
}

}  // namespace

int main() {
    testPlan();
    testWholeRun();
    testWaiting();
    testWithoutMap();
    testStopAndNoData();
    if (gFailures == 0) std::printf("reseat-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
