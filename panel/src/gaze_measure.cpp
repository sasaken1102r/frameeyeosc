// "Measure gaze error": the samples' datagram, the run's plan and timing, the scores and the record's files.
#include "gaze_measure.h"

#include "json.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace gaze_measure {

namespace {

constexpr uint8_t kMagic[4] = {'F', 'E', 'O', 'M'};
constexpr uint8_t kVersion = 1;
constexpr uint8_t kMapped = 1;
constexpr uint8_t kUsable = 2;
constexpr uint8_t kValveHeld = 4;
constexpr uint8_t kPupilBlending = 8;
constexpr uint8_t kBeforeSmoothing = 16;
constexpr double kDeg = M_PI / 180.0;

/**
 * The median of some numbers (NaN for none).
 * @param values the numbers (reordered)
 * @return the median
 */
double median(std::vector<double>& values) {
    if (values.empty()) return NAN;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

/**
 * A number as JSON: null for NaN.
 * @param value the number
 * @return the value
 */
JsonValue number(double value) {
    return std::isfinite(value) ? JsonValue::makeNumber(value) : JsonValue::makeNull();
}

/**
 * Score one source at one place.
 * @param offsets each sample's (dx, dy) in degrees
 * @param errors each sample's angle to the dot
 * @return its score
 */
SourceScore scoreOf(std::vector<double>& dx, std::vector<double>& dy, std::vector<double>& errors) {
    SourceScore s;
    s.n = static_cast<int>(errors.size());
    if (s.n == 0) return s;
    double sum = 0.0;
    for (double e : errors) sum += e;
    s.meanError = sum / s.n;
    s.dx = median(dx);
    s.dy = median(dy);
    s.error = median(errors);
    return s;
}

/**
 * One source over the run.
 * @param result the places
 * @param which the source's score in each place
 * @return the overall numbers
 */
Overall overallOf(const Result& result, SourceScore PositionScore::*which) {
    Overall o;
    std::vector<double> errors;
    double dx = 0.0;
    double dy = 0.0;
    for (int i = 0; i < kPositionCount; ++i) {
        const SourceScore& s = result.positions[i].*which;
        if (!s.counts()) continue;
        errors.push_back(s.error);
        dx += s.dx;
        dy += s.dy;
        if (o.worst < 0 || s.error > o.worstError) {
            o.worst = i;
            o.worstError = s.error;
        }
    }
    o.positions = static_cast<int>(errors.size());
    if (o.positions == 0) return o;
    double sum = 0.0;
    for (double e : errors) sum += e;
    o.meanError = sum / o.positions;
    o.dx = dx / o.positions;
    o.dy = dy / o.positions;
    o.error = median(errors);
    return o;
}

/**
 * Degrees with one decimal ("7.6").
 * @param value degrees
 * @return the text
 */
std::string oneDecimal(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", value);
    return text;
}

/**
 * Degrees as a place is named: whole ones without a decimal ("20", "7.5").
 * @param value degrees
 * @return the text
 */
std::string placeDegrees(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), value == std::floor(value) ? "%.0f" : "%.1f", value);
    return text;
}

/**
 * A text with one string in it.
 * @param format the format (one %s)
 * @param value the string
 * @return the text
 */
std::string fill(const char* format, const std::string& value) {
    char text[256];
    std::snprintf(text, sizeof(text), format, value.c_str());
    return text;
}

}  // namespace

// The centre, the four sides, the four corners (crossing over, so the eyes travel), then four in between
const Position kPositions[kPositionCount] = {
    {0, 0},      {-20, 0},  {20, 0},    {0, 15},    {0, -15},   {-20, 15}, {20, -15}, {20, 15},
    {-20, -15}, {-10, 0},  {10, 0},    {-10, 7.5}, {10, -7.5}, {10, 7.5}, {-10, -7.5},
};

const char* orderName(Order order) {
    return order == Order::Forward ? "forward" : "reverse";
}

