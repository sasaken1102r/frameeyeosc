// The panel. Colors only come from theme.h (in step with the --contrast-report pairs).
#include "panel.h"

#include "diag.h"
#include "draw.h"
#include "fit_text.h"
#include "frame_apps_cairo.h"
#include "host_entry.h"
#include "icons.h"
#include "recorder.h"
#include "report.h"
#include "setup_tools.h"
#include "text_layout.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <ctime>

namespace {

// Left: the status column (always visible). Right: tabs over a content card.
constexpr int kWidth = 1200;
constexpr int kHeight = 700;
constexpr double kPad = 24;
constexpr double kStatusX = kPad;
constexpr double kStatusY = kPad;
constexpr double kStatusW = 372;
constexpr double kStatusH = kHeight - kPad * 2;
constexpr double kRightX = 416;
constexpr double kRight = kWidth - kPad;
constexpr double kTabY = kPad;
constexpr double kTabH = 56;
constexpr double kContentY = 96;
constexpr double kContentH = kHeight - kPad - kContentY;
constexpr double kInnerX = kRightX + 22;
constexpr double kInnerRight = kRight - 22;
constexpr double kLabelW = 206;
constexpr double kControlX = kInnerX + kLabelW + 14;
constexpr double kControlW = kInnerRight - kControlX;
constexpr double kRowTop = 116;
/** A relaxed open eye, as sent (VRCFT): the sent eyelid bars' dotted line, and where ③ is off. */
constexpr double kLidRelaxed = 0.75;
/** How strongly "Valve eyelids"' part is drawn while it only stands by for the cameras (still to be changed). */
constexpr double kStandbyAlpha = 0.6;
/** Below this many samples a second from the eye tracker, the left column marks the rate as low. */
constexpr double kRowH = 64;
constexpr double kRowGap = 2;
constexpr double kCaptionRowH = 84;
constexpr double kControlH = 52;
/** The raw openness range drawn in the lid mark bars. */
constexpr double kLidScaleMax = 1.2;
// The version history (in place of the Advanced tab): a title row, then the rows, scrolled within the card
constexpr double kHistoryTitleY = kContentY + 14;  ///< the title row's top
constexpr double kHistoryTitleH = 48;
constexpr double kHistoryViewTop = kHistoryTitleY + kHistoryTitleH + 18;  ///< the scrolled area, below the title row
constexpr double kHistoryViewBottom = kContentY + kContentH - 16;
constexpr double kHistoryBarW = 44;      ///< the ▲ / ▼ column at the right edge
constexpr double kHistoryHeaderH = 56;   ///< a row's header (the whole row while it is folded)
constexpr double kHistoryRowGap = 8;
constexpr double kHistoryTextSize = 16;  ///< the summary and items (the hints are 15)
constexpr double kHistoryLineStep = 23;
constexpr double kAdvViewBottom = kContentY + kContentH - 12;  ///< the Advanced tab's page is shown above this...
constexpr double kSubTabY = kContentY + 16;                   ///< ...under its sub-tabs
constexpr double kSubTabH = 48;
constexpr double kAdvPageTop = kSubTabY + kSubTabH + 14;      ///< ...from here
constexpr double kAppsRowH = 72;          ///< the row at the bottom of "Version" that opens the other apps' list
constexpr double kAppsHeadY = kContentY + 20;                  ///< the list's title row (and its cross)
constexpr double kAppsFootH = 52;                              ///< its bottom row: the note and "Open the installer"
constexpr double kAppsFootY = kContentY + kContentH - 20 - kAppsFootH;
constexpr double kAppsViewTop = kAppsHeadY + 56 + 14;          ///< the apps, scrolled between these
constexpr double kAppsViewBottom = kAppsFootY - 14;
constexpr double kAppsLineH = 72;         ///< an app in the list
constexpr double kAppsLineGap = 8;
constexpr double kExtraRowH = 72;         ///< "Other apps": a row of the list
constexpr double kExtraRowGap = 8;
constexpr double kExtrasFootH = 48;       ///< ...the flow's bottom row (Cancel / Back, Next / Add / Save)
constexpr double kExtrasFootY = kContentY + kContentH - 16 - kExtrasFootH;
constexpr double kDestStep = 21;          ///< the left column's lines for more destinations

/**
 * The largest text size (down to a minimum) that fits a width.
 * @param pen drawing tools
 * @param text the text
 * @param size the size to start from
 * @param minSize never smaller than this
 * @param maxWidth the width to fit (px)
 * @param bold whether bold
 * @return the size
 */
double fitSize(const Pen& pen, const std::string& text, double size, double minSize, double maxWidth, bool bold) {
    while (size > minSize && pen.measure(text, size, bold) > maxWidth) size -= 1;
    return size;
}

/**
 * The baseline that centers text vertically in a band.
 * @param top band top
 * @param h band height
 * @param size text size
 * @return the baseline y
 */
double centerBaseline(double top, double h, double size) {
    return top + h / 2 + size * 0.36;
}

/**
 * Draw text centered horizontally.
 * @param pen drawing tools
 * @param cx center x
 * @param baseline baseline
 * @param text the text
 * @param size text size
 * @param c color
 * @param bold whether bold
 */
void textCentered(const Pen& pen, double cx, double baseline, const std::string& text, double size, Color c,
                  bool bold) {
    pen.text(cx - pen.measure(text, size, bold) / 2, baseline, text, size, c, bold);
}

/**
 * Draw a card: stacked shadow, fill, border and a 1 px inner highlight on the top edge.
 * @param pen drawing tools
 * @param x left
 * @param y top
 * @param w width
 * @param h height
 * @param r corner radius
 * @param fill fill color
 * @param border border color
 * @param borderWidth border width (0 = none)
 */
void drawCard(const Pen& pen, double x, double y, double w, double h, double r, Color fill, Color border,
              double borderWidth) {
    cairo_t* cr = pen.cr;
    cairo_set_source_rgba(cr, 0, 0, 0, 0.22);
    pen.roundedRect(x - 2, y + 6, w + 4, h + 6, r + 2);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.30);
    pen.roundedRect(x, y + 2, w, h + 1, r);
    cairo_fill(cr);
    pen.color(fill);
    pen.roundedRect(x, y, w, h, r);
    cairo_fill(cr);
    if (borderWidth > 0) {
        pen.color(border);
        cairo_set_line_width(cr, borderWidth);
        pen.roundedRect(x + borderWidth / 2, y + borderWidth / 2, w - borderWidth, h - borderWidth, r - borderWidth / 2);
        cairo_stroke(cr);
    }
    cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
    cairo_set_line_width(cr, 1);
    const double inset = borderWidth + 0.5;
    cairo_move_to(cr, x + r, y + inset);
    cairo_line_to(cr, x + w - r, y + inset);
    cairo_stroke(cr);
}

/**
 * Stroke a rounded rectangle.
 * @param pen drawing tools
 * @param x left
 * @param y top
 * @param w width
 * @param h height
 * @param r corner radius
 * @param c color
 * @param width line width
 */
void strokeRounded(const Pen& pen, double x, double y, double w, double h, double r, Color c, double width) {
    pen.color(c);
    cairo_set_line_width(pen.cr, width);
    pen.roundedRect(x + width / 2, y + width / 2, w - width, h - width, std::max(0.0, r - width / 2));
    cairo_stroke(pen.cr);
}

/**
 * Fill a rounded rectangle.
 * @param pen drawing tools
 * @param x left
 * @param y top
 * @param w width
 * @param h height
 * @param r corner radius
 * @param c color
 */
void fillRounded(const Pen& pen, double x, double y, double w, double h, double r, Color c) {
    pen.color(c);
    pen.roundedRect(x, y, w, h, std::min(r, std::min(w, h) / 2));
    cairo_fill(pen.cr);
}

/**
 * Draw a check mark with lines (no font needed).
 * @param cr cairo
 * @param cx center x
 * @param cy center y
 * @param s size (px)
 * @param c color
 */
void drawCheck(cairo_t* cr, double cx, double cy, double s, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, s * 0.16);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - s * 0.36, cy + s * 0.02);
    cairo_line_to(cr, cx - s * 0.10, cy + s * 0.28);
    cairo_line_to(cr, cx + s * 0.38, cy - s * 0.26);
    cairo_stroke(cr);
}

/** A leading check icon (and a space) on a destination card's line, hung in front of the lines. */
enum class LineMark { None, Check };

/**
 * Split a card line into its leading mark and the rest.
 * @param text the line (ICON_CHECK " Synced to others")
 * @param rest the text after the mark (written)
 * @return the mark
 */
LineMark splitMark(const std::string& text, std::string& rest) {
    static const std::string check = ICON_CHECK " ";
    if (text.compare(0, check.size(), check) == 0) {
        rest = text.substr(check.size());
        return LineMark::Check;
    }
    rest = text;
    return LineMark::None;
}

/**
 * Draw a filled circle.
 * @param cr cairo
 * @param cx center x
 * @param cy center y
 * @param r radius
 * @param c color
 */
void drawDot(cairo_t* cr, double cx, double cy, double r, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_fill(cr);
}

/**
 * Draw a circle outline.
 * @param cr cairo
 * @param cx center x
 * @param cy center y
 * @param r radius
 * @param width line width
 * @param c color
 */
void drawRing(cairo_t* cr, double cx, double cy, double r, double width, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, width);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(cr);
}

/**
 * Draw a small padlock (the "locked by command line" note).
 * @param pen drawing tools
 * @param x left
 * @param baseline the text baseline next to it
 * @param s height (px)
 * @param c color
 */
void drawLock(const Pen& pen, double x, double baseline, double s, Color c) {
    const double bodyW = s * 0.78;
    const double bodyH = s * 0.55;
    const double top = baseline - bodyH;
    fillRounded(pen, x, top, bodyW, bodyH, s * 0.12, c);
    pen.color(c);
    cairo_set_line_width(pen.cr, s * 0.14);
    cairo_new_sub_path(pen.cr);
    cairo_arc(pen.cr, x + bodyW / 2, top, bodyW * 0.30, M_PI, 2 * M_PI);
    cairo_stroke(pen.cr);
}

/**
 * Draw the gaze scale's link: a padlock, closed (the pair linked) or with its shackle lifted off the right (open).
 * @param pen drawing tools
 * @param cx center x
 * @param cy center y
 * @param s size (px)
 * @param open the shackle lifted
 * @param c color
 * @param hole the keyhole's color (the button's fill)
 */
void drawLinkLock(const Pen& pen, double cx, double cy, double s, bool open, Color c, Color hole) {
    cairo_t* cr = pen.cr;
    const double bodyW = s * 0.72;
    const double bodyH = s * 0.52;
    const double bodyTop = cy + s * 0.5 - bodyH;
    fillRounded(pen, cx - bodyW / 2, bodyTop, bodyW, bodyH, s * 0.1, c);
    // The keyhole
    pen.color(hole);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, bodyTop + bodyH * 0.42, s * 0.07, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_rectangle(cr, cx - s * 0.03, bodyTop + bodyH * 0.42, s * 0.06, bodyH * 0.3);
    cairo_fill(cr);
    // The shackle: both legs in the body, or lifted with its right leg out
    const double r = bodyW * 0.32;
    const double legH = s * 0.16;
    const double lift = open ? s * 0.16 : 0.0;
    const double arcY = bodyTop - legH - lift;
    pen.color(c);
    cairo_set_line_width(cr, s * 0.13);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_new_path(cr);
    cairo_move_to(cr, cx - r, bodyTop + 1);
    cairo_line_to(cr, cx - r, arcY);
    cairo_arc(cr, cx, arcY, r, M_PI, 2 * M_PI);
    cairo_line_to(cr, cx + r, open ? arcY + legH * 0.75 : bodyTop + 1);
    cairo_stroke(cr);
}

/**
 * Draw a horizontal line (a minus sign) or a cross (a plus sign).
 * @param cr cairo
 * @param cx center x
 * @param cy center y
 * @param s length (px)
 * @param plus draw the vertical stroke too
 * @param c color
 */
void drawPlusMinus(cairo_t* cr, double cx, double cy, double s, bool plus, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, 3.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx - s / 2, cy);
    cairo_line_to(cr, cx + s / 2, cy);
    if (plus) {
        cairo_move_to(cr, cx, cy - s / 2);
        cairo_line_to(cr, cx, cy + s / 2);
    }
    cairo_stroke(cr);
}

/**
 * A numbered circle (the lid marks ① to ③).
 * @param pen drawing tools
 * @param cx center x
 * @param cy center y
 * @param number 1..5
 * @param fill its color (③ has its own)
 */
/**
 * Paint what was drawn since cairo_push_group faded (a row that doesn't apply now).
 * @param pen drawing tools
 */
void fadeGroup(const Pen& pen) {
    cairo_pop_group_to_source(pen.cr);
    cairo_paint_with_alpha(pen.cr, 0.45);
}

void drawNumberBadge(const Pen& pen, double cx, double cy, int number, Color fill = kAccent) {
    drawDot(pen.cr, cx, cy, 10, fill);
    const std::string text = std::to_string(number);
    textCentered(pen, cx, cy + 5, text, 14, kOnAccent, true);
}

/**
 * Format a pair of numbers as "x 0.02  y -0.10".
 * @param pair the numbers
 * @return the text ("—" if missing)
 */
std::string xyText(const Pair& pair) {
    if (!pair.valid()) return "—";
    char text[64];
    std::snprintf(text, sizeof(text), "x %+.2f   y %+.2f", pair.v[0], pair.v[1]);
    return text;
}

/**
 * Format a value with 2 decimals.
 * @param value the value
 * @return the text ("—" if NaN)
 */
std::string twoDecimals(double value) {
    if (!std::isfinite(value)) return "—";
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return text;
}

/**
 * A small terminal: a rounded box with ">_" (the Konsole buttons).
 * @param pen drawing tools
 * @param x left
 * @param cy center y
 * @param c color
 */
void drawTerminalIcon(const Pen& pen, double x, double cy, Color c) {
    cairo_t* cr = pen.cr;
    strokeRounded(pen, x, cy - 8, 20, 16, 3, c, 1.8);
    pen.color(c);
    cairo_set_line_width(cr, 1.8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, x + 5, cy - 3);
    cairo_line_to(cr, x + 8.5, cy);
    cairo_line_to(cr, x + 5, cy + 3);
    cairo_move_to(cr, x + 10.5, cy + 3.5);
    cairo_line_to(cr, x + 15, cy + 3.5);
    cairo_stroke(cr);
}

/**
 * A play sign: a triangle outline (the video and "start" buttons).
 * @param pen drawing tools
 * @param x left
 * @param cy center y
 * @param c color
 */
void drawPlayIcon(const Pen& pen, double x, double cy, Color c) {
    cairo_t* cr = pen.cr;
    pen.color(c);
    cairo_set_line_width(cr, 2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, x + 2, cy - 7);
    cairo_line_to(cr, x + 14, cy);
    cairo_line_to(cr, x + 2, cy + 7);
    cairo_close_path(cr);
    cairo_stroke(cr);
}

/**
 * An eye: an almond outline and its pupil (whether the cameras see each eye).
 * @param pen drawing tools
 * @param cx center x
 * @param cy center y
 * @param c color
 */
void drawEyeIcon(const Pen& pen, double cx, double cy, Color c) {
    cairo_t* cr = pen.cr;
    pen.color(c);
    cairo_set_line_width(cr, 2);
    cairo_new_path(cr);
    cairo_move_to(cr, cx - 10, cy);
    cairo_curve_to(cr, cx - 5, cy - 7, cx + 5, cy - 7, cx + 10, cy);
    cairo_curve_to(cr, cx + 5, cy + 7, cx - 5, cy + 7, cx - 10, cy);
    cairo_close_path(cr);
    cairo_stroke(cr);
    drawDot(cr, cx, cy, 2.8, c);
}

/**
 * A camera: a rounded body, a lens and a bump on top (the eye cameras).
 * @param pen drawing tools
 * @param cx center x
 * @param cy center y
 * @param c color
 */
void drawCameraIcon(const Pen& pen, double cx, double cy, Color c) {
    cairo_t* cr = pen.cr;
    strokeRounded(pen, cx - 10, cy - 6, 20, 14, 3, c, 1.8);
    pen.color(c);
    cairo_set_line_width(cr, 1.8);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy + 1, 3.6, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - 4, cy - 6);
    cairo_line_to(cr, cx - 2.5, cy - 9);
    cairo_line_to(cr, cx + 2.5, cy - 9);
    cairo_line_to(cr, cx + 4, cy - 6);
    cairo_stroke(cr);
}

/**
 * The tag after an address that isn't in the home network: "VPN" (100.64/10, blue) or "PUBLIC" (amber); none
 * otherwise. Measured (draw false) or drawn, right-aligned.
 * @param pen drawing tools
 * @param t texts
 * @param net what kind of address ("lan", "vpn", "public"; frameeyeosc's words)
 * @param right its right edge
 * @param baseline the line's baseline it sits on
 * @param size its text size
 * @param draw draw it (else only measure)
 * @return its width (0 = none)
 */
double drawNetTag(const Pen& pen, const UiText& t, const std::string& net, double right, double baseline, double size,
                  bool draw = true) {
    const bool vpn = net == "vpn";
    if (!vpn && net != "public") return 0;
    const char* label = vpn ? t.tagVpn : t.tagPublic;
    const double w = pen.measure(label, size, true) + 12;
    if (!draw) return w;
    const double h = size + 8;
    const double top = baseline - size * 0.36 - h / 2;
    fillRounded(pen, right - w, top, w, h, 4, vpn ? kVpnTint : kWarningTint);
    pen.text(right - w + 6, baseline, label, size, vpn ? kSnap : kWarning, true);
    return w;
}

/**
 * The tag's kind for an address as written, while frameeyeosc doesn't say: a typed VPN address, or a public one with
 * lan_only off.
 * @param host the host as written
 * @param lanOnly lan_only
 * @return "vpn", "public" or ""
 */
std::string netOfHost(const std::string& host, bool lanOnly) {
    const extras::Net net = extras::classify(host);
    if (net == extras::Net::Vpn) return "vpn";
    if (net == extras::Net::Public && !lanOnly) return "public";
    return "";
}

/**
 * An extra output's format as the list and the left column show it: its name in config.json.
 * @param format "feo1" / "osc" (anything else as it is)
 * @return "feo1", "osc"; "?" for none
 */
std::string formatShort(const std::string& format) {
    return format.empty() ? std::string("?") : format;
}

/**
 * A keypad's backspace key: a left-pointing key shape with an x, drawn (no font needed).
 * @param pen drawing tools
 * @param cx center x
 * @param cy center y
 * @param scale 1 = the target PC keypad's size
 * @param color its color
 */
void drawBackspace(const Pen& pen, double cx, double cy, double scale, Color color) {
    cairo_t* cr = pen.cr;
    pen.color(color);
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - 22 * scale, cy);
    cairo_line_to(cr, cx - 10 * scale, cy - 13 * scale);
    cairo_line_to(cr, cx + 22 * scale, cy - 13 * scale);
    cairo_line_to(cr, cx + 22 * scale, cy + 13 * scale);
    cairo_line_to(cr, cx - 10 * scale, cy + 13 * scale);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - 1 * scale, cy - 6 * scale);
    cairo_line_to(cr, cx + 11 * scale, cy + 6 * scale);
    cairo_move_to(cr, cx + 11 * scale, cy - 6 * scale);
    cairo_line_to(cr, cx - 1 * scale, cy + 6 * scale);
    cairo_stroke(cr);
}

/**
 * A thin line under a row of the Eyelids tab.
 * @param pen drawing tools
 * @param at the row's bottom
 */
void drawRowDivider(const Pen& pen, double at) {
    pen.color(kDivider);
    cairo_set_line_width(pen.cr, 1);
    cairo_move_to(pen.cr, kInnerX, at + 0.5);
    cairo_line_to(pen.cr, kInnerRight, at + 0.5);
    cairo_stroke(pen.cr);
}

/**
 * An "i" in a circle (the "when..." box).
 * @param pen drawing tools
 * @param cx center x
 * @param cy center y
 * @param c color
 */
void drawInfoIcon(const Pen& pen, double cx, double cy, Color c) {
    cairo_t* cr = pen.cr;
    pen.color(c);
    cairo_set_line_width(cr, 1.8);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, 8, 0, 2 * M_PI);
    cairo_stroke(cr);
    drawDot(cr, cx, cy - 3.6, 1.3, c);
    cairo_move_to(cr, cx, cy - 0.5);
    cairo_line_to(cr, cx, cy + 4);
    cairo_stroke(cr);
}

/**
 * A chevron pointing right (between the calibration's step chips, and on the left column's "next" card).
 * @param pen drawing tools
 * @param cx center x
 * @param cy center y
 * @param size its height
 * @param width line width
 * @param c color
 */
void drawChevron(const Pen& pen, double cx, double cy, double size, double width, Color c) {
    cairo_t* cr = pen.cr;
    pen.color(c);
    cairo_set_line_width(cr, width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - size / 4, cy - size / 2);
    cairo_line_to(cr, cx + size / 4, cy);
    cairo_line_to(cr, cx - size / 4, cy + size / 2);
    cairo_stroke(cr);
}

/** The text size of a step chip, and the room between two (a chevron in its middle). */
constexpr double kStepChipSize = 16;
constexpr double kStepChipGap = 22;

/**
 * A step chip's width (a calibration's steps, and a recording's).
 * @param pen drawing tools
 * @param label its text
 * @param past done (it has a check)
 * @param now running now (bold)
 * @return the width
 */
double stepChipWidth(const Pen& pen, const std::string& label, bool past, bool now) {
    return pen.measure(label, kStepChipSize, now) + 26 + (past ? 22 : 0);
}

/**
 * Draw a step chip: done (a check, on the control color), running now (in the accent) or still to come (an outline).
 * @param pen drawing tools
 * @param x left
 * @param baseline its text's baseline (the chip is 32 high, from 22 above it)
 * @param label its text
 * @param past done
 * @param now running now
 * @return its width
 */
double drawStepChip(const Pen& pen, double x, double baseline, const std::string& label, bool past, bool now) {
    const double h = 32;
    const double check = past ? 22 : 0;
    const double w = stepChipWidth(pen, label, past, now);
    if (now) {
        fillRounded(pen, x, baseline - 22, w, h, h / 2, kAccent);
    } else if (past) {
        fillRounded(pen, x, baseline - 22, w, h, h / 2, kControl);
        drawCheck(pen.cr, x + 19, baseline - 6, 14, kSuccess);
    } else {
        strokeRounded(pen, x, baseline - 22, w, h, h / 2, kDivider, 1.5);
    }
    pen.text(x + 13 + check, baseline - 1, label, kStepChipSize, now ? kOnAccent : kTextMuted, now);
    return w;
}

/**
 * A recording's step chips in one row from kInnerX: the ones eyecam::chipWindow picks, chevrons between them, and a
 * mark for the steps left out at either end ("…" before them, how many after them).
 * @param pen drawing tools
 * @param t texts
 * @param chips the chips
 * @param baseline their text's baseline
 * @param width the row's width
 */
void drawRunChips(const Pen& pen, const UiText& t, const eyecam::StepChips& chips, double baseline, double width) {
    const int count = static_cast<int>(chips.labels.size());
    std::vector<std::string> labels;
    std::vector<double> widths;
    for (int i = 0; i < count; ++i) {
        labels.push_back(eyecam::chipLabel(t, chips.labels[static_cast<size_t>(i)]));
        widths.push_back(stepChipWidth(pen, labels.back(), i < chips.current, i == chips.current));
    }
    const auto more = [&](int hidden) {
        char text[64];
        std::snprintf(text, sizeof(text), t.eyecamChipsMoreFormat, hidden);
        return std::string(text);
    };
    const std::string before = "…";
    // (as wide as either mark can be)
    const double markW = std::max(pen.measure(before, kStepChipSize), pen.measure(more(count), kStepChipSize));
    const eyecam::ChipWindow shown = eyecam::chipWindow(widths, chips.current, width, kStepChipGap, markW);
    double x = kInnerX;
    const auto chevron = [&]() {
        drawChevron(pen, x + kStepChipGap / 2, baseline - 6, 10, 1.8, kBorder);
        x += kStepChipGap;
    };
    if (shown.first > 0) {
        x += pen.text(x, baseline - 1, before, kStepChipSize, kTextMuted);
        chevron();
    }
    for (int i = shown.first; i < shown.end; ++i) {
        x += drawStepChip(pen, x, baseline, labels[static_cast<size_t>(i)], i < chips.current, i == chips.current);
        if (i + 1 < count) chevron();
    }
    if (shown.end < count) pen.text(x, baseline - 1, more(count - shown.end), kStepChipSize, kTextMuted);
}

/**
 * What drives the eyelids now, as the eye cameras' page says it.
 * @param t texts
 * @param use the case (cameraUse)
 * @param s frameeyeosc's status (the cameras' own reason, and the pupils)
 * @return the line ("" while it isn't known)
 */
std::string cameraUseText(const UiText& t, CameraUse use, const EyeStatus& s);

/**
 * A printf into a std::string.
 * @param format the format (one string argument)
 * @param value the argument
 * @return the text
 */
std::string formatText(const char* format, const std::string& value) {
    char text[256];
    std::snprintf(text, sizeof(text), format, value.c_str());
    return text;
}

/**
 * A printf into a std::string, for a longer text with one or two strings.
 * @param format the format (one or two string arguments)
 * @param first the first argument
 * @param second the second (ignored by a format with one)
 * @return the text
 */
std::string formatLong(const char* format, const std::string& first, const std::string& second = "") {
    char text[1024];
    std::snprintf(text, sizeof(text), format, first.c_str(), second.c_str());
    return text;
}

/**
 * What "Saved data" knows of a kind now.
 * @param m the model
 * @param kind the kind
 * @param usage how many and their size
 * @return false while it isn't known (not counted yet, or eyecam-rec doesn't run or count them)
 */
bool dataUsage(const PanelModel& m, stored::Kind kind, stored::Usage& usage) {
    const eyecam::Stored& s = m.eyecam.status.stored;
    switch (kind) {
        case stored::Kind::EyeLogs:
            usage = m.savedData.eyeLogs;
            return m.savedData.eyeLogsCounted;
        case stored::Kind::EyecamRecordings: usage = {static_cast<int>(s.recCount), s.recBytes}; break;
        case stored::Kind::CalibImages: usage = {static_cast<int>(s.calibCount), s.calibBytes}; break;
    }
    return m.eyecam.visible && s.present && s.scannedUnix > 0;
}

/**
 * Whether eyecam-rec records or calibrates now (it deletes nothing then).
 * @param m the model
 * @return true if it does
 */
bool eyecamBusyForData(const PanelModel& m) {
    const eyecam::State state = m.eyecam.status.state;
    return state == eyecam::State::Searching || state == eyecam::State::Recording ||
           state == eyecam::State::Calibrating;
}

/**
 * Whether a kind's "Delete" can be pressed: there is some, no delete of it runs, and for eyecam's, eyecam-rec runs,
 * counts them, deletes nothing else and neither records nor calibrates.
 * @param m the model
 * @param kind the kind
 * @return true if it can
 */
bool dataDeletable(const PanelModel& m, stored::Kind kind) {
    stored::Usage usage;
    if (!dataUsage(m, kind, usage) || usage.count == 0 || m.savedData.of(kind).running) return false;
    if (kind == stored::Kind::EyeLogs) return true;
    return !eyecamBusyForData(m) && m.eyecam.status.stored.deleting.empty();
}

/**
 * How many of a kind and their size, as its row says it.
 * @param t texts
 * @param kind the kind
 * @param usage how many and their size
 * @return "12 回分・2.4 GB"
 */
std::string dataCountText(const UiText& t, stored::Kind kind, const stored::Usage& usage) {
    char text[128];
    const int i = static_cast<int>(kind);
    std::snprintf(text, sizeof(text), usage.count == 1 ? t.dataCountOneFormats[i] : t.dataCountFormats[i], usage.count,
                  contrib::sizeText(usage.bytes).c_str());
    return text;
}

std::string cameraUseText(const UiText& t, CameraUse use, const EyeStatus& s) {
    std::string text;
    switch (use) {
        case CameraUse::Both: text = t.cameraUseBoth; break;
        case CameraUse::Left: text = t.cameraUseLeft; break;
        case CameraUse::Right: text = t.cameraUseRight; break;
        case CameraUse::Off:
        case CameraUse::Valve: text = t.cameraUseValve; break;
        case CameraUse::NotCalibrated: text = formatText(t.cameraUseValveFormat, t.cameraWhyNotCalibrated); break;
        case CameraUse::NoCamera: text = formatText(t.cameraUseValveFormat, t.cameraWhyNoCamera); break;
        case CameraUse::Warming: text = formatText(t.cameraUseValveFormat, t.cameraWhyWarming); break;
        case CameraUse::Error: text = formatText(t.cameraUseValveFormat, s.camera.error); break;
        case CameraUse::Unknown: break;  // frameeyeosc not running, or one that doesn't say
    }
    const bool inUse = use == CameraUse::Both || use == CameraUse::Left || use == CameraUse::Right;
    if (inUse && (s.camera.pupilUsed[0] || s.camera.pupilUsed[1])) text += t.cameraPupilSuffix;
    return text;
}

/**
 * When GitHub last answered, short: the time if it was today, the month and day otherwise.
 * @param unixTime seconds since 1970
 * @return "16:45" or "9/27"
 */
std::string checkedText(long long unixTime) {
    const std::time_t when = static_cast<std::time_t>(unixTime);
    const std::time_t now = std::time(nullptr);
    std::tm whenTm {};
    std::tm nowTm {};
    localtime_r(&when, &whenTm);
    localtime_r(&now, &nowTm);
    char text[32];
    if (whenTm.tm_year == nowTm.tm_year && whenTm.tm_yday == nowTm.tm_yday) {
        std::snprintf(text, sizeof(text), "%d:%02d", whenTm.tm_hour, whenTm.tm_min);
    } else {
        std::snprintf(text, sizeof(text), "%d/%d", whenTm.tm_mon + 1, whenTm.tm_mday);
    }
    return text;
}

/**
 * A version without a leading "v" (the texts add their own).
 * @param version "0.4.0" or "v0.4.0"
 * @return "0.4.0"
 */
std::string bareVersion(const std::string& version) {
    return !version.empty() && (version[0] == 'v' || version[0] == 'V') ? version.substr(1) : version;
}

/**
 * A gaze zero point as shown: 3 decimals and the angle ("+0.010 (+0.5°)"; zero without a sign).
 * @param value the setting
 * @return the text
 */
std::string offsetText(double value) {
    if (!std::isfinite(value)) return "—";
    const double shown = std::fabs(value) < 0.0005 ? 0.0 : value;
    char text[64];
    std::snprintf(text, sizeof(text), shown == 0.0 ? "%.3f (%.1f°)" : "%+.3f (%+.1f°)", shown,
                  shown * gaze_fit::kFullScaleDeg);
    return text;
}

/**
 * What to do while a fit runs.
 * @param t texts
 * @param mode the fit
 * @return the words
 */
const char* howToText(const UiText& t, gaze_fit::Mode mode) {
    switch (mode) {
        case gaze_fit::Mode::Center: return t.fitHowToCenter;
        case gaze_fit::Mode::Full: break;
    }
    return t.fitHowTo;
}

/**
 * A text's sentences (split after "。", or after ". " in English), so each can start a line of its own. An English
 * sentence keeps its full stop; a Japanese one drops it, like the panel's other lines.
 * @param text the text
 * @return the sentences
 */
std::vector<std::string> sentences(const std::string& text) {
    constexpr const char* kJaStop = "。";
    const size_t jaStop = std::strlen(kJaStop);
    std::vector<std::string> out;
    for (size_t from = 0; from < text.size();) {
        const size_t ja = text.find(kJaStop, from);
        const size_t en = text.find(". ", from);
        size_t end = text.size();
        size_t next = text.size();
        if (ja != std::string::npos && (en == std::string::npos || ja < en)) {
            end = ja;
            next = ja + jaStop;
        } else if (en != std::string::npos) {
            end = en + 1;
            next = en + 2;
        }
        out.push_back(text.substr(from, end - from));
        from = next;
    }
    return out;
}

/**
 * Lines for a hint of a few sentences: a sentence that fits goes on the line before it, else on a new one (wrapped
 * itself if it is too long), so that a line never breaks in the middle of a word or a short sentence when it
 * needn't.
 * @param pen drawing tools
 * @param text the text ("。" or ". " between sentences)
 * @param size text size
 * @param bold bold
 * @param width the width
 * @param maxLines at most this many lines
 * @return the lines
 */
std::vector<std::string> sentenceLines(const Pen& pen, const std::string& text, double size, bool bold, double width,
                                       size_t maxLines) {
    constexpr const char* kJaStop = "。";
    std::vector<std::string> lines;
    for (size_t from = 0; from < text.size();) {
        const size_t ja = text.find(kJaStop, from);
        const size_t en = text.find(". ", from);
        size_t end = text.size();
        if (ja != std::string::npos && (en == std::string::npos || ja < en)) {
            end = ja + std::strlen(kJaStop);
        } else if (en != std::string::npos) {
            end = en + 1;
        }
        std::string part = text.substr(from, end - from);
        from = end;
        while (!part.empty() && part[0] == ' ') part.erase(0, 1);
        if (part.empty()) continue;
        // (no space after "。")
        const std::string stop = kJaStop;
        const bool afterJa = !lines.empty() && lines.back().size() >= stop.size() &&
                             lines.back().compare(lines.back().size() - stop.size(), stop.size(), stop) == 0;
        const std::string joined = lines.empty() ? part : lines.back() + (afterJa ? "" : " ") + part;
        if (!lines.empty() && pen.measure(joined, size, bold) <= width) {
            lines.back() = joined;
            continue;
        }
        for (const std::string& line : wrapText(pen, part, size, bold, width, maxLines)) lines.push_back(line);
    }
    if (lines.size() > maxLines) lines.resize(maxLines);
    return lines;
}

/**
 * Whether an action is one of the eye preview sheet's own buttons (its cross, choices, switches and freeze).
 * @param action the action
 * @return true if it is
 */
bool previewSheetAction(PanelAction action) {
    return action == PanelAction::PreviewClose || action == PanelAction::PreviewSource ||
           action == PanelAction::PreviewMirror || action == PanelAction::PreviewCamera ||
           action == PanelAction::PreviewMark || action == PanelAction::PreviewFreeze;
}

/**
 * Whether a fit is running (its targets are up).
 * @param fit the fit
 * @return true while it runs
 */
bool fitRunning(const gaze_fit::View& fit) {
    using gaze_fit::Phase;
    return fit.phase == Phase::Settling || fit.phase == Phase::Capturing || fit.phase == Phase::Reopen;
}

}  // namespace

bool PanelHit::operator==(const PanelHit& other) const {
    const bool sameKey = key == other.key || (key != nullptr && other.key != nullptr && std::strcmp(key, other.key) == 0);
    return action == other.action && sameKey && arg == other.arg;
}

EyePanel::EyePanel(const FontSet& fonts) : fonts_(fonts) {
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
    cr_ = cairo_create(surface_);
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_set_font_options(cr_, options);
    cairo_font_options_destroy(options);
}

EyePanel::~EyePanel() {
    cairo_destroy(cr_);
    cairo_surface_destroy(surface_);
}

void EyePanel::addButton(PanelHit hit, double x, double y, double w, double h, bool usable) {
    if (hitClip_) {
        // In a scrolled view: only the part that shows
        const double top = std::max(y, hitClipTop_);
        const double bottom = std::min(y + h, hitClipBottom_);
        if (bottom - top < 4) return;
        y = top;
        h = bottom - top;
    }
    buttons_.push_back({hit, x, y, w, h, usable});
}

int EyePanel::pointerState(const PanelHit& hit) const {
    if (pressed_ == hit) return 2;
    if (hover_ == hit) return 1;
    return 0;
}

std::vector<EyePanel::HitArea> EyePanel::hitAreas() const {
    std::vector<HitArea> areas;
    for (const auto& b : buttons_) {
        if (b.usable) areas.push_back({b.hit, b.x, b.y, b.w, b.h});
    }
    return areas;
}

PanelHit EyePanel::hitTest(double x, double y) const {
    for (const auto& b : buttons_) {
        if (!b.usable) continue;
        if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) return b.hit;
    }
    return {};
}

double EyePanel::sensitivityAt(double x) const {
    const double value = std::clamp((x - sensTrackX_) / std::max(1.0, sensTrackW_), 0.0, 1.0);
    return std::round(value * 100) / 100;
}

bool EyePanel::takeNumberSlider(std::string& name, double& value) {
    if (!numReleased_) return false;
    numReleased_ = false;
    name = numKey_;
    value = numValue_;
    return std::isfinite(value);
}

bool EyePanel::takeSensitivity(double& value, double now) {
    if (!sensReleased_) return false;
    sensReleased_ = false;
    value = sensValue_;
    sensHeld_ = true;
    sensHoldUntil_ = now + kSensitivityHoldSec;
    return true;
}

void EyePanel::previewSensitivityDrag(double value) {
    sensDragging_ = true;
    sensValue_ = std::clamp(value, 0.0, 1.0);
}

bool EyePanel::pointerMove(double x, double y) {
    // A held slider follows the pointer anywhere (it is let go of by the release, or by leaving the panel)
    if (sensDragging_) {
        const double value = sensitivityAt(x);
        if (value == sensValue_) return false;
        sensValue_ = value;
        return true;
    }
    if (numDragging_) {
        const double value = numberAt(numKey_, x);
        if (!std::isfinite(value) || value == numValue_) return false;
        numValue_ = value;
        return true;
    }
    const PanelHit now = hitTest(x, y);
    if (now == hover_) return false;
    hover_ = now;
    return true;
}

PanelHit EyePanel::pointerDown(double x, double y, double now) {
    hover_ = hitTest(x, y);
    pressed_ = hover_;
    const PanelHit hit = pressed_;
    // Pressing anything else cancels a pending confirmation
    if (hit.action != PanelAction::Quit) quitArmed_ = false;
    if (hit.action != PanelAction::ResetAll) resetArmed_ = false;
    // ...and closes the eye preview (a tab, the left column's notices): only its own buttons keep it
    const bool previewButton = hit.action == PanelAction::PreviewOpen || previewSheetAction(hit.action);
    if (hit.action != PanelAction::None && !previewButton) previewOpen_ = false;
    // ...and "Other apps"' row asking whether to delete it: only its own two buttons keep it (another row's "Delete"
    // opens that one's instead); a write the list couldn't make is forgotten
    const bool rowDelete = hit.action == PanelAction::ExtraRowDelete ||
                           hit.action == PanelAction::ExtraRowDeleteCancel ||
                           hit.action == PanelAction::ExtraRowDeleteConfirm;
    if (hit.action != PanelAction::None && !rowDelete) extrasRowDelete_ = -1;
    if (hit.action != PanelAction::None && extrasStep_ == ExtrasStep::List) extrasError_.clear();
    switch (hit.action) {
        case PanelAction::PreviewOpen:
            // (pressed again while it is open: it closes)
            if (previewOpen_) {
                previewOpen_ = false;
            } else {
                openPreview();
            }
            return {};
        case PanelAction::PreviewClose:
            previewOpen_ = false;
            return {};
        case PanelAction::PreviewSource:
            if (hit.arg < 0 || hit.arg > static_cast<int>(eye_preview::Source::Camera)) return {};
            previewSource_ = static_cast<eye_preview::Source>(hit.arg);
            return {};
        case PanelAction::PreviewMirror:
            previewMirror_ = !previewMirror_;
            return {};
        case PanelAction::PreviewCamera:
            previewCamera_ = !previewCamera_;
            previewCameraKept_ = previewCamera_;
            return {};
        case PanelAction::PreviewMark:
            switch (static_cast<eye_preview::Mark>(hit.arg)) {
                case eye_preview::Mark::Lids: previewMarks_.lids = !previewMarks_.lids; break;
                case eye_preview::Mark::Pupil: previewMarks_.pupil = !previewMarks_.pupil; break;
                case eye_preview::Mark::Window: previewMarks_.window = !previewMarks_.window; break;
            }
            return {};
        case PanelAction::PreviewFreeze:
            // Stopped at what the next draw shows (the model isn't at hand here)
            previewFrozen_ = !previewFrozen_;
            previewFreezePending_ = previewFrozen_;
            return {};
        case PanelAction::Tab:
            setTab(static_cast<PanelTab>(hit.arg));
            closeExtrasFlow();
            closeAppsList();
            closeContrib();
            historyOpen_ = false;
            diagOpen_ = false;
            recordOpen_.clear();
            recordsAllOpen_ = false;
            return {};
        case PanelAction::AdvancedPage:
            // (from the left column's update notice too: the tab opens on it)
            if (hit.arg < 0 || hit.arg > static_cast<int>(AdvPage::Data)) return {};
            setTab(PanelTab::Advanced);
            setAdvPage(static_cast<AdvPage>(hit.arg));
            closeAppsList();
            closeContrib();
            historyOpen_ = false;
            diagOpen_ = false;
            recordOpen_.clear();
            recordsAllOpen_ = false;
            return {};
        case PanelAction::LidsPage:
            // (from the eye cameras tab's pointer to the widening slider too: the tab opens on it)
            if (hit.arg < 0 || hit.arg > static_cast<int>(LidsPage::Brows)) return {};
            setTab(PanelTab::Lids);
            setLidsPage(static_cast<LidsPage>(hit.arg));
            closeAppsList();
            closeContrib();
            historyOpen_ = false;
            diagOpen_ = false;
            recordOpen_.clear();
            recordsAllOpen_ = false;
            return {};
        case PanelAction::GazePage:
            if (hit.arg < 0 || hit.arg > static_cast<int>(GazePage::Glitch)) return {};
            setTab(PanelTab::Gaze);
            setGazePage(static_cast<GazePage>(hit.arg));
            closeAppsList();
            closeContrib();
            return {};
        case PanelAction::OutputPage:
            if (hit.arg < 0 || hit.arg > static_cast<int>(OutputPage::Apps)) return {};
            setTab(PanelTab::Output);
            setOutputPage(static_cast<OutputPage>(hit.arg));
            closeAppsList();
            closeContrib();
            return {};
        case PanelAction::ExtraRowDelete:
            // The row turns into its question (another row's closes)
            extrasError_.clear();
            openExtrasRowDelete(hit.arg);
            return {};
        case PanelAction::ExtraRowDeleteCancel:
            extrasRowDelete_ = -1;
            return {};
        case PanelAction::ExtraRowDeleteConfirm:
            // The caller takes it out (only the row asking, and only while the list shows)
            return extrasRowDelete() >= 0 && hit.arg == extrasRowDelete_ ? hit : PanelHit {};
        case PanelAction::ExtraAdd:
            openExtrasFlow(ExtrasStep::Format, extras::newDraft(static_cast<int>(extrasShown_.size())));
            return {};
        case PanelAction::ExtraEdit:
            if (hit.arg < 0 || hit.arg >= static_cast<int>(extrasShown_.size())) return {};
            openExtrasFlow(ExtrasStep::Format, extras::draftOf(extrasShown_[hit.arg], extrasRaw_[hit.arg]));
            return {};
        case PanelAction::ExtraCancel:
            closeExtrasFlow();
            return {};
        case PanelAction::ExtraStep: {
            // Next only once the step is done: a format chosen, a host that can be sent to
            const ExtrasStep step = static_cast<ExtrasStep>(hit.arg);
            if (hit.arg < 0 || hit.arg > static_cast<int>(ExtrasStep::Port) || step == ExtrasStep::List) return {};
            if (step != ExtrasStep::Format && extrasDraft_.format.empty()) return {};
            if (step == ExtrasStep::Port && !extras::hostUsable(extras::checkHost(extrasDraft_, extrasLanOnly_))) {
                return {};
            }
            extrasStep_ = step;
            extrasDeleteOpen_ = false;
            extrasError_.clear();
            return {};
        }
        case PanelAction::ExtraFormat:
            extras::chooseFormat(extrasDraft_, hit.arg == 1 ? extras::kOsc : extras::kFeo1);
            extrasError_.clear();
            return {};
        case PanelAction::ExtraHost:
            if (hit.arg < 0 || hit.arg > static_cast<int>(extras::HostChoice::Keep)) return {};
            if (hit.arg == static_cast<int>(extras::HostChoice::Keep) && extrasDraft_.kept.empty()) return {};
            extrasDraft_.host = static_cast<extras::HostChoice>(hit.arg);
            extrasError_.clear();
            return {};
        case PanelAction::ExtraKey:
            extrasDraft_.ip = host_entry::keypadInput(extrasDraft_.ip, hit.arg);
            extrasError_.clear();
            return {};
        case PanelAction::ExtraPort:
            extrasDraft_.port = std::clamp(extrasDraft_.port + (hit.arg < 0 ? -1 : 1), 1, 65535);
            extrasError_.clear();
            return {};
        case PanelAction::ExtraPortEnter:
            // The keypad, starting with the port now (its OK sets it as − and ＋ do)
            if (extrasStep_ != ExtrasStep::Port) return {};
            openPortEntry(KeypadFor::ExtraPort, extrasDraft_.port);
            return {};
        case PanelAction::ExtraDelete:
            extrasDeleteOpen_ = extrasDraft_.edit >= 0;
            return {};
        case PanelAction::ExtraDeleteCancel:
            extrasDeleteOpen_ = false;
            return {};
        case PanelAction::ExtraDeleteConfirm:
            // The caller takes it out
            return extrasDeleteOpen() ? hit : PanelHit {};
        case PanelAction::ExtraSave:
            // The caller writes it
            return extrasStep_ == ExtrasStep::Port && !extrasDeleteOpen_ ? hit : PanelHit {};
        case PanelAction::GazeScaleLink:
            // Opened or closed here; the caller writes the second value once it is closed
            if (hit.arg < 0 || hit.arg > 1) return {};
            gazeScaleLinked_[hit.arg] = !gazeScaleLinked_[hit.arg];
            return hit;
        case PanelAction::UpdateBeta:
            // Switched here; the caller saves it and tells the update checker
            if (updateBeta_ == (hit.arg != 0)) return {};
            updateBeta_ = hit.arg != 0;
            return hit;
        case PanelAction::RecordOpen:
            // The caller reads it
            if (hit.arg < 0 || hit.arg >= static_cast<int>(recordNames_.size())) return {};
            openRecord(recordNames_[hit.arg]);
            return hit;
        case PanelAction::RecordsAll:
            // The caller reads them again
            openRecordsAll();
            return hit;
        case PanelAction::RecordBack:
            if (!recordOpen_.empty()) {
                recordOpen_.clear();
            } else {
                recordsAllOpen_ = false;
            }
            return {};
        case PanelAction::HistoryOpen:
            // The caller reads the changelog
            openHistory();
            diagOpen_ = false;
            return hit;
        case PanelAction::DiagOpen:
            // (from the eye cameras tab too; the caller reads the tool's checksum)
            openDiag();
            return hit;
        case PanelAction::DiagClose:
            closeDiag();
            return {};
        case PanelAction::HistoryClose:
            closeHistory();
            return {};
        case PanelAction::HistoryRow:
            if (hit.arg >= 0 && hit.arg < static_cast<int>(historyVersions_.size())) {
                const std::string& version = historyVersions_[hit.arg];
                // One row open at a time: opening one closes the other; the open one closes
                historyRow_ = version == historyRow_ ? std::string() : version;
                historyRowPending_ = false;
                historyReveal_ = !historyRow_.empty();
            }
            return {};
        case PanelAction::HistoryScroll:
            scroll(hit.arg * historyViewH_ / 3);
            return {};
        case PanelAction::AdvancedScroll:
            scroll(hit.arg * (recordsAllOpen_ && recordOpen_.empty() ? listViewH_ : advViewH_) / 3);
            return {};
        case PanelAction::AppsOpen:
            // The caller fetches the list if it is due
            openAppsList();
            return hit;
        case PanelAction::AppsClose:
            closeAppsList();
            return {};
        case PanelAction::AppsInstall:
            // Never straight to Konsole: the confirmation first, with the command it runs
            if (hit.arg < 0 || hit.arg >= static_cast<int>(appsKeys_.size())) return {};
            openAppsConfirm(appsKeys_[hit.arg]);
            return {};
        case PanelAction::AppsMenu:
            openAppsConfirm("");
            return {};
        case PanelAction::AppsScroll:
            scroll(hit.arg * appsViewH_ / 3);
            return {};
        case PanelAction::AppsCancel:
            // Back to the list
            appsConfirmOpen_ = false;
            appsLaunchResult_ = frame_apps::LaunchResult::Started;
            return {};
        case PanelAction::AppsLaunch:
            // The caller opens Konsole for appsConfirmKey()
            return appsConfirmOpen() ? hit : PanelHit {};
        case PanelAction::FitDetails:
            fitDetails_ = !fitDetails_;
            return {};
        case PanelAction::SetupBegin:
            setupBegun_ = true;
            return {};
        case PanelAction::FitDetailsPage:
            fitDetailsPage_ = hit.arg;
            return {};
        case PanelAction::PortEnter:
            // The main output's port: the keypad, starting with the port now (not drawn while it is locked)
            openPortEntry(KeypadFor::Port, hit.arg);
            return {};
        case PanelAction::KeypadKey:
            if (!keypadOpen_) return {};
            keypadText_ = host_entry::keypadInput(
                keypadText_, hit.arg,
                keypadFor_ == KeypadFor::Host ? host_entry::Keypad::Host : host_entry::Keypad::Port);
            keypadError_.clear();
            portError_ = host_entry::PortError::None;
            return {};
        case PanelAction::KeypadOk: {
            if (!keypadOpen_) return {};
            // The target PC: the caller checks and writes it
            if (keypadFor_ == KeypadFor::Host) return hit;
            // A port: 1-65535, or why not under it
            int port = 0;
            portError_ = host_entry::checkPort(keypadText_, port);
            if (portError_ != host_entry::PortError::None) return {};
            const KeypadFor kind = keypadFor_;
            closeKeypad();
            if (kind == KeypadFor::ExtraPort) {
                // Into the draft as − and ＋ put it (the clash checks follow it)
                if (extrasStep_ == ExtrasStep::Port) {
                    extrasDraft_.port = port;
                    extrasError_.clear();
                }
                return {};
            }
            // The main output's: the caller writes it as a setting's choice
            return {PanelAction::SetInteger, key::kPort, port};
        }
        case PanelAction::KeypadCancel:
            closeKeypad();
            return {};
        case PanelAction::EyecamStart:
            // Never straight to the light: the warning first, with a start without it
            openEyecamConfirm(eyecamState_);
            return {};
        case PanelAction::EyecamCalib:
            // Its steps show on the eye cameras tab
            if (eyecamTab_) tab_ = PanelTab::Eyecam;
            closeAppsList();
            closeContrib();
            historyOpen_ = false;
            diagOpen_ = false;
            recordOpen_.clear();
            recordsAllOpen_ = false;
            eyecamConfirm_.close();
            return hit;
        case PanelAction::EyecamSensitivity:
            // Held: follows the pointer until let go of (the caller sends it)
            sensDragging_ = true;
            sensValue_ = sensitivityAt(x);
            return {};
        case PanelAction::NumberSlider:
            // Held: follows the pointer until let go of (the caller writes it)
            if (hit.key == nullptr) return {};
            numDragging_ = true;
            numKey_ = hit.key;
            numValue_ = numberAt(numKey_, x);
            return {};
        case PanelAction::ContribOpen:
            // The caller forgets the last recording's end and failure (each press of "Record" starts afresh)
            openContrib(hit.arg == static_cast<int>(contrib::Page::Manage) ? contrib::Page::Manage
                                                                           : contrib::Page::Record);
            return hit;
        case PanelAction::ContribClose:
            closeContrib();
            return {};
        case PanelAction::ContribAgree:
            contribAgreed_ = hit.arg != 0;
            return {};
        case PanelAction::ContribEyewear:
            // The caller writes the consent with the answer
            if (hit.arg <= static_cast<int>(contrib::Eyewear::Unasked) ||
                hit.arg > static_cast<int>(contrib::Eyewear::NoAnswer)) {
                return {};
            }
            contribAgreed_ = false;
            return hit;
        case PanelAction::ContribDelete:
            contribConfirm_ = true;
            return {};
        case PanelAction::ContribDeleteCancel:
            contribConfirm_ = false;
            return {};
        case PanelAction::ContribDeleteConfirm:
            // The caller deletes it; the sheet shows the deleting, then the consent again
            if (!contribConfirm_) return {};
            contribConfirm_ = false;
            contribPage_ = contrib::Page::Record;
            return hit;
        case PanelAction::DataDelete:
            if (hit.arg < 0 || hit.arg >= stored::kKindCount) return {};
            dataConfirm_ = hit.arg;
            return {};
        case PanelAction::DataDeleteCancel:
            dataConfirm_ = -1;
            return {};
        case PanelAction::DataDeleteConfirm:
            // The caller deletes it (or asks eyecam-rec to); the page shows it under way
            if (dataConfirm_ < 0 || hit.arg != dataConfirm_) return {};
            dataConfirm_ = -1;
            return hit;
        case PanelAction::EyecamChoose:
            if (!eyecamConfirm_.isOpen()) return {};
            // "Cancel" only closes it
            return eyecamConfirm_.choose(static_cast<eyecam::StartChoice>(hit.arg)).empty() ? PanelHit {} : hit;
        case PanelAction::Quit:
            // A single accidental press never quits
            if (quitArmed_ && now <= quitArmedUntil_) return hit;
            quitArmed_ = true;
            quitArmedUntil_ = now + kConfirmSec;
            return {};
        case PanelAction::ResetAll:
            if (resetArmed_ && now <= resetArmedUntil_) {
                resetArmed_ = false;
                return hit;
            }
            resetArmed_ = true;
            resetArmedUntil_ = now + kConfirmSec;
            return {};
        case PanelAction::PromptYes:
        case PanelAction::PromptNo:
            promptOutput_.clear();
            return hit;
        case PanelAction::UpdateConfirm:
        case PanelAction::UpdateCancel:
            updatePromptVersion_.clear();
            return hit;
        default: return hit;
    }
}

bool EyePanel::pointerUp() {
    if (sensDragging_) {
        sensDragging_ = false;
        sensReleased_ = true;
    }
    if (numDragging_) {
        numDragging_ = false;
        numReleased_ = true;
    }
    if (pressed_.action == PanelAction::None) return false;
    pressed_ = {};
    return true;
}

bool EyePanel::pointerLeave() {
    // Leaving the panel lets go of the slider where it is
    if (sensDragging_) {
        sensDragging_ = false;
        sensReleased_ = true;
    }
    if (numDragging_) {
        numDragging_ = false;
        numReleased_ = true;
    }
    const bool changed = hover_.action != PanelAction::None || pressed_.action != PanelAction::None;
    hover_ = {};
    pressed_ = {};
    return changed;
}

bool EyePanel::tick(double now) {
    bool changed = false;
    if (sensHeld_ && now > sensHoldUntil_) {
        sensHeld_ = false;
        changed = true;
    }
    if (quitArmed_ && now > quitArmedUntil_) {
        quitArmed_ = false;
        changed = true;
    }
    if (resetArmed_ && now > resetArmedUntil_) {
        resetArmed_ = false;
        changed = true;
    }
    return changed;
}

void EyePanel::showPrompt(const std::string& output) {
    promptOutput_ = output;
    hover_ = {};
    pressed_ = {};
}

void EyePanel::openHostEntry(const std::string& text) {
    keypadOpen_ = true;
    keypadFor_ = KeypadFor::Host;
    keypadText_ = text;
    keypadError_.clear();
    portError_ = host_entry::PortError::None;
    hover_ = {};
    pressed_ = {};
}

void EyePanel::openPortEntry(KeypadFor kind, int port) {
    if (kind == KeypadFor::Host) return;
    keypadOpen_ = true;
    keypadFor_ = kind;
    keypadText_ = port >= 1 && port <= 65535 ? std::to_string(port) : std::string();
    keypadError_.clear();
    portError_ = host_entry::PortError::None;
    hover_ = {};
    pressed_ = {};
}

void EyePanel::openEyecamConfirm(eyecam::State state) {
    eyecamState_ = state;
    if (eyecamConfirm_.open(state)) std::fprintf(stderr, "[eyecam] light warning shown\n");
}

bool EyePanel::syncEyecam(const eyecam::View& view) {
    eyecamState_ = view.status.state;
    return eyecamConfirm_.sync(view.status.state, view.visible && tab_ == PanelTab::Advanced);
}

void EyePanel::closeKeypad() {
    keypadOpen_ = false;
    keypadError_.clear();
    portError_ = host_entry::PortError::None;
    hover_ = {};
    pressed_ = {};
}

void EyePanel::openHistory() {
    tab_ = PanelTab::Advanced;
    // (from the Version page: "Close" comes back to it)
    advPage_ = AdvPage::Version;
    recordOpen_.clear();
    recordsAllOpen_ = false;
    closeAppsList();
    closeContrib();
    historyOpen_ = true;
    historyRowPending_ = true;
    historyScrollSet_ = false;
    historyReveal_ = false;
    historyScroll_ = 0.0;
}

void EyePanel::openDiag() {
    setTab(PanelTab::Advanced);
    // "Back" comes back to "Having trouble", where its row is (also when it was opened from the eye cameras tab)
    setAdvPage(AdvPage::Trouble);
    diagOpen_ = true;
    historyOpen_ = false;
    closeAppsList();
    closeContrib();
    recordOpen_.clear();
    recordsAllOpen_ = false;
    eyecamConfirm_.close();
}

void EyePanel::openRecord(const std::string& folder) {
    // From all records "Back" goes back to them; from anywhere else to "Having trouble"
    if (tab_ != PanelTab::Advanced) recordsAllOpen_ = false;
    setTab(PanelTab::Advanced);
    setAdvPage(AdvPage::Trouble);
    recordOpen_ = folder;
    historyOpen_ = false;
    closeAppsList();
    closeContrib();
    diagOpen_ = false;
    eyecamConfirm_.close();
}

void EyePanel::openRecordsAll() {
    setTab(PanelTab::Advanced);
    setAdvPage(AdvPage::Trouble);
    recordsAllOpen_ = true;
    recordOpen_.clear();
    listScroll_ = 0.0;
    historyOpen_ = false;
    closeAppsList();
    closeContrib();
    diagOpen_ = false;
    eyecamConfirm_.close();
}

void EyePanel::openContrib(contrib::Page page) {
    setTab(PanelTab::Advanced);
    // (from "Debug tools": its cross comes back to it)
    setAdvPage(AdvPage::Tools);
    historyOpen_ = false;
    diagOpen_ = false;
    recordOpen_.clear();
    recordsAllOpen_ = false;
    eyecamConfirm_.close();
    closeAppsList();
    contribOpen_ = true;
    contribPage_ = page;
    contribConfirm_ = false;
    contribAgreed_ = false;
}

void EyePanel::openAppsList() {
    setTab(PanelTab::Advanced);
    // (from "Version": its cross comes back to it)
    setAdvPage(AdvPage::Version);
    historyOpen_ = false;
    diagOpen_ = false;
    recordOpen_.clear();
    recordsAllOpen_ = false;
    eyecamConfirm_.close();
    closeContrib();
    appsListOpen_ = true;
    appsConfirmOpen_ = false;
    appsLaunchResult_ = frame_apps::LaunchResult::Started;
    appsScroll_ = 0.0;
}

void EyePanel::openPreview() {
    previewOpen_ = true;
    previewFrozen_ = false;
    // The eye pictures start off, so an eye never shows by accident (in a screenshot or a stream); once turned on, they
    // come back each time it opens until the panel restarts or they are turned off (also by the cameras going away)
    previewCamera_ = previewCameraKept_;
    previewFreezePending_ = false;
    // (over whatever shows; the light warning before a recording doesn't stay behind it)
    eyecamConfirm_.close();
}

void EyePanel::setPreviewOptions(eye_preview::Source source, bool mirror, bool camera, bool frozen,
                                 const eye_preview::Marks& marks) {
    previewSource_ = source;
    previewMirror_ = mirror;
    previewCamera_ = camera;
    previewFrozen_ = frozen;
    previewFreezePending_ = frozen;
    previewMarks_ = marks;
}

void EyePanel::openAppsConfirm(const std::string& key) {
    if (!appsListOpen()) openAppsList();
    appsConfirmOpen_ = true;
    appsConfirmKey_ = key;
    appsLaunchResult_ = frame_apps::LaunchResult::Started;
}

void EyePanel::setAppsLaunchResult(frame_apps::LaunchResult result) {
    appsLaunchResult_ = result;
    // Opened: back to the list, where the app shows "Running in Konsole"
    if (result == frame_apps::LaunchResult::Started) appsConfirmOpen_ = false;
}

const frame_apps::Entry* EyePanel::appsConfirmEntry(const PanelModel& model) const {
    if (appsConfirmKey_.empty()) return nullptr;
    for (const frame_apps::Entry& entry : model.apps.entries) {
        if (entry.app.key == appsConfirmKey_) return &entry;
    }
    return nullptr;
}

std::string EyePanel::appsConfirmCommand(const PanelModel& model) const {
    // The same function as the one that makes Konsole's script, from the list now (not the built-in one)
    const frame_apps::Entry* entry = appsConfirmEntry(model);
    return frame_apps::installerCommand(entry != nullptr ? entry->app.key : std::string());
}

void EyePanel::setHistoryRow(const std::string& version) {
    historyRow_ = version;
    historyRowPending_ = false;
    historyReveal_ = false;
}

void EyePanel::setHistoryScroll(double px) {
    historyScroll_ = std::max(0.0, px);
    historyScrollSet_ = true;
    historyReveal_ = false;
}

bool EyePanel::scroll(double dy) {
    if (!wantsScroll() || promptOpen() || keypadOpen_) return false;
    if (appsListOpen_) {
        const double next = std::clamp(appsScroll_ + dy, 0.0, appsMaxScroll_);
        if (std::fabs(next - appsScroll_) < 1e-3) return false;
        appsScroll_ = next;
        return true;
    }
    if (!historyOpen_) {
        // All records, or the Advanced tab's page
        double& at = recordsAllOpen_ ? listScroll_ : advScroll_;
        const double next = std::clamp(at + dy, 0.0, recordsAllOpen_ ? listMaxScroll_ : advMaxScroll_);
        if (std::fabs(next - at) < 1e-3) return false;
        at = next;
        return true;
    }
    const double next = std::max(0.0, std::min(historyMaxScroll_, historyScroll_ + dy));
    if (std::fabs(next - historyScroll_) < 1e-3) return false;
    historyScroll_ = next;
    historyReveal_ = false;
    return true;
}

void EyePanel::showUpdatePrompt(const std::string& version, bool back) {
    updatePromptVersion_ = bareVersion(version);
    updatePromptBack_ = back;
    hover_ = {};
    pressed_ = {};
}

void EyePanel::armQuitForPreview() {
    quitArmed_ = true;
    quitArmedUntil_ = 1e300;
}

void EyePanel::armResetForPreview() {
    resetArmed_ = true;
    resetArmedUntil_ = 1e300;
}

void EyePanel::drawRowLabel(const Pen& pen, const UiText& t, double y, double h, const std::string& title,
                            const std::string& hint, bool locked, double indent, bool badge) {
    const double left = kInnerX + indent;
    const double width = kLabelW - indent;
    // The badge after the title (up to the gap before the controls), the title smaller to leave it room; a title too
    // long for that has it at the start of the hint line instead, so it never wraps or reaches the controls
    const double badgeW = badge ? cameraBadge(pen, t, 0, 0, false) + 8 : 0.0;
    const double reach = width + (badge ? 8 : 0);
    double titleSize = fitSize(pen, title, 20, 14, reach - badgeW, true);
    const bool badgeAfter = badge && pen.measure(title, titleSize, true) + badgeW <= reach;
    if (badge && !badgeAfter) titleSize = fitSize(pen, title, 20, 13, width, true);
    const bool hintLine = !hint.empty() || locked || (badge && !badgeAfter);
    const double titleBaseline = hintLine ? y + h / 2 - 3 : centerBaseline(y, h, titleSize);
    const double titleW = pen.text(left, titleBaseline, title, titleSize, kText, true);
    if (badgeAfter) cameraBadge(pen, t, left + titleW + 8, titleBaseline - titleSize * 0.36);
    if (!hintLine) return;
    const double hintY = y + h / 2 + 19;
    double x = left;
    if (badge && !badgeAfter) {
        cameraBadge(pen, t, x, hintY - 5);
        x += badgeW;
    }
    if (locked) {
        drawLock(pen, x, hintY, 16, kTextMuted);
        x += 20;
        pen.text(x, hintY, t.locked, fitSize(pen, t.locked, 15, 11, left + width - x, false), kTextMuted);
        return;
    }
    if (!hint.empty()) {
        const double size = fitSize(pen, hint, 15, 11, left + width - x, false);
        pen.text(x, hintY, ellipsize(pen, hint, size, false, left + width - x, false), size, kTextMuted);
    }
}

double EyePanel::cameraBadge(const Pen& pen, const UiText& t, double x, double cy, bool draw) {
    const double size = 12;
    const double h = 20;
    const double iconW = 14;
    const double w = 6 + iconW + 4 + pen.measure(t.cameraBadge, size, true) + 8;
    if (!draw) return w;
    cairo_t* cr = pen.cr;
    fillRounded(pen, x, cy - h / 2, w, h, h / 2, kSuccessTint);
    strokeRounded(pen, x, cy - h / 2, w, h, h / 2, kSuccess, 1);
    // The camera icon, smaller
    cairo_save(cr);
    cairo_translate(cr, x + 6 + iconW / 2, cy);
    cairo_scale(cr, 0.62, 0.62);
    drawCameraIcon(pen, 0, 1, kSuccess);
    cairo_restore(cr);
    pen.text(x + 6 + iconW + 4, centerBaseline(cy - h / 2, h, size), t.cameraBadge, size, kSuccess, true);
    return w;
}

double EyePanel::drawCameraLine(const Pen& pen, const std::string& text, double y) {
    cairo_t* cr = pen.cr;
    const double h = 48;
    const double x0 = kInnerX;
    const double w = kInnerRight - kInnerX;
    // Pressed: their tab (only while it is in the tab row; otherwise the line only says it)
    const PanelHit hit {PanelAction::Tab, nullptr, static_cast<int>(PanelTab::Eyecam)};
    const bool usable = eyecamTab_;
    if (usable && pointerState(hit) > 0) fillRounded(pen, x0, y, w, h, 12, kControlHover);
    const double dash[2] = {6, 4};
    cairo_set_dash(cr, dash, 2, 0);
    strokeRounded(pen, x0, y, w, h, 12, kBorder, 1.5);
    cairo_set_dash(cr, nullptr, 0, 0);
    drawCameraIcon(pen, x0 + 28, y + h / 2, kTextMuted);
    const double tx = x0 + 52;
    double right = x0 + w - 16;
    if (usable) {
        const double arrowW = pen.measure(ICON_ARROW_RIGHT, 18, false);
        pen.text(right - arrowW, centerBaseline(y, h, 18), ICON_ARROW_RIGHT, 18, kText);
        right -= arrowW + 12;
    }
    const double size = fitSize(pen, text, 16, 11, right - tx, false);
    pen.text(tx, centerBaseline(y, h, size), ellipsize(pen, text, size, false, right - tx, false), size, kText);
    addButton(hit, x0, y, w, h, usable);
    return h;
}

void EyePanel::drawSegmented(const Pen& pen, double x, double y, double w, double h,
                             const std::vector<Option>& options, int selected, double size, bool locked) {
    cairo_t* cr = pen.cr;
    const double r = h / 2;
    fillRounded(pen, x, y, w, h, r, kControl);
    bool anyUsable = false;
    for (const Option& option : options) anyUsable |= option.usable;
    if (anyUsable && !locked) strokeRounded(pen, x, y, w, h, r, kBorder, 2);

    const double inset = 5;
    const double segW = (w - inset * 2) / options.size();
    for (size_t i = 0; i < options.size(); ++i) {
        const Option& option = options[i];
        const double sx = x + inset + segW * i;
        const double sy = y + inset;
        const double sh = h - inset * 2;
        const bool usable = option.usable && !locked;
        const int pointer = usable ? pointerState(option.hit) : 0;
        const bool isSelected = static_cast<int>(i) == selected;
        Color textColor = usable ? kText : kTextDisabled;
        Color checkColor = kOnAccent;
        if (isSelected && locked) {
            // Locked: an outline instead of the fill, the value is still readable
            strokeRounded(pen, sx, sy, segW, sh, sh / 2, kBorder, 2);
            textColor = kTextMuted;
            checkColor = kTextMuted;
        } else if (isSelected) {
            fillRounded(pen, sx, sy, segW, sh, sh / 2, pointer == 2 ? kAccentPressed : kAccent);
            textColor = kOnAccent;
        } else if (pointer > 0) {
            fillRounded(pen, sx, sy, segW, sh, sh / 2, kControlHover);
        }
        const double checkW = isSelected ? size * 0.9 : 0;
        const double labelSize = fitSize(pen, option.label, size, size * 0.65, segW - 20 - checkW, isSelected);
        const double textW = pen.measure(option.label, labelSize, isSelected) + checkW;
        const double tx = sx + (segW - textW) / 2;
        if (isSelected) drawCheck(cr, tx + checkW * 0.4, sy + sh / 2, size * 0.72, checkColor);
        pen.text(tx + checkW, centerBaseline(sy, sh, labelSize), option.label, labelSize, textColor, isSelected);
        addButton(option.hit, sx, y, segW, h, usable);
    }
}

void EyePanel::drawStepper(const Pen& pen, double x, double y, double w, double h, const char* name, double value,
                           const std::string& text, bool usable, bool locked, double low, double high,
                           const Color* outline, const PanelHit* tap) {
    const SettingSpec* spec = findSetting(name);
    const bool active = usable && !locked;
    const double lower = spec != nullptr ? std::max(spec->min, low) : low;
    const double upper = spec != nullptr ? std::min(spec->max, high) : high;
    const bool canMinus = active && (!std::isfinite(value) || value > lower + 1e-9);
    const bool canPlus = active && (!std::isfinite(value) || value < upper - 1e-9);
    const double r = h / 2;
    fillRounded(pen, x, y, w, h, r, kControl);
    if (active) strokeRounded(pen, x, y, w, h, r, outline != nullptr ? *outline : kBorder, 2);
    const double buttonW = std::min(h, std::max(40.0, w * 0.28));
    const PanelHit minus {PanelAction::Step, name, -1};
    const PanelHit plus {PanelAction::Step, name, 1};
    for (int side = 0; side < 2; ++side) {
        const PanelHit& hit = side == 0 ? minus : plus;
        const bool can = side == 0 ? canMinus : canPlus;
        const double bx = side == 0 ? x : x + w - buttonW;
        const int pointer = can ? pointerState(hit) : 0;
        if (pointer > 0) {
            fillRounded(pen, bx + 4, y + 4, buttonW - 8, h - 8, (h - 8) / 2, pointer == 2 ? kAccentPressed : kControlHover);
        }
        const Color sign = pointer == 2 ? kOnAccent : (can ? kText : kTextDisabled);
        drawPlusMinus(pen.cr, bx + buttonW / 2, y + h / 2, 16, side == 1, sign);
        addButton(hit, bx, y, buttonW, h, can);
    }
    // Thin separators between the buttons and the value (decoration only)
    pen.color(kDivider);
    cairo_set_line_width(pen.cr, 1);
    cairo_move_to(pen.cr, x + buttonW + 0.5, y + 10);
    cairo_line_to(pen.cr, x + buttonW + 0.5, y + h - 10);
    cairo_move_to(pen.cr, x + w - buttonW - 0.5, y + 10);
    cairo_line_to(pen.cr, x + w - buttonW - 0.5, y + h - 10);
    cairo_stroke(pen.cr);
    const double size = fitSize(pen, text, 20, 12, w - buttonW * 2 - 12, true);
    const Color color = locked ? kTextMuted : (usable ? kText : kTextDisabled);
    if (tap != nullptr && active) {
        drawValueTap(pen, *tap, x + buttonW, y, w - 2 * buttonW, h, pen.measure(text, size, true));
    }
    textCentered(pen, x + w / 2, centerBaseline(y, h, size), text, size, color, true);
}

void EyePanel::drawValueTap(const Pen& pen, const PanelHit& hit, double x, double y, double w, double h,
                            double textW) {
    const int pointer = pointerState(hit);
    if (pointer > 0) {
        fillRounded(pen, x + 4, y + 4, w - 8, h - 8, (h - 8) / 2, pointer == 2 ? kAccentTint : kControlHover);
    }
    // (a line under the value: it can be typed)
    pen.color(pointer > 0 ? kAccent : kTextMuted);
    cairo_set_line_width(pen.cr, 2);
    cairo_move_to(pen.cr, x + (w - textW) / 2, y + h - 12);
    cairo_line_to(pen.cr, x + (w + textW) / 2, y + h - 12);
    cairo_stroke(pen.cr);
    addButton(hit, x, y, w, h);
}

void EyePanel::drawCaption(const Pen& pen, double x, double baseline, const std::string& text, int number,
                           bool locked) {
    double tx = x;
    if (number > 0) {
        drawNumberBadge(pen, x + 10, baseline - 5, number);
        tx += 26;
    }
    tx += pen.text(tx, baseline, text, 15, kText, false);
    if (locked) drawLock(pen, tx + 8, baseline + 1, 15, kTextMuted);
}

void EyePanel::drawEyeRateOffer(const Pen& pen, const UiText& t, double x0, double x1, double baseline) {
    const PanelHit hit {PanelAction::EyeRateSet, nullptr, 0};
    const int pointer = pointerState(hit);
    // The pill in the line's own height (under the rate, above the line before the eyelids), the words before it
    const double pillSize = 12;
    const double pillH = 18;
    const double pillTop = baseline - 12.5;
    const double pillW = pen.measure(t.eyeRateSet, pillSize, true) + 18;
    const double pillX = x1 - pillW;
    const double room = pillX - 8 - x0;
    const double minSize = 11;
    if (pen.measure(t.eyeRateSlow, minSize, false) <= room) {
        fillRounded(pen, pillX, pillTop, pillW, pillH, pillH / 2, pointer == 2 ? kAccentPressed : kAccent);
        textCentered(pen, pillX + pillW / 2, centerBaseline(pillTop, pillH, pillSize), t.eyeRateSet, pillSize,
                     kOnAccent, true);
        const double size = fitSize(pen, t.eyeRateSlow, 14, minSize, room, false);
        pen.text(pillX - 8, baseline, t.eyeRateSlow, size, pointer > 0 ? kText : kTextMuted, false, true);
    } else {
        // Too long beside it: the words alone, in the accent, with a chevron
        const std::string line = std::string(t.eyeRateSlow) + " " ICON_CHEVRON_RIGHT;
        const double size = fitSize(pen, line, 14, minSize, x1 - x0, true);
        pen.text(x1, baseline, ellipsize(pen, line, size, true, x1 - x0, false), size,
                 pointer == 2 ? kAccentPressed : kAccent, true, true);
    }
    // (pressed anywhere on the line, or on the rate's row above it: nothing else there can be)
    addButton(hit, x0, baseline - 25, x1 - x0, 32, true);
}

void EyePanel::drawStatus(const Pen& pen, const UiText& t, const PanelModel& m) {
    cairo_t* cr = pen.cr;
    const EyeStatus& s = m.status;
    drawCard(pen, kStatusX, kStatusY, kStatusW, kStatusH, 20, kCard, kDivider, 1);
    const double x0 = kStatusX + 20;
    const double x1 = kStatusX + kStatusW - 20;
    pen.text(x0, 66, t.title, fitSize(pen, t.title, 26, 18, x1 - x0, true), kText, true);
    // While the eye log records: a red mark with the time, right of the title, so it isn't forgotten
    if (m.recording.recording) {
        const std::string text = formatText(t.recordingFormat, recorder::elapsedText(m.recording.elapsedSec));
        const double size = 16;
        const double w = pen.measure(text, size, true);
        pen.text(x1 - w, 64, text, size, kDanger, true);
        drawDot(cr, x1 - w - 12, 58, 5.5, kDanger);
    }

    // Badge: a symbol and a word, never color alone
    {
        enum class Kind { Sending, Waiting, Paused, NotRunning };
        Kind kind = Kind::NotRunning;
        if (s.running) kind = !s.sending ? Kind::Paused : (s.tracking ? Kind::Sending : Kind::Waiting);
        const char* label = kind == Kind::Sending   ? t.badgeSending
                            : kind == Kind::Waiting ? t.badgeWaiting
                            : kind == Kind::Paused  ? t.badgePaused
                                                    : t.badgeNotRunning;
        const Color fill = kind == Kind::Sending ? kSuccessTint : (kind == Kind::NotRunning ? kDangerTint : kControl);
        const Color fg = kind == Kind::Sending ? kSuccess : (kind == Kind::NotRunning ? kDanger : kTextMuted);
        const double y = 84;
        const double h = 40;
        const double size = fitSize(pen, label, 18, 13, x1 - x0 - 56, true);
        const double w = std::min(x1 - x0, pen.measure(label, size, true) + 56);
        fillRounded(pen, x0, y, w, h, h / 2, fill);
        const double ix = x0 + 22;
        const double iy = y + h / 2;
        switch (kind) {
            case Kind::Sending: drawDot(cr, ix, iy, 6.5, fg); break;
            case Kind::Waiting: drawRing(cr, ix, iy, 5.5, 2.5, fg); break;
            case Kind::Paused:
                fillRounded(pen, ix - 6, iy - 7, 4, 14, 1, fg);
                fillRounded(pen, ix + 2, iy - 7, 4, 14, 1, fg);
                break;
            case Kind::NotRunning:
                pen.color(fg);
                cairo_set_line_width(cr, 3);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                cairo_move_to(cr, ix - 5, iy - 5);
                cairo_line_to(cr, ix + 5, iy + 5);
                cairo_move_to(cr, ix + 5, iy - 5);
                cairo_line_to(cr, ix - 5, iy + 5);
                cairo_stroke(cr);
                break;
        }
        // The "not running" word is light on the red tint (red text there is only 4.35:1); the cross stays red
        pen.text(x0 + 40, centerBaseline(y, h, size), label, size, kind == Kind::NotRunning ? kText : fg, true);
    }

    // Destination and rate. More destinations (the pupils straight to VRChat, other apps) get a line each under the
    // main one, whose automatic / fixed then goes on the label's line; everything under them moves down for them,
    // the gaze boxes getting smaller (and the eyelid rows closer when there are many)
    const double labelW = pen.text(x0, 154, t.destination, 15, kTextMuted, true);
    const int more = s.running ? drawMoreDestinations(pen, t, m, x0, x1, 0, kDestStep, false) : 0;
    const TrackerRateCause cause = trackerRateCause(s);
    // (the line under the eye data's rate also stays while "Set 90"'s confirmation or "not taken" shows)
    const bool slowRow = eye_rate::rowShown(m.eyeRate, cause);
    const StatusLayout rows = statusLayout(more, slowRow, false, false);
    const double shift = rows.shift;
    if (!s.running) {
        pen.text(x0, 180, t.notRunningHint1, fitSize(pen, t.notRunningHint1, 16, 12, x1 - x0, false), kText);
        pen.text(x0, 204, t.notRunningHint2, fitSize(pen, t.notRunningHint2, 16, 12, x1 - x0, false), kText);
    } else {
        // (anything else is VRChat: an older frameeyeosc's "etvr" too, as 0.10 reads it)
        const char* receiver = s.output == kOutputLivelink ? t.outputLivelinkShort
                               : s.output == kOutputModule ? t.outputModuleShort
                                                           : t.outputVrchatShort;
        // Refused (lan_only, a broadcast address): the host as set in red with a cross, and why in place of
        // automatic / fixed
        const bool refused = !s.refused.empty();
        const std::string host = SettingsView(m).text(key::kHost);
        const std::string where = refused ? std::string(ICON_CROSS " ") + host
                                  : s.target.empty() ? std::string(t.searchingPc)
                                                     : s.target;
        const std::string destination = std::string(receiver) + " " ICON_ARROW_RIGHT " " + where;
        const double tagW = refused ? 0.0 : drawNetTag(pen, t, s.net, x1, 180, 12, false);
        const double room = x1 - x0 - (tagW > 0 ? tagW + 6 : 0);
        const double size = fitSize(pen, destination, 19, 13, room, true);
        pen.text(x0, 180, ellipsize(pen, destination, size, true, room, false), size, refused ? kDanger : kText, true);
        if (tagW > 0) drawNetTag(pen, t, s.net, x1, 180, 12);
        const std::string mode = refused ? extraReasonText(t, s.refused)
                                         : std::string(s.targetMode == "fixed" ? t.modeFixed : t.modeAuto);
        const Color modeColor = refused ? kDanger : kTextMuted;
        if (more == 0) {
            pen.text(x0, 204, ellipsize(pen, mode, 15, refused, x1 - x0, false), 15, modeColor, refused);
        } else {
            const double modeRoom = x1 - x0 - labelW - 12;
            const double modeSize = fitSize(pen, mode, 14, 10, modeRoom, refused);
            pen.text(x1, 154, ellipsize(pen, mode, modeSize, refused, modeRoom, false), modeSize, modeColor, refused,
                     true);
            drawMoreDestinations(pen, t, m, x0, x1, 180 + kDestStep + 1, kDestStep);
        }
    }
    pen.text(x0, 228 + shift, t.rateLabel, 15, kTextMuted, true);
    {
        char rate[64];
        std::snprintf(rate, sizeof(rate), t.rateFormat, s.rate);
        pen.text(x1, 228 + shift, s.running ? std::string(rate) : std::string("—"), 17, kText, true, true);
    }
    // How fast the eye tracker delivers samples, sent or not: it has been seen at 15 and 46 a second instead of 90+
    pen.text(x0, 250 + shift, t.trackerRateLabel, 15, kTextMuted, true);
    {
        const bool known = s.running && s.tracking && std::isfinite(s.trackerRate);
        const bool low = known && s.trackerRate < kLowTrackerRate;
        char rate[64];
        std::snprintf(rate, sizeof(rate), t.rateFormat, s.trackerRate);
        const std::string text = known ? (low ? std::string(t.trackerRateLowHint) + "  " + rate : std::string(rate))
                                       : std::string("—");
        pen.text(x1, 250 + shift, text, 17, low ? kDanger : kText, true, true);
    }
    // While it is low, a line under it says who was slow: frameeyeosc or the eye tracker. The eyelids make room for it
    // (they move down into the space above the gaze)
    // (with many more destinations the eyelid rows get closer, and the gaze moves down that much less: statusLayout)
    const double dy = rows.dy;
    const double g = rows.g;
    // SteamVR running the eye cameras slowly (its eyeTrackingRateMin below 90): that line, with "Set 90", in the reason's
    // place; after the press, that it was set, or that SteamVR didn't take it (eye_rate)
    const eye_rate::Line rateLine = m.eyeRate.line;
    if (rateLine == eye_rate::Line::Offer && cause == TrackerRateCause::Tracker) {
        drawEyeRateOffer(pen, t, x0, x1, 269 + shift);
    } else if (rateLine == eye_rate::Line::Done || rateLine == eye_rate::Line::Refused) {
        const bool done = rateLine == eye_rate::Line::Done;
        const char* line = done ? t.eyeRateDone : t.eyeRateRefused;
        pen.text(x1, 269 + shift, line, fitSize(pen, line, 14, 11, x1 - x0, true), done ? kSuccess : kDanger, true,
                 true);
    } else if (cause != TrackerRateCause::None) {
        const char* line = cause == TrackerRateCause::Here ? t.trackerRateSlowHere : t.trackerRateSlowTracker;
        pen.text(x1, 269 + shift, line, fitSize(pen, line, 14, 11, x1 - x0, false), kTextMuted, false, true);
    }
    pen.color(kDivider);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, x0, 262.5 + dy);
    cairo_line_to(cr, x1, 262.5 + dy);
    cairo_stroke(cr);

    // Eyelids: a thin gray bar for the raw value, a thick accent bar for the sent value
    const bool live = s.running && s.tracking;
    const double lidsTitleRight = x0 + pen.text(x0, 282 + dy, t.lidsTitle, 15, kTextMuted, true);
    {
        const double sentW = pen.measure(t.legendSent, 14, false);
        const double rawW = pen.measure(t.legendRaw, 14, false);
        double lx = x1 - sentW;
        pen.text(lx, 282 + dy, t.legendSent, 14, kText);
        lx -= 26;
        fillRounded(pen, lx, 272 + dy, 20, 10, 3, kAccent);
        lx -= 18 + rawW;
        pen.text(lx, 282 + dy, t.legendRaw, 14, kText);
        lx -= 26;
        fillRounded(pen, lx, 274 + dy, 20, 6, 2, kTextMuted);
        // Where they come from, after the title: Valve's values, or the eye cameras (as frameeyeosc says)
        const char* from = nullptr;
        switch (cameraUse(s, SettingsView(m).flag(key::kCameraLids))) {
            case CameraUse::Both: from = t.lidsFromCamera; break;
            case CameraUse::Left: from = t.lidsFromCameraLeft; break;
            case CameraUse::Right: from = t.lidsFromCameraRight; break;
            case CameraUse::Unknown: from = s.running ? t.lidsFromValve : nullptr; break;
            default: from = t.lidsFromValve; break;
        }
        if (from != nullptr) {
            const double fx = lidsTitleRight + 8;
            pen.text(fx, 282 + dy, from, fitSize(pen, from, 13, 10, lx - 12 - fx, false), kTextMuted);
        }
    }
    for (int eye = 0; eye < 2; ++eye) {
        const double top = rows.eyeTop[eye];
        pen.text(x0, top + 26, eye == 0 ? t.left : t.right, 18, kText, true);
        const double barX = x0 + 30;
        const double barW = x1 - 58 - barX;
        const double raw = live && s.hasRaw ? s.opennessScaled.v[eye] : NAN;
        const double sent = live && s.hasSent ? s.lids.v[eye] : NAN;
        // Raw (thin)
        fillRounded(pen, barX, top + 4, barW, 10, 5, kBg);
        if (std::isfinite(raw)) fillRounded(pen, barX, top + 4, std::max(10.0, barW * std::clamp(raw, 0.0, 1.0)), 10, 5, kTextMuted);
        strokeRounded(pen, barX, top + 4, barW, 10, 5, kBorder, 1);
        // Sent (thick)
        fillRounded(pen, barX, top + 20, barW, 16, 8, kBg);
        if (std::isfinite(sent)) fillRounded(pen, barX, top + 20, std::max(16.0, barW * std::clamp(sent, 0.0, 1.0)), 16, 8, kAccent);
        strokeRounded(pen, barX, top + 20, barW, 16, 8, kBorder, 1);
        pen.text(x1, top + 14, twoDecimals(raw), 14, kTextMuted, false, true);
        pen.text(x1, top + 36, twoDecimals(sent), 16, kText, true, true);
    }
    pen.color(kDivider);
    cairo_move_to(cr, x0, rows.divider);
    cairo_line_to(cr, x1, rows.divider);
    cairo_stroke(cr);

    // Gaze: raw = ring, sent = filled dot
    pen.text(x0, 430 + g, t.gazeTitle, 15, kTextMuted, true);
    // "Track Dominant Eye Only": the Frame tracks one eye, and both eyes get its gaze. Said on the title
    // line, right-aligned (the per-eye legend gives it its place)
    const char* oneEye = !s.running              ? nullptr
                         : s.dominantEye == "left"  ? t.dominantEyeLeft
                         : s.dominantEye == "right" ? t.dominantEyeRight
                                                    : nullptr;
    if (oneEye != nullptr) {
        const double room = x1 - x0 - pen.measure(t.gazeTitle, 15, true) - 14;
        const double size = fitSize(pen, oneEye, 14, 10, room, false);
        pen.text(x1 - pen.measure(oneEye, size, false), 430 + g, oneEye, size, kText);
    }
    /**
     * A gaze pad: crosshair, a circle at half range, the raw gaze as a ring and the sent gaze as a dot, seen the way
     * the user looks (+x right, +y up).
     */
    const auto gazePad = [&](double bx, double by, double box, const Pair& raw, const Pair& sent, Color dot) {
        const double cx = bx + box / 2;
        const double cy = by + box / 2;
        const bool hasRaw = live && s.hasRaw && raw.valid();
        const bool hasSent = live && s.hasSent && sent.valid();
        const double scale = box / 164;
        fillRounded(pen, bx, by, box, box, 12, kBg);
        pen.color(kDivider, hasRaw || hasSent ? 1.0 : 0.0);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, cx + 0.5, by + 8);
        cairo_line_to(cr, cx + 0.5, by + box - 8);
        cairo_move_to(cr, bx + 8, cy + 0.5);
        cairo_line_to(cr, bx + box - 8, cy + 0.5);
        cairo_stroke(cr);
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, box / 4, 0, 2 * M_PI);
        cairo_stroke(cr);
        strokeRounded(pen, bx, by, box, box, 12, kBorder, 1.5);
        const double half = box / 2 - 12;
        /**
         * Map gaze (-1..1, up positive) into the box.
         */
        const auto point = [&](const Pair& p, double& px, double& py) {
            px = cx + std::clamp(p.v[0], -1.0, 1.0) * half;
            py = cy - std::clamp(p.v[1], -1.0, 1.0) * half;
        };
        double px = 0;
        double py = 0;
        if (hasSent) {
            point(sent, px, py);
            drawDot(cr, px, py, 8 * std::max(scale, 0.8), dot);
        }
        if (hasRaw) {
            point(raw, px, py);
            drawRing(cr, px, py, 11 * std::max(scale, 0.8), 2.5, kText);
        }
        if (!hasRaw && !hasSent) {
            const double size = fitSize(pen, t.noEyeData, 14, 10, box - 16, false);
            const std::vector<std::string> lines = wrapText(pen, t.noEyeData, size, false, box - 16, 2);
            for (size_t i = 0; i < lines.size(); ++i) {
                textCentered(pen, cx, cy + 5 + (i - (lines.size() - 1) / 2.0) * (size + 4), lines[i], size,
                             kTextMuted, false);
            }
        }
        return hasSent;
    };
    // The eye cameras' setup at the bottom (what to do next, or that it is done) makes the gaze pads smaller; an error
    // message there wins over it
    const bool failure = m.panelErrorBroken || !m.panelError.empty() || !m.config.error.empty() ||
                         m.autostart.writeFailed || (s.running && (!s.sourceError.empty() || !s.configError.empty()));
    const eyecam::SetupScreen setup = eyecam::setupScreen(m.eyecam);
    const bool nextCard = m.eyecam.visible &&
                          ((setup != eyecam::SetupScreen::Camera && setup != eyecam::SetupScreen::Done) ||
                           eyecam::toolNotice(m.eyecam.status, m.eyecam.password) != eyecam::ToolNotice::None);
    const bool readyCard = m.eyecam.visible && m.eyecam.readyNotice;
    const bool setupCard = !failure && (nextCard || readyCard);
    // Each eye's own pad while the eyes move separately; one pad for the shared gaze otherwise
    const bool perEye = SettingsView(m).flag(key::kIndependentEyes);
    const StatusLayout gazeRows = statusLayout(more, slowRow, perEye, setupCard);
    if (perEye) {
        // The legend on the title line: ring = raw, the two eye colors = sent
        if (oneEye == nullptr) {
            const double sentW = pen.measure(t.legendSent, 14, false);
            const double rawW = pen.measure(t.legendRaw, 14, false);
            double lx = x1 - sentW;
            pen.text(lx, 430 + g, t.legendSent, 14, kText);
            lx -= 14;
            drawDot(cr, lx, 425 + g, 5, kDotRight);
            lx -= 12;
            drawDot(cr, lx, 425 + g, 5, kDotLeft);
            lx -= 18 + rawW;
            pen.text(lx, 430 + g, t.legendRaw, 14, kText);
            lx -= 14;
            drawRing(cr, lx, 425 + g, 6, 2, kText);
        }
        const double box = gazeRows.box;
        const char* labels[2] = {t.leftEye, t.rightEye};
        const Color colors[2] = {kDotLeft, kDotRight};
        for (int eye = 0; eye < 2; ++eye) {
            // The left eye on the left, as the user sees it (not mirrored)
            const double bx = eye == 0 ? x0 : x1 - box;
            pen.text(bx + 2, 456 + g, labels[eye], 15, kText, true);
            const bool hasSent = gazePad(bx, 464 + g, box, s.rawGazeEye[eye], s.sentGazeEye[eye], colors[eye]);
            const std::string xy = hasSent ? xyText(s.sentGazeEye[eye]) : "—";
            // (the right eye's against the column's right edge: the boxes get small with many destinations)
            const double xySize = fitSize(pen, xy, 14, 10, box, false);
            if (eye == 0) {
                pen.text(bx + 2, 464 + g + box + 20, xy, xySize, kTextMuted);
            } else {
                pen.text(x1, 464 + g + box + 20, xy, xySize, kTextMuted, false, true);
            }
        }
    } else {
        const double box = gazeRows.box;
        const double bx = x0;
        const double by = 444 + g;
        gazePad(bx, by, box, s.gaze, s.sentGaze, kAccent);
        const bool hasRaw = live && s.hasRaw && s.gaze.valid();
        const bool hasSent = live && s.hasSent && s.sentGaze.valid();
        const double lx = bx + box + 18;
        // The legend beside the box, its four lines closer together when moved down (above the bottom row, or the
        // eye cameras' setup there)
        const double first = gazeRows.legendFirst;
        const double k = gazeRows.legendK;
        drawRing(cr, lx + 9, first - 6, 8, 2.5, kText);
        pen.text(lx + 26, first, t.legendRaw, 15, kText);
        pen.text(lx, first + 26 * k, hasRaw ? xyText(s.gaze) : "—",
                 fitSize(pen, xyText(s.gaze), 15, 11, x1 - lx, false), kTextMuted);
        drawDot(cr, lx + 9, first + 64 * k, 7, kAccent);
        pen.text(lx + 26, first + 70 * k, t.legendSent, 15, kText);
        pen.text(lx, first + 96 * k, hasSent ? xyText(s.sentGaze) : "—",
                 fitSize(pen, xyText(s.sentGaze), 15, 11, x1 - lx, false), kTextMuted);
    }

    // One red message at the bottom: the panel's own failure first, then why frameeyeosc can't read the eye tracker,
    // then its config error
    std::string message;
    if (m.panelErrorBroken) {
        message = t.errConfigBroken;
    } else if (!m.panelError.empty()) {
        message = std::string(t.errWrite) + m.panelError;
    } else if (!m.config.error.empty()) {
        message = t.errConfigBroken;
    } else if (m.autostart.writeFailed) {
        message = t.errAutostart;
    } else if (s.running && !s.sourceError.empty()) {
        message = std::string(t.sourceErrorPrefix) + s.sourceError;
    } else if (s.running && !s.configError.empty()) {
        message = std::string(t.errorPrefix) + s.configError;
    }
    // The eye preview's button at the very bottom: wide with its label on its own, or a square with the eye alone at
    // the right of the message or notice there (they make room for it)
    const bool notice = updateNotice(m.update);
    const bool shared = !message.empty() || setupCard || notice;
    const double square = 44;
    const double right = shared ? x1 - square - 8 : x1;
    if (!message.empty()) {
        const std::vector<std::string> lines = wrapText(pen, message, 15, true, right - x0, 2);
        for (size_t i = 0; i < lines.size(); ++i) pen.text(x0, 636 + i * 22, lines[i], 15, kDanger, true);
    } else if (setupCard) {
        drawSetupNotice(pen, t, m, setup, x0, right);
    } else {
        drawUpdateNotice(pen, t, m.update, x0, right);
    }
    if (shared) {
        drawPreviewButton(pen, t, x1 - square, 670 - square, square, square, false);
    } else {
        drawPreviewButton(pen, t, x0, 626, x1 - x0, 44, true);
    }
}

void EyePanel::drawSetupNotice(const Pen& pen, const UiText& t, const PanelModel& m, eyecam::SetupScreen screen,
                               double x0, double x1) {
    using eyecam::SetupScreen;
    const double h = 80;
    const double y = 670 - h;
    const double w = x1 - x0;
    // Right after the setup: it is ready (green, for a short while)
    if (m.eyecam.readyNotice && (screen == SetupScreen::Done || screen == SetupScreen::Camera)) {
        fillRounded(pen, x0, y, w, h, 16, kSuccessTint);
        strokeRounded(pen, x0, y, w, h, 16, kSuccess, 1.5);
        drawCheck(pen.cr, x0 + 26, y + h / 2, 20, kSuccess);
        pen.text(x0 + 50, y + 35, t.readyTitle, fitSize(pen, t.readyTitle, 18, 12, w - 62, true), kSuccess, true);
        pen.text(x0 + 50, y + 59, t.readyNote, fitSize(pen, t.readyNote, 14, 10, w - 62, false), kText);
        return;
    }
    // What to do next, as a button to the eye cameras tab
    // Set up, with an outdated or too old tool: install it again (from the usual page; the cameras may be stopped); or
    // dsp_service not answering: install it
    const eyecam::ToolNotice notice = eyecam::toolNotice(m.eyecam.status, m.eyecam.password);
    if (notice != eyecam::ToolNotice::None && (screen == SetupScreen::Done || screen == SetupScreen::Camera)) {
        const bool tooOld = notice == eyecam::ToolNotice::TooOld;
        const bool dsp = notice == eyecam::ToolNotice::DspUnreachable;
        const PanelHit hit {PanelAction::Tab, nullptr, static_cast<int>(PanelTab::Eyecam)};
        const bool usable = tab_ != PanelTab::Eyecam;
        const int pointer = usable ? pointerState(hit) : 0;
        const Color edge = tooOld ? kDanger : kAccent;
        fillRounded(pen, x0, y, w, h, 16, pointer > 0 ? kControlHover : (tooOld ? kDangerTint : kCard));
        strokeRounded(pen, x0, y, w, h, 16, edge, 2);
        const double textW = w - 48;
        pen.text(x0 + 18, y + 23, t.nextTitle, fitSize(pen, t.nextTitle, 14, 10, textW, true), edge, true);
        const char* name = dsp ? t.setupStepTool : t.setupStepToolAgain;
        pen.text(x0 + 18, y + 48, name, fitSize(pen, name, 18, 12, textW, true), kText, true);
        const char* sub = dsp ? t.nextToolDsp : tooOld ? t.nextToolTooOld : t.nextToolOutdated;
        pen.text(x0 + 18, y + 70, sub, fitSize(pen, sub, 14, 10, textW, false), tooOld ? kText : kTextMuted);
        drawChevron(pen, x1 - 22, y + h / 2, 14, 2.5, edge);
        addButton(hit, x0, y, w, h, usable);
        return;
    }
    // Not set up, the tool outdated or too old: (2) again
    const eyecam::Tool tool = eyecam::toolState(m.eyecam.status);
    const bool outdated = tool == eyecam::Tool::Outdated || tool == eyecam::Tool::TooOld;
    const char* names[3] = {t.setupStepPassword, outdated ? t.setupStepToolAgain : t.setupStepTool, t.setupStepLearn};
    const char* numbers[3] = {"①", "②", "③"};
    const int step = screen == SetupScreen::Pass ? 0 : screen == SetupScreen::Check ? 1 : 2;
    std::string sub;
    switch (screen) {
        case SetupScreen::Pass: sub = t.nextPass; break;
        case SetupScreen::Check: sub = outdated ? t.setupToolUpdated : t.setupLaterTool; break;
        case SetupScreen::Learn: {
            const eyecam::Status& s = m.eyecam.status;
            if (std::isfinite(s.stepRemainingS) && !s.stepLabel.empty()) {
                char text[160];
                std::snprintf(text, sizeof(text), t.nextLearnFormat,
                              static_cast<int>(std::ceil(std::max(0.0, s.stepRemainingS) - 1e-9)));
                sub = text;
            } else {
                sub = t.setupWaitHint2;
            }
            break;
        }
        case SetupScreen::Fail: sub = t.nextFail; break;
        case SetupScreen::Error: sub = t.nextError; break;
        default: sub = t.nextWait; break;
    }
    const PanelHit hit {PanelAction::Tab, nullptr, static_cast<int>(PanelTab::Eyecam)};
    const bool usable = tab_ != PanelTab::Eyecam;
    const int pointer = usable ? pointerState(hit) : 0;
    fillRounded(pen, x0, y, w, h, 16, pointer > 0 ? kControlHover : kCard);
    strokeRounded(pen, x0, y, w, h, 16, kAccent, 2);
    const double textW = w - 48;
    pen.text(x0 + 18, y + 23, t.nextTitle, fitSize(pen, t.nextTitle, 14, 10, textW, true), kAccent, true);
    const std::string title = std::string(numbers[step]) + " " + names[step];
    pen.text(x0 + 18, y + 48, title, fitSize(pen, title, 18, 12, textW, true), kText, true);
    pen.text(x0 + 18, y + 70, sub, fitSize(pen, sub, 14, 10, textW, false), kTextMuted);
    drawChevron(pen, x1 - 22, y + h / 2, 14, 2.5, kAccent);
    addButton(hit, x0, y, w, h, usable);
}

void EyePanel::drawUpdateNotice(const Pen& pen, const UiText& t, const frame_updater::UpdateStatus& u, double x0,
                                double x1) {
    if (!updateNotice(u)) return;
    std::string text;
    switch (u.state) {
        case frame_updater::UpdateState::Available:
            text = formatText(u.prerelease ? t.updateAvailableTestFormat : t.updateAvailableFormat, bareVersion(u.latest));
            break;
        case frame_updater::UpdateState::Installing:
            text = formatText(t.updateInstallingFormat, updateStepText(t, u.step));
            break;
        case frame_updater::UpdateState::Installed:
            text = formatText(t.updateInstalledFormat, bareVersion(u.version));
            break;
        default: return;
    }
    // Opens the Advanced tab on "Version"
    const PanelHit hit {PanelAction::AdvancedPage, nullptr, static_cast<int>(AdvPage::Version)};
    const bool usable = !(advPageShown() && advPage_ == AdvPage::Version);
    const double size = 16;
    const std::vector<std::string> lines = wrapText(pen, text, size, true, x1 - x0 - 58, 2);
    const double h = lines.size() > 1 ? 54 : 42;
    const double y = 670 - h;  // below the gaze box, above the card's bottom edge
    const int pointer = usable ? pointerState(hit) : 0;
    fillRounded(pen, x0, y, x1 - x0, h, 22, pointer > 0 ? kControlHover : kAccentTint);
    strokeRounded(pen, x0, y, x1 - x0, h, 22, kAccent, 2);
    drawDot(pen.cr, x0 + 22, y + h / 2, 6, kAccent);
    const double top = y + (h - lines.size() * 20) / 2;
    for (size_t i = 0; i < lines.size(); ++i) pen.text(x0 + 38, top + 15 + i * 20, lines[i], size, kText, true);
    addButton(hit, x0, y, x1 - x0, h, usable);
}

void EyePanel::drawTabs(const Pen& pen, const UiText& t) {
    // The eye cameras (only while eyecam-rec runs) before Advanced
    std::vector<PanelTab> tabs = {PanelTab::Basic, PanelTab::Output, PanelTab::Gaze, PanelTab::EyeFit, PanelTab::Lids};
    if (eyecamTab_) tabs.push_back(PanelTab::Eyecam);
    tabs.push_back(PanelTab::Advanced);
    std::vector<const char*> labels;
    for (const PanelTab tab : tabs) {
        labels.push_back(tab == PanelTab::Basic    ? t.tabBasic
                         : tab == PanelTab::Output ? t.tabOutput
                         : tab == PanelTab::Gaze   ? t.tabGaze
                         : tab == PanelTab::EyeFit ? t.tabGazeFit
                         : tab == PanelTab::Lids   ? t.tabLids
                         : tab == PanelTab::Eyecam ? t.tabEyecam
                                                   : t.tabAdvanced);
    }
    const int count = static_cast<int>(labels.size());
    const double gap = 8;
    const double pad = 16;
    const double room = kRight - kRightX - gap * (count - 1);
    // One text size for every tab, the largest at which all labels fit with their padding; the space left is shared
    // out evenly, so a long label (目を合わせる) gets a wider tab instead of smaller text
    const auto needed = [&](double size) {
        double sum = 0;
        for (const char* label : labels) sum += pen.measure(label, size, true) + pad * 2;
        return sum;
    };
    double size = 21;
    while (size > 14 && needed(size) > room) size -= 1;
    const double extra = std::max(0.0, room - needed(size)) / count;
    double x = kRightX;
    for (int i = 0; i < count; ++i) {
        const double w = pen.measure(labels[i], size, true) + pad * 2 + extra;
        const PanelHit hit {PanelAction::Tab, nullptr, static_cast<int>(tabs[i])};
        // (none while the eye preview is over the content: any of them closes it)
        const bool selected = tab_ == tabs[i] && !previewOpen_;
        const int pointer = pointerState(hit);
        if (selected) {
            // The chosen tab: accent fill, bold text and a notch pointing at the content
            fillRounded(pen, x, kTabY, w, kTabH, kTabH / 2, kAccent);
            pen.color(kAccent);
            cairo_move_to(pen.cr, x + w / 2 - 11, kTabY + kTabH - 1);
            cairo_line_to(pen.cr, x + w / 2 + 11, kTabY + kTabH - 1);
            cairo_line_to(pen.cr, x + w / 2, kTabY + kTabH + 11);
            cairo_close_path(pen.cr);
            cairo_fill(pen.cr);
            textCentered(pen, x + w / 2, centerBaseline(kTabY, kTabH, size), labels[i], size, kOnAccent, true);
        } else {
            fillRounded(pen, x, kTabY, w, kTabH, kTabH / 2, pointer > 0 ? kControlHover : kControl);
            strokeRounded(pen, x, kTabY, w, kTabH, kTabH / 2, kBorder, 2);
            textCentered(pen, x + w / 2, centerBaseline(kTabY, kTabH, size), labels[i], size, kText, false);
        }
        addButton(hit, x, kTabY, w, kTabH, !selected);
        x += w + gap;
    }
}

void EyePanel::drawBasic(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    double y = kRowTop;
    const double cy = (kRowH - kControlH) / 2;

    // Sending
    {
        const bool locked = v.locked(key::kSending);
        drawRowLabel(pen, t, y, kRowH, t.rowSending, t.hintSending, locked);
        drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                      {{t.send, {PanelAction::SetBool, key::kSending, 1}}, {t.stop, {PanelAction::SetBool, key::kSending, 0}}},
                      v.flag(key::kSending) ? 0 : 1, 20, locked);
    }
    y += kRowH + kRowGap;
    // Where to send: three cards
    y += drawOutputCards(pen, t, v, y) + kRowGap;
    // The eye cameras: the way to set them up, or their switch
    drawEyeCamerasRow(pen, t, m, v, y, kRowH);
    y += kRowH + kRowGap;
    // Reset all / quit (both ask for a second press), at the right end of the next two rows
    const auto dangerButton = [&](bool isQuit, double rowY) {
        const PanelHit hit {isQuit ? PanelAction::Quit : PanelAction::ResetAll, nullptr, 0};
        const bool armed = isQuit ? quitArmed_ : resetArmed_;
        const double x = kControlX + 300 + 14;
        const double w = kInnerRight - x;
        const double by = rowY + cy;
        const int pointer = pointerState(hit);
        const std::string label = isQuit ? (armed ? t.quitConfirm : t.quit) : (armed ? t.resetConfirm : t.resetAll);
        fillRounded(pen, x, by, w, kControlH, kControlH / 2, armed ? kDanger : (pointer > 0 ? kControlHover : kQuitFill));
        strokeRounded(pen, x, by, w, kControlH, kControlH / 2, kDanger, 2);
        const double size = fitSize(pen, label, 19, 12, w - 24, true);
        textCentered(pen, x + w / 2, centerBaseline(by, kControlH, size), label, size, armed ? kOnAccent : kText, true);
        addButton(hit, x, by, w, kControlH);
    };
    // Language (each name in its own language)
    {
        drawRowLabel(pen, t, y, kRowH, t.rowLanguage, "", false);
        drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                      {{"日本語", {PanelAction::Language, key::kLanguage, 0}},
                       {"English", {PanelAction::Language, key::kLanguage, 1}}},
                      m.language == Language::Ja ? 0 : 1, 20);
        dangerButton(false, y);
    }
    y += kRowH + kRowGap;
    // Start with SteamVR
    {
        const Autostart a = m.autostart.autostart;
        const bool usable = a == Autostart::Enabled || a == Autostart::Disabled;
        const char* hint = a == Autostart::Missing ? t.autostartMissing
                           : a == Autostart::Unknown ? t.autostartUnknown
                                                     : t.hintAutostart;
        drawRowLabel(pen, t, y, kRowH, t.rowAutostart, hint, false);
        drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                      {{t.on, {PanelAction::AutostartOn, nullptr, 0}, usable},
                       {t.off, {PanelAction::AutostartOff, nullptr, 0}, usable}},
                      a == Autostart::Enabled ? 0 : (a == Autostart::Disabled ? 1 : -1), 20);
        dangerButton(true, y);
    }
    y += kRowH + 22;
    // What pressing does, where there is room for it
    const std::vector<std::string> lines = wrapText(pen, t.footer, 15, false, kInnerRight - kInnerX, 2);
    if (y + (lines.size() - 1) * 22 + 6 <= kContentY + kContentH - 10) {
        for (size_t i = 0; i < lines.size(); ++i) pen.text(kInnerX, y + i * 22, lines[i], 15, kTextMuted);
    }
}

void EyePanel::drawEyeCamerasRow(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v,
                                  double y, double h) {
    const double cy = (h - kControlH) / 2;
    if (!eyecam::setUp(m.eyecam)) {
        // Not set up: what they add, that they aren't in use, and the way to their tab (where the setup starts)
        drawRowLabel(pen, t, y, h, t.rowEyeCameras, t.hintEyeCamerasNew, false);
        double right = kInnerRight;
        if (eyecamTab_) {
            const std::string label = std::string(t.eyeCamerasStart) + "  " ICON_ARROW_RIGHT;
            const double bh = 46;
            const double bw = std::max(150.0, pen.measure(label, 18, true) + 44);
            drawButton(pen, kInnerRight - bw, y + (h - bh) / 2, bw, bh, label,
                       {PanelAction::Tab, nullptr, static_cast<int>(PanelTab::Eyecam)}, true, false, 18);
            right = kInnerRight - bw - 16;
        }
        const double size = fitSize(pen, t.eyeCamerasUnused, 18, 12, right - kControlX - 4, false);
        pen.text(kControlX + 4, centerBaseline(y, h, size), t.eyeCamerasUnused, size, kTextMuted);
        return;
    }
    // Set up: the switch (camera_lids), and how they are doing after it
    const bool locked = v.locked(key::kCameraLids);
    const bool on = v.flag(key::kCameraLids);
    drawRowLabel(pen, t, y, h, t.rowEyeCameras, t.hintEyeCamerasOn, locked, 0, true);
    const double switchW = 300;
    drawSegmented(pen, kControlX, y + cy, switchW, kControlH,
                  {{t.eyeCamerasUse, {PanelAction::SetBool, key::kCameraLids, 1}},
                   {t.eyeCamerasDontUse, {PanelAction::SetBool, key::kCameraLids, 0}}},
                  on ? 0 : 1, 20, locked);
    const CameraBrief brief = cameraBrief(m.status, on, eyecam::baselineWarming(m.eyecam.status));
    const char* text = brief == CameraBrief::Both      ? t.camBriefBoth
                       : brief == CameraBrief::Left    ? t.camBriefLeft
                       : brief == CameraBrief::Right   ? t.camBriefRight
                       : brief == CameraBrief::Off     ? t.camBriefOff
                       : brief == CameraBrief::Warming ? t.camBriefWarming
                                                       : t.camBriefWaiting;
    const bool good = brief == CameraBrief::Both || brief == CameraBrief::Left || brief == CameraBrief::Right;
    const double dx = kControlX + switchW + 20;
    drawDot(pen.cr, dx + 5, y + h / 2, 5.5, good ? kSuccess : kTextMuted);
    const double tx = dx + 18;
    const double size = fitSize(pen, text, 17, 11, kInnerRight - tx, good);
    pen.text(tx, centerBaseline(y, h, size), ellipsize(pen, text, size, good, kInnerRight - tx, false), size,
             good ? kText : kTextMuted, good);
}

double EyePanel::drawOutputCards(const Pen& pen, const UiText& t, const SettingsView& v, double y) {
    const bool locked = v.locked(key::kOutput);
    const std::string output = v.text(key::kOutput);
    const int selected = argOfOutput(output);
    // The title, and after it what "sync" on the cards means (or the lock note). The cards take the whole width
    // under it, so their names and lines stay large enough to read
    const double titleSize = fitSize(pen, t.rowOutput, 20, 14, kLabelW, true);
    const double titleBaseline = y + 28;
    const double hintX = kInnerX + pen.measure(t.rowOutput, titleSize, true) + 16;
    pen.text(kInnerX, titleBaseline, t.rowOutput, titleSize, kText, true);
    if (locked) {
        drawLock(pen, hintX, titleBaseline, 16, kTextMuted);
        pen.text(hintX + 20, titleBaseline, t.locked, fitSize(pen, t.locked, 15, 11, kInnerRight - hintX - 20, false),
                 kTextMuted);
    } else {
        pen.text(hintX, titleBaseline, t.hintOutput, fitSize(pen, t.hintOutput, 15, 11, kInnerRight - hintX, false),
                 kTextMuted);
    }

    // The cards in the order VRChat, LiveLink, FrameEyeOSC (their button arguments are 0, 2, 3; 1 was ETVR)
    const int args[3] = {0, 2, 3};
    const char* titles[3] = {t.outputVrchat, t.outputLivelink, t.outputModule};
    const double gap = 12;
    const double cardW = (kInnerRight - kInnerX - gap * 2) / 3;
    const double pad = 14;
    const double innerW = cardW - pad * 2;
    const double top = y + 44;  // room for the "Recommended" tag, which sits on the card's top edge
    // One title size for all three cards
    double cardTitleSize = 20;
    for (const char* title : titles) cardTitleSize = std::min(cardTitleSize, fitSize(pen, title, 20, 14, innerW, true));
    // Each card's lines: a drawn mark on the first line of a marked text, the rest indented after it
    const double lineSize = 16;
    const double lineStep = 21;
    const double markW = 20;
    struct Line {
        LineMark mark;
        bool indent;
        std::string text;
    };
    std::vector<Line> lines[3];
    size_t most = 0;
    for (int c = 0; c < 3; ++c) {
        for (const char* text : t.outputMarks[c]) {
            std::string rest;
            const LineMark mark = splitMark(text, rest);
            const double indent = mark == LineMark::None ? 0 : markW;
            const std::vector<std::string> wrapped = wrapText(pen, rest, lineSize, false, innerW - indent, 2);
            for (size_t i = 0; i < wrapped.size(); ++i) {
                lines[c].push_back({i == 0 ? mark : LineMark::None, mark != LineMark::None, wrapped[i]});
            }
        }
        most = std::max(most, lines[c].size());
    }
    const double cardH = pad + cardTitleSize + 8 + most * lineStep + pad - 4;

    for (int c = 0; c < 3; ++c) {
        const double cx = kInnerX + c * (cardW + gap);
        const bool isSelected = selected == args[c];
        const PanelHit hit {PanelAction::SetOutput, key::kOutput, args[c]};
        const bool usable = !locked;
        const int pointer = usable ? pointerState(hit) : 0;
        Color fill = kControl;
        if (isSelected && !locked) {
            fill = kAccentTint;
        } else if (pointer > 0) {
            fill = kControlHover;
        }
        fillRounded(pen, cx, top, cardW, cardH, 14, fill);
        if (isSelected) {
            // Chosen: a thick outline (gray while locked) and a check badge on the top-left corner
            strokeRounded(pen, cx, top, cardW, cardH, 14, locked ? kBorder : kAccent, 3);
            drawDot(pen.cr, cx + 22, top, 12, locked ? kBorder : kAccent);
            drawCheck(pen.cr, cx + 22, top, 15, kOnAccent);
        } else if (!locked) {
            strokeRounded(pen, cx, top, cardW, cardH, 14, kBorder, 2);
        }
        const Color titleColor = locked ? kTextMuted : kText;
        // Muted lines only on the plain card; on the accent tint or the hover fill that would be too faint
        const Color lineColor = locked ? kTextDisabled : (isSelected || pointer > 0 ? kText : kTextMuted);
        pen.text(cx + pad, top + pad + cardTitleSize * 0.9, titles[c], cardTitleSize, titleColor, true);
        double baseline = top + pad + cardTitleSize + 8 + lineSize * 0.9;
        for (const Line& line : lines[c]) {
            const double tx = cx + pad + (line.indent ? markW : 0);
            const double markX = cx + pad + markW * 0.4;
            if (line.mark == LineMark::Check) drawCheck(pen.cr, markX, baseline - lineSize * 0.36, lineSize, kText);
            pen.text(tx, baseline, line.text, lineSize, lineColor);
            baseline += lineStep;
        }
        // LiveLink (recommended) and FrameEyeOSC (all-in-one) get a tag on their top edge
        const char* tag = args[c] == 2 ? t.outputRecommended : args[c] == 3 ? t.outputAllInOne : nullptr;
        if (tag != nullptr) {
            const double tagSize = 13;
            const double tagH = 22;
            const double tagW = pen.measure(tag, tagSize, true) + 16;
            const double tagX = cx + cardW - tagW - 10;
            fillRounded(pen, tagX, top - tagH / 2, tagW, tagH, 7, kAccent);
            textCentered(pen, tagX + tagW / 2, centerBaseline(top - tagH / 2, tagH, tagSize), tag, tagSize, kOnAccent,
                         true);
        }
        addButton(hit, cx, top, cardW, cardH, usable);
    }
    double by = top + cardH;

    // VRChat directly: an avatar that takes its eye parameters as bits (or with other names) doesn't move; VRCFT fits
    // them to each avatar. Plain grey: a way out, not a warning
    if (selected == argOfOutput(kOutputVrchat)) {
        by += 10;
        const double textW = kInnerRight - kInnerX;
        pen.text(kInnerX + 4, by + 15, t.vrchatDirectHint, fitSize(pen, t.vrchatDirectHint, 15, 11, textW - 4, false),
                 kTextMuted);
        by += 28;
    }
    return by - y + 8;
}

void EyePanel::drawOutput(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    const EyeStatus& s = m.status;
    // (under the sub-tabs)
    double y = kAdvPageTop;
    const std::string output = v.text(key::kOutput);
    const bool livelink = output == kOutputLivelink;
    const bool module = output == kOutputModule;
    const bool direct = !livelink && !module;
    // VRChat directly has the most rows: all of them shorter there (the other outputs' as on the other tabs)
    const double rowH = direct ? 50 : kRowH;
    const double controlH = direct ? 42 : kControlH;
    const double cy = (rowH - controlH) / 2;
    // ...and a line under a control (an example address, a hint) closer to it, the row with that line
    const double lineDrop = direct ? 12 : 16;
    const double lineRowH = rowH + (direct ? 16 : 24);
    // The pupils come from the eye cameras: their rows only while those are in use (with the badge), else one line
    const bool camera = eyecam::inUse(m.eyecam, v.flag(key::kCameraLids));

    // Wherever the pupils go straight to VRChat: how the avatar takes them, a float or that many bits (pupil_bits). The
    // hint under the control, like the examples above
    const auto pupilBits = [&](double rowY) {
        const PupilBitsRow row = pupilBitsRow(output, camera, v.flag(key::kPupilsToVrchat));
        if (row == PupilBitsRow::Hidden) return rowY;
        const bool locked = v.locked(key::kPupilBits);
        const bool usable = row == PupilBitsRow::Usable;
        const int bits = static_cast<int>(std::lround(v.number(key::kPupilBits)));
        drawRowLabel(pen, t, rowY, rowH, t.rowPupilBits, "", locked, 0, true);
        std::vector<Option> options;
        for (int n = 0; n <= 4; ++n) {
            options.push_back({n == 0 ? std::string(t.pupilBitsFloat) : std::to_string(n),
                               {PanelAction::SetInteger, key::kPupilBits, n}, usable});
        }
        // Greyed like a locked control (the choice outlined, not filled), so it doesn't look in effect
        drawSegmented(pen, kControlX, rowY + cy, kControlW, controlH, options, bits >= 0 && bits <= 4 ? bits : -1, 19,
                      locked || !usable);
        const char* hint = usable ? t.hintPupilBits : t.hintPupilBitsOff;
        pen.text(kControlX + 4, rowY + rowH + lineDrop, hint, fitSize(pen, hint, 15, 11, kControlW, false), kTextMuted);
        return rowY + lineRowH;
    };

    // Target PC: automatic, fixed to the PC frameeyeosc sends to now, or typed on the keypad; any host set by hand
    // shows in the third choice
    {
        const bool locked = v.locked(key::kHost);
        const std::string host = v.text(key::kHost);
        const bool isAuto = host.empty() || host == "auto";
        const bool canFix = s.running && !hostOfTarget(s.target).empty();
        drawRowLabel(pen, t, y, rowH, t.rowTarget, t.hintTarget, locked);
        drawSegmented(pen, kControlX, y + cy, kControlW, controlH,
                      {{t.targetAuto, {PanelAction::HostAuto, key::kHost, 0}},
                       {t.targetFixNow, {PanelAction::FixHost, key::kHost, 0}, canFix},
                       {isAuto ? std::string(t.targetEnter) : formatText(t.targetManualFormat, host),
                        {PanelAction::HostEnter, key::kHost, 0}}},
                      isAuto ? 0 : 2, 19, locked);
    }
    y += rowH + kRowGap;
    // Port, with the chosen output's default as the hint
    {
        const bool locked = v.locked(key::kPort);
        const bool isDefault = v.portIsDefault();
        const char* hint = livelink ? t.portDefaultLivelink : module ? t.portDefaultModule : t.portDefaultVrchat;
        drawRowLabel(pen, t, y, rowH, t.rowPort, hint, locked);
        // (the value opens the keypad: a port like 11112 is typed, − and ＋ nudge it)
        const PanelHit tap {PanelAction::PortEnter, key::kPort, v.port()};
        drawStepper(pen, kControlX, y + cy, 220, controlH, key::kPort, v.port(), std::to_string(v.port()), true,
                    locked, -1e9, 1e9, nullptr, &tap);
        if (!isDefault && !locked) {
            const PanelHit hit {PanelAction::PortDefault, key::kPort, 0};
            const double bx = kControlX + 232;
            const double bw = 170;
            const int pointer = pointerState(hit);
            fillRounded(pen, bx, y + cy, bw, controlH, controlH / 2, pointer > 0 ? kControlHover : kControl);
            strokeRounded(pen, bx, y + cy, bw, controlH, controlH / 2, kBorder, 2);
            const double size = fitSize(pen, t.portReset, 18, 12, bw - 24, false);
            textCentered(pen, bx + bw / 2, centerBaseline(y + cy, controlH, size), t.portReset, size, kText, false);
            addButton(hit, bx, y + cy, bw, controlH);
        }
    }
    y += rowH + kRowGap;

    if (direct) {
        // VRChat directly: the parameter prefix...
        {
            const bool locked = v.locked(key::kPrefix);
            const std::string prefix = v.text(key::kPrefix);
            const bool none = prefix.empty() || prefix == "/";
            const int selected = prefix == "/FT" ? 0 : (none ? 1 : -1);
            const std::string hint = selected < 0 ? formatText(t.prefixOther, prefix) : "";
            drawRowLabel(pen, t, y, rowH, t.rowPrefix, hint, locked);
            drawSegmented(pen, kControlX, y + cy, 300, controlH,
                          {{"/FT", {PanelAction::PrefixFt, key::kPrefix, 0}},
                           {t.prefixNone, {PanelAction::PrefixNone, key::kPrefix, 0}}},
                          selected, 20, locked);
            std::string path = prefix;
            while (!path.empty() && path.back() == '/') path.pop_back();
            const std::string example = std::string(t.prefixExample) + "/avatar/parameters" + path + "/v2/EyeLeftX";
            pen.text(kControlX + 4, y + rowH + lineDrop, example, fitSize(pen, example, 15, 11, kControlW, false),
                     kTextMuted);
        }
        y += lineRowH;
        // ...and how EyeTrackingActive goes out
        {
            const bool locked = v.locked(key::kEyeTrackingActive);
            const std::string type = v.text(key::kEyeTrackingActive);
            int selected = -1;
            for (int i = 0; i < 3; ++i) {
                if (type == kActiveTypes[i]) selected = i;
            }
            drawRowLabel(pen, t, y, rowH, t.rowActiveType, t.hintActiveType, locked);
            drawSegmented(pen, kControlX, y + cy, kControlW, controlH,
                          {{"Bool", {PanelAction::SetActiveType, key::kEyeTrackingActive, 0}},
                           {"Float", {PanelAction::SetActiveType, key::kEyeTrackingActive, 1}},
                           {t.activeOff, {PanelAction::SetActiveType, key::kEyeTrackingActive, 2}}},
                          selected, 19, locked);
        }
        y += rowH + kRowGap;
        // ...and, for avatars made for Steam Link's own OSC, its names too (never with the prefix)
        {
            const bool locked = v.locked(key::kSteamlinkParams);
            drawRowLabel(pen, t, y, rowH, t.rowSteamlink, t.hintSteamlink, locked);
            drawSegmented(pen, kControlX, y + cy, 300, controlH,
                          {{t.on, {PanelAction::SetBool, key::kSteamlinkParams, 1}},
                           {t.off, {PanelAction::SetBool, key::kSteamlinkParams, 0}}},
                          v.flag(key::kSteamlinkParams) ? 0 : 1, 20, locked);
            const std::string example = std::string(t.prefixExample) + "/avatar/parameters/LeftEyeX" + t.steamlinkNoPrefix;
            pen.text(kControlX + 4, y + rowH + lineDrop, example, fitSize(pen, example, 15, 11, kControlW, false),
                     kTextMuted);
        }
        y += lineRowH;
        // ...and VRChat's own eye tracking input, for avatars without VRCFT parameters
        {
            const bool locked = v.locked(key::kNativeEyes);
            // The hint is too long for the label column: it goes under the control, like the example addresses above
            drawRowLabel(pen, t, y, rowH, t.rowNativeEyes, "", locked);
            drawSegmented(pen, kControlX, y + cy, 300, controlH,
                          {{t.on, {PanelAction::SetBool, key::kNativeEyes, 1}},
                           {t.off, {PanelAction::SetBool, key::kNativeEyes, 0}}},
                          v.flag(key::kNativeEyes) ? 0 : 1, 21, locked);
            pen.text(kControlX + 4, y + rowH + lineDrop, t.hintNativeEyes,
                     fitSize(pen, t.hintNativeEyes, 15, 11, kControlW, false), kTextMuted);
        }
        y += lineRowH;
        // ...and VRChat's own face tracking input (/tracking/face/v1), for avatars with Face Tracking set up
        {
            const bool locked = v.locked(key::kVrcFaceTracking);
            // (its hint under the control too)
            drawRowLabel(pen, t, y, rowH, t.rowVrcFaceTracking, "", locked);
            drawSegmented(pen, kControlX, y + cy, 300, controlH,
                          {{t.on, {PanelAction::SetBool, key::kVrcFaceTracking, 1}},
                           {t.off, {PanelAction::SetBool, key::kVrcFaceTracking, 0}}},
                          v.flag(key::kVrcFaceTracking) ? 0 : 1, 21, locked);
            pen.text(kControlX + 4, y + rowH + lineDrop, t.hintVrcFaceTracking,
                     fitSize(pen, t.hintVrcFaceTracking, 15, 11, kControlW, false), kTextMuted);
        }
        y += lineRowH;
        if (pupilsLineShown(output, camera)) {
            // (a little further down than elsewhere: the hint above sits closer to its row here)
            drawCameraLine(pen, t.cameraLinePupils, y + 6);
        } else {
            pupilBits(y);
        }
        return;
    }

    // LiveLink with the eye cameras: their pupils straight to VRChat (the LiveLink module has none). The hint and where
    // they go now under the control, like the VRChat rows' examples
    if (livelink && pupilsLineShown(output, camera)) y += drawCameraLine(pen, t.cameraLinePupils, y + 2) + 4;
    // FrameEyeOSC carries the pupils itself (through VRCFT): only the line while the eye cameras aren't in use
    if (module && pupilsLineShown(output, camera)) y += drawCameraLine(pen, t.cameraLinePupils, y + 2) + 4;
    if (livelink && pupilsRowShown(output, camera)) {
        const bool locked = v.locked(key::kPupilsToVrchat);
        const bool on = v.flag(key::kPupilsToVrchat);
        // (where they go now under its title, once frameeyeosc says)
        const std::string target =
            on && s.running && !s.pupilTarget.empty() ? formatText(t.pupilsTargetFormat, s.pupilTarget) : std::string();
        drawRowLabel(pen, t, y, rowH, t.rowPupils, target, locked, 0, true);
        drawSegmented(pen, kControlX, y + cy, 300, controlH,
                      {{t.on, {PanelAction::SetBool, key::kPupilsToVrchat, 1}},
                       {t.off, {PanelAction::SetBool, key::kPupilsToVrchat, 0}}},
                      on ? 0 : 1, 21, locked);
        pen.text(kControlX + 4, y + rowH + lineDrop, t.hintPupils, fitSize(pen, t.hintPupils, 15, 11, kControlW, false),
                 kTextMuted);
        y += lineRowH;
        y = pupilBits(y);
    }

    // LiveLink and FrameEyeOSC: what to set up in VRCFT on the PC (nothing else is set here)
    const double boxX = kInnerX;
    const double boxW = kInnerRight - kInnerX;
    const double pad = 20;
    const double top = y + 10;
    const double stepSize = 17;
    const double stepStep = 26;
    const double badgeW = 32;
    const double textW = boxW - pad * 2 - badgeW;
    const char* const* steps = livelink ? t.vrcftStepsLivelink : t.vrcftStepsModule;
    std::vector<std::vector<std::string>> stepLines;
    size_t stepLineCount = 0;
    for (int i = 0; i < 4 && steps[i] != nullptr; ++i) {
        stepLines.push_back(wrapText(pen, steps[i], stepSize, false, textW, 2));
        stepLineCount += stepLines.back().size();
    }
    // FrameEyeOSC isn't in VRCFT's registry: where its zip is, in the accent color under the first step
    const bool link = module && t.vrcftModuleLink != nullptr;
    if (link) ++stepLineCount;
    // ...and, when pressing it couldn't open the browser, why, in red right under it (one line)
    const bool linkFailed = link && moduleLinkErrorShown(output, m.moduleLinkError);
    if (linkFailed) ++stepLineCount;
    std::string note = t.vrcftSetupNote;
    const char* more = module && camera ? t.vrcftModulePupils : nullptr;
    if (more != nullptr) {
        // A space between English sentences, none after "。"
        if (!note.empty() && note.back() == '.') note += " ";
        note += more;
    }
    std::vector<std::string> noteLines = wrapText(pen, note, 15, false, boxW - pad * 2, 3);
    const double titleSize = fitSize(pen, t.vrcftSetupTitle, 19, 14, boxW - pad * 2, true);
    const auto boxHeight = [&] {
        return pad + titleSize + 14 + stepLineCount * stepStep + 8 + noteLines.size() * 21 + pad - 6;
    };
    // The note goes where it doesn't fit (under LiveLink's two pupil rows, should they grow): the steps matter
    if (top + boxHeight() > kContentY + kContentH - 12) noteLines.clear();
    const double boxH = boxHeight();
    strokeRounded(pen, boxX, top, boxW, boxH, 14, kDivider, 2);
    double baseline = top + pad + titleSize * 0.9;
    pen.text(boxX + pad, baseline, t.vrcftSetupTitle, titleSize, kText, true);
    baseline += 14 + stepStep * 0.75;
    for (size_t i = 0; i < stepLines.size(); ++i) {
        drawNumberBadge(pen, boxX + pad + 10, baseline - 6, static_cast<int>(i) + 1);
        for (const std::string& line : stepLines[i]) {
            pen.text(boxX + pad + badgeW, baseline, line, stepSize, kText);
            baseline += stepStep;
        }
        if (i == 0 && link) {
            // Pressed, the page opens in the Frame's Chromium (the zip is for the PC, but the page says what to do)
            const std::string label = std::string(t.vrcftModuleLink) + " " ICON_CHEVRON_RIGHT;
            const double size = fitSize(pen, label.c_str(), stepSize, 13, textW - 12, true);
            const PanelHit hit {PanelAction::OpenModulePage, nullptr, 0};
            const double bx = boxX + pad + badgeW - 8;
            const double bw = pen.measure(label, size, true) + 16;
            const double bh = 40;  // the panel's smallest button: it reaches over the steps around it
            const double by = baseline - size * 0.8 - (bh - size) / 2;
            if (pointerState(hit) > 0) fillRounded(pen, bx, by, bw, bh, 10, kControlHover);
            pen.text(boxX + pad + badgeW, baseline, label, size, kAccent, true);
            addButton(hit, bx, by, bw, bh, true);
            baseline += stepStep;
            if (linkFailed) {
                const std::string failed = formatText(t.vrcftModuleLinkFailedFormat, m.moduleLinkError);
                pen.text(boxX + pad + badgeW, baseline - 2, ellipsize(pen, failed, 14, true, textW, false), 14, kDanger,
                         true);
                baseline += stepStep;
            }
        }
    }
    baseline += 8 - stepStep * 0.75 + 21 * 0.8;
    for (const std::string& line : noteLines) {
        pen.text(boxX + pad, baseline, line, 15, kTextMuted);
        baseline += 21;
    }
}

std::string extraReasonText(const UiText& t, const std::string& reason) {
    static const struct {
        const char* reason;
        const char* UiText::*text;
    } kReasons[] = {
        {"same as main", &UiText::reasonSameAsMain},
        {"same as the pupils", &UiText::reasonSameAsPupils},
        {"duplicate", &UiText::reasonDuplicate},
        {"public address", &UiText::reasonPublicAddress},
        {"broadcast refused", &UiText::reasonBroadcast},
        {"multicast refused", &UiText::reasonMulticast},
        {"not an address to send to", &UiText::reasonNotAddress},
        {"no Steam Link", &UiText::reasonNoSteamLink},
    };
    for (const auto& known : kReasons) {
        if (reason == known.reason) return t.*known.text;
    }
    // ...and the ones with frameeyeosc's detail after them (the system's words, kept as they are)
    static const struct {
        const char* prefix;
        const char* UiText::*format;
    } kDetailed[] = {
        {"could not resolve: ", &UiText::reasonResolveFormat},
        {"can't send yet: ", &UiText::reasonCantSendFormat},
        {"sending failed: ", &UiText::reasonSendFailedFormat},
    };
    for (const auto& known : kDetailed) {
        const std::string prefix = known.prefix;
        if (reason.compare(0, prefix.size(), prefix) == 0) {
            return formatText(t.*known.format, reason.substr(prefix.size()));
        }
    }
    // An entry frameeyeosc can't use (config_error's words): written by a newer version, or after the first three
    if (reason.find("newer frameeyeosc") != std::string::npos) return t.reasonNewerEntry;
    if (reason.rfind("only ", 0) == 0 && reason.find("extra outputs are used") != std::string::npos) {
        return t.reasonOverLimit;
    }
    return reason;
}

StatusLayout statusLayout(int more, bool slow, bool perEye, bool setupCard) {
    StatusLayout l;
    l.shift = std::max(0.0, std::clamp(more, 0, 4) * kDestStep - 24);
    l.dy = l.shift + (slow ? 14 : 0);
    // Many lines: each eyelid row 8 px closer to the one before (the bars still apart)
    const double squeeze = l.shift >= 40 ? 16 : 0;
    for (int eye = 0; eye < 2; ++eye) l.eyeTop[eye] = 296 + l.dy + eye * (48 - squeeze / 2);
    l.divider = std::max(402.5 + l.shift - squeeze, l.eyeTop[1] + 36 + 8.5);
    l.g = l.divider - 402.5;
    l.limit = setupCard ? 590 : 626;
    if (perEye) {
        l.box = std::max(48.0, (setupCard ? 100 : 132) - l.g);
        l.lowest = 464 + l.g + l.box + 20;
    } else {
        l.box = std::max(64.0, (setupCard ? 132 : 164) - l.g);
        // (the legend's four lines closer together, and higher up beside the box, when there is less room under it)
        const double end = setupCard ? 580 : 612;
        l.legendFirst = std::max(444 + l.g + 18, std::min(444 + l.g + 32, end - 64));
        l.legendK = std::min(96.0, end - l.legendFirst) / 96;
        l.lowest = l.legendFirst + 96 * l.legendK;
    }
    return l;
}

const char* extrasSaveLabel(const UiText& t, const extras::Draft& draft, const extras::List& list) {
    if (draft.edit < 0) return t.extrasAddSave;
    for (const extras::Entry& entry : list.entries) {
        if (entry.index == draft.edit && !entry.enabled) return t.extrasSaveAndSend;
    }
    return t.extrasEditSave;
}

void EyePanel::drawOutputTab(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    // Two sub-tabs, drawn as the Gaze, Eyelids and Advanced tabs' (the first time the main destination, then the one
    // open last)
    const std::vector<Option> options = {
        {t.outputPageMain, {PanelAction::OutputPage, nullptr, static_cast<int>(OutputPage::Main)}},
        {t.outputPageApps, {PanelAction::OutputPage, nullptr, static_cast<int>(OutputPage::Apps)}},
    };
    drawPageSwitch(pen, kSubTabY, kSubTabH, options, static_cast<int>(outputPage_), -1);
    if (outputPage_ == OutputPage::Apps) {
        drawExtras(pen, t, m, v);
    } else {
        drawOutput(pen, t, m, v);
    }
}

int EyePanel::drawMoreDestinations(const Pen& pen, const UiText& t, const PanelModel& m, double x0, double x1,
                                   double y, double step, bool draw) {
    const EyeStatus& s = m.status;
    struct Line {
        std::string name;
        std::string format;  ///< "" for the pupils
        std::string where;   ///< the address, or why nothing goes there
        std::string net;     ///< its tag's kind
        bool bad;
    };
    std::vector<Line> lines;
    // The pupils straight to VRChat (LiveLink), then each extra output that is turned on: one line each, four at most
    // (five with the main one)
    if (!s.pupilTarget.empty() || !s.pupilRefused.empty()) {
        const bool bad = !s.pupilRefused.empty();
        lines.push_back({t.pupilsLine, "", bad ? extraReasonText(t, s.pupilRefused) : s.pupilTarget,
                         bad ? std::string() : s.pupilNet, bad});
    }
    if (s.extrasKnown) {
        for (const ExtraLive& e : s.extras) {
            if (!e.enabled || lines.size() >= 4) continue;
            const std::string& why = !e.refused.empty() ? e.refused : e.lastError;
            const bool bad = !why.empty();
            const std::string address = !e.target.empty() ? e.target : e.host + ":" + std::to_string(e.port);
            lines.push_back({e.name, formatShort(e.format), bad ? extraReasonText(t, why) : address,
                             bad ? std::string() : e.net, bad});
        }
    }
    if (!draw) return static_cast<int>(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& line = lines[i];
        const double base = y + static_cast<double>(i) * step;
        const Color color = line.bad ? kDanger : kText;
        // + for one that sends, x for one that doesn't (never the color alone)
        double x = x0 + pen.text(x0, base, line.bad ? ICON_CROSS : ICON_PLUS, 13, color, true) + 5;
        const double tagW = drawNetTag(pen, t, line.net, x1, base, 11, false);
        const double right = x1 - (tagW > 0 ? tagW + 6 : 0);
        x += pen.text(x, base, ellipsize(pen, line.name, 14, true, 120, false), 14, color, true) + 6;
        if (!line.format.empty() && !line.bad) {
            // The format in a small chip, monospaced like its name in config.json (not on a red line: room for why)
            const double fw = pen.measureMono(line.format, 11) + 10;
            fillRounded(pen, x, base - 12, fw, 16, 4, kControl);
            pen.textMono(x + 5, base - 0.5, line.format, 11, kTextMuted);
            x += fw + 6;
        }
        const std::string where = std::string(ICON_ARROW_RIGHT " ") + line.where;
        pen.text(x, base, ellipsize(pen, where, 13, line.bad, right - x, false), 13, line.bad ? kDanger : kTextMuted,
                 line.bad);
        if (tagW > 0) drawNetTag(pen, t, line.net, x1, base, 11);
    }
    return static_cast<int>(lines.size());
}

void EyePanel::drawExtras(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    const EyeStatus& s = m.status;
    const extras::List list = extrasList(m);
    const bool locked = v.locked(key::kExtraOutputs);
    // What the presses need: the entries as drawn, each as config.json has it, and lan_only
    extrasShown_ = list.entries;
    extrasRaw_.clear();
    for (const extras::Entry& entry : list.entries) extrasRaw_.push_back(extras::rawEntry(m.config.root, entry.index));
    extrasLanOnly_ = v.flag(key::kLanOnly);
    // Locked by the command line (--extra, --no-extras), or not a list: nothing to add or change
    if (locked || !list.isList) closeExtrasFlow();
    // A row asking whether to delete it: only while it is still there as it was
    if (extrasRowDelete_ >= 0 && (extrasRowDelete_ >= static_cast<int>(extrasRaw_.size()) ||
                                  extrasRaw_[static_cast<size_t>(extrasRowDelete_)] != extrasRowDeleteOriginal_)) {
        extrasRowDelete_ = -1;
    }
    if (extrasStep_ != ExtrasStep::List) {
        drawExtrasFlow(pen, t, m, v);
        return;
    }
    const double w = kInnerRight - kInnerX;
    double y = kAdvPageTop;
    // The title (the lock at its right while the command line sets the list) and what it is
    double titleRight = kInnerRight;
    if (locked) {
        const double lw = pen.measure(t.locked, 15, false);
        pen.text(kInnerRight - lw, y + 20, t.locked, 15, kTextMuted);
        drawLock(pen, kInnerRight - lw - 20, y + 20, 16, kTextMuted);
        titleRight -= lw + 32;
    }
    pen.text(kInnerX, y + 20, t.extrasTitle, fitSize(pen, t.extrasTitle, 20, 14, titleRight - kInnerX, true), kText,
             true);
    pen.text(kInnerX, y + 44, t.extrasSub, fitSize(pen, t.extrasSub, 15, 11, w, false), kTextMuted);
    y += 58;
    // An older frameeyeosc runs: the list is kept for it, but nothing goes out until it is updated
    if (s.running && !s.extrasKnown) {
        drawInfoIcon(pen, kInnerX + 9, y + 9, kText);
        pen.text(kInnerX + 26, y + 14, t.extrasOldCore, fitSize(pen, t.extrasOldCore, 15, 11, w - 26, false), kText);
        y += 30;
    }
    if (!list.isList) {
        pen.text(kInnerX, y + 16, t.extrasNotList, fitSize(pen, t.extrasNotList, 15, 11, w, true), kDanger, true);
        y += 30;
    }
    const int count = static_cast<int>(list.entries.size());
    for (int i = 0; i < std::min(count, extras::kMax); ++i) {
        drawExtraRow(pen, t, m, list.entries[static_cast<size_t>(i)], y, kExtraRowH, locked);
        y += kExtraRowH + kExtraRowGap;
    }
    // A delete from a row that couldn't be written (config.json changed meanwhile)
    if (!extrasError_.empty()) {
        pen.text(kInnerX, y + 14, ellipsize(pen, extrasError_, 15, true, w, false), 15, kDanger, true);
        y += 26;
    }
    // More than are used (written by hand): only said
    if (count > extras::kMax) {
        char text[200];
        std::snprintf(text, sizeof(text), t.extrasMoreFormat, count - extras::kMax);
        pen.text(kInnerX, y + 14, text, fitSize(pen, text, 15, 11, w, false), kTextMuted);
        y += 26;
    }
    if (list.isList && count < extras::kMax) {
        // "Add": a dashed outline in the accent across the card
        const PanelHit hit {PanelAction::ExtraAdd, nullptr, 0};
        const bool usable = !locked;
        const double h = 48;
        if (usable && pointerState(hit) > 0) fillRounded(pen, kInnerX, y, w, h, h / 2, kControlHover);
        const double dash[2] = {7, 5};
        cairo_set_dash(pen.cr, dash, 2, 0);
        strokeRounded(pen, kInnerX, y, w, h, h / 2, usable ? kAccent : kBorder, 2);
        cairo_set_dash(pen.cr, nullptr, 0, 0);
        char label[200];
        std::snprintf(label, sizeof(label), t.extrasAddFormat, extras::kMax - count);
        const double size = fitSize(pen, label, 17, 12, w - 40, true);
        textCentered(pen, kInnerX + w / 2, centerBaseline(y, h, size), label, size, usable ? kAccent : kTextDisabled,
                     true);
        addButton(hit, kInnerX, y, w, h, usable);
        y += h + 10;
    } else if (list.isList) {
        pen.text(kInnerX, y + 14, t.extrasFull, fitSize(pen, t.extrasFull, 15, 11, w, false), kTextMuted);
        y += 26;
    }
    // lan_only under a line: on for the main destination and every one here
    drawRowDivider(pen, y + 2);
    y += 10;
    const bool lanLocked = v.locked(key::kLanOnly);
    const bool lanOnly = v.flag(key::kLanOnly);
    const double rowH = kRowH;
    drawRowLabel(pen, t, y, rowH, t.rowLanOnly, t.hintLanOnly, lanLocked);
    drawSegmented(pen, kControlX, y + (rowH - kControlH) / 2, 300, kControlH,
                  {{t.on, {PanelAction::SetBool, key::kLanOnly, 1}}, {t.off, {PanelAction::SetBool, key::kLanOnly, 0}}},
                  lanOnly ? 0 : 1, 20, lanLocked);
    // ...and what that means: off, a warning
    const char* hint = lanOnly ? t.lanOnlyOnHint : t.lanOnlyOffHint;
    const std::vector<std::string> lines = sentenceLines(pen, hint, 15, !lanOnly, kControlW, 3);
    for (size_t i = 0; i < lines.size(); ++i) {
        pen.text(kControlX + 4, y + rowH + 12 + static_cast<double>(i) * 20, lines[i], 15,
                 lanOnly ? kTextMuted : kWarning, !lanOnly);
    }
}

void EyePanel::drawExtraRow(const Pen& pen, const UiText& t, const PanelModel& m, const extras::Entry& entry, double y,
                            double h, bool locked) {
    cairo_t* cr = pen.cr;
    const EyeStatus& s = m.status;
    const double x0 = kInnerX;
    const double x1 = kInnerRight;
    // What frameeyeosc says of it (0.10 on, while it runs)
    const ExtraLive* live = nullptr;
    if (s.running && s.extrasKnown) {
        for (const ExtraLive& e : s.extras) {
            if (e.index == entry.index + 1) live = &e;
        }
    }
    // Why it sends nothing: frameeyeosc's reason while it runs, else what the panel sees in the entry
    std::string why;
    if (live != nullptr && (entry.enabled || entry.problem != extras::Problem::None)) {
        const std::string& reason = !live->refused.empty() ? live->refused : live->lastError;
        if (!reason.empty()) why = extraReasonText(t, reason);
    }
    if (why.empty()) {
        switch (entry.problem) {
            case extras::Problem::None: break;
            case extras::Problem::Newer: why = t.reasonNewerEntry; break;
            case extras::Problem::Invalid: why = t.reasonBadEntry; break;
            case extras::Problem::Limit: why = t.reasonOverLimit; break;
        }
    }
    const bool bad = !why.empty();
    if (!locked && entry.index == extrasRowDelete_) {
        // Asking whether to delete it, in its own place: the question, "Cancel" and a red "Delete"
        fillRounded(pen, x0, y, x1 - x0, h, 12, kDangerTint);
        strokeRounded(pen, x0, y, x1 - x0, h, 12, kDanger, 1.5);
        const double bh = 44;
        const double by = y + (h - bh) / 2;
        const double yesW = std::max(110.0, pen.measure(t.extrasDeleteYes, 17, true) + 40);
        const double noW = std::max(110.0, pen.measure(t.extrasRowDeleteNo, 17, true) + 40);
        const double yesX = x1 - 12 - yesW;
        const double noX = yesX - 10 - noW;
        const std::string ask = formatText(t.extrasRowDeleteFormat, entry.name);
        const double room = noX - 16 - (x0 + 18);
        const double size = fitSize(pen, ask, 17, 12, room, true);
        pen.text(x0 + 18, centerBaseline(y, h, size), ellipsize(pen, ask, size, true, room, false), size, kText, true);
        drawButton(pen, noX, by, noW, bh, t.extrasRowDeleteNo,
                   {PanelAction::ExtraRowDeleteCancel, nullptr, entry.index}, true, false, 17);
        const PanelHit confirm {PanelAction::ExtraRowDeleteConfirm, nullptr, entry.index};
        fillRounded(pen, yesX, by, yesW, bh, bh / 2, pointerState(confirm) == 2 ? kAccentPressed : kDanger);
        textCentered(pen, yesX + yesW / 2, centerBaseline(by, bh, 17), t.extrasDeleteYes, 17, kOnAccent, true);
        addButton(confirm, yesX, by, yesW, bh);
        return;
    }
    // Its tag: as frameeyeosc sends to it, else from the address as written
    const std::string net = live != nullptr ? (live->sending ? live->net : std::string())
                                            : netOfHost(entry.host, extrasLanOnly_);
    strokeRounded(pen, x0, y, x1 - x0, h, 12, bad && entry.enabled ? kDanger : kDivider, 1.5);
    // Send / Stop at the left
    const double sw = 168;
    const double sh = 44;
    drawSegmented(pen, x0 + 12, y + (h - sh) / 2, sw, sh,
                  {{t.extrasSend, {PanelAction::ExtraSend, nullptr, entry.index * 2 + 1}},
                   {t.extrasStop, {PanelAction::ExtraSend, nullptr, entry.index * 2}}},
                  entry.enabled ? 0 : 1, 15, locked);
    // "Delete" and "Change" at the right (the same width, their labels smaller where they don't fit), the tag before
    const double bw = 100;
    const double bx = x1 - 12 - bw;
    const double dx = bx - 8 - bw;
    {
        // Delete: quiet red, like "Reset all"; greyed like "Change" while locked
        const PanelHit hit {PanelAction::ExtraRowDelete, nullptr, entry.index};
        const bool usable = !locked;
        const double by = y + (h - sh) / 2;
        const int pointer = usable ? pointerState(hit) : 0;
        if (usable) {
            fillRounded(pen, dx, by, bw, sh, sh / 2, pointer > 0 ? kDangerTint : kQuitFill);
            strokeRounded(pen, dx, by, bw, sh, sh / 2, kDanger, 2);
        } else {
            fillRounded(pen, dx, by, bw, sh, sh / 2, kControl);
            strokeRounded(pen, dx, by, bw, sh, sh / 2, kDivider, 2);
        }
        const double size = fitSize(pen, t.extrasRowDelete, 16, 12, bw - 24, true);
        textCentered(pen, dx + bw / 2, centerBaseline(by, sh, size), t.extrasRowDelete, size,
                     usable ? kText : kTextDisabled, true);
        addButton(hit, dx, by, bw, sh, usable);
    }
    drawButton(pen, bx, y + (h - sh) / 2, bw, sh, t.extrasEdit, {PanelAction::ExtraEdit, nullptr, entry.index}, !locked,
               false, 16);
    double right = dx - 14;
    const double tagW = drawNetTag(pen, t, net, right, y + h / 2 + 4, 12, false);
    if (tagW > 0) {
        drawNetTag(pen, t, net, right, y + h / 2 + 4, 12);
        right -= tagW + 10;
    }
    // Its name, what and where (faded while stopped), and why it sends nothing in red
    const double tx = x0 + 12 + sw + 18;
    const double room = right - tx;
    const double nameY = bad ? y + 25 : y + 30;
    if (!entry.enabled) cairo_push_group(cr);
    pen.text(tx, nameY, ellipsize(pen, entry.name, 17, true, room, false), 17, kText, true);
    std::string host = entry.host;
    if (live != nullptr && !live->target.empty()) {
        host = hostOfTarget(live->target);
    } else if (host == extras::kHostMain) {
        host = t.hostMainShort;
    } else if (host == extras::kHostAuto) {
        host = t.hostAuto;
    }
    std::string what = formatShort(entry.format);
    what += std::string("  " ICON_ARROW_RIGHT " ") + host + (entry.port > 0 ? ":" + std::to_string(entry.port) : "");
    pen.text(tx, nameY + 21, ellipsize(pen, what, 15, false, room, false), 15, kTextMuted);
    if (!entry.enabled) fadeGroup(pen);
    if (bad) pen.text(tx, nameY + 40, ellipsize(pen, why, 14, true, room, false), 14, kDanger, true);
}

double EyePanel::drawExtrasHeader(const Pen& pen, const UiText& t, const char* sub, int step) {
    const double top = kAdvPageTop;
    // 1 / 2 / 3 at the right: three bars, the ones done and this one in the accent, and the step's number
    char count[32];
    std::snprintf(count, sizeof(count), t.extrasStepOfFormat, step);
    const double countW = pen.measure(count, 15, false);
    const double barW = 26;
    const double progW = 3 * barW + 2 * 6 + 10 + countW;
    const double px = kInnerRight - progW;
    for (int i = 0; i < 3; ++i) {
        fillRounded(pen, px + i * (barW + 6), top + 11, barW, 6, 3, i < step ? kAccent : kBorder);
    }
    pen.text(kInnerRight - countW, top + 19, count, 15, kTextMuted);
    const char* title = extrasDraft_.edit >= 0 ? t.extrasEditTitle : t.extrasAddTitle;
    pen.text(kInnerX, top + 20, title, fitSize(pen, title, 20, 14, px - 16 - kInnerX, true), kText, true);
    pen.text(kInnerX, top + 44, sub, fitSize(pen, sub, 15, 11, kInnerRight - kInnerX, false), kTextMuted);
    return top + 58;
}

void EyePanel::drawExtrasFooter(const Pen& pen, const char* left, const PanelHit& leftHit, const char* right,
                                const PanelHit& rightHit, bool rightUsable) {
    const double y = kExtrasFootY;
    if (!extrasError_.empty()) {
        pen.text(kInnerX, y - 14, ellipsize(pen, extrasError_, 15, true, kInnerRight - kInnerX, false), 15, kDanger,
                 true);
    }
    drawButton(pen, kInnerX, y, 180, kExtrasFootH, left, leftHit, true, false, 18);
    drawButton(pen, kInnerRight - 240, y, 240, kExtrasFootH, right, rightHit, rightUsable, true, 18);
}

void EyePanel::drawExtrasFlow(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    (void)v;
    cairo_t* cr = pen.cr;
    const extras::Draft& d = extrasDraft_;
    const double x0 = kInnerX;
    const double x1 = kInnerRight;
    const double w = x1 - x0;
    const extras::Taken taken = extrasTaken(m);
    // Where "main" and "auto" go now, as far as the panel knows ("" = not yet)
    const auto known = [](const std::string& host) {
        return host.empty() || host == extras::kHostAuto ? std::string() : host;
    };
    const std::string mainAt = known(taken.mainHost);
    const std::string steamLinkAt = known(taken.steamLink);
    if (extrasStep_ == ExtrasStep::Format) {
        // 1: two cards, each with its permanent name, what it is for and what it carries
        double y = drawExtrasHeader(pen, t, t.extrasStepFormat, 1) + 8;
        const double gap = 16;
        const double cw = (w - gap) / 2;
        const double ch = 166;
        // (the bullets on the same line in both cards, below the longer description)
        size_t descLines = 1;
        for (const char* desc : {t.formatFeo1Desc, t.formatOscDesc}) {
            descLines = std::max(descLines, wrapText(pen, desc, 15, false, cw - 36, 2).size());
        }
        for (int i = 0; i < 2; ++i) {
            const bool osc = i == 1;
            const char* format = osc ? extras::kOsc : extras::kFeo1;
            const bool chosen = d.format == format;
            const PanelHit hit {PanelAction::ExtraFormat, nullptr, i};
            const double cx = x0 + i * (cw + gap);
            const int pointer = pointerState(hit);
            const Color fill = chosen ? kAccentTint : (pointer > 0 ? kControlHover : kControl);
            fillRounded(pen, cx, y, cw, ch, 14, fill);
            strokeRounded(pen, cx, y, cw, ch, 14, chosen ? kAccent : kBorder, 2);
            const double chipW = pen.measureMono(format, 13) + 16;
            fillRounded(pen, cx + cw - 14 - chipW, y + 14, chipW, 24, 6, chosen ? kAccent : kBg);
            pen.textMono(cx + cw - 14 - chipW + 8, y + 31, format, 13, chosen ? kOnAccent : kTextMuted);
            double tx = cx + 18;
            const double ty = y + 34;
            if (chosen) {
                drawCheck(cr, tx + 8, ty - 7, 18, kAccent);
                tx += 26;
            }
            const char* title = osc ? t.formatOscTitle : t.formatFeo1Title;
            pen.text(tx, ty, title, fitSize(pen, title, 19, 13, cx + cw - 24 - chipW - tx, true), kText, true);
            // (muted only on the plain card: the chosen and pointed-at ones keep full text)
            const Color soft = chosen || pointer > 0 ? kText : kTextMuted;
            double ly = ty + 30;
            for (const std::string& line : wrapText(pen, osc ? t.formatOscDesc : t.formatFeo1Desc, 15, false,
                                                    cw - 36, 2)) {
                pen.text(cx + 18, ly, line, 15, soft);
                ly += 21;
            }
            ly = ty + 30 + static_cast<double>(descLines) * 21 + 8;
            const char* const* bullets = osc ? t.formatOscLines : t.formatFeo1Lines;
            for (int b = 0; b < 2; ++b) {
                drawDot(cr, cx + 24, ly - 5, 3, kAccent);
                pen.text(cx + 34, ly, ellipsize(pen, bullets[b], 15, false, cw - 52, false), 15, kText);
                ly += 23;
            }
            addButton(hit, cx, y, cw, ch);
        }
        y += ch + 28;
        for (const std::string& line : wrapText(pen, t.formatHint, 15, false, w, 2)) {
            pen.text(x0, y, line, 15, kTextMuted);
            y += 21;
        }
        drawExtrasFooter(pen, t.extrasCancel, {PanelAction::ExtraCancel, nullptr, 0}, t.extrasNext,
                         {PanelAction::ExtraStep, nullptr, static_cast<int>(ExtrasStep::Host)}, !d.format.empty());
        return;
    }
    if (extrasStep_ == ExtrasStep::Host) {
        // 2: the main output's PC, the Steam Link PC, this Frame, or an address typed (and one config.json has that
        // the keypad can't type, kept)
        double y = drawExtrasHeader(pen, t, t.extrasStepHost, 2) + 8;
        using extras::HostChoice;
        std::vector<Option> options = {
            {t.hostMain, {PanelAction::ExtraHost, nullptr, static_cast<int>(HostChoice::Main)}},
            {t.hostAuto, {PanelAction::ExtraHost, nullptr, static_cast<int>(HostChoice::Auto)}},
            {t.hostFrame, {PanelAction::ExtraHost, nullptr, static_cast<int>(HostChoice::Frame)}},
            {t.hostIp, {PanelAction::ExtraHost, nullptr, static_cast<int>(HostChoice::Ip)}},
        };
        if (!d.kept.empty()) {
            options.push_back({formatText(t.hostKeepFormat, d.kept),
                               {PanelAction::ExtraHost, nullptr, static_cast<int>(HostChoice::Keep)}});
        }
        drawSegmented(pen, x0, y, w, 52, options, static_cast<int>(d.host), 17);
        y += 52 + 20;
        const extras::HostCheck check = extras::checkHost(d, extrasLanOnly_);
        if (d.host != HostChoice::Ip) {
            // What it is, and where it goes now
            const char* sub = d.host == HostChoice::Main   ? t.hostMainSub
                              : d.host == HostChoice::Auto ? t.hostAutoSub
                              : d.host == HostChoice::Frame ? t.hostFrameSub
                                                           : t.hostKeepSub;
            const std::string at = d.host == HostChoice::Main   ? (mainAt.empty() ? t.hostUnknown : mainAt)
                                   : d.host == HostChoice::Auto ? (steamLinkAt.empty() ? t.hostUnknown : steamLinkAt)
                                   : d.host == HostChoice::Frame ? std::string(extras::kHostFrame)
                                                                : d.kept;
            const double subW = pen.text(x0, y + 8, sub, fitSize(pen, sub, 17, 12, w * 0.65, false), kTextMuted);
            const std::string arrow = std::string("  " ICON_ARROW_RIGHT "  ") + at;
            pen.text(x0 + subW, y + 8, ellipsize(pen, arrow, 17, true, w - subW, false), 17, kText, true);
        } else {
            // The address typed, what kind it is, and the keypad at the right
            const double padW = 276;
            const double padX = x1 - padW;
            const double fieldW = padX - 24 - x0;
            const double fh = 60;
            fillRounded(pen, x0, y, fieldW, fh, 14, kControl);
            strokeRounded(pen, x0, y, fieldW, fh, 14, kAccent, 2);
            const double size = std::min(26.0, (fieldW - 48) / std::max(1.0, pen.measureMono("255.255.255.255", 1)));
            pen.textMono(x0 + 18, centerBaseline(y, fh, size), d.ip, size, kText);
            pen.color(kAccent);
            cairo_rectangle(cr, x0 + 20 + pen.measureMono(d.ip, size), y + 14, 2.5, fh - 28);
            cairo_fill(cr);
            using extras::HostCheck;
            const char* says = t.ipEmpty;
            Color color = kTextMuted;
            bool strong = false;  // (a warning or a refusal: bold)
            switch (check) {
                case HostCheck::Ok:
                case HostCheck::Empty: break;
                case HostCheck::Typing: says = t.ipTyping; break;
                case HostCheck::Lan: says = t.ipLan; color = kSuccess; break;
                case HostCheck::Vpn: says = t.ipVpn; color = kSuccess; break;
                case HostCheck::Public:
                    says = t.ipPublic;
                    color = kWarning;
                    strong = true;
                    break;
                case HostCheck::PublicBlocked:
                case HostCheck::Broadcast:
                case HostCheck::Multicast:
                case HostCheck::Unspecified:
                    says = check == HostCheck::PublicBlocked ? t.ipPublicBlocked
                           : check == HostCheck::Broadcast   ? t.ipBroadcast
                           : check == HostCheck::Multicast   ? t.ipMulticast
                                                             : t.ipUnspecified;
                    color = kDanger;
                    strong = true;
                    break;
            }
            double ly = y + fh + 30;
            for (const std::string& line : sentenceLines(pen, says, 16, strong, fieldW, 3)) {
                pen.text(x0 + 4, ly, line, 16, color, strong);
                ly += 23;
            }
            // The keypad: 1-9, then ".", "0" and backspace, as the target PC's
            const int keys[12] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', host_entry::kBackspace};
            const double gap = 8;
            const double keyW = (padW - 2 * gap) / 3;
            const double keyH = 52;
            for (int i = 0; i < 12; ++i) {
                const double kx = padX + (i % 3) * (keyW + gap);
                const double ky = y + (i / 3) * (keyH + gap);
                const PanelHit hit {PanelAction::ExtraKey, nullptr, keys[i]};
                const int pointer = pointerState(hit);
                fillRounded(pen, kx, ky, keyW, keyH, 12,
                            pointer == 2 ? kAccentPressed : (pointer == 1 ? kControlHover : kControl));
                strokeRounded(pen, kx, ky, keyW, keyH, 12, kBorder, 2);
                const Color key = pointer == 2 ? kOnAccent : kText;
                if (keys[i] == host_entry::kBackspace) {
                    drawBackspace(pen, kx + keyW / 2, ky + keyH / 2, 0.8, key);
                } else {
                    textCentered(pen, kx + keyW / 2, centerBaseline(ky, keyH, 24), std::string(1, keys[i]), 24, key,
                                 true);
                }
                addButton(hit, kx, ky, keyW, keyH);
            }
        }
        drawExtrasFooter(pen, t.extrasBack, {PanelAction::ExtraStep, nullptr, static_cast<int>(ExtrasStep::Format)},
                         t.extrasNext, {PanelAction::ExtraStep, nullptr, static_cast<int>(ExtrasStep::Port)},
                         extras::hostUsable(check));
        return;
    }
    // 3: the port (each format starts at its own) and what it comes to, red where it repeats another destination;
    // while changing, delete
    double y = drawExtrasHeader(pen, t, t.extrasStepPort, 3) + 4;
    const double rowH = kRowH;
    drawRowLabel(pen, t, y, rowH, t.rowPort, d.format == extras::kOsc ? t.portNoteOsc : t.portNoteFeo1, false);
    {
        // − port ＋, as the main port's
        const double sx = kControlX;
        const double sy = y + (rowH - kControlH) / 2;
        const double sw = 240;
        const double sh = kControlH;
        fillRounded(pen, sx, sy, sw, sh, sh / 2, kControl);
        strokeRounded(pen, sx, sy, sw, sh, sh / 2, kBorder, 2);
        const double buttonW = sh;
        for (int side = 0; side < 2; ++side) {
            const PanelHit hit {PanelAction::ExtraPort, nullptr, side == 0 ? -1 : 1};
            const bool can = side == 0 ? d.port > 1 : d.port < 65535;
            const double bx = side == 0 ? sx : sx + sw - buttonW;
            const int pointer = can ? pointerState(hit) : 0;
            if (pointer > 0) {
                fillRounded(pen, bx + 4, sy + 4, buttonW - 8, sh - 8, (sh - 8) / 2,
                            pointer == 2 ? kAccentPressed : kControlHover);
            }
            drawPlusMinus(cr, bx + buttonW / 2, sy + sh / 2, 16, side == 1,
                          pointer == 2 ? kOnAccent : (can ? kText : kTextDisabled));
            addButton(hit, bx, sy, buttonW, sh, can);
        }
        // (the value opens the keypad, as the main port's)
        const std::string port = std::to_string(d.port);
        drawValueTap(pen, {PanelAction::ExtraPortEnter, nullptr, 0}, sx + buttonW, sy, sw - 2 * buttonW, sh,
                     pen.measure(port, 20, true));
        textCentered(pen, sx + sw / 2, centerBaseline(sy, sh, 20), port, 20, kText, true);
    }
    y += rowH + kRowGap;
    // What it comes to: the name, the format, where (as far as known) and its tag; red where it repeats one
    int other = -1;
    const extras::List list = extrasList(m);
    const extras::Clash clash = extras::findClash(d, list, taken, other);
    drawRowLabel(pen, t, y, rowH, t.rowSummary, t.hintSummary, false);
    {
        using extras::HostChoice;
        const std::string host = d.host == HostChoice::Main   ? (mainAt.empty() ? std::string(t.hostMain) : mainAt)
                                 : d.host == HostChoice::Auto ? (steamLinkAt.empty() ? std::string(t.hostAuto)
                                                                                     : steamLinkAt)
                                                              : extras::draftHost(d);
        const double base = y + rowH / 2 + 7;
        double x = kControlX;
        x += pen.text(x, base, ellipsize(pen, d.name, 18, true, 160, false), 18, kText, true) + 12;
        const std::string format = d.format;
        const double fw = pen.measureMono(format, 13) + 14;
        fillRounded(pen, x, base - 16, fw, 22, 5, kControl);
        pen.textMono(x + 7, base - 1, format, 13, kTextMuted);
        x += fw + 12;
        const std::string net = netOfHost(extras::draftHost(d), extrasLanOnly_);
        const double tagW = drawNetTag(pen, t, net, x1, base, 12, false);
        const std::string where = std::string(ICON_ARROW_RIGHT " ") + host + ":" + std::to_string(d.port);
        const double room = x1 - x - (tagW > 0 ? tagW + 10 : 0);
        const double used = pen.text(x, base, ellipsize(pen, where, 17, false, room, false), 17, kText);
        if (tagW > 0) drawNetTag(pen, t, net, x + used + 10 + tagW, base, 12);
    }
    y += rowH;
    if (clash != extras::Clash::None) {
        std::string name = extras::defaultName(other);
        for (const extras::Entry& entry : list.entries) {
            if (entry.index == other) name = entry.name;
        }
        const std::string text = clash == extras::Clash::Main     ? std::string(t.clashMain)
                                 : clash == extras::Clash::Pupils ? std::string(t.clashPupils)
                                                                  : formatText(t.clashDuplicateFormat, name);
        pen.text(kControlX, y + 4, ellipsize(pen, text, 15, true, x1 - kControlX, false), 15, kDanger, true);
        y += 22;
    }
    y += 12;
    if (d.edit >= 0) {
        if (!extrasDeleteOpen_) {
            // Delete: quiet red, like "Reset all" (its confirmation comes next, in the panel)
            const PanelHit hit {PanelAction::ExtraDelete, nullptr, 0};
            const double bw = std::min(w, pen.measure(t.extrasDelete, 17, true) + 56);
            const double bh = 46;
            fillRounded(pen, x0, y, bw, bh, bh / 2, pointerState(hit) > 0 ? kDangerTint : kQuitFill);
            strokeRounded(pen, x0, y, bw, bh, bh / 2, kDanger, 2);
            textCentered(pen, x0 + bw / 2, centerBaseline(y, bh, 17), t.extrasDelete, 17, kText, true);
            addButton(hit, x0, y, bw, bh);
        } else {
            // Its confirmation: what goes, "Keep" and "Delete"
            const double bh = 68;
            fillRounded(pen, x0, y, w, bh, 14, kDangerTint);
            strokeRounded(pen, x0, y, w, bh, 14, kDanger, 1.5);
            const double bw = 132;
            const double gap = 10;
            const double yes = x1 - 12 - bw;
            const double no = yes - gap - bw;
            const std::string ask = formatText(t.extrasDeleteFormat, d.name);
            const double room = no - 16 - (x0 + 18);
            const double size = fitSize(pen, ask, 17, 12, room, true);
            pen.text(x0 + 18, centerBaseline(y, bh, size), ellipsize(pen, ask, size, true, room, false), size, kText,
                     true);
            drawButton(pen, no, y + (bh - 46) / 2, bw, 46, t.extrasDeleteNo,
                       {PanelAction::ExtraDeleteCancel, nullptr, 0}, true, false, 17);
            const PanelHit confirm {PanelAction::ExtraDeleteConfirm, nullptr, 0};
            fillRounded(pen, yes, y + (bh - 46) / 2, bw, 46, 23, pointerState(confirm) == 2 ? kAccentPressed : kDanger);
            textCentered(pen, yes + bw / 2, centerBaseline(y + (bh - 46) / 2, 46, 17), t.extrasDeleteYes, 17,
                         kOnAccent, true);
            addButton(confirm, yes, y + (bh - 46) / 2, bw, 46);
        }
    }
    drawExtrasFooter(pen, t.extrasBack, {PanelAction::ExtraStep, nullptr, static_cast<int>(ExtrasStep::Host)},
                     extrasSaveLabel(t, d, list), {PanelAction::ExtraSave, nullptr, 0},
                     clash == extras::Clash::None && !extrasDeleteOpen_);
}

void EyePanel::drawGaze(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    // Three sub-tabs, drawn as the Eyelids and Advanced tabs' (the first time "Motion", then the one open last)
    const GazePage pages[3] = {GazePage::Motion, GazePage::Reach, GazePage::Glitch};
    const char* labels[3] = {t.gazeMotion, t.gazeReach, t.gazeGlitch};
    std::vector<Option> options;
    for (int i = 0; i < 3; ++i) {
        options.push_back({labels[i], {PanelAction::GazePage, nullptr, static_cast<int>(pages[i])}});
    }
    drawPageSwitch(pen, kSubTabY, kSubTabH, options, static_cast<int>(gazePage_), -1);
    switch (gazePage_) {
        case GazePage::Motion: drawGazeMotion(pen, t, v, kAdvPageTop); break;
        case GazePage::Reach: drawGazeReach(pen, t, m, v, kAdvPageTop); break;
        case GazePage::Glitch: drawGazeGlitch(pen, t, v, kAdvPageTop); break;
    }
}

void EyePanel::drawGazeMotion(const Pen& pen, const UiText& t, const SettingsView& v, double top) {
    double y = top;
    const double cy = (kRowH - kControlH) / 2;
    const bool raw = v.flag(key::kRaw);

    // Smoothing on / off (on = raw false)
    {
        const bool locked = v.locked(key::kRaw);
        drawRowLabel(pen, t, y, kRowH, t.rowSmoothing, t.hintSmoothing, locked);
        drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                      {{t.on, {PanelAction::SetBool, key::kRaw, 0}}, {t.off, {PanelAction::SetBool, key::kRaw, 1}}},
                      raw ? 1 : 0, 20, locked);
    }
    y += kRowH + kRowGap;
    // Presets
    {
        const bool locked = v.locked(key::kGazeMinCutoff) || v.locked(key::kGazeBeta) || v.locked(key::kGazeDCutoff);
        const int preset = matchingGazePreset(v);
        const std::string hint = raw ? t.rawOnNote : (preset < 0 ? t.custom : "");
        drawRowLabel(pen, t, y, kRowH, t.rowStrength, hint, locked);
        drawSegmented(pen, kControlX, y + cy, 390, kControlH,
                      {{t.strengthLight, {PanelAction::Preset, nullptr, 0}},
                       {t.strengthMedium, {PanelAction::Preset, nullptr, 1}},
                       {t.strengthStrong, {PanelAction::Preset, nullptr, 2}}},
                      preset, 20, locked);
    }
    y += kRowH + kRowGap;
    // The three One Euro values
    {
        drawRowLabel(pen, t, y, kCaptionRowH, t.rowFine, t.lowerSmoother, false);
        const char* keys[3] = {key::kGazeMinCutoff, key::kGazeBeta, key::kGazeDCutoff};
        const char* captions[3] = {t.capStill, t.capFast, t.capChange};
        const double gap = 11;
        const double w = (kControlW - gap * 2) / 3;
        for (int i = 0; i < 3; ++i) {
            const double x = kControlX + i * (w + gap);
            drawCaption(pen, x + 4, y + 17, captions[i], 0, v.locked(keys[i]));
            const double value = v.number(keys[i]);
            drawStepper(pen, x, y + 28, w, kControlH, keys[i], value, formatSetting(keys[i], value), true,
                        v.locked(keys[i]));
        }
    }
    y += kCaptionRowH + kRowGap;
    // Deadzone (also in degrees: 1.0 = 45°)
    {
        const bool locked = v.locked(key::kGazeDeadzone);
        const double value = v.number(key::kGazeDeadzone);
        char text[64];
        std::snprintf(text, sizeof(text), "%.3f (%.1f°)", value, value * 45.0);
        drawRowLabel(pen, t, y, kRowH, t.rowDeadzone, t.hintDeadzone, locked);
        drawStepper(pen, kControlX, y + cy, 260, kControlH, key::kGazeDeadzone, value, text, true, locked);
    }
}

void EyePanel::drawGazeReach(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v,
                             double top) {
    double y = top;
    const double cy = (kRowH - kControlH) / 2;
    // The gaze scale: left [−  100%  ＋] lock right [...], then up and down the same way. A closed lock (accent)
    // changes both of its pair together; open (grey), each on its own; closing it again sets the second to the first
    {
        const double lineH = kCaptionRowH;
        // The title, and the hint under it in as many lines as it takes (three at most)
        {
            const double titleSize = fitSize(pen, t.rowGazeScale, 20, 14, kLabelW, true);
            pen.text(kInnerX, y + 38, t.rowGazeScale, titleSize, kText, true);
            const std::vector<std::string> lines = wrapText(pen, t.hintGazeScale, 15, false, kLabelW, 3);
            double baseline = y + 38 + 26;
            for (const std::string& line : lines) {
                pen.text(kInnerX, baseline, line, 15, kTextMuted);
                baseline += 21;
            }
        }
        const double lockW = 52;
        const double gap = 14;
        const double stepW = (kControlW - lockW - gap * 2) / 2;
        const char* captions[2][2] = {{t.capScaleLeft, t.capScaleRight}, {t.capScaleUp, t.capScaleDown}};
        for (int pair = 0; pair < 2; ++pair) {
            const double ly = y + pair * lineH;
            const double sy = ly + 28;
            for (int side = 0; side < 2; ++side) {
                const char* name = kGazeScaleKeys[pair][side];
                const double x = kControlX + side * (stepW + lockW + gap * 2);
                const bool locked = v.locked(name);
                drawCaption(pen, x + 4, ly + 17, captions[pair][side], 0, locked);
                const double value = v.number(name);
                drawStepper(pen, x, sy, stepW, kControlH, name, value, formatPercent(value), true, locked);
            }
            // The lock between them; not while either is locked by the command line, and then open (each moves on
            // its own, so they can differ)
            const bool usable = gazeScaleLinkUsable(v, pair);
            const bool linked = usable && gazeScaleLinked_[pair];
            const PanelHit hit {PanelAction::GazeScaleLink, nullptr, pair};
            const double lx = kControlX + stepW + gap;
            const int pointer = usable ? pointerState(hit) : 0;
            const Color fill = pointer == 2 ? kAccentPressed : (pointer == 1 ? kControlHover : kControl);
            fillRounded(pen, lx, sy, lockW, kControlH, kControlH / 2, fill);
            if (usable) strokeRounded(pen, lx, sy, lockW, kControlH, kControlH / 2, linked ? kAccent : kBorder, 2);
            const Color icon = !usable ? kTextDisabled : pointer == 2 ? kOnAccent : linked ? kAccent : kTextMuted;
            drawLinkLock(pen, lx + lockW / 2, sy + kControlH / 2 + 1, 24, !linked, icon, fill);
            addButton(hit, lx, sy, lockW, kControlH, usable);
        }
        y += lineH * 2 + kRowGap;
    }
    // Independent eyes; while the Frame tracks one eye alone, both eyes get its gaze either way
    {
        const bool locked = v.locked(key::kIndependentEyes);
        const bool oneEye = m.status.running && !m.status.dominantEye.empty();
        const char* hint = oneEye ? t.hintIndependentOneEye : t.hintIndependent;
        drawRowLabel(pen, t, y, kRowH, t.rowIndependent, hint, locked);
        drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                      {{t.on, {PanelAction::SetBool, key::kIndependentEyes, 1}},
                       {t.off, {PanelAction::SetBool, key::kIndependentEyes, 0}}},
                      v.flag(key::kIndependentEyes) ? 0 : 1, 20, locked);
    }
    y += kRowH + kRowGap;
    // How much of each eye's own sideways gaze (own_eye_gaze): only while the eyes move separately, faded otherwise;
    // what the ends do beside it
    {
        const bool locked = v.locked(key::kOwnEyeGaze);
        const bool usable = v.flag(key::kIndependentEyes);
        const double stepperW = 196;
        const double value = v.number(key::kOwnEyeGaze);
        const double nx = kControlX + stepperW + 16;
        if (!usable) cairo_push_group(pen.cr);
        drawRowLabel(pen, t, y, kRowH, t.rowOwnEyeGaze, "", locked);
        drawStepper(pen, kControlX, y + cy, stepperW, kControlH, key::kOwnEyeGaze, value, formatPercent(value), usable,
                    locked);
        {
            const std::vector<std::string> lines = wrapText(pen, t.hintOwnEyeGaze, 13, false, kInnerRight - nx, 3);
            double baseline = y + kRowH / 2 + 4 - (static_cast<double>(lines.size()) - 1) * 8.5;
            for (const std::string& line : lines) {
                pen.text(nx, baseline, line, 13, kTextMuted);
                baseline += 17;
            }
        }
        if (!usable) fadeGroup(pen);
    }
}

void EyePanel::drawGazeGlitch(const Pen& pen, const UiText& t, const SettingsView& v, double top) {
    double y = top;
    const double cy = (kRowH - kControlH) / 2;
    // Hold gaze while blinking (0 = off) and its threshold
    {
        const bool locked = v.locked(key::kGazeHoldBelow);
        const double value = v.number(key::kGazeHoldBelow);
        const bool on = value > 0;
        drawRowLabel(pen, t, y, kRowH, t.rowHold, t.hintHold, locked);
        drawSegmented(pen, kControlX, y + cy, 200, kControlH,
                      {{t.on, {PanelAction::NumberOn, key::kGazeHoldBelow, 0}},
                       {t.off, {PanelAction::NumberOff, key::kGazeHoldBelow, 0}}},
                      on ? 0 : 1, 20, locked);
        drawStepper(pen, kControlX + 212, y + cy, 220, kControlH, key::kGazeHoldBelow, value,
                    on ? formatSetting(key::kGazeHoldBelow, value) : std::string("—"), on, locked);
    }
    y += kRowH + kRowGap;
    // Skip an eye's gaze while its covariance is high (0 = off) and the limit
    {
        const bool locked = v.locked(key::kGazeQualityLimit);
        const double value = v.number(key::kGazeQualityLimit);
        const bool on = value > 0;
        drawRowLabel(pen, t, y, kRowH, t.rowQuality, t.hintQuality, locked);
        drawSegmented(pen, kControlX, y + cy, 200, kControlH,
                      {{t.on, {PanelAction::NumberOn, key::kGazeQualityLimit, 0}},
                       {t.off, {PanelAction::NumberOff, key::kGazeQualityLimit, 0}}},
                      on ? 0 : 1, 20, locked);
        drawStepper(pen, kControlX + 212, y + cy, 220, kControlH, key::kGazeQualityLimit, value,
                    on ? formatSetting(key::kGazeQualityLimit, value) : std::string("—"), on, locked);
    }
    y += kRowH + kRowGap;
    // 3-sample median on gaze and openness
    {
        const bool locked = v.locked(key::kDespike);
        drawRowLabel(pen, t, y, kRowH, t.rowDespike, t.hintDespike, locked);
        drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                      {{t.on, {PanelAction::SetBool, key::kDespike, 1}},
                       {t.off, {PanelAction::SetBool, key::kDespike, 0}}},
                      v.flag(key::kDespike) ? 0 : 1, 20, locked);
    }
}

void EyePanel::drawButton(const Pen& pen, double x, double y, double w, double h, const std::string& label,
                           const PanelHit& hit, bool usable, bool accent, double textSize) {
    const int pointer = usable ? pointerState(hit) : 0;
    const bool filled = accent && usable;
    if (filled) {
        fillRounded(pen, x, y, w, h, h / 2, pointer == 2 ? kAccentPressed : kAccent);
    } else {
        fillRounded(pen, x, y, w, h, h / 2, pointer > 0 ? kControlHover : kControl);
        strokeRounded(pen, x, y, w, h, h / 2, usable ? kBorder : kDivider, 2);
    }
    const double size = fitSize(pen, label, textSize, 12, w - 24, true);
    const Color color = filled ? kOnAccent : (usable ? kText : kTextDisabled);
    textCentered(pen, x + w / 2, centerBaseline(y, h, size), label, size, color, true);
    addButton(hit, x, y, w, h, usable);
}

void EyePanel::drawEyeFit(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    using gaze_fit::Mode;
    using gaze_fit::Phase;
    const gaze_fit::View& fit = m.fit;
    const FitInConfig saved = fitInConfig(v);
    const bool anyFitted = saved.gazeFitted || saved.lidsFitted[0] || saved.lidsFitted[1];
    double y = kRowTop;
    const double cy = (kRowH - kControlH) / 2;
    const bool anyLocked = fitKeysLocked(v);
    const bool busy = fitRunning(fit);
    // While it runs, only "look at the dot" and "Stop": the dots show in front of this panel
    if (busy) {
        drawEyeFitRunning(pen, t, fit);
        return;
    }
    // (not while the eye cameras calibrate: the user looks where its steps say, up and down too)
    const bool camerasCalibrating = eyecam::calibrationUnderWay(m.eyecam);
    const bool canRun = m.status.running && !anyLocked && !camerasCalibrating;
    const AutoRecenter rewear = autoRecenter(m.config);

    // The one button to press: fit (again); and the re-wear fit (the middle dot), the one auto_recenter runs by itself
    // when the headset is put on (see auto_recenter.h)
    {
        drawRowLabel(pen, t, y, kRowH, t.rowFit, t.hintFit, false);
        const double gap = 12;
        const double smallW = std::max(190.0, pen.measure(t.fitCenterOnly, 19, true) + 32);
        const double bigW = kControlW - smallW - gap;
        drawButton(pen, kControlX, y + cy, bigW, kControlH, anyFitted ? t.fitAgain : t.fitStart,
                   {PanelAction::FitStart, nullptr, 0}, canRun, true);
        drawButton(pen, kControlX + bigW + gap, y + cy, smallW, kControlH, t.fitCenterOnly,
                   {PanelAction::FitCenter, nullptr, 0}, canRun, false);
    }
    y += kRowH + kRowGap;
    // What runs by itself when the headset is put on again (auto_recenter): a setting, so a row of its own that
    // stays put whether "Fine-tune" is open or not
    {
        const double h = 50;
        const double controlH = 42;
        drawRowLabel(pen, t, y, h, t.rowAutoRecenter, t.hintAutoRecenter, false);
        drawSegmented(pen, kControlX, y + (h - controlH) / 2, kControlW, controlH,
                      {{t.autoRecenterOff, {PanelAction::SetAutoRecenter, key::kAutoRecenter, 0}},
                       {t.autoRecenterCenter, {PanelAction::SetAutoRecenter, key::kAutoRecenter, 1}}},
                      static_cast<int>(rewear), 18);
        y += h + 6;
    }
    // What is going on: how it works, the run, the result (with "Reset"), or why it stopped
    {
        const bool compact = fitDetails_;
        const double h = compact ? 50 : 184;
        const double x0 = kInnerX;
        const double x1 = kInnerRight;
        strokeRounded(pen, x0, y, x1 - x0, h, 14, kDivider, 1.5);
        std::string title;
        std::vector<std::string> paragraphs;
        bool error = false;
        bool check = false;
        bool reset = false;
        bool failed = false;  // it failed (not stopped): its record's bar under the box
        char text[320];
        switch (fit.phase) {
            case Phase::Settling:
            case Phase::Capturing:
            case Phase::Reopen: break;  // drawn by drawEyeFitRunning
            case Phase::Failed:
                if (fitStopped(fit)) {
                    // Stopped by the user: why, not an error
                    title = failureText(t, fit);
                    paragraphs.push_back(t.fitUnchanged);
                    // Re-centering by itself, stopped by the dashboard SteamVR opened (or the user did): it tries again
                    if (fit.failure == gaze_fit::Failure::DashboardOpened && m.fitRetry) {
                        paragraphs.push_back(t.fitRetryAfterDashboard);
                    }
                    break;
                }
                error = true;
                failed = true;
                title = t.fitFailed;
                paragraphs.push_back(failureText(t, fit));
                // The numbers behind it (what was measured against what was needed), for asking for help
                if (const std::string detail = failureDetailText(t, fit); !detail.empty()) paragraphs.push_back(detail);
                break;
            case Phase::Idle:
            case Phase::Done:
                if (anyFitted) {
                    check = true;
                    reset = true;
                    title = fit.phase != Phase::Done ? t.fitFitted
                                                     : (fit.mode == Mode::Center ? t.fitDoneCenter : t.fitDone);
                    // First, so it shows whatever follows: an eye the eye camera stood in for, Valve's openness stuck
                    // shut (its lid fit kept)
                    const std::string standIn = standInText(t, fit);
                    for (size_t start = 0; start < standIn.size();) {
                        const size_t end = std::min(standIn.find('\n', start), standIn.size());
                        paragraphs.push_back(standIn.substr(start, end - start));
                        start = end + 1;
                    }
                    const gaze_fit::Values& r = saved.values;
                    // One line each: the gaze center, the gaze range, and each eye's lid readings
                    if (saved.gazeFitted) {
                        std::snprintf(text, sizeof(text), t.fitGazeCenterFormat, offsetText(r.offsetX).c_str(),
                                      offsetText(r.offsetY).c_str());
                        paragraphs.push_back(text);
                        std::snprintf(text, sizeof(text), t.fitGazeRangeFormat, twoDecimals(r.gainX).c_str(),
                                      twoDecimals(r.gainUp).c_str(), twoDecimals(r.gainDown).c_str());
                        paragraphs.push_back(text);
                    } else {
                        paragraphs.push_back(t.fitGazeNone);
                    }
                    // Per eye: a first whole fit with an eye the eye camera stood in for fits only the other one
                    for (const std::string& line : lidFitLines(t, r, saved.lidsFitted)) paragraphs.push_back(line);
                    if (r.hasLids) {
                        // An eye that can't widen by itself follows the other one (lid_widen)
                        const WidenState widen = widenState(v);
                        if (widen.mode > 0 && widen.room[0] != widen.room[1]) {
                            paragraphs.push_back(widen.room[1] ? t.widenFollowsLeft : t.widenFollowsRight);
                        } else if (widen.mode > 0 && !widen.room[0] && !widen.room[1]) {
                            paragraphs.push_back(t.widenNoRoom);
                        }
                    }
                } else {
                    title = !m.status.running ? t.fitNeedsRunning : (anyLocked ? t.fitLocked : "");
                    error = !title.empty();
                    // How it goes, a sentence a line
                    paragraphs.push_back(t.fitIntro);
                    for (const std::string& sentence : sentences(t.fitHowTo)) paragraphs.push_back(sentence);
                }
                break;
        }
        // The eye cameras calibrating: why the buttons are grey comes first (the rest follows it)
        if (camerasCalibrating) {
            if (!title.empty()) paragraphs.insert(paragraphs.begin(), title);
            title = t.fitCamerasCalibrating;
            error = false;
            check = false;
        }
        // Folded down to one line while "Fine-tune" is open
        if (compact) {
            if (title.empty()) title = t.fitNotYet;
            paragraphs.clear();
        }
        const double buttonW = 150;
        const bool button = reset;
        double textX = x0 + 20;
        if (check) {
            drawCheck(pen.cr, x0 + 32, compact ? y + h / 2 : y + 32, 24, kSuccess);
            textX = x0 + 56;
        }
        const double textW = (button ? x1 - 20 - buttonW - 16 : x1 - 20) - textX;
        const std::vector<std::string> titleLines =
            title.empty() ? std::vector<std::string>() : wrapText(pen, title, 18, true, textW, 1);
        std::vector<std::string> detailLines;
        const size_t maxDetail = titleLines.empty() ? 7 : 6;
        for (const std::string& paragraph : paragraphs) {
            if (detailLines.size() >= maxDetail) break;
            for (const std::string& line : wrapText(pen, paragraph, 15, false, textW, maxDetail - detailLines.size())) {
                detailLines.push_back(line);
            }
        }
        const double lineCount = titleLines.size() * 26.0 + detailLines.size() * 22.0;
        double baseline = y + (h - lineCount) / 2;
        for (const std::string& line : titleLines) {
            baseline += 26;
            pen.text(textX, baseline - 6, line, 18, error ? kDanger : kText, true);
        }
        for (const std::string& line : detailLines) {
            baseline += 22;
            pen.text(textX, baseline - 5, line, 15, kTextMuted);
        }
        const double buttonH = compact ? 40 : kControlH;
        if (reset) {
            drawButton(pen, x1 - 20 - buttonW, y + (h - buttonH) / 2, buttonW, buttonH, t.fitReset,
                       {PanelAction::FitReset, nullptr, 0}, !anyLocked && !busy, false);
        }
        y += h + (compact ? 8 : 12);
        // Its log was saved: where to find it
        if (failed && !compact && !m.records.lastFit.empty()) {
            y += drawRecordBar(pen, t, m.records.lastFit, false, kInnerX, kInnerRight, y) + 12;
        }
    }
    // "Fine-tune": the values by hand, folded away by default. Next to it the sounds switch, as wide as its longer
    // wording, and once it is open the page switch on the right
    {
        const double gap = 10;
        const double detailsW = 150;
        const double pagesW = 190;
        const auto switchW = [&](std::initializer_list<const char*> labels) {
            double widest = 0;
            for (const char* label : labels) widest = std::max(widest, pen.measure(label, 19, true));
            return widest + 32;
        };
        const double room = kInnerRight - kInnerX - detailsW - gap - (fitDetails_ ? pagesW + gap : 0);
        const double soundsW = std::min(room, switchW({t.fitSoundsOn, t.fitSoundsOff}));
        const std::string label =
            std::string(t.fitDetails) + (fitDetails_ ? "  " ICON_CHEVRON_UP : "  " ICON_CHEVRON_DOWN);
        drawButton(pen, kInnerX, y, detailsW, 38, label, {PanelAction::FitDetails, nullptr, 0}, true, false);
        // Sound cues on / off: one button that says the state and flips it
        const double x = kInnerX + detailsW + gap;
        {
            const bool on = v.flag(key::kFitSounds);
            const bool locked = v.locked(key::kFitSounds);
            drawButton(pen, x, y, soundsW, 38, on ? t.fitSoundsOn : t.fitSoundsOff,
                       {PanelAction::SetBool, key::kFitSounds, on ? 0 : 1}, !locked, false);
        }
        // Once open: the gaze values, or the eyelid values
        if (fitDetails_) {
            drawSegmented(pen, kInnerRight - pagesW, y, pagesW, 38,
                          {{t.detailsGaze, {PanelAction::FitDetailsPage, nullptr, 0}},
                           {t.detailsLids, {PanelAction::FitDetailsPage, nullptr, 1}}},
                          fitDetailsPage_, 18);
        }
        y += 38 + 8;
    }
    if (!fitDetails_) {
        drawEyeFitInfo(pen, t, y + 4);
        return;
    }
    if (fitDetailsPage_ == 0) {
        drawEyeFitGaze(pen, t, v, busy, y);
    } else {
        drawEyeFitLids(pen, t, v, saved, busy, y);
    }
}

void EyePanel::drawEyeFitRunning(const Pen& pen, const UiText& t, const gaze_fit::View& fit) {
    // Calm and still, since the dots show in front of it: one line to say what to do, under it how (muted, never
    // changing while the fit runs: no step count or ring to pull the eyes), the middle of the card left empty where
    // the dots usually are, and a big "Stop" at the bottom
    const double cx = (kRightX + kRight) / 2;
    const double width = kInnerRight - kInnerX - 40;
    const double titleSize = 26;
    double baseline = kContentY + 64;
    textCentered(pen, cx, baseline, t.fitRunningTitle, titleSize, kText, true);
    baseline += 12;
    // A sentence a line (wrapped if one is too long), so none breaks in the middle of a word
    size_t lines = 0;
    for (const std::string& sentence : sentences(howToText(t, fit.mode))) {
        if (lines >= 4) break;
        for (const std::string& line : wrapText(pen, sentence, 17, false, width, 4 - lines)) {
            baseline += 26;
            textCentered(pen, cx, baseline, line, 17, kTextMuted, false);
            ++lines;
        }
    }
    const double stopW = 360;
    const double stopH = 84;
    drawButton(pen, cx - stopW / 2, kContentY + kContentH - 36 - stopH, stopW, stopH, t.fitStop,
               {PanelAction::FitStop, nullptr, 0}, true, false, 32);
}

void EyePanel::drawEyeFitGaze(const Pen& pen, const UiText& t, const SettingsView& v, bool busy, double y) {
    const char* offsetKeys[2] = {key::kGazeOffsetX, key::kGazeOffsetY};
    const char* gainKeys[3] = {key::kGazeGainX, key::kGazeGainUp, key::kGazeGainDown};
    // A little tighter than the other tabs' caption rows, so the page fits under the Eye fit tab's rows
    const double rowH = 72;
    const double rowStepperH = 46;
    // The gaze zero point
    {
        drawRowLabel(pen, t, y, rowH, t.rowOffset, t.hintOffset, false);
        const char* captions[2] = {t.capLeftRight, t.capUpDown};
        const double gap = 12;
        const double w = (kControlW - gap) / 2;
        for (int i = 0; i < 2; ++i) {
            const double x = kControlX + i * (w + gap);
            const bool locked = v.locked(offsetKeys[i]);
            drawCaption(pen, x + 4, y + 15, captions[i], 0, locked);
            const double value = v.number(offsetKeys[i]);
            drawStepper(pen, x, y + 24, w, rowStepperH, offsetKeys[i], value, offsetText(value), !busy, locked);
        }
    }
    y += rowH + kRowGap;
    // The gaze gains
    {
        drawRowLabel(pen, t, y, rowH, t.rowGain, t.hintGain, false);
        const char* captions[3] = {t.capLeftRight, t.capUp, t.capDown};
        const double gap = 11;
        const double w = (kControlW - gap * 2) / 3;
        for (int i = 0; i < 3; ++i) {
            const double x = kControlX + i * (w + gap);
            const bool locked = v.locked(gainKeys[i]);
            drawCaption(pen, x + 4, y + 15, captions[i], 0, locked);
            const double value = v.number(gainKeys[i]);
            drawStepper(pen, x, y + 24, w, rowStepperH, gainKeys[i], value, formatSetting(gainKeys[i], value), !busy,
                        locked);
        }
    }
    y += rowH + kRowGap;
    // Holding the sideways gaze when looking far down, where the Frame's x jumps
    {
        const double h = 50;
        const bool locked = v.locked(key::kGazeDownHoldXDeg);
        const double value = v.number(key::kGazeDownHoldXDeg);
        drawRowLabel(pen, t, y, h, t.rowDownHold, t.hintDownHold, locked);
        const std::string text = value > 0 ? formatText(t.downHoldFormat, formatSetting(key::kGazeDownHoldXDeg, value))
                                           : std::string(t.off);
        drawStepper(pen, kControlX, y + (h - 42) / 2, 200, 42, key::kGazeDownHoldXDeg, value, text, !busy, locked);
    }
}

void EyePanel::drawEyeFitInfo(const Pen& pen, const UiText& t, double y) {
    // How fitting works now, in the room under "Fine-tune" while it is folded: each wear one dot, the whole fit only
    // at first and when something is off, and no tilt any more
    const double x0 = kInnerX;
    const double x1 = kInnerRight;
    const double bottom = kContentY + kContentH - 20;
    if (bottom - y < 120) return;
    const double h = std::min(bottom - y, 170.0);
    strokeRounded(pen, x0, y, x1 - x0, h, 14, kDivider, 1.5);
    const double textX = x0 + 20;
    const double textW = x1 - 20 - textX;
    double baseline = y + 34;
    pen.text(textX, baseline, t.fitInfoTitle, fitSize(pen, t.fitInfoTitle, 18, 14, textW, true), kText, true);
    for (const char* line : {t.fitInfoRewear, t.fitInfoFull}) {
        baseline += 32;
        pen.text(textX, baseline, line, fitSize(pen, line, 16, 12, textW, false), kText);
    }
    baseline += 32;
    pen.text(textX, baseline, t.fitInfoNoTilt, fitSize(pen, t.fitInfoNoTilt, 15, 11, textW, false), kTextMuted);
}

void EyePanel::drawEyeFitLids(const Pen& pen, const UiText& t, const SettingsView& v, const FitInConfig& saved,
                              bool busy, double y) {
    // Each eye's lid readings, across the whole width: closed, up, straight ahead, down
    {
        const double titleSize = 18;
        pen.text(kInnerX, y + 16, t.rowLidFit, titleSize, kText, true);
        const double titleW = pen.measure(t.rowLidFit, titleSize, true);
        pen.text(kInnerX + titleW + 12, y + 16, t.hintLidFit, 15, kTextMuted);
        const char* captions[4] = {t.capClosed, t.capUp, t.capAhead, t.capDown};
        const double labelW = 40;
        const double gap = 10;
        const double w = (kInnerRight - kInnerX - labelW - gap * 3) / 4;
        const double stepperH = 44;
        for (int i = 0; i < 4; ++i) {
            const double x = kInnerX + labelW + i * (w + gap);
            drawCaption(pen, x + 4, y + 40, captions[i], 0, v.locked(kLidFitKeys[0][i]) || v.locked(kLidFitKeys[1][i]));
        }
        for (int eye = 0; eye < 2; ++eye) {
            const double rowY = y + 48 + eye * (stepperH + 4);
            pen.text(kInnerX + 4, centerBaseline(rowY, stepperH, 18), eye == 0 ? t.left : t.right, 18, kText, true);
            for (int i = 0; i < 4; ++i) {
                const char* name = kLidFitKeys[eye][i];
                const double x = kInnerX + labelW + i * (w + gap);
                const double value = v.number(name);
                double low = -1e9;
                double high = 1e9;
                lidMarkBounds(name, v, low, high);
                // Only a fitted eye's readings can be changed; fitting is what sets them
                drawStepper(pen, x, rowY, w, stepperH, name, value, formatSetting(name, value),
                            !busy && saved.lidsFitted[eye], v.locked(name), low, high);
            }
        }
    }
}

void EyePanel::drawLids(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    // Four sub-tabs where the Advanced tab has its own (the first time "Look", then the one open last)
    const LidsPage pages[4] = {LidsPage::Look, LidsPage::Blinks, LidsPage::Fine, LidsPage::Brows};
    const char* labels[4] = {t.lidsLook, t.lidsBlinks, t.lidsFine, t.lidsBrows};
    std::vector<Option> options;
    for (int i = 0; i < 4; ++i) {
        options.push_back({labels[i], {PanelAction::LidsPage, nullptr, static_cast<int>(pages[i])}});
    }
    drawPageSwitch(pen, kSubTabY, kSubTabH, options, static_cast<int>(lidsPage_), -1);
    snapMark_ = {};
    switch (lidsPage_) {
        case LidsPage::Look: drawLidsLook(pen, t, m, v, kAdvPageTop); break;
        case LidsPage::Blinks: drawLidsBlinks(pen, t, m, v, kAdvPageTop); break;
        case LidsPage::Fine: drawLidsDetails(pen, t, m, v, kAdvPageTop); break;
        case LidsPage::Brows: drawLidsBrows(pen, t, v, kAdvPageTop); break;
    }
}

void EyePanel::drawLidsBrows(const Pen& pen, const UiText& t, const SettingsView& v, double top) {
    const double cy = (kRowH - kControlH) / 2;
    double y = top;
    // Brows raised with widening (brow_from_widen): with the cameras or without (Valve's openness widens too), and
    // whatever "Widening" says (the brows go by the widening before it is held back). What it does under its choices
    const bool brows = v.flag(key::kBrowFromWiden);
    {
        const bool locked = v.locked(key::kBrowFromWiden);
        drawRowLabel(pen, t, y, kRowH, t.rowBrowFromWiden, t.browFromWidenHint, locked);
        drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                      {{t.widenSwitchOn, {PanelAction::SetBool, key::kBrowFromWiden, 1}},
                       {t.widenSwitchOff, {PanelAction::SetBool, key::kBrowFromWiden, 0}}},
                      brows ? 0 : 1, 20, locked);
        // Up to three lines; a line break in the words starts a new line there (between sentences in Japanese)
        double baseline = y + kRowH + 12;
        size_t lines = 0;
        const std::string note = t.browFromWidenNote;
        for (size_t from = 0; from <= note.size() && lines < 3;) {
            const size_t end = std::min(note.find('\n', from), note.size());
            for (const std::string& line :
                 wrapText(pen, note.substr(from, end - from), 14, false, kInnerRight - kControlX, 3 - lines)) {
                pen.text(kControlX, baseline, line, 14, kTextMuted);
                baseline += 19;
                ++lines;
            }
            from = end + 1;
        }
        y = baseline - 7;
        drawRowDivider(pen, y);
    }
    // How far they go up (brow_strength): only while it is on
    if (brows) {
        const bool locked = v.locked(key::kBrowStrength);
        drawRowLabel(pen, t, y, kRowH, t.rowBrowStrength, t.browStrengthHint, locked);
        drawNumberSlider(pen, key::kBrowStrength, numberShown(key::kBrowStrength, v.number(key::kBrowStrength)),
                         t.browStrengthWeak, t.browStrengthStrong, kControlX, kInnerRight, y + 12, kRowH - 24, !locked);
        y += kRowH;
        drawRowDivider(pen, y);
    }
}

void EyePanel::drawLidsLook(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v, double top) {
    const EyeStatus& s = m.status;
    const WidenSlider widen = widenSlider(v, s);
    const double cy = (kRowH - kControlH) / 2;
    double y = top;

    // The eye cameras in use (set up and switched on, on the Basic tab): widening is theirs
    const bool camera = eyecam::inUse(m.eyecam, v.flag(key::kCameraLids));
    // Where the eyelids come from now, and where the cameras are switched on and off (once they are set up)
    {
        const double h = 52;
        const bool used[2] = {camera && s.running && s.camera.used[0], camera && s.running && s.camera.used[1]};
        fillRounded(pen, kInnerX, y, kInnerRight - kInnerX, h, 12, kBg);
        if (camera) {
            drawCameraIcon(pen, kInnerX + 30, y + h / 2, used[0] || used[1] ? kSuccess : kTextMuted);
        } else {
            drawEyeIcon(pen, kInnerX + 30, y + h / 2, kTextMuted);
        }
        const char* source = !camera                ? t.lidsNowValve
                             : used[0] && used[1] ? t.lidsNowBoth
                             : used[0]            ? t.lidsNowLeft
                             : used[1]            ? t.lidsNowRight
                                                  : t.lidsNowWaiting;
        const char* note = eyecam::setUp(m.eyecam) ? t.lidsNowSwitch : "";
        const double noteSize = 14;
        const double noteW = note[0] == '\0' ? 0.0 : pen.measure(note, noteSize, false);
        if (noteW > 0) pen.text(kInnerRight - 16, centerBaseline(y, h, noteSize), note, noteSize, kTextMuted, false, true);
        double x = kInnerX + 52;
        x += pen.text(x, centerBaseline(y, h, 15), t.lidsNowLabel, 15, kTextMuted) + 12;
        const double room = kInnerRight - 16 - noteW - 16 - x;
        const double size = fitSize(pen, source, 19, 13, room, true);
        pen.text(x, centerBaseline(y, h, size), source, size, kText, true);
        y += h + 10;
    }
    // Widening at all (widen): with the cameras or without. Off: no eyelid above relaxed open, closing and squint as
    // they are; the slider below is greyed meanwhile
    const bool widens = v.flag(key::kWiden);
    {
        const bool locked = v.locked(key::kWiden);
        drawRowLabel(pen, t, y, kRowH, t.rowWidenSwitch, t.widenSwitchHint, locked);
        drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                      {{t.widenSwitchOn, {PanelAction::SetBool, key::kWiden, 1}},
                       {t.widenSwitchOff, {PanelAction::SetBool, key::kWiden, 0}}},
                      widens ? 0 : 1, 20, locked);
        y += kRowH;
        drawRowDivider(pen, y);
    }
    // How easily widening shows: one slider, whatever drives it (only with the eye cameras: a line at the end otherwise)
    if (camera && widen.control == WidenControl::Saturated) {
        // Nothing to widen by on this SteamOS without the cameras: one row saying so, and the way to the cameras
        drawRowLabel(pen, t, y, kRowH, t.rowWidenEase, t.widenHintSaturated, false, 0, true);
        double textRight = kInnerRight;
        if (eyecamTab_) {
            const double bh = 46;
            const std::string label = std::string(t.lidsCamButton) + "  " ICON_ARROW_RIGHT;
            const double bw = std::max(160.0, pen.measure(label, 17, true) + 40);
            const double bx = kInnerRight - bw;
            drawButton(pen, bx, y + (kRowH - bh) / 2, bw, bh, label,
                       {PanelAction::Tab, nullptr, static_cast<int>(PanelTab::Eyecam)}, true, false, 17);
            textRight = bx - 14;
        }
        drawInfoIcon(pen, kControlX + 10, y + kRowH / 2, kTextMuted);
        const double tx = kControlX + 30;
        const double size = fitSize(pen, t.widenSaturated, 17, 11, textRight - tx, false);
        pen.text(tx, centerBaseline(y, kRowH, size), t.widenSaturated, size, kText);
        y += kRowH;
    } else if (camera) {
        const double h = 78;
        const bool locked = widen.control == WidenControl::Valve && v.locked(key::kLidWiden);
        const char* hint = widen.control == WidenControl::Camera  ? t.widenHintCamera
                           : widen.control == WidenControl::Mixed ? t.widenHintMixed
                                                                  : t.widenHintValve;
        drawRowLabel(pen, t, y, h, t.rowWidenEase, hint, locked, 0, true);
        drawWidenSlider(pen, t, m, v, widen, kControlX, kInnerRight, y + 6, 40);
        // (an eye on Valve's values without a fit doesn't widen at all while the openness is saturated: one eye on
        // the cameras with the other on a saturated SteamOS)
        const bool saturated = m.status.running && m.status.opennessSaturated;
        const char* unfitted = saturated ? t.widenSaturated : t.widenNoteUnfitted;
        const char* note = !widens                              ? t.widenNoteOff
                           : widen.unfittedNote                   ? unfitted
                           : widen.control == WidenControl::Valve ? t.widenNoteValve
                                                                  : t.widenNoteCamera;
        pen.text(kControlX, y + 66, note, fitSize(pen, note, 14, 11, kInnerRight - kControlX, false), kTextMuted);
        y += h;
    }
    // The eyelid veto (eyecam-rec's lid_veto): with the eye cameras, from an eyecam-rec that says it. It shows what
    // status.json says; a press sends "set lid_veto on|off"
    const eyecam::Status& es = m.eyecam.status;
    if (camera && es.hasLidVeto) {
        drawRowDivider(pen, y);
        drawRowLabel(pen, t, y, kRowH, t.rowLidVeto, "", false, 0, true);
        drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                      {{t.widenSwitchOn, {PanelAction::EyecamLidVeto, nullptr, 1}},
                       {t.widenSwitchOff, {PanelAction::EyecamLidVeto, nullptr, 0}}},
                      es.lidVeto ? 0 : 1, 20);
        double baseline = y + kRowH + 12;
        for (const std::string& line : wrapText(pen, t.lidVetoNote, 14, false, kInnerRight - kControlX, 3)) {
            pen.text(kControlX, baseline, line, 14, kTextMuted);
            baseline += 19;
        }
        y = baseline - 7;
    }
    if (camera) drawRowDivider(pen, y);
    // Eyelid smoothing: four presets (the two values themselves on "Valve eyelids")
    {
        const bool locked = v.locked(key::kLidMinCutoff) || v.locked(key::kLidBeta);
        const int preset = matchingLidPreset(v);
        drawRowLabel(pen, t, y, kRowH, t.rowLidSmooth, preset < 0 ? t.custom : t.lidSmoothHint, locked);
        std::vector<Option> options;
        const char* names[kLidPresetCount] = {t.strengthLight, t.strengthMedium, t.strengthStrong, t.strengthMax};
        for (int i = 0; i < kLidPresetCount; ++i) options.push_back({names[i], {PanelAction::LidPreset, nullptr, i}});
        // (wider than the other switches, so four English names keep their size)
        drawSegmented(pen, kControlX, y + cy, 400, kControlH, options, preset, 20, locked);
        y += kRowH;
        drawRowDivider(pen, y);
    }
    // Sync both lids: a slider (0 = off)
    {
        const bool locked = v.locked(key::kLidSync);
        drawRowLabel(pen, t, y, kRowH, t.rowSync, t.syncHint, locked);
        drawNumberSlider(pen, key::kLidSync, numberShown(key::kLidSync, v.number(key::kLidSync)), t.syncOff,
                         t.syncStrong, kControlX, kInnerRight, y + 12, kRowH - 24, !locked);
        y += kRowH;
        drawRowDivider(pen, y);
    }
    // Without the eye cameras: what they would add here
    if (!camera) drawCameraLine(pen, t.cameraLineWiden, y + 16);
}

void EyePanel::drawLidsBlinks(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v,
                              double top) {
    const EyeStatus& s = m.status;
    const double cy = (kRowH - kControlH) / 2;
    double y = top;

    // Blinks: how long a closed eye stays closed
    {
        const bool locked = v.locked(key::kBlinkHoldMs);
        drawRowLabel(pen, t, y, kRowH, t.rowBlink, t.blinkHint, locked);
        const double labelW = 52;
        const double stepperW = 220;
        pen.text(kControlX, centerBaseline(y, kRowH, 18), t.blinkHold,
                 fitSize(pen, t.blinkHold, 18, 12, labelW - 6, true), kText, true);
        const double hold = v.number(key::kBlinkHoldMs);
        char holdText[32];
        std::snprintf(holdText, sizeof(holdText), "%.0f ms", hold);
        drawStepper(pen, kControlX + labelW, y + cy, stepperW, kControlH, key::kBlinkHoldMs, hold, holdText, true, locked);
        const double nx = kControlX + labelW + stepperW + 16;
        const std::vector<std::string> lines = wrapText(pen, t.blinkHoldCaption, 15, false, kInnerRight - nx, 2);
        double baseline = y + kRowH / 2 + 5 - (static_cast<double>(lines.size()) - 1) * 10;
        for (const std::string& line : lines) {
            pen.text(nx, baseline, line, 15, kTextMuted);
            baseline += 20;
        }
        y += kRowH;
        drawRowDivider(pen, y);
    }
    // ...closing both when one is closed (blink_sync_below; 0 = off)
    {
        const bool locked = v.locked(key::kBlinkSyncBelow);
        drawRowLabel(pen, t, y, kRowH, t.rowBlinkBoth, t.blinkBothHint, locked);
        const double stepperW = 196;
        const double sync = v.number(key::kBlinkSyncBelow);
        drawStepper(pen, kControlX, y + cy, stepperW, kControlH, key::kBlinkSyncBelow, sync,
                    formatSetting(key::kBlinkSyncBelow, sync), true, locked);
        const double nx = kControlX + stepperW + 16;
        const std::vector<std::string> lines = wrapText(pen, t.blinkBothNote, 13, false, kInnerRight - nx, 3);
        double baseline = y + kRowH / 2 + 4 - (static_cast<double>(lines.size()) - 1) * 8.5;
        for (const std::string& line : lines) {
            pen.text(nx, baseline, line, 13, kTextMuted);
            baseline += 17;
        }
        y += kRowH;
        drawRowDivider(pen, y);
    }
    y = drawSentLids(pen, t, m, v, y);
    // The lowest eyelid while an eye camera sees the eye open (camera_lid_floor; 0 = off): while the eye cameras are in
    // use (before they know the eyes too, so it can be set; without an eye on them the note says when it takes effect).
    // Otherwise a line saying what they would add. Its stepper's edges in the green of its line on the bars above
    if (!eyecam::inUse(m.eyecam, v.flag(key::kCameraLids))) {
        drawCameraLine(pen, t.cameraLineFloor, y + 16);
    } else {
        const bool onCameras = s.running && (s.camera.used[0] || s.camera.used[1]);
        const bool locked = v.locked(key::kCameraLidFloor);
        drawRowLabel(pen, t, y, kRowH, t.rowLidFloor, t.lidFloorHint, locked, 0, true);
        const double stepperW = 196;
        const double floor = v.number(key::kCameraLidFloor);
        drawStepper(pen, kControlX, y + cy, stepperW, kControlH, key::kCameraLidFloor, floor,
                    formatSetting(key::kCameraLidFloor, floor), true, locked, -1e9, 1e9, &kSuccess);
        const double nx = kControlX + stepperW + 16;
        {
            const char* note = onCameras ? t.lidFloorNote : t.lidFloorNoteIdle;
            const std::vector<std::string> lines = wrapText(pen, note, 13, false, kInnerRight - nx, 3);
            double baseline = y + kRowH / 2 + 4 - (static_cast<double>(lines.size()) - 1) * 8.5;
            for (const std::string& line : lines) {
                pen.text(nx, baseline, line, 13, kTextMuted);
                baseline += 17;
            }
        }
        y += kRowH;
        drawRowDivider(pen, y);
    }
}

double EyePanel::drawSentLids(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v, double y) {
    const EyeStatus& s = m.status;
    cairo_t* cr = pen.cr;
    const double cy = (kRowH - kControlH) / 2;
    // The title, and what the bars show
    {
        y += 8;
        const double titleSize = 18;
        pen.text(kInnerX + 4, y + 22, t.sentLidsTitle, titleSize, kText, true);
        const double hx = kInnerX + 4 + pen.measure(t.sentLidsTitle, titleSize, true) + 14;
        const double size = fitSize(pen, t.sentLidsHint, 14, 10, kInnerRight - hx, false);
        pen.text(hx, y + 21, t.sentLidsHint, size, kTextMuted);
        y += 34;
    }
    // Each eye's eyelid as sent now (VRCFT, 0..1), in the cameras' green where its eyelid is theirs and with whose it is
    // on the right; relaxed open dotted; ③ for the eyes without the cameras' eyelid (lid_open_snap) and for those with
    // it (camera_lid_open_snap), each faded while it applies to neither eye; the camera floor in green while it is on
    // (the cameras in use), faded while no eye is on them
    {
        const double chipW = 92;
        const double barX = kInnerX + 34;
        const double barW = kInnerRight - chipW - barX;
        /**
         * x of a sent eyelid on the bars.
         */
        const auto xOf = [&](double value) { return std::round(barX + std::clamp(value, 0.0, 1.0) * barW); };
        const bool live = s.running && s.tracking && s.hasSent;
        const bool onCamera[2] = {live && s.camera.used[0], live && s.camera.used[1]};
        const double badgeY = y + 10;
        const double top = y + 24;
        const double bh = 18;
        const double step = 28;
        const double bottom = top + step + bh;
        for (int eye = 0; eye < 2; ++eye) {
            const double by = top + eye * step;
            pen.text(kInnerX, centerBaseline(by, bh, 17), eye == 0 ? t.left : t.right, 17, kText, true);
            fillRounded(pen, barX, by, barW, bh, 6, kBg);
            const double value = live ? s.lidsVrcft.v[eye] : NAN;
            if (std::isfinite(value)) {
                fillRounded(pen, barX, by, std::max(12.0, xOf(value) - barX), bh, 6, onCamera[eye] ? kSuccess : kAccent);
            }
            strokeRounded(pen, barX, by, barW, bh, 6, kBorder, 1.5);
            if (live) {
                const double cx = barX + barW + 14;
                if (onCamera[eye]) {
                    cameraBadge(pen, t, cx, by + bh / 2);
                } else {
                    const double w = pen.measure(t.chipValve, 13, true) + 18;
                    strokeRounded(pen, cx, by - 2, w, bh + 4, (bh + 4) / 2, kBorder, 1.5);
                    pen.text(cx + 9, centerBaseline(by - 2, bh + 4, 13), t.chipValve, 13, kTextMuted, true);
                }
            }
        }
        if (!live) textCentered(pen, barX + barW / 2, centerBaseline(top, bh, 14), t.noEyeData, 14, kTextMuted, false);
        // Relaxed open: dotted
        {
            const double x = xOf(kLidRelaxed);
            const double dots[2] = {2, 3};
            cairo_set_dash(cr, dots, 2, 0);
            pen.color(kTextMuted);
            cairo_set_line_width(cr, 2);
            cairo_move_to(cr, x, top - 4);
            cairo_line_to(cr, x, bottom + 4);
            cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0);
        }
        // The camera floor
        const double floor = v.number(key::kCameraLidFloor);
        if (eyecam::inUse(m.eyecam, v.flag(key::kCameraLids)) && std::isfinite(floor) && floor > 0) {
            const bool any = onCamera[0] || onCamera[1];
            const double x = xOf(floor);
            for (const bool edge : {true, false}) {
                pen.color(edge ? kBg : (any || !live ? kSuccess : kSuccessFaded));
                cairo_set_line_width(cr, edge ? 5 : 2);
                cairo_move_to(cr, x, top - 4);
                cairo_line_to(cr, x, bottom + 4);
                cairo_stroke(cr);
            }
        }
        // ③, both values
        snapMark_ = {};
        const char* keys[2] = {key::kLidOpenSnap, key::kCameraLidOpenSnap};
        const double dash[2] = {5, 4};
        for (int i = 0; i < 2; ++i) {
            const double snap = v.number(keys[i]);
            if (!std::isfinite(snap) || snap >= kLidRelaxed - 1e-9) continue;
            // (without eye data, as for the eyes as set up: the cameras' value where they are in use)
            const bool applies = live ? (i == 0 ? !onCamera[0] || !onCamera[1] : onCamera[0] || onCamera[1])
                                      : (i == 0 || eyecam::inUse(m.eyecam, v.flag(key::kCameraLids)));
            snapMark_.shown = true;
            snapMark_.x[i] = xOf(snap);
            snapMark_.faded[i] = !applies;
            cairo_set_dash(cr, dash, 2, 0);
            for (const bool edge : {true, false}) {
                pen.color(edge ? kBg : (applies ? kSnap : kSnapFaded));
                cairo_set_line_width(cr, edge ? 5 : 2);
                cairo_move_to(cr, snapMark_.x[i], top - 4);
                cairo_line_to(cr, snapMark_.x[i], bottom + 4);
                cairo_stroke(cr);
            }
            cairo_set_dash(cr, nullptr, 0, 0);
        }
        // Their badges, the second moved aside where they would overlap, with a short lead back to its line
        for (int i = 0; i < 2; ++i) {
            if (!std::isfinite(snapMark_.x[i])) continue;
            double bx = snapMark_.x[i];
            if (i == 1 && std::isfinite(snapMark_.badgeX[0]) && std::abs(bx - snapMark_.badgeX[0]) < 22) {
                bx = snapMark_.badgeX[0] + (bx >= snapMark_.x[0] ? 22 : -22);
            }
            bx = std::clamp(bx, barX + 10, barX + barW - 10);
            const Color fill = snapMark_.faded[i] ? kSnapFaded : kSnap;
            if (std::abs(bx - snapMark_.x[i]) > 0.5) {
                pen.color(fill);
                cairo_set_line_width(cr, 2);
                cairo_move_to(cr, bx, badgeY + 6);
                cairo_line_to(cr, snapMark_.x[i], top - 4);
                cairo_stroke(cr);
            }
            drawNumberBadge(pen, bx, badgeY, 3, fill);
            snapMark_.badgeX[i] = bx;
        }
        y = bottom + 14;
    }
    // ③'s row: both values, each with whose eyes it is for on its left. In ③'s color, as are the steppers' edges
    {
        const bool locked = v.locked(key::kLidOpenSnap) || v.locked(key::kCameraLidOpenSnap);
        drawNumberBadge(pen, kInnerX + 10, y + kRowH / 2 - 9, 3, kSnap);
        drawRowLabel(pen, t, y, kRowH, t.rowLidOpenSnap, t.lidOpenSnapHint, locked, 26);
        const double stepperW = 150;
        {
            const char* keys[2] = {key::kLidOpenSnap, key::kCameraLidOpenSnap};
            const char* captions[2] = {t.capSnapValve, t.capSnapCamera};
            const double captionW = 84;
            for (int i = 0; i < 2; ++i) {
                const double x = kControlX + i * (captionW + stepperW + 12);
                const double value = v.number(keys[i]);
                const double size = fitSize(pen, captions[i], 15, 10, captionW - 6, true);
                pen.text(x, centerBaseline(y, kRowH, size), captions[i], size, kText, true);
                drawStepper(pen, x + captionW, y + cy, stepperW, kControlH, keys[i], value,
                            formatSetting(keys[i], value), true, v.locked(keys[i]), -1e9, 1e9, &kSnap);
            }
        }
        y += kRowH;
        drawRowDivider(pen, y);
    }
    return y;
}

void EyePanel::drawLidsDetails(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v,
                               double top) {
    const EyeStatus& s = m.status;
    cairo_t* cr = pen.cr;
    double y = top;
    const double cy = (kRowH - kControlH) / 2;
    // Eyes with an eye fit: closing and opening come from the fit, widening from the slider (lid_widen). The learned
    // calibration and the marks only matter for eyes without one
    const WidenState widen = widenState(v);
    const bool anyFitted = widen.fitted[0] || widen.fitted[1];
    const bool fromCameras = lidsFromCameras(s);

    // The page is how Valve's openness is read (the sent eyelid, with ③, is on "Blinks & squint"): its title, and beside
    // it what decides the marks now. While the cameras supply both eyes all of it only stands by for when they lose an
    // eye, so it says so and is drawn lighter, still to be changed
    const bool standby = s.running && s.camera.used[0] && s.camera.used[1];
    {
        const char* note = t.marksTitle;
        bool notice = false;
        if (anyFitted) note = t.lidMarksUnused;
        if (widen.mode > 0 && widen.fitted[0] && widen.fitted[1]) {
            if (!widen.room[0] && widen.room[1]) note = t.widenFollowsLeft;
            if (widen.room[0] && !widen.room[1]) note = t.widenFollowsRight;
            if (!widen.room[0] && !widen.room[1]) note = t.widenNoRoom;
            notice = widen.room[0] != widen.room[1] || !widen.room[0];
        }
        // On a SteamOS where a relaxed open eye already reads 1.0 there is nothing above it to widen by
        if (s.running && s.opennessSaturated) {
            note = t.opennessSaturated;
            notice = true;
        }
        // (while the cameras drive both eyelids all of it stands by, the cap included)
        if (standby) {
            note = t.valveReadStandby;
            notice = true;
        }
        const double titleSize = 18;
        pen.text(kInnerX + 4, y + 22, t.valveReadTitle, titleSize, kText, true);
        const double nx = kInnerX + 4 + pen.measure(t.valveReadTitle, titleSize, true) + 14;
        const double size = fitSize(pen, note, 15, 10, kInnerRight - nx, notice);
        pen.text(nx, y + 21, note, size, notice ? kText : kTextMuted, notice);
        y += 38;
    }
    if (standby) cairo_push_group(cr);
    // Auto calibration: on / off, what it learned, reset (only for eyes without a fit, and not from the cameras)
    if (!anyFitted && !fromCameras) {
        const bool locked = v.locked(key::kLidCalibration);
        const bool on = v.flag(key::kLidCalibration);
        std::string learned;
        if (s.running && (std::isfinite(s.relaxed.v[0]) || std::isfinite(s.relaxed.v[1]))) {
            char text[128];
            std::snprintf(text, sizeof(text), t.learnedFormat, twoDecimals(s.relaxed.v[0]).c_str(),
                          twoDecimals(s.relaxed.v[1]).c_str());
            learned = text;
        } else if (s.running && on) {
            learned = t.notLearned;
        }
        drawRowLabel(pen, t, y, kRowH, t.rowCalibration, learned, locked);
        drawSegmented(pen, kControlX, y + cy, 180, kControlH,
                      {{t.on, {PanelAction::SetBool, key::kLidCalibration, 1}},
                       {t.off, {PanelAction::SetBool, key::kLidCalibration, 0}}},
                      on ? 0 : 1, 20, locked);
        if (s.running && s.learning) {
            const double lx = kControlX + 194;
            drawDot(cr, lx + 6, y + kRowH / 2, 5, kAccent);
            pen.text(lx + 18, centerBaseline(y, kRowH, 16), t.learning,
                     fitSize(pen, t.learning, 16, 11, kInnerRight - 158 - lx - 18, false), kText);
        }
        const PanelHit hit {PanelAction::CalibrationReset, key::kCalibrationReset, 0};
        const double bw = 150;
        const double bx = kInnerRight - bw;
        const int pointer = on ? pointerState(hit) : 0;
        fillRounded(pen, bx, y + cy, bw, kControlH, kControlH / 2, pointer > 0 ? kControlHover : kControl);
        if (on) strokeRounded(pen, bx, y + cy, bw, kControlH, kControlH / 2, kBorder, 2);
        const double size = fitSize(pen, t.calibrationReset, 19, 12, bw - 24, true);
        textCentered(pen, bx + bw / 2, centerBaseline(y + cy, kControlH, size), t.calibrationReset, size,
                     on ? kText : kTextDisabled, true);
        addButton(hit, bx, y + cy, bw, kControlH, on);
        y += kRowH + kRowGap;
    }
    // Per-eye scales: automatic (from calibration) or fixed; for a fitted eye a fine-tune after the fit (automatic
    // is 1 there)
    {
        const FitInConfig lidFit = fitInConfig(v);
        const bool anyLidFit = lidFit.lidsFitted[0] || lidFit.lidsFitted[1];
        const bool lockedL = v.locked(key::kLidScaleLeft);
        const bool lockedR = v.locked(key::kLidScaleRight);
        const double left = v.number(key::kLidScaleLeft);
        const double right = v.number(key::kLidScaleRight);
        const bool fixedL = std::isfinite(left);
        const bool fixedR = std::isfinite(right);
        const int selected = !fixedL && !fixedR ? 0 : (fixedL && fixedR ? 1 : -1);
        const char* hint = anyLidFit ? t.hintScaleFitted : (selected == 0 ? t.hintScaleAuto : t.hintScaleFixed);
        drawRowLabel(pen, t, y, kRowH, t.rowScale, hint, lockedL || lockedR);
        drawSegmented(pen, kControlX, y + cy, 170, kControlH,
                      {{t.scaleAuto, {PanelAction::ScaleAuto, nullptr, 0}},
                       {t.scaleFixed, {PanelAction::ScaleFixed, nullptr, 0}}},
                      selected, 19, lockedL || lockedR);
        const double stepperW = (kControlW - 170 - 10 - 44 - 8) / 2;
        for (int eye = 0; eye < 2; ++eye) {
            const char* name = eye == 0 ? key::kLidScaleLeft : key::kLidScaleRight;
            const bool fixed = eye == 0 ? fixedL : fixedR;
            const double value = fixed ? (eye == 0 ? left : right) : (s.running ? s.scales.v[eye] : NAN);
            const double lx = kControlX + 180 + eye * (22 + stepperW + 8);
            pen.text(lx, centerBaseline(y, kRowH, 18), eye == 0 ? t.left : t.right, 18, kText, true);
            drawStepper(pen, lx + 22, y + cy, stepperW, kControlH, name, value, twoDecimals(value), fixed,
                        eye == 0 ? lockedL : lockedR);
        }
    }
    y += kRowH + kRowGap;
    // The raw openness of each eye with the two marks laid over it. No widening marks: SteamOS caps the openness at
    // 1.0 (a relaxed open eye reads it), so above relaxed open nothing comes from Valve's values for eyes without a
    // fit; lid_widen_start and lid_wide stay settings (config.json, --lid-widen-start, --lid-wide)
    const char* marks[2] = {key::kLidClosed, key::kLidOpen};
    {
        const double top = y - 14;
        const double barX = kInnerX + 34;
        const double barW = kInnerRight - barX;
        /**
         * x of an openness value on the bars.
         */
        const auto xOf = [&](double value) { return barX + std::clamp(value / kLidScaleMax, 0.0, 1.0) * barW; };
        const bool live = s.running && s.tracking && s.hasRaw;
        for (int eye = 0; eye < 2; ++eye) {
            const double by = top + 50 + eye * 28;
            const double bh = 20;
            pen.text(kInnerX, centerBaseline(by, bh, 17), eye == 0 ? t.left : t.right, 17, kText, true);
            fillRounded(pen, barX, by, barW, bh, 6, kBg);
            const double value = live ? s.opennessScaled.v[eye] : NAN;
            if (std::isfinite(value)) fillRounded(pen, barX, by, std::max(12.0, xOf(value) - barX), bh, 6, kAccent);
            strokeRounded(pen, barX, by, barW, bh, 6, kBorder, 1.5);
        }
        if (!live) {
            textCentered(pen, barX + barW / 2, centerBaseline(top + 50, 20, 14), t.noEyeData, 14, kTextMuted, false);
        }
        // Mark lines: a light line with dark edges, visible on the accent fill and on the dark track
        for (int i = 0; i < 2; ++i) {
            const double x = std::round(xOf(v.number(marks[i])));
            pen.color(kBg);
            cairo_set_line_width(cr, 6);
            cairo_move_to(cr, x, top + 44);
            cairo_line_to(cr, x, top + 104);
            cairo_stroke(cr);
            pen.color(kText);
            cairo_set_line_width(cr, 2);
            cairo_move_to(cr, x, top + 44);
            cairo_line_to(cr, x, top + 104);
            cairo_stroke(cr);
            drawNumberBadge(pen, x, top + 34, i + 1);
        }
        y = top + 110;
    }
    // The two marks, side by side
    {
        const char* captions[2] = {t.markClosed, t.markOpen};
        const double gap = 12;
        const double w = (kInnerRight - kInnerX - gap) / 2;
        for (int i = 0; i < 2; ++i) {
            const double x = kInnerX + i * (w + gap);
            drawCaption(pen, x + 2, y + 18, captions[i], i + 1, v.locked(marks[i]));
            double low = 0;
            double high = 0;
            lidMarkBounds(marks[i], v, low, high);
            const double value = v.number(marks[i]);
            drawStepper(pen, x, y + 28, w, kControlH, marks[i], value, formatSetting(marks[i], value), true,
                        v.locked(marks[i]), low, high);
        }
        y += kCaptionRowH + kRowGap;
    }
    if (standby) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, kStandbyAlpha);
    }
    // Eyelid smoothing: the two One Euro values behind the presets, each named on its left (one row high, so the
    // page fits with the row above)
    {
        drawRowLabel(pen, t, y, kRowH, t.rowLidSmooth, t.lowerSmoother, false);
        const char* keys[2] = {key::kLidMinCutoff, key::kLidBeta};
        const char* captions[2] = {t.capStill, t.capFast};
        const double stepperW = 140;
        const double half = (kControlW - 16) / 2;
        for (int i = 0; i < 2; ++i) {
            const double x = kControlX + i * (half + 16);
            const double captionW = half - stepperW - 8;
            const bool locked = v.locked(keys[i]);
            const double size = fitSize(pen, captions[i], 15, 10, captionW, false);
            pen.text(x, centerBaseline(y, kRowH, size), captions[i], size, locked ? kTextDisabled : kTextMuted);
            const double value = v.number(keys[i]);
            drawStepper(pen, x + captionW + 8, y + cy, stepperW, kControlH, keys[i], value,
                        formatSetting(keys[i], value), true, locked);
        }
    }
}

double EyePanel::drawSectionTitle(const Pen& pen, double y, const std::string& title, double lineRight) {
    pen.text(kInnerX, y + 20, title, 15, kTextMuted, true);
    pen.color(kDivider);
    cairo_set_line_width(pen.cr, 1);
    cairo_move_to(pen.cr, kInnerX, y + 29.5);
    cairo_line_to(pen.cr, lineRight > 0 ? lineRight : kInnerRight, y + 29.5);
    cairo_stroke(pen.cr);
    return 36;
}

void EyePanel::drawAdvanced(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    if (historyOpen_) {
        drawHistory(pen, t, m);
        return;
    }
    if (diagOpen_) {
        drawDiag(pen, t, m);
        return;
    }
    if (!recordOpen_.empty()) {
        drawRecord(pen, t, m);
        return;
    }
    if (recordsAllOpen_) {
        drawRecordsAll(pen, t, m);
        return;
    }
    if (appsListOpen_) {
        const size_t before = buttons_.size();
        drawAppsList(pen, t, m);
        if (appsConfirmOpen_) {
            // Over the list, which can't be pressed then and is dimmed further (the tabs and the left column stay)
            buttons_.resize(before);
            pen.color(kBg, 0.7);
            pen.roundedRect(kRightX, kContentY, kRight - kRightX, kContentH, 20);
            cairo_fill(pen.cr);
            drawAppsConfirm(pen, t, m);
        }
        return;
    }
    // "Record for squint training": its sheet in the tab's place (its recording runs in it)
    if (contribOpen_) {
        drawContrib(pen, t, m);
        return;
    }
    // The developer recording: its light warning, and its steps while it runs, in the tab's place (one for squint
    // training with its sheet closed: titled with that)
    const eyecam::State recording = m.eyecam.status.state;
    if (eyecamConfirm_.isOpen() ||
        (m.eyecam.visible && (recording == eyecam::State::Searching || recording == eyecam::State::Recording))) {
        drawRun(pen, t, m, m.contrib.running ? t.contribRow : "");
        return;
    }
    // "Saved data"'s confirmation before a delete, in the Data page's place
    if (dataConfirm_ >= 0 && advPage_ == AdvPage::Data) {
        drawDataDelete(pen, t, m);
        return;
    }
    // The sub-tabs, and under them the one chosen (scrolled only if it doesn't fit)
    drawSubTabs(pen, t, m, kSubTabY, kSubTabH);
    drawScrolled(pen, kAdvPageTop, kAdvViewBottom, advScroll_, advMaxScroll_, advViewH_, advScrollable_,
                 [&](double top, double right) { return drawAdvancedPage(pen, t, m, v, top, right); });
}

void EyePanel::drawSubTabs(const Pen& pen, const UiText& t, const PanelModel& m, double y, double h) {
    const AdvPage pages[5] = {AdvPage::Version, AdvPage::Trouble, AdvPage::Tools, AdvPage::Files, AdvPage::Data};
    const char* labels[5] = {t.advVersion, t.advTrouble, t.advTools, t.advFiles, t.advData};
    std::vector<Option> options;
    int selected = 0;
    for (int i = 0; i < 5; ++i) {
        options.push_back({labels[i], {PanelAction::AdvancedPage, nullptr, static_cast<int>(pages[i])}});
        if (advPage_ == pages[i]) selected = i;
    }
    // A new release: a dot after "Version" (not for going back to an older one)
    const bool update = m.update.state == frame_updater::UpdateState::Available && !m.update.backToStable;
    drawPageSwitch(pen, y, h, options, selected, update ? 0 : -1);
}

void EyePanel::drawPageSwitch(const Pen& pen, double y, double h, const std::vector<Option>& pages, int selected,
                              int dot) {
    const int count = static_cast<int>(pages.size());
    if (count == 0) return;
    const double x0 = kInnerX;
    const double w = kInnerRight - kInnerX;
    fillRounded(pen, x0, y, w, h, h / 2, kBg);
    strokeRounded(pen, x0, y, w, h, h / 2, kBorder, 1.5);
    const double inset = 4;
    const double segW = (w - inset * 2) / count;
    const double segH = h - inset * 2;
    const double dotW = 16;
    // One size for all, the largest at which each fits
    double size = 17;
    for (int i = 0; i < count; ++i) {
        const double room = segW - 24 - (i == dot ? dotW : 0);
        size = std::min(size, fitSize(pen, pages[i].label, 17, 12, room, true));
    }
    for (int i = 0; i < count; ++i) {
        const double sx = x0 + inset + segW * i;
        const double sy = y + inset;
        const PanelHit& hit = pages[i].hit;
        const bool chosen = i == selected;
        const int pointer = chosen ? 0 : pointerState(hit);
        if (chosen) {
            fillRounded(pen, sx, sy, segW, segH, segH / 2, kAccent);
        } else if (pointer > 0) {
            fillRounded(pen, sx, sy, segW, segH, segH / 2, kControlHover);
        }
        const Color color = chosen ? kOnAccent : kText;
        const double textW = pen.measure(pages[i].label, size, chosen) + (i == dot ? dotW : 0);
        const double tx = sx + (segW - textW) / 2;
        pen.text(tx, centerBaseline(sy, segH, size), pages[i].label, size, color, chosen);
        if (i == dot) drawDot(pen.cr, tx + textW - dotW / 2 + 3, sy + segH / 2, 4.5, chosen ? kOnAccent : kAccent);
        addButton(hit, sx, y, segW, h, !chosen);
    }
}

void EyePanel::drawScrolled(const Pen& pen, double viewTop, double viewBottom, double& scroll, double& maxScroll,
                            double& viewH, bool& scrollable,
                            const std::function<double(double top, double right)>& page) {
    cairo_t* cr = pen.cr;
    viewH = viewBottom - viewTop;
    const size_t firstButton = buttons_.size();
    const size_t firstName = recordNames_.size();

    // As it is when it fits
    cairo_push_group(cr);
    const double fullH = page(viewTop, kInnerRight);
    if (fullH <= viewH) {
        cairo_pop_group_to_source(cr);
        cairo_paint(cr);
        scroll = 0.0;
        maxScroll = 0.0;
        scrollable = false;
        return;
    }
    cairo_pattern_destroy(cairo_pop_group(cr));
    buttons_.resize(firstButton);
    recordNames_.resize(firstName);

    // Too tall: a little narrower beside ▲ / ▼, clipped to the view and scrolled (only what shows can be pressed)
    const double barX = kInnerRight - kHistoryBarW;
    const double right = barX - 14;
    scrollable = true;
    hitClip_ = true;
    hitClipTop_ = viewTop;
    hitClipBottom_ = viewBottom;
    double pageH = 0.0;
    for (int pass = 0;; ++pass) {
        cairo_push_group(cr);
        cairo_rectangle(cr, kRightX + 2, viewTop, barX - 8 - kRightX, viewH);
        cairo_clip(cr);
        pageH = page(viewTop - scroll, right);
        maxScroll = std::max(0.0, pageH - viewH);
        const double kept = std::clamp(scroll, 0.0, maxScroll);
        if (std::fabs(kept - scroll) < 1e-6 || pass > 0) {
            scroll = kept;
            cairo_pop_group_to_source(cr);
            cairo_paint(cr);
            break;
        }
        // Scrolled past the end (the page got shorter, or --adv-scroll): again, ending at the page's end
        scroll = kept;
        cairo_pattern_destroy(cairo_pop_group(cr));
        buttons_.resize(firstButton);
        recordNames_.resize(firstName);
    }
    hitClip_ = false;
    drawScrollBar(pen, PanelAction::AdvancedScroll, barX, viewTop, viewBottom, scroll, maxScroll, pageH);
}

double EyePanel::drawAdvancedPage(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v,
                                  double top, double right) {
    switch (advPage_) {
        case AdvPage::Version: return drawVersionPage(pen, t, m, v, top, right);
        case AdvPage::Trouble: return drawTroublePage(pen, t, m, top, right);
        case AdvPage::Tools: return drawToolsPage(pen, t, m, v, top, right);
        case AdvPage::Files: return drawFilesPage(pen, t, m, top, right);
        case AdvPage::Data: return drawDataPage(pen, t, m, top, right);
    }
    return 0.0;
}

double EyePanel::drawVersionPage(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v,
                                 double top, double right) {
    using frame_updater::UpdateState;
    const frame_updater::UpdateStatus& u = m.update;
    const std::string current = bareVersion(u.current);
    const std::string latest = bareVersion(u.latest);
    const TestVersionView test = testVersionView(u, updateBeta_);

    /** A button beside the version. */
    struct RowButton {
        PanelHit hit;
        std::string label;
        bool accent;
    };
    const PanelHit dismiss {PanelAction::UpdateDismiss, nullptr, 0};
    const PanelHit install {PanelAction::UpdateInstall, nullptr, 0};
    std::string primary;    // what came of the check
    std::string secondary;  // muted, under it
    bool error = false;     // primary in red, on a line of its own
    std::vector<RowButton> buttons;
    switch (u.state) {
        case UpdateState::Unknown:
            if (u.checking) primary = t.updateChecking;
            break;
        case UpdateState::UpToDate:
            primary = formatText(test.using_ ? t.updateUsingTestFormat : t.updateUpToDateFormat, current);
            break;
        case UpdateState::Available:
            if (test.back) {
                // Going back to the older regular version: offered, not urged (a plain button)
                primary = formatText(t.updateBackFormat, latest);
            } else {
                primary = formatText(test.offered ? t.updateAvailableTestFormat : t.updateAvailableFormat, latest);
            }
            if (!u.installable) {
                secondary = test.back ? t.updateBackManual : t.updateManual;
            } else if (test.back) {
                buttons.push_back({install, t.updateBackButton, false});
            } else {
                buttons.push_back({install, formatText(t.updateToFormat, latest), true});
            }
            break;
        case UpdateState::Installing: primary = formatText(t.updateInstallingFormat, updateStepText(t, u.step)); break;
        case UpdateState::Installed:
            primary = formatText(t.updateInstalledFormat, bareVersion(u.version));
            buttons.push_back({dismiss, t.updateDismiss, false});
            break;
        case UpdateState::CheckFailed:
            primary = std::string(t.updateCheckFailed) + " " + updateReasonText(t, u.error);
            error = true;
            break;
        case UpdateState::InstallFailed:
            primary = std::string(t.updateInstallFailed) + " " + updateReasonText(t, u.error);
            secondary = t.updateLogHint;
            error = true;
            buttons.push_back({install, t.updateRetry, false});
            buttons.push_back({dismiss, t.updateDismiss, false});
            break;
    }
    // A check running over an earlier answer: the answer stays, with a note under it
    if (u.checking && u.state != UpdateState::Unknown && u.state != UpdateState::Installing) {
        secondary = t.updateChecking;
    }

    // The buttons at the right, side by side
    const double bh = 50;
    const double gap = 10;
    std::vector<double> widths;
    double buttonsW = 0;
    for (const RowButton& b : buttons) {
        widths.push_back(std::max(120.0, pen.measure(b.label, 18, true) + 52));
        buttonsW += widths.back() + (buttonsW > 0 ? gap : 0);
    }
    const double textRight = buttons.empty() ? right : right - buttonsW - 20;
    const double textW = textRight - kInnerX;

    // The version (large), then when it was checked and what came of it
    const double y = top + 4;
    const std::string title = current.empty() ? std::string(t.rowVersion) : "v" + current;
    pen.text(kInnerX, y + 32, title, fitSize(pen, title, 32, 20, textW, true), kText, true);
    const std::string checked = u.checkedAt > 0 ? formatText(t.checkedFormat, checkedText(u.checkedAt)) : std::string();
    struct Line {
        std::string text;
        double size;
        Color color;
        bool bold;
    };
    std::vector<Line> lines;
    if (!error) {
        std::string line = checked;
        if (!primary.empty()) line += (line.empty() ? "" : t.condSeparator) + primary;
        for (const std::string& part : wrapText(pen, line, 16, false, textW, 3)) lines.push_back({part, 16, kTextMuted, false});
    } else {
        if (!checked.empty()) lines.push_back({checked, 15, kTextMuted, false});
        for (const std::string& part : wrapText(pen, primary, 16, true, textW, 4)) lines.push_back({part, 16, kDanger, true});
    }
    for (const std::string& part : wrapText(pen, secondary, 14, false, textW, 2)) lines.push_back({part, 14, kTextMuted, false});
    double baseline = y + 32;
    for (const Line& line : lines) {
        baseline += line.size + 8;
        pen.text(kInnerX, baseline, line.text, line.size, line.color, line.bold);
    }
    const double textBottom = baseline + 8;
    const double blockBottom = std::max(textBottom, y + 64);
    double bx = right - buttonsW;
    const double by = y + (blockBottom - y - bh) / 2;
    for (size_t i = 0; i < buttons.size(); ++i) {
        drawButton(pen, bx, by, widths[i], bh, buttons[i].label, buttons[i].hit, true, buttons[i].accent, 18);
        bx += widths[i] + gap;
    }
    double bottom = blockBottom;

    // The new release's summary (from its release notes), up to four lines in a box
    const std::string notes = updateNotes(u, m.language);
    if (!notes.empty()) {
        const double boxTop = bottom + 14;
        const double padX = 18;
        const std::vector<std::string> noteLines = wrapText(pen, notes, 16, false, right - kInnerX - padX * 2, 4);
        const double boxH = 40 + noteLines.size() * 24 + 8;
        fillRounded(pen, kInnerX, boxTop, right - kInnerX, boxH, 14, kControl);
        const std::string heading = formatText(t.updateNotesFormat, latest.empty() ? current : latest);
        pen.text(kInnerX + padX, boxTop + 28, heading, 14, kAccent, true);
        double noteBaseline = boxTop + 34;
        for (const std::string& line : noteLines) {
            noteBaseline += 24;
            pen.text(kInnerX + padX, noteBaseline, line, 16, kText);
        }
        bottom = boxTop + boxH;
    }

    // "Version history", the automatic check (a chip that switches it) and "Check now"
    const double rowTop = bottom + 18;
    const double rowH = 44;
    double x = kInnerX;
    {
        const double w = pen.measure(t.historyButton, 16, true) + 44;
        drawButton(pen, x, rowTop, w, rowH, t.historyButton, {PanelAction::HistoryOpen, nullptr, 0}, true, false, 16);
        x += w + 10;
    }
    const double checkW = pen.measure(t.updateCheckNow, 16, true) + 44;
    {
        const bool checkOn = v.flag(key::kUpdateCheck);
        const PanelHit hit {PanelAction::SetBool, key::kUpdateCheck, checkOn ? 0 : 1};
        const std::string state = checkOn ? t.on : t.off;
        const double labelSize = 15;
        const double stateW = pen.measure(state, labelSize, true);
        const double chipMax = right - checkW - 10 - x;
        const std::string label = ellipsize(pen, t.updateCheckChip, labelSize, false, chipMax - 28 - 10 - stateW, false);
        const double labelW = pen.measure(label, labelSize, false);
        const double chipW = labelW + 10 + stateW + 28;
        const int pointer = pointerState(hit);
        fillRounded(pen, x, rowTop, chipW, rowH, rowH / 2, pointer > 0 ? kControlHover : kControl);
        strokeRounded(pen, x, rowTop, chipW, rowH, rowH / 2, kBorder, 2);
        const double chipBaseline = centerBaseline(rowTop, rowH, labelSize);
        pen.text(x + 14, chipBaseline, label, labelSize, kText);
        pen.text(x + 14 + labelW + 10, chipBaseline, state, labelSize, checkOn ? kAccent : kTextMuted, true);
        addButton(hit, x, rowTop, chipW, rowH);
    }
    // (not while a check or an install runs)
    drawButton(pen, right - checkW, rowTop, checkW, rowH, t.updateCheckNow, {PanelAction::UpdateCheck, nullptr, 0},
               !u.checking && u.state != UpdateState::Installing, false, 16);
    double ey = rowTop + rowH + 16;
    // "Get test versions" (panel.json, not config.json): On / Off, with a small badge while it is off and a test version
    // newer than the running one exists
    {
        const double h = 46;
        const double segW = std::min(200.0, right - kInnerX - 300);
        const double labelW = right - segW - 16 - kInnerX;
        const double badgeSize = 13;
        const double badgeW = test.badge ? pen.measure(t.updateTestAvailable, badgeSize, true) + 20 : 0.0;
        const double labelSize = fitSize(pen, t.rowUpdateBeta, 18, 13, labelW - (test.badge ? badgeW + 10 : 0.0), true);
        pen.text(kInnerX, ey + 20, t.rowUpdateBeta, labelSize, kText, true);
        if (test.badge) {
            const double bx = kInnerX + pen.measure(t.rowUpdateBeta, labelSize, true) + 10;
            const double bh = 22;
            const double by = ey + 13 - bh / 2;
            fillRounded(pen, bx, by, badgeW, bh, bh / 2, kAccentTint);
            strokeRounded(pen, bx, by, badgeW, bh, bh / 2, kAccent, 1.5);
            textCentered(pen, bx + badgeW / 2, centerBaseline(by, bh, badgeSize), t.updateTestAvailable, badgeSize, kAccent,
                         true);
        }
        const char* hint = test.keepHint ? t.hintUpdateBetaKeep : t.hintUpdateBeta;
        pen.text(kInnerX, ey + 40, hint, fitSize(pen, hint, 14, 10, labelW, false), kTextMuted);
        drawSegmented(pen, right - segW, ey + 2, segW, 42,
                      {{t.on, {PanelAction::UpdateBeta, nullptr, 1}}, {t.off, {PanelAction::UpdateBeta, nullptr, 0}}},
                      updateBeta_ ? 0 : 1, 16, false);
        ey += h + 14;
    }
    // Last, the other Frame apps: at the bottom of the card while the page fits, else under the rest (scrolled to)
    if (!m.apps.entries.empty()) {
        const double viewH = kAdvViewBottom - kAdvPageTop;
        const double rowY = std::max(ey + 22, top + viewH - 8 - kAppsRowH);
        drawAppsRow(pen, t, m, kInnerX, rowY, right);
        return rowY + kAppsRowH + 8 - top;
    }
    return ey + 8 - top;
}

void EyePanel::drawAppIcon(const Pen& pen, const frame_apps::Entry& entry, double x, double y, double size) {
    if (frame_apps::drawIcon(pen.cr, entry, x, y, size)) return;
    // No icon (an app only the site lists, whose icon didn't come): its letters on a dark tile
    fillRounded(pen, x, y, size, size, size * 0.22, kBg);
    const double textSize = fitSize(pen, entry.app.mono, std::round(size * 0.34), 9, size - 8, true);
    textCentered(pen, x + size / 2, centerBaseline(y, size, textSize), entry.app.mono, textSize, kAccent, true);
}

void EyePanel::drawAppsRow(const Pen& pen, const UiText& t, const PanelModel& m, double x0, double y, double x1) {
    cairo_t* cr = pen.cr;
    const PanelHit hit {PanelAction::AppsOpen, nullptr, 0};
    const int pointer = pointerState(hit);
    const double w = x1 - x0;
    if (pointer > 0) fillRounded(pen, x0, y, w, kAppsRowH, 14, kControlHover);
    strokeRounded(pen, x0, y, w, kAppsRowH, 14, kBorder, 2);
    // The apps' icons, small and side by side (whole, so their own labels show)
    const std::vector<frame_apps::Entry>& apps = m.apps.entries;
    const size_t shown = std::min<size_t>(apps.size(), 4);
    const double tile = 36;
    const double step = tile + 6;
    const double iy = y + (kAppsRowH - tile) / 2;
    for (size_t i = 0; i < shown; ++i) drawAppIcon(pen, apps[i], x0 + 16 + step * i, iy, tile);
    const double iconsW = shown == 0 ? 0 : tile + step * (shown - 1);
    // "Frame apps by sasaken@" and how many aren't installed
    const double textX = x0 + 16 + iconsW + 16;
    const double textW = x1 - 44 - textX;
    pen.text(textX, y + 32, ellipsize(pen, t.appsAllTitle, 16, true, textW, false), 16, kText, true);
    pen.text(textX, y + 54, ellipsize(pen, appsCountText(t, apps), 13, false, textW, false), 13, kTextMuted);
    // A chevron (lines) at the right
    cairo_save(cr);
    pen.color(kTextMuted);
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    const double ax = x1 - 24;
    const double ay = y + kAppsRowH / 2;
    cairo_move_to(cr, ax - 4, ay - 8);
    cairo_line_to(cr, ax + 4, ay);
    cairo_line_to(cr, ax - 4, ay + 8);
    cairo_stroke(cr);
    cairo_restore(cr);
    addButton(hit, x0, y, w, kAppsRowH);
}

void EyePanel::drawAppLine(const Pen& pen, const UiText& t, Language language, const frame_apps::Entry& entry,
                           int index, double x, double y, double w, double h) {
    using frame_apps::AppState;
    fillRounded(pen, x, y, w, h, 14, kControl);
    const double tile = 48;
    const double tx = x + 12;
    drawAppIcon(pen, entry, tx, y + (h - tile) / 2, tile);
    // At the right: "Install", or a chip with its state
    double rightEdge = x + w - 12;
    if (entry.state == AppState::Missing && !entry.busy) {
        const double bw = std::max(100.0, pen.measure(t.appsInstall, 16, true) + 44);
        const double bx = rightEdge - bw;
        drawButton(pen, bx, y + (h - 48) / 2, bw, 48, t.appsInstall, {PanelAction::AppsInstall, nullptr, index}, true,
                   true, 16);
        rightEdge = bx - 12;
    } else if (entry.busy || entry.state != AppState::Unknown) {
        const bool running = entry.state == AppState::Running;
        const char* label = entry.busy ? t.appsChipBusy : (running ? t.appsChipRunning : t.appsChipInstalled);
        const Color fill = entry.busy ? kAccentTint : (running ? kSuccessTint : kCard);
        const Color fg = entry.busy ? kAccent : (running ? kSuccess : kTextMuted);
        const double dotW = entry.busy ? 0 : 16;
        const double cw = 14 + dotW + pen.measure(label, 14, true) + 14;
        const double cx = rightEdge - cw;
        const double cy = y + (h - 32) / 2;
        fillRounded(pen, cx, cy, cw, 32, 16, fill);
        if (!entry.busy) drawDot(pen.cr, cx + 14 + 4, cy + 16, 4, fg);
        pen.text(cx + 14 + dotW, centerBaseline(cy, 32, 14), label, 14, fg, true);
        rightEdge = cx - 12;
    }
    // Its name and what it does (the installer's line, in the panel's language)
    const double textX = tx + tile + 14;
    const double textW = std::max(40.0, rightEdge - textX);
    const frame_apps::Lang lang = language == Language::Ja ? frame_apps::Lang::Ja : frame_apps::Lang::En;
    pen.text(textX, y + h / 2 - 5, ellipsize(pen, entry.app.name, 16, true, textW, false), 16, kText, true);
    pen.text(textX, y + h / 2 + 18, ellipsize(pen, entry.app.desc(lang), 13, false, textW, false), 13, kTextMuted);
}

void EyePanel::drawSheetClose(const Pen& pen, const PanelHit& hit, double x, double y, double s) {
    cairo_t* cr = pen.cr;
    const int pointer = pointerState(hit);
    fillRounded(pen, x, y, s, s, 14, pointer == 2 ? kAccentPressed : (pointer == 1 ? kControlHover : kControl));
    strokeRounded(pen, x, y, s, s, 14, kBorder, 2);
    cairo_save(cr);
    pen.color(pointer == 2 ? kOnAccent : kText);
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    const double cx = x + s / 2;
    const double cy = y + s / 2;
    cairo_move_to(cr, cx - 8, cy - 8);
    cairo_line_to(cr, cx + 8, cy + 8);
    cairo_move_to(cr, cx + 8, cy - 8);
    cairo_line_to(cr, cx - 8, cy + 8);
    cairo_stroke(cr);
    cairo_restore(cr);
    addButton(hit, x, y, s, s);
}

void EyePanel::drawAppsList(const Pen& pen, const UiText& t, const PanelModel& m) {
    cairo_t* cr = pen.cr;
    const std::vector<frame_apps::Entry>& apps = m.apps.entries;
    // A sheet over the content card (the tabs and the left column stay)
    drawCard(pen, kRightX, kContentY, kRight - kRightX, kContentH, 20, kCard, kBorder, 2);

    // The title and what is done where, and the cross at the right
    const double closeS = 52;
    const double closeX = kInnerRight - closeS;
    drawSheetClose(pen, {PanelAction::AppsClose, nullptr, 0}, closeX, kAppsHeadY + 2, closeS);
    const double headW = closeX - 16 - kInnerX;
    pen.text(kInnerX, kAppsHeadY + 26, t.appsAllTitle, fitSize(pen, t.appsAllTitle, 22, 16, headW, true), kText, true);
    pen.text(kInnerX, kAppsHeadY + 50, ellipsize(pen, t.appsListSub, 14, false, headW, false), 14, kTextMuted);

    // At the bottom: where the list comes from (two lines at most) and "Open the installer" (busy while its Konsole
    // is open)
    {
        const bool busy = m.apps.menuBusy;
        const std::string label = busy ? t.appsChipBusy : t.appsOpenInstaller;
        const double menuW = pen.measure(label, 16, true) + 48;
        const double menuX = kInnerRight - menuW;
        drawButton(pen, menuX, kAppsFootY, menuW, kAppsFootH, label, {PanelAction::AppsMenu, nullptr, 0}, !busy, false,
                   16);
        // (a "\n" in the text ends a line there, a size smaller if a part would otherwise wrap; two lines in all)
        const std::string text = t.appsListNote;
        const double noteW = menuX - 18 - kInnerX;
        std::vector<std::string> parts;
        for (size_t start = 0; start <= text.size();) {
            const size_t end = std::min(text.find('\n', start), text.size());
            parts.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        double noteSize = 13;
        if (parts.size() > 1) {
            for (const std::string& part : parts) noteSize = std::min(noteSize, fitSize(pen, part, 13, 12, noteW, false));
        }
        std::vector<std::string> note;
        for (const std::string& part : parts) {
            if (note.size() >= 2) break;
            for (const std::string& line : wrapText(pen, part, noteSize, false, noteW, 2 - note.size())) {
                note.push_back(line);
            }
        }
        double baseline = kAppsFootY + kAppsFootH / 2 + 4.5 - (static_cast<double>(note.size()) - 1) * 9;
        for (const std::string& line : note) {
            pen.text(kInnerX, baseline, line, noteSize, kTextMuted);
            baseline += 18;
        }
    }

    // The apps, one row each, clipped between the title and the bottom row and scrolled with ▲ / ▼ when they don't fit
    const double viewTop = kAppsViewTop;
    const double viewBottom = kAppsViewBottom;
    const double viewH = viewBottom - viewTop;
    const double pitch = kAppsLineH + kAppsLineGap;
    const double contentH = apps.empty() ? 0.0 : apps.size() * pitch - kAppsLineGap;
    appsViewH_ = viewH;
    appsMaxScroll_ = std::max(0.0, contentH - viewH);
    appsScroll_ = std::clamp(appsScroll_, 0.0, appsMaxScroll_);
    appsScrollable_ = appsMaxScroll_ > 0;
    appsKeys_.clear();
    for (const frame_apps::Entry& entry : apps) appsKeys_.push_back(entry.app.key);
    const double barX = kInnerRight - kHistoryBarW;
    const double lineRight = appsScrollable_ ? barX - 14 : kInnerRight;
    cairo_save(cr);
    cairo_rectangle(cr, kInnerX - 2, viewTop, lineRight - kInnerX + 4, viewH);
    cairo_clip(cr);
    hitClip_ = true;
    hitClipTop_ = viewTop;
    hitClipBottom_ = viewBottom;
    for (size_t i = 0; i < apps.size(); ++i) {
        const double y = viewTop + i * pitch - appsScroll_;
        if (y + kAppsLineH < viewTop || y > viewBottom) continue;
        drawAppLine(pen, t, m.language, apps[i], static_cast<int>(i), kInnerX, y, lineRight - kInnerX, kAppsLineH);
    }
    hitClip_ = false;
    cairo_restore(cr);
    if (appsScrollable_) {
        drawScrollBar(pen, PanelAction::AppsScroll, barX, viewTop, viewBottom, appsScroll_, appsMaxScroll_, contentH);
    }
}

void EyePanel::drawAppsConfirm(const Pen& pen, const UiText& t, const PanelModel& m) {
    using frame_apps::LaunchResult;
    // The app and the command from the list now (it may be one only the site has); Konsole gets the same command
    const frame_apps::Entry* entry = appsConfirmEntry(m);
    const bool unknown = !appsConfirmKey_.empty() && entry == nullptr;  // gone from the list: can't be opened
    std::string title = t.appsConfirmMenuTitle;
    if (entry != nullptr) {
        char text[256];
        std::snprintf(text, sizeof(text), t.appsConfirmTitleFormat, entry->app.name.c_str());
        title = text;
    }
    const std::string command = appsConfirmCommand(m);
    const char* error = unknown ? t.appsErrorFailed : nullptr;
    switch (appsLaunchResult_) {
        case LaunchResult::Started: break;
        case LaunchResult::Busy: error = t.appsErrorBusy; break;
        case LaunchResult::NoDisplay: error = t.appsErrorNoDisplay; break;
        case LaunchResult::NoKonsole: error = t.appsErrorNoKonsole; break;
        case LaunchResult::UnknownApp:
        case LaunchResult::Failed: error = t.appsErrorFailed; break;
    }
    const char* bullets[3] = {entry != nullptr ? t.appsConfirmProgress : t.appsConfirmMenu, t.appsConfirmNoSudo,
                              t.appsConfirmClose};

    const double w = kRight - kRightX - 40;
    const double x = kRightX + 20;
    const double ix = x + 28;
    const double innerW = w - 56;
    const std::vector<std::string> errorLines =
        error != nullptr ? wrapText(pen, error, 15, true, innerW, 2) : std::vector<std::string> {};
    const double titleH = 40;
    const double h = 26 + titleH + 18 + 26 + 10 + 56 + 16 + 3 * 26 +
                     (errorLines.empty() ? 0.0 : 6 + errorLines.size() * 22.0) + 18 + kControlH + 26;
    double y = kContentY + (kContentH - h) / 2;
    drawCard(pen, x, y, w, h, 20, kCard, kAccent, 2);
    y += 26;

    // The app's icon and "Install <name>" (the menu: its title only)
    double titleX = ix;
    if (entry != nullptr) {
        drawAppIcon(pen, *entry, ix, y, titleH);
        titleX += titleH + 14;
    }
    const double titleW = ix + innerW - titleX;
    const double titleSize = fitSize(pen, title, 22, 16, titleW, true);
    pen.text(titleX, centerBaseline(y, titleH, titleSize), ellipsize(pen, title, titleSize, true, titleW, false),
             titleSize, kText, true);
    y += titleH + 18;

    // The command Konsole runs, as it would be typed (monospaced; smaller before it is ever cut)
    pen.text(ix, y + 18, ellipsize(pen, t.appsConfirmLead, 16, false, innerW, false), 16, kText);
    y += 26 + 10;
    fillRounded(pen, ix, y, innerW, 56, 14, kBg);
    strokeRounded(pen, ix, y, innerW, 56, 14, kDivider, 2);
    double commandSize = 17;
    while (commandSize > 12 && pen.measureMono(command, commandSize) > innerW - 36) commandSize -= 1;
    std::string shown = command;
    if (pen.measureMono(command, commandSize) > innerW - 36) {
        // (an ASCII command: cut at any byte)
        while (shown.size() > 1 && pen.measureMono(shown + "...", commandSize) > innerW - 36) shown.pop_back();
        shown += "...";
    }
    pen.textMono(ix + 18, centerBaseline(y, 56, commandSize), shown, commandSize, kText);
    y += 56 + 16;

    // What happens then
    for (const char* line : bullets) {
        drawDot(pen.cr, ix + 5, y + 13, 3.5, kAccent);
        pen.text(ix + 20, y + 18, ellipsize(pen, line, 15, false, innerW - 20, false), 15, kText);
        y += 26;
    }
    // Why it couldn't open
    if (!errorLines.empty()) {
        y += 6;
        for (const std::string& line : errorLines) {
            pen.text(ix, y + 17, line, 15, kDanger, true);
            y += 22;
        }
    }
    y += 18;

    // "Cancel" and "Open in Konsole" at the right (not while the menu's Konsole is open, nor for an app gone from the
    // list)
    const bool usable = !unknown && !(entry == nullptr && m.apps.menuBusy);
    const double launchW = std::max(200.0, pen.measure(t.appsConfirmLaunch, 18, true) + 56);
    const double launchX = ix + innerW - launchW;
    drawButton(pen, launchX, y, launchW, kControlH, t.appsConfirmLaunch, {PanelAction::AppsLaunch, nullptr, 0}, usable,
               true, 18);
    const double cancelW = std::max(140.0, pen.measure(t.appsConfirmCancel, 18, true) + 48);
    drawButton(pen, launchX - 12 - cancelW, y, cancelW, kControlH, t.appsConfirmCancel,
               {PanelAction::AppsCancel, nullptr, 0}, true, false, 18);
}

double EyePanel::drawTroublePage(const Pen& pen, const UiText& t, const PanelModel& m, double top, double right) {
    double y = top + 4;
    // The diagnostics: what it is for, its code (to read out without opening it) and "Open"
    {
        const double h = 60;
        const double bh = 46;
        const double bw = std::max(110.0, pen.measure(t.diagOpen, 18, true) + 48);
        const double bx = right - bw;
        drawButton(pen, bx, y + (h - bh) / 2, bw, bh, t.diagOpen, {PanelAction::DiagOpen, nullptr, 0}, true, false, 18);
        const std::string code = diag::code(m);
        const double codeSize = fitSize(pen, code, 17, 12, (right - kInnerX) / 2, true);
        const double chipH = 34;
        const double chipW = pen.measure(code, codeSize, true) + 24;
        const double chipX = bx - 14 - chipW;
        const double chipY = y + (h - chipH) / 2;
        fillRounded(pen, chipX, chipY, chipW, chipH, 8, kAccent);
        textCentered(pen, chipX + chipW / 2, centerBaseline(chipY, chipH, codeSize), code, codeSize, kOnAccent, true);
        const double room = chipX - 16 - kInnerX;
        pen.text(kInnerX, y + 24, t.diagTitle, fitSize(pen, t.diagTitle, 20, 14, room, true), kText, true);
        pen.text(kInnerX, y + 48, ellipsize(pen, t.diagRowHint, 14, false, room, false), 14, kTextMuted);
        y += h + 16;
    }
    // SteamVR's eye camera speed (only with a SteamVR that has the setting)
    {
        const double h = drawEyeRateRow(pen, t, m, y, right);
        if (h > 0) y += h + 16;
    }
    // The newest records
    {
        const double titleW = pen.text(kInnerX, y + 18, t.recordsTitle, 17, kText, true);
        const double hintX = kInnerX + titleW + 14;
        pen.text(hintX, y + 18, ellipsize(pen, t.recordsHint, 14, false, right - hintX, false), 14, kTextMuted);
        y += 30;
    }
    const std::vector<report::Summary>& records = m.records.list;
    const size_t shown = std::min<size_t>(3, records.size());
    const double rowH = 54;
    const double boxH = shown > 0 ? shown * rowH + 8 : 64;
    fillRounded(pen, kInnerX, y, right - kInnerX, boxH, 14, kBg);
    strokeRounded(pen, kInnerX, y, right - kInnerX, boxH, 14, kDivider, 1);
    if (shown == 0) {
        pen.text(kInnerX + 18, centerBaseline(y, boxH, 15),
                 ellipsize(pen, t.recordsNone, 15, false, right - kInnerX - 36, false), 15, kTextMuted);
    }
    for (size_t i = 0; i < shown; ++i) {
        const double rowY = y + 4 + i * rowH;
        if (i > 0) {
            pen.color(kDivider);
            cairo_set_line_width(pen.cr, 1);
            cairo_move_to(pen.cr, kInnerX + 14, rowY + 0.5);
            cairo_line_to(pen.cr, right - 14, rowY + 0.5);
            cairo_stroke(pen.cr);
        }
        drawRecordRow(pen, t, records[i], kInnerX + 4, right - 4, rowY, rowH);
    }
    y += boxH + 14;
    // Over SSH, and all of them
    {
        const double h = 42;
        const std::string all = formatText(t.recordsAllFormat, std::to_string(records.size()));
        const double w = pen.measure(all, 16, true) + 44;
        drawButton(pen, right - w, y, w, h, all, {PanelAction::RecordsAll, nullptr, 0}, !records.empty(), false, 16);
        pen.text(kInnerX, centerBaseline(y, h, 14), ellipsize(pen, t.recordsSsh, 14, false, right - w - 16 - kInnerX, false),
                 14, kTextMuted);
        y += h;
    }
    return y + 8 - top;
}

double EyePanel::drawEyeRateRow(const Pen& pen, const UiText& t, const PanelModel& m, double y, double right) {
    const eye_rate::View& rate = m.eyeRate;
    if (!rate.setting.exists) return 0.0;
    // The two choices on the right, as wide as the longer one needs (narrower beside the scroll buttons)
    const double segSize = 17;
    const double segH = 48;
    const double segW = std::min(
        right - kInnerX - 260,
        2 * (std::max(pen.measure(t.eyeRateFull, segSize, true), pen.measure(t.eyeRateSteamVr, segSize, true)) +
             segSize * 0.9 + 28) +
            10);
    const double segX = right - segW;
    const int chosen = eye_rate::choice(rate.setting) == eye_rate::Choice::Full ? 0 : 1;
    drawSegmented(pen, segX, y + 6, segW, segH,
                  {{t.eyeRateFull, {PanelAction::EyeRateChoose, nullptr, static_cast<int>(eye_rate::Choice::Full)}},
                   {t.eyeRateSteamVr,
                    {PanelAction::EyeRateChoose, nullptr, static_cast<int>(eye_rate::Choice::SteamVr)}}},
                  chosen, segSize);
    // The title and hint as the diagnostics' (the hint on two lines if it needs them), the rate now under them
    const double room = segX - 16 - kInnerX;
    pen.text(kInnerX, y + 24, t.rowEyeRate, fitSize(pen, t.rowEyeRate, 20, 14, room, true), kText, true);
    double baseline = y + 48;
    for (const std::string& line : wrapText(pen, t.hintEyeRate, 14, false, room, 2)) {
        pen.text(kInnerX, baseline, line, 14, kTextMuted);
        baseline += 19;
    }
    const EyeStatus& s = m.status;
    if (s.running && s.tracking && std::isfinite(s.trackerRate)) {
        const bool low = eye_rate::low(s);
        char text[128];
        std::snprintf(text, sizeof(text), t.eyeRateNowFormat, s.trackerRate);
        std::string now = text;
        if (low) now += std::string("  ") + t.trackerRateLowHint;
        pen.text(kInnerX, baseline + 3, ellipsize(pen, now, 14, low, room, false), 14, low ? kDanger : kText, low);
    }
    baseline += 3;
    return std::max(segH + 12, baseline + 6 - y);
}

void EyePanel::drawRecordRow(const Pen& pen, const UiText& t, const report::Summary& r, double x0, double x1, double y,
                             double h) {
    const int index = static_cast<int>(recordNames_.size());
    recordNames_.push_back(r.folder);
    const PanelHit hit {PanelAction::RecordOpen, nullptr, index};
    const bool en = &t == &uiText(Language::En);
    // "View" at the right, the result's badge left of it
    const double bh = 38;
    const double bw = std::max(80.0, pen.measure(t.recordView, 16, true) + 40);
    const double bx = x1 - 10 - bw;
    drawButton(pen, bx, y + (h - bh) / 2, bw, bh, t.recordView, hit, true, false, 16);
    const double badgeW = drawResultBadge(pen, t, r.result, bx - 14, y + h / 2, 15);
    const double textRight = bx - 14 - badgeW - 14;
    // When, what, and a short line
    double x = x0 + 14;
    const std::time_t when = static_cast<std::time_t>(std::floor(r.start));
    std::tm local {};
    localtime_r(&when, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%m/%d %H:%M", &local);
    const double baseline = centerBaseline(y, h, 16);
    pen.text(x, baseline, stamp, 15, kTextMuted);
    x += pen.measure("00/00 00:00", 15, false) + 18;
    const std::string kind = report::kindLabel(t, r);
    x += pen.text(x, baseline, ellipsize(pen, kind, 16, true, std::max(40.0, textRight - x), false), 16, kText, true);
    const std::string& brief = en && !r.briefEn.empty() ? r.briefEn : r.brief;
    if (!brief.empty() && textRight - x - 14 > 40) {
        pen.text(x + 14, baseline, ellipsize(pen, brief, 14, false, textRight - x - 14, false), 14, kTextMuted);
    }
}

double EyePanel::drawResultBadge(const Pen& pen, const UiText& t, report::Result result, double right, double cy,
                                 double size) {
    const char* label = report::resultLabel(t, result);
    // Stopped (nothing went wrong) in grey, like a paused badge
    const Color fg = result == report::Result::Ok        ? kSuccess
                     : result == report::Result::Failed  ? kDangerSoft
                     : result == report::Result::Stopped ? kTextMuted
                                                         : kAccent;
    const Color bg = result == report::Result::Ok        ? kSuccessTint
                     : result == report::Result::Failed  ? kDangerTint
                     : result == report::Result::Stopped ? kControl
                                                         : kAccentTint;
    const double h = size + 14;
    const double w = pen.measure(label, size, true) + 26;
    fillRounded(pen, right - w, cy - h / 2, w, h, h / 2, bg);
    textCentered(pen, right - w / 2, centerBaseline(cy - h / 2, h, size), label, size, fg, true);
    return w;
}

double EyePanel::drawToolsPage(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v, double top,
                               double right) {
    double y = top + 4;
    // The debug gaze dots (a head-locked dot where the sent gaze points)
    {
        const double h = 64;
        const bool locked = v.locked(key::kGazeDebugDots);
        const bool on = v.flag(key::kGazeDebugDots);
        const double rowTop = y + (h - kControlH) / 2;
        // Narrower beside the scroll buttons
        const bool narrow = right < kInnerRight;
        const double segW = narrow ? 170 : 200;
        drawRowLabel(pen, t, y, h, t.rowGazeDots, t.hintGazeDots, locked);
        drawSegmented(pen, kControlX, rowTop, segW, kControlH,
                      {{t.on, {PanelAction::SetBool, key::kGazeDebugDots, 1}},
                       {t.off, {PanelAction::SetBool, key::kGazeDebugDots, 0}}},
                      on ? 0 : 1, 20, locked);
        // How far ahead the dots are: "Dot distance  − 1.0 m ＋", greyed out while the dots are off
        const double stepperW = narrow ? 156 : 180;
        const double stepperX = right - stepperW;
        const double captionRight = stepperX - 10;
        const double captionLeft = kControlX + segW + 10;
        const double captionSize = fitSize(pen, t.dotDistance, 16, 12, captionRight - captionLeft, false);
        const double captionW = pen.measure(t.dotDistance, captionSize, false);
        pen.text(captionRight - captionW, centerBaseline(y, h, captionSize), t.dotDistance, captionSize,
                 on ? kText : kTextMuted);
        const double distance = v.number(key::kGazeDebugDotsDistanceM);
        drawStepper(pen, stepperX, rowTop, stepperW, kControlH, key::kGazeDebugDotsDistanceM, distance,
                    formatSetting(key::kGazeDebugDotsDistanceM, distance) + " m", on, false);
        y += h + 2;
        // Under it: the rings (the gaze before the scale), with when they show beside the switch; greyed out (the
        // value still readable) while the dots are off
        const bool rings = v.flag(key::kGazeDebugRings);
        drawRowLabel(pen, t, y, h, t.rowGazeRings, t.hintGazeRings, false);
        drawSegmented(pen, kControlX, rowTop + h + 2, segW, kControlH,
                      {{t.on, {PanelAction::SetBool, key::kGazeDebugRings, 1}, on},
                       {t.off, {PanelAction::SetBool, key::kGazeDebugRings, 0}, on}},
                      rings ? 0 : 1, 20, !on);
        const double noteX = kControlX + segW + 14;
        const std::vector<std::string> note = wrapText(pen, t.gazeRingsNote, 14, false, right - noteX, 2);
        double baseline = y + h / 2 - (note.size() - 1) * 9 + 5;
        for (const std::string& line : note) {
            pen.text(noteX, baseline, line, 14, on ? kTextMuted : kTextDisabled);
            baseline += 18;
        }
        y += h + 14;
    }
    // The eye log: record the raw eye data to a file, from now until "Stop" (or 60 minutes)
    {
        const recorder::View& r = m.recording;
        const double buttonW = 160;
        const double textX = kControlX + buttonW + 14;
        const double textW = right - textX;
        // Next to the button: where the files go (wrapped), with the eye cameras in use that eyecam's numbers go beside
        // them (an eyecam-rec that keeps them), and the limit; or why it could not record (up to three lines)
        std::vector<std::string> lines;
        Color color = kTextMuted;
        bool bold = false;
        if (!r.error.empty() && !r.recording) {
            lines = wrapText(pen, formatText(t.eyeLogFailedFormat, r.error), 14, true, textW, 3);
            color = kDanger;
            bold = true;
        } else {
            const std::string where = formatText(t.eyeLogWhereFormat, recorder::shortPath(recorder::defaultDir()));
            lines = wrapText(pen, where, 14, false, textW, 2);
            if (eyecam::inUse(m.eyecam, v.flag(key::kCameraLids)) && m.eyecam.status.hasDriftlog) {
                for (const std::string& line : wrapText(pen, t.eyeLogDrift, 14, false, textW, 2)) lines.push_back(line);
            }
            // After the 60-minute limit stopped it: say so instead of the limit
            lines.push_back(r.autoStopped ? t.eyeLogAutoStopped : t.eyeLogLimit);
            if (r.autoStopped) color = kText;
        }
        // (taller for more than three lines)
        const double h = std::max(60.0, lines.size() * 18.0 + 6);
        drawRowLabel(pen, t, y, h, t.rowEyeLog, "", false);
        const std::string label =
            r.recording ? formatText(t.eyeLogStopFormat, recorder::elapsedText(r.elapsedSec)) : std::string(t.eyeLogRecord);
        drawButton(pen, kControlX, y + (h - 46) / 2, buttonW, 46, label, {PanelAction::RecordToggle, nullptr, 0}, true,
                   false);
        double baseline = y + h / 2 - (lines.size() - 1) * 9 + 5;
        for (const std::string& line : lines) {
            pen.text(textX, baseline, line, 14, color, bold);
            baseline += 18;
        }
        y += h + 14;
    }
    // Developer: eyecam-rec's eye recording (its start opens the light warning; a failed one says why), only while it
    // runs
    if (m.eyecam.visible) {
        const eyecam::Status& es = m.eyecam.status;
        const double h = 64;
        const double bh = 46;
        const bool failed = eyecam::errorShown(m.eyecam, true);
        const bool ready = es.state == eyecam::State::Idle || es.state == eyecam::State::Error;
        // Its title with a "Developer" badge, and what it does
        {
            const double titleSize = fitSize(pen, t.devRecord, 20, 14, kLabelW - 70, true);
            const double titleW = pen.text(kInnerX, y + h / 2 - 3, t.devRecord, titleSize, kText, true);
            const double badgeSize = 12;
            const double badgeW = pen.measure(t.devTitle, badgeSize, true) + 16;
            const double badgeH = 20;
            const double badgeX = kInnerX + titleW + 8;
            const double badgeY = y + h / 2 - 3 - titleSize * 0.36 - badgeH / 2;
            fillRounded(pen, badgeX, badgeY, badgeW, badgeH, badgeH / 2, kAccentTint);
            textCentered(pen, badgeX + badgeW / 2, centerBaseline(badgeY, badgeH, badgeSize), t.devTitle, badgeSize,
                         kText, true);
            pen.text(kInnerX, y + h / 2 + 19, t.devRecordHint, fitSize(pen, t.devRecordHint, 15, 11, kLabelW, false),
                     kTextMuted);
        }
        const double bw = 160;
        drawButton(pen, kControlX, y + (h - bh) / 2, bw, bh, failed ? t.eyecamRetry : t.eyecamStart,
                   {PanelAction::EyecamStart, nullptr, 0}, ready && !m.eyecam.busy, false);
        if (!failed) {
            // Where the video goes, before anyone starts one
            const double textX = kControlX + bw + 14;
            const std::vector<std::string> lines = wrapText(pen, t.eyecamStorageRow, 13, false, right - textX, 3);
            double baseline = y + h / 2 - (lines.size() - 1) * 8.5 + 5;
            for (const std::string& line : lines) {
                pen.text(textX, baseline, line, 13, kTextMuted);
                baseline += 17;
            }
        } else {
            // Why it failed, and "Back" at the right (dismissed in the panel only, as on the eye cameras tab)
            const double backW = pen.measure(t.eyecamBack, 16, true) + 40;
            drawButton(pen, right - backW, y + (h - bh) / 2, backW, bh, t.eyecamBack,
                       {PanelAction::EyecamBack, nullptr, 0}, true, false, 16);
            const double textX = kControlX + bw + 14;
            const double textRight = right - backW - 12;
            const std::string& message = eyecam::shownMessage(es, m.language);
            const std::string why = message.empty() ? std::string(t.eyecamErrorTitle) : message;
            const std::vector<std::string> lines = wrapText(pen, why, 14, true, textRight - textX, 2);
            double baseline = y + h / 2 - (lines.size() - 1) * 9 + 5;
            for (const std::string& line : lines) {
                pen.text(textX, baseline, line, 14, kDanger, true);
                baseline += 18;
            }
        }
        y += h;
        // Under it, "Record for squint training": recordings for the developer (usable once the cameras are set up)
        y += 10;
        y += drawContribRow(pen, t, m, y, right);
    }
    return y + 8 - top;
}

double EyePanel::drawContribRow(const Pen& pen, const UiText& t, const PanelModel& m, double y, double right) {
    const contrib::View& c = m.contrib;
    const double h = 64;
    const double bh = 46;
    const bool setUp = eyecam::setUp(m.eyecam);
    drawRowLabel(pen, t, y, h, t.contribRow, t.contribRowHint, false);
    // "Record", and "Recordings" once there is a folder
    const double recordW = std::max(110.0, pen.measure(t.contribRecord, 18, true) + 40);
    const double listW = std::max(130.0, pen.measure(t.contribRecordings, 16, true) + 36);
    drawButton(pen, kControlX, y + (h - bh) / 2, recordW, bh, t.contribRecord,
               {PanelAction::ContribOpen, nullptr, static_cast<int>(contrib::Page::Record)}, setUp, false);
    drawButton(pen, kControlX + recordW + 10, y + (h - bh) / 2, listW, bh, t.contribRecordings,
               {PanelAction::ContribOpen, nullptr, static_cast<int>(contrib::Page::Manage)}, setUp && c.exists,
               false, 16);
    // Beside them: what it is (and its size), how many so far, or why it can't be used yet
    std::string text;
    const int count = static_cast<int>(c.info.recordings.size());
    if (!setUp) {
        text = t.contribRowNotSetUp;
    } else if (count == 0) {
        text = formatLong(t.contribRowIntroFormat, contrib::sizeText(contrib::estimateBytes(c.fps)));
    } else {
        char line[256];
        std::snprintf(line, sizeof(line), t.contribRowCountFormat, count,
                      contrib::sizeText(contrib::recordedBytes(c.info)).c_str());
        text = line;
    }
    const double textX = kControlX + recordW + 10 + listW + 14;
    const std::vector<std::string> lines = wrapText(pen, text, 13, false, right - textX, 3);
    double baseline = y + h / 2 - (lines.size() - 1) * 8.5 + 5;
    for (const std::string& line : lines) {
        pen.text(textX, baseline, line, 13, kTextMuted);
        baseline += 17;
    }
    return h;
}

double EyePanel::drawFilesPage(const Pen& pen, const UiText& t, const PanelModel& m, double top, double right) {
    const EyeStatus& s = m.status;
    const double controlW = right - kControlX;
    double y = top + 4;
    // Read-only rows: the title (muted) on the left, the text on the right
    const double infoH = 34;
    const double infoSize = 16;
    const auto infoRow = [&](const std::string& title, const std::string& value, bool keepEnd) {
        const double baseline = y + infoH / 2 + infoSize * 0.36;
        pen.text(kInnerX, baseline, title, fitSize(pen, title, infoSize, 11, kLabelW, false), kTextMuted);
        pen.text(kControlX, baseline, ellipsize(pen, value, infoSize, false, controlW, keepEnd), infoSize, kText);
        y += infoH;
    };
    // File locations
    infoRow(t.rowConfigPath, m.configPath, true);
    if (s.running && !s.configPath.empty() && s.configPath != m.configPath) {
        const std::string warning = t.configPathMismatch + s.configPath;
        pen.text(kControlX, y + 14, ellipsize(pen, warning, 14, true, controlW, true), 14, kDanger, true);
        y += 22;
    }
    infoRow(t.rowCalibrationPath, s.running && !s.calibrationPath.empty() ? s.calibrationPath : "—", true);
    infoRow(t.rowStatusPath, m.statusPath, true);
    infoRow(t.recordsTitle, report::shortPathOf(m.records.dir.empty() ? report::defaultDir() : m.records.dir) + "/", true);
    // frameeyeosc process
    {
        std::string text = t.notRunning;
        if (s.running) {
            const int minutes = static_cast<int>(std::max(0.0, s.time - s.started) / 60);
            char uptime[64];
            if (minutes >= 60) {
                std::snprintf(uptime, sizeof(uptime), t.hoursMinutesFormat, minutes / 60, minutes % 60);
            } else {
                std::snprintf(uptime, sizeof(uptime), t.minutesFormat, minutes);
            }
            char line[128];
            std::snprintf(line, sizeof(line), t.coreFormat, s.pid, uptime);
            text = line;
        }
        infoRow(t.rowCore, text, false);
    }
    // Locked by the command line, with the values in effect (up to three lines; where they come from under the title)
    {
        std::vector<std::string> items;
        if (s.running) {
            for (const std::string& name : s.locked) {
                std::string value = "?";
                if (const JsonValue* effective = s.effective.get(name)) {
                    if (effective->isBool()) value = effective->boolean ? "true" : "false";
                    if (effective->isNumber()) {
                        value = effective->integer ? std::to_string(static_cast<long long>(effective->number))
                                                   : formatSetting(name, effective->number);
                    }
                    if (effective->isString()) value = "\"" + effective->text + "\"";
                    if (effective->isNull()) value = "null";
                }
                items.push_back(name + " = " + value);
            }
        }
        if (items.empty()) items.push_back(s.running ? t.noneLocked : "—");
        std::vector<std::string> lines;
        for (const std::string& item : items) {
            if (!lines.empty() && pen.measure(lines.back() + ",  " + item, infoSize, false) <= controlW) {
                lines.back() += ",  " + item;
            } else {
                lines.push_back(ellipsize(pen, item, infoSize, false, controlW, false));
            }
        }
        if (lines.size() > 3) lines.resize(3);
        const double baseline = y + infoH / 2 + infoSize * 0.36;
        pen.text(kInnerX, baseline, t.rowLockedList, fitSize(pen, t.rowLockedList, infoSize, 11, kLabelW, false),
                 kTextMuted);
        pen.text(kInnerX, baseline + 22, t.hintLockedList, fitSize(pen, t.hintLockedList, 13, 10, kLabelW, false),
                 kTextMuted);
        for (size_t i = 0; i < lines.size(); ++i) pen.text(kControlX, baseline + i * 24, lines[i], infoSize, kText);
        y += infoH + std::max(26.0, (lines.size() - 1) * 24.0);
    }
    return y + 8 - top;
}

double EyePanel::drawDataPage(const Pen& pen, const UiText& t, const PanelModel& m, double top, double right) {
    const stored::View& d = m.savedData;
    double y = top + 4;
    // The title, and what it is beside it
    const double titleW = pen.text(kInnerX, y + 22, t.dataTitle, 20, kText, true);
    const double hintX = kInnerX + titleW + 14;
    pen.text(hintX, y + 22, ellipsize(pen, t.dataHint, 14, false, right - hintX, false), 14, kTextMuted);
    y += 44;
    // A row each: eyecam's only where eyecam-rec is installed (its status file is there)
    const double h = 64;
    const double bh = 44;
    const double bw = std::max(110.0, pen.measure(t.dataDelete, 17, true) + 44);
    for (int i = 0; i < stored::kKindCount; ++i) {
        const stored::Kind kind = static_cast<stored::Kind>(i);
        const bool eyecamKind = kind != stored::Kind::EyeLogs;
        if (eyecamKind && !m.eyecam.status.present) continue;
        const stored::Delete& job = d.of(kind);
        drawRowLabel(pen, t, y, h, t.dataRows[i], t.dataRowHints[i], false);
        // Beside it: how many and their size, and under that what goes on (or why it can't be deleted now)
        stored::Usage usage;
        const bool known = dataUsage(m, kind, usage);
        const bool eyecamRuns = m.eyecam.visible && m.eyecam.status.stored.present;
        const std::string value = known ? (usage.count == 0 ? std::string(t.dataNone) : dataCountText(t, kind, usage))
                                        : std::string(eyecamKind && !eyecamRuns ? "—" : t.dataCounting);
        std::string note;
        Color noteColor = kTextMuted;
        bool noteBold = false;
        if (job.running) {
            note = t.dataDeleting;
            noteColor = kText;
            noteBold = true;
        } else if (eyecamKind && !eyecamRuns) {
            note = t.dataNoEyecam;
        } else if (eyecamKind && eyecamBusyForData(m)) {
            note = t.dataBusy;
        } else if (job.done && !job.ok) {
            note = formatText(t.dataFailedFormat, job.error);
            noteColor = kDanger;
            noteBold = true;
        } else if (job.done) {
            note = formatText(t.dataDeletedFormat, dataCountText(t, kind, job.usage));
            noteColor = kSuccess;
            noteBold = true;
        } else if (!eyecamKind && m.recording.recording) {
            note = t.dataRecordingNote;
        }
        drawButton(pen, right - bw, y + (h - bh) / 2, bw, bh, t.dataDelete, {PanelAction::DataDelete, nullptr, i},
                   dataDeletable(m, kind), false, 17);
        const double textW = right - bw - 14 - kControlX;
        if (note.empty()) {
            pen.text(kControlX, centerBaseline(y, h, 17), ellipsize(pen, value, 17, false, textW, false), 17, kText);
        } else {
            pen.text(kControlX, y + h / 2 - 4, ellipsize(pen, value, 17, false, textW, false), 17, kText);
            const double noteSize = fitSize(pen, note, 13, 11, textW, noteBold);
            pen.text(kControlX, y + h / 2 + 17, ellipsize(pen, note, noteSize, noteBold, textW, false), noteSize,
                     noteColor, noteBold);
        }
        y += h + 10;
    }
    return y + 8 - top;
}

void EyePanel::drawDataDelete(const Pen& pen, const UiText& t, const PanelModel& m) {
    const stored::Kind kind = static_cast<stored::Kind>(dataConfirm_);
    stored::Usage usage;
    dataUsage(m, kind, usage);
    // What goes, from where, and what stays
    const std::string folder =
        recorder::shortPath(kind == stored::Kind::EyeLogs ? recorder::defaultDir() : contrib::defaultHome()) + "/";
    std::string body = formatLong(t.dataDeleteFormats[dataConfirm_], folder, dataCountText(t, kind, usage));
    if (kind == stored::Kind::EyeLogs && m.recording.recording) {
        body += (m.language == Language::En ? " " : "") + std::string(t.dataDeleteRecording);
    }
    // A card in the middle, as the other confirmations
    const double w = kRight - kRightX - 120;
    const double x = kRightX + 60;
    const double ix = x + 28;
    const double innerW = w - 56;
    const std::vector<std::string> lines = wrapText(pen, body, 17, false, innerW, 5);
    const double h = 26 + 32 + 14 + lines.size() * 26.0 + 22 + kControlH + 26;
    double y = kContentY + 80 + std::max(0.0, (kContentH - 80 - h) / 2);
    drawCard(pen, x, y, w, h, 20, kCard, kDanger, 2);
    y += 26;
    const char* title = t.dataDeleteTitles[dataConfirm_];
    pen.text(ix, y + 24, title, fitSize(pen, title, 24, 16, innerW, true), kText, true);
    y += 32 + 14;
    for (const std::string& line : lines) {
        pen.text(ix, y + 17, line, 17, kText);
        y += 26;
    }
    y += 22;
    const double yesW = std::max(160.0, pen.measure(t.dataDeleteYes, 18, true) + 56);
    const double yesX = ix + innerW - yesW;
    drawButton(pen, yesX, y, yesW, kControlH, t.dataDeleteYes, {PanelAction::DataDeleteConfirm, nullptr, dataConfirm_},
               dataDeletable(m, kind), false, 18);
    const double noW = std::max(140.0, pen.measure(t.dataCancel, 18, true) + 48);
    drawButton(pen, yesX - 12 - noW, y, noW, kControlH, t.dataCancel, {PanelAction::DataDeleteCancel, nullptr, 0},
               true, true, 18);
}

void EyePanel::drawRecord(const Pen& pen, const UiText& t, const PanelModel& m) {
    const report::Summary& r = m.records.opened;
    const bool en = m.language == Language::En;
    const double top = kContentY + 18;
    const double backH = 42;
    const double backW = pen.measure(t.recordBack, 16, true) + 40;
    drawButton(pen, kInnerX, top, backW, backH, t.recordBack, {PanelAction::RecordBack, nullptr, 0}, true, false, 16);
    if (!m.records.openedFound || r.folder != recordOpen_) {
        pen.text(kInnerX, top + 100, t.recordMissing, 18, kTextMuted);
        pen.text(kInnerX, top + 130, recordOpen_, 15, kTextMuted);
        return;
    }
    // What ran and when, the conditions under it, the result's badge at the right
    const double badgeW = drawResultBadge(pen, t, r.result, kInnerRight, top + backH / 2, 16);
    const double titleX = kInnerX + backW + 16;
    const double titleRoom = kInnerRight - badgeW - 16 - titleX;
    {
        const std::time_t when = static_cast<std::time_t>(std::floor(r.start));
        std::tm local {};
        localtime_r(&when, &local);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%m/%d %H:%M", &local);
        const std::string title = report::kindLabel(t, r) + t.condSeparator + stamp;
        pen.text(titleX, top + 19, ellipsize(pen, title, 21, true, titleRoom, false), 21, kText, true);
        pen.text(titleX, top + 40, ellipsize(pen, report::conditionsText(t, r), 14, false, titleRoom, false), 14,
                 kTextMuted);
    }
    double y = top + backH + 16;
    // Why (or what came of it) in a box: red for a failure, grey for a stop
    {
        const std::string& reason = en && !r.reasonEn.empty() ? r.reasonEn : r.reason;
        const bool failed = r.result == report::Result::Failed;
        const bool stopped = r.result == report::Result::Stopped;
        const Color fg = failed    ? kDangerSoft
                         : stopped ? kTextMuted
                         : r.result == report::Result::Partial ? kAccent
                                                               : kSuccess;
        const Color line = failed ? kDanger : stopped ? kBorder : fg;
        const Color bg = failed    ? kDangerTint
                         : stopped ? kControl
                         : r.result == report::Result::Partial ? kAccentTint
                                                               : kSuccessTint;
        const char* label = r.result == report::Result::Ok ? t.recordOutcome : t.recordReason;
        const double padX = 16;
        const double labelW = pen.measure(label, 16, true);
        const double textX = kInnerX + padX + labelW + 4;
        const std::vector<std::string> lines =
            wrapText(pen, reason.empty() ? std::string("—") : reason, 16, false, kInnerRight - padX - textX, 3);
        // 24 px a line with 10 px above and below: a 16 px line's baseline 17 px into its row centres it
        const double boxH = 20 + lines.size() * 24;
        fillRounded(pen, kInnerX, y, kInnerRight - kInnerX, boxH, 12, bg);
        strokeRounded(pen, kInnerX, y, kInnerRight - kInnerX, boxH, 12, line, 1.5);
        pen.text(kInnerX + padX, y + 27, label, 16, fg, true);
        double baseline = y + 3;
        for (const std::string& line : lines) {
            baseline += 24;
            pen.text(textX, baseline, line, 16, kText);
        }
        y += boxH + 14;
    }
    // Two cards: the key log lines, and the folder's files
    const double bottom = kContentY + kContentH - 18;
    const double gap = 12;
    const double colW = (kInnerRight - kInnerX - gap) / 2;
    const double colH = bottom - y;
    const double padX = 14;
    {
        const double x = kInnerX;
        fillRounded(pen, x, y, colW, colH, 12, kControl);
        pen.text(x + padX, y + 26, t.recordFlow, 14, kAccent, true);
        const double size = 14;
        const double step = 22;
        const size_t fits = static_cast<size_t>(std::max(1.0, std::floor((colH - 44) / step)));
        // As many as fit: the first two, and the last ones (how it ended)
        std::vector<report::LogLine> lines = r.flow;
        if (lines.size() > fits) {
            std::vector<report::LogLine> kept(lines.begin(), lines.begin() + std::min<size_t>(2, fits / 3));
            kept.insert(kept.end(), lines.end() - (fits - kept.size()), lines.end());
            lines = kept;
        }
        double sourceW = 0;
        for (const report::Source source :
             {report::Source::Core, report::Source::Panel, report::Source::Eyecam, report::Source::Valve}) {
            sourceW = std::max(sourceW, pen.measure(report::sourceLabel(t, source), 13, false));
        }
        const double timeW = pen.measure("00:00:00", size, false);
        double baseline = y + 32;
        if (lines.empty()) pen.text(x + padX, baseline + step, t.recordFlowNone, size, kTextMuted);
        for (const report::LogLine& line : lines) {
            baseline += step;
            const std::time_t when = static_cast<std::time_t>(std::floor(line.at));
            std::tm local {};
            localtime_r(&when, &local);
            char stamp[16];
            std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
            double lx = x + padX;
            pen.text(lx, baseline, stamp, size, kTextMuted);
            lx += timeW + 8;
            pen.text(lx, baseline, report::sourceLabel(t, line.source), 13, kTextMuted);
            lx += sourceW + 8;
            pen.text(lx, baseline, ellipsize(pen, report::flowText(t, line), size, false, x + colW - padX - lx, false),
                     size, kText);
        }
    }
    {
        const double x = kInnerX + colW + gap;
        const double x1 = x + colW - padX;
        fillRounded(pen, x, y, colW, colH, 12, kControl);
        pen.text(x + padX, y + 26, t.recordContents, 14, kAccent, true);
        double baseline = y + 32;
        for (const report::FileInfo& file : m.records.files) {
            const char* what = file.name == "report.txt"          ? t.recordFileReport
                               : file.name == "logs.txt"          ? t.recordFileLogs
                               : file.name == "status.jsonl"      ? t.recordFileStatus
                               : file.name == "calib_result.json" ? t.recordFileCalib
                                                                  : nullptr;
            const std::string label = what != nullptr ? what : file.name;
            const std::string size = report::sizeText(file.bytes);
            baseline += 24;
            const double sizeW = pen.measure(size, 14, false);
            pen.text(x1 - sizeW, baseline, size, 14, kTextMuted);
            pen.text(x + padX, baseline, ellipsize(pen, label, 15, false, x1 - sizeW - 12 - x - padX, false), 15, kText);
        }
        baseline += 16;
        pen.color(kDivider);
        cairo_set_line_width(pen.cr, 1);
        cairo_move_to(pen.cr, x + padX, baseline + 0.5);
        cairo_line_to(pen.cr, x1, baseline + 0.5);
        cairo_stroke(pen.cr);
        baseline += 26;
        const double codeLabelW = pen.text(x + padX, baseline, t.recordCode, 14, kTextMuted);
        pen.text(x + padX + codeLabelW + 10, baseline,
                 ellipsize(pen, r.code.empty() ? std::string("—") : r.code, 15, true, x1 - x - padX - codeLabelW - 10, false),
                 15, kText, true);
        const std::string folder =
            report::shortPathOf((m.records.dir.empty() ? report::defaultDir() : m.records.dir) + "/" + r.folder) + "/";
        // The folder, broken after a "/" where it doesn't fit (two lines at most)
        std::vector<std::string> lines(1);
        for (size_t at = 0; at < folder.size();) {
            const size_t slash = folder.find('/', at);
            const std::string part = folder.substr(at, slash == std::string::npos ? std::string::npos : slash + 1 - at);
            at = slash == std::string::npos ? folder.size() : slash + 1;
            if (!lines.back().empty() && pen.measure(lines.back() + part, 13, false) > x1 - x - padX && lines.size() < 2) {
                lines.emplace_back();
            }
            lines.back() += part;
        }
        for (const std::string& line : lines) {
            baseline += 20;
            pen.text(x + padX, baseline, ellipsize(pen, line, 13, false, x1 - x - padX, true), 13, kTextMuted);
        }
    }
}

void EyePanel::drawRecordsAll(const Pen& pen, const UiText& t, const PanelModel& m) {
    const std::vector<report::Summary>& records = m.records.list;
    const double top = kContentY + 18;
    const double backH = 42;
    const double backW = pen.measure(t.recordBack, 16, true) + 40;
    drawButton(pen, kInnerX, top, backW, backH, t.recordBack, {PanelAction::RecordBack, nullptr, 0}, true, false, 16);
    const std::string title = formatText(t.recordsAllTitleFormat, std::to_string(records.size()));
    const double titleX = kInnerX + backW + 16;
    const double noteW = pen.measure(t.recordsKept, 14, false);
    pen.text(titleX, centerBaseline(top, backH, 21),
             ellipsize(pen, title, 21, true, kInnerRight - noteW - 16 - titleX, false), 21, kText, true);
    pen.text(kInnerRight - noteW, centerBaseline(top, backH, 14), t.recordsKept, 14, kTextMuted);
    const double viewTop = top + backH + 14;
    if (records.empty()) {
        pen.text(kInnerX, viewTop + 40, t.recordsNone, 16, kTextMuted);
        listScroll_ = 0.0;
        listMaxScroll_ = 0.0;
        listScrollable_ = false;
        return;
    }
    drawScrolled(pen, viewTop, kAdvViewBottom, listScroll_, listMaxScroll_, listViewH_, listScrollable_,
                 [&](double pageTop, double right) {
                     const double rowH = 54;
                     const double gap = 6;
                     double y = pageTop;
                     for (const report::Summary& record : records) {
                         fillRounded(pen, kInnerX, y, right - kInnerX, rowH, 12, kBg);
                         drawRecordRow(pen, t, record, kInnerX, right, y, rowH);
                         y += rowH + gap;
                     }
                     return y - gap - pageTop;
                 });
}

double EyePanel::drawRecordBar(const Pen& pen, const UiText& t, const std::string& folder, bool calib, double x0,
                               double x1, double y, bool draw) {
    const char* text = calib ? t.recordSavedCalib : t.recordSavedFit;
    const double padX = 16;
    const double iconW = 30;
    const double bh = 38;
    const double bw = pen.measure(t.recordShow, 16, true) + 40;
    const double textX = x0 + padX + iconW;
    const double textW = x1 - padX - bw - 14 - textX;
    const std::vector<std::string> lines = wrapText(pen, text, 15, false, textW, 2);
    const double h = std::max(56.0, lines.size() * 21.0 + 22);
    if (!draw) return h;
    fillRounded(pen, x0, y, x1 - x0, h, 12, kControl);
    // A sheet with a check on it
    {
        cairo_t* cr = pen.cr;
        const double ix = x0 + padX;
        const double iy = y + h / 2 - 11;
        pen.color(kSuccess);
        cairo_set_line_width(cr, 2);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_move_to(cr, ix + 3, iy + 1);
        cairo_line_to(cr, ix + 13, iy + 1);
        cairo_line_to(cr, ix + 19, iy + 7);
        cairo_line_to(cr, ix + 19, iy + 21);
        cairo_line_to(cr, ix + 3, iy + 21);
        cairo_close_path(cr);
        cairo_stroke(cr);
        cairo_move_to(cr, ix + 7, iy + 13);
        cairo_line_to(cr, ix + 10, iy + 16);
        cairo_line_to(cr, ix + 15, iy + 10);
        cairo_stroke(cr);
    }
    double baseline = y + h / 2 - (lines.size() - 1) * 10.5 + 5;
    for (const std::string& line : lines) {
        pen.text(textX, baseline, line, 15, kText);
        baseline += 21;
    }
    const int index = static_cast<int>(recordNames_.size());
    recordNames_.push_back(folder);
    drawButton(pen, x1 - padX + 6 - bw, y + (h - bh) / 2, bw, bh, t.recordShow, {PanelAction::RecordOpen, nullptr, index},
               true, false, 16);
    return h;
}

double EyePanel::drawDiagChip(const Pen& pen, const UiText& t, double right, double top, double h, double size,
                               bool draw) {
    const PanelHit hit {PanelAction::DiagOpen, nullptr, 0};
    const double w = pen.measure(t.diagButton, size, true) + 32;
    if (!draw) return w;
    const double x = right - w;
    const int pointer = pointerState(hit);
    fillRounded(pen, x, top, w, h, h / 2, pointer > 0 ? kControlHover : kControl);
    strokeRounded(pen, x, top, w, h, h / 2, kBorder, 2);
    textCentered(pen, x + w / 2, centerBaseline(top, h, size), t.diagButton, size, kText, true);
    addButton(hit, x, top, w, h);
    return w;
}

void EyePanel::drawDiag(const Pen& pen, const UiText& t, const PanelModel& m) {
    const double top = kContentY + 14;
    const double headerH = 56;
    // "Back" at the right, the code left of it (its label over it), the title and what the page is for at the left
    const double backW = std::max(110.0, pen.measure(t.diagBack, 18, true) + 48);
    drawButton(pen, kInnerRight - backW, top + 4, backW, headerH - 8, t.diagBack, {PanelAction::DiagClose, nullptr, 0},
               true, false, 18);
    const std::string code = diag::code(m);
    const double codeSize = 20;
    const double chipH = 32;
    const double chipW = pen.measure(code, codeSize, true) + 28;
    const double chipRight = kInnerRight - backW - 14;
    const double chipX = chipRight - chipW;
    const double chipY = top + headerH - chipH - 2;
    pen.text(chipRight, chipY - 5, t.diagCodeLabel, 13, kTextMuted, false, true);
    fillRounded(pen, chipX, chipY, chipW, chipH, 8, kAccent);
    textCentered(pen, chipX + chipW / 2, centerBaseline(chipY, chipH, codeSize), code, codeSize, kOnAccent, true);
    const double codeLeft = std::min(chipX, chipRight - pen.measure(t.diagCodeLabel, 13, false));
    const double titleRoom = std::max(120.0, codeLeft - 20 - kInnerX);
    pen.text(kInnerX, top + 25, t.diagTitle, fitSize(pen, t.diagTitle, 22, 16, titleRoom, true), kText, true);
    pen.text(kInnerX, top + 48, t.diagSub, fitSize(pen, t.diagSub, 15, 11, titleRoom, false), kTextMuted);

    // The four cards, two by two: each row of cards as tall as its fuller card needs, the room left shared out
    const std::vector<diag::Card> cards = diag::cards(t, m);
    const double gap = 12;
    const double gridTop = top + headerH + 10;
    const double gridBottom = kContentY + kContentH - 14;
    const double cardW = (kInnerRight - kInnerX - gap) / 2;
    const double padX = 14;
    const double innerW = cardW - padX * 2;
    const double titleBaseline = 25;  // from the card's top
    const double firstGap = 26;       // the title's baseline to the first row's
    const double bottomPad = 12;      // the last baseline to the card's bottom
    /** A row as laid out: its value's lines, beside the label (one) or under it. */
    struct Laid {
        bool under = false;
        std::vector<std::string> lines;
    };
    /** How a card is laid out: the text size and its rows. */
    struct Layout {
        double size = 15;
        std::vector<Laid> rows;
        double height = 0;  ///< the card's height it needs
    };
    const auto layout = [&](const diag::Card& card, double size, size_t maxLines) {
        Layout l;
        l.size = size;
        const double step = size + 8;
        const double wrapStep = size + 5;
        double h = titleBaseline + firstGap - step;
        for (const diag::Row& row : card.rows) {
            Laid laid;
            const double labelW = pen.measure(row.label, size - 1, false);
            if (labelW + 14 + pen.measure(row.value, size, row.bad) <= innerW) {
                laid.lines = {row.value};
                h += step;
            } else {
                laid.under = true;
                laid.lines = wrapText(pen, row.value, size, row.bad, innerW - 12, maxLines);
                h += step + laid.lines.size() * wrapStep;
            }
            l.rows.push_back(laid);
        }
        l.height = h + bottomPad;
        return l;
    };
    // Each card as it would like to be (values under their labels up to three lines)
    std::vector<Layout> wanted;
    for (size_t i = 0; i < cards.size() && i < 4; ++i) wanted.push_back(layout(cards[i], 15, 3));
    const double room = gridBottom - gridTop - gap;
    double rowH[2] = {0, 0};
    for (size_t i = 0; i < wanted.size(); ++i) rowH[i / 2] = std::max(rowH[i / 2], wanted[i].height);
    if (rowH[0] + rowH[1] <= room) {
        const double extra = (room - rowH[0] - rowH[1]) / 2;
        rowH[0] += extra;
        rowH[1] += extra;
    } else {
        // Too much: shared out as they need it (each card shrinks to fit below)
        const double need = rowH[0] + rowH[1];
        rowH[0] = room * rowH[0] / need;
        rowH[1] = room - rowH[0];
    }
    for (size_t i = 0; i < wanted.size(); ++i) {
        const diag::Card& card = cards[i];
        const double x = kInnerX + (i % 2) * (cardW + gap);
        const double y = gridTop + (i / 2) * (rowH[0] + gap);
        const double cardH = rowH[i / 2];
        // Shrunk until it fits: two lines under a label, then one, then smaller type
        Layout l = wanted[i];
        for (int pass = 0; l.height > cardH && pass < 5; ++pass) {
            l = layout(card, pass < 2 ? 15 : 15 - (pass - 1), pass == 0 ? 2 : 1);
        }
        fillRounded(pen, x, y, cardW, cardH, 12, kBg);
        pen.text(x + padX, y + titleBaseline, card.title, 15, kAccent, true);
        const double step = l.size + 8;
        const double wrapStep = l.size + 5;
        double baseline = y + titleBaseline + firstGap;
        for (size_t r = 0; r < card.rows.size(); ++r) {
            const diag::Row& row = card.rows[r];
            const Laid& laid = l.rows[r];
            const Color valueColor = row.bad ? kDanger : kText;
            pen.text(x + padX, baseline, row.label, l.size - 1, kTextMuted);
            if (!laid.under) {
                pen.text(x + cardW - padX, baseline, laid.lines.front(), l.size, valueColor, row.bad, true);
            } else {
                for (const std::string& line : laid.lines) {
                    baseline += wrapStep;
                    pen.text(x + padX + 12, baseline, line, l.size, valueColor, row.bad);
                }
            }
            baseline += step;
        }
    }
}

void EyePanel::drawHistory(const Pen& pen, const UiText& t, const PanelModel& m) {
    cairo_t* cr = pen.cr;
    const std::vector<changelog::Entry>& entries = m.history.entries;

    // The title row: the title, and "Close" back to the Advanced tab
    const double closeW = 150;
    const double titleSize = fitSize(pen, t.historyTitle, 22, 16, kInnerRight - closeW - 20 - kInnerX, true);
    pen.text(kInnerX, centerBaseline(kHistoryTitleY, kHistoryTitleH, titleSize), t.historyTitle, titleSize, kText, true);
    drawButton(pen, kInnerRight - closeW, kHistoryTitleY + 2, closeW, kHistoryTitleH - 4, t.historyClose,
               {PanelAction::HistoryClose, nullptr, 0}, true, false);
    pen.color(kDivider);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, kInnerX, kHistoryTitleY + kHistoryTitleH + 9.5);
    cairo_line_to(cr, kInnerRight, kHistoryTitleY + kHistoryTitleH + 9.5);
    cairo_stroke(cr);

    const double viewTop = kHistoryViewTop;
    const double viewBottom = kHistoryViewBottom;
    const double viewH = viewBottom - viewTop;
    const double barX = kInnerRight - kHistoryBarW;
    const double rowX = kInnerX;
    const double rowW = barX - 14 - rowX;
    const double padX = 18;
    const double textX = rowX + padX;
    const double textW = rowW - padX * 2;
    const double bulletIndent = 20;
    historyViewH_ = viewH;
    historyVersions_.clear();
    for (const changelog::Entry& entry : entries) historyVersions_.push_back(entry.version);
    if (entries.empty()) {
        historyScroll_ = 0.0;
        historyMaxScroll_ = 0.0;
        textCentered(pen, (kInnerX + kInnerRight) / 2, viewTop + 80, t.historyMissing, 18, kTextMuted, false);
        return;
    }

    // Right after opening: the installed version's row, else the newest
    if (historyRowPending_) {
        historyRowPending_ = false;
        const std::string installed = bareVersion(m.update.current);
        const bool listed = std::find(historyVersions_.begin(), historyVersions_.end(), installed) != historyVersions_.end();
        historyRow_ = listed ? installed : historyVersions_.front();
        historyReveal_ = !historyScrollSet_;
    }
    const auto found = std::find(historyVersions_.begin(), historyVersions_.end(), historyRow_);
    const int open = found == historyVersions_.end() ? -1 : static_cast<int>(found - historyVersions_.begin());

    // The open row's text: the whole summary, and the items (English ones are first sentences, up to two lines)
    std::vector<std::string> summaryLines;
    std::vector<std::vector<std::string>> bulletLines;
    double openH = kHistoryHeaderH;
    if (open >= 0) {
        const changelog::Entry& entry = entries[open];
        if (!entry.summary.empty()) {
            summaryLines = wrapText(pen, entry.summary, kHistoryTextSize, false, textW, 20);
            openH += summaryLines.size() * kHistoryLineStep + 8;
        }
        for (const std::string& bullet : entry.bullets) {
            bulletLines.push_back(
                wrapText(pen, bullet, kHistoryTextSize, false, textW - bulletIndent, entry.japanese ? 20 : 2));
            openH += bulletLines.back().size() * kHistoryLineStep + 5;
        }
        openH += 10;
    }

    // Where each row is in the list, how far it can scroll, and the open row brought into view
    std::vector<double> rowTop;
    double contentH = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        rowTop.push_back(contentH);
        contentH += (static_cast<int>(i) == open ? openH : kHistoryHeaderH) + kHistoryRowGap;
    }
    contentH -= kHistoryRowGap;
    historyMaxScroll_ = std::max(0.0, contentH - viewH);
    if (historyReveal_ && open >= 0) {
        // As much of the row as fits, never past its header
        const double top = rowTop[open];
        if (top + openH > historyScroll_ + viewH) historyScroll_ = std::min(top, top + openH - viewH);
        if (top < historyScroll_) historyScroll_ = top;
    }
    historyReveal_ = false;
    historyScroll_ = std::max(0.0, std::min(historyMaxScroll_, historyScroll_));

    // The rows, clipped to the area under the title row
    cairo_save(cr);
    cairo_rectangle(cr, rowX - 2, viewTop, rowW + 4, viewH);
    cairo_clip(cr);
    for (size_t i = 0; i < entries.size(); ++i) {
        const changelog::Entry& entry = entries[i];
        const bool isOpen = static_cast<int>(i) == open;
        const double h = isOpen ? openH : kHistoryHeaderH;
        const double y = viewTop + rowTop[i] - historyScroll_;
        if (y + h < viewTop || y > viewBottom) continue;
        const PanelHit hit {PanelAction::HistoryRow, nullptr, static_cast<int>(i)};
        const int pointer = pointerState(hit);
        if (isOpen) {
            fillRounded(pen, rowX, y, rowW, h, 14, kAccentTint);
            if (pointer > 0) fillRounded(pen, rowX + 4, y + 4, rowW - 8, kHistoryHeaderH - 8, 10, kControlHover);
            strokeRounded(pen, rowX, y, rowW, h, 14, kAccent, 2);
        } else {
            fillRounded(pen, rowX, y, rowW, h, 14, pointer > 0 ? kControlHover : kControl);
            strokeRounded(pen, rowX, y, rowW, h, 14, kBorder, 2);
        }
        // A triangle pointing right (folded) or down (open)
        const double cx = textX + 5;
        const double cy = y + kHistoryHeaderH / 2;
        pen.color(kText);
        if (isOpen) {
            cairo_move_to(cr, cx - 7, cy - 4);
            cairo_line_to(cr, cx + 7, cy - 4);
            cairo_line_to(cr, cx, cy + 5);
        } else {
            cairo_move_to(cr, cx - 4, cy - 7);
            cairo_line_to(cr, cx + 5, cy);
            cairo_line_to(cr, cx - 4, cy + 7);
        }
        cairo_close_path(cr);
        cairo_fill(cr);
        // "0.7.1 · 10/3", then (folded) the summary on the rest of the line
        const std::string header = entry.date.empty() ? entry.version : entry.version + " · " + entry.date;
        const double headerX = textX + 22;
        const double baseline = centerBaseline(y, kHistoryHeaderH, 18);
        const double headerW = pen.text(headerX, baseline, header, 18, kText, true);
        if (!isOpen) {
            // No summary paragraph: the first item stands in
            const std::string line = !entry.summary.empty() ? entry.summary
                                     : !entry.bullets.empty() ? entry.bullets.front()
                                                              : std::string();
            const double x = headerX + headerW + 18;
            const double room = rowX + rowW - padX - x;
            if (!line.empty() && room > 40) {
                pen.text(x, centerBaseline(y, kHistoryHeaderH, kHistoryTextSize),
                         ellipsize(pen, line, kHistoryTextSize, false, room, false), kHistoryTextSize,
                         pointer > 0 ? kText : kTextMuted);
            }
        } else {
            double top = y + kHistoryHeaderH - 4;
            for (const std::string& line : summaryLines) {
                top += kHistoryLineStep;
                pen.text(textX, top - 6, line, kHistoryTextSize, kText);
            }
            if (!summaryLines.empty()) top += 8;
            for (const std::vector<std::string>& lines : bulletLines) {
                for (size_t l = 0; l < lines.size(); ++l) {
                    top += kHistoryLineStep;
                    if (l == 0) drawDot(cr, textX + 6, top - 11, 3.5, kAccent);
                    pen.text(textX + bulletIndent, top - 6, lines[l], kHistoryTextSize, kText);
                }
                top += 5;
            }
        }
        // Only the part of the header that shows can be pressed
        const double hitTop = std::max(y, viewTop);
        const double hitBottom = std::min(y + kHistoryHeaderH, viewBottom);
        if (hitBottom - hitTop > 4) addButton(hit, rowX, hitTop, rowW, hitBottom - hitTop);
    }
    cairo_restore(cr);

    // ▲ / ▼ at the right edge (a third of the view per press), with where the view is between them
    drawScrollBar(pen, PanelAction::HistoryScroll, barX, viewTop, viewBottom, historyScroll_, historyMaxScroll_,
                  contentH);
}

void EyePanel::drawScrollBar(const Pen& pen, PanelAction action, double x, double viewTop, double viewBottom,
                             double scroll, double maxScroll, double contentH) {
    cairo_t* cr = pen.cr;
    const double viewH = viewBottom - viewTop;
    const double barCx = x + kHistoryBarW / 2;
    const auto arrow = [&](int direction, double by) {
        const PanelHit hit {action, nullptr, direction};
        const bool can = direction < 0 ? scroll > 0.5 : scroll < maxScroll - 0.5;
        const int pointer = can ? pointerState(hit) : 0;
        fillRounded(pen, x, by, kHistoryBarW, kHistoryBarW, 14,
                    pointer == 2 ? kAccentPressed : (pointer == 1 ? kControlHover : kControl));
        strokeRounded(pen, x, by, kHistoryBarW, kHistoryBarW, 14, can ? kBorder : kDivider, 2);
        pen.color(pointer == 2 ? kOnAccent : (can ? kText : kTextDisabled));
        const double cy = by + kHistoryBarW / 2;
        // A chevron (lines, not a filled triangle), pointing up or down
        cairo_save(cr);
        cairo_set_line_width(cr, 2.5);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_move_to(cr, barCx - 8, cy - direction * 4);
        cairo_line_to(cr, barCx, cy + direction * 4);
        cairo_line_to(cr, barCx + 8, cy - direction * 4);
        cairo_stroke(cr);
        cairo_restore(cr);
        addButton(hit, x, by, kHistoryBarW, kHistoryBarW, can);
    };
    arrow(-1, viewTop);
    arrow(1, viewBottom - kHistoryBarW);
    const double trackTop = viewTop + kHistoryBarW + 10;
    const double trackH = viewBottom - kHistoryBarW - 10 - trackTop;
    fillRounded(pen, barCx - 3, trackTop, 6, trackH, 3, kControl);
    if (maxScroll > 0 && contentH > 0) {
        const double thumbH = std::max(36.0, trackH * viewH / contentH);
        const double thumbY = trackTop + (trackH - thumbH) * scroll / maxScroll;
        fillRounded(pen, barCx - 3, thumbY, 6, thumbH, 3, kBorder);
    }
}

void EyePanel::drawPrompt(const Pen& pen, const UiText& t) {
    // Only the prompt's buttons stay usable
    buttons_.clear();
    cairo_set_source_rgba(pen.cr, 0, 0, 0, 0.62);
    pen.roundedRect(0, 0, kWidth, kHeight, 24);
    cairo_fill(pen.cr);
    const bool update = !updatePromptVersion_.empty();
    const bool livelink = promptOutput_ == kOutputLivelink;
    const bool module = promptOutput_ == kOutputModule;
    const double w = 760;
    const double h = 270;
    const double x = (kWidth - w) / 2;
    const double y = (kHeight - h) / 2;
    drawCard(pen, x, y, w, h, 24, kCard, kAccent, 2);
    const std::string title = update ? formatText(updatePromptBack_ ? t.updateBackConfirmFormat : t.updateConfirmFormat,
                                                  updatePromptVersion_)
                                     : livelink   ? t.promptLivelink
                                     : module     ? t.promptModule
                                                  : t.promptVrchat;
    const double titleSize = fitSize(pen, title, 24, 16, w - 60, true);
    textCentered(pen, x + w / 2, y + 62, title, titleSize, kText, true);
    std::string detail1 = livelink   ? t.promptLivelinkDetail1
                          : module   ? t.promptModuleDetail1
                                     : t.promptVrchatDetail1;
    std::string detail2 = livelink   ? t.promptLivelinkDetail2
                          : module   ? t.promptModuleDetail2
                                     : t.promptVrchatDetail2;
    if (update) {
        const char* hint = updatePromptBack_ ? t.updateBackConfirmHint : t.updateConfirmHint;
        const std::vector<std::string> lines = wrapText(pen, hint, 17, false, w - 60, 2);
        detail1 = lines.empty() ? "" : lines[0];
        detail2 = lines.size() > 1 ? lines[1] : "";
    }
    textCentered(pen, x + w / 2, y + 108, detail1, fitSize(pen, detail1, 17, 12, w - 60, false), kTextMuted, false);
    if (!detail2.empty()) {
        textCentered(pen, x + w / 2, y + 136, detail2, fitSize(pen, detail2, 17, 12, w - 60, false), kTextMuted,
                     false);
    }
    const int arg = std::max(0, argOfOutput(promptOutput_));
    const double bw = 240;
    const double by = y + h - 32 - kControlH - 4;
    for (int i = 0; i < 2; ++i) {
        const bool yes = i == 0;
        PanelHit hit {yes ? PanelAction::PromptYes : PanelAction::PromptNo, nullptr, arg};
        if (update) hit = {yes ? PanelAction::UpdateConfirm : PanelAction::UpdateCancel, nullptr, 0};
        const double bx = yes ? x + w / 2 - bw - 12 : x + w / 2 + 12;
        const int pointer = pointerState(hit);
        const char* updateYes = updatePromptBack_ ? t.updateBackConfirmYes : t.updateConfirmYes;
        const std::string label = update ? (yes ? updateYes : t.updateConfirmNo) : (yes ? t.promptYes : t.promptNo);
        if (yes) {
            fillRounded(pen, bx, by, bw, kControlH + 4, (kControlH + 4) / 2, pointer == 2 ? kAccentPressed : kAccent);
        } else {
            fillRounded(pen, bx, by, bw, kControlH + 4, (kControlH + 4) / 2, pointer > 0 ? kControlHover : kControl);
            strokeRounded(pen, bx, by, bw, kControlH + 4, (kControlH + 4) / 2, kBorder, 2);
        }
        const double size = fitSize(pen, label, 21, 14, bw - 24, true);
        textCentered(pen, bx + bw / 2, centerBaseline(by, kControlH + 4, size), label, size, yes ? kOnAccent : kText,
                     true);
        addButton(hit, bx, by, bw, kControlH + 4);
    }
}

void EyePanel::drawKeypad(const Pen& pen, const UiText& t) {
    const bool host = keypadFor_ == KeypadFor::Host;
    // Only the keypad's buttons stay usable
    buttons_.clear();
    cairo_set_source_rgba(pen.cr, 0, 0, 0, 0.62);
    pen.roundedRect(0, 0, kWidth, kHeight, 24);
    cairo_fill(pen.cr);
    const double w = 760;
    const double h = 560;
    const double x = (kWidth - w) / 2;
    const double y = (kHeight - h) / 2;
    drawCard(pen, x, y, w, h, 24, kCard, kAccent, 2);
    const char* title = host ? t.hostEntryTitle : t.portEntryTitle;
    textCentered(pen, x + w / 2, y + 52, title, fitSize(pen, title, 24, 16, w - 60, true), kText, true);

    // What is typed, with a caret
    const double fx = x + 40;
    const double fy = y + 78;
    const double fw = w - 80;
    const double fh = 60;
    fillRounded(pen, fx, fy, fw, fh, 14, kControl);
    strokeRounded(pen, fx, fy, fw, fh, 14, kAccent, 2);
    const double textSize = fitSize(pen, keypadText_ + "|", 28, 14, fw - 40, true);
    const double tw = pen.text(fx + 20, centerBaseline(fy, fh, textSize), keypadText_, textSize, kText, true);
    pen.color(kAccent);
    cairo_rectangle(pen.cr, fx + 22 + tw, fy + 14, 2.5, fh - 28);
    cairo_fill(pen.cr);
    // Why it can't be used, or how to type it
    using host_entry::PortError;
    const std::string problem = host                             ? keypadError_
                                : portError_ == PortError::Empty ? std::string(t.hostErrEmpty)
                                : portError_ == PortError::Range ? std::string(t.portErrRange)
                                                                 : std::string();
    const bool error = !problem.empty();
    const std::string below = error ? problem : std::string(host ? t.hostEntryHint : t.portEntryHint);
    pen.text(fx + 4, fy + fh + 30, below, fitSize(pen, below, 17, 12, fw - 8, error), error ? kDanger : kTextMuted,
             error);

    // The keypad: 1-9, then "." (the address's only: a port's leaves its place empty), "0" and backspace
    const double keyW = 124;
    const double keyH = 64;
    const double gap = 12;
    const double kx = x + 40;
    const double ky = y + 196;
    const int keys[12] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', host ? '.' : 0, '0', host_entry::kBackspace};
    for (int i = 0; i < 12; ++i) {
        if (keys[i] == 0) continue;
        const double bx = kx + (i % 3) * (keyW + gap);
        const double by = ky + (i / 3) * (keyH + gap);
        const PanelHit hit {PanelAction::KeypadKey, nullptr, keys[i]};
        const int pointer = pointerState(hit);
        fillRounded(pen, bx, by, keyW, keyH, 16, pointer == 2 ? kAccentPressed : (pointer == 1 ? kControlHover : kControl));
        strokeRounded(pen, bx, by, keyW, keyH, 16, kBorder, 2);
        const Color color = pointer == 2 ? kOnAccent : kText;
        if (keys[i] == host_entry::kBackspace) {
            drawBackspace(pen, bx + keyW / 2, by + keyH / 2, 1.0, color);
        } else {
            const std::string label(1, static_cast<char>(keys[i]));
            textCentered(pen, bx + keyW / 2, centerBaseline(by, keyH, 28), label, 28, color, true);
        }
        addButton(hit, bx, by, keyW, keyH);
    }

    // On the right, lined up with the keys: OK three rows tall (like a keypad's Enter), cancel by the bottom row
    const double bw = w - 80 - 3 * keyW - 2 * gap - 28;
    const double bx = x + w - 40 - bw;
    const struct {
        PanelHit hit;
        const char* label;
        bool accent;
        double y;
        double h;
    } side[2] = {{{PanelAction::KeypadOk, nullptr, 0}, t.hostEntryOk, true, ky, 3 * keyH + 2 * gap},
                 {{PanelAction::KeypadCancel, nullptr, 0}, t.hostEntryCancel, false, ky + 3 * (keyH + gap), keyH}};
    for (const auto& button : side) {
        const int pointer = pointerState(button.hit);
        if (button.accent) {
            fillRounded(pen, bx, button.y, bw, button.h, 16, pointer == 2 ? kAccentPressed : kAccent);
        } else {
            fillRounded(pen, bx, button.y, bw, button.h, 16, pointer > 0 ? kControlHover : kControl);
            strokeRounded(pen, bx, button.y, bw, button.h, 16, kBorder, 2);
        }
        const double size = fitSize(pen, button.label, button.accent ? 26 : 21, 13, bw - 28, true);
        textCentered(pen, bx + bw / 2, centerBaseline(button.y, button.h, size), button.label, size,
                     button.accent ? kOnAccent : kText, true);
        addButton(button.hit, bx, button.y, bw, button.h);
    }
}

void EyePanel::drawRun(const Pen& pen, const UiText& t, const PanelModel& m, const std::string& title) {
    using eyecam::State;
    const eyecam::View& view = m.eyecam;
    const eyecam::Status& s = view.status;
    const double cx = (kInnerX + kInnerRight) / 2;
    const double width = kInnerRight - kInnerX;
    const double bottom = kContentY + kContentH - 20;
    // Centered lines, as large as fit
    const auto centered = [&](double baseline, const std::string& text, double size, double minSize, Color c,
                              bool bold) {
        textCentered(pen, cx, baseline, text, fitSize(pen, text, size, minSize, width, bold), c, bold);
    };
    const auto wrappedCentered = [&](double baseline, const std::string& text, double size, Color c, bool bold,
                                     size_t maxLines) {
        for (const std::string& line : wrapText(pen, text, size, bold, width, maxLines)) {
            textCentered(pen, cx, baseline, line, size, c, bold);
            baseline += size * 1.4;
        }
        return baseline;
    };
    const auto number = [](double value) {
        if (!std::isfinite(value)) return std::string("—");
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", value);
        return std::string(text);
    };
    const auto clock = [](double seconds) {
        if (!std::isfinite(seconds) || seconds < 0) return std::string("—");
        const long whole = std::lround(std::floor(seconds));
        char text[32];
        std::snprintf(text, sizeof(text), "%ld:%02ld", whole / 60, whole % 60);
        return std::string(text);
    };
    char fps[160];
    std::snprintf(fps, sizeof(fps), t.eyecamFpsFormat, number(s.fpsL).c_str(), number(s.fpsR).c_str());
    const std::string fpsLine = fps;
    const bool stopUsable = !view.busy;
    const bool startUsable = !view.busy;
    // A calibration running, or the one that just failed
    const bool calibRun = s.state == State::Calibrating || (s.state == State::Error && eyecam::isCalib(view.lastRun));
    const eyecam::Calib calib = view.lastRun == eyecam::Run::CalibUser ? eyecam::Calib::User : eyecam::Calib::Wear;

    double y = kRowTop - 6;
    y += drawSectionTitle(pen, y, !title.empty() ? title : calibRun ? t.tabEyecam : t.eyecamTitle);

    // The light warning takes the place of idle's and error's views while it is open (the recorder's message too:
    // it is about the run before)
    bool messageShown = eyecamConfirm_.isOpen();
    if (eyecamConfirm_.isOpen()) {
        drawEyecamConfirm(pen, t, view, y);
    } else switch (s.state) {
        case State::Searching: {
            centered(y + 130, t.eyecamSearching, 52, 24, kText, true);
            // Why the video isn't found yet, when eyecam-rec says; otherwise the frame rates
            const std::string why = eyecam::searchText(t, s);
            if (why.empty()) {
                centered(y + 190, fpsLine, 24, 14, kTextMuted, false);
            } else {
                centered(y + 190, why, 28, 14, kDanger, true);
            }
            const double w = 240;
            drawButton(pen, cx - w / 2, y + 250, w, 68, t.eyecamStop, {PanelAction::EyecamStop, nullptr, 0},
                       stopUsable, false, 26);
            break;
        }
        case State::Recording:
        case State::Calibrating: {
            const bool calibrating = s.state == State::Calibrating;
            // A calibration waits for the video before its first step
            if (calibrating && s.stepLabel.empty()) {
                centered(y + 130, t.eyecamCalibWaiting, 44, 24, kText, true);
                if (!s.locked) {
                    const std::string why = eyecam::searchText(t, s);
                    centered(y + 190, why.empty() ? std::string(t.eyecamNotLocked) : why, 32, 14, kDanger, true);
                }
                const double w = 240;
                drawButton(pen, cx - w / 2, y + 250, w, 68, t.eyecamStop, {PanelAction::EyecamStop, nullptr, 0},
                           stopUsable, false, 26);
                break;
            }
            // The step number (left) and the fps (right), small
            double stepRight = kInnerX;
            if (s.stepCount > 0) {
                char step[64];
                const int shown = std::max(1, std::min(s.stepIndex + 1, s.stepCount));
                std::snprintf(step, sizeof(step), t.eyecamStepFormat, shown, s.stepCount);
                stepRight = kInnerX + pen.text(kInnerX, y + 22, step, 22, kTextMuted, true) + 14;
            }
            // A calibration says which one beside it, in the accent: this is no recording
            if (calibrating) {
                const std::string name = calib == eyecam::Calib::User ? t.eyecamCalibUserTitle : t.eyecamCalibWearTitle;
                const double size = 18;
                const double w = pen.measure(name, size, true) + 24;
                const double h = 30;
                fillRounded(pen, stepRight, y + 1, w, h, h / 2, kAccentTint);
                strokeRounded(pen, stepRight, y + 1, w, h, h / 2, kAccent, 2);
                textCentered(pen, stepRight + w / 2, centerBaseline(y + 1, h, size), name, size, kText, true);
            }
            // A run without the bright and dark steps says so beside it
            else if (eyecam::withoutLight(s)) {
                const double size = 18;
                const double w = pen.measure(t.eyecamNoLight, size, true) + 24;
                const double h = 30;
                fillRounded(pen, stepRight, y + 1, w, h, h / 2, kControl);
                strokeRounded(pen, stepRight, y + 1, w, h, h / 2, kBorder, 2);
                textCentered(pen, stepRight + w / 2, centerBaseline(y + 1, h, size), t.eyecamNoLight, size, kText, true);
            }
            pen.text(kInnerRight, y + 22, fpsLine, fitSize(pen, fpsLine, 18, 12, width / 2, false), kTextMuted, false,
                     true);
            // A recording's steps under it, as chips (as many as fit around the one now; none when eyecam's protocol
            // file can't be read), and everything below that lower down
            const eyecam::StepChips chips = calibrating ? eyecam::StepChips() : eyecam::recordingChips(view);
            if (!chips.labels.empty()) {
                drawRunChips(pen, t, chips, y + 66, width);
                y += 40;
            }
            // The instruction, as large as it fits
            centered(y + 150, eyecam::instruction(t, s.stepLabel), 96, 36, kText, true);
            // The seconds left of the step; while the cameras lost the eyes (the headset came off; the time runs on),
            // a call to put it back on in their place
            if (!s.locked) {
                const std::string why = eyecam::searchText(t, s);
                centered(y + 226, why.empty() ? std::string(t.eyecamNotLocked) : why, 40, 14, kDanger, true);
            } else if (std::isfinite(s.stepRemainingS)) {
                char left[64];
                std::snprintf(left, sizeof(left), t.eyecamRemainingFormat,
                              static_cast<int>(std::ceil(std::max(0.0, s.stepRemainingS) - 1e-9)));
                centered(y + 226, left, 44, 20, kAccent, true);
            }
            // The whole run: a bar, and the time so far against the total
            const double barY = y + 262;
            const double barH = 22;
            fillRounded(pen, kInnerX, barY, width, barH, barH / 2, kControl);
            strokeRounded(pen, kInnerX, barY, width, barH, barH / 2, kBorder, 2);
            if (std::isfinite(s.elapsedS) && std::isfinite(s.totalS) && s.totalS > 0) {
                const double done = std::clamp(s.elapsedS / s.totalS, 0.0, 1.0);
                if (done > 0) fillRounded(pen, kInnerX, barY, std::max(barH, width * done), barH, barH / 2, kAccent);
            }
            const std::string times = clock(s.elapsedS) + " / " + clock(s.totalS);
            pen.text(kInnerX, barY + barH + 26, times, 18, kTextMuted);
            // Nothing left to stop at the end (eyecam-rec answers "err" to it): no button
            if (eyecam::parseStep(s.stepLabel) != eyecam::Step::End) {
                const double w = 220;
                drawButton(pen, cx - w / 2, barY + barH + 44, w, 62, t.eyecamStop,
                           {PanelAction::EyecamStop, nullptr, 0}, stopUsable, false, 26);
            }
            break;
        }
        case State::Error: {
            centered(y + 76, calibRun ? t.eyecamCalibErrorTitle : t.eyecamErrorTitle, 34, 20, kDanger, true);
            // A calibration's: the eyes eyecam-rec's message names, centered under the title
            const int eyes = calibRun ? eyecam::failedEyes(s) : 0;
            if (eyes != 0) {
                double lineW = pen.measure(t.setupErrorEyes, 17, true);
                for (int eye = 0; eye < 2; ++eye) {
                    if ((eyes & (eye == 0 ? eyecam::kLeftEyeBit : eyecam::kRightEyeBit)) == 0) continue;
                    lineW += 22 + 28 + pen.measure(eye == 0 ? t.leftEye : t.rightEye, 20, true);
                }
                double ex = cx - lineW / 2;
                ex += pen.text(ex, y + 120, t.setupErrorEyes, 17, kTextMuted, true);
                for (int eye = 0; eye < 2; ++eye) {
                    if ((eyes & (eye == 0 ? eyecam::kLeftEyeBit : eyecam::kRightEyeBit)) == 0) continue;
                    drawEyeIcon(pen, ex + 22 + 10, y + 114, eye == 0 ? kDotLeft : kDotRight);
                    ex += 22 + 28 + pen.text(ex + 22 + 28, y + 120, eye == 0 ? t.leftEye : t.rightEye, 20, kDanger, true);
                }
            }
            // The user's calibration again can't be offered once the headset was put back on (bit 0 gone): a note
            // above the buttons says so
            const bool userBlocked = calibRun && calib == eyecam::Calib::User && !eyecam::userCalibAllowed(s);
            // eyecam-rec's reason (in English when it gives one; with its counts), as large as fits in 3 lines. With
            // the eyes' line and that note, a third line at that size would run into the note: 2 lines, or 3 at 17 or
            // smaller when 2 don't hold it (the third then ends above the note)
            const std::string& message = eyecam::shownMessage(s, m.language);
            if (!message.empty()) {
                size_t maxLines = 3;
                double size = wrapSize(pen, message, 24, 17, false, width, maxLines);
                if (eyes != 0 && userBlocked) {
                    maxLines = 2;
                    size = wrapSize(pen, message, 24, 17, false, width, maxLines);
                    if (wrapText(pen, message, size, false, width, 3).size() > 2) {
                        maxLines = 3;
                        size = wrapSize(pen, message, 17, 14, false, width, maxLines);
                    }
                }
                wrappedCentered(eyes != 0 ? y + 160 : y + 130, message, size, kText, false, maxLines);
                messageShown = true;
            }
            // The way on, and "Back" beside it (the error is dismissed in the panel only: eyecam-rec stays in "error"
            // until the next command, and the tab would show it again on every visit)
            const double w = 400;
            const double backW = 200;
            const double gap = 20;
            const double bh = 88;
            const double left = cx - (w + gap + backW) / 2;
            const double by = y + 262;
            if (calibRun) {
                // The same calibration again; the user's only while this wear's is still there. Put back on since
                // (bit 0 gone), it can't be: say so, and offer this wear's calibration instead
                if (userBlocked) {
                    centered(by - 22, t.eyecamUserNeedsWear, 22, 14, kAccent, true);
                    drawButton(pen, left, by, w, bh, t.camCalibButton,
                               {PanelAction::EyecamCalib, nullptr, static_cast<int>(eyecam::Calib::Wear)}, startUsable,
                               true, 28);
                } else {
                    drawButton(pen, left, by, w, bh, t.eyecamCalibRetry,
                               {PanelAction::EyecamCalib, nullptr, static_cast<int>(calib)}, startUsable, true, 34);
                }
            } else {
                drawButton(pen, left, by, w, bh, t.eyecamRetry, {PanelAction::EyecamStart, nullptr, 0}, startUsable,
                           true, 34);
            }
            drawButton(pen, left + w + gap, by, backW, bh, t.eyecamBack, {PanelAction::EyecamBack, nullptr, 0}, true,
                       false, 28);
            // A calibration's log was saved: where to find it
            if (calibRun && !m.records.lastCalib.empty()) {
                drawRecordBar(pen, t, m.records.lastCalib, true, kInnerX, kInnerRight, by + bh + 16);
            }
            break;
        }
        default: {
            centered(y + 90, formatText(t.eyecamUnknownFormat, s.stateText), 30, 18, kText, true);
            break;
        }
    }

    // At the bottom: the recorder's message, a command on its way, and a command that failed
    struct Line {
        std::string text;
        double size;
        Color color;
        bool bold;
    };
    std::vector<Line> lines;
    const std::string& message = eyecam::shownMessage(s, m.language);
    if (!message.empty() && !messageShown) {
        for (const std::string& line : wrapText(pen, message, 20, false, width, 2)) lines.push_back({line, 20, kText, false});
    }
    if (view.busy) {
        lines.push_back({t.eyecamSending, 18, kTextMuted, false});
    } else if (view.hasReply && !view.reply.ok) {
        char failed[512];
        std::snprintf(failed, sizeof(failed), t.eyecamReplyFailedFormat, view.reply.command.c_str(),
                      view.reply.error.c_str());
        for (const std::string& line : wrapText(pen, failed, 18, true, width, 2)) lines.push_back({line, 18, kDanger, true});
    }
    double baseline = bottom;
    for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
        textCentered(pen, cx, baseline, line->text, line->size, line->color, line->bold);
        baseline -= line->size * 1.4;
    }
}

void EyePanel::drawEyecam(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    // Not set up and nothing of the setup done yet: what they add, and "Get started" into the checklist
    if (eyecam::introShown(m.eyecam, setupBegun_)) {
        drawCameraIntro(pen, t);
        return;
    }
    const eyecam::SetupScreen screen = eyecam::setupScreen(m.eyecam);
    if (screen != eyecam::SetupScreen::Camera) {
        drawSetup(pen, t, m, screen);
        return;
    }
    // Set up: a calibration from the page, and how it ended, in the setup's card; its failure as it always has
    switch (eyecam::pageScreen(m.eyecam)) {
        case eyecam::PageScreen::Calibrating: drawPageCalib(pen, t, m, eyecam::SetupScreen::Learn); return;
        case eyecam::PageScreen::Error: drawRun(pen, t, m); return;
        case eyecam::PageScreen::Result:
            drawPageCalib(pen, t, m,
                          m.eyecam.flow.calibResult() == eyecam::CalibResult::Default ? eyecam::SetupScreen::Fail
                                                                                      : eyecam::SetupScreen::Calibrated);
            return;
        case eyecam::PageScreen::Page: break;
    }
    drawCameraPage(pen, t, m, v);
}

void EyePanel::drawCameraIntro(const Pen& pen, const UiText& t) {
    const double x0 = kInnerX;
    const double x1 = kInnerRight;
    // The title with a camera before it
    double y = kContentY + 46;
    drawCameraIcon(pen, x0 + 14, y - 8, kAccent);
    pen.text(x0 + 38, y, t.camIntroTitle, fitSize(pen, t.camIntroTitle, 22, 15, x1 - x0 - 38, true), kText, true);
    y += 22;
    // What they add, a check each
    for (const char* item : t.camIntro) {
        const std::vector<std::string> lines = wrapText(pen, item, 18, false, x1 - x0 - 44, 2);
        y += 14;
        for (size_t i = 0; i < lines.size(); ++i) {
            y += 26;
            if (i == 0) drawCheck(pen.cr, x0 + 16, y - 6, 16, kSuccess);
            pen.text(x0 + 36, y, lines[i], 18, kText);
        }
    }
    // The one-time setup, and the eye video staying in the Frame
    {
        y += 24;
        const double pad = 18;
        const std::vector<std::string> lines = wrapText(pen, t.camIntroBox, 16, false, x1 - x0 - pad * 2 - 30, 3);
        const double boxH = pad * 2 + lines.size() * 24 - 4;
        fillRounded(pen, x0, y, x1 - x0, boxH, 12, kBg);
        drawInfoIcon(pen, x0 + pad + 8, y + pad + 9, kAccent);
        double baseline = y + pad + 15;
        for (const std::string& line : lines) {
            pen.text(x0 + pad + 30, baseline, line, 16, kText);
            baseline += 24;
        }
        y += boxH;
    }
    // "Get started": the setup checklist (password, the tool in Konsole, the calibration)
    const double bh = 56;
    const double bw = std::max(300.0, pen.measure(t.camIntroButton, 20, true) + 64);
    const double by = std::min(y + 26, kContentY + kContentH - 24 - bh - 30);
    drawButton(pen, x0, by, bw, bh, t.camIntroButton, {PanelAction::SetupBegin, nullptr, 0}, true, true, 20);
    const double nx = x0 + bw + 20;
    const std::vector<std::string> note = wrapText(pen, t.camIntroNote, 15, false, x1 - nx, 2);
    double baseline = by + bh / 2 + 5 - (static_cast<double>(note.size()) - 1) * 10;
    for (const std::string& line : note) {
        pen.text(nx, baseline, line, 15, kTextMuted);
        baseline += 20;
    }
}

void EyePanel::drawPageCalib(const Pen& pen, const UiText& t, const PanelModel& m, eyecam::SetupScreen screen) {
    const double cardX = kInnerX;
    const double cardRight = kInnerRight - 2;
    const double padX = 22;
    // The heading: which calibration, and what it is for
    const bool user = screen == eyecam::SetupScreen::Calibrated
                          ? m.eyecam.flow.calibResult() == eyecam::CalibResult::User
                          : eyecam::calibOf(m.eyecam.lastRun) == eyecam::Calib::User;
    const char* name = user ? t.eyecamCalibUserTitle : t.eyecamCalibWearTitle;
    const char* note = user ? t.camRowUser : t.calibWearNote;
    pen.text(kInnerX, 140, name, fitSize(pen, name, 19, 14, 420, true), kText, true);
    pen.text(kInnerRight, 139, note, 14, kTextMuted, false, true);
    const double cardTop = 158;
    const double cardH = setupCard(pen, t, m, screen, cardX + padX, cardRight - padX, cardTop, false, true);
    // (green when it ended well, unless one eye went on its earlier values, or the user's on the standard look-down
    // correction or squint, or the previous one)
    const bool partial = user ? eyecam::userCalibCard(t, m.eyecam.status).fallback
                              : eyecam::partialEye(m.eyecam.status) >= 0;
    strokeRounded(pen, cardX, cardTop, cardRight - cardX, cardH, 14,
                  screen == eyecam::SetupScreen::Calibrated && !partial ? kSuccess : kAccent, 2);
    setupCard(pen, t, m, screen, cardX + padX, cardRight - padX, cardTop, true, true);
}

void EyePanel::drawIconButton(const Pen& pen, double x, double y, double w, double h, const std::string& label,
                              const PanelHit& hit, bool usable, bool accent, double textSize, ButtonIcon icon) {
    const int pointer = usable ? pointerState(hit) : 0;
    const bool filled = accent && usable;
    if (filled) {
        fillRounded(pen, x, y, w, h, h / 2, pointer == 2 ? kAccentPressed : kAccent);
    } else {
        fillRounded(pen, x, y, w, h, h / 2, pointer > 0 ? kControlHover : kControl);
        strokeRounded(pen, x, y, w, h, h / 2, usable ? kBorder : kDivider, 2);
    }
    const Color color = filled ? kOnAccent : (usable ? kText : kTextDisabled);
    const double iconW = icon == ButtonIcon::None ? 0 : 28;
    const double size = fitSize(pen, label, textSize, 12, w - 32 - iconW, true);
    const double contentW = iconW + pen.measure(label, size, true);
    const double left = x + (w - contentW) / 2;
    if (icon == ButtonIcon::Terminal) drawTerminalIcon(pen, left, y + h / 2, color);
    if (icon == ButtonIcon::Play) drawPlayIcon(pen, left + 2, y + h / 2, color);
    if (icon == ButtonIcon::Pause) {
        fillRounded(pen, left + 3, y + h / 2 - 7, 4, 14, 1, color);
        fillRounded(pen, left + 11, y + h / 2 - 7, 4, 14, 1, color);
    }
    pen.text(left + iconW, centerBaseline(y, h, size), label, size, color, true);
    addButton(hit, x, y, w, h, usable);
}

void EyePanel::drawSetup(const Pen& pen, const UiText& t, const PanelModel& m, eyecam::SetupScreen screen) {
    using eyecam::SetupScreen;
    cairo_t* cr = pen.cr;
    const bool done = screen == SetupScreen::Done;
    // The current step: (1), (2), (3), or past them all
    const int current = screen == SetupScreen::Pass ? 0 : screen == SetupScreen::Check ? 1 : done ? 3 : 2;

    // The title, and at its right how far it is
    pen.text(kInnerX, 138, t.setupTitle, fitSize(pen, t.setupTitle, 19, 14, 450, true), kText, true);
    {
        const char* note = done ? t.setupAllDone : current == 2 ? t.setupOneLeft : t.setupOptional;
        const double size = fitSize(pen, note, done ? 15 : 14, 11, 240, done);
        pen.text(kInnerRight - pen.measure(note, size, done), 137, note, size, done ? kSuccess : kTextMuted, done);
    }

    const double circleX = kInnerX + 20;
    const double radius = 17;
    const double cardX = kInnerX + 52;
    const double cardRight = kInnerRight - 2;
    const double padX = 22;
    const double rowStep = 43;
    const double firstY = 174;
    // The tool outdated or too old: (2) again
    const eyecam::Tool tool = eyecam::toolState(m.eyecam.status);
    const bool outdated = tool == eyecam::Tool::Outdated || tool == eyecam::Tool::TooOld;
    const char* names[4] = {t.setupStepPassword, outdated ? t.setupStepToolAgain : t.setupStepTool, t.setupStepLearn,
                            t.setupStepDone};
    const char* later[4] = {"", outdated ? t.setupToolUpdated : t.setupLaterTool, t.setupLaterLearn, t.setupLaterDone};
    const char* doneLabels[3] = {t.setupPasswordLabel, t.setupToolLabel, t.setupLearnLabel};
    // (with the buffer from dsp_service neither the password nor the tool was needed)
    const bool dsp = eyecam::fromDsp(m.eyecam.status);
    const char* doneValues[3] = {dsp ? t.setupNotNeeded : t.setupPasswordSet, dsp ? t.setupNotNeeded : t.setupToolDone,
                                 t.setupLearnDone};

    // The steps done in a row each, the current one in a card, those still to come under it
    const double cardTop = firstY - 17 + rowStep * current;
    const double cardH = setupCard(pen, t, m, screen, cardX + padX, cardRight - padX, cardTop, false);
    const double cardBottom = cardTop + cardH;
    const int laterCount = done ? 0 : 3 - current;
    // Set up already (the tool removed): (3) stays done, it isn't asked again
    const bool learned = (screen == SetupScreen::Pass || screen == SetupScreen::Check) &&
                         eyecam::setupComplete(m.eyecam.status);
    const double lastY = laterCount > 0 ? cardBottom + 28 + rowStep * (laterCount - 1) : cardBottom - 20;
    pen.color(kDivider);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, circleX, firstY);
    cairo_line_to(cr, circleX, lastY);
    cairo_stroke(cr);
    for (int i = 0; i < current; ++i) {
        const double cy = firstY + rowStep * i;
        drawDot(cr, circleX, cy, radius, kSuccess);
        drawCheck(cr, circleX, cy, 18, kBg);
        double x = circleX + 32;
        x += pen.text(x, cy + 6, doneLabels[i], 17, kText);
        x += pen.text(x, cy + 6, doneValues[i], 17, kSuccess, true);
        if (i == 0 && !dsp) pen.text(x + 14, cy + 6, t.setupAutoChecked, 14, kTextMuted);
        if (i == 1 && dsp) pen.text(x + 14, cy + 6, t.setupNoSudo, 14, kTextMuted);
    }
    const double markY = cardTop + 34;
    if (done) {
        drawDot(cr, circleX, markY, radius, kSuccess);
        drawCheck(cr, circleX, markY, 18, kBg);
    } else {
        drawDot(cr, circleX, markY, radius, kAccent);
        textCentered(pen, circleX, markY + 6, std::to_string(current + 1), 16, kOnAccent, true);
    }
    strokeRounded(pen, cardX, cardTop, cardRight - cardX, cardH, 14, done ? kSuccess : kAccent, 2);
    setupCard(pen, t, m, screen, cardX + padX, cardRight - padX, cardTop, true);
    for (int k = 0; k < laterCount; ++k) {
        const int i = current + 1 + k;
        const double cy = cardBottom + 28 + rowStep * k;
        if (i == 2 && learned) {
            drawDot(cr, circleX, cy, radius, kSuccess);
            drawCheck(cr, circleX, cy, 18, kBg);
            const double x = circleX + 32 + pen.text(circleX + 32, cy + 6, doneLabels[i], 17, kText);
            pen.text(x, cy + 6, doneValues[i], 17, kSuccess, true);
            continue;
        }
        drawDot(cr, circleX, cy, radius, kCard);
        drawRing(cr, circleX, cy, radius - 1, 2, kBorder);
        textCentered(pen, circleX, cy + 6, std::to_string(i + 1), 16, kTextMuted, true);
        const double nameRight = circleX + 32 + pen.text(circleX + 32, cy + 6, names[i], 17, kTextMuted);
        pen.text(nameRight + 14, cy + 6, later[i], 14, kTextMuted);
    }
}

double EyePanel::setupCard(const Pen& pen, const UiText& t, const PanelModel& m, eyecam::SetupScreen screen,
                           double x0, double x1, double top, bool draw, bool page) {
    using eyecam::SetupScreen;
    using eyecam::State;
    cairo_t* cr = pen.cr;
    const eyecam::View& view = m.eyecam;
    const eyecam::Status& s = view.status;
    // Which calibration: the setup's is always this wear's; from the page, the one that ran
    const eyecam::Calib calib =
        !page ? eyecam::Calib::Wear
              : (screen == SetupScreen::Calibrated ? (view.flow.calibResult() == eyecam::CalibResult::User
                                                          ? eyecam::Calib::User
                                                          : eyecam::Calib::Wear)
                                                   : eyecam::calibOf(view.lastRun));
    // (from the page the calibration's name is the heading above the card: the card says what to do, or "result")
    const char* learnName = !page                         ? t.setupStepLearn
                            : screen == SetupScreen::Learn ? t.calibCardTitle
                                                           : t.calibResultTitle;
    const double w = x1 - x0;
    const bool busy = view.busy;
    double y = top;  // the last baseline (or bottom) laid out
    // Measured on the first call (draw false), drawn on the second, the same way
    const auto text = [&](double x, double baseline, const std::string& str, double size, Color c, bool bold) {
        return draw ? pen.text(x, baseline, str, size, c, bold) : pen.measure(str, size, bold);
    };
    // The title, with a pill at the right saying what it waits for
    const auto title = [&](const char* name, const char* pillText, Color dot) {
        double pillW = 0;
        if (pillText != nullptr) {
            const double size = 14;
            const double h = 30;
            pillW = pen.measure(pillText, size, false) + 40;
            if (draw) {
                fillRounded(pen, x1 - pillW, top + 19, pillW, h, h / 2, kControl);
                drawDot(cr, x1 - pillW + 17, top + 34, 4, dot);
                pen.text(x1 - pillW + 29, centerBaseline(top + 19, h, size), pillText, size, kText);
            }
        }
        text(x0, top + 42, name, fitSize(pen, name, 22, 15, w - pillW - 16, true), kText, true);
        y = top + 42;
    };
    // A paragraph under what came before (wrapped)
    const auto para = [&](const std::string& str, double size, Color c, bool bold, double advance, double step,
                          size_t maxLines) {
        bool first = true;
        for (const std::string& line : wrapText(pen, str, size, bold, w, maxLines)) {
            y += first ? advance : step;
            first = false;
            text(x0, y, line, size, c, bold);
        }
    };
    // Rows of a muted label and its text (wrapped next to it)
    const auto labelWidth = [&](std::initializer_list<const char*> labels) {
        double width = 0;
        for (const char* label : labels) width = std::max(width, pen.measure(label, 15, true));
        return std::max(100.0, width + 18);
    };
    const auto row = [&](const char* label, const std::string& value, double labelW, double advance) {
        y += advance;
        text(x0, y, label, 15, kTextMuted, true);
        bool first = true;
        for (const std::string& line : wrapText(pen, value, 16, false, w - labelW, 2)) {
            if (!first) y += 24;
            first = false;
            text(x0 + labelW, y, line, 16, kText, false);
        }
    };
    // A box (the dark background) from top to top + h
    const auto box = [&](double boxTop, double h) {
        if (draw) fillRounded(pen, x0, boxTop, w, h, 10, kBg);
    };
    const bool hasVideo = setup_tools::kVideoUrl[0] != '\0';
    // The video button at the right of a button row, and the line under it (only with a video)
    const auto video = [&](double by, double bh) {
        if (!hasVideo) return by + bh;
        const double vw = pen.measure(t.setupVideo, 18, true) + 72;
        if (draw) {
            drawIconButton(pen, x1 - vw, by, vw, bh, t.setupVideo, {PanelAction::SetupVideo, nullptr, 0}, true, false,
                           18, ButtonIcon::Play);
            const std::string note = std::string(t.setupVideoNote) + setup_tools::kVideoUrl;
            const double size = fitSize(pen, note, 14, 10, w, false);
            pen.text(x1 - pen.measure(note, size, false), by + bh + 22, note, size, kTextMuted);
        }
        return by + bh + 28;
    };
    // "calib wear" went through without one eye: its title ("左目は前の値を使うよ")...
    const int partial = calib == eyecam::Calib::Wear ? eyecam::partialEye(s) : -1;
    const auto partialTitle = [&]() {
        return formatText(eyecam::partialProvisional(s) ? t.partialTitleProvFormat : t.partialTitlePrevFormat,
                          partial == 0 ? t.partialEyeLeft : t.partialEyeRight);
    };
    // ...each eye in a box (learned now, or on its earlier or provisional values), with eyecam-rec's message (its
    // reason and counts) under them
    const auto partialBox = [&]() {
        const bool provisional = eyecam::partialProvisional(s);
        const std::string& message = eyecam::shownMessage(s, m.language);
        const std::vector<std::string> lines =
            message.empty() ? std::vector<std::string>() : wrapText(pen, message, 14, false, w - 36, 2);
        const double boxTop = y + 18;
        const double rowsBottom = boxTop + 14 + 2 * 32;
        const double boxH = rowsBottom - boxTop + (lines.empty() ? 6 : 16 + lines.size() * 20);
        box(boxTop, boxH);
        double labelW = 0;
        for (const char* label : {t.leftEye, t.rightEye}) labelW = std::max(labelW, pen.measure(label, 17, false));
        const double valueX = x0 + 50 + std::max(110.0, labelW + 30);
        for (int eye = 0; eye < 2; ++eye) {
            const double baseline = boxTop + 36 + eye * 32;
            const bool failedOne = eye == partial;
            if (draw) drawEyeIcon(pen, x0 + 28, baseline - 6, eye == 0 ? kDotLeft : kDotRight);
            text(x0 + 50, baseline, eye == 0 ? t.leftEye : t.rightEye, 17, kTextMuted, false);
            const char* value = !failedOne ? t.partialNew : provisional ? t.partialProv : t.partialPrev;
            text(valueX, baseline, value, 17, failedOne ? kAccent : kSuccess, true);
        }
        double baseline = rowsBottom + 20;
        for (const std::string& line : lines) {
            text(x0 + 18, baseline, line, 14, kTextMuted, false);
            baseline += 20;
        }
        y = boxTop + boxH;
    };
    // ...and its buttons: on (accent), or the calibration again
    const auto partialButtons = [&](const char* proceed) {
        const double by = y + 18;
        const double bh = 46;
        const double pw = std::max(120.0, pen.measure(proceed, 18, true) + 56);
        const double aw = pen.measure(t.setupAgain, 18, true) + 76;
        if (draw) {
            drawButton(pen, x0, by, pw, bh, proceed, {PanelAction::SetupProceed, nullptr, 0}, true, true, 18);
            drawIconButton(pen, x0 + pw + 10, by, aw, bh, t.setupAgain,
                           {PanelAction::EyecamCalib, nullptr, static_cast<int>(eyecam::Calib::Wear)}, !busy, false, 18,
                           ButtonIcon::Play);
        }
        y = by + bh;
    };

    switch (screen) {
        case SetupScreen::Pass: {
            title(t.setupStepPassword, t.setupPassPill, kAccent);
            para(t.setupPassBody, 17, kText, false, 37, 27, 3);
            const double labelW = labelWidth({t.setupPassWhere, t.setupPassKonsole, t.setupPassMemo});
            // Where in the settings: the path as chips
            y += 40;
            text(x0, y, t.setupPassWhere, 15, kTextMuted, true);
            double cx = x0 + labelW;
            const char* path[3] = {t.setupPassPath1, t.setupPassPath2, t.setupPassPath3};
            for (int i = 0; i < 3; ++i) {
                const double chipW = pen.measure(path[i], 16, false) + 24;
                if (draw) {
                    fillRounded(pen, cx, y - 21, chipW, 30, 6, kControl);
                    pen.text(cx + 12, y, path[i], 16, kText);
                    if (i < 2) drawChevron(pen, cx + chipW + 13, y - 6, 10, 1.8, kTextMuted);
                }
                cx += chipW + 26;
            }
            row(t.setupPassKonsole, t.setupPassKonsoleHow, labelW, 34);
            row(t.setupPassMemo, t.setupPassMemoText, labelW, 30);
            const double by = y + 22;
            const double bh = 46;
            const double bw = pen.measure(t.setupPassButton, 18, true) + 72;
            if (draw) {
                drawIconButton(pen, x0, by, bw, bh, t.setupPassButton, {PanelAction::SetupKonsole, nullptr, 1}, true,
                               false, 18, ButtonIcon::Terminal);
            }
            y = video(by, bh);
            break;
        }
        case SetupScreen::Check: {
            // An outdated or too old tool: the same, to do again (it says so under the title)
            const eyecam::Tool tool = eyecam::toolState(s);
            const bool again = tool == eyecam::Tool::Outdated || tool == eyecam::Tool::TooOld;
            title(again ? t.setupStepToolAgain : t.setupStepTool, t.setupCheckPill, kAccent);
            if (again) para(t.setupToolUpdated, 17, kAccent, true, 34, 26, 2);
            // How: the Konsole button, Enter, the password (typed in already)
            y += 36;
            if (draw) drawTerminalIcon(pen, x0, y - 6, kText);
            const double flowRight = x0 + 28 + text(x0 + 28, y, t.setupCheckFlow, 17, kText, true);
            text(flowRight + 12, y, t.setupCheckTyped, 14, kTextMuted, false);
            // The command, to see (and to type over SSH)
            const double boxTop = y + 14;
            const double boxH = 56;
            if (draw) {
                fillRounded(pen, x0, boxTop, w, boxH, 10, kBg);
                strokeRounded(pen, x0, boxTop, w, boxH, 10, kBorder, 1.5);
                const double size = fitSize(pen, setup_tools::kShownInstallCommand, 21, 13, w - 36, false);
                pen.text(x0 + 18, centerBaseline(boxTop, boxH, size), setup_tools::kShownInstallCommand, size, kText);
            }
            y = boxTop + boxH;
            const double labelW = labelWidth({t.setupCheckWhat, t.setupCheckPassword, t.setupCheckSsh});
            row(t.setupCheckWhat, t.setupCheckWhatText, labelW, 32);
            row(t.setupCheckPassword, t.setupCheckPasswordText, labelW, 26);
            row(t.setupCheckSsh, t.setupCheckSshText, labelW, 26);
            const double by = y + 18;
            const double bh = 46;
            const double bw = pen.measure(t.setupCheckButton, 18, true) + 72;
            if (draw) {
                drawIconButton(pen, x0, by, bw, bh, t.setupCheckButton, {PanelAction::SetupKonsole, nullptr, 0}, true,
                               true, 18, ButtonIcon::Terminal);
            }
            y = video(by, bh);
            break;
        }
        case SetupScreen::Wait: {
            title(t.setupStepLearn, t.setupWaitPill, kAccent);
            y += 50;
            text(x0, y, t.setupWaitTitle, fitSize(pen, t.setupWaitTitle, 26, 16, w, true), kText, true);
            // What it needs: the camera video (frames coming in, not just the buffers held), and each eye seen (its
            // pupil found, when eyecam-rec says)
            const bool video = eyecam::videoFlowing(s);
            const double boxTop = y + 18;
            const double boxH = 126;
            box(boxTop, boxH);
            const double valueX = x0 + 210;
            for (int i = 0; i < 3; ++i) {
                const double baseline = boxTop + 38 + i * 35;
                const eyecam::EyeSight sight = i == 0 ? eyecam::EyeSight::Seen : eyecam::eyeSight(s, i - 1);
                const bool ok = i == 0 ? video : sight == eyecam::EyeSight::Seen;
                if (draw) {
                    if (i == 0) {
                        if (ok) {
                            drawCheck(cr, x0 + 28, baseline - 6, 16, kSuccess);
                        } else {
                            drawRing(cr, x0 + 28, baseline - 6, 6, 2, kTextMuted);
                        }
                    } else {
                        drawEyeIcon(pen, x0 + 28, baseline - 6, i == 1 ? kDotLeft : kDotRight);
                    }
                }
                const char* label = i == 0 ? t.setupWaitVideo : i == 1 ? t.leftEye : t.rightEye;
                // The video coming in slowly (eyecam-rec copes, but it is worth knowing): how fast, in the accent
                const double fps = eyecam::cameraFps(s);
                const bool slow = i == 0 && ok && fps < eyecam::kLowCameraFps;
                char slowText[96] = "";
                if (slow) {
                    std::snprintf(slowText, sizeof(slowText), t.setupWaitVideoSlowFormat, static_cast<int>(std::lround(fps)));
                }
                const char* value = slow    ? slowText
                                    : i == 0 ? (ok ? t.setupWaitVideoOk : t.setupWaitVideoNo)
                                    : sight == eyecam::EyeSight::Seen    ? t.setupWaitEyeOk
                                    : sight == eyecam::EyeSight::NoPupil ? t.setupWaitEyeNoPupil
                                    : sight == eyecam::EyeSight::Weak    ? t.setupWaitEyeWeak
                                                                         : t.setupWaitEyeNo;
                // The video without the pupil in red (the calibration would fail), only some of the time in the accent;
                // one eye not seen while the video is there (its frames stopped) in the accent too
                const bool noPupil = i > 0 && sight == eyecam::EyeSight::NoPupil;
                const bool weak = i > 0 && (sight == eyecam::EyeSight::Weak ||
                                            (s.locked && sight == eyecam::EyeSight::NotSeen));
                const Color color = slow ? kAccent : ok ? kSuccess : noPupil ? kDanger : weak ? kAccent : kTextMuted;
                const bool bold = ok || noPupil || weak;
                text(x0 + 50, baseline, label, 17, kTextMuted, false);
                text(valueX, baseline, value, fitSize(pen, value, 17, 12, x1 - 18 - valueX, bold), color, bold);
            }
            y = boxTop + boxH;
            // Whenever the video is there and eyecam-rec is ready (the pupils not found only warn: see under it)
            const double by = y + 14;
            const double bh = 46;
            const double bw = std::max(186.0, pen.measure(t.setupWaitButton, 18, true) + 76);
            const bool usable = s.locked && (s.state == State::Idle || s.state == State::Error) && !busy;
            if (draw) {
                drawIconButton(pen, x0, by, bw, bh, t.setupWaitButton,
                               {PanelAction::EyecamCalib, nullptr, static_cast<int>(eyecam::Calib::Wear)}, usable,
                               true, 18, ButtonIcon::Play);
                const double hx = x0 + bw + 18;
                pen.text(hx, by + 19, t.setupWaitHint1, fitSize(pen, t.setupWaitHint1, 15, 11, x1 - hx, false),
                         kTextMuted);
                pen.text(hx, by + 41, t.setupWaitHint2, fitSize(pen, t.setupWaitHint2, 15, 11, x1 - hx, false),
                         kTextMuted);
            }
            y = by + bh;
            // An eye's pupil not found: put the headset on again first. The video not found: why, as eyecam-rec says.
            // Otherwise when it can be pressed
            const std::string why = eyecam::searchText(t, s);
            if (s.locked && eyecam::pupilTrouble(s)) {
                para(t.setupWaitNoPupil, 15, kDanger, true, 32, 21, 2);
            } else if (!why.empty()) {
                // (with "Diagnostics" at the right, for someone who stays stuck there)
                const double chipH = 30;
                const double chipW = drawDiagChip(pen, t, x1, 0, chipH, 14, false);
                bool first = true;
                for (const std::string& line : wrapText(pen, why, 15, true, w - chipW - 14, 2)) {
                    y += first ? 32 : 21;
                    if (first && draw) drawDiagChip(pen, t, x1, y - 20, chipH, 14);
                    first = false;
                    text(x0, y, line, 15, kAccent, true);
                }
            } else {
                para(t.setupWaitFoot, 14, kTextMuted, false, 30, 20, 2);
            }
            break;
        }
        case SetupScreen::Learn: {
            title(learnName, t.setupLearnPill, kAccent);
            // The instruction, large, in the accent (and what it means, beside it, for widening)
            const eyecam::Step step = eyecam::parseStep(s.stepLabel);
            const std::string instruction = s.stepLabel.empty() ? t.eyecamCalibWaiting : eyecam::instruction(t, s.stepLabel);
            y += 58;
            const double size = fitSize(pen, instruction, 40, 22, w * 0.7, true);
            const double right = x0 + text(x0, y, instruction, size, kAccent, true);
            if (step == eyecam::Step::Widen) text(right + 18, y, t.setupLearnWidenHint, 17, kTextMuted, false);
            // The steps as chips (the calibration's protocol: a countdown, then these; none for another one)
            const std::vector<std::string> chips = eyecam::calibChips(s, calib);
            const int count = static_cast<int>(chips.size());
            if (count > 0) {
                y += 40;
                double cx = x0;
                for (int i = 0; i < count; ++i) {
                    const std::string label = eyecam::chipLabel(t, chips[i]);
                    const bool past = i < s.stepIndex - 1;
                    const bool now = i == s.stepIndex - 1;
                    const double chipW = draw ? drawStepChip(pen, cx, y, label, past, now)
                                              : stepChipWidth(pen, label, past, now);
                    if (draw && i + 1 < count) {
                        drawChevron(pen, cx + chipW + kStepChipGap / 2, y - 6, 10, 1.8, kBorder);
                    }
                    cx += chipW + kStepChipGap;
                }
            }
            // The seconds left of this step (or the call to put the headset back on), and the whole run
            const double boxTop = y + 18;
            const double boxH = 98;
            box(boxTop, boxH);
            if (draw) {
                const double bx = x0 + 18;
                const double br = x1 - 18;
                const double baseline = boxTop + 56;
                if (!s.locked) {
                    // (why, when eyecam-rec says)
                    const std::string why = eyecam::searchText(t, s);
                    const std::string call = why.empty() ? std::string(t.eyecamNotLocked) : why;
                    pen.text(bx, baseline, call, fitSize(pen, call, 26, 14, br - bx, true), kDanger, true);
                } else if (std::isfinite(s.stepRemainingS)) {
                    const int left = static_cast<int>(std::ceil(std::max(0.0, s.stepRemainingS) - 1e-9));
                    double lx = bx;
                    if (t.setupLeftBefore[0] != '\0') lx += pen.text(lx, baseline, t.setupLeftBefore, 17, kTextMuted) + 14;
                    lx += pen.text(lx, baseline, std::to_string(left), 40, kAccent, true) + 14;
                    pen.text(lx, baseline, t.setupLeftAfter, 17, kTextMuted);
                }
                if (s.stepCount > 1 && std::isfinite(s.totalS)) {
                    char line[160];
                    std::snprintf(line, sizeof(line), t.setupLearnStepFormat, std::max(1, s.stepIndex),
                                  s.stepCount - 1, static_cast<int>(std::lround(s.totalS)));
                    pen.text(br, baseline, line, 15, kTextMuted, false, true);
                }
                const double barY = boxTop + 70;
                fillRounded(pen, bx, barY, br - bx, 12, 6, kControl);
                if (std::isfinite(s.elapsedS) && std::isfinite(s.totalS) && s.totalS > 0) {
                    const double doneShare = std::clamp(s.elapsedS / s.totalS, 0.0, 1.0);
                    if (doneShare > 0) fillRounded(pen, bx, barY, std::max(12.0, (br - bx) * doneShare), 12, 6, kAccent);
                }
            }
            y = boxTop + boxH;
            const double by = y + 12;
            const double bh = 46;
            const double bw = std::max(96.0, pen.measure(t.setupStop, 18, true) + 44);
            if (draw) {
                pen.text(x0, by + 29, t.setupLearnFoot, fitSize(pen, t.setupLearnFoot, 15, 11, w - bw - 16, false),
                         kTextMuted);
                drawButton(pen, x1 - bw, by, bw, bh, t.setupStop, {PanelAction::EyecamStop, nullptr, 0}, !busy, false,
                           18);
            }
            y = by + bh;
            break;
        }
        case SetupScreen::Fail:
        case SetupScreen::Error: {
            const bool failed = screen == SetupScreen::Error;
            title(learnName, failed ? t.setupErrorPill : partial >= 0 ? t.partialPill : t.setupFailPill,
                  failed ? kDanger : partial >= 0 ? kAccent : kTextMuted);
            y += 50;
            const char* big = failed ? t.eyecamCalibErrorTitle : t.setupFailTitle;
            text(x0, y, big, fitSize(pen, big, 26, 16, w, true), failed ? kDanger : kText, true);
            if (failed) {
                // The eyes eyecam-rec's message names...
                const int eyes = eyecam::failedEyes(s);
                if (eyes != 0) {
                    y += 36;
                    double ex = x0 + text(x0, y, t.setupErrorEyes, 15, kTextMuted, true) + 18;
                    for (int eye = 0; eye < 2; ++eye) {
                        if ((eyes & (eye == 0 ? eyecam::kLeftEyeBit : eyecam::kRightEyeBit)) == 0) continue;
                        if (draw) drawEyeIcon(pen, ex + 10, y - 6, eye == 0 ? kDotLeft : kDotRight);
                        ex += 28 + text(ex + 28, y, eye == 0 ? t.leftEye : t.rightEye, 17, kDanger, true) + 22;
                    }
                }
                // ...and its reason with the counts (in English when it gives one), as large as fits in 4 lines
                const std::string& message = eyecam::shownMessage(s, m.language);
                if (!message.empty()) {
                    const double size = wrapSize(pen, message, 17, 14, false, w, 4);
                    para(message, size, kText, false, eyes != 0 ? 32 : 38, std::round(size * 1.5), 4);
                }
                // Its log was saved: where to find it
                if (!m.records.lastCalib.empty()) {
                    y += 16;
                    y += drawRecordBar(pen, t, m.records.lastCalib, true, x0, x1, y, draw) - 12;
                }
            } else {
                para(t.setupFailBody, 17, kText, false, 38, 26, 2);
                // ...and the eye that went on its earlier or provisional values
                if (partial >= 0) para(partialTitle(), 17, kAccent, true, 30, 24, 2);
                // What it learned (each eye, when one went on its earlier values), and the standard values for
                // widening
                const double boxTop = y + 18;
                const double boxH = 104;
                box(boxTop, boxH);
                const char* labels[3] = {t.setupFailClosed, t.setupFailNormal, t.setupFailWiden};
                if (partial >= 0) {
                    labels[0] = t.leftEye;
                    labels[1] = t.rightEye;
                }
                const char* provisional = eyecam::partialProvisional(s) ? t.partialProv : t.partialPrev;
                for (int i = 0; i < 3; ++i) {
                    const double baseline = boxTop + 30 + i * 30;
                    const char* value = i == 2 ? t.setupFailWidenValue
                                        : partial < 0 ? t.setupLearnDone
                                        : i == partial ? provisional
                                                       : t.partialNew;
                    const Color color = i == 2 ? kText : partial == i ? kAccent : kSuccess;
                    if (draw) {
                        if (i < 2 && partial >= 0) {
                            drawEyeIcon(pen, x0 + 28, baseline - 6, i == 0 ? kDotLeft : kDotRight);
                        } else if (i < 2) {
                            drawCheck(cr, x0 + 28, baseline - 6, 15, kSuccess);
                        } else {
                            pen.color(kTextMuted);
                            cairo_set_line_width(cr, 2);
                            cairo_move_to(cr, x0 + 22, baseline - 6);
                            cairo_line_to(cr, x0 + 34, baseline - 6);
                            cairo_stroke(cr);
                        }
                    }
                    text(x0 + 48, baseline, labels[i], 17, kTextMuted, false);
                    text(x0 + 178, baseline, value, 17, color, true);
                }
                y = boxTop + boxH;
            }
            const double by = y + (failed ? 22 : 12);
            const double bh = 46;
            double bx = x0;
            if (!failed) {
                const double pw = pen.measure(t.setupFailProceed, 18, true) + 48;
                if (draw) {
                    drawButton(pen, bx, by, pw, bh, t.setupFailProceed, {PanelAction::SetupProceed, nullptr, 0}, true,
                               true, 18);
                }
                bx += pw + 10;
            }
            const double aw = pen.measure(t.setupAgain, 18, true) + (failed ? 76 : 48);
            // A failed one: "Back" at the right, to the step's button (eyecam-rec stays in "error" until the next
            // command)
            const double backW = failed ? pen.measure(t.eyecamBack, 18, true) + 48 : 0;
            if (draw) {
                drawIconButton(pen, bx, by, aw, bh, t.setupAgain,
                               {PanelAction::EyecamCalib, nullptr, static_cast<int>(eyecam::Calib::Wear)}, !busy,
                               failed, 18, failed ? ButtonIcon::Play : ButtonIcon::None);
                if (failed) {
                    drawButton(pen, x1 - backW, by, backW, bh, t.eyecamBack, {PanelAction::EyecamBack, nullptr, 0},
                               true, false, 18);
                }
                const char* note = failed ? "" : t.setupFailLater;
                const double nx = bx + aw + 14;
                const double noteRight = failed ? x1 - backW - 14 : x1;
                if (note[0] != '\0') {
                    const std::vector<std::string> lines = wrapText(pen, note, 15, false, noteRight - nx, 2);
                    double baseline = by + bh / 2 + 5 - (lines.size() - 1) * 10;
                    for (const std::string& line : lines) {
                        pen.text(nx, baseline, line, 15, kTextMuted);
                        baseline += 20;
                    }
                }
            }
            y = by + bh;
            // A failed one, under its buttons: the video not there, or an eye's pupil not found (before "again")
            if (failed && !s.locked) {
                const std::string why = eyecam::searchText(t, s);
                para(why.empty() ? std::string(t.setupWaitFoot) : why, 14, kTextMuted, false, 30, 20, 2);
            } else if (failed && eyecam::pupilTrouble(s)) {
                para(t.setupWaitNoPupil, 15, kDanger, true, 32, 21, 2);
            }
            break;
        }
        case SetupScreen::Done: {
            y = top + 50;
            if (partial >= 0) {
                // Set up, but one eye on its earlier or provisional values: which, and the calibration again
                const std::string big = partialTitle();
                text(x0, y, big, fitSize(pen, big, 26, 16, w, true), kText, true);
                para(t.partialSetupBody, 17, kText, false, 40, 26, 2);
                partialBox();
                partialButtons(t.setupDoneButton);
                break;
            }
            text(x0, y, t.setupDoneTitle, fitSize(pen, t.setupDoneTitle, 26, 16, w, true), kText, true);
            para(t.setupDoneBody, 17, kText, false, 44, 28, 3);
            // If something is off later: where to go
            const double boxTop = y + 18;
            const double boxH = 112;
            box(boxTop, boxH);
            text(x0 + 18, boxTop + 28, t.setupDoneHelp, 14, kTextMuted, true);
            const double colX = x0 + 18 + std::max(240.0, std::max(pen.measure(t.setupDoneHelp1, 16, false),
                                                                    pen.measure(t.setupDoneHelp2, 16, false)) + 20);
            text(x0 + 18, boxTop + 60, t.setupDoneHelp1, 16, kTextMuted, false);
            text(colX, boxTop + 60, t.setupDoneHelp1Do, 16, kText, true);
            text(x0 + 18, boxTop + 90, t.setupDoneHelp2, 16, kTextMuted, false);
            text(colX, boxTop + 90, t.setupDoneHelp2Do, 16, kText, true);
            y = boxTop + boxH;
            const double by = y + 14;
            const double bh = 46;
            const double bw = pen.measure(t.setupDoneButton, 18, true) + 56;
            if (draw) {
                drawButton(pen, x0, by, bw, bh, t.setupDoneButton, {PanelAction::SetupProceed, nullptr, 0}, true, true,
                           18);
                const double nx = x0 + bw + 14;
                pen.text(nx, by + 29, t.setupDoneNote, fitSize(pen, t.setupDoneNote, 15, 11, x1 - nx, false),
                         kTextMuted);
            }
            y = by + bh;
            break;
        }
        case SetupScreen::Calibrated: {
            // A calibration from the page ended well: a line on what it learned, and back to the page
            const bool user = calib == eyecam::Calib::User;
            // (the user's with the standard look-down correction or squint on an eye, or the previous one kept)
            const eyecam::UserCalibCard card =
                user ? eyecam::userCalibCard(t, s) : eyecam::UserCalibCard{t.calibDonePill, t.calibDoneBody, false};
            title(learnName, partial >= 0 ? t.partialPill : card.pill,
                  partial >= 0 || card.fallback ? kAccent : kSuccess);
            y += 50;
            if (partial >= 0) {
                // This wear's without one eye: which, and the calibration again
                const std::string big = partialTitle();
                text(x0, y, big, fitSize(pen, big, 26, 16, w, true), kText, true);
                para(t.partialPageBody, 17, kText, false, 38, 26, 2);
                partialBox();
                partialButtons(t.calibDoneButton);
                break;
            }
            const char* big = user ? t.calibUserDoneTitle : t.calibDoneTitle;
            text(x0, y, big, fitSize(pen, big, 26, 16, w, true), kText, true);
            // (a body of two sentences or more, when a squint went on other values, gets a fourth line)
            const bool longer = card.body.find(t.userSentenceSep) != std::string::npos &&
                                (eyecam::squintDefaultEyes(s) | eyecam::squintPreviousEyes(s)) != 0;
            para(card.body, 17, kText, false, 38, 26, !card.fallback ? 2 : longer ? 4 : 3);
            const double by = y + 22;
            const double bh = 46;
            const double bw = std::max(120.0, pen.measure(t.calibDoneButton, 18, true) + 64);
            if (draw) {
                drawButton(pen, x0, by, bw, bh, t.calibDoneButton, {PanelAction::SetupProceed, nullptr, 0}, true, true,
                           18);
            }
            y = by + bh;
            break;
        }
        case SetupScreen::Camera: break;
    }
    // The setup's Konsole that didn't open
    if (!view.spawnError.empty() && (screen == SetupScreen::Pass || screen == SetupScreen::Check)) {
        y += 24;
        text(x0, y, ellipsize(pen, view.spawnError, 14, true, w, false), 14, kDanger, true);
    }
    return y - top + 22;
}

void EyePanel::drawWidenSlider(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v,
                               const WidenSlider& widen, double x0, double x1, double y, double h) {
    const eyecam::Status& es = m.eyecam.status;
    const bool camera = widen.control == WidenControl::Camera || widen.control == WidenControl::Mixed;
    const int level = widenState(v).mode;
    // The cameras' sensitivity (its own value while held, and after it is let go of until the file has it), or
    // lid_widen's level at its stop
    double value = widenStop(level);
    bool usable = false;
    if (camera) {
        const double file = std::isfinite(es.widenSensitivity) ? std::clamp(es.widenSensitivity, 0.0, 1.0) : 0.5;
        if (sensHeld_ && std::fabs(file - sensValue_) < 0.005) sensHeld_ = false;
        value = sensDragging_ || sensHeld_ ? sensValue_ : file;
        usable = m.eyecam.visible && es.hasWidenSensitivity;
    } else if (widen.control == WidenControl::Valve) {
        usable = !v.locked(key::kLidWiden);
    }
    // Widening off (widen): nothing it sets shows, so it is greyed like a slider that can't be moved
    if (!v.flag(key::kWiden)) usable = false;
    const double mid = y + h / 2;
    const double endSize = 15;
    const double dullW = pen.measure(t.eyecamSensitivityDull, endSize, false);
    const double sharpW = pen.measure(t.eyecamSensitivitySharp, endSize, false);
    // The value at the right end, in a fixed width so the track doesn't move with it: the number for the cameras,
    // the level's name for lid_widen
    std::string number;
    double valueW = pen.measure("0.00", 18, true);
    if (camera) {
        char text[16];
        std::snprintf(text, sizeof(text), "%.2f", value);
        number = text;
    } else {
        number = t.widenModes[std::clamp(level, 0, 3)];
        for (int i = 0; i < 4; ++i) valueW = std::max(valueW, pen.measure(t.widenModes[i], 18, true));
    }
    const Color ink = usable ? kText : kTextDisabled;
    pen.text(x1, centerBaseline(y, h, 18), number, 18, ink, true, true);
    pen.text(x0, centerBaseline(y, h, endSize), t.eyecamSensitivityDull, endSize, kTextMuted);
    const double sharpX = x1 - valueW - 22 - sharpW;
    pen.text(sharpX, centerBaseline(y, h, endSize), t.eyecamSensitivitySharp, endSize, kTextMuted);
    const double trackX = x0 + dullW + 22;
    const double trackW = sharpX - 22 - trackX;
    const double trackH = 10;
    fillRounded(pen, trackX, mid - trackH / 2, trackW, trackH, trackH / 2, kControl);
    strokeRounded(pen, trackX, mid - trackH / 2, trackW, trackH, trackH / 2, kBorder, 1.5);
    const Color fill = usable ? kAccent : kTextDisabled;
    if (value > 0) fillRounded(pen, trackX, mid - trackH / 2, std::max(trackH, trackW * value), trackH, trackH / 2, fill);
    // lid_widen's four stops, as ticks under the track
    if (!camera) {
        for (int i = 0; i < 4; ++i) drawDot(pen.cr, trackX + trackW * widenStop(i), mid + 18, 2.5, kBorder);
    }
    // The knob: larger while held or under the pointer, with a card-colored edge so it stands off the track
    const PanelHit sensHit {PanelAction::EyecamSensitivity, nullptr, 0};
    const bool active = usable && camera && (sensDragging_ || pointerState(sensHit) > 0);
    const double r = active ? 14 : 12;
    drawDot(pen.cr, trackX + trackW * value, mid, r + 3, kCard);
    drawDot(pen.cr, trackX + trackW * value, mid, r, fill);
    if (!usable) return;
    if (camera) {
        sensTrackX_ = trackX;
        sensTrackW_ = trackW;
        addButton(sensHit, trackX - 18, y, trackW + 36, h);
        return;
    }
    // lid_widen: a press anywhere goes to the nearest stop
    for (int i = 0; i < 4; ++i) {
        const double from = i == 0 ? trackX - 18 : trackX + trackW * (widenStop(i - 1) + widenStop(i)) / 2;
        const double to = i == 3 ? trackX + trackW + 18 : trackX + trackW * (widenStop(i) + widenStop(i + 1)) / 2;
        addButton({PanelAction::SetLidWiden, key::kLidWiden, i}, from, y, to - from, h);
    }
}

void EyePanel::drawNumberSlider(const Pen& pen, const char* name, double value, const char* low, const char* high,
                                double x0, double x1, double y, double h, bool usable) {
    const SettingSpec* spec = findSetting(name);
    const double lower = spec != nullptr ? spec->min : 0.0;
    const double upper = spec != nullptr ? spec->max : 1.0;
    const double share = std::isfinite(value) ? std::clamp((value - lower) / std::max(1e-9, upper - lower), 0.0, 1.0) : 0;
    const double mid = y + h / 2;
    const double endSize = 15;
    const double lowW = pen.measure(low, endSize, false);
    const double highW = pen.measure(high, endSize, false);
    const std::string number = formatSetting(name, value);
    const double valueW = pen.measure("0.00", 18, true);
    const Color ink = usable ? kText : kTextDisabled;
    pen.text(x1, centerBaseline(y, h, 18), number, 18, ink, true, true);
    pen.text(x0, centerBaseline(y, h, endSize), low, endSize, kTextMuted);
    const double highX = x1 - valueW - 22 - highW;
    pen.text(highX, centerBaseline(y, h, endSize), high, endSize, kTextMuted);
    const double trackX = x0 + lowW + 22;
    const double trackW = highX - 22 - trackX;
    const double trackH = 10;
    fillRounded(pen, trackX, mid - trackH / 2, trackW, trackH, trackH / 2, kControl);
    strokeRounded(pen, trackX, mid - trackH / 2, trackW, trackH, trackH / 2, kBorder, 1.5);
    const Color fill = usable ? kAccent : kTextDisabled;
    if (share > 0) fillRounded(pen, trackX, mid - trackH / 2, std::max(trackH, trackW * share), trackH, trackH / 2, fill);
    const PanelHit hit {PanelAction::NumberSlider, name, 0};
    const bool held = numDragging_ && numKey_ == name;
    const double r = usable && (held || pointerState(hit) > 0) ? 14 : 12;
    drawDot(pen.cr, trackX + trackW * share, mid, r + 3, kCard);
    drawDot(pen.cr, trackX + trackW * share, mid, r, fill);
    if (!usable) return;
    numTrack_[name] = {trackX, trackW};
    addButton(hit, trackX - 18, y, trackW + 36, h);
}

double EyePanel::numberShown(const char* name, double value) const {
    return numDragging_ && numKey_ == name ? numValue_ : value;
}

double EyePanel::numberAt(const std::string& name, double x) const {
    const auto track = numTrack_.find(name);
    const SettingSpec* spec = findSetting(name);
    if (track == numTrack_.end() || spec == nullptr) return NAN;
    const double share = std::clamp((x - track->second.first) / std::max(1.0, track->second.second), 0.0, 1.0);
    return snapValue(*spec, spec->min + share * (spec->max - spec->min));
}

void EyePanel::drawCameraPage(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    cairo_t* cr = pen.cr;
    const eyecam::View& view = m.eyecam;
    const eyecam::Status& s = view.status;
    const EyeStatus& es = m.status;
    const bool busy = view.busy;
    const bool ready = s.state == eyecam::State::Idle || s.state == eyecam::State::Error;
    double y = kRowTop - 8;
    // A thin line under each row
    const auto divider = [&](double at) {
        pen.color(kDivider);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, kInnerX, at + 0.5);
        cairo_line_to(cr, kInnerRight, at + 0.5);
        cairo_stroke(cr);
    };

    // The tool to install again (set up): outdated but working, or below the safety floor with the cameras stopped;
    // or to install while dsp_service doesn't answer (passwd first while SteamOS has none). The same Konsole as the
    // checklist's (2); it goes by itself once eyecam-rec stops saying so
    const eyecam::ToolNotice notice = eyecam::toolNotice(s, view.password);
    if (notice != eyecam::ToolNotice::None) {
        const bool tooOld = notice == eyecam::ToolNotice::TooOld;
        const bool dsp = notice == eyecam::ToolNotice::DspUnreachable;
        const bool passwd = dsp && view.password == eyecam::PasswordState::NotSet;
        const char* label = passwd ? t.setupPassButton : t.setupCheckButton;
        const char* text = passwd ? t.toolNoticeDspPassword
                           : dsp  ? t.toolNoticeDsp
                           : tooOld ? t.toolNoticeTooOld
                                    : t.toolNoticeOutdated;
        const Color edge = tooOld ? kDanger : kAccent;
        const double bh = 44;
        const double bw = pen.measure(label, 17, true) + 64;
        const double bx = kInnerRight - 14 - bw;
        const double textX = kInnerX + 52;
        const std::vector<std::string> lines = wrapText(pen, text, 16, true, bx - 16 - textX, 3);
        const bool failed = !view.spawnError.empty();
        const double textH = lines.size() * 22 + (failed ? 20 : 0);
        const double boxH = std::max(bh + 20, textH + 22);
        fillRounded(pen, kInnerX, y, kInnerRight - kInnerX, boxH, 14, tooOld ? kDangerTint : kAccentTint);
        strokeRounded(pen, kInnerX, y, kInnerRight - kInnerX, boxH, 14, edge, 2);
        drawInfoIcon(pen, kInnerX + 26, y + boxH / 2, edge);
        double baseline = y + (boxH - textH) / 2 + 16;
        for (const std::string& line : lines) {
            pen.text(textX, baseline, line, 16, kText, true);
            baseline += 22;
        }
        if (failed) {
            pen.text(textX, baseline - 2, ellipsize(pen, view.spawnError, 13, true, bx - 16 - textX, false), 13, kDanger,
                     true);
        }
        drawIconButton(pen, bx, y + (boxH - bh) / 2, bw, bh, label,
                       {PanelAction::SetupKonsole, nullptr, passwd ? setup_tools::kKonsolePasswd : setup_tools::kKonsoleInstall},
                       true, true, 17, ButtonIcon::Terminal);
        y += boxH + 6;
    }

    // Now: what drives the eyelids, and whether the relaxed eyes are learned (its line is also the bottom sentence's
    // last case)
    const bool warming = eyecam::baselineWarming(s);
    const CameraUse use = cameraUse(es, v.flag(key::kCameraLids), warming);
    std::string reason;
    {
        const bool inUse = use == CameraUse::Both || use == CameraUse::Left || use == CameraUse::Right;
        // The pupils going straight to VRChat (LiveLink) get a line under it
        const bool pupils = inUse && !es.pupilTarget.empty() && (es.camera.pupilUsed[0] || es.camera.pupilUsed[1]);
        const double h = pupils ? 80 : 64;
        const double lineH = pupils ? 52 : h;  // the state line's part of the row
        drawRowLabel(pen, t, y, h, t.camRowState, t.camRowStateHint, false);
        // (while learning, the pill says so: the line itself stays short)
        // (camera_lids off turns eyecam-rec's live off too: the line says the cameras are off, not why live is)
        std::string state = !s.live && use != CameraUse::Off ? std::string(t.eyecamLiveOff)
                            : use == CameraUse::Warming ? std::string(t.cameraUseValve)
                                                        : cameraUseText(t, use, es);
        reason = state;
        if (state.empty()) state = "—";
        // The pill: learning them now (with the seconds; not while no camera values come), or learned
        std::string pill;
        bool learned = false;
        if (warmingShown(warming, use)) {
            if (std::isfinite(s.warmupRemainingS)) {
                char left[128];
                std::snprintf(left, sizeof(left), t.eyecamWarmingFormat,
                              static_cast<int>(std::ceil(std::max(0.0, s.warmupRemainingS) - 1e-9)));
                pill = left;
            }
        } else if (learnedShown(!warming && (s.baseline == "ready" ||
                                             (s.calibState & (eyecam::kCalibWearBit | eyecam::kCalibAutoBit)) != 0),
                                s.live, use)) {
            pill = t.camLearned;
            learned = true;
        }
        // Laid out from the card's right edge: the pill's width first (at most a little over half the row), then the
        // line in what is left (smaller, then cut with "…"), so nothing goes past the card
        const double pillH = 32;
        const double textX = kControlX + 20;
        const double pillPad = learned ? 46 : 28;
        std::string pillText = pill;
        double pillSize = 15;
        if (!pill.empty()) {
            const double maxPillText = (kInnerRight - textX) * 0.55 - pillPad;
            pillSize = fitSize(pen, pill, 15, 11, maxPillText, false);
            pillText = ellipsize(pen, pill, pillSize, false, maxPillText, false);
        }
        const double pillW = pill.empty() ? 0 : pen.measure(pillText, pillSize, false) + pillPad;
        const double room = kInnerRight - textX - (pill.empty() ? 0 : pillW + 14);
        const double size = fitSize(pen, state, 19, 13, room, inUse);
        state = ellipsize(pen, state, size, inUse, room, false);
        drawDot(cr, kControlX + 6, y + lineH / 2, 5.5, inUse ? kSuccess : kTextMuted);
        const double textRight = textX + pen.text(textX, centerBaseline(y, lineH, size), state, size,
                                                  inUse ? kText : kTextMuted, inUse);
        if (pupils) {
            pen.text(textX, y + lineH + 14, t.camPupilLine,
                     fitSize(pen, t.camPupilLine, 15, 11, kInnerRight - textX, false), kTextMuted);
        }
        if (!pill.empty()) {
            // (right after the line; it was given room for that)
            const double px = std::min(textRight + 14, kInnerRight - pillW);
            const double py = y + (lineH - pillH) / 2;
            fillRounded(pen, px, py, pillW, pillH, pillH / 2, learned ? kSuccessTint : kAccentTint);
            double tx = px + 14;
            if (learned) {
                drawCheck(cr, px + 22, py + pillH / 2, 15, kSuccess);
                tx = px + 36;
            }
            pen.text(tx, centerBaseline(py, pillH, pillSize), pillText, pillSize, learned ? kSuccess : kText);
        }
        y += h;
        // Under it: learn the relaxed eyes again (eyecam-rec's "relearn": the warm-up as when the headset is put on,
        // this wear's calibration kept), while the cameras are read with the eye video; the pill then counts down
        // again. A line of its own, so the state and the pill's seconds keep their room
        {
            const double rh = 48;
            const double rbH = 38;
            const double rbW = pen.measure(t.camRelearnButton, 16, true) + 40;
            const double rbX = kInnerRight - rbW;
            const bool canRelearn = eyecam::relearnAllowed(view.visible, s, v.flag(key::kCameraLids), relearnUse(use));
            drawButton(pen, rbX, y + (rh - rbH) / 2 - 4, rbW, rbH, t.camRelearnButton,
                       {PanelAction::EyecamRelearn, nullptr, 0}, canRelearn && !busy, false, 16);
            const double room = rbX - 14 - textX;
            pen.text(textX, centerBaseline(y - 4, rh, 15), ellipsize(pen, t.camRelearnHint, 15, false, room, false), 15,
                     kTextMuted);
            y += rh;
        }
        divider(y);
    }
    // A calibration when something feels off, and the user's own (optional, for squinting)
    for (int i = 0; i < 2; ++i) {
        const bool user = i == 1;
        const double h = user ? 60 : 68;
        drawRowLabel(pen, t, y, h, user ? t.camRowUser : t.camRowCalib, user ? t.camRowUserHint : t.camRowCalibHint,
                     false);
        const double bw = 264;
        const double bh = user ? 42 : 46;
        // (camera_lids off stops eyecam-rec's processing too: nothing to calibrate then)
        const bool lidsOn = v.flag(key::kCameraLids);
        const bool allowed = lidsOn && (!user || eyecam::userCalibAllowed(s));
        drawButton(pen, kControlX, y + (h - bh) / 2, bw, bh, user ? t.camUserButton : t.camCalibButton,
                   {PanelAction::EyecamCalib, nullptr, static_cast<int>(user ? eyecam::Calib::User : eyecam::Calib::Wear)},
                   ready && allowed && !busy, false, user ? 17 : 18);
        const double sx = kControlX + bw + 16;
        const double sw = kInnerRight - sx;
        if (allowed) {
            const char* first = user ? t.camUserSide1 : t.camCalibSide1;
            const char* second = user ? t.camUserSide2 : t.camCalibSide2;
            pen.text(sx, y + h / 2 - 3, first, fitSize(pen, first, 15, 11, sw, false), kTextMuted);
            pen.text(sx, y + h / 2 + 17, second, fitSize(pen, second, 15, 11, sw, false), kTextMuted);
        } else {
            // Why not, on up to two lines like the hints beside the buttons ("\n" where the text breaks them itself)
            const std::string why = !lidsOn ? t.camCalibNeedsLids : t.camUserNeedsCalib;
            std::vector<std::string> lines;
            const size_t brk = why.find('\n');
            if (brk != std::string::npos) {
                for (const std::string& part : {why.substr(0, brk), why.substr(brk + 1)}) {
                    lines.push_back(ellipsize(pen, part, 15, false, sw, false));
                }
            } else {
                lines = wrapText(pen, why, 15, false, sw, 2);
            }
            double baseline = lines.size() > 1 ? y + h / 2 - 3 : centerBaseline(y, h, 15);
            for (const std::string& line : lines) {
                pen.text(sx, baseline, line, 15, kTextMuted);
                baseline += 20;
            }
        }
        y += h;
        if (!user) divider(y);
    }
    // Widening moved to the Eyelids tab (one slider, whatever drives the eyelids): a pointer there, to its "Look"
    {
        const double boxTop = y + 6;
        const double boxH = 56;
        fillRounded(pen, kInnerX, boxTop, kInnerRight - kInnerX, boxH, 12, kBg);
        drawEyeIcon(pen, kInnerX + 28, boxTop + boxH / 2, kAccent);
        const std::string label = std::string(t.camWidenButton) + "  " ICON_ARROW_RIGHT;
        const double bh = 42;
        const double bw = std::max(130.0, pen.measure(label, 17, true) + 44);
        const double bx = kInnerRight - 10 - bw;
        drawButton(pen, bx, boxTop + (boxH - bh) / 2, bw, bh, label,
                   {PanelAction::LidsPage, nullptr, static_cast<int>(LidsPage::Look)}, true, false, 17);
        const double tx = kInnerX + 50;
        pen.text(tx, centerBaseline(boxTop, boxH, 17), t.camWidenNotice,
                 fitSize(pen, t.camWidenNotice, 17, 11, bx - 14 - tx, false), kText);
        y = boxTop + boxH + 4;
    }

    // At the bottom: eyecam-rec's message, a command on its way, and a command that failed
    struct Line {
        std::string text;
        Color color;
        bool bold;
        bool link = false;  ///< "Diagnostics" at its right (the "why no video" line)
    };
    const double width = kInnerRight - kInnerX;
    std::vector<Line> lines;
    // Ready (idle): a sentence from the same things as the "Now" row, in place of eyecam-rec's own message; running
    // anything else: its message as it is
    std::string sentence = eyecam::shownMessage(s, m.language);
    bool whyLine = false;
    if (s.state == eyecam::State::Idle) {
        switch (cameraLine(use, warming, s.live, s.locked, v.text(key::kOutput) == kOutputVrchat)) {
            case CameraLine::BothVrchat:
                sentence = v.flag(key::kWiden) ? t.camLineBothVrchat : t.camLineBothNoWiden;
                break;
            case CameraLine::Both: sentence = v.flag(key::kWiden) ? t.camLineBoth : t.camLineBothNoWiden; break;
            case CameraLine::Left: sentence = t.camLineLeft; break;
            case CameraLine::Right: sentence = t.camLineRight; break;
            case CameraLine::Warming: {
                sentence = t.camLineWarming;
                if (std::isfinite(s.warmupRemainingS)) {
                    char text[200];
                    std::snprintf(text, sizeof(text), t.camLineWarmingFormat,
                                  static_cast<int>(std::ceil(std::max(0.0, s.warmupRemainingS) - 1e-9)));
                    sentence = text;
                }
                break;
            }
            case CameraLine::PutOn: {
                // (why the video isn't found, when eyecam-rec says; then with "Diagnostics" beside it)
                const std::string why = eyecam::searchText(t, s);
                sentence = why.empty() ? std::string(t.camLinePutOn) : why;
                whyLine = !why.empty();
                break;
            }
            case CameraLine::Off: sentence = t.camLineOff; break;
            case CameraLine::Reason: sentence = reason; break;
        }
    }
    const double linkH = 30;
    const double linkW = whyLine ? drawDiagChip(pen, t, kInnerRight, 0, linkH, 14, false) : 0.0;
    if (!sentence.empty()) {
        const double room = whyLine ? width - linkW - 14 : width;
        for (const std::string& line : wrapText(pen, sentence, 17, false, room, 2)) {
            lines.push_back({line, kText, false, whyLine});
        }
    }
    if (view.busy) {
        lines.push_back({t.eyecamSending, kTextMuted, false});
    } else if (view.hasReply && !view.reply.ok) {
        char failed[512];
        std::snprintf(failed, sizeof(failed), t.eyecamReplyFailedFormat, view.reply.command.c_str(),
                      view.reply.error.c_str());
        for (const std::string& line : wrapText(pen, failed, 17, true, width, 2)) lines.push_back({line, kDanger, true});
    }
    const double bottom = kContentY + kContentH - 14;
    double baseline = bottom;
    bool linked = false;
    for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
        // (the why line centered left of "Diagnostics")
        const double right = line->link ? kInnerRight - linkW - 14 : kInnerRight;
        textCentered(pen, (kInnerX + right) / 2, baseline, line->text, 17, line->color, line->bold);
        // (once, beside the why line's last line)
        if (line->link && !linked) {
            drawDiagChip(pen, t, kInnerRight, baseline - 21, linkH, 14);
            linked = true;
        }
        baseline -= 24;
    }

    // How the video is taken, then "When...": what to do about widening, in a box down to the lines above with a gap
    // of at least 10 px over their letters (rows that don't fit are left out, the box too if none does; the card goes
    // first). `baseline` is now one line above the top line's baseline: that line's letters start about 14 px above
    // its own baseline
    const double boxBottom = lines.empty() ? bottom + 2 : baseline + 24 - 14 - 10;
    const double cardH = drawSourceCard(pen, t, view, y + 8, boxBottom - (y + 8));
    if (cardH > 0) y += 8 + cardH;
    const double boxTop = y + 8;
    const double rowStep = 30;
    const int rows = helpRows(boxTop, boxBottom);
    if (rows < 1) return;
    const double boxH = helpBoxHeight(rows);
    fillRounded(pen, kInnerX, boxTop, width, boxH, 12, kBg);
    drawInfoIcon(pen, kInnerX + 28, boxTop + 24, kAccent);
    pen.text(kInnerX + 46, boxTop + 30, t.camHelpTitle, 17, kText, true);
    const char* situations[3] = {t.camHelp1, t.camHelp2, t.camHelp3};
    const char* remedies[3] = {t.camHelp1Do, t.camHelp2Do, t.camHelp3Do};
    double colW = 0;
    for (const char* situation : situations) colW = std::max(colW, pen.measure(situation, 16, false));
    const double colX = kInnerX + 20 + std::max(250.0, colW + 24);
    for (int i = 0; i < rows; ++i) {
        const double rowBaseline = boxTop + 62 + i * rowStep;
        pen.text(kInnerX + 20, rowBaseline, situations[i], 16, kTextMuted);
        pen.text(colX, rowBaseline, remedies[i], fitSize(pen, remedies[i], 16, 11, kInnerRight - 16 - colX, false), kText);
    }
}

double EyePanel::drawSourceCard(const Pen& pen, const UiText& t, const eyecam::View& view, double top, double room) {
    using eyecam::GrabTool;
    using eyecam::SourceChoice;
    const eyecam::Status& s = view.status;
    if (!eyecam::sourceCardShown(s)) return 0;
    cairo_t* cr = pen.cr;
    const bool choice = eyecam::sourceChoiceShown(s);
    const double x0 = kInnerX;
    const double x1 = kInnerRight;
    const double textX = x0 + 50;
    const double headH = 58;
    const double toolH = choice ? 52 : 0;
    const double lineH = 19;
    const GrabTool tool = eyecam::grabTool(s);
    const bool removable = eyecam::grabRemoveAllowed(s);
    // (no SteamOS password: sudo would fail, so passwd first, as the checklist's (1))
    const bool passwordFirst = choice && eyecam::installNeedsPassword(view);

    // Under the rows: a password to set first, why "Remove" can't be pressed, or that auto takes the tool later (or
    // Konsole's failure), then the trade-off. Left out from the bottom up while the card doesn't fit
    std::string note;
    Color noteColor = kText;
    if (choice && !view.spawnError.empty()) {
        note = view.spawnError;
        noteColor = kDanger;
    } else if (passwordFirst) {
        note = t.srcNeedsPassword;
    } else if (choice && tool != GrabTool::Missing && !removable) {
        note = t.srcRemoveNeedsDsp;
    } else if (choice && eyecam::grabAfterReacquire(s)) {
        note = t.srcSwitchLater;
    }
    const double wrapW = x1 - x0 - 32;
    std::vector<std::string> noteLines = note.empty() ? std::vector<std::string>{}
                                                      : wrapText(pen, note, 15, false, wrapW, 2);
    std::vector<std::string> tradeLines = wrapText(pen, t.srcTradeoff, 14, false, wrapW, 2);
    const auto height = [&] {
        const double lines = static_cast<double>(noteLines.size() + tradeLines.size());
        return 8 + headH + toolH + lines * lineH + (lines > 0 ? 12 : 2);
    };
    if (height() > room) tradeLines.clear();
    if (height() > room) noteLines.clear();
    const double h = height();
    fillRounded(pen, x0, top, x1 - x0, h, 12, kBg);

    // Its title and what is in use (the buffers held, or being taken), the choice at the right
    double y = top + 8;
    const double segW = 300;
    const double segH = 42;
    const double segX = x1 - 10 - segW;
    const double textRight = (choice ? segX : x1) - 14;
    drawCameraIcon(pen, x0 + 28, y + 20, kAccent);
    pen.text(textX, y + 24, t.srcTitle, fitSize(pen, t.srcTitle, 18, 13, textRight - textX, true), kText, true);
    const bool dsp = s.source == "dsp";
    const std::string now = s.hasBuffers ? (dsp ? t.srcNowDsp : t.srcNowGrab) : (dsp ? t.srcWaitDsp : t.srcWaitGrab);
    drawDot(cr, textX + 5, y + 43, 4.5, s.hasBuffers ? kSuccess : kTextMuted);
    pen.text(textX + 16, y + 48, ellipsize(pen, now, 15, false, textRight - textX - 16, false), 15,
             s.hasBuffers ? kText : kTextMuted);
    if (choice) {
        // (eyecam-rec refuses it while recording or calibrating: shown as it is, not pressable)
        const bool running = s.state == eyecam::State::Searching || s.state == eyecam::State::Recording ||
                             s.state == eyecam::State::Calibrating;
        std::vector<Option> options;
        for (const auto& [label, value] : {std::pair<const char*, SourceChoice>{t.srcAuto, SourceChoice::Auto},
                                           {t.srcDsp, SourceChoice::Dsp},
                                           {t.srcGrab, SourceChoice::Grab}}) {
            options.push_back({label, {PanelAction::EyecamSource, nullptr, static_cast<int>(value)},
                               eyecam::sourceChoiceAllowed(view, value)});
        }
        drawSegmented(pen, segX, y + (headH - segH) / 2, segW, segH, options, static_cast<int>(eyecam::sourceChoice(s)),
                      16, running);
    }
    y += headH;

    // The tool: installed or not, and its Konsole ("Install" with its sha256 first, as the checklist's (2), or passwd
    // while SteamOS has no password; "Remove" only while the buffers come from dsp_service)
    if (choice) {
        const bool missing = tool == GrabTool::Missing;
        const std::string label = passwordFirst ? t.setupPassButton : missing ? t.srcInstall : t.srcRemove;
        const double bh = 42;
        const double bw = std::min(380.0, pen.measure(label, 16, true) + 64);
        const double bx = x1 - 10 - bw;
        drawIconButton(pen, bx, y + (toolH - bh) / 2, bw, bh, label,
                       {PanelAction::SetupKonsole, nullptr,
                        passwordFirst ? setup_tools::kKonsolePasswd
                        : missing     ? setup_tools::kKonsoleInstall
                                      : setup_tools::kKonsoleUninstall},
                       missing || removable, false, 16, ButtonIcon::Terminal);
        const std::string state = missing ? t.srcToolOut : tool == GrabTool::Ready ? t.srcToolIn : t.srcToolBroken;
        const double room = bx - 14 - (x0 + 16);
        const double size = fitSize(pen, state, 16, 12, room, false);
        pen.text(x0 + 16, centerBaseline(y, toolH, size), ellipsize(pen, state, size, false, room, false), size, kText);
        y += toolH;
    }
    y += 4;
    for (const std::string& line : noteLines) {
        pen.text(x0 + 16, y + 14, line, 15, noteColor);
        y += lineH;
    }
    for (const std::string& line : tradeLines) {
        pen.text(x0 + 16, y + 14, line, 14, kTextMuted);
        y += lineH;
    }
    return h;
}

void EyePanel::drawEyecamConfirm(const Pen& pen, const UiText& t, const eyecam::View& view, double y) {
    using eyecam::StartChoice;
    const double cx = (kInnerX + kInnerRight) / 2;
    const double width = kInnerRight - kInnerX;

    // The title in red, after a red warning sign (a triangle with "!"), centered together
    const std::string title = t.eyecamLightTitle;
    const double titleSize = fitSize(pen, title, 36, 20, width - 80, true);
    const double sign = 42;
    const double signH = sign * 0.88;
    const double gap = 14;
    const double left = cx - (sign + gap + pen.measure(title, titleSize, true)) / 2;
    const double titleBaseline = y + 56;
    const double signTop = titleBaseline - titleSize * 0.36 - signH / 2 - 2;
    pen.color(kDanger);
    cairo_new_path(pen.cr);
    cairo_move_to(pen.cr, left + sign / 2, signTop);
    cairo_line_to(pen.cr, left + sign, signTop + signH);
    cairo_line_to(pen.cr, left, signTop + signH);
    cairo_close_path(pen.cr);
    cairo_fill(pen.cr);
    textCentered(pen, left + sign / 2, signTop + signH - 6, "!", 26, kOnAccent, true);
    pen.text(left + sign + gap, titleBaseline, title, titleSize, kDanger, true);

    // The warning in a red box: "\n" breaks a line, "\n\n" starts a paragraph, and each line wraps if it must
    const double textSize = 22;
    const double lineH = textSize * 1.42;
    const double padX = 24;
    const double padY = 20;
    std::vector<std::string> lines;
    std::vector<bool> paragraphStart;  // a line that starts a paragraph after the first gets a little more space
    const std::string text = t.eyecamLightWarning;
    bool newParagraph = false;
    size_t from = 0;
    while (from <= text.size()) {
        const size_t end = std::min(text.find('\n', from), text.size());
        if (end == from) newParagraph = true;
        const std::vector<std::string> wrapped = wrapText(pen, text.substr(from, end - from), textSize, false,
                                                          width - padX * 2, 4);
        for (size_t i = 0; i < wrapped.size(); ++i) {
            paragraphStart.push_back(i == 0 && newParagraph && !lines.empty());
            lines.push_back(wrapped[i]);
            newParagraph = false;
        }
        from = end + 1;
    }
    const double paragraphGap = 8;
    double textH = textSize;
    for (size_t i = 1; i < lines.size(); ++i) textH += lineH + (paragraphStart[i] ? paragraphGap : 0);
    const double boxY = y + 84;
    const double boxH = padY * 2 + textH + 4;
    fillRounded(pen, kInnerX, boxY, width, boxH, 16, kDangerTint);
    strokeRounded(pen, kInnerX, boxY, width, boxH, 16, kDanger, 2);
    double baseline = boxY + padY + textSize;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) baseline += lineH + (paragraphStart[i] ? paragraphGap : 0);
        pen.text(kInnerX + padX, baseline, lines[i], textSize, kText);
    }

    // Where the video goes and how big it is (it stays until the user deletes it)
    const double noteSize = 17;
    double noteBaseline = boxY + boxH + 10;
    for (const std::string& line : wrapText(pen, t.eyecamStorageNote, noteSize, false, width, 2)) {
        noteBaseline += noteSize * 1.4;
        pen.text(kInnerX + 4, noteBaseline, line, noteSize, kText);
    }

    // With the light, without it (side by side, the same weight: neither is the "default"), and cancel under them
    const bool usable = !view.busy;
    const double rowY = noteBaseline + 18;
    const double gapX = 20;
    const double bw = (width - gapX) / 2;
    const double bh = 76;
    drawButton(pen, kInnerX, rowY, bw, bh, t.eyecamStartWithLight,
               {PanelAction::EyecamChoose, nullptr, static_cast<int>(StartChoice::WithLight)}, usable, false, 28);
    drawButton(pen, kInnerX + bw + gapX, rowY, bw, bh, t.eyecamStartNoLight,
               {PanelAction::EyecamChoose, nullptr, static_cast<int>(StartChoice::WithoutLight)}, usable, false, 28);
    const double cw = 240;
    drawButton(pen, cx - cw / 2, rowY + bh + 16, cw, 56, t.eyecamCancel,
               {PanelAction::EyecamChoose, nullptr, static_cast<int>(StartChoice::Cancel)}, true, false, 24);
}

void EyePanel::drawContrib(const Pen& pen, const UiText& t, const PanelModel& m) {
    using contrib::Screen;
    const contrib::View& c = m.contrib;
    const Screen screen = contrib::screen(c, contribPage_, contribAgreed_);
    // The recording: the eye capture's run, titled with it (its "Stop" as always); until eyecam-rec shows it running
    // (or after it ended, until the loop has followed that), only that it is on its way
    if (screen == Screen::Running) {
        const eyecam::State state = m.eyecam.status.state;
        if (state == eyecam::State::Searching || state == eyecam::State::Recording) {
            drawRun(pen, t, m, t.contribRow);
        } else {
            drawSectionTitle(pen, kRowTop - 6, t.contribRow);
            textCentered(pen, (kInnerX + kInnerRight) / 2, kContentY + kContentH / 2, t.eyecamSending, 28, kTextMuted,
                         true);
        }
        return;
    }
    const double width = kInnerRight - kInnerX;
    const double bottom = kContentY + kContentH - 20;
    const bool jobRunning = c.progress.running;
    const int count = static_cast<int>(c.info.recordings.size());
    // (Valve's samples, which the labels need, only come while frameeyeosc runs and gets eye data)
    const bool eyeData = m.status.running && m.status.tracking;

    // The title and the cross
    const double closeS = 52;
    const double closeX = kInnerRight - closeS;
    const double headY = kContentY + 20;
    drawSheetClose(pen, {PanelAction::ContribClose, nullptr, 0}, closeX, headY + 2, closeS);
    const double titleRight = closeX - 16;
    pen.text(kInnerX, headY + 26, t.contribRow, fitSize(pen, t.contribRow, 22, 16, titleRight - kInnerX, true), kText,
             true);
    pen.text(kInnerX, headY + 50, ellipsize(pen, t.contribAbout, 14, false, titleRight - kInnerX, false), 14,
             kTextMuted);
    if (contribConfirm_) {
        drawContribDelete(pen, t, m);
        return;
    }
    if (screen == Screen::Deleting) {
        textCentered(pen, (kInnerX + kInnerRight) / 2, kContentY + kContentH / 2, t.contribDeleting, 28, kText, true);
        return;
    }

    // Lines from the top down: wrapped, as many as fit in maxLines
    double y = headY + 70;
    const auto paragraph = [&](const std::string& text, double size, Color color, bool bold, size_t maxLines,
                               double x = kInnerX) {
        for (const std::string& line : wrapText(pen, text, size, bold, kInnerRight - x, maxLines)) {
            pen.text(x, y + size, line, size, color, bold);
            y += size * 1.4;
        }
    };
    // How to send it, in a box with the warning's edge: a private share link, never anywhere public
    const auto sendNote = [&](double size) {
        const double pad = 14;
        const std::vector<std::string> lines = wrapText(pen, t.contribSendNote, size, false, width - pad * 2 - 30, 3);
        const double boxH = pad * 2 + lines.size() * size * 1.4 - size * 0.4;
        fillRounded(pen, kInnerX, y, width, boxH, 12, kWarningTint);
        strokeRounded(pen, kInnerX, y, width, boxH, 12, kWarning, 1.5);
        drawInfoIcon(pen, kInnerX + pad + 8, y + pad + size * 0.55, kWarning);
        double baseline = y + pad + size * 0.85;
        for (const std::string& line : lines) {
            pen.text(kInnerX + pad + 30, baseline, line, size, kWarningText);
            baseline += size * 1.4;
        }
        y += boxH;
    };
    // A failure on the disk (info.txt, a folder), above everything else
    if (!c.error.empty()) paragraph(formatLong(t.contribErrorFormat, c.error), 15, kDanger, true, 2);
    // At the bottom right: the screen's main button
    const double mainH = 60;
    const double mainY = bottom - mainH;
    const auto mainButton = [&](const std::string& label, const PanelHit& hit, bool usable, bool accent) {
        const double w = std::max(300.0, pen.measure(label, 22, true) + 64);
        drawButton(pen, kInnerRight - w, mainY, w, mainH, label, hit, usable, accent, 22);
        return kInnerRight - w;
    };
    // Above it: why "Start" can't be pressed (no room, eyecam-rec busy), else the free space
    const auto startState = [&]() {
        if (!contrib::fits(c.freeBytes, c.fps)) {
            const long long need = contrib::estimateBytes(c.fps) + contrib::kMarginBytes;
            pen.text(kInnerX, mainY - 16, formatLong(t.contribNoSpaceFormat, contrib::sizeText(need)), 16, kDanger,
                     true);
        } else if (!eyeData) {
            pen.text(kInnerX, mainY - 16, ellipsize(pen, t.contribNoEyeData, 16, false, width, false), 16, kDanger,
                     true);
        } else if (!contrib::startAllowed(c, m.eyecam, eyeData)) {
            pen.text(kInnerX, mainY - 16, ellipsize(pen, t.contribWaitEyecam, 16, false, width, false), 16,
                     kTextMuted);
        } else if (c.freeBytes >= 0) {
            const std::string line = formatLong(t.contribSpaceFormat, contrib::sizeText(c.freeBytes),
                                                contrib::sizeText(contrib::estimateBytes(c.fps)));
            pen.text(kInnerX, mainY - 16, ellipsize(pen, line, 16, false, width, false), 16, kTextMuted);
        }
    };
    const auto countLine = [&]() {
        char text[256];
        std::snprintf(text, sizeof(text), t.contribCountFormat, count,
                      contrib::sizeText(contrib::recordedBytes(c.info)).c_str());
        return std::string(text);
    };

    switch (screen) {
        case Screen::Consent: {
            if (c.deleted) {
                drawCheck(pen.cr, kInnerX + 9, y + 10, 16, kSuccess);
                pen.text(kInnerX + 26, y + 16, t.contribDeleted, 16, kSuccess, true);
                y += 30;
            }
            paragraph(t.contribLead, 18, kText, false, 2);
            y += 8;
            // What, how it's used, removing it: a title at the left, its text beside it
            const std::string blocks[3][2] = {
                {t.contribWhatTitle, formatLong(t.contribWhatFormat, contrib::sizeText(contrib::estimateBytes(c.fps)))},
                {t.contribUseTitle, t.contribUse},
                {t.contribRemoveTitle, t.contribRemove},
            };
            double labelW = 0;
            for (const auto& block : blocks) labelW = std::max(labelW, pen.measure(block[0], 16, true));
            const double textX = kInnerX + labelW + 24;
            for (const auto& block : blocks) {
                fillRounded(pen, kInnerX - 2, y + 4, 4, 18, 2, kAccent);
                pen.text(kInnerX + 10, y + 18, block[0], 16, kText, true);
                const double top = y;
                paragraph(block[1], 16, kText, false, 3, textX);
                y = std::max(y, top + 26) + 6;
            }
            sendNote(16);
            y += 8;
            paragraph(t.contribNote, 15, kTextMuted, false, 1);
            const double agreeX = mainButton(t.contribAgree, {PanelAction::ContribAgree, nullptr, 1},
                                             eyecam::setUp(m.eyecam), true);
            const double cancelW = std::max(140.0, pen.measure(t.contribCancel, 20, true) + 48);
            drawButton(pen, agreeX - 14 - cancelW, mainY, cancelW, mainH, t.contribCancel,
                       {PanelAction::ContribClose, nullptr, 0}, true, false, 20);
            break;
        }
        case Screen::Eyewear: {
            y += 10;
            paragraph(t.contribEyewearTitle, 24, kText, true, 2);
            y += 4;
            paragraph(t.contribEyewearWhy, 16, kTextMuted, false, 2);
            y += 20;
            // The four answers, two to a row, the same weight (none is the default)
            const double gap = 16;
            const double bw = (width - gap) / 2;
            const double bh = 64;
            for (int i = 0; i < 4; ++i) {
                const double bx = kInnerX + (i % 2) * (bw + gap);
                const double by = y + (i / 2) * (bh + gap);
                drawButton(pen, bx, by, bw, bh, t.contribEyewear[i],
                           {PanelAction::ContribEyewear, nullptr, static_cast<int>(contrib::Eyewear::None) + i}, true,
                           false, 20);
            }
            const double backW = std::max(140.0, pen.measure(t.contribBack, 20, true) + 48);
            drawButton(pen, kInnerX, mainY, backW, mainH, t.contribBack, {PanelAction::ContribAgree, nullptr, 0}, true,
                       false, 20);
            break;
        }
        case Screen::Ready: {
            paragraph(t.contribReadyTitle, 24, kText, true, 1);
            y += 6;
            for (const char* tip : t.contribTips) {
                drawDot(pen.cr, kInnerX + 7, y + 13, 4, kAccent);
                paragraph(tip, 18, kText, false, 2, kInnerX + 22);
                y += 3;
            }
            if (count > 0) {
                y += 6;
                paragraph(countLine(), 16, kTextMuted, false, 1);
            }
            startState();
            mainButton(t.contribStart, {PanelAction::ContribStart, nullptr, 0},
                       contrib::startAllowed(c, m.eyecam, eyeData), true);
            break;
        }
        case Screen::Failed: {
            paragraph(t.contribFailedTitle, 26, kDanger, true, 1);
            y += 6;
            const contrib::Failure& f = c.failure;
            const std::string& message = m.language == Language::En && !f.messageEn.empty() ? f.messageEn : f.message;
            std::string why;
            switch (f.why) {
                case contrib::Why::None:
                case contrib::Why::Stopped: why = t.contribWhyStopped; break;
                case contrib::Why::Error: why = formatLong(t.contribWhyErrorFormat, message); break;
                case contrib::Why::Refused: why = formatLong(t.contribWhyRefusedFormat, message); break;
                case contrib::Why::Gone: why = t.contribWhyGone; break;
                case contrib::Why::NoStart: why = t.contribWhyNoStart; break;
                case contrib::Why::Missing: why = t.contribWhyMissing; break;
                case contrib::Why::Move: why = formatLong(t.contribWhyMoveFormat, message); break;
                case contrib::Why::Incomplete: why = formatLong(t.contribWhyIncompleteFormat, message); break;
                case contrib::Why::NoVideo:
                    why = formatLong(t.contribWhyNoVideoFormat, message == "L"   ? t.leftEye
                                                                : message == "R" ? t.rightEye
                                                                                 : t.contribBothEyes);
                    break;
                case contrib::Why::Uneven: {
                    // ("8490/2124": the left eye's frames, then the right eye's)
                    const size_t slash = message.find('/');
                    why = formatLong(t.contribWhyUnevenFormat, message.substr(0, slash),
                                     slash == std::string::npos ? std::string() : message.substr(slash + 1));
                    break;
                }
                case contrib::Why::Truncated: why = formatLong(t.contribWhyTruncatedFormat, message); break;
                case contrib::Why::Valve: why = formatLong(t.contribWhyValveFormat, message); break;
            }
            paragraph(why, 19, kText, false, 3);
            if (!f.kept.empty()) {
                y += 4;
                paragraph(t.contribKept, 16, kTextMuted, false, 2);
            }
            startState();
            mainButton(t.contribRetry, {PanelAction::ContribStart, nullptr, 0},
                       contrib::startAllowed(c, m.eyecam, eyeData), true);
            break;
        }
        case Screen::Done: {
            const bool done = contribPage_ == contrib::Page::Record;
            if (done) {
                drawCheck(pen.cr, kInnerX + 14, y + 16, 24, kSuccess);
                pen.text(kInnerX + 38, y + 26, t.contribDoneTitle,
                         fitSize(pen, t.contribDoneTitle, 26, 18, width - 38, true), kText, true);
            } else {
                pen.text(kInnerX, y + 26, t.contribManageTitle, 26, kText, true);
            }
            y += 44;
            paragraph(countLine(), 17, kText, true, 1);
            pen.text(kInnerX, y + 15,
                     ellipsize(pen, formatLong(t.contribWhereFormat, recorder::shortPath(c.dir) + "/"), 15, false,
                               width, true),
                     15, kTextMuted);
            y += 30;
            if (done) {
                paragraph(t.contribAgain, 16, kText, false, 2);
                y += 4;
            }
            sendNote(15);
            y += 12;
            // "Make one file", and beside it how that stands (again once there are more recordings than it holds)
            const contrib::Progress& p = c.progress;
            const bool packing = p.running && p.job == contrib::Job::Pack;
            const long long packNeed = contrib::packNeedBytes(c.info, c.compress);
            const bool room = c.freeBytes < 0 || c.freeBytes >= packNeed;
            const bool packedAll = !c.info.packed.empty() && c.info.packedCount >= count;
            const double packW = std::max(280.0, pen.measure(t.contribPack, 19, true) + 56);
            const double packH = 54;
            drawButton(pen, kInnerX, y, packW, packH, t.contribPack, {PanelAction::ContribPack, nullptr, 0},
                       !jobRunning && room && count > 0 && !packedAll && !c.running, true, 19);
            const double sx = kInnerX + packW + 18;
            const double sw = kInnerRight - sx;
            std::string state;
            Color color = kTextMuted;
            bool bold = false;
            if (packing) {
                const double share = p.total > 0 ? std::clamp(static_cast<double>(p.done) / p.total, 0.0, 1.0) : 0.0;
                char text[128];
                std::snprintf(text, sizeof(text), t.contribPackingFormat, static_cast<int>(share * 100));
                state = text;
                color = kText;
                // (a bar under the line)
                const double barY = y + packH - 16;
                fillRounded(pen, sx, barY, sw, 12, 6, kControl);
                if (share > 0) fillRounded(pen, sx, barY, std::max(12.0, sw * share), 12, 6, kAccent);
            } else if (p.job == contrib::Job::Pack && p.finished && !p.ok) {
                state = formatLong(t.contribPackFailedFormat, p.error);
                color = kDanger;
                bold = true;
            } else if (packedAll) {
                // (done, and under it the file's name, cut at its start if it must be)
                const std::string packed = formatLong(t.contribPackedFormat, std::to_string(c.info.packedCount),
                                                      contrib::sizeText(c.info.packedBytes));
                pen.text(sx, y + packH / 2 - 4, packed, 16, kSuccess, true);
                pen.text(sx, y + packH / 2 + 18, ellipsize(pen, c.info.packed, 15, false, sw, true), 15, kText);
            } else if (!room) {
                state = formatLong(t.contribPackNoSpaceFormat, contrib::sizeText(packNeed));
                color = kDanger;
                bold = true;
            } else if (count > 0) {
                state = formatLong(t.contribPackHintFormat, contrib::sizeText(packNeed));
            }
            const std::vector<std::string> lines = wrapText(pen, state, 15, bold, sw, packing ? 1 : 2);
            double baseline = packing ? y + 22 : y + packH / 2 + 5 - (static_cast<double>(lines.size()) - 1) * 10;
            for (const std::string& line : lines) {
                pen.text(sx, baseline, line, 15, color, bold);
                baseline += 20;
            }
            y += packH + 8;
            // (what the file is: zstd's, which Windows doesn't open by itself)
            if (c.compress) paragraph(t.contribPackFormatNote, 14, kTextMuted, false, 1);
            // Recordings the pack left out (a file missing): to record again
            if (!c.incomplete.empty()) {
                std::string names;
                for (const std::string& name : c.incomplete) names += (names.empty() ? "" : ", ") + name;
                paragraph(formatLong(t.contribIncompleteFormat, names), 15, kDanger, true, 2);
            }
            // At the bottom: deleting them (after sending), and "Close"
            const double deleteW = std::max(200.0, pen.measure(t.contribDelete, 18, true) + 48);
            drawButton(pen, kInnerX, bottom - 52, deleteW, 52, t.contribDelete,
                       {PanelAction::ContribDelete, nullptr, 0}, !jobRunning && !c.running && c.exists, false, 18);
            const double closeW = std::max(160.0, pen.measure(t.contribClose, 18, true) + 48);
            drawButton(pen, kInnerRight - closeW, bottom - 52, closeW, 52, t.contribClose,
                       {PanelAction::ContribClose, nullptr, 0}, true, false, 18);
            break;
        }
        case Screen::Running:
        case Screen::Deleting: break;
    }
}

void EyePanel::drawContribDelete(const Pen& pen, const UiText& t, const PanelModel& m) {
    const contrib::View& c = m.contrib;
    // A card in the middle, as the other confirmations
    const double w = kRight - kRightX - 120;
    const double x = kRightX + 60;
    const double ix = x + 28;
    const double innerW = w - 56;
    const std::string body = formatLong(t.contribDeleteFormat, recorder::shortPath(c.dir) + "/",
                                        contrib::sizeText(c.totalBytes));
    const std::vector<std::string> lines = wrapText(pen, body, 17, false, innerW, 4);
    const double h = 26 + 32 + 14 + lines.size() * 26.0 + 22 + kControlH + 26;
    double y = kContentY + 80 + std::max(0.0, (kContentH - 80 - h) / 2);
    drawCard(pen, x, y, w, h, 20, kCard, kDanger, 2);
    y += 26;
    pen.text(ix, y + 24, t.contribDeleteTitle, fitSize(pen, t.contribDeleteTitle, 24, 16, innerW, true), kText, true);
    y += 32 + 14;
    for (const std::string& line : lines) {
        pen.text(ix, y + 17, line, 17, kText);
        y += 26;
    }
    y += 22;
    const double yesW = std::max(160.0, pen.measure(t.contribDeleteYes, 18, true) + 56);
    const double yesX = ix + innerW - yesW;
    drawButton(pen, yesX, y, yesW, kControlH, t.contribDeleteYes, {PanelAction::ContribDeleteConfirm, nullptr, 0},
               !c.progress.running && !c.running, false, 18);
    const double noW = std::max(140.0, pen.measure(t.contribCancel, 18, true) + 48);
    drawButton(pen, yesX - 12 - noW, y, noW, kControlH, t.contribCancel, {PanelAction::ContribDeleteCancel, nullptr, 0},
               true, true, 18);
}

void EyePanel::drawPreviewButton(const Pen& pen, const UiText& t, double x, double y, double w, double h, bool label) {
    const PanelHit hit {PanelAction::PreviewOpen, nullptr, 0};
    const int pointer = pointerState(hit);
    // Open: the accent tint (pressed again, it closes)
    const double r = label ? h / 2 : 14;
    fillRounded(pen, x, y, w, h, r, previewOpen_ ? kAccentTint : (pointer > 0 ? kControlHover : kControl));
    strokeRounded(pen, x, y, w, h, r, kAccent, 2);
    if (label) {
        const double size = fitSize(pen, t.previewButton, 17, 12, w - 70, true);
        const double contentW = 30 + pen.measure(t.previewButton, size, true);
        const double left = x + (w - contentW) / 2;
        drawEyeIcon(pen, left + 11, y + h / 2, kAccent);
        pen.text(left + 30, centerBaseline(y, h, size), t.previewButton, size, kText, true);
    } else {
        drawEyeIcon(pen, x + w / 2, y + h / 2, kAccent);
    }
    addButton(hit, x, y, w, h);
}

void EyePanel::drawToggle(const Pen& pen, double x, double y, double w, double h, const std::string& label,
                          const PanelHit& hit, bool on, bool usable) {
    const int pointer = usable ? pointerState(hit) : 0;
    const Color edge = on ? kAccent : (usable ? kBorder : kDivider);
    fillRounded(pen, x, y, w, h, h / 2, on ? kAccentTint : (pointer > 0 ? kControlHover : kControl));
    strokeRounded(pen, x, y, w, h, h / 2, edge, 2);
    // A box, filled with a check while on (never the color alone)
    const double box = 18;
    const double bx = x + 16;
    const double by = y + (h - box) / 2;
    if (on) {
        fillRounded(pen, bx, by, box, box, 5, kAccent);
        drawCheck(pen.cr, bx + box / 2, by + box / 2, 14, kOnAccent);
    } else {
        strokeRounded(pen, bx, by, box, box, 5, edge, 2);
    }
    const double size = fitSize(pen, label, 16, 11, w - (bx + box + 8 - x) - 14, on);
    pen.text(bx + box + 8, centerBaseline(y, h, size), label, size, usable ? kText : kTextDisabled, on);
    addButton(hit, x, y, w, h, usable);
}

void EyePanel::drawPreviewEye(const Pen& pen, const eye_preview::EyeShown& eye, double x, double y, double w,
                              double h, bool turned) {
    cairo_t* cr = pen.cr;
    // Without a value: relaxed open, faded
    if (!eye.known) cairo_push_group(cr);
    // The corners, and the control points of the almond's top (as wide as the lids open) and bottom
    const double lx = x + 10;
    const double rx = x + w - 10;
    const double cy = y + h * 0.55;
    const double c1 = x + w * 0.3;
    const double c2 = x + w * 0.7;
    const double top = y - h * 0.12;
    const double bottom = cy + h * 0.45;
    // The lids: the upper one down from the top to its eyelid (to the bottom when closed), the lower one up for a squint
    const double upper = bottom - eye_preview::upperLidOpen(eye.lid) * (bottom - top);
    const double lower = bottom - eye_preview::lowerLidRise(eye.squint) * (bottom - cy);
    const auto curve = [&](double control, bool forward) {
        if (forward) {
            cairo_curve_to(cr, c1, control, c2, control, rx, cy);
        } else {
            cairo_curve_to(cr, c2, control, c1, control, lx, cy);
        }
    };
    const auto almond = [&]() {
        cairo_new_path(cr);
        cairo_move_to(cr, lx, cy);
        curve(top, true);
        curve(bottom, false);
        cairo_close_path(cr);
    };
    almond();
    pen.color(kEyeLid);
    cairo_fill(cr);
    if (upper < lower - 0.5) {
        // The open part: the white, the iris where the eye looks (sideways turned round when seen from the front) and
        // its pupil
        cairo_save(cr);
        cairo_new_path(cr);
        cairo_move_to(cr, lx, cy);
        curve(upper, true);
        curve(lower, false);
        cairo_close_path(cr);
        cairo_clip(cr);
        pen.color(kEyeWhite);
        cairo_paint(cr);
        const double iris = h * 0.27;
        const double sideways = eye_preview::gazeShare(eye.gazeX, false) * w * 0.22;
        const double ix = x + w / 2 + (turned ? -sideways : sideways);
        const double iy = cy - 4 - eye_preview::gazeShare(eye.gazeY, true) * h * 0.16;
        drawDot(cr, ix, iy, iris, kEyeIris);
        drawRing(cr, ix, iy, iris - 1.5, 3, kEyeIrisRim);
        drawDot(cr, ix, iy, eye_preview::pupilRadius(iris, eye.pupilMm), kEyeDark);
        // (a highlight, so it reads as an eye)
        cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
        cairo_new_sub_path(cr);
        cairo_arc(cr, ix + iris * 0.32, iy - iris * 0.36, iris * 0.11, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_restore(cr);
        // The lids' edges: the upper one dark like lashes, the lower one thin
        pen.color(kEyeDark);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width(cr, 4);
        cairo_new_path(cr);
        cairo_move_to(cr, lx, cy);
        curve(upper, true);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 1.5);
        cairo_new_path(cr);
        cairo_move_to(cr, rx, cy);
        curve(lower, false);
        cairo_stroke(cr);
    } else {
        // Closed: the lids meet
        pen.color(kEyeDark);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width(cr, 4);
        cairo_new_path(cr);
        cairo_move_to(cr, rx, cy);
        curve(lower, false);
        cairo_stroke(cr);
    }
    almond();
    pen.color(kBorder);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    // The brow, while one is sent: an arch over the middle of the eye, resting just above it and raised with the brow
    // (the room above the box is the caller's: eye_preview::kBrowHeadroom)
    if (std::isfinite(eye.brow)) {
        const double peak = y - (eye_preview::kBrowRest + eye_preview::browLift(eye.brow)) * h;
        const double ends = peak + eye_preview::kBrowArch * h;
        const double control = peak - eye_preview::kBrowArch * h / 3;
        pen.color(kEyeBrow);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width(cr, eye_preview::kBrowThickness * h);
        cairo_new_path(cr);
        cairo_move_to(cr, x + w * 0.22, ends);
        cairo_curve_to(cr, x + w * 0.4, control, x + w * 0.6, control, x + w * 0.78, ends);
        cairo_stroke(cr);
    }
    if (!eye.known) fadeGroup(pen);
}

void EyePanel::drawPreviewPicture(const Pen& pen, const UiText& t, const eye_preview::Picture& picture, bool fresh,
                                  double x, double y, double side, bool flip, bool smooth,
                                  const eye_preview::Marks& marks) {
    cairo_t* cr = pen.cr;
    const double r = 12;
    fillRounded(pen, x, y, side, side, r, kBg);
    const int n = picture.side;
    if (!fresh || n <= 0 || picture.pixels.size() != static_cast<size_t>(n) * n) {
        const std::vector<std::string> lines = wrapText(pen, t.previewWaitingPicture, 14, false, side - 20, 2);
        for (size_t i = 0; i < lines.size(); ++i) {
            textCentered(pen, x + side / 2, y + side / 2 + 5 + (i - (lines.size() - 1) / 2.0) * 20, lines[i], 14,
                         kTextMuted, false);
        }
        strokeRounded(pen, x, y, side, side, r, kBorder, 1.5);
        return;
    }
    // The grey picture as an RGB image (only in memory, for this draw), scaled into the box and mirrored as asked
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, n);
    std::vector<uint8_t> rgb(static_cast<size_t>(stride) * n);
    for (int row = 0; row < n; ++row) {
        uint32_t* out = reinterpret_cast<uint32_t*>(rgb.data() + static_cast<size_t>(row) * stride);
        const uint8_t* in = picture.pixels.data() + static_cast<size_t>(row) * n;
        for (int col = 0; col < n; ++col) out[col] = in[col] * 0x010101u;
    }
    cairo_surface_t* image = cairo_image_surface_create_for_data(rgb.data(), CAIRO_FORMAT_RGB24, n, n, stride);
    const double scale = side / n;
    cairo_save(cr);
    pen.roundedRect(x, y, side, side, r);
    cairo_clip(cr);
    cairo_save(cr);
    cairo_translate(cr, flip ? x + side : x, y);
    cairo_scale(cr, flip ? -scale : scale, scale);
    cairo_set_source_surface(cr, image, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(image);
    // The marks, in the picture's own coordinates (mirrored with it)
    const auto px = [&](double v) { return flip ? x + side - v * scale : x + v * scale; };
    const auto py = [&](double v) { return y + v * scale; };
    const double* win = picture.window;
    if (marks.window && std::isfinite(win[0]) && std::isfinite(win[1]) && std::isfinite(win[2]) &&
        std::isfinite(win[3])) {
        const double left = std::min(px(win[0]), px(win[2]));
        const double right = std::max(px(win[0]), px(win[2]));
        const double dash[2] = {6, 4};
        cairo_set_dash(cr, dash, 2, 0);
        pen.color(kPreviewWindow);
        cairo_set_line_width(cr, 2);
        cairo_rectangle(cr, left, py(win[1]), right - left, py(win[3]) - py(win[1]));
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
    }
    // Where eyecam-rec found the lids on this frame (none where it found none), a dot each on its line: the upper lid's
    // edge (its margin, which the opening is measured from) and the lower lid's, each on a dark edge so it shows on
    // bright skin too. Dots, not the whole lines: the lines' ends jump from picture to picture while the opening
    // doesn't. At the pupil's column, where eyecam-rec measures the opening (feat.rs: the lines' y_at at the pupil's
    // centre); without a pupil, the line's middle. Smoothed over the pictures but in Raw (the lower one jumps a few
    // px from picture to picture)
    const eye_preview::SmoothedLids lids = smooth ? picture.smoothed
                                                  : eye_preview::SmoothedLids {picture.upper, picture.margin,
                                                                               picture.lower, picture.eyeLid};
    const auto lidDot = [&](const eye_preview::LidLine& line, double below, Color color) {
        if (!line.present) return;
        const double at = picture.pupil && std::isfinite(picture.pupilX) ? picture.pupilX : (line.x0 + line.x1) / 2;
        const double lx = std::clamp(at, std::min(line.x0, line.x1), std::max(line.x0, line.x1));
        const double dx = px(lx);
        const double dy = py(eye_preview::lidLineY(line, lx) + below);
        pen.color(kBg, 0.7);
        cairo_new_sub_path(cr);
        cairo_arc(cr, dx, dy, 6.5, 0, 2 * M_PI);
        cairo_fill(cr);
        drawDot(cr, dx, dy, 4.5, color);
    };
    if (marks.lids && picture.lids) {
        lidDot(lids.upper, std::isfinite(lids.margin) ? lids.margin : 0.0, kPreviewLidUpper);
        lidDot(lids.lower, 0.0, kPreviewLidLower);
    }
    if (marks.pupil && picture.pupil) {
        drawRing(cr, px(picture.pupilX), py(picture.pupilY), picture.pupilR * scale, 2.5, kPreviewPupil);
    }
    const auto badge = [&](double by, const char* text) {
        const double size = 13;
        const double w = pen.measure(text, size, true) + 18;
        pen.color(kBg, 0.85);
        pen.roundedRect(x + 8, by, w, 24, 12);
        cairo_fill(cr);
        pen.text(x + 17, centerBaseline(by, 24, size), text, size, kText, true);
    };
    if (picture.closed) badge(y + 8, t.previewClosed);
    // The EyeLid eyecam-rec sent for this very frame, under its dots (and off with them)
    if (marks.lids && picture.lids && std::isfinite(lids.eyeLid)) {
        char lid[64];
        std::snprintf(lid, sizeof(lid), t.previewPictureLidFormat, lids.eyeLid);
        badge(y + side - 32, lid);
    }
    cairo_restore(cr);
    strokeRounded(pen, x, y, side, side, r, kBorder, 1.5);
}

void EyePanel::drawPreview(const Pen& pen, const UiText& t, const PanelModel& m) {
    using eye_preview::CameraState;
    using eye_preview::Source;
    // Stopped: what the first draw after "Freeze" showed, kept until "Resume"
    // Where the eyelids come from now (frameeyeosc's camera.used, as the eyelids card has it), for the badges
    eye_preview::CameraLids lids;
    lids.known = m.status.running;
    for (int eye = 0; eye < 2; ++eye) lids.used[eye] = m.status.running && m.status.camera.used[eye];
    if (previewFreezePending_) {
        previewKept_ = m.preview;
        previewKeptLids_ = lids;
        previewFreezePending_ = false;
    }
    const eye_preview::View& pv = previewFrozen_ ? previewKept_ : m.preview;
    const CameraState camera = eye_preview::cameraState(m.eyecam, SettingsView(m).flag(key::kCameraLids), pv.refused);
    const bool cameraValues = eye_preview::cameraValuesAvailable(camera);
    // ("Camera" while the camera's values can't be had shows the values as sent)
    const Source source = previewSource_ == Source::Camera && !cameraValues ? Source::Sent : previewSource_;
    // The eye cameras can't give pictures: their switch goes off, to be pressed again once they can (as the loop does)
    if (camera != CameraState::Available) stopPreviewPictures();
    const bool pictures = previewCamera_;
    // A sheet over the content card (the tabs and the left column stay), like the other Frame apps' list
    drawCard(pen, kRightX, kContentY, kRight - kRightX, kContentH, 20, kCard, kBorder, 2);
    const double closeS = 48;
    const double closeX = kInnerRight - closeS;
    const double headY = kContentY + 14;
    drawSheetClose(pen, {PanelAction::PreviewClose, nullptr, 0}, closeX, headY, closeS);
    pen.text(kInnerX, headY + 33, t.previewButton, fitSize(pen, t.previewButton, 24, 16, closeX - 16 - kInnerX, true),
             kText, true);

    // The controls: which values, the mirror, the pictures, freeze (one text size, the largest at which the switches
    // leave the choices room)
    const double rowY = headY + closeS + 14;
    const double rowH = 44;
    const double gap = 12;
    double size = 16;
    double mirrorW = 0;
    double cameraW = 0;
    double freezeW = 0;
    double choicesW = 0;
    for (; size >= 12; size -= 1) {
        mirrorW = pen.measure(t.previewMirror, size, true) + 58;
        cameraW = pen.measure(t.previewPicture, size, true) + 58;
        freezeW = std::max(pen.measure(t.previewFreeze, size, true), pen.measure(t.previewResume, size, true)) + 64;
        choicesW = kInnerRight - kInnerX - mirrorW - cameraW - freezeW - gap * 3;
        if (choicesW >= 270) break;
    }
    const std::vector<Option> sources = {
        {t.previewSourceSent, {PanelAction::PreviewSource, nullptr, static_cast<int>(Source::Sent)}, true},
        {t.previewSourceRaw, {PanelAction::PreviewSource, nullptr, static_cast<int>(Source::Raw)}, true},
        {t.previewSourceCamera, {PanelAction::PreviewSource, nullptr, static_cast<int>(Source::Camera)}, cameraValues},
    };
    drawSegmented(pen, kInnerX, rowY, choicesW, rowH, sources, static_cast<int>(source), size);
    double x = kInnerX + choicesW + gap;
    drawToggle(pen, x, rowY, mirrorW, rowH, t.previewMirror, {PanelAction::PreviewMirror, nullptr, 0}, previewMirror_,
               true);
    x += mirrorW + gap;
    const bool cameraUsable = camera == CameraState::Available;
    drawToggle(pen, x, rowY, cameraW, rowH, t.previewPicture, {PanelAction::PreviewCamera, nullptr, 0}, previewCamera_,
               cameraUsable);
    x += cameraW + gap;
    drawIconButton(pen, x, rowY, freezeW, rowH, previewFrozen_ ? t.previewResume : t.previewFreeze,
                   {PanelAction::PreviewFreeze, nullptr, 0}, true, false, size,
                   previewFrozen_ ? ButtonIcon::Play : ButtonIcon::Pause);
    // Why the pictures (and the camera's values) can't show, under the switches
    const char* why = nullptr;
    switch (camera) {
        case CameraState::Available: break;
        case CameraState::NotRunning: why = t.previewCamNotRunning; break;
        case CameraState::NotSetUp: why = t.previewCamNotSetUp; break;
        case CameraState::NotInUse: why = t.previewCamNotInUse; break;
        case CameraState::Old: why = t.previewCamOld; break;
        case CameraState::Refused: why = t.previewCamRefused; break;
    }
    if (why != nullptr) {
        const double whySize = fitSize(pen, why, 14, 11, kInnerRight - kInnerX, false);
        pen.text(kInnerRight, rowY + rowH + 22, why, whySize, kTextMuted, false, true);
    }

    // Both eyes, each with its chips, and under them its camera picture; without the pictures, in the middle of the
    // space. In a mirror the right eye is on the right, seen from the front on the left (eye_preview::drawnColumn)
    const eye_preview::CameraLids& shownLids = previewFrozen_ ? previewKeptLids_ : lids;
    const eye_preview::Shown shown =
        pv.hasPacket ? eye_preview::shown(pv.packet, source, shownLids) : eye_preview::Shown {};
    double eyeW = 255;
    double eyeH = 170;
    const double chipsH = 28;
    const double pictureSide = 176;
    const double footer = kContentY + kContentH - 36;
    const double areaTop = rowY + rowH + (why != nullptr ? 34 : 12);
    const double belowEye = 6 + chipsH + (pictures ? 10 + pictureSide : 0);
    // While brows are sent, room above the eyes for them raised all the way; where the space is short (the pictures
    // on), the eyes are drawn smaller to make it (as wide for their height)
    double browRoom = 0;
    if (shown.brows) {
        const double space = footer - 22 - areaTop - belowEye - 2;
        const double fitH = std::floor(space / (1 + eye_preview::kBrowHeadroom));
        eyeW = eyeW * std::min(eyeH, fitH) / eyeH;
        eyeH = std::min(eyeH, fitH);
        browRoom = std::ceil(eye_preview::kBrowHeadroom * eyeH) + 2;
    }
    const double blockH = browRoom + eyeH + belowEye;
    const double blockTop = pictures ? areaTop : areaTop + std::max(0.0, (footer - 22 - areaTop - blockH) / 2);
    const double top = blockTop + browRoom;
    const double half = (kInnerRight - kInnerX) / 2;
    for (int at = 0; at < 2; ++at) {
        const int eye = eye_preview::drawnColumn(0, previewMirror_) == at ? 0 : 1;
        const eye_preview::EyeShown& e = shown.eyes[eye];
        const double cx = kInnerX + half * (at + 0.5);
        const double ex = cx - eyeW / 2;
        drawPreviewEye(pen, e, ex, top, eyeW, eyeH, eye_preview::gazeTurned(previewMirror_));
        // Which eye, in the box's top corner on the outer side
        const char* name = eye == 0 ? t.leftEye : t.rightEye;
        pen.text(at == 0 ? ex : ex + eyeW, top + 16, name, 15, kTextMuted, true, at != 0);
        // Its chips: the eyelid, where it came from, the pupil
        char lid[64];
        if (shown.valid && std::isfinite(e.lidValue)) {
            std::snprintf(lid, sizeof(lid), t.previewLidFormat, e.lidValue);
        } else {
            std::snprintf(lid, sizeof(lid), "%s", t.previewLidNone);
        }
        char pupil[64];
        if (shown.valid && std::isfinite(e.pupilMm)) {
            std::snprintf(pupil, sizeof(pupil), t.previewPupilFormat, e.pupilMm);
        } else {
            std::snprintf(pupil, sizeof(pupil), "%s", t.previewPupilNone);
        }
        const double chipSize = 14;
        const double lidW = pen.measure(lid, chipSize) + 22;
        const double pupilW = pen.measure(pupil, chipSize) + 22;
        // (where the eyelid came from: only for an eye with a sample)
        const bool sampled = shown.valid && e.known;
        const double badgeW = !sampled ? 0.0
                              : e.camera ? cameraBadge(pen, t, 0, 0, false)
                                         : pen.measure(t.previewFromValve, 12, true) + 18;
        const double chipGap = 8;
        const double chipsY = top + eyeH + 6;
        double chipX = cx - (lidW + badgeW + pupilW + chipGap * (sampled ? 2 : 1)) / 2;
        const auto chip = [&](const char* text, double w) {
            fillRounded(pen, chipX, chipsY, w, chipsH, chipsH / 2, kControl);
            pen.text(chipX + 11, centerBaseline(chipsY, chipsH, chipSize), text, chipSize, kText);
            chipX += w + chipGap;
        };
        chip(lid, lidW);
        if (sampled && e.camera) {
            cameraBadge(pen, t, chipX, chipsY + chipsH / 2);
        } else if (sampled) {
            const double h = 20;
            const double by = chipsY + (chipsH - h) / 2;
            fillRounded(pen, chipX, by, badgeW, h, h / 2, kControl);
            strokeRounded(pen, chipX, by, badgeW, h, h / 2, kBorder, 1);
            pen.text(chipX + 9, centerBaseline(by, h, 12), t.previewFromValve, 12, kTextMuted, true);
        }
        if (sampled) chipX += badgeW + chipGap;
        chip(pupil, pupilW);
        // The camera picture, with its temporal side toward the outside of the sheet (eyecam-rec gives both eyes with
        // it at the left: the one in the right column is flipped)
        if (pictures) {
            const eye_preview::Picture& picture = pv.pictures.eyes[eye];
            const bool fresh = pv.hasPictures && eye_preview::pictureFresh(picture, pv.nowNs);
            drawPreviewPicture(pen, t, picture, fresh, cx - pictureSide / 2, chipsY + chipsH + 10, pictureSide,
                               eye_preview::pictureFlipped(eye, previewMirror_), source != Source::Raw, previewMarks_);
        }
    }
    // The marks' switches, one under the other in the space between the two pictures (they go for both; nothing
    // else is there, so the eyes keep their size)
    if (pictures) {
        using eye_preview::Mark;
        const struct {
            const char* label;
            Mark mark;
            bool on;
        } switches[] = {{t.previewMarkLids, Mark::Lids, previewMarks_.lids},
                        {t.previewMarkPupil, Mark::Pupil, previewMarks_.pupil},
                        {t.previewMarkWindow, Mark::Window, previewMarks_.window}};
        const double spaceW = half - pictureSide - 2 * gap;
        double switchW = 0;
        for (const auto& s : switches) switchW = std::max(switchW, pen.measure(s.label, 16, true) + 58);
        switchW = std::min(switchW, spaceW);
        const double switchGap = 10;
        const double pictureY = top + eyeH + 6 + chipsH + 10;
        double switchY = pictureY + (pictureSide - (rowH * 3 + switchGap * 2)) / 2;
        for (const auto& s : switches) {
            drawToggle(pen, kInnerX + half - switchW / 2, switchY, switchW, rowH, s.label,
                       {PanelAction::PreviewMark, nullptr, static_cast<int>(s.mark)}, s.on, true);
            switchY += rowH + switchGap;
        }
    }

    // The footer: the gaze (as the user looks: + right, + up), and what the pictures show or what the drawing is for
    std::string gaze = t.noEyeData;
    if (shown.valid) {
        // (no "-0")
        const double sideways = std::round(shown.gazeX * 45.0) + 0.0;
        const double upDown = std::round(shown.gazeY * 45.0) + 0.0;
        char text[128];
        std::snprintf(text, sizeof(text), t.previewGazeFormat, sideways, upDown);
        gaze = text;
    }
    pen.text(kInnerX, footer, gaze, fitSize(pen, gaze, 15, 11, kInnerRight - kInnerX, true), kText, true);
    const char* note = pictures ? t.previewNotePicture : t.previewNoteShape;
    pen.text(kInnerX, footer + 22, note, fitSize(pen, note, 14, 10, kInnerRight - kInnerX, false), kTextMuted);
}

void EyePanel::render(const PanelModel& model) {
    const Pen pen {cr_, &fonts_};
    const UiText& t = uiText(model.language);
    const SettingsView view(model);
    buttons_.clear();
    snapMark_ = {};

    // Opaque background (the contrast ratios assume it)
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);
    fillRounded(pen, 0, 0, kWidth, kHeight, 24, kBg);

    // The developer tab comes and goes with eyecam-rec; once it is gone, its page goes back to the first tab
    eyecamTab_ = model.eyecam.visible;
    if (!eyecamTab_ && tab_ == PanelTab::Eyecam) tab_ = PanelTab::Basic;
    syncEyecam(model.eyecam);
    // The Advanced tab's page starts at its top again once the tab is left (drawAdvanced sets these while it scrolls)
    if (tab_ != PanelTab::Advanced) advScroll_ = 0.0;
    advScrollable_ = false;
    listScrollable_ = false;
    hitClip_ = false;
    recordNames_.clear();
    appsKeys_.clear();
    appsScrollable_ = false;

    drawStatus(pen, t, model);
    drawTabs(pen, t);
    drawCard(pen, kRightX, kContentY, kRight - kRightX, kContentH, 20, kCard, kDivider, 1);
    if (previewOpen_) {
        drawPreview(pen, t, model);
    } else {
        switch (tab_) {
            case PanelTab::Basic: drawBasic(pen, t, model, view); break;
            case PanelTab::Output: drawOutputTab(pen, t, model, view); break;
            case PanelTab::Gaze: drawGaze(pen, t, model, view); break;
            case PanelTab::EyeFit: drawEyeFit(pen, t, model, view); break;
            case PanelTab::Lids: drawLids(pen, t, model, view); break;
            case PanelTab::Advanced: drawAdvanced(pen, t, model, view); break;
            case PanelTab::Eyecam: drawEyecam(pen, t, model, view); break;
        }
    }
    // While a fit runs on its tab, the rest is dimmed and can't be pressed, except the tabs (leaving stops the fit);
    // also with the eye preview open over it (an automatic re-center starting), whose own buttons stay
    if (tab_ == PanelTab::EyeFit && fitRunning(model.fit)) {
        pen.color(kBg, 0.72);
        cairo_rectangle(pen.cr, kStatusX, kStatusY, kStatusW, kStatusH);
        cairo_fill(pen.cr);
        pen.color(kBg, 0.5);
        cairo_rectangle(pen.cr, kRightX, kTabY, kRight - kRightX, kTabH);
        cairo_fill(pen.cr);
        for (Button& b : buttons_) {
            const PanelAction a = b.hit.action;
            if (a != PanelAction::Tab && a != PanelAction::FitStop && !(previewOpen_ && previewSheetAction(a))) {
                b.usable = false;
            }
        }
    }
    if (promptOpen()) drawPrompt(pen, t);
    if (keypadOpen_) drawKeypad(pen, t);

    // Forget hover on a button that is gone or can no longer be pressed
    bool hoverFound = false;
    for (const auto& b : buttons_) hoverFound |= b.usable && b.hit == hover_;
    if (!hoverFound) hover_ = {};
    cairo_surface_flush(surface_);
}

const std::vector<uint8_t>& EyePanel::toRgba() {
    surfaceToRgba(surface_, rgba_);
    return rgba_;
}

bool EyePanel::writePng(const std::string& path) const {
    return cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

int EyePanel::width() const {
    return kWidth;
}

int EyePanel::height() const {
    return kHeight;
}

std::string appsCountText(const UiText& t, const std::vector<frame_apps::Entry>& entries) {
    int missing = 0;
    for (const frame_apps::Entry& entry : entries) missing += entry.state == frame_apps::AppState::Missing ? 1 : 0;
    char text[200];
    if (missing > 0) {
        std::snprintf(text, sizeof(text), t.appsAllCountFormat, static_cast<int>(entries.size()), missing);
    } else {
        std::snprintf(text, sizeof(text), t.appsAllInstalledFormat, static_cast<int>(entries.size()));
    }
    return text;
}

void renderThumbnail(const FontSet& fonts, int size, std::vector<uint8_t>& rgba, const std::string& pngPath) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* cr = cairo_create(surface);
    const Pen pen {cr, &fonts};
    const double s = size / 256.0;

    fillRounded(pen, 8 * s, 8 * s, 240 * s, 240 * s, 48 * s, kBg);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_set_line_width(cr, 2 * s);
    pen.roundedRect(9 * s, 9 * s, 238 * s, 238 * s, 47 * s);
    cairo_stroke(cr);

    // The eye: an almond outline, an accent iris with a soft glow, a dark pupil and a highlight
    const double cx = 128 * s;
    const double cy = 104 * s;
    /**
     * The almond shape of the eye.
     */
    const auto almond = [&]() {
        cairo_new_path(cr);
        cairo_move_to(cr, 30 * s, cy);
        cairo_curve_to(cr, 80 * s, 38 * s, 176 * s, 38 * s, 226 * s, cy);
        cairo_curve_to(cr, 176 * s, 170 * s, 80 * s, 170 * s, 30 * s, cy);
        cairo_close_path(cr);
    };
    almond();
    pen.color(kCard);
    cairo_fill_preserve(cr);
    cairo_save(cr);
    cairo_clip(cr);
    for (int i = 3; i >= 1; --i) {
        pen.color(kAccent, 0.10);
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, (40 + i * 6) * s, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    drawDot(cr, cx, cy, 40 * s, kAccent);
    drawDot(cr, cx, cy, 17 * s, kBg);
    drawDot(cr, cx + 14 * s, cy - 14 * s, 7 * s, kText);
    cairo_restore(cr);
    almond();
    pen.color(kText);
    cairo_set_line_width(cr, 9 * s);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);

    const double w = pen.measure("Eye", 56 * s, true);
    pen.text((size - w) / 2, 228 * s, "Eye", 56 * s, kText, true);

    cairo_surface_flush(surface);
    surfaceToRgba(surface, rgba);
    if (!pngPath.empty()) cairo_surface_write_to_png(surface, pngPath.c_str());
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}
