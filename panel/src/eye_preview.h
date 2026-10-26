// The eye preview (目のプレビュー): a sheet that draws both eyes as they go out (or as Valve or the eye camera read them)
// and, under each, that eye's picture from the eye camera with what eyecam-rec found on it. The values come from
// frameeyeosc's datagrams on the gaze dots socket (gaze_dots.h: the panel binds it while the sheet is open, and
// frameeyeosc sends while it is there); the pictures from eyecam-rec's preview file, which it only writes while the
// panel asks ("preview on" on ctl.sock, renewed every kRenewSec; it stops by itself a few seconds after the last).
// The pictures are only shown: the panel maps the file read-only and never writes them anywhere. Nothing here draws or
// talks to OpenVR, so it is tested on its own (eye_preview_test.cpp).
#pragma once

#include "eyecam.h"
#include "gaze_dots.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace eye_preview {

/** Which values drive the drawn eyes: as sent, as read (Valve's openness, the gaze before smoothing), the camera's. */
enum class Source { Sent, Raw, Camera };

/** One drawn eye. */
struct EyeShown {
    bool known = false;     ///< there is an eyelid to draw (else drawn faded, relaxed open)
    double lid = 0.75;      ///< VRCFT scale: 0 closed, 0.75 relaxed open, 1 widened (what is drawn)
    double lidValue = NAN;  ///< the eyelid as its source has it (Valve's openness for Raw; the chip's number)
    double squint = 0.0;    ///< 0..1 (raises the lower lid)
    double pupilMm = NAN;   ///< the pupil's diameter (NaN: none; drawn at kDefaultPupilMm)
    double gazeX = 0.0;     ///< that eye's gaze (1.0 = 45°, + right)
    double gazeY = 0.0;     ///< (+ up)
    bool camera = false;    ///< its eyelid came from the eye camera (the source badge)
    bool closedByCamera = false;  ///< the eye camera reads it closed
    double brow = NAN;      ///< the brow as sent, 0 at rest .. 1 raised (NaN: none drawn; Sent only, see shown)
};

/** Both eyes as drawn, and the gaze for the footer. */
struct Shown {
    bool valid = false;  ///< there was a sample
    bool brows = false;  ///< brows are sent (brow_from_widen): the eyes leave room above them for the brows
    EyeShown eyes[2];    ///< left, right (anatomical)
    double gazeX = 0.0;  ///< the combined gaze (1.0 = 45°)
    double gazeY = 0.0;
};

/**
 * Which eyes' eyelids the eye cameras drive now: frameeyeosc's camera.used while it runs. It changes seldom, so it
 * comes from the status rather than each sample's LidFrom (which is the camera's only on samples where its value
 * replaced the eye server's, not while it merely agrees).
 */
struct CameraLids {
    bool known = false;             ///< frameeyeosc's status is there (else the badge goes by LidFrom)
    bool used[2] = {false, false};  ///< the cameras drive this eye's eyelid (left, right)
};

/** The pupil drawn when none is known (mm). */
constexpr double kDefaultPupilMm = 4.0;
/** The iris's diameter the pupil is drawn against (mm; eyecam-rec assumes the same). */
constexpr double kIrisMm = 11.8;

/**
 * The drawn eyes from one of frameeyeosc's samples. Sent: the eyelid, squint, pupil and gaze as sent, the badge from
 * whether the eye cameras drive that eye's eyelid (lids, as the eyelids card has it; unknown, from the sample's
 * LidFrom: the eye server's usual mix is Valve's, anything else the camera's), and the brows while they are sent. Raw:
 * Valve's openness after the eye's scale (1.0 drawn as relaxed open) and the gaze before smoothing, no squint or pupil
 * (Valve has none). Camera: the eye camera's own eyelid, squint and pupil, and the gaze before smoothing (the camera
 * gives frameeyeosc none); an eye without fresh camera values is not known. Raw and Camera draw no brows: nothing
 * measures them (frameeyeosc makes them from the widening), so there is nothing "as read" to show; the room above the
 * eyes stays, so switching the values doesn't move the eyes.
 * @param packet the sample
 * @param source which values
 * @param lids where the eyelids come from now (Sent's badge)
 * @return the eyes
 */
Shown shown(const gaze_dots::Packet& packet, Source source, const CameraLids& lids = {});

