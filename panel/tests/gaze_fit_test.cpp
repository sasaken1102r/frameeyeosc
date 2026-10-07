// Tests for the eye fit (gaze_fit.cpp): the fit itself and the session, without OpenVR or files.
// Built with the panel as gaze-fit-test; exits non-zero on failure.
#include "gaze_fit.h"

#include <cmath>
#include <cstdio>
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
 * Whether two numbers are within 1e-9 of each other.
 * @param a one
 * @param b the other
 * @return true if equal enough
 */
bool near(double a, double b) {
    return std::fabs(a - b) < 1e-9;
}

using namespace gaze_fit;

const double kSide = kSideDeg / kFullScaleDeg;
const double kUpDown = kUpDownDeg / kFullScaleDeg;

/** The dashboard closed; open on this panel (a fit started from it); open on another overlay's page. */
const Dashboard kClosed {};
const Dashboard kOpen {true, true};
const Dashboard kElsewhere {true, false};

/**
 * A steady capture with the eyes open.
 * @param x average x
 * @param y average y
 * @param left left eye openness
 * @param right right eye openness
 * @return the capture
 */
Measured steady(double x, double y, double left = 0.9, double right = 0.8) {
    Measured m;
    m.x = x;
    m.y = y;
    m.spread = 0.01;
    m.samples = 120;
    m.hasOpenness = true;
    m.openness[0] = left;
    m.openness[1] = right;
    return m;
}

/**
 * The eyes-shut capture.
 * @param left left eye openness
 * @param right right eye openness
 * @return the capture
 */
Measured shut(double left = 0.15, double right = 0.26) {
    Measured m;
    m.samples = 130;
    m.hasOpenness = true;
    m.openness[0] = left;
    m.openness[1] = right;
    return m;
}

/**
 * status.json as seen while frameeyeosc runs, with a capture in it.
 * @param id the capture id (0 for none)
 * @param done whether it is finished
 * @param m its result
 * @param gaze whether it has a gaze average (not for the eyes-shut step)
 * @return the status
 */
EyeStatus runningWith(long long id, bool done, const Measured& m, bool gaze = true) {
    EyeStatus status;
    status.present = status.running = status.tracking = true;
    if (id != 0) {
        GazeCaptureStatus& c = status.capture;
        c.present = true;
        c.id = id;
        c.target = "x";
        c.done = done;
        c.samples = done ? m.samples : 0;
        c.hasAverage = done && gaze && m.samples > 0;
        c.x = m.x;
        c.y = m.y;
        c.spread = m.spread;
        c.hasOpenness = done && m.hasOpenness;
        c.openness[0] = m.openness[0];
        c.openness[1] = m.openness[1];
        c.hasEyeX = done && gaze && m.hasEyeX;
        c.xEye[0] = m.xEye[0];
        c.xEye[1] = m.xEye[1];
    }
    return status;
}

/** The five gaze captures of a user whose tracker reads 10% short sideways, like the live run. */
void fivePoints(Measured points[kPointCount]) {
    points[static_cast<int>(Point::Center)] = steady(0.0116, -0.0196, 0.92, 0.81);
    points[static_cast<int>(Point::Up)] = steady(0.0, -0.0196 + kUpDown / 0.9, 0.93, 0.86);
    points[static_cast<int>(Point::Down)] = steady(0.0, -0.0196 - kUpDown / 0.88, 0.77, 0.75);
    points[static_cast<int>(Point::Left)] = steady(0.0116 - kSide / 0.93, -0.1);
    points[static_cast<int>(Point::Right)] = steady(0.0116 + kSide / 0.93, -0.08);
    points[static_cast<int>(Point::Closed)] = shut();
}

void testFit() {
    Measured points[kPointCount];
    fivePoints(points);
    Values v;
    Point failed = Point::Center;
    CHECK(fitGaze(points, v, failed));
    CHECK(near(v.offsetX, 0.012) && near(v.offsetY, -0.02));  // rounded to 3 decimals
    CHECK(near(v.gainX, 0.93) && near(v.gainUp, 0.9) && near(v.gainDown, 0.88));
    CHECK(fitLids(points, v) && v.hasLids);
    CHECK(near(v.lidClosed[0], 0.15) && near(v.lidClosed[1], 0.26));
    CHECK(near(v.lidOpen[0], 0.92) && near(v.lidUp[1], 0.86) && near(v.lidDown[0], 0.77));

    // A point that went the wrong way (or hardly moved) is reported, and nothing is fitted
    Measured wrong[kPointCount];
    fivePoints(wrong);
    wrong[static_cast<int>(Point::Right)] = steady(0.0116 - 0.05, -0.08);
    CHECK(!fitGaze(wrong, v, failed) && failed == Point::Right);
    // Eyelids that barely closed can't be fitted
    Measured blinkless[kPointCount];
    fivePoints(blinkless);
    blinkless[static_cast<int>(Point::Closed)] = shut(0.85, 0.26);
    Values lids;
    CHECK(!fitLids(blinkless, lids) && !lids.hasLids);

    // Gains stay within 0.5..2 and zero points within ±0.5
    Measured far[kPointCount];
    fivePoints(far);
    far[static_cast<int>(Point::Up)] = steady(0.0, 0.9);
    far[static_cast<int>(Point::Center)] = steady(0.7, -0.0195);
    far[static_cast<int>(Point::Left)] = steady(0.7 - kSide, 0.0);
    far[static_cast<int>(Point::Right)] = steady(0.7 + kSide, 0.0);
    CHECK(fitGaze(far, v, failed));
    CHECK(near(v.gainUp, kGainMin) && near(v.offsetX, kOffsetLimit));
}

