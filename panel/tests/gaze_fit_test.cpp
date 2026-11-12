// Tests for the eye fit (gaze_fit.cpp): the fit itself and the session, without OpenVR or files.
// Built with the panel as gaze-fit-test; exits non-zero on failure.
#include "gaze_fit.h"

#include <cmath>
#include <cstdio>
#include <string>
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
        c.cameraOpen[0] = done && m.cameraOpen[0];
        c.cameraOpen[1] = done && m.cameraOpen[1];
        for (int eye = 0; eye < 2; ++eye) {
            c.hasPupil[eye] = done && m.hasPupil[eye];
            c.pupil[eye][0] = m.pupil[eye][0];
            c.pupil[eye][1] = m.pupil[eye][1];
            c.pupilFresh[eye] = done ? m.pupilFresh[eye] : 0;
        }
    }
    return status;
}

/** A made-up pupil map for both eyes (synthetic numbers, like the ones seen: about 240 degrees per unit, the pupil's
 *  v going down as the eye looks up, a little cross-talk). */
PupilMap madeUpMap(int eye) {
    PupilMap map;
    map.yaw[0] = eye == 0 ? -42.0 : -38.0;
    map.yaw[1] = 240.0;
    map.yaw[2] = 15.0;
    map.pitch[0] = eye == 0 ? 25.0 : 23.0;
    map.pitch[1] = -10.0;
    map.pitch[2] = -230.0;
    return map;
}

/**
 * The pupil pair that a map sends to a direction (the map's inverse).
 * @param map the map
 * @param yaw the direction's yaw (degrees)
 * @param pitch ...and pitch
 * @param out u, v
 */
void pairFor(const PupilMap& map, double yaw, double pitch, double out[2]) {
    const double a = map.yaw[1], b = map.yaw[2], c = map.pitch[1], d = map.pitch[2];
    const double ry = yaw - map.yaw[0], rp = pitch - map.pitch[0];
    const double det = a * d - b * c;
    out[0] = (d * ry - b * rp) / det;
    out[1] = (a * rp - c * ry) / det;
}

/**
 * Give the five gaze captures each eye's pupil pair for its direction to its target, as if the eye looked exactly
 * there, shifted by `shift` degrees (the headset sitting differently), with `fresh` fresh samples.
 */
