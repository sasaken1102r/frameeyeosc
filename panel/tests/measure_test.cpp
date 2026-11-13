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
    CHECK(lines == sim.session.rows().size() + 1 && csv.rfind("panel_s,unix,position,scored,sample_time", 0) == 0);
    CHECK(csv.find(",pupil_state,pupil_blending,valve_x,") != std::string::npos);
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
    ::close(sender);
    receiver.close();
    CHECK(::access(path.c_str(), F_OK) != 0);
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testDatagram();
    testGeometry();
    testRun();
    testWithoutMap();
    testValveFallback();
    testStopAndNoData();
    testTexts();
    testSocket();
    if (gFailures == 0) std::printf("measure-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