/**
 * Each eye's raw x that frameeyeosc would report for a point, for an eye whose tracker reads (angle / gain + offset).
 * @param m the capture to add it to
 * @param yawDeg the target
 * @param gains each eye's gain
 * @param offsets each eye's offset
 */
void withEyes(Measured& m, double yawDeg, const double gains[2], const double offsets[2]) {
    m.hasEyeX = true;
    for (int eye = 0; eye < 2; ++eye) m.xEye[eye] = eyeAngle(yawDeg, eye, kDefaultIpdM) / gains[eye] + offsets[eye];
}

void testEyes() {
    // Seen from between the eyes the target is straight ahead; the left eye turns right to see it, the right left
    const double left = eyeAngle(0.0, 0, 0.063);
    const double right = eyeAngle(0.0, 1, 0.063);
    CHECK(left > 0 && near(left, -right));
    // ...as seen from the eye at the distance the dots are drawn (0.9 m: about 2.0°)
    CHECK(std::fabs(left * kFullScaleDeg - std::atan2(0.0315, kTargetDistanceM) * 180 / M_PI) < 1e-9);
    CHECK(near(kTargetDistanceM, 0.9) && std::fabs(left * kFullScaleDeg - 2.0) < 0.01);
    CHECK(eyeAngle(kSideDeg, 0, 0.063) > eyeAngle(kSideDeg, 1, 0.063));
    CHECK(near(eyeAngle(kSideDeg, 0, 0.0), kSideDeg / kFullScaleDeg));

    const double gains[2] = {0.95, 0.9};
    const double offsets[2] = {0.03, -0.01};
    Measured points[kPointCount];
    fivePoints(points);
    withEyes(points[static_cast<int>(Point::Center)], 0.0, gains, offsets);
    withEyes(points[static_cast<int>(Point::Left)], -kSideDeg, gains, offsets);
    withEyes(points[static_cast<int>(Point::Right)], kSideDeg, gains, offsets);
    Values v;
    CHECK(fitEyes(points, kDefaultIpdM, v) && v.hasEyeX);
    CHECK(near(v.eyeGainX[0], 0.95) && near(v.eyeGainX[1], 0.9));
    CHECK(std::fabs(v.eyeOffsetX[0] - 0.03) < 0.0011 && std::fabs(v.eyeOffsetX[1] + 0.01) < 0.0011);

    // Re-centering moves each eye's zero point and keeps its gain
    Measured moved = points[static_cast<int>(Point::Center)];
    for (double& x : moved.xEye) x += 0.05;
    const Values centered = fitCenter(moved, v, kDefaultIpdM);
    CHECK(std::fabs(centered.eyeOffsetX[0] - 0.08) < 0.0011 && near(centered.eyeGainX[1], 0.9));

    // Without per-eye x (an older frameeyeosc) nothing is fitted and nothing fails
    Measured plain[kPointCount];
    fivePoints(plain);
    Values none;
    CHECK(fitEyes(plain, kDefaultIpdM, none) && !none.hasEyeX);
    // An eye that went the wrong way between the side targets fails
    points[static_cast<int>(Point::Right)].xEye[1] = points[static_cast<int>(Point::Left)].xEye[1] - 0.1;
    CHECK(!fitEyes(points, kDefaultIpdM, v));
}

/** How a made-up user's tracker reads: frameeyeosc's correction with these values gives back the true angles. */
struct User {
    double offsetX = 0.012;
    double offsetY = -0.02;
    double gainX = 0.93;
    double gainUp = 0.9;
    double gainDown = 0.88;
    double rollDeg = 6.7;
    double eyeOffsetX[2] = {0.031, -0.006};
    double eyeGainX[2] = {0.95, 0.9};
};

/**
 * What frameeyeosc reports for a dot: the true angle turned back by the tilt and divided by the gains, around the
 * zero point. Each eye's x is made with the combined y (the Frame reports one up / down angle for both eyes).
 * @param user the user
 * @param yawDeg the dot, right
 * @param pitchDeg the dot, up
 * @return the capture
 */
Measured rawFor(const User& user, double yawDeg, double pitchDeg) {
    const double roll = user.rollDeg * M_PI / 180.0;
    const double c = std::cos(roll);
    const double s = std::sin(roll);
    const double u = yawDeg / kFullScaleDeg / user.gainX;
    const double v = pitchDeg / kFullScaleDeg / (pitchDeg >= 0 ? user.gainUp : user.gainDown);
    Measured m = steady(user.offsetX + u * c - v * s, user.offsetY + u * s + v * c);
    m.hasEyeX = true;
    for (int eye = 0; eye < 2; ++eye) {
        const double angle = eyeAngle(yawDeg, eye, kDefaultIpdM) / user.eyeGainX[eye];
        m.xEye[eye] = user.eyeOffsetX[eye] + (angle - (m.y - user.offsetY) * s) / c;
    }
    return m;
}

/**
 * The five gaze captures and the eyes-shut one of a made-up user.
 * @param user the user
 * @param points where they go
 */
