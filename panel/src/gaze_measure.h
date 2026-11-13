// "Measure gaze error" (Advanced > Debug tools): for about a minute a head-locked dot hops through kPositionCount
// places inside the eye fit's range, and for each the panel scores how far two gazes sit from it: Valve's combined
// gaze as gaze_source "valve" sends it, and the pupils' as "pupil" sends it (only while both pupils give it), both
// before the user's gaze scale. Nothing is written to config.json and nothing frameeyeosc sends changes: while this
// panel binds kSocketName in the status folder, frameeyeosc passes each sample's two gazes to it (src/dots.rs, "FEOM"),
// worked out a second time for the source not in use, as for the debug dots' compare dot. Each sample is put to the
// place shown when it arrived, by the panel's own clock: the panel shows each place and receives the samples, so one
// clock times both. Only the samples from kSettleSec after a hop to the next hop count (the eyes have arrived), and
// only while both eyes are open enough. Nothing here talks to OpenVR or cairo (measure-test).
#pragma once

#include "i18n.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gaze_measure {

/** The socket's name in the status folder (frameeyeosc's MEASURE_SOCKET_NAME). */
constexpr const char* kSocketName = "gaze-measure.sock";
/**
 * One sample (version 1), little-endian: b"FEOM", 1, flags (bit 0 both eyes have a pupil map, bit 1 the sample counts:
 * both eyes open enough, as the eye fit's captures count them, bit 2 Valve's gaze is held), the pupils' state (0 none
 * without a map, 1 both pupils, 2 the left pupil only, 3 the right only, 4 held, 5 Valve's standing in), a zero byte,
 * the sample time (f64), Valve's combined x and y as "valve" sends them, the pupils' as "pupil" sends them (f32; 1.0 =
 * 45°; both before the user's gaze scale), each eye's Valve openness (f32).
 */
constexpr size_t kSampleSize = 40;
/** How many places the dot goes to. */
constexpr int kPositionCount = 15;
/** The countdown at the centre before the first place. */
constexpr double kCountdownSec = 3.0;
/** How long the dot stays at each place... */
constexpr double kHoldSec = 3.5;
/** ...and from when after it hops its samples count (the eyes have arrived; no smooth pursuit). */
constexpr double kSettleSec = 1.0;
/** Without a sample for this long the run stops (frameeyeosc stopped, or the eyes aren't tracked). */
constexpr double kNoDataSec = 3.0;
/** A place counts for a source with at least this many samples (of about 225). */
constexpr int kMinSamples = 20;
/** At most this many samples are kept (a run has about 5000). */
constexpr size_t kMaxRows = 9000;
/** The whole run (s). */
constexpr double kRunSec = kCountdownSec + kPositionCount * kHoldSec;

/** A place (degrees right and up from straight ahead, as the eye fit's targets: gaze_fit::kTargetDistanceM away). */
struct Position {
    double yawDeg = 0.0;
    double pitchDeg = 0.0;
};
/** The places in order: the centre, the four sides, the four corners (crossing over), then four in between. */
extern const Position kPositions[kPositionCount];

/** Where the pupils' gaze came from in a sample (pupil.rs's InUse, with None when there is no map). */
enum class PupilState : uint8_t { None = 0, Pupil, Left, Right, Hold, Valve };
constexpr int kStateCount = 6;

/** One sample from frameeyeosc. */
struct Sample {
    double time = 0.0;          ///< the eye server's sample time
    bool mapped = false;        ///< both eyes have a pupil map (else no pupil gaze)
    bool usable = false;        ///< both eyes open enough (the eye fit's rule)
    bool valveHeld = false;     ///< Valve's gaze was held (eyes shut, a blink)
    PupilState pupilState = PupilState::None;
    float valve[2] = {0, 0};    ///< x, y; 1.0 = 45°
    float pupil[2] = {0, 0};    ///< x, y; meaningful while pupilState is Pupil
    float openness[2] = {NAN, NAN};
};

/**
 * Read a sample.
 * @param data the bytes
 * @param size how many
 * @param out the sample
 * @return false if it is not one (of version 1) or its gaze isn't finite
 */
bool decode(const uint8_t* data, size_t size, Sample& out);

/**
 * Write a sample (for tests).
 * @param sample the sample
 * @return the bytes
 */
std::vector<uint8_t> encode(const Sample& sample);

/**
 * A place's direction as the gaze angles of a ray from between the eyes (degrees: atan2(x, -z), atan2(y, -z)).
 * @param position the place
 * @param out x, y
 */
void targetGaze(const Position& position, double out[2]);

/**
 * The angle between two rays given as gaze angles (degrees).
 * @param a x, y
 * @param b x, y
 * @return degrees
 */
double angleBetween(const double a[2], const double b[2]);

/** A sample as received: when (the panel's clock), the place then shown (-1 in the countdown), whether it counts. */
struct Row {
    double at = 0.0;
    double wall = 0.0;   ///< wall clock (Unix seconds)
    int position = -1;
    bool scored = false;
    Sample sample;
};

/** One source at one place: from its counting samples, the medians of its offset and error. */
struct SourceScore {
    int n = 0;
    double dx = NAN;         ///< median sideways offset (degrees; + right)
    double dy = NAN;         ///< median up/down offset (+ up)
    double error = NAN;      ///< median angle to the dot
    double meanError = NAN;  ///< mean angle to the dot
    bool counts() const { return n >= kMinSamples; }
};

