// Tests for the Eyelids and eye cameras tabs as drawn (案E): which buttons they offer for each source of the eyelids
// (the widening slider's routing, its three sub-tabs each with its own controls inside the card and remembered while
// other tabs show, the sensitivity slider moved off the eye cameras page)
// and a setting's slider let go of, the setup's (3) with a pupil not found, and a calibration that went through without
// one eye, the Advanced tab's sub-tabs (switching, remembered while other tabs show, the update notice to "Version",
// the diagnostics from the eye cameras tab back to "Having trouble"), its pages fitting, the records (the newest three,
// one open, all of them scrolled: kept within the list, ▲ / ▼, only what shows can be pressed) and the failure screens'
// bar, the Eye fit tab while a fit runs (only "Stop" and the tabs), the icons in texts (measured and drawn in their
// own boxes, never starting a line, cut whole), and the other Frame apps (the row at the bottom of "Version" and its
// count, the list's rows by state, the confirmation's command and buttons, scrolling), and the eye preview (its button
// in the left column, alone or beside a notice; its sheet's buttons, what closes it, freeze, without the cameras), and
// "Help train the AI" (its row on Debug tools, each screen's buttons, its recording, the delete's confirmation). Built
// with the panel as panel-test (it renders offscreen with the panel's fonts) and
// run after it is built; exits non-zero on failure.
#include "config.h"
#include "draw.h"
#include "frame_apps.h"
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
    // Valve's values (the eye cameras in use, neither eye on them now): lid_widen's four stops side by side
    {
        EyePanel panel(fonts);
        panel.setTab(PanelTab::Lids);
        panel.render(modelWith(Lids::Valve));
        const std::vector<EyePanel::HitArea> stops = hits(panel, PanelAction::SetLidWiden, key::kLidWiden);
        CHECK(stops.size() == 4);
        for (size_t i = 0; i < stops.size(); ++i) {
            CHECK(stops[i].hit.arg == static_cast<int>(i));
            if (i > 0) CHECK(std::fabs(stops[i - 1].x + stops[i - 1].w - stops[i].x) < 1e-6);
        }
        CHECK(hits(panel, PanelAction::EyecamSensitivity).empty());
        // ...locked on frameeyeosc's command line: none
        PanelModel locked = modelWith(Lids::Valve);
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

void testRelearnButton(const FontSet& fonts) {
    // "Relearn my eyes" at the right of the eye cameras' state row: usable while eyecam-rec reads the cameras live
    // with the eye video and the cameras drive the eyelids (or learn to)
    EyePanel panel(fonts);
    PanelModel m = modelWith(Lids::Both);
    panel.setTab(PanelTab::Eyecam);
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamRelearn).empty());  // not live yet
    eyecam::Status& e = m.eyecam.status;
    e.live = e.locked = true;
    e.hasBaseline = true;
    e.baseline = "ready";
    panel.render(m);
    const std::vector<EyePanel::HitArea> relearn = hits(panel, PanelAction::EyecamRelearn);
    CHECK(relearn.size() == 1);
    // ...on the card, within the panel's right edge, at the state row (above the calibration buttons)
    const std::vector<EyePanel::HitArea> calib = hits(panel, PanelAction::EyecamCalib);
    if (relearn.size() == 1 && !calib.empty()) {
        CHECK(relearn[0].x + relearn[0].w <= panel.width() && relearn[0].y + relearn[0].h <= calib[0].y);
    }
    // While learning (the countdown in the pill) it can be pressed again; not while a command waits, nor with the
    // cameras unused, camera_lids off or no eye video
    e.baseline = "warming";
    e.warmupRemainingS = 12;
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamRelearn).size() == 1);
    m.eyecam.busy = true;
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamRelearn).empty());
    m.eyecam.busy = false;
    e.locked = false;
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamRelearn).empty());
    e.locked = true;
    PanelModel valve = modelWith(Lids::Valve);
    valve.eyecam.status = e;
    valve.status.camera.calibState = 5;
    panel.render(valve);
    CHECK(hits(panel, PanelAction::EyecamRelearn).empty());
    // The model's rule: the cameras in use, or learning to
    CHECK(relearnUse(CameraUse::Both) && relearnUse(CameraUse::Left) && relearnUse(CameraUse::Right) &&
          relearnUse(CameraUse::Warming));
    for (CameraUse other : {CameraUse::Unknown, CameraUse::Off, CameraUse::NotCalibrated, CameraUse::NoCamera,
                            CameraUse::Error, CameraUse::Valve}) {
        CHECK(!relearnUse(other));
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
    CHECK(hits(panel, PanelAction::SetBool, key::kCameraLids).empty());  // the switch is on the Basic tab now
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
        // (the eye preview's button in the left column is always there: not a way out of the tab)
        if (area.hit.action == PanelAction::PreviewOpen) continue;
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
    CHECK(hits(panel, PanelAction::LidsPage).size() == 1);  // the page's pointer to the widening

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

void testWidenSwitch(const FontSet& fonts) {
    // "Widening" on the Eyelids tab's "Look": On / Off (widen), whatever drives the eyelids; Off greys the slider
    for (const Lids lids : {Lids::Valve, Lids::Saturated, Lids::Both, Lids::Left}) {
        for (const bool eyecam : {true, false}) {
            for (const Language language : {Language::Ja, Language::En}) {
                EyePanel panel(fonts);
                panel.setTab(PanelTab::Lids);
                panel.setLidsPage(LidsPage::Look);
                PanelModel m = modelWith(lids, eyecam);
                m.language = language;
                panel.render(m);
                std::vector<EyePanel::HitArea> both = hits(panel, PanelAction::SetBool, key::kWiden);
                CHECK(both.size() == 2);
                if (both.size() == 2) CHECK(both[0].hit.arg == 1 && both[1].hit.arg == 0);
                // Pressing "Off" asks for widen = false
                const PanelHit off = pressArg(panel, PanelAction::SetBool, 0);
                CHECK(off.action == PanelAction::SetBool && off.arg == 0 && std::string(off.key) == key::kWiden);
                // Off: the slider can't be moved (nothing it sets shows), the switch still can
                m.config.root.set(key::kWiden, JsonValue::makeBool(false));
                panel.render(m);
                CHECK(hits(panel, PanelAction::SetBool, key::kWiden).size() == 2);
                CHECK(hits(panel, PanelAction::EyecamSensitivity).empty());
                CHECK(hits(panel, PanelAction::SetLidWiden).empty());
                for (const EyePanel::HitArea& area : panel.hitAreas()) {
                    if (area.hit.action == PanelAction::Tab && area.y < 100) continue;
                    if (area.hit.action == PanelAction::PreviewOpen) continue;
                    CHECK(area.x >= 400 && area.y >= 110 && area.y + area.h <= 676);
                }
                // Locked by frameeyeosc's command line (--no-widen): nothing to press
                m.status.locked.push_back(key::kWiden);
                panel.render(m);
                CHECK(hits(panel, PanelAction::SetBool, key::kWiden).empty());
            }
        }
    }
}

void testLidVetoSwitch(const FontSet& fonts) {
    // "Half-shut check (AI)" on the Eyelids tab's "Look": On / Off, only with the eye cameras in use and an eyecam-rec
    // that says lid_veto; it shows what status.json says
    for (const Lids lids : {Lids::Valve, Lids::Saturated, Lids::Both, Lids::Left}) {
        for (const Language language : {Language::Ja, Language::En}) {
            EyePanel panel(fonts);
            panel.setTab(PanelTab::Lids);
            panel.setLidsPage(LidsPage::Look);
            PanelModel m = modelWith(lids);
            m.language = language;
            // An older eyecam-rec without lid_veto: no row
            panel.render(m);
            CHECK(hits(panel, PanelAction::EyecamLidVeto).empty());
            m.eyecam.status.hasLidVeto = true;
            for (const bool on : {false, true}) {
                m.eyecam.status.lidVeto = on;
                panel.render(m);
                const std::vector<EyePanel::HitArea> both = hits(panel, PanelAction::EyecamLidVeto);
                CHECK(both.size() == 2);
                if (both.size() == 2) CHECK(both[0].hit.arg == 1 && both[1].hit.arg == 0);
                // Pressing the other choice asks eyecam-rec for it
                const PanelHit hit = pressArg(panel, PanelAction::EyecamLidVeto, on ? 0 : 1);
                CHECK(hit.action == PanelAction::EyecamLidVeto && hit.arg == (on ? 0 : 1));
                // Everything still on the page
                for (const EyePanel::HitArea& area : panel.hitAreas()) {
                    if (area.hit.action == PanelAction::Tab && area.y < 100) continue;
                    if (area.hit.action == PanelAction::PreviewOpen) continue;
                    CHECK(area.x >= 400 && area.y >= 110 && area.y + area.h <= 676);
                }
            }
            // The cameras switched off (camera_lids), or eyecam-rec gone: no row
            m.config.root.set(key::kCameraLids, JsonValue::makeBool(false));
            panel.render(m);
            CHECK(hits(panel, PanelAction::EyecamLidVeto).empty());
            m.config.root.set(key::kCameraLids, JsonValue::makeBool(true));
            m.eyecam.visible = false;
            panel.render(m);
            CHECK(hits(panel, PanelAction::EyecamLidVeto).empty());
        }
    }
    // Not set up: no row
    PanelModel m = modelWith(Lids::Both, false);
    m.eyecam.status.hasLidVeto = true;
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Lids);
    panel.setLidsPage(LidsPage::Look);
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamLidVeto).empty());
    // Not on the other pages
    m = modelWith(Lids::Both);
    m.eyecam.status.hasLidVeto = true;
    for (const LidsPage page : {LidsPage::Blinks, LidsPage::Fine}) {
        panel.setLidsPage(page);
        panel.render(m);
        CHECK(hits(panel, PanelAction::EyecamLidVeto).empty());
    }
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
                CHECK(area.hit.action == PanelAction::Tab || area.hit.action == PanelAction::DiagClose ||
                      area.hit.action == PanelAction::PreviewOpen);
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
    // x 472 over 682 px for 0..1.2, so 806 and 843
    CHECK(mark.x[0] == 806 && mark.x[1] == 843);
    CHECK(!mark.faded[0] && !mark.faded[1]);
    CHECK(std::fabs(snapOnBar(0.53, 0.3, 0.8, 1.0) - (0.3 + 0.53 / 0.75 * 0.5)) < 1e-12);
    CHECK(std::fabs(snapOnBar(0.53, 0.3, 0.8, 0.9) - (0.3 + 0.53 / 0.75 * 0.5) * 0.9) < 1e-12);
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

void testEyeBehavior(const FontSet& fonts) {
    // Advanced > Version: "How the eyes move", v2 (the default) or v1; both segments to press, nothing when locked
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Advanced);
    panel.setAdvPage(AdvPage::Version);
    PanelModel m = modelWith(Lids::Valve);
    panel.render(m);
    std::vector<EyePanel::HitArea> found = hits(panel, PanelAction::SetInteger, key::kEyeBehavior);
    CHECK(found.size() == 2 && found[0].hit.arg == 2 && found[1].hit.arg == 1);
    for (const EyePanel::HitArea& area : found) CHECK(area.y >= 110 && area.y + area.h <= 676);
    PanelModel locked = m;
    locked.status.locked.push_back(key::kEyeBehavior);
    panel.render(locked);
    CHECK(hits(panel, PanelAction::SetInteger, key::kEyeBehavior).empty());

    // v1: the Eyelids rows that only v2 uses are faded (nothing to press) with a button to that switch, and (5) is not
    // drawn on the bars; v2 has them as usual
    PanelModel v1 = m;
    v1.config.root.set(key::kEyeBehavior, JsonValue::makeNumber(1, true));
    const auto toVersion = [&]() {
        for (const EyePanel::HitArea& area : hits(panel, PanelAction::AdvancedPage)) {
            if (area.hit.arg == static_cast<int>(AdvPage::Version) && area.y > 100) return true;
        }
        return false;
    };
    panel.setTab(PanelTab::Lids);
    panel.setLidsPage(LidsPage::Fine);
    panel.render(m);
    CHECK(panel.snapMark().shown && hits(panel, PanelAction::Step, key::kLidOpenSnap).size() == 2 && !toVersion());
    panel.render(v1);
    CHECK(!panel.snapMark().shown && hits(panel, PanelAction::Step, key::kLidOpenSnap).empty() && toVersion());
    panel.setLidsPage(LidsPage::Blinks);
    m.config.root.set(key::kCameraLidFloor, JsonValue::makeNumber(0.25));
    v1.config.root.set(key::kCameraLidFloor, JsonValue::makeNumber(0.25));
    panel.render(m);
    CHECK(hits(panel, PanelAction::Step, key::kCameraLidFloor).size() == 2 && !toVersion());
    panel.render(v1);
    CHECK(hits(panel, PanelAction::Step, key::kCameraLidFloor).empty() && toVersion());
    // The button goes there
    pressArg(panel, PanelAction::AdvancedPage, static_cast<int>(AdvPage::Version));
    CHECK(panel.tab() == PanelTab::Advanced && panel.advPage() == AdvPage::Version);
}