void userPoints(const User& user, Measured points[kPointCount]) {
    for (int i = 0; i < kPointCount - 1; ++i) {
        const Target& t = target(static_cast<Point>(i));
        points[i] = rawFor(user, t.yawDeg, t.pitchDeg);
    }
    points[static_cast<int>(Point::Closed)] = shut();
}

/**
 * frameeyeosc's correction of one gaze pair with fitted values.
 * @param v the values
 * @param x raw x (combined, or an eye's own)
 * @param y raw y
 * @param eye -1 for the combined gaze, else the eye
 * @param outX the corrected x
 * @param outY the corrected y
 */
void corrected(const Values& v, double x, double y, int eye, double& outX, double& outY) {
    const double offset = eye < 0 ? v.offsetX : v.eyeOffsetX[eye];
    const double gain = eye < 0 ? v.gainX : v.eyeGainX[eye];
    level(x - offset, y - v.offsetY, v.rollDeg, outX, outY);
    outX *= gain;
    outY *= outY >= 0 ? v.gainUp : v.gainDown;
}

void testTilt() {
    for (const double roll : {6.7, -1.9, 0.0}) {
        User user;
        user.rollDeg = roll;
        Measured points[kPointCount];
        userPoints(user, points);
        // Both ways of seeing the tilt agree on consistent captures
        CHECK(near(rollFromUpDown(points[1], points[2]), roll));
        CHECK(std::fabs(rollFromSides(points[3], points[4]) - roll) < 1e-6);
        Values v;
        Point failed = Point::Center;
        CHECK(fitGaze(points, v, failed));
        CHECK(near(v.rollDeg, roll) && near(v.offsetX, 0.012) && near(v.offsetY, -0.02));
        CHECK(near(v.gainX, 0.93) && near(v.gainUp, 0.9) && near(v.gainDown, 0.88));
        CHECK(fitEyes(points, kDefaultIpdM, v) && v.hasEyeX);
        CHECK(near(v.eyeGainX[0], 0.95) && near(v.eyeGainX[1], 0.9));
        CHECK(std::fabs(v.eyeOffsetX[0] - 0.031) < 0.0011 && std::fabs(v.eyeOffsetX[1] + 0.006) < 0.0011);
        // Corrected with the fit, the side dots come out level and as far as they are, up straight up
        for (int i = 1; i < 5; ++i) {
            const Target& t = target(static_cast<Point>(i));
            double x = 0.0;
            double y = 0.0;
            corrected(v, points[i].x, points[i].y, -1, x, y);
            CHECK(std::fabs(x - t.yawDeg / kFullScaleDeg) < 0.002 && std::fabs(y - t.pitchDeg / kFullScaleDeg) < 0.002);
            // Each eye's own x lands on its own angle to the dot
            for (int eye = 0; eye < 2; ++eye) {
                corrected(v, points[i].xEye[eye], points[i].y, eye, x, y);
                CHECK(std::fabs(x - eyeAngle(t.yawDeg, eye, kDefaultIpdM)) < 0.002);
            }
        }
    }
    // Beyond ±20° it is held at 20°, and rounded to 0.1°
    CHECK(near(rollFromUpDown(steady(-0.3, 0.4), steady(0.3, -0.4)), kRollLimitDeg));
    CHECK(near(rollFromUpDown(steady(0.3, 0.4), steady(-0.3, -0.4)), -kRollLimitDeg));
    CHECK(near(rollFromUpDown(steady(-0.01745, 0.5), steady(0.01745, -0.5)), 2.0));
}

void testLoggedCaptures() {
    // Raw five-point captures from the journal of four full fits (19:31, 23:35, 00:03, and 23:37 a day later),
    // 1.0 = 45°
    const double logged[4][5][2] = {
        {{-0.0654, -0.0112}, {-0.1003, 0.3168}, {0.0022, -0.3757}, {-0.5133, -0.0992}, {0.4173, 0.0099}},
        {{-0.0433, -0.0489}, {-0.0886, 0.3247}, {-0.0141, -0.3654}, {-0.5326, -0.1347}, {0.3971, -0.0097}},
        {{0.0012, 0.0198}, {-0.0107, 0.3932}, {0.0015, -0.2545}, {-0.4783, 0.0183}, {0.4919, -0.0136}},
        {{-0.0063, -0.0033}, {0.0045, 0.3760}, {0.0272, -0.2851}, {-0.4414, 0.0094}, {0.4854, -0.0514}},
    };
    const double upDown[4] = {8.4, 6.2, 1.1, 2.0};
    const double sides[4] = {6.7, 7.7, -1.9, -3.8};
    for (int run = 0; run < 4; ++run) {
        Measured points[kPointCount];
        for (int i = 0; i < 5; ++i) points[i] = steady(logged[run][i][0], logged[run][i][1]);
        points[static_cast<int>(Point::Closed)] = shut();
        Values v;
        Point failed = Point::Center;
        CHECK(fitGaze(points, v, failed) && near(v.rollDeg, upDown[run]));
        CHECK(std::fabs(rollFromSides(points[3], points[4]) - sides[run]) < 0.05);
        // Leveled, the down dot is straight below the up one (within the 0.1° rounding of the tilt)
        double ux = 0.0;
        double uy = 0.0;
        double dx = 0.0;
        double dy = 0.0;
        level(points[1].x - points[0].x, points[1].y - points[0].y, v.rollDeg, ux, uy);
        level(points[2].x - points[0].x, points[2].y - points[0].y, v.rollDeg, dx, dy);
        CHECK(std::fabs(ux - dx) < (uy - dy) * std::tan(0.05 * M_PI / 180.0) + 1e-9);
    }
}

