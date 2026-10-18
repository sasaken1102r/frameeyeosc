// The debug gaze dots: the packet, the geometry and the socket.
#include "gaze_dots.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace gaze_dots {

namespace {

constexpr uint8_t kMagic[4] = {'F', 'E', 'O', 'D'};
constexpr uint8_t kVersion = 3;
/** Where the eyes' values start. */
constexpr size_t kEyesAt = 64;
/** Where an eye's brow is in its values (u16), and the flag that says the brows are there (version 3). */
constexpr size_t kBrowAt = 34;
constexpr uint8_t kBrowsFlag = 4;
/** Where the gaze before the scale starts (version 3: right after the eyes). */
constexpr size_t kUnscaledAt = kEyesAt + 2 * kEyeSize;
static_assert(kUnscaledAt == kPacketSizeV2 && kUnscaledAt + 24 == kPacketSize, "version 3 is version 2 and 24 bytes");

// The Frame is little-endian (aarch64), like every machine this is built for; the packet is read with memcpy
static_assert(sizeof(float) == 4 && sizeof(double) == 8, "the packet has 4-byte floats and an 8-byte double");

}  // namespace

bool decode(const uint8_t* data, size_t size, Packet& out) {
    // Version 2 (an older frameeyeosc) is read too: it has no gaze before the scale, so that is the gaze as sent
    const bool v3 = size == kPacketSize && data[4] == kVersion;
    const bool v2 = size == kPacketSizeV2 && data[4] == 2;
    if (!(v3 || v2) || std::memcmp(data, kMagic, 4) != 0) return false;
    Packet packet;
    packet.independent = (data[5] & 1) != 0;
    packet.scaled = v3 && (data[5] & 2) != 0;
    packet.brows = v3 && (data[5] & kBrowsFlag) != 0;
    std::memcpy(&packet.time, data + 8, 8);
    std::memcpy(packet.gaze, data + 16, 24);
    std::memcpy(packet.rawGaze, data + 40, 24);
    std::memcpy(packet.unscaledGaze, v3 ? data + kUnscaledAt : data + 16, 24);
    for (int i = 0; i < 6; ++i) {
        const bool finite = std::isfinite(packet.gaze[i]) && std::isfinite(packet.rawGaze[i]);
        if (!finite || !std::isfinite(packet.unscaledGaze[i])) return false;
    }
    for (int eye = 0; eye < 2; ++eye) {
        // (its values may be NaN: none for that eye)
        const uint8_t* at = data + kEyesAt + eye * kEyeSize;
        PacketEye& e = packet.eyes[eye];
        float values[8];
        std::memcpy(values, at, sizeof(values));
        e.lid = values[0];
        e.openness = values[1];
        e.opennessScaled = values[2];
        e.cameraLid = values[3];
        e.squint = values[4];
        e.pupilMm = values[5];
        e.cameraSquint = values[6];
        e.cameraPupilMm = values[7];
        e.lidFrom = at[32];
        e.cameraClosed = (at[33] & 1) != 0;
        if (packet.brows) e.brow = static_cast<float>(at[kBrowAt] | (at[kBrowAt + 1] << 8)) / 65535.0f;
    }
    out = packet;
    return true;
}

std::vector<uint8_t> encode(const Packet& packet) {
    std::vector<uint8_t> data(kPacketSize, 0);
    std::memcpy(data.data(), kMagic, 4);
    data[4] = kVersion;
    data[5] = static_cast<uint8_t>((packet.independent ? 1 : 0) | (packet.scaled ? 2 : 0) |
                                   (packet.brows ? kBrowsFlag : 0));
    std::memcpy(data.data() + 8, &packet.time, 8);
    std::memcpy(data.data() + 16, packet.gaze, 24);
    std::memcpy(data.data() + 40, packet.rawGaze, 24);
    for (int eye = 0; eye < 2; ++eye) {
        uint8_t* at = data.data() + kEyesAt + eye * kEyeSize;
        const PacketEye& e = packet.eyes[eye];
        const float values[8] = {e.lid,    e.openness, e.opennessScaled, e.cameraLid,
                                 e.squint, e.pupilMm,  e.cameraSquint,   e.cameraPupilMm};
        std::memcpy(at, values, sizeof(values));
        at[32] = static_cast<uint8_t>(e.lidFrom);
        at[33] = e.cameraClosed ? 1 : 0;
        if (packet.brows) {
            // (kept within 0..1, NaN as 0, as frameeyeosc writes it)
            const float brow = std::isfinite(e.brow) ? std::clamp(e.brow, 0.0f, 1.0f) : 0.0f;
            const auto bits = static_cast<uint16_t>(std::lround(brow * 65535.0f));
            at[kBrowAt] = static_cast<uint8_t>(bits & 0xff);
            at[kBrowAt + 1] = static_cast<uint8_t>(bits >> 8);
        }
    }
    std::memcpy(data.data() + kUnscaledAt, packet.unscaledGaze, 24);
    return data;
}

Pose dotPose(double x, double y, int eye, double ipd, double distance) {
    const double scale = 45.0 * M_PI / 180.0;
    // The ray the tracker's angles describe, from the eye
    const double rx = std::tan(x * scale);
    const double ry = std::tan(y * scale);
    const double length = std::sqrt(rx * rx + ry * ry + 1.0);
    Pose pose;
    pose.position.x = eye * ipd / 2 + distance * rx / length;
    pose.position.y = distance * ry / length;
    pose.position.z = -distance / length;
    // Facing back along the ray: turned right by its yaw, up by its pitch
    pose.yawDeg = std::atan2(rx, 1.0) * 180.0 / M_PI;
    pose.pitchDeg = std::asin(ry / length) * 180.0 / M_PI;
    return pose;
}

double dotDistance(double setting) {
    if (!std::isfinite(setting)) return kDefaultDistanceM;
    return std::clamp(setting, kMinDistanceM, kMaxDistanceM);
}

double dotWidth(double distance) {
    return kWidthM * distance / kWidthAtM;
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

bool Receiver::poll(Packet& latest) {
    if (fd_ < 0) return false;
    bool got = false;
    uint8_t buffer[kPacketSize + 8];
    for (;;) {
        const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), 0);
        if (n < 0) break;  // EAGAIN: nothing more waiting
        Packet packet;
        if (decode(buffer, static_cast<size_t>(n), packet)) {
            latest = packet;
            got = true;
        }
    }
    return got;
}

}  // namespace gaze_dots
