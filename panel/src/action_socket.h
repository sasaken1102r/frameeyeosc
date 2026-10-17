// Actions from outside the panel: `frameeyeosc-panel --action NAME` asks the resident panel to do what one of its
// buttons does (start the gaze re-center or the eye fit, stop it, relearn the relaxed eyes on the eye cameras, open
// the panel), for other apps on the Frame (the
// aux button of frame-aux-shortcuts: press for the re-center, hold for the eye fit). The resident panel listens on a
// Unix stream socket in $XDG_RUNTIME_DIR (only the same user can reach it: the folder is the user's, the socket is
// 0600 and each connection's uid is checked); the command connects, sends one line, waits for one line and exits with
// its code. It never starts the panel. The panel answers on its main loop without waiting (nothing here blocks it),
// and what an action may do is decided here from the same state the panel's buttons look at, so all of it is tested
// without OpenVR (action_socket_test.cpp).
//
// The line protocol (ASCII, one line each way, then the panel closes the connection):
//   request: the action's name ("recenter", "fit", "stop", "relearn", "open"), at most kMaxLine bytes with the
//            newline
//   reply:   the exit code, a space and a short English text ("0 started", "6 busy (a fit is running; ...)")
// An unknown name is answered with code 1 (a newer command talking to an older panel); anything longer than kMaxLine,
// or not a line at all, is dropped without an answer.
#pragma once

#include <string>
#include <vector>

