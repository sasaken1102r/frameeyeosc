// The version history: reading CHANGELOG.md / CHANGELOG.ja.md into what the view shows.
#include "changelog.h"

#include "icons.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace changelog {

namespace {

/** Files larger than this are not read (the changelog is about 40 KB). */
constexpr std::streamoff kMaxFileBytes = 4 * 1024 * 1024;

/** Symbols written in the changelog and the icon markers the panel draws in their place (icons.h). */
constexpr std::pair<const char*, const char*> kSymbols[] = {
    {"→", ICON_ARROW_RIGHT}, {"▲", ICON_TRIANGLE_UP}, {"▼", ICON_TRIANGLE_DOWN}, {"♪", ICON_NOTE},
    {"‹", ICON_CHEVRON_LEFT}, {"›", ICON_CHEVRON_RIGHT}, {"✓", ICON_CHECK},
};

/**
 * Whether text starts with a prefix.
 * @param text the text
 * @param prefix the prefix
 * @return true if it does
 */
bool startsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

/**
 * Text without spaces and tabs at either end.
 * @param text the text
 * @return the trimmed text
 */
std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    const size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

/**
 * Join a line onto a paragraph: with a space, except between two non-ASCII characters (Japanese lines join
 * directly).
 * @param text the paragraph so far
 * @param line the next line (trimmed)
 * @return the joined text
 */
std::string joinLine(const std::string& text, const std::string& line) {
    if (text.empty()) return line;
    if (line.empty()) return text;
    const bool wide = static_cast<unsigned char>(text.back()) >= 0x80 && static_cast<unsigned char>(line.front()) >= 0x80;
    return wide ? text + line : text + " " + line;
}

/**
 * Read a "## X.Y.Z (YYYY-MM-DD)" heading.
 * @param line the line
 * @param version where to write "X.Y.Z"
 * @param date where to write "YYYY-MM-DD" ("" if the heading has none)
 * @return true if it is a version heading
 */
bool versionHeading(const std::string& line, std::string& version, std::string& date) {
    if (!startsWith(line, "## ")) return false;
    std::string rest = trim(line.substr(3));
    if (!rest.empty() && (rest[0] == 'v' || rest[0] == 'V')) rest.erase(0, 1);
    size_t end = 0;
    int dots = 0;
    while (end < rest.size() && ((rest[end] >= '0' && rest[end] <= '9') || rest[end] == '.')) {
        if (rest[end] == '.') ++dots;
        ++end;
    }
    // A pre-release suffix ("-rc1") belongs to the version
    while (end < rest.size() && rest[end] != ' ' && rest[end] != '(') ++end;
    if (end == 0 || dots < 2 || rest[0] < '0' || rest[0] > '9') return false;
    version = rest.substr(0, end);
    date.clear();
    const size_t open = rest.find('(', end);
    const size_t close = open == std::string::npos ? std::string::npos : rest.find(')', open);
    if (close != std::string::npos) date = trim(rest.substr(open + 1, close - open - 1));
    return true;
}

/**
 * The numbers of a version, to sort by.
 * @param version "0.7.1" (a suffix is ignored)
 * @return its numbers
 */
std::vector<long> versionNumbers(const std::string& version) {
    std::vector<long> numbers;
    long value = 0;
    bool any = false;
    for (const char c : version) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            any = true;
        } else if (c == '.') {
            numbers.push_back(value);
            value = 0;
            any = false;
        } else {
            break;
        }
    }
    if (any) numbers.push_back(value);
    return numbers;
}

/**
 * Whether a paragraph is the English file's Japanese summary ("日本語: ...").
 * @param paragraph the paragraph
 * @return true if it is
 */
bool isJapaneseNote(const std::string& paragraph) {
    return startsWith(paragraph, "日本語:") || startsWith(paragraph, "日本語：");
}

/**
 * Read a whole file.
 * @param path the file
 * @param text where to write it
 * @return true if it was read
 */
bool readFile(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || size > kMaxFileBytes) return false;
    in.seekg(0, std::ios::beg);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

