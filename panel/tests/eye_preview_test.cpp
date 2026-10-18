// Tests for the eye preview (eye_preview.cpp): which values drive the drawn eyes, the lids' and pupil's geometry,
// eyecam-rec's preview file (made-up grey pictures written to a folder under /tmp, never an eye), when the pictures can
// show and when "preview on" / "off" go out. Built with the panel as eye-preview-test; exits non-zero on failure.
#include "eye_preview.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

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
bool near(double a, double b, double tolerance = 1e-6) {
    return std::fabs(a - b) < tolerance;
}

using namespace eye_preview;

/** A sample with every value set: the left eye from the camera's widening, the right one Valve's and squinting. */
gaze_dots::Packet packet() {
    gaze_dots::Packet p;
    const float sent[6] = {0.1f, -0.1f, 0.2f, -0.1f, 0.15f, -0.1f};
    const float raw[6] = {0.3f, 0.2f, 0.4f, 0.2f, 0.35f, 0.2f};
    std::memcpy(p.gaze, sent, sizeof(sent));
    std::memcpy(p.rawGaze, raw, sizeof(raw));
    p.eyes[0].lid = 0.9f;
    p.eyes[0].openness = 1.0f;
    p.eyes[0].opennessScaled = 1.2f;
    p.eyes[0].cameraLid = 0.95f;
    p.eyes[0].squint = 0.0f;
    p.eyes[0].pupilMm = 6.1f;
    p.eyes[0].cameraSquint = 0.05f;
    p.eyes[0].cameraPupilMm = 6.3f;
    p.eyes[0].lidFrom = 4;
    p.eyes[1].lid = 0.5f;
    p.eyes[1].openness = 0.66f;
    p.eyes[1].opennessScaled = 0.6f;
    p.eyes[1].squint = 0.4f;
    p.eyes[1].lidFrom = 0;
    p.eyes[1].cameraClosed = true;
    return p;
}

void testShown() {
    const gaze_dots::Packet p = packet();
    // As sent: the eyelid, squint, pupil and gaze as they went out; the badge from LidFrom
    Shown s = shown(p, Source::Sent);
    CHECK(s.valid && near(s.gazeX, 0.15) && near(s.gazeY, -0.1));
    CHECK(s.eyes[0].known && near(s.eyes[0].lid, 0.9) && near(s.eyes[0].lidValue, 0.9) && s.eyes[0].camera);
    CHECK(near(s.eyes[0].pupilMm, 6.1) && near(s.eyes[0].gazeX, 0.1) && near(s.eyes[1].gazeX, 0.2));
    CHECK(near(s.eyes[1].lid, 0.5) && near(s.eyes[1].squint, 0.4) && !s.eyes[1].camera && s.eyes[1].closedByCamera);
    CHECK(std::isnan(s.eyes[1].pupilMm));
    // As read: Valve's openness after the scale, 1.0 drawn as relaxed open (1.2 a little widened); the raw gaze; no
    // squint or pupil, and Valve's
    s = shown(p, Source::Raw);
    CHECK(near(s.gazeX, 0.35) && near(s.eyes[0].gazeY, 0.2));
    CHECK(near(s.eyes[0].lid, 0.9) && near(s.eyes[0].lidValue, 1.2) && !s.eyes[0].camera);
    CHECK(near(s.eyes[1].lid, 0.45) && near(s.eyes[1].squint, 0.0) && std::isnan(s.eyes[0].pupilMm));
    // The camera's own: an eye without fresh camera values isn't known, and drawn relaxed open
    s = shown(p, Source::Camera);
    CHECK(near(s.eyes[0].lid, 0.95) && near(s.eyes[0].squint, 0.05) && near(s.eyes[0].pupilMm, 6.3));
    CHECK(s.eyes[0].camera && near(s.gazeX, 0.35));
    CHECK(!s.eyes[1].known && near(s.eyes[1].lid, 0.75) && std::isnan(s.eyes[1].lidValue));
    // A value past the scale is kept within it
    gaze_dots::Packet wide = p;
    wide.eyes[0].lid = 1.4f;
    wide.eyes[0].squint = -0.2f;
    s = shown(wide, Source::Sent);
    CHECK(near(s.eyes[0].lid, 1.0) && near(s.eyes[0].squint, 0.0) && near(s.eyes[0].lidValue, 1.4, 1e-5));
    // No brows sent: no room for them, none drawn
    for (const Source source : {Source::Sent, Source::Raw, Source::Camera}) {
        s = shown(p, source);
        CHECK(!s.brows && std::isnan(s.eyes[0].brow) && std::isnan(s.eyes[1].brow));
    }
}

