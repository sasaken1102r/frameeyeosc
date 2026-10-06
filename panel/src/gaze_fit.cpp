// The eye fit: targets, the fit and the session.
#include "gaze_fit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gaze_fit {

namespace {

constexpr Target kTargets[kPointCount] = {
    {Point::Center, "center", 0.0, 0.0},
    {Point::Up, "up", 0.0, kUpDownDeg},
    {Point::Down, "down", 0.0, -kUpDownDeg},
    {Point::Left, "left", -kSideDeg, 0.0},
    {Point::Right, "right", kSideDeg, 0.0},
    // The eyes-shut step shows its words straight ahead
    {Point::Closed, "closed", 0.0, 0.0},
};

/**
 * Round to a number of decimals.
 * @param value the value
 * @param decimals digits after the point
 * @return the rounded value
 */
double roundTo(double value, int decimals) {
    const double scale = std::pow(10.0, decimals);
    return std::round(value * scale) / scale;
}

/**
 * A zero point as written: within range, 3 decimals (the panel's stepper shows 3).
 * @param value the measured center
 * @return the setting
 */
double offsetSetting(double value) {
    return roundTo(std::clamp(value, -kOffsetLimit, kOffsetLimit), 3);
}

/**
 * A gain as written: within range, 2 decimals.
 * @param value the computed gain
 * @return the setting
 */
double gainSetting(double value) {
    return roundTo(std::clamp(value, kGainMin, kGainMax), 2);
}

/**
 * A capture by step.
 * @param points the captures
 * @param point which
 * @return the capture
 */
const Measured& at(const Measured points[kPointCount], Point point) {
    return points[static_cast<int>(point)];
}

/** The re-wear fit's steps. */
constexpr Point kTiltPoints[3] = {Point::Center, Point::Up, Point::Down};

/**
 * Whether the three captures a per-eye fit needs (straight ahead, left and right) all have each eye's x.
 * @param points the captures
 * @return true if they do
 */
bool haveEyeX(const Measured points[kPointCount]) {
    for (Point point : {Point::Center, Point::Left, Point::Right}) {
        if (!at(points, point).hasEyeX) return false;
    }
    return true;
}

/**
 * How far a point moved from the center in its own direction, after leveling with the tilt.
 * @param points the captures
 * @param point which (Up, Down, Left or Right)
 * @param rollDeg the tilt
 * @return the distance (negative: the wrong way)
 */
double movedFromCenter(const Measured points[kPointCount], Point point, double rollDeg) {
    const Measured& center = at(points, Point::Center);
    const Measured& m = at(points, point);
    double x = 0.0;
    double y = 0.0;
    level(m.x - center.x, m.y - center.y, rollDeg, x, y);
    switch (point) {
        case Point::Up: return y;
        case Point::Down: return -y;
        case Point::Left: return -x;
        case Point::Right: return x;
        default: return 0.0;
    }
}

/**
 * Whether the given points moved at least kMinMoveFraction of their target angle the right way.
 * @param points the captures
 * @param which the points to check, in order
 * @param count how many
 * @param rollDeg the tilt
 * @param failed the first that did not
 * @param detail its numbers (may be null)
 * @return true if all did
 */
bool pointsMoved(const Measured points[kPointCount], const Point* which, int count, double rollDeg, Point& failed,
                 FailureDetail* detail) {
    for (int i = 0; i < count; ++i) {
        const Point point = which[i];
        const double targetAngle = (point == Point::Up || point == Point::Down ? kUpDownDeg : kSideDeg) / kFullScaleDeg;
        const double moved = movedFromCenter(points, point, rollDeg);
        if (!(moved >= kMinMoveFraction * targetAngle)) {
            failed = point;
            if (detail != nullptr) {
                detail->eye = -1;
                detail->movedDeg = moved * kFullScaleDeg;
                detail->neededDeg = kMinMoveFraction * targetAngle * kFullScaleDeg;
            }
            return false;
        }
    }
    return true;
}

/**
 * How far an eye's own leveled x moved from the left dot to the right one.
 * @param points the captures (per-eye x set)
 * @param eye 0 = left, 1 = right
 * @param rollDeg the tilt
 * @return the distance
 */
double eyeSpan(const Measured points[kPointCount], int eye, double rollDeg) {
    const Measured& left = at(points, Point::Left);
    const Measured& right = at(points, Point::Right);
    double x = 0.0;
    double y = 0.0;
    level(right.xEye[eye] - left.xEye[eye], right.y - left.y, rollDeg, x, y);
    return x;
}

/**
 * Whether an eye's own sideways gaze moved far enough between the side dots.
 * @param points the captures (per-eye x set)
 * @param eye 0 = left, 1 = right
 * @param rollDeg the tilt
 * @param ipd the distance between the eyes (m)
 * @param detail on failure, which eye and how far it moved (may be null)
 * @return true if it did
 */
bool eyeMoved(const Measured points[kPointCount], int eye, double rollDeg, double ipd, FailureDetail* detail) {
    const double span = eyeSpan(points, eye, rollDeg);
    const double expectedSpan = eyeAngle(kSideDeg, eye, ipd) - eyeAngle(-kSideDeg, eye, ipd);
    if (span >= kMinMoveFraction * expectedSpan) return true;
    if (detail != nullptr) {
        detail->eye = eye;
        detail->movedDeg = span * kFullScaleDeg;
        detail->neededDeg = kMinMoveFraction * expectedSpan * kFullScaleDeg;
    }
    return false;
}

}  // namespace