void testBrowsPage(const FontSet& fonts) {
    // The Eyelids tab's "Brows": "Widen raises brows" On / Off (brow_from_widen), whatever drives the eyelids and
    // whatever "Widening" says; "Brow raise strength" (brow_strength) only while it is on
    for (const Lids lids : {Lids::Valve, Lids::Saturated, Lids::Both, Lids::Left}) {
        for (const bool eyecam : {true, false}) {
            for (const Language language : {Language::Ja, Language::En}) {
                for (const bool widen : {true, false}) {
                    EyePanel panel(fonts);
                    panel.setTab(PanelTab::Lids);
                    panel.setLidsPage(LidsPage::Brows);
                    PanelModel m = modelWith(lids, eyecam);
                    m.language = language;
                    if (!widen) m.config.root.set(key::kWiden, JsonValue::makeBool(false));
                    panel.render(m);
                    // The four sub-tabs side by side in the bar, the others pressable, in order
                    CHECK((lidsTabs(panel) == std::vector<int> {0, 1, 2}));
                    std::vector<EyePanel::HitArea> tabs;
                    for (const EyePanel::HitArea& area : hits(panel, PanelAction::LidsPage)) {
                        if (area.y < 200) tabs.push_back(area);
                    }
                    for (size_t i = 1; i < tabs.size(); ++i) CHECK(tabs[i].x >= tabs[i - 1].x + tabs[i - 1].w - 1);
                    std::vector<EyePanel::HitArea> both = hits(panel, PanelAction::SetBool, key::kBrowFromWiden);
                    CHECK(both.size() == 2);
                    if (both.size() == 2) CHECK(both[0].hit.arg == 1 && both[1].hit.arg == 0);
                    CHECK(hits(panel, PanelAction::NumberSlider, key::kBrowStrength).empty());
                    // Pressing "On" asks for brow_from_widen = true
                    const PanelHit on = pressArg(panel, PanelAction::SetBool, 1);
                    CHECK(on.action == PanelAction::SetBool && on.arg == 1 && std::string(on.key) == key::kBrowFromWiden);
                    // On: the strength's slider below it; nothing else of the Eyelids tab's here
                    m.config.root.set(key::kBrowFromWiden, JsonValue::makeBool(true));
                    panel.render(m);
                    CHECK(hits(panel, PanelAction::SetBool, key::kBrowFromWiden).size() == 2);
                    const std::vector<EyePanel::HitArea> sliders = hits(panel, PanelAction::NumberSlider, key::kBrowStrength);
                    CHECK(sliders.size() == 1);
                    if (sliders.size() == 1) CHECK(sliders[0].y > both[0].y + both[0].h + 30);
                    CHECK(hits(panel, PanelAction::SetBool, key::kWiden).empty());
                    CHECK(hits(panel, PanelAction::NumberSlider, key::kLidSync).empty());
                    CHECK(hits(panel, PanelAction::LidPreset).empty());
                    for (const EyePanel::HitArea& area : panel.hitAreas()) {
                        if (area.hit.action == PanelAction::Tab && area.y < 100) continue;
                        if (area.hit.action == PanelAction::PreviewOpen) continue;
                        CHECK(area.x >= 400 && area.y >= 110 && area.y + area.h <= 676);
                    }
                    // Locked by frameeyeosc's command line: nothing to press
                    m.status.locked.push_back(key::kBrowFromWiden);
                    m.status.locked.push_back(key::kBrowStrength);
                    panel.render(m);
                    CHECK(hits(panel, PanelAction::SetBool, key::kBrowFromWiden).empty());
                    CHECK(hits(panel, PanelAction::NumberSlider, key::kBrowStrength).empty());
                }
            }
        }
    }
    // The strength's slider gives its value on the 0.05 grid, 0..1
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Lids);
    panel.setLidsPage(LidsPage::Brows);
    PanelModel m = modelWith(Lids::Valve, false);
    m.config.root.set(key::kBrowFromWiden, JsonValue::makeBool(true));
    panel.render(m);
    const std::vector<EyePanel::HitArea> sliders = hits(panel, PanelAction::NumberSlider, key::kBrowStrength);
    CHECK(sliders.size() == 1);
    if (sliders.size() != 1) return;
    const EyePanel::HitArea& area = sliders[0];
    std::string name;
    double value = 0.0;
    panel.pointerDown(area.x + 18 + 2, area.y + area.h / 2, 0.0);
    panel.pointerUp();
    CHECK(panel.takeNumberSlider(name, value));
    CHECK(name == key::kBrowStrength && std::fabs(value) < 1e-9);
    panel.pointerDown(area.x + area.w * 0.62, area.y + area.h / 2, 0.0);
    panel.pointerUp();
    CHECK(panel.takeNumberSlider(name, value));
    CHECK(name == key::kBrowStrength && std::fabs(value / 0.05 - std::round(value / 0.05)) < 1e-6);
    CHECK(value > 0.4 && value < 0.8);
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
                for (const LidsPage page : {LidsPage::Look, LidsPage::Blinks, LidsPage::Fine, LidsPage::Brows}) {
                    panel.setLidsPage(page);
                    panel.render(m);
                    // The other three sub-tabs can be pressed
                    std::vector<int> others;
                    for (int i = 0; i < 4; ++i) {
                        if (i != static_cast<int>(page)) others.push_back(i);
                    }
                    CHECK(lidsTabs(panel) == others);
                    const bool look = page == LidsPage::Look;
                    const bool blinks = page == LidsPage::Blinks;
                    const bool fine = page == LidsPage::Fine;
                    const bool brows = page == LidsPage::Brows;
                    // Brows: raising them with widening (off by default: no strength), with the eye cameras or without
                    CHECK(hits(panel, PanelAction::SetBool, key::kBrowFromWiden).size() == (brows ? 2u : 0u));
                    CHECK(hits(panel, PanelAction::NumberSlider, key::kBrowStrength).empty());
                    // Look: the widening (whatever drives it), the smoothing presets, the sync slider
                    CHECK(hits(panel, PanelAction::LidPreset).size() == (look ? 4u : 0u));
                    // ...and widening at all (widen), with the eye cameras or without
                    CHECK(hits(panel, PanelAction::SetBool, key::kWiden).size() == (look ? 2u : 0u));
                    CHECK(hits(panel, PanelAction::NumberSlider, key::kLidSync).size() == (look ? 1u : 0u));
                    if (!look) {
                        CHECK(hits(panel, PanelAction::SetLidWiden).empty());
                        CHECK(hits(panel, PanelAction::EyecamSensitivity).empty());
                        CHECK(!pointsTo(panel, PanelTab::Eyecam));
                    } else if (lids == Lids::Valve) {
                        // (only while the eye cameras are in use: one line about them in its place otherwise)
                        CHECK(hits(panel, PanelAction::SetLidWiden).size() == (eyecam ? 4u : 0u));
                    } else if (lids == Lids::Saturated) {
                        // One row: no slider, the way to the eye cameras while their tab is there
                        CHECK(hits(panel, PanelAction::SetLidWiden).empty());
                        CHECK(hits(panel, PanelAction::EyecamSensitivity).empty());
                        CHECK(pointsTo(panel, PanelTab::Eyecam) == eyecam);
                    } else {
                        CHECK(hits(panel, PanelAction::SetLidWiden).empty());
                        CHECK(hits(panel, PanelAction::EyecamSensitivity).size() == (eyecam ? 1u : 0u));
                    }
                    // Blinks & squint: the hold, both eyes together, and the eyelid floor (while the eye cameras are in
                    // use; off, its minimum, by default: only + can be pressed)
                    CHECK(hits(panel, PanelAction::Step, key::kBlinkHoldMs).empty() == !blinks);
                    CHECK(hits(panel, PanelAction::Step, key::kBlinkSyncBelow).empty() == !blinks);
                    CHECK(hits(panel, PanelAction::Step, key::kCameraLidFloor).size() == (blinks && eyecam ? 1u : 0u));
                    // Fine-tune: the scales, the marks (3 and 4 not while the cameras drive both), the smoothing
                    // values, and the auto calibration for eyes without a fit, unless the cameras drive both
                    CHECK(hits(panel, PanelAction::ScaleAuto).size() == (fine ? 1u : 0u));
                    for (const char* name : {key::kLidClosed, key::kLidOpen, key::kLidMinCutoff, key::kLidBeta}) {
                        CHECK(hits(panel, PanelAction::Step, name).empty() == !fine);
                    }
                    // ...and "Treat nearly open as open" (lid_open_snap, 0.53 by default: both ways), whatever drives
                    // the eyelids
                    CHECK(hits(panel, PanelAction::Step, key::kLidOpenSnap).size() == (fine ? 2u : 0u));
                    const bool camerasBoth = lids == Lids::Both;
                    for (const char* name : {key::kLidWidenStart, key::kLidWide}) {
                        CHECK(hits(panel, PanelAction::Step, name).empty() == (!fine || camerasBoth));
                    }
                    const bool calibration = fine && !camerasBoth;
                    CHECK(hits(panel, PanelAction::SetBool, key::kLidCalibration).size() == (calibration ? 2u : 0u));
                    // Every button besides the tab row (and the eye preview's in the left column): inside the card,
                    // from the sub-tabs down
                    for (const EyePanel::HitArea& area : panel.hitAreas()) {
                        if (area.hit.action == PanelAction::Tab && area.y < 100) continue;
                        if (area.hit.action == PanelAction::PreviewOpen) continue;
                        CHECK(area.x >= 400 && area.y >= 110 && area.y + area.h <= 676);
                    }
                    CHECK(belowCard(panel) == plain);
                }
            }
        }
    }
}

/**
 * The usable sub-tab buttons of the Gaze tab as last drawn.
 * @param panel the panel
 * @return their args (the pages)
 */
std::vector<int> gazeTabs(const EyePanel& panel) {
    std::vector<int> args;
    for (const EyePanel::HitArea& area : hits(panel, PanelAction::GazePage)) {
        if (area.y < 200) args.push_back(area.hit.arg);
    }
    return args;
}

void testGazePages(const FontSet& fonts) {
    // The Gaze tab's three sub-tabs: each with its own rows and none of the others', everything inside the card
    // (no scrolling), in both languages, with keys locked by the command line or not, v1 or v2, one eye or both
    const char* scaleKeys[4] = {key::kGazeScaleLeft, key::kGazeScaleRight, key::kGazeScaleUp, key::kGazeScaleDown};
    for (const Language language : {Language::Ja, Language::En}) {
        for (const bool locked : {false, true}) {
            for (const bool v1 : {false, true}) {
                PanelModel m = modelWith(Lids::Valve, false);
                m.language = language;
                if (locked) m.status.locked = {"raw", "independent_eyes", "gaze_hold_below", "gaze_scale_right"};
                if (v1) m.config.root.set(key::kEyeBehavior, JsonValue::makeNumber(1, true));
                if (v1) m.status.dominantEye = "left";
                EyePanel basic(fonts);
                basic.render(m);
                const std::vector<uint8_t> plain = belowCard(basic);
                EyePanel panel(fonts);
                panel.setTab(PanelTab::Gaze);
                for (const GazePage page : {GazePage::Motion, GazePage::Reach, GazePage::Glitch}) {
                    panel.setGazePage(page);
                    panel.render(m);
                    std::vector<int> others;
                    for (int i = 0; i < 3; ++i) {
                        if (i != static_cast<int>(page)) others.push_back(i);
                    }
                    CHECK(gazeTabs(panel) == others);
                    const bool motion = page == GazePage::Motion;
                    const bool reach = page == GazePage::Reach;
                    const bool glitch = page == GazePage::Glitch;
                    // Motion: smoothing on / off, the presets, the three values, the deadzone
                    CHECK(hits(panel, PanelAction::SetBool, key::kRaw).size() == (motion && !locked ? 2u : 0u));
                    CHECK(hits(panel, PanelAction::Preset).size() == (motion ? 3u : 0u));
                    for (const char* name : {key::kGazeMinCutoff, key::kGazeBeta, key::kGazeDCutoff, key::kGazeDeadzone}) {
                        CHECK(hits(panel, PanelAction::Step, name).empty() == !motion);
                    }
                    // Reach: the four scales at 100% (both ways), the two locks, moving the eyes separately
                    for (const char* name : scaleKeys) {
                        const bool pinned = locked && std::string(name) == key::kGazeScaleRight;
                        CHECK(hits(panel, PanelAction::Step, name).size() == (reach && !pinned ? 2u : 0u));
                    }
                    const std::vector<EyePanel::HitArea> locks = hits(panel, PanelAction::GazeScaleLink);
                    CHECK(locks.size() == (reach ? (locked ? 1u : 2u) : 0u));
                    for (const EyePanel::HitArea& area : locks) CHECK(area.w >= 44 && area.h >= 44);
                    if (reach && locked && locks.size() == 1) CHECK(locks[0].hit.arg == 1);
                    CHECK(hits(panel, PanelAction::SetBool, key::kIndependentEyes).size() == (reach && !locked ? 2u : 0u));
                    // ...and under it each eye's own gaze, nothing to press while they don't (testOwnEyeGazeRow)
                    CHECK(hits(panel, PanelAction::Step, key::kOwnEyeGaze).empty());
                    // Glitch guard: the hold, skipping unreliable gaze, the despike
                    CHECK(hits(panel, PanelAction::NumberOff, key::kGazeHoldBelow).size() ==
                          (glitch && !locked ? 1u : 0u));
                    CHECK(hits(panel, PanelAction::NumberOn, key::kGazeQualityLimit).size() == (glitch ? 1u : 0u));
                    CHECK(hits(panel, PanelAction::SetBool, key::kDespike).size() == (glitch ? 2u : 0u));
                    // Every button besides the tab row: inside the card, from the sub-tabs down; nothing below it
                    for (const EyePanel::HitArea& area : panel.hitAreas()) {
                        if (area.hit.action == PanelAction::Tab && area.y < 100) continue;
                        if (area.hit.action == PanelAction::PreviewOpen) continue;  // (the left column's)
                        CHECK(area.x >= 400 && area.y >= 110 && area.y + area.h <= 676 && area.x + area.w <= 1176);
                        if (area.hit.action == PanelAction::Step) CHECK(area.w >= 44 && area.h >= 44);
                    }
                    CHECK(belowCard(panel) == plain);
                }
            }
        }
    }
}

