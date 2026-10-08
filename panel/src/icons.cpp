// The icon markers in texts: finding them, their widths, and their plain-text stand-ins (drawing is in draw.cpp).
#include "icons.h"

namespace icon {

namespace {

/** One icon: how wide it is (in em) and what stands for it in plain text. */
struct Spec {
    Icon icon;
    double em;
    const char* plain;
};

// In marker order (U+E000 up)
constexpr Spec kSpecs[] = {
    {Icon::ChevronLeft, 0.78, "‹"},     {Icon::ChevronRight, 0.62, "›"},   {Icon::ChevronUp, 0.72, "˄"},
    {Icon::ChevronDown, 0.72, "˅"},     {Icon::TriangleUp, 0.86, "▲"},     {Icon::TriangleDown, 0.86, "▼"},
    {Icon::ArrowRight, 0.96, "→"},      {Icon::Play, 0.72, "▶"},           {Icon::Check, 0.82, "✓"},
    {Icon::Cross, 0.74, "×"},           {Icon::Plus, 0.74, "+"},           {Icon::Minus, 0.74, "-"},
    {Icon::Note, 0.78, "♪"},            {Icon::Dot, 0.50, "●"},            {Icon::Ring, 1.06, "◯"},
    {Icon::TriangleOutline, 1.06, "△"},
};
constexpr size_t kCount = sizeof(kSpecs) / sizeof(kSpecs[0]);

/**
 * The index of the marker at a position.
 * @param text UTF-8 text
 * @param pos byte position
 * @return 0..kCount-1, or kCount if none starts there
 */
size_t indexAt(const std::string& text, size_t pos) {
    if (pos + kMarkerBytes > text.size()) return kCount;
    const auto b0 = static_cast<unsigned char>(text[pos]);
    const auto b1 = static_cast<unsigned char>(text[pos + 1]);
    const auto b2 = static_cast<unsigned char>(text[pos + 2]);
    if (b0 != 0xEE || b1 != 0x80 || b2 < 0x80) return kCount;
    const size_t index = b2 - 0x80;
    return index < kCount ? index : kCount;
}

}  // namespace

Icon at(const std::string& text, size_t pos) {
    const size_t index = indexAt(text, pos);
    return index < kCount ? kSpecs[index].icon : Icon::None;
}

bool any(const std::string& text) {
    for (size_t pos = text.find('\xEE'); pos != std::string::npos; pos = text.find('\xEE', pos + 1)) {
        if (indexAt(text, pos) < kCount) return true;
    }
    return false;
}

double advance(Icon which, double size) {
    for (const Spec& spec : kSpecs) {
        if (spec.icon == which) return spec.em * size;
    }
    return 0.0;
}

bool keepsWithPrevious(Icon which) {
    return which == Icon::ArrowRight || which == Icon::ChevronRight;
}

std::string plain(const std::string& text) {
    if (!any(text)) return text;
    std::string out;
    out.reserve(text.size());
    for (size_t pos = 0; pos < text.size();) {
        const size_t index = indexAt(text, pos);
        if (index < kCount) {
            out += kSpecs[index].plain;
            pos += kMarkerBytes;
        } else {
            out += text[pos];
            ++pos;
        }
    }
    return out;
}

}  // namespace icon
