// "Headset re-seat test" (Advanced > Debug tools, beside the headset slip test): a head-locked dot and a line under it
// say what to do: keep still on the centre dot; then ten times: take the headset off and put it back on (the step waits
// until the eyes are back and open, and gives the wearer kReseatMinSec at least), then look at nine dots (the gaze
// error check's centre, sides and corners, 2 s each). The dot is fixed to the display, so each round's nine dots show
// how the gaze sits in that seat: each round is scored against the dots (each source's median at each dot against
// the dot's own direction) and against the still start's centre (how far the seat moved the centre). It is the data
// for refitting the pupil gaze across re-seats, so the eye video is recorded alongside (eyecam-rec's "start reseat":
// its glints.csv has every glint per frame), and each sample keeps its raw values (gaze_measure::Raw: the eye
// server's pupil pair and the eye camera's glint offset) so the pupil gaze can be worked out again later. Nothing is
// written to config.json and nothing frameeyeosc sends changes: the samples come on the gaze error check's socket
// (gaze_measure.h), bound only while the test runs. With frameeyeosc's gaze guard on, the guarded gaze comes with them
// and is scored as a third source, on Valve's samples (as the slip test does). Each sample is put to the step shown
// when it arrived, by the panel's own clock. Nothing here talks to OpenVR or cairo (reseat-test).
#pragma once

#include "gaze_measure.h"
#include "i18n.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace reseat_check {

/** What a step asks for. */
enum class StepKind { Still, Reseat, Dot };

/** One step of the test. */
struct Step {
    StepKind kind = StepKind::Still;
    int round = 0;           ///< 0 for the still start, 1..kRounds for the rounds
    int place = -1;          ///< a dot's place (index into gaze_measure::kPositions), -1 otherwise
    double seconds = 0.0;    ///< how long it is shown (a re-seat: kReseatMinSec at least, until the eyes are back)
    double yawDeg = 0.0;     ///< where the dot is (degrees right of straight ahead, as gaze_measure::Position)
    double pitchDeg = 0.0;   ///< ...and up
};

/** How many re-seats. */
constexpr int kRounds = 10;
/** The dots after each: the gaze error check's first nine places (centre, sides, corners). */
constexpr int kPlaces = 9;
/** How many steps: the still start, then each round's re-seat and dots. */
constexpr int kStepCount = 1 + kRounds * (1 + kPlaces);
/** The still start (s)... */
constexpr double kStillSec = 10.0;
/** ...each dot... */
constexpr double kDotSec = 2.0;
/** ...and a re-seat: the dots come no sooner than this after it is asked for (the wearer's time)... */
constexpr double kReseatMinSec = 8.0;
/** ...and only once the eyes have been back (both open enough, Valve's gaze not held) this long without a gap... */
constexpr double kSettledSec = 3.0;
/** ...a gap in the samples this long breaks that... */
constexpr double kGapSec = 0.5;
/** ...and without it this long after the re-seat was asked for, the test fails. */
constexpr double kReseatMaxSec = 60.0;
/** The test at its shortest (each re-seat kReseatMinSec): 270 s. */
constexpr double kMinRunSec = kStillSec + kRounds * (kReseatMinSec + kPlaces * kDotSec);
/** ...and at its longest (each re-seat kReseatMaxSec): 790 s. */
constexpr double kMaxRunSec = kStillSec + kRounds * (kReseatMaxSec + kPlaces * kDotSec);
/** A dot counts for a source with at least this many samples (a dot's counted second has about 90). */
constexpr int kMinSamples = 20;
/** Without a sample for this long (outside a re-seat) the test stops (frameeyeosc stopped, or the eyes aren't
 *  tracked). */
constexpr double kNoDataSec = 3.0;
/** At most this many samples are kept (a test at its longest has about 71000). */
constexpr size_t kMaxRows = 80000;
/** reseat.json's "version". */
constexpr int kRecordVersion = 1;
/** eyecam-rec's protocol the eye video is recorded with: one long "normal" step (longer than the test), no light. */
constexpr const char* kVideoProtocol = "reseat";