const Target& target(Point point) {
    return kTargets[static_cast<int>(point)];
}

int pointCount(Mode mode) {
    switch (mode) {
        case Mode::Center: return 1;
        case Mode::Tilt: return 3;
        case Mode::Full: break;
    }
    return kPointCount;
}

Point pointAt(Mode mode, int index) {
    if (mode == Mode::Center) return Point::Center;
    if (mode == Mode::Tilt) return kTiltPoints[std::clamp(index, 0, 2)];
    return kTargets[std::clamp(index, 0, kPointCount - 1)].point;
}

int samplesNeeded(const Measured& measured) {
    if (measured.received <= 0) return kMinSamples;
    const int share = static_cast<int>(std::ceil(kMinUsableShare * measured.received - 1e-9));
    return std::min(kMinSamples, std::max(kMinSamplesFloor, share));
}

std::string tryText(Point point, int attempt, const Measured& measured, const Measured& center, const char* outcome) {
    char text[256];
    int n = 0;
    if (measured.received > 0) {
        n = std::snprintf(text, sizeof(text), "%s try %d: %d of %d samples usable", target(point).name, attempt,
                          measured.samples, measured.received);
        if (std::isfinite(measured.rateHz)) {
            n += std::snprintf(text + n, sizeof(text) - n, " at %.0f Hz", measured.rateHz);
        }
        n += std::snprintf(text + n, sizeof(text) - n, " (needs %d), ", samplesNeeded(measured));
    } else {
        n = std::snprintf(text, sizeof(text), "%s try %d: %d samples (min %d), ", target(point).name, attempt,
                          measured.samples, kMinSamples);
    }
    if (point == Point::Closed) {
        const auto limit = [&](int eye) { return kClosedShare * center.openness[eye]; };
        std::snprintf(text + n, sizeof(text) - n, "openness L %.3f R %.3f (below %.3f / %.3f) -> %s",
                      measured.openness[0], measured.openness[1], limit(0), limit(1), outcome);
    } else if (std::isfinite(measured.spread) && measured.samples > 0) {
        std::snprintf(text + n, sizeof(text) - n, "spread %.1f° (max %.1f°) -> %s", measured.spread * kFullScaleDeg,
                      kMaxSpread * kFullScaleDeg, outcome);
    } else {
        std::snprintf(text + n, sizeof(text) - n, "no gaze average -> %s", outcome);
    }
    return text;
}

bool usable(const Measured& measured) {
    return measured.samples >= samplesNeeded(measured) && std::isfinite(measured.x) && std::isfinite(measured.y) &&
           std::isfinite(measured.spread) && measured.spread <= kMaxSpread;
}

bool usableClosed(const Measured& closed, const Measured& center) {
    if (closed.samples < samplesNeeded(closed) || !closed.hasOpenness || !center.hasOpenness) return false;
    for (int eye = 0; eye < 2; ++eye) {
        if (!(closed.openness[eye] < kClosedShare * center.openness[eye])) return false;
    }
    return true;
}

