// Tests for the panel's shared rules (model.cpp): what an eye fit writes and what "Reset" clears, which re-wear fit
// auto_recenter asks for, the output types behind the destination cards, the gaze presets and their migration,
// status.json's source_error, dominant_eye and openness_saturated, and which summary of a new release is shown. Built
// with the panel as model-test; exits non-zero on failure.
#include "model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
 * A config root with a fit and a per-eye scale tweak already in it.
 * @return the root object
 */
JsonValue tweakedRoot() {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    root.set(key::kLidScaleLeft, JsonValue::makeNumber(1.1));
    root.set(key::kLidScaleRight, JsonValue::makeNumber(0.69));
    for (const auto& eye : kLidFitKeys) {
        for (const char* name : eye) root.set(name, JsonValue::makeNumber(0.5));
    }
    return root;
}

/**
 * A number in the root, or NaN when it is null or missing.
 * @param root the root object
 * @param name the key
 * @return the number
 */
double numberIn(const JsonValue& root, const char* name) {
    const JsonValue* value = root.get(name);
    return value != nullptr && value->isNumber() ? value->number : NAN;
}

/**
 * A measured eye fit.
 * @param lids whether it measured the eyelids
 * @return the values
 */
gaze_fit::Values measured(bool lids) {
    gaze_fit::Values values;
    values.offsetX = 0.01;
    values.offsetY = -0.02;
    values.gainUp = 0.88;
    values.hasLids = lids;
    for (int eye = 0; eye < 2; ++eye) {
        values.lidClosed[eye] = 0.2;
        values.lidUp[eye] = 0.9;
        values.lidOpen[eye] = 0.85;
        values.lidDown[eye] = 0.7;
    }
    return values;
}

/** The whole fit clears the scale tweaks with the new lid readings; the re-wear fits leave them. */
void testFitResetsScales() {
    JsonValue full = tweakedRoot();
    applyFitValues(full, measured(true), gaze_fit::Mode::Full);
    CHECK(full.get(key::kLidScaleLeft) != nullptr && full.get(key::kLidScaleLeft)->isNull());
    CHECK(full.get(key::kLidScaleRight) != nullptr && full.get(key::kLidScaleRight)->isNull());
    CHECK(std::fabs(numberIn(full, key::kLidFitOpenLeft) - 0.85) < 1e-9);

    // Without eyelid readings the old lid fit stays, and so do its tweaks
    JsonValue gazeOnly = tweakedRoot();
    applyFitValues(gazeOnly, measured(false), gaze_fit::Mode::Full);
    CHECK(std::fabs(numberIn(gazeOnly, key::kLidScaleRight) - 0.69) < 1e-9);
    CHECK(std::fabs(numberIn(gazeOnly, key::kLidFitOpenLeft) - 0.5) < 1e-9);

    JsonValue rewear = tweakedRoot();
    applyFitValues(rewear, measured(true), gaze_fit::Mode::Center);
    CHECK(std::fabs(numberIn(rewear, key::kLidScaleLeft) - 1.1) < 1e-9);
    CHECK(std::fabs(numberIn(rewear, key::kLidScaleRight) - 0.69) < 1e-9);
    CHECK(std::fabs(numberIn(rewear, key::kLidFitOpenLeft) - 0.5) < 1e-9);
    CHECK(std::fabs(numberIn(rewear, key::kGazeOffsetX) - 0.01) < 1e-9);
    // Re-centering writes the zero point only: no tilt, no gains
    CHECK(std::isnan(numberIn(rewear, key::kGazeRollDeg)) && std::isnan(numberIn(rewear, key::kGazeGainUp)));

    // An eye the fit didn't measure (Valve's openness stuck shut, the eye camera stood in) keeps its lid fit and
    // scale; the other eye is written as usual
    JsonValue kept = tweakedRoot();
    gaze_fit::Values rightKept = measured(true);
    rightKept.lidsKept[1] = true;
    rightKept.lidOpen[1] = 0.0;
    applyFitValues(kept, rightKept, gaze_fit::Mode::Full);
    CHECK(kept.get(key::kLidScaleLeft) != nullptr && kept.get(key::kLidScaleLeft)->isNull());
    CHECK(std::fabs(numberIn(kept, key::kLidScaleRight) - 0.69) < 1e-9);
    CHECK(std::fabs(numberIn(kept, key::kLidFitOpenLeft) - 0.85) < 1e-9);
    for (const char* name : kLidFitKeys[1]) CHECK(std::fabs(numberIn(kept, name) - 0.5) < 1e-9);
    // ...and an eye without a lid fit stays without one
    JsonValue none;
    none.type = JsonValue::Type::Object;
    applyFitValues(none, rightKept, gaze_fit::Mode::Full);
    for (const char* name : kLidFitKeys[1]) CHECK(none.get(name) == nullptr);
    CHECK(none.get(key::kLidScaleRight) == nullptr && std::fabs(numberIn(none, key::kLidFitClosedLeft) - 0.2) < 1e-9);
    // So the Eye fit tab has the left eye's lid fit only (its line, "not fitted" for the right; see lidFitLines), and
    // the left eye alone takes lid_widen
    PanelModel oneEye;
    oneEye.config.exists = true;
    oneEye.config.root = none;
    const FitInConfig fitted = fitInConfig(SettingsView(oneEye));
    CHECK(fitted.lidsFitted[0] && !fitted.lidsFitted[1] && !fitted.values.hasLids);
    CHECK(std::fabs(fitted.values.lidOpen[0] - 0.85) < 1e-9 && std::fabs(fitted.values.lidDown[0] - 0.7) < 1e-9);
    const WidenState widen = widenState(SettingsView(oneEye));
    CHECK(widen.fitted[0] && !widen.fitted[1] && !widen.room[1]);
}

/** The whole fit writes the zero point, the up gain, 1 sideways and down, no tilt, and takes each eye's own x out. */
void testFitWritesPolicy() {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    root.set(key::kGazeRollDeg, JsonValue::makeNumber(6.7));
    root.set(key::kGazeGainX, JsonValue::makeNumber(0.93));
    root.set(key::kGazeGainDown, JsonValue::makeNumber(0.88));
    root.set(key::kGazeOffsetXLeft, JsonValue::makeNumber(0.031));
    root.set(key::kGazeOffsetXRight, JsonValue::makeNumber(-0.006));
    root.set(key::kGazeGainXLeft, JsonValue::makeNumber(0.95));
    root.set(key::kGazeGainXRight, JsonValue::makeNumber(0.9));
    JsonValue rewear = root;
    applyFitValues(root, measured(true), gaze_fit::Mode::Full);
    CHECK(numberIn(root, key::kGazeRollDeg) == 0.0 && numberIn(root, key::kGazeGainX) == 1.0);
    CHECK(numberIn(root, key::kGazeGainDown) == 1.0 && std::fabs(numberIn(root, key::kGazeGainUp) - 0.88) < 1e-9);
    CHECK(std::fabs(numberIn(root, key::kGazeOffsetY) + 0.02) < 1e-9);
    for (const char* name : {key::kGazeOffsetXLeft, key::kGazeOffsetXRight, key::kGazeGainXLeft, key::kGazeGainXRight}) {
        CHECK(root.get(name) != nullptr && root.get(name)->isNull());
    }
    // Re-centering leaves whatever else is there (set by hand) alone
    applyFitValues(rewear, measured(true), gaze_fit::Mode::Center);
    CHECK(numberIn(rewear, key::kGazeRollDeg) == 6.7 && numberIn(rewear, key::kGazeGainXLeft) == 0.95);
    // What the Eye fit tab shows of it
    PanelModel model;
    model.config.exists = true;
    model.config.root = root;
    const FitInConfig fit = fitInConfig(SettingsView(model));
    CHECK(fit.gazeFitted && fit.values.hasLids && std::fabs(fit.values.gainUp - 0.88) < 1e-9);
    CHECK(fit.values.gainX == 1.0 && fit.values.gainDown == 1.0 && fit.values.rollDeg == 0.0);
}

/** "Reset" clears the scales along with the fit. */
void testResetClearsScales() {
    const std::vector<std::string> keys = fitResetKeys();
    const auto has = [&](const char* name) { return std::find(keys.begin(), keys.end(), name) != keys.end(); };
    CHECK(has(key::kLidScaleLeft) && has(key::kLidScaleRight));
    CHECK(has(key::kGazeOffsetX) && has(key::kGazeRollDeg) && has(key::kGazeGainXRight));
    for (const auto& eye : kLidFitKeys) {
        for (const char* name : eye) CHECK(has(name));
    }
}

/**
 * auto_recenter as written in a config.
 * @param value the value, or null for none
 * @return the kind
 */
AutoRecenter recenterOf(const JsonValue& value) {
    ConfigFile config;
    config.exists = true;
    config.root.type = JsonValue::Type::Object;
    if (!value.isNull()) config.root.set(key::kAutoRecenter, value);
    return autoRecenter(config);
}

/** Re-centering (the middle dot) is the default; "tilt" from before 0.7.7 reads as it. */
void testRecenterDefault() {
    CHECK(recenterOf(JsonValue::makeNull()) == AutoRecenter::Center);
    CHECK(recenterOf(JsonValue::makeString("center")) == AutoRecenter::Center);
    CHECK(recenterOf(JsonValue::makeString("tilt")) == AutoRecenter::Center);
    CHECK(recenterOf(JsonValue::makeString("off")) == AutoRecenter::Off);
    CHECK(recenterOf(JsonValue::makeString("sideways")) == AutoRecenter::Center);
    CHECK(recenterOf(JsonValue::makeBool(true)) == AutoRecenter::Center);
    CHECK(recenterOf(JsonValue::makeBool(false)) == AutoRecenter::Off);
    CHECK(std::string(kAutoRecenterModes[0]) == "off" && std::string(kAutoRecenterModes[1]) == "center");
    const SettingSpec* spec = findSetting(key::kAutoRecenter);
    CHECK(spec != nullptr && std::string(spec->defaultText) == "center");
}

/**
 * widenState for a made-up model: both eyes fitted with these straight-ahead readings.
 * @param left the left eye's open reading
 * @param right the right eye's
 * @param mode lid_widen
 * @return the state
 */
WidenState widenOf(double left, double right, const char* mode) {
    PanelModel model;
    model.config.exists = true;
    model.config.root.type = JsonValue::Type::Object;
    const double open[2] = {left, right};
    for (int eye = 0; eye < 2; ++eye) {
        const double readings[4] = {0.2, open[eye] + 0.01, open[eye], open[eye] - 0.1};
        for (int i = 0; i < 4; ++i) model.config.root.set(kLidFitKeys[eye][i], JsonValue::makeNumber(readings[i]));
    }
    if (mode != nullptr) model.config.root.set(key::kLidWiden, JsonValue::makeString(mode));
    return widenState(SettingsView(model));
}

