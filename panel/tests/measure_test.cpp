// Tests for the gaze error check (gaze_measure.cpp): its datagram, the plan's timing, the scores, the record's files
// and the socket, without OpenVR. Built with the panel as measure-test; exits non-zero on failure. All numbers are
// made up.
#include "gaze_measure.h"
#include "json.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
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
 * Whether two numbers are within a tolerance.
 * @param a one
 * @param b the other
 * @param tolerance how close
 * @return true if close enough
 */
bool near(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) < tolerance;
}

using namespace gaze_measure;

void testDatagram() {
    Sample sample;
    sample.time = 19892.729813125;
    sample.mapped = true;
    sample.usable = true;
    sample.pupilState = PupilState::Pupil;
    sample.valve[0] = 0.125f;
    sample.valve[1] = -0.25f;
    sample.pupil[0] = 0.5f;
    sample.pupil[1] = 0.0625f;
    sample.openness[0] = 0.75f;
    sample.openness[1] = 0.5f;
    const std::vector<uint8_t> bytes = encode(sample);
    CHECK(bytes.size() == kSampleSize && kSampleSize == 40);
    // The same bytes frameeyeosc writes (src/dots.rs, the_measure_datagram_reads_back_and_is_no_other)
    CHECK(std::memcmp(bytes.data(), "FEOM\x01\x03\x01\x00", 8) == 0);
    Sample read;
    CHECK(decode(bytes.data(), bytes.size(), read));
    CHECK(read.time == sample.time && read.mapped && read.usable && !read.valveHeld && read.pupilState == PupilState::Pupil);
    CHECK(read.valve[0] == 0.125f && read.valve[1] == -0.25f && read.pupil[0] == 0.5f && read.pupil[1] == 0.0625f);
    CHECK(read.openness[0] == 0.75f && read.openness[1] == 0.5f);
    // Held, no map, not counted: bit 2 only, state 0
    Sample held;
    held.valveHeld = true;
    const std::vector<uint8_t> heldBytes = encode(held);
    CHECK(std::memcmp(heldBytes.data(), "FEOM\x01\x04\x00\x00", 8) == 0);
    CHECK(decode(heldBytes.data(), heldBytes.size(), read) && read.valveHeld && !read.mapped && !read.usable);
    // Still easing: bit 3 (as frameeyeosc writes it), nothing else moves; a frameeyeosc before it: clear
    Sample easing = sample;
    easing.pupilBlending = true;
    const std::vector<uint8_t> eased = encode(easing);
    CHECK(std::memcmp(eased.data(), "FEOM\x01\x0b\x01\x00", 8) == 0);
    int differ = 0;
    for (size_t i = 0; i < kSampleSize; ++i) differ += eased[i] != bytes[i] ? 1 : 0;
    CHECK(differ == 1 && decode(eased.data(), eased.size(), read) && read.pupilBlending);
    CHECK(decode(bytes.data(), bytes.size(), read) && !read.pupilBlending);
    // Before smoothing: bit 4 (as frameeyeosc writes it); clear from a frameeyeosc before it
    Sample before = sample;
    before.beforeSmoothing = true;
    const std::vector<uint8_t> beforeBytes = encode(before);
    CHECK(std::memcmp(beforeBytes.data(), "FEOM\x01\x13\x01\x00", 8) == 0);
    CHECK(decode(beforeBytes.data(), beforeBytes.size(), read) && read.beforeSmoothing);
    CHECK(decode(bytes.data(), bytes.size(), read) && !read.beforeSmoothing);
    // Another version or size, Valve's gaze not a number, the pupils' not a number while they count: ignored; the
    // pupils' not a number while another state is in use: read
    std::vector<uint8_t> wrong = bytes;
    wrong[4] = 2;
    CHECK(!decode(wrong.data(), wrong.size(), read));
    CHECK(!decode(bytes.data(), bytes.size() - 1, read));
    const uint8_t nan[4] = {0, 0, 0xc0, 0x7f};
    wrong = bytes;
    std::memcpy(wrong.data() + 16, nan, 4);
    CHECK(!decode(wrong.data(), wrong.size(), read));
    wrong = bytes;
    std::memcpy(wrong.data() + 24, nan, 4);
    CHECK(!decode(wrong.data(), wrong.size(), read));
    wrong[6] = 2;
    CHECK(decode(wrong.data(), wrong.size(), read) && read.pupilState == PupilState::Left);
    // A state frameeyeosc doesn't have: none
    wrong[6] = 9;
    CHECK(decode(wrong.data(), wrong.size(), read) && read.pupilState == PupilState::None);
    // The debug dots' datagrams are not this
    std::vector<uint8_t> dots(160, 0);
    std::memcpy(dots.data(), "FEOD\x03", 5);
    CHECK(!decode(dots.data(), dots.size(), read));
}

