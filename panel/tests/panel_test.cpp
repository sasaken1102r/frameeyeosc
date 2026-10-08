// Tests for the Eyelids and eye cameras tabs as drawn (案E): which buttons they offer for each source of the eyelids
// (the widening slider's routing, its three sub-tabs each with its own controls inside the card and remembered while
// other tabs show, the sensitivity slider moved off the eye cameras page)
// and a setting's slider let go of, the setup's (3) with a pupil not found, and a calibration that went through without
// one eye, the Advanced tab's sub-tabs (switching, remembered while other tabs show, the update notice to "Version",
// the diagnostics from the eye cameras tab back to "Having trouble"), its pages fitting, the records (the newest three,
// one open, all of them scrolled: kept within the list, ▲ / ▼, only what shows can be pressed) and the failure screens'
// bar, the Eye fit tab while a fit runs (only "Stop" and the tabs), and the icons in texts (measured and drawn in their
// own boxes, never starting a line, cut whole). Built with
// the panel as panel-test (it renders offscreen with the panel's fonts) and
// run after it is built; exits non-zero on failure.
#include "config.h"
#include "draw.h"
#include "i18n.h"
#include "icons.h"
#include "model.h"
#include "panel.h"
#include "text_layout.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

/** Where the eyelids come from in a test. */
enum class Lids { Valve, Both, Left, Saturated };

/**
 * A model: frameeyeosc running with the eyelids from there, eyecam-rec set up and idle (unless not).
 * @param lids where the eyelids come from
 * @param eyecam eyecam-rec runs and is set up (the eye cameras tab shows its page)
 * @return the model
 */
PanelModel modelWith(Lids lids, bool eyecam = true) {
    PanelModel m;
    m.config.exists = true;
    m.config.root.type = JsonValue::Type::Object;
    EyeStatus& s = m.status;
    s.present = s.running = true;
    s.camera.known = true;
    s.camera.present = lids != Lids::Valve && lids != Lids::Saturated;
    s.camera.calibState = 5;
    s.camera.used[0] = lids == Lids::Both || lids == Lids::Left;
    s.camera.used[1] = lids == Lids::Both;
    s.opennessSaturated = lids == Lids::Saturated;
    if (eyecam) {
        eyecam::Status& e = m.eyecam.status;
        e.present = true;
        e.state = eyecam::State::Idle;
        e.stateText = "idle";
        e.hasSetupDone = e.setupDone = true;
        e.hasBuffers = true;
        e.calibState = eyecam::kCalibWearBit | eyecam::kCalibAutoBit;  // calibrated: the user's can run too
        e.autoGrab = "ok";
        e.hasWidenSensitivity = true;
        e.widenSensitivity = 0.5;
        e.fpsL = e.fpsR = NAN;
        m.eyecam.visible = true;
        m.eyecam.password = eyecam::PasswordState::Set;
    }
    return m;
}

/**
 * The usable buttons with this action (and key) as last drawn.
 * @param panel the panel
 * @param action the action
 * @param key the key (nullptr = any)
 * @return them
 */
std::vector<EyePanel::HitArea> hits(const EyePanel& panel, PanelAction action, const char* key = nullptr) {
    std::vector<EyePanel::HitArea> found;
    for (const EyePanel::HitArea& area : panel.hitAreas()) {
        if (area.hit.action != action) continue;
        if (key != nullptr && (area.hit.key == nullptr || std::string(area.hit.key) != key)) continue;
        found.push_back(area);
    }
    return found;
}

/**
 * Whether a usable tab button to this tab was drawn (outside the tab row).
 * @param panel the panel
 * @param tab the tab
 * @return true if there is one below the tab row
 */
bool pointsTo(const EyePanel& panel, PanelTab tab) {
    for (const EyePanel::HitArea& area : hits(panel, PanelAction::Tab)) {
        if (area.hit.arg == static_cast<int>(tab) && area.y > 100) return true;
    }
    return false;
}

void testWidenRouting(const FontSet& fonts) {
    // The cameras on both eyes: their sensitivity, no lid_widen stops
    {
        EyePanel panel(fonts);
        panel.setTab(PanelTab::Lids);
        panel.render(modelWith(Lids::Both));
        CHECK(hits(panel, PanelAction::EyecamSensitivity).size() == 1);
        CHECK(hits(panel, PanelAction::SetLidWiden).empty());
        CHECK(!pointsTo(panel, PanelTab::Eyecam));
    }
    // One eye: still the sensitivity (the Valve eye's level goes with it, in the loop)
    {
        EyePanel panel(fonts);
        panel.setTab(PanelTab::Lids);
        panel.render(modelWith(Lids::Left));
        CHECK(hits(panel, PanelAction::EyecamSensitivity).size() == 1);
        CHECK(hits(panel, PanelAction::SetLidWiden).empty());
    }
    // ...but an eyecam-rec without widen_sensitivity can't be moved
    {
        EyePanel panel(fonts);
        panel.setTab(PanelTab::Lids);
        PanelModel m = modelWith(Lids::Both);
        m.eyecam.status.hasWidenSensitivity = false;
        panel.render(m);
        CHECK(hits(panel, PanelAction::EyecamSensitivity).empty());
    }
    // Valve's values: lid_widen's four stops side by side, left to right
    {
        EyePanel panel(fonts);
        panel.setTab(PanelTab::Lids);
        panel.render(modelWith(Lids::Valve, false));
        const std::vector<EyePanel::HitArea> stops = hits(panel, PanelAction::SetLidWiden, key::kLidWiden);
        CHECK(stops.size() == 4);
        for (size_t i = 0; i < stops.size(); ++i) {
            CHECK(stops[i].hit.arg == static_cast<int>(i));
            if (i > 0) CHECK(std::fabs(stops[i - 1].x + stops[i - 1].w - stops[i].x) < 1e-6);
        }
        CHECK(hits(panel, PanelAction::EyecamSensitivity).empty());
        // ...locked on frameeyeosc's command line: none
        PanelModel locked = modelWith(Lids::Valve, false);
        locked.status.locked.push_back(key::kLidWiden);
        panel.render(locked);
        CHECK(hits(panel, PanelAction::SetLidWiden).empty());
    }
    // A SteamOS that caps openness, no camera: greyed (nothing to press), the way to the eye cameras instead
    {
        EyePanel panel(fonts);
        panel.setTab(PanelTab::Lids);
        PanelModel m = modelWith(Lids::Saturated);
        m.eyecam.status.hasSetupDone = true;
        m.eyecam.status.setupDone = false;  // not set up yet
        panel.render(m);
        CHECK(hits(panel, PanelAction::SetLidWiden).empty());
        CHECK(hits(panel, PanelAction::EyecamSensitivity).empty());
        CHECK(pointsTo(panel, PanelTab::Eyecam));
        // ...without eyecam-rec (no eye cameras tab) no button to it
        panel.render(modelWith(Lids::Saturated, false));
        CHECK(!pointsTo(panel, PanelTab::Eyecam));
        // ...with one camera eye, the slider drives the cameras again
        PanelModel left = modelWith(Lids::Left);
        left.status.opennessSaturated = true;
        panel.render(left);
        CHECK(hits(panel, PanelAction::EyecamSensitivity).size() == 1);
    }
}

void testSliderMoved(const FontSet& fonts) {
    // The eye cameras' page: no sensitivity slider any more, a pointer to the Eyelids tab instead
    EyePanel panel(fonts);
    const PanelModel m = modelWith(Lids::Both);
    panel.setTab(PanelTab::Eyecam);
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamSensitivity).empty());
    const std::vector<EyePanel::HitArea> pointer = hits(panel, PanelAction::LidsPage);
    CHECK(pointer.size() == 1 && pointer[0].hit.arg == static_cast<int>(LidsPage::Look));
    CHECK(hits(panel, PanelAction::SetBool, key::kCameraLids).size() == 2);
    CHECK(hits(panel, PanelAction::EyecamCalib).size() == 2);
    // ...the pointer goes there, to "Look" where the slider is (also when another sub-tab was open last)
    panel.setLidsPage(LidsPage::Fine);
    if (pointer.size() == 1) {
        panel.pointerDown(pointer[0].x + pointer[0].w / 2, pointer[0].y + pointer[0].h / 2, 0.0);
        panel.pointerUp();
    }
    CHECK(panel.tab() == PanelTab::Lids && panel.lidsPage() == LidsPage::Look);
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamSensitivity).size() == 1);
}

void testNumberSlider(const FontSet& fonts) {
    // lid_sync's slider: pressed, dragged and let go of gives its value once, on its step grid
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Lids);
    const PanelModel m = modelWith(Lids::Valve, false);
    panel.render(m);
    const std::vector<EyePanel::HitArea> sliders = hits(panel, PanelAction::NumberSlider, key::kLidSync);
    CHECK(sliders.size() == 1);
    if (sliders.size() != 1) return;
    const EyePanel::HitArea& area = sliders[0];
    std::string name;
    double value = 0.0;
    CHECK(!panel.takeNumberSlider(name, value));
    panel.pointerDown(area.x + 18 + 2, area.y + area.h / 2, 0.0);  // the track's left end: 0
    panel.pointerMove(area.x + area.w - 18 - 2, area.y + area.h / 2);  // dragged to the right end: 1
    panel.pointerUp();
    CHECK(panel.takeNumberSlider(name, value));
    CHECK(name == key::kLidSync);
    CHECK(std::fabs(value - 1.0) < 1e-9);
    CHECK(!panel.takeNumberSlider(name, value));
    // ...a point near the middle snaps onto the 0.05 grid
    panel.pointerDown(area.x + area.w * 0.513, area.y + area.h / 2, 0.0);
    panel.pointerUp();
    CHECK(panel.takeNumberSlider(name, value));
    CHECK(std::fabs(value / 0.05 - std::round(value / 0.05)) < 1e-6 && value > 0.4 && value < 0.6);
    // ...locked: no slider to press
    PanelModel locked = m;
    locked.status.locked.push_back(key::kLidSync);
    panel.render(locked);
    CHECK(hits(panel, PanelAction::NumberSlider).empty());
}

}  // namespace