void testBrows() {
    gaze_dots::Packet p = packet();
    p.brows = true;
    p.eyes[0].brow = 0.6f;
    p.eyes[1].brow = 0.0f;
    // Sent: each eye's brow as sent (0 at rest is drawn, at rest)
    Shown s = shown(p, Source::Sent);
    CHECK(s.brows && near(s.eyes[0].brow, 0.6) && near(s.eyes[1].brow, 0.0));
    // Raw and Camera: nothing measures brows, so none are drawn, but the room above the eyes stays (no jump)
    for (const Source source : {Source::Raw, Source::Camera}) {
        s = shown(p, source);
        CHECK(s.brows && std::isnan(s.eyes[0].brow) && std::isnan(s.eyes[1].brow));
    }
    // Kept within 0..1, NaN at rest
    p.eyes[0].brow = 1.5f;
    p.eyes[1].brow = NAN;
    s = shown(p, Source::Sent);
    CHECK(near(s.eyes[0].brow, 1.0) && near(s.eyes[1].brow, 0.0));
    // The lift: none at rest, kBrowRise all the way, smoothly between; the room above the eye covers it
    CHECK(near(browLift(0.0), 0.0) && near(browLift(1.0), kBrowRise) && near(browLift(0.5), kBrowRise / 2));
    CHECK(near(browLift(NAN), 0.0) && near(browLift(-1.0), 0.0) && near(browLift(3.0), kBrowRise));
    CHECK(kBrowRise >= 0.25 && kBrowRise <= 0.35);
    CHECK(near(kBrowHeadroom, kBrowRest + kBrowRise + kBrowThickness / 2));
    // The made-up brows: sent, both at the level
    View view;
    fake(view, 1);
    CHECK(!view.packet.brows);
    fakeBrows(view, 0.5);
    CHECK(view.packet.brows && near(view.packet.eyes[0].brow, 0.5) && near(view.packet.eyes[1].brow, 0.5));
}

void testGeometry() {
    // The upper lid: closed at 0, at kRelaxedOpen relaxed open, all the way up widened, and always moving with the lid
    CHECK(near(upperLidOpen(0.0), 0.0) && near(upperLidOpen(0.75), kRelaxedOpen) && near(upperLidOpen(1.0), 1.0));
    CHECK(near(upperLidOpen(-1.0), 0.0) && near(upperLidOpen(2.0), 1.0) && near(upperLidOpen(NAN), kRelaxedOpen));
    double before = -1.0;
    bool rising = true;
    for (int i = 0; i <= 100; ++i) {
        const double at = upperLidOpen(i / 100.0);
        rising &= at > before;
        before = at;
    }
    CHECK(rising);
    // Widening shows: from relaxed open to 1.0 the lid rises by the rest of the way
    CHECK(upperLidOpen(0.9) > kRelaxedOpen + 0.1);
    CHECK(near(lowerLidRise(0.0), 0.0) && near(lowerLidRise(1.0), kMaxSquintRise) && near(lowerLidRise(NAN), 0.0));
    // The pupil against the iris (11.8 mm across): 5.9 mm is half its radius; none drawn at 4 mm; kept within 1.5..9
    CHECK(near(pupilRadius(50, 5.9), 25) && near(pupilRadius(50, NAN), 50 * 4.0 / 11.8));
    CHECK(near(pupilRadius(50, 0.5), 50 * 1.5 / 11.8) && near(pupilRadius(50, 20), 50 * 9.0 / 11.8));
    // The iris: 35° sideways and 30° up or down all the way
    CHECK(near(gazeShare(35.0 / 45.0, false), 1.0) && near(gazeShare(-15.0 / 45.0, true), -0.5));
    CHECK(near(gazeShare(1.0, true), 1.0) && near(gazeShare(NAN, false), 0.0));
}

