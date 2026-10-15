// Tests for checking a typed target PC address (host_entry.cpp), the eye fit's failure texts in both languages and
// how a fit ends in its record (fit_text.cpp), and that no text draws an arrow or symbol as a character (they are icon
// markers, icons.h). Built with the panel as text-test; exits non-zero on failure.
#include "fit_text.h"
#include "host_entry.h"
#include "icons.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

using host_entry::HostError;
using host_entry::checkHost;

void testHosts() {
    // IPv4 addresses only (a host name can only be set in config.json)
    for (const char* good : {"192.168.1.20", "10.0.0.1", "0.0.0.0", "255.255.255.255"}) {
        CHECK(checkHost(good) == HostError::None);
    }
    CHECK(checkHost("") == HostError::Empty);
    // Not four numbers 0-255 separated by dots
    for (const char* bad : {"256.1.1.1", "1.2.3", "1.2.3.4.5", "1..2.3", ".1.2.3", "1.2.3.", "1234.1.1.1", "192", ".",
                            "192.168.1.20:9000", "my-pc", "auto", "1.2.3.4 ", "010.0.0.1", "192.168.010.5", "00.1.2.3"}) {
        CHECK(checkHost(bad) == HostError::Ipv4);
    }

    // The keypad: digits and dots, backspace, at most 15 characters
    std::string text;
    for (const char c : std::string("192.168.1.20")) text = host_entry::keypadInput(text, c);
    SAME(text, "192.168.1.20");
    text = host_entry::keypadInput(text, host_entry::kBackspace);
    SAME(text, "192.168.1.2");
    SAME(host_entry::keypadInput(text, 'a'), "192.168.1.2");
    SAME(host_entry::keypadInput("", host_entry::kBackspace), "");
    SAME(host_entry::keypadInput("255.255.255.255", '1'), "255.255.255.255");
}

