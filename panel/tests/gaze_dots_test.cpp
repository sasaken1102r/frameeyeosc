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
    packet.independent = true;
    const std::vector<uint8_t> bytes = encode(packet);
    CHECK(bytes.size() == kPacketSize && std::memcmp(bytes.data(), "FEOD\x01\x01\x00\x00", 8) == 0);
    Packet read;
    CHECK(decode(bytes.data(), bytes.size(), read));
    CHECK(read.time == packet.time && read.independent && std::memcmp(read.gaze, gaze, sizeof(gaze)) == 0);
    // The same bytes frameeyeosc writes (src/dots.rs): time 1.0, combined gaze only
    const uint8_t fromRust[kPacketSize] = {'F', 'E', 'O', 'D', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f,
                                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0, 0x3f, 0, 0, 0x80, 0xbe};
    CHECK(decode(fromRust, kPacketSize, read) && read.time == 1.0 && !read.independent);
    CHECK(read.gaze[4] == 0.5f && read.gaze[5] == -0.25f);
    std::vector<uint8_t> wrong = bytes;
    wrong[4] = 2;
    CHECK(!decode(wrong.data(), wrong.size(), read));
    CHECK(!decode(bytes.data(), bytes.size() - 1, read));
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
    testGeometry();
    testSocket();
    if (gFailures == 0) std::printf("gaze-dots-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
