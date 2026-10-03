// The developer tab "Eye capture": eyecam-rec (an eye-camera recorder outside this repo) writes its state to
// $XDG_RUNTIME_DIR/eyecam/status.json about 10 times a second and takes "start" / "stop" on ctl.sock there. The
// panel only reads that file and talks to that socket; it never creates anything in that folder and never runs the
// recorder or its root helper. Everything here works without OpenVR and cairo (eyecam-test).
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
/** The command the user runs once over SSH to give the recorder the camera buffers (the panel only shows it). */
constexpr const char* kGrabCommand = "sudo /home/steamos/eyecam-src/target/release/eyecam-grab";

/** The recorder's state ("state" in status.json). */
enum class State {
    Missing,     ///< no status file (or it could not be read)
    WaitingFds,  ///< "waiting_fds": waiting for eyecam-grab to hand over the camera buffers
    Idle,        ///< "idle": ready to start
    Searching,   ///< "searching": started, looking for the eyes
    Recording,   ///< "recording": going through the steps
    Error,       ///< "error"
    Stopped,     ///< "stopped": the recorder exited
    Unknown,     ///< any other text
};

/** One step's instruction ("step_label"). */
enum class Step { LeadIn, Normal, Widen, Close, Squint, LookUp, LookDown, Bright, Dark, End, Unknown };

/** The full-view overlay during the bright and dark steps. */
enum class Fill { None, Bright, Dark };

/** status.json as read. Numbers missing from it are NaN (steps -1). */
struct Status {
    bool present = false;    ///< the file was read and parsed as an object
    std::string readError;   ///< why not (for the log)
    double mtime = 0.0;      ///< when the file was written (Unix seconds; it is replaced atomically)
    State state = State::Missing;
    std::string stateText;   ///< "state" as written
    std::string message;     ///< a short Japanese line from the recorder, shown as is ("" = none)
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
};

/** The recorder's reply to a command. */
struct Reply {
    bool ok = false;
    std::string command;  ///< "start" / "stop"
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

}  // namespace eyecam
