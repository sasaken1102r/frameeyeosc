// Implementation of the shared drawing helpers.
#include "draw.h"

#include "icons.h"

#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

/**
 * Cleanup callback cairo calls to close the FT_Face once it's no longer needed.
 * @param face the FT_Face
 */
void destroyFtFace(void* face) {
    FT_Done_Face(static_cast<FT_Face>(face));
}

const cairo_user_data_key_t kFtFaceKey {};

}  // namespace

FontSet::FontSet() {
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) == 0) ftLibrary_ = library;
}

FontSet::~FontSet() {
    release();
    // Don't close FT_Library here: the FT_Face must stay alive as long as cairo holds it
    // (the OS will clean it up on exit).
}

cairo_font_face_t* FontSet::createFace(const std::string& path, bool bold) {
    FT_Face face = nullptr;
    if (ftLibrary_ != nullptr && FT_New_Face(static_cast<FT_Library>(ftLibrary_), path.c_str(), 0, &face) == 0) {
        cairo_font_face_t* cairoFace = cairo_ft_font_face_create_for_ft_face(face, 0);
        if (cairo_font_face_set_user_data(cairoFace, &kFtFaceKey, face, destroyFtFace) == CAIRO_STATUS_SUCCESS) {
            return cairoFace;
        }
        cairo_font_face_destroy(cairoFace);
        FT_Done_Face(face);
    }
    std::fprintf(stderr, "[draw] can't read font %s, falling back to Noto Sans CJK JP\n", path.c_str());
    return cairo_toy_font_face_create("Noto Sans CJK JP", CAIRO_FONT_SLANT_NORMAL,
                                      bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
}

void FontSet::release() {
    // If a cairo_t still holds a reference, the font stays alive until that reference is dropped
    if (regular_ != nullptr) cairo_font_face_destroy(regular_);
    if (bold_ != nullptr) cairo_font_face_destroy(bold_);
    regular_ = nullptr;
    bold_ = nullptr;
}

void FontSet::load(const std::string& regularPath, const std::string& boldPath) {
    if (regular_ != nullptr && regularPath == regularPath_ && boldPath == boldPath_) return;
    release();
    regular_ = createFace(regularPath, false);
    bold_ = createFace(boldPath, true);
    regularPath_ = regularPath;
    boldPath_ = boldPath;
}

namespace {

/**
 * The stroke width of an icon at a text size: about the font's stem, thicker for bold.
 * @param size text size (px)
 * @param bold whether bold
 * @return the width (px)
 */
double iconStroke(double size, bool bold) {
    return std::max(1.1, size * (bold ? 0.115 : 0.075));
}

/**
 * Where an icon's middle sits. Arrows and the small marks: halfway between the middle of the x-height and the middle
 * of the capitals (about 0.32 em up), so they line up with lowercase, capitals and kana alike. The full-width marks
 * (◯ and △, which stand among kana like a character): the middle of the capitals (about 0.37 em). Read from the font
 * in use.
 * @param cr cairo, with the text's font and size set
 * @param which the icon
 * @param baseline the text baseline
 * @param size text size (px)
 * @return the center y
 */
double iconCenterY(cairo_t* cr, icon::Icon which, double baseline, double size) {
    cairo_text_extents_t x;
    cairo_text_extents_t cap;
    cairo_text_extents(cr, "x", &x);
    cairo_text_extents(cr, "H", &cap);
    const double xHeight = x.height > 0 ? -x.y_bearing : size * 0.54;
    const double capHeight = cap.height > 0 ? -cap.y_bearing : size * 0.73;
    if (which == icon::Icon::Ring || which == icon::Icon::TriangleOutline) return baseline - capHeight / 2;
    return baseline - (xHeight + capHeight) / 4;
}

/**
 * Draw one icon in its box on a text line (the current source color).
 * @param cr cairo, with the text's font and size set
 * @param which the icon
 * @param x the box's left edge
 * @param baseline the text baseline
 * @param size text size (px)
 * @param bold whether the text is bold (thicker lines)
 */
void drawIcon(cairo_t* cr, icon::Icon which, double x, double baseline, double size, bool bold) {
    using icon::Icon;
    const double cx = x + icon::advance(which, size) / 2;
    const double cy = iconCenterY(cr, which, baseline, size);
    const double s = size;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_line_width(cr, iconStroke(size, bold));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    // A filled shape with its corners rounded by a thin stroke of the same color
    const auto fillShape = [&]() {
        cairo_close_path(cr);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, s * 0.06);
        cairo_stroke(cr);
    };
    switch (which) {
        case Icon::ChevronLeft:
        case Icon::ChevronRight: {
            const double dx = (which == Icon::ChevronRight ? 1 : -1) * s * 0.13;
            cairo_move_to(cr, cx - dx, cy - s * 0.26);
            cairo_line_to(cr, cx + dx, cy);
            cairo_line_to(cr, cx - dx, cy + s * 0.26);
            cairo_stroke(cr);
            break;
        }
        case Icon::ChevronUp:
        case Icon::ChevronDown: {
            const double dy = (which == Icon::ChevronDown ? 1 : -1) * s * 0.13;
            cairo_move_to(cr, cx - s * 0.26, cy - dy);
            cairo_line_to(cr, cx, cy + dy);
            cairo_line_to(cr, cx + s * 0.26, cy - dy);
            cairo_stroke(cr);
            break;
        }
        case Icon::TriangleUp:
        case Icon::TriangleDown: {
            const double dy = (which == Icon::TriangleDown ? 1 : -1) * s * 0.27;
            cairo_move_to(cr, cx - s * 0.33, cy - dy);
            cairo_line_to(cr, cx + s * 0.33, cy - dy);
            cairo_line_to(cr, cx, cy + dy);
            fillShape();
            break;
        }
        case Icon::ArrowRight: {
            const double left = cx - s * 0.38;
            const double right = cx + s * 0.38;
            const double head = s * 0.21;
            cairo_move_to(cr, left, cy);
            cairo_line_to(cr, right, cy);
            cairo_move_to(cr, right - head, cy - head);
            cairo_line_to(cr, right, cy);
            cairo_line_to(cr, right - head, cy + head);
            cairo_stroke(cr);
            break;
        }
        case Icon::Play:
            cairo_move_to(cr, cx - s * 0.18, cy - s * 0.26);
            cairo_line_to(cr, cx + s * 0.24, cy);
            cairo_line_to(cr, cx - s * 0.18, cy + s * 0.26);
            fillShape();
            break;
        case Icon::Check:
            cairo_move_to(cr, cx - s * 0.27, cy + s * 0.01);
            cairo_line_to(cr, cx - s * 0.08, cy + s * 0.21);
            cairo_line_to(cr, cx + s * 0.28, cy - s * 0.20);
            cairo_stroke(cr);
            break;
        case Icon::Cross: {
            const double r = s * 0.21;
            cairo_move_to(cr, cx - r, cy - r);
            cairo_line_to(cr, cx + r, cy + r);
            cairo_move_to(cr, cx + r, cy - r);
            cairo_line_to(cr, cx - r, cy + r);
            cairo_stroke(cr);
            break;
        }
        case Icon::Plus:
        case Icon::Minus:
            cairo_move_to(cr, cx - s * 0.25, cy);
            cairo_line_to(cr, cx + s * 0.25, cy);
            if (which == Icon::Plus) {
                cairo_move_to(cr, cx, cy - s * 0.25);
                cairo_line_to(cr, cx, cy + s * 0.25);
            }
            cairo_stroke(cr);
            break;
        case Icon::Note: {
            // An eighth note: a tilted head on the baseline, a stem up to about the capitals, and a flag
            const double headX = cx - s * 0.16;
            const double headY = baseline - s * 0.12;
            cairo_save(cr);
            cairo_translate(cr, headX, headY);
            cairo_rotate(cr, -0.38);
            cairo_scale(cr, s * 0.165, s * 0.118);
            cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
            cairo_restore(cr);
            cairo_fill(cr);
            const double stemX = headX + s * 0.14;
            const double top = baseline - s * 0.78;
            cairo_move_to(cr, stemX, headY - s * 0.02);
            cairo_line_to(cr, stemX, top);
            cairo_curve_to(cr, stemX + s * 0.05, top + s * 0.17, stemX + s * 0.25, top + s * 0.20, stemX + s * 0.17,
                           top + s * 0.42);
            cairo_stroke(cr);
            break;
        }
        case Icon::Dot:
            cairo_arc(cr, cx, cy, s * 0.14, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
        case Icon::Ring:
            cairo_arc(cr, cx, cy, s * 0.44, 0, 2 * M_PI);
            cairo_stroke(cr);
            break;
        case Icon::TriangleOutline: {
            const double h = s * 0.76;
            const double bottom = cy + h * 0.5;
            cairo_move_to(cr, cx, bottom - h);
            cairo_line_to(cr, cx + s * 0.44, bottom);
            cairo_line_to(cr, cx - s * 0.44, bottom);
            cairo_close_path(cr);
            cairo_stroke(cr);
            break;
        }
        case Icon::None: break;
    }
    cairo_restore(cr);
}

}  // namespace

double Pen::measure(const std::string& text, double size, bool isBold) const {
    cairo_set_font_face(cr, isBold ? fonts->bold() : fonts->regular());
    cairo_set_font_size(cr, size);
    cairo_text_extents_t extents;
    if (!icon::any(text)) {
        cairo_text_extents(cr, text.c_str(), &extents);
        return extents.x_advance;
    }
    // The runs of text between the icons, and the icons' own widths
    double width = 0;
    std::string run;
    for (size_t pos = 0; pos < text.size();) {
        const icon::Icon which = icon::at(text, pos);
        if (which == icon::Icon::None) {
            run += text[pos++];
            continue;
        }
        if (!run.empty()) {
            cairo_text_extents(cr, run.c_str(), &extents);
            width += extents.x_advance;
            run.clear();
        }
        width += icon::advance(which, size);
        pos += icon::kMarkerBytes;
    }
    if (!run.empty()) {
        cairo_text_extents(cr, run.c_str(), &extents);
        width += extents.x_advance;
    }
    return width;
}

double Pen::text(double x, double y, const std::string& text, double size, Color c, bool isBold,
                 bool alignRight) const {
    color(c);
    // Right-aligned text starts its width to the left; otherwise the width is read from the current point after
    // drawing (so the text is laid out only once)
    if (alignRight) x -= measure(text, size, isBold);
    cairo_set_font_face(cr, isBold ? fonts->bold() : fonts->regular());
    cairo_set_font_size(cr, size);
    cairo_move_to(cr, x, y);
    if (!icon::any(text)) {
        cairo_show_text(cr, text.c_str());
    } else {
        // The runs as text, each icon drawn in its own box between them
        double penX = x;
        std::string run;
        const auto flush = [&]() {
            if (run.empty()) return;
            cairo_move_to(cr, penX, y);
            cairo_show_text(cr, run.c_str());
            double endY = y;
            cairo_get_current_point(cr, &penX, &endY);
            run.clear();
        };
        for (size_t pos = 0; pos < text.size();) {
            const icon::Icon which = icon::at(text, pos);
            if (which == icon::Icon::None) {
                run += text[pos++];
                continue;
            }
            flush();
            drawIcon(cr, which, penX, y, size, isBold);
            penX += icon::advance(which, size);
            pos += icon::kMarkerBytes;
        }
        flush();
        cairo_move_to(cr, penX, y);
    }
    double endX = x;
    double endY = y;
    cairo_get_current_point(cr, &endX, &endY);
    return endX - x;
}

void Pen::roundedRect(double x, double y, double w, double h, double r) const {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void Pen::dot(double x, double y, Color c) const {
    color(c);
    cairo_arc(cr, x, y, 4.0, 0, 2 * M_PI);
    cairo_fill(cr);
}

void surfaceToRgba(cairo_surface_t* surface, std::vector<uint8_t>& out) {
    // Precompute the un-premultiply math (c * 255 / a) as a lookup table so we don't divide per pixel
    static const std::vector<uint8_t> unpremultiply = [] {
        std::vector<uint8_t> table(256 * 256);
        for (uint32_t a = 0; a < 256; ++a) {
            for (uint32_t c = 0; c < 256; ++c) {
                const uint32_t value = a == 0 ? 0 : std::min<uint32_t>(255, (c * 255 + a / 2) / a);
                table[a * 256 + c] = static_cast<uint8_t>(value);
            }
        }
        return table;
    }();

    cairo_surface_flush(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const uint8_t* data = cairo_image_surface_get_data(surface);
    out.resize(static_cast<size_t>(width) * height * 4);
    uint8_t* dst = out.data();
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(data + static_cast<size_t>(y) * stride);
        for (int x = 0; x < width; ++x) {
            // cairo stores premultiplied ARGB (native-endian 32-bit); un-premultiply and reorder to RGBA
            const uint32_t p = row[x];
            const uint32_t a = p >> 24;
            const uint8_t* line = unpremultiply.data() + a * 256;
            dst[0] = line[(p >> 16) & 0xFF];
            dst[1] = line[(p >> 8) & 0xFF];
            dst[2] = line[p & 0xFF];
            dst[3] = static_cast<uint8_t>(a);
            dst += 4;
        }
    }
}
