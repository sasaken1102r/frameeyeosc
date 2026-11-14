// "Measure gaze error" (Advanced > Debug tools): for about a minute a head-locked dot hops through kPositionCount
// places inside the eye fit's range, and for each the panel scores how far two gazes sit from it: Valve's combined
// gaze as gaze_source "valve" sends it, and the pupils' as "pupil" sends it (only while both pupils give it), both
// before the user's gaze scale. Nothing is written to config.json and nothing frameeyeosc sends changes: while this
// panel binds kSocketName in the status folder, frameeyeosc passes each sample's two gazes to it (src/dots.rs, "FEOM"),
// worked out a second time for the source not in use, as for the debug dots' compare dot (it starts cold when this
// panel's socket is first seen, within a second of the start; nothing counts before 1 s after the first place, 4 s
// after the start, so its smoothing has settled by then). Each sample is put to the
// place shown when it arrived, by the panel's own clock: the panel shows each place and receives the samples, so one
// clock times both. Only the samples from kSettleSec after a hop to the next hop count (the eyes have arrived), and
// only while both eyes are open enough. Valve is scored twice: on exactly the samples the pupils were scored on (both
// pupils, not easing from another source), its main number while a map is there so the two compare like with like,
// and on every counted sample. Nothing here talks to OpenVR or cairo (measure-test).
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
 * both eyes open enough, as the eye fit's captures count them, bit 2 Valve's gaze is held, bit 3 the pupils' gaze is
 * still easing from the source before: pupil.rs's blend; clear from a frameeyeosc before it; bit 4 the gazes are
 * before any smoothing, the same with smoothing on or off; from a frameeyeosc before it they are the gazes as sent,
 * smoothed), the pupils' state (0 none
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

/** Which way a run goes through kPositions: as listed, or the other way round (the countdown at the centre first
 *  either way). Runs alternate, so a drift over the minute and a place's own offset can be told apart. */
enum class Order { Forward, Reverse };

/**
 * @param order the order
 * @return "forward" or "reverse" (measure.json, samples.csv)
 */
const char* orderName(Order order);

/**
 * The order after a run that went `last` way.
 * @param last the last run's order
 * @return the other one
 */
inline Order nextOrder(Order last) { return last == Order::Forward ? Order::Reverse : Order::Forward; }

/**
 * The last run's order from its measure.json: its "order" (a record from before the alternation went forward).
 * @param json the file's text ("" if there is none)
 * @param found where to write whether there was a run at all
 * @return its order (Forward when none)
 */
Order orderInRecord(const std::string& json, bool& found);

/**
 * Which place a run shows at a step.
 * @param order the run's order
 * @param step 0.. kPositionCount-1
 * @return the place (index into kPositions)
 */
inline int placeAt(Order order, int step) { return order == Order::Forward ? step : kPositionCount - 1 - step; }

/** Where the pupils' gaze came from in a sample (pupil.rs's InUse, with None when there is no map). */
enum class PupilState : uint8_t { None = 0, Pupil, Left, Right, Hold, Valve };
constexpr int kStateCount = 6;

/**
 * What a sample's pupil gaze is worked out from, in a datagram of its own just before it (frameeyeosc's FEOR, version
 * 1, src/dots.rs), little-endian: b"FEOR", 1, 3 zero bytes, the sample time (f64), the eye server's pupil pair as read
 * (extra 0..3, f32: left x, y, right x, y), then each eye's eye-camera values as that sample read them (kRawEyeSize
 * bytes, the left eye first; zero without them): u32 flags (bit 0 fresh values with eyecam-rec's glint part, 1 a pupil,
 * 2 the slip worked out from that frame, 3 a template), u32 the template's id, u64 its time (CLOCK_MONOTONIC ns), u64
 * the frame's capture time (CLOCK_MONOTONIC ns: eyecam-rec's glints.csv's t_mono_ns), f32 x 2 the pupil centre (px),
 * f32 x 2 the slip (px), u32 glints matched, u32 frames since worked out, u32 glints found, u32 zero. A frameeyeosc
 * before it sends none: the samples then have no raw values (Raw::has false).
 */
constexpr size_t kRawSize = 144;
constexpr size_t kRawEyeSize = 56;

/** One eye's eye-camera values with a sample (frameeyeosc's eyecam_live::CamSlip). */
struct RawEye {
    bool present = false;    ///< fresh values with eyecam-rec's glint part
    bool pupil = false;      ///< a pupil in that frame (pupilX / pupilY)
    bool tracked = false;    ///< the slip was worked out from that frame
    bool templated = false;  ///< a template (templateId / templateNs)
    uint32_t templateId = 0;
    uint64_t templateNs = 0;
    uint64_t frameNs = 0;    ///< the frame's capture time (CLOCK_MONOTONIC ns)
    float pupilX = NAN;
    float pupilY = NAN;
    float slipX = 0;
    float slipY = 0;
    uint32_t matches = 0;
    uint32_t sinceGood = 0;
    uint32_t glints = 0;
};