Order orderInRecord(const std::string& json, bool& found) {
    found = false;
    JsonValue root;
    std::string error;
    if (json.empty() || !parseJson(json, root, error) || !root.isObject()) return Order::Forward;
    found = true;
    const JsonValue* order = root.get("order");
    return order != nullptr && order->isString() && order->text == orderName(Order::Reverse) ? Order::Reverse
                                                                                              : Order::Forward;
}

bool decode(const uint8_t* data, size_t size, Sample& out) {
    if (size != kSampleSize || std::memcmp(data, kMagic, 4) != 0 || data[4] != kVersion) return false;
    Sample s;
    s.mapped = (data[5] & kMapped) != 0;
    s.usable = (data[5] & kUsable) != 0;
    s.valveHeld = (data[5] & kValveHeld) != 0;
    s.pupilBlending = (data[5] & kPupilBlending) != 0;
    s.beforeSmoothing = (data[5] & kBeforeSmoothing) != 0;
    s.pupilState = data[6] < kStateCount ? static_cast<PupilState>(data[6]) : PupilState::None;
    std::memcpy(&s.time, data + 8, 8);
    std::memcpy(s.valve, data + 16, 8);
    std::memcpy(s.pupil, data + 24, 8);
    std::memcpy(s.openness, data + 32, 8);
    if (!std::isfinite(s.valve[0]) || !std::isfinite(s.valve[1])) return false;
    if (s.pupilState == PupilState::Pupil && !(std::isfinite(s.pupil[0]) && std::isfinite(s.pupil[1]))) return false;
    out = s;
    return true;
}

std::vector<uint8_t> encode(const Sample& sample) {
    std::vector<uint8_t> data(kSampleSize, 0);
    std::memcpy(data.data(), kMagic, 4);
    data[4] = kVersion;
    data[5] = static_cast<uint8_t>((sample.mapped ? kMapped : 0) | (sample.usable ? kUsable : 0) |
                                   (sample.valveHeld ? kValveHeld : 0) | (sample.pupilBlending ? kPupilBlending : 0) |
                                   (sample.beforeSmoothing ? kBeforeSmoothing : 0));
    data[6] = static_cast<uint8_t>(sample.pupilState);
    std::memcpy(data.data() + 8, &sample.time, 8);
    std::memcpy(data.data() + 16, sample.valve, 8);
    std::memcpy(data.data() + 24, sample.pupil, 8);
    std::memcpy(data.data() + 32, sample.openness, 8);
    return data;
}

void targetGaze(const Position& position, double out[2]) {
    // The place as the eye fit's target is put (VrOverlay::showTarget): along (sin yaw cos pitch, sin pitch,
    // -cos yaw cos pitch) from between the eyes; its gaze angles are sideways and up over the forward distance
    const double yaw = position.yawDeg * kDeg;
    const double pitch = position.pitchDeg * kDeg;
    const double ahead = std::cos(yaw) * std::cos(pitch);
    out[0] = std::atan2(std::sin(yaw) * std::cos(pitch), ahead) / kDeg;
    out[1] = std::atan2(std::sin(pitch), ahead) / kDeg;
}

double angleBetween(const double a[2], const double b[2]) {
    const double ax = std::tan(a[0] * kDeg);
    const double ay = std::tan(a[1] * kDeg);
    const double bx = std::tan(b[0] * kDeg);
    const double by = std::tan(b[1] * kDeg);
    const double dot = ax * bx + ay * by + 1.0;
    const double la = std::sqrt(ax * ax + ay * ay + 1.0);
    const double lb = std::sqrt(bx * bx + by * by + 1.0);
    return std::acos(std::clamp(dot / (la * lb), -1.0, 1.0)) / kDeg;
}

bool valveSameMain(const Result& result) {
    return result.mapped && result.valveSame.positions > 0;
}

const Overall& valveMain(const Result& result) {
    return valveSameMain(result) ? result.valveSame : result.valve;
}