double eyeAngle(double yawDeg, int eye, double ipd) {
    const double yaw = yawDeg * M_PI / 180.0;
    // The target seen from the eye: the left eye is at -ipd/2, so the target is ipd/2 further right of it
    const double side = kTargetDistanceM * std::sin(yaw) + (eye == 0 ? ipd / 2 : -ipd / 2);
    return std::atan2(side, kTargetDistanceM * std::cos(yaw)) * 180.0 / M_PI / kFullScaleDeg;
}

double rollFromUpDown(const Measured& up, const Measured& down) {
    // Tilted by θ, a move straight up along the headset leans the other way: dx = -sinθ·dy
    const double deg = std::atan2(-(up.x - down.x), up.y - down.y) * 180.0 / M_PI;
    return roundTo(std::clamp(deg, -kRollLimitDeg, kRollLimitDeg), 1);
}

double rollFromSides(const Measured& left, const Measured& right) {
    return std::atan2(right.y - left.y, right.x - left.x) * 180.0 / M_PI;
}

void level(double dx, double dy, double rollDeg, double& x, double& y) {
    const double roll = rollDeg * M_PI / 180.0;
    const double c = std::cos(roll);
    const double s = std::sin(roll);
    x = dx * c + dy * s;
    y = -dx * s + dy * c;
}

double eyeOffset(const Measured& center, int eye, double offsetY, double rollDeg, double gain, double ipd) {
    // frameeyeosc sends ((xEye - offset)·cosθ + (y - offsetY)·sinθ)·gain; straight ahead that has to be the eye's own
    // angle to the dot
    const double roll = rollDeg * M_PI / 180.0;
    return center.xEye[eye] + (center.y - offsetY) * std::tan(roll) - eyeAngle(0.0, eye, ipd) / (gain * std::cos(roll));
}

bool fitEyes(const Measured points[kPointCount], double ipd, Values& out, FailureDetail* detail) {
    if (!haveEyeX(points)) return true;  // an older frameeyeosc: nothing to fit, not a failure
    Values fitted = out;
    for (int eye = 0; eye < 2; ++eye) {
        if (!eyeMoved(points, eye, out.rollDeg, ipd, detail)) return false;
        // The leveled span sets the gain, and the center then lands on the eye's own angle
        const double expectedSpan = eyeAngle(kSideDeg, eye, ipd) - eyeAngle(-kSideDeg, eye, ipd);
        const double gain = gainSetting(expectedSpan / eyeSpan(points, eye, out.rollDeg));
        fitted.eyeGainX[eye] = gain;
        fitted.eyeOffsetX[eye] =
            offsetSetting(eyeOffset(at(points, Point::Center), eye, out.offsetY, out.rollDeg, gain, ipd));
    }
    fitted.hasEyeX = true;
    out = fitted;
    return true;
}

Values fitCenter(const Measured& center, const Values& current, double ipd) {
    Values values = current;
    values.offsetX = offsetSetting(center.x);
    values.offsetY = offsetSetting(center.y);
    if (current.hasEyeX && center.hasEyeX) {
        for (int eye = 0; eye < 2; ++eye) {
            values.eyeOffsetX[eye] =
                offsetSetting(eyeOffset(center, eye, values.offsetY, current.rollDeg, current.eyeGainX[eye], ipd));
        }
    }
    return values;
}

bool fitGaze(const Measured points[kPointCount], Values& out, Point& failed, FailureDetail* detail) {
    const double side = kSideDeg / kFullScaleDeg;
    const double upDown = kUpDownDeg / kFullScaleDeg;
    const double roll = rollFromUpDown(at(points, Point::Up), at(points, Point::Down));
    // How far each point moved from the center, leveled and counted in its own direction
    const Point checks[4] = {Point::Up, Point::Down, Point::Left, Point::Right};
    if (!pointsMoved(points, checks, 4, roll, failed, detail)) return false;
    const Measured& center = at(points, Point::Center);
    out.offsetX = offsetSetting(center.x);
    out.offsetY = offsetSetting(center.y);
    out.rollDeg = roll;
    const double left = movedFromCenter(points, Point::Left, roll);
    const double right = movedFromCenter(points, Point::Right, roll);
    out.gainX = gainSetting(2.0 * side / (left + right));
    out.gainUp = gainSetting(upDown / movedFromCenter(points, Point::Up, roll));
    out.gainDown = gainSetting(upDown / movedFromCenter(points, Point::Down, roll));
    return true;
}

