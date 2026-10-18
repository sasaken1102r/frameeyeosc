// Tests for the debug gaze dots (gaze_dots.cpp): the packet, the geometry and the socket, without OpenVR.
// Built with the panel as gaze-dots-test; exits non-zero on failure.
#include "gaze_dots.h"
#include "gaze_fit.h"

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

using namespace gaze_dots;

void testPacket() {
    Packet packet;
    packet.time = 19892.729813125;
    const float gaze[6] = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f};
    std::memcpy(packet.gaze, gaze, sizeof(gaze));
    const float raw[6] = {-0.1f, 0.2f, -0.3f, 0.4f, -0.5f, 0.6f};
    std::memcpy(packet.rawGaze, raw, sizeof(raw));
    const float unscaled[6] = {0.05f, -0.1f, 0.25f, -0.3f, 0.15f, -0.2f};
    std::memcpy(packet.unscaledGaze, unscaled, sizeof(unscaled));
    packet.independent = true;
    packet.scaled = true;
    packet.eyes[0].lid = 0.72f;
    packet.eyes[0].openness = 0.98f;
    packet.eyes[0].opennessScaled = 1.02f;
    packet.eyes[0].squint = 0.25f;
    packet.eyes[0].cameraSquint = 0.3f;
    packet.eyes[0].cameraPupilMm = 6.1f;
    packet.eyes[0].lidFrom = 4;
    packet.eyes[0].cameraClosed = true;
    packet.eyes[1].lid = 0.0f;
    packet.eyes[1].cameraLid = 0.81f;
    packet.eyes[1].pupilMm = 5.5f;
    const std::vector<uint8_t> bytes = encode(packet);
    CHECK(kPacketSize == 160 && bytes.size() == kPacketSize);
    CHECK(std::memcmp(bytes.data(), "FEOD\x03\x03\x00\x00", 8) == 0);
    Packet read;
    CHECK(decode(bytes.data(), bytes.size(), read));
    CHECK(read.time == packet.time && read.independent && std::memcmp(read.gaze, gaze, sizeof(gaze)) == 0);
    CHECK(std::memcmp(read.rawGaze, raw, sizeof(raw)) == 0);
    CHECK(read.scaled && std::memcmp(read.unscaledGaze, unscaled, sizeof(unscaled)) == 0);
    const PacketEye& left = read.eyes[0];
    CHECK(left.lid == 0.72f && left.openness == 0.98f && left.opennessScaled == 1.02f && left.squint == 0.25f);
    CHECK(std::isnan(left.cameraLid) && std::isnan(left.pupilMm) && left.cameraSquint == 0.3f);
    CHECK(left.cameraPupilMm == 6.1f && left.lidFrom == 4 && left.cameraClosed);
    const PacketEye& right = read.eyes[1];
    CHECK(right.lid == 0.0f && right.cameraLid == 0.81f && right.pupilMm == 5.5f && std::isnan(right.openness));
    CHECK(right.lidFrom == 0 && !right.cameraClosed);
    // The same bytes frameeyeosc writes (src/dots.rs): time 1.0, the combined gaze as sent, the left eye's lid as
    // sent 0.75 from the camera (4: its widening), its camera eyelid NaN and the camera reading it closed; the gaze
    // scale in use, the combined x before it 0.25
    std::vector<uint8_t> fromRust(kPacketSize, 0);
    const uint8_t head[16] = {'F', 'E', 'O', 'D', 3, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f};
    std::memcpy(fromRust.data(), head, sizeof(head));
    const uint8_t x[4] = {0, 0, 0, 0x3f};
    const uint8_t y[4] = {0, 0, 0x80, 0xbe};
    const uint8_t lid[4] = {0, 0, 0x40, 0x3f};
    const uint8_t nan[4] = {0, 0, 0xc0, 0x7f};
    std::memcpy(fromRust.data() + 32, x, 4);
    std::memcpy(fromRust.data() + 36, y, 4);
    std::memcpy(fromRust.data() + 64, lid, 4);
    std::memcpy(fromRust.data() + 76, nan, 4);
    fromRust[96] = 4;
    fromRust[97] = 1;
    const uint8_t quarter[4] = {0, 0, 0x80, 0x3e};
    std::memcpy(fromRust.data() + 152, quarter, 4);
    CHECK(decode(fromRust.data(), kPacketSize, read) && read.time == 1.0 && !read.independent);
    CHECK(read.gaze[4] == 0.5f && read.gaze[5] == -0.25f && read.rawGaze[4] == 0.0f);
    CHECK(read.eyes[0].lid == 0.75f && std::isnan(read.eyes[0].cameraLid) && read.eyes[0].lidFrom == 4);
    CHECK(read.eyes[0].cameraClosed && read.eyes[1].lid == 0.0f && !read.eyes[1].cameraClosed);
    CHECK(read.scaled && read.unscaledGaze[4] == 0.25f && read.unscaledGaze[5] == 0.0f);
    // Version 2 (136 bytes, from frameeyeosc 0.7.7 and before) is still read: no rings, the gaze before the scale is
    // the gaze as sent
    std::vector<uint8_t> v2(fromRust.begin(), fromRust.begin() + kPacketSizeV2);
    v2[4] = 2;
    v2[5] = 3;  // (bit 1 meant nothing in version 2)
    CHECK(decode(v2.data(), v2.size(), read) && read.independent && !read.scaled);
    CHECK(read.gaze[4] == 0.5f && read.unscaledGaze[4] == 0.5f && read.unscaledGaze[5] == -0.25f);
    CHECK(read.eyes[0].lid == 0.75f && read.eyes[0].lidFrom == 4);
    // Another version (1 was 40 bytes), a version with the other's size, another size, or a gaze that isn't a
    // number: ignored
    std::vector<uint8_t> wrong = bytes;
    wrong[4] = 1;
    CHECK(!decode(wrong.data(), wrong.size(), read));
    wrong[4] = 2;
    CHECK(!decode(wrong.data(), wrong.size(), read));
    wrong[4] = 4;
    CHECK(!decode(wrong.data(), wrong.size(), read));
    v2[4] = 3;
    CHECK(!decode(v2.data(), v2.size(), read));
    CHECK(!decode(bytes.data(), bytes.size() - 1, read));
    CHECK(!decode(bytes.data(), 40, read));
    wrong = bytes;
    std::memcpy(wrong.data() + 40, nan, 4);
    CHECK(!decode(wrong.data(), wrong.size(), read));
    wrong = bytes;
    std::memcpy(wrong.data() + 156, nan, 4);
    CHECK(!decode(wrong.data(), wrong.size(), read));
}

