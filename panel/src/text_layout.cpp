// Laying text out on the panel (see text_layout.h).
#include "text_layout.h"

#include "icons.h"

#include <algorithm>

namespace {

/**
 * Whether a unit starts with a CJK character that may not start a line (closing brackets and punctuation; an arrow
 * may be joined to it, "」→").
 * @param unit one character, or a word
 * @return true for 。、）」』，．！？ first
 */
bool closesLine(const std::string& unit) {
    static const char* const marks[] = {"。", "、", "）", "」", "』", "，", "．", "！", "？"};
    for (const char* mark : marks) {
        if (unit.rfind(mark, 0) == 0) return true;
    }
    return false;
}

}  // namespace

size_t utf8Length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

std::string ellipsize(const Pen& pen, const std::string& text, double size, bool bold, double maxWidth, bool keepEnd) {
    if (pen.measure(text, size, bold) <= maxWidth) return text;
    // Keep byte boundaries, not a separately allocated string for every character. Reuse the candidate's
    // capacity while trimming (an icon marker is one 3-byte character, so it still goes as a whole).
    std::vector<size_t> ends;
    for (size_t i = 0; i < text.size();) {
        const size_t n = std::min(utf8Length(static_cast<unsigned char>(text[i])), text.size() - i);
        i += n;
        ends.push_back(i);
    }
    std::string candidate;
    candidate.reserve(text.size() + 3);
    if (keepEnd) candidate = "…" + text;
    else candidate = text;
    size_t first = 0;
    for (size_t left = ends.size(); left > 0; --left) {
        if (keepEnd) {
            candidate.erase(3, ends[ends.size() - left] - first);
            first = ends[ends.size() - left];
        } else {
            candidate.resize(left > 1 ? ends[left - 2] : 0);
            candidate += "…";
        }
        if (pen.measure(candidate, size, bold) <= maxWidth) return candidate;
    }
    return "…";
}

std::vector<std::string> wrapText(const Pen& pen, const std::string& text, double size, bool bold, double maxWidth,
                                  size_t maxLines) {
    // Units: a word with its trailing spaces, or one CJK character. An arrow or a "›" joins the unit before it (and
    // the spaces after it join too), so it never starts a line
    std::vector<std::string> units;
    std::string word;
    for (size_t i = 0; i < text.size();) {
        const size_t n = std::min(utf8Length(static_cast<unsigned char>(text[i])), text.size() - i);
        const size_t start = i;
        i += n;
        if (icon::keepsWithPrevious(icon::at(text, start))) {
            if (word.empty() && !units.empty()) {
                word = units.back();
                units.pop_back();
                // Only spaces before it: the unit before those too
                if (word.find_first_not_of(' ') == std::string::npos && !units.empty()) {
                    word = units.back() + word;
                    units.pop_back();
                }
            }
            word.append(text, start, n);
        } else if (n >= 3) {
            if (!word.empty()) units.push_back(word);
            word.clear();
            units.push_back(text.substr(start, n));
        } else if (text[start] == ' ') {
            word += ' ';
            units.push_back(word);
            word.clear();
        } else {
            word.append(text, start, n);
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
            // Out of lines: put the rest on the last line and cut it with "…" (the space trimmed off the line goes
            // back between it and the rest)
            std::string rest = lines.back();
            if (next > 0 && units[next - 1].back() == ' ') rest += ' ';
            for (size_t r = next; r < units.size(); ++r) rest += units[r];
            lines.back() = ellipsize(pen, rest, size, bold, maxWidth, false);
            return lines;
        }
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    if (!line.empty()) lines.push_back(ellipsize(pen, line, size, bold, maxWidth, false));
    return lines;
}

double wrapSize(const Pen& pen, const std::string& text, double size, double minSize, bool bold, double maxWidth,
                size_t maxLines) {
    while (size > minSize && wrapText(pen, text, size, bold, maxWidth, maxLines + 1).size() > maxLines) size -= 1;
    return size;
}