void testPupilBitsRow(const FontSet& fonts) {
    // The Output tab's "How the avatar takes pupils": five usable segments where the pupils go straight to VRChat
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Output);
    const auto segments = [&](PanelModel m) {
        panel.render(m);
        return hits(panel, PanelAction::SetInteger, key::kPupilBits);
    };
    PanelModel direct = modelWith(Lids::Both);
    std::vector<EyePanel::HitArea> found = segments(direct);
    CHECK(found.size() == 5);
    for (size_t i = 0; i < found.size(); ++i) CHECK(found[i].hit.arg == static_cast<int>(i));
    PanelModel livelink = direct;
    livelink.config.root.set(key::kOutput, JsonValue::makeString(kOutputLivelink));
    CHECK(segments(livelink).size() == 5);
    // Greyed (drawn, nothing to press): LiveLink without "Send pupils straight to VRChat", or the cameras off
    PanelModel pupilsOff = livelink;
    pupilsOff.config.root.set(key::kPupilsToVrchat, JsonValue::makeBool(false));
    CHECK(segments(pupilsOff).empty());
    CHECK(hits(panel, PanelAction::SetBool, key::kPupilsToVrchat).size() == 2);
    PanelModel camerasOff = direct;
    camerasOff.config.root.set(key::kCameraLids, JsonValue::makeBool(false));
    CHECK(segments(camerasOff).empty());
    // Not there: without the eye cameras, or for ETVR; locked: nothing to press
    CHECK(segments(modelWith(Lids::Valve, false)).empty());
    PanelModel etvr = direct;
    etvr.config.root.set(key::kOutput, JsonValue::makeString(kOutputEtvr));
    CHECK(segments(etvr).empty());
    PanelModel locked = direct;
    locked.status.locked.push_back(key::kPupilBits);
    CHECK(segments(locked).empty());
    // The other VRChat rows are still all there above it
    panel.render(direct);
    CHECK(hits(panel, PanelAction::SetBool, key::kNativeEyes).size() == 2);
    CHECK(hits(panel, PanelAction::SetActiveType).size() == 3);
}

/**
 * Whether the screen as last drawn offers a way out: a usable control besides the tab row (the tab buttons are at the
 * top; the left column's card to a tab counts).
 * @param panel the panel
 * @return true if there is one
 */
bool hasWayOut(const EyePanel& panel) {
    for (const EyePanel::HitArea& area : panel.hitAreas()) {
        if (area.hit.action != PanelAction::Tab || area.y > 100) return true;
    }
    return false;
}

/**
 * eyecam-rec in a state, set up (or not) as modelWith makes it, with these fields.
 * @param state the state ("error", "calibrating", ...)
 * @param setUp setup_done
 * @return the model
 */
PanelModel eyecamIn(const char* state, bool setUp = true) {
    PanelModel m = modelWith(Lids::Both);
    eyecam::Status& e = m.eyecam.status;
    e.stateText = state;
    e.state = eyecam::parseState(state);
    e.setupDone = setUp;
    e.locked = true;  // the headset on, both eyes seen (the setup's (3) waits for that, with nothing else to press)
    return m;
}

void testErrorBack(const FontSet& fonts) {
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Eyecam);
    // A failed user calibration, the headset taken off since (calib_state lost bit 0): not a dead end any more. No
    // grey "Calibrate again", but this wear's calibration and "Back"
    PanelModel m = eyecamIn("error");
    m.eyecam.lastRun = eyecam::Run::CalibUser;
    m.eyecam.status.message = "右目: 下を見ても目の開きが変わっていない";
    m.eyecam.status.calibState = eyecam::kCalibAutoBit;
    panel.render(m);
    std::vector<EyePanel::HitArea> calib = hits(panel, PanelAction::EyecamCalib);
    CHECK(calib.size() == 1);
    if (calib.size() == 1) CHECK(calib[0].hit.arg == static_cast<int>(eyecam::Calib::Wear));
    CHECK(hits(panel, PanelAction::EyecamBack).size() == 1);
    // ...with bit 0 still there: the user's again, and "Back"
    m.eyecam.status.calibState = eyecam::kCalibWearBit | eyecam::kCalibAutoBit;
    panel.render(m);
    calib = hits(panel, PanelAction::EyecamCalib);
    CHECK(calib.size() == 1);
    if (calib.size() == 1) CHECK(calib[0].hit.arg == static_cast<int>(eyecam::Calib::User));
    CHECK(hits(panel, PanelAction::EyecamBack).size() == 1);
    // ..."Back" pressed (the loop calls dismissError): the page
    m.eyecam.flow.dismissError(m.eyecam.lastRun, m.eyecam.status);
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamBack).empty());
    CHECK(hits(panel, PanelAction::SetBool, key::kCameraLids).size() == 2);

    // The recording's error on the Advanced tab (at the bottom of its page): "Start again" and "Back"; after "Back",
    // "Start recording" alone
    PanelModel rec = eyecamIn("error");
    rec.eyecam.lastRun = eyecam::Run::Recording;
    rec.eyecam.status.message = "右のカメラの映像が 3 秒届きません";
    panel.setTab(PanelTab::Advanced);
    panel.setAdvPage(AdvPage::Tools);
    panel.render(rec);
    CHECK(hits(panel, PanelAction::EyecamStart).size() == 1);
    CHECK(hits(panel, PanelAction::EyecamBack).size() == 1);
    rec.eyecam.flow.dismissError(rec.eyecam.lastRun, rec.eyecam.status);
    panel.render(rec);
    CHECK(hits(panel, PanelAction::EyecamStart).size() == 1);
    CHECK(hits(panel, PanelAction::EyecamBack).empty());
}

void testPupilSetup(const FontSet& fonts) {
    // The setup's (3) with an eye's pupil not found: still pressable (the video is there), with the warning under it
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Eyecam);
    PanelModel m = eyecamIn("idle", false);
    m.eyecam.status.hasPupil = true;
    m.eyecam.status.live = true;
    m.eyecam.status.pupil[0] = 0.97;
    m.eyecam.status.pupil[1] = 0.03;
    panel.render(m);
    std::vector<EyePanel::HitArea> calib = hits(panel, PanelAction::EyecamCalib);
    CHECK(calib.size() == 1);
    if (calib.size() == 1) CHECK(calib[0].hit.arg == static_cast<int>(eyecam::Calib::Wear));
    // ...not without the video
    m.eyecam.status.locked = false;
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamCalib).empty());
}

void testSearchSetup(const FontSet& fonts) {
    // The setup's (3) while the video isn't found, for each reason eyecam-rec gives (and an older one without it):
    // nothing to press but the tabs, drawn without trouble
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Eyecam);
    for (const char* reason : {"not_worn", "no_video", "one_eye", ""}) {
        for (const Language language : {Language::Ja, Language::En}) {
            PanelModel m = eyecamIn("idle", false);
            m.language = language;
            m.eyecam.status.locked = false;
            m.eyecam.status.hasBuffers = true;
            m.eyecam.status.hasSearch = reason[0] != '\0';
            m.eyecam.status.search = reason;
            m.eyecam.status.prox = 12.0;
            panel.render(m);
            CHECK(hits(panel, PanelAction::EyecamCalib).empty());
            // Beside the reason, a way to the diagnostics page (not without one)
            CHECK(hits(panel, PanelAction::DiagOpen).size() == (reason[0] != '\0' ? 1u : 0u));
        }
    }
    // Pressed, it shows the page on the Advanced tab
    PanelModel m = eyecamIn("idle", false);
    m.eyecam.status.locked = false;
    m.eyecam.status.hasBuffers = true;
    m.eyecam.status.hasSearch = true;
    m.eyecam.status.search = "not_worn";
    panel.render(m);
    const std::vector<EyePanel::HitArea> link = hits(panel, PanelAction::DiagOpen);
    CHECK(link.size() == 1);
    if (link.empty()) return;
    const PanelHit hit = panel.pointerDown(link[0].x + link[0].w / 2, link[0].y + link[0].h / 2, 0.0);
    panel.pointerUp();
    CHECK(hit.action == PanelAction::DiagOpen);
    CHECK(panel.tab() == PanelTab::Advanced && panel.diagOpen());
    panel.render(m);
    CHECK(hits(panel, PanelAction::DiagClose).size() == 1);
}

/**
 * Press the one usable button with this action.
 * @param panel the panel (rendered)
 * @param action the action
 * @return what the press returned (action None if there is no such button)
 */
PanelHit press(EyePanel& panel, PanelAction action) {
    const std::vector<EyePanel::HitArea> found = hits(panel, action);
    if (found.size() != 1) return {};
    const PanelHit hit = panel.pointerDown(found[0].x + found[0].w / 2, found[0].y + found[0].h / 2, 0.0);
    panel.pointerUp();
    return hit;
}