Result score(const std::vector<Row>& rows) {
    Result result;
    std::vector<double> vdx[kPositionCount], vdy[kPositionCount], verr[kPositionCount];
    std::vector<double> pdx[kPositionCount], pdy[kPositionCount], perr[kPositionCount];
    std::vector<double> sdx[kPositionCount], sdy[kPositionCount], serr[kPositionCount];
    for (int i = 0; i < kPositionCount; ++i) {
        result.positions[i].target = kPositions[i];
        targetGaze(kPositions[i], result.positions[i].gaze);
    }
    for (const Row& row : rows) {
        if (row.sample.mapped) result.mapped = true;
        if (!row.scored || row.position < 0 || row.position >= kPositionCount) continue;
        PositionScore& p = result.positions[row.position];
        const int state = static_cast<int>(row.sample.pupilState);
        ++p.window;
        ++p.states[state];
        ++result.window;
        ++result.states[state];
        // Only samples with both eyes open enough (blinks and shut eyes left out), as the eye fit counts them
        if (!row.sample.usable) continue;
        const double valve[2] = {row.sample.valve[0] * 45.0, row.sample.valve[1] * 45.0};
        vdx[row.position].push_back(valve[0] - p.gaze[0]);
        vdy[row.position].push_back(valve[1] - p.gaze[1]);
        verr[row.position].push_back(angleBetween(valve, p.gaze));
        // The pupils only from both pupils and not while easing from another source; Valve again on exactly those
        if (row.sample.pupilState == PupilState::Pupil && !row.sample.pupilBlending) {
            const double pupil[2] = {row.sample.pupil[0] * 45.0, row.sample.pupil[1] * 45.0};
            pdx[row.position].push_back(pupil[0] - p.gaze[0]);
            pdy[row.position].push_back(pupil[1] - p.gaze[1]);
            perr[row.position].push_back(angleBetween(pupil, p.gaze));
            sdx[row.position].push_back(vdx[row.position].back());
            sdy[row.position].push_back(vdy[row.position].back());
            serr[row.position].push_back(verr[row.position].back());
        }
    }
    for (int i = 0; i < kPositionCount; ++i) {
        result.positions[i].valve = scoreOf(vdx[i], vdy[i], verr[i]);
        result.positions[i].valveSame = scoreOf(sdx[i], sdy[i], serr[i]);
        result.positions[i].pupil = scoreOf(pdx[i], pdy[i], perr[i]);
    }
    result.valve = overallOf(result, &PositionScore::valve);
    result.valveSame = overallOf(result, &PositionScore::valveSame);
    result.pupil = overallOf(result, &PositionScore::pupil);
    return result;
}

void Session::start(double now, double unixNow, Order order) {
    view_ = View();
    view_.phase = Phase::Countdown;
    view_.order = order;
    view_.startUnix = unixNow;
    rows_.clear();
    rows_.reserve(6000);
    start_ = now;
    lastSample_ = now;
    for (double& at : shownAt_) at = NAN;
    ended_ = false;
    appeared_ = false;
}

void Session::end(Phase phase, double unixNow) {
    view_.phase = phase;
    view_.endUnix = unixNow;
    view_.result = score(rows_);
    ended_ = true;
}

void Session::stop(double now, double unixNow) {
    (void)now;
    if (!active()) return;
    end(Phase::Stopped, unixNow);
}

