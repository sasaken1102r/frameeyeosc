// Tests for reading the version history (changelog.cpp): both files' formats, skipping the English file's "日本語:"
// paragraph, cutting English items to their first sentence, removing markdown and taking a version missing in
// Japanese from the English file. Built with the panel as changelog-test; exits non-zero on failure.
#include "changelog.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

/**
 * Record a failed check.
 * @param ok the check
 * @param what what was checked
 * @param line where
 */
void check(bool ok, const char* what, int line) {
    if (ok) return;
    ++gFailures;
    std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
}

#define CHECK(condition) check((condition), #condition, __LINE__)

/**
 * Compare two texts, printing both when they differ.
 * @param got the text made
 * @param expected the text wanted
 * @param line where
 */
void same(const std::string& got, const std::string& expected, int line) {
    if (got == expected) return;
    ++gFailures;
    std::fprintf(stderr, "FAILED line %d:\n  got      \"%s\"\n  expected \"%s\"\n", line, got.c_str(), expected.c_str());
}

#define SAME(got, expected) same((got), (expected), __LINE__)

/** The English file's format: a summary, the "日本語:" paragraph, long items, an indented line, markdown. */
const char* const kEnglish =
    "# Changelog\n"
    "\n"
    "## Unreleased\n"
    "\n"
    "- Not released yet. Never shown.\n"
    "\n"
    "## 0.7.1 (2026-10-03)\n"
    "\n"
    "Eye data at the full rate while **Steam Link** streams.\n"
    "\n"
    "日本語: Steam Link で配信中でも目のデータが全部届くように。\n"
    "\n"
    "- The eye data no longer drops to about half. The eye tracker publishes a frame only if asked.\n"
    "- Sending never waits for the network (`Dropped … datagrams`). The sockets don't block any more.\n"
    "- Thanks to [@jelle619](https://github.com/jelle619) (#6), e.g. for the capture. More text here.\n"
    "\n"
    "## 0.7.0 (2026-10-02)\n"
    "\n"
    "A softer gaze.\n"
    "\n"
    "- New presets: Light 0.5 / 3.0 / 0.8, Medium 0.3 / 1.5 / 0.5. The old ones crept on.\n"
    "- It says \"stopped.\" Then it goes on.\n"
    "\r\n"
    "## 0.5.3 (2026-09-29)\r\n"
    "\r\n"
    "- The eye fit works with a slower eye tracker\r\n"
    "  that sends 15 a second. Second sentence.\r\n"
    "- Last item with no period\r\n";

/** The Japanese file's format: short items as written; 0.7.0 is missing. */
const char* const kJapanese =
    "# 更新履歴\n"
    "\n"
    "## 0.7.1 (2026-10-03)\n"
    "\n"
    "Steam Link で配信中でも、目のデータが全部届くように。\n"
    "\n"
    "- 目のデータが半分に減らなくなりました。処理が 1 フレームより長くても大丈夫。\n"
    "- `steamlink_params` で Steam Link の名前でも\n"
    "  送れます。\n"
    "\n"
    "## 0.5.3 (2026-09-29)\n"
    "\n"
    "- 遅いトラッカーでも目を合わせられます。\n";

void testParse() {
    const std::vector<changelog::Section> sections = changelog::parse(kEnglish);
    // "## Unreleased" and "# Changelog" are skipped
    CHECK(sections.size() == 3);
    if (sections.size() != 3) return;
    SAME(sections[0].version, "0.7.1");
    SAME(sections[0].date, "2026-10-03");
    CHECK(sections[0].paragraphs.size() == 2);
    CHECK(sections[0].bullets.size() == 3);
    SAME(sections[1].version, "0.7.0");
    // CRLF lines, and an indented line joined to its item
    SAME(sections[2].version, "0.5.3");
    CHECK(sections[2].paragraphs.empty());
    CHECK(sections[2].bullets.size() == 2);
    if (sections[2].bullets.size() == 2) {
        SAME(sections[2].bullets[0], "The eye fit works with a slower eye tracker that sends 15 a second. Second sentence.");
    }

    // Headings with a "v", without a date, and with a suffix; things that aren't versions
    const std::vector<changelog::Section> other =
        changelog::parse("## v1.2.3\n- a\n## 2.0.0-rc1 (2027-01-02)\n- b\n## 1.2\n- c\n## Notes\n- d\n");
    CHECK(other.size() == 2);
    if (other.size() == 2) {
        SAME(other[0].version, "1.2.3");
        SAME(other[0].date, "");
        SAME(other[1].version, "2.0.0-rc1");
        SAME(other[1].date, "2027-01-02");
        CHECK(other[1].bullets.size() == 1);
    }
    CHECK(changelog::parse("").empty());
}

