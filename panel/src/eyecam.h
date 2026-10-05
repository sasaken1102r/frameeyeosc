// The eye cameras tab: eyecam-rec (an eye-camera recorder outside this repo) writes its state to
// $XDG_RUNTIME_DIR/eyecam/status.json about 10 times a second and takes "start" / "stop" / "calib wear" /
// "calib user" / "set widen_sensitivity" on ctl.sock there. It reads the eyelids from the eye cameras live, for
// frameeyeosc, once it has a baseline for this wear (calibrated, or learned from the relaxed eyes by itself). Until the
// first setup is done the tab is a checklist (a password, the camera tool, the first calibration); the developer
// recording is on the Advanced tab. The panel only reads that file and talks to that socket; it never creates
// anything in that folder and never runs the recorder or its root helper. Everything here works without OpenVR and
// cairo (eyecam-test).
#pragma once

#include "i18n.h"

#include <string>
#include <vector>

namespace eyecam {

/** The tab shows while the status file was written this recently (s), and its state isn't "stopped". */
constexpr double kVisibleSec = 5.0;
/** The bright / dark overlay hides once the status file is older than this (s), so it can never stay up. */
constexpr double kOverlayStaleSec = 1.0;
/** The longest wait for the recorder's one-line reply (s). */
constexpr double kReplyTimeoutSec = 2.0;
/** The full-view light fades in over this long (s) when it appears, so the view never jumps to white or black. */
constexpr double kFadeInSec = 0.7;
/** ...and out over this long (s) when its step ends while recording goes on (also before the other color). */
constexpr double kFadeOutSec = 0.5;
/** At most this much time (s) counts toward one fade-in step, so a stalled loop never makes the light jump up. */
constexpr double kMaxFadeInStepSec = 0.1;
/** "start" alone records eyecam-rec's default protocol (widen, with the bright and dark steps). */
constexpr const char* kStartCommand = "start";
/** The protocol without the bright and dark steps, for anyone who may be sensitive to light. */
constexpr const char* kNoLightProtocol = "widen_nolight";
/** The calibration for this wear (18 s): needed each time the headset is put on. */
constexpr const char* kCalibWearCommand = "calib wear";
/** The user's own calibration (18 s): once, after a calibration for this wear. */
constexpr const char* kCalibUserCommand = "calib user";
/** calib_state's bits: calibrated for this wear (cleared when the headset comes off), and for the user. */
constexpr int kCalibWearBit = 1;
constexpr int kCalibUserBit = 2;
/** calib_state's bit for a baseline eyecam-rec learned by itself for this wear (the relaxed eyes; newer eyecam-rec). */
constexpr int kCalibAutoBit = 4;
/** How often at most the widening sensitivity is sent while its slider is dragged (s); it is sent again on release. */
constexpr double kSensitivitySendSec = 0.3;

/** The recorder's state ("state" in status.json). */
enum class State {
    Missing,     ///< no status file (or it could not be read)
    WaitingFds,  ///< "waiting_fds": waiting for eyecam-grab to hand over the camera buffers
    Idle,        ///< "idle": ready to start
    Searching,   ///< "searching": started, looking for the eyes
    Recording,   ///< "recording": going through the steps
    Calibrating, ///< "calibrating": a calibration ("calib wear" / "calib user"), also while it waits for the video
    Error,       ///< "error"
    Stopped,     ///< "stopped": the recorder exited
    Unknown,     ///< any other text
};

/** One step's instruction ("step_label"). */
enum class Step { LeadIn, Normal, Widen, Close, Squint, LookUp, LookDown, Bright, Dark, End, Unknown };

/** The full-view overlay during the bright and dark steps. */
enum class Fill { None, Bright, Dark };

/** A button in the light warning shown before a start. */
enum class StartChoice { WithLight, WithoutLight, Cancel };

/** A calibration (no light warning: it has no bright or dark steps). */
enum class Calib { Wear, User };

/** What eyecam-rec ran last, to tell a failed calibration from a failed recording. */
enum class Run { None, Recording, CalibWear, CalibUser };

/** The steps of "calib wear" after its countdown, as the chips show them (eyecam-rec's protocol). */
constexpr const char* kCalibWearSteps[] = {"close", "normal", "widen", "normal", "widen"};

/** ...and of "calib user". */
constexpr const char* kCalibUserSteps[] = {"squint", "look_up", "look_down"};

/** Whether the SteamOS user has a password ("steamos-passwd --has-password"; the setup's first step). */
enum class PasswordState {
    Unknown,  ///< not checked yet, or it can't be (not SteamOS)
    Set,
    NotSet,
};

/** How the camera tool (eyecam-grab, installed with sudo) stands. */
enum class Tool {
    Missing,   ///< not installed (or not usable: no capability, unsafe)
    TooOld,    ///< installed, but older than eyecam's safety floor: eyecam-rec doesn't start it, the cameras wait
    Outdated,  ///< installed and working, but an update brought a newer one (grab_outdated)
    Current,   ///< installed, working and up to date
};

/** What the usual page (and the left column) asks of a set-up user about the tool. */
enum class ToolNotice {
    None,
    Outdated,  ///< install it again when convenient: the one installed still works
    TooOld,    ///< install it again: until then the cameras are stopped (Valve's values only)
};

/** The setup's steps, in order; the current one is the first not met. */
enum class SetupStep {
    Password,  ///< (1) a SteamOS password, needed once for (2)
    Tool,      ///< (2) the camera tool (eyecam-grab) installed with sudo
    Learn,     ///< (3) the first calibration ("calib wear")
    Done,      ///< (4) set up: the eye cameras' usual page
};

/** What the eye cameras' tab shows. */
enum class SetupScreen {
    Pass,    ///< the checklist at (1)
    Check,   ///< ...at (2)
    Wait,    ///< ...at (3), before its button
    Learn,   ///< ...at (3), calibrating
    Error,   ///< ...at (3), the calibration failed
    Fail,    ///< ...at (3), calibrated but widening fell back to the standard values ("このまま進む" / again)
    Done,    ///< the checklist done, right after the setup's calibration (once)
    Camera,  ///< the usual page (set up)
    Calibrated,  ///< (the usual page only, never from setupScreen) a calibration from it ended well
};

/** How the setup's calibration ended, for the screen after it. */
enum class SetupResult { None, Done, Fail };

/** How a calibration started from the usual page ended, for the card after it. */
enum class CalibResult {
    None,
    Measured,  ///< "calib wear", widening measured (or an eyecam-rec that doesn't say): "校正できたよ"
    Default,   ///< "calib wear", widening on the standard values: the setup's question again
    User,      ///< "calib user" done
};

/** What the usual page shows (once set up). */
enum class PageScreen {
    Page,         ///< the page itself
    Calibrating,  ///< a calibration from it: the setup's (3) card, titled with the calibration
    Error,        ///< ...that failed (as before: the error and "again")
    Result,       ///< ...that ended: SetupFlow::calibResult
};

/** The most of status.json that is read (eyecam-rec writes one line of well under 2 KB). */
constexpr size_t kMaxStatusBytes = 64 * 1024;

/** The last look for the eyes' video ("search_detail"; for the diagnostics page). */
struct SearchDetail {
    bool known = false;       ///< "search_detail" is there (an object, or null before the first look; an older
                              ///< eyecam-rec doesn't write it)
    bool present = false;     ///< ...and an object
    int candidates = 0;       ///< picture-like frames in changed memory
    double refreshHz = 0.0;   ///< how often they were rewritten a second (median)
    int slots = 0;            ///< the ring's slots found
    bool bothEyes = false;
    std::string stoppedAt;    ///< "" (found), "no_candidates", "not_refreshing", "few_slots", "one_eye"
    int changedBlocks = -1;   ///< 64 KiB blocks of the buffers changed before it (-1 when missing)
};

/** The last "calib wear" ("last_calib"; for the diagnostics page). Numbers it doesn't have are NaN. */
struct LastCalib {
    bool known = false;         ///< "last_calib" is there (an object, or null: none yet; an older eyecam-rec doesn't
                                ///< write it)
    bool present = false;       ///< ...and an object
    std::string time;           ///< local "YYYY-MM-DD HH:MM:SS"
    bool ok = false;
    std::string failedEye;      ///< "L" / "R" (went through without it), "LR" (failed), ""
    std::string message;        ///< eyecam-rec's (Japanese)
    std::string messageEn;      ///< ...in English ("" when missing)
    double pupilFrames[2] = {0.0, 0.0};   ///< per eye (left, right): normal-step frames with the pupil
    double normalFrames[2] = {0.0, 0.0};  ///< ...normal-step frames
    double pupilX[2] = {0.0, 0.0};        ///< ...the median pupil position (px)
    double pupilY[2] = {0.0, 0.0};
    double window[2][2] = {{0.0, 0.0}, {0.0, 0.0}};  ///< ...the search window's left and right edge (px)
};

/** status.json as read. Numbers missing from it are NaN (steps -1). */
struct Status {
    bool present = false;    ///< the file was read and parsed as an object
    std::string readError;   ///< why not (for the log)
    double mtime = 0.0;      ///< when the file was written (Unix seconds; it is replaced atomically)
    State state = State::Missing;
    std::string stateText;   ///< "state" as written
    std::string message;     ///< a short Japanese line from the recorder, shown as is ("" = none)
    std::string messageEn;   ///< the same in English ("message_en"; "" when missing: then message is shown)
    bool locked = false;
    double fpsL = 0.0;       ///< NaN when missing
    double fpsR = 0.0;
    int stepIndex = -1;
    int stepCount = -1;
    std::string stepLabel;   ///< "normal", "widen", ... as written
    double stepRemainingS = 0.0;
    double elapsedS = 0.0;
    double totalS = 0.0;
    std::string sessionDir;
    std::string protocol;
    int calibState = 0;            ///< kCalibWearBit | kCalibUserBit | kCalibAutoBit (0 when missing)
    bool recalibSuggested = false; ///< drifted since the calibration: "calib wear" again
    bool live = false;             ///< the eyelids are read from the cameras live
    double liveMs = 0.0;           ///< how long one live frame took (ms; NaN when missing)
    std::string autoGrab;          ///< "auto_grab": how eyecam-rec takes the buffers by itself ("" when missing)
    bool grabOutdated = false;     ///< "grab_outdated": the installed eyecam-grab (root's copy) differs from the one
                                   ///< bundled with eyecam-rec, so an update brought a new one (false when missing)
    bool hasBaseline = false;      ///< "baseline" is there: an eyecam-rec that learns the relaxed eyes by itself
    std::string baseline;          ///< "warming" (learning them) or "ready" ("" when missing)
    double warmupRemainingS = 0.0; ///< seconds left of the learning, while warming (NaN when missing)
    bool hasCalibSaved = false;    ///< "calib_saved" is there
    bool calibSaved = false;       ///< a calibration for the wear was saved once (calib.json), so it never asks again
    bool hasWidenSensitivity = false;  ///< "widen_sensitivity" is there (the slider shows only then)
    double widenSensitivity = 0.0;     ///< 0 dull (rarely widens by itself) .. 1 sensitive (NaN when missing)
    bool hasBuffers = false;       ///< "has_buffers": eyecam-rec holds the camera buffers (false when missing)
    bool hasSetupDone = false;     ///< "setup_done" is there (a newer eyecam-rec)
    bool setupDone = false;        ///< the setup's calibration was done once
    std::string lastCalibWiden;    ///< "last_calib_widen": "measured" / "default" ("" when missing)
    std::string calibFailedEye;    ///< "calib_failed_eye": "L" / "R" (that eye's part of the last "calib wear" failed:
                                   ///< it went through, that eye on its earlier or provisional values), "LR" (both:
                                   ///< it failed), "" (none, or an eyecam-rec before it)
    bool hasPupil = false;         ///< "pupil_l" / "pupil_r" are there (a newer eyecam-rec that says whether it finds
                                   ///< the pupils; without them "locked" is all there is)
    double pupil[2] = {0.0, 0.0};  ///< per eye (left, right), the share of the last 2 s of frames with the pupil found,
                                   ///< 0..1 (NaN when null: live processing off, or that eye's video stopped)
    double prox = 0.0;             ///< "prox": the proximity sensor's reading (-1: eyecam-rec can't read it; NaN when
                                   ///< missing)
    bool hasSearch = false;        ///< "search" is there (a newer eyecam-rec that says why the video isn't found)
    std::string search;            ///< "search": not_worn / no_video / one_eye while searching unlocked, else ""
    double proxMin = 0.0;          ///< "prox_min": above it the headset counts as worn (NaN when missing)
    SearchDetail searchDetail;     ///< "search_detail"
    LastCalib lastCalib;           ///< "last_calib"
    std::string lastError;         ///< "last_error": the last error's message, kept after it ("" = none)
    std::string lastErrorEn;       ///< ...in English
    double lastErrorUnix = 0.0;    ///< ...when (Unix seconds; 0 = none)
};

/**
 * The calibrations as the panel follows them. When one that began before the setup was complete ends with the setup
 * complete, it says how (Done once, so the checklist can show its last screen; Fail while widening fell back to the
 * standard values). When one from the usual page ends at its last step, not stopped, it says how too (calibResult).
 * That is all it keeps, each tied to its run: whether the tool is in and the setup is done are read from each status,
 * so a removed tool or a setup_done gone back to false shows the checklist again at once (and drops both results:
 * they are kept only while setupStep says Done; the page's only while eyecam-rec stays idle).
 */
class SetupFlow {
public:
    /**
     * Follow eyecam-rec (each time its status is read).
     * @param status the status
     * @param now monotonic seconds (when Done began, for the left column's short notice)
     */
    void follow(const Status& status, double now);