/**
 * The steps in order.
 * @return kStepCount steps
 */
const std::vector<Step>& steps();

/**
 * From when after its start a step's samples count: the eyes have arrived (30 % of the step, at least 1 s and at most
 * 2 s; a re-seat's never count).
 * @param step the step
 * @return seconds
 */
double settleSec(const Step& step);

/**
 * A step's name in the record ("still", "reseat_3", "dot_-20_15").
 * @param step the step
 * @return the name
 */
std::string stepName(const Step& step);

/**
 * What the line under the dot says for a step.
 * @param t texts
 * @param step the step
 * @param waiting a re-seat past its wearer's time with the eyes not back yet
 * @return the text
 */
std::string instruction(const UiText& t, const Step& step, bool waiting = false);

/** A sample as received: when (the panel's clock), the step then shown, whether it counts. */
struct Row {
    double at = 0.0;
    double wall = 0.0;  ///< wall clock (Unix seconds)
    int step = -1;
    bool counted = false;  ///< a dot or the still start, from settleSec after it started
    gaze_measure::Sample sample;
};

/** One source at one dot (or the still start): the medians of its counted samples (degrees; 1.0 in the sample = 45°). */
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
    SourceStep guarded;  ///< the guarded gaze (frameeyeosc's gaze_guard) on Valve's samples that have it
};

/** One source in one round. */
struct RoundSource {
    int dots = 0;       ///< dots that counted
    double error = NAN; ///< the median of their angles to their dots (degrees)
    double dx = NAN;    ///< the mean of their offsets from their dots (degrees, + right): where this seat put the gaze
    double dy = NAN;    ///< (+ up)
    double centreDx = NAN;  ///< the centre dot against the still start's (degrees): how far this seat moved it
    double centreDy = NAN;
    bool counts() const { return dots > 0; }
};

/** One round. */
struct RoundScore {
    RoundSource valve;
    RoundSource pupil;
    RoundSource guarded;
    double reseatSec = NAN;  ///< how long its re-seat step took (s)
    double offSec = NAN;     ///< the longest time without a usable sample in it (s): about how long the eyes were away
};

/** One source over the rounds that count for it. */
struct Overall {
    int rounds = 0;
    double error = NAN;   ///< the mean of the rounds' errors
    double spread = NAN;  ///< the mean distance of the rounds' offsets (dx, dy) from their mean: how much a re-seat moves it
    double centre = NAN;  ///< the mean size of the rounds' centre moves (against the still start)
    bool counts() const { return rounds > 0; }
};

/** The test's result. */
struct Result {
    bool mapped = false;         ///< a pupil map was there (the pupils scored)
    std::vector<StepScore> steps;  ///< kStepCount
    RoundScore rounds[kRounds];
    Overall valve;
    Overall pupil;
    bool guardSeen = false;      ///< samples came with the gaze guard's values (gaze_guard on)
    Overall guarded;
    gaze_measure::Missing missing;  ///< samples without their FEOR / FEOG (summary "missing_datagrams")
};

/**
 * Score the samples.
 * @param rows the samples
 * @param reseatSec how long each round's re-seat step took (NaN: not shown; kRounds of them, or nullptr)
 * @param offSec ...the longest time without a usable sample in it (or nullptr)
 * @return the result
 */
Result score(const std::vector<Row>& rows, const double* reseatSec = nullptr, const double* offSec = nullptr);

enum class Phase { Idle, Running, Done, Stopped, Failed };

/** The eye video alongside (eyecam-rec's recording, started and stopped by the panel's loop). */
enum class VideoState {
    Off,       ///< not recorded: eyecam-rec wasn't there or wasn't idle (reason says which)
    Asked,     ///< "start reseat" sent, no reply yet
    On,        ///< eyecam-rec took it
    Refused,   ///< eyecam-rec said no (reason: its reply)
};