void testMarkdown() {
    SAME(changelog::stripMarkdown("Reads `config.json` ([#6](https://x/6)) **now**"), "Reads config.json (#6) now");
    SAME(changelog::stripMarkdown("[konsti219/frameeyeosc](https://github.com/konsti219/frameeyeosc) and [`x`](y)"),
         "konsti219/frameeyeosc and x");
    // Inside code nothing else is removed
    SAME(changelog::stripMarkdown("`__init__` and `**`"), "__init__ and **");
    // Brackets that aren't a link stay
    SAME(changelog::stripMarkdown("[fit] center (min 45)"), "[fit] center (min 45)");
    SAME(changelog::stripMarkdown("__bold__ text"), "bold text");
}

void testFirstSentence() {
    SAME(changelog::firstSentence("One. Two."), "One.");
    SAME(changelog::firstSentence("Only one."), "Only one.");
    SAME(changelog::firstSentence("No period"), "No period");
    // Numbers and versions don't end it
    SAME(changelog::firstSentence("Light 0.5 / 3.0, SteamOS 0.4.3 works. Next."), "Light 0.5 / 3.0, SteamOS 0.4.3 works.");
    SAME(changelog::firstSentence("Use e.g. the panel, i.e. this. Next."), "Use e.g. the panel, i.e. this.");
    SAME(changelog::firstSentence("Logs, etc. are kept. Next."), "Logs, etc. are kept.");
    // A closing quote or bracket stays with the sentence
    SAME(changelog::firstSentence("It says \"stopped.\" Then more."), "It says \"stopped.\"");
    SAME(changelog::firstSentence("(As before.) Then more."), "(As before.)");
    SAME(changelog::firstSentence("目が届くように。次の文。"), "目が届くように。");
    SAME(changelog::firstSentence(""), "");
}

void testDates() {
    SAME(changelog::shortDate("2026-10-03"), "10/3");
    SAME(changelog::shortDate("2026-09-29"), "9/29");
    SAME(changelog::shortDate(""), "");
    SAME(changelog::shortDate("2026-13-01"), "");
    SAME(changelog::shortDate("soon"), "");
}