void testViews() {
    // In a mirror: the left eye on the left, the right on the right, the irises moving the way the user looks
    CHECK(drawnColumn(0, true) == 0 && drawnColumn(1, true) == 1 && !gazeTurned(true));
    // Seen from the front: the right eye on the left, sideways turned round
    CHECK(drawnColumn(0, false) == 1 && drawnColumn(1, false) == 0 && gazeTurned(false));
    // The pictures' ear side (at the left as eyecam-rec gives them) toward the outside of the sheet in both views: the
    // picture in the right column flipped. So each eye's picture flips between the views (mirror images)
    for (const bool mirror : {true, false}) {
        for (int eye = 0; eye < 2; ++eye) {
            const bool earLeft = !pictureFlipped(eye, mirror);
            CHECK(earLeft == (drawnColumn(eye, mirror) == 0));
            CHECK(pictureFlipped(eye, mirror) != pictureFlipped(eye, !mirror));
        }
    }
    CHECK(!pictureFlipped(0, true) && pictureFlipped(1, true) && pictureFlipped(0, false) && !pictureFlipped(1, false));
}

/**
 * Made-up pictures of a side: a gradient (not an eye), the left eye's pupil found, the right one's not and closed.
 * @param side their side
 * @return them
 */
Pictures madeUp(int side) {
    Pictures p;
    p.on = true;
    p.writerPid = 1234;
    p.writerStartNs = 99;
    for (int eye = 0; eye < 2; ++eye) {
        Picture& pic = p.eyes[eye];
        pic.present = true;
        pic.timeNs = 1000 + eye;
        pic.count = 7 + eye;
        pic.side = side;
        pic.pixels.resize(static_cast<size_t>(side) * side);
        for (size_t i = 0; i < pic.pixels.size(); ++i) pic.pixels[i] = static_cast<uint8_t>((i * 7 + eye) % 251);
        const double window[4] = {93, 35, 173, 160};
        std::copy(window, window + 4, pic.window);
    }
    p.eyes[0].pupil = true;
    p.eyes[0].pupilX = 120;
    p.eyes[0].pupilY = 95;
    p.eyes[0].pupilR = 10.5;
    p.eyes[1].closed = true;
    return p;
}