/** Which eye can widen by itself, as frameeyeosc decides it. */
void testWidenState() {
    // One user's eyes: the left reads 0.945 straight ahead (no room), the right 0.835; "normal" by default
    WidenState s = widenOf(0.945, 0.835, nullptr);
    CHECK(s.mode == 2 && s.fitted[0] && s.fitted[1] && !s.room[0] && s.room[1]);
    // 0.90 + 0.07 is just within 0.97, 0.91 is not; "low" needs 0.87 or less, "high" 0.93
    s = widenOf(0.90, 0.91, "normal");
    CHECK(s.room[0] && !s.room[1]);
    s = widenOf(0.87, 0.88, "low");
    CHECK(s.mode == 1 && s.room[0] && !s.room[1]);
    s = widenOf(0.93, 0.94, "high");
    CHECK(s.mode == 3 && s.room[0] && !s.room[1]);
    // Off: nobody widens
    s = widenOf(0.8, 0.8, "off");
    CHECK(s.mode == 0 && !s.room[0] && !s.room[1]);
    // Not fitted: the setting doesn't apply
    PanelModel plain;
    plain.config.exists = true;
    plain.config.root.type = JsonValue::Type::Object;
    s = widenState(SettingsView(plain));
    CHECK(!s.fitted[0] && !s.fitted[1] && !s.room[0] && !s.room[1]);
    const SettingSpec* spec = findSetting(key::kLidWiden);
    CHECK(spec != nullptr && std::string(spec->defaultText) == "normal");
}

/**
 * widenSlider for a made-up model: frameeyeosc running with these eyes on the cameras.
 * @param left the left eye's lid from the cameras
 * @param right the right eye's
 * @param saturated a relaxed open eye reads 1.0 (SteamOS 0.4.3)
 * @param fitted which eyes have an eye fit (0 none, 1 left, 2 right, 3 both)
 * @param running frameeyeosc runs
 * @return the slider
 */
WidenSlider sliderOf(bool left, bool right, bool saturated, int fitted, bool running = true) {
    PanelModel model;
    model.config.exists = true;
    model.config.root.type = JsonValue::Type::Object;
    for (int eye = 0; eye < 2; ++eye) {
        if ((fitted & (1 << eye)) == 0) continue;
        const double readings[4] = {0.2, 0.86, 0.85, 0.75};
        for (int i = 0; i < 4; ++i) model.config.root.set(kLidFitKeys[eye][i], JsonValue::makeNumber(readings[i]));
    }
    model.status.running = running;
    model.status.camera.known = true;
    model.status.camera.used[0] = left;
    model.status.camera.used[1] = right;
    model.status.opennessSaturated = saturated;
    return widenSlider(SettingsView(model), model.status);
}

/** The Eyelids tab's one widening slider: the cameras, one camera and lid_widen, lid_widen, or nothing. */
void testWidenSlider() {
    // Both eyes on the cameras: their sensitivity, whatever the cap or the fit says
    for (const bool saturated : {false, true}) {
        for (int fitted = 0; fitted < 4; ++fitted) {
            const WidenSlider s = sliderOf(true, true, saturated, fitted);
            CHECK(s.control == WidenControl::Camera && !s.unfittedNote);
        }
    }
    // One eye: both (the cameras, and lid_widen for the other one), also on a capped SteamOS
    for (const bool saturated : {false, true}) {
        CHECK(sliderOf(true, false, saturated, 3).control == WidenControl::Mixed);
        CHECK(sliderOf(false, true, saturated, 3).control == WidenControl::Mixed);
    }
    // ...the note only when the Valve eye has no fit (the camera eye's fit doesn't matter)
    CHECK(!sliderOf(true, false, false, 2).unfittedNote);
    CHECK(sliderOf(true, false, false, 1).unfittedNote);
    CHECK(!sliderOf(false, true, false, 1).unfittedNote);
    CHECK(sliderOf(false, true, false, 2).unfittedNote);
    // Valve's values: lid_widen; the note while an eye has no fit
    CHECK(sliderOf(false, false, false, 3).control == WidenControl::Valve);
    CHECK(!sliderOf(false, false, false, 3).unfittedNote);
    for (const int fitted : {0, 1, 2}) {
        const WidenSlider s = sliderOf(false, false, false, fitted);
        CHECK(s.control == WidenControl::Valve && s.unfittedNote);
    }
    // A capped SteamOS without a camera: nothing (greyed), with or without a fit
    for (int fitted = 0; fitted < 4; ++fitted) {
        const WidenSlider s = sliderOf(false, false, true, fitted);
        CHECK(s.control == WidenControl::Saturated && !s.unfittedNote);
    }
    // frameeyeosc not running: nothing it reports counts (Valve's values, lid_widen)
    CHECK(sliderOf(true, true, true, 3, false).control == WidenControl::Valve);
    // The four stops and the level nearest a position
    for (int level = 0; level < 4; ++level) CHECK(widenLevelAt(widenStop(level)) == level);
    CHECK(widenStop(0) == 0.0 && widenStop(3) == 1.0);
    CHECK(widenLevelAt(0.0) == 0 && widenLevelAt(0.16) == 0 && widenLevelAt(0.17) == 1 && widenLevelAt(0.5) == 2);
    CHECK(widenLevelAt(0.83) == 2 && widenLevelAt(0.84) == 3 && widenLevelAt(1.0) == 3);
    CHECK(widenLevelAt(-1.0) == 0 && widenLevelAt(2.0) == 3 && widenLevelAt(NAN) == 2);
    // (the cameras' default 0.5 gives lid_widen's default, normal)
    CHECK(std::string(kLidWidenModes[widenLevelAt(0.5)]) == findSetting(key::kLidWiden)->defaultText);
}

/** Eyelid smoothing presets: medium is the defaults; values at none read as custom. */
void testLidPresets() {
    const LidPreset* presets = lidPresets();
    CHECK(findSetting(key::kLidMinCutoff)->defaultNumber == presets[1].minCutoff);
    CHECK(findSetting(key::kLidBeta)->defaultNumber == presets[1].beta);
    for (int i = 0; i < 2; ++i) {
        CHECK(presets[i].minCutoff > presets[i + 1].minCutoff && presets[i].beta > presets[i + 1].beta);
    }
    PanelModel model;
    model.config.exists = true;
    model.config.root.type = JsonValue::Type::Object;
    CHECK(matchingLidPreset(SettingsView(model)) == 1);
    for (int i = 0; i < 3; ++i) {
        model.config.root.set(key::kLidMinCutoff, JsonValue::makeNumber(presets[i].minCutoff));
        model.config.root.set(key::kLidBeta, JsonValue::makeNumber(presets[i].beta));
        CHECK(matchingLidPreset(SettingsView(model)) == i);
        // (each value one the steppers can reach)
        for (const char* name : {key::kLidMinCutoff, key::kLidBeta}) {
            const SettingSpec* spec = findSetting(name);
            const double value = numberIn(model.config.root, name);
            CHECK(value >= spec->min && value <= spec->max && snapValue(*spec, value) == value);
        }
    }
    model.config.root.set(key::kLidBeta, JsonValue::makeNumber(4.5));
    CHECK(matchingLidPreset(SettingsView(model)) == -1);
    // A slider's value onto the step grid, within the range
    const SettingSpec* sync = findSetting(key::kLidSync);
    CHECK(snapValue(*sync, 0.512) == 0.5 && snapValue(*sync, 0.53) == 0.55);
    CHECK(snapValue(*sync, -0.3) == 0.0 && snapValue(*sync, 1.7) == 1.0);
}

/** lid_open_snap as a share of the open reading (0.70-1.00) goes over to the sent eyelid, once. */
void testMigrateLidOpenSnap() {
    const auto withSnap = [](double snap) {
        JsonValue root;
        root.type = JsonValue::Type::Object;
        root.set(key::kLidWiden, JsonValue::makeString("normal"));
        root.set(key::kVersion, JsonValue::makeNumber(kConfigVersion, true));
        root.set(key::kLidOpenSnap, JsonValue::makeNumber(snap));
        return root;
    };
    const double cases[][2] = {{0.8, 0.53}, {0.85, 0.59}, {0.9, 0.64}, {0.95, 0.70}, {1.0, 0.75}};
    for (const auto& c : cases) {
        JsonValue root = withSnap(c[0]);
        std::string log;
        CHECK(configNeedsMigration(root));
        CHECK(migrateLidOpenSnap(root, log));
        CHECK(std::fabs(numberIn(root, key::kLidOpenSnap) - c[1]) < 1e-9);
        CHECK(log.find("lid_open_snap") != std::string::npos);
        // Once only
        CHECK(!configNeedsMigration(root) && !migrateLidOpenSnap(root, log));
    }
    // The new kind (0.70 and 0.75 included), none, and values never allowed: left alone
    for (const double snap : {0.0, 0.53, 0.7, 0.75, 1.2}) {
        JsonValue root = withSnap(snap);
        std::string log;
        CHECK(!configNeedsMigration(root) && !migrateLidOpenSnap(root, log));
        CHECK(numberIn(root, key::kLidOpenSnap) == snap);
    }
    // The default is the new kind's
    CHECK(std::fabs(findSetting(key::kLidOpenSnap)->defaultNumber - 0.53) < 1e-9);
    CHECK(findSetting(key::kLidOpenSnap)->max == 0.75 && findSetting(key::kLidOpenSnap)->step == 0.01);
}

/** A 0.5.x config: scales next to a lid fit go, once. */
void testMigrateLidScales() {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    for (const char* name : kLidFitKeys[0]) root.set(name, JsonValue::makeNumber(0.5));
    root.set(key::kLidScaleLeft, JsonValue::makeNumber(1.15));
    root.set(key::kLidScaleRight, JsonValue::makeNumber(0.9));  // the right eye is not fitted: kept
    std::string log;
    CHECK(migrateLidScales(root, log));
    CHECK(root.get(key::kLidScaleLeft) != nullptr && root.get(key::kLidScaleLeft)->isNull());
    CHECK(std::fabs(numberIn(root, key::kLidScaleRight) - 0.9) < 1e-9);
    CHECK(root.get(key::kLidWiden) != nullptr && root.get(key::kLidWiden)->text == "normal");
    CHECK(log.find("lid_scale_left 1.15 -> null") != std::string::npos);
    // Once only: a scale set afterwards stays
    root.set(key::kLidScaleLeft, JsonValue::makeNumber(0.95));
    CHECK(!migrateLidScales(root, log) && std::fabs(numberIn(root, key::kLidScaleLeft) - 0.95) < 1e-9);
    // Nothing to clear: only lid_widen is added (so later scales are not touched either)
    JsonValue plain;
    plain.type = JsonValue::Type::Object;
    plain.set(key::kLidScaleLeft, JsonValue::makeNumber(1.1));
    CHECK(migrateLidScales(plain, log) && log.empty() && std::fabs(numberIn(plain, key::kLidScaleLeft) - 1.1) < 1e-9);
    CHECK(plain.get(key::kLidWiden) != nullptr);
}