Tick Session::tick(double now, double unixNow) {
    Tick tick;
    if (!active()) {
        if (ended_) {
            tick.ended = true;
            ended_ = false;
        }
        return tick;
    }
    // No samples for a while: frameeyeosc stopped, or the eyes aren't tracked
    if (now - lastSample_ > kNoDataSec) {
        end(Phase::Failed, unixNow);
        ended_ = false;
        tick.ended = true;
        return tick;
    }
    const double runFrom = start_ + kCountdownSec;
    if (now < runFrom) {
        // The countdown at the centre: 3, 2, 1
        tick.showTarget = true;
        tick.yawDeg = kPositions[0].yawDeg;
        tick.pitchDeg = kPositions[0].pitchDeg;
        tick.seconds = static_cast<int>(std::ceil(runFrom - now));
        tick.progress = (runFrom - now) / kCountdownSec;
        // (the dot appearing: a cue)
        tick.hopped = !appeared_;
        appeared_ = true;
        return tick;
    }
    const int step = static_cast<int>(std::floor((now - runFrom) / kHoldSec));
    if (step >= kPositionCount) {
        view_.positionsDone = kPositionCount;
        end(Phase::Done, unixNow);
        ended_ = false;
        tick.ended = true;
        return tick;
    }
    view_.phase = Phase::Running;
    const int index = placeAt(view_.order, step);
    if (step != view_.step) {
        // The dot moves now: its window is timed from when it is shown
        view_.positionsDone = std::max(0, step);
        view_.step = step;
        view_.index = index;
        shownAt_[index] = now;
        tick.hopped = true;
    }
    tick.showTarget = true;
    tick.yawDeg = kPositions[index].yawDeg;
    tick.pitchDeg = kPositions[index].pitchDeg;
    tick.progress = std::clamp(1.0 - (now - shownAt_[index]) / kHoldSec, 0.0, 1.0);
    return tick;
}

void Session::add(double now, double unixNow, const Sample& sample) {
    if (!active()) return;
    lastSample_ = now;
    ++view_.samples;
    if (sample.mapped) view_.mapped = true;
    if (!sample.beforeSmoothing) view_.beforeSmoothing = false;
    if (rows_.size() >= kMaxRows) return;
    Row row;
    row.at = now;
    row.wall = unixNow;
    row.sample = sample;
    // The place shown when it arrived (the one shown last at or before it, whichever way the run goes), counted from
    // kSettleSec after it was shown until the next one
    for (int i = 0; i < kPositionCount; ++i) {
        if (!std::isfinite(shownAt_[i]) || shownAt_[i] > now) continue;
        if (row.position < 0 || shownAt_[i] > shownAt_[row.position]) row.position = i;
    }
    if (row.position >= 0) {
        const double shown = shownAt_[row.position];
        row.scored = now >= shown + kSettleSec && now < shown + kHoldSec;
    }
    rows_.push_back(row);
}

void Session::dismiss() {
    if (active()) return;
    view_ = View();
    rows_.clear();
    ended_ = false;
}