/**
 * Whether a file can be opened for reading.
 * @param path the file
 * @return true if it can
 */
bool readable(const std::string& path) {
    return access(path.c_str(), R_OK) == 0;
}

}  // namespace

std::vector<Section> parse(const std::string& text) {
    std::vector<Section> sections;
    bool inSection = false;
    enum class Block { None, Paragraph, Bullet };
    Block block = Block::None;
    bool blank = false;  // a blank line since the last text line
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (startsWith(line, "#")) {
            std::string version;
            std::string date;
            inSection = versionHeading(line, version, date);
            if (inSection) sections.push_back({version, date, {}, {}});
            block = Block::None;
            blank = false;
            continue;
        }
        if (!inSection) continue;
        Section& section = sections.back();
        const std::string trimmed = trim(line);
        if (trimmed.empty()) {
            blank = true;
            continue;
        }
        const bool indented = line[0] == ' ' || line[0] == '\t';
        if (!indented && (startsWith(line, "- ") || startsWith(line, "* "))) {
            section.bullets.push_back(trim(line.substr(2)));
            block = Block::Bullet;
        } else if (block == Block::Bullet && (indented || !blank)) {
            // An indented line (or one right under the item) belongs to the item
            section.bullets.back() = joinLine(section.bullets.back(), trimmed);
        } else if (block == Block::Paragraph && !blank) {
            section.paragraphs.back() = joinLine(section.paragraphs.back(), trimmed);
        } else {
            section.paragraphs.push_back(trimmed);
            block = Block::Paragraph;
        }
        blank = false;
    }
    return sections;
}