/**
 * A config root with these gaze values (none written for a negative one) and this version (none for 0).
 * @param minCutoff gaze_min_cutoff
 * @param beta gaze_beta
 * @param dCutoff gaze_d_cutoff
 * @param deadzone gaze_deadzone
 * @param version version
 * @return the root object
 */
JsonValue gazeRoot(double minCutoff, double beta, double dCutoff, double deadzone, int version) {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    root.set(key::kLidWiden, JsonValue::makeString("high"));
    if (version > 0) root.set(key::kVersion, JsonValue::makeNumber(version, true));
    const char* keys[4] = {key::kGazeMinCutoff, key::kGazeBeta, key::kGazeDCutoff, key::kGazeDeadzone};
    const double values[4] = {minCutoff, beta, dCutoff, deadzone};
    for (int i = 0; i < 4; ++i) {
        if (values[i] >= 0) root.set(keys[i], JsonValue::makeNumber(values[i]));
    }
    return root;
}

/**
 * Which preset a config root's gaze values match, as the panel shows it.
 * @param root the root object
 * @return 0..2, or -1
 */
int presetOf(const JsonValue& root) {
    PanelModel model;
    model.config.exists = true;
    model.config.root = root;
    return matchingGazePreset(SettingsView(model));
}

/** Medium is frameeyeosc's default, and from light to strong each preset smooths more at rest. */
void testGazePresets() {
    const GazePreset* presets = gazePresets();
    CHECK(findSetting(key::kGazeMinCutoff)->defaultNumber == presets[1].minCutoff);
    CHECK(findSetting(key::kGazeBeta)->defaultNumber == presets[1].beta);
    CHECK(findSetting(key::kGazeDCutoff)->defaultNumber == presets[1].dCutoff);
    CHECK(findSetting(key::kGazeDeadzone)->defaultNumber == 0.005);
    CHECK(findSetting(key::kVersion)->defaultNumber == kConfigVersion);
    for (int i = 0; i < 2; ++i) {
        CHECK(presets[i].minCutoff > presets[i + 1].minCutoff && presets[i].beta > presets[i + 1].beta);
        CHECK(presets[i].dCutoff >= presets[i + 1].dCutoff);
    }
}

/** Gaze values exactly at an old preset become the new preset of the same name, once; anything else stays. */
void testMigrateGazePresets() {
    const double old[3][3] = {{1.0, 1.5, 1.0}, {0.4, 0.8, 0.5}, {0.2, 0.4, 0.3}};
    std::string log;
    for (int i = 0; i < 3; ++i) {
        JsonValue root = gazeRoot(old[i][0], old[i][1], old[i][2], 0.02, 1);
        CHECK(presetOf(root) == -1 && configNeedsMigration(root));
        CHECK(migrateGazePresets(root, log));
        CHECK(presetOf(root) == i);
        CHECK(std::fabs(numberIn(root, key::kGazeDeadzone) - 0.005) < 1e-9);
        CHECK(numberIn(root, key::kVersion) == kGazePresetsVersion && root.get(key::kVersion)->integer);
        CHECK(log.find(" -> ") != std::string::npos && log.find("gaze_deadzone 0.02 -> 0.005") != std::string::npos);
        std::string eyeLog;
        CHECK(configNeedsMigration(root) && migrateEyeOffsets(root, eyeLog) && configNeedsMigration(root));
        CHECK(migrateFitPolicy(root, eyeLog) && !configNeedsMigration(root));
        // Once only: the old values written again afterwards (by hand) stay
        root = gazeRoot(old[i][0], old[i][1], old[i][2], 0.02, kConfigVersion);
        CHECK(!migrateGazePresets(root, log) && numberIn(root, key::kGazeBeta) == old[i][1]);
    }
    // No version at all counts as 1
    JsonValue unversioned = gazeRoot(0.4, 0.8, 0.5, 0.02, 0);
    CHECK(configNeedsMigration(unversioned) && migrateGazePresets(unversioned, log) && presetOf(unversioned) == 1);
    // Own values: kept, and so is their deadzone; only the version is written
    JsonValue own = gazeRoot(0.2, 0.4, 0.35, 0.02, 1);
    CHECK(migrateGazePresets(own, log) && log.empty());
    CHECK(numberIn(own, key::kGazeDCutoff) == 0.35 && numberIn(own, key::kGazeDeadzone) == 0.02);
    CHECK(numberIn(own, key::kVersion) == kGazePresetsVersion);
    // A preset with a deadzone of one's own: the preset moves, the deadzone stays
    JsonValue tuned = gazeRoot(0.2, 0.4, 0.3, 0.015, 1);
    CHECK(migrateGazePresets(tuned, log) && presetOf(tuned) == 2 && numberIn(tuned, key::kGazeDeadzone) == 0.015);
    CHECK(log.find("deadzone") == std::string::npos);
    // Values not written: frameeyeosc's defaults (the new medium) apply, nothing to match
    JsonValue empty = gazeRoot(-1, -1, -1, -1, 0);
    CHECK(migrateGazePresets(empty, log) && log.empty() && empty.get(key::kGazeMinCutoff) == nullptr);
    // Needed for a version 1 file, and for one without lid_widen (from before 0.6.0) whatever its version
    CHECK(configNeedsMigration(gazeRoot(0.3, 1.5, 0.5, 0.005, 1)));
    CHECK(!configNeedsMigration(gazeRoot(0.3, 1.5, 0.5, 0.005, kConfigVersion)));
    JsonValue noWiden;
    noWiden.type = JsonValue::Type::Object;
    noWiden.set(key::kVersion, JsonValue::makeNumber(kConfigVersion, true));
    CHECK(configNeedsMigration(noWiden));
}

/** Each eye's own sideways zero point from before becomes one for both, once; the gains stay. */
void testMigrateEyeOffsets() {
    const auto withEyes = [](double version, double left, double right) {
        JsonValue root;
        root.type = JsonValue::Type::Object;
        root.set(key::kLidWiden, JsonValue::makeString("normal"));
        if (version > 0) root.set(key::kVersion, JsonValue::makeNumber(version, true));
        root.set(key::kGazeOffsetX, JsonValue::makeNumber(0.077));
        root.set(key::kGazeOffsetXLeft, JsonValue::makeNumber(left));
        root.set(key::kGazeOffsetXRight, JsonValue::makeNumber(right));
        root.set(key::kGazeGainXLeft, JsonValue::makeNumber(1.03));
        root.set(key::kGazeGainXRight, JsonValue::makeNumber(0.93));
        return root;
    };
    std::string log;
    // The author's config at 22:47 on 2026-10-08, and the one a minute after the 17° capture
    JsonValue root = withEyes(2, 0.078, 0.082);
    CHECK(configNeedsMigration(root) && migrateEyeOffsets(root, log));
    CHECK(std::fabs(numberIn(root, key::kGazeOffsetXLeft) - 0.08) < 1e-9);
    CHECK(std::fabs(numberIn(root, key::kGazeOffsetXRight) - 0.08) < 1e-9);
    CHECK(numberIn(root, key::kGazeGainXLeft) == 1.03 && numberIn(root, key::kGazeGainXRight) == 0.93);
    CHECK(numberIn(root, key::kGazeOffsetX) == 0.077);
    CHECK(numberIn(root, key::kVersion) == kEyeOffsetsVersion && root.get(key::kVersion)->integer);
    CHECK(log.find("+0.078 / +0.082 -> +0.080") != std::string::npos);
    // ...and then the fit policy (version 4)
    CHECK(configNeedsMigration(root) && !migrateEyeOffsets(root, log));
    JsonValue apart = withEyes(2, 0.061, -0.223);
    CHECK(migrateEyeOffsets(apart, log) && std::fabs(numberIn(apart, key::kGazeOffsetXLeft) + 0.081) < 1e-9);
    // Once only: zero points set apart by hand afterwards stay
    JsonValue later = withEyes(kEyeOffsetsVersion, 0.01, 0.03);
    CHECK(!migrateEyeOffsets(later, log));
    CHECK(numberIn(later, key::kGazeOffsetXLeft) == 0.01 && numberIn(later, key::kGazeOffsetXRight) == 0.03);
    // Already the same, or never fitted per eye: only the version is written
    JsonValue same = withEyes(2, 0.05, 0.05);
    CHECK(migrateEyeOffsets(same, log) && log.empty() && numberIn(same, key::kGazeOffsetXLeft) == 0.05);
    JsonValue plain;
    plain.type = JsonValue::Type::Object;
    plain.set(key::kVersion, JsonValue::makeNumber(2, true));
    CHECK(migrateEyeOffsets(plain, log) && log.empty() && plain.get(key::kGazeOffsetXLeft) == nullptr);
    CHECK(numberIn(plain, key::kVersion) == kEyeOffsetsVersion);
    // A version 1 file goes through the gaze presets first, then this
    JsonValue old = withEyes(1, 0.0, 0.02);
    std::string gazeLog;
    CHECK(migrateGazePresets(old, gazeLog) && numberIn(old, key::kVersion) == kGazePresetsVersion);
    CHECK(migrateEyeOffsets(old, log) && std::fabs(numberIn(old, key::kGazeOffsetXRight) - 0.01) < 1e-9);
}