void testEntries() {
    const std::vector<changelog::Section> en = changelog::parse(kEnglish);
    const std::vector<changelog::Section> ja = changelog::parse(kJapanese);

    // English: the first paragraph that isn't "日本語:", markdown gone; each item's first sentence
    const std::vector<changelog::Entry> english = changelog::merge(en, ja, false);
    CHECK(english.size() == 3);
    if (english.size() == 3) {
        SAME(english[0].version, "0.7.1");
        SAME(english[0].date, "10/3");
        SAME(english[0].summary, "Eye data at the full rate while Steam Link streams.");
        CHECK(english[0].bullets.size() == 3);
        if (english[0].bullets.size() == 3) {
            SAME(english[0].bullets[0], "The eye data no longer drops to about half.");
            SAME(english[0].bullets[1], "Sending never waits for the network (Dropped … datagrams).");
            SAME(english[0].bullets[2], "Thanks to @jelle619 (#6), e.g. for the capture.");
        }
        SAME(english[1].bullets[0], "New presets: Light 0.5 / 3.0 / 0.8, Medium 0.3 / 1.5 / 0.5.");
        SAME(english[1].bullets[1], "It says \"stopped.\"");
        // No summary paragraph: none (the view folds it with the first item)
        SAME(english[2].summary, "");
        SAME(english[2].bullets[0], "The eye fit works with a slower eye tracker that sends 15 a second.");
        SAME(english[2].bullets[1], "Last item with no period");
    }
    // Only "日本語:" as a paragraph: no summary
    {
        const std::vector<changelog::Entry> only =
            changelog::merge(changelog::parse("## 1.0.0 (2026-01-01)\n\n日本語：要約\n\n- Item. More.\n"), {}, false);
        CHECK(only.size() == 1 && only[0].summary.empty() && only[0].bullets.size() == 1);
    }

    // Japanese: as written (the indented line joins without a space); 0.7.0 comes from the English file
    const std::vector<changelog::Entry> japanese = changelog::merge(en, ja, true);
    CHECK(japanese.size() == 3);
    if (japanese.size() == 3) {
        SAME(japanese[0].summary, "Steam Link で配信中でも、目のデータが全部届くように。");
        CHECK(japanese[0].bullets.size() == 2);
        if (japanese[0].bullets.size() == 2) {
            SAME(japanese[0].bullets[0], "目のデータが半分に減らなくなりました。処理が 1 フレームより長くても大丈夫。");
            SAME(japanese[0].bullets[1], "steamlink_params で Steam Link の名前でも送れます。");
        }
        SAME(japanese[1].version, "0.7.0");
        SAME(japanese[1].summary, "A softer gaze.");
        SAME(japanese[1].bullets[0], "New presets: Light 0.5 / 3.0 / 0.8, Medium 0.3 / 1.5 / 0.5.");
        SAME(japanese[2].version, "0.5.3");
        SAME(japanese[2].bullets[0], "遅いトラッカーでも目を合わせられます。");
    }
    // Japanese without its file: all English
    CHECK(changelog::merge(en, {}, true).size() == 3);
    // A version only in Japanese still shows, in version order
    {
        const std::vector<changelog::Entry> extra =
            changelog::merge(en, changelog::parse("## 0.8.0 (2026-10-10)\n\n新しい版。\n"), true);
        CHECK(extra.size() == 4 && extra[0].version == "0.8.0" && extra[1].version == "0.7.1");
    }
    // Newest first even when the file isn't
    {
        const std::vector<changelog::Entry> order =
            changelog::merge(changelog::parse("## 0.9.0\n- a\n## 0.10.0\n- b\n## 0.9.1\n- c\n"), {}, false);
        CHECK(order.size() == 3 && order[0].version == "0.10.0" && order[1].version == "0.9.1" &&
              order[2].version == "0.9.0");
    }
}

/**
 * Write a file.
 * @param path where
 * @param text what
 */
void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

void testLoad() {
    char base[] = "/tmp/changelog-test-XXXXXX";
    if (mkdtemp(base) == nullptr) {
        check(false, "mkdtemp", __LINE__);
        return;
    }
    const std::string dir = base;
    const std::string empty = dir + "/empty";
    const std::string both = dir + "/both";
    const std::string jaOnly = dir + "/ja";
    for (const std::string& d : {empty, both, jaOnly}) {
        const std::string command = "mkdir -p '" + d + "'";
        CHECK(std::system(command.c_str()) == 0);
    }
    writeFile(both + "/CHANGELOG.md", kEnglish);
    writeFile(both + "/CHANGELOG.ja.md", kJapanese);
    writeFile(jaOnly + "/CHANGELOG.ja.md", kJapanese);

    // Missing everywhere
    changelog::History none = changelog::load({empty, dir + "/nothing-here"}, true);
    CHECK(none.loaded && !none.found && none.entries.empty());
    // The first folder with the files
    changelog::History ja = changelog::load({empty, both}, true);
    CHECK(ja.found && ja.dir == both && ja.japanese && ja.entries.size() == 3);
    if (ja.entries.size() == 3) SAME(ja.entries[0].summary, "Steam Link で配信中でも、目のデータが全部届くように。");
    changelog::History en = changelog::load({empty, both}, false);
    CHECK(en.found && !en.japanese && en.entries.size() == 3);
    if (en.entries.size() == 3) SAME(en.entries[0].summary, "Eye data at the full rate while Steam Link streams.");
    // Only the Japanese file: a Japanese panel shows it, an English one says it's missing
    CHECK(changelog::load({jaOnly}, true).entries.size() == 2);
    CHECK(!changelog::load({jaOnly}, false).found);

    const std::string cleanup = "rm -rf '" + dir + "'";
    CHECK(std::system(cleanup.c_str()) == 0);
}

}  // namespace

int main() {
    testParse();
    testMarkdown();
    testFirstSentence();
    testDates();
    testEntries();
    testLoad();
    if (gFailures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("changelog-test: all passed\n");
    return 0;
}
