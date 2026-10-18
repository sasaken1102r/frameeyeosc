// The debug gaze dots: frameeyeosc streams the gaze it sends (one datagram per sample, see src/dots.rs) to a Unix
// socket in the status folder while "gaze_debug_dots" is on, and the panel shows a dot 1 m ahead where that gaze
// points: one for the combined gaze, or one per eye, each along that eye's own ray. While the user's gaze scale is in
// use (not 100 % on all sides), a ring of the same colour shows where the gaze pointed before the scale (where the eyes
// look), around the dot when they agree. The same datagrams carry each
// eye's eyelid, squint and pupil (as sent, from Valve and from the eye camera) for the eye preview, which binds the
// socket while it is open (frameeyeosc sends while the socket is there). Nothing here talks to OpenVR, so the packet
// and the geometry are tested on their own (gaze_dots_test.cpp).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gaze_dots {

/** The socket's name in the status folder (the same as frameeyeosc's). */
constexpr const char* kSocketName = "gaze-dots.sock";
/**
 * One datagram (version 3), little-endian: b"FEOD", 3, flags (bit 0 per-eye gaze, bit 1 the gaze scale in use, bit 2
 * brows are sent), 2 zero bytes, time (f64), six gaze values as sent and six before smoothing (f32), each eye's values
 * (kEyeSize bytes; see src/dots.rs; its brow a u16 at 34, 65535 = 1.0, in what were two reserved zero bytes), then six
 * gaze values as sent but before the user's gaze scale (f32). The brows came in 0.7.11 with the same version and size:
 * a panel from before ignores them, and an older frameeyeosc's datagram (bit 2 clear) reads as without brows.
 */
constexpr size_t kPacketSize = 160;
/** Version 2 (0.7.7 and before): the same without the gaze before the scale (and its flag), still read (as unscaled).
 *  Any other version is ignored. */
constexpr size_t kPacketSizeV2 = 136;
/** One eye's part of it. */
constexpr size_t kEyeSize = 36;
/** How far along each ray the dots are shown (m; gaze_debug_dots_distance_m), the same with the dashboard open or
 *  closed so they never jump. Plain overlays show over the dashboard (about 1.35 m away) up to about 1 m: at 1.2 m
 *  it hid them, depending on where the head was. Up to 2 m is allowed for use with the dashboard closed... */
constexpr double kDefaultDistanceM = 1.0;
/** ...within this range. */
constexpr double kMinDistanceM = 0.3;
constexpr double kMaxDistanceM = 2.0;
/** A dot is this wide (m) at kWidthAtM (about 1 degree), and scaled with its distance to look the same size. */
constexpr double kWidthM = 0.035;
constexpr double kWidthAtM = 2.0;
/** The ring (the gaze before the scale) is this many times as wide as a dot, so it goes around the dot when they meet. */
constexpr double kRingWidthScale = 1.6;
/** Without a packet for this long, the dots are hidden (frameeyeosc stopped or tracking is lost). */
constexpr double kStaleSec = 1.0;

/** One eye's values in a sample. NaN where there is none (no fresh eye-camera values for the eye, nothing sent). */
struct PacketEye {
    float lid = NAN;             ///< the eyelid as sent (VRCFT: 0 closed, 0.75 relaxed open, 1 widened)
    float openness = NAN;        ///< Valve's openness as read
    float opennessScaled = NAN;  ///< ...after the eye's scale (the left column's raw bar)
    float cameraLid = NAN;       ///< the eye camera's eyelid (eyecam-rec's, VRCFT scale)
    float squint = NAN;          ///< the squint as sent
    float pupilMm = NAN;         ///< the pupil diameter as sent (mm)
    float cameraSquint = NAN;    ///< the eye camera's squint
    float cameraPupilMm = NAN;   ///< the eye camera's pupil diameter (mm)
    int lidFrom = 0;             ///< where the sent eyelid came from: 0 the eye server's usual mix, 1.. the eye camera
    bool cameraClosed = false;   ///< the eye camera reads the eye closed
    float brow = 0.0f;           ///< the brow as sent, 0 at rest .. 1 raised all the way (0 while no brows are sent)
};

/** One sample as frameeyeosc sent it. */
struct Packet {
    double time = 0.0;
    float gaze[6] = {0, 0, 0, 0, 0, 0};     ///< left x/y, right x/y, combined x/y as sent; 1.0 = 45°
    float rawGaze[6] = {0, 0, 0, 0, 0, 0};  ///< the same before smoothing
    float unscaledGaze[6] = {0, 0, 0, 0, 0, 0};  ///< the same as sent, but before the user's gaze scale
    bool independent = false;              ///< each eye's own gaze is sent (two dots)
    bool scaled = false;                   ///< the user's gaze scale is in use (not 1 on all sides): rings too
    bool brows = false;                    ///< brows are sent (brow_from_widen, to an output that takes them)
    PacketEye eyes[2];                     ///< the left eye, the right eye (anatomical)
};

/**
 * Read a datagram.
 * @param data the bytes
 * @param size how many
 * @param out the sample
 * @return false if it is not a gaze dots packet of version 3 or 2 (or its gaze isn't finite)
 */
bool decode(const uint8_t* data, size_t size, Packet& out);

/**
 * Write a datagram (the other direction, for tests).
 * @param packet the sample
 * @return the bytes
 */
std::vector<uint8_t> encode(const Packet& packet);

/** A point in head space: +x right, +y up, -z ahead (m). */
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/** Where a dot goes and which way it faces. */
struct Pose {
    Vec3 position;
    double yawDeg = 0.0;    ///< turned this far right...
    double pitchDeg = 0.0;  ///< ...and up, facing back along the ray
};

/**
 * Where a gaze points, a distance along the ray from an eye. The gaze values are the tracker's angles
 * (atan2(x, -z) and atan2(y, -z), 1.0 = 45°), so the ray is (tan x, tan y, -1).
 * @param x sideways gaze (1.0 = 45° right)
 * @param y up/down gaze (1.0 = 45° up)
 * @param eye -1 = the left eye (at -ipd/2), 0 = between the eyes, 1 = the right eye (at +ipd/2)
 * @param ipd the distance between the eyes (m)
 * @param distance how far along the ray (m)
 * @return the pose
 */
Pose dotPose(double x, double y, int eye, double ipd, double distance);

/**
 * How far along the rays the dots go: the gaze_debug_dots_distance_m setting, kept within
 * kMinDistanceM..kMaxDistanceM (kDefaultDistanceM if unset).
 * @param setting the setting (m; NaN if unset)
 * @return the distance (m)
 */
double dotDistance(double setting);

/**
 * How wide a dot is at a distance, so it always looks the same size.
 * @param distance how far along its ray (m)
 * @return the width (m)
 */
double dotWidth(double distance);

/** The panel's end of the socket: bound while the dots are on or the eye preview is open, non-blocking. */
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
     * Read everything waiting and keep the newest.
     * @param latest the newest sample (unchanged if none)
     * @return true if there was one
     */
    bool poll(Packet& latest);

private:
    int fd_ = -1;
    std::string path_;
};

}  // namespace gaze_dots