void testRewearTilt() {
    // Fitted once (tilted 6.7°), then put on again: moved and tilted the other way, the gains the same
    Measured first[kPointCount];
    userPoints(User(), first);
    Values current;
    Point failed = Point::Center;
    CHECK(fitGaze(first, current, failed) && fitEyes(first, kDefaultIpdM, current) && fitLids(first, current));
    User again;
    again.offsetX = 0.03;
    again.offsetY = 0.05;
    again.rollDeg = -1.9;
    again.eyeOffsetX[0] = 0.05;
    again.eyeOffsetX[1] = 0.012;
    Measured points[kPointCount];
    userPoints(again, points);
    Values v;
    CHECK(fitTilt(points, current, kDefaultIpdM, v, failed));
    CHECK(near(v.rollDeg, -1.9) && near(v.offsetX, 0.03) && near(v.offsetY, 0.05));
    // Only straight ahead, up and down are used
    Measured three[kPointCount];
    three[0] = points[0];
    three[1] = points[1];
    three[2] = points[2];
    Values fromThree;
    CHECK(fitTilt(three, current, kDefaultIpdM, fromThree, failed) && near(fromThree.rollDeg, -1.9));
    CHECK(near(fromThree.eyeOffsetX[0], v.eyeOffsetX[0]) && near(fromThree.eyeOffsetX[1], v.eyeOffsetX[1]));
    CHECK(std::fabs(v.eyeOffsetX[0] - 0.05) < 0.0011 && std::fabs(v.eyeOffsetX[1] - 0.012) < 0.0011);
    // The gains, each eye's gain and the eyelids stay
    CHECK(near(v.gainX, current.gainX) && near(v.gainUp, current.gainUp) && near(v.gainDown, current.gainDown));
    CHECK(near(v.eyeGainX[0], current.eyeGainX[0]) && near(v.eyeGainX[1], current.eyeGainX[1]));
    CHECK(v.hasLids && near(v.lidClosed[1], current.lidClosed[1]) && near(v.lidOpen[0], current.lidOpen[0]));
    // Corrected, the side dots come out level again and the up / down dots straight above and below
    for (int i = 1; i < 5; ++i) {
        double x = 0.0;
        double y = 0.0;
        corrected(v, points[i].x, points[i].y, -1, x, y);
        CHECK(std::fabs(i < 3 ? x : y) < 0.002);
    }
    // Without each eye's own fit, nothing of it is made up
    Values plain = current;
    plain.hasEyeX = false;
    plain.eyeOffsetX[0] = plain.eyeOffsetX[1] = 0.0;
    CHECK(fitTilt(points, plain, kDefaultIpdM, v, failed) && !v.hasEyeX && near(v.eyeOffsetX[0], 0.0));
    // A dot that did not move fails like in the full fit, and nothing is changed
    Measured stuck[kPointCount];
    userPoints(again, stuck);
    stuck[static_cast<int>(Point::Down)] = stuck[static_cast<int>(Point::Center)];
    Values untouched;
    CHECK(!fitTilt(stuck, current, kDefaultIpdM, untouched, failed) && failed == Point::Down);
    CHECK(near(untouched.offsetX, 0.0));

    // Re-centering only keeps the tilt, and each eye still lands on its own angle straight ahead
    User moved;
    moved.offsetX = 0.05;
    moved.offsetY = 0.03;
    moved.eyeOffsetX[0] = 0.069;
    moved.eyeOffsetX[1] = 0.032;
    const Measured center = rawFor(moved, 0.0, 0.0);
    const Values centered = fitCenter(center, current, kDefaultIpdM);
    CHECK(near(centered.rollDeg, 6.7) && near(centered.offsetX, 0.05) && near(centered.offsetY, 0.03));
    CHECK(std::fabs(centered.eyeOffsetX[0] - 0.069) < 0.0011 && std::fabs(centered.eyeOffsetX[1] - 0.032) < 0.0011);
}

