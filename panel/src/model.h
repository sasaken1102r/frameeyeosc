// What the panel shows, put together from config.json, status.json and the autostart state, and the rules that
// several screens share (gaze presets, recommended settings per output, the order of the four lid marks).
#pragma once

#include "autostart.h"
#include "changelog.h"
#include "config.h"
#include "eyecam.h"
#include "gaze_fit.h"
#include "i18n.h"
#include "recorder.h"
#include "status.h"
#include "update_check.h"

#include <string>
#include <vector>

/** Everything one frame of the panel is drawn from. */
struct PanelModel {
    ConfigFile config;           ///< config.json as last read
    EyeStatus status;            ///< status.json as last read
    AutostartState autostart;    ///< the systemd unit state
    std::string configPath;      ///< the file the panel writes
    std::string statusPath;      ///< the file the panel reads
    std::string panelError;      ///< the panel's own last write failure (English detail); empty if none
    bool panelErrorBroken = false;  ///< that failure was because config.json is broken
    Language language = Language::Ja;
    frame_updater::UpdateStatus update;  ///< new-release check and install (see frame-updater)
    gaze_fit::View fit;          ///< the eye fit session (Eye fit tab)
    recorder::View recording;    ///< the eye log (Advanced tab, and a mark in the left column while it records)
    std::vector<std::string> changelogDirs;  ///< where to look for CHANGELOG.md (changelog::defaultDirs)
    changelog::History history;  ///< the version history, read when it is opened
    eyecam::View eyecam;         ///< eyecam-rec, for the developer tab "Eye capture" (only shown while it runs)
    std::string eyecamDir;       ///< its folder (status.json and ctl.sock; eyecam::defaultDir or --eyecam-dir)
};

/**
 * The value shown for a setting: the command-line value from status.json while it is locked there,
 * otherwise the value in config.json (or its default).
 */
class SettingsView {
public:
    /**
     * @param model the model (must outlive the view)
     */
    explicit SettingsView(const PanelModel& model) : model_(model) {}

    /**
     * Whether a key is set on frameeyeosc's command line.
     * @param name the key
     * @return true if the panel must not change it
     */
    bool locked(const std::string& name) const;

    /**
     * The value shown for a key.
     * @param name the key
     * @return the value (null for a nullable key that is automatic)
     */
    JsonValue value(const std::string& name) const;

    /**
     * A number key.
     * @param name the key
     * @return the number, or NaN if null
     */
    double number(const std::string& name) const;

    /**
     * A boolean key.
     * @param name the key
     * @return the value
     */
    bool flag(const std::string& name) const;

    /**
     * A string key.
     * @param name the key
     * @return the value
     */
    std::string text(const std::string& name) const;

    /**
     * The port in use: the "port" key, or the default of the output type when it is null.
     * @return the port
     */
    int port() const;

    /**
     * Whether "port" is null (follows the output type).
     * @return true if automatic
     */
    bool portIsDefault() const;

private:
    const PanelModel& model_;
};

/** The eye fit as config.json holds it (shown on the Eye fit tab even when frameeyeosc is not running). */
struct FitInConfig {
    bool gazeFitted = false;              ///< the zero point, a gain or the tilt is not the default
    bool eyeXFitted = false;              ///< each eye's own sideways zero point and gain are set
    bool lidsFitted[2] = {false, false};  ///< all four readings of that eye are set
    gaze_fit::Values values;              ///< hasLids when both eyes are fitted
};

/** The lid fit keys, [eye][closed, up, open, down]. */
extern const char* const kLidFitKeys[2][4];

/**
 * The eye fit in the settings.
 * @param view the settings
 * @return what is fitted, and the values
 */
FitInConfig fitInConfig(const SettingsView& view);

/**
 * Whether a key the eye fit writes is set on frameeyeosc's command line (then the fit can't run).
 * @param view the settings
 * @return true if any is locked
 */
bool fitKeysLocked(const SettingsView& view);