/** The raw values' datagram (FEOR) as frameeyeosc writes it (src/dots.rs, the_raw_datagram_reads_back_and_is_no_other). */
void testRawDatagram() {
    Raw raw;
    raw.has = true;
    const float pair[4] = {0.077017f, 0.019944f, 0.105099f, 0.034945f};
    std::memcpy(raw.pair, pair, sizeof(pair));
    RawEye& left = raw.eye[0];
    left.present = left.pupil = left.tracked = left.templated = true;
    left.templateId = 3;
    left.templateNs = 1200000000000ULL;
    left.frameNs = 1234567890123ULL;
    left.pupilX = 240.5f;
    left.pupilY = 190.25f;
    left.slipX = 1.5f;
    left.slipY = -2.25f;
    left.matches = 4;
    left.glints = 6;
    const std::vector<uint8_t> bytes = encodeRaw(19892.729813125, raw);
    CHECK(bytes.size() == kRawSize && kRawSize == 144);
    CHECK(std::memcmp(bytes.data(), "FEOR\x01\x00\x00\x00", 8) == 0);
    uint32_t flags = 0;
    std::memcpy(&flags, bytes.data() + 32, 4);
    uint64_t frame = 0;
    std::memcpy(&frame, bytes.data() + 48, 8);
    CHECK(flags == 15 && frame == 1234567890123ULL);
    bool rightZero = true;
    for (size_t i = 88; i < kRawSize; ++i) rightZero = rightZero && bytes[i] == 0;
    CHECK(rightZero);
    double time = 0;
    Raw read;
    CHECK(decodeRaw(bytes.data(), bytes.size(), time, read) && time == 19892.729813125 && read.has);
    CHECK(read.pair[2] == 0.105099f && read.eye[0].present && read.eye[0].templateId == 3 && read.eye[0].glints == 6);
    CHECK(read.eye[0].pupilY == 190.25f && read.eye[0].slipY == -2.25f && !read.eye[1].present);
    // Not a sample, nor a sample this; another version or size: not this either
    Sample sample;
    CHECK(!decode(bytes.data(), bytes.size(), sample) && !decode(bytes.data(), kSampleSize, sample));
    CHECK(!decodeRaw(encode(sample).data(), kSampleSize, time, read));
    std::vector<uint8_t> wrong = bytes;
    wrong[4] = 2;
    CHECK(!decodeRaw(wrong.data(), wrong.size(), time, read) && !decodeRaw(bytes.data(), kRawSize - 1, time, read));
    // As samples.csv columns: the pair, then each eye (an eye without values: 0 and empty cells)
    std::string line;
    CHECK(decodeRaw(bytes.data(), bytes.size(), time, read));
    appendRawCsv(line, read);
    CHECK(line == ",1,0.077017,0.019944,0.105099,0.034945,1,1234567890123,240.500,190.250,1,3,1200000000000,1.500,-2.250,4,0,6,"
                  "0,,,,,,,,,,,");
    size_t header = 0;
    for (const char* c = rawCsvHeader(); *c; ++c) header += *c == ',' ? 1 : 0;
    size_t cells = 0;
    for (char c : line) cells += c == ',' ? 1 : 0;
    CHECK(header == 29 && cells == header);
    std::string none;
    appendRawCsv(none, Raw());
    cells = 0;
    for (char c : none) cells += c == ',' ? 1 : 0;
    CHECK(none.rfind(",0,,", 0) == 0 && cells == header);
}

void testGeometry() {
    double g[2];
    targetGaze({0, 0}, g);
    CHECK(near(g[0], 0) && near(g[1], 0));
    targetGaze({20, 0}, g);
    CHECK(near(g[0], 20) && near(g[1], 0));
    targetGaze({0, -15}, g);
    CHECK(near(g[0], 0) && near(g[1], -15));
    // A corner: sideways as its yaw, up over the forward distance (a little more than its pitch)
    targetGaze({20, 15}, g);
    const double up = std::atan(std::tan(15 * M_PI / 180) / std::cos(20 * M_PI / 180)) * 180 / M_PI;
    CHECK(near(g[0], 20) && near(g[1], up) && g[1] > 15.9 && g[1] < 16.0);
    const double a[2] = {0, 0};
    const double b[2] = {10, 0};
    const double c[2] = {3, 4};
    CHECK(near(angleBetween(a, a), 0, 1e-6) && near(angleBetween(a, b), 10, 1e-9));
    CHECK(near(angleBetween(a, c), 5, 0.02) && near(angleBetween(b, a), 10, 1e-9));
    // The plan: inside the eye fit's range, the centre first, every place different
    for (int i = 0; i < kPositionCount; ++i) {
        CHECK(std::fabs(kPositions[i].yawDeg) <= 20 && std::fabs(kPositions[i].pitchDeg) <= 15);
        for (int j = 0; j < i; ++j) {
            CHECK(kPositions[i].yawDeg != kPositions[j].yawDeg || kPositions[i].pitchDeg != kPositions[j].pitchDeg);
        }
    }
    CHECK(kPositions[0].yawDeg == 0 && kPositions[0].pitchDeg == 0);
    CHECK(near(kRunSec, 55.5));
}