namespace action_socket {

/** The socket's name in $XDG_RUNTIME_DIR (next to the panel's single-instance lock). */
constexpr const char* kSocketName = "frameeyeosc-panel.sock";
/** The longest a request or reply line may be (bytes, with the newline). Longer ones are not read. */
constexpr size_t kMaxLine = 128;
/** How long the command waits for the panel's answer (ms). The panel answers within one pass of its loop (at most
 *  0.25 s apart, with the dashboard closed), "relearn" once eyecam-rec has answered (kEyecamReplySec at most). */
constexpr int kReplyTimeoutMs = 2000;
/** How long the panel waits for eyecam-rec's answer to "relearn" (s), so its own answer stays within kReplyTimeoutMs.
 *  eyecam-rec answers in tens of ms. */
constexpr double kEyecamReplySec = 1.0;
/** How long the panel keeps a connection that hasn't sent its line (s), so a stuck client can't pile up. */
constexpr double kRequestTimeoutSec = 1.0;
/** At most this many connections wait for their line at once; more are closed at once. */
constexpr size_t kMaxClients = 8;

/** The exit codes of `--action` (fixed: other apps show a message per code; new ones only go at the end). Every usage
 *  error of the program, not only of --action, exits with Usage. */
enum class Code {
    Accepted = 0,      ///< the action started (stop: it stopped something; open: the panel was shown)
    Usage = 1,         ///< an unknown action, an unknown option or bad arguments
    NotRunning = 2,    ///< the panel isn't running (no socket, or nobody listening on it)
    NoAnswer = 3,      ///< the panel didn't answer in time, or not in this protocol
    NoFrameeyeosc = 4, ///< frameeyeosc isn't running
    NotFitted = 5,     ///< the re-center needs a gaze fit first
    BusyFit = 6,       ///< an eye fit is running (send stop first)
    NothingToStop = 7, ///< stop, with no fit or re-center running
    Locked = 8,        ///< the fit's values are set on frameeyeosc's command line (the panel's fit buttons are grey)
    NoEyeData = 9,     ///< frameeyeosc runs, but no eye data comes in (the headset off, or eye tracking stopped)
    BusyRecenter = 10, ///< a re-center is running (also the automatic one after putting the headset on)
    NoCameras = 11,    ///< relearn: the eye cameras aren't in use or not ready (off, not set up, eyecam-rec not running
                       ///< or busy, no eye video), or eyecam-rec said no
};

/** The actions (more can follow: pause / resume sending, the eye cameras' calibration, the gaze dots). */
enum class Action {
    Recenter,  ///< the gaze re-center, as the "Re-center only" button (the middle dot, ~2.5 s)
    Fit,       ///< the eye fit, as "Fit again" (~20 s)
    Stop,      ///< stop a running fit or re-center, as its "Stop"
    Open,      ///< open the panel, as a second launch does
    Relearn,   ///< relearn the relaxed eyes on the eye cameras (~30 s), as "Relearn my eyes" on the Eye cameras tab
};

/**
 * The action with this name.
 * @param name as on the command line and in a request ("recenter", "fit", "stop", "open")
 * @param action set if known
 * @return false for an unknown name
 */
bool parseAction(const std::string& name, Action& action);

/**
 * An action's name.
 * @param action the action
 * @return its name ("recenter", ...)
 */
const char* actionName(Action action);

/** @return the names joined with '|' for the usage line ("recenter|fit|stop|relearn|open") */
std::string actionNames();

/** What the panel's buttons look at, at the moment a request comes in. */
struct State {
    bool running = false;     ///< frameeyeosc runs (status.json is fresh and its process alive)
    bool tracking = false;    ///< ...and eye data comes in
    bool locked = false;      ///< a fit value is set on frameeyeosc's command line (fitKeysLocked)
    bool gazeFitted = false;  ///< the gaze has been fitted (FitInConfig::gazeFitted)
    bool fitActive = false;   ///< a fit or re-center runs (gaze_fit::Session::active)
    bool fitIsRecenter = false;  ///< ...and it is a re-center (the middle dot only)
    /** The eye cameras drive the eyelids (or learn to) and eyecam-rec reads them live, idle with the eye video
     *  (eyecam::relearnAllowed: what the "Relearn my eyes" button needs) */
    bool camerasReady = false;
};

/** What the panel then does. */
enum class Effect {
    None,           ///< nothing (the answer says why)
    StartRecenter,  ///< start the re-center (gaze_fit::Mode::Center)
    StartFit,       ///< start the whole eye fit (gaze_fit::Mode::Full)
    StopFit,        ///< stop the running one, as "Stop" does
    ShowPanel,      ///< open the dashboard on the panel, as a second launch does
    Relearn,        ///< send "relearn" to eyecam-rec, and answer with what it says (relearnAnswer)
};

/** The answer to a request, and what to do for it. */
struct Decision {
    Code code = Code::Accepted;
    std::string text;  ///< the short English text of the reply ("started", "nothing to stop", ...)
    Effect effect = Effect::None;
};

/**
 * Decide what an action does now, with the conditions of the button it stands for: the fit buttons need frameeyeosc
 * running and their values not locked, and not while a fit runs (they are hidden then; only "Stop" shows). The
 * re-center also needs a gaze fit, like the automatic one after putting the headset on, and both need eye data.
 * Relearning needs the eye cameras ready, and waits for a fit too (the fit measures the eyelids). Checked in that
 * order: busy, frameeyeosc, eye data, then the action's own.
 * @param action the action
 * @param state the panel's state now
 * @return the code, the reply's text and what to do
 */
Decision decide(Action action, const State& state);

/**
 * The answer to "relearn" once eyecam-rec has replied to it.
 * @param ok it said "ok"
 * @param error otherwise why ("err"'s reason, Japanese; or what went wrong talking to it, English)
 * @return Accepted, or NoCameras with a short English text
 */
Decision relearnAnswer(bool ok, const std::string& error);

/**
 * The reply line for an answer.
 * @param code the exit code
 * @param text its text (kept to printable ASCII and cut to fit kMaxLine)
 * @return the line, with its newline
 */
std::string formatReply(Code code, const std::string& text);

/**
 * Read a reply line.
 * @param line the line (with or without its newline)
 * @param code set to the exit code in it
 * @param text set to its text
 * @return false if it isn't a reply (no code, or an unknown one): the command then exits with NoAnswer
 */
bool parseReply(const std::string& line, int& code, std::string& text);

/**
 * The line the command prints to stderr for a result, e.g. "recenter: started".
 * @param action the action's name as given
 * @param text the result's text
 * @return the line, without a newline
 */
std::string resultLine(const std::string& action, const std::string& text);

/**
 * The socket's path: $XDG_RUNTIME_DIR/frameeyeosc-panel.sock, or /run/user/<uid>/ without it (e.g. over SSH).
 * @return the path, or "" when neither folder is there (the panel then doesn't listen)
 */
std::string defaultSocketPath();

/** The outcome of one request from the command's side. */
struct Result {
    int code = static_cast<int>(Code::NoAnswer);
    std::string text;  ///< the panel's text, or why there was none
};

/**
 * Send one request and wait for its answer (the command's side; blocks up to timeoutMs).
 * @param socketPath the panel's socket
 * @param action the action's name (sent as given, so a newer panel's actions can be tried too)
 * @param timeoutMs the longest wait for the answer
 * @return the exit code and the text: NotRunning if nothing listens there, NoAnswer if no proper answer came in time
 */
Result request(const std::string& socketPath, const std::string& action, int timeoutMs = kReplyTimeoutMs);

/**
 * Run `--action` from the command line's arguments after the program name (the caller prints the line to stderr).
 * @param args the arguments (exactly "--action" and a name)
 * @param socketPath the panel's socket
 * @param timeoutMs the longest wait for the answer
 * @param line set to the one line to print ("recenter: started")
 * @return the exit code
 */
int runCommand(const std::vector<std::string>& args, const std::string& socketPath, int timeoutMs, std::string& line);

/** A request taken in by the panel, to be answered with Server::answer. */
struct Request {
    int id = 0;          ///< which connection to answer
    Action action = Action::Open;
    int peerPid = 0;     ///< who sent it (SO_PEERCRED), for the log
};

/**
 * The panel's side: a listening socket polled from the main loop. Never blocks: connections are taken in, read and
 * answered without waiting (a reply line always fits a Unix socket's buffer).
 */
class Server {
public:
    Server() = default;
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /**
     * Listen. Only the single resident panel (the one holding the instance lock) calls this, so a socket file already
     * there was left by a panel that crashed and is replaced. Anything else at that path is left alone.
     * @param path the socket's path
     * @param error why not, if it fails
     * @return true if listening
     */
    bool open(const std::string& path, std::string& error);

