// The eye preview: the drawn eyes' values, eyecam-rec's preview file, and when to ask for it.
#include "eye_preview.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>

namespace eye_preview {

namespace {

/** Where the seqlock's counter is in the file (odd while eyecam-rec writes). */
constexpr size_t kSeqAt = 24;
/** A copy is tried this many times while eyecam-rec writes; then it waits for the next read. */
constexpr int kMaxTries = 8;
/** An eye's flags: the pupil was found, the eye reads closed. */
constexpr uint32_t kPupilFound = 1;
constexpr uint32_t kClosed = 2;

// The Frame is little-endian (aarch64), like every machine this is built for; the file is read with memcpy
static_assert(sizeof(float) == 4, "the file has 4-byte floats");

/**
 * A little-endian u32 at an offset.
 * @param data the bytes
 * @param at the offset
 * @return the number
 */
uint32_t u32At(const uint8_t* data, size_t at) {
    uint32_t value = 0;
    std::memcpy(&value, data + at, 4);
    return value;
}

/**
 * A little-endian u64 at an offset.
 * @param data the bytes
 * @param at the offset
 * @return the number
 */
uint64_t u64At(const uint8_t* data, size_t at) {
    uint64_t value = 0;
    std::memcpy(&value, data + at, 8);
    return value;
}

/**
 * A little-endian f32 at an offset.
 * @param data the bytes
 * @param at the offset
 * @return the number
 */
double f32At(const uint8_t* data, size_t at) {
    float value = 0;
    std::memcpy(&value, data + at, 4);
    return value;
}

/**
 * Write a little-endian number at an offset.
 * @param data the bytes
 * @param at the offset
 * @param value the number
 */
template <typename T>
void put(std::vector<uint8_t>& data, size_t at, T value) {
    std::memcpy(data.data() + at, &value, sizeof(T));
}

/**
 * A value that is a number, or a fallback.
 * @param value the value
 * @param fallback for NaN
 * @return the number
 */
double orElse(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
}

/**
 * A lid line from the file: its first and last column, then its rows there and halfway.
 * @param data the bytes
 * @param at where it starts
 * @return the line (not present unless all five are numbers and it runs left to right)
 */
LidLine lidLineAt(const uint8_t* data, size_t at) {
    LidLine line;
    line.x0 = f32At(data, at);
    line.x1 = f32At(data, at + 4);
    for (int i = 0; i < 3; ++i) line.y[i] = f32At(data, at + 8 + i * 4);
    line.present = std::isfinite(line.x0) && std::isfinite(line.x1) && line.x1 > line.x0 && std::isfinite(line.y[0]) &&
                   std::isfinite(line.y[1]) && std::isfinite(line.y[2]);
    return line;
}

/**
 * Write a lid line as the file has it (NaN for one not present).
 * @param data the bytes
 * @param at where it starts
 * @param line the line
 */
void putLidLine(std::vector<uint8_t>& data, size_t at, const LidLine& line) {
    const double values[5] = {line.x0, line.x1, line.y[0], line.y[1], line.y[2]};
    for (int i = 0; i < 5; ++i) put<float>(data, at + i * 4, line.present ? static_cast<float>(values[i]) : NAN);
}

}  // namespace

double lidLineY(const LidLine& line, double x) {
    // (Lagrange through the three rows: s 0 at x0, 0.5 halfway, 1 at x1)
    const double s = (x - line.x0) / (line.x1 - line.x0);
    return line.y[0] * (1 - s) * (1 - 2 * s) + line.y[1] * 4 * s * (1 - s) + line.y[2] * s * (2 * s - 1);
}

Shown shown(const gaze_dots::Packet& packet, Source source, const CameraLids& lids) {
    Shown s;
    s.valid = true;
    s.brows = packet.brows;
    // The camera gives frameeyeosc no gaze: Valve's before smoothing, as Raw
    const float* gaze = source == Source::Sent ? packet.gaze : packet.rawGaze;
    s.gazeX = gaze[4];
    s.gazeY = gaze[5];
    for (int eye = 0; eye < 2; ++eye) {
        const gaze_dots::PacketEye& in = packet.eyes[eye];
        EyeShown& out = s.eyes[eye];
        out.gazeX = gaze[eye * 2];
        out.gazeY = gaze[eye * 2 + 1];
        out.closedByCamera = in.cameraClosed;
        switch (source) {
            case Source::Sent:
                out.known = std::isfinite(in.lid);
                out.lid = in.lid;
                out.lidValue = in.lid;
                out.squint = orElse(in.squint, 0.0);
                out.pupilMm = in.pupilMm;
                // Where the eyelid comes from, as frameeyeosc's status has it; without one, LidFrom (0 the eye
                // server's usual mix, anything else the camera overrode it)
                out.camera = lids.known ? lids.used[eye] : in.lidFrom != 0;
                if (packet.brows) out.brow = std::clamp(orElse(in.brow, 0.0), 0.0, 1.0);
                break;
            case Source::Raw:
                // Valve's openness after the eye's scale: 1.0 (an open eye) drawn as relaxed open
                out.known = std::isfinite(in.opennessScaled);
                out.lid = in.opennessScaled * 0.75;
                out.lidValue = in.opennessScaled;
                break;
            case Source::Camera:
                out.known = std::isfinite(in.cameraLid);
                out.lid = in.cameraLid;
                out.lidValue = in.cameraLid;
                out.squint = orElse(in.cameraSquint, 0.0);
                out.pupilMm = in.cameraPupilMm;
                out.camera = true;
                break;
        }
        out.lid = out.known ? std::clamp(out.lid, 0.0, 1.0) : 0.75;
        out.squint = std::clamp(out.squint, 0.0, 1.0);
    }
    return s;
}

double upperLidOpen(double lid) {
    const double l = std::clamp(orElse(lid, 0.75), 0.0, 1.0);
    if (l <= 0.75) return l / 0.75 * kRelaxedOpen;
    return kRelaxedOpen + (l - 0.75) / 0.25 * (1.0 - kRelaxedOpen);
}

double lowerLidRise(double squint) {
    return std::clamp(orElse(squint, 0.0), 0.0, 1.0) * kMaxSquintRise;
}

double browLift(double brow) {
    return std::clamp(orElse(brow, 0.0), 0.0, 1.0) * kBrowRise;
}

double pupilRadius(double irisRadius, double mm) {
    return irisRadius * std::clamp(orElse(mm, kDefaultPupilMm), 1.5, 9.0) / kIrisMm;
}

double gazeShare(double gaze, bool vertical) {
    return std::clamp(orElse(gaze, 0.0) * 45.0 / (vertical ? 30.0 : 35.0), -1.0, 1.0);
}

int drawnColumn(int eye, bool mirror) {
    return mirror ? eye : 1 - eye;
}

bool gazeTurned(bool mirror) {
    return !mirror;
}

bool pictureFlipped(int eye, bool mirror) {
    return drawnColumn(eye, mirror) == 1;
}

bool parsePictures(const uint8_t* data, size_t size, Pictures& out, std::string& why) {
    if (size < kHeaderSize) {
        why = "too short for the header (" + std::to_string(size) + " bytes)";
        return false;
    }
    if (u32At(data, 0) != kMagic) {
        why = "not eyecam-rec's preview";
        return false;
    }
    if (u32At(data, 4) != kVersion) {
        why = "version " + std::to_string(u32At(data, 4)) + ", not " + std::to_string(kVersion);
        return false;
    }
    const uint32_t header = u32At(data, 8);
    const uint32_t eyeSize = u32At(data, 12);
    const uint32_t width = u32At(data, 16);
    const uint32_t height = u32At(data, 20);
    if (header != kHeaderSize || width != height || width == 0 || width > static_cast<uint32_t>(kMaxSide) ||
        eyeSize != kEyeHeaderSize + static_cast<size_t>(width) * height) {
        why = "unexpected sizes (header " + std::to_string(header) + ", eye " + std::to_string(eyeSize) + ", " +
              std::to_string(width) + "x" + std::to_string(height) + ")";
        return false;
    }
    if (size < kHeaderSize + 2 * static_cast<size_t>(eyeSize)) {
        why = "shorter than its two pictures (" + std::to_string(size) + " bytes)";
        return false;
    }
    // The lid lines, after both pictures, when the header says where (0 in an older eyecam-rec's file) and they fit
    const size_t lidsAt = u32At(data, kLidsAtOffset);
    const size_t lidSize = u32At(data, kLidSizeOffset);
    const bool lids = lidsAt >= kHeaderSize + 2 * static_cast<size_t>(eyeSize) && lidSize >= kLidBytes &&
                      lidsAt + 2 * lidSize <= size;
    Pictures pictures;
    pictures.on = u32At(data, 48) == 1;
    pictures.writerPid = u32At(data, 28);
    pictures.writerStartNs = u64At(data, 32);
    for (int eye = 0; eye < 2; ++eye) {
        const size_t at = kHeaderSize + eye * static_cast<size_t>(eyeSize);
        Picture& p = pictures.eyes[eye];
        p.timeNs = u64At(data, at);
        p.present = p.timeNs != 0;
        p.count = u64At(data, at + 8);
        const uint32_t flags = u32At(data, at + 16);
        p.closed = (flags & kClosed) != 0;
        p.pupilX = f32At(data, at + 20);
        p.pupilY = f32At(data, at + 24);
        p.pupilR = f32At(data, at + 28);
        p.pupil = (flags & kPupilFound) != 0 && std::isfinite(p.pupilX) && std::isfinite(p.pupilY) &&
                  std::isfinite(p.pupilR) && p.pupilR > 0;
        for (int i = 0; i < 4; ++i) p.window[i] = f32At(data, at + 32 + i * 4);
        p.side = static_cast<int>(width);
        p.pixels.assign(data + at + kEyeHeaderSize, data + at + eyeSize);
        p.lids = lids;
        if (lids) {
            const size_t l = lidsAt + eye * lidSize;
            p.eyeLid = f32At(data, l);
            p.upper = lidLineAt(data, l + 4);
            p.margin = f32At(data, l + 24);
            p.lower = lidLineAt(data, l + 28);
        }
    }
    out = std::move(pictures);
    return true;
}

std::vector<uint8_t> encodePictures(const Pictures& pictures, int side) {
    const size_t eyeSize = kEyeHeaderSize + static_cast<size_t>(side) * side;
    const size_t lidsAt = kHeaderSize + 2 * eyeSize;
    const size_t lidSize = 64;
    const size_t used = lidsAt + 2 * lidSize;
    std::vector<uint8_t> data((used + 4095) / 4096 * 4096, 0);
    put<uint32_t>(data, kLidsAtOffset, static_cast<uint32_t>(lidsAt));
    put<uint32_t>(data, kLidSizeOffset, static_cast<uint32_t>(lidSize));
    put<uint32_t>(data, 0, kMagic);
    put<uint32_t>(data, 4, kVersion);
    put<uint32_t>(data, 8, kHeaderSize);
    put<uint32_t>(data, 12, static_cast<uint32_t>(eyeSize));
    put<uint32_t>(data, 16, static_cast<uint32_t>(side));
    put<uint32_t>(data, 20, static_cast<uint32_t>(side));
    put<uint32_t>(data, 28, pictures.writerPid);
    put<uint64_t>(data, 32, pictures.writerStartNs);
    put<uint32_t>(data, 48, pictures.on ? 1 : 0);
    for (int eye = 0; eye < 2; ++eye) {
        const Picture& p = pictures.eyes[eye];
        const size_t at = kHeaderSize + eye * eyeSize;
        put<uint64_t>(data, at, p.present ? p.timeNs : 0);
        put<uint64_t>(data, at + 8, p.count);
        put<uint32_t>(data, at + 16, (p.pupil ? kPupilFound : 0) | (p.closed ? kClosed : 0));
        const float marks[7] = {static_cast<float>(p.pupilX),    static_cast<float>(p.pupilY),
                                static_cast<float>(p.pupilR),    static_cast<float>(p.window[0]),
                                static_cast<float>(p.window[1]), static_cast<float>(p.window[2]),
                                static_cast<float>(p.window[3])};
        std::memcpy(data.data() + at + 20, marks, sizeof(marks));
        if (p.pixels.size() == static_cast<size_t>(side) * side) {
            std::memcpy(data.data() + at + kEyeHeaderSize, p.pixels.data(), p.pixels.size());
        }
        const size_t l = lidsAt + eye * lidSize;
        put<float>(data, l, static_cast<float>(p.eyeLid));
        putLidLine(data, l + 4, p.upper);
        put<float>(data, l + 24, static_cast<float>(p.margin));
        putLidLine(data, l + 28, p.lower);
    }
    return data;
}

PictureReader::~PictureReader() {
    close();
}

void PictureReader::close() {
    if (map_ != nullptr) ::munmap(map_, size_);
    if (fd_ >= 0) ::close(fd_);
    map_ = nullptr;
    fd_ = -1;
    size_ = 0;
    inode_ = 0;
    lastSeq_ = 1;
}

ReadResult PictureReader::read(const std::string& path, Pictures& out, std::string& why) {
    // Replaced (a new eyecam-rec, or turned off and on again) or gone (turned off): let go of the old one
    struct stat now {};
    if (::stat(path.c_str(), &now) != 0) {
        close();
        badInode_ = 0;
        return ReadResult::Missing;
    }
    if (fd_ >= 0 && now.st_ino != inode_) close();
    // Found not to be a preview file: not mapped and copied again until it is replaced
    if (badInode_ != 0 && now.st_ino == badInode_) {
        why = badWhy_;
        return ReadResult::Invalid;
    }
    badInode_ = 0;
    if (fd_ < 0) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            if (errno == ENOENT) return ReadResult::Missing;
            why = std::string("open: ") + std::strerror(errno);
            return ReadResult::Invalid;
        }
        struct stat st {};
        // Only a regular file of this user's (eyecam-rec runs as the same user)
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid() ||
            st.st_size < static_cast<off_t>(kHeaderSize)) {
            why = "not a regular file of this user's, or too short";
            ::close(fd);
            return ReadResult::Invalid;
        }
        void* map = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            why = std::string("mmap: ") + std::strerror(errno);
            ::close(fd);
            return ReadResult::Invalid;
        }
        fd_ = fd;
        map_ = map;
        size_ = static_cast<size_t>(st.st_size);
        inode_ = st.st_ino;
        lastSeq_ = 1;
    }
    const uint8_t* base = static_cast<const uint8_t*>(map_);
    const auto seq = [&]() {
        return __atomic_load_n(reinterpret_cast<const uint32_t*>(base + kSeqAt), __ATOMIC_ACQUIRE);
    };
    std::vector<uint8_t> copy(size_);
    for (int tries = 0; tries < kMaxTries; ++tries) {
        const uint32_t before = seq();
        if (before % 2 == 1) continue;
        if (before == lastSeq_) return ReadResult::Unchanged;
        std::memcpy(copy.data(), base, size_);
        // The copy's reads may not move after the second look at the counter
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq() != before) continue;
        Pictures pictures;
        if (!parsePictures(copy.data(), copy.size(), pictures, why)) {
            badInode_ = inode_;
            badWhy_ = why;
            close();
            return ReadResult::Invalid;
        }
        lastSeq_ = before;
        out = std::move(pictures);
        return ReadResult::Read;
    }
    return ReadResult::Busy;
}

