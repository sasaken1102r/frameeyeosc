// The panel. Colors only come from theme.h (in step with the --contrast-report pairs).
#include "panel.h"

#include "draw.h"
#include "fit_text.h"
#include "host_entry.h"
#include "recorder.h"
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
/** Below this many samples a second from the eye tracker, the left column marks the rate as low. */
constexpr double kLowTrackerRate = 60;
constexpr double kRowH = 64;
constexpr double kRowGap = 2;
constexpr double kCaptionRowH = 84;
constexpr double kControlH = 52;
constexpr double kUpdateRowH = 138;  ///< the version row (four lines of text and the update check chip)
/** The raw openness range drawn in the lid mark bars. */
constexpr double kLidScaleMax = 1.2;

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
 * Length in bytes of the UTF-8 character that starts with this byte.
 * @param lead the first byte
 * @return 1..4
 */
size_t utf8Length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

/**
 * Shorten text with "…" until it fits.
 * @param pen drawing tools
 * @param text the text
 * @param size text size
 * @param bold whether bold
 * @param maxWidth the width to fit
 * @param keepEnd keep the end and cut the start (for file paths)
 * @return the text that fits
 */
std::string ellipsize(const Pen& pen, const std::string& text, double size, bool bold, double maxWidth, bool keepEnd) {
    if (pen.measure(text, size, bold) <= maxWidth) return text;
    std::vector<std::string> chars;
    for (size_t i = 0; i < text.size();) {
        const size_t n = std::min(utf8Length(static_cast<unsigned char>(text[i])), text.size() - i);
        chars.push_back(text.substr(i, n));
        i += n;
    }
    while (!chars.empty()) {
        if (keepEnd) {
            chars.erase(chars.begin());
        } else {
            chars.pop_back();
        }
        std::string joined;
        for (const auto& c : chars) joined += c;
        const std::string candidate = keepEnd ? "…" + joined : joined + "…";
        if (pen.measure(candidate, size, bold) <= maxWidth) return candidate;
    }
    return "…";
}

/**
 * Whether a CJK character may not start a line (closing brackets and punctuation).
 * @param unit one character
 * @return true for 。、）」』，．！？
 */
bool closesLine(const std::string& unit) {
    static const char* const marks[] = {"。", "、", "）", "」", "』", "，", "．", "！", "？"};
    for (const char* mark : marks) {
        if (unit == mark) return true;
    }
    return false;
}

/**
 * Break text into lines that fit a width. Breaks at spaces, and between any two CJK characters, but never before
 * a closing mark (the character before it moves down with it).
 * The last allowed line is shortened with "…" if the text does not fit.
 * @param pen drawing tools
 * @param text the text
 * @param size text size
 * @param bold whether bold
 * @param maxWidth line width
 * @param maxLines most lines
 * @return the lines
 */