    /** "使いはじめる", "このまま進む" or "OK": on to the usual page (eyecam-rec says setup_done after either). */
    void proceed();

    /** The dashboard closed: the checklist's done screen, and the page's "calibrated", are not shown again. */
    void closed();

    /** The panel sent "stop" (the calibration running now ends without a result). */
    void stopSent();

    /**
     * "Back" on a failed calibration's or recording's error: that error is no longer shown (eyecam-rec stays in
     * "error" until the next command, so without this the tab would show it on every visit). It is tied to the
     * error's identity, its run and message: a different one shows again, and so does any error after eyecam-rec
     * has left "error" (follow forgets the dismissal then).
     * @param run the run that failed (View::lastRun)
     * @param status the status showing the error
     */
    void dismissError(Run run, const Status& status);

    /**
     * @param run the run that failed (View::lastRun)
     * @param status the status
     * @return true if this error was dismissed with "Back"
     */
    bool errorDismissed(Run run, const Status& status) const;

    /** @return how the last calibration from the usual page ended (None once dismissed or no longer idle) */
    CalibResult calibResult() const { return calibResult_; }

    /**
     * @return how the setup's calibration ended (None once it is dismissed, another one starts, or the setup is no
     * longer complete)
     */
    SetupResult result() const { return result_; }