void testFailureTexts() {
    using gaze_fit::Failure;
    using gaze_fit::Point;
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    gaze_fit::View fit;
    fit.phase = gaze_fit::Phase::Failed;

    // Unsteady: the samples usable and the spread, against what is needed, and the tries
    fit.failure = Failure::Unsteady;
    fit.point = Point::Center;
    fit.detail.tries = 3;
    fit.detail.last.samples = 30;
    fit.detail.last.spread = 3.4 / 45;
    SAME(failureText(ja, fit), "正面 の点で視線が落ち着きませんでした（目を閉じていたかも）");
    SAME(failureDetailText(ja, fit), "正面の点: 使えたサンプル 30/45・ばらつき 3.4°（2.7° まで）・3 回");
    SAME(failureDetailText(en, fit), "Center dot: 30 of 45 samples usable · spread 3.4° (max 2.7°) · 3 tries");
    // From 0.5.3 frameeyeosc says how many came in and how fast; the need is 60% of them, at least 12
    fit.detail.last.samples = 10;
    fit.detail.last.received = 26;
    fit.detail.last.rateHz = 15.2;
    fit.detail.last.spread = 0.2 / 45;
    SAME(failureDetailText(ja, fit), "正面の点: 使えたサンプル 10/26（毎秒 15・16 以上が必要）・ばらつき 0.2°（2.7° まで）・3 回");
    SAME(failureDetailText(en, fit), "Center dot: 10 of 26 samples usable at 15 Hz (needs 16) · spread 0.2° (max 2.7°) · 3 tries");
    fit.detail.last.received = 0;
    fit.detail.last.rateHz = NAN;
    // Without any usable sample there is no spread
    fit.detail.last.samples = 0;
    fit.detail.last.spread = NAN;
    SAME(failureDetailText(ja, fit), "正面の点: 使えたサンプル 0/45・ばらつき —（2.7° まで）・3 回");

    // The eyes did not read as shut: each eye against its limit
    fit = gaze_fit::View();
    fit.failure = Failure::NotClosed;
    fit.point = Point::Closed;
    fit.detail.tries = 3;
    fit.detail.last.samples = 200;
    fit.detail.last.openness[0] = 0.62;
    fit.detail.last.openness[1] = 0.40;
    fit.detail.closedBelow[0] = 0.56;
    fit.detail.closedBelow[1] = 0.53;
    SAME(failureDetailText(ja, fit), "目を閉じる: 左 0.62（0.56 未満が必要）・右 0.40（0.53 未満が必要）・3 回");
    SAME(failureDetailText(en, fit), "Eyes closed: L 0.62 (needs below 0.56) · R 0.40 (needs below 0.53) · 3 tries");
    // Too few samples are mentioned too
    fit.detail.last.samples = 20;
    SAME(failureDetailText(en, fit),
         "Eyes closed: 20 of 45 samples usable · L 0.62 (needs below 0.56) · R 0.40 (needs below 0.53) · 3 tries");
    // An eye the eye camera stood in for wasn't checked, so it isn't in the numbers
    fit.detail.last.samples = 200;
    fit.detail.closedBelow[1] = NAN;
    SAME(failureDetailText(ja, fit), "目を閉じる: 左 0.62（0.56 未満が必要）・3 回");

    // A point that hardly moved
    fit = gaze_fit::View();
    fit.failure = Failure::NoMovement;
    fit.point = Point::Up;
    fit.detail.movedDeg = 2.1;
    fit.detail.neededDeg = 3.75;
    SAME(failureDetailText(ja, fit), "上の点: 動いたのは 2.1°（3.8° 以上が必要）");
    SAME(failureDetailText(en, fit), "Up dot: moved 2.1° (needs 3.8°)");
    fit.point = Point::Right;
    fit.detail.movedDeg = 1.0;
    fit.detail.neededDeg = 5.0;
    SAME(failureDetailText(ja, fit), "右の点: 動いたのは 1.0°（5.0° 以上が必要）");
    SAME(failureDetailText(en, fit), "Right dot: moved 1.0° (needs 5.0°)");

    // Eyelids that barely changed: which eye and reading
    fit = gaze_fit::View();
    fit.failure = Failure::NoLidRange;
    fit.detail.eye = 1;
    fit.detail.lidPoint = Point::Down;
    fit.detail.lidOpen = 0.30;
    fit.detail.lidClosed = 0.25;
    SAME(failureDetailText(ja, fit), "右のまぶた（下）: 開き 0.30 と閉じ 0.25 の差 0.05（0.10 以上が必要）");
    SAME(failureDetailText(en, fit), "R eyelid (down dot): open 0.30 vs closed 0.25, 0.05 apart (needs 0.10)");

    // Nothing to add for the others
    fit = gaze_fit::View();
    fit.failure = Failure::Cancelled;
    SAME(failureDetailText(ja, fit), "");

    // Stopped: by "Stop", by leaving the tab, by opening the dashboard (no word of waiting for it to close)
    SAME(failureText(ja, fit), "止めました");
    SAME(failureText(en, fit), "Stopped");
    fit.failure = Failure::Left;
    SAME(failureText(ja, fit), "ほかのタブや画面に移ったので止めました");
    SAME(failureText(en, fit), "Stopped: you went to another tab or page");
    SAME(failureDetailText(en, fit), "");
    fit.failure = Failure::DashboardOpened;
    SAME(failureText(ja, fit), "ダッシュボードを開いたので止めました");
    SAME(failureText(en, fit), "Stopped: the dashboard was opened");
    // The fit's words no longer ask to close the dashboard (the intro says it can stay open)
    for (const UiText* t : {&ja, &en}) {
        for (const char* text : {t->fitHowTo, t->fitHowToCenter, t->fitRunningTitle}) {
            const std::string s = text;
            CHECK(s.find("ダッシュボード") == std::string::npos && s.find("dashboard") == std::string::npos);
        }
    }
    SAME(ja.fitIntro, "［目を合わせる］を押すと、すぐに点が出ます（ダッシュボードは開いたままで大丈夫）");
    SAME(en.fitIntro, "Press \"Fit my eyes\" and a dot shows right away (the dashboard can stay open).");
}

/**
 * How a fit ends in its record: done, failed (red, with the numbers), or stopped (not a failure: grey, why only).
 */
