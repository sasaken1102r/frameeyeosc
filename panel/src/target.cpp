// Drawing the eye fit's target.
#include "target.h"

#include "draw.h"
#include "theme.h"

#include <algorithm>
#include <cmath>

namespace {

/**
 * Draw text centered on a point's x, shrinking it to fit a width.
 * @param pen drawing tools
 * @param cx center x
 * @param baseline text baseline
 * @param text the text
 * @param size the size to start from
 * @param maxWidth the width to fit
 * @param c the color
 */
void centeredText(const Pen& pen, double cx, double baseline, const std::string& text, double size, double maxWidth,
                  Color c) {
    while (size > 10 && pen.measure(text, size, true) > maxWidth) size -= 1;
    const double w = pen.measure(text, size, true);
    pen.text(cx - w / 2, baseline, text, size, c, true);
}

/**
 * Draw the target on a clear image of kTargetImageSize.
 * @param surface the image
 * @param fonts the fonts
 * @param style as renderTarget
 * @param label as renderTarget
 * @param seconds as renderTarget
 * @param progress as renderTarget
 */
void drawTarget(cairo_surface_t* surface, const FontSet& fonts, gaze_fit::TargetStyle style, const std::string& label,
                int seconds, double progress) {
    using gaze_fit::TargetStyle;
    const int size = kTargetImageSize;
    cairo_t* cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    const Pen pen {cr, &fonts};
    const double c = size / 2.0;

    // A dark disc behind everything keeps the target readable over bright scenes
    pen.color(kBg, 0.72);
    cairo_arc(cr, c, c, size * 0.48, 0, 2 * M_PI);
    cairo_fill(cr);

    // The ring: a faint full circle, and the part left in the accent color, running down clockwise from the top
    if (style != TargetStyle::OpenEyes) {
        const double ring = size * 0.36;
        cairo_set_line_width(cr, size * 0.045);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        pen.color(kBorder, 0.55);
        cairo_arc(cr, c, c, ring, 0, 2 * M_PI);
        cairo_stroke(cr);
        const double left = std::clamp(progress, 0.0, 1.0);
        if (left > 0.001) {
            pen.color(kAccent);
            cairo_arc(cr, c, c, ring, -M_PI / 2, -M_PI / 2 + left * 2 * M_PI);
            cairo_stroke(cr);
        }
    }

    if (style == TargetStyle::Dot) {
        // The dot to look at: light, with a dark edge so it shows on light and dark backgrounds
        pen.color(kBg);
        cairo_arc(cr, c, c, size * 0.06, 0, 2 * M_PI);
        cairo_fill(cr);
        pen.color(kText);
        cairo_arc(cr, c, c, size * 0.042, 0, 2 * M_PI);
        cairo_fill(cr);
        // The seconds left, small and muted under the dot so they don't pull the eyes away
        if (seconds > 0) centeredText(pen, c, c + size * 0.22, std::to_string(seconds), size * 0.12, size, kTextMuted);
    } else {
        // The eyes-shut step: its words (one line, or two a little smaller), and while counting down to closing, a
        // large number
        const bool counting = style == TargetStyle::CloseEyes && seconds > 0;
        const size_t newline = label.find('\n');
        const bool twoLines = newline != std::string::npos;
        double textSize = size * (twoLines ? 0.1 : 0.12);
        const double last = counting ? c - size * (twoLines ? 0.02 : 0.08) : c + size * (twoLines ? 0.1 : 0.045);
        if (twoLines) {
            // Both lines the same size, the upper one narrower inside the ring
            const std::string first = label.substr(0, newline);
            const std::string second = label.substr(newline + 1);
            while (textSize > 10 && (pen.measure(first, textSize, true) > size * 0.6 ||
                                     pen.measure(second, textSize, true) > size * 0.64)) {
                textSize -= 1;
            }
            centeredText(pen, c, last - textSize * 1.25, first, textSize, size, kText);
            centeredText(pen, c, last, second, textSize, size, kText);
        } else {
            centeredText(pen, c, last, label, textSize, size * 0.6, kText);
        }
        if (counting) centeredText(pen, c, c + size * 0.21, std::to_string(seconds), size * 0.18, size * 0.5, kAccent);
    }

    cairo_surface_flush(surface);
    cairo_destroy(cr);
}

}  // namespace

void renderTarget(const FontSet& fonts, gaze_fit::TargetStyle style, const std::string& label, int seconds,
                  double progress, std::vector<uint8_t>& rgba, const std::string& pngPath) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kTargetImageSize, kTargetImageSize);
    drawTarget(surface, fonts, style, label, seconds, progress);
    surfaceToRgba(surface, rgba);
    if (!pngPath.empty()) cairo_surface_write_to_png(surface, pngPath.c_str());
    cairo_surface_destroy(surface);
}

TargetPainter::~TargetPainter() {
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
}

bool TargetPainter::paint(const FontSet& fonts, gaze_fit::TargetStyle style, const std::string& label, int seconds,
                          double progress) {
    const long ring = std::lround(std::clamp(progress, 0.0, 1.0) * kRingSteps);
    const std::string key = std::to_string(static_cast<int>(style)) + "|" + std::to_string(seconds) + "|" +
                            std::to_string(ring) + "|" + label;
    if (key == key_) return false;
    if (surface_ == nullptr) {
        surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kTargetImageSize, kTargetImageSize);
    }
    drawTarget(surface_, fonts, style, label, seconds, static_cast<double>(ring) / kRingSteps);
    surfaceToRgba(surface_, rgba_);
    key_ = key;
    return true;
}

void renderFill(const FontSet& fonts, bool bright, const std::string& label, std::vector<uint8_t>& rgba,
                const std::string& pngPath) {
    const int size = kFillImageSize;
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* cr = cairo_create(surface);
    const Pen pen {cr, &fonts};
    pen.color(bright ? kFillBright : kFillDark);
    cairo_paint(cr);
    // The instruction, about 5 degrees tall in the middle of the view
    centeredText(pen, size / 2.0, size / 2.0 + 14, label, 40, size * 0.5, bright ? kFillBrightText : kFillDarkText);
    cairo_surface_flush(surface);
    surfaceToRgba(surface, rgba);
    if (!pngPath.empty()) cairo_surface_write_to_png(surface, pngPath.c_str());
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

void renderGazeDot(DotKind kind, std::vector<uint8_t>& rgba, const std::string& pngPath) {
    const int size = kDotImageSize;
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* cr = cairo_create(surface);
    const double c = size / 2.0;
    const Color fill = kind == DotKind::Left ? kDotLeft : (kind == DotKind::Right ? kDotRight : kDotBoth);
    cairo_set_source_rgb(cr, kBg.r, kBg.g, kBg.b);
    cairo_arc(cr, c, c, size * 0.46, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, fill.r, fill.g, fill.b);
    cairo_arc(cr, c, c, size * 0.34, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_surface_flush(surface);
    surfaceToRgba(surface, rgba);
    if (!pngPath.empty()) cairo_surface_write_to_png(surface, pngPath.c_str());
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}
