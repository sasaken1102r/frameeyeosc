// Tests for the Eyelids and eye cameras tabs as drawn (案E): which buttons they offer for each source of the eyelids
// (the widening slider's routing, "Fine-tune" folded and open, the sensitivity slider moved off the eye cameras page)
// and a setting's slider let go of, the setup's (3) with a pupil not found, and a calibration that went through without
// one eye. Built with the panel as panel-test (it renders offscreen with the panel's fonts) and
// run after it is built; exits non-zero on failure.
#include "config.h"
#include "draw.h"
#include "model.h"
#include "panel.h"

#include <cmath>
#include <cstdio>
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

void testFold(const FontSet& fonts) {
    // Folded: the main rows (presets, the sync slider, "Fine-tune"); no marks, scales or One Euro values
    EyePanel panel(fonts);
    panel.setTab(PanelTab::Lids);
    const PanelModel m = modelWith(Lids::Valve, false);
    panel.render(m);
    CHECK(hits(panel, PanelAction::LidMarks).size() == 1);
    CHECK(hits(panel, PanelAction::LidPreset).size() == 3);
    CHECK(hits(panel, PanelAction::NumberSlider, key::kLidSync).size() == 1);
    CHECK(!hits(panel, PanelAction::Step, key::kBlinkHoldMs).empty());
    CHECK(!hits(panel, PanelAction::Step, key::kBlinkSyncBelow).empty());
    for (const char* hidden : {key::kLidClosed, key::kLidOpen, key::kLidMinCutoff, key::kLidBeta, key::kLidSync}) {
        CHECK(hits(panel, PanelAction::Step, hidden).empty());
    }
    CHECK(hits(panel, PanelAction::ScaleAuto).empty());
    CHECK(hits(panel, PanelAction::SetBool, key::kLidCalibration).empty());
    // "Fine-tune" pressed: the details in their place, and the same button folds them again
    const EyePanel::HitArea open = hits(panel, PanelAction::LidMarks)[0];
    panel.pointerDown(open.x + open.w / 2, open.y + open.h / 2, 0.0);
    panel.pointerUp();
    panel.render(m);
    for (const char* shown : {key::kLidClosed, key::kLidOpen, key::kLidMinCutoff, key::kLidBeta}) {
        CHECK(!hits(panel, PanelAction::Step, shown).empty());
    }
    CHECK(hits(panel, PanelAction::ScaleAuto).size() == 1);
    CHECK(hits(panel, PanelAction::SetBool, key::kLidCalibration).size() == 2);  // on / off (no eye fit)
    CHECK(hits(panel, PanelAction::SetLidWiden).empty());
    CHECK(hits(panel, PanelAction::LidPreset).empty());
    const EyePanel::HitArea close = hits(panel, PanelAction::LidMarks)[0];
    panel.pointerDown(close.x + close.w / 2, close.y + close.h / 2, 0.0);
    panel.pointerUp();
    panel.render(m);
    CHECK(hits(panel, PanelAction::LidPreset).size() == 3);
    CHECK(hits(panel, PanelAction::Step, key::kLidClosed).empty());
    // Open with the cameras driving both: marks 3 and 4 can't be stepped, 1 and 2 can
    panel.setLidMarks(true);
    panel.render(modelWith(Lids::Both));
    CHECK(!hits(panel, PanelAction::Step, key::kLidClosed).empty());
    CHECK(hits(panel, PanelAction::Step, key::kLidWidenStart).empty());
    CHECK(hits(panel, PanelAction::Step, key::kLidWide).empty());
}

void testSliderMoved(const FontSet& fonts) {
    // The eye cameras' page: no sensitivity slider any more, a pointer to the Eyelids tab instead
    EyePanel panel(fonts);
    const PanelModel m = modelWith(Lids::Both);
    panel.setTab(PanelTab::Eyecam);
    panel.render(m);
    CHECK(hits(panel, PanelAction::EyecamSensitivity).empty());
    CHECK(pointsTo(panel, PanelTab::Lids));
    CHECK(hits(panel, PanelAction::SetBool, key::kCameraLids).size() == 2);
    CHECK(hits(panel, PanelAction::EyecamCalib).size() == 2);
    // ...the pointer goes there, where the slider is
    for (const EyePanel::HitArea& area : hits(panel, PanelAction::Tab)) {
        if (area.hit.arg != static_cast<int>(PanelTab::Lids) || area.y < 100) continue;
        panel.pointerDown(area.x + area.w / 2, area.y + area.h / 2, 0.0);
        panel.pointerUp();
        break;
    }
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

    // The recording's error on the Advanced tab: "Start again" and "Back"; after "Back", "Start recording" alone
    PanelModel rec = eyecamIn("error");
    rec.eyecam.lastRun = eyecam::Run::Recording;
    rec.eyecam.status.message = "右のカメラの映像が 3 秒届きません";
    panel.setTab(PanelTab::Advanced);
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

int main() {
    FontSet fonts;
    fonts.load(kFontPath, kBoldFontPath);
    testWidenRouting(fonts);
    testFold(fonts);
    testSliderMoved(fonts);
    testNumberSlider(fonts);
    testPupilBitsRow(fonts);
    testToolNotice(fonts);
    testErrorBack(fonts);
    testWayOut(fonts);
    testCalibNeedsLids(fonts);
    testPupilSetup(fonts);
    testPartialResult(fonts);
    if (gFailures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("panel-test: all passed\n");
    return 0;
}