/** An eye fit from before version 4 keeps only the zero point and the up gain, once, whatever eye_behavior is. */
void testMigrateFitPolicy() {
    const auto fitted = [](double version, int behavior) {
        JsonValue root;
        root.type = JsonValue::Type::Object;
        root.set(key::kLidWiden, JsonValue::makeString("normal"));
        root.set(key::kVersion, JsonValue::makeNumber(version, true));
        if (behavior > 0) root.set(key::kEyeBehavior, JsonValue::makeNumber(behavior, true));
        root.set(key::kGazeOffsetX, JsonValue::makeNumber(0.012));
        root.set(key::kGazeOffsetY, JsonValue::makeNumber(-0.02));
        root.set(key::kGazeGainX, JsonValue::makeNumber(0.93));
        root.set(key::kGazeGainUp, JsonValue::makeNumber(0.9));
        root.set(key::kGazeGainDown, JsonValue::makeNumber(0.88));
        root.set(key::kGazeRollDeg, JsonValue::makeNumber(6.7));
        root.set(key::kGazeOffsetXLeft, JsonValue::makeNumber(0.012));
        root.set(key::kGazeOffsetXRight, JsonValue::makeNumber(0.012));
        root.set(key::kGazeGainXLeft, JsonValue::makeNumber(0.95));
        root.set(key::kGazeGainXRight, JsonValue::makeNumber(0.9));
        root.set(key::kAutoRecenter, JsonValue::makeString("tilt"));
        root.set(key::kGazeScaleUp, JsonValue::makeNumber(1.2));
        return root;
    };
    for (const int behavior : {0, 1, 2}) {
        JsonValue root = fitted(kEyeOffsetsVersion, behavior);
        std::string log;
        CHECK(configNeedsMigration(root) && migrateFitPolicy(root, log));
        CHECK(numberIn(root, key::kGazeRollDeg) == 0.0 && numberIn(root, key::kGazeGainX) == 1.0);
        CHECK(numberIn(root, key::kGazeGainDown) == 1.0);
        // Kept: the zero point, the up gain, and anything not the fit's (the gaze scale, eye_behavior)
        CHECK(numberIn(root, key::kGazeOffsetX) == 0.012 && numberIn(root, key::kGazeOffsetY) == -0.02);
        CHECK(numberIn(root, key::kGazeGainUp) == 0.9 && numberIn(root, key::kGazeScaleUp) == 1.2);
        CHECK(behavior == 0 || numberIn(root, key::kEyeBehavior) == behavior);
        for (const char* name :
             {key::kGazeOffsetXLeft, key::kGazeOffsetXRight, key::kGazeGainXLeft, key::kGazeGainXRight}) {
            CHECK(root.get(name) != nullptr && root.get(name)->isNull());
        }
        CHECK(root.get(key::kAutoRecenter)->text == "center");
        CHECK(numberIn(root, key::kVersion) == kConfigVersion && root.get(key::kVersion)->integer);
        CHECK(log == "gaze_roll_deg 6.7 -> 0, gaze_gain_x 0.93 -> 1, gaze_gain_down 0.88 -> 1, "
                     "gaze_offset_x_left 0.012 -> null, gaze_offset_x_right 0.012 -> null, "
                     "gaze_gain_x_left 0.95 -> null, gaze_gain_x_right 0.9 -> null, "
                     "auto_recenter \"tilt\" -> \"center\"");
        // Once only: a tilt or gains set by hand afterwards stay
        CHECK(!configNeedsMigration(root));
        root.set(key::kGazeRollDeg, JsonValue::makeNumber(2.0));
        CHECK(!migrateFitPolicy(root, log) && numberIn(root, key::kGazeRollDeg) == 2.0);
    }
    // Nothing of the old fit set: only the version
    JsonValue plain;
    plain.type = JsonValue::Type::Object;
    plain.set(key::kVersion, JsonValue::makeNumber(kEyeOffsetsVersion, true));
    plain.set(key::kAutoRecenter, JsonValue::makeString("off"));
    std::string log;
    CHECK(migrateFitPolicy(plain, log) && log.empty() && numberIn(plain, key::kVersion) == kConfigVersion);
    CHECK(plain.get(key::kGazeRollDeg) == nullptr && plain.get(key::kAutoRecenter)->text == "off");
    // A file of the current version is left alone
    JsonValue current = fitted(kConfigVersion, 2);
    CHECK(!migrateFitPolicy(current, log) && numberIn(current, key::kGazeRollDeg) == 6.7);
    // A version 2 file goes through each step in turn
    JsonValue old = fitted(2, 1);
    old.set(key::kGazeOffsetXRight, JsonValue::makeNumber(0.02));
    CHECK(migrateEyeOffsets(old, log) && migrateFitPolicy(old, log) && !configNeedsMigration(old));
    CHECK(old.get(key::kGazeOffsetXRight)->isNull() && numberIn(old, key::kGazeGainUp) == 0.9);
}

/** The destination cards' button arguments stand for the output types both ways. */
void testOutputArgs() {
    for (int arg = 0; arg < 4; ++arg) CHECK(argOfOutput(outputOfArg(arg)) == arg);
    CHECK(std::string(outputOfArg(2)) == kOutputLivelink);
    CHECK(std::string(outputOfArg(3)) == kOutputModule);
    CHECK(argOfOutput("osc") == -1);
    // ETVR is going away: the Basic tab warns while it is the output (it still works)
    CHECK(outputDeprecated(kOutputEtvr));
    for (const char* output : {kOutputVrchat, kOutputLivelink, kOutputModule, ""}) CHECK(!outputDeprecated(output));
    // Its own default port
    PanelModel m;
    m.config.root.type = JsonValue::Type::Object;
    m.config.root.set(key::kOutput, JsonValue::makeString(kOutputModule));
    CHECK(SettingsView(m).port() == 11112 && SettingsView(m).portIsDefault());
    // The same recommended settings as LiveLink (neither module smooths)
    const std::vector<SettingChange> module = recommendedSettings(kOutputModule, SettingsView(m));
    const std::vector<SettingChange> livelink = recommendedSettings(kOutputLivelink, SettingsView(m));
    CHECK(module.size() == livelink.size());
    for (size_t i = 0; i < module.size() && i < livelink.size(); ++i) {
        CHECK(std::string(module[i].key) == livelink[i].key && module[i].value.number == livelink[i].value.number);
    }
}

/** status.json says why frameeyeosc can't read the eye tracker in source_error; null or missing means it can. */
void testSourceError() {
    const std::string reason = "unsupported eye shared-memory version 6; supported: 4, 5";
    EyeStatus status = parseStatus("{\"pid\": 1, \"tracking\": false, \"source_error\": \"" + reason + "\"}", 0, false);
    CHECK(status.present && !status.tracking && status.sourceError == reason && status.configError.empty());
    status = parseStatus("{\"pid\": 1, \"tracking\": true, \"source_error\": null}", 0, false);
    CHECK(status.present && status.tracking && status.sourceError.empty());
    // From a frameeyeosc older than the field
    CHECK(parseStatus("{\"pid\": 1}", 0, false).sourceError.empty());
}

/** status.json's dominant_eye ("left" / "right", else nothing) and openness_saturated (missing = false). */
void testDominantEyeAndSaturation() {
    EyeStatus status = parseStatus("{\"pid\": 1, \"dominant_eye\": \"right\", \"openness_saturated\": true}", 0, false);
    CHECK(status.dominantEye == "right" && status.opennessSaturated);
    status = parseStatus("{\"pid\": 1, \"dominant_eye\": \"left\", \"openness_saturated\": false}", 0, false);
    CHECK(status.dominantEye == "left" && !status.opennessSaturated);
    status = parseStatus("{\"pid\": 1, \"dominant_eye\": null}", 0, false);
    CHECK(status.dominantEye.empty() && !status.opennessSaturated);
    // From frameeyeosc before 0.7.0, or a value this doesn't know
    CHECK(parseStatus("{\"pid\": 1}", 0, false).dominantEye.empty());
    CHECK(parseStatus("{\"pid\": 1, \"dominant_eye\": \"both\"}", 0, false).dominantEye.empty());
}

/** status.json's missed_rate, max_processing_ms and dropped_rate, and who was slow while the eye data rate is low. */
void testTrackerRateCause() {
    const auto status = [](const std::string& fields) {
        EyeStatus s = parseStatus("{\"pid\": 1, \"time\": 0, \"tracking\": true, " + fields + "}", 0, false);
        return s;
    };
    EyeStatus s = status("\"tracker_rate\": 46, \"missed_rate\": 44, \"max_processing_ms\": 15.2, \"dropped_rate\": 3");
    CHECK(s.running && s.trackerRate == 46 && s.missedRate == 44 && s.maxProcessingMs == 15.2 && s.droppedRate == 3);
    // Missed samples that would have made the rate high enough: frameeyeosc was slow
    CHECK(trackerRateCause(s) == TrackerRateCause::Here);
    // So is one sample taking longer than a rate of 60 a second leaves for it
    s = status("\"tracker_rate\": 46, \"missed_rate\": 0, \"max_processing_ms\": 17, \"dropped_rate\": 0");
    CHECK(trackerRateCause(s) == TrackerRateCause::Here);
    // Nothing missed and quick: the eye tracker itself delivered few
    s = status("\"tracker_rate\": 15, \"missed_rate\": 0, \"max_processing_ms\": 1.4, \"dropped_rate\": 120");
    CHECK(trackerRateCause(s) == TrackerRateCause::Tracker);
    s = status("\"tracker_rate\": 46, \"missed_rate\": 3, \"max_processing_ms\": 12, \"dropped_rate\": 0");
    CHECK(trackerRateCause(s) == TrackerRateCause::Tracker);
    // Not low, not tracking, or not running: nothing to say
    s = status("\"tracker_rate\": 89, \"missed_rate\": 0, \"max_processing_ms\": 30, \"dropped_rate\": 0");
    CHECK(trackerRateCause(s) == TrackerRateCause::None);
    s.trackerRate = 15;
    s.tracking = false;
    CHECK(trackerRateCause(s) == TrackerRateCause::None);
    s.tracking = true;
    s.running = false;
    CHECK(trackerRateCause(s) == TrackerRateCause::None);
    // From a frameeyeosc before these numbers (or before a second of tracking): can't be told
    s = status("\"tracker_rate\": 15");
    CHECK(std::isnan(s.missedRate) && std::isnan(s.maxProcessingMs) && std::isnan(s.droppedRate));
    CHECK(trackerRateCause(s) == TrackerRateCause::None);
    s = status("\"tracker_rate\": 15, \"missed_rate\": null, \"max_processing_ms\": null, \"dropped_rate\": 0");
    CHECK(trackerRateCause(s) == TrackerRateCause::None && s.droppedRate == 0);
}

/** steamlink_params (the Output tab's "Steam Link names" toggle): off by default, read from config.json, and
 *  frameeyeosc's own value while --steamlink-params locks it. */
void testSteamlinkParams() {
    const SettingSpec* spec = findSetting(key::kSteamlinkParams);
    CHECK(spec != nullptr && spec->type == SettingType::Bool && spec->defaultNumber == 0);
    PanelModel m;
    m.config.exists = true;
    m.config.root.type = JsonValue::Type::Object;
    CHECK(!SettingsView(m).flag(key::kSteamlinkParams));
    // A new config.json has it, off
    CHECK(spec != nullptr && defaultValue(*spec).isBool() && !defaultValue(*spec).boolean);
    m.config.root.set(key::kSteamlinkParams, JsonValue::makeBool(true));
    CHECK(SettingsView(m).flag(key::kSteamlinkParams) && !SettingsView(m).locked(key::kSteamlinkParams));
    // Not a bool: the default
    m.config.root.set(key::kSteamlinkParams, JsonValue::makeString("yes"));
    CHECK(!SettingsView(m).flag(key::kSteamlinkParams));
    // Locked by frameeyeosc's command line: its value, whatever the file says
    m.config.root.set(key::kSteamlinkParams, JsonValue::makeBool(false));
    m.status = parseStatus(
        "{\"pid\": 1, \"locked\": [\"steamlink_params\"], \"effective\": {\"steamlink_params\": true}}", 0, false);
    CHECK(SettingsView(m).locked(key::kSteamlinkParams) && SettingsView(m).flag(key::kSteamlinkParams));
}

/** native_eyes (the Output tab's "VRChat's own eye tracking" toggle): off by default, read from config.json, and
 *  frameeyeosc's own value while --native-eyes locks it. */