    /**
     * Whether the left column's "ready" notice shows: for kReadyNoticeSec after the setup was done.
     * @param now monotonic seconds
     * @return true while it shows
     */
    bool readyNotice(double now) const;

    /** How long the left column says the eye cameras are ready (s). */
    static constexpr double kReadyNoticeSec = 12.0;

private:
    bool seen_ = false;            ///< a status was followed
    bool calibrating_ = false;     ///< it was calibrating at the last one
    bool setupCalib_ = false;      ///< ...and that calibration is the setup's
    bool completeBefore_ = false;  ///< the setup was complete before it
    bool reachedEnd_ = false;      ///< ...it got to its last step
    bool stopSent_ = false;        ///< ...the panel stopped it
    Run run_ = Run::None;          ///< which calibration (followRun)
    SetupResult result_ = SetupResult::None;
    CalibResult calibResult_ = CalibResult::None;
    double doneAt_ = -1e9;
    std::string dismissed_;        ///< the error "Back" dismissed (errorKey), "" = none
};


/** The recorder's reply to a command. */
struct Reply {
    bool ok = false;
    std::string command;  ///< "start" / "stop" / "calib wear" / ...
    std::string error;    ///< "err <reason>"'s reason, or what went wrong talking to it (English); "" when ok
};

/** What the panel draws of the recorder. */
struct View {
    Status status;
    bool visible = false;   ///< the tab is shown (tabVisible)
    bool busy = false;      ///< a command is waiting for its reply
    std::string busyCommand;
    bool hasReply = false;  ///< a command was answered (or failed) since the panel started
    Reply reply;            ///< the last one
    Run lastRun = Run::None;  ///< what ran last (followRun / runOfCommand)
    PasswordState password = PasswordState::Unknown;  ///< the setup's first step, checked by the panel
    SetupFlow flow;           ///< the setup's calibration
    bool readyNotice = false; ///< the left column says "ready" (flow.readyNotice, set by the loop)
    std::string spawnError;   ///< why a Konsole for the setup didn't open ("" = none)
};

/**
 * Whether the view shows an error the user hasn't dismissed: eyecam-rec in "error" after this run (a calibration's or a
 * recording's), not dismissed with "Back".
 * @param view the recorder
 * @param recording true for a recording's error, false for a calibration's
 * @return true to show it
 */
bool errorShown(const View& view, bool recording);

/**
 * The recorder's folder: $XDG_RUNTIME_DIR/eyecam, or /run/user/<uid>/eyecam without it (e.g. over SSH).
 * @return the folder
 */
std::string defaultDir();

/**
 * Parse a state name.
 * @param text "idle" and so on
 * @return the state (Unknown for anything else, Missing for "")
 */
State parseState(const std::string& text);

/**
 * Parse a step label.
 * @param label "normal" and so on
 * @return the step (Unknown for anything else)
 */
Step parseStep(const std::string& label);

/**
 * Parse the text of status.json. Missing or odd fields keep their defaults.
 * @param text the JSON text
 * @param mtime when it was written (Unix seconds)
 * @return the status (present = false if it isn't a JSON object)
 */
Status parseStatus(const std::string& text, double mtime);

/**
 * Read dir/status.json and its modification time.
 * @param dir the recorder's folder
 * @return the status (present = false if missing or unreadable)
 */
Status readStatus(const std::string& dir);

/**
 * How old the file is, either way (a clock step backwards counts as old too).
 * @param status the status
 * @param now Unix seconds
 * @return seconds
 */
double age(const Status& status, double now);

/**
 * Whether the tab shows: the file is there, its state isn't "stopped", and it was written in the last kVisibleSec.
 * @param status the status
 * @param now Unix seconds
 * @return true if shown
 */
bool tabVisible(const Status& status, double now);

/**
 * The full-view overlay to show: white during a "bright" step, black during a "dark" one, while recording, and
 * only while the tab shows and the file was written in the last kOverlayStaleSec. Anything else hides it.
 * @param status the status
 * @param now Unix seconds
 * @return what to show
 */
Fill fillFor(const Status& status, double now);

/**
 * Whether the full-view light has to go at once, without fading: the status file is more than kOverlayStaleSec old
 * (or can't be read), the tab is gone, the recorder isn't recording any more (stopped, failed, ...), or "stop" is on
 * its way. Fading is only for a step that ends while the recording goes on.
 * @param view the view (its status, and the command waiting for its reply)
 * @param now Unix seconds
 * @return true to hide it now
 */
bool hideLightAtOnce(const View& view, double now);

/** The full-view light as shown: which one, and how opaque. */
struct Light {
    Fill fill = Fill::None;  ///< None = hidden
    double alpha = 0.0;      ///< 0..1 (the overlay's alpha)
    double at = 0.0;         ///< when it was worked out (monotonic seconds)
};

/**
 * The light a moment later. The wanted one fades in (alpha 0 -> 1 over kFadeInSec); one no longer wanted fades out
 * (to 0 over kFadeOutSec), and only then is the next one (bright <-> dark) put up, from 0. hideNow drops it at once.
 * Alpha moves by the time since light.at, so it is the same however often this is called.
 * @param light the light as last worked out
 * @param wanted what fillFor says now
 * @param hideNow hideLightAtOnce
 * @param now monotonic seconds
 * @return the light to show now
 */
Light stepLight(const Light& light, Fill wanted, bool hideNow, double now);

/**
 * Whether the light is still on its way in or out (the loop runs faster then).
 * @param light the light
 * @param wanted what fillFor says now
 * @return true while fading
 */
bool lightFading(const Light& light, Fill wanted);

/**
 * Whether a recording runs without the full-view light (its protocol is kNoLightProtocol).
 * @param status the status
 * @return true without light
 */
bool withoutLight(const Status& status);

/**
 * The command a button in the light warning sends.
 * @param choice the button
 * @return "start", "start widen_nolight", or "" for Cancel (nothing to send)
 */
std::string startCommand(StartChoice choice);

/**
 * The command for a calibration.
 * @param calib which one
 * @return "calib wear" or "calib user"
 */
std::string calibCommand(Calib calib);

/**
 * Whether a calibration for the user can start: only after one for this wear (eyecam-rec says no otherwise).
 * @param status the status
 * @return true if calibrated for this wear
 */
bool userCalibAllowed(const Status& status);

/**
 * The run a command starts, if the recorder takes it.
 * @param command the command as sent
 * @return Recording for "start ...", CalibWear / CalibUser for the calibrations, None for anything else
 */
Run runOfCommand(const std::string& command);

/**
 * What ran last, after reading the status: recording (searching too) is a recording; calibrating is a calibration,
 * for the user while its steps (squint, look_up, look_down) show, for this wear while its own (close, normal, widen)
 * show, and otherwise (the countdown, the wait for the video) the one already known (a calibration for this wear
 * if none). Other states keep the last one, so an error after a calibration is still the calibration's.
 * @param last what ran last until now
 * @param status the status
 * @return what ran last
 */
Run followRun(Run last, const Status& status);

/**
 * Whether a run is a calibration.
 * @param run the run
 * @return true for CalibWear and CalibUser
 */
bool isCalib(Run run);

/**
 * Whether eyecam-rec is learning the relaxed eyes for this wear (a newer eyecam-rec, "baseline": "warming").
 * @param status the status
 * @return true while warming
 */
bool baselineWarming(const Status& status);

/**
 * eyecam-rec's line in the panel's language: in English its message_en when there is one, else (and in Japanese)
 * its message.
 * @param status the status
 * @param language the panel's language
 * @return the line ("" = none)
 */
const std::string& shownMessage(const Status& status, Language language);

/**
 * How the camera tool stands, from this status alone:
 * - TooOld: auto_grab "too_old" (below eyecam's safety floor; eyecam-rec doesn't start it), whatever else it says.
 * - Missing: not installed. Installed means eyecam-rec holds the buffers (has_buffers, or a state that only comes after
 *   it has them: idle, searching, recording, calibrating) or auto_grab says the tool is there ("ok", "trying",
 *   "waiting_tracker", "failed: ..."; not "missing", "no_cap", "unsafe: ..."). Without auto_grab (an older eyecam-rec)
 *   only the buffers say so. A failed calibration's error still has has_buffers and auto_grab (eyecam-rec writes every
 *   field in every state), so it stays installed.
 * - Outdated: installed and working, with grab_outdated (an update brought a newer one).
 * - Current: installed and working.
 * @param status the status
 * @return the tool's state
 */
Tool toolState(const Status& status);

/**
 * Whether the camera tool is installed and works (Outdated or Current): what eyecam-rec can take the buffers with.
 * needsManualGrab is its opposite.
 * @param status the status
 * @return true if it works
 */
bool toolInstalled(const Status& status);

/**
 * What a set-up user is asked about the tool, on the usual page and in the left column: an outdated tool (still
 * working) or one below the safety floor (the cameras stopped) once the setup step is Done; nothing otherwise (before
 * that the checklist's (2) asks, as "install again"). Goes away by itself once eyecam-rec stops saying so.
 * @param status the status
 * @param password the password check
 * @return the notice
 */
ToolNotice toolNotice(const Status& status, PasswordState password);

/**
 * Whether the setup is complete: setup_done. An older eyecam-rec doesn't write it; then the setup counts as done
 * once there is any baseline (calib_state bit 0 or 2) or a saved wear calibration (calib_saved), so a user who already
 * uses the cameras is never sent back through the checklist.
 * @param status the status
 * @return true if complete
 */
bool setupComplete(const Status& status);

/**
 * The current step, from this status and password check alone: the first one not met.
 * - Set up (setupComplete): Done while the tool is there at all, outdated or too old included (the usual page asks for
 *   it then, see toolNotice); a missing one goes back to (1) / (2). An eyecam-rec without auto_grab can't say, so set
 *   up is Done there.
 * - Not set up: (2) is met only by a Current tool (an outdated or too old one is asked for again there); then (3),
 *   whatever the password check says (it was needed to install it). Without it, (1) only while the password is known
 *   to be missing, else (2).
 * @param status the status
 * @param password the password check
 * @return the step
 */
SetupStep setupStep(const Status& status, PasswordState password);

/**
 * The chips of a calibration: its steps after the countdown (lead_in), when step_count matches its protocol
 * (status.json gives only the count, the index and the label: the steps are eyecam-rec's, per command), else none.
 * @param status the status
 * @param calib which calibration
 * @return the step labels ("close", ...; empty = no chips)
 */
std::vector<std::string> calibChips(const Status& status, Calib calib);

/**
 * Which calibration a run is.
 * @param run the run (anything but CalibUser is this wear's)
 * @return the calibration
 */
Calib calibOf(Run run);

/**
 * What the usual page shows: a calibration running, its error (with its run), how the last one ended, or the page.
 * Only once setupScreen says Camera.
 * @param view the recorder
 * @return the screen
 */
PageScreen pageScreen(const View& view);

/**
 * What the eye cameras' tab shows: how the setup's calibration ended while that is still to be shown, else the
 * current step (at (3): calibrating, the calibration's error, or the button), else the usual page.
 * @param view the recorder (its status, password check, flow and last run)
 * @return the screen
 */
SetupScreen setupScreen(const View& view);

/**
 * The command that sets the widening sensitivity (eyecam-rec takes it in every state, applies it at once and keeps
 * it).
 * @param value 0 dull .. 1 sensitive; kept within 0..1
 * @return "set widen_sensitivity 0.60" (two decimals), or "" for a value that isn't a number
 */
std::string sensitivityCommand(double value);

/**
 * The widening sensitivity on its way to eyecam-rec: the value let go of is always sent; while the slider is
 * dragged, at most every kSensitivitySendSec and only when it moved. One command at a time, so a value waits while
 * another command is out, and only the newest one goes.
 */
class SensitivitySender {
public:
    /**
     * The slider was let go of.
     * @param value its value
     */
    void released(double value);