/**
 * How far the upper lid is up between closed (0) and the widest the drawing goes (1): relaxed open (0.75) at
 * kRelaxedOpen, widening above it.
 * @param lid the eyelid (VRCFT scale; kept within 0..1)
 * @return 0..1
 */
double upperLidOpen(double lid);
/** Where relaxed open puts the upper lid (upperLidOpen). */
constexpr double kRelaxedOpen = 0.72;

/**
 * How far the lower lid rises toward the middle of the eye for a squint.
 * @param squint 0..1
 * @return 0 (none) .. kMaxSquintRise
 */
double lowerLidRise(double squint);
/** The most a full squint raises the lower lid (a share of the way to the eye's middle line). */
constexpr double kMaxSquintRise = 0.45;

/**
 * How far a brow is raised above where it rests, as a share of the drawn eye's height: up to kBrowRise.
 * @param brow 0 at rest .. 1 raised all the way (NaN as 0; kept within 0..1)
 * @return 0..kBrowRise
 */
double browLift(double brow);
/** The most a brow rises (a share of the drawn eye's height). */
constexpr double kBrowRise = 0.3;
/** Where a brow's highest point rests, above the top of the drawn eye's box (a share of its height)... */
constexpr double kBrowRest = 0.04;
/** ...how far its ends are below that point, and how thick it is (shares of the height). */
constexpr double kBrowArch = 0.08;
constexpr double kBrowThickness = 0.04;
/** The room a brow raised all the way needs above the drawn eye's box (a share of its height). */
constexpr double kBrowHeadroom = kBrowRest + kBrowRise + kBrowThickness / 2;

/**
 * The pupil's radius in the drawing.
 * @param irisRadius the iris's radius (px)
 * @param mm the pupil's diameter (NaN: kDefaultPupilMm; kept within 1.5..9 mm)
 * @return px
 */
double pupilRadius(double irisRadius, double mm);

/**
 * How far the iris moves from the middle for a gaze, as a share of how far it can go: 35° sideways or 30° up / down
 * is all the way.
 * @param gaze the gaze (1.0 = 45°)
 * @param vertical up / down
 * @return -1..1
 */
double gazeShare(double gaze, bool vertical);

/**
 * Which column of the sheet an eye is drawn in. In a mirror ("Mirror" on) the user's left eye is on the left and the
 * right eye on the right, as they see themselves in a mirror; seen from the front (off: as someone facing them, or the
 * avatar from the front) the right eye is on the left.
 * @param eye 0 left, 1 right (anatomical)
 * @param mirror as in a mirror
 * @return 0 the left column, 1 the right one
 */
int drawnColumn(int eye, bool mirror);

/**
 * Whether the drawn irises move the other way to the gaze sideways: not in a mirror (looking to one's right moves them
 * right), seen from the front they do.
 * @param mirror as in a mirror
 * @return true if turned round
 */
bool gazeTurned(bool mirror);

/**
 * Whether an eye's camera picture is flipped left to right, so its temporal (ear) side is toward the outside of the
 * sheet in both views: eyecam-rec gives both eyes with that side at the left, so the picture in the right column is
 * flipped. (The two views thus show mirror images of each other, as the drawn eyes do.)
 * @param eye 0 left, 1 right (anatomical)
 * @param mirror as in a mirror
 * @return true if flipped
 */
bool pictureFlipped(int eye, bool mirror);

// ---- eyecam-rec's preview file (tools/eyecam/src/preview.rs) ----

/** Its name in eyecam-rec's folder. */
constexpr const char* kFileName = "preview";
/** "EYPV", little-endian. */
constexpr uint32_t kMagic = 0x56505945u;
constexpr uint32_t kVersion = 1;
/** The header, and one eye's header before its picture. */
constexpr size_t kHeaderSize = 64;
constexpr size_t kEyeHeaderSize = 64;
/** The largest picture side the panel takes (the frames are 400 px). */
constexpr int kMaxSide = 400;
/** A picture older than this (s) isn't shown (the cameras stopped, or the headset is off). */
constexpr double kStaleSec = 0.5;
/** "preview on" goes out this often while the sheet shows the pictures (eyecam-rec keeps it on for 3 s). */
constexpr double kRenewSec = 1.0;

/** The lid lines' block, after both pictures: its offset and size are in the header (0 in an older eyecam-rec's). */
constexpr size_t kLidsAtOffset = 52;
constexpr size_t kLidSizeOffset = 56;
/** The bytes of one eye's lid lines the panel reads (the block may be larger). */
constexpr size_t kLidBytes = 48;