void testCenterAndUsable() {
    Values current;
    current.gainX = 1.3;
    current.hasLids = true;
    current.lidOpen[0] = 0.9;
    const Values v = fitCenter(steady(-0.0314, 0.1234), current);
    CHECK(near(v.offsetX, -0.031) && near(v.offsetY, 0.123));
    CHECK(near(v.gainX, 1.3) && v.hasLids && near(v.lidOpen[0], 0.9));

    CHECK(usable(steady(0, 0)));
    Measured few = steady(0, 0);
    few.samples = kMinSamples - 1;
    CHECK(!usable(few));
    // A slow tracker (15 a second, as seen on a Frame): 26 of 26 samples usable is plenty
    Measured slow = steady(0, 0);
    slow.samples = 26;
    slow.received = 26;
    slow.rateHz = 15.0;
    CHECK(samplesNeeded(slow) == 16 && usable(slow));
    CHECK(tryText(Point::Center, 1, slow, Measured(), "ok") ==
          "center try 1: 26 of 26 samples usable at 15 Hz (needs 16), spread 0.5° (max 2.7°) -> ok");
    // ...but not when most of them were blinks, nor below the floor of 12
    slow.samples = 15;
    CHECK(!usable(slow));
    slow.samples = 10;
    slow.received = 10;
    CHECK(samplesNeeded(slow) == kMinSamplesFloor && !usable(slow));
    // At 90 a second no stricter than 0.5.2: 45 of the 153 that came in (60% would be 92)
    Measured fast = steady(0, 0);
    fast.received = 153;
    fast.samples = 44;
    CHECK(samplesNeeded(fast) == kMinSamples && !usable(fast));
    fast.samples = 45;
    CHECK(usable(fast));
    // In between, 60%: 50 came in, 30 needed
    fast.received = 50;
    CHECK(samplesNeeded(fast) == 30);
    Measured shaky = steady(0, 0);
    shaky.spread = kMaxSpread + 0.001;
    CHECK(!usable(shaky));
    CHECK(!usable(Measured()));

    const Measured center = steady(0, 0, 0.92, 0.81);
    CHECK(usableClosed(shut(), center));
    CHECK(!usableClosed(shut(0.15, 0.7), center));  // the right eye stayed open
    Measured briefly = shut();
    briefly.samples = 10;
    CHECK(!usableClosed(briefly, center));
}

/**
 * Take a session through one step: settle, ask, answer.
 * @param s the session
 * @param now the time (advanced)
 * @param id the next capture id (advanced)
 * @param answer the capture's result
 * @param settle how long the step settles (the first dot's first try kFirstSettleExtraSec more)
 * @param gaze whether the answer has a gaze average
 * @return the actions after the answer
 */
Actions runStep(Session& s, double& now, long long& id, const Measured& answer, double settle = kSettleSec,
                bool gaze = true, const Dashboard& dashboard = kClosed) {
    if (gaze && s.view().index == 0 && s.view().attempt == 1) settle += kFirstSettleExtraSec;
    s.tick(now, dashboard, runningWith(0, false, {}));
    now += settle;
    const Actions a = s.tick(now, dashboard, runningWith(0, false, {}));
    CHECK(a.writeCapture);
    CHECK(near(a.captureSec, gaze ? kCaptureSec : kClosedSec) && near(a.skipSec, gaze ? kCaptureSkipSec : kClosedSkipSec));
    s.captureSent(++id, now);
    now += (gaze ? kCaptureSec : kClosedSec) + 0.2;
    return s.tick(now, dashboard, runningWith(id, true, answer, gaze));
}