std::string stripMarkdown(const std::string& text) {
    std::string out;
    bool code = false;
    for (size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '`') {
            code = !code;
            ++i;
            continue;
        }
        if (!code) {
            if (text.compare(i, 2, "**") == 0 || text.compare(i, 2, "__") == 0) {
                i += 2;
                continue;
            }
            // The panel's arrows and symbols, as the icons the panel draws them with
            bool symbol = false;
            for (const auto& pair : kSymbols) {
                const size_t n = std::char_traits<char>::length(pair.first);
                if (text.compare(i, n, pair.first) == 0) {
                    out += pair.second;
                    i += n;
                    symbol = true;
                    break;
                }
            }
            if (symbol) continue;
            // [text](url) -> text
            if (c == '[') {
                const size_t close = text.find("](", i);
                const size_t end = close == std::string::npos ? std::string::npos : text.find(')', close + 2);
                if (end != std::string::npos && text.find(']', i) == close) {
                    out += stripMarkdown(text.substr(i + 1, close - i - 1));
                    i = end + 1;
                    continue;
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

std::string firstSentence(const std::string& text) {
    static const char* const kAbbreviations[] = {"e.g.", "i.e.", "vs."};
    for (size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 3, "。") == 0) return text.substr(0, i + 3);
        if (text[i] != '.') continue;
        // The period, and a closing quote or bracket right after it
        size_t end = i + 1;
        while (end < text.size() && (text[end] == '"' || text[end] == ')')) ++end;
        if (end >= text.size() || text[end] != ' ') continue;
        bool abbreviation = false;
        for (const char* word : kAbbreviations) {
            const size_t n = std::char_traits<char>::length(word);
            abbreviation |= i + 1 >= n && text.compare(i + 1 - n, n, word) == 0;
        }
        // "etc." only ends the sentence before a capital
        if (i >= 3 && text.compare(i - 3, 4, "etc.") == 0 && end + 1 < text.size() && text[end + 1] >= 'a' &&
            text[end + 1] <= 'z') {
            abbreviation = true;
        }
        if (!abbreviation) return text.substr(0, end);
    }
    return text;
}

std::string shortDate(const std::string& date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return "";
    for (const size_t i : {0, 1, 2, 3, 5, 6, 8, 9}) {
        if (date[i] < '0' || date[i] > '9') return "";
    }
    const int month = std::atoi(date.substr(5, 2).c_str());
    const int day = std::atoi(date.substr(8, 2).c_str());
    if (month < 1 || month > 12 || day < 1 || day > 31) return "";
    return std::to_string(month) + "/" + std::to_string(day);
}

Entry englishEntry(const Section& section) {
    Entry entry;
    entry.version = section.version;
    entry.date = shortDate(section.date);
    for (const std::string& paragraph : section.paragraphs) {
        if (isJapaneseNote(paragraph)) continue;
        entry.summary = stripMarkdown(paragraph);
        break;
    }
    for (const std::string& bullet : section.bullets) entry.bullets.push_back(firstSentence(stripMarkdown(bullet)));
    return entry;
}

Entry japaneseEntry(const Section& section) {
    Entry entry;
    entry.japanese = true;
    entry.version = section.version;
    entry.date = shortDate(section.date);
    for (const std::string& paragraph : section.paragraphs) entry.summary = joinLine(entry.summary, stripMarkdown(paragraph));
    for (const std::string& bullet : section.bullets) entry.bullets.push_back(stripMarkdown(bullet));
    return entry;
}

std::vector<Entry> merge(const std::vector<Section>& english, const std::vector<Section>& japanese, bool japanesePanel) {
    std::vector<Entry> entries;
    std::vector<std::string> seen;
    const auto add = [&](const Entry& entry) {
        if (std::find(seen.begin(), seen.end(), entry.version) != seen.end()) return;
        seen.push_back(entry.version);
        entries.push_back(entry);
    };
    const auto findJapanese = [&](const std::string& version) -> const Section* {
        for (const Section& section : japanese) {
            if (section.version == version) return &section;
        }
        return nullptr;
    };
    for (const Section& section : english) {
        const Section* ja = japanesePanel ? findJapanese(section.version) : nullptr;
        add(ja != nullptr ? japaneseEntry(*ja) : englishEntry(section));
    }
    if (japanesePanel) {
        for (const Section& section : japanese) add(japaneseEntry(section));
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return versionNumbers(a.version) > versionNumbers(b.version);
    });
    return entries;
}

std::vector<std::string> defaultDirs() {
    std::vector<std::string> dirs;
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        const std::string path(exe, static_cast<size_t>(n));
        const std::string dir = path.substr(0, path.rfind('/'));
        dirs.push_back(dir);
        // A development build in <repository>/panel/build
        const std::string build = "/panel/build";
        if (dir.size() > build.size() && dir.compare(dir.size() - build.size(), build.size(), build) == 0) {
            dirs.push_back(dir.substr(0, dir.size() - build.size()));
        }
    }
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    if (data != nullptr && data[0] == '/') {
        dirs.push_back(std::string(data) + "/frameeyeosc");
    } else if (home != nullptr && home[0] == '/') {
        dirs.push_back(std::string(home) + "/.local/share/frameeyeosc");
    }
    return dirs;
}

History load(const std::vector<std::string>& dirs, bool japanesePanel) {
    History history;
    history.loaded = true;
    history.japanese = japanesePanel;
    // The first folder with the English file, else (for a Japanese panel) the first with the Japanese one
    for (int pass = 0; pass < 2 && history.dir.empty(); ++pass) {
        if (pass == 1 && !japanesePanel) break;
        for (const std::string& dir : dirs) {
            if (readable(dir + (pass == 0 ? "/CHANGELOG.md" : "/CHANGELOG.ja.md"))) {
                history.dir = dir;
                break;
            }
        }
    }
    if (history.dir.empty()) return history;
    std::string text;
    std::vector<Section> english;
    std::vector<Section> japanese;
    if (readFile(history.dir + "/CHANGELOG.md", text)) {
        english = parse(text);
        history.found = true;
    }
    if (japanesePanel && readFile(history.dir + "/CHANGELOG.ja.md", text)) {
        japanese = parse(text);
        history.found = true;
    }
    history.entries = merge(english, japanese, japanesePanel);
    return history;
}

}  // namespace changelog