/** Whether each eye can widen by itself (lid_widen, for eyes with an eye fit). */
struct WidenState {
    int mode = 2;                        ///< index into kLidWidenModes (0 = off)
    bool fitted[2] = {false, false};     ///< the eye has a lid fit (the setting applies to it)
    bool room[2] = {false, false};       ///< ...and its straight-ahead reading leaves room below 1.000 to widen
};

/**
 * How lid_widen works out for each eye, the way frameeyeosc decides it (widen_room): a fitted eye has room when its
 * straight-ahead open reading plus the mode's widening start is at most 0.97 (the openness stops at 1.000). An eye
 * without room widens with the other eye; neither with room: no widening.
 * @param view the settings
 * @return the state
 */
WidenState widenState(const SettingsView& view);

/**
 * Bring a settings file from 0.5.x or earlier (no lid_widen in it) up to date, once: those versions ignored
 * lid_scale_left/right for an eye with a lid fit, so a value left there (1.15, say) would suddenly move that eye now
 * that the scale fine-tunes the fit. Such scales go back to null, and lid_widen is written ("normal", the default),
 * which marks the file as done (a scale set afterwards is kept).
 * @param root the config's root object (changed in place)
 * @param log what was changed, for the log (empty if only lid_widen was added)
 * @return true if root changed (lid_widen was missing)
 */
bool migrateLidScales(JsonValue& root, std::string& log);

/**
 * Bring a settings file from before the version 2 presets (version 1 or none) up to date, once: gaze smoothing that
 * exactly matches one of the old light / medium / strong presets becomes the new preset of the same name, and if it
 * did, a deadzone at the old default (0.02) becomes the new one (0.005). Any other values are the user's own and stay.
 * version is then written as 2, which marks the file as done.
 * @param root the config's root object (changed in place)
 * @param log what was changed, for the log (empty if nothing but the version)
 * @return true if root changed (the version was older)
 */
bool migrateGazePresets(JsonValue& root, std::string& log);

/**
 * Whether a settings file still needs migrateLidScales or migrateGazePresets (no lid_widen, or a version below 2).
 * @param root the config's root object
 * @return true if one of them would change it
 */
bool configNeedsMigration(const JsonValue& root);

/** The fit run by itself when the headset is put on (auto_recenter). */
enum class AutoRecenter { Off, Center, Tilt };

/**
 * auto_recenter as written: "off", "center" or "tilt". A true / false from before it had three values reads as
 * the default / "off", and anything else as the default, "center" (the tilt from one wearing's re-wear fits
 * scattered by about ±5°, as much as it corrects).
 * @param config the config
 * @return the kind
 */
AutoRecenter autoRecenter(const ConfigFile& config);

/**
 * The re-wear fit for an auto_recenter kind: the one it runs by itself, and the one the button next to "Fit again"
 * runs (re-centering only when auto_recenter is off).
 * @param kind the kind
 * @return gaze_fit::Mode::Center or gaze_fit::Mode::Tilt
 */
gaze_fit::Mode rewearMode(AutoRecenter kind);

/**
 * Write an eye fit's result into config.json: the gaze zero point; with the side dots (the whole fit, and the
 * re-wear fit with the tilt) also the tilt; each eye's sideways values when measured; for the whole fit also the
 * gains, each eye's lid readings, and lid_scale_left/right back to null when it measured the eyelids (an old
 * tweak must not sit on a new fit).
 * @param root the config's root object
 * @param values the result
 * @param mode the mode
 */
void applyFitValues(JsonValue& root, const gaze_fit::Values& values, gaze_fit::Mode mode);

/**
 * What "Reset" on the Eye fit tab puts back to the defaults: the gaze fit, each eye's sideways values, the lid
 * readings and the per-eye lid scales (fine-tunes of the lid fit).
 * @return the keys
 */
std::vector<std::string> fitResetKeys();

/** A gaze smoothing preset (the three One Euro values). */
struct GazePreset {
    double minCutoff;
    double beta;
    double dCutoff;
};

/**
 * The three gaze presets: light, medium (= the defaults), strong.
 * @return the presets
 */
const GazePreset* gazePresets();

/**
 * Which preset the current values match.
 * @param view the settings
 * @return 0..2, or -1 for custom values
 */
int matchingGazePreset(const SettingsView& view);