/** A run at 90 samples a second with the panel ticking just before each: Valve off by (-7.6, +2.4), the pupils by
 *  (+0.5, -0.3); the eyes still travelling for 0.5 s after each hop, and a blink with wild values. */
struct Simulation {
    Session session;
    int hops = 0;
    int ends = 0;
    int easingPlaces = 0;   // the pupils easing all through the first this many places
    std::vector<int> countdown;
    double now = 100.0;
    bool mapped = true;

    void run(double until, bool samples = true) {
        const double dt = 1.0 / 90;
        while (now < until) {
            const Tick tick = session.tick(now, 1.79e9 + now);
            hops += tick.hopped ? 1 : 0;
            ends += tick.ended ? 1 : 0;
            if (tick.seconds > 0 && (countdown.empty() || countdown.back() != tick.seconds)) countdown.push_back(tick.seconds);
            if (samples && session.active()) session.add(now + 0.002, 1.79e9 + now, sampleAt(now));
            now += dt;
        }
    }

    Sample sampleAt(double at) const {
        Sample s;
        s.time = at * 1.5 + 7;  // (the eye server's own clock: not the panel's)
        s.mapped = mapped;
        s.usable = true;
        const View& v = session.view();
        const int index = v.index;
        double g[2] = {0, 0};
        if (index >= 0) targetGaze(kPositions[index], g);
        const double since = index >= 0 ? at - session.shownAt()[index] : 0;
        const bool travelling = index >= 0 && since < 0.5;
        const bool blink = index == 5 && since > 2.0 && since < 2.2;
        // Where only the left pupil is seen (place 3), Valve is worse: off by 15° there
        const double vx = travelling ? 30 : g[0] - (index == 3 ? 15.0 : 7.6);
        const double vy = travelling ? -25 : g[1] + 2.4;
        s.valve[0] = static_cast<float>(vx / 45);
        s.valve[1] = static_cast<float>(vy / 45);
        if (blink) {
            s.usable = false;
            s.valve[0] = 0.9f;
        }
        if (mapped) {
            s.pupilState = index == 3 ? PupilState::Left : PupilState::Pupil;
            s.pupil[0] = static_cast<float>((travelling ? -30 : g[0] + 0.5) / 45);
            s.pupil[1] = static_cast<float>((travelling ? 20 : g[1] - 0.3) / 45);
            if (index >= 0 && index < easingPlaces) s.pupilBlending = true;
            // At place 7 the pupils come back from a moment of one eye half-way through: easing, and wild
            if (index == 7 && since >= 2.0 && since < 2.25) {
                s.pupilBlending = true;
                s.pupil[0] = static_cast<float>((g[0] + 9) / 45);
            }
        }
        return s;
    }
};