/** own_eye_gaze (Gaze > Reach, "Own gaze per eye", under "Move eyes separately"): − / ＋ in steps of 5 % only while
 *  the eyes move separately in v2 and it isn't locked; faded otherwise, in v1 with the button to that switch. Inside
 *  the card in both languages. */
void testOwnEyeGazeRow(const FontSet& fonts) {
    for (const Language language : {Language::Ja, Language::En}) {
        for (const bool independent : {false, true}) {
            for (const bool v1 : {false, true}) {
                for (const bool locked : {false, true}) {
                    for (const double value : {0.0, 0.5, 1.0}) {
                        PanelModel m = modelWith(Lids::Valve, false);
                        m.language = language;
                        m.config.root.set(key::kIndependentEyes, JsonValue::makeBool(independent));
                        m.config.root.set(key::kOwnEyeGaze, JsonValue::makeNumber(value));
                        if (v1) m.config.root.set(key::kEyeBehavior, JsonValue::makeNumber(1, true));
                        if (locked) m.status.locked.push_back(key::kOwnEyeGaze);
                        EyePanel panel(fonts);
                        panel.setTab(PanelTab::Gaze);
                        panel.setGazePage(GazePage::Reach);
                        panel.render(m);
                        const std::vector<EyePanel::HitArea> steps = hits(panel, PanelAction::Step, key::kOwnEyeGaze);
                        const bool usable = independent && !v1 && !locked;
                        // At 0 % only ＋, at 100 % only −
                        CHECK(steps.size() == (!usable ? 0u : (value > 0 && value < 1 ? 2u : 1u)));
                        const std::vector<EyePanel::HitArea> separate =
                            hits(panel, PanelAction::SetBool, key::kIndependentEyes);
                        CHECK(separate.size() == 2);
                        for (const EyePanel::HitArea& area : steps) {
                            CHECK(area.w >= 44 && area.h >= 44 && area.x >= 400 && area.x + area.w <= 1176);
                            CHECK(area.y > separate[0].y + separate[0].h && area.y + area.h <= 664);
                        }
                        int toVersion = 0;
                        for (const EyePanel::HitArea& area : hits(panel, PanelAction::AdvancedPage)) {
                            if (area.x >= 400 && area.hit.arg == static_cast<int>(AdvPage::Version)) ++toVersion;
                        }
                        CHECK(toVersion == (v1 ? 1 : 0));
                    }
                }
            }
        }
    }
}

void testGazeScaleLocks(const FontSet& fonts) {
    PanelModel m = modelWith(Lids::Valve, false);
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Gaze);
    panel.render(m);
    // The first time: "Motion"; the locks closed
    CHECK(panel.gazePage() == GazePage::Motion && panel.gazeScaleLinked(0) && panel.gazeScaleLinked(1));
    CHECK((gazeTabs(panel) == std::vector<int> {1, 2}));
    CHECK(pressArg(panel, PanelAction::GazePage, static_cast<int>(GazePage::Reach)).action == PanelAction::None);
    CHECK(panel.gazePage() == GazePage::Reach && panel.tab() == PanelTab::Gaze);
    panel.render(m);
    // A lock opens and closes in the panel; the press goes on to the caller (which writes the second value)
    PanelHit hit = pressArg(panel, PanelAction::GazeScaleLink, 0);
    CHECK(hit.action == PanelAction::GazeScaleLink && hit.arg == 0);
    CHECK(!panel.gazeScaleLinked(0) && panel.gazeScaleLinked(1));
    panel.render(m);
    hit = pressArg(panel, PanelAction::GazeScaleLink, 1);
    CHECK(hit.action == PanelAction::GazeScaleLink && !panel.gazeScaleLinked(1));
    panel.render(m);
    CHECK(pressArg(panel, PanelAction::GazeScaleLink, 0).action == PanelAction::GazeScaleLink);
    CHECK(panel.gazeScaleLinked(0) && !panel.gazeScaleLinked(1));
    // Open and closed draw differently (the padlock's shackle)
    panel.render(m);
    const std::vector<uint8_t> closed = panel.toRgba();
    panel.setGazeScaleLinked(0, false);
    panel.render(m);
    CHECK(panel.toRgba() != closed);
    // A value of a pair locked by the command line: that pair's lock draws open, linked or not, and can't be pressed
    PanelModel rightLocked = m;
    rightLocked.status.locked = {key::kGazeScaleRight};
    panel.render(rightLocked);
    const std::vector<uint8_t> lockedOpen = panel.toRgba();
    panel.setGazeScaleLinked(0, true);
    panel.render(rightLocked);
    CHECK(panel.toRgba() == lockedOpen);
    const std::vector<EyePanel::HitArea> links = hits(panel, PanelAction::GazeScaleLink);
    CHECK(links.size() == 1 && links[0].hit.arg == 1);
    panel.setGazeScaleLinked(0, false);
    // − / ＋ go to the caller as Step on their own key
    const std::vector<EyePanel::HitArea> plus = hits(panel, PanelAction::Step, key::kGazeScaleUp);
    CHECK(plus.size() == 2 && plus[0].hit.arg == -1 && plus[1].hit.arg == 1);
    // 100% shows as "100%"; at 5% only ＋ can be pressed, at 200% only −, at 25% both
    m.config.root.set(key::kGazeScaleDown, JsonValue::makeNumber(0.05));
    m.config.root.set(key::kGazeScaleLeft, JsonValue::makeNumber(2.0));
    m.config.root.set(key::kGazeScaleRight, JsonValue::makeNumber(0.25));
    panel.render(m);
    const std::vector<EyePanel::HitArea> down = hits(panel, PanelAction::Step, key::kGazeScaleDown);
    CHECK(down.size() == 1 && down[0].hit.arg == 1);
    const std::vector<EyePanel::HitArea> left = hits(panel, PanelAction::Step, key::kGazeScaleLeft);
    CHECK(left.size() == 1 && left[0].hit.arg == -1);
    CHECK(hits(panel, PanelAction::Step, key::kGazeScaleRight).size() == 2);
    // Kept while another tab shows, shown again when the tab is chosen again; the Eyelids tab's sub-tab is its own
    pressTab(panel, PanelTab::Basic);
    panel.render(m);
    CHECK(gazeTabs(panel).empty() && panel.gazePage() == GazePage::Reach && !panel.gazeScaleLinked(0));
    pressTab(panel, PanelTab::Lids);
    panel.render(m);
    pressArg(panel, PanelAction::LidsPage, static_cast<int>(LidsPage::Fine));
    CHECK(panel.lidsPage() == LidsPage::Fine && panel.gazePage() == GazePage::Reach);
    pressTab(panel, PanelTab::Gaze);
    CHECK(panel.tab() == PanelTab::Gaze && panel.gazePage() == GazePage::Reach);
    // As remembered from before (the caller sets it at start)
    EyePanel next(fonts);
    next.setGazePage(GazePage::Glitch);
    next.setTab(PanelTab::Gaze);
    next.render(m);
    CHECK((gazeTabs(next) == std::vector<int> {0, 1}));
    CHECK(hits(next, PanelAction::SetBool, key::kDespike).size() == 2);
}

void testLidsRemembered(const FontSet& fonts) {
    const PanelModel m = modelWith(Lids::Valve, false);
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Lids);
    panel.render(m);
    // The first time: "Look"
    CHECK(panel.lidsPage() == LidsPage::Look);
    CHECK((lidsTabs(panel) == std::vector<int> {1, 2, 3}));
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
    CHECK((lidsTabs(next) == std::vector<int> {0, 2, 3}));
    CHECK(!hits(next, PanelAction::Step, key::kBlinkHoldMs).empty());
}

/**
 * Advanced > Debug tools: the rings switch (gaze_debug_rings) on its own row under "Show gaze dots", On / Off lined up
 * with the dots' switch; only pressable while the dots are on, like the dots' distance.
 */