uint64_t monotonicNs() {
    timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

bool pictureFresh(const Picture& picture, uint64_t nowNs) {
    if (!picture.present || picture.side <= 0) return false;
    // (a time a little ahead of now is rounding between the two clocks' readings)
    if (picture.timeNs > nowNs) return picture.timeNs - nowNs < 50000000ull;
    return static_cast<double>(nowNs - picture.timeNs) <= kStaleSec * 1e9;
}

CameraState cameraState(const eyecam::View& view, bool cameraLids, bool refused) {
    if (!view.visible) return CameraState::NotRunning;
    if (!eyecam::setUp(view)) return CameraState::NotSetUp;
    if (!cameraLids) return CameraState::NotInUse;
    if (!view.status.hasPreview) return CameraState::Old;
    if (refused) return CameraState::Refused;
    return CameraState::Available;
}

bool cameraValuesAvailable(CameraState state) {
    return state != CameraState::NotRunning && state != CameraState::NotSetUp && state != CameraState::NotInUse;
}

bool Asker::next(bool wanted, bool busy, double now, std::string& command) {
    if (busy) return false;
    if (wanted) {
        if (on_ && now - sentAt_ < kRenewSec) return false;
        command = "preview on";
        on_ = true;
        sentAt_ = now;
        return true;
    }
    if (!on_) return false;
    command = "preview off";
    on_ = false;
    return true;
}

bool tookReply(View& view, bool ok, double now) {
    const bool changed = view.refused == ok;
    view.refused = !ok;
    if (!ok) view.refusedAt = now;
    return changed;
}

bool refusalOver(View& view, double now) {
    if (!view.refused || now - view.refusedAt < kRefusedSec) return false;
    view.refused = false;
    return true;
}

void fake(View& view, uint64_t nowNs) {
    gaze_dots::Packet& p = view.packet;
    p = gaze_dots::Packet {};
    p.time = 1.0;
    p.independent = false;
    // Looking a little right and down; the raw gaze a little further and less steady
    const float sent[6] = {0.10f, -0.07f, 0.10f, -0.07f, 0.10f, -0.07f};
    const float raw[6] = {0.14f, -0.09f, 0.11f, -0.10f, 0.125f, -0.095f};
    std::memcpy(p.gaze, sent, sizeof(sent));
    std::memcpy(p.rawGaze, raw, sizeof(raw));
    // The left eye a little widened, from the camera; the right one narrowed (a squint), from Valve
    gaze_dots::PacketEye& left = p.eyes[0];
    left.lid = 0.86f;
    left.openness = 1.0f;
    left.opennessScaled = 1.02f;
    left.cameraLid = 0.88f;
    left.squint = 0.0f;
    left.pupilMm = 6.1f;
    left.cameraSquint = 0.02f;
    left.cameraPupilMm = 6.2f;
    left.lidFrom = 4;
    gaze_dots::PacketEye& right = p.eyes[1];
    right.lid = 0.55f;
    right.openness = 0.74f;
    right.opennessScaled = 0.73f;
    right.cameraLid = 0.58f;
    right.squint = 0.4f;
    right.pupilMm = 5.8f;
    right.cameraSquint = 0.42f;
    right.cameraPupilMm = 5.7f;
    right.lidFrom = 0;
    view.hasPacket = true;
    // Grey pictures with a lighter disc and a dark one in it (not an eye), and the lens edge's dark band on the right
    const int side = 200;
    view.pictures = Pictures {};
    view.pictures.on = true;
    view.pictures.writerPid = 1;
    view.pictures.writerStartNs = 1;
    const double centres[2][2] = {{112.0, 96.0}, {120.0, 102.0}};
    for (int eye = 0; eye < 2; ++eye) {
        Picture& pic = view.pictures.eyes[eye];
        pic.present = true;
        pic.timeNs = nowNs;
        pic.count = 42;
        pic.side = side;
        pic.pixels.assign(static_cast<size_t>(side) * side, 0);
        const double cx = centres[eye][0];
        const double cy = centres[eye][1];
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const double d = std::hypot(x - cx, y - cy);
                double v = 70 + x * 0.25 + y * 0.15;
                if (d < 44) v = 92;
                if (d < 19 + eye) v = 14;
                if (x > 176) v = 12;
                pic.pixels[static_cast<size_t>(y) * side + x] = static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
            }
        }
        pic.pupil = true;
        pic.pupilX = cx;
        pic.pupilY = cy;
        pic.pupilR = 19 + eye;
        const double window[4] = {93.0, 35.0, 173.0, 160.0};
        std::copy(window, window + 4, pic.window);
        pic.closed = false;
        // Lid lines over the disc, the right eye's narrowed (its squint)
        const double narrow = eye == 0 ? 0.0 : 9.0;
        pic.lids = true;
        pic.eyeLid = p.eyes[eye].cameraLid;
        for (LidLine* line : {&pic.upper, &pic.lower}) {
            line->present = true;
            line->x0 = cx - 50;
            line->x1 = cx + 56;
        }
        const double upper[3] = {cy - 22 + narrow, cy - 33 + narrow, cy - 20 + narrow};
        const double lower[3] = {cy + 30 - narrow, cy + 36 - narrow, cy + 28 - narrow};
        std::copy(upper, upper + 3, pic.upper.y);
        std::copy(lower, lower + 3, pic.lower.y);
        pic.margin = 4;
    }
    view.hasPictures = true;
    view.nowNs = nowNs;
    view.refused = false;
}

void fakeBrows(View& view, double level) {
    view.packet.brows = true;
    for (gaze_dots::PacketEye& eye : view.packet.eyes) eye.brow = static_cast<float>(level);
}

}  // namespace eye_preview
