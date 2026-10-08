// Screen colors (kept in this one place) and the WCAG 2.x contrast ratio.
// Drawing code only uses these colors, and --contrast-report checks the same pairs and prints pass / fail.
#pragma once

#include "draw.h"

#include <string>
#include <vector>

/**
 * Make a color from hex RGB (e.g. 0xe27dfd).
 * @param rgb 0xRRGGBB
 * @return the color (0..1)
 */
constexpr Color hexColor(unsigned rgb) {
    return {((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0};
}

/**
 * Mix two colors (what a translucent color looks like over an opaque background).
 * @param top the color on top
 * @param bottom the color below
 * @param alpha opacity of the top color (0..1)
 * @return the mixed color
 */
constexpr Color blendColor(Color top, Color bottom, double alpha) {
    return {top.r * alpha + bottom.r * (1 - alpha), top.g * alpha + bottom.g * (1 - alpha),
            top.b * alpha + bottom.b * (1 - alpha)};
}

// ---- Background levels (GitHub dark style) ----
constexpr Color kBg = hexColor(0x0d1117);            ///< panel background
constexpr Color kCard = hexColor(0x161b22);          ///< cards
constexpr Color kControl = hexColor(0x21262d);       ///< buttons and pills
constexpr Color kControlHover = hexColor(0x30363d);  ///< a button under the pointer or pressed
constexpr Color kDivider = hexColor(0x30363d);       ///< decorative lines (never the only way to see a control)
// ---- Text ----
constexpr Color kText = hexColor(0xe6edf3);          ///< body text
constexpr Color kTextMuted = hexColor(0x9198a1);     ///< secondary text (hints, headings)
constexpr Color kTextDisabled = hexColor(0x7d8590);  ///< text of buttons that can't be pressed
// ---- Control outlines (3:1 or more against the background; WCAG 1.4.11) ----
constexpr Color kBorder = hexColor(0x6e7681);
// ---- Accent ----
constexpr Color kAccent = hexColor(0xe27dfd);         ///< selection, live values
constexpr Color kAccentPressed = hexColor(0xc45fe0);  ///< while pressed (a little darker)
constexpr Color kOnAccent = hexColor(0x0d1117);       ///< text on an accent fill (white is only 2.4:1)
constexpr double kAccentTintAlpha = 0.20;             ///< light accent fill and glow
constexpr Color kAccentTint = blendColor(kAccent, kCard, kAccentTintAlpha);  ///< light accent fill on a card
/** A second accent for mark ⑤ (lid_open_snap) on the eyelid bars and its row, a blue apart from the pink marks */
constexpr Color kSnap = hexColor(0x79c0ff);
/** Mark ⑤ for an eye the cameras supply (it doesn't apply there): the blue sunk into the card */
constexpr Color kSnapFaded = blendColor(kSnap, kCard, 0.4);
// ---- States (never by color alone; there is always a symbol or a word) ----
/** The debug gaze dots: combined, left eye (light cyan) and right eye (a darker orange), told apart by lightness too */
constexpr Color kDotBoth = hexColor(0xe6edf3);
constexpr Color kDotLeft = hexColor(0x6ee2ff);
constexpr Color kDotRight = hexColor(0xd9701c);
constexpr Color kSuccess = hexColor(0x3fb950);    ///< sending
constexpr Color kSuccessTint = blendColor(kSuccess, kCard, 0.15);
constexpr Color kDanger = hexColor(0xf85149);     ///< errors, not running
constexpr Color kDangerTint = blendColor(kDanger, kCard, 0.15);
constexpr Color kDangerSoft = hexColor(0xff8a8a); ///< red text on the red tint (kDanger is only 4.4:1 there)
constexpr Color kQuitFill = blendColor(kDanger, kCard, 0.12);  ///< quit / reset buttons (quiet)
// ---- The eye capture's full-view overlay (a light stimulus, not UI) ----
// Pure white and black for the bright and dark steps, and the instruction on them on purpose faint, so it barely
// changes how bright the view is. Not in the contrast report: it isn't meant to be easy to read.
constexpr Color kFillBright = hexColor(0xffffff);
constexpr Color kFillBrightText = hexColor(0xc4c4c4);
constexpr Color kFillDark = hexColor(0x000000);
constexpr Color kFillDarkText = hexColor(0x3c3c3c);

/** How a pair is checked. */
enum class ContrastKind {
    Text,      ///< text (4.5:1 or more)
    Ui,        ///< control outlines, selection, graphics (3:1 or more; WCAG 1.4.11)
    Disabled,  ///< text of buttons that can't be pressed (exempt in WCAG; aim for a readable 3:1)
};

/** One text or control color used on screen, with its background. */
struct ContrastPair {
    const char* what;  ///< where it is used
    Color fg;
    Color bg;
    ContrastKind kind;
};

/**
 * WCAG 2.x relative luminance.
 * @param c the color
 * @return 0 (black) .. 1 (white)
 */
double relativeLuminance(Color c);

/**
 * WCAG 2.x contrast ratio.
 * @param a a color
 * @param b a color
 * @return 1..21
 */
double contrastRatio(Color a, Color b);

/**
 * The ratio a kind needs.
 * @param kind the kind
 * @return 4.5 or 3
 */
double requiredRatio(ContrastKind kind);

/**
 * All pairs used on screen (keep in step with panel.cpp).
 * @return the list
 */
const std::vector<ContrastPair>& contrastPairs();

/**
 * --contrast-report: print every pair's ratio and whether it passes.
 * @return 0 if all pass, 1 otherwise
 */
int printContrastReport();