/** A lid line eyecam-rec found on a picture: a parabola through its rows at x0, halfway and x1 (picture px). */
struct LidLine {
    bool present = false;           ///< found on this frame (else none is drawn)
    double x0 = NAN;                ///< its first and last column
    double x1 = NAN;
    double y[3] = {NAN, NAN, NAN};  ///< its row at x0, halfway between and at x1
};

/**
 * A lid line's row at a column (the parabola through its three rows).
 * @param line the line (present)
 * @param x the column (picture px)
 * @return the row (picture px)
 */
double lidLineY(const LidLine& line, double x);

/** A picture's lid lines and EyeLid, smoothed over the pictures for the drawing (LidSmoother). */
struct SmoothedLids {
    LidLine upper;
    double margin = NAN;
    LidLine lower;
    double eyeLid = NAN;
};

/** One eye's picture, as eyecam-rec wrote it: upright, the temporal (ear) side at small x. */
struct Picture {
    bool present = false;       ///< a picture was written (its time isn't 0)
    uint64_t timeNs = 0;        ///< the frame's capture time (CLOCK_MONOTONIC ns)
    uint64_t count = 0;         ///< pictures of this eye so far
    int side = 0;               ///< width and height (px)
    std::vector<uint8_t> pixels;  ///< side * side, 8-bit grey, row by row from the top
    bool pupil = false;         ///< the pupil was found...
    double pupilX = NAN;        ///< ...its centre and radius (picture px)
    double pupilY = NAN;
    double pupilR = NAN;
    double window[4] = {NAN, NAN, NAN, NAN};  ///< the pupil search window: left, top, right, bottom (picture px)
    bool closed = false;        ///< eyecam-rec reads the eye closed
    bool lids = false;          ///< the file has the lid lines (an older eyecam-rec's has none)
    LidLine upper;              ///< the upper lid's skin line...
    double margin = NAN;        ///< ...and its margin this far below it (picture px; the opening is measured from it)
    LidLine lower;              ///< the lower lid's line
    double eyeLid = NAN;        ///< the EyeLid eyecam-rec sent for this frame (NaN: its values weren't valid)
    SmoothedLids smoothed;      ///< the same smoothed over the pictures (as read until a LidSmoother takes them)
};

/** Both pictures. */
struct Pictures {
    bool on = false;            ///< eyecam-rec has the preview on (0 once it turned it off)
    uint32_t writerPid = 0;
    uint64_t writerStartNs = 0;
    Picture eyes[2];            ///< left, right (anatomical)
};

/**
 * Read a consistent copy of the file: the magic, the version, the header's and each eye's size, a square picture of at
 * most kMaxSide, and the whole of it within `size`; refused otherwise (another or a broken file). The lid lines are
 * read when the header says where they are and they fit (an older eyecam-rec's file has none: read without them).
 * @param data the bytes
 * @param size how many
 * @param out the pictures
 * @param why why it was refused (English, for the log)
 * @return true if read
 */
bool parsePictures(const uint8_t* data, size_t size, Pictures& out, std::string& why);

/**
 * The file's bytes as eyecam-rec writes them (the other direction, for tests and --fake-preview's checks).
 * @param pictures the pictures (each eye's pixels side * side, the same side for both)
 * @param side their side
 * @return the bytes (a whole number of pages, like the file)
 */
std::vector<uint8_t> encodePictures(const Pictures& pictures, int side);

/** What reading the file came to. */
enum class ReadResult {
    Read,       ///< new pictures (written since the last read)
    Unchanged,  ///< the same as last time
    Missing,    ///< no file (the preview is off, or eyecam-rec doesn't make one)
    Invalid,    ///< not a preview file of this version (see why)
    Busy,       ///< eyecam-rec was writing through every try (read again next time)
};

/**
 * The file mapped read-only (never written). Opened when it is there, and again when it was replaced (another inode)
 * or went away; a copy is taken under its seqlock.
 */
class PictureReader {
public:
    PictureReader() = default;
    ~PictureReader();
    PictureReader(const PictureReader&) = delete;
    PictureReader& operator=(const PictureReader&) = delete;

    /**
     * Read the pictures if they changed.
     * @param path the file
     * @param out where to write them (only on Read)
     * @param why why not (English, for the log)
     * @return what came of it
     */
    ReadResult read(const std::string& path, Pictures& out, std::string& why);