/** A sample's raw values. */
struct Raw {
    bool has = false;                    ///< they came with the sample
    float pair[4] = {NAN, NAN, NAN, NAN};  ///< the eye server's pupil pair (extra 0..3)
    RawEye eye[2];                       ///< left, right
};

/** One sample from frameeyeosc. */
struct Sample {
    double time = 0.0;          ///< the eye server's sample time
    bool mapped = false;        ///< both eyes have a pupil map (else no pupil gaze)
    bool usable = false;        ///< both eyes open enough (the eye fit's rule)
    bool valveHeld = false;     ///< Valve's gaze was held (eyes shut, a blink)
    bool pupilBlending = false; ///< the pupils' gaze was still easing from the source before (not scored)
    bool beforeSmoothing = false;  ///< the gazes are before any smoothing (else as sent: an older frameeyeosc)
    PupilState pupilState = PupilState::None;
    float valve[2] = {0, 0};    ///< x, y; 1.0 = 45°
    float pupil[2] = {0, 0};    ///< x, y; meaningful while pupilState is Pupil
    float openness[2] = {NAN, NAN};
    Raw raw;                    ///< what the pupil gaze is worked out from (the FEOR before it, if one came)
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
 * Read a sample's raw values (FEOR).
 * @param data the bytes
 * @param size how many
 * @param time where to write the sample time they go with
 * @param out the values (has set)
 * @return false if it is not that datagram (of version 1)
 */
bool decodeRaw(const uint8_t* data, size_t size, double& time, Raw& out);

/**
 * Write a sample's raw values (for tests).
 * @param time the sample time
 * @param raw the values
 * @return the bytes
 */
std::vector<uint8_t> encodeRaw(double time, const Raw& raw);

/**
 * The raw values' columns in a record's samples.csv, after a sample's own: a comma first, then raw (whether they came),
 * pair_lx, pair_ly, pair_rx, pair_ry, and per eye (cam_l_*, then cam_r_*) fresh, frame_ns, pupil_x, pupil_y, tracked,
 * template, template_ns, slip_x, slip_y, matches, since_good, glints.
 * @return the text
 */
const char* rawCsvHeader();

/**
 * A sample's raw values as those columns (a comma first; empty cells without them).
 * @param out the line they are added to
 * @param raw the values
 */
void appendRawCsv(std::string& out, const Raw& raw);

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
    SourceScore valve;                ///< Valve on every counted sample
    SourceScore valveSame;            ///< Valve on the samples the pupils were scored on
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
    Overall valve;                    ///< Valve on every counted sample
    Overall valveSame;                ///< Valve on the pupils' samples (its main number while there is a map)
    Overall pupil;
    int window = 0;                   ///< samples in the windows
    int states[kStateCount] = {};
};

/**
 * Valve's main number: on the pupils' own samples (valveSame) while there is a map and it counts somewhere; on every
 * counted sample otherwise (no map, or no place where the pupils' samples were enough: many changes of source, each
 * eased for a quarter of a second).
 * @param result the scores
 * @return true if it is valveSame
 */
bool valveSameMain(const Result& result);

/**
 * @param result the scores
 * @return Valve's main number (valveSameMain)
 */
const Overall& valveMain(const Result& result);

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
    Order order = Order::Forward;  ///< which way this run goes
    int index = -1;            ///< the place shown (-1 in the countdown)
    int step = -1;             ///< ...as the how-many-th of the run (0-based; -1 in the countdown)
    double startUnix = 0.0;    ///< wall clock at the start (Valve's log can be matched to it)...
    double endUnix = 0.0;      ///< ...and at the end
    int positionsDone = 0;     ///< places fully shown
    bool mapped = false;       ///< samples said a pupil map is there
    bool beforeSmoothing = true;  ///< every sample's gazes were before smoothing (false if one came as sent)
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
     * @param order which way through the places
     */
    void start(double now, double unixNow, Order order = Order::Forward);

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
 * samples.csv: a row per sample received (the panel's time, the wall clock, the run's order, the place and its step in
 * the run, whether it counts, then the sample's fields, its raw values last: rawCsvHeader).
 * @param rows the samples
 * @param order the run's order
 * @return the text
 */
std::string samplesCsv(const std::vector<Row>& rows, Order order = Order::Forward);

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
    bool rawWaiting_ = false;   ///< a sample's raw values came: they go with the next sample of the same time
    double rawTime_ = 0.0;
    Raw raw_;
};

}  // namespace gaze_measure
