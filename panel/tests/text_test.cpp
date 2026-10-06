// Tests for checking a typed target PC address (host_entry.cpp), the eye fit's failure texts in both languages
// (fit_text.cpp), and that no text draws an arrow or symbol as a character (they are icon markers, icons.h). Built with
// the panel as text-test; exits non-zero on failure.
#include "fit_text.h"
#include "host_entry.h"
#include "icons.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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

    // A point that hardly moved, and an eye's own sideways fit
    fit = gaze_fit::View();
    fit.failure = Failure::NoMovement;
    fit.point = Point::Up;
    fit.detail.movedDeg = 2.1;
    fit.detail.neededDeg = 3.75;
    SAME(failureDetailText(ja, fit), "上の点: 動いたのは 2.1°（3.8° 以上が必要）");
    SAME(failureDetailText(en, fit), "Up dot: moved 2.1° (needs 3.8°)");
    fit.point = Point::Right;
    fit.detail.eye = 0;
    fit.detail.movedDeg = 5.0;
    fit.detail.neededDeg = 10.0;
    SAME(failureDetailText(ja, fit), "左目の左右: 動いたのは 5.0°（10.0° 以上が必要）");
    SAME(failureDetailText(en, fit), "Left eye sideways: moved 5.0° (needs 10.0°)");

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
        for (const char* text : {t->fitHowTo, t->fitHowToCenter, t->fitHowToTilt, t->fitRunningTitle}) {
            const std::string s = text;
            CHECK(s.find("ダッシュボード") == std::string::npos && s.find("dashboard") == std::string::npos);
        }
    }
    SAME(ja.fitIntro, "［目を合わせる］を押すと、すぐに点が出ます（ダッシュボードは開いたままで大丈夫）");
    SAME(en.fitIntro, "Press \"Fit my eyes\" and a dot shows right away (the dashboard can stay open).");
}

/**
 * No text in either table has an arrow or a symbol the panel draws with paths: they are icon markers, so the font
 * never draws them. Kept: "×" for "times" (×0.99) and "＋" as a word ("+ is right", "camera + Valve").
 */
void testNoSymbolGlyphs() {
    static_assert(sizeof(UiText) % sizeof(const char*) == 0, "UiText holds only strings");
    const char* const glyphs[] = {"‹", "›", "▲", "▼", "△", "▽", "→", "←", "♪", "✓", "✗", "◯", "●", "▶", "▷"};
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

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testHosts();
    testFailureTexts();
    testNoSymbolGlyphs();
    if (gFailures == 0) std::printf("text-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