void testGazeRingsRow(const FontSet& fonts) {
    for (const Language language : {Language::Ja, Language::En}) {
        for (const bool dots : {false, true}) {
            for (const bool rings : {true, false}) {
                EyePanel panel(fonts);
                PanelModel m = modelWith(Lids::Both, true);
                m.language = language;
                m.config.root.set(key::kGazeDebugDots, JsonValue::makeBool(dots));
                if (!rings) m.config.root.set(key::kGazeDebugRings, JsonValue::makeBool(false));
                panel.setTab(PanelTab::Advanced);
                panel.setAdvPage(AdvPage::Tools);
                panel.render(m);
                const std::vector<EyePanel::HitArea> dotHits = hits(panel, PanelAction::SetBool, key::kGazeDebugDots);
                const std::vector<EyePanel::HitArea> ringHits = hits(panel, PanelAction::SetBool, key::kGazeDebugRings);
                CHECK(dotHits.size() == 2);
                CHECK(hits(panel, PanelAction::Step, key::kGazeDebugDotsDistanceM).empty() == !dots);
                if (!dots) {
                    // Greyed out: nothing to press
                    CHECK(ringHits.empty());
                    continue;
                }
                CHECK(ringHits.size() == 2);
                if (ringHits.size() != 2 || dotHits.size() != 2) continue;
                // On then Off, right under the dots' On / Off, the same size
                CHECK(ringHits[0].hit.arg == 1 && ringHits[1].hit.arg == 0);
                for (size_t i = 0; i < 2; ++i) {
                    CHECK(std::fabs(ringHits[i].x - dotHits[i].x) < 0.5 && std::fabs(ringHits[i].w - dotHits[i].w) < 0.5);
                    CHECK(ringHits[i].y >= dotHits[i].y + dotHits[i].h);
                }
                // Pressing the other choice gives the key to set
                bool found = false;
                for (const EyePanel::HitArea& area : hits(panel, PanelAction::SetBool, key::kGazeDebugRings)) {
                    if (area.hit.arg != (rings ? 0 : 1)) continue;
                    const PanelHit pressed = panel.pointerDown(area.x + area.w / 2, area.y + area.h / 2, 0.0);
                    panel.pointerUp();
                    CHECK(pressed.action == PanelAction::SetBool && std::string(pressed.key) == key::kGazeDebugRings);
                    found = true;
                }
                CHECK(found);
            }
        }
    }
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
        CHECK(hits(panel, PanelAction::SetBool, key::kGazeDebugRings).empty());  // the dots off: greyed out
        CHECK(hits(panel, PanelAction::RecordToggle).size() == 1 && hits(panel, PanelAction::EyecamStart).size() == 1);
        CHECK(hits(panel, PanelAction::DiagOpen).empty());
        pressArg(panel, PanelAction::AdvancedPage, static_cast<int>(AdvPage::Files));
        CHECK(panel.advPage() == AdvPage::Files);
        panel.render(m);
        for (const EyePanel::HitArea& area : panel.hitAreas()) {
            CHECK(area.hit.action == PanelAction::Tab || area.hit.action == PanelAction::AdvancedPage ||
                  area.hit.action == PanelAction::PreviewOpen);
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

void testUpdateBeta(const FontSet& fonts) {
    // "Get test versions" on the Version page: On / Off, kept by the panel (the caller saves it); the page fits with the
    // badge, the keep-hint and a test version on offer, in both languages and every update state
    EyePanel panel(fonts);
    PanelModel m = modelWith(Lids::Both, false);
    m.update.state = frame_updater::UpdateState::UpToDate;
    m.update.current = "0.7.10";
    m.update.latest = "0.7.10";
    panel.setTab(PanelTab::Advanced);
    panel.setAdvPage(AdvPage::Version);
    panel.render(m);
    CHECK(!panel.updateBeta());
    CHECK(hits(panel, PanelAction::UpdateBeta).size() == 2);
    const PanelHit on = pressArg(panel, PanelAction::UpdateBeta, 1);
    CHECK(on.action == PanelAction::UpdateBeta && on.arg == 1 && panel.updateBeta());
    panel.render(m);
    // Pressing the chosen one again changes nothing (nothing to save)
    CHECK(pressArg(panel, PanelAction::UpdateBeta, 1).action == PanelAction::None && panel.updateBeta());
    panel.render(m);
    CHECK(pressArg(panel, PanelAction::UpdateBeta, 0).action == PanelAction::UpdateBeta && !panel.updateBeta());

    using frame_updater::UpdateState;
    for (const bool beta : {false, true}) {
        for (const Language language : {Language::Ja, Language::En}) {
            for (const UpdateState state : {UpdateState::Unknown, UpdateState::UpToDate, UpdateState::Available,
                                            UpdateState::Installing, UpdateState::Installed, UpdateState::CheckFailed,
                                            UpdateState::InstallFailed}) {
                for (const bool onTest : {false, true}) {
                    PanelModel u = m;
                    u.language = language;
                    u.update.state = state;
                    u.update.checkedAt = 1791287109;
                    u.update.error = "network";
                    u.update.version = "0.7.11-beta.1";
                    u.update.step = "download";
                    u.update.installable = true;
                    u.update.notes = std::string(400, 'x');
                    u.update.current = onTest ? "0.7.11-beta.2" : "0.7.10";
                    u.update.latest = beta ? "0.7.11-beta.12" : "0.7.11";
                    u.update.prerelease = beta;
                    u.update.testVersion = beta ? "" : "0.7.11-beta.12";
                    EyePanel p(fonts);
                    p.setUpdateBeta(beta);
                    p.setTab(PanelTab::Advanced);
                    p.setAdvPage(AdvPage::Version);
                    p.render(u);
                    CHECK(p.advancedMaxScroll() == 0.0 && !p.wantsScroll());
                    CHECK(hits(p, PanelAction::UpdateBeta).size() == 2);
                    for (const EyePanel::HitArea& area : p.hitAreas()) {
                        if (area.hit.action == PanelAction::Tab || area.x < 400) continue;
                        CHECK(area.y >= 110 && area.y + area.h <= 676);
                    }
                }
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
            CHECK(area.hit.action == PanelAction::Tab || area.hit.action == PanelAction::RecordBack ||
                  area.hit.action == PanelAction::PreviewOpen);
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
    // The switch to turn it on again: on the Basic tab
    panel.setTab(PanelTab::Basic);
    panel.render(m);
    CHECK(hits(panel, PanelAction::SetBool, key::kCameraLids).size() == 2);
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
        CHECK(hits(panel, PanelAction::LidsPage).size() == 1);
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
        for (const gaze_fit::Mode mode : {gaze_fit::Mode::Full, gaze_fit::Mode::Center}) {
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
    // The eye preview open over the fit's tab as a fit starts (an automatic re-center): the left column (its button
    // too) can't be pressed either; the tabs and the sheet's own buttons can
    {
        EyePanel over(fonts);
        over.setTab(PanelTab::EyeFit);
        PanelModel idle = modelWith(Lids::Valve, false);
        over.render(idle);
        press(over, PanelAction::PreviewOpen);
        over.render(m);
        CHECK(over.previewOpen() && hits(over, PanelAction::PreviewClose).size() == 1);
        CHECK(hits(over, PanelAction::PreviewOpen).empty() && hits(over, PanelAction::Tab).size() >= 5);
        for (const EyePanel::HitArea& area : over.hitAreas()) {
            const PanelAction a = area.hit.action;
            CHECK(a == PanelAction::Tab || a == PanelAction::PreviewClose || a == PanelAction::PreviewSource ||
                  a == PanelAction::PreviewMirror || a == PanelAction::PreviewFreeze ||
                  a == PanelAction::PreviewCamera || a == PanelAction::PreviewMark);
            CHECK(a == PanelAction::Tab || area.x >= 416);
        }
        // Its own buttons still work (the fit goes on behind it)
        press(over, PanelAction::PreviewMirror);
        CHECK(over.previewOpen());
        // The fit over: the left column again
        over.render(idle);
        CHECK(hits(over, PanelAction::PreviewOpen).size() == 1);
    }
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

void testFitTabPolicy(const FontSet& fonts) {
    EyePanel panel(fonts);
    panel.setTab(PanelTab::EyeFit);
    PanelModel m = modelWith(Lids::Valve, false);
    m.config.root.set(key::kGazeOffsetX, JsonValue::makeNumber(0.012));
    m.config.root.set(key::kGazeGainUp, JsonValue::makeNumber(0.88));
    panel.render(m);
    // The whole fit and re-centering (the middle dot)
    CHECK(hits(panel, PanelAction::FitStart).size() == 1 && hits(panel, PanelAction::FitCenter).size() == 1);
    // "When put on": nothing, or the middle dot; no tilt any more
    const std::vector<EyePanel::HitArea> recenter = hits(panel, PanelAction::SetAutoRecenter);
    CHECK(recenter.size() == 2);
    for (const EyePanel::HitArea& area : recenter) CHECK(area.hit.arg == 0 || area.hit.arg == 1);
    // Fine-tune: the zero point and all three gains by hand (the fit only writes the up gain now), the far-down hold;
    // no tilt and no per-eye values
    panel.setFitDetails(true);
    panel.render(m);
    for (const char* name : {key::kGazeOffsetX, key::kGazeOffsetY, key::kGazeGainX, key::kGazeGainUp,
                             key::kGazeGainDown, key::kGazeDownHoldXDeg}) {
        CHECK(hits(panel, PanelAction::Step, name).size() == 2);
    }
    for (const char* name : {key::kGazeRollDeg, key::kGazeOffsetXLeft, key::kGazeOffsetXRight, key::kGazeGainXLeft,
                             key::kGazeGainXRight}) {
        CHECK(hits(panel, PanelAction::Step, name).empty());
    }
    // Everything on the page stays inside the card
    for (const EyePanel::HitArea& area : panel.hitAreas()) CHECK(area.y + area.h <= 680);
    // While the eye cameras calibrate (the user looks up and down for it), neither fit can be pressed; the rest of the
    // page can. Also from the moment the panel sent the calibration, before eyecam-rec says so
    panel.setFitDetails(false);
    m.eyecam.visible = true;
    m.eyecam.status.state = eyecam::State::Calibrating;
    panel.render(m);
    CHECK(hits(panel, PanelAction::FitStart).empty() && hits(panel, PanelAction::FitCenter).empty());
    CHECK(hits(panel, PanelAction::SetAutoRecenter).size() == 2 && hits(panel, PanelAction::FitDetails).size() == 1);
    m.eyecam.status.state = eyecam::State::Idle;
    m.eyecam.busy = true;
    m.eyecam.busyCommand = eyecam::kCalibUserCommand;
    panel.render(m);
    CHECK(hits(panel, PanelAction::FitStart).empty() && hits(panel, PanelAction::FitCenter).empty());
    // Over: back
    m.eyecam.busy = false;
    m.eyecam.busyCommand.clear();
    panel.render(m);
    CHECK(hits(panel, PanelAction::FitStart).size() == 1 && hits(panel, PanelAction::FitCenter).size() == 1);
}

/**
 * A model whose eye cameras aren't set up: eyecam-rec runs, nothing of its setup done (no camera tool yet).
 * @return the model
 */
PanelModel notSetUp() {
    PanelModel m = modelWith(Lids::Valve);
    eyecam::Status& e = m.eyecam.status;
    e.setupDone = false;
    e.autoGrab = "missing";
    e.hasBuffers = false;
    e.calibState = 0;
    e.state = eyecam::State::WaitingFds;  // (no tool: waiting for the camera buffers)
    e.stateText = "waiting_fds";
    return m;
}

/**
 * Whether every usable button besides the tab row is inside the card and at least 44 px high.
 * @param panel the panel
 * @return true if they all are
 */
bool allInsideAndTall(const EyePanel& panel) {
    for (const EyePanel::HitArea& area : panel.hitAreas()) {
        if (area.hit.action == PanelAction::Tab && area.y < 100) continue;
        // (the left column's card and the sub-tabs are not this change's; the content's own buttons are)
        if (area.x < 400) continue;
        if (area.y < 110 || area.y + area.h > 676 || area.h < 40) return false;
    }
    return true;
}

/**
 * Whether the card itself (not the left column) has a way to the eye cameras tab: the Basic tab's button, or a dashed
 * line in place of the camera rows.
 * @param panel the panel
 * @return true if there is one
 */
bool inCard(const EyePanel& panel) {
    for (const EyePanel::HitArea& area : hits(panel, PanelAction::Tab)) {
        if (area.hit.arg == static_cast<int>(PanelTab::Eyecam) && area.y > 100 && area.x > 400) return true;
    }
    return false;
}

void testOutputCards(const FontSet& fonts) {
    // The Basic tab: three cards (VRChat, LiveLink, FrameEyeOSC), and ETVR as a small button under them with its notice
    // between the two only while it is the output
    for (Language language : {Language::Ja, Language::En}) {
        EyePanel panel(fonts);
        PanelModel m = modelWith(Lids::Both);
        m.language = language;
        const auto with = [&](const char* output) {
            PanelModel copy = m;
            copy.config.root.set(key::kOutput, JsonValue::makeString(output));
            return copy;
        };
        for (const char* output : {kOutputVrchat, kOutputLivelink, kOutputModule, kOutputEtvr}) {
            panel.render(with(output));
            const std::vector<EyePanel::HitArea> found = hits(panel, PanelAction::SetOutput);
            CHECK(found.size() == 4);
            if (found.size() != 4) continue;
            // The cards in a row, as wide as each other, in the order VRChat, LiveLink, FrameEyeOSC
            const int args[3] = {0, 2, 3};
            for (int c = 0; c < 3; ++c) {
                CHECK(found[c].hit.arg == args[c]);
                CHECK(found[c].y == found[0].y && std::fabs(found[c].w - found[0].w) < 0.5 && found[c].h > 80);
                if (c > 0) CHECK(found[c].x > found[c - 1].x + found[c - 1].w);
            }
            // ETVR: a small button under them
            const EyePanel::HitArea& etvr = found[3];
            CHECK(etvr.hit.arg == 1 && etvr.w < found[0].w && etvr.h >= 40 && etvr.h < 50);
            CHECK(etvr.x == found[0].x);
            // ...right under the cards, or under the notice (between the two) while ETVR is the output, or under the
            // one-line hint (where to go if the eyes don't move) while VRChat directly is
            const double gap = etvr.y - (found[0].y + found[0].h);
            if (std::string(output) == kOutputEtvr) {
                CHECK(gap > 60 && gap < 90);
            } else if (std::string(output) == kOutputVrchat) {
                CHECK(gap > 30 && gap < 50);
            } else {
                CHECK(gap > 0 && gap < 20);
            }
            CHECK(outputDeprecated(output) == (std::string(output) == kOutputEtvr));
            // Everything still fits, the rows below too
            CHECK(allInsideAndTall(panel));
            CHECK(hits(panel, PanelAction::ResetAll).size() == 1 && hits(panel, PanelAction::Quit).size() == 1);
            CHECK(hits(panel, PanelAction::Language).size() == 2 && hits(panel, PanelAction::AutostartOn).size() <= 1);
            const std::vector<EyePanel::HitArea> quit = hits(panel, PanelAction::Quit);
            if (!quit.empty()) CHECK(quit[0].y + quit[0].h <= 664);
        }
        // Pressing the small button switches to ETVR; a card to its output
        panel.render(with(kOutputLivelink));
        std::vector<EyePanel::HitArea> found = hits(panel, PanelAction::SetOutput);
        if (found.size() == 4) {
            const PanelHit pressed = panel.pointerDown(found[3].x + found[3].w / 2, found[3].y + found[3].h / 2, 0.0);
            panel.pointerUp();
            CHECK(pressed.action == PanelAction::SetOutput && pressed.arg == 1);
            const PanelHit card = panel.pointerDown(found[2].x + found[2].w / 2, found[2].y + found[2].h / 2, 0.0);
            panel.pointerUp();
            CHECK(card.action == PanelAction::SetOutput && card.arg == 3);
        }
        // Locked: drawn, nothing to press
        PanelModel locked = with(kOutputEtvr);
        locked.status.locked.push_back(key::kOutput);
        panel.render(locked);
        CHECK(hits(panel, PanelAction::SetOutput).empty());
        // The Output tab has no module row any more: the destination is chosen on the cards alone
        panel.setTab(PanelTab::Output);
        for (const char* output : {kOutputLivelink, kOutputModule, kOutputEtvr}) {
            panel.render(with(output));
            CHECK(hits(panel, PanelAction::SetOutput).empty() && allInsideAndTall(panel));
        }
        // (FrameEyeOSC: no pupil rows, VRCFT sends them as the avatar takes them; LiveLink keeps its)
        panel.render(with(kOutputModule));
        CHECK(hits(panel, PanelAction::SetBool, key::kPupilsToVrchat).empty());
        CHECK(hits(panel, PanelAction::SetInteger, key::kPupilBits).empty());
        panel.render(with(kOutputLivelink));
        CHECK(hits(panel, PanelAction::SetBool, key::kPupilsToVrchat).size() == 2);
    }
}

/**
 * The red pixels (kDanger's, anti-aliased edges left out) right of the left column, as last drawn.
 * @param panel the panel (rendered)
 * @return how many there are
 */
int redRight(EyePanel& panel) {
    const std::vector<uint8_t>& rgba = panel.toRgba();
    const size_t w = static_cast<size_t>(panel.width());
    int count = 0;
    for (size_t y = 0; y < static_cast<size_t>(panel.height()); ++y) {
        for (size_t x = 410; x < w; ++x) {
            const uint8_t* p = &rgba[(y * w + x) * 4];
            if (p[0] > 200 && p[1] < 120 && p[2] < 110) ++count;
        }
    }
    return count;
}

void testModuleLinkError(const FontSet& fonts) {
    // A failed module link writes its own line and leaves the eye cameras' setup's alone (and the other way round)
    {
        PanelModel m = modelWith(Lids::Both);
        noteSpawnResult(m, true, "flatpak not found");
        CHECK(m.moduleLinkError == "flatpak not found" && m.eyecam.spawnError.empty());
        noteSpawnResult(m, false, "konsole: konsole not found");
        CHECK(m.eyecam.spawnError == "konsole: konsole not found" && m.moduleLinkError == "flatpak not found");
        // Opened the next time: only its own line goes
        noteSpawnResult(m, true, "");
        CHECK(m.moduleLinkError.empty() && m.eyecam.spawnError == "konsole: konsole not found");
        noteSpawnResult(m, false, "");
        CHECK(m.eyecam.spawnError.empty());
    }
    // Drawn only for FrameEyeOSC (where the link is), and only with a failure
    CHECK(moduleLinkErrorShown(kOutputModule, "flatpak not found"));
    CHECK(!moduleLinkErrorShown(kOutputModule, ""));
    for (const char* output : {kOutputVrchat, kOutputLivelink, kOutputEtvr}) {
        CHECK(!moduleLinkErrorShown(output, "flatpak not found"));
    }
    for (Language language : {Language::Ja, Language::En}) {
        for (const bool camera : {true, false}) {
            EyePanel panel(fonts);
            PanelModel m = modelWith(camera ? Lids::Both : Lids::Valve);
            m.language = language;
            m.config.root.set(key::kOutput, JsonValue::makeString(kOutputModule));
            panel.setTab(PanelTab::Output);
            panel.render(m);
            const int before = redRight(panel);
            const std::vector<EyePanel::HitArea> link = hits(panel, PanelAction::OpenModulePage);
            // The red line under the link on the Output tab, the link where it was, everything still inside
            PanelModel failed = m;
            noteSpawnResult(failed, true, "flatpak not found");
            panel.render(failed);
            CHECK(redRight(panel) > before + 100);
            const std::vector<EyePanel::HitArea> still = hits(panel, PanelAction::OpenModulePage);
            CHECK(link.size() == 1 && still.size() == 1);
            if (link.size() == 1 && still.size() == 1) CHECK(link[0].y == still[0].y && link[0].x == still[0].x);
            CHECK(allInsideAndTall(panel));
            // ...and nowhere else: the Basic tab, and the eye cameras' setup at (2) (where the setup's own failure shows)
            // look as they did
            for (PanelModel* model : {&m, &failed}) model->eyecam.status = notSetUp().eyecam.status;
            panel.setSetupBegun(true);
            for (const PanelTab tab : {PanelTab::Basic, PanelTab::Eyecam}) {
                panel.setTab(tab);
                panel.render(m);
                const std::vector<uint8_t> clean = panel.toRgba();
                panel.render(failed);
                CHECK(panel.toRgba() == clean);
                if (tab != PanelTab::Eyecam) continue;
                // (the setup's own failure does show there)
                CHECK(hits(panel, PanelAction::SetupKonsole).size() == 1);
                PanelModel setupFailed = m;
                noteSpawnResult(setupFailed, false, "konsole: konsole not found");
                panel.render(setupFailed);
                CHECK(panel.toRgba() != clean);
            }
            // Another output: no line (the link isn't there either)
            PanelModel other = m;
            other.config.root.set(key::kOutput, JsonValue::makeString(kOutputLivelink));
            failed.config.root.set(key::kOutput, JsonValue::makeString(kOutputLivelink));
            panel.setTab(PanelTab::Output);
            panel.render(other);
            const std::vector<uint8_t> clean = panel.toRgba();
            panel.render(failed);
            CHECK(hits(panel, PanelAction::OpenModulePage).empty() && panel.toRgba() == clean);
        }
    }
}

void testCameraRows(const FontSet& fonts) {
    for (const Language language : {Language::Ja, Language::En}) {
        // Not set up: the Basic tab's row leads to the eye cameras tab (no switch yet)
        PanelModel none = notSetUp();
        none.language = language;
        EyePanel panel(fonts);
        panel.render(none);
        CHECK(hits(panel, PanelAction::SetBool, key::kCameraLids).empty());
        CHECK(inCard(panel));
        CHECK(allInsideAndTall(panel));
        for (const EyePanel::HitArea& area : hits(panel, PanelAction::Tab)) {
            if (area.y > 100 && area.x > 400) CHECK(area.h >= 44);
        }
        // ...the camera-only rows aren't there: one dashed line each place instead, which leads to their tab
        panel.setTab(PanelTab::Lids);
        panel.setLidsPage(LidsPage::Look);
        panel.render(none);
        CHECK(hits(panel, PanelAction::SetLidWiden).empty() && hits(panel, PanelAction::EyecamSensitivity).empty());
        CHECK(inCard(panel) && allInsideAndTall(panel));
        panel.setLidsPage(LidsPage::Blinks);
        panel.render(none);
        CHECK(hits(panel, PanelAction::Step, key::kCameraLidFloor).empty() && inCard(panel));
        panel.setLidsPage(LidsPage::Fine);
        panel.render(none);
        CHECK(!inCard(panel));  // nothing there only for the cameras
        panel.setTab(PanelTab::Output);
        for (const char* output : {kOutputVrchat, kOutputLivelink, kOutputEtvr, kOutputModule}) {
            PanelModel m = none;
            m.config.root.set(key::kOutput, JsonValue::makeString(output));
            panel.render(m);
            CHECK(hits(panel, PanelAction::SetInteger, key::kPupilBits).empty());
            CHECK(hits(panel, PanelAction::SetBool, key::kPupilsToVrchat).empty());
            // (ETVR sends no pupils at all: no line there)
            CHECK(inCard(panel) == (std::string(output) != kOutputEtvr));
            CHECK(allInsideAndTall(panel));
        }
        // ...and their tab says what they add, with "Get started" into the setup checklist
        panel.setTab(PanelTab::Eyecam);
        panel.render(none);
        const std::vector<EyePanel::HitArea> begin = hits(panel, PanelAction::SetupBegin);
        CHECK(begin.size() == 1 && hits(panel, PanelAction::SetupKonsole).empty() && allInsideAndTall(panel));
        if (begin.size() == 1) {
            CHECK(begin[0].h >= 44);
            panel.pointerDown(begin[0].x + begin[0].w / 2, begin[0].y + begin[0].h / 2, 0.0);
            panel.pointerUp();
        }
        CHECK(panel.setupBegun());
        panel.render(none);
        // (the checklist's (2): the tool's Konsole with the sudo line typed in)
        const std::vector<EyePanel::HitArea> konsole = hits(panel, PanelAction::SetupKonsole);
        CHECK(konsole.size() == 1 && hits(panel, PanelAction::SetupBegin).empty());
        if (konsole.size() == 1) CHECK(konsole[0].hit.arg == 0);
        // ...the password missing: (1) first, its Konsole for passwd
        PanelModel noPassword = none;
        noPassword.eyecam.password = eyecam::PasswordState::NotSet;
        EyePanel fresh(fonts);
        fresh.setTab(PanelTab::Eyecam);
        fresh.render(noPassword);
        CHECK(hits(fresh, PanelAction::SetupBegin).size() == 1);
        fresh.setSetupBegun(true);
        fresh.render(noPassword);
        CHECK(hits(fresh, PanelAction::SetupBegin).empty() && hits(fresh, PanelAction::SetupKonsole).size() == 1);
        // ...the tool already in (the setup halfway): the checklist straight away
        PanelModel halfway = none;
        halfway.eyecam.status.autoGrab = "ok";
        halfway.eyecam.status.state = eyecam::State::Idle;
        halfway.eyecam.status.stateText = "idle";
        fresh.setSetupBegun(false);
        fresh.render(halfway);
        CHECK(hits(fresh, PanelAction::SetupBegin).empty());
        // ...without eyecam-rec (no tab): no way there from the Basic tab or the lines
        PanelModel gone = modelWith(Lids::Valve, false);
        gone.language = language;
        EyePanel plain(fonts);
        plain.render(gone);
        CHECK(!inCard(plain) && hits(plain, PanelAction::SetBool, key::kCameraLids).empty());

        // In use (set up, switched on): the switch on the Basic tab, the camera rows back, no lines
        PanelModel on = modelWith(Lids::Both);
        on.language = language;
        on.config.root.set(key::kOutput, JsonValue::makeString(kOutputLivelink));
        EyePanel used(fonts);
        used.render(on);
        const std::vector<EyePanel::HitArea> sw = hits(used, PanelAction::SetBool, key::kCameraLids);
        CHECK(sw.size() == 2 && allInsideAndTall(used));
        if (sw.size() == 2) CHECK(sw[0].hit.arg == 1 && sw[1].hit.arg == 0);  // "Use", "Don't use"
        CHECK(!inCard(used));
        used.setTab(PanelTab::Lids);
        used.setLidsPage(LidsPage::Look);
        used.render(on);
        CHECK(hits(used, PanelAction::EyecamSensitivity).size() == 1 && !inCard(used));
        used.setLidsPage(LidsPage::Blinks);
        used.render(on);
        CHECK(hits(used, PanelAction::Step, key::kCameraLidFloor).size() == 1 && !inCard(used));
        used.setTab(PanelTab::Output);
        used.render(on);
        CHECK(hits(used, PanelAction::SetBool, key::kPupilsToVrchat).size() == 2);
        CHECK(hits(used, PanelAction::SetInteger, key::kPupilBits).size() == 5 && !inCard(used));
        CHECK(allInsideAndTall(used));
        used.setTab(PanelTab::Eyecam);
        used.render(on);
        CHECK(hits(used, PanelAction::SetBool, key::kCameraLids).empty() && hits(used, PanelAction::SetupBegin).empty());
        CHECK(hits(used, PanelAction::EyecamCalib).size() == 2 && allInsideAndTall(used));

        // Set up but switched off: the switch on the Basic tab; their page as usual (the calibrations wait); the
        // camera rows go as without them
        PanelModel off = on;
        off.config.root.set(key::kCameraLids, JsonValue::makeBool(false));
        off.status.camera.used[0] = off.status.camera.used[1] = false;
        EyePanel offPanel(fonts);
        offPanel.render(off);
        CHECK(hits(offPanel, PanelAction::SetBool, key::kCameraLids).size() == 2 && allInsideAndTall(offPanel));
        offPanel.setTab(PanelTab::Eyecam);
        offPanel.render(off);
        CHECK(hits(offPanel, PanelAction::SetupBegin).empty() && hits(offPanel, PanelAction::EyecamCalib).empty());
        CHECK(hits(offPanel, PanelAction::LidsPage).size() == 1);
        offPanel.setTab(PanelTab::Lids);
        offPanel.setLidsPage(LidsPage::Look);
        offPanel.render(off);
        CHECK(hits(offPanel, PanelAction::EyecamSensitivity).empty() && hits(offPanel, PanelAction::SetLidWiden).empty());
        CHECK(inCard(offPanel));
        offPanel.setTab(PanelTab::Output);
        offPanel.render(off);
        CHECK(hits(offPanel, PanelAction::SetInteger, key::kPupilBits).empty() && inCard(offPanel));
        // ...pressing a dashed line opens their tab
        for (const EyePanel::HitArea& area : hits(offPanel, PanelAction::Tab)) {
            if (area.y < 100 || area.hit.arg != static_cast<int>(PanelTab::Eyecam)) continue;
            offPanel.pointerDown(area.x + area.w / 2, area.y + area.h / 2, 0.0);
            offPanel.pointerUp();
            break;
        }
        CHECK(offPanel.tab() == PanelTab::Eyecam);
    }
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
    for (int k = 1; k <= 17; ++k) {
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

/**
 * The other Frame apps as frameeyeosc's panel gets them: the built-in list without frameeyeosc, keyboard and perf not
 * installed, mic running and aux installed.
 * @param busy short names whose Konsole is open ("" = the menu)
 * @return the list
 */
AppsView appsWith(const std::vector<std::string>& busy = {}) {
    using frame_apps::AppState;
    frame_apps::ManagerConfig config {"frameeyeosc", {}};
    config.fetch.cacheDir.clear();  // the built-in list, not one fetched before
    frame_apps::AppsManager apps(config);
    apps.setForPreview({{"frameeyeosc", AppState::Running},
                        {"frame-jp-keyboard", AppState::Missing},
                        {"frame-mic-tuner", AppState::Running},
                        {"frame-perf-overlay", AppState::Missing},
                        {"frame-aux-shortcuts", AppState::Installed}},
                       busy);
    AppsView view;
    view.entries = apps.entries();
    view.menuBusy = apps.busy("");
    return view;
}

/**
 * The list's index of an app.
 * @param apps the list
 * @param key its short name
 * @return the index (-1 if it isn't there)
 */
int appIndex(const AppsView& apps, const std::string& key) {
    for (size_t i = 0; i < apps.entries.size(); ++i) {
        if (apps.entries[i].app.key == key) return static_cast<int>(i);
    }
    return -1;
}

void testApps(const FontSet& fonts) {
    using frame_apps::AppState;
    using frame_apps::LaunchResult;
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    // The list leaves frameeyeosc out; the count says how many of the four aren't installed
    AppsView apps = appsWith();
    CHECK(apps.entries.size() == 4 && appIndex(apps, "eye") < 0);
    CHECK(appsCountText(ja, apps.entries) == "4 個のうち 2 個が入っていません");
    CHECK(appsCountText(en, apps.entries) == "2 of 4 not installed");
    {
        std::vector<frame_apps::Entry> all = apps.entries;
        for (frame_apps::Entry& entry : all) entry.state = AppState::Installed;
        CHECK(appsCountText(ja, all) == "4 個すべて入っています");
        CHECK(appsCountText(en, all) == "All 4 are installed");
        all[1].state = AppState::Missing;
        CHECK(appsCountText(ja, all) == "4 個のうち 1 個が入っていません");
    }

    for (const Language language : {Language::Ja, Language::En}) {
        EyePanel panel(fonts);
        PanelModel m = modelWith(Lids::Both, false);
        m.language = language;
        m.update.state = frame_updater::UpdateState::UpToDate;
        m.update.current = "0.7.7";
        panel.setTab(PanelTab::Advanced);
        panel.setAdvPage(AdvPage::Version);
        // Without a list there is no row
        panel.render(m);
        CHECK(hits(panel, PanelAction::AppsOpen).empty());
        // With one: the row at the bottom of the card, the page still without ▲ / ▼
        m.apps = apps;
        panel.render(m);
        const std::vector<EyePanel::HitArea> row = hits(panel, PanelAction::AppsOpen);
        CHECK(row.size() == 1);
        if (row.size() == 1) {
            CHECK(row[0].h == 72 && row[0].y + row[0].h <= 676 - 12 && row[0].y + row[0].h >= 676 - 30);
            CHECK(row[0].x >= 430 && row[0].x + row[0].w <= 1160);
        }
        CHECK(panel.advancedMaxScroll() == 0.0 && panel.appsShown() && !panel.appsListOpen());
        // Every update state: the row stays reachable (under the rest, scrolled to, when the page is taller), inside
        // the card
        using frame_updater::UpdateState;
        for (const UpdateState state : {UpdateState::Unknown, UpdateState::UpToDate, UpdateState::Available,
                                        UpdateState::Installing, UpdateState::Installed, UpdateState::CheckFailed,
                                        UpdateState::InstallFailed}) {
            for (const bool checking : {false, true}) {
                PanelModel tall = m;
                tall.update.state = state;
                tall.update.installable = true;
                tall.update.latest = "0.7.8";
                tall.update.checking = checking;
                tall.update.checkedAt = 1791287109;
                tall.update.error = "network";
                tall.update.version = "0.7.8";
                tall.update.step = "download";
                tall.update.notes = std::string(400, 'x');
                panel.setAdvancedScroll(0);
                panel.render(tall);
                panel.scroll(10000);
                panel.render(tall);
                const std::vector<EyePanel::HitArea> found = hits(panel, PanelAction::AppsOpen);
                CHECK(found.size() == 1);
                if (found.size() == 1) CHECK(found[0].y >= 174 && found[0].y + found[0].h <= 664);
            }
        }
        panel.setAdvancedScroll(0);

        // The row opens the list (the caller fetches it if due): rows by state, the cross and "Open the installer"
        panel.render(m);
        CHECK(press(panel, PanelAction::AppsOpen).action == PanelAction::AppsOpen);
        CHECK(panel.appsListOpen() && !panel.appsConfirmOpen() && panel.appsShown() && !panel.advPageShown());
        CHECK(panel.tab() == PanelTab::Advanced && panel.advPage() == AdvPage::Version);
        panel.render(m);
        CHECK(hits(panel, PanelAction::AppsClose).size() == 1 && hits(panel, PanelAction::AppsMenu).size() == 1);
        CHECK(subTabs(panel).empty() && hits(panel, PanelAction::UpdateCheck).empty());
        // "Install" only on the two that aren't installed (keyboard, perf); a chip on the running and installed ones
        std::vector<int> installs;
        for (const EyePanel::HitArea& area : hits(panel, PanelAction::AppsInstall)) installs.push_back(area.hit.arg);
        CHECK((installs == std::vector<int> {appIndex(apps, "keyboard"), appIndex(apps, "perf")}));
        // Four fit: no ▲ / ▼, and the controller doesn't scroll it
        CHECK(hits(panel, PanelAction::AppsScroll).empty() && !panel.wantsScroll() && !panel.scroll(40));
        // Everything on the list inside the card, below the tab row
        for (const EyePanel::HitArea& area : panel.hitAreas()) {
            if (area.hit.action == PanelAction::Tab || area.x < 400) continue;
            CHECK(area.y >= 96 && area.y + area.h <= 676 && area.x >= 416 && area.x + area.w <= 1176);
        }
        // An app whose Konsole is open: no "Install" (its chip says so)
        {
            PanelModel busy = m;
            busy.apps = appsWith({"perf"});
            panel.render(busy);
            installs.clear();
            for (const EyePanel::HitArea& area : hits(panel, PanelAction::AppsInstall)) installs.push_back(area.hit.arg);
            CHECK((installs == std::vector<int> {appIndex(apps, "keyboard")}));
        }

        // "Install": first the confirmation, with the command Konsole will run; only its buttons and the tabs work
        panel.render(m);
        CHECK(pressArg(panel, PanelAction::AppsInstall, appIndex(apps, "keyboard")).action == PanelAction::None);
        CHECK(panel.appsConfirmOpen() && panel.appsConfirmKey() == "keyboard");
        panel.render(m);
        CHECK(panel.appsConfirmEntry(m) != nullptr && panel.appsConfirmEntry(m)->app.name == "frame-jp-keyboard");
        CHECK(panel.appsConfirmCommand(m) == frame_apps::installerCommand("keyboard"));
        CHECK(panel.appsConfirmCommand(m) == "curl -fsSL https://frame.sasaken1102s.net | sh -s -- install keyboard");
        CHECK(hits(panel, PanelAction::AppsLaunch).size() == 1 && hits(panel, PanelAction::AppsCancel).size() == 1);
        CHECK(hits(panel, PanelAction::AppsInstall).empty() && hits(panel, PanelAction::AppsClose).empty() &&
              hits(panel, PanelAction::AppsMenu).empty());
        for (const EyePanel::HitArea& area : panel.hitAreas()) {
            if (area.x < 400) continue;
            CHECK(area.hit.action == PanelAction::Tab || area.hit.action == PanelAction::AppsLaunch ||
                  area.hit.action == PanelAction::AppsCancel);
            CHECK(area.y + area.h <= 676);
        }
        // "Open in Konsole" goes to the caller; one that didn't open stays with its reason, one that did goes back
        CHECK(press(panel, PanelAction::AppsLaunch).action == PanelAction::AppsLaunch);
        panel.setAppsLaunchResult(LaunchResult::NoKonsole);
        panel.render(m);
        CHECK(panel.appsConfirmOpen() && hits(panel, PanelAction::AppsLaunch).size() == 1);
        panel.setAppsLaunchResult(LaunchResult::Started);
        CHECK(panel.appsListOpen() && !panel.appsConfirmOpen());
        // "Cancel" goes back to the list too
        panel.render(m);
        pressArg(panel, PanelAction::AppsInstall, appIndex(apps, "perf"));
        panel.render(m);
        CHECK(panel.appsConfirmCommand(m) == frame_apps::installerCommand("perf"));
        CHECK(press(panel, PanelAction::AppsCancel).action == PanelAction::None);
        CHECK(panel.appsListOpen() && !panel.appsConfirmOpen());

        // "Open the installer": the menu's command; not while the menu's Konsole is still open
        panel.render(m);
        CHECK(press(panel, PanelAction::AppsMenu).action == PanelAction::None);
        CHECK(panel.appsConfirmOpen() && panel.appsConfirmKey().empty());
        panel.render(m);
        CHECK(panel.appsConfirmEntry(m) == nullptr);
        CHECK(panel.appsConfirmCommand(m) == frame_apps::installerCommand(""));
        CHECK(panel.appsConfirmCommand(m) == "curl -fsSL https://frame.sasaken1102s.net | sh");
        CHECK(press(panel, PanelAction::AppsLaunch).action == PanelAction::AppsLaunch);
        {
            PanelModel menuBusy = m;
            menuBusy.apps.menuBusy = true;
            panel.render(menuBusy);
            CHECK(hits(panel, PanelAction::AppsLaunch).empty());
            panel.closeAppsConfirm();
            panel.render(menuBusy);
            CHECK(hits(panel, PanelAction::AppsMenu).empty());
        }

        // An app only the site lists: its name and command from the list now; gone from it, it can't be opened
        {
            PanelModel site = m;
            frame_apps::Entry extra = apps.entries.front();
            extra.app.name = "frame-example";
            extra.app.key = "example";
            extra.state = AppState::Missing;
            site.apps.entries.push_back(extra);
            panel.render(site);
            CHECK(pressArg(panel, PanelAction::AppsInstall, 4).action == PanelAction::None);
            panel.render(site);
            CHECK(panel.appsConfirmEntry(site) != nullptr && panel.appsConfirmEntry(site)->app.name == "frame-example");
            CHECK(panel.appsConfirmCommand(site) == "curl -fsSL https://frame.sasaken1102s.net | sh -s -- install example");
            panel.render(m);
            CHECK(panel.appsConfirmEntry(m) == nullptr && panel.appsConfirmCommand(m) == frame_apps::installerCommand(""));
            CHECK(hits(panel, PanelAction::AppsLaunch).empty());
            CHECK(press(panel, PanelAction::AppsCancel).action == PanelAction::None);
        }

        // Many apps: the list scrolls (the controller and ▲ / ▼), and only what shows can be pressed
        {
            PanelModel many = m;
            for (int i = 0; i < 6; ++i) {
                frame_apps::Entry extra = apps.entries.front();
                extra.app.name = "frame-example-" + std::to_string(i);
                extra.state = AppState::Missing;
                many.apps.entries.push_back(extra);
            }
            panel.render(many);
            CHECK(panel.wantsScroll() && hits(panel, PanelAction::AppsScroll).size() == 1);
            for (const EyePanel::HitArea& area : hits(panel, PanelAction::AppsInstall)) {
                CHECK(area.y >= 186 && area.y + area.h <= 590);
            }
            CHECK(panel.scroll(60));
            CHECK(panel.scroll(-1000));
            CHECK(!panel.scroll(-10));
            CHECK(pressArg(panel, PanelAction::AppsScroll, 1).action == PanelAction::None);
            panel.render(many);
            CHECK(hits(panel, PanelAction::AppsScroll).size() == 2);
            panel.scroll(10000);
            panel.render(many);
            CHECK(hits(panel, PanelAction::AppsScroll).size() == 1);
            for (const EyePanel::HitArea& area : hits(panel, PanelAction::AppsInstall)) {
                CHECK(area.y >= 186 && area.y + area.h <= 590);
            }
        }

        // The cross goes back to "Version"; another tab closes the list too
        panel.render(m);
        CHECK(press(panel, PanelAction::AppsClose).action == PanelAction::None);
        CHECK(!panel.appsListOpen() && panel.advPageShown() && panel.advPage() == AdvPage::Version);
        panel.render(m);
        press(panel, PanelAction::AppsOpen);
        panel.render(m);
        pressTab(panel, PanelTab::Basic);
        CHECK(panel.tab() == PanelTab::Basic && !panel.appsListOpen());
        panel.render(m);
        pressTab(panel, PanelTab::Advanced);
        panel.render(m);
        CHECK(!panel.appsListOpen() && hits(panel, PanelAction::AppsOpen).size() == 1);
        // The version history opened over it closes it
        press(panel, PanelAction::AppsOpen);
        panel.openHistory();
        CHECK(!panel.appsListOpen() && panel.historyOpen());
    }
}

/**
 * Whether two areas overlap.
 * @param a one
 * @param b the other
 * @return true if they share some of their inside
 */
bool overlaps(const EyePanel::HitArea& a, const EyePanel::HitArea& b) {
    // (by more than half a pixel: a segmented control's choices meet edge to edge)
    return a.x + 0.5 < b.x + b.w && b.x + 0.5 < a.x + a.w && a.y + 0.5 < b.y + b.h && b.y + 0.5 < a.y + a.h;
}

/**
 * The eye preview's own buttons (the cross, the choices, the switches, freeze, the marks' switches) as last drawn.
 * @param panel the panel
 * @return them
 */
std::vector<EyePanel::HitArea> previewButtons(const EyePanel& panel) {
    std::vector<EyePanel::HitArea> found;
    for (const EyePanel::HitArea& area : panel.hitAreas()) {
        const PanelAction a = area.hit.action;
        if (a == PanelAction::PreviewClose || a == PanelAction::PreviewSource || a == PanelAction::PreviewMirror ||
            a == PanelAction::PreviewCamera || a == PanelAction::PreviewMark || a == PanelAction::PreviewFreeze) {
            found.push_back(area);
        }
    }
    return found;
}

void testPreview(const FontSet& fonts) {
    for (const Language language : {Language::Ja, Language::En}) {
        EyePanel panel(fonts);
        PanelModel m = modelWith(Lids::Both);
        m.language = language;
        m.update.state = frame_updater::UpdateState::UpToDate;
        m.eyecam.status.hasPreview = true;
        eye_preview::fake(m.preview, eye_preview::monotonicNs());
        // The button at the bottom of the left column: wide, inside the column, under the gaze
        panel.render(m);
        std::vector<EyePanel::HitArea> button = hits(panel, PanelAction::PreviewOpen);
        CHECK(button.size() == 1);
        if (button.size() == 1) {
            CHECK(button[0].x >= 44 && button[0].x + button[0].w <= 376 && button[0].w >= 300);
            CHECK(button[0].y >= 616 && button[0].y + button[0].h <= 670);
        }
        CHECK(!panel.previewOpen());

        // It opens the sheet over the tabs' content: its cross, three choices, two switches and freeze, inside the
        // card, apart from each other; the left column and every tab stay (none chosen: each closes it)
        CHECK(press(panel, PanelAction::PreviewOpen).action == PanelAction::None);
        // The pictures start off, so an eye never shows by accident
        CHECK(panel.previewOpen() && !panel.previewPictures());
        panel.render(m);
        const std::vector<EyePanel::HitArea> own = previewButtons(panel);
        CHECK(own.size() == 7);
        CHECK(hits(panel, PanelAction::PreviewClose).size() == 1 && hits(panel, PanelAction::PreviewSource).size() == 3);
        CHECK(hits(panel, PanelAction::PreviewMirror).size() == 1 && hits(panel, PanelAction::PreviewCamera).size() == 1);
        CHECK(hits(panel, PanelAction::PreviewFreeze).size() == 1);
        for (size_t i = 0; i < own.size(); ++i) {
            const EyePanel::HitArea& a = own[i];
            CHECK(a.x >= 438 && a.x + a.w <= 1154 + 0.5 && a.y >= 96 && a.y + a.h <= 676);
            for (size_t j = i + 1; j < own.size(); ++j) CHECK(!overlaps(a, own[j]));
        }
        int tabs = 0;
        for (const EyePanel::HitArea& area : hits(panel, PanelAction::Tab)) tabs += area.y < 100 ? 1 : 0;
        CHECK(tabs == 7);
        // Nothing of the tab below it can be pressed
        for (const EyePanel::HitArea& area : panel.hitAreas()) {
            const bool sheet = area.x >= 416 && area.y >= 96;
            const PanelAction a = area.hit.action;
            const bool ownButton = a == PanelAction::PreviewClose || a == PanelAction::PreviewSource ||
                                   a == PanelAction::PreviewMirror || a == PanelAction::PreviewCamera ||
                                   a == PanelAction::PreviewMark || a == PanelAction::PreviewFreeze;
            if (sheet) CHECK(ownButton);
        }
        // The button stays, now closing it
        CHECK(hits(panel, PanelAction::PreviewOpen).size() == 1);

        // The pictures' switch (and only that) asks for them and stops; the choices and the mirror keep it open
        CHECK(press(panel, PanelAction::PreviewCamera).action == PanelAction::None);
        CHECK(panel.previewOpen() && panel.previewPictures());
        panel.render(m);
        press(panel, PanelAction::PreviewCamera);
        CHECK(panel.previewOpen() && !panel.previewPictures());
        panel.render(m);
        press(panel, PanelAction::PreviewCamera);
        CHECK(panel.previewPictures());
        // Closed and opened again: still on (the switch is kept while the panel runs)
        press(panel, PanelAction::PreviewClose);
        panel.render(m);
        press(panel, PanelAction::PreviewOpen);
        CHECK(panel.previewOpen() && panel.previewPictures());
        panel.render(m);
        // Turned off, closed and opened again: off
        press(panel, PanelAction::PreviewCamera);
        CHECK(!panel.previewPictures());
        press(panel, PanelAction::PreviewClose);
        panel.render(m);
        press(panel, PanelAction::PreviewOpen);
        CHECK(panel.previewOpen() && !panel.previewPictures());
        panel.render(m);
        press(panel, PanelAction::PreviewCamera);
        CHECK(panel.previewPictures());
        panel.render(m);

        // With the pictures: a switch for each mark over them, all on at first, one under the other in the space
        // between the two pictures (176 px wide, centred in each half of the sheet), apart from the other buttons
        {
            const std::vector<EyePanel::HitArea> marks = hits(panel, PanelAction::PreviewMark);
            CHECK(marks.size() == 3);
            CHECK(panel.previewMarks().lids && panel.previewMarks().pupil && panel.previewMarks().window);
            const double half = (1154 - 438) / 2.0;
            for (const EyePanel::HitArea& a : marks) {
                CHECK(a.x >= 438 + half / 2 + 88 && a.x + a.w <= 438 + half * 1.5 - 88);
                // (above the footer's gaze line)
                CHECK(a.y >= 96 && a.y + a.h <= 676 - 36 - 22);
            }
            const std::vector<EyePanel::HitArea> all = previewButtons(panel);
            CHECK(all.size() == 10);
            for (size_t i = 0; i < all.size(); ++i) {
                for (size_t j = i + 1; j < all.size(); ++j) CHECK(!overlaps(all[i], all[j]));
            }
            // Each takes its mark off the pictures and puts it back; the sheet stays open
            // (the two pictures' boxes only, without the switches between them)
            const auto pictureBoxes = [&](const std::vector<uint8_t>& rgba) {
                std::vector<uint8_t> out;
                for (int y = 440; y < 620; ++y) {
                    for (const int left : {529, 887}) {
                        const size_t from = (static_cast<size_t>(y) * panel.width() + left) * 4;
                        out.insert(out.end(), rgba.begin() + from, rgba.begin() + from + 176 * 4);
                    }
                }
                return out;
            };
            panel.pointerLeave();
            panel.render(m);
            const std::vector<uint8_t> every = panel.toRgba();
            for (const eye_preview::Mark mark :
                 {eye_preview::Mark::Lids, eye_preview::Mark::Pupil, eye_preview::Mark::Window}) {
                CHECK(pressArg(panel, PanelAction::PreviewMark, static_cast<int>(mark)).action == PanelAction::None);
                CHECK(panel.previewOpen() && panel.previewPictures());
                const eye_preview::Marks& shown = panel.previewMarks();
                CHECK(shown.lids == (mark != eye_preview::Mark::Lids));
                CHECK(shown.pupil == (mark != eye_preview::Mark::Pupil));
                CHECK(shown.window == (mark != eye_preview::Mark::Window));
                panel.pointerLeave();
                panel.render(m);
                CHECK(pictureBoxes(panel.toRgba()) != pictureBoxes(every));
                pressArg(panel, PanelAction::PreviewMark, static_cast<int>(mark));
                panel.pointerLeave();
                panel.render(m);
                CHECK(panel.toRgba() == every);
            }
            // Kept when it closes and opens again (while the panel runs)
            pressArg(panel, PanelAction::PreviewMark, static_cast<int>(eye_preview::Mark::Pupil));
            press(panel, PanelAction::PreviewClose);
            panel.render(m);
            press(panel, PanelAction::PreviewOpen);
            panel.render(m);
            CHECK(panel.previewMarks().lids && !panel.previewMarks().pupil && panel.previewMarks().window);
            // Without the pictures: no switches for them (and no room kept)
            press(panel, PanelAction::PreviewCamera);
            panel.render(m);
            CHECK(hits(panel, PanelAction::PreviewMark).empty() && previewButtons(panel).size() == 7);
            press(panel, PanelAction::PreviewCamera);
            panel.render(m);
            pressArg(panel, PanelAction::PreviewMark, static_cast<int>(eye_preview::Mark::Pupil));
            CHECK(panel.previewMarks().pupil);
            panel.render(m);
        }
        pressArg(panel, PanelAction::PreviewSource, static_cast<int>(eye_preview::Source::Raw));
        panel.render(m);
        press(panel, PanelAction::PreviewMirror);
        panel.render(m);
        CHECK(panel.previewOpen());

        // Freeze: what shows stays as it was while the values move on; "Resume" lets it move again
        panel.render(m);
        press(panel, PanelAction::PreviewFreeze);
        panel.pointerLeave();
        panel.render(m);
        const std::vector<uint8_t> frozen = panel.toRgba();
        PanelModel moved = m;
        moved.preview.packet.eyes[1].lid = 0.1f;
        moved.preview.packet.eyes[1].opennessScaled = 0.1f;
        moved.preview.packet.rawGaze[2] = -0.5f;
        panel.render(moved);
        CHECK(panel.toRgba() == frozen);
        press(panel, PanelAction::PreviewFreeze);
        panel.pointerLeave();
        panel.render(moved);
        CHECK(panel.toRgba() != frozen);

        // Brows sent: drawn over the eyes (a picture of its own), the sheet's buttons where they were; frozen with the
        // rest, and the brow's level shows
        {
            PanelModel browed = moved;
            eye_preview::fakeBrows(browed.preview, 0.0);
            pressArg(panel, PanelAction::PreviewSource, static_cast<int>(eye_preview::Source::Sent));
            panel.pointerLeave();
            panel.render(moved);
            const std::vector<uint8_t> plain = panel.toRgba();
            const std::vector<EyePanel::HitArea> before = previewButtons(panel);
            panel.render(browed);
            const std::vector<uint8_t> rest = panel.toRgba();
            CHECK(rest != plain);
            const std::vector<EyePanel::HitArea> after = previewButtons(panel);
            CHECK(after.size() == before.size());
            for (size_t i = 0; i < after.size() && i < before.size(); ++i) {
                CHECK(after[i].x == before[i].x && after[i].y == before[i].y && after[i].w == before[i].w);
            }
            eye_preview::fakeBrows(browed.preview, 1.0);
            panel.render(browed);
            const std::vector<uint8_t> raised = panel.toRgba();
            CHECK(raised != rest);
            press(panel, PanelAction::PreviewFreeze);
            panel.pointerLeave();
            panel.render(browed);
            const std::vector<uint8_t> held = panel.toRgba();
            eye_preview::fakeBrows(browed.preview, 0.0);
            panel.render(browed);
            CHECK(panel.toRgba() == held);
            press(panel, PanelAction::PreviewFreeze);
            panel.pointerLeave();
            panel.render(browed);
            CHECK(panel.toRgba() == rest);
            // Raw: no brows drawn (nothing measures them), but the eyes stay where they are with brows
            pressArg(panel, PanelAction::PreviewSource, static_cast<int>(eye_preview::Source::Raw));
            panel.pointerLeave();
            eye_preview::fakeBrows(browed.preview, 1.0);
            panel.render(browed);
            const std::vector<uint8_t> rawUp = panel.toRgba();
            eye_preview::fakeBrows(browed.preview, 0.0);
            panel.render(browed);
            CHECK(panel.toRgba() == rawUp);
            // Brows off again: exactly as without
            pressArg(panel, PanelAction::PreviewSource, static_cast<int>(eye_preview::Source::Sent));
            panel.pointerLeave();
            panel.render(moved);
            CHECK(panel.toRgba() == plain);
        }

        // Without the eye cameras in use: no pictures' switch and no "Camera" values to press, the rest as before
        CHECK(panel.previewPictures());
        for (const int why : {0, 1, 2, 3}) {
            PanelModel without = m;
            if (why == 0) without.eyecam.visible = false;
            if (why == 1) without.eyecam.status.setupDone = false;
            if (why == 2) without.config.root.set(key::kCameraLids, JsonValue::makeBool(false));
            if (why == 3) without.eyecam.status.hasPreview = false;
            panel.render(without);
            CHECK(hits(panel, PanelAction::PreviewCamera).empty());
            bool cameraValues = false;
            for (const EyePanel::HitArea& area : hits(panel, PanelAction::PreviewSource)) {
                cameraValues |= area.hit.arg == static_cast<int>(eye_preview::Source::Camera);
            }
            // (an eyecam-rec without the pictures still has the camera's values)
            CHECK(cameraValues == (why == 3));
            CHECK(hits(panel, PanelAction::PreviewMirror).size() == 1 && hits(panel, PanelAction::PreviewClose).size() == 1);
            // The pictures' switch went off with them...
            CHECK(panel.previewOpen() && !panel.previewPictures());
            // ...and stays off once they can show again (no pictures by themselves when eyecam-rec comes back)
            panel.render(m);
            CHECK(hits(panel, PanelAction::PreviewCamera).size() == 1 && !panel.previewPictures());
            press(panel, PanelAction::PreviewCamera);
            CHECK(panel.previewPictures());
        }
        // eyecam-rec refusing "preview on": the same
        {
            PanelModel refused = m;
            refused.preview.refused = true;
            panel.render(refused);
            CHECK(hits(panel, PanelAction::PreviewCamera).empty() && !panel.previewPictures());
            panel.render(m);
            CHECK(!panel.previewPictures());
        }
        // Without frameeyeosc's samples, and with no pictures yet: drawn all the same
        PanelModel empty = m;
        empty.preview = eye_preview::View();
        panel.render(empty);
        CHECK(previewButtons(panel).size() == 7);

        // Its cross closes it; so does its button pressed again, and any tab
        press(panel, PanelAction::PreviewClose);
        CHECK(!panel.previewOpen());
        panel.render(m);
        press(panel, PanelAction::PreviewOpen);
        panel.render(m);
        press(panel, PanelAction::PreviewOpen);
        CHECK(!panel.previewOpen());
        panel.render(m);
        press(panel, PanelAction::PreviewOpen);
        panel.render(m);
        pressTab(panel, PanelTab::Gaze);
        CHECK(!panel.previewOpen() && panel.tab() == PanelTab::Gaze);
        // ...and the tab it was opened over, pressed while it shows
        panel.render(m);
        press(panel, PanelAction::PreviewOpen);
        panel.render(m);
        pressTab(panel, PanelTab::Gaze);
        CHECK(!panel.previewOpen() && panel.tab() == PanelTab::Gaze);

        // A notice or a message at the bottom of the left column: the button a square at the right of it, both there
        for (const int which : {0, 1, 2}) {
            PanelModel shared = m;
            if (which == 0) {
                shared.update.state = frame_updater::UpdateState::Available;
                shared.update.latest = "0.7.8";
            }
            if (which == 1) shared.panelError = "rename failed: Read-only file system";
            if (which == 2) shared.eyecam.status.setupDone = false;
            panel.render(shared);
            button = hits(panel, PanelAction::PreviewOpen);
            CHECK(button.size() == 1);
            if (button.size() != 1) continue;
            CHECK(button[0].w == 44 && button[0].h == 44 && button[0].x + button[0].w <= 376 + 0.5);
            CHECK(button[0].y + button[0].h <= 670 + 0.5);
            for (const EyePanel::HitArea& area : panel.hitAreas()) {
                if (area.x < 416 && area.hit.action != PanelAction::PreviewOpen) CHECK(!overlaps(area, button[0]));
            }
            if (which == 0) CHECK(hits(panel, PanelAction::AdvancedPage).size() == 1);
        }
    }
}

/**
 * The usable buttons with this action and argument as last drawn.
 * @param panel the panel
 * @param action the action
 * @param arg its argument
 * @return how many
 */
size_t hitsArg(const EyePanel& panel, PanelAction action, int arg) {
    size_t count = 0;
    for (const EyePanel::HitArea& area : hits(panel, action)) count += area.hit.arg == arg ? 1 : 0;
    return count;
}

void testContrib(const FontSet& fonts) {
    // "Record for squint training": its row on Debug tools (only while eyecam-rec runs, usable once the cameras are set
    // up; "Recordings" once there is a folder), each screen's buttons in both languages, its recording shown in it, the
    // delete's confirmation, and what closes it
    {
        EyePanel panel(fonts);
        panel.setTab(PanelTab::Advanced);
        panel.setAdvPage(AdvPage::Tools);
        panel.render(modelWith(Lids::Both, false));
        CHECK(hits(panel, PanelAction::ContribOpen).empty());
        PanelModel notSetUp = modelWith(Lids::Both, true);
        notSetUp.eyecam.status.setupDone = false;
        notSetUp.eyecam.status.calibState = 0;
        panel.render(notSetUp);
        CHECK(hits(panel, PanelAction::ContribOpen).empty());
    }
    const int record = static_cast<int>(contrib::Page::Record);
    const int manage = static_cast<int>(contrib::Page::Manage);
    for (const Language language : {Language::Ja, Language::En}) {
        EyePanel panel(fonts);
        panel.setTab(PanelTab::Advanced);
        panel.setAdvPage(AdvPage::Tools);
        PanelModel m = modelWith(Lids::Both, true);
        m.language = language;
        m.contrib.loaded = true;
        m.contrib.dir = "/home/steamos/eyecam/contrib";
        m.contrib.freeBytes = 30'000'000'000LL;
        m.status.tracking = true;
        panel.render(m);
        CHECK(hitsArg(panel, PanelAction::ContribOpen, record) == 1);
        CHECK(hitsArg(panel, PanelAction::ContribOpen, manage) == 0);
        CHECK(pressArg(panel, PanelAction::ContribOpen, record).action == PanelAction::ContribOpen);
        CHECK(panel.contribOpen() && panel.contribPage() == contrib::Page::Record && !panel.advPageShown());
        // The consent: "Agree and continue", the cross and "Cancel", no sub-tabs
        panel.render(m);
        CHECK(hitsArg(panel, PanelAction::ContribAgree, 1) == 1);
        CHECK(hits(panel, PanelAction::ContribClose).size() == 2);
        CHECK(hits(panel, PanelAction::AdvancedPage).empty() && hits(panel, PanelAction::ContribStart).empty());
        CHECK(pressArg(panel, PanelAction::ContribAgree, 1).action == PanelAction::None);
        CHECK(panel.contribAgreed());
        // ...then glasses or inserts: four answers (the caller writes the consent with it), and "Back"
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribEyewear).size() == 4);
        CHECK(hitsArg(panel, PanelAction::ContribAgree, 0) == 1);
        const int inserts = static_cast<int>(contrib::Eyewear::Inserts);
        const PanelHit answer = pressArg(panel, PanelAction::ContribEyewear, inserts);
        CHECK(answer.action == PanelAction::ContribEyewear && answer.arg == inserts);
        CHECK(!panel.contribAgreed());
        m.contrib.exists = true;
        m.contrib.info.consentVersion = contrib::kConsentVersion;
        m.contrib.info.eyewear = contrib::Eyewear::Inserts;
        // The start; none while a command waits or without the room
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribStart).size() == 1);
        m.eyecam.busy = true;
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribStart).empty());
        m.eyecam.busy = false;
        m.contrib.freeBytes = 2'000'000'000LL;
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribStart).empty());
        m.contrib.freeBytes = 30'000'000'000LL;
        // ...nor without frameeyeosc's eye data (Valve's values are recorded with it)
        m.status.tracking = false;
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribStart).empty());
        m.status.tracking = true;
        // Its recording runs in the sheet: the eye capture's run with its "Stop"
        m.contrib.running = true;
        eyecam::Status& s = m.eyecam.status;
        s.state = eyecam::State::Recording;
        s.stateText = "recording";
        s.locked = true;
        s.protocol = contrib::kProtocol;
        s.stepLabel = "look_left";
        s.stepCount = 23;
        s.stepIndex = 19;
        s.elapsedS = 80;
        s.totalS = 94.5;
        s.stepRemainingS = 3;
        panel.render(m);
        CHECK(hits(panel, PanelAction::EyecamStop).size() == 1);
        CHECK(hits(panel, PanelAction::ContribStart).empty() && hits(panel, PanelAction::ContribDelete).empty());
        s.state = eyecam::State::Idle;
        s.stateText = "idle";
        m.contrib.running = false;
        // It didn't finish: "Record again"
        m.contrib.failure.shown = true;
        m.contrib.failure.why = contrib::Why::Incomplete;
        m.contrib.failure.message = "cues.csv";
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribStart).size() == 1);
        m.contrib.failure = contrib::Failure();
        // Recorded: pack, delete, close (and the cross)
        m.contrib.justDone = true;
        m.contrib.info.recordings.resize(2);
        m.contrib.info.recordings[0].bytes = m.contrib.info.recordings[1].bytes = 2'700'000'000LL;
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribPack).size() == 1 && hits(panel, PanelAction::ContribDelete).size() == 1);
        CHECK(hits(panel, PanelAction::ContribClose).size() == 2 && hits(panel, PanelAction::ContribStart).empty());
        // Packing: no second one, no delete; packed with all of them: none until there are more
        m.contrib.progress.job = contrib::Job::Pack;
        m.contrib.progress.running = true;
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribPack).empty() && hits(panel, PanelAction::ContribDelete).empty());
        m.contrib.progress = contrib::Progress();
        m.contrib.info.packed = "contrib_2026-10-25_14-25-02.tar.zst";
        m.contrib.info.packedCount = 2;
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribPack).empty());
        m.contrib.info.recordings.resize(3);
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribPack).size() == 1);
        // Deleting: its confirmation first (only its buttons), "Cancel" back, then the delete (back to "Record")
        CHECK(press(panel, PanelAction::ContribDelete).action == PanelAction::None);
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribDeleteConfirm).size() == 1);
        CHECK(hits(panel, PanelAction::ContribDelete).empty() && hits(panel, PanelAction::ContribPack).empty());
        CHECK(press(panel, PanelAction::ContribDeleteCancel).action == PanelAction::None);
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribDeleteConfirm).empty());
        CHECK(hits(panel, PanelAction::ContribDelete).size() == 1);
        press(panel, PanelAction::ContribDelete);
        panel.render(m);
        CHECK(press(panel, PanelAction::ContribDeleteConfirm).action == PanelAction::ContribDeleteConfirm);
        // A tab closes it; the row offers "Recordings" now there is a folder, which opens the end's screen
        pressTab(panel, PanelTab::Basic);
        CHECK(!panel.contribOpen());
        panel.render(m);
        pressTab(panel, PanelTab::Advanced);
        m.contrib.justDone = false;
        panel.render(m);
        CHECK(panel.advPageShown() && hitsArg(panel, PanelAction::ContribOpen, manage) == 1);
        pressArg(panel, PanelAction::ContribOpen, manage);
        CHECK(panel.contribPage() == contrib::Page::Manage);
        panel.render(m);
        CHECK(hits(panel, PanelAction::ContribPack).size() == 1 && hits(panel, PanelAction::ContribStart).empty());
    }
}

