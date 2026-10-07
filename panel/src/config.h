// The settings file shared with frameeyeosc (config.json). The panel is the only writer: it re-reads the file,
// changes the keys it was asked to change, and writes everything back (unknown keys are kept) through a temporary
// file, fsync and rename. frameeyeosc only reads it.
#pragma once

#include "json.h"

#include <functional>
#include <string>
#include <vector>

/** How a setting is stored in config.json. */
enum class SettingType {
    Bool,
    Number,          ///< a float
    NullableNumber,  ///< a float or null (null = automatic)
    Integer,
    NullableInteger, ///< an integer or null (null = the default for the output type)
    String,
};

/** One key of config.json: its type, default and the range the panel's − / ＋ buttons keep it in. */
struct SettingSpec {
    const char* key;
    SettingType type;
    double defaultNumber;     ///< default for numbers and booleans (1 = true); ignored for strings and nullables
    const char* defaultText;  ///< default for strings
    double min;               ///< smallest value the panel writes
    double max;               ///< largest value the panel writes
    double step;              ///< how much one press of − / ＋ changes the value
    int decimals;             ///< digits shown after the decimal point
    double onNumber = 0;      ///< what "On" sets for a number that is off (0) by default
};

/** Screen font (Noto Sans CJK; found through fontconfig if the file is missing). */
constexpr const char* kFontPath = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";
constexpr const char* kBoldFontPath = "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc";

/**
 * config.json's "version": 2 from the 0.7.0 gaze presets (see migrateGazePresets); a file without it, or with 1, is
 * brought up to date once. frameeyeosc does not read it.
 */
constexpr int kConfigVersion = 2;