/** What the panel shows. */
struct View {
    Phase phase = Phase::Idle;
    int step = -1;               ///< the step shown (0..kStepCount-1; -1 before the first)
    int round = 0;               ///< its round (0: the still start)
    double stepLeft = 0.0;       ///< seconds left of it (a re-seat: of the wearer's time)
    bool waiting = false;        ///< a re-seat past the wearer's time, the eyes not back yet
    double startUnix = 0.0;      ///< wall clock at the start (Valve's log can be matched to it)...
    double endUnix = 0.0;        ///< ...and at the end
    int stepsDone = 0;           ///< steps fully shown
    int roundsDone = 0;          ///< rounds whose dots were all shown
    bool timedOut = false;       ///< (Failed) the eyes didn't come back within kReseatMaxSec of a re-seat
    bool mapped = false;         ///< samples said a pupil map is there
    bool beforeSmoothing = true; ///< every sample's gazes were before smoothing
    bool raw = false;            ///< samples came with their raw values (a frameeyeosc that sends them)
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
    double progress = 0.0;   ///< how much of the step's time is left (1 at its start; a re-seat: of the wearer's time)
    int seconds = 0;         ///< whole seconds left of it (rounded up; 0 for a re-seat past the wearer's time)
    bool waiting = false;    ///< a re-seat past the wearer's time, the eyes not back yet (its line changes)
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

    /** @return how long each round's re-seat step took (s; NaN if it hasn't ended) */
    const double* reseatSec() const { return reseatSec_; }

    /** @return the longest time without a usable sample in each round's re-seat step (s; NaN if none seen) */
    const double* offSec() const { return offSec_; }

    /** @return when it started (monotonic) */
    double startedAt() const { return start_; }

private:
    View view_;
    std::vector<Row> rows_;
    double start_ = 0.0;
    double lastSample_ = 0.0;
    double usableSince_ = NAN;   ///< the eyes back without a gap since then (NaN: not now)
    double lastUsable_ = NAN;    ///< the last usable sample
    double shownAt_[kStepCount] = {};
    double shownUnix_[kStepCount] = {};
    double reseatSec_[kRounds] = {};
    double offSec_[kRounds] = {};
    bool ended_ = false;

    void end(Phase phase, double unixNow);
    void show(int step, double now, double unixNow);
};

/**
 * reseat.json: the test's times (wall clock in ms), the plan, per step its name, round, place, when it was shown
 * (seconds from the start, the wall clock, and the eye server's time of its first sample, which is valve.csv's and
 * glints.csv's t_cam / sample_time in the eye video's recording), where the dot was and each source's medians, per
 * round its scores and how its re-seat went, then the overall numbers and the eye video.
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
 * samples.csv: a row per sample received (the panel's time, the wall clock, the step, its name, its round and the time
 * into it, whether it counts, then the sample's fields as the gaze error check's samples.csv has them, the gaze guard's
 * (gaze_measure::kGuardColumns), its raw values last: gaze_measure::rawCsvHeader).
 * @param rows the samples
 * @param shownAt when each step was shown (monotonic)
 * @return the text
 */
std::string samplesCsv(const std::vector<Row>& rows, const double* shownAt);

/**
 * A short line for the record list and the result: "Valve 1.2°・瞳 3.4°" / "Valve 1.2°, pupil 3.4°" (the mean of the
 * rounds' errors), and the guarded gaze's after them while it was there ("・見張り付き 1.5°" / ", guarded 1.5°").
 * @param t texts
 * @param result the scores
 * @return the line ("" if no source counts)
 */
std::string briefText(const UiText& t, const Result& result);

/**
 * The same for the change from one re-seat to the next (Overall::spread).
 * @param t texts
 * @param result the scores
 * @return the line ("" if no source counts)
 */
std::string spreadText(const UiText& t, const Result& result);

}  // namespace reseat_check