    /** Let go of the file (the sheet closed). */
    void close();

private:
    int fd_ = -1;
    void* map_ = nullptr;
    size_t size_ = 0;
    ino_t inode_ = 0;
    uint32_t lastSeq_ = 1;  ///< odd: nothing read yet
    ino_t badInode_ = 0;    ///< a file found not to be a preview file (let go of; not read again until replaced)
    std::string badWhy_;    ///< ...why
};

/**
 * The lid lines' smoothing (picture px; in Hz): calm at rest (about 0.16 s), the cutoff up 0.05 Hz for each px a second
 * the line moves, so a blink is followed within a picture or two. Tuned on the developer's recordings (the lower line
 * jumps a few px for single pictures; about 60% of its picture-to-picture zigzag goes).
 */
constexpr double kLidMinCutoffHz = 1.0;
constexpr double kLidBeta = 0.05;
constexpr double kLidDCutoffHz = 1.0;
/** The same for the EyeLid on the picture (0..1: about 50 px of lid to the unit). */
constexpr double kEyeLidBeta = 2.5;
/** A gap between pictures longer than this (s) starts the smoothing over (no line swept in from an old place). */
constexpr double kLidResetSec = 0.3;

/**
 * The one-euro filter (Casiez, Roussel and Vogel, 2012) on the pictures' capture times: a low-pass whose cutoff rises
 * with the speed, so a value that jitters in place is damped and one that moves fast (a blink) is followed. No value
 * (NaN), a gap of more than kLidResetSec or a time not after the last starts it over at the new value.
 */
class OneEuro {
public:
    /**
     * @param minCutoff the cutoff at rest (Hz)
     * @param beta how much the cutoff rises with the speed (Hz per unit a second)
     * @param dCutoff the cutoff of the speed itself (Hz)
     */
    explicit OneEuro(double minCutoff = kLidMinCutoffHz, double beta = kLidBeta, double dCutoff = kLidDCutoffHz)
        : minCutoff_(minCutoff), beta_(beta), dCutoff_(dCutoff) {}

    /**
     * The next value.
     * @param value the value (NaN: none, and the filter starts over)
     * @param t its time (s)
     * @return the smoothed value (the value itself when the filter starts over)
     */
    double step(double value, double t);

    /** Start over at the next value. */
    void reset() { started_ = false; }

private:
    double minCutoff_;
    double beta_;
    double dCutoff_;
    bool started_ = false;
    double x_ = 0.0;   ///< the smoothed value...
    double dx_ = 0.0;  ///< ...its smoothed speed (a second)
    double t_ = 0.0;   ///< ...at this time
};

/**
 * Smooths each eye's lid lines and EyeLid over the pictures read (every source but Raw draws them smoothed). Each line
 * starts over where it has none on a picture, so one that comes back starts where it is.
 */
class LidSmoother {
public:
    /**
     * Fill each new picture's `smoothed` (one already taken, by its count and time, is left as it is).
     * @param pictures as just read
     */
    void take(Pictures& pictures);

    /** Start over (the pictures stopped). */
    void reset();

private:
    /** One eye's filters: each line's first and last column and its rows there and halfway, the margin, the EyeLid. */
    struct Eye {
        uint64_t count = 0;
        uint64_t timeNs = 0;
        OneEuro upper[5];
        OneEuro lower[5];
        OneEuro margin;
        OneEuro eyeLid {kLidMinCutoffHz, kEyeLidBeta, kLidDCutoffHz};
    };
    Eye eyes_[2];
};

/**
 * Whether a picture is new enough to show.
 * @param picture the picture
 * @param nowNs CLOCK_MONOTONIC now (ns)
 * @return true while it is at most kStaleSec old
 */
bool pictureFresh(const Picture& picture, uint64_t nowNs);

/** @return CLOCK_MONOTONIC now (ns), the clock the pictures are stamped with */
uint64_t monotonicNs();

/** Whether the camera pictures can show, and why not. */
enum class CameraState {
    Available,   ///< eyecam-rec runs, is set up and in use, and makes pictures
    NotRunning,  ///< eyecam-rec isn't running (no eye cameras tab)
    NotSetUp,    ///< the eye cameras' setup isn't done
    NotInUse,    ///< camera_lids is off ("Don't use" on the Basic tab)
    Old,         ///< an eyecam-rec without the preview (its status.json has no "preview")
    Refused,     ///< eyecam-rec refused "preview on"
};

