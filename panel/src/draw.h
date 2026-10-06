// Shared drawing helpers (colors, fonts, text/rounded-rect drawing, OpenVR pixel conversion).
#pragma once

#include <cairo.h>

#include <cstdint>
#include <string>
#include <vector>

/** An RGB color (0-1). */
struct Color {
    double r, g, b;
};

// Color constants live in theme.h (so contrast-ratio checks use the same definitions)

/**
 * The two fonts used: regular and bold. Loaded once and reused.
 */
class FontSet {
public:
    FontSet();
    ~FontSet();
    FontSet(const FontSet&) = delete;
    FontSet& operator=(const FontSet&) = delete;

    /**
     * Load fonts (does nothing if the paths are unchanged from last time). Falls back to
     * fontconfig's "Noto Sans CJK JP" if the files can't be read.
     * @param regularPath path to the regular-weight font (for a .ttc, face 0 = Japanese)
     * @param boldPath path to the bold-weight font
     */
    void load(const std::string& regularPath, const std::string& boldPath);

    /** @return the regular-weight font */
    cairo_font_face_t* regular() const { return regular_; }
    /** @return the bold-weight font */
    cairo_font_face_t* bold() const { return bold_; }

private:
    void* ftLibrary_ = nullptr;  ///< FT_Library
    cairo_font_face_t* regular_ = nullptr;
    cairo_font_face_t* bold_ = nullptr;
    std::string regularPath_;
    std::string boldPath_;

    /**
     * Build a cairo font from a font file.
     * @param path the font file
     * @param bold whether to bold the fallback font
     * @return the created font
     */
    cairo_font_face_t* createFace(const std::string& path, bool bold);

    /** Release the loaded fonts. */
    void release();
};

/**
 * The tools used while drawing (cairo context and fonts), bundled together.
 */
struct Pen {
    cairo_t* cr;
    const FontSet* fonts;

    /**
     * Set the drawing color.
     * @param c the color
     * @param alpha opacity
     */
    void color(Color c, double alpha = 1.0) const { cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha); }

    /**
     * Measure the width of a string. An icon marker (icons.h) counts as its icon's width.
     * @param text UTF-8 string
     * @param size font size (px)
     * @param isBold whether to use bold
     * @return width (px)
     */
    double measure(const std::string& text, double size, bool isBold = false) const;

    /**
     * Draw a string. An icon marker (icons.h) is drawn as its icon with paths, in the same color and with lines as
     * thick as the font's.
     * @param x left edge (right edge if alignRight)
     * @param y baseline
     * @param text UTF-8 string
     * @param size font size (px)
     * @param c color
     * @param isBold whether to use bold
     * @param alignRight whether to right-align
     * @return width of the drawn string (px)
     */
    double text(double x, double y, const std::string& text, double size, Color c, bool isBold = false,
                bool alignRight = false) const;

    /**
     * Build the path for a rounded rectangle.
     * @param x left
     * @param y top
     * @param w width
     * @param h height
     * @param r corner radius
     */
    void roundedRect(double x, double y, double w, double h, double r) const;

    /**
     * Draw a small filled dot (for legends).
     * @param x center x
     * @param y center y
     * @param c color
     */
    void dot(double x, double y, Color c) const;
};

/**
 * Convert a cairo image (premultiplied ARGB) to non-premultiplied RGBA for OpenVR.
 * @param surface the cairo image (CAIRO_FORMAT_ARGB32)
 * @param out destination buffer (sized to width * height * 4 bytes)
 */
void surfaceToRgba(cairo_surface_t* surface, std::vector<uint8_t>& out);