    /**
     * The slider is being dragged.
     * @param value its value now
     * @param now monotonic seconds
     */
    void dragged(double value, double now);

    /**
     * The command to send now, if any.
     * @param busy another command waits for its reply
     * @param now monotonic seconds
     * @param command where to write it
     * @return true if there is one (it counts as sent)
     */
    bool next(bool busy, double now, std::string& command);

private:
    bool pending_ = false;
    double value_ = 0.0;
    bool sent_ = false;      ///< something was sent (lastValue_ / lastAt_ are set)
    double lastValue_ = 0.0;
    double lastAt_ = 0.0;
};

/**
 * Whether a reply is to the widening sensitivity (its error puts the slider back to the file's value).
 * @param command the command answered
 * @return true for "set widen_sensitivity ..."
 */
bool isSensitivityCommand(const std::string& command);

/**
 * The light warning before a start: the start button (idle) and the retry button (error) open it instead of
 * starting; its buttons start with or without the light, or close it. It closes by itself once the recorder's state
 * is no longer the one it was opened in, or the tab is hidden.
 */
class StartConfirm {
public:
    /**
     * Open it (only in idle or error).
     * @param state the recorder's state now
     * @return true if it opened
     */
    bool open(State state);

    /**
     * Close it if the state left the one it was opened in, or the tab isn't on screen.
     * @param state the recorder's state now
     * @param tabShown the eye capture tab is there and is the one shown
     * @return true if it closed in this call (redraw)
     */
    bool sync(State state, bool tabShown);