void testRun() {
    Simulation sim;
    sim.session.start(sim.now, 1.79e9 + sim.now);
    CHECK(sim.session.active() && sim.session.view().phase == Phase::Countdown);
    CHECK(sim.session.view().startUnix == 1.79e9 + 100.0);
    sim.run(100.0 + 2.0);
    CHECK(sim.session.view().phase == Phase::Countdown && sim.hops == 1);
    sim.run(100.0 + kRunSec + 1.0);
    // The countdown 3, 2, 1 at the centre; the dot appearing and each hop a cue; one end
    CHECK((sim.countdown == std::vector<int> {3, 2, 1}));
    CHECK(sim.hops == 1 + kPositionCount && sim.ends == 1);
    const View& v = sim.session.view();
    CHECK(v.phase == Phase::Done && v.positionsDone == kPositionCount && !sim.session.active());
    CHECK(near(v.endUnix - v.startUnix, kRunSec, 0.05));
    const Result& r = v.result;
    CHECK(r.mapped);
    // Each place: about 2.5 s of samples counted (from 1 s after its hop), the travel and the blink left out
    for (int i = 0; i < kPositionCount; ++i) {
        const PositionScore& p = r.positions[i];
        CHECK(p.window >= 223 && p.window <= 227);
        CHECK(near(p.valve.dx, i == 3 ? -15.0 : -7.6, 1e-4) && near(p.valve.dy, 2.4, 1e-4));
        CHECK(i != 5 ? p.valve.n == p.window : (p.valve.n >= p.window - 19 && p.valve.n <= p.window - 17));
        if (i == 3) {
            CHECK(p.pupil.n == 0 && !p.pupil.counts() && p.states[static_cast<int>(PupilState::Left)] == p.window);
            CHECK(p.valveSame.n == 0 && !p.valveSame.counts() && p.valve.counts());
        } else {
            CHECK(near(p.pupil.dx, 0.5, 1e-4) && near(p.pupil.dy, -0.3, 1e-4) && p.pupil.counts());
            // Valve on the same samples as the pupils: exactly as many
            CHECK(p.valveSame.n == p.pupil.n && near(p.valveSame.dx, -7.6, 1e-4));
        }
        // The easing samples left out of the pupils (and so of Valve on the same samples), not of Valve's own
        if (i == 7) CHECK(p.pupil.n >= p.window - 23 && p.pupil.n <= p.window - 21 && p.valve.n == p.window);
    }
    // Overall: the mean offset; the error about the offsets' length (a little more at the corners). Valve on every
    // sample has place 3 (15° off) in it; on the pupils' samples not, like the pupils
    CHECK(r.valve.positions == kPositionCount && near(r.valve.dx, (-7.6 * 14 - 15.0) / 15, 1e-4));
    CHECK(near(r.valve.dy, 2.4, 1e-4) && r.valve.meanError > r.valveSame.meanError + 0.4);
    CHECK(r.valveSame.positions == kPositionCount - 1 && near(r.valveSame.dx, -7.6, 1e-4));
    CHECK(r.valveSame.error > 7.8 && r.valveSame.error < 8.5 && r.valveSame.meanError > 7.8 && r.valveSame.meanError < 8.6);
    CHECK(r.valve.worst == 3 && r.valveSame.worst != 3);
    CHECK(r.pupil.positions == kPositionCount - 1 && near(r.pupil.dx, 0.5, 1e-4) && near(r.pupil.dy, -0.3, 1e-4));
    CHECK(r.pupil.error > 0.55 && r.pupil.error < 0.65);
    CHECK(near(r.valve.worstError, r.positions[3].valve.error));
    CHECK(r.states[static_cast<int>(PupilState::Left)] == r.positions[3].window);
    // The record's files: JSON with every place, a CSV row a sample
    const std::string json = recordJson(v, sim.session.shownAt(), sim.session.startedAt(), r);
    JsonValue root;
    std::string error;
    CHECK(parseJson(json, root, error));
    const JsonValue* positions = root.get("positions");
    CHECK(positions != nullptr && positions->isArray() && positions->items.size() == kPositionCount);
    CHECK(root.get("result") != nullptr && root.get("result")->text == "done");
    CHECK(root.get("start_ms") != nullptr && near(root.get("start_ms")->number, 1790000100000.0, 1.0));
    CHECK(root.get("end_ms") != nullptr && near(root.get("end_ms")->number - root.get("start_ms")->number, kRunSec * 1000, 50));
    if (positions != nullptr && positions->items.size() == kPositionCount) {
        const JsonValue* valve = positions->items[3].get("valve");
        const JsonValue* pupil = positions->items[3].get("pupil");
        CHECK(valve != nullptr && valve->get("dx_deg") != nullptr && near(valve->get("dx_deg")->number, -15.0, 1e-3));
        CHECK(pupil != nullptr && pupil->get("dx_deg") != nullptr && pupil->get("dx_deg")->isNull());
        const JsonValue* same = positions->items[3].get("valve_same");
        CHECK(same != nullptr && same->get("n") != nullptr && same->get("n")->number == 0);
        const JsonValue* sameAll = root.get("valve_same");
        CHECK(sameAll != nullptr && sameAll->get("positions") != nullptr && sameAll->get("positions")->number == 14);
        CHECK(root.get("valve") != nullptr && root.get("valve")->get("positions")->number == 15);
        const JsonValue* shown = positions->items[1].get("shown_s");
        CHECK(shown != nullptr && near(shown->number, kCountdownSec + kHoldSec, 0.02));
    }
    const std::string csv = samplesCsv(sim.session.rows());
    size_t lines = 0;
    for (char ch : csv) lines += ch == '\n' ? 1 : 0;
    CHECK(lines == sim.session.rows().size() + 1 && csv.rfind("panel_s,unix,order,run_index,position,scored,sample_time", 0) == 0);
    CHECK(csv.find(",forward,") != std::string::npos && csv.find(",reverse,") == std::string::npos);
    CHECK(root.get("order") != nullptr && root.get("order")->text == "forward");
    CHECK(csv.find(",pupil_state,pupil_blending,before_smoothing,valve_x,") != std::string::npos);
    // The simulation's samples came without bit 4 (as from an older frameeyeosc): said so
    CHECK(root.get("values") != nullptr && root.get("values")->text == "as sent (smoothed)" && !v.beforeSmoothing);
    CHECK(sim.session.rows().size() >= 4990 && sim.session.rows().size() <= 5010);
    // Closing the result: back to nothing
    sim.session.dismiss();
    CHECK(sim.session.view().phase == Phase::Idle && sim.session.rows().empty());
}