void withPupils(Measured points[kPointCount], double ipd, double shift[2], int fresh = 140) {
    const Point gaze[5] = {Point::Center, Point::Up, Point::Down, Point::Left, Point::Right};
    for (Point point : gaze) {
        Measured& m = points[static_cast<int>(point)];
        for (int eye = 0; eye < 2; ++eye) {
            double direction[2];
            eyeDirection(target(point), eye, ipd, direction);
            pairFor(madeUpMap(eye), direction[0] - shift[0], direction[1] - shift[1], m.pupil[eye]);
            m.hasPupil[eye] = true;
            m.pupilFresh[eye] = fresh;
        }
    }
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
    // The tilt, the sideways and the down gain from before go back to 0 / 1: only the zero point and the up gain are
    // measured
    Values v;
    v.rollDeg = 6.7;
    v.gainX = 0.93;
    v.gainDown = 0.88;
    Point failed = Point::Center;
    Notes notes;
    CHECK(fitGaze(points, v, failed, nullptr, &notes));
    CHECK(near(v.offsetX, 0.012) && near(v.offsetY, -0.02));  // rounded to 3 decimals
    CHECK(near(v.gainX, 1.0) && near(v.gainUp, 0.9) && near(v.gainDown, 1.0) && near(v.rollDeg, 0.0));
    // What the dots gave is still measured, for the log
    CHECK(near(notes.gainX, 0.93) && near(notes.gainUp, 0.9) && near(notes.gainDown, 0.88) && notes.upGainFits == 1);
    CHECK(fitLids(points, v) && v.hasLids);
    CHECK(near(v.lidClosed[0], 0.15) && near(v.lidClosed[1], 0.26));
    CHECK(near(v.lidOpen[0], 0.92) && near(v.lidUp[1], 0.86) && near(v.lidDown[0], 0.77));

    // The up gain written is the mean with the fits before (this one 0.90)
    Values averaged;
    CHECK(fitGaze(points, averaged, failed, nullptr, &notes, {0.8, 0.85}));
    CHECK(near(averaged.gainUp, 0.85) && notes.upGainFits == 3 && near(notes.gainUp, 0.9));

    // A point that went the wrong way (or hardly moved) is reported, and nothing is fitted; the side dots still count
    Measured wrong[kPointCount];
    fivePoints(wrong);
    wrong[static_cast<int>(Point::Right)] = steady(0.0116 - 0.05, -0.08);
    Values untouched;
    untouched.offsetX = 0.3;
    CHECK(!fitGaze(wrong, untouched, failed) && failed == Point::Right && near(untouched.offsetX, 0.3));
    Measured flatDown[kPointCount];
    fivePoints(flatDown);
    flatDown[static_cast<int>(Point::Down)] = flatDown[static_cast<int>(Point::Center)];
    FailureDetail detail;
    CHECK(!fitGaze(flatDown, v, failed, &detail) && failed == Point::Down);
    CHECK(detail.eye == -1 && near(detail.movedDeg, 0.0) && near(detail.neededDeg, 3.75));
    // Eyelids that barely closed can't be fitted
    Measured blinkless[kPointCount];
    fivePoints(blinkless);
    blinkless[static_cast<int>(Point::Closed)] = shut(0.85, 0.26);
    Values lids;
    CHECK(!fitLids(blinkless, lids) && !lids.hasLids);

    // The up gain stays within 0.5..2 and zero points within ±0.5
    Measured far[kPointCount];
    fivePoints(far);
    far[static_cast<int>(Point::Up)] = steady(0.0, 0.9);
    far[static_cast<int>(Point::Center)] = steady(0.7, -0.0195);
    far[static_cast<int>(Point::Left)] = steady(0.7 - kSide, 0.0);
    far[static_cast<int>(Point::Right)] = steady(0.7 + kSide, 0.0);
    CHECK(fitGaze(far, v, failed, nullptr, &notes));
    CHECK(near(v.gainUp, kGainMin) && near(notes.gainUp, kGainMin) && near(v.offsetX, kOffsetLimit));
}

void testUpGains() {
    int fits = 0;
    // Alone; then with the fits before, the last kUpGainFits - 1 of them (oldest first)
    CHECK(near(meanUpGain(0.88, {}, &fits), 0.88) && fits == 1);
    CHECK(near(meanUpGain(0.89, {1.01, 0.89, 0.86}, &fits), 0.91) && fits == 4);
    // Five wears on 2026-10-09 (0.89, 1.01, 0.89, 0.83, 0.78): their mean, 0.88
    CHECK(near(meanUpGain(0.78, {0.89, 1.01, 0.89, 0.83}, &fits), 0.88) && fits == 5);
    // An older one beyond kUpGainFits doesn't count
    CHECK(near(meanUpGain(0.78, {2.0, 0.89, 1.01, 0.89, 0.83}, &fits), 0.88) && fits == kUpGainFits);
    // Within range and rounded to 2 decimals; anything not a number is skipped
    CHECK(near(meanUpGain(0.3, {0.4}), kGainMin));
    CHECK(near(meanUpGain(0.904, {NAN}, &fits), 0.9) && fits == 1);

    // What is remembered: the last kUpGainFits, oldest first
    CHECK(nextUpGains({}, 0.88) == std::vector<double>({0.88}));
    const std::vector<double> five = {0.89, 1.01, 0.89, 0.83, 0.78};
    const std::vector<double> next = nextUpGains(five, 0.9);
    CHECK(next == std::vector<double>({1.01, 0.89, 0.83, 0.78, 0.9}));
    CHECK(nextUpGains({NAN, 0.8}, 0.9) == std::vector<double>({0.8, 0.9}));
}