void testFitRecord() {
    using gaze_fit::Failure;
    gaze_fit::View fit;
    fit.phase = gaze_fit::Phase::Failed;
    // Stopped: "Stop", another tab or page, the dashboard opened
    for (const Failure why : {Failure::Cancelled, Failure::Left, Failure::DashboardOpened}) {
        fit.failure = why;
        CHECK(fitStopped(fit));
        report::Summary s;
        fillFitResult(s, fit);
        CHECK(s.result == report::Result::Stopped);
        SAME(s.reason, failureText(uiText(Language::Ja), fit));
        SAME(s.reasonEn, failureText(uiText(Language::En), fit));
        CHECK(s.brief == s.reason && s.briefEn == s.reasonEn);
    }
    report::Summary s;
    fit.failure = Failure::Left;
    fillFitResult(s, fit);
    SAME(s.reason, "ほかのタブや画面に移ったので止めました");
    // A real failure stays one, with its numbers
    fit.failure = Failure::Unsteady;
    fit.point = gaze_fit::Point::Center;
    fit.detail.tries = 3;
    fit.detail.last.samples = 30;
    fit.detail.last.spread = 3.4 / 45;
    CHECK(!fitStopped(fit));
    fillFitResult(s, fit);
    CHECK(s.result == report::Result::Failed);
    SAME(s.brief, "正面 の点で視線が落ち着きませんでした（目を閉じていたかも）");
    SAME(s.reason, s.brief + " 正面の点: 使えたサンプル 30/45・ばらつき 3.4°（2.7° まで）・3 回");
    for (const Failure why : {Failure::NotRunning, Failure::NoResult, Failure::WriteFailed}) {
        fit.failure = why;
        CHECK(!fitStopped(fit));
        fillFitResult(s, fit);
        CHECK(s.result == report::Result::Failed);
    }
    // Done
    fit = gaze_fit::View();
    fit.phase = gaze_fit::Phase::Done;
    fit.mode = gaze_fit::Mode::Center;
    fit.values.offsetX = 0.021;
    fit.values.offsetY = -0.013;
    CHECK(!fitStopped(fit));
    fillFitResult(s, fit);
    CHECK(s.result == report::Result::Ok);
    CHECK(s.reasonEn.find("+0.021") != std::string::npos && s.reasonEn.find("-0.013") != std::string::npos);
    const std::string plain = s.reason;
    CHECK(standInText(uiText(Language::Ja), fit).empty());

    // An eye the eye camera stood in for (Valve's openness stuck at 0): said on the tab and in the record
    fit.notes.standIn[1] = true;
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    SAME(standInText(ja, fit), ja.fitStandInCenterRight);
    fillFitResult(s, fit);
    SAME(s.reason, plain + " " + ja.fitStandInCenterRight);
    CHECK(s.reasonEn.find(en.fitStandInCenterRight) != std::string::npos);
    fit.mode = gaze_fit::Mode::Full;
    SAME(standInText(en, fit), en.fitStandInRight);
    fit.notes.standIn[0] = true;
    SAME(standInText(ja, fit), std::string(ja.fitStandInLeft) + "\n" + ja.fitStandInRight);
    fillFitResult(s, fit);
    CHECK(s.reason.find('\n') == std::string::npos);
    // Only once it is done
    fit.phase = gaze_fit::Phase::Failed;
    CHECK(standInText(ja, fit).empty());
    // The words say which eye, in the panel's own names for them
    CHECK(std::string(ja.fitStandInRight).find("右目") != std::string::npos);
    CHECK(std::string(en.fitStandInLeft).find("left") != std::string::npos);
}

/**
 * No text in either table has an arrow or a symbol the panel draws with paths: they are icon markers, so the font
 * never draws them. Kept: "×" for "times" (×0.99) and "＋" as a word ("+ is right", "camera + Valve").
 */
