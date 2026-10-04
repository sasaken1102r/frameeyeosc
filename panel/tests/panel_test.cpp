// Tests for the Eyelids and eye cameras tabs as drawn (案E): which buttons they offer for each source of the eyelids
// (the widening slider's routing, "Fine-tune" folded and open, the sensitivity slider moved off the eye cameras page)
// and a setting's slider let go of. Built with the panel as panel-test (it renders offscreen with the panel's fonts) and
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

int main() {
    FontSet fonts;
    fonts.load(kFontPath, kBoldFontPath);
    testWidenRouting(fonts);
    testFold(fonts);
    testSliderMoved(fonts);
    testNumberSlider(fonts);
    if (gFailures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("panel-test: all passed\n");
    return 0;
}
