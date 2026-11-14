// "Headset slip test" (Advanced > Debug tools, beside "Measure gaze error"), version 2: for about three minutes a
// head-locked dot and a line under it say what to do: keep still; look at nine dots (the gaze error check's centre,
// sides and corners: frames of known gaze for the eye video's per-gaze glint templates); push the headset up, down,
// left and right a few millimetres and hold it, let go and push it back, re-seat it (all four twice); open the eyes
// wide, narrow them; look at four dots; keep still. The dot is fixed to the display, so in every step where it is
// straight ahead the eyes look the same way in the headset's frame: whatever Valve's gaze or the pupils' gaze does
// there is slip error. Each such step is scored against the first still step (the headset often stays where it was
// pushed, so its neighbours are no reference); for each push direction the result says how far each gaze was off
// while held, the rounds averaged. The record also keeps the version 1 number (the hold minus the steps around it). Nothing is written to config.json and nothing frameeyeosc sends changes: the samples come on the gaze
// error check's socket (gaze_measure.h: "FEOM", both gazes before smoothing and the gaze scale), bound only while the
// test runs. Each sample is put to the step shown when it arrived, by the panel's own clock. The eye cameras'
// recording (eyecam-rec's "start free") is started and stopped by the panel's loop, not here; this only keeps what
// the loop tells it about it. Nothing here talks to OpenVR or cairo (slip-test).
#pragma once

#include "gaze_measure.h"
#include "i18n.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace slip_check {

/** What a step asks for. */
enum class StepKind { Still, Template, Push, Release, Reseat, Widen, Normal, Squint, Look };

/** Which way: the headset pushed (as the wearer sees it), or the dot moved to (Look). */
enum class Dir { None, Up, Down, Left, Right };
/** The four push directions, in this order (Up, Down, Left, Right). */
constexpr int kDirCount = 4;

/** One step of the test. */
struct Step {
    StepKind kind = StepKind::Still;
    Dir dir = Dir::None;
    double seconds = 0.0;
    double yawDeg = 0.0;    ///< where the dot is (degrees right of straight ahead, as gaze_measure::Position)
    double pitchDeg = 0.0;  ///< ...and up
};

/** How many steps. */
constexpr int kStepCount = 43;
/** slip.json's "version" (1: the first test, without the template dots and the re-seat steps, scored against the
 *  steps around each hold). */
constexpr int kRecordVersion = 2;
/** The steps in order. */
extern const Step kSteps[kStepCount];
/** The whole test (s). */
constexpr double kRunSec = 178.5;
/** A step counts for a source with at least this many samples (a 5 s step has about 315 at 90 a second). */
constexpr int kMinSamples = 20;
/** Without a sample for this long the test stops (frameeyeosc stopped, or the eyes aren't tracked). */
constexpr double kNoDataSec = 3.0;
/** At most this many samples are kept (a test has about 16000). */
constexpr size_t kMaxRows = 20000;
/** eyecam-rec's protocol the eye video is recorded with: one long "normal" step (longer than the test), no light. */
constexpr const char* kVideoProtocol = "free";

/**
 * When a step starts (seconds from the start of the test).
 * @param index 0..kStepCount-1
 * @return seconds
 */
double stepStart(int index);

/**
 * From when after its start a step's samples count: the hands have pushed or let go, the eyes have arrived (30 % of
 * the step, at least 1 s and at most 2 s).
 * @param step the step
 * @return seconds
 */
double settleSec(const Step& step);

/**
 * Whether a step's dot is straight ahead with nothing asked of the eyes but to look at it: still, push, release,
 * re-seat, widen, normal, squint. These are scored against the first still step.
 * @param step the step
 * @return true for them
 */
bool straightAhead(const Step& step);

/**
 * A step's name in the record ("still", "dot_-20_15", "push_up", "release", "reseat", "widen", "normal", "squint",
 * "look_left", ...).
 * @param step the step
 * @return the name
 */
std::string stepName(const Step& step);

/**
 * What the line under the dot says for a step.
 * @param t texts
 * @param step the step
 * @return the text
 */
std::string instruction(const UiText& t, const Step& step);

/**
 * A push direction's index (0 Up, 1 Down, 2 Left, 3 Right).
 * @param dir the direction
 * @return the index (-1 for None)
 */
int dirIndex(Dir dir);

/** A sample as received: when (the panel's clock), the step then shown, whether it counts. */
struct Row {
    double at = 0.0;
    double wall = 0.0;  ///< wall clock (Unix seconds)
    int step = -1;
    bool counted = false;  ///< from settleSec after the step started
    gaze_measure::Sample sample;
};

/** One source in one step: the medians of its counted samples (degrees; 1.0 in the sample = 45°). */
struct SourceStep {
    int n = 0;
    double x = NAN;
    double y = NAN;
    bool counts() const { return n >= kMinSamples; }
};

/** One step's numbers. */
struct StepScore {
    SourceStep valve;  ///< Valve's gaze: both eyes open enough, not held
    SourceStep pupil;  ///< the pupils' gaze: also both pupils, not easing from another source
};

/** How far a source was off during a kind of hold (a push direction, widening, narrowing), its rounds averaged. */
struct Move {
    int rounds = 0;   ///< holds that counted (the hold and at least one still part next to it)
    double dx = NAN;  ///< degrees, + right
    double dy = NAN;  ///< degrees, + up
    bool counts() const { return rounds > 0; }
    double size() const { return std::hypot(dx, dy); }
};

/** Both sources for one kind of hold. */
struct MoveScore {
    Move valve;
    Move pupil;
};

