// "Headset slip test": the steps and their timing, the scores and the record's files.
#include "slip_check.h"

#include "json.h"

#include <algorithm>
#include <cstdio>

namespace slip_check {

namespace {

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
 * A push step (5 s; the dot straight ahead).
 * @param dir which way
 * @return the step
 */
constexpr Step push(Dir dir) { return {StepKind::Push, dir, 5.0, 0.0, 0.0}; }
/** The let-go-and-push-back step after each push... */
constexpr Step kRelease = {StepKind::Release, Dir::None, 5.0, 0.0, 0.0};
/** ...and the re-seat step after that. */
constexpr Step kReseat = {StepKind::Reseat, Dir::None, 3.0, 0.0, 0.0};

/**
 * A template dot (2.5 s): a place of known gaze for the eye video's per-gaze glint templates.
 * @param yaw degrees right
 * @param pitch degrees up
 * @return the step
 */
constexpr Step dot(double yaw, double pitch) { return {StepKind::Template, Dir::None, 2.5, yaw, pitch}; }

/**
 * One source's move during a hold: its median minus the mean of the still parts' medians around it (those that
 * count), NaN if the hold or both of them don't count.
 * @param hold the hold's numbers
 * @param before the step before it
 * @param after the step after it
 * @param dx where to write the sideways move
 * @param dy ...and the up/down one
 * @return true if it counts
 */
bool moveOf(const SourceStep& hold, const SourceStep& before, const SourceStep& after, double& dx, double& dy) {
    if (!hold.counts() || (!before.counts() && !after.counts())) return false;
    double bx = 0.0;
    double by = 0.0;
    int n = 0;
    for (const SourceStep* s : {&before, &after}) {
        if (!s->counts()) continue;
        bx += s->x;
        by += s->y;
        ++n;
    }
    dx = hold.x - bx / n;
    dy = hold.y - by / n;
    return true;
}

/**
 * Add one round's move to a kind of hold (summed now, averaged by finishMove).
 * @param move the kind's move
 * @param dx the round's sideways move
 * @param dy ...up/down
 */
void addRound(Move& move, double dx, double dy) {
    if (move.rounds == 0) move.dx = move.dy = 0.0;
    move.dx += dx;
    move.dy += dy;
    ++move.rounds;
}

/**
 * Average a kind of hold's rounds.
 * @param move the kind's move
 */
void finishMove(Move& move) {
    if (move.rounds == 0) return;
    move.dx /= move.rounds;
    move.dy /= move.rounds;
}

/**
 * A move as JSON.
 * @param move the move
 * @return the object
 */
JsonValue moveJson(const Move& move) {
    JsonValue v;
    v.type = JsonValue::Type::Object;
    v.set("rounds", JsonValue::makeNumber(move.rounds, true));
    v.set("dx_deg", number(move.dx));
    v.set("dy_deg", number(move.dy));
    v.set("size_deg", number(move.counts() ? move.size() : NAN));
    return v;
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

}  // namespace

// Still; the nine template dots (the gaze error check's first nine places: centre, sides, corners); the four pushes
// twice, each let go of and pushed back, then re-seated; wide, normal, narrow, normal; the dot up, down, left, right
// (the gaze error check's places); still
const Step kSteps[kStepCount] = {
    {StepKind::Still, Dir::None, 10.0, 0.0, 0.0},
    dot(0, 0), dot(-20, 0), dot(20, 0), dot(0, 15), dot(0, -15), dot(-20, 15), dot(20, -15), dot(20, 15), dot(-20, -15),
    push(Dir::Up), kRelease, kReseat, push(Dir::Down), kRelease, kReseat,
    push(Dir::Left), kRelease, kReseat, push(Dir::Right), kRelease, kReseat,
    push(Dir::Up), kRelease, kReseat, push(Dir::Down), kRelease, kReseat,
    push(Dir::Left), kRelease, kReseat, push(Dir::Right), kRelease, kReseat,
    {StepKind::Widen, Dir::None, 5.0, 0.0, 0.0},
    {StepKind::Normal, Dir::None, 5.0, 0.0, 0.0},
    {StepKind::Squint, Dir::None, 5.0, 0.0, 0.0},
    {StepKind::Normal, Dir::None, 5.0, 0.0, 0.0},
    {StepKind::Look, Dir::Up, 3.0, 0.0, 15.0},
    {StepKind::Look, Dir::Down, 3.0, 0.0, -15.0},
    {StepKind::Look, Dir::Left, 3.0, -20.0, 0.0},
    {StepKind::Look, Dir::Right, 3.0, 20.0, 0.0},
    {StepKind::Still, Dir::None, 10.0, 0.0, 0.0},
};

double stepStart(int index) {
    double at = 0.0;
    for (int i = 0; i < index && i < kStepCount; ++i) at += kSteps[i].seconds;
    return at;
}

double settleSec(const Step& step) {
    return std::clamp(0.3 * step.seconds, 1.0, 2.0);
}

bool straightAhead(const Step& step) {
    return step.kind != StepKind::Template && step.kind != StepKind::Look;
}

std::string stepName(const Step& step) {
    const char* dirs[] = {"", "up", "down", "left", "right"};
    const std::string dir = dirs[static_cast<int>(step.dir)];
    switch (step.kind) {
        case StepKind::Still: return "still";
        case StepKind::Template: {
            char name[32];
            std::snprintf(name, sizeof(name), "dot_%.0f_%.0f", step.yawDeg, step.pitchDeg);
            return name;
        }
        case StepKind::Push: return "push_" + dir;
        case StepKind::Release: return "release";
        case StepKind::Reseat: return "reseat";
        case StepKind::Widen: return "widen";
        case StepKind::Normal: return "normal";
        case StepKind::Squint: return "squint";
        case StepKind::Look: return "look_" + dir;
    }
    return "still";
}

std::string instruction(const UiText& t, const Step& step) {
    const char* dirs[] = {"", t.slipDirUp, t.slipDirDown, t.slipDirLeft, t.slipDirRight};
    switch (step.kind) {
        case StepKind::Still: return t.slipStill;
        case StepKind::Push: return fill(t.slipPushFormat, dirs[static_cast<int>(step.dir)]);
        case StepKind::Release: return t.slipRelease;
        case StepKind::Reseat: return t.slipReseat;
        case StepKind::Template: return t.slipLook;
        case StepKind::Widen: return t.slipWiden;
        case StepKind::Normal: return t.slipNormal;
        case StepKind::Squint: return t.slipSquint;
        case StepKind::Look: return t.slipLook;
    }
    return t.slipStill;
}

int dirIndex(Dir dir) {
    switch (dir) {
        case Dir::Up: return 0;
        case Dir::Down: return 1;
        case Dir::Left: return 2;
        case Dir::Right: return 3;
        case Dir::None: return -1;
    }
    return -1;
}

Result score(const std::vector<Row>& rows) {
    Result result;
    std::vector<double> vx[kStepCount], vy[kStepCount], px[kStepCount], py[kStepCount], gx[kStepCount], gy[kStepCount];
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
    // Each hold against the first still step (version 2), and against the steps right before and after it (version 1)
    const StepScore& still = result.steps[0];
    const SourceStep none;
    for (int i = 1; i + 1 < kStepCount; ++i) {
        const Step& step = kSteps[i];
        MoveScore* into = nullptr;
        MoveScore* around = nullptr;
        if (step.kind == StepKind::Push) {
            into = &result.pushes[dirIndex(step.dir)];
            around = &result.pushesAround[dirIndex(step.dir)];
        }
        if (step.kind == StepKind::Widen) {
            into = &result.widen;
            around = &result.widenAround;
        }
        if (step.kind == StepKind::Squint) {
            into = &result.squint;
            around = &result.squintAround;
        }
        if (into == nullptr) continue;
        // (only neighbours with the dot straight ahead: the first push follows a template dot)
        const StepScore blank;
        const StepScore& before = straightAhead(kSteps[i - 1]) ? result.steps[i - 1] : blank;
        const StepScore& hold = result.steps[i];
        const StepScore& after = straightAhead(kSteps[i + 1]) ? result.steps[i + 1] : blank;
        double dx = 0.0;
        double dy = 0.0;
        if (moveOf(hold.valve, still.valve, none, dx, dy)) addRound(into->valve, dx, dy);
        if (moveOf(hold.pupil, still.pupil, none, dx, dy)) addRound(into->pupil, dx, dy);
        if (moveOf(hold.valve, before.valve, after.valve, dx, dy)) addRound(around->valve, dx, dy);
        if (moveOf(hold.pupil, before.pupil, after.pupil, dx, dy)) addRound(around->pupil, dx, dy);
        if (moveOf(hold.guarded, still.guarded, none, dx, dy)) addRound(into->guarded, dx, dy);
        if (moveOf(hold.guarded, before.guarded, after.guarded, dx, dy)) addRound(around->guarded, dx, dy);
    }
    double valveSum = 0.0;
    double pupilSum = 0.0;
    double guardedSum = 0.0;
    for (MoveScore& m : result.pushes) {
        finishMove(m.valve);
        finishMove(m.pupil);
        finishMove(m.guarded);
        if (m.valve.counts()) {
            valveSum += m.valve.size();
            ++result.valveDirs;
        }
        if (m.pupil.counts()) {
            pupilSum += m.pupil.size();
            ++result.pupilDirs;
        }
        if (m.guarded.counts()) {
            guardedSum += m.guarded.size();
            ++result.guardedDirs;
        }
    }
    for (MoveScore& m : result.pushesAround) {
        finishMove(m.valve);
        finishMove(m.pupil);
        finishMove(m.guarded);
    }
    for (MoveScore* m : {&result.widen, &result.squint, &result.widenAround, &result.squintAround}) {
        finishMove(m->valve);
        finishMove(m->pupil);
        finishMove(m->guarded);
    }
    if (result.valveDirs > 0) result.valveMean = valveSum / result.valveDirs;
    if (result.pupilDirs > 0) result.pupilMean = pupilSum / result.pupilDirs;
    if (result.guardedDirs > 0) result.guardedMean = guardedSum / result.guardedDirs;
    return result;
}

void Session::start(double now, double unixNow) {
    view_ = View();
    view_.phase = Phase::Running;
    view_.startUnix = unixNow;
    rows_.clear();
    rows_.reserve(13000);
    start_ = now;
    lastSample_ = now;
    for (double& at : shownAt_) at = NAN;
    for (double& at : shownUnix_) at = NAN;
    ended_ = false;
}

void Session::end(Phase phase, double unixNow) {
    view_.phase = phase;
    view_.endUnix = unixNow;
    view_.result = score(rows_);
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
    // No samples for a while: frameeyeosc stopped, or the eyes aren't tracked
    if (now - lastSample_ > kNoDataSec) {
        end(Phase::Failed, unixNow);
        ended_ = false;
        tick.ended = true;
        return tick;
    }
    const double into = now - start_;
    if (into >= kRunSec) {
        view_.stepsDone = kStepCount;
        end(Phase::Done, unixNow);
        ended_ = false;
        tick.ended = true;
        return tick;
    }
    int step = 0;
    while (step + 1 < kStepCount && into >= stepStart(step + 1)) ++step;
    if (step != view_.step) {
        // The step starts now: its samples are timed from when it is shown
        view_.stepsDone = std::max(0, step);
        view_.step = step;
        shownAt_[step] = now;
        shownUnix_[step] = unixNow;
        tick.changed = true;
    }
    const Step& s = kSteps[step];
    const double left = std::max(0.0, stepStart(step) + s.seconds - into);
    view_.stepLeft = left;
    tick.showTarget = true;
    tick.step = step;
    tick.yawDeg = s.yawDeg;
    tick.pitchDeg = s.pitchDeg;
    tick.progress = std::clamp(left / s.seconds, 0.0, 1.0);
    tick.seconds = static_cast<int>(std::ceil(left - 1e-9));
    return tick;
}

void Session::add(double now, double unixNow, const gaze_measure::Sample& sample) {
    if (!active()) return;
    lastSample_ = now;
    ++view_.samples;
    if (sample.mapped) view_.mapped = true;
    if (!sample.beforeSmoothing) view_.beforeSmoothing = false;
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
    if (row.step >= 0) row.counted = now >= shownAt_[row.step] + settleSec(kSteps[row.step]);
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
    root.set("mapped", JsonValue::makeBool(result.mapped));
    root.set("values", JsonValue::makeString(view.beforeSmoothing ? "before smoothing" : "as sent (smoothed)"));
    JsonValue plan;
    plan.type = JsonValue::Type::Object;
    plan.set("run_s", JsonValue::makeNumber(kRunSec));
    plan.set("settle", JsonValue::makeString("a step's samples count from 30 % of it on, at least 1 s and at most 2 s"));
    plan.set("min_samples", JsonValue::makeNumber(kMinSamples, true));
    plan.set("dot", JsonValue::makeString("fixed to the headset 0.9 m ahead at the step's yaw and pitch"));
    plan.set("valve", JsonValue::makeString("combined gaze as gaze_source valve works it out, before smoothing and "
                                            "the gaze scale; samples with both eyes open enough, not held"));
    plan.set("pupil", JsonValue::makeString("combined gaze as gaze_source pupil works it out, before smoothing and the "
                                            "gaze scale; also only from both pupils, not easing from another source"));
    plan.set("move", JsonValue::makeString("a hold's median minus the first still step's, averaged over its rounds; "
                                           "each straight-ahead step's off_still the same"));
    plan.set("move_around", JsonValue::makeString("(version 1's) a hold's median minus the mean of the medians of the "
                                                  "steps right before and after it (those that count)"));
    if (result.guardSeen) {
        plan.set("guarded", JsonValue::makeString("combined gaze as gaze_guard sends it (Valve's with the guard's "
                                                  "offset), before smoothing and the gaze scale, on Valve's samples; "
                                                  "valve is then Valve's own, without the guard"));
    }
    root.set("plan", plan);
    JsonValue steps;
    steps.type = JsonValue::Type::Array;
    for (int i = 0; i < kStepCount; ++i) {
        const Step& step = kSteps[i];
        JsonValue v;
        v.type = JsonValue::Type::Object;
        v.set("index", JsonValue::makeNumber(i, true));
        v.set("name", JsonValue::makeString(stepName(step)));
        v.set("seconds", JsonValue::makeNumber(step.seconds));
        v.set("planned_s", JsonValue::makeNumber(stepStart(i)));
        const bool shown = shownAt != nullptr && std::isfinite(shownAt[i]);
        v.set("shown_s", number(shown ? shownAt[i] - startedAt : NAN));
        v.set("shown_ms", shown && shownUnix != nullptr && std::isfinite(shownUnix[i])
                              ? JsonValue::makeNumber(std::llround(shownUnix[i] * 1000), true)
                              : JsonValue::makeNull());
        // The eye server's time of its first sample (the eye video's valve.csv has the same sample_time)
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
        v.set("valve", sourceJson(result.steps[i].valve));
        v.set("pupil", sourceJson(result.steps[i].pupil));
        if (result.guardSeen) v.set("guarded", sourceJson(result.steps[i].guarded));
        // A straight-ahead step against the first still step (the same true gaze)
        if (straightAhead(step) && i > 0) {
            const auto offStill = [&](const SourceStep& hold, const SourceStep& still) {
                Move m;
                double dx = 0.0;
                double dy = 0.0;
                if (moveOf(hold, still, SourceStep(), dx, dy)) addRound(m, dx, dy);
                return moveJson(m);
            };
            JsonValue off;
            off.type = JsonValue::Type::Object;
            off.set("valve", offStill(result.steps[i].valve, result.steps[0].valve));
            off.set("pupil", offStill(result.steps[i].pupil, result.steps[0].pupil));
            if (result.guardSeen) off.set("guarded", offStill(result.steps[i].guarded, result.steps[0].guarded));
            v.set("off_still", off);
        }
        steps.items.push_back(v);
    }
    root.set("steps", steps);
    JsonValue moves;
    moves.type = JsonValue::Type::Object;
    const char* dirs[kDirCount] = {"push_up", "push_down", "push_left", "push_right"};
    const auto both = [&result](const MoveScore& m) {
        JsonValue v;
        v.type = JsonValue::Type::Object;
        v.set("valve", moveJson(m.valve));
        v.set("pupil", moveJson(m.pupil));
        if (result.guardSeen) v.set("guarded", moveJson(m.guarded));
        return v;
    };
    for (int d = 0; d < kDirCount; ++d) moves.set(dirs[d], both(result.pushes[d]));
    moves.set("widen", both(result.widen));
    moves.set("squint", both(result.squint));
    root.set("moves", moves);
    JsonValue around;
    around.type = JsonValue::Type::Object;
    for (int d = 0; d < kDirCount; ++d) around.set(dirs[d], both(result.pushesAround[d]));
    around.set("widen", both(result.widenAround));
    around.set("squint", both(result.squintAround));
    root.set("moves_around", around);
    JsonValue summary;
    summary.type = JsonValue::Type::Object;
    summary.set("valve_mean_deg", number(result.valveMean));
    summary.set("valve_directions", JsonValue::makeNumber(result.valveDirs, true));
    summary.set("pupil_mean_deg", number(result.pupilMean));
    summary.set("pupil_directions", JsonValue::makeNumber(result.pupilDirs, true));
    if (result.guardSeen) {
        summary.set("guarded_mean_deg", number(result.guardedMean));
        summary.set("guarded_directions", JsonValue::makeNumber(result.guardedDirs, true));
    }
    root.set("summary", summary);
    JsonValue video;
    video.type = JsonValue::Type::Object;
    const char* states[] = {"off", "asked", "on", "refused"};
    video.set("state", JsonValue::makeString(states[static_cast<int>(view.video)]));
    video.set("protocol", JsonValue::makeString(kVideoProtocol));
    video.set("reason", JsonValue::makeString(view.videoReason));
    video.set("session_dir", JsonValue::makeString(view.videoDir));
    video.set("align", JsonValue::makeString("samples.csv's sample_time is the eye server's, as valve.csv's sample_time "
                                             "in the recording; each step's first_sample_time marks its start"));
    root.set("video", video);
    root.set("note", JsonValue::makeString("the dot is fixed to the display, so a move while the headset is pushed is "
                                           "slip error; compare start_ms and end_ms with Valve's eye tracker log "
                                           "(logs.txt)"));
    return writeJson(root) + "\n";
}

std::string samplesCsv(const std::vector<Row>& rows, const double* shownAt) {
    std::string out =
        "panel_s,unix,step,step_name,step_s,counted,sample_time,mapped,usable,valve_held,pupil_state,pupil_blending,"
        "before_smoothing,valve_x,valve_y,pupil_x,pupil_y,openness_left,openness_right," +
        std::string(gaze_measure::kGuardColumns);
    out += gaze_measure::rawCsvHeader();
    out += '\n';
    char line[360];
    for (const Row& r : rows) {
        const gaze_measure::Sample& s = r.sample;
        const bool known = r.step >= 0 && r.step < kStepCount;
        const std::string name = known ? stepName(kSteps[r.step]) : "";
        const double into = known && shownAt != nullptr ? r.at - shownAt[r.step] : NAN;
        std::snprintf(line, sizeof(line),
                      "%.4f,%.3f,%d,%s,%.3f,%d,%.6f,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.4f,%.4f,", r.at, r.wall,
                      r.step, name.c_str(), into, r.counted ? 1 : 0, s.time, s.mapped ? 1 : 0, s.usable ? 1 : 0,
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
    if (result.valveDirs > 0) out = fill(t.measureBriefValve, oneDecimal(result.valveMean));
    if (result.pupilDirs > 0) {
        if (!out.empty()) out += t.measureBriefAnd;
        out += fill(t.measureBriefPupil, oneDecimal(result.pupilMean));
    }
    if (result.guardedDirs > 0) {
        if (!out.empty()) out += t.measureBriefAnd;
        out += fill(t.measureBriefGuarded, oneDecimal(result.guardedMean));
    }
    return out;
}

}  // namespace slip_check