void testNativeEyes() {
    const SettingSpec* spec = findSetting(key::kNativeEyes);
    CHECK(spec != nullptr && spec->type == SettingType::Bool && spec->defaultNumber == 0);
    PanelModel m;
    m.config.exists = true;
    m.config.root.type = JsonValue::Type::Object;
    CHECK(!SettingsView(m).flag(key::kNativeEyes));
    // A new config.json has it, off (so "Reset all" clears it too)
    CHECK(spec != nullptr && defaultValue(*spec).isBool() && !defaultValue(*spec).boolean);
    m.config.root.set(key::kNativeEyes, JsonValue::makeBool(true));
    CHECK(SettingsView(m).flag(key::kNativeEyes) && !SettingsView(m).locked(key::kNativeEyes));
    // Not a bool: the default
    m.config.root.set(key::kNativeEyes, JsonValue::makeString("yes"));
    CHECK(!SettingsView(m).flag(key::kNativeEyes));
    // Locked by frameeyeosc's command line: its value, whatever the file says
    m.config.root.set(key::kNativeEyes, JsonValue::makeBool(false));
    m.status = parseStatus(
        "{\"pid\": 1, \"locked\": [\"native_eyes\"], \"effective\": {\"native_eyes\": true}}", 0, false);
    CHECK(SettingsView(m).locked(key::kNativeEyes) && SettingsView(m).flag(key::kNativeEyes));
}

/** camera_lids (the eye capture tab's "Eyelids from the eye cameras"): on by default, read from config.json, and
 *  frameeyeosc's own value while its command line locks it. */
void testCameraLids() {
    const SettingSpec* spec = findSetting(key::kCameraLids);
    CHECK(spec != nullptr && spec->type == SettingType::Bool && spec->defaultNumber == 1);
    PanelModel m;
    m.config.root = JsonValue();
    m.config.root.type = JsonValue::Type::Object;
    CHECK(SettingsView(m).flag(key::kCameraLids));
    m.config.root.set(key::kCameraLids, JsonValue::makeBool(false));
    CHECK(!SettingsView(m).flag(key::kCameraLids) && !SettingsView(m).locked(key::kCameraLids));
    // Not a boolean: the default
    m.config.root.set(key::kCameraLids, JsonValue::makeString("no"));
    CHECK(SettingsView(m).flag(key::kCameraLids));
    m.config.root.set(key::kCameraLids, JsonValue::makeBool(true));
    m.status = parseStatus(
        "{\"pid\": 1, \"locked\": [\"camera_lids\"], \"effective\": {\"camera_lids\": false}}", 0, false);
    CHECK(SettingsView(m).locked(key::kCameraLids) && !SettingsView(m).flag(key::kCameraLids));
}

/** widen (the Eyelids tab's "Widening"): on by default, read from config.json and written back as a bool, and
 *  frameeyeosc's own value while its command line (--no-widen) locks it. */
void testWiden() {
    const SettingSpec* spec = findSetting(key::kWiden);
    CHECK(spec != nullptr && spec->type == SettingType::Bool && spec->defaultNumber == 1);
    CHECK(spec != nullptr && defaultValue(*spec).isBool() && defaultValue(*spec).boolean);
    PanelModel m;
    m.config.root = JsonValue();
    m.config.root.type = JsonValue::Type::Object;
    CHECK(SettingsView(m).flag(key::kWiden));
    m.config.root.set(key::kWiden, JsonValue::makeBool(false));
    CHECK(!SettingsView(m).flag(key::kWiden) && !SettingsView(m).locked(key::kWiden));
    // Through the file and back: "widen": false, as frameeyeosc reads it
    const std::string text = writeJson(m.config.root);
    CHECK(text.find("\"widen\": false") != std::string::npos);
    JsonValue back;
    std::string error;
    CHECK(parseJson(text, back, error) && back.get(key::kWiden) != nullptr && back.get(key::kWiden)->isBool() &&
          !back.get(key::kWiden)->boolean);
    // lid_widen is a setting of its own
    CHECK(SettingsView(m).text(key::kLidWiden) == "normal");
    // Not a boolean: the default
    m.config.root.set(key::kWiden, JsonValue::makeString("no"));
    CHECK(SettingsView(m).flag(key::kWiden));
    m.status = parseStatus("{\"pid\": 1, \"locked\": [\"widen\"], \"effective\": {\"widen\": false}}", 0, false);
    CHECK(SettingsView(m).locked(key::kWiden) && !SettingsView(m).flag(key::kWiden));
}

/** brow_from_widen and brow_strength (the Eyelids tab's "Widen raises brows" and "Brow raise strength"): off
 *  and 0.8 by default, read from config.json and written back as frameeyeosc reads them, on the slider's 0.05 grid, and
 *  frameeyeosc's own values while its command line (--brow-from-widen, --brow-strength) locks them. */
void testBrowSettings() {
    const SettingSpec* on = findSetting(key::kBrowFromWiden);
    CHECK(on != nullptr && on->type == SettingType::Bool && on->defaultNumber == 0);
    CHECK(on != nullptr && defaultValue(*on).isBool() && !defaultValue(*on).boolean);
    const SettingSpec* strength = findSetting(key::kBrowStrength);
    CHECK(strength != nullptr && strength->type == SettingType::Number && strength->defaultNumber == 0.8);
    CHECK(strength != nullptr && strength->min == 0.0 && strength->max == 1.0 && strength->step == 0.05);
    if (strength != nullptr) {
        CHECK(snapValue(*strength, 0.63) == 0.65 && snapValue(*strength, 1.4) == 1.0 && snapValue(*strength, -1) == 0.0);
    }
    CHECK(formatSetting(key::kBrowStrength, 0.8) == "0.80");
    PanelModel m;
    m.config.root = JsonValue();
    m.config.root.type = JsonValue::Type::Object;
    CHECK(!SettingsView(m).flag(key::kBrowFromWiden));
    CHECK(std::fabs(SettingsView(m).number(key::kBrowStrength) - 0.8) < 1e-12);
    m.config.root.set(key::kBrowFromWiden, JsonValue::makeBool(true));
    m.config.root.set(key::kBrowStrength, JsonValue::makeNumber(0.55));
    CHECK(SettingsView(m).flag(key::kBrowFromWiden) && !SettingsView(m).locked(key::kBrowFromWiden));
    // Through the file and back: "brow_from_widen": true, "brow_strength": 0.55, as frameeyeosc reads them
    const std::string text = writeJson(m.config.root);
    CHECK(text.find("\"brow_from_widen\": true") != std::string::npos);
    CHECK(text.find("\"brow_strength\": 0.55") != std::string::npos);
    JsonValue back;
    std::string error;
    CHECK(parseJson(text, back, error));
    const JsonValue* flag = back.get(key::kBrowFromWiden);
    const JsonValue* value = back.get(key::kBrowStrength);
    CHECK(flag != nullptr && flag->isBool() && flag->boolean);
    CHECK(value != nullptr && value->isNumber() && std::fabs(value->number - 0.55) < 1e-12);
    // widen is a setting of its own
    CHECK(SettingsView(m).flag(key::kWiden));
    // Not a boolean: the default
    m.config.root.set(key::kBrowFromWiden, JsonValue::makeString("yes"));
    CHECK(!SettingsView(m).flag(key::kBrowFromWiden));
    m.status = parseStatus(
        "{\"pid\": 1, \"locked\": [\"brow_from_widen\", \"brow_strength\"], "
        "\"effective\": {\"brow_from_widen\": true, \"brow_strength\": 0.3}}",
        0, false);
    CHECK(SettingsView(m).locked(key::kBrowFromWiden) && SettingsView(m).flag(key::kBrowFromWiden));
    CHECK(SettingsView(m).locked(key::kBrowStrength) && std::fabs(SettingsView(m).number(key::kBrowStrength) - 0.3) < 1e-12);
}

/** gaze_debug_rings (the rings by the debug gaze dots, Advanced > Debug tools): the panel's own, like the dots'
 *  distance; on by default and read from config.json, off only when the file says false. */
void testGazeDebugRings() {
    const SettingSpec* spec = findSetting(key::kGazeDebugRings);
    CHECK(spec != nullptr && spec->type == SettingType::Bool && spec->defaultNumber == 1);
    CHECK(std::string(key::kGazeDebugRings) == "gaze_debug_rings");
    PanelModel m;
    m.config.root = JsonValue();
    m.config.root.type = JsonValue::Type::Object;
    CHECK(SettingsView(m).flag(key::kGazeDebugRings) && m.config.flag(key::kGazeDebugRings));
    m.config.root.set(key::kGazeDebugRings, JsonValue::makeBool(false));
    CHECK(!SettingsView(m).flag(key::kGazeDebugRings) && !m.config.flag(key::kGazeDebugRings));
    CHECK(!SettingsView(m).locked(key::kGazeDebugRings));
    // Not a boolean: the default
    m.config.root.set(key::kGazeDebugRings, JsonValue::makeString("no"));
    CHECK(SettingsView(m).flag(key::kGazeDebugRings));
    // Independent of the dots' own switch
    m.config.root.set(key::kGazeDebugRings, JsonValue::makeBool(false));
    m.config.root.set(key::kGazeDebugDots, JsonValue::makeBool(true));
    CHECK(SettingsView(m).flag(key::kGazeDebugDots) && !SettingsView(m).flag(key::kGazeDebugRings));
}

/** frameeyeosc's "camera" and the camera values sent, and what the eye capture tab says about them. */
/** pupils_to_vrchat (the Output tab's "Send pupils straight to VRChat"): on by default, read from config.json and
 *  written back, frameeyeosc's own value while --no-pupils-to-vrchat locks it, and shown only for LiveLink with the eye
 *  cameras. */