    /**
     * A button in it was pressed: it closes.
     * @param choice the button
     * @return the command to send ("" = none)
     */
    std::string choose(StartChoice choice);

    /** Close it. */
    void close() { open_ = false; }

    /** @return true while it shows */
    bool isOpen() const { return open_; }

private:
    bool open_ = false;
    State openedIn_ = State::Missing;
};

/**
 * The instruction for a step, in the panel's language.
 * @param t the text table
 * @param label the step label as written (an unknown one is shown as is)
 * @return the text
 */
std::string instruction(const UiText& t, const std::string& label);

/**
 * Read the recorder's one-line reply: "ok", or "err <reason>".
 * @param line the line (a trailing newline is ignored)
 * @param command the command it answers
 * @return the reply (an empty or other line is an error that says so)
 */
Reply parseReply(const std::string& line, const std::string& command);

/**
 * A short text of what the tab shows, rounded as drawn, so the panel is only redrawn when it changes.
 * @param view the view
 * @return the text
 */
std::string signature(const View& view);

/**
 * Sends one command to ctl.sock and waits for its reply without blocking the caller: send() connects and writes
 * (a unix socket takes it at once or fails), poll() reads whatever has come back. Only one command at a time.
 */
class Control {
public:
    Control() = default;
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;

    /**
     * Send a command ("start" / "stop"). Fails at once (a reply with the reason) if the socket can't be reached.
     * @param socketPath the socket
     * @param command the command, without the newline
     * @param now monotonic seconds
     * @param timeoutSec the longest wait for the reply
     * @return false if another command is still waiting, or it failed at once (see reply())
     */
    bool send(const std::string& socketPath, const std::string& command, double now,
              double timeoutSec = kReplyTimeoutSec);