bool fitTilt(const Measured points[kPointCount], const Values& current, double ipd, Values& out, Point& failed,
             FailureDetail* detail) {
    const double roll = rollFromUpDown(at(points, Point::Up), at(points, Point::Down));
    const Point checks[2] = {Point::Up, Point::Down};
    if (!pointsMoved(points, checks, 2, roll, failed, detail)) return false;
    Values values = current;
    const Measured& center = at(points, Point::Center);
    values.offsetX = offsetSetting(center.x);
    values.offsetY = offsetSetting(center.y);
    values.rollDeg = roll;
    // Each eye's own zero point for the new tilt, keeping its gain
    if (current.hasEyeX && center.hasEyeX) {
        for (int eye = 0; eye < 2; ++eye) {
            values.eyeOffsetX[eye] =
                offsetSetting(eyeOffset(center, eye, values.offsetY, roll, current.eyeGainX[eye], ipd));
        }
    }
    out = values;
    return true;
}

bool fitLids(const Measured points[kPointCount], Values& out, FailureDetail* detail) {
    const Measured& closed = at(points, Point::Closed);
    const Measured* open[3] = {&at(points, Point::Up), &at(points, Point::Center), &at(points, Point::Down)};
    const Point openPoints[3] = {Point::Up, Point::Center, Point::Down};
    for (int eye = 0; eye < 2; ++eye) {
        for (int i = 0; i < 3; ++i) {
            const Measured* m = open[i];
            if (!m->hasOpenness || !(m->openness[eye] >= closed.openness[eye] + kMinLidRange)) {
                if (detail != nullptr) {
                    detail->eye = eye;
                    detail->lidPoint = openPoints[i];
                    detail->lidOpen = m->hasOpenness ? m->openness[eye] : NAN;
                    detail->lidClosed = closed.openness[eye];
                }
                return false;
            }
        }
    }
    for (int eye = 0; eye < 2; ++eye) {
        out.lidClosed[eye] = roundTo(closed.openness[eye], 3);
        out.lidUp[eye] = roundTo(open[0]->openness[eye], 3);
        out.lidOpen[eye] = roundTo(open[1]->openness[eye], 3);
        out.lidDown[eye] = roundTo(open[2]->openness[eye], 3);
    }
    out.hasLids = true;
    return true;
}

void Session::start(Mode mode, const Values& current, double now, double ipd) {
    *this = Session();
    // No waiting for the dashboard: the first dot shows right away (nearer than the dashboard, so over it)
    phase_ = Phase::Settling;
    phaseAt_ = now;
    mode_ = mode;
    current_ = current;
    ipd_ = ipd;
}

void Session::cancel(Failure why) {
    if (active()) fail(why);
}

bool Session::active() const {
    return phase_ == Phase::Settling || phase_ == Phase::Capturing || phase_ == Phase::Reopen;
}

void Session::fail(Failure failure) {
    phase_ = Phase::Failed;
    failure_ = failure;
    failedPoint_ = point();
}

void Session::failMovement(Point point) {
    for (int i = 0; i < pointCount(mode_); ++i) {
        if (pointAt(mode_, i) == point) index_ = i;
    }
    fail(Failure::NoMovement);
}

void Session::writeFailed() {
    // After the final write too: the result is then not in effect
    if (active() || phase_ == Phase::Done) fail(Failure::WriteFailed);
}

void Session::captureSent(long long id, double now) {
    if (phase_ != Phase::Capturing || !requested_) return;
    requested_ = false;
    captureId_ = id;
    phaseAt_ = now;
}

void Session::next(double now, Actions& actions) {
    if (point() == Point::Closed) {
        // Nothing more to measure; "open your eyes" first, then the result
        phase_ = Phase::Reopen;
        phaseAt_ = now;
        return;
    }
    previousPoint_ = point();
    ++index_;
    attempt_ = 1;
    if (index_ >= pointCount(mode_)) {
        finish(actions);
        return;
    }
    // Before asking to close the eyes, make sure the gaze part worked
    if (point() == Point::Closed) {
        Values gaze = current_;
        Point failed = Point::Center;
        if (!fitGaze(measured_, gaze, failed, &detail_)) {
            failMovement(failed);
            return;
        }
        if (!fitEyes(measured_, ipd_, gaze, &detail_)) {
            failMovement(Point::Right);
            return;
        }
    }
    phase_ = Phase::Settling;
    phaseAt_ = now;
}

