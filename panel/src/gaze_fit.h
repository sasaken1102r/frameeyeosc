// The eye fit (the "Eye fit" tab): where the targets are, how captured averages become the gaze zero point, the up gain
// and each eye's lid fit, and the step-by-step session the panel runs (with the dashboard open or closed).
// Five wears scored against the true dots (2026-10-09) showed that only the zero point measured fresh each wear carries
// over: Valve's sideways and down gains (0.73-1.30 down) and its tilt changed from wear to wear, and keeping them made
// the next wear worse than Valve's raw gaze (3.36° against 2.78° off), while the up gain stayed about the same (mean
// 0.88). So the whole fit writes the zero point, the mean up gain of the last kUpGainFits whole fits and the eyelids;
// sideways and down stay 1 and the tilt 0, and each wear only re-centers.
// Nothing here talks to OpenVR or writes files, so it can be tested on its own (gaze_fit_test.cpp).
#pragma once

#include "status.h"

#include <cmath>
#include <string>
#include <vector>

namespace gaze_fit {

/** The whole fit (five gaze points and the eyes-shut step), or re-centering the gaze only (one dot), for putting the
 *  headset back on. */
enum class Mode { Full, Center };

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
/** The up gain written is the mean of the up gains measured by this many whole fits at most, this one included (five
 *  wears read 0.78-1.01; their mean took the up dot from 2.5° off to 1.7°). */
constexpr int kUpGainFits = 5;

/**
 * A step's target.
 * @param point which one
 * @return where it is
 */
const Target& target(Point point);

/**
 * How many steps a mode has.
 * @param mode the mode
 * @return 6 or 1
 */
int pointCount(Mode mode);

/**
 * The step shown at a position.
 * @param mode the mode
 * @param index 0-based step
 * @return the step
 */
Point pointAt(Mode mode, int index);

/** Fewest fresh pupil samples each eye needs at each dot for the pupil map (a capture takes about 150 at 90 Hz). */
constexpr int kMinPupilFresh = 30;
/** A pupil map whose five dots it fits worse than this (degrees, root mean square of each eye's) is not written. */
constexpr double kMaxPupilRmsDeg = 4.0;
/** ...nor one whose pupil hardly moves between the dots: the smaller spread of the five pupil pairs (the square root
 *  of the smaller eigenvalue of their covariance) must be at least this (the eye server's units; about 0.04 seen). */
constexpr double kMinPupilSpread = 0.01;

/** One eye's pupil map (frameeyeosc's pupil_* keys, src/pupil.rs): from its pupil pair (u, v) to the direction it
 *  looks, in degrees: yaw = yaw[0] + yaw[1] * u + yaw[2] * v, pitch likewise (yaw + right, pitch + up). */
struct PupilMap {
    double yaw[3] = {0.0, 0.0, 0.0};
    double pitch[3] = {0.0, 0.0, 0.0};
};

/** One capture's result (the raw combined gaze and openness, before any correction or scale). */
struct Measured {
    double x = 0.0;
    double y = 0.0;
    double spread = 0.0;
    double openness[2] = {0.0, 0.0};  ///< left, right
    bool hasOpenness = false;
    /** Each eye that the eye camera saw open while Valve read it closing (frameeyeosc's camera_open): Valve's openness
     *  can get stuck at 0 for a whole wear, and then that eye's openness here is not the eye's (see standIns). */
    bool cameraOpen[2] = {false, false};
    int samples = 0;    ///< usable samples averaged...
    int received = 0;   ///< ...of these that came in (0 = not reported)
    double rateHz = NAN;  ///< the tracker's rate over the capture
    /** Each eye's pupil pair (the median of its fresh pairs) and how many samples had it fresh: for the pupil map. */
    bool hasPupil[2] = {false, false};
    double pupil[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
    int pupilFresh[2] = {0, 0};
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
    double closedBelow[2] = {NAN, NAN};  ///< NotClosed: each eye had to read below this (kClosedShare of ahead; NaN:
                                         ///< not checked, standIns)
    int eye = -1;                       ///< NoMovement: -1 (the gaze); NoLidRange: the eye
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
 * @param standIn the eyes not checked (see standIns; may be null: none)
 * @return true if usable
 */
bool usableClosed(const Measured& closed, const Measured& center, const bool* standIn = nullptr);

/**
 * The eyes whose openness the lid fit can't use: Valve read them stuck shut (at 0) while the eye camera saw them open,
 * in any of the captures the lid fit reads (up, center, down and eyes shut; frameeyeosc lets their gaze count then,
 * as long as the other eye reads open). Their lid fit stays as it was.
 * @param points the captures, indexed by Point (those not taken yet left as Measured())
 * @param out left, right
 */
void standIns(const Measured points[kPointCount], bool out[2]);

/** What a whole fit measured besides what it wrote, for the log. */
struct Notes {
    double gainX = NAN;     ///< the gains this fit's own dots gave (only the up gain is used, averaged)
    double gainUp = NAN;
    double gainDown = NAN;
    int upGainFits = 0;     ///< the whole fits the written up gain is the mean of, this one included
    /** The eyes the eye camera stood in for, Valve's openness stuck shut (standIns; for re-centering, the center
     *  capture's): measured all the same, but a whole fit kept their lid fit. */
    bool standIn[2] = {false, false};
};

/** The settings a fit writes. */
struct Values {
    double offsetX = 0.0;
    double offsetY = 0.0;
    double gainX = 1.0;
    double gainUp = 1.0;
    double gainDown = 1.0;
    double rollDeg = 0.0;  ///< the headset's tilt (gaze_roll_deg): a whole fit writes 0 (it isn't measured any more)
    bool hasLids = false;  ///< the eyelid readings below are set...
    /** ...except for these eyes, when a fit set them: not measured (standIns), so the lid fit in config.json and the
     *  eye's scale stay as they are (the readings below are whatever came in, possibly none). */
    bool lidsKept[2] = {false, false};
    double lidClosed[2] = {0.0, 0.0};  ///< left, right
    double lidUp[2] = {0.0, 0.0};
    double lidOpen[2] = {0.0, 0.0};
    double lidDown[2] = {0.0, 0.0};
    /** Both eyes' pupil maps are set (from the config before a fit; a whole fit replaces them when its pupils could be
     *  used, see fitPupilMaps, and keeps them otherwise). */
    bool hasPupilMap = false;
    PupilMap pupilMap[2];
    /** Both eyes' pupil centres are set (yaw, pitch in degrees; see pupilCenters): written with the map, or alone by a
     *  re-center. */
    bool hasPupilCenter = false;
    double pupilCenter[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
};

/**
 * Where an eye looks to see a target at kTargetDistanceM: its direction from that eye, not from between the eyes.
 * The same as eyeAngle sideways (the left eye sits ipd/2 to the left), and up / down from the eye's own straight ahead.
 * @param target the target
 * @param eye 0 = left, 1 = right
 * @param ipd the distance between the eyes (m)
 * @param out yaw (+ right) and pitch (+ up), degrees
 */
void eyeDirection(const Target& target, int eye, double ipd, double out[2]);

/**
 * Where a pupil map points for a pupil pair.
 * @param map the eye's map
 * @param pupil its pupil pair (u, v)
 * @param out yaw and pitch, degrees
 */
void applyPupilMap(const PupilMap& map, const double pupil[2], double out[2]);

/**
 * Both eyes' pupil maps from the five gaze captures (center, up, down, left, right): per eye the affine map (least
 * squares) from its pupil pairs to its directions to the five targets (eyeDirection). Nothing is set unless both eyes
 * can be used: every dot has the eye's pupil from at least kMinPupilFresh fresh samples, the pairs spread at least
 * kMinPupilSpread, and the map fits its dots within kMaxPupilRmsDeg.
 * @param points the captures, indexed by Point
 * @param ipd the distance between the eyes (m)
 * @param out the maps (set only on success)
 * @param rmsDeg each eye's fit over its five dots, degrees (set only on success)
 * @param why on failure, the first reason, in English for the log
 * @return true if both maps were fitted
 */
bool fitPupilMaps(const Measured points[kPointCount], double ipd, PupilMap out[2], double rmsDeg[2], std::string& why);

/**
 * Both eyes' pupil centres from a straight-ahead capture: what makes each eye's map point at its direction to the
 * center target (target minus map). Nothing is set unless both eyes have their pupil from at least kMinPupilFresh
 * fresh samples.
 * @param center the center capture
 * @param maps both eyes' maps
 * @param ipd the distance between the eyes (m)
 * @param out each eye's centre, yaw and pitch in degrees (set only on success)
 * @param why on failure, why, in English for the log
 * @return true if both were set
 */
bool pupilCenters(const Measured& center, const PupilMap maps[2], double ipd, double out[2][2], std::string& why);

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
 * The up gain a whole fit writes: the mean of the one it measured and the previous whole fits' (the last
 * kUpGainFits - 1 of them), within range, 2 decimals.
 * @param measured this fit's up gain
 * @param previous the previous whole fits' up gains, oldest first
 * @param fits how many were averaged, this one included (may be null)
 * @return the setting
 */
double meanUpGain(double measured, const std::vector<double>& previous, int* fits = nullptr);

/**
 * The up gains to remember after a whole fit: the previous ones and this one, the last kUpGainFits of them.
 * @param previous the previous whole fits' up gains, oldest first
 * @param measured this fit's
 * @return the list, oldest first
 */
std::vector<double> nextUpGains(const std::vector<double>& previous, double measured);

/**
 * The zero point from the center capture; everything else stays as it is.
 * @param center the center capture
 * @param current the settings now
 * @return the new settings (rounded, within range)
 */
Values fitCenter(const Measured& center, const Values& current);

/**
 * The zero point and the up gain from the five gaze captures: the zero point is the center capture, and the up gain
 * makes the up dot come out at its angle (gain = target / (up - center)), averaged with the previous fits'
 * (meanUpGain). Sideways and down stay 1 and the tilt 0. Each side and up / down point must move far enough its own way.
 * @param points the captures, indexed by Point
 * @param out the settings before the fit in; the new gaze settings out (rounded, within range), the lid readings
 *            left alone
 * @param failed the first point that did not move far enough the right way
 * @param detail on failure, how far it moved (may be null)
 * @param notes the gains this fit's dots gave (may be null)
 * @param previousUpGains the previous whole fits' up gains, oldest first
 * @return false if a point did not move far enough the right way
 */
bool fitGaze(const Measured points[kPointCount], Values& out, Point& failed, FailureDetail* detail = nullptr,
             Notes* notes = nullptr, const std::vector<double>& previousUpGains = {});

/**
 * Each eye's lid fit: the eyes-shut reading, and the open readings looking up, straight ahead and down. An eye the
 * eye camera stood in for (standIns) is neither checked nor fitted: its readings in `out` stay and lidsKept is set.
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
    Notes notes;               ///< what a whole fit measured (Done)
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
    std::string log;            ///< a try's numbers to log (tryText), and at the end the gains; lines split by '\n'
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
     * @param mode the whole fit or re-centering only
     * @param current the settings now (kept where the mode does not change them)
     * @param now monotonic seconds
     * @param upGains the previous whole fits' up gains, oldest first (the whole fit writes their mean with its own)
     * @param ipd the distance between the eyes (m), for each eye's direction to the targets (the pupil map)
     */
    void start(Mode mode, const Values& current, double now, const std::vector<double>& upGains = {},
               double ipd = kDefaultIpdM);

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
    std::vector<double> upGains_;
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
