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

/** The steps of "calib wear" after its countdown, as the setup checklist shows them (eyecam-rec's protocol). */
constexpr const char* kCalibWearSteps[] = {"close", "normal", "widen", "normal", "widen"};

/** Whether the SteamOS user has a password ("steamos-passwd --has-password"; the setup's first step). */
enum class PasswordState {
    Unknown,  ///< not checked yet, or it can't be (not SteamOS)
    Set,
    NotSet,
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
};

/** How the setup's calibration ended, for the screen after it. */
enum class SetupResult { None, Done, Fail };

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
};

/**
 * The setup's calibration as the panel follows it: when a calibration that began before the setup was complete ends
 * with the setup complete, it says how (Done once, so the checklist can show its last screen; Fail while widening fell
 * back to the standard values). That is all it keeps: whether the tool is in and the setup is done are read from each
 * status, so a removed tool or a setup_done gone back to false shows the checklist again at once (and drops a result:
 * it is kept only while setupStep says Done).
 */
class SetupFlow {
public:
    /**
     * Follow eyecam-rec (each time its status is read).
     * @param status the status
     * @param now monotonic seconds (when Done began, for the left column's short notice)
     */
    void follow(const Status& status, double now);

    /** "使いはじめる" or "このまま進む": on to the usual page (eyecam-rec says setup_done after either). */
    void proceed();

    /** The dashboard closed: the checklist's done screen is not shown again. */
    void closed();

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
    SetupResult result_ = SetupResult::None;
    double doneAt_ = -1e9;
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
 * Whether the camera tool is in, from this status alone: eyecam-rec holds the buffers (has_buffers, or a state that only
 * comes after it has them: idle, searching, recording, calibrating), or auto_grab says the tool is there ("ok",
 * "trying", "waiting_tracker", "failed: ..."; not "missing", "no_cap", "unsafe: ..."). Without auto_grab (an older
 * eyecam-rec) only the buffers say so. A failed calibration's error still has has_buffers and auto_grab (eyecam-rec
 * writes every field in every state), so it stays in. needsManualGrab is its opposite.
 * @param status the status
 * @return true if installed
 */
bool toolInstalled(const Status& status);

/**
 * Whether the setup is complete: setup_done. An older eyecam-rec doesn't write it; then the setup counts as done
 * once there is any baseline (calib_state bit 0 or 2) or a saved wear calibration (calib_saved), so a user who already
 * uses the cameras is never sent back through the checklist.
 * @param status the status
 * @return true if complete
 */
bool setupComplete(const Status& status);

/**
 * The current step, from this status and password check alone: the first one not met. The tool not in is (1) only
 * while the password is known to be missing, else (2), even when set up (an eyecam-rec without auto_grab can't say,
 * so set up is Done there); the tool in is Done when complete, else (3), whatever the password check says (it was
 * needed to install it).
 * @param status the status
 * @param password the password check
 * @return the step
 */
SetupStep setupStep(const Status& status, PasswordState password);

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

/**
 * Whether waiting_fds needs the user to run eyecam-grab with sudo: the tool is not in (toolInstalled, the same rule:
 * "auto_grab" missing, no_cap or unsafe: ..., or an eyecam-rec without it). Otherwise it is waiting for the eye
 * tracker or retrying on its own, and the command would only confuse.
 * @param s the status as read
 * @return true to show the sudo command
 */
bool needsManualGrab(const Status& s);

}  // namespace eyecam