std::vector<std::string> wrapText(const Pen& pen, const std::string& text, double size, bool bold, double maxWidth,
                                  size_t maxLines) {
    // Units: a word with its trailing spaces, or one CJK character
    std::vector<std::string> units;
    std::string word;
    for (size_t i = 0; i < text.size();) {
        const size_t n = std::min(utf8Length(static_cast<unsigned char>(text[i])), text.size() - i);
        const std::string ch = text.substr(i, n);
        i += n;
        if (n >= 3) {
            if (!word.empty()) units.push_back(word);
            word.clear();
            units.push_back(ch);
        } else if (ch == " ") {
            word += ch;
            units.push_back(word);
            word.clear();
        } else {
            word += ch;
        }
    }
    if (!word.empty()) units.push_back(word);

    std::vector<std::string> lines;
    std::string line;
    size_t lineStart = 0;  // the line's first unit
    for (size_t u = 0; u < units.size(); ++u) {
        const std::string candidate = line + units[u];
        std::string trimmed = candidate;
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        if (line.empty() || pen.measure(trimmed, size, bold) <= maxWidth) {
            line = candidate;
            continue;
        }
        // The next line starts here, or a character earlier if this one is a closing mark
        const size_t next = closesLine(units[u]) && u > lineStart + 1 && units[u - 1].size() >= 3 ? u - 1 : u;
        std::string head;
        for (size_t i = lineStart; i < next; ++i) head += units[i];
        while (!head.empty() && head.back() == ' ') head.pop_back();
        lines.push_back(head);
        line.clear();
        for (size_t i = next; i <= u; ++i) line += units[i];
        lineStart = next;
        if (lines.size() == maxLines) {
            // Out of lines: put the rest on the last line and cut it with "…"
            std::string rest = lines.back();
            for (size_t r = next; r < units.size(); ++r) rest += units[r];
            lines.back() = ellipsize(pen, rest, size, bold, maxWidth, false);
            return lines;
        }
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    if (!line.empty()) lines.push_back(ellipsize(pen, line, size, bold, maxWidth, false));
    return lines;
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

/**
 * Draw a cross with lines (no font needed), the counterpart of drawCheck.
 * @param cr cairo
 * @param cx center x
 * @param cy center y
 * @param s size (px)
 * @param c color
 */
void drawCross(cairo_t* cr, double cx, double cy, double s, Color c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
    cairo_set_line_width(cr, s * 0.16);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    const double r = s * 0.28;
    cairo_move_to(cr, cx - r, cy - r);
    cairo_line_to(cr, cx + r, cy + r);
    cairo_move_to(cr, cx + r, cy - r);
    cairo_line_to(cr, cx - r, cy + r);
    cairo_stroke(cr);
}

/** A leading "✓ " or "✗ " on a destination card's line, drawn with lines since the font may not have them. */
enum class LineMark { None, Check, Cross };

/**
 * Split a card line into its leading mark and the rest.
 * @param text the line ("✓ Wide eyes")
 * @param rest the text after the mark (written)
 * @return the mark
 */
LineMark splitMark(const std::string& text, std::string& rest) {
    static const std::string check = "\u2713 ";
    static const std::string cross = "\u2717 ";
    if (text.compare(0, check.size(), check) == 0) {
        rest = text.substr(check.size());
        return LineMark::Check;
    }
    if (text.compare(0, cross.size(), cross) == 0) {
        rest = text.substr(cross.size());
        return LineMark::Cross;
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
 * A numbered circle (the lid marks ① to ④).
 * @param pen drawing tools
 * @param cx center x
 * @param cy center y
 * @param number 1..4
 */
void drawNumberBadge(const Pen& pen, double cx, double cy, int number) {
    drawDot(pen.cr, cx, cy, 10, kAccent);
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
 * The headset's tilt as shown: 1 decimal and degrees ("+6.7°"; zero without a sign).
 * @param deg the setting
 * @return the text
 */
std::string rollText(double deg) {
    if (!std::isfinite(deg)) return "—";
    const double shown = std::fabs(deg) < 0.05 ? 0.0 : deg;
    char text[32];
    std::snprintf(text, sizeof(text), shown == 0.0 ? "%.1f°" : "%+.1f°", shown);
    return text;
}

/**
 * What to do while a fit waits or runs.
 * @param t texts
 * @param mode the fit
 * @return the words
 */
const char* waitingText(const UiText& t, gaze_fit::Mode mode) {
    switch (mode) {
        case gaze_fit::Mode::Center: return t.fitWaitingCenter;
        case gaze_fit::Mode::Tilt: return t.fitWaitingTilt;
        case gaze_fit::Mode::Full: break;
    }
    return t.fitHowTo;
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
    buttons_.push_back({hit, x, y, w, h, usable});
}

int EyePanel::pointerState(const PanelHit& hit) const {
    if (pressed_ == hit) return 2;
    if (hover_ == hit) return 1;
    return 0;
}

PanelHit EyePanel::hitTest(double x, double y) const {
    for (const auto& b : buttons_) {
        if (!b.usable) continue;
        if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) return b.hit;
    }
    return {};
}

bool EyePanel::pointerMove(double x, double y) {
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
    switch (hit.action) {
        case PanelAction::Tab:
            tab_ = static_cast<PanelTab>(hit.arg);
            return {};
        case PanelAction::FitDetails:
            fitDetails_ = !fitDetails_;
            return {};
        case PanelAction::FitDetailsPage:
            fitDetailsPage_ = hit.arg;
            return {};
        case PanelAction::LidMarks:
            lidMarksOpen_ = !lidMarksOpen_;
            return {};
        case PanelAction::HostKey:
            hostEntryText_ = host_entry::keypadInput(hostEntryText_, hit.arg);
            hostEntryError_.clear();
            return {};
        case PanelAction::HostCancel:
            closeHostEntry();
            return {};
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
    if (pressed_.action == PanelAction::None) return false;
    pressed_ = {};
    return true;
}

bool EyePanel::pointerLeave() {
    const bool changed = hover_.action != PanelAction::None || pressed_.action != PanelAction::None;
    hover_ = {};
    pressed_ = {};
    return changed;
}

bool EyePanel::tick(double now) {
    bool changed = false;
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
    hostEntryOpen_ = true;
    hostEntryText_ = text;
    hostEntryError_.clear();
    hover_ = {};
    pressed_ = {};
}

void EyePanel::closeHostEntry() {
    hostEntryOpen_ = false;
    hostEntryError_.clear();
    hover_ = {};
    pressed_ = {};
}

void EyePanel::showUpdatePrompt(const std::string& version) {
    updatePromptVersion_ = bareVersion(version);
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
                            const std::string& hint, bool locked) {
    const double titleSize = fitSize(pen, title, 20, 14, kLabelW, true);
    if (hint.empty() && !locked) {
        pen.text(kInnerX, centerBaseline(y, h, titleSize), title, titleSize, kText, true);
        return;
    }
    pen.text(kInnerX, y + h / 2 - 3, title, titleSize, kText, true);
    const double hintY = y + h / 2 + 19;
    if (locked) {
        drawLock(pen, kInnerX, hintY, 16, kTextMuted);
        const double x = kInnerX + 20;
        pen.text(x, hintY, t.locked, fitSize(pen, t.locked, 15, 11, kLabelW - 20, false), kTextMuted);
        return;
    }
    pen.text(kInnerX, hintY, hint, fitSize(pen, hint, 15, 11, kLabelW, false), kTextMuted);
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
                           const std::string& text, bool usable, bool locked, double low, double high) {
    const SettingSpec* spec = findSetting(name);
    const bool active = usable && !locked;
    const double lower = spec != nullptr ? std::max(spec->min, low) : low;
    const double upper = spec != nullptr ? std::min(spec->max, high) : high;
    const bool canMinus = active && (!std::isfinite(value) || value > lower + 1e-9);
    const bool canPlus = active && (!std::isfinite(value) || value < upper - 1e-9);
    const double r = h / 2;
    fillRounded(pen, x, y, w, h, r, kControl);
    if (active) strokeRounded(pen, x, y, w, h, r, kBorder, 2);
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
    textCentered(pen, x + w / 2, centerBaseline(y, h, size), text, size, color, true);
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

    // Destination and rate
    pen.text(x0, 154, t.destination, 15, kTextMuted, true);
    if (!s.running) {
        pen.text(x0, 180, t.notRunningHint1, fitSize(pen, t.notRunningHint1, 16, 12, x1 - x0, false), kText);
        pen.text(x0, 204, t.notRunningHint2, fitSize(pen, t.notRunningHint2, 16, 12, x1 - x0, false), kText);
    } else {
        const char* receiver = s.output == kOutputEtvr       ? t.outputEtvrShort
                               : s.output == kOutputLivelink ? t.outputLivelinkShort
                                                             : t.outputVrchatShort;
        const std::string destination =
            std::string(receiver) + " → " + (s.target.empty() ? std::string(t.searchingPc) : s.target);
        const double size = fitSize(pen, destination, 19, 13, x1 - x0, true);
        pen.text(x0, 180, destination, size, kText, true);
        pen.text(x0, 204, s.targetMode == "fixed" ? t.modeFixed : t.modeAuto, 15, kTextMuted);
    }
    pen.text(x0, 228, t.rateLabel, 15, kTextMuted, true);
    {
        char rate[64];
        std::snprintf(rate, sizeof(rate), t.rateFormat, s.rate);
        pen.text(x1, 228, s.running ? std::string(rate) : std::string("—"), 17, kText, true, true);
    }
    // How fast the eye tracker delivers samples, sent or not: it has been seen at 15 a second instead of 90+
    pen.text(x0, 250, t.trackerRateLabel, 15, kTextMuted, true);
    {
        const bool known = s.running && s.tracking && std::isfinite(s.trackerRate);
        const bool low = known && s.trackerRate < kLowTrackerRate;
        char rate[64];
        std::snprintf(rate, sizeof(rate), t.rateFormat, s.trackerRate);
        const std::string text = known ? (low ? std::string(t.trackerRateLowHint) + "  " + rate : std::string(rate))
                                       : std::string("—");
        pen.text(x1, 250, text, 17, low ? kDanger : kText, true, true);
    }
    pen.color(kDivider);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, x0, 262.5);
    cairo_line_to(cr, x1, 262.5);
    cairo_stroke(cr);

    // Eyelids: a thin gray bar for the raw value, a thick accent bar for the sent value
    const bool live = s.running && s.tracking;
    pen.text(x0, 282, t.lidsTitle, 15, kTextMuted, true);
    {
        const double sentW = pen.measure(t.legendSent, 14, false);
        const double rawW = pen.measure(t.legendRaw, 14, false);
        double lx = x1 - sentW;
        pen.text(lx, 282, t.legendSent, 14, kText);
        lx -= 26;
        fillRounded(pen, lx, 272, 20, 10, 3, kAccent);
        lx -= 18 + rawW;
        pen.text(lx, 282, t.legendRaw, 14, kText);
        lx -= 26;
        fillRounded(pen, lx, 274, 20, 6, 2, kTextMuted);
    }
    for (int eye = 0; eye < 2; ++eye) {
        const double top = 296 + eye * 48;
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
    cairo_move_to(cr, x0, 402.5);
    cairo_line_to(cr, x1, 402.5);
    cairo_stroke(cr);

    // Gaze: raw = ring, sent = filled dot
    pen.text(x0, 430, t.gazeTitle, 15, kTextMuted, true);
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
    // Each eye's own pad while the eyes move separately; one pad for the shared gaze otherwise
    const bool perEye = SettingsView(m).flag(key::kIndependentEyes);
    if (perEye) {
        // The legend on the title line: ring = raw, the two eye colors = sent
        {
            const double sentW = pen.measure(t.legendSent, 14, false);
            const double rawW = pen.measure(t.legendRaw, 14, false);
            double lx = x1 - sentW;
            pen.text(lx, 430, t.legendSent, 14, kText);
            lx -= 14;
            drawDot(cr, lx, 425, 5, kDotRight);
            lx -= 12;
            drawDot(cr, lx, 425, 5, kDotLeft);
            lx -= 18 + rawW;
            pen.text(lx, 430, t.legendRaw, 14, kText);
            lx -= 14;
            drawRing(cr, lx, 425, 6, 2, kText);
        }
        const double box = 132;
        const char* labels[2] = {t.leftEye, t.rightEye};
        const Color colors[2] = {kDotLeft, kDotRight};
        for (int eye = 0; eye < 2; ++eye) {
            // The left eye on the left, as the user sees it (not mirrored)
            const double bx = eye == 0 ? x0 : x1 - box;
            pen.text(bx + 2, 456, labels[eye], 15, kText, true);
            const bool hasSent = gazePad(bx, 464, box, s.rawGazeEye[eye], s.sentGazeEye[eye], colors[eye]);
            const std::string xy = hasSent ? xyText(s.sentGazeEye[eye]) : "—";
            pen.text(bx + 2, 618, xy, fitSize(pen, xy, 14, 10, box, false), kTextMuted);
        }
    } else {
        const double box = 164;
        const double bx = x0;
        const double by = 444;
        gazePad(bx, by, box, s.gaze, s.sentGaze, kAccent);
        const bool hasRaw = live && s.hasRaw && s.gaze.valid();
        const bool hasSent = live && s.hasSent && s.sentGaze.valid();
        const double lx = bx + box + 18;
        drawRing(cr, lx + 9, 470, 8, 2.5, kText);
        pen.text(lx + 26, 476, t.legendRaw, 15, kText);
        pen.text(lx, 502, hasRaw ? xyText(s.gaze) : "—", fitSize(pen, xyText(s.gaze), 15, 11, x1 - lx, false),
                 kTextMuted);
        drawDot(cr, lx + 9, 540, 7, kAccent);
        pen.text(lx + 26, 546, t.legendSent, 15, kText);
        pen.text(lx, 572, hasSent ? xyText(s.sentGaze) : "—", fitSize(pen, xyText(s.sentGaze), 15, 11, x1 - lx, false),
                 kTextMuted);
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
    if (!message.empty()) {
        const std::vector<std::string> lines = wrapText(pen, message, 15, true, x1 - x0, 2);
        for (size_t i = 0; i < lines.size(); ++i) pen.text(x0, 636 + i * 22, lines[i], 15, kDanger, true);
    } else {
        drawUpdateNotice(pen, t, m.update, x0, x1);
    }
}

void EyePanel::drawUpdateNotice(const Pen& pen, const UiText& t, const frame_updater::UpdateStatus& u, double x0,
                                double x1) {
    std::string text;
    switch (u.state) {
        case frame_updater::UpdateState::Available:
            text = formatText(t.updateAvailableFormat, bareVersion(u.latest));
            break;
        case frame_updater::UpdateState::Installing:
            text = formatText(t.updateInstallingFormat, updateStepText(t, u.step));
            break;
        case frame_updater::UpdateState::Installed:
            text = formatText(t.updateInstalledFormat, bareVersion(u.version));
            break;
        default: return;
    }
    // Opens the Advanced tab, where the version row is
    const PanelHit hit {PanelAction::Tab, nullptr, static_cast<int>(PanelTab::Advanced)};
    const bool usable = tab_ != PanelTab::Advanced;
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
    // In PanelTab order
    const char* labels[] = {t.tabBasic, t.tabOutput, t.tabGaze, t.tabGazeFit, t.tabLids, t.tabAdvanced};
    const int count = static_cast<int>(std::size(labels));
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
        const PanelHit hit {PanelAction::Tab, nullptr, i};
        const bool selected = static_cast<int>(tab_) == i;
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
    // Language (each name in its own language)
    {
        drawRowLabel(pen, t, y, kRowH, t.rowLanguage, "", false);
        drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                      {{"日本語", {PanelAction::Language, key::kLanguage, 0}},
                       {"English", {PanelAction::Language, key::kLanguage, 1}}},
                      m.language == Language::Ja ? 0 : 1, 20);
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
    }
    y += kRowH + kRowGap + 2;
    // Reset all / quit (both ask for a second press)
    for (int i = 0; i < 2; ++i) {
        const bool isQuit = i == 1;
        const PanelHit hit {isQuit ? PanelAction::Quit : PanelAction::ResetAll, nullptr, 0};
        const bool armed = isQuit ? quitArmed_ : resetArmed_;
        const double w = 250;
        const double x = isQuit ? kInnerRight - w : kInnerX;
        const int pointer = pointerState(hit);
        const std::string label = isQuit ? (armed ? t.quitConfirm : t.quit) : (armed ? t.resetConfirm : t.resetAll);
        fillRounded(pen, x, y, w, kControlH, kControlH / 2, armed ? kDanger : (pointer > 0 ? kControlHover : kQuitFill));
        strokeRounded(pen, x, y, w, kControlH, kControlH / 2, kDanger, 2);
        const double size = fitSize(pen, label, 19, 12, w - 24, true);
        textCentered(pen, x + w / 2, centerBaseline(y, kControlH, size), label, size, armed ? kOnAccent : kText, true);
        addButton(hit, x, y, w, kControlH);
    }
    y += kControlH + 28;
    const std::vector<std::string> lines = wrapText(pen, t.footer, 15, false, kInnerRight - kInnerX, 2);
    for (size_t i = 0; i < lines.size(); ++i) pen.text(kInnerX, y + i * 22, lines[i], 15, kTextMuted);
}

double EyePanel::drawOutputCards(const Pen& pen, const UiText& t, const SettingsView& v, double y) {
    const bool locked = v.locked(key::kOutput);
    const int selected = argOfOutput(v.text(key::kOutput));
    // The title, and after it what "sync" on the cards means (or the lock note). The cards take the whole width
    // under it, so their names and lines stay large enough to read
    const double titleSize = fitSize(pen, t.rowOutput, 20, 14, kLabelW, true);
    const double titleBaseline = y + 30;
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

    // The cards in the order VRChat, LiveLink, ETVR (their button arguments are 0, 2, 1)
    const int args[3] = {0, 2, 1};
    const char* titles[3] = {t.outputVrchat, t.outputLivelink, t.outputEtvr};
    const int marks[3] = {0, 1, 2};  // rows of t.outputMarks
    const double gap = 12;
    const double cardW = (kInnerRight - kInnerX - gap * 2) / 3;
    const double pad = 16;
    const double innerW = cardW - pad * 2;
    const double top = y + 52;  // room for the "Recommended" tag, which sits on the card's top edge
    // One title size for all three cards
    double cardTitleSize = 20;
    for (const char* title : titles) cardTitleSize = std::min(cardTitleSize, fitSize(pen, title, 20, 14, innerW, true));
    // Each card's lines: a drawn mark on the first line of a marked text, the rest indented after it
    const double lineSize = 16;
    const double lineStep = 22;
    const double markW = 20;
    struct Line {
        LineMark mark;
        bool indent;
        std::string text;
    };
    std::vector<Line> lines[3];
    size_t most = 0;
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) {
            std::string rest;
            const LineMark mark = splitMark(t.outputMarks[marks[c]][k], rest);
            const double indent = mark == LineMark::None ? 0 : markW;
            const std::vector<std::string> wrapped = wrapText(pen, rest, lineSize, false, innerW - indent, 2);
            for (size_t i = 0; i < wrapped.size(); ++i) {
                lines[c].push_back({i == 0 ? mark : LineMark::None, mark != LineMark::None, wrapped[i]});
            }
        }
        most = std::max(most, lines[c].size());
    }
    const double cardH = pad + cardTitleSize + 10 + most * lineStep + pad - 4;

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
        double baseline = top + pad + cardTitleSize + 10 + lineSize * 0.9;
        for (const Line& line : lines[c]) {
            const double tx = cx + pad + (line.indent ? markW : 0);
            const double midY = baseline - lineSize * 0.36;
            if (line.mark == LineMark::Check) drawCheck(pen.cr, cx + pad + markW * 0.4, midY, lineSize, kText);
            if (line.mark == LineMark::Cross) drawCross(pen.cr, cx + pad + markW * 0.4, midY, lineSize, kText);
            pen.text(tx, baseline, line.text, lineSize, lineColor);
            baseline += lineStep;
        }
        // The recommended one gets a tag on its top edge
        if (args[c] == 2) {
            const double tagSize = 13;
            const double tagH = 22;
            const double tagW = pen.measure(t.outputRecommended, tagSize, true) + 16;
            const double tagX = cx + cardW - tagW - 10;
            fillRounded(pen, tagX, top - tagH / 2, tagW, tagH, 7, kAccent);
            textCentered(pen, tagX + tagW / 2, centerBaseline(top - tagH / 2, tagH, tagSize), t.outputRecommended,
                         tagSize, kOnAccent, true);
        }
        addButton(hit, cx, top, cardW, cardH, usable);
    }
    return top + cardH - y + 8;
}

void EyePanel::drawOutput(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    const EyeStatus& s = m.status;
    double y = kRowTop;
    const double cy = (kRowH - kControlH) / 2;
    const std::string output = v.text(key::kOutput);
    const bool livelink = output == kOutputLivelink;
    const bool etvr = output == kOutputEtvr;

    // Target PC: automatic, fixed to the PC frameeyeosc sends to now, or typed on the keypad; any host set by hand
    // shows in the third choice
    {
        const bool locked = v.locked(key::kHost);
        const std::string host = v.text(key::kHost);
        const bool isAuto = host.empty() || host == "auto";
        const bool canFix = s.running && !hostOfTarget(s.target).empty();
        drawRowLabel(pen, t, y, kRowH, t.rowTarget, t.hintTarget, locked);
        drawSegmented(pen, kControlX, y + cy, kControlW, kControlH,
                      {{t.targetAuto, {PanelAction::HostAuto, key::kHost, 0}},
                       {t.targetFixNow, {PanelAction::FixHost, key::kHost, 0}, canFix},
                       {isAuto ? std::string(t.targetEnter) : formatText(t.targetManualFormat, host),
                        {PanelAction::HostEnter, key::kHost, 0}}},
                      isAuto ? 0 : 2, 19, locked);
    }
    y += kRowH + kRowGap;
    // Port, with the chosen output's default as the hint
    {
        const bool locked = v.locked(key::kPort);
        const bool isDefault = v.portIsDefault();
        const char* hint = livelink ? t.portDefaultLivelink : (etvr ? t.portDefaultEtvr : t.portDefaultVrchat);
        drawRowLabel(pen, t, y, kRowH, t.rowPort, hint, locked);
        drawStepper(pen, kControlX, y + cy, 220, kControlH, key::kPort, v.port(), std::to_string(v.port()), true,
                    locked);
        if (!isDefault && !locked) {
            const PanelHit hit {PanelAction::PortDefault, key::kPort, 0};
            const double bx = kControlX + 232;
            const double bw = 170;
            const int pointer = pointerState(hit);
            fillRounded(pen, bx, y + cy, bw, kControlH, kControlH / 2, pointer > 0 ? kControlHover : kControl);
            strokeRounded(pen, bx, y + cy, bw, kControlH, kControlH / 2, kBorder, 2);
            const double size = fitSize(pen, t.portReset, 18, 12, bw - 24, false);
            textCentered(pen, bx + bw / 2, centerBaseline(y + cy, kControlH, size), t.portReset, size, kText, false);
            addButton(hit, bx, y + cy, bw, kControlH);
        }
    }
    y += kRowH + kRowGap;

    if (!livelink && !etvr) {
        // VRChat directly: the parameter prefix...
        {
            const bool locked = v.locked(key::kPrefix);
            const std::string prefix = v.text(key::kPrefix);
            const bool none = prefix.empty() || prefix == "/";
            const int selected = prefix == "/FT" ? 0 : (none ? 1 : -1);
            const std::string hint = selected < 0 ? formatText(t.prefixOther, prefix) : "";
            drawRowLabel(pen, t, y, kRowH, t.rowPrefix, hint, locked);
            drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                          {{"/FT", {PanelAction::PrefixFt, key::kPrefix, 0}},
                           {t.prefixNone, {PanelAction::PrefixNone, key::kPrefix, 0}}},
                          selected, 20, locked);
            std::string path = prefix;
            while (!path.empty() && path.back() == '/') path.pop_back();
            const std::string example = std::string(t.prefixExample) + "/avatar/parameters" + path + "/v2/EyeLeftX";
            pen.text(kControlX + 4, y + kRowH + 16, example, fitSize(pen, example, 15, 11, kControlW, false),
                     kTextMuted);
        }
        y += kRowH + 24;
        // ...and how EyeTrackingActive goes out
        {
            const bool locked = v.locked(key::kEyeTrackingActive);
            const std::string type = v.text(key::kEyeTrackingActive);
            int selected = -1;
            for (int i = 0; i < 3; ++i) {
                if (type == kActiveTypes[i]) selected = i;
            }
            drawRowLabel(pen, t, y, kRowH, t.rowActiveType, t.hintActiveType, locked);
            drawSegmented(pen, kControlX, y + cy, kControlW, kControlH,
                          {{"Bool", {PanelAction::SetActiveType, key::kEyeTrackingActive, 0}},
                           {"Float", {PanelAction::SetActiveType, key::kEyeTrackingActive, 1}},
                           {t.activeOff, {PanelAction::SetActiveType, key::kEyeTrackingActive, 2}}},
                          selected, 19, locked);
        }
        return;
    }

    // LiveLink and ETVR: what to set up in VRCFT on the PC (nothing else is set here)
    const double boxX = kInnerX;
    const double boxW = kInnerRight - kInnerX;
    const double pad = 20;
    const double top = y + 10;
    const double stepSize = 17;
    const double stepStep = 26;
    const double badgeW = 32;
    const double textW = boxW - pad * 2 - badgeW;
    const char* const* steps = livelink ? t.vrcftStepsLivelink : t.vrcftStepsEtvr;
    std::vector<std::vector<std::string>> stepLines;
    size_t stepLineCount = 0;
    for (int i = 0; i < 3; ++i) {
        stepLines.push_back(wrapText(pen, steps[i], stepSize, false, textW, 2));
        stepLineCount += stepLines.back().size();
    }
    std::string note = t.vrcftSetupNote;
    if (etvr) {
        // A space between English sentences, none after "。"
        if (!note.empty() && note.back() == '.') note += " ";
        note += t.vrcftNoWide;
    }
    const std::vector<std::string> noteLines = wrapText(pen, note, 15, false, boxW - pad * 2, 3);
    const double titleSize = fitSize(pen, t.vrcftSetupTitle, 19, 14, boxW - pad * 2, true);
    const double boxH = pad + titleSize + 14 + stepLineCount * stepStep + 8 + noteLines.size() * 21 + pad - 6;
    strokeRounded(pen, boxX, top, boxW, boxH, 14, kDivider, 2);
    double baseline = top + pad + titleSize * 0.9;
    pen.text(boxX + pad, baseline, t.vrcftSetupTitle, titleSize, kText, true);
    baseline += 14 + stepStep * 0.75;
    for (int i = 0; i < 3; ++i) {
        drawNumberBadge(pen, boxX + pad + 10, baseline - 6, i + 1);
        for (const std::string& line : stepLines[i]) {
            pen.text(boxX + pad + badgeW, baseline, line, stepSize, kText);
            baseline += stepStep;
        }
    }
    baseline += 8 - stepStep * 0.75 + 21 * 0.8;
    for (const std::string& line : noteLines) {
        pen.text(boxX + pad, baseline, line, 15, kTextMuted);
        baseline += 21;
    }
}

void EyePanel::drawGaze(const Pen& pen, const UiText& t, const SettingsView& v) {
    double y = kRowTop;
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
    y += kRowH + kRowGap;
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
    // Independent eyes
    {
        const bool locked = v.locked(key::kIndependentEyes);
        drawRowLabel(pen, t, y, kRowH, t.rowIndependent, t.hintIndependent, locked);
        drawSegmented(pen, kControlX, y + cy, 300, kControlH,
                      {{t.on, {PanelAction::SetBool, key::kIndependentEyes, 1}},
                       {t.off, {PanelAction::SetBool, key::kIndependentEyes, 0}}},
                      v.flag(key::kIndependentEyes) ? 0 : 1, 20, locked);
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
                           const PanelHit& hit, bool usable, bool accent) {
    const int pointer = usable ? pointerState(hit) : 0;
    const bool filled = accent && usable;
    if (filled) {
        fillRounded(pen, x, y, w, h, h / 2, pointer == 2 ? kAccentPressed : kAccent);
    } else {
        fillRounded(pen, x, y, w, h, h / 2, pointer > 0 ? kControlHover : kControl);
        strokeRounded(pen, x, y, w, h, h / 2, usable ? kBorder : kDivider, 2);
    }
    const double size = fitSize(pen, label, 19, 12, w - 24, true);
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
    const bool busy = fit.phase == Phase::Waiting || fit.phase == Phase::Settling || fit.phase == Phase::Capturing ||
                      fit.phase == Phase::Reopen;
    const bool canRun = m.status.running && !anyLocked && !busy;
    const AutoRecenter rewear = autoRecenter(m.config);

    // The one button to press: fit (again); and the re-wear fit, the kind auto_recenter runs by itself when the
    // headset is put on (see auto_recenter.h), re-centering only when that is off
    {
        drawRowLabel(pen, t, y, kRowH, t.rowFit, t.hintFit, false);
        const double gap = 12;
        const double smallW = std::max({190.0, pen.measure(t.fitCenterOnly, 19, true) + 32,
                                        pen.measure(t.fitCenterTilt, 19, true) + 32});
        const double bigW = kControlW - smallW - gap;
        drawButton(pen, kControlX, y + cy, bigW, kControlH, anyFitted ? t.fitAgain : t.fitStart,
                   {PanelAction::FitStart, nullptr, 0}, canRun, true);
        drawButton(pen, kControlX + bigW + gap, y + cy, smallW, kControlH,
                   rewear == AutoRecenter::Tilt ? t.fitCenterTilt : t.fitCenterOnly,
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
                       {t.autoRecenterCenter, {PanelAction::SetAutoRecenter, key::kAutoRecenter, 1}},
                       {t.autoRecenterTilt, {PanelAction::SetAutoRecenter, key::kAutoRecenter, 2}}},
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
        bool stop = false;
        bool reset = false;
        char text[320];
        switch (fit.phase) {
            case Phase::Waiting:
                title = t.fitWaiting;
                paragraphs.push_back(waitingText(t, fit.mode));
                stop = true;
                break;
            case Phase::Settling:
            case Phase::Capturing:
            case Phase::Reopen:
                std::snprintf(text, sizeof(text), t.fitRunningFormat, pointName(t, fit.point), fit.index + 1, fit.count);
                title = text;
                if (fit.attempt > 1) {
                    std::snprintf(text, sizeof(text), t.fitRetryFormat, fit.attempt);
                    title += text;
                }
                paragraphs.push_back(waitingText(t, fit.mode));
                break;
            case Phase::Failed:
                error = true;
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
                                                     : (fit.mode == Mode::Center ? t.fitDoneCenter
                                                        : fit.mode == Mode::Tilt ? t.fitDoneTilt
                                                                                 : t.fitDone);
                    const gaze_fit::Values& r = saved.values;
                    // One line each: the gaze center, the gaze range, and each eye's lid readings
                    if (saved.gazeFitted) {
                        std::snprintf(text, sizeof(text), t.fitGazeCenterFormat, offsetText(r.offsetX).c_str(),
                                      offsetText(r.offsetY).c_str(), rollText(r.rollDeg).c_str());
                        paragraphs.push_back(text);
                        std::snprintf(text, sizeof(text), t.fitGazeRangeFormat, twoDecimals(r.gainX).c_str(),
                                      twoDecimals(r.gainUp).c_str(), twoDecimals(r.gainDown).c_str());
                        paragraphs.push_back(text);
                        if (r.hasEyeX) {
                            std::snprintf(text, sizeof(text), t.fitEyeXFormat, offsetText(r.eyeOffsetX[0]).c_str(),
                                          twoDecimals(r.eyeGainX[0]).c_str(), offsetText(r.eyeOffsetX[1]).c_str(),
                                          twoDecimals(r.eyeGainX[1]).c_str());
                            paragraphs.push_back(text);
                        }
                    } else {
                        paragraphs.push_back(t.fitGazeNone);
                    }
                    if (r.hasLids) {
                        for (int eye = 0; eye < 2; ++eye) {
                            char drop[32];
                            std::snprintf(drop, sizeof(drop), "%+.0f%%", (r.lidDown[eye] / r.lidOpen[eye] - 1) * 100);
                            std::snprintf(text, sizeof(text), t.fitLidFormat, eye == 0 ? t.left : t.right,
                                          twoDecimals(r.lidOpen[eye]).c_str(), twoDecimals(r.lidClosed[eye]).c_str(),
                                          drop);
                            paragraphs.push_back(text);
                        }
                        // An eye that can't widen by itself follows the other one (lid_widen)
                        const WidenState widen = widenState(v);
                        if (widen.mode > 0 && widen.room[0] != widen.room[1]) {
                            paragraphs.push_back(widen.room[1] ? t.widenFollowsLeft : t.widenFollowsRight);
                        } else if (widen.mode > 0 && !widen.room[0] && !widen.room[1]) {
                            paragraphs.push_back(t.widenNoRoom);
                        }
                    } else {
                        paragraphs.push_back(t.fitLidsNone);
                    }
                } else {
                    title = !m.status.running ? t.fitNeedsRunning : (anyLocked ? t.fitLocked : "");
                    error = !title.empty();
                    paragraphs.push_back(t.fitIntro);
                }
                break;
        }
        // Folded down to one line while "Fine-tune" is open
        if (compact) {
            if (title.empty()) title = t.fitNotYet;
            paragraphs.clear();
        }
        const double buttonW = 150;
        const bool button = stop || reset;
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
        if (stop) {
            drawButton(pen, x1 - 20 - buttonW, y + (h - buttonH) / 2, buttonW, buttonH, t.fitStop,
                       {PanelAction::FitStop, nullptr, 0}, true, false);
        } else if (reset) {
            drawButton(pen, x1 - 20 - buttonW, y + (h - buttonH) / 2, buttonW, buttonH, t.fitReset,
                       {PanelAction::FitReset, nullptr, 0}, !anyLocked && !busy, false);
        }
        y += h + (compact ? 8 : 12);
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
        const std::string label = std::string(t.fitDetails) + (fitDetails_ ? "  ▲" : "  ▼");
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
    if (!fitDetails_) return;
    if (fitDetailsPage_ == 0) {
        drawEyeFitGaze(pen, t, v, saved, busy, y);
    } else {
        drawEyeFitLids(pen, t, v, saved, busy, y);
    }
}

void EyePanel::drawEyeFitGaze(const Pen& pen, const UiText& t, const SettingsView& v, const FitInConfig& saved,
                              bool busy, double y) {
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
        // The headset's tilt, on the same row: its name and hint, then its stepper at the right
        const bool tiltLocked = v.locked(key::kGazeRollDeg);
        const double tilt = v.number(key::kGazeRollDeg);
        const double stepperW = 160;
        const double labelX = kControlX + 200 + 16;
        const double labelW = kInnerRight - stepperW - 12 - labelX;
        const char* hint = tiltLocked ? t.locked : t.hintTilt;
        pen.text(labelX, y + h / 2 - 3, t.rowTilt, fitSize(pen, t.rowTilt, 20, 14, labelW, true), kText, true);
        pen.text(labelX, y + h / 2 + 19, hint, fitSize(pen, hint, 15, 11, labelW, false), kTextMuted);
        drawStepper(pen, kInnerRight - stepperW, y + (h - 42) / 2, stepperW, 42, key::kGazeRollDeg, tilt,
                    rollText(tilt), !busy, tiltLocked);
        y += h + kRowGap;
    }
    // Each eye's own sideways zero point and gain (for --independent-eyes), across the whole width
    {
        const double titleSize = 18;
        pen.text(kInnerX, y + 15, t.rowEyeX, titleSize, kText, true);
        const double titleW = pen.measure(t.rowEyeX, titleSize, true);
        pen.text(kInnerX + titleW + 12, y + 15, t.hintEyeX, 15, kTextMuted);
        const char* keys[2][2] = {{key::kGazeOffsetXLeft, key::kGazeGainXLeft},
                                  {key::kGazeOffsetXRight, key::kGazeGainXRight}};
        const char* captions[2] = {t.rowOffset, t.rowGain};
        const double labelW = 40;
        const double gap = 10;
        const double w = (kInnerRight - kInnerX - labelW - gap) / 2;
        const double stepperH = 42;
        for (int i = 0; i < 2; ++i) {
            drawCaption(pen, kInnerX + labelW + i * (w + gap) + 4, y + 35, captions[i], 0,
                        v.locked(keys[0][i]) || v.locked(keys[1][i]));
        }
        for (int eye = 0; eye < 2; ++eye) {
            const double rowY = y + 41 + eye * (stepperH + 4);
            pen.text(kInnerX + 4, centerBaseline(rowY, stepperH, 18), eye == 0 ? t.left : t.right, 18, kText, true);
            for (int i = 0; i < 2; ++i) {
                const char* name = keys[eye][i];
                const double value = v.number(name);
                const std::string text = i == 0 ? offsetText(value) : formatSetting(name, value);
                // Set by fitting; until then each eye uses the shared values
                drawStepper(pen, kInnerX + labelW + i * (w + gap), rowY, w, stepperH, name, value, text,
                            !busy && saved.eyeXFitted, v.locked(name));
            }
        }
    }
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
    const EyeStatus& s = m.status;
    cairo_t* cr = pen.cr;
    double y = kRowTop;
    const double cy = (kRowH - kControlH) / 2;
    // Eyes with an eye fit: closing and opening come from the fit, widening from lid_widen. The learned calibration
    // and the lid marks only matter for eyes without one, so the Widen row takes the calibration's place and the
    // marks fold away behind "Fine-tune"
    const WidenState widen = widenState(v);
    const bool anyFitted = widen.fitted[0] || widen.fitted[1];

    if (anyFitted) {
        const bool locked = v.locked(key::kLidWiden);
        drawRowLabel(pen, t, y, kRowH, t.rowWiden, t.hintWiden, locked);
        std::vector<Option> options;
        for (int i = 0; i < 4; ++i) options.push_back({t.widenModes[i], {PanelAction::SetLidWiden, key::kLidWiden, i}});
        drawSegmented(pen, kControlX, y + cy, kControlW, kControlH, options, widen.mode, 19, locked);
    }
    // Auto calibration: on / off, what it learned, reset
    else {
        const bool locked = v.locked(key::kLidCalibration);
        const bool on = v.flag(key::kLidCalibration);
        std::string learned;
        const FitInConfig saved = fitInConfig(v);
        const bool lidFit = saved.lidsFitted[0] && saved.lidsFitted[1];
        if (lidFit) {
            learned = t.lidFitInUse;
        } else if (s.running && (std::isfinite(s.relaxed.v[0]) || std::isfinite(s.relaxed.v[1]))) {
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
        if (s.running && s.learning && !lidFit) {
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
    }
    y += kRowH + kRowGap;
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
    // Fitted: a line saying what decides the eyelids (or that an eye follows the other one), and "Fine-tune" to open
    // the marks
    const bool showMarks = !anyFitted || lidMarksOpen_;
    const double marksShift = anyFitted ? 8 : 0;
    if (anyFitted) {
        const double bw = 150;
        const std::string label = std::string(t.fitDetails) + (lidMarksOpen_ ? "  ▲" : "  ▼");
        drawButton(pen, kInnerRight - bw, y, bw, 28, label, {PanelAction::LidMarks, nullptr, 0}, true, false);
        const char* note = lidMarksOpen_ ? t.lidMarksUnused : t.lidMarksFitted;
        bool notice = false;
        if (widen.mode > 0 && widen.fitted[0] && widen.fitted[1]) {
            notice = widen.room[0] != widen.room[1] || !widen.room[0];
            if (!widen.room[0] && widen.room[1]) note = t.widenFollowsLeft;
            if (widen.room[0] && !widen.room[1]) note = t.widenFollowsRight;
            if (!widen.room[0] && !widen.room[1]) note = t.widenNoRoom;
        }
        pen.text(kInnerX, y + 18, note, fitSize(pen, note, 15, 11, kInnerRight - bw - 12 - kInnerX, notice),
                 notice ? kText : kTextMuted, notice);
    }
    // The raw openness of each eye with the four marks laid over it
    {
        const double top = y + marksShift;
        if (!anyFitted) {
            pen.text(kInnerX, top + 16, t.marksTitle,
                     fitSize(pen, t.marksTitle, 15, 11, kInnerRight - kInnerX, false), kTextMuted);
        }
        const double barX = kInnerX + 34;
        const double barW = kInnerRight - barX;
        /**
         * x of an openness value on the bars.
         */
        const auto xOf = [&](double value) { return barX + std::clamp(value / kLidScaleMax, 0.0, 1.0) * barW; };
        const char* marks[4] = {key::kLidClosed, key::kLidOpen, key::kLidWidenStart, key::kLidWide};
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
        for (int i = 0; i < (showMarks ? 4 : 0); ++i) {
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
    }
    y += 110 + marksShift;
    // The four marks (3 and 4 greyed for fitted eyes, which widen by lid_widen)
    if (showMarks) {
        const char* marks[4] = {key::kLidClosed, key::kLidOpen, key::kLidWidenStart, key::kLidWide};
        const char* captions[4] = {t.markClosed, t.markOpen, t.markWidenStart, t.markWide};
        const double gap = 12;
        const double w = (kInnerRight - kInnerX - gap * 3) / 4;
        for (int i = 0; i < 4; ++i) {
            const double x = kInnerX + i * (w + gap);
            drawCaption(pen, x + 2, y + 18, captions[i], i + 1, v.locked(marks[i]));
            double low = 0;
            double high = 0;
            lidMarkBounds(marks[i], v, low, high);
            const double value = v.number(marks[i]);
            const bool usable = !(anyFitted && i >= 2);
            drawStepper(pen, x, y + 28, w, kControlH, marks[i], value, formatSetting(marks[i], value), usable,
                        v.locked(marks[i]), low, high);
        }
        y += kCaptionRowH + kRowGap;
    }
    // Sync both lids
    {
        const bool locked = v.locked(key::kLidSync);
        const double value = v.number(key::kLidSync);
        drawRowLabel(pen, t, y, kRowH, t.rowSync, t.hintSync, locked);
        drawStepper(pen, kControlX, y + cy, 220, kControlH, key::kLidSync, value, formatSetting(key::kLidSync, value),
                    true, locked);
    }
    y += kRowH + kRowGap;
    // Blinks: how long a closed eye stays closed, and closing both when one is closed (0 = off for each)
    {
        const bool lockedHold = v.locked(key::kBlinkHoldMs);
        const bool lockedSync = v.locked(key::kBlinkSyncBelow);
        drawRowLabel(pen, t, y, kRowH, t.rowBlink, t.hintBlink, lockedHold || lockedSync);
        const double labelW = 52;
        const double gap = 12;
        const double stepperW = (kControlW - labelW * 2 - gap) / 2;
        const double hold = v.number(key::kBlinkHoldMs);
        char holdText[32];
        std::snprintf(holdText, sizeof(holdText), "%.0f ms", hold);
        const double sync = v.number(key::kBlinkSyncBelow);
        for (int i = 0; i < 2; ++i) {
            const double lx = kControlX + i * (labelW + stepperW + gap);
            const char* label = i == 0 ? t.blinkHold : t.blinkSync;
            pen.text(lx, centerBaseline(y, kRowH, 18), label, fitSize(pen, label, 18, 12, labelW - 6, true), kText,
                     true);
            if (i == 0) {
                drawStepper(pen, lx + labelW, y + cy, stepperW, kControlH, key::kBlinkHoldMs, hold, holdText, true,
                            lockedHold);
            } else {
                drawStepper(pen, lx + labelW, y + cy, stepperW, kControlH, key::kBlinkSyncBelow, sync,
                            formatSetting(key::kBlinkSyncBelow, sync), true, lockedSync);
            }
        }
    }
    y += kRowH + kRowGap;
    // Eyelid smoothing (two One Euro values)
    {
        drawRowLabel(pen, t, y, kCaptionRowH, t.rowLidSmooth, t.lowerSmoother, false);
        const char* keys[2] = {key::kLidMinCutoff, key::kLidBeta};
        const char* captions[2] = {t.capStill, t.capFast};
        for (int i = 0; i < 2; ++i) {
            const double x = kControlX + i * 232;
            drawCaption(pen, x + 4, y + 17, captions[i], 0, v.locked(keys[i]));
            const double value = v.number(keys[i]);
            drawStepper(pen, x, y + 28, 220, kControlH, keys[i], value, formatSetting(keys[i], value), true,
                        v.locked(keys[i]));
        }
    }
}

double EyePanel::drawSectionTitle(const Pen& pen, double y, const std::string& title) {
    pen.text(kInnerX, y + 20, title, 15, kTextMuted, true);
    pen.color(kDivider);
    cairo_set_line_width(pen.cr, 1);
    cairo_move_to(pen.cr, kInnerX, y + 29.5);
    cairo_line_to(pen.cr, kInnerRight, y + 29.5);
    cairo_stroke(pen.cr);
    return 36;
}

void EyePanel::drawAdvanced(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v) {
    const EyeStatus& s = m.status;
    double y = kRowTop - 6;

    // Version, new release check and install, and the automatic check
    drawUpdateRow(pen, t, m.update, v.flag(key::kUpdateCheck), y);
    y += kUpdateRowH + 10;

    y += drawSectionTitle(pen, y, t.sectionTools);
    // The debug gaze dots (a head-locked dot where the sent gaze points)
    {
        const double h = 60;
        const bool locked = v.locked(key::kGazeDebugDots);
        const bool on = v.flag(key::kGazeDebugDots);
        const double top = y + (h - kControlH) / 2;
        drawRowLabel(pen, t, y, h, t.rowGazeDots, t.hintGazeDots, locked);
        drawSegmented(pen, kControlX, top, 200, kControlH,
                      {{t.on, {PanelAction::SetBool, key::kGazeDebugDots, 1}},
                       {t.off, {PanelAction::SetBool, key::kGazeDebugDots, 0}}},
                      on ? 0 : 1, 20, locked);
        // How far ahead the dots are: "Dot distance  − 1.0 m ＋", greyed out while the dots are off
        const double stepperW = 180;
        const double stepperX = kInnerRight - stepperW;
        const double captionRight = stepperX - 12;
        const double captionLeft = kControlX + 200 + 12;
        const double captionSize = fitSize(pen, t.dotDistance, 16, 12, captionRight - captionLeft, false);
        const double captionW = pen.measure(t.dotDistance, captionSize, false);
        pen.text(captionRight - captionW, centerBaseline(y, h, captionSize), t.dotDistance, captionSize,
                 on ? kText : kTextMuted);
        const double distance = v.number(key::kGazeDebugDotsDistanceM);
        drawStepper(pen, stepperX, top, stepperW, kControlH, key::kGazeDebugDotsDistanceM, distance,
                    formatSetting(key::kGazeDebugDotsDistanceM, distance) + " m", on, false);
        y += h + 8;
    }
    // The eye log: record the raw eye data to a file, from now until "Stop" (or 60 minutes)
    {
        const double h = 52;
        const recorder::View& r = m.recording;
        drawRowLabel(pen, t, y, h, t.rowEyeLog, "", false);
        const std::string label =
            r.recording ? formatText(t.eyeLogStopFormat, recorder::elapsedText(r.elapsedSec)) : std::string(t.eyeLogRecord);
        const double buttonW = 160;
        drawButton(pen, kControlX, y + (h - 40) / 2, buttonW, 40, label, {PanelAction::RecordToggle, nullptr, 0}, true,
                   false);
        const double textX = kControlX + buttonW + 14;
        const double textW = kInnerRight - textX;
        // Two short lines next to the button: where the files go and the limit, or why it could not record
        std::vector<std::string> lines;
        Color color = kTextMuted;
        bool bold = false;
        if (!r.error.empty() && !r.recording) {
            lines = wrapText(pen, formatText(t.eyeLogFailedFormat, r.error), 14, true, textW, 2);
            color = kDanger;
            bold = true;
        } else {
            const std::string where = formatText(t.eyeLogWhereFormat, recorder::shortPath(recorder::defaultDir()));
            // After the 60-minute limit stopped it: say so instead of the limit
            lines = {ellipsize(pen, where, 14, false, textW, true), r.autoStopped ? t.eyeLogAutoStopped : t.eyeLogLimit};
            if (r.autoStopped) {
                color = kText;
            }
        }
        double baseline = y + h / 2 - (lines.size() - 1) * 9 + 5;
        for (const std::string& line : lines) {
            pen.text(textX, baseline, line, 14, color, bold);
            baseline += 18;
        }
        y += h + 10;
    }

    y += drawSectionTitle(pen, y, t.sectionFiles);
    // Read-only rows in smaller type: the title (muted) on the left, the text on the right
    const double infoH = 28;
    const double infoSize = 15;
    const auto infoRow = [&](const std::string& title, const std::string& value, bool keepEnd) {
        const double baseline = y + infoH / 2 + infoSize * 0.36;
        pen.text(kInnerX, baseline, title, fitSize(pen, title, infoSize, 11, kLabelW, false), kTextMuted);
        pen.text(kControlX, baseline, ellipsize(pen, value, infoSize, false, kControlW, keepEnd), infoSize, kText);
        y += infoH;
    };
    // File locations
    infoRow(t.rowConfigPath, m.configPath, true);
    if (s.running && !s.configPath.empty() && s.configPath != m.configPath) {
        const std::string warning = t.configPathMismatch + s.configPath;
        pen.text(kControlX, y + 14, ellipsize(pen, warning, 14, true, kControlW, true), 14, kDanger, true);
        y += 22;
    }
    infoRow(t.rowCalibrationPath, s.running && !s.calibrationPath.empty() ? s.calibrationPath : "—", true);
    infoRow(t.rowStatusPath, m.statusPath, true);
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
    // Locked by the command line, with the values in effect (up to two lines; where they come from under the title)
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
            if (!lines.empty() && pen.measure(lines.back() + ",  " + item, infoSize, false) <= kControlW) {
                lines.back() += ",  " + item;
            } else {
                lines.push_back(ellipsize(pen, item, infoSize, false, kControlW, false));
            }
        }
        if (lines.size() > 2) lines.resize(2);
        const double baseline = y + infoH / 2 + infoSize * 0.36;
        pen.text(kInnerX, baseline, t.rowLockedList, fitSize(pen, t.rowLockedList, infoSize, 11, kLabelW, false),
                 kTextMuted);
        pen.text(kInnerX, baseline + 20, t.hintLockedList, fitSize(pen, t.hintLockedList, 13, 10, kLabelW, false),
                 kTextMuted);
        for (size_t i = 0; i < lines.size(); ++i) pen.text(kControlX, baseline + i * 22, lines[i], infoSize, kText);
    }
}

void EyePanel::drawUpdateRow(const Pen& pen, const UiText& t, const frame_updater::UpdateStatus& u, bool checkOn,
                              double y) {
    using frame_updater::UpdateState;
    const double h = kUpdateRowH;
    const std::string current = bareVersion(u.current);
    std::string hint = "v" + current;
    if (u.checkedAt > 0) hint += formatText(t.checkedFormat, checkedText(u.checkedAt));
    drawRowLabel(pen, t, y, h, t.rowVersion, hint, false);

    /** A button at the right end of the row. */
    struct RowButton {
        PanelHit hit;
        const char* label;
        bool accent;
    };
    const PanelHit check {PanelAction::UpdateCheck, nullptr, 0};
    const PanelHit dismiss {PanelAction::UpdateDismiss, nullptr, 0};
    const PanelHit install {PanelAction::UpdateInstall, nullptr, 0};
    std::string primary;    // bold
    std::string secondary;  // muted
    bool error = false;     // primary in red
    std::vector<RowButton> buttons;
    switch (u.state) {
        case UpdateState::Unknown:
            if (u.checking) primary = t.updateChecking;
            buttons.push_back({check, t.updateCheckNow, false});
            break;
        case UpdateState::UpToDate:
            primary = formatText(t.updateUpToDateFormat, current);
            buttons.push_back({check, t.updateCheckNow, false});
            break;
        case UpdateState::Available:
            primary = formatText(t.updateAvailableFormat, bareVersion(u.latest));
            if (u.installable) {
                buttons.push_back({install, t.updateButton, true});
            } else {
                secondary = t.updateManual;
                buttons.push_back({check, t.updateCheckNow, false});
            }
            break;
        case UpdateState::Installing:
            primary = formatText(t.updateInstallingFormat, updateStepText(t, u.step));
            break;
        case UpdateState::Installed:
            primary = formatText(t.updateInstalledFormat, bareVersion(u.version));
            buttons.push_back({dismiss, t.updateDismiss, false});
            break;
        case UpdateState::CheckFailed:
            primary = std::string(t.updateCheckFailed) + " " + updateReasonText(t, u.error);
            error = true;
            buttons.push_back({check, t.updateCheckNow, false});
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

    // Buttons at the right end; two are stacked so the texts keep their width
    const double gap = 8;
    const double bw = 170;
    const double bh = buttons.size() > 1 ? (h - gap) / 2 : kControlH;
    const double left = kInnerRight - bw - 12;
    for (size_t i = 0; i < buttons.size(); ++i) {
        const RowButton& b = buttons[i];
        const double bx = kInnerRight - bw;
        const double by = buttons.size() > 1 ? y + i * (bh + gap) : y + (h - bh) / 2;
        // A check can't be started while one runs
        const bool usable = !(b.hit.action == PanelAction::UpdateCheck && u.checking);
        const int pointer = usable ? pointerState(b.hit) : 0;
        if (b.accent) {
            fillRounded(pen, bx, by, bw, bh, bh / 2, pointer == 2 ? kAccentPressed : kAccent);
        } else {
            fillRounded(pen, bx, by, bw, bh, bh / 2, pointer > 0 ? kControlHover : kControl);
            strokeRounded(pen, bx, by, bw, bh, bh / 2, usable ? kBorder : kDivider, 2);
        }
        const double size = fitSize(pen, b.label, 19, 12, bw - 20, true);
        const Color labelColor = b.accent ? kOnAccent : (usable ? kText : kTextDisabled);
        textCentered(pen, bx + bw / 2, centerBaseline(by, bh, size), b.label, size, labelColor, true);
        addButton(b.hit, bx, by, bw, bh, usable);
    }

    // The automatic check, as a chip at the bottom of the text column: "Check at start and daily  On"; pressing it
    // switches it
    const double chipH = 34;
    const double chipY = y + h - chipH - 2;
    {
        const PanelHit hit {PanelAction::SetBool, key::kUpdateCheck, checkOn ? 0 : 1};
        const std::string state = checkOn ? t.on : t.off;
        const double labelSize = 15;
        const double chipMax = left - kControlX;
        const double stateW = pen.measure(state, labelSize, true);
        const double labelRoom = chipMax - 28 - 10 - stateW;
        const std::string label = ellipsize(pen, t.updateCheckChip, labelSize, false, labelRoom, false);
        const double labelW = pen.measure(label, labelSize, false);
        const double chipW = labelW + 10 + stateW + 28;
        const int pointer = pointerState(hit);
        fillRounded(pen, kControlX, chipY, chipW, chipH, chipH / 2, pointer > 0 ? kControlHover : kControl);
        strokeRounded(pen, kControlX, chipY, chipW, chipH, chipH / 2, kBorder, 2);
        const double baseline = centerBaseline(chipY, chipH, labelSize);
        pen.text(kControlX + 14, baseline, label, labelSize, kText);
        pen.text(kControlX + 14 + labelW + 10, baseline, state, labelSize, checkOn ? kAccent : kTextMuted, true);
        addButton(hit, kControlX, chipY, chipW, chipH);
    }

    // The texts, wrapped to at most four lines and centered above the chip
    const double textW = (buttons.empty() ? kInnerRight : left) - kControlX;
    const double textH = chipY - y - 4;
    const double primarySize = 17;
    const double secondarySize = 14;
    const size_t maxLines = 4;
    const std::vector<std::string> primaryLines =
        primary.empty() ? std::vector<std::string>() : wrapText(pen, primary, primarySize, true, textW, secondary.empty() ? 4 : 3);
    const std::vector<std::string> secondaryLines =
        secondary.empty() || primaryLines.size() >= maxLines
            ? std::vector<std::string>()
            : wrapText(pen, secondary, secondarySize, false, textW, maxLines - primaryLines.size());
    const double primaryStep = 22;
    const double secondaryStep = 20;
    double baseline = y + (textH - primaryLines.size() * primaryStep - secondaryLines.size() * secondaryStep) / 2;
    for (const std::string& line : primaryLines) {
        baseline += primaryStep;
        pen.text(kControlX, baseline - 6, line, primarySize, error ? kDanger : kText, true);
    }
    for (const std::string& line : secondaryLines) {
        baseline += secondaryStep;
        pen.text(kControlX, baseline - 5, line, secondarySize, kTextMuted);
    }
}

void EyePanel::drawPrompt(const Pen& pen, const UiText& t) {
    // Only the prompt's buttons stay usable
    buttons_.clear();
    cairo_set_source_rgba(pen.cr, 0, 0, 0, 0.62);
    pen.roundedRect(0, 0, kWidth, kHeight, 24);
    cairo_fill(pen.cr);
    const bool update = !updatePromptVersion_.empty();
    const bool etvr = promptOutput_ == kOutputEtvr;
    const bool livelink = promptOutput_ == kOutputLivelink;
    const double w = 760;
    const double h = 270;
    const double x = (kWidth - w) / 2;
    const double y = (kHeight - h) / 2;
    drawCard(pen, x, y, w, h, 24, kCard, kAccent, 2);
    const std::string title = update ? formatText(t.updateConfirmFormat, updatePromptVersion_)
                                     : (etvr ? t.promptEtvr : (livelink ? t.promptLivelink : t.promptVrchat));
    const double titleSize = fitSize(pen, title, 24, 16, w - 60, true);
    textCentered(pen, x + w / 2, y + 62, title, titleSize, kText, true);
    std::string detail1 = etvr ? t.promptEtvrDetail1 : (livelink ? t.promptLivelinkDetail1 : t.promptVrchatDetail1);
    std::string detail2 = etvr ? t.promptEtvrDetail2 : (livelink ? t.promptLivelinkDetail2 : t.promptVrchatDetail2);
    if (update) {
        const std::vector<std::string> lines = wrapText(pen, t.updateConfirmHint, 17, false, w - 60, 2);
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
        const std::string label = update ? (yes ? t.updateConfirmYes : t.updateConfirmNo) : (yes ? t.promptYes : t.promptNo);
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

void EyePanel::drawHostEntry(const Pen& pen, const UiText& t) {
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
    textCentered(pen, x + w / 2, y + 52, t.hostEntryTitle, fitSize(pen, t.hostEntryTitle, 24, 16, w - 60, true), kText,
                 true);

    // What is typed, with a caret
    const double fx = x + 40;
    const double fy = y + 78;
    const double fw = w - 80;
    const double fh = 60;
    fillRounded(pen, fx, fy, fw, fh, 14, kControl);
    strokeRounded(pen, fx, fy, fw, fh, 14, kAccent, 2);
    const double textSize = fitSize(pen, hostEntryText_ + "|", 28, 14, fw - 40, true);
    const double tw = pen.text(fx + 20, centerBaseline(fy, fh, textSize), hostEntryText_, textSize, kText, true);
    pen.color(kAccent);
    cairo_rectangle(pen.cr, fx + 22 + tw, fy + 14, 2.5, fh - 28);
    cairo_fill(pen.cr);
    // Why it can't be used, or how to type it
    const bool error = !hostEntryError_.empty();
    const std::string below = error ? hostEntryError_ : std::string(t.hostEntryHint);
    pen.text(fx + 4, fy + fh + 30, below, fitSize(pen, below, 17, 12, fw - 8, error), error ? kDanger : kTextMuted,
             error);

    // The keypad: 1-9, then ".", "0" and backspace
    const double keyW = 124;
    const double keyH = 64;
    const double gap = 12;
    const double kx = x + 40;
    const double ky = y + 196;
    const int keys[12] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', host_entry::kBackspace};
    for (int i = 0; i < 12; ++i) {
        const double bx = kx + (i % 3) * (keyW + gap);
        const double by = ky + (i / 3) * (keyH + gap);
        const PanelHit hit {PanelAction::HostKey, nullptr, keys[i]};
        const int pointer = pointerState(hit);
        fillRounded(pen, bx, by, keyW, keyH, 16, pointer == 2 ? kAccentPressed : (pointer == 1 ? kControlHover : kControl));
        strokeRounded(pen, bx, by, keyW, keyH, 16, kBorder, 2);
        const Color color = pointer == 2 ? kOnAccent : kText;
        if (keys[i] == host_entry::kBackspace) {
            // A left-pointing key shape with an x, drawn (no font needed)
            const double cx = bx + keyW / 2;
            const double cy = by + keyH / 2;
            cairo_t* cr = pen.cr;
            pen.color(color);
            cairo_set_line_width(cr, 2.5);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_move_to(cr, cx - 22, cy);
            cairo_line_to(cr, cx - 10, cy - 13);
            cairo_line_to(cr, cx + 22, cy - 13);
            cairo_line_to(cr, cx + 22, cy + 13);
            cairo_line_to(cr, cx - 10, cy + 13);
            cairo_close_path(cr);
            cairo_stroke(cr);
            cairo_move_to(cr, cx - 1, cy - 6);
            cairo_line_to(cr, cx + 11, cy + 6);
            cairo_move_to(cr, cx + 11, cy - 6);
            cairo_line_to(cr, cx - 1, cy + 6);
            cairo_stroke(cr);
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
    } side[2] = {{{PanelAction::HostOk, nullptr, 0}, t.hostEntryOk, true, ky, 3 * keyH + 2 * gap},
                 {{PanelAction::HostCancel, nullptr, 0}, t.hostEntryCancel, false, ky + 3 * (keyH + gap), keyH}};
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

void EyePanel::render(const PanelModel& model) {
    const Pen pen {cr_, &fonts_};
    const UiText& t = uiText(model.language);
    const SettingsView view(model);
    buttons_.clear();

    // Opaque background (the contrast ratios assume it)
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);
    fillRounded(pen, 0, 0, kWidth, kHeight, 24, kBg);

    drawStatus(pen, t, model);
    drawTabs(pen, t);
    drawCard(pen, kRightX, kContentY, kRight - kRightX, kContentH, 20, kCard, kDivider, 1);
    switch (tab_) {
        case PanelTab::Basic: drawBasic(pen, t, model, view); break;
        case PanelTab::Output: drawOutput(pen, t, model, view); break;
        case PanelTab::Gaze: drawGaze(pen, t, view); break;
        case PanelTab::EyeFit: drawEyeFit(pen, t, model, view); break;
        case PanelTab::Lids: drawLids(pen, t, model, view); break;
        case PanelTab::Advanced: drawAdvanced(pen, t, model, view); break;
    }
    if (promptOpen()) drawPrompt(pen, t);
    if (hostEntryOpen_) drawHostEntry(pen, t);

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
