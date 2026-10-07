// Colors and the contrast ratio report.
#include "theme.h"

#include <cmath>
#include <cstdio>

namespace {

/**
 * Convert one sRGB channel to linear light (the WCAG 2.x formula).
 * @param c 0..1
 * @return the linear value
 */
double linearChannel(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/**
 * Format a color as #rrggbb.
 * @param c the color
 * @return the text
 */
std::string hexText(Color c) {
    char text[16];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", static_cast<int>(std::lround(c.r * 255)),
                  static_cast<int>(std::lround(c.g * 255)), static_cast<int>(std::lround(c.b * 255)));
    return text;
}

/**
 * The name of a kind (for the report).
 * @param kind the kind
 * @return the name
 */
const char* kindName(ContrastKind kind) {
    switch (kind) {
        case ContrastKind::Text: return "text";
        case ContrastKind::Ui: return "ui";
        case ContrastKind::Disabled: return "disabled (target)";
    }
    return "";
}

}  // namespace

double relativeLuminance(Color c) {
    return 0.2126 * linearChannel(c.r) + 0.7152 * linearChannel(c.g) + 0.0722 * linearChannel(c.b);
}

double contrastRatio(Color a, Color b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    const double light = la > lb ? la : lb;
    const double dark = la > lb ? lb : la;
    return (light + 0.05) / (dark + 0.05);
}

double requiredRatio(ContrastKind kind) {
    return kind == ContrastKind::Text ? 4.5 : 3.0;
}