/** Keys the panel writes (the names are shared with frameeyeosc). */
namespace key {
constexpr const char* kVersion = "version";
constexpr const char* kSending = "sending";
constexpr const char* kOutput = "output";
constexpr const char* kHost = "host";
constexpr const char* kPort = "port";
constexpr const char* kPrefix = "prefix";
constexpr const char* kEyeTrackingActive = "eye_tracking_active";
/** VRChat output: also send Steam Link's own avatar parameter names (LeftEyeX, RightEyeLid, ...; no prefix). */
constexpr const char* kSteamlinkParams = "steamlink_params";
/** VRChat output: also send VRChat's own eye tracking input (/tracking/eye/*), for avatars without VRCFT parameters. */
constexpr const char* kNativeEyes = "native_eyes";
/** LiveLink output: send the eye cameras' pupils straight to VRChat (the LiveLink module has none; default on). */
constexpr const char* kPupilsToVrchat = "pupils_to_vrchat";
/**
 * Wherever the pupils go to VRChat: also send the dilation as this many bool parameters (PupilDilation1, 2, 4, 8) for
 * avatars that take it bit-packed; 0 (the default) sends the float only.
 */
constexpr const char* kPupilBits = "pupil_bits";
/** Eyelids (and squint) from the eye cameras where eyecam-rec reads them live and is calibrated (default on). */
constexpr const char* kCameraLids = "camera_lids";
constexpr const char* kRaw = "raw";
constexpr const char* kGazeMinCutoff = "gaze_min_cutoff";
constexpr const char* kGazeBeta = "gaze_beta";
constexpr const char* kGazeDCutoff = "gaze_d_cutoff";
constexpr const char* kGazeDeadzone = "gaze_deadzone";
constexpr const char* kGazeHoldBelow = "gaze_hold_below";
constexpr const char* kIndependentEyes = "independent_eyes";
constexpr const char* kLidMinCutoff = "lid_min_cutoff";
constexpr const char* kLidBeta = "lid_beta";
constexpr const char* kLidClosed = "lid_closed";
constexpr const char* kLidOpen = "lid_open";
constexpr const char* kLidWidenStart = "lid_widen_start";
constexpr const char* kLidWide = "lid_wide";
constexpr const char* kLidScaleLeft = "lid_scale_left";
constexpr const char* kLidScaleRight = "lid_scale_right";
constexpr const char* kLidCalibration = "lid_calibration";
constexpr const char* kLidSync = "lid_sync";
constexpr const char* kGazeQualityLimit = "gaze_quality_limit";
constexpr const char* kBlinkHoldMs = "blink_hold_ms";
constexpr const char* kDespike = "despike";
constexpr const char* kBlinkSyncBelow = "blink_sync_below";
constexpr const char* kCameraLidFloor = "camera_lid_floor";
constexpr const char* kGazeOffsetX = "gaze_offset_x";
constexpr const char* kGazeOffsetY = "gaze_offset_y";
constexpr const char* kGazeGainX = "gaze_gain_x";
constexpr const char* kGazeGainUp = "gaze_gain_up";
constexpr const char* kGazeGainDown = "gaze_gain_down";
constexpr const char* kGazeRollDeg = "gaze_roll_deg";
constexpr const char* kGazeDownHoldXDeg = "gaze_down_hold_x_deg";
constexpr const char* kGazeOffsetXLeft = "gaze_offset_x_left";
constexpr const char* kGazeOffsetXRight = "gaze_offset_x_right";
constexpr const char* kGazeGainXLeft = "gaze_gain_x_left";
constexpr const char* kGazeGainXRight = "gaze_gain_x_right";
constexpr const char* kGazeDebugDots = "gaze_debug_dots";
/** The panel's own: how far ahead the debug gaze dots are (m; frameeyeosc ignores it). */
constexpr const char* kGazeDebugDotsDistanceM = "gaze_debug_dots_distance_m";
/** The panel's own: play sound cues during the eye fit (frameeyeosc ignores it). */
constexpr const char* kFitSounds = "fit_sounds";
/** The panel's own: the fit run by itself when the headset is put on, "center" (the default), "tilt" or "off"
 *  (see kAutoRecenterModes; frameeyeosc ignores it). */
constexpr const char* kAutoRecenter = "auto_recenter";
/** How easily an eye with an eye fit widens: see kLidWidenModes. */
constexpr const char* kLidWiden = "lid_widen";
constexpr const char* kLidFitClosedLeft = "lid_fit_closed_left";
constexpr const char* kLidFitClosedRight = "lid_fit_closed_right";
constexpr const char* kLidFitUpLeft = "lid_fit_up_left";
constexpr const char* kLidFitUpRight = "lid_fit_up_right";
constexpr const char* kLidFitOpenLeft = "lid_fit_open_left";
constexpr const char* kLidFitOpenRight = "lid_fit_open_right";
constexpr const char* kLidFitDownLeft = "lid_fit_down_left";
constexpr const char* kLidFitDownRight = "lid_fit_down_right";
/** A request, not a setting: {"id": N, "target": "center"} asks frameeyeosc to average the gaze (eye fit). */
constexpr const char* kGazeCapture = "gaze_capture";
constexpr const char* kCalibrationReset = "calibration_reset";
constexpr const char* kLanguage = "language";
constexpr const char* kUpdateCheck = "update_check";  ///< panel only: look for a new release on GitHub
}  // namespace key

/** Output types (the "output" key). */
constexpr const char* kOutputVrchat = "vrchat";
constexpr const char* kOutputEtvr = "etvr";
constexpr const char* kOutputLivelink = "livelink";  ///< Live Link Face packets for VRCFaceTracking's LiveLink module
/** eye_tracking_active values: how EyeTrackingActive is sent in VRChat mode */
constexpr const char* kActiveTypes[3] = {"bool", "float", "off"};
/** auto_recenter values, in the order the Eye fit tab's "When put on" row shows them: nothing, one dot (the
 *  default), the dot and the up / down dots */
