// "Headset re-seat test": the steps and their timing, the scores and the record's files.
#include "reseat_check.h"

#include "json.h"

#include <algorithm>
#include <cstdio>

namespace reseat_check {

namespace {

/** A round counts for a source with at least this many of its dots. */
constexpr int kMinDots = 5;
/** A stretch this long without a usable sample in a re-seat step is the headset taken off (s)... */
constexpr double kOffSec = 1.0;
/** ...and without one, the dots still come once the eyes are settled this long after the re-seat was asked for (the
 *  headset taken off without a gap in the samples, say). */
constexpr double kNoOffSec = 20.0;

/**
 * The median of some numbers (NaN for none).
 * @param values the numbers (reordered)
 * @return the median
 */
double median(std::vector<double>& values) {
    if (values.empty()) return NAN;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

/**
 * A number as JSON: null for NaN.
 * @param value the number
 * @return the value
 */
JsonValue number(double value) {
    return std::isfinite(value) ? JsonValue::makeNumber(value) : JsonValue::makeNull();
}

/**
 * A text with one string in it.
 * @param format the format (one %s)
 * @param value the string
 * @return the text
 */
std::string fill(const char* format, const std::string& value) {
    char text[256];
    std::snprintf(text, sizeof(text), format, value.c_str());
    return text;
}

/**
 * Degrees with one decimal ("0.4").
 * @param value degrees
 * @return the text
 */
std::string oneDecimal(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", value);
    return text;
}

/**
 * The plan: the still start, then each round's re-seat and its nine dots.
 * @return the steps
 */
std::vector<Step> makeSteps() {
    std::vector<Step> out;
    out.push_back({StepKind::Still, 0, -1, kStillSec, 0.0, 0.0});
    for (int r = 1; r <= kRounds; ++r) {
        out.push_back({StepKind::Reseat, r, -1, kReseatMinSec, 0.0, 0.0});
        for (int p = 0; p < kPlaces; ++p) {
            const gaze_measure::Position& at = gaze_measure::kPositions[p];
            out.push_back({StepKind::Dot, r, p, kDotSec, at.yawDeg, at.pitchDeg});
        }
    }
    return out;
}

/**
 * The step of a round's dot.
 * @param round 1..kRounds
 * @param place 0..kPlaces-1
 * @return its index
 */
int dotStep(int round, int place) {
    return 1 + (round - 1) * (1 + kPlaces) + 1 + place;
}

/**
 * One source's round: its dots against their places and its centre dot against the still start.
 * @param result the steps' numbers
 * @param round 1..kRounds
 * @param which the source in a step's numbers
 * @return the round's numbers
 */
RoundSource roundOf(const Result& result, int round, SourceStep StepScore::*which) {
    RoundSource out;
    std::vector<double> errors;
    double sx = 0.0;
    double sy = 0.0;
    for (int p = 0; p < kPlaces; ++p) {
        const SourceStep& s = result.steps[dotStep(round, p)].*which;
        if (!s.counts()) continue;
        double target[2];
        gaze_measure::targetGaze(gaze_measure::kPositions[p], target);
        const double at[2] = {s.x, s.y};
        errors.push_back(gaze_measure::angleBetween(at, target));
        sx += s.x - target[0];
        sy += s.y - target[1];
    }
    const SourceStep& centre = result.steps[dotStep(round, 0)].*which;
    const SourceStep& still = result.steps[0].*which;
    if (centre.counts() && still.counts()) {
        out.centreDx = centre.x - still.x;
        out.centreDy = centre.y - still.y;
    }
    if (static_cast<int>(errors.size()) < kMinDots) return out;
    out.dots = static_cast<int>(errors.size());
    out.dx = sx / out.dots;
    out.dy = sy / out.dots;
    out.error = median(errors);
    return out;
}

/**
 * One source over the rounds.
 * @param result the rounds' numbers
 * @param which the source in a round's numbers
 * @return the overall numbers
 */
Overall overallOf(const Result& result, RoundSource RoundScore::*which) {
    Overall o;
    double error = 0.0;
    double mx = 0.0;
    double my = 0.0;
    double centre = 0.0;
    int centres = 0;
    for (const RoundScore& r : result.rounds) {
        const RoundSource& s = r.*which;
        if (std::isfinite(s.centreDx)) {
            centre += std::hypot(s.centreDx, s.centreDy);
            ++centres;
        }
        if (!s.counts()) continue;
        error += s.error;
        mx += s.dx;
        my += s.dy;
        ++o.rounds;
    }
    if (centres > 0) o.centre = centre / centres;
    if (o.rounds == 0) return o;
    o.error = error / o.rounds;
    mx /= o.rounds;
    my /= o.rounds;
    double spread = 0.0;
    for (const RoundScore& r : result.rounds) {
        const RoundSource& s = r.*which;
        if (s.counts()) spread += std::hypot(s.dx - mx, s.dy - my);
    }
    o.spread = spread / o.rounds;
    return o;
}

/**
 * A source's step numbers as JSON.
 * @param s the numbers
 * @return the object
 */
JsonValue sourceJson(const SourceStep& s) {
    JsonValue v;
    v.type = JsonValue::Type::Object;
    v.set("n", JsonValue::makeNumber(s.n, true));
    v.set("x_deg", number(s.x));
    v.set("y_deg", number(s.y));
    v.set("counts", JsonValue::makeBool(s.counts()));
    return v;
}

/**
 * A source's round as JSON.
 * @param s the numbers
 * @return the object
 */
JsonValue roundJson(const RoundSource& s) {
    JsonValue v;
    v.type = JsonValue::Type::Object;
    v.set("dots", JsonValue::makeNumber(s.dots, true));
    v.set("error_deg", number(s.error));
    v.set("dx_deg", number(s.dx));
    v.set("dy_deg", number(s.dy));
    v.set("centre_dx_deg", number(s.centreDx));
    v.set("centre_dy_deg", number(s.centreDy));
    v.set("counts", JsonValue::makeBool(s.counts()));
    return v;
}

/**
 * A source over the rounds as JSON.
 * @param o the numbers
 * @return the object
 */
JsonValue overallJson(const Overall& o) {
    JsonValue v;
    v.type = JsonValue::Type::Object;
    v.set("rounds", JsonValue::makeNumber(o.rounds, true));
    v.set("error_deg", number(o.error));
    v.set("spread_deg", number(o.spread));
    v.set("centre_deg", number(o.centre));
    return v;
}

}  // namespace

const std::vector<Step>& steps() {
    static const std::vector<Step> all = makeSteps();
    return all;
}

double settleSec(const Step& step) {
    if (step.kind == StepKind::Reseat) return INFINITY;
    return std::clamp(0.3 * step.seconds, 1.0, 2.0);
}

std::string stepName(const Step& step) {
    switch (step.kind) {
        case StepKind::Still: return "still";
        case StepKind::Reseat: return "reseat_" + std::to_string(step.round);
        case StepKind::Dot: {
            char name[32];
            std::snprintf(name, sizeof(name), "dot_%.0f_%.0f", step.yawDeg, step.pitchDeg);
            return name;
        }
    }
    return "still";
}

std::string instruction(const UiText& t, const Step& step, bool waiting) {
    switch (step.kind) {
        case StepKind::Still: return t.slipStill;
        case StepKind::Reseat: return waiting ? t.reseatBack : t.reseatOff;
        case StepKind::Dot: return t.slipLook;
    }
    return t.slipStill;
}

Result score(const std::vector<Row>& rows, const double* reseatSec, const double* offSec) {
    Result result;
    result.steps.resize(kStepCount);
    std::vector<std::vector<double>> vx(kStepCount), vy(kStepCount), px(kStepCount), py(kStepCount);
    std::vector<std::vector<double>> gx(kStepCount), gy(kStepCount);
    for (const Row& row : rows) {
        if (row.sample.mapped) result.mapped = true;
        if (row.sample.guarded) result.guardSeen = true;
        if (!row.counted || row.step < 0 || row.step >= kStepCount) continue;
        // Valve with both eyes open enough and not held (blinks left out), the guarded gaze on the same samples; the
        // pupils also only from both pupils and not while easing from another source
        const gaze_measure::Sample& s = row.sample;
        if (!s.usable || s.valveHeld) continue;
        vx[row.step].push_back(s.valve[0] * 45.0);
        vy[row.step].push_back(s.valve[1] * 45.0);
        if (s.guarded) {
            gx[row.step].push_back(s.guard[0] * 45.0);
            gy[row.step].push_back(s.guard[1] * 45.0);
        }
        if (s.pupilState == gaze_measure::PupilState::Pupil && !s.pupilBlending) {
            px[row.step].push_back(s.pupil[0] * 45.0);
            py[row.step].push_back(s.pupil[1] * 45.0);
        }
    }
    for (int i = 0; i < kStepCount; ++i) {
        StepScore& st = result.steps[i];
        st.valve.n = static_cast<int>(vx[i].size());
        st.valve.x = median(vx[i]);
        st.valve.y = median(vy[i]);
        st.pupil.n = static_cast<int>(px[i].size());
        st.pupil.x = median(px[i]);
        st.pupil.y = median(py[i]);
        st.guarded.n = static_cast<int>(gx[i].size());
        st.guarded.x = median(gx[i]);
        st.guarded.y = median(gy[i]);
    }
    for (int r = 1; r <= kRounds; ++r) {
        RoundScore& rs = result.rounds[r - 1];
        rs.valve = roundOf(result, r, &StepScore::valve);
        rs.pupil = roundOf(result, r, &StepScore::pupil);
        rs.guarded = roundOf(result, r, &StepScore::guarded);
        if (reseatSec != nullptr) rs.reseatSec = reseatSec[r - 1];
        if (offSec != nullptr) rs.offSec = offSec[r - 1];
    }
    result.valve = overallOf(result, &RoundScore::valve);
    result.pupil = overallOf(result, &RoundScore::pupil);
    result.guarded = overallOf(result, &RoundScore::guarded);
    result.missing = gaze_measure::missingOf(rows);
    return result;
}

void Session::start(double now, double unixNow) {
    view_ = View();
    view_.phase = Phase::Running;
    view_.startUnix = unixNow;
    rows_.clear();
    rows_.reserve(40000);
    start_ = now;
    lastSample_ = now;
    usableSince_ = NAN;
    lastUsable_ = NAN;
    for (double& at : shownAt_) at = NAN;
    for (double& at : shownUnix_) at = NAN;
    for (double& s : reseatSec_) s = NAN;
    for (double& s : offSec_) s = NAN;
    ended_ = false;
}

void Session::show(int step, double now, double unixNow) {
    view_.step = step;
    view_.stepsDone = step;
    const Step& s = steps()[step];
    view_.round = s.round;
    if (s.kind == StepKind::Reseat) view_.roundsDone = s.round - 1;
    shownAt_[step] = now;
    shownUnix_[step] = unixNow;
}

void Session::end(Phase phase, double unixNow) {
    view_.phase = phase;
    view_.endUnix = unixNow;
    view_.waiting = false;
    view_.result = score(rows_, reseatSec_, offSec_);
    ended_ = true;
}

void Session::stop(double unixNow) {
    if (!active()) return;
    end(Phase::Stopped, unixNow);
}

Tick Session::tick(double now, double unixNow) {
    Tick tick;
    if (!active()) {
        if (ended_) {
            tick.ended = true;
            ended_ = false;
        }
        return tick;
    }
    if (view_.step < 0) {
        show(0, now, unixNow);
        tick.changed = true;
    }
    const auto finish = [&](Phase phase) {
        end(phase, unixNow);
        ended_ = false;
        tick.ended = true;
        return tick;
    };
    // A gap in the samples: the eyes are not back (or not any more)
    if (now - lastSample_ > kGapSec) usableSince_ = NAN;
    const int k = view_.step;
    const Step& s = steps()[k];
    // No samples for a while outside a re-seat: frameeyeosc stopped, or the eyes aren't tracked
    if (s.kind != StepKind::Reseat && now - lastSample_ > kNoDataSec) return finish(Phase::Failed);
    const double into = now - shownAt_[k];
    bool next = into >= s.seconds;
    if (s.kind == StepKind::Reseat) {
        // On once the wearer's time is up and the eyes are back and settled (after being away, or late without that)
        const bool settled = std::isfinite(usableSince_) && now - usableSince_ >= kSettledSec;
        const bool wentOff = std::isfinite(offSec_[s.round - 1]) && offSec_[s.round - 1] >= kOffSec;
        next = into >= kReseatMinSec && settled && (wentOff || into >= kNoOffSec);
        if (next) reseatSec_[s.round - 1] = into;
        if (!next && into >= kReseatMaxSec) {
            reseatSec_[s.round - 1] = into;
            view_.timedOut = true;
            return finish(Phase::Failed);
        }
    }
    if (next) {
        if (k + 1 >= kStepCount) {
            view_.stepsDone = kStepCount;
            view_.roundsDone = kRounds;
            return finish(Phase::Done);
        }
        // (a timed step's next one starts where it ends, so the frames between don't add up over the test; after a
        // re-seat, now)
        const double at = s.kind == StepKind::Reseat ? now : std::min(now, shownAt_[k] + s.seconds);
        show(k + 1, at, unixNow - (now - at));
        tick.changed = true;
    }
    const int cur = view_.step;
    const Step& c = steps()[cur];
    const double in = now - shownAt_[cur];
    const double length = c.kind == StepKind::Reseat ? kReseatMinSec : c.seconds;
    const double left = std::max(0.0, length - in);
    const bool wentOff = c.kind == StepKind::Reseat && std::isfinite(offSec_[c.round - 1]) && offSec_[c.round - 1] >= kOffSec;
    view_.stepLeft = left;
    view_.waiting = c.kind == StepKind::Reseat && in >= kReseatMinSec && wentOff;
    tick.showTarget = true;
    tick.step = cur;
    tick.yawDeg = c.yawDeg;
    tick.pitchDeg = c.pitchDeg;
    tick.progress = std::clamp(left / length, 0.0, 1.0);
    tick.seconds = static_cast<int>(std::ceil(left - 1e-9));
    tick.waiting = view_.waiting;
    return tick;
}

void Session::add(double now, double unixNow, const gaze_measure::Sample& sample) {
    if (!active()) return;
    const double before = lastSample_;
    lastSample_ = now;
    ++view_.samples;
    if (sample.mapped) view_.mapped = true;
    if (!sample.beforeSmoothing) view_.beforeSmoothing = false;
    if (sample.raw.has) view_.raw = true;
    // The eyes back: usable samples without a gap since usableSince_
    const bool usable = sample.usable && !sample.valveHeld;
    if (!usable || now - before > kGapSec) usableSince_ = NAN;
    if (usable && !std::isfinite(usableSince_)) usableSince_ = now;
    // In a re-seat: the longest stretch without a usable sample (within the step)
    const int k = view_.step;
    if (usable && k >= 0 && steps()[k].kind == StepKind::Reseat) {
        const double from = std::isfinite(lastUsable_) ? std::max(lastUsable_, shownAt_[k]) : shownAt_[k];
        double& off = offSec_[steps()[k].round - 1];
        off = std::max(std::isfinite(off) ? off : 0.0, now - from);
    }
    if (usable) lastUsable_ = now;
    if (rows_.size() >= kMaxRows) return;
    Row row;
    row.at = now;
    row.wall = unixNow;
    row.sample = sample;
    // The step shown when it arrived (the newest one shown at or before it), counted from settleSec after it was shown
    for (int i = kStepCount - 1; i >= 0; --i) {
        if (std::isfinite(shownAt_[i]) && shownAt_[i] <= now) {
            row.step = i;
            break;
        }
    }
    if (row.step >= 0) row.counted = now >= shownAt_[row.step] + settleSec(steps()[row.step]);
    rows_.push_back(row);
}

void Session::setVideo(VideoState state, const std::string& reason) {
    view_.video = state;
    if (!reason.empty()) view_.videoReason = reason;
}

void Session::setVideoDir(const std::string& dir) {
    if (!dir.empty()) view_.videoDir = dir;
}

void Session::dismiss() {
    if (active()) return;
    view_ = View();
    rows_.clear();
    ended_ = false;
}

std::string recordJson(const View& view, const std::vector<Row>& rows, const double* shownAt, const double* shownUnix,
                       double startedAt, const Result& result) {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    const char* phases[] = {"idle", "running", "done", "stopped", "failed"};
    root.set("version", JsonValue::makeNumber(kRecordVersion, true));
    root.set("result", JsonValue::makeString(phases[static_cast<int>(view.phase)]));
    // (whole milliseconds: the JSON writer keeps 6 digits of other numbers)
    root.set("start_ms", JsonValue::makeNumber(std::llround(view.startUnix * 1000), true));
    root.set("end_ms", JsonValue::makeNumber(std::llround(view.endUnix * 1000), true));
    root.set("samples", JsonValue::makeNumber(view.samples, true));
    root.set("steps_done", JsonValue::makeNumber(view.stepsDone, true));
    root.set("rounds_done", JsonValue::makeNumber(view.roundsDone, true));
    root.set("timed_out", JsonValue::makeBool(view.timedOut));
    root.set("mapped", JsonValue::makeBool(result.mapped));
    root.set("values", JsonValue::makeString(view.beforeSmoothing ? "before smoothing" : "as sent (smoothed)"));
    root.set("raw", JsonValue::makeBool(view.raw));
    JsonValue plan;
    plan.type = JsonValue::Type::Object;
    plan.set("rounds", JsonValue::makeNumber(kRounds, true));
    plan.set("still_s", JsonValue::makeNumber(kStillSec));
    plan.set("dot_s", JsonValue::makeNumber(kDotSec));
    plan.set("reseat_min_s", JsonValue::makeNumber(kReseatMinSec));
    plan.set("settled_s", JsonValue::makeNumber(kSettledSec));
    plan.set("gap_s", JsonValue::makeNumber(kGapSec));
    plan.set("off_s", JsonValue::makeNumber(kOffSec));
    plan.set("no_off_s", JsonValue::makeNumber(kNoOffSec));
    plan.set("reseat_max_s", JsonValue::makeNumber(kReseatMaxSec));
    plan.set("min_run_s", JsonValue::makeNumber(kMinRunSec));
    plan.set("reseat", JsonValue::makeString(
                           "the dots come once reseat_min_s is over and the samples have been usable (both eyes open "
                           "enough, Valve's gaze not held) for settled_s without a gap of gap_s, after a stretch of "
                           "off_s without a usable sample (the headset off) or from no_off_s on without one; failed "
                           "after reseat_max_s"));
    plan.set("settle", JsonValue::makeString("a dot's or the still start's samples count from 30 % of it on, at least "
                                             "1 s and at most 2 s; a re-seat's never"));
    plan.set("min_samples", JsonValue::makeNumber(kMinSamples, true));
    plan.set("min_dots", JsonValue::makeNumber(kMinDots, true));
    plan.set("dot", JsonValue::makeString("fixed to the headset 0.9 m ahead at the step's yaw and pitch"));
    plan.set("valve", JsonValue::makeString("combined gaze as gaze_source valve works it out, before smoothing and "
                                            "the gaze scale; samples with both eyes open enough, not held"));
    plan.set("pupil", JsonValue::makeString("combined gaze as gaze_source pupil works it out, before smoothing and the "
                                            "gaze scale; also only from both pupils, not easing from another source"));
    if (result.guardSeen) {
        plan.set("guarded", JsonValue::makeString("combined gaze as gaze_guard sends it (Valve's with the guard's "
                                                  "offset), before smoothing and the gaze scale, on Valve's samples; "
                                                  "valve is then Valve's own, without the guard"));
    }
    plan.set("round", JsonValue::makeString("error: the median of its dots' angles to their dots; dx, dy: the mean of "
                                            "its dots' offsets from them; centre: its centre dot's median minus the "
                                            "still start's"));
    plan.set("overall", JsonValue::makeString("error: the mean of the rounds' errors; spread: the mean distance of the "
                                              "rounds' (dx, dy) from their mean; centre: the mean size of the rounds' "
                                              "centre moves"));
    root.set("plan", plan);
    JsonValue stepsOut;
    stepsOut.type = JsonValue::Type::Array;
    for (int i = 0; i < kStepCount; ++i) {
        const Step& step = steps()[i];
        JsonValue v;
        v.type = JsonValue::Type::Object;
        v.set("index", JsonValue::makeNumber(i, true));
        v.set("name", JsonValue::makeString(stepName(step)));
        v.set("round", JsonValue::makeNumber(step.round, true));
        v.set("place", JsonValue::makeNumber(step.place, true));
        const bool shown = shownAt != nullptr && std::isfinite(shownAt[i]);
        v.set("shown_s", number(shown ? shownAt[i] - startedAt : NAN));
        v.set("shown_ms", shown && shownUnix != nullptr && std::isfinite(shownUnix[i])
                              ? JsonValue::makeNumber(std::llround(shownUnix[i] * 1000), true)
                              : JsonValue::makeNull());
        // The eye server's time of its first sample (the eye video's valve.csv and glints.csv have the same clock)
        double first = NAN;
        for (const Row& row : rows) {
            if (row.step == i) {
                first = row.sample.time;
                break;
            }
        }
        v.set("first_sample_time", number(first));
        v.set("yaw_deg", JsonValue::makeNumber(step.yawDeg));
        v.set("pitch_deg", JsonValue::makeNumber(step.pitchDeg));
        if (step.kind != StepKind::Reseat) {
            v.set("valve", sourceJson(result.steps[i].valve));
            v.set("pupil", sourceJson(result.steps[i].pupil));
            if (result.guardSeen) v.set("guarded", sourceJson(result.steps[i].guarded));
        }
        stepsOut.items.push_back(v);
    }
    root.set("steps", stepsOut);
    JsonValue rounds;
    rounds.type = JsonValue::Type::Array;
    for (int r = 0; r < kRounds; ++r) {
        const RoundScore& rs = result.rounds[r];
        JsonValue v;
        v.type = JsonValue::Type::Object;
        v.set("round", JsonValue::makeNumber(r + 1, true));
        v.set("reseat_s", number(rs.reseatSec));
        v.set("off_s", number(rs.offSec));
        v.set("valve", roundJson(rs.valve));
        v.set("pupil", roundJson(rs.pupil));
        if (result.guardSeen) v.set("guarded", roundJson(rs.guarded));
        rounds.items.push_back(v);
    }
    root.set("rounds", rounds);
    JsonValue summary;
    summary.type = JsonValue::Type::Object;
    summary.set("valve", overallJson(result.valve));
    summary.set("pupil", overallJson(result.pupil));
    if (result.guardSeen) summary.set("guarded", overallJson(result.guarded));
    // Samples that came without their raw values or the gaze guard's (dropped on the way; null: none came at all)
    summary.set("missing_datagrams", gaze_measure::missingJson(result.missing));
    root.set("summary", summary);
    JsonValue video;
    video.type = JsonValue::Type::Object;
    const char* states[] = {"off", "asked", "on", "refused"};
    video.set("state", JsonValue::makeString(states[static_cast<int>(view.video)]));
    video.set("protocol", JsonValue::makeString(kVideoProtocol));
    video.set("reason", JsonValue::makeString(view.videoReason));
    video.set("session_dir", JsonValue::makeString(view.videoDir));
    video.set("align", JsonValue::makeString("samples.csv's sample_time is the eye server's, as valve.csv's sample_time "
                                             "and glints.csv's t_cam in the recording; each step's first_sample_time "
                                             "marks its start; samples.csv's cam_l_frame_ns / cam_r_frame_ns are "
                                             "glints.csv's t_mono_ns of the frame each sample read"));
    root.set("video", video);
    root.set("note", JsonValue::makeString("the dot is fixed to the display, so how a round's gaze sits on its dots "
                                           "changes with each re-seat; compare start_ms and end_ms with Valve's eye "
                                           "tracker log (logs.txt)"));
    return writeJson(root) + "\n";
}

std::string samplesCsv(const std::vector<Row>& rows, const double* shownAt) {
    std::string out =
        "panel_s,unix,step,step_name,round,step_s,counted,sample_time,mapped,usable,valve_held,pupil_state,"
        "pupil_blending,before_smoothing,valve_x,valve_y,pupil_x,pupil_y,openness_left,openness_right," +
        std::string(gaze_measure::kGuardColumns);
    out += gaze_measure::rawCsvHeader();
    out += '\n';
    out.reserve(out.size() + rows.size() * 360);
    char line[360];
    for (const Row& r : rows) {
        const gaze_measure::Sample& s = r.sample;
        const bool known = r.step >= 0 && r.step < kStepCount;
        const std::string name = known ? stepName(steps()[r.step]) : "";
        const int round = known ? steps()[r.step].round : -1;
        const double into = known && shownAt != nullptr ? r.at - shownAt[r.step] : NAN;
        std::snprintf(line, sizeof(line),
                      "%.4f,%.3f,%d,%s,%d,%.3f,%d,%.6f,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.4f,%.4f,", r.at, r.wall,
                      r.step, name.c_str(), round, into, r.counted ? 1 : 0, s.time, s.mapped ? 1 : 0, s.usable ? 1 : 0,
                      s.valveHeld ? 1 : 0, static_cast<int>(s.pupilState), s.pupilBlending ? 1 : 0,
                      s.beforeSmoothing ? 1 : 0, s.valve[0], s.valve[1], s.pupil[0], s.pupil[1], s.openness[0],
                      s.openness[1]);
        out += line;
        out += gaze_measure::guardCells(s);
        gaze_measure::appendRawCsv(out, s.raw);
        out += '\n';
    }
    return out;
}

std::string briefText(const UiText& t, const Result& result) {
    std::string out;
    if (result.valve.counts()) out = fill(t.measureBriefValve, oneDecimal(result.valve.error));
    if (result.pupil.counts()) {
        if (!out.empty()) out += t.measureBriefAnd;
        out += fill(t.measureBriefPupil, oneDecimal(result.pupil.error));
    }
    if (result.guarded.counts()) {
        if (!out.empty()) out += t.measureBriefAnd;
        out += fill(t.measureBriefGuarded, oneDecimal(result.guarded.error));
    }
    return out;
}

std::string spreadText(const UiText& t, const Result& result) {
    std::string out;
    if (result.valve.counts()) out = fill(t.measureBriefValve, oneDecimal(result.valve.spread));
    if (result.pupil.counts()) {
        if (!out.empty()) out += t.measureBriefAnd;
        out += fill(t.measureBriefPupil, oneDecimal(result.pupil.spread));
    }
    if (result.guarded.counts()) {
        if (!out.empty()) out += t.measureBriefAnd;
        out += fill(t.measureBriefGuarded, oneDecimal(result.guarded.spread));
    }
    return out;
}

}  // namespace reseat_check