void testBrows() {
    Packet packet;
    packet.eyes[0].lid = 0.9f;
    packet.eyes[1].lid = 0.8f;
    const std::vector<uint8_t> without = encode(packet);
    Packet read;
    // Without brows: flag bit 2 clear and the bytes zero, as before 0.7.11, read as no brows
    CHECK(without[5] == 0 && without[64 + 34] == 0 && without[64 + 35] == 0 && without[100 + 34] == 0);
    CHECK(decode(without.data(), without.size(), read) && !read.brows && read.eyes[0].brow == 0.0f);
    // With brows: the same version and size, bit 2, each eye's brow a u16 at 34 (65535 = 1.0)
    packet.brows = true;
    packet.eyes[0].brow = 0.5f;
    packet.eyes[1].brow = 1.0f;
    const std::vector<uint8_t> with = encode(packet);
    CHECK(with.size() == kPacketSize && with[4] == 3 && with[5] == 4);
    CHECK(with[64 + 34] == 0x00 && with[64 + 35] == 0x80 && with[100 + 34] == 0xff && with[100 + 35] == 0xff);
    CHECK(decode(with.data(), with.size(), read) && read.brows);
    CHECK(near(read.eyes[0].brow, 0.5, 1e-4) && read.eyes[1].brow == 1.0f && read.eyes[0].lid == 0.9f);
    // Only byte 5's bit 2 and the four brow bytes differ: a panel from before (which reads neither) reads it as without
    int differ = 0;
    for (size_t i = 0; i < kPacketSize; ++i) differ += with[i] != without[i] ? 1 : 0;
    CHECK(differ == 4);  // (0.5 is 0x8000: its low byte stays 0)
    // The bytes frameeyeosc writes (src/dots.rs): bit 2, the left brow 0.5 (0x8000), the right 1.0
    std::vector<uint8_t> fromRust(kPacketSize, 0);
    const uint8_t head[8] = {'F', 'E', 'O', 'D', 3, 4, 0, 0};
    std::memcpy(fromRust.data(), head, sizeof(head));
    fromRust[64 + 35] = 0x80;
    fromRust[100 + 34] = 0xff;
    fromRust[100 + 35] = 0xff;
    CHECK(decode(fromRust.data(), kPacketSize, read) && read.brows);
    CHECK(near(read.eyes[0].brow, 32768.0 / 65535.0, 1e-6) && read.eyes[1].brow == 1.0f);
    // Bit 2 clear: the bytes aren't read (no brows), whatever is in them
    fromRust[5] = 0;
    CHECK(decode(fromRust.data(), kPacketSize, read) && !read.brows && read.eyes[1].brow == 0.0f);
    // Version 2 never has brows (bit 2 meant nothing there)
    std::vector<uint8_t> v2(with.begin(), with.begin() + kPacketSizeV2);
    v2[4] = 2;
    CHECK(decode(v2.data(), v2.size(), read) && !read.brows && read.eyes[0].brow == 0.0f);
    // Written within 0..1, NaN as 0
    packet.eyes[0].brow = NAN;
    packet.eyes[1].brow = 1.7f;
    const std::vector<uint8_t> odd = encode(packet);
    CHECK(decode(odd.data(), odd.size(), read) && read.eyes[0].brow == 0.0f && read.eyes[1].brow == 1.0f);
}

