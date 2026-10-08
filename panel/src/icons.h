// Small icons drawn with paths inside the panel's text: arrows, chevrons, triangles, a check, a note and the like.
// A text marks where one goes with a private-use character (the ICON_* macros, so they join string literals:
// "下のボタン " ICON_ARROW_RIGHT " Enter"), and Pen::measure / Pen::text (draw.cpp) give it its width and draw it in
// the text's color, so wrapping, cutting with "…" and centering count it like a character. Nothing here draws, so
// the plain-text side (for report.txt and --report) is tested without cairo.
#pragma once

#include <cstddef>
#include <string>

// The markers: U+E000 to U+E00F (3 bytes in UTF-8, "\xEE\x80\x80" to "\xEE\x80\x8F")
#define ICON_CHEVRON_LEFT "\xEE\x80\x80"    ///< ‹ (back)
#define ICON_CHEVRON_RIGHT "\xEE\x80\x81"   ///< › (go to)
#define ICON_CHEVRON_UP "\xEE\x80\x82"      ///< ˄
#define ICON_CHEVRON_DOWN "\xEE\x80\x83"    ///< ˅
#define ICON_TRIANGLE_UP "\xEE\x80\x84"     ///< ▲ (fold)
#define ICON_TRIANGLE_DOWN "\xEE\x80\x85"   ///< ▼ (unfold)
#define ICON_ARROW_RIGHT "\xEE\x80\x86"     ///< → (then, to)
#define ICON_PLAY "\xEE\x80\x87"            ///< ▶
#define ICON_CHECK "\xEE\x80\x88"           ///< ✓
#define ICON_CROSS "\xEE\x80\x89"           ///< ✗ / × (no)
#define ICON_PLUS "\xEE\x80\x8A"            ///< ＋
#define ICON_MINUS "\xEE\x80\x8B"           ///< −
#define ICON_NOTE "\xEE\x80\x8C"            ///< ♪ (sounds)
#define ICON_DOT "\xEE\x80\x8D"             ///< ● (a small dot)
#define ICON_RING "\xEE\x80\x8E"            ///< ◯ (yes)
#define ICON_TRIANGLE_OUTLINE "\xEE\x80\x8F"  ///< △ (partly)

namespace icon {

/** An icon in a text. */
enum class Icon {
    None,
    ChevronLeft,
    ChevronRight,
    ChevronUp,
    ChevronDown,
    TriangleUp,
    TriangleDown,
    ArrowRight,
    Play,
    Check,
    Cross,
    Plus,
    Minus,
    Note,
    Dot,
    Ring,
    TriangleOutline,
};

/** The bytes of one marker in UTF-8. */
constexpr size_t kMarkerBytes = 3;

/**
 * The icon whose marker starts at a position.
 * @param text UTF-8 text
 * @param pos byte position
 * @return the icon (None if no marker starts there)
 */
Icon at(const std::string& text, size_t pos);

/**
 * Whether a text has any marker (the fast path: most texts have none).
 * @param text UTF-8 text
 * @return true if one is in it
 */
bool any(const std::string& text);

/**
 * How much room an icon takes on a line, its side bearings included (like a character's advance).
 * @param which the icon
 * @param size the text size (px)
 * @return the width (px)
 */
double advance(Icon which, double size);

/**
 * Whether an icon must not start a line (an arrow between two steps, or a "›" between two places to go, stays at the
 * end of the line before).
 * @param which the icon
 * @return true for the arrow and the right chevron
 */
bool keepsWithPrevious(Icon which);

/**
 * A text with its markers as plain characters ("→", "‹", "▼", ...), for what isn't drawn by the panel
 * (report.txt, --report).
 * @param text UTF-8 text
 * @return the text
 */
std::string plain(const std::string& text);

}  // namespace icon