/** One place. */
struct PositionScore {
    Position target;
    double gaze[2] = {0, 0};          ///< the dot's gaze angles (targetGaze)
    SourceScore valve;
    SourceScore pupil;
    int window = 0;                   ///< samples in its window (from kSettleSec on)
    int states[kStateCount] = {};     ///< ...by the pupils' state
};

/** One source over the run, from the places that count for it. */
struct Overall {
    int positions = 0;
    double error = NAN;       ///< the median of the places' median errors
    double meanError = NAN;   ///< their mean
    double dx = NAN;          ///< the mean of the places' median offsets
    double dy = NAN;
    int worst = -1;           ///< the place with the largest median error
    double worstError = NAN;
};

/** A run's result. */
struct Result {
    bool mapped = false;              ///< a pupil map was there (pupil scored)
    PositionScore positions[kPositionCount];
    Overall valve;
    Overall pupil;
    int window = 0;                   ///< samples in the windows
    int states[kStateCount] = {};
};

/**
 * Score the samples.
 * @param rows the samples
 * @return the result
 */
Result score(const std::vector<Row>& rows);

enum class Phase { Idle, Countdown, Running, Done, Stopped, Failed };

/** What the panel shows. */
struct View {
    Phase phase = Phase::Idle;
    int index = -1;            ///< the place shown (-1 in the countdown)
    double startUnix = 0.0;    ///< wall clock at the start (Valve's log can be matched to it)...
    double endUnix = 0.0;      ///< ...and at the end
    int positionsDone = 0;     ///< places fully shown
    bool mapped = false;       ///< samples said a pupil map is there
    int samples = 0;           ///< samples received
    Result result;             ///< (Done)
};

/** What to do this frame. */
struct Tick {
    bool showTarget = false;
    double yawDeg = 0.0;
    double pitchDeg = 0.0;
    int seconds = 0;          ///< the countdown on the dot (0 = none)
    double progress = 0.0;    ///< the ring: how much of this place's time is left
    bool hopped = false;      ///< the dot just moved to a new place (or appeared): a cue
    bool ended = false;       ///< the run just ended (done, stopped or failed)
};

/** One run. */
class Session {
public:
    /**
     * Start (a run already going starts over).
     * @param now the panel's monotonic seconds
     * @param unixNow wall clock
     */
    void start(double now, double unixNow);

    /**
     * Stop before the end ("Stop", leaving the page, the panel closing).
     * @param now monotonic seconds
     * @param unixNow wall clock
     */
    void stop(double now, double unixNow);

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
    void add(double now, double unixNow, const Sample& sample);

    /** The result was closed: back to Idle. */
    void dismiss();

    /** @return true during the countdown and the run */
    bool active() const { return view_.phase == Phase::Countdown || view_.phase == Phase::Running; }

    /** @return what the panel shows */
    const View& view() const { return view_; }

    /** @return the samples received */
    const std::vector<Row>& rows() const { return rows_; }

    /** @return when each place was shown (monotonic; NaN if not yet) */
    const double* shownAt() const { return shownAt_; }

    /** @return when it started (monotonic) */
    double startedAt() const { return start_; }

private:
    View view_;
    std::vector<Row> rows_;
    double start_ = 0.0;
    double lastSample_ = 0.0;
    double shownAt_[kPositionCount] = {};
    bool ended_ = false;
    bool appeared_ = false;  ///< the dot has been shown (its first cue played)

    void end(Phase phase, double unixNow);
};

/**
 * measure.json: the run's times (wall clock in ms, as summary.json's), the plan, per place its dot, when it was shown
 * (seconds from the start) and each source's medians and counts and the states' counts, and the overall numbers.
 * @param view the run
 * @param shownAt when each place was shown (monotonic)
 * @param startedAt when the run started (monotonic)
 * @param result the scores (of whatever was measured, also for a stopped run)
 * @return the JSON
 */
std::string recordJson(const View& view, const double* shownAt, double startedAt, const Result& result);

/**
 * samples.csv: a row per sample received (the panel's time, the wall clock, the place, whether it counts, then the
 * sample's fields).
 * @param rows the samples
 * @return the text
 */
std::string samplesCsv(const std::vector<Row>& rows);

/**
 * An offset in words: "左に 7.6°、上に 2.4°" / "7.6° left, 2.4° up" (a part under 0.05° left out).
 * @param t texts
 * @param dx degrees, + right
 * @param dy degrees, + up
 * @return the text
 */
std::string shiftText(const UiText& t, double dx, double dy);

/**
 * A place in words: "真ん中" / "center", "右 20°・上 15°" / "20° right, 15° up".
 * @param t texts
 * @param position the place
 * @return the text
 */
std::string positionText(const UiText& t, const Position& position);

/**
 * A short line for the record list: "Valve 7.9°・瞳 1.3°" / "Valve 7.9°, pupil 1.3°".
 * @param t texts
 * @param result the scores
 * @return the line ("" if neither source counts)
 */
std::string briefText(const UiText& t, const Result& result);

/** The panel's end of the check's socket: bound only while a check runs (frameeyeosc sends while it is there). */
class Receiver {
public:
    Receiver() = default;
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    /**
     * Bind the socket (a stale one left by a crash is replaced).
     * @param path the socket path
     * @param error why it failed
     * @return true if listening
     */
    bool open(const std::string& path, std::string& error);

    /** Stop listening and remove the socket file. */
    void close();

    /** @return true while listening */
    bool isOpen() const { return fd_ >= 0; }

    /**
     * Read everything waiting.
     * @param out the samples read are added to it (anything else is ignored)
     * @return how many were added
     */
    size_t poll(std::vector<Sample>& out);

private:
    int fd_ = -1;
    std::string path_;
};

}  // namespace gaze_measure