/** The pupils easing everywhere (a change of source every few samples), or at some places: Valve falls back. */
void testValveFallback() {
    for (const int easingPlaces : {kPositionCount, 7}) {
        Simulation sim;
        sim.easingPlaces = easingPlaces;
        sim.session.start(sim.now, 0);
        sim.run(100.0 + kRunSec + 1.0);
        const Result& r = sim.session.view().result;
        CHECK(r.mapped && r.valve.positions == kPositionCount);
        if (easingPlaces == kPositionCount) {
            // No place: Valve's main number on every sample, the pupils too few
            CHECK(r.valveSame.positions == 0 && r.pupil.positions == 0 && !valveSameMain(r));
            CHECK(&valveMain(r) == &r.valve && briefText(uiText(Language::En), r).rfind("Valve ", 0) == 0);
            const std::string json = recordJson(sim.session.view(), sim.session.shownAt(), sim.session.startedAt(), r);
            JsonValue root;
            std::string error;
            CHECK(parseJson(json, root, error) && root.get("valve_main") != nullptr && root.get("valve_main")->text == "valve");
        } else {
            // Some places: on those (place 3 has one pupil only), and still the main number
            CHECK(r.valveSame.positions == kPositionCount - easingPlaces && valveSameMain(r));
            for (int i = 0; i < kPositionCount; ++i) {
                CHECK(r.positions[i].valveSame.counts() == (i >= easingPlaces && i != 3));
                CHECK(r.positions[i].valve.counts());
            }
        }
    }
}

/** Runs alternate: forward, then the places the other way round (the countdown at the centre first either way). */
void testOrder() {
    CHECK(std::string(orderName(Order::Forward)) == "forward" && std::string(orderName(Order::Reverse)) == "reverse");
    CHECK(nextOrder(Order::Forward) == Order::Reverse && nextOrder(Order::Reverse) == Order::Forward);
    CHECK(placeAt(Order::Forward, 0) == 0 && placeAt(Order::Reverse, 0) == kPositionCount - 1);
    CHECK(placeAt(Order::Reverse, kPositionCount - 1) == 0);
    // The last run's order from its record: none, one from before the alternation (forward), forward, reverse
    bool found = true;
    CHECK(orderInRecord("", found) == Order::Forward && !found);
    CHECK(orderInRecord("not json", found) == Order::Forward && !found);
    CHECK(orderInRecord("{\"result\": \"done\"}", found) == Order::Forward && found);
    CHECK(orderInRecord("{\"order\": \"forward\"}", found) == Order::Forward && found);
    CHECK(orderInRecord("{\"order\": \"reverse\"}", found) == Order::Reverse && found);
    // A reversed run: the countdown at the centre, then the last place first and the centre last
    Simulation sim;
    sim.session.start(sim.now, 0, Order::Reverse);
    CHECK(sim.session.view().order == Order::Reverse);
    std::vector<int> shown;
    const double dt = 1.0 / 90;
    bool centreFirst = false;
    while (sim.now < 100.0 + kRunSec + 1.0) {
        const Tick tick = sim.session.tick(sim.now, sim.now);
        if (sim.session.view().phase == Phase::Countdown && tick.showTarget) {
            centreFirst = tick.yawDeg == kPositions[0].yawDeg && tick.pitchDeg == kPositions[0].pitchDeg;
        }
        if (tick.hopped && sim.session.view().index >= 0) shown.push_back(sim.session.view().index);
        if (sim.session.active()) sim.session.add(sim.now + 0.002, sim.now, sim.sampleAt(sim.now));
        sim.now += dt;
    }
    CHECK(centreFirst && shown.size() == static_cast<size_t>(kPositionCount));
    for (size_t i = 0; i < shown.size(); ++i) CHECK(shown[i] == kPositionCount - 1 - static_cast<int>(i));
    const View& v = sim.session.view();
    CHECK(v.phase == Phase::Done && v.positionsDone == kPositionCount);
    // Each place shown in its turn, and scored as itself (the same offsets as forward)
    const double* at = sim.session.shownAt();
    for (int i = 1; i < kPositionCount; ++i) CHECK(at[i] < at[i - 1]);
    for (int i = 0; i < kPositionCount; ++i) {
        CHECK(near(v.result.positions[i].valve.dx, i == 3 ? -15.0 : -7.6, 1e-4));
        CHECK(v.result.positions[i].window >= 223 && v.result.positions[i].window <= 227);
    }
    // The record: its order, each place's step in the run, and the order on every sample's row
    JsonValue root;
    std::string error;
    CHECK(parseJson(recordJson(v, at, sim.session.startedAt(), v.result), root, error));
    CHECK(root.get("order") != nullptr && root.get("order")->text == "reverse");
    const JsonValue* positions = root.get("positions");
    CHECK(positions != nullptr && positions->items.size() == static_cast<size_t>(kPositionCount));
    if (positions != nullptr && positions->items.size() == static_cast<size_t>(kPositionCount)) {
        CHECK(positions->items[0].get("run_index")->number == kPositionCount - 1);
        CHECK(positions->items[kPositionCount - 1].get("run_index")->number == 0);
        CHECK(positions->items[0].get("shown_s")->number > positions->items[1].get("shown_s")->number);
    }
    const std::string csv = samplesCsv(sim.session.rows(), v.order);
    CHECK(csv.find(",reverse,") != std::string::npos && csv.find(",forward,") == std::string::npos);
    // (a row of the first place shown: place 14, its step 0)
    CHECK(csv.find(",reverse,0,14,") != std::string::npos);
}