/**
 * Press a usable button with this action and argument.
 * @param panel the panel (rendered)
 * @param action the action
 * @param arg its argument
 * @return what the press returned (action None if there is no such button)
 */
PanelHit pressArg(EyePanel& panel, PanelAction action, int arg) {
    for (const EyePanel::HitArea& area : hits(panel, action)) {
        if (area.hit.arg != arg) continue;
        const PanelHit hit = panel.pointerDown(area.x + area.w / 2, area.y + area.h / 2, 0.0);
        panel.pointerUp();
        return hit;
    }
    return {};
}

/**
 * Press a tab in the tab row.
 * @param panel the panel (rendered)
 * @param tab the tab
 */
void pressTab(EyePanel& panel, PanelTab tab) {
    for (const EyePanel::HitArea& area : hits(panel, PanelAction::Tab)) {
        if (area.hit.arg != static_cast<int>(tab) || area.y > 100) continue;
        panel.pointerDown(area.x + area.w / 2, area.y + area.h / 2, 0.0);
        panel.pointerUp();
        return;
    }
}

/**
 * Made-up records, newest first, a minute apart.
 * @param count how many
 * @return them
 */
std::vector<report::Summary> records(int count) {
    std::vector<report::Summary> list;
    for (int i = 0; i < count; ++i) {
        report::Summary s;
        s.kind = i % 3 == 0 ? report::Kind::Fit : i % 3 == 1 ? report::Kind::CalibWear : report::Kind::Recenter;
        s.result = i % 4 == 1 ? report::Result::Failed : i % 4 == 2 ? report::Result::Partial : report::Result::Ok;
        s.start = 1791287109.0 - i * 60;
        s.end = s.start + 8;
        s.brief = i % 4 == 1 ? "正面の点で視線が落ち着きませんでした" : "";
        s.reason = "理由";
        s.folder = report::folderName(s.kind, s.start);
        list.push_back(s);
    }
    return list;
}

void testDiagPage(const FontSet& fonts) {
    // The Advanced tab offers it, with eyecam-rec and without
    for (const bool eyecam : {true, false}) {
        for (const Language language : {Language::Ja, Language::En}) {
            EyePanel panel(fonts);
            panel.setTab(PanelTab::Advanced);
            PanelModel m = modelWith(Lids::Both, eyecam);
            m.language = language;
            panel.render(m);
            // First "Version" (the version history there); the diagnostics on "Having trouble"
            CHECK(panel.advPage() == AdvPage::Version);
            CHECK(hits(panel, PanelAction::HistoryOpen).size() == 1);
            CHECK(hits(panel, PanelAction::DiagOpen).empty());
            pressArg(panel, PanelAction::AdvancedPage, static_cast<int>(AdvPage::Trouble));
            CHECK(panel.advPage() == AdvPage::Trouble);
            panel.render(m);
            CHECK(hits(panel, PanelAction::HistoryOpen).empty());
            CHECK(press(panel, PanelAction::DiagOpen).action == PanelAction::DiagOpen);
            CHECK(panel.diagOpen() && !panel.historyOpen());
            // The page: only "Back" and the tabs
            panel.render(m);
            CHECK(hits(panel, PanelAction::DiagOpen).empty());
            CHECK(hits(panel, PanelAction::HistoryOpen).empty());
            for (const EyePanel::HitArea& area : panel.hitAreas()) {
                CHECK(area.hit.action == PanelAction::Tab || area.hit.action == PanelAction::DiagClose);
            }
            // "Back" returns to "Having trouble"; another tab closes it too
            CHECK(press(panel, PanelAction::DiagClose).action == PanelAction::None);
            CHECK(!panel.diagOpen() && panel.tab() == PanelTab::Advanced);
            CHECK(panel.advPageShown() && panel.advPage() == AdvPage::Trouble);
            panel.render(m);
            press(panel, PanelAction::DiagOpen);
            panel.render(m);
            panel.pointerDown(0, 0, 0.0);  // (nothing there)
            for (const EyePanel::HitArea& area : hits(panel, PanelAction::Tab)) {
                if (area.hit.arg != static_cast<int>(PanelTab::Basic)) continue;
                panel.pointerDown(area.x + area.w / 2, area.y + area.h / 2, 0.0);
                panel.pointerUp();
            }
            CHECK(panel.tab() == PanelTab::Basic && !panel.diagOpen());
            panel.setTab(PanelTab::Advanced);
            CHECK(!panel.diagOpen());
        }
    }
    // The eye cameras' page: beside its "why no video" line only
    for (const char* reason : {"no_video", ""}) {
        EyePanel panel(fonts);
        panel.setTab(PanelTab::Eyecam);
        PanelModel m = eyecamIn("idle");
        m.eyecam.status.live = true;
        m.eyecam.status.locked = false;
        m.eyecam.status.hasSearch = reason[0] != '\0';
        m.eyecam.status.search = reason;
        m.status.camera.used[0] = m.status.camera.used[1] = false;
        m.status.camera.present = false;
        panel.render(m);
        CHECK(hits(panel, PanelAction::DiagOpen).size() == (reason[0] != '\0' ? 1u : 0u));
    }
}

/**
 * The Advanced tab's ▲ (-1) or ▼ (1) as last drawn.
 * @param panel the panel
 * @param direction -1 or 1
 * @param area where to write it
 * @return true if it is usable
 */
bool arrow(const EyePanel& panel, int direction, EyePanel::HitArea& area) {
    for (const EyePanel::HitArea& found : hits(panel, PanelAction::AdvancedScroll)) {
        if (found.hit.arg != direction) continue;
        area = found;
        return true;
    }
    return false;
}

/**
 * Press the Advanced tab's ▲ or ▼.
 * @param panel the panel
 * @param direction -1 or 1
 * @return true if it was there to press
 */
bool pressArrow(EyePanel& panel, int direction) {
    EyePanel::HitArea area;
    if (!arrow(panel, direction, area)) return false;
    panel.pointerDown(area.x + area.w / 2, area.y + area.h / 2, 0.0);
    panel.pointerUp();
    return true;
}

/**
 * The usable sub-tab buttons' pages as last drawn.
 * @param panel the panel
 * @return their args
 */
std::vector<int> subTabs(const EyePanel& panel) {
    std::vector<int> args;
    for (const EyePanel::HitArea& area : hits(panel, PanelAction::AdvancedPage)) {
        if (area.y < 200) args.push_back(area.hit.arg);
    }
    return args;
}

/**
 * The usable sub-tab buttons of the Eyelids tab as last drawn.
 * @param panel the panel
 * @return their args (the pages)
 */
std::vector<int> lidsTabs(const EyePanel& panel) {
    std::vector<int> args;
    for (const EyePanel::HitArea& area : hits(panel, PanelAction::LidsPage)) {
        if (area.y < 200) args.push_back(area.hit.arg);
    }
    return args;
}

/**
 * The image below the tabs' card (from 2 px under its edge to the bottom, right of the left column), as last drawn.
 * @param panel the panel (rendered)
 * @return its pixels
 */
std::vector<uint8_t> belowCard(EyePanel& panel) {
    const std::vector<uint8_t>& rgba = panel.toRgba();
    const size_t w = static_cast<size_t>(panel.width());
    std::vector<uint8_t> band;
    for (size_t y = 678; y < static_cast<size_t>(panel.height()); ++y) {
        const size_t from = (y * w + 410) * 4;
        band.insert(band.end(), rgba.begin() + from, rgba.begin() + (y + 1) * w * 4);
    }
    return band;
}

/** Mark ⑤ (lid_open_snap) on the Fine-tune page's bars: where it lands, faded with the cameras, gone when off. */
void testSnapMark(const FontSet& fonts) {
    // The left eye fitted with a fine-tune of 0.9 (the snap comes before it), the right one without a fit; the lid
    // marks 0.30 / 0.80 and the snap 0.53 (the defaults)
    PanelModel m = modelWith(Lids::Valve, false);
    const double fit[4] = {0.0, 1.0, 1.0, 1.0};
    for (int i = 0; i < 4; ++i) m.config.root.set(kLidFitKeys[0][i], JsonValue::makeNumber(fit[i]));
    m.config.root.set(key::kLidScaleLeft, JsonValue::makeNumber(0.9));
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Lids);
    panel.setLidsPage(LidsPage::Fine);
    panel.render(m);
    const EyePanel::SnapMark& mark = panel.snapMark();
    CHECK(mark.shown);
    // On the bars' scale: 0.30 + 0.53 / 0.75 * 0.50 = 0.6533, times 0.9 for the fitted eye = 0.588; the bars run from
    // x 472 over 682 px for 0..1.2, so 806 and 843; the bands end where the ease reaches 0.75 (0.64: 0.7267 / 0.654)
    CHECK(mark.x[0] == 806 && mark.x[1] == 843);
    CHECK(mark.endX[0] == std::round(472 + 0.7267 * 0.9 / 1.2 * 682) && mark.endX[1] == std::round(472 + 0.7267 / 1.2 * 682));
    CHECK(!mark.faded[0] && !mark.faded[1]);
    const SnapOnBar at = snapOnBar(0.53, 0.3, 0.8, 1.0);
    CHECK(std::fabs(at.start - (0.3 + 0.53 / 0.75 * 0.5)) < 1e-12 && std::fabs(at.end - (0.3 + 0.64 / 0.75 * 0.5)) < 1e-12);
    // The badge sits on its line (nothing of ① to ④ near), and the row's stepper is there
    CHECK(mark.badgeX == mark.x[0]);
    CHECK(hits(panel, PanelAction::Step, key::kLidOpenSnap).size() == 2);
    // Next to ②: the badge moves aside
    m.config.root.set(key::kLidScaleLeft, JsonValue::makeNumber(1.2));
    panel.render(m);
    const double open = std::round(472 + 0.8 / 1.2 * 682);
    CHECK(std::fabs(panel.snapMark().badgeX - open) >= 22 - 1e-9);
    m.config.root.set(key::kLidScaleLeft, JsonValue::makeNumber(0.9));
    // The cameras on the left eye: that eye's mark faded, the badge over the right eye's line
    PanelModel camera = modelWith(Lids::Left, false);
    camera.config = m.config;
    panel.render(camera);
    CHECK(panel.snapMark().shown && panel.snapMark().faded[0] && !panel.snapMark().faded[1]);
    CHECK(panel.snapMark().badgeX == panel.snapMark().x[1]);
    // Both on the cameras: both faded, still shown
    PanelModel both = modelWith(Lids::Both, false);
    both.config = m.config;
    panel.render(both);
    CHECK(panel.snapMark().shown && panel.snapMark().faded[0] && panel.snapMark().faded[1]);
    // Off (0.75): no mark, no band; the row stays
    m.config.root.set(key::kLidOpenSnap, JsonValue::makeNumber(0.75));
    panel.render(m);
    CHECK(!panel.snapMark().shown && std::isnan(panel.snapMark().x[0]));
    CHECK(hits(panel, PanelAction::Step, key::kLidOpenSnap).size() == 1);
    // Other pages: nothing
    m.config.root.set(key::kLidOpenSnap, JsonValue::makeNumber(0.53));
    panel.setLidsPage(LidsPage::Look);
    panel.render(m);
    CHECK(!panel.snapMark().shown);
}

