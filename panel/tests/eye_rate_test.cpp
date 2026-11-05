// Tests for SteamVR's eye camera speed (eye_rate.cpp): when the left column offers "Set 90" (only for a slow eye
// tracker, a SteamVR with the setting, below 90), what pressing it shows (the confirmation, then "not taken" if the
// rate stays low, and the offer again), the row kept for those, and which choice "Having trouble" shows. Built with
// the panel as eye-rate-test; exits non-zero on failure.
#include "eye_rate.h"

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

using eye_rate::Line;
using eye_rate::View;

/** How a status's eye data rate came about. */
enum class Rate { Fine, SlowTracker, SlowHere, NotTracking };

/**
 * A status: frameeyeosc running, its eye data rate as asked.
 * @param rate fine (90), slow from the eye tracker (15, nothing missed), slow here (46, 44 missed), or not tracking
 * @return the status
 */
EyeStatus status(Rate rate) {
    EyeStatus s;
    s.present = s.running = true;
    s.tracking = rate != Rate::NotTracking;
    s.trackerRate = rate == Rate::Fine ? 89.6 : rate == Rate::SlowHere ? 46.0 : 15.0;
    s.missedRate = rate == Rate::SlowHere ? 44.0 : 0.0;
    s.maxProcessingMs = 1.4;
    s.droppedRate = 0.0;
    return s;
}

/**
 * A state with the setting as SteamVR has it.
 * @param min eyeTrackingRateMin (NaN: a SteamVR without the setting)
 * @return the state
 */
View withMin(double min) {
    View v;
    v.setting.exists = std::isfinite(min);
    v.setting.min = min;
    return v;
}

/** The offer: only for the eye tracker being slow, with the setting there and below 90. */
void testOffered() {
    const EyeStatus slow = status(Rate::SlowTracker);
    CHECK(trackerRateCause(slow) == TrackerRateCause::Tracker);
    CHECK(eye_rate::offered(withMin(15), slow));
    CHECK(eye_rate::offered(withMin(89.5), slow));
    // At 90 or above, or without the setting (an older SteamVR): today's line only
    CHECK(!eye_rate::offered(withMin(90), slow));
    CHECK(!eye_rate::offered(withMin(120), slow));
    CHECK(!eye_rate::offered(withMin(NAN), slow));
    // frameeyeosc itself slow, the rate fine, or nothing tracked: no offer
    CHECK(trackerRateCause(status(Rate::SlowHere)) == TrackerRateCause::Here);
    CHECK(!eye_rate::offered(withMin(15), status(Rate::SlowHere)));
    CHECK(!eye_rate::offered(withMin(15), status(Rate::Fine)));
    CHECK(!eye_rate::offered(withMin(15), status(Rate::NotTracking)));
    EyeStatus stopped = slow;
    stopped.running = false;
    CHECK(!eye_rate::offered(withMin(15), stopped));
    // An older frameeyeosc without the missed samples: the cause can't be told, no offer
    EyeStatus old = slow;
    old.missedRate = NAN;
    CHECK(!eye_rate::offered(withMin(15), old));

    // update sets the line the same way
    View v = withMin(15);
    CHECK(eye_rate::update(v, slow, 10.0) && v.line == Line::Offer);
    CHECK(!eye_rate::update(v, slow, 10.1) && v.line == Line::Offer);
    CHECK(eye_rate::update(v, status(Rate::SlowHere), 10.2) && v.line == Line::None);
    View none = withMin(NAN);
    CHECK(!eye_rate::update(none, slow, 10.0) && none.line == Line::None);
}

/** Pressed, and SteamVR took it: the confirmation for kCheckSec (even once the rate is fine), then nothing. */
void testPressedTaken() {
    View v = withMin(15);
    eye_rate::update(v, status(Rate::SlowTracker), 100.0);
    eye_rate::pressed(v, true, 100.0);
    v.setting.min = 90;  // read back
    CHECK(eye_rate::update(v, status(Rate::SlowTracker), 100.1) && v.line == Line::Done);
    // The rate fine a second later: the confirmation stays, and keeps its row though the reason has gone
    CHECK(!eye_rate::update(v, status(Rate::Fine), 101.5) && v.line == Line::Done);
    CHECK(eye_rate::rowShown(v, TrackerRateCause::None));
    CHECK(!eye_rate::update(v, status(Rate::Fine), 100.0 + eye_rate::kCheckSec - 0.01) && v.line == Line::Done);
    // Its time over with the rate fine: nothing (no reason line either)
    CHECK(eye_rate::update(v, status(Rate::Fine), 100.0 + eye_rate::kCheckSec) && v.line == Line::None);
    CHECK(!eye_rate::rowShown(v, TrackerRateCause::None));
    CHECK(!v.refused && v.pressedAt < 0);
}