std::string recordJson(const View& view, const double* shownAt, double startedAt, const Result& result) {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    const char* phases[] = {"idle", "countdown", "running", "done", "stopped", "failed"};
    root.set("result", JsonValue::makeString(phases[static_cast<int>(view.phase)]));
    // (whole milliseconds: the JSON writer keeps 6 digits of other numbers)
    root.set("start_ms", JsonValue::makeNumber(std::llround(view.startUnix * 1000), true));
    root.set("end_ms", JsonValue::makeNumber(std::llround(view.endUnix * 1000), true));
    root.set("samples", JsonValue::makeNumber(view.samples, true));
    // Which way the run went through the places (they alternate run by run); each place's step is its "run_index"
    root.set("order", JsonValue::makeString(orderName(view.order)));
    root.set("mapped", JsonValue::makeBool(result.mapped));
    // Before smoothing (and so the same with smoothing on or off), or as sent by a frameeyeosc from before that
    root.set("values", JsonValue::makeString(view.beforeSmoothing ? "before smoothing" : "as sent (smoothed)"));
    JsonValue plan;
    plan.type = JsonValue::Type::Object;
    plan.set("countdown_s", JsonValue::makeNumber(kCountdownSec));
    plan.set("hold_s", JsonValue::makeNumber(kHoldSec));
    plan.set("settle_s", JsonValue::makeNumber(kSettleSec));
    plan.set("min_samples", JsonValue::makeNumber(kMinSamples, true));
    plan.set("distance_m", JsonValue::makeNumber(0.9));
    plan.set("valve", JsonValue::makeString("combined gaze as gaze_source valve works it out, before smoothing and "
                                            "the gaze scale"));
    plan.set("pupil", JsonValue::makeString("combined gaze as gaze_source pupil works it out, before smoothing and the "
                                            "gaze scale, only while both pupils give it"));
    plan.set("counted", JsonValue::makeString("samples with both eyes open enough, from settle_s after the hop"));
    plan.set("valve_same", JsonValue::makeString("Valve on exactly the samples the pupils were scored on (both pupils, "
                                                 "not easing from another source)"));
    root.set("plan", plan);
    const auto source = [](const SourceScore& s) {
        JsonValue v;
        v.type = JsonValue::Type::Object;
        v.set("n", JsonValue::makeNumber(s.n, true));
        v.set("dx_deg", number(s.dx));
        v.set("dy_deg", number(s.dy));
        v.set("error_deg", number(s.error));
        v.set("mean_error_deg", number(s.meanError));
        v.set("counts", JsonValue::makeBool(s.counts()));
        return v;
    };
    const char* stateNames[kStateCount] = {"none", "pupil", "left", "right", "hold", "valve"};
    JsonValue positions;
    positions.type = JsonValue::Type::Array;
    for (int i = 0; i < kPositionCount; ++i) {
        const PositionScore& p = result.positions[i];
        JsonValue v;
        v.type = JsonValue::Type::Object;
        v.set("index", JsonValue::makeNumber(i, true));
        v.set("run_index", JsonValue::makeNumber(placeAt(view.order, i), true));
        v.set("yaw_deg", JsonValue::makeNumber(p.target.yawDeg));
        v.set("pitch_deg", JsonValue::makeNumber(p.target.pitchDeg));
        v.set("target_gaze_x_deg", JsonValue::makeNumber(p.gaze[0]));
        v.set("target_gaze_y_deg", JsonValue::makeNumber(p.gaze[1]));
        v.set("shown_s", number(shownAt != nullptr ? shownAt[i] - startedAt : NAN));
        v.set("window", JsonValue::makeNumber(p.window, true));
        JsonValue states;
        states.type = JsonValue::Type::Object;
        for (int s = 0; s < kStateCount; ++s) states.set(stateNames[s], JsonValue::makeNumber(p.states[s], true));
        v.set("pupil_states", states);
        v.set("valve", source(p.valve));
        v.set("valve_same", source(p.valveSame));
        v.set("pupil", source(p.pupil));
        positions.items.push_back(v);
    }
    root.set("positions", positions);
    const auto overall = [](const Overall& o) {
        JsonValue v;
        v.type = JsonValue::Type::Object;
        v.set("positions", JsonValue::makeNumber(o.positions, true));
        v.set("error_deg", number(o.error));
        v.set("mean_error_deg", number(o.meanError));
        v.set("dx_deg", number(o.dx));
        v.set("dy_deg", number(o.dy));
        v.set("worst", o.worst >= 0 ? JsonValue::makeNumber(o.worst, true) : JsonValue::makeNull());
        v.set("worst_error_deg", number(o.worstError));
        return v;
    };
    root.set("valve", overall(result.valve));
    root.set("valve_same", overall(result.valveSame));
    root.set("valve_main", JsonValue::makeString(valveSameMain(result) ? "valve_same" : "valve"));
    root.set("pupil", overall(result.pupil));
    JsonValue states;
    states.type = JsonValue::Type::Object;
    for (int s = 0; s < kStateCount; ++s) states.set(stateNames[s], JsonValue::makeNumber(result.states[s], true));
    root.set("pupil_states", states);
    root.set("note", JsonValue::makeString("Valve's self-calibration may use what is on screen; compare start_ms and "
                                           "end_ms with Valve's eye tracker log (logs.txt)"));
    return writeJson(root) + "\n";
}