constexpr const char* kAutoRecenterModes[3] = {"off", "center", "tilt"};
/** lid_widen values, in the order the Eyelids tab shows them: never, less, normal (the default), more */
constexpr const char* kLidWidenModes[4] = {"off", "low", "normal", "high"};
/** Default ports of the output types (used while "port" is null). */
constexpr int kPortVrchat = 9000;
constexpr int kPortEtvr = 8889;
constexpr int kPortLivelink = 11111;

/**
 * All keys in the order a new config.json is written.
 * @return the table (lives for the whole program)
 */
const std::vector<SettingSpec>& settingSpecs();

/**
 * Find a key in the table.
 * @param name the key
 * @return its spec, or nullptr if the panel does not know it
 */
const SettingSpec* findSetting(const std::string& name);

/**
 * The default value of a key as JSON.
 * @param spec the key
 * @return its default
 */
JsonValue defaultValue(const SettingSpec& spec);

/**
 * The contents of config.json as last read. Getters fall back to the default when a key is missing or has the
 * wrong type, the same way frameeyeosc reads it.
 */
struct ConfigFile {
    JsonValue root;        ///< the whole object as read (unknown keys included)
    bool exists = false;   ///< the file was there
    std::string error;     ///< not empty if the file exists but is not a valid JSON object

    /**
     * The value of a key, or its default.
     * @param name the key
     * @return the value (null for a nullable key set to null or missing)
     */
    JsonValue value(const std::string& name) const;

    /**
     * A number key, or its default.
     * @param name the key
     * @return the number (NaN for a nullable key that is null)
     */
    double number(const std::string& name) const;

    /**
     * A boolean key, or its default.
     * @param name the key
     * @return the value
     */
    bool flag(const std::string& name) const;

    /**
     * A string key, or its default.
     * @param name the key
     * @return the value
     */
    std::string text(const std::string& name) const;
};

/**
 * The default place of config.json ($XDG_CONFIG_HOME/frameeyeosc or ~/.config/frameeyeosc).
 * @return the path
 */
std::string defaultConfigPath();

/**
 * Read config.json. A missing file is not an error (exists = false, all defaults).
 * @param path the file
 * @return what was read; `error` is set if the file is broken
 */
ConfigFile readConfigFile(const std::string& path);

/**
 * The file's modification time and size, to notice changes cheaply.
 * @param path the file
 * @return a value that changes when the file changes (0 if it does not exist)
 */
std::string configStamp(const std::string& path);

/**
 * Re-read config.json, apply a change to its object and write it back atomically (temporary file in the same
 * folder, fsync, rename). A missing file is created with every key at its default before the change. A broken
 * file is left alone and the call fails, so nothing the user wrote is lost.
 * @param path the file
 * @param change edits the object (unknown keys must be kept)
 * @param error why it failed
 * @return true if written
 */
bool updateConfigFile(const std::string& path, const std::function<void(JsonValue&)>& change, std::string& error);

/**
 * "Reset everything": every known setting goes back to its default. The language, the calibration reset counter
 * and unknown keys stay. A broken file is first copied to config.json.broken, then replaced.
 * @param path the file
 * @param error why it failed
 * @return true if written
 */
bool resetConfigFile(const std::string& path, std::string& error);

/**
 * Round a value onto the step grid and into its range (used by the − / ＋ buttons).
 * @param spec the key
 * @param current the value now
 * @param direction -1 or +1
 * @param low extra lower bound (for keys that must stay above another key)
 * @param high extra upper bound
 * @return the new value
 */
double stepValue(const SettingSpec& spec, double current, int direction, double low, double high);

/**
 * Round a value to the key's shown decimals, so the file does not collect 0.30000000000000004.
 * @param spec the key
 * @param value the value
 * @return the rounded value
 */
double roundToDecimals(const SettingSpec& spec, double value);

/**
 * A value onto a setting's step grid, within its range (a slider's position).
 * @param spec the setting
 * @param value the value
 * @return the value on the grid
 */
double snapValue(const SettingSpec& spec, double value);