void testWithoutMap() {
    Simulation sim;
    sim.mapped = false;
    sim.session.start(sim.now, 0);
    sim.run(100.0 + kRunSec + 1.0);
    const Result& r = sim.session.view().result;
    CHECK(sim.session.view().phase == Phase::Done && !r.mapped && !sim.session.view().mapped);
    CHECK(r.valve.positions == kPositionCount && r.pupil.positions == 0 && std::isnan(r.pupil.error));
    CHECK(r.valveSame.positions == 0 && briefText(uiText(Language::En), r).rfind("Valve ", 0) == 0);
    CHECK(r.states[0] == r.window);
}

void testStopAndNoData() {
    // Stopped after the fourth place has shown: stopped, the places done, one end
    Simulation sim;
    sim.session.start(sim.now, 0);
    sim.run(100.0 + kCountdownSec + 3 * kHoldSec + 1.0);
    sim.session.stop(sim.now, 5);
    CHECK(sim.session.view().phase == Phase::Stopped && sim.session.view().positionsDone == 3);
    CHECK(sim.session.view().endUnix == 5 && !sim.session.active());
    const Tick ended = sim.session.tick(sim.now, 6);
    CHECK(ended.ended && !ended.showTarget);
    CHECK(!sim.session.tick(sim.now + 0.1, 6).ended);
    // ...its scores of what was measured, for the record
    CHECK(sim.session.view().result.positions[1].valve.counts() && !sim.session.view().result.positions[4].valve.counts());
    // No eye data: failed after kNoDataSec
    Simulation quiet;
    quiet.session.start(quiet.now, 0);
    quiet.run(100.0 + kNoDataSec - 0.1, false);
    CHECK(quiet.session.active() && quiet.ends == 0);
    quiet.run(100.0 + kNoDataSec + 0.2, false);
    CHECK(quiet.session.view().phase == Phase::Failed && quiet.ends == 1);
    // A late panel frame: the window is timed from when the dot really moved, not from the plan
    Session late;
    late.start(0, 0);
    Sample s;
    s.usable = true;
    for (double at = 0; at < kCountdownSec; at += 0.01) {
        late.tick(at, 0);
        late.add(at, 0, s);
    }
    late.tick(kCountdownSec + 0.2, 0);  // (0.2 s late)
    CHECK(near(late.shownAt()[0], kCountdownSec + 0.2));
    late.add(kCountdownSec + 1.1, 0, s);
    late.add(kCountdownSec + 1.25, 0, s);
    CHECK(!late.rows()[late.rows().size() - 2].scored && late.rows().back().scored && late.rows().back().position == 0);
}

void testTexts() {
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    CHECK(shiftText(ja, -7.6, 2.4) == "左に 7.6°、上に 2.4°");
    CHECK(shiftText(en, -7.6, 2.4) == "7.6° left, 2.4° up");
    CHECK(shiftText(ja, 0.01, -1.26) == "下に 1.3°" && shiftText(en, 0.0, 0.0) == "about 0°");
    CHECK(positionText(ja, {20, 15}) == "右 20°・上 15°" && positionText(en, {20, 15}) == "20° right, 15° up");
    CHECK(positionText(ja, {-10, 7.5}) == "左 10°・上 7.5°" && positionText(en, {0, -15}) == "15° down");
    CHECK(positionText(ja, {0, 0}) == "真ん中" && positionText(en, {0, 0}) == "center");
    Result r;
    CHECK(briefText(ja, r).empty());
    r.valve.positions = 15;
    r.valve.error = 7.94;
    CHECK(briefText(ja, r) == "Valve 7.9°");
    // With a map, Valve's number on the pupils' samples; where those count nowhere, on every sample again
    r.mapped = true;
    CHECK(!valveSameMain(r) && &valveMain(r) == &r.valve && briefText(ja, r) == "Valve 7.9°");
    r.valveSame.positions = 14;
    r.valveSame.error = 7.46;
    CHECK(valveSameMain(r) && &valveMain(r) == &r.valveSame);
    CHECK(briefText(ja, r) == "Valve 7.5°");
    r.pupil.positions = 14;
    r.pupil.error = 1.25;
    CHECK(briefText(ja, r) == "Valve 7.5°・瞳 1.2°" || briefText(ja, r) == "Valve 7.5°・瞳 1.3°");
    CHECK(briefText(en, r).rfind("Valve 7.5°, pupil 1.", 0) == 0);
}