void testLidsPages(const FontSet& fonts) {
    // The Eyelids tab's three sub-tabs, for each source of the eyelids: each with its own controls and none of the
    // others', and nothing drawn or to press below the card
    for (const Lids lids : {Lids::Valve, Lids::Saturated, Lids::Both, Lids::Left}) {
        for (const bool eyecam : {true, false}) {
            for (const Language language : {Language::Ja, Language::En}) {
                PanelModel m = modelWith(lids, eyecam);
                m.language = language;
                // What is below the card on another tab (nothing of a tab's own reaches there)
                EyePanel basic(fonts);
                basic.render(m);
                const std::vector<uint8_t> plain = belowCard(basic);
                EyePanel panel(fonts);
                panel.setTab(PanelTab::Lids);
                for (const LidsPage page : {LidsPage::Look, LidsPage::Blinks, LidsPage::Fine}) {
                    panel.setLidsPage(page);
                    panel.render(m);
                    // The other two sub-tabs can be pressed
                    std::vector<int> others;
                    for (int i = 0; i < 3; ++i) {
                        if (i != static_cast<int>(page)) others.push_back(i);
                    }
                    CHECK(lidsTabs(panel) == others);
                    const bool look = page == LidsPage::Look;
                    const bool blinks = page == LidsPage::Blinks;
                    const bool fine = page == LidsPage::Fine;
                    // Look: the widening (whatever drives it), the smoothing presets, the sync slider
                    CHECK(hits(panel, PanelAction::LidPreset).size() == (look ? 3u : 0u));
                    CHECK(hits(panel, PanelAction::NumberSlider, key::kLidSync).size() == (look ? 1u : 0u));
                    if (!look) {
                        CHECK(hits(panel, PanelAction::SetLidWiden).empty());
                        CHECK(hits(panel, PanelAction::EyecamSensitivity).empty());
                        CHECK(!pointsTo(panel, PanelTab::Eyecam));
                    } else if (lids == Lids::Valve) {
                        CHECK(hits(panel, PanelAction::SetLidWiden).size() == 4);
                    } else if (lids == Lids::Saturated) {
                        // One row: no slider, the way to the eye cameras while their tab is there
                        CHECK(hits(panel, PanelAction::SetLidWiden).empty());
                        CHECK(hits(panel, PanelAction::EyecamSensitivity).empty());
                        CHECK(pointsTo(panel, PanelTab::Eyecam) == eyecam);
                    } else {
                        CHECK(hits(panel, PanelAction::SetLidWiden).empty());
                        CHECK(hits(panel, PanelAction::EyecamSensitivity).size() == (eyecam ? 1u : 0u));
                    }
                    // Blinks & squint: the hold, both eyes together, and the eyelid floor (always there; off, its
                    // minimum, by default: only + can be pressed)
                    CHECK(hits(panel, PanelAction::Step, key::kBlinkHoldMs).empty() == !blinks);
                    CHECK(hits(panel, PanelAction::Step, key::kBlinkSyncBelow).empty() == !blinks);
                    CHECK(hits(panel, PanelAction::Step, key::kCameraLidFloor).size() == (blinks ? 1u : 0u));
                    // Fine-tune: the scales, the marks (3 and 4 not while the cameras drive both), the smoothing
                    // values, and the auto calibration for eyes without a fit, unless the cameras drive both
                    CHECK(hits(panel, PanelAction::ScaleAuto).size() == (fine ? 1u : 0u));
                    for (const char* name : {key::kLidClosed, key::kLidOpen, key::kLidMinCutoff, key::kLidBeta}) {
                        CHECK(hits(panel, PanelAction::Step, name).empty() == !fine);
                    }
                    // ...and "Treat nearly open as open" (lid_open_snap, 0.80 by default: both ways), whatever drives
                    // the eyelids
                    CHECK(hits(panel, PanelAction::Step, key::kLidOpenSnap).size() == (fine ? 2u : 0u));
                    const bool camerasBoth = lids == Lids::Both;
                    for (const char* name : {key::kLidWidenStart, key::kLidWide}) {
                        CHECK(hits(panel, PanelAction::Step, name).empty() == (!fine || camerasBoth));
                    }
                    const bool calibration = fine && !camerasBoth;
                    CHECK(hits(panel, PanelAction::SetBool, key::kLidCalibration).size() == (calibration ? 2u : 0u));
                    // Every button besides the tab row: inside the card, from the sub-tabs down
                    for (const EyePanel::HitArea& area : panel.hitAreas()) {
                        if (area.hit.action == PanelAction::Tab && area.y < 100) continue;
                        CHECK(area.x >= 400 && area.y >= 110 && area.y + area.h <= 676);
                    }
                    CHECK(belowCard(panel) == plain);
                }
            }
        }
    }
}

void testLidsRemembered(const FontSet& fonts) {
    const PanelModel m = modelWith(Lids::Valve, false);
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Lids);
    panel.render(m);
    // The first time: "Look"
    CHECK(panel.lidsPage() == LidsPage::Look);
    CHECK((lidsTabs(panel) == std::vector<int> {1, 2}));
    CHECK(pressArg(panel, PanelAction::LidsPage, static_cast<int>(LidsPage::Fine)).action == PanelAction::None);
    CHECK(panel.lidsPage() == LidsPage::Fine && panel.tab() == PanelTab::Lids);
    panel.render(m);
    CHECK(hits(panel, PanelAction::ScaleAuto).size() == 1);
    // Kept while another tab shows (no sub-tabs there), shown again when the tab is chosen again
    pressTab(panel, PanelTab::Basic);
    panel.render(m);
    CHECK(lidsTabs(panel).empty() && panel.lidsPage() == LidsPage::Fine);
    pressTab(panel, PanelTab::Lids);
    CHECK(panel.tab() == PanelTab::Lids && panel.lidsPage() == LidsPage::Fine);
    // The Advanced tab's sub-tab is its own
    panel.render(m);
    pressTab(panel, PanelTab::Advanced);
    panel.render(m);
    pressArg(panel, PanelAction::AdvancedPage, static_cast<int>(AdvPage::Files));
    CHECK(panel.advPage() == AdvPage::Files && panel.lidsPage() == LidsPage::Fine);
    // As remembered from before (the caller sets it at start)
    EyePanel next(fonts);
    next.setLidsPage(LidsPage::Blinks);
    next.setTab(PanelTab::Lids);
    next.render(m);
    CHECK((lidsTabs(next) == std::vector<int> {0, 2}));
    CHECK(!hits(next, PanelAction::Step, key::kBlinkHoldMs).empty());
}