void testLoggedCaptures() {
    // Raw five-point captures from the journal of four full fits (19:31, 23:35, 00:03, and 23:37 a day later),
    // 1.0 = 45°: each writes its own zero point and up gain, never a tilt, a sideways or a down gain
    const double logged[4][5][2] = {
        {{-0.0654, -0.0112}, {-0.1003, 0.3168}, {0.0022, -0.3757}, {-0.5133, -0.0992}, {0.4173, 0.0099}},
        {{-0.0433, -0.0489}, {-0.0886, 0.3247}, {-0.0141, -0.3654}, {-0.5326, -0.1347}, {0.3971, -0.0097}},
        {{0.0012, 0.0198}, {-0.0107, 0.3932}, {0.0015, -0.2545}, {-0.4783, 0.0183}, {0.4919, -0.0136}},
        {{-0.0063, -0.0033}, {0.0045, 0.3760}, {0.0272, -0.2851}, {-0.4414, 0.0094}, {0.4854, -0.0514}},
    };
    std::vector<double> history;
    double sum = 0.0;
    for (int run = 0; run < 4; ++run) {
        Measured points[kPointCount];
        for (int i = 0; i < 5; ++i) points[i] = steady(logged[run][i][0], logged[run][i][1]);
        points[static_cast<int>(Point::Closed)] = shut();
        Values v;
        v.rollDeg = 4.4;
        Notes notes;
        Point failed = Point::Center;
        CHECK(fitGaze(points, v, failed, nullptr, &notes, history));
        CHECK(near(v.offsetX, std::round(logged[run][0][0] * 1000) / 1000));
        CHECK(near(v.rollDeg, 0.0) && near(v.gainX, 1.0) && near(v.gainDown, 1.0));
        const double up = std::round(kUpDown / (logged[run][1][1] - logged[run][0][1]) * 100) / 100;
        CHECK(near(notes.gainUp, up) && notes.upGainFits == run + 1);
        sum += up;
        CHECK(near(v.gainUp, std::round(sum / (run + 1) * 100) / 100));
        history = nextUpGains(history, notes.gainUp);
    }
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
    CHECK(near(a.values.gainX, 1.0) && near(a.values.gainUp, 0.9) && near(a.values.gainDown, 1.0));
    CHECK(near(a.values.rollDeg, 0.0) && a.values.hasLids && near(a.values.lidClosed[1], 0.26));
    CHECK(id == 7);
    // The gains the dots gave, and what was written
    CHECK(a.log == "gains measured: sideways 0.93, up 0.90, down 0.88; written: up 0.90 (mean of 1 fit), "
                   "sideways and down 1, no tilt\npupil map not made: the left eye's pupil at the center dot: 0 fresh "
                   "samples (needs 30)");
    CHECK(!a.values.hasPupilMap && !a.values.hasPupilCenter);
    CHECK(near(s.view().notes.gainUp, 0.9) && s.view().notes.upGainFits == 1);
    // Done stays done
    a = s.tick(now + 1, kClosed, runningWith(id, true, points[5], false));
    CHECK(!a.writeValues && !a.writeCapture);
}

/**
 * Take a whole fit through all its steps.
 * @param s the session (started)
 * @param points the captures
 * @return the actions at the end
 */
Actions runFull(Session& s, const Measured points[kPointCount]) {
    double now = 0.0;
    long long id = 0;
    for (int i = 0; i < 5; ++i) runStep(s, now, id, points[i]);
    runStep(s, now, id, points[static_cast<int>(Point::Closed)], kCloseSettleSec, false);
    now += kReopenSec;
    return s.tick(now, kClosed, runningWith(0, false, {}));
}

void testFullSessionUpGains() {
    // The up gains of the fits before (panel.json) are averaged in, and said so in the log
    Measured points[kPointCount];
    fivePoints(points);
    Session s;
    s.start(Mode::Full, Values(), 0.0, {0.8, 0.85});
    const Actions a = runFull(s, points);
    CHECK(a.writeValues && near(a.values.gainUp, 0.85) && s.view().notes.upGainFits == 3);
    CHECK(near(s.view().notes.gainUp, 0.9));
    CHECK(a.log.find("written: up 0.85 (mean of 3 fits)") != std::string::npos);
    // Each eye's numbers don't come into it: a capture's per-eye x is not read
    CHECK(s.view().count == 6 && s.view().mode == Mode::Full);
}