std::string samplesCsv(const std::vector<Row>& rows, Order order) {
    std::string out =
        "panel_s,unix,order,run_index,position,scored,sample_time,mapped,usable,valve_held,pupil_state,pupil_blending,before_smoothing,"
        "valve_x,valve_y,pupil_x,pupil_y,openness_left,openness_right\n";
    char line[320];
    for (const Row& r : rows) {
        const Sample& s = r.sample;
        std::snprintf(line, sizeof(line), "%.4f,%.3f,%s,%d,%d,%d,%.6f,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.4f,%.4f\n",
                      r.at, r.wall, orderName(order), r.position < 0 ? -1 : placeAt(order, r.position), r.position, r.scored ? 1 : 0, s.time, s.mapped ? 1 : 0, s.usable ? 1 : 0,
                      s.valveHeld ? 1 : 0, static_cast<int>(s.pupilState), s.pupilBlending ? 1 : 0,
                      s.beforeSmoothing ? 1 : 0, s.valve[0],
                      s.valve[1], s.pupil[0],
                      s.pupil[1], s.openness[0], s.openness[1]);
        out += line;
    }
    return out;
}

std::string shiftText(const UiText& t, double dx, double dy) {
    std::string parts[2];
    if (std::fabs(dx) >= 0.05) parts[0] = fill(dx < 0 ? t.measureLeftFormat : t.measureRightFormat, oneDecimal(std::fabs(dx)));
    if (std::fabs(dy) >= 0.05) parts[1] = fill(dy < 0 ? t.measureDownFormat : t.measureUpFormat, oneDecimal(std::fabs(dy)));
    if (parts[0].empty()) return parts[1].empty() ? std::string(t.measureNoShift) : parts[1];
    if (parts[1].empty()) return parts[0];
    return parts[0] + t.measureAnd + parts[1];
}

std::string positionText(const UiText& t, const Position& position) {
    std::string parts[2];
    if (position.yawDeg != 0) {
        parts[0] = fill(position.yawDeg < 0 ? t.measurePlaceLeft : t.measurePlaceRight,
                        placeDegrees(std::fabs(position.yawDeg)));
    }
    if (position.pitchDeg != 0) {
        parts[1] = fill(position.pitchDeg < 0 ? t.measurePlaceDown : t.measurePlaceUp,
                        placeDegrees(std::fabs(position.pitchDeg)));
    }
    if (parts[0].empty() && parts[1].empty()) return t.measurePlaceCenter;
    if (parts[0].empty()) return parts[1];
    if (parts[1].empty()) return parts[0];
    return parts[0] + t.measurePlaceAnd + parts[1];
}

std::string briefText(const UiText& t, const Result& result) {
    std::string out;
    const Overall& valve = valveMain(result);
    if (valve.positions > 0) out = fill(t.measureBriefValve, oneDecimal(valve.error));
    if (result.pupil.positions > 0) {
        if (!out.empty()) out += t.measureBriefAnd;
        out += fill(t.measureBriefPupil, oneDecimal(result.pupil.error));
    }
    return out;
}

Receiver::~Receiver() {
    close();
}

bool Receiver::open(const std::string& path, std::string& error) {
    close();
    sockaddr_un address {};
    if (path.size() >= sizeof(address.sun_path)) {
        error = "socket path too long: " + path;
        return false;
    }
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    // Only this panel binds it (single instance), so an existing file is left over from a crash
    ::unlink(path.c_str());
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        error = "bind " + path + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    fd_ = fd;
    path_ = path;
    return true;
}

void Receiver::close() {
    if (fd_ < 0) return;
    ::close(fd_);
    ::unlink(path_.c_str());
    fd_ = -1;
}

size_t Receiver::poll(std::vector<Sample>& out) {
    if (fd_ < 0) return 0;
    size_t added = 0;
    uint8_t buffer[kSampleSize + 8];
    for (;;) {
        const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), 0);
        if (n < 0) break;  // EAGAIN: nothing more waiting
        Sample sample;
        if (decode(buffer, static_cast<size_t>(n), sample)) {
            out.push_back(sample);
            ++added;
        }
    }
    return added;
}

}  // namespace gaze_measure