void testSubTabs(const FontSet& fonts) {
    for (const Language language : {Language::Ja, Language::En}) {
        EyePanel panel(fonts);
        PanelModel m = modelWith(Lids::Both, true);
        m.language = language;
        m.records.list = records(4);
        panel.setTab(PanelTab::Advanced);
        panel.render(m);
        // The first time: "Version"; the other three can be pressed
        CHECK(panel.advPage() == AdvPage::Version && panel.advPageShown());
        CHECK((subTabs(panel) == std::vector<int> {1, 2, 3}));
        CHECK(hits(panel, PanelAction::UpdateCheck).size() == 1 && hits(panel, PanelAction::HistoryOpen).size() == 1);
        CHECK(hits(panel, PanelAction::SetBool, key::kUpdateCheck).size() == 1);
        // Each page with its own buttons, and nothing of the others'
        CHECK(pressArg(panel, PanelAction::AdvancedPage, static_cast<int>(AdvPage::Trouble)).action == PanelAction::None);
        CHECK(panel.advPage() == AdvPage::Trouble);
        panel.render(m);
        CHECK((subTabs(panel) == std::vector<int> {0, 2, 3}));
        CHECK(hits(panel, PanelAction::DiagOpen).size() == 1 && hits(panel, PanelAction::RecordsAll).size() == 1);
        CHECK(hits(panel, PanelAction::RecordOpen).size() == 3);
        CHECK(hits(panel, PanelAction::UpdateCheck).empty());
        pressArg(panel, PanelAction::AdvancedPage, static_cast<int>(AdvPage::Tools));
        CHECK(panel.advPage() == AdvPage::Tools);
        panel.render(m);
        CHECK(hits(panel, PanelAction::SetBool, key::kGazeDebugDots).size() == 2);
        CHECK(hits(panel, PanelAction::RecordToggle).size() == 1 && hits(panel, PanelAction::EyecamStart).size() == 1);
        CHECK(hits(panel, PanelAction::DiagOpen).empty());
        pressArg(panel, PanelAction::AdvancedPage, static_cast<int>(AdvPage::Files));
        CHECK(panel.advPage() == AdvPage::Files);
        panel.render(m);
        for (const EyePanel::HitArea& area : panel.hitAreas()) {
            CHECK(area.hit.action == PanelAction::Tab || area.hit.action == PanelAction::AdvancedPage);
        }
        // Remembered while another tab shows, and shown again when the tab is chosen again
        pressTab(panel, PanelTab::Basic);
        CHECK(panel.tab() == PanelTab::Basic && panel.advPage() == AdvPage::Files);
        panel.render(m);
        CHECK(subTabs(panel).empty());
        pressTab(panel, PanelTab::Advanced);
        CHECK(panel.tab() == PanelTab::Advanced && panel.advPage() == AdvPage::Files);
        // As remembered from before (the caller sets it at start)
        EyePanel next(fonts);
        next.setAdvPage(AdvPage::Tools);
        next.setTab(PanelTab::Advanced);
        next.render(m);
        CHECK((subTabs(next) == std::vector<int> {0, 1, 3}));
        CHECK(hits(next, PanelAction::RecordToggle).size() == 1);
        // Without eyecam-rec, "Debug tools" has no eye capture
        PanelModel plain = modelWith(Lids::Valve, false);
        next.render(plain);
        CHECK(hits(next, PanelAction::EyecamStart).empty() && hits(next, PanelAction::RecordToggle).size() == 1);
    }
}

void testUpdateNotice(const FontSet& fonts) {
    // A new release: the left column's notice opens the Advanced tab on "Version", whichever sub-tab was open
    EyePanel panel(fonts);
    PanelModel m = modelWith(Lids::Both, false);
    m.update.state = frame_updater::UpdateState::Available;
    m.update.current = "0.7.5";
    m.update.latest = "0.7.6";
    m.update.installable = true;
    m.update.notes = "Eye fit with the dashboard open.";
    panel.setAdvPage(AdvPage::Files);
    panel.setTab(PanelTab::Gaze);
    panel.render(m);
    std::vector<EyePanel::HitArea> notice;
    for (const EyePanel::HitArea& area : hits(panel, PanelAction::AdvancedPage)) {
        if (area.x < 400) notice.push_back(area);
    }
    CHECK(notice.size() == 1);
    if (notice.empty()) return;
    CHECK(notice[0].hit.arg == static_cast<int>(AdvPage::Version));
    panel.pointerDown(notice[0].x + notice[0].w / 2, notice[0].y + notice[0].h / 2, 0.0);
    panel.pointerUp();
    CHECK(panel.tab() == PanelTab::Advanced && panel.advPage() == AdvPage::Version);
    panel.render(m);
    // There: the update button, and the notice can't be pressed again
    CHECK(hits(panel, PanelAction::UpdateInstall).size() == 1);
    for (const EyePanel::HitArea& area : hits(panel, PanelAction::AdvancedPage)) CHECK(area.x > 400);
    // On another sub-tab it can
    pressArg(panel, PanelAction::AdvancedPage, static_cast<int>(AdvPage::Tools));
    panel.render(m);
    bool again = false;
    for (const EyePanel::HitArea& area : hits(panel, PanelAction::AdvancedPage)) again |= area.x < 400;
    CHECK(again);
    // Every update state's page fits and offers what it should
    using frame_updater::UpdateState;
    for (const UpdateState state : {UpdateState::Unknown, UpdateState::UpToDate, UpdateState::Available,
                                    UpdateState::Installing, UpdateState::Installed, UpdateState::CheckFailed,
                                    UpdateState::InstallFailed}) {
        for (const bool checking : {false, true}) {
            for (const Language language : {Language::Ja, Language::En}) {
                PanelModel u = m;
                u.language = language;
                u.update.state = state;
                u.update.checking = checking;
                u.update.checkedAt = 1791287109;
                u.update.error = "network";
                u.update.version = "0.7.6";
                u.update.step = "download";
                u.update.notes = std::string(400, 'x');
                panel.setTab(PanelTab::Advanced);
                panel.setAdvPage(AdvPage::Version);
                panel.render(u);
                CHECK(panel.advancedMaxScroll() == 0.0 && !panel.wantsScroll());
                const size_t installs = hits(panel, PanelAction::UpdateInstall).size();
                CHECK(installs == (state == UpdateState::Available || state == UpdateState::InstallFailed ? 1u : 0u));
                CHECK(hits(panel, PanelAction::UpdateDismiss).size() ==
                      (state == UpdateState::Installed || state == UpdateState::InstallFailed ? 1u : 0u));
                // "Check now" is always there, not while a check or an install runs
                CHECK(hits(panel, PanelAction::UpdateCheck).size() ==
                      (checking || state == UpdateState::Installing ? 0u : 1u));
                CHECK(hits(panel, PanelAction::HistoryOpen).size() == 1);
            }
        }
    }
}

void testDiagFromEyecam(const FontSet& fonts) {
    // The eye cameras tab's "Diagnostics": the page on the Advanced tab, "Back" to "Having trouble"
    EyePanel panel(fonts);
    panel.setAdvPage(AdvPage::Tools);
    panel.setTab(PanelTab::Eyecam);
    PanelModel m = eyecamIn("idle", false);
    m.eyecam.status.locked = false;
    m.eyecam.status.hasBuffers = true;
    m.eyecam.status.hasSearch = true;
    m.eyecam.status.search = "no_video";
    panel.render(m);
    CHECK(press(panel, PanelAction::DiagOpen).action == PanelAction::DiagOpen);
    CHECK(panel.tab() == PanelTab::Advanced && panel.diagOpen() && panel.advPage() == AdvPage::Trouble);
    panel.render(m);
    CHECK(press(panel, PanelAction::DiagClose).action == PanelAction::None);
    panel.render(m);
    CHECK(panel.advPageShown() && panel.advPage() == AdvPage::Trouble && hits(panel, PanelAction::DiagOpen).size() == 1);
    // The version history comes back to "Version"
    pressArg(panel, PanelAction::AdvancedPage, static_cast<int>(AdvPage::Version));
    panel.render(m);
    CHECK(press(panel, PanelAction::HistoryOpen).action == PanelAction::HistoryOpen);
    panel.render(m);
    CHECK(press(panel, PanelAction::HistoryClose).action == PanelAction::None);
    CHECK(panel.advPageShown() && panel.advPage() == AdvPage::Version);
}

void testPagesFit(const FontSet& fonts) {
    // Every sub-page fits its view (no ▲ / ▼, nothing cut), with eyecam-rec and without, in both languages, with
    // records, an eye log that failed and keys locked by the command line
    for (const bool eyecam : {true, false}) {
        for (const Language language : {Language::Ja, Language::En}) {
            for (const AdvPage page : {AdvPage::Version, AdvPage::Trouble, AdvPage::Tools, AdvPage::Files}) {
                EyePanel panel(fonts);
                PanelModel m = modelWith(Lids::Both, eyecam);
                m.language = language;
                m.records.list = records(10);
                m.recording.error = "can't create ~/.local/share/frameeyeosc/recordings: Read-only file system";
                m.status.locked = {"output", "port", "raw", "lid_open", "independent_eyes", "gaze_offset_y",
                                   "steamlink_params", "native_eyes", "camera_lids", "pupil_bits"};
                m.update.state = frame_updater::UpdateState::Available;
                m.update.installable = true;
                m.update.latest = "0.7.6";
                m.update.notes = std::string(300, 'y');
                panel.setTab(PanelTab::Advanced);
                panel.setAdvPage(page);
                panel.render(m);
                CHECK(panel.advancedMaxScroll() == 0.0);
                CHECK(!panel.wantsScroll());
                CHECK(hits(panel, PanelAction::AdvancedScroll).empty());
                CHECK(!panel.scroll(40));
                // Everything below the sub-tabs, inside the card
                for (const EyePanel::HitArea& area : panel.hitAreas()) {
                    if (area.hit.action == PanelAction::Tab || area.x < 400) continue;
                    CHECK(area.y >= 110 && area.y + area.h <= 676);
                }
            }
        }
    }
}