void testLidFitLines() {
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    gaze_fit::Values v;
    v.lidOpen[0] = 0.85;
    v.lidClosed[0] = 0.2;
    v.lidDown[0] = 0.7;
    v.lidOpen[1] = 0.9;
    v.lidClosed[1] = 0.25;
    v.lidDown[1] = 0.9;
    // Both eyes fitted: a line each
    const bool both[2] = {true, true};
    const std::vector<std::string> lines = lidFitLines(ja, v, both);
    CHECK(lines.size() == 2);
    SAME(lines[0], "まぶた 左: 開 0.85・閉 0.20・下を見ると -18%");
    SAME(lines[1], "まぶた 右: 開 0.90・閉 0.25・下を見ると +0%");
    // A first whole fit with the eye camera standing in for the right eye: the left eye's readings, the right "not
    // fitted"
    const bool left[2] = {true, false};
    const std::vector<std::string> one = lidFitLines(ja, v, left);
    CHECK(one.size() == 2);
    SAME(one[0], lines[0]);
    SAME(one[1], "まぶた 右: 合わせていません（自動で覚えています）");
    const bool right[2] = {false, true};
    const std::vector<std::string> other = lidFitLines(en, v, right);
    CHECK(other.size() == 2);
    SAME(other[0], "Eyelid L: not fitted (learned automatically)");
    SAME(other[1], "Eyelid R: open 0.90, closed 0.25, looking down +0%");
    // Neither: one line, as before
    const bool neither[2] = {false, false};
    const std::vector<std::string> none = lidFitLines(en, v, neither);
    CHECK(none.size() == 1);
    if (!none.empty()) SAME(none[0], en.fitLidsNone);
}

void testNoSymbolGlyphs() {
    static_assert(sizeof(UiText) % sizeof(const char*) == 0, "UiText holds only strings");
    const char* const glyphs[] = {"‹", "›", "▲", "▼", "△", "▽", "→", "←", "♪", "✓", "✗", "◯", "◎", "●", "▶", "▷"};
    int markers = 0;
    for (const Language language : {Language::Ja, Language::En}) {
        const UiText& t = uiText(language);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&t);
        for (size_t at = 0; at < sizeof(UiText); at += sizeof(const char*)) {
            const char* text = nullptr;
            std::memcpy(&text, bytes + at, sizeof(text));
            if (text == nullptr) continue;
            const std::string s = text;
            markers += icon::any(s) ? 1 : 0;
            for (const char* glyph : glyphs) {
                if (s.find(glyph) != std::string::npos) {
                    ++gFailures;
                    std::fprintf(stderr, "FAILED: \"%s\" has %s (use an icon marker)\n", text, glyph);
                }
            }
        }
    }
    // The arrows, chevrons, the note, the marks and the like are there as markers
    CHECK(markers >= 20);
}

/**
 * The destination cards' lines: two each (sync, VRCFT), no widening any more, and FrameEyeOSC's sync is the double
 * ring (VRChat's the triangle, the other two the ring or a check), which the hint above the cards explains.
 */
void testOutputMarks() {
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    static_assert(sizeof(ja.outputMarks[0]) / sizeof(ja.outputMarks[0][0]) == 2, "two lines a card");
    for (const UiText* t : {&ja, &en}) {
        for (const auto& lines : t->outputMarks) {
            for (const char* line : lines) {
                CHECK(line != nullptr && line[0] != '\0');
                if (line == nullptr) continue;
                CHECK(std::string(line).find("見開き") == std::string::npos);
                CHECK(std::string(line).find("ide eyes") == std::string::npos);
            }
        }
        CHECK(std::string(t->hintOutput).find(ICON_DOUBLE_RING) != std::string::npos);
    }
    SAME(ja.outputMarks[0][0], "同期 " ICON_TRIANGLE_OUTLINE);
    SAME(ja.outputMarks[1][0], "同期 " ICON_RING);
    SAME(ja.outputMarks[2][0], "同期 " ICON_RING);
    SAME(ja.outputMarks[3][0], "同期 " ICON_DOUBLE_RING);
    SAME(en.outputMarks[1][0], ICON_CHECK " Synced to others");
    SAME(en.outputMarks[3][0], ICON_DOUBLE_RING " Synced to others");
    CHECK(std::string(ja.hintOutput).find(ICON_TRIANGLE_OUTLINE) != std::string::npos);
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testHosts();
    testFailureTexts();
    testFitRecord();
    testLidFitLines();
    testNoSymbolGlyphs();
    testOutputMarks();
    if (gFailures == 0) std::printf("text-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