void testPupilsToVrchat() {
    const SettingSpec* spec = findSetting(key::kPupilsToVrchat);
    CHECK(spec != nullptr && spec->type == SettingType::Bool && spec->defaultNumber == 1);
    CHECK(defaultValue(*spec).isBool() && defaultValue(*spec).boolean);
    PanelModel m;
    m.config.root = JsonValue();
    m.config.root.type = JsonValue::Type::Object;
    CHECK(SettingsView(m).flag(key::kPupilsToVrchat));
    m.config.root.set(key::kPupilsToVrchat, JsonValue::makeBool(false));
    CHECK(!SettingsView(m).flag(key::kPupilsToVrchat) && !SettingsView(m).locked(key::kPupilsToVrchat));
    // Round trip through the JSON text, as the panel writes and reads config.json
    {
        JsonValue back;
        std::string error;
        CHECK(parseJson(writeJson(m.config.root), back, error));
        ConfigFile file;
        file.root = back;
        CHECK(!file.flag(key::kPupilsToVrchat));
        back.set(key::kPupilsToVrchat, JsonValue::makeBool(true));
        CHECK(parseJson(writeJson(back), file.root, error));
        CHECK(file.flag(key::kPupilsToVrchat));
    }
    // Not a boolean: the default
    m.config.root.set(key::kPupilsToVrchat, JsonValue::makeString("no"));
    CHECK(SettingsView(m).flag(key::kPupilsToVrchat));
    // Locked by --no-pupils-to-vrchat: frameeyeosc's value
    m.config.root.set(key::kPupilsToVrchat, JsonValue::makeBool(true));
    m.status = parseStatus(
        "{\"pid\": 1, \"locked\": [\"pupils_to_vrchat\"], \"effective\": {\"pupils_to_vrchat\": false}, "
        "\"pupil_target\": null}",
        0, false);
    CHECK(SettingsView(m).locked(key::kPupilsToVrchat) && !SettingsView(m).flag(key::kPupilsToVrchat));
    CHECK(m.status.pupilTarget.empty());
    // Where they go, from status.json
    CHECK(parseStatus("{\"pid\": 1, \"pupil_target\": \"192.168.0.60:9000\"}", 0, false).pupilTarget ==
          "192.168.0.60:9000");
    // Shown only for LiveLink while the eye cameras are in use
    CHECK(pupilsRowShown(kOutputLivelink, true));
    CHECK(!pupilsRowShown(kOutputLivelink, false));
    CHECK(!pupilsRowShown(kOutputVrchat, true));
    CHECK(!pupilsRowShown(kOutputEtvr, true));
    CHECK(!pupilsRowShown("", true));
    // ...and in their place, without them, the one dashed line (where pupils could go: VRChat directly and LiveLink)
    CHECK(pupilsLineShown(kOutputLivelink, false) && pupilsLineShown(kOutputVrchat, false));
    CHECK(!pupilsLineShown(kOutputEtvr, false) && !pupilsLineShown("", false));
    for (const char* output : {kOutputVrchat, kOutputLivelink, kOutputEtvr, kOutputModule}) {
        CHECK(!pupilsLineShown(output, true));
    }
    // The FrameEyeOSC module carries the pupils itself (through VRCFT): the line without the cameras, never the row
    CHECK(pupilsLineShown(kOutputModule, false));
    CHECK(!pupilsRowShown(kOutputModule, true) && !pupilsRowShown(kOutputModule, false));
}

/** The short state after the Basic tab's eye camera switch follows the eye cameras page's "Now" row. */
void testCameraBrief() {
    EyeStatus s;
    s.present = s.running = true;
    s.camera.known = s.camera.present = true;
    s.camera.calibState = 5;
    s.camera.used[0] = s.camera.used[1] = true;
    CHECK(cameraBrief(s, true, false) == CameraBrief::Both);
    // (camera values still in use just after switching off: frameeyeosc hasn't caught up yet)
    CHECK(cameraBrief(s, false, false) == CameraBrief::Both);
    s.camera.used[1] = false;
    CHECK(cameraBrief(s, true, false) == CameraBrief::Left);
    s.camera.used[0] = false;
    s.camera.used[1] = true;
    CHECK(cameraBrief(s, true, false) == CameraBrief::Right);
    s.camera.used[1] = false;
    CHECK(cameraBrief(s, false, false) == CameraBrief::Off);
    // Learning the relaxed eyes, no baseline yet
    s.camera.calibState = 0;
    CHECK(cameraBrief(s, true, true) == CameraBrief::Warming);
    // Anything else: waiting (no camera values, not calibrated, frameeyeosc not running); off stays off
    CHECK(cameraBrief(s, true, false) == CameraBrief::Waiting);
    s.camera.present = false;
    CHECK(cameraBrief(s, true, false) == CameraBrief::Waiting);
    EyeStatus stopped;
    CHECK(cameraBrief(stopped, true, false) == CameraBrief::Waiting);
    CHECK(cameraBrief(stopped, false, false) == CameraBrief::Off);
}

/** pupil_bits (the Output tab's "How the avatar takes pupils"): an integer 0..4, 0 by default, and where its row
 *  shows (wherever the pupils go straight to VRChat) and greys (while they don't go there now). */
void testPupilBits() {
    const SettingSpec* spec = findSetting(key::kPupilBits);
    CHECK(spec != nullptr && spec->type == SettingType::Integer && spec->defaultNumber == 0);
    CHECK(spec != nullptr && spec->min == 0 && spec->max == 4);
    PanelModel m;
    m.config.root.type = JsonValue::Type::Object;
    CHECK(SettingsView(m).number(key::kPupilBits) == 0);
    m.config.root.set(key::kPupilBits, JsonValue::makeNumber(3, true));
    CHECK(SettingsView(m).number(key::kPupilBits) == 3);
    CHECK(writeJson(m.config.root).find("\"pupil_bits\": 3") != std::string::npos);
    // Locked by --pupil-bits: frameeyeosc's value
    m.status = parseStatus("{\"pid\": 1, \"locked\": [\"pupil_bits\"], \"effective\": {\"pupil_bits\": 2}}", 0, false);
    CHECK(SettingsView(m).locked(key::kPupilBits) && SettingsView(m).number(key::kPupilBits) == 2);
    // The row: only while the eye cameras are in use (set up and switched on), for VRChat directly and LiveLink;
    // greyed while the pupils don't go there (LiveLink without pupils_to_vrchat)
    CHECK(pupilBitsRow(kOutputVrchat, true, true) == PupilBitsRow::Usable);
    CHECK(pupilBitsRow(kOutputVrchat, true, false) == PupilBitsRow::Usable);  // pupils_to_vrchat is LiveLink's
    CHECK(pupilBitsRow(kOutputLivelink, true, true) == PupilBitsRow::Usable);
    CHECK(pupilBitsRow(kOutputLivelink, true, false) == PupilBitsRow::Greyed);
    for (const char* output : {kOutputVrchat, kOutputLivelink, kOutputEtvr, ""}) {
        CHECK(pupilBitsRow(output, false, true) == PupilBitsRow::Hidden);  // not in use (camera_lids off too)
    }
    CHECK(pupilBitsRow(kOutputEtvr, true, true) == PupilBitsRow::Hidden);
    // FrameEyeOSC: VRCFT sends the pupils in whatever form the avatar takes them
    CHECK(pupilBitsRow(kOutputModule, true, true) == PupilBitsRow::Hidden);
    CHECK(pupilBitsRow(kOutputModule, true, false) == PupilBitsRow::Hidden);
}

/** The bottom sentence of the eye cameras' page, for every case, in its order. */
void testCameraLine() {
    // The cameras in use come first, whatever else is going on
    for (const bool warming : {false, true}) {
        for (const bool live : {false, true}) {
            for (const bool locked : {false, true}) {
                CHECK(cameraLine(CameraUse::Both, warming, live, locked, true) == CameraLine::BothVrchat);
                CHECK(cameraLine(CameraUse::Both, warming, live, locked, false) == CameraLine::Both);
                CHECK(cameraLine(CameraUse::Left, warming, live, locked, true) == CameraLine::Left);
                CHECK(cameraLine(CameraUse::Right, warming, live, locked, false) == CameraLine::Right);
                // ...then camera_lids off
                CHECK(cameraLine(CameraUse::Off, warming, live, locked, true) == CameraLine::Off);
            }
        }
    }
    // Not read live: the "Now" row's reason
    for (const CameraUse use : {CameraUse::NoCamera, CameraUse::Warming, CameraUse::NotCalibrated, CameraUse::Error}) {
        CHECK(cameraLine(use, true, false, false, true) == CameraLine::Reason);
    }
    // Learning the relaxed eyes, with camera values arriving (headset on or off for a moment)
    CHECK(cameraLine(CameraUse::Warming, true, true, true, true) == CameraLine::Warming);
    CHECK(cameraLine(CameraUse::Warming, true, true, false, true) == CameraLine::Warming);
    // ...but without camera values the headset is off: put it on
    CHECK(cameraLine(CameraUse::NoCamera, true, true, false, true) == CameraLine::PutOn);
    CHECK(cameraLine(CameraUse::NoCamera, false, true, false, true) == CameraLine::PutOn);
    CHECK(cameraLine(CameraUse::Unknown, false, true, false, true) == CameraLine::PutOn);
    // The headset on, nothing learning, the cameras not used: the row's reason
    for (const CameraUse use : {CameraUse::NoCamera, CameraUse::NotCalibrated, CameraUse::Error, CameraUse::Valve,
                                CameraUse::Unknown}) {
        CHECK(cameraLine(use, false, true, true, true) == CameraLine::Reason);
    }
    // A camera error with the headset on: the reason (not "put it on")
    CHECK(cameraLine(CameraUse::Error, false, true, true, false) == CameraLine::Reason);
    // The sentence agrees with the row: learning only where the row shows its pill
    for (const CameraUse use : {CameraUse::NoCamera, CameraUse::Warming, CameraUse::Valve}) {
        CHECK((cameraLine(use, true, true, true, true) == CameraLine::Warming) == warmingShown(true, use));
    }
}

/** "Learned your relaxed eyes" only while camera values arrive; the "When..." box's rows always fit above the
 *  bottom sentence. */
void testLearnedAndHelpRows() {
    for (const CameraUse use : {CameraUse::Unknown, CameraUse::Off, CameraUse::Both, CameraUse::Left, CameraUse::Right,
                                CameraUse::NotCalibrated, CameraUse::Error, CameraUse::Valve}) {
        CHECK(learnedShown(true, true, use));
        CHECK(!learnedShown(true, false, use));  // eyecam-rec not reading the cameras (waiting for the tool)
        CHECK(!learnedShown(false, true, use));
    }
    CHECK(!learnedShown(true, true, CameraUse::NoCamera));  // the headset off: no values reach frameeyeosc
    // Rows: as many as fit, the box never past its bottom
    CHECK(helpBoxHeight(3) == 130 && helpBoxHeight(1) == 70);
    for (double space = 0; space < 200; space += 1) {
        const int rows = helpRows(100, 100 + space);
        CHECK(rows >= 0 && rows <= 3);
        if (rows > 0) CHECK(100 + helpBoxHeight(rows) <= 100 + space);
        if (rows < 3) CHECK(100 + helpBoxHeight(rows + 1) > 100 + space);
    }
    CHECK(helpRows(100, 169) == 0 && helpRows(100, 170) == 1 && helpRows(100, 230) == 3);
}

/** "Learning your relaxed eyes (N s left)" only while it can move: not without camera values (the headset off). */
void testWarmingShown() {
    for (const CameraUse use : {CameraUse::Unknown, CameraUse::Off, CameraUse::Both, CameraUse::Left, CameraUse::Right,
                                CameraUse::NotCalibrated, CameraUse::Warming, CameraUse::Error, CameraUse::Valve}) {
        CHECK(warmingShown(true, use));
        CHECK(!warmingShown(false, use));
    }
    CHECK(!warmingShown(true, CameraUse::NoCamera));
    CHECK(!warmingShown(false, CameraUse::NoCamera));
    // frameeyeosc without camera values while eyecam-rec warms up: the reason says so, the countdown doesn't show
    EyeStatus s = parseStatus("{\"pid\": 1, \"time\": 0, \"camera\": {\"present\": false, \"calib_state\": 0}}",
                              0, false);
    CHECK(cameraUse(s, true, true) == CameraUse::NoCamera);
    CHECK(!warmingShown(true, cameraUse(s, true, true)));
}