void testFullSession() {
    Measured points[kPointCount];
    fivePoints(points);
    Session s;
    double now = 100.0;
    long long id = 0;
    s.start(Mode::Full, Values(), now);
    CHECK(s.active() && s.view().phase == Phase::Settling && s.view().count == 6);

    // Started from the panel, the dashboard open: the first step shows at once (no waiting for the dashboard to
    // close): the dot straight ahead, the ring full and no number until it is measured
    Actions a = s.tick(now, kOpen, runningWith(0, false, {}));
    CHECK(a.showTarget && a.style == TargetStyle::Dot && a.seconds == 0 && near(a.progress, 1.0));
    CHECK(near(a.yawDeg, 0.0) && near(a.pitchDeg, 0.0));
    // The first dot waits kFirstSettleExtraSec longer (the eyes are still on the button just pressed)
    const double firstSettle = kSettleSec + kFirstSettleExtraSec;
    CHECK(near(firstSettle, 1.5));
    a = s.tick(now + kSettleSec, kOpen, runningWith(0, false, {}));
    CHECK(!a.writeCapture && s.view().phase == Phase::Settling && a.seconds == 0);
    CHECK(near(a.progress, (firstSettle - kSettleSec + kCaptureSec) / (firstSettle + kCaptureSec)));
    a = s.tick(now + firstSettle - 0.01, kOpen, runningWith(0, false, {}));
    CHECK(!a.writeCapture);
    now += firstSettle;
    a = s.tick(now, kOpen, runningWith(0, false, {}));
    CHECK(a.writeCapture && std::string(a.target) == "center" && near(a.captureSec, 2.0) && near(a.skipSec, 0.3));
    CHECK(near(kSettleSec + kCaptureSec, 2.5));
    s.captureSent(++id, now);
    // An old capture's result is not ours; the measured seconds count down 2, 1 and the ring runs down evenly
    a = s.tick(now + 0.5, kOpen, runningWith(id - 1 + 100, true, steady(0.3, 0.3)));
    CHECK(s.view().phase == Phase::Capturing && a.seconds == 2 && near(a.progress, 1.5 / 3.5));
    a = s.tick(now + 1.0, kOpen, runningWith(id, false, {}));
    a = s.tick(now + 1.5, kOpen, runningWith(id, false, {}));
    CHECK(a.seconds == 1 && near(a.progress, 0.5 / 3.5));
    now += 2.2;
    a = s.tick(now, kOpen, runningWith(id, true, points[0]));
    CHECK(s.view().point == Point::Up && s.view().phase == Phase::Settling);
    // Later dots settle as before
    a = s.tick(now + kSettleSec - 0.01, kOpen, runningWith(0, false, {}));
    CHECK(!a.writeCapture && near(a.progress, (0.01 + kCaptureSec) / 2.5));

    // The dot glides up from the center over kMoveSec
    a = s.tick(now + kMoveSec / 2, kOpen, runningWith(0, false, {}));
    CHECK(a.pitchDeg > 0.0 && a.pitchDeg < kUpDownDeg);
    a = s.tick(now + kMoveSec, kOpen, runningWith(0, false, {}));
    CHECK(near(a.pitchDeg, kUpDownDeg));

    // An unsteady capture is tried again at the same point, without gliding. The dashboard is closed meanwhile: the
    // fit goes on (the dots stay)
    Measured shaky = points[1];
    shaky.spread = 0.2;
    runStep(s, now, id, shaky);
    CHECK(s.active());
    CHECK(s.view().point == Point::Up && s.view().attempt == 2 && s.view().phase == Phase::Settling);
    a = s.tick(now + 0.01, kClosed, runningWith(0, false, {}));
    CHECK(near(a.pitchDeg, kUpDownDeg));
    for (int i = 1; i < 5; ++i) runStep(s, now, id, points[i]);
    CHECK(s.view().point == Point::Closed);

    // The eyes-shut step: "close your eyes" 3, 2, 1, then "keep them closed", then "open your eyes"
    // The ring runs the full circle over the countdown: full at 3.0 s left, half at 1.5 s, nearly empty at 0.1 s
    a = s.tick(now, kClosed, runningWith(0, false, {}));
    CHECK(a.style == TargetStyle::CloseEyes && a.seconds == 3 && near(a.progress, 1.0));
    a = s.tick(now + 1.5, kClosed, runningWith(0, false, {}));
    CHECK(a.seconds == 2 && near(a.progress, 0.5));
    a = s.tick(now + 2.9, kClosed, runningWith(0, false, {}));
    CHECK(a.seconds == 1 && near(a.progress, 0.1 / 3));
    a = s.tick(now + 2.5, kClosed, runningWith(0, false, {}));
    CHECK(a.style == TargetStyle::CloseEyes && a.seconds == 1 && !a.writeCapture);
    now += kCloseSettleSec;
    a = s.tick(now, kClosed, runningWith(0, false, {}));
    CHECK(a.writeCapture && std::string(a.target) == "closed" && a.style == TargetStyle::KeepClosed);
    CHECK(near(a.captureSec, 3.0) && near(a.skipSec, 0.5) && near(a.progress, 1.0));
    s.captureSent(++id, now);
    // ...and again the whole way round while the eyes are shut
    a = s.tick(now + 1.5, kClosed, runningWith(id, false, {}));
    CHECK(a.style == TargetStyle::KeepClosed && near(a.progress, 0.5));
    now += kClosedSec + 0.2;
    a = s.tick(now, kClosed, runningWith(id, true, points[5], false));
    CHECK(s.view().phase == Phase::Reopen && a.style == TargetStyle::OpenEyes && !a.writeValues);
    now += kReopenSec;
    a = s.tick(now, kClosed, runningWith(0, false, {}));
    CHECK(a.writeValues && !a.showTarget && s.view().phase == Phase::Done && !s.active());
    CHECK(near(a.values.gainX, 0.93) && a.values.hasLids && near(a.values.lidClosed[1], 0.26));
    CHECK(id == 7);
    // Done stays done
    a = s.tick(now + 1, kClosed, runningWith(id, true, points[5], false));
    CHECK(!a.writeValues && !a.writeCapture);
}

void testCenterSession() {
    Session s;
    double now = 0.0;
    long long id = 0;
    Values current;
    current.gainUp = 1.4;
    current.hasLids = true;
    s.start(Mode::Center, current, now);
    CHECK(s.view().count == 1);
    // Re-centering needs no openness
    Measured gazeOnly = steady(0.05, -0.12);
    gazeOnly.hasOpenness = false;
    const Actions a = runStep(s, now, id, gazeOnly);
    CHECK(a.writeValues && s.view().phase == Phase::Done);
    CHECK(near(a.values.offsetX, 0.05) && near(a.values.offsetY, -0.12) && near(a.values.gainUp, 1.4));
    CHECK(a.values.hasLids);
}