void Session::finish(Actions& actions) {
    // The tilt goes to the log with the last try's numbers
    const auto note = [&actions](const char* line) {
        if (!actions.log.empty()) actions.log += '\n';
        actions.log += line;
    };
    char text[128];
    Point failed = Point::Center;
    if (mode_ == Mode::Center) {
        result_ = fitCenter(measured_[static_cast<int>(Point::Center)], current_, ipd_);
    } else if (mode_ == Mode::Tilt) {
        std::snprintf(text, sizeof(text), "tilt %+.1f° from up/down (was %+.1f°)",
                      rollFromUpDown(at(measured_, Point::Up), at(measured_, Point::Down)), current_.rollDeg);
        note(text);
        if (!fitTilt(measured_, current_, ipd_, result_, failed, &detail_)) {
            failMovement(failed);
            return;
        }
    } else {
        // Both ways of seeing the tilt, to tell how well they agree (only the up / down one is used)
        std::snprintf(text, sizeof(text), "tilt %+.1f° from up/down, %+.1f° from the sides",
                      rollFromUpDown(at(measured_, Point::Up), at(measured_, Point::Down)),
                      rollFromSides(at(measured_, Point::Left), at(measured_, Point::Right)));
        note(text);
        result_ = current_;
        if (!fitGaze(measured_, result_, failed, &detail_)) {
            failMovement(failed);
            return;
        }
        // Each eye's own sideways fit replaces the one before; without per-eye data there is none
        result_.hasEyeX = false;
        if (!fitEyes(measured_, ipd_, result_, &detail_)) {
            failMovement(Point::Right);
            return;
        }
        if (!fitLids(measured_, result_, &detail_)) {
            fail(Failure::NoLidRange);
            return;
        }
    }
    phase_ = Phase::Done;
    actions.writeValues = true;
    actions.values = result_;
}