    /** Stop listening, close the waiting connections and remove the socket file (if it is still ours). */
    void close();

    /** @return true while listening */
    bool isOpen() const { return fd_ >= 0; }

    /**
     * Take in new connections and read what they sent. Unknown actions are answered here (Usage); requests from
     * another user, too long lines and connections silent for kRequestTimeoutSec are closed without an answer.
     * @param now monotonic seconds
     * @return the requests complete now, each to be answered with answer()
     */
    std::vector<Request> poll(double now);

    /**
     * Answer a request from poll() and close its connection.
     * @param id the request's id
     * @param code the exit code
     * @param text the reply's text
     */
    void answer(int id, Code code, const std::string& text);

    /** @return lines to log about what poll() dropped or answered by itself (cleared when read) */
    std::vector<std::string> takeLog();

private:
    /** A connection waiting for its line or its answer. */
    struct Client {
        int id = 0;
        int fd = -1;
        int pid = 0;
        std::string received;
        double since = 0.0;
        bool taken = false;  ///< its request went to the caller; waiting for answer()
    };
    int fd_ = -1;
    std::string path_;
    unsigned long long inode_ = 0;  ///< the socket file bound, so close() only removes our own
    int nextId_ = 1;
    std::vector<Client> clients_;
    std::vector<std::string> log_;

    /**
     * Send a reply and close a connection.
     * @param client the connection
     * @param code the exit code
     * @param text the reply's text
     */
    static void reply(Client& client, Code code, const std::string& text);

    /** Drop the closed connections from the list. */
    void prune();
};

}  // namespace action_socket