/** The Eyelids tab shows the cameras' widening in Widen's place only while both eyes are on the cameras. */
void testLidsFromCameras() {
    const auto status = [](const std::string& used, bool running = true) {
        EyeStatus s = parseStatus("{\"pid\": 1, \"time\": 0, \"camera\": {\"present\": true, \"calib_state\": 5, "
                                  "\"used\": " + used + "}}", 0, false);
        s.running = running;
        return s;
    };
    CHECK(lidsFromCameras(status("[true, true]")));
    CHECK(!lidsFromCameras(status("[true, false]")));
    CHECK(!lidsFromCameras(status("[false, true]")));
    CHECK(!lidsFromCameras(status("[false, false]")));
    CHECK(!lidsFromCameras(status("[true, true]", false)));  // frameeyeosc not running
    CHECK(!lidsFromCameras(parseStatus("{\"pid\": 1, \"time\": 0}", 0, false)));  // no camera (older)
}

void testCameraStatus() {
    // An older frameeyeosc, or none of it: unknown, nothing sent from the cameras
    {
        const EyeStatus s = parseStatus("{\"pid\": 1, \"time\": 0, \"sent\": {\"lids\": [0.7, 0.7]}}", 0, false);
        CHECK(s.running && !s.camera.known);
        CHECK(std::isnan(s.squint.v[0]) && std::isnan(s.squint.v[1]) && std::isnan(s.pupilDilation));
        CHECK(cameraUse(s, true) == CameraUse::Unknown);
        const EyeStatus none = parseStatus("{\"pid\": 1, \"time\": 0, \"camera\": null}", 0, false);
        CHECK(!none.camera.known && cameraUse(none, true) == CameraUse::Unknown);
    }
    // Everything
    const EyeStatus s = parseStatus(
        "{\"pid\": 1, \"time\": 0, \"sent\": {\"squint\": [0.25, 0.5], \"pupil_dilation\": 0.4}, "
        "\"camera\": {\"present\": true, \"calib_state\": 3, \"recalib_suggested\": true, \"used\": [true, true], "
        "\"pupil_used\": [true, false], \"error\": null}}",
        0, false);
    CHECK(s.camera.known && s.camera.present && s.camera.calibState == 3 && s.camera.recalibSuggested);
    CHECK(s.camera.used[0] && s.camera.used[1] && s.camera.pupilUsed[0] && !s.camera.pupilUsed[1]);
    CHECK(s.camera.error.empty());
    CHECK(s.squint.v[0] == 0.25 && s.squint.v[1] == 0.5 && s.pupilDilation == 0.4);
    CHECK(cameraUse(s, true) == CameraUse::Both);
    // squint and pupil_dilation null while the cameras don't drive them
    {
        const EyeStatus off = parseStatus(
            "{\"pid\": 1, \"time\": 0, \"sent\": {\"squint\": null, \"pupil_dilation\": null}}", 0, false);
        CHECK(std::isnan(off.squint.v[0]) && std::isnan(off.pupilDilation));
    }
    // Odd values are missing
    {
        const EyeStatus odd = parseStatus(
            "{\"pid\": 1, \"time\": 0, \"camera\": {\"present\": 1, \"calib_state\": \"3\", \"used\": [1, true], "
            "\"pupil_used\": true, \"error\": 5}}",
            0, false);
        CHECK(odd.camera.known && !odd.camera.present && odd.camera.calibState == 0);
        CHECK(!odd.camera.used[0] && odd.camera.used[1] && !odd.camera.pupilUsed[0] && odd.camera.error.empty());
    }

    // What the line says
    const auto use = [](const std::string& camera, bool cameraLids, bool running = true) {
        EyeStatus s = parseStatus("{\"pid\": 1, \"time\": 0, \"camera\": " + camera + "}", 0, false);
        s.running = running;
        return cameraUse(s, cameraLids);
    };
    CHECK(use("{\"present\": true, \"calib_state\": 3, \"used\": [true, false]}", true) == CameraUse::Left);
    CHECK(use("{\"present\": true, \"calib_state\": 3, \"used\": [false, true]}", true) == CameraUse::Right);
    CHECK(use("{\"present\": true, \"calib_state\": 3, \"used\": [false, false]}", false) == CameraUse::Off);
    CHECK(use("{\"present\": false, \"calib_state\": 0}", true) == CameraUse::NoCamera);
    CHECK(use("{\"present\": true, \"calib_state\": 2}", true) == CameraUse::NotCalibrated);
    // A baseline eyecam-rec learned by itself (bit 2) is one too; while it learns, that is the reason
    CHECK(use("{\"present\": true, \"calib_state\": 4}", true) == CameraUse::Valve);
    CHECK(use("{\"present\": true, \"calib_state\": 6}", true) == CameraUse::Valve);
    {
        EyeStatus s = parseStatus("{\"pid\": 1, \"time\": 0, \"camera\": {\"present\": true, \"calib_state\": 2}}",
                                  0, false);
        CHECK(cameraUse(s, true, true) == CameraUse::Warming);
        CHECK(cameraUse(s, true, false) == CameraUse::NotCalibrated);
        // ...but the setting, no camera values or the cameras in use come first
        CHECK(cameraUse(s, false, true) == CameraUse::Off);
        s.camera.present = false;
        CHECK(cameraUse(s, true, true) == CameraUse::NoCamera);
        s.camera.present = true;
        s.camera.used[0] = s.camera.used[1] = true;
        CHECK(cameraUse(s, true, true) == CameraUse::Both);
        // ...and a baseline already there isn't "learning"
        s.camera.used[0] = s.camera.used[1] = false;
        s.camera.calibState = 4;
        CHECK(cameraUse(s, true, true) == CameraUse::Valve);
    }
    CHECK(use("{\"present\": true, \"calib_state\": 1, \"error\": \"frames are stale\"}", true) == CameraUse::Error);
    CHECK(use("{\"present\": true, \"calib_state\": 1}", true) == CameraUse::Valve);
    // Still in use just after the switch went off (frameeyeosc hasn't read it yet): says what it does
    CHECK(use("{\"present\": true, \"calib_state\": 3, \"used\": [true, true]}", false) == CameraUse::Both);
    // frameeyeosc not running: unknown
    CHECK(use("{\"present\": true, \"calib_state\": 3, \"used\": [true, true]}", true, false) == CameraUse::Unknown);
}

/** The new release's summary under the update row: Japanese on a Japanese panel when the release has it, else
 *  English, and nothing unless a newer release is available. */
void testUpdateNotes() {
    frame_updater::UpdateStatus u;
    u.state = frame_updater::UpdateState::Available;
    u.notes = "Faster eye data.";
    u.notesJa = "目のデータが速くなる。";
    CHECK(updateNotes(u, Language::Ja) == "目のデータが速くなる。");
    CHECK(updateNotes(u, Language::En) == "Faster eye data.");
    // No Japanese paragraph in the release text: English on both
    u.notesJa.clear();
    CHECK(updateNotes(u, Language::Ja) == "Faster eye data.");
    // No release text at all: nothing
    u.notes.clear();
    CHECK(updateNotes(u, Language::Ja).empty() && updateNotes(u, Language::En).empty());
    // Only while an update is available (not while it installs, or once installed)
    u.notes = "Faster eye data.";
    u.notesJa = "目のデータが速くなる。";
    for (const auto state : {frame_updater::UpdateState::Unknown, frame_updater::UpdateState::UpToDate,
                             frame_updater::UpdateState::Installing, frame_updater::UpdateState::Installed,
                             frame_updater::UpdateState::CheckFailed, frame_updater::UpdateState::InstallFailed}) {
        u.state = state;
        CHECK(updateNotes(u, Language::Ja).empty() && updateNotes(u, Language::En).empty());
    }
    // Going back to an older regular version: its summary isn't news
    u.state = frame_updater::UpdateState::Available;
    u.backToStable = true;
    CHECK(updateNotes(u, Language::Ja).empty() && updateNotes(u, Language::En).empty());
}

/** Test versions on the Version page: the "Test version available" badge only while "Get test versions" is off and a
 *  test version newer than the running one exists; "Using test version" and the keep-hint on a test version. */