void testFile() {
    const Pictures pictures = madeUp(200);
    const std::vector<uint8_t> bytes = encodePictures(pictures, 200);
    // As eyecam-rec lays it out (tools/eyecam/src/preview.rs): 64 + 2 * (64 + 200 * 200) in whole pages
    CHECK(bytes.size() == 81920 && std::memcmp(bytes.data(), "EYPV", 4) == 0 && bytes[4] == 1);
    Pictures read;
    std::string why;
    CHECK(parsePictures(bytes.data(), bytes.size(), read, why));
    CHECK(read.on && read.writerPid == 1234 && read.writerStartNs == 99);
    CHECK(read.eyes[0].present && read.eyes[0].timeNs == 1000 && read.eyes[1].count == 8 && read.eyes[0].side == 200);
    CHECK(read.eyes[0].pixels == pictures.eyes[0].pixels && read.eyes[1].pixels == pictures.eyes[1].pixels);
    CHECK(read.eyes[0].pupil && near(read.eyes[0].pupilX, 120) && near(read.eyes[0].pupilR, 10.5) && !read.eyes[0].closed);
    CHECK(!read.eyes[1].pupil && read.eyes[1].closed && near(read.eyes[1].window[2], 173));
    // eyecam-rec's own bytes for one eye's header (written by hand here): time 5, 2 pictures, the pupil found at
    // (120, 95) r 10.5 and the window 93, 35, 173, 160
    std::vector<uint8_t> fromRust = encodePictures(Pictures {}, 200);
    const uint8_t eye[52] = {5,    0, 0, 0,    0, 0, 0, 0,    2,    0, 0,    0,    0,    0,    0,    0,    1, 0,
                             0,    0, 0, 0,    0xf0, 0x42,    0, 0, 0xbe, 0x42, 0,    0,    0x28, 0x41, 0, 0,
                             0xba, 0x42,  0, 0, 0x0c, 0x42,    0, 0, 0x2d, 0x43,    0, 0, 0x20, 0x43};
    std::memcpy(fromRust.data() + 64, eye, sizeof(eye));
    CHECK(parsePictures(fromRust.data(), fromRust.size(), read, why));
    CHECK(read.eyes[0].present && read.eyes[0].timeNs == 5 && read.eyes[0].count == 2 && read.eyes[0].pupil);
    CHECK(near(read.eyes[0].pupilX, 120) && near(read.eyes[0].pupilY, 95) && near(read.eyes[0].pupilR, 10.5));
    CHECK(near(read.eyes[0].window[0], 93) && near(read.eyes[0].window[3], 160) && !read.eyes[1].present);
    // Refused: another file, another version, sizes that don't add up, a picture too big, too short
    const auto refused = [&](std::vector<uint8_t> b, size_t at, uint8_t value) {
        b[at] = value;
        Pictures out;
        std::string reason;
        return !parsePictures(b.data(), b.size(), out, reason) && !reason.empty();
    };
    CHECK(refused(bytes, 0, 'X'));
    CHECK(refused(bytes, 4, 2));
    CHECK(refused(bytes, 8, 32));
    CHECK(refused(bytes, 12, 0));
    CHECK(refused(bytes, 16, 199));
    const std::vector<uint8_t> huge = encodePictures(madeUp(401), 401);
    CHECK(!parsePictures(huge.data(), huge.size(), read, why));
    CHECK(!parsePictures(bytes.data(), 64 + 2 * (64 + 200 * 200) - 1, read, why));
    CHECK(!parsePictures(bytes.data(), 10, read, why));
}

/**
 * Write a file by putting a new one in its place (as eyecam-rec does).
 * @param path where
 * @param bytes what
 */
void replaceFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    const std::string temp = path + ".new";
    std::ofstream(temp, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::rename(temp.c_str(), path.c_str());
}

/**
 * Change bytes of a file in place (as eyecam-rec writes into its mapping).
 * @param path where
 * @param at the offset
 * @param bytes what
 */