void testRecords(const FontSet& fonts) {
    for (const Language language : {Language::Ja, Language::En}) {
        EyePanel panel(fonts);
        PanelModel m = modelWith(Lids::Both, false);
        m.language = language;
        m.records.dir = "/home/steamos/.local/state/frameeyeosc/reports";
        // None yet: "All records" can't be pressed
        panel.setAdvPage(AdvPage::Trouble);
        panel.setTab(PanelTab::Advanced);
        panel.render(m);
        CHECK(hits(panel, PanelAction::RecordOpen).empty() && hits(panel, PanelAction::RecordsAll).empty());
        // The newest three, each with "View"
        m.records.list = records(10);
        panel.render(m);
        const std::vector<EyePanel::HitArea> rows = hits(panel, PanelAction::RecordOpen);
        CHECK(rows.size() == 3);
        if (rows.size() != 3) continue;
        CHECK(rows[0].y < rows[1].y && rows[1].y < rows[2].y);
        // One opened: the caller reads it (RecordOpen comes back), the view shows only "Back" and the tabs
        const PanelHit open = panel.pointerDown(rows[1].x + rows[1].w / 2, rows[1].y + rows[1].h / 2, 0.0);
        panel.pointerUp();
        CHECK(open.action == PanelAction::RecordOpen);
        CHECK(panel.recordShown() == m.records.list[1].folder);
        m.records.opened = m.records.list[1];
        m.records.openedFound = true;
        m.records.opened.flow = {{m.records.opened.start, report::Source::Panel, "[fit] eye fit, IPD 69.6 mm, dashboard open"}};
        m.records.files = {{"report.txt", 4000}, {"logs.txt", 86000}, {"summary.json", 2000}};
        panel.render(m);
        for (const EyePanel::HitArea& area : panel.hitAreas()) {
            CHECK(area.hit.action == PanelAction::Tab || area.hit.action == PanelAction::RecordBack);
        }
        CHECK(!panel.wantsScroll() && !panel.advPageShown());
        // ...gone meanwhile: still a way back
        m.records.openedFound = false;
        panel.render(m);
        CHECK(hits(panel, PanelAction::RecordBack).size() == 1);
        CHECK(press(panel, PanelAction::RecordBack).action == PanelAction::None);
        CHECK(panel.recordShown().empty() && panel.advPageShown() && panel.advPage() == AdvPage::Trouble);

        // All of them: taller than the view, scrolled from the top, the caller reads them again
        panel.render(m);
        CHECK(press(panel, PanelAction::RecordsAll).action == PanelAction::RecordsAll);
        CHECK(panel.recordsAllOpen());
        panel.render(m);
        CHECK(panel.wantsScroll());
        const double max = panel.recordsMaxScroll();
        CHECK(max > 0 && panel.recordsScroll() == 0.0);
        EyePanel::HitArea up;
        EyePanel::HitArea down;
        CHECK(!arrow(panel, -1, up) && arrow(panel, 1, down));
        // Only those that show can be pressed, and only the part of them that shows
        const double viewBottom = down.y + down.h;
        std::vector<EyePanel::HitArea> shown = hits(panel, PanelAction::RecordOpen);
        CHECK(shown.size() >= 6 && shown.size() < 10);
        for (const EyePanel::HitArea& area : shown) CHECK(area.y + area.h <= viewBottom + 1e-6);
        // Kept within the list
        CHECK(!panel.scroll(-50));
        CHECK(panel.scroll(1e6));
        CHECK(std::fabs(panel.recordsScroll() - max) < 1e-6);
        CHECK(!panel.scroll(10));
        panel.render(m);
        CHECK(arrow(panel, -1, up) && !arrow(panel, 1, down));
        const double viewTop = up.y;
        shown = hits(panel, PanelAction::RecordOpen);
        CHECK(!shown.empty());
        for (const EyePanel::HitArea& area : shown) CHECK(area.y >= viewTop - 1e-6);
        // ▲: a third of the view
        CHECK(pressArrow(panel, -1));
        CHECK(std::fabs(panel.recordsScroll() - std::max(0.0, max - (viewBottom - viewTop) / 3)) < 1e-6);
        panel.render(m);
        // The oldest opened from there: "Back" comes back to the list, where it was
        CHECK(panel.scroll(1e6));
        panel.render(m);
        shown = hits(panel, PanelAction::RecordOpen);
        if (!shown.empty()) {
            const EyePanel::HitArea last = shown.back();
            CHECK(panel.pointerDown(last.x + last.w / 2, last.y + last.h / 2, 0.0).action == PanelAction::RecordOpen);
            panel.pointerUp();
            CHECK(panel.recordShown() == m.records.list.back().folder);
            CHECK(!panel.recordsAllOpen() && !panel.wantsScroll());
            panel.render(m);
            CHECK(press(panel, PanelAction::RecordBack).action == PanelAction::None);
            CHECK(panel.recordsAllOpen() && std::fabs(panel.recordsScroll() - max) < 1e-6);
            panel.render(m);
            // ...and from the list to "Having trouble"
            CHECK(press(panel, PanelAction::RecordBack).action == PanelAction::None);
            CHECK(!panel.recordsAllOpen() && panel.advPageShown() && panel.advPage() == AdvPage::Trouble);
        }
        // Another tab closes them all
        panel.render(m);
        press(panel, PanelAction::RecordsAll);
        panel.render(m);
        pressTab(panel, PanelTab::Basic);
        CHECK(panel.tab() == PanelTab::Basic && !panel.recordsAllOpen());
        panel.setTab(PanelTab::Advanced);
        CHECK(panel.advPageShown());
    }
}

void testRecordBars(const FontSet& fonts) {
    // A failed fit: the bar under its box, once its record is written; pressing it shows the record on the Advanced tab
    {
        EyePanel panel(fonts);
        panel.setTab(PanelTab::EyeFit);
        PanelModel m = modelWith(Lids::Valve, false);
        m.fit.phase = gaze_fit::Phase::Failed;
        m.fit.failure = gaze_fit::Failure::Unsteady;
        panel.render(m);
        CHECK(hits(panel, PanelAction::RecordOpen).empty());
        m.records.lastFit = "fit_2026-10-06_20-45-09";
        panel.render(m);
        CHECK(hits(panel, PanelAction::RecordOpen).size() == 1);
        CHECK(hits(panel, PanelAction::FitStart).size() == 1 && hits(panel, PanelAction::FitDetails).size() == 1);
        CHECK(press(panel, PanelAction::RecordOpen).action == PanelAction::RecordOpen);
        CHECK(panel.tab() == PanelTab::Advanced && panel.recordShown() == "fit_2026-10-06_20-45-09");
        CHECK(panel.advPage() == AdvPage::Trouble);
        panel.render(m);
        CHECK(press(panel, PanelAction::RecordBack).action == PanelAction::None);
        CHECK(panel.advPageShown() && panel.advPage() == AdvPage::Trouble);
        // Stopped (no failure), or with "Fine-tune" open: no bar
        panel.setTab(PanelTab::EyeFit);
        m.fit.failure = gaze_fit::Failure::Cancelled;
        panel.render(m);
        CHECK(hits(panel, PanelAction::RecordOpen).empty());
        m.fit.failure = gaze_fit::Failure::NoMovement;
        panel.setFitDetails(true);
        panel.render(m);
        CHECK(hits(panel, PanelAction::RecordOpen).empty());
    }
    // A failed calibration from the eye cameras' page, and the setup's (3)
    for (const bool setUp : {true, false}) {
        for (const Language language : {Language::Ja, Language::En}) {
            EyePanel panel(fonts);
            panel.setTab(PanelTab::Eyecam);
            PanelModel m = eyecamIn("error", setUp);
            m.language = language;
            m.eyecam.lastRun = eyecam::Run::CalibWear;
            m.eyecam.status.calibFailedEye = "LR";
            m.eyecam.status.message =
                "両目の瞳がうまく見えなかった（HMD のかぶり方を直して、もう一度）[左 2/81・右 5/81、15 必要]";
            panel.render(m);
            CHECK(hits(panel, PanelAction::RecordOpen).empty());
            m.records.lastCalib = "calib-wear_2026-10-06_19-43-02";
            panel.render(m);
            const std::vector<EyePanel::HitArea> bar = hits(panel, PanelAction::RecordOpen);
            CHECK(bar.size() == 1);
            CHECK(hits(panel, PanelAction::EyecamCalib).size() == 1 && hits(panel, PanelAction::EyecamBack).size() == 1);
            // Inside the card, apart from the buttons
            if (bar.size() == 1) {
                CHECK(bar[0].y + bar[0].h <= 676);
                for (const EyePanel::HitArea& area : hits(panel, PanelAction::EyecamCalib)) {
                    CHECK(bar[0].y + bar[0].h <= area.y || area.y + area.h <= bar[0].y);
                }
            }
            CHECK(press(panel, PanelAction::RecordOpen).action == PanelAction::RecordOpen);
            CHECK(panel.recordShown() == "calib-wear_2026-10-06_19-43-02");
        }
    }
}

/**
 * eyecam-rec idle after a "calib wear" that went through without one eye.
 * @param setup the setup's (its done screen), or one from the usual page
 * @param widen "measured" or "default"
 * @return the model
 */
PanelModel partialResult(bool setup, const char* widen) {
    PanelModel m = eyecamIn("idle", true);
    eyecam::Status before = m.eyecam.status;
    before.state = eyecam::State::Calibrating;
    before.protocol = eyecam::kCalibWearCommand;
    before.stepCount = 6;
    before.stepIndex = 5;
    if (setup) before.setupDone = false;
    if (!setup) m.eyecam.flow.follow(m.eyecam.status, 0.0);
    m.eyecam.flow.follow(before, 0.5);
    m.eyecam.status.lastCalibWiden = widen;
    m.eyecam.status.calibFailedEye = "R";
    m.eyecam.status.message = "校正できた（右目は瞳がうまく見えなかったので、前の値を使うよ）[12/486、90 必要]";
    m.eyecam.flow.follow(m.eyecam.status, 1.0);
    m.eyecam.lastRun = eyecam::Run::CalibWear;
    return m;
}