/**
 * Whether the camera pictures can show.
 * @param view eyecam-rec as read
 * @param cameraLids the camera_lids setting
 * @param refused eyecam-rec refused "preview on" (since the sheet opened)
 * @return the state
 */
CameraState cameraState(const eyecam::View& view, bool cameraLids, bool refused);

/**
 * Whether the camera's own values can be shown ("カメラの値"): the eye cameras are in use.
 * @param state cameraState
 * @return true unless eyecam-rec doesn't run, isn't set up or isn't used
 */
bool cameraValuesAvailable(CameraState state);

/**
 * "preview on" while the pictures are wanted (at once, then every kRenewSec), "preview off" once when they stop being
 * wanted. One command at a time: it waits while another is out.
 */
class Asker {
public:
    /**
     * The command to send now, if any.
     * @param wanted the sheet is open with the camera pictures on (and they can show)
     * @param busy a command is still out
     * @param now monotonic seconds
     * @param command where to write it
     * @return true if there is one (it counts as sent)
     */
    bool next(bool wanted, bool busy, double now, std::string& command);

private:
    bool on_ = false;       ///< "preview on" went out and no "off" since
    double sentAt_ = 0.0;   ///< when it last went out
};

/** What the sheet is drawn from (filled by the loop, or by --fake-preview). */
struct View {
    bool hasPacket = false;      ///< a sample from frameeyeosc came within kPacketStaleSec
    gaze_dots::Packet packet;    ///< ...the newest
    bool hasPictures = false;    ///< the preview file was read
    Pictures pictures;           ///< ...as last read
    uint64_t nowNs = 0;          ///< CLOCK_MONOTONIC when drawn (the pictures' freshness)
    bool refused = false;        ///< eyecam-rec refused "preview on" (or didn't answer it)...
    double refusedAt = 0.0;      ///< ...last at this time (monotonic s)
};

/** Without a sample for this long (s), the drawn eyes say there is no eye data. */
constexpr double kPacketStaleSec = 1.0;

/**
 * A refused "preview on" (an error, or no answer in time) shows why for this long (s); then the pictures' switch can be
 * pressed again (it went off) to ask once more.
 */
constexpr double kRefusedSec = 5.0;

/**
 * Take eyecam-rec's answer to "preview on".
 * @param view the sheet's values (refused, refusedAt)
 * @param ok answered "ok"
 * @param now monotonic seconds
 * @return true if whether it is refused changed (draw again)
 */
bool tookReply(View& view, bool ok, double now);

/**
 * A refusal kRefusedSec old is forgotten (the switch can ask again).
 * @param view the sheet's values
 * @param now monotonic seconds
 * @return true if it was (draw again)
 */
bool refusalOver(View& view, double now);

/**
 * The sheet is drawn for new values or pictures at most this often (s): about 30 a second, the panel's usual pace,
 * also while the debug gaze dots run the loop at every display frame (each sample would draw it 90 times a second). A
 * little under 1/30, so the loop's 33 ms passes each draw it.
 */
constexpr double kDrawSec = 0.03;

/** When the sheet is drawn for what came in: at most every kDrawSec, and what came last is never left undrawn. */
class DrawPacer {
public:
    /** New values or pictures came in. */
    void changed() { pending_ = true; }

    /**
     * Whether to draw for them now.
     * @param now monotonic seconds
     * @return true if something came in and kDrawSec has passed since the last draw
     */
    bool due(double now) const { return pending_ && now - drawnAt_ >= kDrawSec; }

    /**
     * The panel was drawn (for this or anything else): what came in is on it.
     * @param now monotonic seconds
     */
    void drawn(double now) {
        pending_ = false;
        drawnAt_ = now;
    }

private:
    bool pending_ = false;
    double drawnAt_ = -1e9;
};

/**
 * Made-up values and pictures for --fake-preview and the tests: both eyes open, the left one a little widened and from
 * the camera, the right one squinting, looking a little right and down; grey pictures with a dark disc where the pupil
 * would be (not an eye: no real eye picture is ever used). No brows (see fakeBrows).
 * @param view where to write them
 * @param nowNs the pictures' time
 */
void fake(View& view, uint64_t nowNs);

/**
 * Made-up brows on the made-up values (--fake-preview-brows): brows are sent, both at `level`.
 * @param view the values (from fake)
 * @param level 0 at rest .. 1 raised all the way
 */
void fakeBrows(View& view, double level);

}  // namespace eye_preview
