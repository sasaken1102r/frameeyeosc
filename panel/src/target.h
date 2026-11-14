// The eye fit's target: a small dot to look at, a ring that runs down while the step is measured, and the seconds
// left; for the eyes-shut step, words and a countdown instead of the dot. It is shown as its own overlay fixed to
// the head (see VrOverlay::showTarget), never on the dashboard.
#pragma once

#include "gaze_fit.h"

#include <cairo.h>

#include <cstdint>
#include <string>
#include <vector>

class FontSet;

/** The target image's edge length (px). */
constexpr int kTargetImageSize = 256;
/** The ring is drawn in this many steps (half a degree each), so an unchanged picture is not drawn again. */
constexpr int kRingSteps = 720;

/**
 * Draw the target.
 * @param fonts the fonts
 * @param style dot, or one of the eyes-shut step's looks
 * @param label the words for the eyes-shut step ("Close your eyes"; a "\n" starts a second line); unused for the dot
 * @param seconds the seconds left (0 = none): small under the dot, large while counting down to closing the eyes
 * @param progress how much of the ring is left (0..1)
 * @param rgba where to write un-premultiplied RGBA (kTargetImageSize squared)
 * @param pngPath also save a PNG here if not empty
 */
void renderTarget(const FontSet& fonts, gaze_fit::TargetStyle style, const std::string& label, int seconds,
                  double progress, std::vector<uint8_t>& rgba, const std::string& pngPath = "");

/**
 * Draws the target for the VR loop: only when the picture changes (the ring in kRingSteps steps), on one image kept
 * for the whole run.
 */
class TargetPainter {
public:
    TargetPainter() = default;
    ~TargetPainter();
    TargetPainter(const TargetPainter&) = delete;
    TargetPainter& operator=(const TargetPainter&) = delete;

    /**
     * Draw the target if it looks different from last time.
     * @param fonts the fonts
     * @param style as renderTarget
     * @param label as renderTarget
     * @param seconds as renderTarget
     * @param progress as renderTarget
     * @return true if it was drawn (then rgba() holds the new picture)
     */
    bool paint(const FontSet& fonts, gaze_fit::TargetStyle style, const std::string& label, int seconds,
               double progress);

    /** @return the picture, non-premultiplied RGBA (kTargetImageSize squared) */
    const std::vector<uint8_t>& rgba() const { return rgba_; }

    /** Forget the last picture, so the next paint draws (after the image could not be sent). */
    void reset() { key_.clear(); }

private:
    cairo_surface_t* surface_ = nullptr;
    std::string key_;             ///< what the picture shows
    std::vector<uint8_t> rgba_;
};

/** The eye capture's full-view overlay image's edge length (px; about 8 px a degree as it is shown). */
constexpr int kFillImageSize = 1024;

/**
 * Draw the eye capture's full-view overlay for a bright or dark step: all white or all black, with the instruction
 * faintly in the middle so the user knows what is going on (see VrOverlay::showFill).
 * @param fonts the fonts
 * @param bright white (else black)
 * @param label the instruction
 * @param rgba where to write un-premultiplied RGBA (kFillImageSize squared)
 * @param pngPath also save a PNG here if not empty
 */
void renderFill(const FontSet& fonts, bool bright, const std::string& label, std::vector<uint8_t>& rgba,
                const std::string& pngPath = "");

/** The slip test's caption image (px): about 40 px a degree as it is shown (VrOverlay::showCaption). */
constexpr int kCaptionImageW = 1024;
constexpr int kCaptionImageH = 160;
/** Its bar is drawn in this many steps, so an unchanged picture is not drawn again. */
constexpr int kCaptionBarSteps = 200;

/**
 * Draw the slip test's caption under the dot: what to do now in large type on a dark band, and a bar under it that
 * runs down with the step's time, the seconds left at its end.
 * @param fonts the fonts
 * @param text what to do
 * @param progress how much of the step is left (0..1)
 * @param seconds whole seconds left
 * @param rgba where to write un-premultiplied RGBA (kCaptionImageW x kCaptionImageH)
 * @param pngPath also save a PNG here if not empty
 */
void renderCaption(const FontSet& fonts, const std::string& text, double progress, int seconds,
                   std::vector<uint8_t>& rgba, const std::string& pngPath = "");

/** Draws the caption for the VR loop: only when the picture changes (the bar in kCaptionBarSteps steps). */
class CaptionPainter {
public:
    CaptionPainter() = default;
    ~CaptionPainter();
    CaptionPainter(const CaptionPainter&) = delete;
    CaptionPainter& operator=(const CaptionPainter&) = delete;

    /**
     * Draw the caption if it looks different from last time.
     * @param fonts the fonts
     * @param text as renderCaption
     * @param progress as renderCaption
     * @param seconds as renderCaption
     * @return true if it was drawn (then rgba() holds the new picture)
     */
    bool paint(const FontSet& fonts, const std::string& text, double progress, int seconds);

    /** @return the picture, non-premultiplied RGBA */
    const std::vector<uint8_t>& rgba() const { return rgba_; }

    /** Forget the last picture, so the next paint draws. */
    void reset() { key_.clear(); }

private:
    cairo_surface_t* surface_ = nullptr;
    std::string key_;
    std::vector<uint8_t> rgba_;
};

/** The debug gaze dot image's edge length (px). */
constexpr int kDotImageSize = 64;

/** Which debug gaze dot (Other: the gaze source not in use, gaze_debug_dots_compare; a dot only, no ring). */
enum class DotKind { Both, Left, Right, Other };

/**
 * Draw a debug gaze dot: a filled circle with a dark edge (combined: light gray, left eye: light cyan, right eye:
 * darker orange); the other gaze source's a violet diamond with a dark edge, told apart by its shape too.
 * @param kind which dot
 * @param rgba where to write un-premultiplied RGBA (kDotImageSize squared)
 * @param pngPath also save a PNG here if not empty
 */
void renderGazeDot(DotKind kind, std::vector<uint8_t>& rgba, const std::string& pngPath = "");

/**
 * Draw a debug gaze ring (the gaze before the user's gaze scale): a ring of the dot's colour with dark edges and a clear
 * middle, shown gaze_dots::kRingWidthScale times as wide as a dot, so a dot at the same place sits inside it.
 * @param kind which eye's colour
 * @param rgba where to write un-premultiplied RGBA (kDotImageSize squared)
 * @param pngPath also save a PNG here if not empty
 */
void renderGazeRing(DotKind kind, std::vector<uint8_t>& rgba, const std::string& pngPath = "");