    /**
     * Read the reply if it has come, or give up after the timeout. Never waits.
     * @param now monotonic seconds
     * @return true when a command finished in this call (reply() is new)
     */
    bool poll(double now);

    /** @return true while a command waits for its reply */
    bool busy() const { return fd_ >= 0; }

    /** @return the command waiting, or the last one */
    const std::string& command() const { return command_; }

    /** @return true once a command has finished */
    bool hasReply() const { return hasReply_; }

    /** @return the last finished command's reply */
    const Reply& reply() const { return reply_; }

private:
    int fd_ = -1;
    std::string command_;
    std::string received_;
    double deadline_ = 0.0;
    bool hasReply_ = false;
    Reply reply_;

    /**
     * Close the socket and keep the reply.
     * @param reply the reply
     */
    void finish(const Reply& reply);
};

/** The eyes as bits (failedEyes): the left one... */
constexpr int kLeftEyeBit = 1;
/** ...and the right one. */
constexpr int kRightEyeBit = 2;

/** At least this share of frames with the pupil: the eye is seen ("見えてるよ"). */
constexpr double kPupilSeenShare = 0.5;
/** Under this share, with the video there: the pupil isn't found (the calibration needs about 18 %). */
constexpr double kPupilMissingShare = 0.2;

/** How one eye looks to eyecam-rec, as the setup's (3) shows it. */
enum class EyeSight {
    NotSeen,  ///< no video ("locked" false), or with live processing on, that eye's video stopped (pupil null)
    NoPupil,  ///< the video is there, but the pupil is found in under kPupilMissingShare of the frames
    Weak,     ///< ...found, but in under kPupilSeenShare of them
    Seen,     ///< the video and the pupil (or an eyecam-rec that doesn't say, or live processing off: the video)
};

/**
 * How one eye looks to eyecam-rec: from "locked" and that eye's pupil share. An eyecam-rec without pupil_l / pupil_r
 * (or with live processing off, when they are null) says only "locked": seen or not, as before.
 * @param status the status
 * @param eye 0 left, 1 right
 * @return how it looks
 */
EyeSight eyeSight(const Status& status, int eye);

/** Why eyecam-rec hasn't found the eyes' video while it searches ("search"). */
enum class Search {
    None,     ///< found (locked), not searching, not looked yet, or an eyecam-rec that doesn't say
    NotWorn,  ///< the proximity sensor says the headset is off, and no video was found
    NoVideo,  ///< worn (or the sensor unreadable), but no eye video in the buffers (eye tracking off?)
    OneEye,   ///< only one camera's video
};

/**
 * Why the eyes' video isn't found, from "search" (only while "locked" is false).
 * @param status the status
 * @return the reason, or None
 */
Search searchReason(const Status& status);

/**
 * Whether the eyes' video is coming in: "locked", or a frame rate above 0 (holding the buffers alone isn't enough).
 * @param status the status
 * @return true while frames come
 */
bool videoFlowing(const Status& status);

/**
 * One short line saying why the eyes' video isn't found (searchReason), with the proximity reading for NotWorn.
 * @param t the texts
 * @param status the status
 * @return the line, or "" for Search::None
 */
std::string searchText(const UiText& t, const Status& status);

/**
 * Whether an eye's video is there but its pupil isn't found well (NoPupil or Weak): the setup's (3) asks to put the
 * headset on again before calibrating.
 * @param status the status
 * @return true for either eye
 */
bool pupilTrouble(const Status& status);

/**
 * Which eyes a failed calibration names: eyecam-rec's (Japanese) message says "両目" for both, "左目" / "右目" for
 * each (also an older eyecam-rec's "右目: ..."). Read from the message, not calib_failed_eye: a calibration that
 * failed before its fit (the video lost) names no eye, and leaves calib_failed_eye as it was.
 * @param status the status (only an error names eyes)
 * @return kLeftEyeBit | kRightEyeBit, or 0
 */
int failedEyes(const Status& status);

/**
 * The eye a "calib wear" went through without (calib_failed_eye "L" / "R"): it is on its earlier values, or
 * provisional ones.
 * @param status the status
 * @return 0 left, 1 right, -1 none (also "LR": that one failed)
 */
int partialEye(const Status& status);

/**
 * Whether partialEye's eye got provisional values (there were no earlier ones): eyecam-rec's message says "仮の値".
 * @param status the status
 * @return true for provisional, false for its previous values
 */
bool partialProvisional(const Status& status);

/**
 * Whether waiting_fds needs the user to run eyecam-grab with sudo: the tool is missing or too old (not toolInstalled).
 * Otherwise it is waiting for the eye tracker or retrying on its own, and the command would only confuse.
 * @param s the status as read
 * @return true to show the sudo command
 */
bool needsManualGrab(const Status& s);

}  // namespace eyecam