void testVrcFaceTrackingRow(const FontSet& fonts) {
    // The Output tab's "Also send to VRChat face tracking": On / Off right under "VRChat's own eyes too", only while
    // VRChat directly is the output. With the eye cameras in use (the pupil row under it), set up but off (the dashed
    // line) or not there at all, in both languages: inside the card, apart from the rows around it
    for (const Language language : {Language::Ja, Language::En}) {
        for (const int cameras : {0, 1, 2}) {  // in use, switched off, no eyecam-rec
            PanelModel m = cameras == 2 ? modelWith(Lids::Valve, false) : modelWith(Lids::Both);
            m.language = language;
            if (cameras == 1) {
                m.config.root.set(key::kCameraLids, JsonValue::makeBool(false));
                m.status.camera.used[0] = m.status.camera.used[1] = false;
            }
            EyePanel panel(fonts);
            panel.setTab(PanelTab::Output);
            panel.render(m);
            const std::vector<EyePanel::HitArea> face = hits(panel, PanelAction::SetBool, key::kVrcFaceTracking);
            const std::vector<EyePanel::HitArea> native = hits(panel, PanelAction::SetBool, key::kNativeEyes);
            CHECK(face.size() == 2 && native.size() == 2);
            if (face.size() != 2 || native.size() != 2) continue;
            CHECK(face[0].hit.arg == 1 && face[1].hit.arg == 0);  // "On", "Off"
            for (const EyePanel::HitArea& a : face) {
                CHECK(a.x >= 438 && a.x + a.w <= 1154 + 0.5 && a.y >= 110 && a.y + a.h <= 676 && a.h >= 40);
                // (below the native eyes' row and its hint)
                for (const EyePanel::HitArea& n : native) CHECK(!overlaps(a, n) && a.y >= n.y + n.h + 16);
            }
            // ...and what comes under it (the pupils, or the dashed line to the eye cameras tab) further down
            std::vector<EyePanel::HitArea> below = hits(panel, PanelAction::SetInteger, key::kPupilBits);
            for (const EyePanel::HitArea& area : hits(panel, PanelAction::Tab)) {
                if (area.y > 100 && area.x > 400) below.push_back(area);
            }
            CHECK(below.size() == (cameras == 0 ? 5u : (cameras == 1 ? 1u : 0u)));
            for (const EyePanel::HitArea& b : below) CHECK(b.y >= face[0].y + face[0].h + 16 && b.y + b.h <= 676);
            CHECK(allInsideAndTall(panel));
            // Locked: drawn, nothing to press (the row above still there)
            PanelModel locked = m;
            locked.status.locked.push_back(key::kVrcFaceTracking);
            panel.render(locked);
            CHECK(hits(panel, PanelAction::SetBool, key::kVrcFaceTracking).empty());
            CHECK(hits(panel, PanelAction::SetBool, key::kNativeEyes).size() == 2);
            // The other outputs: not there, their rows as before
            for (const char* output : {kOutputLivelink, kOutputModule, kOutputEtvr}) {
                PanelModel other = m;
                other.config.root.set(key::kOutput, JsonValue::makeString(output));
                panel.render(other);
                CHECK(hits(panel, PanelAction::SetBool, key::kVrcFaceTracking).empty());
                CHECK(hits(panel, PanelAction::SetBool, key::kNativeEyes).empty() && allInsideAndTall(panel));
            }
        }
    }
}