/** Pressed, but the rate stays low: "not taken" for kRefusedSec, then the offer again, even with the setting at 90. */
void testPressedRefused() {
    View v = withMin(15);
    const EyeStatus slow = status(Rate::SlowTracker);
    eye_rate::update(v, slow, 200.0);
    eye_rate::pressed(v, true, 200.0);
    v.setting.min = 90;
    eye_rate::update(v, slow, 201.0);
    CHECK(v.line == Line::Done);
    CHECK(eye_rate::update(v, slow, 200.0 + eye_rate::kCheckSec + 0.05) && v.line == Line::Refused);
    CHECK(v.refused && eye_rate::rowShown(v, TrackerRateCause::None));
    // (timed from the check, not from when the loop happened to look)
    CHECK(!eye_rate::update(v, slow, 200.0 + eye_rate::kCheckSec + eye_rate::kRefusedSec - 0.01) &&
          v.line == Line::Refused);
    CHECK(eye_rate::update(v, slow, 200.0 + eye_rate::kCheckSec + eye_rate::kRefusedSec) && v.line == Line::Offer);
    CHECK(eye_rate::offered(v, slow));
    // Pressed again: the confirmation again
    eye_rate::pressed(v, true, 300.0);
    CHECK(eye_rate::update(v, slow, 300.0) && v.line == Line::Done);
    // Once the rate is fine, the offer at 90 is gone for the next time it is low
    eye_rate::update(v, status(Rate::Fine), 306.0);
    CHECK(v.line == Line::None && !v.refused);
    CHECK(!eye_rate::update(v, slow, 307.0) && v.line == Line::None);
}

/** The write failing: "not taken" at once, then the offer again. */
void testWriteFailed() {
    View v = withMin(15);
    const EyeStatus slow = status(Rate::SlowTracker);
    eye_rate::update(v, slow, 50.0);
    eye_rate::pressed(v, false, 50.0);
    CHECK(eye_rate::update(v, slow, 50.0) && v.line == Line::Refused);
    CHECK(eye_rate::update(v, slow, 50.0 + eye_rate::kRefusedSec) && v.line == Line::Offer);
}

/** The rate low only for frameeyeosc's own reason after the press: the check still says "not taken" (it is low). */
void testRefusedWhileOtherCause() {
    View v = withMin(15);
    eye_rate::pressed(v, true, 10.0);
    eye_rate::update(v, status(Rate::SlowHere), 15.0);
    CHECK(v.line == Line::Refused);
    // ...but the offer itself only comes back for the eye tracker being slow
    eye_rate::update(v, status(Rate::SlowHere), 15.0 + eye_rate::kRefusedSec);
    CHECK(v.line == Line::None);
    // Not tracking at the check: nothing to tell, so not "not taken"
    View w = withMin(15);
    eye_rate::pressed(w, true, 10.0);
    eye_rate::update(w, status(Rate::NotTracking), 15.0);
    CHECK(w.line == Line::None && !w.refused);
}

/** "Having trouble"'s choice: 90 or more is "Always 90", anything lower SteamVR's. */
void testChoice() {
    eye_rate::Setting s;
    s.exists = true;
    s.min = 90;
    CHECK(eye_rate::choice(s) == eye_rate::Choice::Full);
    s.min = 120;
    CHECK(eye_rate::choice(s) == eye_rate::Choice::Full);
    s.min = 89.9;
    CHECK(eye_rate::choice(s) == eye_rate::Choice::SteamVr);
    s.min = 15;
    CHECK(eye_rate::choice(s) == eye_rate::Choice::SteamVr);
}

/** The row under "Eye data": with a reason, or while the confirmation or "not taken" shows. */
void testRowShown() {
    View v = withMin(15);
    CHECK(!eye_rate::rowShown(v, TrackerRateCause::None));
    CHECK(eye_rate::rowShown(v, TrackerRateCause::Here));
    CHECK(eye_rate::rowShown(v, TrackerRateCause::Tracker));
    v.line = Line::Offer;
    CHECK(!eye_rate::rowShown(v, TrackerRateCause::None));
    v.line = Line::Done;
    CHECK(eye_rate::rowShown(v, TrackerRateCause::None));
    v.line = Line::Refused;
    CHECK(eye_rate::rowShown(v, TrackerRateCause::None));
}

/** The signature changes with what is drawn: the setting there or not, its value, the line. */
void testSignature() {
    View a = withMin(15);
    View b = withMin(90);
    View c = withMin(NAN);
    CHECK(eye_rate::signature(a) != eye_rate::signature(b));
    CHECK(eye_rate::signature(a) != eye_rate::signature(c));
    View d = a;
    d.line = Line::Offer;
    CHECK(eye_rate::signature(a) != eye_rate::signature(d));
    CHECK(eye_rate::signature(withMin(15)) == eye_rate::signature(a));
    CHECK(a.setting == withMin(15).setting && a.setting != b.setting && c.setting == withMin(NAN).setting);
}

}  // namespace

int main() {
    testOffered();
    testPressedTaken();
    testPressedRefused();
    testWriteFailed();
    testRefusedWhileOtherCause();
    testChoice();
    testRowShown();
    testSignature();
    if (gFailures == 0) std::printf("eye-rate-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