const std::vector<ContrastPair>& contrastPairs() {
    // Every pair drawn in panel.cpp (all text is checked at 4.5:1 whatever its size)
    static const std::vector<ContrastPair> pairs = {
        // Text on backgrounds
        {"body text (panel background)", kText, kBg, ContrastKind::Text},
        {"titles, values, captions (card)", kText, kCard, ContrastKind::Text},
        {"hints, headings, legends (card)", kTextMuted, kCard, ContrastKind::Text},
        {"\"no eye data\" (bar and gaze box track)", kTextMuted, kBg, ContrastKind::Text},
        // Buttons, pills, tabs
        {"button text", kText, kControl, ContrastKind::Text},
        {"button text (hovered / pressed)", kText, kControlHover, ContrastKind::Text},
        {"selected text (accent fill)", kOnAccent, kAccent, ContrastKind::Text},
        {"selected text (pressed)", kOnAccent, kAccentPressed, ContrastKind::Text},
        {"locked value, waiting / paused badge (pill)", kTextMuted, kControl, ContrastKind::Text},
        {"text of buttons that can't be pressed", kTextDisabled, kControl, ContrastKind::Disabled},
        {"quit / reset button text", kText, kQuitFill, ContrastKind::Text},
        {"quit / reset confirmation text (red fill)", kOnAccent, kDanger, ContrastKind::Text},
        // States
        {"\"Sending\" badge", kSuccess, kSuccessTint, ContrastKind::Text},
        {"\"not running\" badge", kText, kDangerTint, ContrastKind::Text},
        {"error line (card)", kDanger, kCard, ContrastKind::Text},
        {"new release notice text (accent tint)", kText, kAccentTint, ContrastKind::Text},
        {"destination card lines (card; chosen or hovered ones use body text)", kTextMuted, kControl, ContrastKind::Text},
        {"chosen destination card text (accent tint)", kText, kAccentTint, ContrastKind::Text},
        {"eye camera calibration: done chip, calibration chip, the first tab's note (accent tint)", kText, kAccentTint,
         ContrastKind::Text},
        {"update check chip \"On\" (pill)", kAccent, kControl, ContrastKind::Text},
        {"update check chip \"On\" (hovered)", kAccent, kControlHover, ContrastKind::Text},
        // Controls (WCAG 1.4.11)
        {"control outlines, bar tracks (card)", kBorder, kCard, ContrastKind::Ui},
        {"locked choice outline (pill)", kBorder, kControl, ContrastKind::Ui},
        {"selected choice fill (pill)", kAccent, kControl, ContrastKind::Ui},
        {"selected tab fill and notch (panel background)", kAccent, kBg, ContrastKind::Ui},
        {"learning dot, tab notch (card)", kAccent, kCard, ContrastKind::Ui},
        {"new release notice outline and dot (accent tint)", kAccent, kAccentTint, ContrastKind::Ui},
        {"eye camera calibration: chip outline and check, the note's button (accent tint)", kAccent, kAccentTint,
         ContrastKind::Ui},
        {"eye camera calibration: \"not yet\" ring (pill)", kTextMuted, kControl, ContrastKind::Ui},
        {"eye camera setup: steps done (card)", kSuccess, kCard, ContrastKind::Text},
        {"eye camera setup: what it sees, its seconds left (dark box)", kSuccess, kBg, ContrastKind::Text},
        {"eye camera setup: the seconds left (dark box)", kAccent, kBg, ContrastKind::Text},
        {"diagnostics: a problem now (dark card)", kDanger, kBg, ContrastKind::Text},
        {"eye camera setup: the left column's next step (card)", kAccent, kCard, ContrastKind::Text},
        {"eye camera setup: ready (green tint)", kText, kSuccessTint, ContrastKind::Text},
        {"eye camera setup: steps to come, the chip's chevrons (card)", kBorder, kCard, ContrastKind::Ui},
        {"quit / reset outline (card)", kDanger, kCard, ContrastKind::Ui},
        {"- / + signs", kText, kControl, ContrastKind::Ui},
        {"- / + signs (hovered)", kText, kControlHover, ContrastKind::Ui},
        {"- / + signs (pressed)", kOnAccent, kAccentPressed, ContrastKind::Ui},
        {"- / + signs that can't be pressed", kTextDisabled, kControl, ContrastKind::Disabled},
        {"badge dot (\"Sending\")", kSuccess, kSuccessTint, ContrastKind::Ui},
        {"badge cross (\"not running\")", kDanger, kDangerTint, ContrastKind::Ui},
        {"badge ring / pause bars", kTextMuted, kControl, ContrastKind::Ui},
        // Live values
        {"sent eyelid bar, raw openness bar (track)", kAccent, kBg, ContrastKind::Ui},
        {"raw eyelid bar (track)", kTextMuted, kBg, ContrastKind::Ui},
        {"raw gaze ring, lid mark lines (track)", kText, kBg, ContrastKind::Ui},
        {"sent gaze dot (gaze box)", kAccent, kBg, ContrastKind::Ui},
        {"lid mark line edges (accent fill)", kBg, kAccent, ContrastKind::Ui},
        {"lid mark lines between the bars (card)", kText, kCard, ContrastKind::Ui},
        {"numbered mark circles (card)", kAccent, kCard, ContrastKind::Ui},
        {"eye fit done check mark (card)", kSuccess, kCard, ContrastKind::Ui},
        // The eye fit target (drawn on its own dark disc, shown over the scene)
        {"target dot (disc)", kText, kBg, ContrastKind::Ui},
        {"target ring (disc)", kAccent, kBg, ContrastKind::Ui},
        {"target seconds (disc)", kTextMuted, kBg, ContrastKind::Text},
        {"target words (disc)", kText, kBg, ContrastKind::Text},
        {"target countdown (disc)", kAccent, kBg, ContrastKind::Text},
        // The debug gaze dots (each on its own dark edge, shown over the scene)
        {"debug gaze dot, combined (dark edge)", kDotBoth, kBg, ContrastKind::Ui},
        {"debug gaze dot, left eye (dark edge)", kDotLeft, kBg, ContrastKind::Ui},
        {"debug gaze dot, right eye (dark edge)", kDotRight, kBg, ContrastKind::Ui},
        // The left column's two-eye gaze pads (independent eyes): the same eye colors on the pad background
        {"left eye's sent gaze dot (gaze pad)", kDotLeft, kBg, ContrastKind::Ui},
        {"right eye's sent gaze dot (gaze pad)", kDotRight, kBg, ContrastKind::Ui},
        {"eye colors in the gaze legend (card)", kDotRight, kCard, ContrastKind::Ui},
        // The Advanced tab's sub-tabs and the records
        {"Advanced tab's sub-tabs: outline (panel background)", kBorder, kBg, ContrastKind::Ui},
        {"records: \"Failed\" badge, a failure's \"Why\" (red tint)", kDangerSoft, kDangerTint, ContrastKind::Text},
        {"records: a failure's box outline (red tint)", kDanger, kDangerTint, ContrastKind::Ui},
        {"records: \"One eye\" badge, its \"Why\" (accent tint)", kAccent, kAccentTint, ContrastKind::Text},
        {"records: \"OK\" badge, its \"Result\" (green tint)", kSuccess, kSuccessTint, ContrastKind::Text},
        {"records: \"Stopped\" badge, its \"Why\" (pill-colored)", kTextMuted, kControl, ContrastKind::Text},
        {"records: a stop's box outline (pill-colored)", kBorder, kControl, ContrastKind::Ui},
        {"records: card titles (pill-colored card)", kAccent, kControl, ContrastKind::Text},
        {"records: the saved bar's sheet (pill-colored card)", kSuccess, kControl, ContrastKind::Ui},
    };
    return pairs;
}

int printContrastReport() {
    int failures = 0;
    double lowest = 100.0;
    const ContrastPair* lowestPair = nullptr;
    std::printf("%-6s %-7s %-7s %6s %5s  %s\n", "result", "fg", "bg", "ratio", "need", "where");
    for (const ContrastPair& pair : contrastPairs()) {
        const double ratio = contrastRatio(pair.fg, pair.bg);
        const double need = requiredRatio(pair.kind);
        const bool ok = ratio >= need;
        if (!ok) ++failures;
        if (ratio < lowest) {
            lowest = ratio;
            lowestPair = &pair;
        }
        std::printf("%-6s %s %s %6.2f %5.1f  %s (%s)\n", ok ? "pass" : "FAIL", hexText(pair.fg).c_str(),
                    hexText(pair.bg).c_str(), ratio, need, pair.what, kindName(pair.kind));
    }
    if (lowestPair != nullptr) {
        std::printf("lowest ratio: %.2f (%s: %s / %s)\n", lowest, lowestPair->what, hexText(lowestPair->fg).c_str(),
                    hexText(lowestPair->bg).c_str());
    }
    std::printf("%d of %zu pairs fail\n", failures, contrastPairs().size());
    return failures == 0 ? 0 : 1;
}