void writeAt(const std::string& path, size_t at, const std::vector<uint8_t>& bytes) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(at));
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void testReader() {
    const std::string dir = "/tmp/frameeyeosc-eye-preview-test-" + std::to_string(::getpid());
    ::mkdir(dir.c_str(), 0700);
    const std::string path = dir + "/" + kFileName;
    PictureReader reader;
    Pictures out;
    std::string why;
    // None yet (the preview is off)
    CHECK(reader.read(path, out, why) == ReadResult::Missing);
    Pictures pictures = madeUp(200);
    std::vector<uint8_t> bytes = encodePictures(pictures, 200);
    replaceFile(path, bytes);
    CHECK(reader.read(path, out, why) == ReadResult::Read && out.eyes[0].pixels == pictures.eyes[0].pixels);
    // The same (its seqlock's counter unchanged): nothing new
    CHECK(reader.read(path, out, why) == ReadResult::Unchanged);
    // Being written (an odd counter) through every try: read again later
    writeAt(path, 24, {1, 0, 0, 0});
    CHECK(reader.read(path, out, why) == ReadResult::Busy);
    // Written (the counter even again, past the last): the new picture, through the same mapping
    std::vector<uint8_t> bright(200 * 200, 200);
    writeAt(path, 64 + 64, bright);
    writeAt(path, 24, {2, 0, 0, 0});
    CHECK(reader.read(path, out, why) == ReadResult::Read && out.eyes[0].pixels == bright);
    // Replaced by a new file (a new eyecam-rec, or the preview on again): opened again
    pictures.writerPid = 5678;
    replaceFile(path, encodePictures(pictures, 200));
    CHECK(reader.read(path, out, why) == ReadResult::Read && out.writerPid == 5678);
    // Not eyecam-rec's preview: refused, with why
    std::vector<uint8_t> other = encodePictures(pictures, 200);
    other[0] = 'X';
    replaceFile(path, other);
    CHECK(reader.read(path, out, why) == ReadResult::Invalid && !why.empty());
    // ...and let go of: not mapped and copied again while it is the same file (even put right in place), with the
    // same why
    const std::string first = why;
    writeAt(path, 0, {'E', 'Y', 'P', 'V'});
    why.clear();
    CHECK(reader.read(path, out, why) == ReadResult::Invalid && why == first);
    // Replaced by a good one: read
    replaceFile(path, encodePictures(pictures, 200));
    CHECK(reader.read(path, out, why) == ReadResult::Read);
    // Gone (the preview turned off): let go of
    ::unlink(path.c_str());
    CHECK(reader.read(path, out, why) == ReadResult::Missing);
    // A link in its place is not followed
    replaceFile(dir + "/elsewhere", encodePictures(pictures, 200));
    CHECK(::symlink((dir + "/elsewhere").c_str(), path.c_str()) == 0);
    CHECK(reader.read(path, out, why) == ReadResult::Invalid);
    ::unlink(path.c_str());
    ::unlink((dir + "/elsewhere").c_str());
    reader.close();
    ::rmdir(dir.c_str());
}

void testFresh() {
    Picture p;
    CHECK(!pictureFresh(p, 1000000000ull));
    p.present = true;
    p.side = 200;
    p.timeNs = 10000000000ull;
    CHECK(pictureFresh(p, 10000000000ull + 400000000ull));
    CHECK(!pictureFresh(p, 10000000000ull + 600000000ull));
    // (a little ahead: the clocks read a moment apart)
    CHECK(pictureFresh(p, 10000000000ull - 10000000ull) && !pictureFresh(p, 10000000000ull - 200000000ull));
}

void testCameraState() {
    eyecam::View view;
    CHECK(cameraState(view, true, false) == CameraState::NotRunning);
    view.visible = true;
    view.status.present = true;
    view.status.hasSetupDone = true;
    view.status.setupDone = false;
    CHECK(cameraState(view, true, false) == CameraState::NotSetUp);
    view.status.setupDone = true;
    CHECK(cameraState(view, false, false) == CameraState::NotInUse);
    CHECK(cameraState(view, true, false) == CameraState::Old);
    view.status.hasPreview = true;
    CHECK(cameraState(view, true, false) == CameraState::Available);
    CHECK(cameraState(view, true, true) == CameraState::Refused);
    // The camera's values need the eye cameras in use only
    CHECK(cameraValuesAvailable(CameraState::Available) && cameraValuesAvailable(CameraState::Old));
    CHECK(cameraValuesAvailable(CameraState::Refused) && !cameraValuesAvailable(CameraState::NotInUse));
    CHECK(!cameraValuesAvailable(CameraState::NotRunning) && !cameraValuesAvailable(CameraState::NotSetUp));
}