void testPartialResult(const FontSet& fonts) {
    // One eye on its earlier values: on, or this wear's calibration again (the setup's done screen and the page's)
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Eyecam);
    for (const bool setup : {true, false}) {
        PanelModel m = partialResult(setup, "measured");
        CHECK(setup ? m.eyecam.flow.result() == eyecam::SetupResult::Done
                    : m.eyecam.flow.calibResult() == eyecam::CalibResult::Measured);
        panel.render(m);
        CHECK(hits(panel, PanelAction::SetupProceed).size() == 1);
        std::vector<EyePanel::HitArea> calib = hits(panel, PanelAction::EyecamCalib);
        CHECK(calib.size() == 1);
        if (calib.size() == 1) CHECK(calib[0].hit.arg == static_cast<int>(eyecam::Calib::Wear));
        // ...without a failed eye, as before: the done screen's one button (the page's calibrations not under it)
        m.eyecam.status.calibFailedEye.clear();
        panel.render(m);
        CHECK(hits(panel, PanelAction::SetupProceed).size() == 1);
        if (setup) CHECK(hits(panel, PanelAction::EyecamCalib).empty());
    }
    // A user calibration after it (calib_failed_eye kept from the wear's): its own result, no "again"
    {
        PanelModel m = eyecamIn("idle", true);
        m.eyecam.status.calibFailedEye = "R";
        eyecam::Status before = m.eyecam.status;
        before.state = eyecam::State::Calibrating;
        before.protocol = eyecam::kCalibUserCommand;
        before.stepCount = 4;
        before.stepIndex = 3;
        m.eyecam.flow.follow(m.eyecam.status, 0.0);
        m.eyecam.flow.follow(before, 0.5);
        m.eyecam.flow.follow(m.eyecam.status, 1.0);
        m.eyecam.lastRun = eyecam::Run::CalibUser;
        CHECK(m.eyecam.flow.calibResult() == eyecam::CalibResult::User);
        panel.render(m);
        CHECK(hits(panel, PanelAction::SetupProceed).size() == 1);
        CHECK(hits(panel, PanelAction::EyecamCalib).empty());
    }
}

void testCalibNeedsLids(const FontSet& fonts) {
    // camera_lids off: the page's calibrations can't be pressed (eyecam-rec doesn't process the video then); on: they can
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Eyecam);
    PanelModel m = eyecamIn("idle");
    m.eyecam.status.calibState = eyecam::kCalibWearBit | eyecam::kCalibAutoBit;
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamCalib).size() == 2);
    m.config.root.set(key::kCameraLids, JsonValue::makeBool(false));
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamCalib).empty());
    CHECK(hits(panel, PanelAction::SetBool, key::kCameraLids).size() == 2);  // the switch to turn it on again
}

void testWayOut(const FontSet& fonts) {
    // Every screen of the eye cameras tab and of the Advanced tab's recording offers a usable control
    struct Screen {
        const char* name;
        PanelModel model;
        PanelTab tab;
    };
    std::vector<Screen> screens;
    const auto add = [&](const char* name, PanelModel m, PanelTab tab = PanelTab::Eyecam) {
        screens.push_back({name, m, tab});
    };
    add("page", eyecamIn("idle"));
    add("page, waiting for the tool", eyecamIn("waiting_fds"));
    {
        PanelModel m = eyecamIn("calibrating");
        m.eyecam.lastRun = eyecam::Run::CalibWear;
        m.eyecam.status.stepLabel = "close";
        m.eyecam.status.stepIndex = 1;
        m.eyecam.status.stepCount = 6;
        add("page, calibrating", m);
        m.eyecam.status.stepLabel.clear();
        add("page, calibrating, waiting for the video", m);
    }
    for (const eyecam::Run run : {eyecam::Run::CalibWear, eyecam::Run::CalibUser}) {
        for (const int bits : {eyecam::kCalibAutoBit, eyecam::kCalibWearBit | eyecam::kCalibAutoBit}) {
            PanelModel m = eyecamIn("error");
            m.eyecam.lastRun = run;
            m.eyecam.status.calibState = bits;
            add("page, calibration error", m);
            m.eyecam.busy = true;  // a command on its way: the retry waits, "Back" doesn't
            add("page, calibration error, busy", m);
        }
    }
    {
        PanelModel m = eyecamIn("idle", false);
        m.eyecam.password = eyecam::PasswordState::NotSet;
        m.eyecam.status.autoGrab = "missing";
        m.eyecam.status.hasBuffers = false;
        m.eyecam.status.state = eyecam::State::WaitingFds;
        m.eyecam.status.stateText = "waiting_fds";
        add("setup (1)", m);
        m.eyecam.password = eyecam::PasswordState::Set;
        add("setup (2)", m);
    }
    add("setup (3)", eyecamIn("idle", false));
    {
        PanelModel m = eyecamIn("calibrating", false);
        m.eyecam.lastRun = eyecam::Run::CalibWear;
        m.eyecam.status.stepLabel = "widen";
        m.eyecam.status.stepIndex = 3;
        m.eyecam.status.stepCount = 6;
        add("setup (3), calibrating", m);
        PanelModel e = eyecamIn("error", false);
        e.eyecam.lastRun = eyecam::Run::CalibWear;
        add("setup (3), error", e);
        // The setup's calibration ending: widening on the standard values, or done
        for (const char* widen : {"default", "measured"}) {
            PanelModel r = eyecamIn("idle", true);
            eyecam::Status before = r.eyecam.status;
            before.state = eyecam::State::Calibrating;
            before.setupDone = false;
            r.eyecam.flow.follow(before, 0.0);
            r.eyecam.status.lastCalibWiden = widen;
            r.eyecam.flow.follow(r.eyecam.status, 1.0);
            add("setup's result", r);
        }
        // ...one eye on its earlier values (the setup's, and the page's)
        for (const char* widen : {"default", "measured"}) {
            add("setup's result, one eye", partialResult(true, widen));
            add("page's result, one eye", partialResult(false, widen));
        }
        PanelModel both = eyecamIn("error", false);
        both.eyecam.lastRun = eyecam::Run::CalibWear;
        both.eyecam.status.calibFailedEye = "LR";
        both.eyecam.status.message = "両目の瞳がうまく見えなかった（HMD のかぶり方を直して、もう一度）[左 12/486・右 30/486、90 必要]";
        add("setup (3), both eyes failed", both);
    }
    {
        PanelModel m = eyecamIn("error");
        m.eyecam.lastRun = eyecam::Run::Recording;
        add("Advanced, recording error", m, PanelTab::Advanced);
        PanelModel s = eyecamIn("searching");
        add("Advanced, searching", s, PanelTab::Advanced);
        PanelModel r = eyecamIn("recording");
        r.eyecam.status.stepLabel = "widen";
        r.eyecam.status.stepIndex = 1;
        r.eyecam.status.stepCount = 9;
        add("Advanced, recording", r, PanelTab::Advanced);
    }
    EyePanel panel(fonts);
    for (const Screen& screen : screens) {
        panel.setTab(screen.tab);
        panel.render(screen.model);
        if (!hasWayOut(panel)) {
            ++gFailures;
            std::fprintf(stderr, "FAILED: no usable control on \"%s\"\n", screen.name);
        }
    }
    // The light warning, too
    panel.setTab(PanelTab::Advanced);
    panel.openEyecamConfirm(eyecam::State::Idle);
    panel.render(eyecamIn("idle"));
    CHECK(hasWayOut(panel));
}

void testToolNotice(const FontSet& fonts) {
    // Set up, the tool outdated (still works) or too old (the cameras stopped): the usual page with a card and its
    // Konsole button, and the left column's card to the tab; neither once the tool is current again
    EyePanel panel(fonts);
    for (const char* grab : {"outdated", "too_old", "current"}) {
        PanelModel m = modelWith(Lids::Both);
        eyecam::Status& e = m.eyecam.status;
        const bool current = std::string(grab) == "current";
        e.grabOutdated = !current;
        if (std::string(grab) == "too_old") {
            e.autoGrab = "too_old";
            e.state = eyecam::State::WaitingFds;
            e.stateText = "waiting_fds";
            e.hasBuffers = false;
        }
        panel.setTab(PanelTab::Eyecam);
        panel.render(m);
        // The page, not the checklist: its rows are there either way
        CHECK(hits(panel, PanelAction::SetBool, key::kCameraLids).size() == 2);
        CHECK(hits(panel, PanelAction::SetupKonsole).size() == (current ? 0u : 1u));
        if (!current) CHECK(hits(panel, PanelAction::SetupKonsole)[0].hit.arg == 0);  // install_grab.sh, not passwd
        // From another tab, the left column's card leads to the eye cameras tab
        panel.setTab(PanelTab::Basic);
        panel.render(m);
        CHECK(pointsTo(panel, PanelTab::Eyecam) == !current);
    }
}

