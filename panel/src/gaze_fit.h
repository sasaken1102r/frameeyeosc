// The eye fit (the "Eye fit" tab): where the targets are, how captured averages become the gaze zero point and
// gains and each eye's lid fit, and the step-by-step session the panel runs (with the dashboard open or closed).
// Nothing here talks to OpenVR or writes files, so it can be tested on its own (gaze_fit_test.cpp).
#pragma once

#include "status.h"

#include <cmath>
#include <string>

namespace gaze_fit {

/** The whole fit (five gaze points and the eyes-shut step); re-centering the gaze only (one dot); or re-centering and
 *  measuring the headset's tilt again (straight ahead, up and down), for putting the headset back on. */
enum class Mode { Full, Center, Tilt };

/** The steps, in the order the full fit shows them. */
enum class Point { Center, Up, Down, Left, Right, Closed };

/** How many steps there are at most. */
constexpr int kPointCount = 6;

/** A step's target as seen from the head. */
struct Target {
    Point point;
    const char* name;  ///< sent with the capture request and logged by frameeyeosc ("closed" = eyes shut)
    double yawDeg;     ///< degrees to the right
    double pitchDeg;   ///< degrees up
};

/** How far the side and the up / down targets are from straight ahead. */
constexpr double kSideDeg = 20.0;
constexpr double kUpDownDeg = 15.0;
/** The gaze angle that frameeyeosc sends as 1.0. */
constexpr double kFullScaleDeg = 45.0;
/** How far ahead the targets are shown (m). Each eye's own angle to a target depends on it (eyeAngle), so this is
 *  the distance they are really drawn at. Nearer than the open dashboard (about 1.35 m) so they show over it: plain
 *  overlays showed over it at 0.45-1.0 m, and at 1.2 m it hid them depending on where the head was (gaze_dots.h). */
constexpr double kTargetDistanceM = 0.9;
/** The distance between the eyes when SteamVR doesn't say (m). */
constexpr double kDefaultIpdM = 0.063;
/** A capture is used when at least this share of the samples that came in are usable (the eyes open and the gaze
 *  reliable)... The tracker's rate varies: 90-136 a second while streaming, and 15 has been seen, so no fixed count
 *  fits all... */
constexpr double kMinUsableShare = 0.6;
/** ...and there are at least this many (about 0.8 s at 15 a second)... */
constexpr int kMinSamplesFloor = 12;
/** ...but never more than this (0.5.2's fixed count, so the fit is no stricter at the usual rates), which is also
 *  the count from a frameeyeosc that does not say how many came in (before 0.5.3)... */
constexpr int kMinSamples = 45;
/** ...and its gaze spreads no more than this (on the -1..1 scale; 0.06 is about 2.7°). */
constexpr double kMaxSpread = 0.06;
/** The eyes-shut capture counts when each eye reads below this share of its straight-ahead open reading. */
constexpr double kClosedShare = 0.7;
/** Each eye's open readings must be at least this far above its closed one (frameeyeosc checks the same). */
constexpr double kMinLidRange = 0.1;
/** Tries per step before giving up. */
constexpr int kMaxAttempts = 3;
/** How long each gaze point takes in all: the dot glides over and the eyes find it, then it is measured. The one
 *  number that makes the fit faster or slower. */
constexpr double kPointSec = 2.5;
/** Of that, seconds a target shows before its capture is asked for, so the eyes can find it... */
constexpr double kSettleSec = 0.5;
/** ...of which the dot spends this long gliding over from the previous target. */
constexpr double kMoveSec = 0.35;
/** The first dot's first try settles this much longer: the fit starts at the button press, and the eyes are still
 *  on the button (later dots and tries are unchanged). */
constexpr double kFirstSettleExtraSec = 1.0;
/** The rest is measured by frameeyeosc... */
constexpr double kCaptureSec = kPointSec - kSettleSec;
/** ...skipping its first samples while the eyes settle on the dot. */
constexpr double kCaptureSkipSec = 0.3;
static_assert((kCaptureSec - kCaptureSkipSec) * 15 >= 2 * kMinSamplesFloor,
              "at 15 Hz, twice kMinSamplesFloor, so a blink still leaves enough");
/** The eyes-shut step: "close your eyes for 3 s" counts down 3, 2, 1 this long, then the capture is asked for... */
constexpr double kCloseSettleSec = 3.0;
/** ...which lasts this long, the time the eyes are shut (what the target says)... */
constexpr double kClosedSec = 3.0;
/** ...skipping its first half second while the eyes close. */
constexpr double kClosedSkipSec = 0.5;
/** After it, "open your eyes" shows this long before the result is written. */
constexpr double kReopenSec = 1.5;
/** Seconds to wait for a capture's result: frameeyeosc checks config.json every 0.1 s and gives up 3 s after the
 *  capture should have ended. */
constexpr double kResultTimeoutSec = 8.0;
/** With the dashboard open, how long it may show another page (not this panel's) before the fit stops. Long enough
 *  for the dashboard closing (the panel and the dashboard need not hide in the same frame). */
constexpr double kAwaySec = 0.5;
/** A side / up / down point must move at least this share of its target angle, the right way. */
constexpr double kMinMoveFraction = 0.25;
/** The allowed zero points and gains (the same as frameeyeosc's). */
constexpr double kOffsetLimit = 0.5;
constexpr double kGainMin = 0.5;
constexpr double kGainMax = 2.0;
/** The largest tilt written either way (degrees; the same as frameeyeosc's gaze_roll_deg). */
constexpr double kRollLimitDeg = 20.0;
/** The full fit takes the tilt from the up / down dots and from the side dots together when they are at most this far
 *  apart (degrees), and keeps the previous tilt otherwise. Four logged fits had them 1.5-5.8° apart. */
constexpr double kRollAgreeDeg = 5.0;
/** Re-centering with the tilt keeps the previous tilt when it measures one further from it than this (degrees): on
 *  2026-10-08 it read -2.5°, -9.0°, -10.1° and +3.0° within 12 minutes, after a full fit that read -2.5° and -2.9°.
 *  A headset tilted that much more needs the full fit. */
constexpr double kRollJumpDeg = 5.0;
/** How far Valve's left - right sideways gaze straight ahead may be from what the dot's distance makes it (degrees)
 *  before the fit says so (and the full fit keeps each eye's previous gain): on 2026-10-08, 20 captures of a dot that
 *  needed 4.4° read 0.1-17.3°. */
constexpr double kEyeSpreadToleranceDeg = 3.0;

/**
 * A step's target.
 * @param point which one
 * @return where it is
 */
const Target& target(Point point);

/**
 * How many steps a mode has.
 * @param mode the mode
 * @return 6, 1 or 3
 */
int pointCount(Mode mode);

/**
 * The step shown at a position.
 * @param mode the mode
 * @param index 0-based step
 * @return the step
 */
Point pointAt(Mode mode, int index);

/** One capture's result (the raw combined gaze and openness, before any correction or scale). */
struct Measured {
    double x = 0.0;
    double y = 0.0;
    double spread = 0.0;
    bool hasEyeX = false;         ///< xEye is set
    double xEye[2] = {0.0, 0.0};  ///< each eye's own raw sideways gaze, left / right
    double openness[2] = {0.0, 0.0};  ///< left, right
    bool hasOpenness = false;
    int samples = 0;    ///< usable samples averaged...
    int received = 0;   ///< ...of these that came in (0 = not reported)
    double rateHz = NAN;  ///< the tracker's rate over the capture
};

/**
 * How many usable samples a capture needs: kMinUsableShare of those that came in, at least kMinSamplesFloor and at
 * most kMinSamples (kMinSamples if frameeyeosc did not say how many came in).
 * @param measured the capture
 * @return the count
 */
int samplesNeeded(const Measured& measured);

/** The numbers behind a failure, for the panel and the log. */
struct FailureDetail {
    int tries = 0;                      ///< Unsteady / NotClosed: the tries made...
    Measured last;                      ///< ...and the last one
    double closedBelow[2] = {NAN, NAN};  ///< NotClosed: each eye had to read below this (kClosedShare of ahead)
    int eye = -1;                       ///< NoMovement: -1 = the gaze, 0 / 1 = that eye's sideways fit; NoLidRange: the eye
    double movedDeg = NAN;              ///< NoMovement: how far it moved its way...
    double neededDeg = NAN;             ///< ...and how far it had to
    Point lidPoint = Point::Center;     ///< NoLidRange: the open reading nearest the shut one...
    double lidOpen = NAN;               ///< ...that reading (NaN if there was none)...
    double lidClosed = NAN;             ///< ...and the shut one
};

/**
 * One try's numbers for the log, e.g. "center try 2: 128 samples (min 45), spread 3.4° (max 2.7°) -> again".
 * @param point the step
 * @param attempt the try (1-based)
 * @param measured its capture
 * @param center the straight-ahead capture (for the eyes-shut step's limits)
 * @param outcome "ok", "again" or "failed"
 * @return the line
 */
std::string tryText(Point point, int attempt, const Measured& measured, const Measured& center, const char* outcome);

/**
 * Whether a gaze capture is steady and long enough to use.
 * @param measured the capture
 * @return true if usable
 */
bool usable(const Measured& measured);

/**
 * Whether the eyes-shut capture is long enough and the eyes were shut.
 * @param closed the eyes-shut capture
 * @param center the straight-ahead capture (its openness is "open")
 * @return true if usable
 */
bool usableClosed(const Measured& closed, const Measured& center);

/** What a fit noticed about its own captures and did about it, for the result and the log. */
struct Notes {
    double eyeSpreadDeg = NAN;          ///< Valve's left - right sideways gaze at the straight-ahead dot (NaN: no per-eye x)
    double eyeSpreadExpectedDeg = NAN;  ///< what the dot's distance and the IPD make it
    bool eyeSpreadOff = false;          ///< more than kEyeSpreadToleranceDeg apart
    bool eyeGainsKept = false;          ///< the full fit kept each eye's previous gain because of that
    double rollUpDownDeg = NAN;         ///< the tilt from the up / down dots (Tilt and Full)
    double rollSidesDeg = NAN;          ///< the tilt from the side dots (Full)
    bool rollKept = false;              ///< the tilt measured was not used: the previous one was kept...
    bool rollDisagreed = false;         ///< ...because up / down and the sides disagreed (Full), else it jumped (Tilt)
    double rollPreviousDeg = NAN;       ///< the tilt before
};

/** The settings a fit writes. */
struct Values {
    double offsetX = 0.0;
    double offsetY = 0.0;
    double gainX = 1.0;
    double gainUp = 1.0;
    double gainDown = 1.0;
    double rollDeg = 0.0;  ///< the headset's tilt, undone around the zero point before the gains (gaze_roll_deg)
    bool hasEyeX = false;  ///< each eye's own sideways zero point and gain below are set
    double eyeOffsetX[2] = {0.0, 0.0};  ///< left, right
    double eyeGainX[2] = {1.0, 1.0};
    bool hasLids = false;  ///< the eyelid readings below are set
    double lidClosed[2] = {0.0, 0.0};  ///< left, right
    double lidUp[2] = {0.0, 0.0};
    double lidOpen[2] = {0.0, 0.0};
    double lidDown[2] = {0.0, 0.0};
};

/**
 * Where an eye really looks, on the -1..1 scale, to see a target that is `yawDeg` right of straight ahead at
 * kTargetDistanceM: its angle from that eye, not from between the eyes. +x is right. The left eye sits ipd/2 to
 * the left, so it turns right a little to see a target straight ahead, and the right eye turns left.
 * @param yawDeg the target's angle from between the eyes
 * @param eye 0 = left, 1 = right
 * @param ipd the distance between the eyes (m)
 * @return the angle (1.0 = 45°)
 */
double eyeAngle(double yawDeg, int eye, double ipd);

/**
 * The headset's tilt from the up / down dots: tilted by θ, the move from the down dot to the up one leans the other
 * way (dx = -sinθ·dy). The re-wear fit uses only these dots; they gave a steadier tilt than the side dots between
 * fits (+2.0°, +2.4°, +1.7° against -3.8°, +2.7°, -2.1°), but a per-eye gaze that turns in looking down shifts them
 * too (fullFitRoll, kRollJumpDeg).
 * @param up the up capture
 * @param down the down capture
 * @return the setting (within ±kRollLimitDeg, 0.1° steps); positive: looking right reads higher
 */
double rollFromUpDown(const Measured& up, const Measured& down);

/**
 * The tilt as the side dots see it (the angle of the line from the left dot to the right one), for fullFitRoll.
 * @param left the left capture
 * @param right the right capture
 * @return degrees, not rounded
 */
double rollFromSides(const Measured& left, const Measured& right);

/**
 * A raw gaze relative to the zero point, with the tilt undone (what frameeyeosc does before the gains).
 * @param dx x minus the sideways zero point
 * @param dy y minus the up / down zero point
 * @param rollDeg the tilt
 * @param x the level x
 * @param y the level y
 */
void level(double dx, double dy, double rollDeg, double& x, double& y);

/**
 * Valve's left - right sideways gaze at the straight-ahead dot against what it has to be, into notes.
 * @param center the straight-ahead capture
 * @param ipd the distance between the eyes (m)
 * @param notes where it goes (nothing without per-eye x)
 */
void noteEyeSpread(const Measured& center, double ipd, Notes& notes);

/**
 * Each eye's own sideways gain, from how far its own x moved between the side dots against how far it had to (see
 * eyeAngle). Both eyes get the shared zero point (out.offsetX): the Frame's per-eye x straight ahead says more about
 * where its estimate of how far away you look happened to be than about the eye (a dot needing 4.4° between the eyes
 * read 0.1-17.3°), and frameeyeosc turns the eyes in by a fixed amount instead. When Valve's left - right straight
 * ahead is more than kEyeSpreadToleranceDeg off, each eye keeps the gain it had (`previous`, if it had one; else the
 * shared gain). Needs every gaze point's per-eye x; otherwise nothing is set. Uses the tilt already in `out` (fitGaze
 * first).
 * @param points the captures, indexed by Point
 * @param ipd the distance between the eyes (m)
 * @param out where they go (hasEyeX set when fitted)
 * @param detail on failure, which eye and how far it moved (may be null)
 * @param previous the settings before the fit, for the gains kept (may be null)
 * @param notes what was noticed (may be null)
 * @return false if an eye did not move far enough the right way between the side targets
 */
bool fitEyes(const Measured points[kPointCount], double ipd, Values& out, FailureDetail* detail = nullptr,
             const Values* previous = nullptr, Notes* notes = nullptr);

/**
 * The zero point from the center capture; everything else (the tilt too) stays as it is. Each eye's own zero point,
 * when there is one, becomes the shared one, keeping its gain.
 * @param center the center capture
 * @param current the settings now
 * @param ipd the distance between the eyes (m)
 * @param notes what was noticed (may be null)
 * @return the new settings (rounded, within range)
 */
Values fitCenter(const Measured& center, const Values& current, double ipd = kDefaultIpdM, Notes* notes = nullptr);

/**
 * The tilt a full fit uses: the mean of rollFromUpDown and rollFromSides when they are at most kRollAgreeDeg apart,
 * else the previous one.
 * @param points the captures, indexed by Point
 * @param previousDeg the tilt before the fit
 * @param notes what was noticed (may be null)
 * @return the tilt (within ±kRollLimitDeg, 0.1° steps)
 */
double fullFitRoll(const Measured points[kPointCount], double previousDeg, Notes* notes = nullptr);

/**
 * The zero point, the tilt (fullFitRoll, with out.rollDeg as the previous one) and the three gains from the five
 * gaze captures. Every point is leveled around the center first; each gain then makes the target angle come out as
 * that angle: gain = target / (point - center), with left and right averaged into one gain.
 * @param points the captures, indexed by Point
 * @param out the settings before the fit in; the new gaze settings out (rounded, within range), the lid readings
 *            left alone
 * @param failed the first point that did not move far enough the right way
 * @param notes what was noticed (may be null)
 * @return false if a point did not move far enough the right way
 */
bool fitGaze(const Measured points[kPointCount], Values& out, Point& failed, FailureDetail* detail = nullptr,
             Notes* notes = nullptr);

/**
 * The re-wear fit: the zero point from the center capture and the tilt from the up / down captures, unless that is
 * more than kRollJumpDeg from the tilt now (then the tilt now stays); each eye's own zero point becomes the shared one
 * with its gain kept. The gains and the eyelids stay as they are. The up / down dots must move as far as in the full
 * fit.
 * @param points the captures, indexed by Point (Center, Up and Down used)
 * @param current the settings now
 * @param ipd the distance between the eyes (m)
 * @param out the new settings (rounded, within range)
 * @param failed the point that did not move far enough the right way
 * @param detail on failure, how far it moved (may be null)
 * @param notes what was noticed (may be null)
 * @return false if the up or the down point did not move far enough
 */
bool fitTilt(const Measured points[kPointCount], const Values& current, double ipd, Values& out, Point& failed,
             FailureDetail* detail = nullptr, Notes* notes = nullptr);

/**
 * Each eye's lid fit: the eyes-shut reading, and the open readings looking up, straight ahead and down.
 * @param points the captures, indexed by Point (Closed included)
 * @param out where the lid readings go (hasLids set)
 * @param detail on failure, which eye and reading (may be null)
 * @return false if an eye's open readings are not at least kMinLidRange above its closed one
 */
bool fitLids(const Measured points[kPointCount], Values& out, FailureDetail* detail = nullptr);

/** Where a session is. */
enum class Phase {
    Idle,       ///< nothing going on (maybe showing the last result)
    Settling,   ///< a target is shown; its capture is asked for after the settle time
    Capturing,  ///< waiting for frameeyeosc's result
    Reopen,     ///< "open your eyes" after the eyes-shut step
    Done,       ///< the new settings were written
    Failed,     ///< stopped; see Failure
};

/** Why a session stopped. */
enum class Failure {
    None,
    Cancelled,     ///< "Stop" pressed
    Left,          ///< another tab chosen, or the dashboard showed another page for kAwaySec
    DashboardOpened,  ///< the dashboard was opened while the fit ran without it
    NotRunning,    ///< frameeyeosc is not running
    NoResult,      ///< frameeyeosc did not answer a capture
    Unsteady,      ///< a point stayed unsteady or eyes closed for kMaxAttempts tries
    NotClosed,     ///< the eyes-shut step never read as shut
    NoMovement,    ///< a point did not move far enough the right way
    NoLidRange,    ///< the eyelids barely changed between open and shut
    WriteFailed,   ///< config.json could not be written
};

/** How the target looks at the moment. */
enum class TargetStyle {
    Dot,          ///< a dot to look at
    CloseEyes,    ///< "close your eyes" with a countdown
    KeepClosed,   ///< "keep them closed"
    OpenEyes,     ///< "open your eyes"
};

/** What the panel shows about a session. */
struct View {
    Phase phase = Phase::Idle;
    Mode mode = Mode::Full;
    int index = 0;             ///< the step shown (0-based)
    int count = 1;             ///< steps in this mode
    Point point = Point::Center;  ///< the step shown, or the one that failed
    int attempt = 1;           ///< try at this step (1-based)
    Failure failure = Failure::None;
    FailureDetail detail;      ///< the numbers behind the failure
    Values values;             ///< the settings written (Done)
    Notes notes;               ///< what the fit noticed (Done)
};

/** What the caller does after a tick. */
struct Actions {
    bool writeCapture = false;  ///< write a gaze_capture request for `target`, then call captureSent / writeFailed
    const char* target = "";
    double captureSec = 0.0;    ///< how long that capture lasts...
    double skipSec = 0.0;       ///< ...and how much of its start frameeyeosc skips
    bool writeValues = false;   ///< write `values` (once, when done): in Center mode only the zero point
    Values values;
    bool showTarget = false;    ///< show the head-locked target (hide it otherwise)
    TargetStyle style = TargetStyle::Dot;
    double yawDeg = 0.0;        ///< where, gliding between targets
    double pitchDeg = 0.0;
    int seconds = 0;            ///< the countdown on the target (0 = none): the seconds measured, or the eyes-shut 3, 2, 1
    double progress = 0.0;      ///< the ring on the target, 1 -> 0 over one step
    bool arrived = false;       ///< the target has finished gliding to this step (or didn't have to move)
    std::string log;            ///< a try's numbers to log (tryText), and at the end the tilt; lines split by '\n'
};

/** What the SteamVR dashboard is doing, for Session::tick. */
struct Dashboard {
    bool open = false;        ///< the dashboard is open...
    bool panelShown = false;  ///< ...on this panel (not on another overlay's page)
};

/**
 * One fit. The caller ticks it every frame while it is active, with what the dashboard is doing and the latest
 * status.json, and carries out the returned actions. The targets show at once, the dashboard open or closed (they are
 * nearer than it). With it open (started from the panel), closing it does not stop the fit; opening it while the
 * fit runs without it does (the panel's "Stop" can't be reached then, and the eyes would be on the dashboard).
 */
class Session {
public:
    /**
     * Start: the first target shows at the next tick.
     * @param mode the whole fit, re-centering only, or re-centering and the tilt
     * @param current the settings now (kept where the mode does not change them)
     * @param now monotonic seconds
     * @param ipd the distance between the eyes (m), for each eye's own angle to the targets
     */
    void start(Mode mode, const Values& current, double now, double ipd = kDefaultIpdM);