int main() {
    FontSet fonts;
    fonts.load(kFontPath, kBoldFontPath);
    testContrib(fonts);
    testWidenRouting(fonts);
    testWidenSwitch(fonts);
    testLidVetoSwitch(fonts);
    testLidsPages(fonts);
    testSnapMark(fonts);
    testEyeBehavior(fonts);
    testLidsRemembered(fonts);
    testGazePages(fonts);
    testOwnEyeGazeRow(fonts);
    testGazeScaleLocks(fonts);
    testSliderMoved(fonts);
    testRelearnButton(fonts);
    testNumberSlider(fonts);
    testBrowsPage(fonts);
    testPupilBitsRow(fonts);
    testVrcFaceTrackingRow(fonts);
    testOutputCards(fonts);
    testModuleLinkError(fonts);
    testToolNotice(fonts);
    testErrorBack(fonts);
    testWayOut(fonts);
    testCalibNeedsLids(fonts);
    testPupilSetup(fonts);
    testSearchSetup(fonts);
    testDiagPage(fonts);
    testSubTabs(fonts);
    testGazeRingsRow(fonts);
    testUpdateNotice(fonts);
    testUpdateBeta(fonts);
    testDiagFromEyecam(fonts);
    testPagesFit(fonts);
    testRecords(fonts);
    testRecordBars(fonts);
    testPartialResult(fonts);
    testFitRunning(fonts);
    testFitTabPolicy(fonts);
    testCameraRows(fonts);
    testIconText(fonts);
    testApps(fonts);
    testPreview(fonts);
    if (gFailures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("panel-test: all passed\n");
    return 0;
}