void testCenterSession() {
    Session s;
    double now = 0.0;
    long long id = 0;
    Values current;
    current.gainUp = 1.4;
    current.rollDeg = 2.0;
    current.hasLids = true;
    s.start(Mode::Center, current, now);
    CHECK(s.view().count == 1);
    // Re-centering needs no openness
    Measured gazeOnly = steady(0.05, -0.12);
    gazeOnly.hasOpenness = false;
    const Actions a = runStep(s, now, id, gazeOnly);
    CHECK(a.writeValues && s.view().phase == Phase::Done);
    CHECK(near(a.values.offsetX, 0.05) && near(a.values.offsetY, -0.12) && near(a.values.gainUp, 1.4));
    // Only the zero point: whatever else there is (a tilt set by hand) stays, and nothing is logged but the try
    CHECK(a.values.hasLids && near(a.values.rollDeg, 2.0));
    CHECK(a.log.rfind("center try 1: ", 0) == 0 && a.log.find('\n') == std::string::npos);
    CHECK(pointCount(Mode::Center) == 1 && pointAt(Mode::Center, 3) == Point::Center);
}

void testPupilMap() {
    const double ipd = 0.064;
    // Each eye's direction to a target from its own place: sideways as eyeAngle, up / down from the eye
    for (int eye = 0; eye < 2; ++eye) {
        double direction[2];
        eyeDirection(target(Point::Right), eye, ipd, direction);
        CHECK(near(direction[0], eyeAngle(kSideDeg, eye, ipd) * kFullScaleDeg) && near(direction[1], 0.0));
        eyeDirection(target(Point::Up), eye, ipd, direction);
        // (sideways over the forward distance, as frameeyeosc's gaze angles are: shorter looking up)
        const double side = std::atan2(eye == 0 ? ipd / 2 : -ipd / 2, kTargetDistanceM * std::cos(kUpDownDeg * M_PI / 180));
        CHECK(near(direction[1], kUpDownDeg) && near(direction[0], side * 180 / M_PI));
    }
    // Five dots seen exactly through a made-up map: the map comes back, and points at each eye's targets
    Measured points[kPointCount];
    fivePoints(points);
    double none[2] = {0.0, 0.0};
    withPupils(points, ipd, none);
    PupilMap maps[2];
    double rms[2] = {-1, -1};
    std::string why;
    CHECK(fitPupilMaps(points, ipd, maps, rms, why) && why.empty());
    for (int eye = 0; eye < 2; ++eye) {
        const PupilMap want = madeUpMap(eye);
        for (int i = 0; i < 3; ++i) {
            CHECK(std::fabs(maps[eye].yaw[i] - want.yaw[i]) < 1e-6 && std::fabs(maps[eye].pitch[i] - want.pitch[i]) < 1e-6);
        }
        CHECK(rms[eye] < 1e-9);
    }
    // Its centres from the center dot: 0 here; a re-center after the headset moved 3 deg right and 2 down brings the
    // mapped centre back onto the target
    double centers[2][2];
    CHECK(pupilCenters(points[static_cast<int>(Point::Center)], maps, ipd, centers, why));
    CHECK(std::fabs(centers[0][0]) < 1e-9 && std::fabs(centers[1][1]) < 1e-9);
    Measured moved[kPointCount];
    fivePoints(moved);
    double shift[2] = {3.0, -2.0};
    withPupils(moved, ipd, shift);
    CHECK(pupilCenters(moved[static_cast<int>(Point::Center)], maps, ipd, centers, why));
    for (int eye = 0; eye < 2; ++eye) {
        CHECK(std::fabs(centers[eye][0] - 3.0) < 1e-9 && std::fabs(centers[eye][1] + 2.0) < 1e-9);
        // ...and then every dot of that wear lands on its target
        double wanted[2], mapped[2];
        eyeDirection(target(Point::Left), eye, ipd, wanted);
        applyPupilMap(maps[eye], moved[static_cast<int>(Point::Left)].pupil[eye], mapped);
        CHECK(std::fabs(mapped[0] + centers[eye][0] - wanted[0]) < 1e-9);
    }
    // Not used: too few fresh samples at a dot, a pupil that hardly moves, a map that misses its dots
    Measured few[kPointCount];
    fivePoints(few);
    withPupils(few, ipd, none);
    few[static_cast<int>(Point::Down)].pupilFresh[1] = kMinPupilFresh - 1;
    maps[0].yaw[0] = 99;
    CHECK(!fitPupilMaps(few, ipd, maps, rms, why) && near(maps[0].yaw[0], 99));
    CHECK(why == "the right eye's pupil at the down dot: 29 fresh samples (needs 30)");
    Measured still[kPointCount];
    fivePoints(still);
    withPupils(still, ipd, none);
    for (Point point : {Point::Up, Point::Down, Point::Left, Point::Right}) {
        for (int k = 0; k < 2; ++k) still[static_cast<int>(point)].pupil[0][k] = still[static_cast<int>(Point::Center)].pupil[0][k];
    }
    CHECK(!fitPupilMaps(still, ipd, maps, rms, why) && why.find("hardly moved") != std::string::npos);
    Measured off[kPointCount];
    fivePoints(off);
    withPupils(off, ipd, none);
    off[static_cast<int>(Point::Center)].pupil[1][1] += 0.08;  // about 18 deg off at one dot
    CHECK(!fitPupilMaps(off, ipd, maps, rms, why) && why.find("misses its dots") != std::string::npos);
    Measured noPupil[kPointCount];
    fivePoints(noPupil);
    CHECK(!pupilCenters(noPupil[static_cast<int>(Point::Center)], maps, ipd, centers, why));
    CHECK(why == "the left eye's pupil at the center dot: 0 fresh samples (needs 30)");

    // The whole fit's session: the map and its centre are in what it writes, and in the log
    Session s;
    s.start(Mode::Full, Values(), 0.0, {}, ipd);
    Actions a = runFull(s, points);
    CHECK(a.writeValues && a.values.hasPupilMap && a.values.hasPupilCenter);
    CHECK(std::fabs(a.values.pupilMap[1].pitch[2] - madeUpMap(1).pitch[2]) < 1e-6);
    CHECK(a.log.find("\npupil map: fits its dots within 0.00° (L) and 0.00° (R) rms; center L ") != std::string::npos);
    // ...a whole fit whose pupils can't be used keeps the map it had
    Values current;
    current.hasPupilMap = true;
    current.pupilMap[0] = madeUpMap(0);
    current.pupilMap[1] = madeUpMap(1);
    current.hasPupilCenter = true;
    current.pupilCenter[0][0] = 1.25;
    s.start(Mode::Full, current, 0.0, {}, ipd);
    a = runFull(s, few);
    CHECK(a.writeValues && a.values.hasPupilMap && near(a.values.pupilMap[0].yaw[0], -42.0));
    CHECK(near(a.values.pupilCenter[0][0], 1.25));
    CHECK(a.log.find("\npupil map kept as it was: the right eye's pupil at the down dot") != std::string::npos);
    // A re-center with the map: the centre for this wear, after the try's line; without pupils it stays
    s.start(Mode::Center, current, 0.0, {}, ipd);
    double t = 0.0;
    long long id = 0;
    a = runStep(s, t, id, moved[static_cast<int>(Point::Center)]);
    CHECK(a.writeValues && a.values.hasPupilCenter && std::fabs(a.values.pupilCenter[1][0] - 3.0) < 1e-6);
    CHECK(a.log.rfind("center try 1: ", 0) == 0 && a.log.find("\npupil center: L +3.00 -2.00, R +3.00 -2.00 deg") !=
          std::string::npos);
    t = 0.0;
    s.start(Mode::Center, current, 0.0, {}, ipd);
    a = runStep(s, t, id, noPupil[static_cast<int>(Point::Center)]);
    CHECK(a.writeValues && near(a.values.pupilCenter[0][0], 1.25));
    CHECK(a.log.find("\npupil center kept as it was: the left eye's pupil at the center dot") != std::string::npos);
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
        s.start(Mode::Center, Values(), 0.0);
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

/**
 * A capture whose right eye Valve read stuck at 0 while the eye camera saw it open (frameeyeosc's camera_open).
 * @param m the capture as it would have been
 * @return the capture
 */
Measured rightStuck(Measured m) {
    m.openness[1] = 0.0;
    m.cameraOpen[1] = true;
    return m;
}

void testStandIn() {
    // 2026-10-11: Valve read the right eye 0.000 for a whole wear while it was open. frameeyeosc let the gaze count
    // because the eye camera saw it open, and says so per capture; shut, the camera sees the eye closed, so that
    // capture isn't flagged, and Valve's 0 is no use either
    Measured points[kPointCount];
    fivePoints(points);
    for (int i = 0; i < 5; ++i) points[i] = rightStuck(points[i]);
    points[static_cast<int>(Point::Closed)] = shut(0.15, 0.0);
    bool standIn[2] = {true, false};
    standIns(points, standIn);
    CHECK(!standIn[0] && standIn[1]);
    // The side dots don't count (the lid fit doesn't read them); any one of the others does
    Measured upOnly[kPointCount];
    fivePoints(upOnly);
    upOnly[static_cast<int>(Point::Left)].cameraOpen[0] = true;
    standIns(upOnly, standIn);
    CHECK(!standIn[0] && !standIn[1]);
    upOnly[static_cast<int>(Point::Up)].cameraOpen[1] = true;
    standIns(upOnly, standIn);
    CHECK(!standIn[0] && standIn[1]);

    // The eyes-shut step checks only the left eye then: the right eye's "open" was Valve's 0 too
    const Measured& center = points[0];
    const bool rightOnly[2] = {false, true};
    CHECK(!usableClosed(points[5], center));
    CHECK(usableClosed(points[5], center, rightOnly));
    CHECK(!usableClosed(shut(0.85, 0.0), center, rightOnly));  // the left eye stayed open
    // Not flagged, the same as before
    const bool none[2] = {false, false};
    CHECK(usableClosed(shut(), points[0], none) == usableClosed(shut(), points[0]));

    // The lid fit leaves the right eye as it was (here a fit from before) and fits the left one
    Values v;
    v.lidClosed[1] = 0.2;
    v.lidUp[1] = 0.9;
    v.lidOpen[1] = 0.85;
    v.lidDown[1] = 0.7;
    FailureDetail detail;
    CHECK(fitLids(points, v, &detail) && v.hasLids && !v.lidsKept[0] && v.lidsKept[1]);
    CHECK(near(v.lidClosed[0], 0.15) && near(v.lidUp[0], 0.93) && near(v.lidOpen[0], 0.92) && near(v.lidDown[0], 0.77));
    CHECK(near(v.lidClosed[1], 0.2) && near(v.lidUp[1], 0.9) && near(v.lidOpen[1], 0.85) && near(v.lidDown[1], 0.7));
    // Unflagged, Valve's 0 fails the right eye's range check, as it would have
    Measured unflagged[kPointCount];
    for (int i = 0; i < kPointCount; ++i) {
        unflagged[i] = points[i];
        unflagged[i].cameraOpen[1] = false;
    }
    Values lids;
    CHECK(!fitLids(unflagged, lids, &detail) && detail.eye == 1 && !lids.hasLids);
    // ...and the left eye is still checked with the right one flagged
    Measured leftShallow[kPointCount];
    for (int i = 0; i < kPointCount; ++i) leftShallow[i] = points[i];
    leftShallow[static_cast<int>(Point::Closed)] = shut(0.85, 0.0);
    CHECK(!fitLids(leftShallow, lids, &detail) && detail.eye == 0);
    // An ordinary fit keeps nothing
    Measured ordinary[kPointCount];
    fivePoints(ordinary);
    CHECK(fitLids(ordinary, lids) && !lids.lidsKept[0] && !lids.lidsKept[1]);

    // The try's log line says which eye
    CHECK(tryText(Point::Center, 1, points[0], Measured(), "ok") ==
          "center try 1: 120 samples (min 45), R open on the eye camera, spread 0.5° (max 2.7°) -> ok");
    CHECK(tryText(Point::Closed, 1, points[5], center, "ok") ==
          "closed try 1: 130 samples (min 45), openness L 0.150 R 0.000 (below 0.644 / -) -> ok");
    Measured both = points[0];
    both.cameraOpen[0] = true;
    CHECK(tryText(Point::Center, 1, both, Measured(), "ok").find(", L and R open on the eye camera, ") !=
          std::string::npos);
    // Unflagged, as before
    Measured ordinaryCenter = points[0];
    ordinaryCenter.openness[1] = 0.81;
    ordinaryCenter.cameraOpen[1] = false;
    CHECK(tryText(Point::Closed, 1, shut(), ordinaryCenter, "ok") ==
          "closed try 1: 130 samples (min 45), openness L 0.150 R 0.260 (below 0.644 / 0.567) -> ok");

    // A whole fit: done, the right eye's lid fit and scale kept, and the notes say which eye
    Session s;
    Values current;
    current.hasLids = true;
    current.lidClosed[1] = 0.2;
    current.lidOpen[1] = 0.85;
    s.start(Mode::Full, current, 0.0);
    Actions a = runFull(s, points);
    CHECK(a.writeValues && s.view().phase == Phase::Done);
    CHECK(a.values.hasLids && !a.values.lidsKept[0] && a.values.lidsKept[1]);
    CHECK(near(a.values.lidClosed[0], 0.15) && near(a.values.lidClosed[1], 0.2) && near(a.values.lidOpen[1], 0.85));
    CHECK(!s.view().notes.standIn[0] && s.view().notes.standIn[1]);
    // An ordinary one has no stand-in
    Measured plain[kPointCount];
    fivePoints(plain);
    s.start(Mode::Full, current, 0.0);
    a = runFull(s, plain);
    CHECK(a.writeValues && !a.values.lidsKept[0] && !a.values.lidsKept[1]);
    CHECK(!s.view().notes.standIn[0] && !s.view().notes.standIn[1]);

    // Re-centering: the zero point as usual, and the notes say which eye
    double now = 0.0;
    long long id = 0;
    s.start(Mode::Center, Values(), now);
    a = runStep(s, now, id, points[0]);
    CHECK(a.writeValues && near(a.values.offsetX, 0.012) && !s.view().notes.standIn[0] && s.view().notes.standIn[1]);

    // Read from status.json's gaze_capture; a frameeyeosc before 0.7.7 doesn't say, which reads as neither
    const char* withFlag =
        "{\"version\":1,\"pid\":1,\"time\":1791200000,\"gaze_capture\":{\"id\":3,\"target\":\"center\",\"state\":\"done\","
        "\"x\":0.01,\"y\":-0.02,\"spread\":0.01,\"x_left\":0.0,\"x_right\":0.0,\"openness\":[0.98,0.0],"
        "\"camera_open\":[false,true],\"samples\":125,\"received\":135,\"rate\":90.0}}";
    const EyeStatus flagged = parseStatus(withFlag, 1791200000, false);
    CHECK(flagged.capture.present && flagged.capture.done && flagged.capture.hasOpenness);
    CHECK(!flagged.capture.cameraOpen[0] && flagged.capture.cameraOpen[1]);
    const EyeStatus older = parseStatus(
        "{\"version\":1,\"pid\":1,\"time\":1791200000,\"gaze_capture\":{\"id\":3,\"target\":\"center\",\"state\":\"done\","
        "\"openness\":[0.98,0.0],\"samples\":0,\"received\":135}}",
        1791200000, false);
    CHECK(older.capture.present && !older.capture.cameraOpen[0] && !older.capture.cameraOpen[1]);

    // The eyes never shut with the right eye flagged: only the left eye's limit is in the numbers (from 0 again, as
    // the other runs: runStep's steps come out exact there)
    now = 0.0;
    s.start(Mode::Full, Values(), now);
    for (int i = 0; i < 5; ++i) runStep(s, now, id, points[i]);
    for (int i = 0; i < kMaxAttempts; ++i) runStep(s, now, id, shut(0.85, 0.0), kCloseSettleSec, false);
    CHECK(s.view().failure == Failure::NotClosed);
    CHECK(near(s.view().detail.closedBelow[0], 0.7 * 0.92) && std::isnan(s.view().detail.closedBelow[1]));
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testFit();
    testUpGains();
    testLoggedCaptures();
    testCenterAndUsable();
    testStandIn();
    testFullSession();
    testFullSessionUpGains();
    testCenterSession();
    testPupilMap();
    testFailures();
    if (gFailures == 0) std::printf("gaze-fit-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