void testTiltSession() {
    Measured first[kPointCount];
    userPoints(User(), first);
    Values current;
    Point failed = Point::Center;
    CHECK(fitGaze(first, current, failed) && fitEyes(first, kDefaultIpdM, current));
    User again;
    again.rollDeg = -1.9;
    again.offsetY = 0.05;
    Measured points[kPointCount];
    userPoints(again, points);
    Session s;
    double now = 0.0;
    long long id = 0;
    s.start(Mode::Tilt, current, now);
    CHECK(s.view().count == 3 && s.view().mode == Mode::Tilt);
    // Straight ahead, then up, then down; the openness is not needed
    const Point order[3] = {Point::Center, Point::Up, Point::Down};
    Actions a;
    for (int i = 0; i < 3; ++i) {
        CHECK(s.view().point == order[i] && s.view().index == i);
        Measured gazeOnly = points[static_cast<int>(order[i])];
        gazeOnly.hasOpenness = false;
        a = runStep(s, now, id, gazeOnly);
        if (i == 0) {
            // The dot glides up
            const Actions glide = s.tick(now + kMoveSec / 2, kClosed, runningWith(0, false, {}));
            CHECK(glide.pitchDeg > 0.0 && glide.pitchDeg < kUpDownDeg && near(glide.yawDeg, 0.0));
        }
    }
    CHECK(a.writeValues && s.view().phase == Phase::Done && id == 3);
    CHECK(near(a.values.rollDeg, -1.9) && near(a.values.offsetY, 0.05) && near(a.values.gainUp, current.gainUp));
    // The last try's numbers, then the tilt: one line each
    CHECK(a.log.rfind("down try 1: ", 0) == 0);
    CHECK(a.log.find("\ntilt -1.9° from up/down (was +6.7°)") != std::string::npos);

    // A dot that did not move: stops there, like the full fit
    Session stuck;
    now = 0.0;
    id = 0;
    stuck.start(Mode::Tilt, current, now);
    runStep(stuck, now, id, points[static_cast<int>(Point::Center)]);
    runStep(stuck, now, id, points[static_cast<int>(Point::Up)]);
    a = runStep(stuck, now, id, points[static_cast<int>(Point::Center)]);
    CHECK(!a.writeValues && stuck.view().failure == Failure::NoMovement && stuck.view().point == Point::Down);
    CHECK(stuck.view().detail.eye == -1 && near(stuck.view().detail.neededDeg, 3.75));

    // The full fit logs the tilt both ways
    Session full;
    now = 0.0;
    id = 0;
    full.start(Mode::Full, Values(), now);
    for (int i = 0; i < 5; ++i) runStep(full, now, id, first[i]);
    runStep(full, now, id, shut(), kCloseSettleSec, false);
    now += kReopenSec;
    a = full.tick(now, kClosed, runningWith(0, false, {}));
    CHECK(a.writeValues && near(a.values.rollDeg, 6.7));
    CHECK(a.log == "tilt +6.7° from up/down, +6.7° from the sides");
}