/** One key and the value to write. */
struct SettingChange {
    const char* key;
    JsonValue value;
};

/**
 * The output type a SetOutput / PromptYes button stands for.
 * @param arg 0 VRChat, 1 ETVR, 2 LiveLink (anything else is VRChat)
 * @return kOutputVrchat, kOutputEtvr or kOutputLivelink
 */
const char* outputOfArg(int arg);

/**
 * The button argument of an output type (the other way round from outputOfArg).
 * @param output the "output" value
 * @return 0 VRChat, 1 ETVR, 2 LiveLink; -1 for anything else
 */
int argOfOutput(const std::string& output);

/**
 * The recommended settings of an output type (asked once after switching). Locked keys are left out.
 * VRChat and LiveLink: the default gaze and eyelid smoothing (the LiveLink module smooths nothing). ETVR: default
 * gaze smoothing and lighter eyelid smoothing, because the ETVR module already smooths the eyelids.
 * @param output kOutputVrchat, kOutputEtvr or kOutputLivelink
 * @param view the settings (to skip locked keys)
 * @return the changes
 */
std::vector<SettingChange> recommendedSettings(const std::string& output, const SettingsView& view);

/**
 * The range a lid mark may move in without passing its neighbours (closed < open <= widen start <= widest), or
 * a lid fit reading in (closed at least 0.1 below the three open readings).
 * @param name one of the four lid keys, or a lid fit key
 * @param view the settings
 * @param low lower bound (written)
 * @param high upper bound (written)
 */
void lidMarkBounds(const std::string& name, const SettingsView& view, double& low, double& high);

/**
 * Format a number of a key with its decimals (e.g. "0.30").
 * @param name the key
 * @param value the number
 * @return the text ("—" for NaN)
 */
std::string formatSetting(const std::string& name, double value);

/**
 * The IP part of an "IP:PORT" target ("[v6]:port" loses its brackets).
 * @param target the target
 * @return the host, or "" if there is none
 */
std::string hostOfTarget(const std::string& target);

/** Where the eyelids come from, as frameeyeosc reports it (the eye capture tab's line under camera_lids). */
enum class CameraUse {
    Unknown,        ///< frameeyeosc isn't running, or doesn't report the cameras (no eyecam-rec, or older)
    Off,            ///< camera_lids is off: Valve's values
    Both,           ///< the cameras drive both eyes
    Left,           ///< only the left eye (the right one: Valve's)
    Right,          ///< only the right eye
    NotCalibrated,  ///< Valve's values: not calibrated for this wear
    Warming,        ///< Valve's values: eyecam-rec is still learning the relaxed eyes for this wear
    NoCamera,       ///< Valve's values: no live camera values reach frameeyeosc
    Error,          ///< Valve's values: frameeyeosc gives a reason (CameraStatus::error)
    Valve,          ///< Valve's values, for no reason given
};

/**
 * Where the eyelids come from now. The camera values in use win over everything (camera_lids off but still used
 * means frameeyeosc hasn't caught up yet); otherwise why not: the setting, no camera values, no baseline for this
 * wear (neither calibrated nor learned by itself: still learning it while eyecam-rec says so), frameeyeosc's own
 * reason.
 * @param status frameeyeosc's status
 * @param cameraLids the camera_lids setting
 * @param warming eyecam-rec is learning the relaxed eyes now (eyecam::baselineWarming)
 * @return the case
 */
CameraUse cameraUse(const EyeStatus& status, bool cameraLids, bool warming = false);

/**
 * Whether the eye cameras drive both eyelids now (frameeyeosc's camera.used): then widening is their sensitivity, and
 * the Eyelids tab shows that in Widen's place.
 * @param status frameeyeosc's status
 * @return true while it runs and both eyes are on the cameras
 */
bool lidsFromCameras(const EyeStatus& status);

/**
 * The new release's summary shown under the update row: the Japanese one on a Japanese panel when the release text
 * has one, otherwise the English one.
 * @param update the update status
 * @param language the panel's language
 * @return the text, or "" when no newer release is available or it has no summary
 */
std::string updateNotes(const frame_updater::UpdateStatus& update, Language language);