void testAsker() {
    Asker asker;
    std::string command;
    // Not wanted: nothing (never an "off" that wasn't preceded by an "on")
    CHECK(!asker.next(false, false, 0.0, command));
    // Wanted: at once, then every kRenewSec, never while another command is out
    CHECK(asker.next(true, false, 10.0, command) && command == "preview on");
    CHECK(!asker.next(true, false, 10.5, command));
    CHECK(!asker.next(true, true, 11.0, command));
    CHECK(asker.next(true, false, 11.0, command) && command == "preview on");
    // No longer wanted: "off" once (after a command still out)
    CHECK(!asker.next(false, true, 11.2, command));
    CHECK(asker.next(false, false, 11.3, command) && command == "preview off");
    CHECK(!asker.next(false, false, 12.0, command));
    // Wanted again: "on" at once
    CHECK(asker.next(true, false, 12.1, command) && command == "preview on");
}

void testRefusal() {
    View view;
    // Refused (or no answer in time): shown, and why kept for kRefusedSec from the last refusal
    CHECK(tookReply(view, false, 10.0) && view.refused);
    CHECK(!refusalOver(view, 10.0 + kRefusedSec - 0.1) && view.refused);
    CHECK(!tookReply(view, false, 12.0) && view.refused);
    CHECK(!refusalOver(view, 12.0 + kRefusedSec - 0.1));
    // Then forgotten (the switch can ask again), once
    CHECK(refusalOver(view, 12.0 + kRefusedSec) && !view.refused);
    CHECK(!refusalOver(view, 30.0));
    // "ok": taken back at once
    CHECK(tookReply(view, false, 40.0) && tookReply(view, true, 40.5) && !view.refused);
    CHECK(!tookReply(view, true, 41.0));
}

void testPacer() {
    DrawPacer pacer;
    // Nothing new: nothing to draw
    CHECK(!pacer.due(0.0));
    pacer.changed();
    CHECK(pacer.due(0.0));
    pacer.drawn(0.0);
    CHECK(!pacer.due(0.001));
    // Samples at 90 a second while the loop runs at every display frame: drawn about 30 times a second
    int draws = 0;
    for (int k = 1; k <= 90; ++k) {
        const double now = k / 90.0;
        pacer.changed();
        if (pacer.due(now)) {
            pacer.drawn(now);
            ++draws;
        }
    }
    CHECK(draws >= 28 && draws <= 31);
    // The loop's usual 33 ms passes: each draws (a little under 1/30 apart is enough)
    pacer.drawn(10.0);
    pacer.changed();
    CHECK(pacer.due(10.0 + 0.0305));
    // The last sample is drawn even with none after it
    pacer.drawn(20.0);
    pacer.changed();
    CHECK(!pacer.due(20.01) && pacer.due(20.04));
}

void testFake() {
    View view;
    fake(view, 5000000000ull);
    // Made up: samples and both pictures, fresh, the pupils where the dark discs are; no real eye anywhere
    CHECK(view.hasPacket && view.hasPictures && view.pictures.on);
    for (int eye = 0; eye < 2; ++eye) {
        const Picture& p = view.pictures.eyes[eye];
        CHECK(pictureFresh(p, view.nowNs) && p.side == 200 && p.pixels.size() == 200u * 200u && p.pupil);
        CHECK(p.pixels[static_cast<size_t>(std::lround(p.pupilY)) * 200 + static_cast<size_t>(std::lround(p.pupilX))] < 20);
    }
    const Shown s = shown(view.packet, Source::Sent);
    CHECK(s.eyes[0].camera && !s.eyes[1].camera && s.eyes[0].lid > 0.75 && s.eyes[1].squint > 0.3);
    // It survives the file's round trip
    Pictures back;
    std::string why;
    const std::vector<uint8_t> bytes = encodePictures(view.pictures, 200);
    CHECK(parsePictures(bytes.data(), bytes.size(), back, why) && back.eyes[1].pixels == view.pictures.eyes[1].pixels);
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testShown();
    testBrows();
    testGeometry();
    testViews();
    testFile();
    testReader();
    testFresh();
    testCameraState();
    testAsker();
    testRefusal();
    testPacer();
    testFake();
    if (gFailures == 0) std::printf("eye-preview-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