    /**
     * Stop.
     * @param why Failure::Cancelled ("Stop" pressed) or Failure::Left (another tab chosen)
     */
    void cancel(Failure why = Failure::Cancelled);

    /**
     * Move on.
     * @param now monotonic seconds
     * @param dashboard what the SteamVR dashboard is doing
     * @param status the latest status.json
     * @return what to do
     */
    Actions tick(double now, const Dashboard& dashboard, const EyeStatus& status);

    /**
     * The capture request asked for by the last tick was written.
     * @param id its id (frameeyeosc answers with the same id)
     * @param now monotonic seconds
     */
    void captureSent(long long id, double now);

    /** A write asked for by the last tick failed. */
    void writeFailed();

    /** @return true while showing targets */
    bool active() const;

    /** @return what the panel shows */
    View view() const;

private:
    Phase phase_ = Phase::Idle;
    Mode mode_ = Mode::Full;
    Failure failure_ = Failure::None;
    Values current_;
    Values result_;
    double ipd_ = kDefaultIpdM;
    Measured measured_[kPointCount];
    int index_ = 0;
    int attempt_ = 1;
    Point failedPoint_ = Point::Center;
    FailureDetail detail_;
    Notes notes_;
    Point previousPoint_ = Point::Center;  ///< where the dot glides from
    bool dashboardSeen_ = false;   ///< a tick has said whether the dashboard is open...
    bool dashboardWasOpen_ = false;  ///< ...and it was, at the last tick
    double awaySince_ = -1.0;    ///< when the dashboard began showing another page (-1: it isn't)
    double phaseAt_ = 0.0;       ///< when Settling, Capturing or Reopen began
    long long captureId_ = 0;    ///< 0 until captureSent
    bool requested_ = false;     ///< the request was asked for and not yet confirmed

    /**
     * Stop with a failure.
     * @param failure why
     */
    void fail(Failure failure);

    /**
     * Show the next step, or finish.
     * @param now monotonic seconds
     * @param actions where to ask for the final write
     */
    void next(double now, Actions& actions);

    /**
     * Compute the result and ask for it to be written.
     * @param actions where to ask for the write
     */
    void finish(Actions& actions);

    /** @return the step shown now */
    Point point() const { return pointAt(mode_, index_); }

    /**
     * Stop because a point did not move far enough.
     * @param point which one
     */
    void failMovement(Point point);

    /** @return how long the current step settles (the first dot's first try longer) */
    double settleSec() const {
        if (point() == Point::Closed) return kCloseSettleSec;
        return index_ == 0 && attempt_ == 1 ? kSettleSec + kFirstSettleExtraSec : kSettleSec;
    }

    /** @return how long the current step's capture lasts */
    double captureSec() const { return point() == Point::Closed ? kClosedSec : kCaptureSec; }
};

}  // namespace gaze_fit