void testGeometry() {
    // Straight ahead from between the eyes: 2 m ahead
    const double kDistanceM = 2.0;
    Pose pose = dotPose(0.0, 0.0, 0, 0.063, kDistanceM);
    CHECK(near(pose.position.x, 0) && near(pose.position.y, 0) && near(pose.position.z, -kDistanceM));
    // Each eye's straight-ahead ray starts at that eye
    pose = dotPose(0.0, 0.0, -1, 0.063, kDistanceM);
    CHECK(near(pose.position.x, -0.0315) && near(pose.position.z, -kDistanceM));
    // 45° right and 45° up (1.0 each) lies along (1, 1, -1)
    pose = dotPose(1.0, 1.0, 0, 0.063, kDistanceM);
    const double side = kDistanceM / std::sqrt(3.0);
    CHECK(near(pose.position.x, side) && near(pose.position.y, side) && near(pose.position.z, -side));
    CHECK(near(pose.yawDeg, 45.0) && near(pose.pitchDeg, std::asin(1 / std::sqrt(3.0)) * 180 / M_PI));
    // Eyes turned in the way the eye fit expects for its dot straight ahead (gaze_fit::kTargetDistanceM, 0.9 m):
    // both dots, that far along their rays, meet near it
    const double fitM = gaze_fit::kTargetDistanceM;
    const Pose left = dotPose(gaze_fit::eyeAngle(0.0, 0, 0.063), 0.0, -1, 0.063, fitM);
    const Pose right = dotPose(gaze_fit::eyeAngle(0.0, 1, 0.063), 0.0, 1, 0.063, fitM);
    // (0.9 m along each ray, and the dot is 0.9 m ahead: they differ by a fraction of a millimetre)
    CHECK(near(left.position.x, 0.0, 1e-4) && near(right.position.x, 0.0, 1e-4));
    CHECK(near(left.position.z, -fitM, 1e-3) && left.yawDeg > 0 && right.yawDeg < 0);

    // At another distance: the same ray from the same eye and the same facing, only nearer
    for (const double distance : {0.5, 1.07, 2.0}) {
        const Pose far = dotPose(0.3, -0.2, -1, 0.063, kDistanceM);
        const Pose at = dotPose(0.3, -0.2, -1, 0.063, distance);
        const double eyeX = -0.0315;
        const double length = std::sqrt((at.position.x - eyeX) * (at.position.x - eyeX) +
                                        at.position.y * at.position.y + at.position.z * at.position.z);
        CHECK(near(length, distance));
        const double share = distance / kDistanceM;
        CHECK(near(at.position.x - eyeX, (far.position.x - eyeX) * share) && near(at.position.y, far.position.y * share) &&
              near(at.position.z, far.position.z * share));
        CHECK(near(at.yawDeg, far.yawDeg) && near(at.pitchDeg, far.pitchDeg));
        // The same angular size: width over distance stays put
        CHECK(near(dotWidth(distance) / distance, kWidthM / kWidthAtM));
    }
    // The two eyes' dots for the fit's dot straight ahead (0.9 m), shown twice as far: each on its own ray, so they
    // have crossed over, each as far to the other side as its eye (the eyes cross 0.9 m away, in front of them)
    const Pose leftFar = dotPose(gaze_fit::eyeAngle(0.0, 0, 0.063), 0.0, -1, 0.063, 2 * fitM);
    const Pose rightFar = dotPose(gaze_fit::eyeAngle(0.0, 1, 0.063), 0.0, 1, 0.063, 2 * fitM);
    CHECK(near(leftFar.position.x, 0.0315, 1e-4) && near(rightFar.position.x, -0.0315, 1e-4));

    // Which distance: the setting, 1 m if unset, kept within 0.3..2 m
    CHECK(near(dotDistance(std::nan("")), 1.0) && near(kDefaultDistanceM, 1.0));
    CHECK(near(dotDistance(0.8), 0.8) && near(dotDistance(0.05), 0.3) && near(dotDistance(5.0), 2.0));
    // At the default a dot is 17.5 mm wide (1 degree, as 35 mm at 2 m) on its eye's ray: straight ahead from the left eye
    const Pose leftDefault = dotPose(0.0, 0.0, -1, 0.063, dotDistance(std::nan("")));
    CHECK(near(leftDefault.position.x, -0.0315) && near(leftDefault.position.z, -1.0));
    CHECK(near(dotWidth(1.0), 0.0175));
}

void testSocket() {
    const std::string path = "/tmp/frameeyeosc-gaze-dots-test-" + std::to_string(::getpid()) + ".sock";
    Receiver receiver;
    std::string error;
    CHECK(receiver.open(path, error));
    Packet latest;
    CHECK(!receiver.poll(latest));
    const int sender = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    for (int i = 1; i <= 3; ++i) {
        Packet packet;
        packet.time = i;
        const std::vector<uint8_t> bytes = encode(packet);
        ::sendto(sender, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    }
    const uint8_t junk[3] = {1, 2, 3};
    ::sendto(sender, junk, sizeof(junk), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    // Everything waiting is read; the newest good packet is kept
    CHECK(receiver.poll(latest) && latest.time == 3.0);
    CHECK(!receiver.poll(latest));
    ::close(sender);
    receiver.close();
    CHECK(::access(path.c_str(), F_OK) != 0);
    // A file left by a crash is replaced
    CHECK(receiver.open(path, error) && receiver.open(path, error));
    receiver.close();
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testPacket();
    testBrows();
    testGeometry();
    testSocket();
    if (gFailures == 0) std::printf("gaze-dots-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
