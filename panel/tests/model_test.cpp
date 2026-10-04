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
    values.rollDeg = 2.0;
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

    for (gaze_fit::Mode mode : {gaze_fit::Mode::Center, gaze_fit::Mode::Tilt}) {
        JsonValue rewear = tweakedRoot();
        applyFitValues(rewear, measured(true), mode);
        CHECK(std::fabs(numberIn(rewear, key::kLidScaleLeft) - 1.1) < 1e-9);
        CHECK(std::fabs(numberIn(rewear, key::kLidScaleRight) - 0.69) < 1e-9);
        CHECK(std::fabs(numberIn(rewear, key::kLidFitOpenLeft) - 0.5) < 1e-9);
        CHECK(std::fabs(numberIn(rewear, key::kGazeOffsetX) - 0.01) < 1e-9);
        // Only the re-wear fit with the side dots sets the tilt
        CHECK(std::isnan(numberIn(rewear, key::kGazeRollDeg)) == (mode == gaze_fit::Mode::Center));
    }
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

/** Re-centering only is the default; the tilt only when chosen; the button re-centers when it is off. */
void testRecenterDefault() {
    CHECK(recenterOf(JsonValue::makeNull()) == AutoRecenter::Center);
    CHECK(recenterOf(JsonValue::makeString("center")) == AutoRecenter::Center);
    CHECK(recenterOf(JsonValue::makeString("tilt")) == AutoRecenter::Tilt);
    CHECK(recenterOf(JsonValue::makeString("off")) == AutoRecenter::Off);
    CHECK(recenterOf(JsonValue::makeString("sideways")) == AutoRecenter::Center);
    CHECK(recenterOf(JsonValue::makeBool(true)) == AutoRecenter::Center);
    CHECK(recenterOf(JsonValue::makeBool(false)) == AutoRecenter::Off);
    CHECK(rewearMode(AutoRecenter::Off) == gaze_fit::Mode::Center);
    CHECK(rewearMode(AutoRecenter::Center) == gaze_fit::Mode::Center);
    CHECK(rewearMode(AutoRecenter::Tilt) == gaze_fit::Mode::Tilt);
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
        CHECK(numberIn(root, key::kVersion) == kConfigVersion && root.get(key::kVersion)->integer);
        CHECK(log.find(" -> ") != std::string::npos && log.find("gaze_deadzone 0.02 -> 0.005") != std::string::npos);
        CHECK(!configNeedsMigration(root));
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
    CHECK(numberIn(own, key::kVersion) == kConfigVersion);
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

/** The destination cards' button arguments stand for the output types both ways. */
void testOutputArgs() {
    for (int arg = 0; arg < 3; ++arg) CHECK(argOfOutput(outputOfArg(arg)) == arg);
    CHECK(std::string(outputOfArg(2)) == kOutputLivelink);
    CHECK(argOfOutput("osc") == -1);
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

/** frameeyeosc's "camera" and the camera values sent, and what the eye capture tab says about them. */
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
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testFitResetsScales();
    testResetClearsScales();
    testRecenterDefault();
    testOutputArgs();
    testWidenState();
    testMigrateLidScales();
    testSourceError();
    testDominantEyeAndSaturation();
    testTrackerRateCause();
    testGazePresets();
    testMigrateGazePresets();
    testSteamlinkParams();
    testNativeEyes();
    testCameraLids();
    testCameraStatus();
    testLidsFromCameras();
    testWarmingShown();
    testCameraLine();
    testUpdateNotes();
    if (gFailures == 0) std::printf("model-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