/** The test's result. */
struct Result {
    bool mapped = false;               ///< a pupil map was there (the pupils scored)
    StepScore steps[kStepCount];
    MoveScore pushes[kDirCount];       ///< Up, Down, Left, Right: the hold against the first still step
    MoveScore widen;
    MoveScore squint;
    MoveScore pushesAround[kDirCount]; ///< ...the hold against the steps right before and after it (version 1's)
    MoveScore widenAround;
    MoveScore squintAround;
    double valveMean = NAN;            ///< the mean of the push directions' sizes that count (against the still)
    double pupilMean = NAN;
    int valveDirs = 0;                 ///< ...how many
    int pupilDirs = 0;
};

/**
 * Score the samples.
 * @param rows the samples
 * @return the result
 */
Result score(const std::vector<Row>& rows);

enum class Phase { Idle, Running, Done, Stopped, Failed };

/** The eye video alongside (eyecam-rec's recording, started and stopped by the panel's loop). */
enum class VideoState {
    Off,       ///< not recorded: eyecam-rec wasn't there or wasn't idle (reason says which)
    Asked,     ///< "start free" sent, no reply yet
    On,        ///< eyecam-rec took it
    Refused,   ///< eyecam-rec said no (reason: its reply)
};

/** What the panel shows. */
struct View {
    Phase phase = Phase::Idle;
    int step = -1;               ///< the step shown (0..kStepCount-1; -1 before the first)
    double stepLeft = 0.0;       ///< seconds left of it
    double startUnix = 0.0;      ///< wall clock at the start (Valve's log can be matched to it)...
    double endUnix = 0.0;        ///< ...and at the end
    int stepsDone = 0;           ///< steps fully shown
    bool mapped = false;         ///< samples said a pupil map is there
    bool beforeSmoothing = true; ///< every sample's gazes were before smoothing
    int samples = 0;             ///< samples received
    VideoState video = VideoState::Off;
    std::string videoReason;     ///< why there is no video ("" = none to say)
    std::string videoDir;        ///< eyecam-rec's recording folder, as its status said while it recorded
    Result result;               ///< (Done)
};

/** What to do this frame. */
struct Tick {
    bool showTarget = false;
    double yawDeg = 0.0;
    double pitchDeg = 0.0;
    int step = -1;           ///< the step shown
    double progress = 0.0;   ///< how much of the step's time is left (1 at its start)
    int seconds = 0;         ///< whole seconds left of it (rounded up)
    bool changed = false;    ///< a new step just started: a cue
    bool ended = false;      ///< the test just ended (done, stopped or failed)
};

/** One test. */
class Session {
public:
    /**
     * Start (a test already going starts over).
     * @param now the panel's monotonic seconds
     * @param unixNow wall clock
     */
    void start(double now, double unixNow);

    /**
     * Stop before the end ("Stop", leaving the page, the panel closing).
     * @param unixNow wall clock
     */
    void stop(double unixNow);

    /**
     * Every frame while active.
     * @param now monotonic seconds
     * @param unixNow wall clock
     * @return what to show
     */
    Tick tick(double now, double unixNow);

    /**
     * A sample arrived.
     * @param now monotonic seconds when it was read
     * @param unixNow wall clock then
     * @param sample the sample
     */
    void add(double now, double unixNow, const gaze_measure::Sample& sample);

    /**
     * What the loop knows of the eye video.
     * @param state its state
     * @param reason why there is none ("" keeps the last)
     */
    void setVideo(VideoState state, const std::string& reason = "");

    /**
     * eyecam-rec's recording folder while it records (kept once known).
     * @param dir the folder
     */
    void setVideoDir(const std::string& dir);

    /** The result was closed: back to Idle. */
    void dismiss();

    /** @return true while it runs */
    bool active() const { return view_.phase == Phase::Running; }

    /** @return what the panel shows */
    const View& view() const { return view_; }

    /** @return the samples received */
    const std::vector<Row>& rows() const { return rows_; }

    /** @return when each step was shown (monotonic; NaN if not yet) */
    const double* shownAt() const { return shownAt_; }

    /** @return the same on the wall clock */
    const double* shownUnix() const { return shownUnix_; }

    /** @return when it started (monotonic) */
    double startedAt() const { return start_; }

private:
    View view_;
    std::vector<Row> rows_;
    double start_ = 0.0;
    double lastSample_ = 0.0;
    double shownAt_[kStepCount] = {};
    double shownUnix_[kStepCount] = {};
    bool ended_ = false;

    void end(Phase phase, double unixNow);
};

/**
 * slip.json: the test's times (wall clock in ms), the plan, per step its name, its planned and actual start (seconds
 * from the start, the wall clock, and the eye server's time of its first sample, which is valve.csv's sample_time in
 * the eye video's recording), where the dot was and each source's medians, then the holds' moves and the eye video.
 * @param view the test
 * @param rows its samples
 * @param shownAt when each step was shown (monotonic)
 * @param shownUnix ...on the wall clock
 * @param startedAt when it started (monotonic)
 * @param result the scores
 * @return the JSON
 */
std::string recordJson(const View& view, const std::vector<Row>& rows, const double* shownAt, const double* shownUnix,
                       double startedAt, const Result& result);

/**
 * samples.csv: a row per sample received (the panel's time, the wall clock, the step, its name and the time into it,
 * whether it counts, then the sample's fields as the gaze error check's samples.csv has them).
 * @param rows the samples
 * @param shownAt when each step was shown (monotonic)
 * @return the text
 */
std::string samplesCsv(const std::vector<Row>& rows, const double* shownAt);

/**
 * A short line for the record list and the result: "Valve 0.4°・瞳 3.1°" / "Valve 0.4°, pupil 3.1°" (the mean move
 * over the push directions).
 * @param t texts
 * @param result the scores
 * @return the line ("" if neither source counts)
 */
std::string briefText(const UiText& t, const Result& result);

}  // namespace slip_check