void testFitRunning(const FontSet& fonts) {
    // Before: the fit's buttons, no "Stop"
    EyePanel panel(fonts);
    panel.setTab(PanelTab::EyeFit);
    PanelModel m = modelWith(Lids::Valve, false);
    panel.render(m);
    CHECK(hits(panel, PanelAction::FitStart).size() == 1 && hits(panel, PanelAction::FitCenter).size() == 1);
    CHECK(hits(panel, PanelAction::FitStop).empty());
    // Running (started with the dashboard open, so the panel shows behind the dots): one big "Stop" low in the card
    // and the tabs; nothing else can be pressed (the rest is dimmed), in every step and mode
    for (const gaze_fit::Phase phase : {gaze_fit::Phase::Settling, gaze_fit::Phase::Capturing, gaze_fit::Phase::Reopen}) {
        for (const gaze_fit::Mode mode : {gaze_fit::Mode::Full, gaze_fit::Mode::Center, gaze_fit::Mode::Tilt}) {
            m.fit = gaze_fit::View();
            m.fit.phase = phase;
            m.fit.mode = mode;
            panel.render(m);
            const std::vector<EyePanel::HitArea> stop = hits(panel, PanelAction::FitStop);
            CHECK(stop.size() == 1);
            CHECK(stop.size() == 1 && stop[0].w >= 300 && stop[0].h >= 80 && stop[0].y > 500);
            CHECK(hits(panel, PanelAction::Tab).size() >= 5);  // the other tabs
            for (const EyePanel::HitArea& area : panel.hitAreas()) {
                CHECK(area.hit.action == PanelAction::Tab || area.hit.action == PanelAction::FitStop);
            }
        }
    }
    // Pressing "Stop" gives it to the caller (which stops the fit)
    {
        const EyePanel::HitArea stop = hits(panel, PanelAction::FitStop)[0];
        const PanelHit hit = panel.pointerDown(stop.x + stop.w / 2, stop.y + stop.h / 2, 0.0);
        panel.pointerUp();
        CHECK(hit.action == PanelAction::FitStop);
    }
    // Another tab chosen: the tab changes (the caller sees it and stops the fit)
    for (const EyePanel::HitArea& area : hits(panel, PanelAction::Tab)) {
        if (area.hit.arg != static_cast<int>(PanelTab::Basic)) continue;
        panel.pointerDown(area.x + area.w / 2, area.y + area.h / 2, 0.0);
        panel.pointerUp();
    }
    CHECK(panel.tab() == PanelTab::Basic);
    // A fit running while another tab shows (re-centering when the headset was put on): that tab as usual
    panel.render(m);
    CHECK(hits(panel, PanelAction::FitStop).empty() && panel.hitAreas().size() > hits(panel, PanelAction::Tab).size());
    // Stopped: back to the buttons, with why
    panel.setTab(PanelTab::EyeFit);
    m.fit.phase = gaze_fit::Phase::Failed;
    m.fit.failure = gaze_fit::Failure::Cancelled;
    panel.render(m);
    CHECK(hits(panel, PanelAction::FitStop).empty() && hits(panel, PanelAction::FitStart).size() == 1);
    CHECK(hits(panel, PanelAction::FitDetails).size() == 1);
}

/**
 * The ink drawn in a column range of a surface.
 * @param surface the image (ARGB32)
 * @param x0 first column
 * @param x1 one past the last column
 * @param top first row with ink (written; -1 if none)
 * @param bottom last row with ink (written)
 * @return how many pixels have ink
 */
int inkIn(cairo_surface_t* surface, int x0, int x1, int& top, int& bottom) {
    cairo_surface_flush(surface);
    const int w = cairo_image_surface_get_width(surface);
    const int h = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    int count = 0;
    top = -1;
    bottom = -1;
    for (int y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(data + static_cast<size_t>(y) * stride);
        for (int x = std::max(0, x0); x < std::min(w, x1); ++x) {
            if ((row[x] >> 24) < 40) continue;
            ++count;
            if (top < 0) top = y;
            bottom = y;
        }
    }
    return count;
}

void testIconText(const FontSet& fonts) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 400, 80);
    cairo_t* cr = cairo_create(surface);
    const Pen pen {cr, &fonts};
    const Color white {1, 1, 1};
    // Measuring: an icon is its own width between the runs of text (the font isn't asked for it)
    for (const bool bold : {false, true}) {
        const double size = 17;
        const double arrow = icon::advance(icon::Icon::ArrowRight, size);
        CHECK(arrow > size * 0.8 && arrow < size * 1.1);
        const double joined = pen.measure("Enter " ICON_ARROW_RIGHT " password", size, bold);
        const double parts = pen.measure("Enter ", size, bold) + arrow + pen.measure(" password", size, bold);
        CHECK(std::fabs(joined - parts) < 0.5);
        const double back = pen.measure(ICON_CHEVRON_LEFT " 戻る", size, bold);
        CHECK(std::fabs(back - icon::advance(icon::Icon::ChevronLeft, size) - pen.measure(" 戻る", size, bold)) < 0.5);
        // Drawn as wide as measured, left or right aligned
        CHECK(std::fabs(pen.text(10, 40, "Enter " ICON_ARROW_RIGHT " password", size, white, bold) - joined) < 0.5);
        CHECK(std::fabs(pen.text(390, 40, "Enter " ICON_ARROW_RIGHT " password", size, white, bold, true) - joined) <
              0.5);
    }
    // Drawing: each icon puts ink inside its own box only, around the middle of the line
    for (int k = 1; k <= 16; ++k) {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_restore(cr);
        const std::string marker = std::string("\xEE\x80") + static_cast<char>(0x80 + k - 1);
        const icon::Icon which = icon::at(marker, 0);
        CHECK(which != icon::Icon::None);
        const double size = 20;
        const double x = 100;
        const double baseline = 50;
        const double w = pen.text(x, baseline, "a" + marker + "a", size, white);
        const double aW = pen.measure("a", size, false);
        CHECK(std::fabs(w - aW * 2 - icon::advance(which, size)) < 0.5);
        int top = 0;
        int bottom = 0;
        const double boxLeft = x + aW;
        const double boxRight = boxLeft + icon::advance(which, size);
        const int inside = inkIn(surface, static_cast<int>(boxLeft + 1), static_cast<int>(boxRight), top, bottom);
        CHECK(inside > 10);
        // Within the line, like a kanji: from about the capitals' top down to just under the baseline
        CHECK(top >= baseline - size * 0.9 && bottom <= baseline + size * 0.12 + 1);
        if (which != icon::Icon::Note) {
            // The others are centred about a third of the size above the baseline
            const double middle = (top + bottom) / 2.0;
            CHECK(middle > baseline - size * 0.42 && middle < baseline - size * 0.22);
        }
    }
    // Wrapping: an arrow or a "›" never starts a line, nor a closing mark (one with an arrow joined to it, "」→",
    // neither), and nothing is lost between the lines
    for (const Language language : {Language::Ja, Language::En}) {
        const UiText& t = uiText(language);
        for (const char* text : {t.setupPassKonsoleHow, t.setupCheckFlow, t.setupPassMemoText, t.recordSavedFit,
                                 t.recordSavedCalib,
                                 "「詳細」" ICON_ARROW_RIGHT "「困ったとき」" ICON_ARROW_RIGHT "「最近の記録」"}) {
            for (double width = 60; width <= 600; width += 7) {
                const std::vector<std::string> lines = wrapText(pen, text, 15, false, width, 20);
                std::string joined;
                for (size_t n = 0; n < lines.size(); ++n) {
                    const std::string& line = lines[n];
                    const size_t first = line.find_first_not_of(' ');
                    CHECK(first == std::string::npos || (icon::at(line, first) != icon::Icon::ArrowRight &&
                                                         icon::at(line, first) != icon::Icon::ChevronRight));
                    if (n > 0 && first != std::string::npos) {
                        for (const char* mark : {"」", "）", "。", "、"}) {
                            CHECK(line.compare(first, std::strlen(mark), mark) != 0);
                        }
                    }
                    joined += line;
                }
                // (unless a word wider than the line was cut)
                if (joined.find("…") != std::string::npos) continue;
                std::string wanted = text;
                wanted.erase(std::remove(wanted.begin(), wanted.end(), ' '), wanted.end());
                joined.erase(std::remove(joined.begin(), joined.end(), ' '), joined.end());
                CHECK(joined == wanted);
            }
        }
    }
    // Cutting with "…": an icon goes whole or not at all
    for (double width = 10; width <= 300; width += 3) {
        const std::string cut = ellipsize(pen, "正面 1 回目 0/153 " ICON_ARROW_RIGHT " もう一度", 14, false, width, false);
        CHECK(icon::plain(cut).find('\xEE') == std::string::npos);
        CHECK(pen.measure(cut, 14, false) <= std::max(width, pen.measure("…", 14, false)) + 0.01);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

int main() {
    FontSet fonts;
    fonts.load(kFontPath, kBoldFontPath);
    testWidenRouting(fonts);
    testLidsPages(fonts);
    testSnapMark(fonts);
    testLidsRemembered(fonts);
    testSliderMoved(fonts);
    testNumberSlider(fonts);
    testPupilBitsRow(fonts);
    testToolNotice(fonts);
    testErrorBack(fonts);
    testWayOut(fonts);
    testCalibNeedsLids(fonts);
    testPupilSetup(fonts);
    testSearchSetup(fonts);
    testDiagPage(fonts);
    testSubTabs(fonts);
    testUpdateNotice(fonts);
    testDiagFromEyecam(fonts);
    testPagesFit(fonts);
    testRecords(fonts);
    testRecordBars(fonts);
    testPartialResult(fonts);
    testFitRunning(fonts);
    testIconText(fonts);
    if (gFailures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("panel-test: all passed\n");
    return 0;
}