void testFailures() {
    Measured points[kPointCount];
    fivePoints(points);
    {
        // Three unsteady tries in a row
        Session s;
        double now = 0.0;
        long long id = 0;
        s.start(Mode::Center, Values(), now);
        Measured shaky = points[0];
        shaky.samples = 3;
        Actions a;
        for (int i = 0; i < kMaxAttempts; ++i) {
            a = runStep(s, now, id, shaky);
            // Every try is logged with its numbers
            const std::string expected = "center try " + std::to_string(i + 1) + ": 3 samples (min 45), spread ";
            CHECK(a.log.rfind(expected, 0) == 0);
            CHECK(a.log.find(i + 1 < kMaxAttempts ? "-> again" : "-> failed") != std::string::npos);
        }
        CHECK(s.view().phase == Phase::Failed && s.view().failure == Failure::Unsteady);
        // The numbers behind it: the tries and the last one
        CHECK(s.view().detail.tries == kMaxAttempts && s.view().detail.last.samples == 3);
        Measured wide = steady(0, 0);
        wide.spread = 3.4 / 45;
        CHECK(tryText(Point::Center, 2, wide, Measured(), "again") ==
              "center try 2: 120 samples (min 45), spread 3.4° (max 2.7°) -> again");
    }
    {
        // The eyes never shut: three tries, then NotClosed
        Session s;
        double now = 0.0;
        long long id = 0;
        s.start(Mode::Full, Values(), now);
        for (int i = 0; i < 5; ++i) runStep(s, now, id, points[i]);
        Actions a;
        for (int i = 0; i < kMaxAttempts; ++i) a = runStep(s, now, id, steady(0, 0), kCloseSettleSec, false);
        CHECK(s.view().failure == Failure::NotClosed && s.view().point == Point::Closed);
        // Each eye had to read below 70% of its straight-ahead reading
        CHECK(near(s.view().detail.closedBelow[0], 0.7 * points[0].openness[0]) && s.view().detail.tries == 3);
        CHECK(a.log.rfind("closed try 3: 120 samples (min 45), openness L ", 0) == 0);
    }
    {
        // "Down" did not move: stops before asking to close the eyes
        Session s;
        double now = 0.0;
        long long id = 0;
        s.start(Mode::Full, Values(), now);
        Measured flat[kPointCount];
        fivePoints(flat);
        flat[static_cast<int>(Point::Down)] = flat[static_cast<int>(Point::Center)];
        for (int i = 0; i < 5; ++i) runStep(s, now, id, flat[i]);
        CHECK(s.view().failure == Failure::NoMovement && s.view().point == Point::Down && id == 5);
        // It did not move at all, and had to move a quarter of 15°
        const FailureDetail& d = s.view().detail;
        CHECK(d.eye == -1 && near(d.movedDeg, 0.0) && near(d.neededDeg, 3.75));
    }
    {
        // Eyelids that barely closed: the gaze is fine, but nothing is written
        Session s;
        double now = 0.0;
        long long id = 0;
        s.start(Mode::Full, Values(), now);
        // The right eye reads low looking down, and its shut reading is less than 0.1 below that
        Measured lowDown[kPointCount];
        fivePoints(lowDown);
        lowDown[static_cast<int>(Point::Down)].openness[1] = 0.6;
        for (int i = 0; i < 5; ++i) runStep(s, now, id, lowDown[i]);
        runStep(s, now, id, shut(0.15, 0.55), kCloseSettleSec, false);
        now += kReopenSec;
        const Actions a = s.tick(now, kClosed, runningWith(0, false, {}));
        CHECK(!a.writeValues && s.view().failure == Failure::NoLidRange);
        // Which eye and reading: the right eye looking down, 0.6 open against 0.55 shut
        const FailureDetail& d = s.view().detail;
        CHECK(d.eye == 1 && d.lidPoint == Point::Down && near(d.lidOpen, 0.6) && near(d.lidClosed, 0.55));
    }
    {
        // Opening the dashboard during a run started without it (re-centering when the headset is put on) stops it
        // and hides the target
        Session s;
        s.start(Mode::Full, Values(), 0.0);
        s.tick(0.5, kClosed, runningWith(0, false, {}));
        const Actions a = s.tick(0.8, kOpen, runningWith(0, false, {}));
        CHECK(!a.showTarget && s.view().failure == Failure::DashboardOpened);
    }
    {
        // Started with it open and closed halfway: the run goes on; opened again, it stops (the way to stop it
        // once the panel's "Stop" can't be reached)
        Session s;
        s.start(Mode::Center, Values(), 0.0);
        CHECK(s.tick(0.1, kOpen, runningWith(0, false, {})).showTarget);
        CHECK(s.tick(0.3, kClosed, runningWith(0, false, {})).showTarget && s.active());
        const Actions a = s.tick(0.4, kOpen, runningWith(0, false, {}));
        CHECK(!a.showTarget && s.view().failure == Failure::DashboardOpened);
    }
    {
        // Open all along on this panel: nothing stops it, and it runs to the end
        Session s;
        double now = 0.0;
        long long id = 0;
        s.start(Mode::Center, Values(), now);
        const Actions a = runStep(s, now, id, steady(0.05, -0.12), kSettleSec, true, kOpen);
        CHECK(a.writeValues && s.view().phase == Phase::Done);
    }
    {
        // The dashboard showing another page: a moment of it (the dashboard closing: the panel hides a frame before
        // it) goes on; kAwaySec of it stops the run
        Session s;
        s.start(Mode::Full, Values(), 0.0);
        s.tick(0.1, kOpen, runningWith(0, false, {}));
        s.tick(0.2, kElsewhere, runningWith(0, false, {}));
        CHECK(s.tick(0.2 + kAwaySec - 0.05, kElsewhere, runningWith(0, false, {})).showTarget);
        // Back on the panel and away again: counted afresh
        s.tick(0.2 + kAwaySec - 0.04, kOpen, runningWith(0, false, {}));
        CHECK(s.tick(0.2 + kAwaySec + 0.1, kElsewhere, runningWith(0, false, {})).showTarget && s.active());
        // ...and then closed: it goes on
        CHECK(s.tick(0.2 + kAwaySec + 0.2, kClosed, runningWith(0, false, {})).showTarget && s.active());
        CHECK(s.tick(5.0, kClosed, runningWith(0, false, {})).showTarget && s.active());
        // Open all along, on another page
        Session away;
        away.start(Mode::Full, Values(), 0.0);
        away.tick(0.1, kOpen, runningWith(0, false, {}));
        away.tick(0.25, kElsewhere, runningWith(0, false, {}));
        const Actions a = away.tick(0.25 + kAwaySec, kElsewhere, runningWith(0, false, {}));
        CHECK(!a.showTarget && away.view().failure == Failure::Left);
    }
    {
        // frameeyeosc not running
        Session s;
        s.start(Mode::Center, Values(), 0.0);
        s.tick(0.1, kOpen, EyeStatus());
        CHECK(s.view().failure == Failure::NotRunning);
    }
    {
        // No answer to a capture
        Session s;
        s.start(Mode::Center, Values(), 0.0);
        s.tick(0.1, kClosed, runningWith(0, false, {}));
        CHECK(s.tick(2.2, kClosed, runningWith(0, false, {})).writeCapture);
        s.captureSent(1, 2.2);
        s.tick(2.2 + kResultTimeoutSec + 0.1, kClosed, runningWith(0, false, {}));
        CHECK(s.view().failure == Failure::NoResult);
    }
    {
        // "Stop" pressed, another tab chosen, and a failed write
        Session s;
        s.start(Mode::Full, Values(), 0.0);
        s.tick(0.1, kOpen, runningWith(0, false, {}));
        s.cancel();
        CHECK(s.view().failure == Failure::Cancelled && !s.active());
        CHECK(!s.tick(0.2, kOpen, runningWith(0, false, {})).showTarget);
        s.start(Mode::Tilt, Values(), 0.0);
        s.tick(0.1, kOpen, runningWith(0, false, {}));
        s.cancel(Failure::Left);
        CHECK(s.view().failure == Failure::Left && !s.active());
        // Stopping a stopped fit changes nothing
        s.cancel();
        CHECK(s.view().failure == Failure::Left);
        s.start(Mode::Center, Values(), 0.0);
        s.tick(0.1, kOpen, runningWith(0, false, {}));
        const Actions a = s.tick(2.2, kOpen, runningWith(0, false, {}));
        CHECK(a.writeCapture);
        s.writeFailed();
        CHECK(s.view().failure == Failure::WriteFailed);
    }
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testFit();
    testEyes();
    testTilt();
    testLoggedCaptures();
    testRewearTilt();
    testCenterAndUsable();
    testFullSession();
    testCenterSession();
    testTiltSession();
    testFailures();
    if (gFailures == 0) std::printf("gaze-fit-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