void testTestVersionView() {
    frame_updater::UpdateStatus u;
    u.state = frame_updater::UpdateState::UpToDate;
    u.current = "0.7.10";
    u.latest = "0.7.10";
    // Nothing newer on the beta channel: no badge, either way
    TestVersionView v = testVersionView(u, false);
    CHECK(!v.badge && !v.using_ && !v.keepHint && !v.offered);
    CHECK(!testVersionView(u, true).badge);
    // A newer test version: the badge while off, none while on (the update row says it then)
    u.testVersion = "0.7.11-beta.1";
    CHECK(testVersionView(u, false).badge);
    CHECK(!testVersionView(u, true).badge);
    // Also beside an available regular version
    u.state = frame_updater::UpdateState::Available;
    u.current = "0.7.9";
    CHECK(testVersionView(u, false).badge && !testVersionView(u, false).offered);
    // Not newer than the running one (a stale answer after updating): no badge
    u.current = "0.7.11-beta.1";
    CHECK(!testVersionView(u, false).badge);
    u.current = "0.7.11";
    CHECK(!testVersionView(u, false).badge);
    // No version known yet
    u.state = frame_updater::UpdateState::Unknown;
    u.current = "0.7.10";
    u.testVersion.clear();
    CHECK(!testVersionView(u, false).badge);

    // Running a test version: "Using test version"; off, the hint says it stays until the next regular version
    u.state = frame_updater::UpdateState::UpToDate;
    u.current = "0.7.11-beta.2";
    u.latest = "0.7.10";
    v = testVersionView(u, false);
    CHECK(v.using_ && v.keepHint && !v.badge);
    v = testVersionView(u, true);
    CHECK(v.using_ && !v.keepHint);
    // A newer test version on top of the running one (off): the badge too
    u.testVersion = "0.7.11-beta.3";
    CHECK(testVersionView(u, false).badge);
    u.testVersion.clear();

    // The beta channel offers a test version: "Test version X is available"; a regular one: not
    u.state = frame_updater::UpdateState::Available;
    u.current = "0.7.10";
    u.latest = "0.7.11-beta.1";
    u.prerelease = true;
    CHECK(testVersionView(u, true).offered && !testVersionView(u, true).badge);
    u.latest = "0.7.11";
    u.prerelease = false;
    CHECK(!testVersionView(u, true).offered);
    // Only while it is available
    u.prerelease = true;
    u.state = frame_updater::UpdateState::Installing;
    CHECK(!testVersionView(u, true).offered);

    // Turned off on a test version, the older regular version is offered going back to: on the Version page only, no
    // notice or dot like an update's
    u.state = frame_updater::UpdateState::Available;
    u.current = "0.7.11-beta.2";
    u.latest = "0.7.10";
    u.prerelease = false;
    u.installable = true;
    u.backToStable = true;
    v = testVersionView(u, false);
    CHECK(v.back && v.using_ && v.keepHint && !v.offered && !v.badge);
    CHECK(!updateNotice(u));
    // A newer regular version is a plain update, with the notice
    u.latest = "0.7.11";
    u.backToStable = false;
    CHECK(!testVersionView(u, false).back && updateNotice(u));
    // Installing or installed (going back too) still shows the notice; nothing else does
    u.backToStable = true;
    for (const auto state : {frame_updater::UpdateState::Installing, frame_updater::UpdateState::Installed}) {
        u.state = state;
        CHECK(!testVersionView(u, false).back && updateNotice(u));
    }
    for (const auto state : {frame_updater::UpdateState::Unknown, frame_updater::UpdateState::UpToDate,
                             frame_updater::UpdateState::CheckFailed, frame_updater::UpdateState::InstallFailed}) {
        u.state = state;
        CHECK(!testVersionView(u, false).back && !updateNotice(u));
    }
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
/**
 * A model with the gaze scale at these values (and these keys locked by the command line).
 * @param values left, right, up, down
 * @param locked keys locked
 * @return the model
 */
PanelModel scaleModel(const double (&values)[4], const std::vector<std::string>& locked = {}) {
    PanelModel m;
    m.config.exists = true;
    m.config.root.type = JsonValue::Type::Object;
    for (int i = 0; i < 4; ++i) m.config.root.set(kGazeScaleKeys[i / 2][i % 2], JsonValue::makeNumber(values[i]));
    m.status.present = m.status.running = true;  // (keys are locked only while frameeyeosc runs)
    m.status.locked = locked;
    return m;
}

/**
 * The value a change list writes for a key.
 * @param changes the changes
 * @param name the key
 * @return it, or NaN if the key isn't written
 */
double written(const std::vector<SettingChange>& changes, const char* name) {
    for (const SettingChange& change : changes) {
        if (std::string(change.key) == name && change.value.isNumber()) return change.value.number;
    }
    return NAN;
}

/** The gaze scale: shown as percent, its pairs, the locks at start, − / ＋ with the lock closed or open, re-linking. */
void testGazeScale() {
    CHECK(formatPercent(1.0) == "100%" && formatPercent(0.25) == "25%" && formatPercent(1.5) == "150%");
    CHECK(formatPercent(0.05) == "5%" && formatPercent(2.0) == "200%");
    CHECK(formatPercent(0.95) == "95%" && formatPercent(1.05) == "105%" && formatPercent(0.55) == "55%");
    CHECK(formatPercent(NAN) == "—");
    // Its range and step, and the default where the file has none
    for (const auto& pair : kGazeScaleKeys) {
        for (const char* name : pair) {
            const SettingSpec* spec = findSetting(name);
            CHECK(spec != nullptr && spec->min == 0.05 && spec->max == 2.0 && spec->step == 0.05);
            CHECK(spec != nullptr && spec->defaultNumber == 1.0);
        }
    }
    PanelModel plain;
    plain.config.exists = true;
    plain.config.root.type = JsonValue::Type::Object;
    CHECK(SettingsView(plain).number(key::kGazeScaleDown) == 1.0);
    // ...so a file from before it needs nothing done
    plain.config.root.set(key::kVersion, JsonValue::makeNumber(kConfigVersion, true));
    plain.config.root.set(key::kLidWiden, JsonValue::makeString("normal"));
    CHECK(!configNeedsMigration(plain.config.root));
    // The pairs
    CHECK(gazeScalePair(key::kGazeScaleLeft) == 0 && gazeScalePair(key::kGazeScaleRight) == 0);
    CHECK(gazeScalePair(key::kGazeScaleUp) == 1 && gazeScalePair(key::kGazeScaleDown) == 1);
    CHECK(gazeScalePair(key::kGazeGainUp) == -1);
    // At start: closed as left, but open while the two differ
    CHECK(gazeScaleLinkShown(true, 1.0, 1.0) && gazeScaleLinkShown(true, 0.8, 0.8));
    CHECK(!gazeScaleLinkShown(true, 1.2, 0.9) && !gazeScaleLinkShown(false, 1.0, 1.0));
    // ＋ on the left, linked: both go to 105%; open: the left alone
    const PanelModel ones = scaleModel({1.0, 1.0, 1.0, 1.0});
    const SettingsView view(ones);
    std::vector<SettingChange> changes = gazeScaleStep(view, key::kGazeScaleLeft, 1, true);
    CHECK(changes.size() == 2 && std::fabs(written(changes, key::kGazeScaleLeft) - 1.05) < 1e-9 &&
          std::fabs(written(changes, key::kGazeScaleRight) - 1.05) < 1e-9);
    changes = gazeScaleStep(view, key::kGazeScaleLeft, 1, false);
    CHECK(changes.size() == 1 && std::fabs(written(changes, key::kGazeScaleLeft) - 1.05) < 1e-9);
    // − on the second of a pair, linked, moves the first too
    changes = gazeScaleStep(view, key::kGazeScaleDown, -1, true);
    CHECK(changes.size() == 2 && std::fabs(written(changes, key::kGazeScaleUp) - 0.95) < 1e-9 &&
          std::fabs(written(changes, key::kGazeScaleDown) - 0.95) < 1e-9 && std::isnan(written(changes, key::kGazeScaleLeft)));
    // At the ends (5 % and 200 %): nothing moves past them; one step in from them is 10 % and 195 %
    const PanelModel ends = scaleModel({0.05, 0.05, 2.0, 2.0});
    CHECK(gazeScaleStep(SettingsView(ends), key::kGazeScaleRight, -1, true).empty());
    CHECK(gazeScaleStep(SettingsView(ends), key::kGazeScaleUp, 1, true).empty());
    changes = gazeScaleStep(SettingsView(ends), key::kGazeScaleLeft, 1, false);
    CHECK(changes.size() == 1 && std::fabs(written(changes, key::kGazeScaleLeft) - 0.1) < 1e-9);
    changes = gazeScaleStep(SettingsView(ends), key::kGazeScaleDown, -1, false);
    CHECK(changes.size() == 1 && std::fabs(written(changes, key::kGazeScaleDown) - 1.95) < 1e-9);
    // ...and the old ends (25 % and 150 %) go on past them
    const PanelModel oldEnds = scaleModel({0.25, 0.25, 1.5, 1.5});
    changes = gazeScaleStep(SettingsView(oldEnds), key::kGazeScaleRight, -1, false);
    CHECK(changes.size() == 1 && std::fabs(written(changes, key::kGazeScaleRight) - 0.2) < 1e-9);
    changes = gazeScaleStep(SettingsView(oldEnds), key::kGazeScaleUp, 1, false);
    CHECK(changes.size() == 1 && std::fabs(written(changes, key::kGazeScaleUp) - 1.55) < 1e-9);
    // Linked, with the other one locked by the command line: only the one pressed; the one pressed locked: nothing
    const PanelModel locked = scaleModel({1.0, 1.0, 1.0, 1.0}, {key::kGazeScaleRight});
    changes = gazeScaleStep(SettingsView(locked), key::kGazeScaleLeft, 1, true);
    CHECK(changes.size() == 1 && std::string(changes[0].key) == key::kGazeScaleLeft);
    CHECK(gazeScaleStep(SettingsView(locked), key::kGazeScaleRight, 1, true).empty());
    // ...so that pair's lock is not usable (drawn open), the other pair's is
    CHECK(!gazeScaleLinkUsable(SettingsView(locked), 0) && gazeScaleLinkUsable(SettingsView(locked), 1));
    CHECK(gazeScaleLinkUsable(view, 0) && gazeScaleLinkUsable(view, 1) && !gazeScaleLinkUsable(view, 2));
    // The first of a pair locked: the second moves on its own
    const PanelModel firstLocked = scaleModel({1.0, 1.0, 1.0, 1.0}, {key::kGazeScaleUp});
    CHECK(!gazeScaleLinkUsable(SettingsView(firstLocked), 1));
    changes = gazeScaleStep(SettingsView(firstLocked), key::kGazeScaleDown, -1, true);
    CHECK(changes.size() == 1 && std::fabs(written(changes, key::kGazeScaleDown) - 0.95) < 1e-9);
    CHECK(gazeScaleStep(view, key::kGazeGainUp, 1, true).empty());
    // Closing a lock again: the second takes the first (right := left, down := up); nothing when they are the same
    const PanelModel apart = scaleModel({1.2, 0.9, 0.75, 1.1});
    changes = gazeScaleRelink(SettingsView(apart), 0);
    CHECK(changes.size() == 1 && std::fabs(written(changes, key::kGazeScaleRight) - 1.2) < 1e-9);
    changes = gazeScaleRelink(SettingsView(apart), 1);
    CHECK(changes.size() == 1 && std::fabs(written(changes, key::kGazeScaleDown) - 0.75) < 1e-9);
    CHECK(gazeScaleRelink(view, 0).empty() && gazeScaleRelink(view, 1).empty() && gazeScaleRelink(view, 2).empty());
    CHECK(gazeScaleRelink(SettingsView(scaleModel({1.2, 0.9, 1.0, 1.0}, {key::kGazeScaleRight})), 0).empty());
    // The eye fit leaves it alone: neither "Reset" nor a fit's values touch it
    const std::vector<std::string> keys = fitResetKeys();
    for (const auto& pair : kGazeScaleKeys) {
        for (const char* name : pair) CHECK(std::find(keys.begin(), keys.end(), name) == keys.end());
    }
    JsonValue root = scaleModel({1.2, 0.9, 0.75, 1.1}).config.root;
    applyFitValues(root, measured(true), gaze_fit::Mode::Full);
    CHECK(std::fabs(numberIn(root, key::kGazeScaleLeft) - 1.2) < 1e-9 && std::fabs(numberIn(root, key::kGazeScaleDown) - 1.1) < 1e-9);
}

int main() {
    testGazeScale();
    testFitResetsScales();
    testFitWritesPolicy();
    testResetClearsScales();
    testRecenterDefault();
    testOutputArgs();
    testWidenState();
    testWidenSlider();
    testLidPresets();
    testMigrateLidScales();
    testMigrateLidOpenSnap();
    testSourceError();
    testDominantEyeAndSaturation();
    testTrackerRateCause();
    testGazePresets();
    testMigrateGazePresets();
    testMigrateFitPolicy();
    testMigrateEyeOffsets();
    testSteamlinkParams();
    testNativeEyes();
    testCameraLids();
    testWiden();
    testBrowSettings();
    testGazeDebugRings();
    testCameraStatus();
    testLidsFromCameras();
    testWarmingShown();
    testCameraLine();
    testPupilsToVrchat();
    testCameraBrief();
    testPupilBits();
    testLearnedAndHelpRows();
    testUpdateNotes();
    testTestVersionView();
    if (gFailures == 0) std::printf("model-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