void testSocket() {
    const std::string path = "/tmp/frameeyeosc-measure-test-" + std::to_string(::getpid()) + ".sock";
    Receiver receiver;
    std::string error;
    CHECK(receiver.open(path, error));
    std::vector<Sample> got;
    CHECK(receiver.poll(got) == 0);
    const int sender = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const auto send = [&](const std::vector<uint8_t>& data) {
        ::sendto(sender, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    };
    for (int i = 1; i <= 3; ++i) {
        Sample s;
        s.time = i;
        send(encode(s));
    }
    send(std::vector<uint8_t> {1, 2, 3});
    // Every sample waiting, in order, the junk left out
    CHECK(receiver.poll(got) == 3 && got.size() == 3 && got[0].time == 1 && got[2].time == 3);
    CHECK(!got[0].guarded && !got[2].guarded);
    CHECK(!got[0].raw.has && !got[2].raw.has);
    // The gaze guard's values come just before their sample: put on it; on another one's (another time), not; and
    // they don't count as a sample themselves
    Guard guard;
    guard.time = 4;
    guard.trusted = true;
    guard.gaze[0] = 0.25f;
    guard.offset[2] = -0.125f;
    send(encodeGuard(guard));
    Sample s;
    s.time = 4;
    send(encode(s));
    guard.time = 9;
    send(encodeGuard(guard));
    s.time = 5;
    send(encode(s));
    send(encode(s));
    got.clear();
    CHECK(receiver.poll(got) == 3 && got.size() == 3);
    CHECK(got[0].guarded && got[0].guardTrusted && got[0].guard[0] == 0.25f && got[0].guardOffset[2] == -0.125f);
    CHECK(!got[1].guarded && !got[2].guarded);
    // Raw values just before a sample of the same time go with it; ones for another time, or with nothing after them
    // in this poll, go with none (a later sample doesn't take them)
    got.clear();
    Raw raw;
    raw.pair[0] = 0.25f;
    s.time = 7;
    send(encodeRaw(7, raw));
    send(encode(s));
    s.time = 8;
    send(encodeRaw(99, raw));
    send(encode(s));
    send(encodeRaw(9, raw));
    CHECK(receiver.poll(got) == 2 && got[0].raw.has && got[0].raw.pair[0] == 0.25f && !got[1].raw.has);
    s.time = 10;
    send(encode(s));
    got.clear();
    CHECK(receiver.poll(got) == 1 && !got[0].raw.has);
    // Both before one sample, in either order (frameeyeosc sends FEOR, FEOG, FEOM): both put on it, neither on the
    // sample after it; each only as the sample's own (a guard of another time doesn't stop the raw values, nor they it)
    guard.time = 11;
    raw.pair[1] = -0.5f;
    s.time = 11;
    send(encodeRaw(11, raw));
    send(encodeGuard(guard));
    send(encode(s));
    s.time = 12;
    guard.time = 12;
    send(encodeGuard(guard));
    send(encodeRaw(12, raw));
    send(encode(s));
    s.time = 13;
    send(encode(s));
    guard.time = 99;
    s.time = 14;
    send(encodeGuard(guard));
    send(encodeRaw(14, raw));
    send(encode(s));
    got.clear();
    CHECK(receiver.poll(got) == 4 && got.size() == 4);
    CHECK(got[0].guarded && got[0].raw.has && got[0].raw.pair[1] == -0.5f && got[0].guard[0] == 0.25f);
    CHECK(got[1].guarded && got[1].raw.has && got[1].raw.pair[0] == 0.25f);
    CHECK(!got[2].guarded && !got[2].raw.has);
    CHECK(!got[3].guarded && got[3].raw.has);
    // A datagram bigger than any known (a later frameeyeosc's) is read cut to the buffer's size: ignored, and it
    // doesn't take the values waiting for the next sample
    send(encodeRaw(15, raw));
    send(std::vector<uint8_t>(4096, 0x46));
    s.time = 15;
    send(encode(s));
    got.clear();
    CHECK(receiver.poll(got) == 1 && got[0].raw.has);
    ::close(sender);
    receiver.close();
    CHECK(::access(path.c_str(), F_OK) != 0);
}

void testGuard() {
    // The datagram: the same bytes frameeyeosc writes (src/dots.rs, the_guard_datagram_reads_back_and_is_no_other)
    Guard guard;
    guard.time = 19892.729813125;
    guard.trusted = true;
    guard.gaze[0] = 0.125f;
    guard.gaze[1] = -0.25f;
    guard.offset[0] = -0.375f;
    guard.offset[1] = 0.0625f;
    guard.offset[2] = -0.3125f;
    guard.hasWitness = true;
    guard.witness[0] = 0.1f;
    guard.witness[1] = -0.2f;
    const std::vector<uint8_t> bytes = encodeGuard(guard);
    CHECK(bytes.size() == kGuardSize && kGuardSize == 48);
    CHECK(std::memcmp(bytes.data(), "FEOG\x01\x03\x00\x00", 8) == 0);
    Guard read;
    CHECK(decodeGuard(bytes.data(), bytes.size(), read));
    CHECK(read.time == guard.time && read.trusted && read.hasWitness && read.gaze[1] == -0.25f);
    CHECK(read.offset[2] == -0.3125f && read.witness[1] == -0.2f);
    // Not a sample, nor a sample this; another version or size, or the guarded gaze not a number: ignored
    Sample sample;
    CHECK(!decode(bytes.data(), bytes.size(), sample) && !decode(bytes.data(), kSampleSize, sample));
    CHECK(!decodeGuard(encode(sample).data(), kSampleSize, read));
    std::vector<uint8_t> wrong = bytes;
    wrong[4] = 2;
    CHECK(!decodeGuard(wrong.data(), wrong.size(), read) && !decodeGuard(bytes.data(), bytes.size() - 1, read));
    const uint8_t nan[4] = {0, 0, 0xc0, 0x7f};
    wrong = bytes;
    std::memcpy(wrong.data() + 16, nan, 4);
    CHECK(!decodeGuard(wrong.data(), wrong.size(), read));
    // Scored as a third source on Valve's samples: here the guard has taken Valve's 7.6° sideways out
    Simulation sim;
    sim.session.start(sim.now, 0);
    const double dt = 1.0 / 90;
    while (sim.now < 100.0 + kRunSec + 1.0) {
        sim.session.tick(sim.now, sim.now);
        if (sim.session.active()) {
            Sample s = sim.sampleAt(sim.now);
            Guard g;
            g.time = s.time;
            g.gaze[0] = static_cast<float>(s.valve[0] + 7.6 / 45);
            g.gaze[1] = s.valve[1];
            g.offset[0] = g.offset[2] = static_cast<float>(7.6 / 45);
            attachGuard(g, s);
            sim.session.add(sim.now + 0.002, sim.now, s);
        }
        sim.now += dt;
    }
    const Result& r = sim.session.view().result;
    CHECK(r.guardSeen && r.guarded.positions == kPositionCount);
    // (place 3, where Valve is 15° off, stays 7.4° off)
    CHECK(near(r.positions[0].guarded.dx, 0.0, 0.05) && near(r.positions[0].valve.dx, -7.6, 0.05));
    CHECK(r.positions[0].guarded.n == r.positions[0].valve.n);
    CHECK(near(r.positions[3].guarded.dx, -7.4, 0.05));
    JsonValue root;
    std::string error;
    CHECK(parseJson(recordJson(sim.session.view(), sim.session.shownAt(), sim.session.startedAt(), r), root, error));
    CHECK(root.get("guarded") != nullptr && root.get("plan")->get("guarded") != nullptr);
    CHECK(root.get("positions")->items[0].get("guarded")->get("n")->number == r.positions[0].guarded.n);
    const std::string csv = samplesCsv(sim.session.rows());
    CHECK(csv.find(",openness_right,guarded,guard_trusted,guarded_x,guarded_y,guard_offset_left_x,") != std::string::npos);
    CHECK(csv.find(",1,0,") != std::string::npos);
    // The guard's columns, then the raw values' last; every row as many cells as the header
    CHECK(csv.find(",guard_offset_right_y,raw,pair_lx,") != std::string::npos);
    {
        size_t header = std::string::npos;
        bool same = true;
        for (size_t start = 0; start < csv.size();) {
            const size_t end = csv.find('\n', start);
            size_t commas = 0;
            for (size_t i = start; i < end; ++i) commas += csv[i] == ',' ? 1 : 0;
            if (header == std::string::npos) header = commas;
            same = same && commas == header;
            start = end + 1;
        }
        CHECK(same && header == 18 + 8 + 29);
    }
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    CHECK(briefText(ja, r).find("・見張り付き ") != std::string::npos && briefText(en, r).find(", guarded ") != std::string::npos);
    // Without the guard: none of it in the record, and empty cells
    Simulation plain;
    plain.session.start(plain.now, 0);
    plain.run(100.0 + kRunSec + 1.0);
    const Result& p = plain.session.view().result;
    CHECK(!p.guardSeen && p.guarded.positions == 0);
    CHECK(parseJson(recordJson(plain.session.view(), plain.session.shownAt(), plain.session.startedAt(), p), root, error));
    CHECK(root.get("guarded") == nullptr && root.get("plan")->get("guarded") == nullptr);
    // (the guard's 0 and 7 empty cells, then the raw values' columns: none came here either)
    CHECK(samplesCsv(plain.session.rows()).find(",0,,,,,,,,0,") != std::string::npos);
    CHECK(briefText(en, p).find("guarded") == std::string::npos);
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testDatagram();
    testRawDatagram();
    testGeometry();
    testRun();
    testWithoutMap();
    testOrder();
    testValveFallback();
    testStopAndNoData();
    testTexts();
    testSocket();
    testGuard();
    if (gFailures == 0) std::printf("measure-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