Actions Session::tick(double now, const Dashboard& dashboard, const EyeStatus& status) {
    Actions actions;
    if (!active()) return actions;
    // The dashboard open or closed, the targets show over it. Closing it goes on; opening it while the fit runs
    // without it stops it (the way to stop a fit started with it closed, and the eyes would be on it). With it open,
    // another page (not this panel's) stops it too, once that has lasted kAwaySec
    const bool opened = dashboardSeen_ && !dashboardWasOpen_ && dashboard.open;
    dashboardSeen_ = true;
    dashboardWasOpen_ = dashboard.open;
    if (!dashboard.open || dashboard.panelShown) {
        awaySince_ = -1.0;
    } else if (awaySince_ < 0.0) {
        awaySince_ = now;
    }
    if (opened) {
        fail(Failure::DashboardOpened);
    } else if (!status.running) {
        fail(Failure::NotRunning);
    } else if (awaySince_ >= 0.0 && now - awaySince_ >= kAwaySec) {
        fail(Failure::Left);
    }
    if (phase_ == Phase::Settling && now - phaseAt_ >= settleSec()) {
        phase_ = Phase::Capturing;
        phaseAt_ = now;
        requested_ = true;
        captureId_ = 0;
        actions.writeCapture = true;
        actions.target = target(point()).name;
        actions.captureSec = captureSec();
        actions.skipSec = point() == Point::Closed ? kClosedSkipSec : kCaptureSkipSec;
    } else if (phase_ == Phase::Capturing && !requested_) {
        const GazeCaptureStatus& capture = status.capture;
        const bool ours = captureId_ != 0 && capture.present && capture.id == captureId_;
        if (ours && capture.done) {
            Measured measured;
            measured.samples = capture.samples;
            measured.received = capture.received;
            measured.rateHz = capture.rate;
            if (capture.hasAverage) {
                measured.x = capture.x;
                measured.y = capture.y;
                measured.spread = capture.spread;
            }
            measured.hasEyeX = capture.hasEyeX;
            measured.xEye[0] = capture.xEye[0];
            measured.xEye[1] = capture.xEye[1];
            measured.hasOpenness = capture.hasOpenness;
            measured.openness[0] = capture.openness[0];
            measured.openness[1] = capture.openness[1];
            const bool closedStep = point() == Point::Closed;
            // The full fit needs each gaze point's openness too, for the lid fit
            const Measured& center = measured_[static_cast<int>(Point::Center)];
            const bool ok = closedStep ? usableClosed(measured, center)
                                       : usable(measured) && (mode_ != Mode::Full || measured.hasOpenness);
            const bool last = attempt_ >= kMaxAttempts;
            actions.log = tryText(point(), attempt_, measured, center, ok ? "ok" : (last ? "failed" : "again"));
            if (ok) {
                measured_[static_cast<int>(point())] = measured;
                next(now, actions);
            } else if (++attempt_ > kMaxAttempts) {
                attempt_ = kMaxAttempts;
                detail_.tries = kMaxAttempts;
                detail_.last = measured;
                if (closedStep) {
                    for (int eye = 0; eye < 2; ++eye) detail_.closedBelow[eye] = kClosedShare * center.openness[eye];
                }
                fail(closedStep ? Failure::NotClosed : Failure::Unsteady);
            } else {
                previousPoint_ = point();
                phase_ = Phase::Settling;
                phaseAt_ = now;
            }
        } else if (now - phaseAt_ >= kResultTimeoutSec) {
            fail(Failure::NoResult);
        }
    } else if (phase_ == Phase::Reopen && now - phaseAt_ >= kReopenSec) {
        finish(actions);
    }

    if (phase_ == Phase::Settling || phase_ == Phase::Capturing || phase_ == Phase::Reopen) {
        const bool closedStep = point() == Point::Closed;
        const double settle = settleSec();
        const double capture = captureSec();
        // Seconds left in the step, the capture counted from when it was asked for (frameeyeosc starts it within a
        // tenth of a second), so the ring runs down evenly
        double left = capture;
        if (phase_ == Phase::Settling) {
            left += std::max(0.0, settle - (now - phaseAt_));
        } else if (phase_ == Phase::Reopen) {
            left = 0.0;
        } else {
            left = std::max(0.0, capture - (now - phaseAt_));
        }
        actions.showTarget = true;
        actions.progress = std::clamp(left / (settle + capture), 0.0, 1.0);
        // Under the dot, the seconds being measured (2, 1); nothing while it glides over and the eyes find it
        actions.seconds = phase_ == Phase::Capturing ? std::max(1, static_cast<int>(std::ceil(left - 1e-9))) : 0;
        if (closedStep) {
            actions.style = phase_ == Phase::Settling    ? TargetStyle::CloseEyes
                            : phase_ == Phase::Capturing ? TargetStyle::KeepClosed
                                                         : TargetStyle::OpenEyes;
            // Counting down to closing the eyes; nothing to count while they are shut
            actions.seconds = phase_ == Phase::Settling
                                  ? std::max(1, static_cast<int>(std::ceil(settle - (now - phaseAt_) - 1e-9)))
                                  : 0;
            // The ring runs the whole way round for the 3, 2, 1, and again while the eyes are shut (not half for each)
            actions.progress = phase_ == Phase::Settling
                                   ? std::clamp((settle - (now - phaseAt_)) / settle, 0.0, 1.0)
                                   : std::clamp(left / capture, 0.0, 1.0);
        }
        // Glide from the previous target to this one at the start of a step (smoothstep easing)
        const Target& from = target(previousPoint_);
        const Target& to = target(point());
        double t = phase_ == Phase::Settling ? std::clamp((now - phaseAt_) / kMoveSec, 0.0, 1.0) : 1.0;
        actions.arrived = t >= 1.0 || previousPoint_ == point();
        t = t * t * (3.0 - 2.0 * t);
        actions.yawDeg = from.yawDeg + (to.yawDeg - from.yawDeg) * t;
        actions.pitchDeg = from.pitchDeg + (to.pitchDeg - from.pitchDeg) * t;
    }
    return actions;
}

View Session::view() const {
    View view;
    view.phase = phase_;
    view.mode = mode_;
    view.index = index_;
    view.count = pointCount(mode_);
    view.point = phase_ == Phase::Failed ? failedPoint_ : point();
    view.attempt = attempt_;
    view.failure = failure_;
    view.detail = detail_;
    view.values = result_;
    return view;
}

}  // namespace gaze_fit
