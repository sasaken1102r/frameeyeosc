// The version history on the Advanced tab: CHANGELOG.md (and CHANGELOG.ja.md for a Japanese panel) read into one
// list, newest first, as plain text. Nothing here draws, so it is tested on its own (changelog_test.cpp).
#pragma once

#include <string>
#include <vector>

namespace changelog {

/** One version's section as written ("## 0.7.1 (2026-10-03)" up to the next heading). */
struct Section {
    std::string version;                  ///< "0.7.1"
    std::string date;                     ///< "2026-10-03" ("" when the heading has none)
    std::vector<std::string> paragraphs;  ///< the text paragraphs (lines joined), in order
    std::vector<std::string> bullets;     ///< the "- " items (with their indented lines), in order
};

/** One version as the panel shows it (markdown removed). */
struct Entry {
    std::string version;               ///< "0.7.1"
    std::string date;                  ///< "10/3" ("" when the heading has none)
    std::string summary;               ///< "" when the section has none (the first bullet stands in when folded)
    std::vector<std::string> bullets;  ///< English: each one's first sentence; Japanese: as written
    bool japanese = false;             ///< from CHANGELOG.ja.md (shown in full; English items are cut short)
};

/** What the history view shows. */
struct History {
    bool loaded = false;           ///< read at least once
    bool japanese = false;         ///< read for a Japanese panel
    bool found = false;            ///< CHANGELOG.md or CHANGELOG.ja.md was found
    std::string dir;               ///< the folder they were read from
    std::vector<Entry> entries;    ///< newest first
};

/**
 * Split a changelog into its version sections. Only "## X.Y.Z" headings (with an optional "(YYYY-MM-DD)") start
 * one; other headings ("# Changelog", "## Unreleased") end it and their text is skipped.
 * @param text the file
 * @return the sections, in file order
 */
std::vector<Section> parse(const std::string& text);

/**
 * Remove markdown: backticks, links (their text stays) and bold.
 * @param text one paragraph
 * @return plain text
 * @example
 * stripMarkdown("Reads `config.json` ([#6](https://x)) **now**") // "Reads config.json (#6) now"
 */
std::string stripMarkdown(const std::string& text);

/**
 * The first sentence: up to and including the first "." followed by a space (with a closing quote or bracket in
 * between) or "。". "e.g.", "i.e.", "vs." and "etc." before a lowercase word don't end it.
 * @param text plain text
 * @return the sentence (the whole text when it has only one)
 */
std::string firstSentence(const std::string& text);

/**
 * A heading's date as the rows show it.
 * @param date "2026-10-03"
 * @return "10/3" ("" when it isn't YYYY-MM-DD)
 */
std::string shortDate(const std::string& date);

/**
 * A section for an English panel: the first paragraph that doesn't start with "日本語:" as the summary, and each
 * bullet's first sentence.
 * @param section the section
 * @return the entry
 */
Entry englishEntry(const Section& section);

/**
 * A section of CHANGELOG.ja.md: its paragraphs as the summary and its bullets, as written.
 * @param section the section
 * @return the entry
 */
Entry japaneseEntry(const Section& section);

/**
 * The list the view shows, newest version first. A Japanese panel takes each version from the Japanese file when it
 * has it, and from the English one otherwise; an English panel only uses the English file.
 * @param english CHANGELOG.md's sections
 * @param japanese CHANGELOG.ja.md's sections
 * @param japanesePanel the panel's language is Japanese
 * @return the entries
 */
std::vector<Entry> merge(const std::vector<Section>& english, const std::vector<Section>& japanese, bool japanesePanel);

/**
 * Where the changelog files may be, in order: next to the panel binary (a release tarball), the checkout of a
 * development build (panel/build -> the repository), and the folder install.sh puts them in.
 * @return the folders
 */
std::vector<std::string> defaultDirs();

/**
 * Read the changelog from the first folder that has CHANGELOG.md or CHANGELOG.ja.md.
 * @param dirs the folders to look in
 * @param japanesePanel the panel's language is Japanese
 * @return the history (found = false when no folder has either file)
 */
History load(const std::vector<std::string>& dirs, bool japanesePanel);

}  // namespace changelog
