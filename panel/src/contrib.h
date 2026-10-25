// "Help train the AI" (Advanced tab, Debug tools): eye-camera recordings a user makes for the developer, to tune and
// train eyecam's half-shut check (the eyelid veto's CNN). Three recordings of eyecam's protocol without light, each in
// a wear of its own (the headset taken off and put back on before the next one), then one of free use; a second free
// use after them is optional. eyecam-rec records each into ~/eyecam/rec_<date>/ as always (with its cues.csv,
// valve.csv, frames.csv and meta.txt); once it has ended the panel moves the folder into the contribution's folder
// ~/eyecam/contrib_<date>/, whose info.txt keeps the progress (so it goes on after the panel or the Frame restarts) and
// says what is in it for the developer (no personal data). The folder can be packed into one file and deleted from the
// panel. Everything here works without OpenVR and cairo (contrib-test).
#pragma once

#include "eyecam.h"

#include <atomic>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace contrib {

/** The guided recordings' protocol (eyecam's protocol_widen_nolight.txt: no full-view light). */
constexpr const char* kGuidedProtocol = "widen_nolight";
/** The free use's (protocol_free.txt: 180 s of "normal", no prompts). */
constexpr const char* kFreeProtocol = "free";
/** How long the guided protocol's steps take (s; the lead-in included). */
constexpr double kGuidedSec = 59.0;
/** ...and the free use's. */
constexpr double kFreeSec = 180.0;
/** eyecam-rec records this much past a protocol's last step (s), for the end beep. */
constexpr double kTailSec = 1.5;
/** The steps everyone is asked for: three guided, one free use. */
constexpr int kRequiredSteps = 4;
/** ...and the optional second free use after them. */
constexpr int kStepCount = 5;
/** What eyecam-rec writes per frame and eye (a 400x400 grey frame, its header, its rows), as eyecam-rec estimates. */
constexpr double kFrameBytes = 160300.0;
/** The eye cameras' usual rate, for the estimates while eyecam-rec doesn't say. */
constexpr double kDefaultFps = 90.0;
/** Space left free after everything (eyecam-rec stops a recording with less than 1 GiB left). */
constexpr long long kMarginBytes = 1LL << 30;
/** The packed file is at most this share of the recordings (zstd -3 makes the eye video about 0.62 of its size). */
constexpr double kPackShare = 0.75;
/** The headset counts as taken off once the eye video stopped (or the sensor read off) this long (s)... */
constexpr double kOffSec = 3.0;
/** ...and as put back on once the video (or the sensor) was back this long after that (s). */
constexpr double kOnSec = 2.0;
/** "I put it back on" can be pressed this long after a step began waiting for the re-wear (s). */
constexpr double kFallbackSec = 20.0;
/** A start that eyecam-rec doesn't show running within this long (s) didn't start. */
constexpr double kStartGraceSec = 8.0;
/** eyecam-rec silent (its status not fresh) this long (s) ends a recording as failed. */
constexpr double kGoneSec = 10.0;
/** The contribution's file in its folder. */
constexpr const char* kInfoFile = "info.txt";
/** Where tries that didn't finish go in it (not packed; deleted with it). */
constexpr const char* kAbortedDir = "aborted";

/** How the re-wear before a step was known (info.txt). */
constexpr const char* kWearFirst = "first";        ///< the first step: the wear the user consented in
constexpr const char* kWearDetected = "detected";  ///< seen: off (the eye video or the sensor), then on again
constexpr const char* kWearButton = "button";      ///< "I put it back on" pressed

/** One step as info.txt keeps it. */
struct Step {
    std::string protocol;  ///< the protocol recorded ("" = not done)
    std::string rec;       ///< its recording's folder in the contribution's folder ("" = not done)
    long long bytes = 0;   ///< its size
    std::string wear;      ///< how the re-wear before it was known (kWear...)
    std::string done;      ///< when it ended (local "YYYY-MM-DD HH:MM:SS")
};

/** info.txt: the contribution's progress, and what is in it for the developer. */
struct Info {
    std::string appVersion;   ///< the panel's version at the consent
    std::string consent;      ///< when the user agreed (local "YYYY-MM-DD HH:MM:SS +zzzz")
    Step steps[kStepCount];
    std::string nextWear;     ///< how the re-wear before the next step was known ("" = not yet; kWear...)
    int running = -1;         ///< the step whose recording was started (-1 = none)
    double runningSince = 0;  ///< ...when its start was sent (Unix seconds)
    std::vector<std::string> aborted;  ///< tries that didn't finish ("rec_... stopped"), kept in aborted/
    std::string packed;       ///< the one file packed from it, in its folder ("" = none)
    long long packedBytes = 0;
};

/**
 * The protocol of a step.
 * @param step 0 .. kStepCount - 1
 * @return kGuidedProtocol for the first three, kFreeProtocol after them
 */
const char* protocolOf(int step);

/**
 * Whether a step is free use.
 * @param step the step
 * @return true for 3 and 4
 */
bool isFree(int step);

/**
 * Whether a step waits for the headset to be put on again first: all but the first.
 * @param step the step
 * @return true for 1 and on
 */
bool needsRewear(int step);

/**
 * What a step's recording takes on the disk (eyecam-rec's own estimate: both eyes, every frame).
 * @param step the step
 * @param fps the eye cameras' frame rate (kDefaultFps when not a number above 0)
 * @return bytes
 */
long long estimateBytes(int step, double fps);

/**
 * The step to do next: the first of the required ones not done; once they are, the optional one when it is asked for
 * (extra) and not done.
 * @param info the progress
 * @param extra the optional free use was asked for
 * @return the step, or kStepCount when there is none
 */
int nextStep(const Info& info, bool extra);

/**
 * How many of the required steps are done.
 * @param info the progress
 * @return 0 .. kRequiredSteps
 */
int requiredDone(const Info& info);

/**
 * The recordings' size so far.
 * @param info the progress
 * @return bytes
 */
long long recordedBytes(const Info& info);

/**
 * The space still needed for the required steps not done, with kMarginBytes.
 * @param info the progress
 * @param fps the eye cameras' frame rate
 * @return bytes
 */
long long neededBytes(const Info& info, double fps);

/**
 * Whether a step's recording fits: its estimate and kMarginBytes are free (or the free space isn't known).
 * @param step the step
 * @param freeBytes the free space (-1 = unknown)
 * @param fps the eye cameras' frame rate
 * @return true if it fits
 */
bool fits(int step, long long freeBytes, double fps);

/**
 * What packing needs free: kPackShare of the recordings (all of them without zstd) and kMarginBytes.
 * @param info the progress
 * @param compressed packed with zstd
 * @return bytes
 */
long long packNeedBytes(const Info& info, bool compressed);

/**
 * info.txt's text.
 * @param info the progress
 * @return the text (a comment, then "key=value" lines)
 */
std::string formatInfo(const Info& info);

/**
 * Read info.txt's text: "key=value" lines, "#" comments; unknown keys and odd lines are skipped.
 * @param text the text
 * @param info filled in
 * @return false if it has no consent (not a contribution's)
 */
bool parseInfo(const std::string& text, Info& info);

/**
 * Whether a name is a contribution's folder: "contrib_YYYY-MM-DD_HH-MM-SS".
 * @param name the name
 * @return true if it is
 */
bool isFolderName(const std::string& name);

/**
 * Whether a name is eyecam-rec's recording folder: "rec_YYYY-MM-DD_HH-MM-SS".
 * @param name the name
 * @return true if it is
 */
bool isRecordingName(const std::string& name);

/**
 * A contribution's folder name for a time.
 * @param when the time
 * @return "contrib_YYYY-MM-DD_HH-MM-SS" (local)
 */
std::string folderName(std::time_t when);

/**
 * A local time as info.txt writes it.
 * @param when the time
 * @param zone with the time zone's offset (" +0900")
 * @return "YYYY-MM-DD HH:MM:SS" (with the offset)
 */
std::string localTime(std::time_t when, bool zone);

/**
 * Where eyecam-rec records ($HOME/eyecam), and where the contributions go.
 * @return the folder
 */
std::string defaultHome();

/**
 * The contribution to show: the newest folder in home with a contribution's name and an info.txt that reads.
 * @param home the folder
 * @return its path, or "" for none
 */
std::string findCurrent(const std::string& home);

/**
 * Read a contribution's info.txt.
 * @param dir its folder
 * @param info filled in
 * @param error why not
 * @return true if read
 */
bool readInfo(const std::string& dir, Info& info, std::string& error);

/**
 * Write a contribution's info.txt (a temporary file, then renamed over it).
 * @param dir its folder
 * @param info the progress
 * @param error why not
 * @return true if written
 */
bool writeInfo(const std::string& dir, const Info& info, std::string& error);

/**
 * Start a contribution: its folder in home, and info.txt with the consent.
 * @param home where it goes
 * @param version the panel's version
 * @param now the time of the consent
 * @param dir its folder
 * @param info its progress
 * @param error why not
 * @return true if made
 */
bool create(const std::string& home, const std::string& version, std::time_t now, std::string& dir, Info& info,
            std::string& error);

/**
 * The free space where a folder is (for the user: what an unprivileged user can fill).
 * @param path the folder
 * @return bytes, or -1 if it can't be read
 */
long long freeBytes(const std::string& path);

/**
 * The size of the regular files in a folder and its folders (links not followed).
 * @param path the folder
 * @return bytes
 */
long long folderBytes(const std::string& path);

/**
 * Move a recording eyecam-rec has finished into the contribution's folder (aborted/ for one that didn't finish). Only
 * one of eyecam-rec's own folders, right in home ("rec_<date>"), is moved.
 * @param home eyecam-rec's folder
 * @param dir the contribution's folder
 * @param sessionDir the recording's folder (status.json's session_dir)
 * @param aborted into aborted/
 * @param name its name there (aborted/ included)
 * @param error why not
 * @return true if moved
 */
bool takeRecording(const std::string& home, const std::string& dir, const std::string& sessionDir, bool aborted,
                   std::string& name, std::string& error);

/** How a recording of a step ended. */
enum class Outcome {
    Running,  ///< not yet
    Done,     ///< it ran its full time
    Stopped,  ///< stopped early (the user's "Stop", the eyes lost): its folder (if any) holds part of it
    Failed,   ///< eyecam-rec failed, refused, went away, or never started it
};

/** Why a try didn't finish (the screen says it). */
enum class Why { None, Stopped, Error, Refused, Gone, NoStart, Missing, Move };

/** A recording's end. */
struct RunEnd {
    Outcome outcome = Outcome::Running;
    std::string dir;        ///< its folder ("" = none was made)
    Why why = Why::None;
    std::string message;    ///< eyecam-rec's (Japanese) or the reply's reason
    std::string messageEn;  ///< eyecam-rec's in English ("" = none)
};

/**
 * Follows a step's recording from the moment its start went out until eyecam-rec has ended it, from status.json alone
 * (also after the panel restarted in the middle of it: began again with the time the start was sent).
 */
class RunWatch {
public:
    /**
     * A start was sent.
     * @param step the step
     * @param protocol its protocol
     * @param sentUnix when (Unix seconds)
     */
    void begin(int step, const std::string& protocol, double sentUnix);

    /** It ended (or was given up). */
    void end() { step_ = -1; }

    /** @return true while following one */
    bool active() const { return step_ >= 0; }

    /** @return the step followed (-1 = none) */
    int step() const { return step_; }

    /**
     * Follow it. It runs while eyecam-rec searches or records this protocol; it ended once eyecam-rec is idle or in
     * error again, after showing it running, or with a recording folder made after the start (a panel that restarted
     * meanwhile). Idle with the folder: Done, or Stopped when eyecam-rec says it ended early (last_session_aborted);
     * idle without one: Stopped (stopped while it searched). In error: Failed. Not running kStartGraceSec after the
     * start, or eyecam-rec silent kGoneSec: Failed.
     * @param s eyecam-rec's status
     * @param fresh it is fresh (eyecam::tabVisible)
     * @param nowUnix Unix seconds
     * @param sessionMtime the modification time of s.sessionDir (Unix seconds; NaN when it isn't there)
     * @return how it stands
     */
    RunEnd follow(const eyecam::Status& s, bool fresh, double nowUnix, double sessionMtime);

private:
    int step_ = -1;
    std::string protocol_;
    double sent_ = 0.0;
    bool seen_ = false;         ///< eyecam-rec was seen running it
    double staleSince_ = -1.0;  ///< since when its status isn't fresh (-1 = it is)
};

/**
 * Watches for the headset being taken off and put back on before a step, from eyecam-rec's status: off once the eye
 * video stopped for kOffSec (idle, with its live processing on, which keeps looking at it), the proximity sensor read
 * off for kOffSec after it read worn (some faces never reach its threshold: then only the video counts), or the
 * Frame slept for kOffSec (the panel's loop stood still: taking the headset off lets it sleep); then on again once
 * the video (without live processing, the sensor) is back for kOnSec.
 */
class WearWatch {
public:
    /**
     * Start watching (a step began waiting for the re-wear).
     * @param now seconds on a clock that runs through suspend
     */
    void start(double now);

    /** Stop watching. */
    void stop() { watching_ = false; }

    /**
     * Follow eyecam-rec (each time its status is read).
     * @param s its status
     * @param fresh it is fresh
     * @param now seconds on a clock that runs through suspend
     */
    void follow(const eyecam::Status& s, bool fresh, double now);

    /** @return true while watching */
    bool watching() const { return watching_; }

    /** @return true once the headset was seen off */
    bool offSeen() const { return off_; }

    /** @return true once it was seen off and then on again */
    bool detected() const { return detected_; }

    /**
     * Whether "I put it back on" can be pressed: kFallbackSec after the watch started.
     * @param now seconds on the same clock
     * @return true once it can
     */
    bool fallbackReady(double now) const { return watching_ && now - start_ >= kFallbackSec; }

private:
    bool watching_ = false;
    double start_ = 0.0;
    double last_ = -1.0;          ///< the last follow (-1 = none yet)
    bool proxWorn_ = false;       ///< the sensor read worn since the watch started
    double proxOffSince_ = -1.0;
    double videoOffSince_ = -1.0;
    double onSince_ = -1.0;
    bool off_ = false;
    bool detected_ = false;
};

/** What a try that didn't finish left for the screen. */
struct Failure {
    int step = -1;          ///< the step (-1 = none)
    Why why = Why::None;
    std::string message;    ///< eyecam-rec's (Japanese) or the reason
    std::string messageEn;
    std::string kept;       ///< where its part went ("aborted/rec_..."; "" = none)
};

/** A job in the background. */
enum class Job { None, Pack, Delete };

/** How the job stands (Worker::progress). */
struct Progress {
    Job job = Job::None;
    bool running = false;
    long long done = 0;       ///< bytes packed so far
    long long total = 0;      ///< ...of these
    bool finished = false;    ///< it ended since it started (ok or error)
    bool ok = false;
    std::string error;        ///< why it failed (English)
    std::string output;       ///< the packed file's name in the folder
    long long outputBytes = 0;
};

/** The contribution as the panel shows it (PanelModel::contrib; the loop fills it in). */
struct View {
    bool loaded = false;         ///< looked for since the panel started
    std::string home;            ///< eyecam-rec's folder (defaultHome)
    std::string dir;             ///< the contribution's folder ("" = none: the first screen)
    Info info;
    long long totalBytes = 0;    ///< everything in its folder (the recordings, aborted/, a pack), as last counted
    std::string error;           ///< info.txt couldn't be written, a folder couldn't be made (English; "" = none)
    long long freeBytes = -1;    ///< free space there (-1 = unknown)
    double fps = kDefaultFps;    ///< the eye cameras' rate for the estimates
    int running = -1;            ///< the step recording now (-1 = none)
    bool offSeen = false;        ///< the step waiting for its re-wear saw the headset off
    bool fallbackReady = false;  ///< ..."I put it back on" can be pressed
    Failure failure;             ///< the last try that didn't finish
    Progress progress;           ///< packing or deleting
    bool compress = true;        ///< zstd is there (the pack is .tar.zst; else .tar)
    bool deleted = false;        ///< a contribution was just deleted (the first screen says so)
};

/** What the contribution's sheet shows. */
enum class Screen {
    Consent,   ///< what is recorded and how it is used, and "I'll help"
    Rewear,    ///< take the headset off and put it back on
    Ready,     ///< the next step's start
    Running,   ///< its recording (the eye capture's run)
    Failed,    ///< the last try didn't finish: why, and again
    Done,      ///< all recorded: pack, send, delete
    Deleting,  ///< being deleted
};

/**
 * The sheet's screen.
 * @param view the contribution
 * @param extra the optional free use was asked for
 * @return the screen
 */
Screen screen(const View& view, bool extra);

/**
 * Whether a step's start can be pressed: eyecam-rec there and idle (or in error), set up, no command waiting, nothing
 * running or being packed, and the step's recording fits.
 * @param view the contribution
 * @param eyecam eyecam-rec
 * @param step the step
 * @return true if it can
 */
bool startAllowed(const View& view, const eyecam::View& eyecam, int step);

/** One file or folder in the pack. */
struct Entry {
    std::string path;    ///< on the disk
    std::string name;    ///< in the archive ("contrib_.../rec_.../eye_L.raw"; folders end in "/")
    long long size = 0;  ///< bytes (0 for a folder)
    std::time_t mtime = 0;
    bool folder = false;
};

/**
 * What a contribution's pack holds: its folder, info.txt, and each finished step's recording (its files sorted by
 * name). Not aborted/ or an earlier pack.
 * @param dir the contribution's folder
 * @param info its progress
 * @param entries filled in
 * @param error why not (a recording missing, a name too long)
 * @return true if all are there
 */
bool packEntries(const std::string& dir, const Info& info, std::vector<Entry>& entries, std::string& error);

/**
 * A tar (ustar) header for an entry: sizes of 8 GiB and more in GNU's base-256.
 * @param entry the entry (its name under 100 bytes)
 * @return the 512 bytes, or "" for a name too long
 */
std::string tarHeader(const Entry& entry);

/**
 * The pack's file name.
 * @param dir the contribution's folder
 * @param compressed with zstd
 * @return "contrib_<date>.tar.zst" (or ".tar")
 */
std::string packName(const std::string& dir, bool compressed);

/**
 * Whether zstd can be run (in PATH).
 * @return true if it can
 */
bool zstdAvailable();

/**
 * Packs or deletes a contribution on a thread of its own, at a low priority, so the panel's loop never waits for it.
 * The pack is written beside the recordings as "<name>.part" and renamed once complete; a failed one is removed. The
 * recordings themselves are never touched by a pack.
 */
class Worker {
public:
    Worker() = default;
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /**
     * Pack a contribution into one file in its folder (tar, through zstd -3 when compressed).
     * @param dir its folder
     * @param info its progress
     * @param compressed through zstd
     * @return false if a job runs, or what it holds can't be listed (progress says why)
     */
    bool pack(const std::string& dir, const Info& info, bool compressed);

    /**
     * Delete a contribution's folder (only one named like one, right in home).
     * @param home eyecam-rec's folder
     * @param dir its folder
     * @return false if a job runs, or the folder isn't a contribution's
     */
    bool remove(const std::string& home, const std::string& dir);

    /** @return how the job stands */
    Progress progress() const;

    /**
     * The job that ended, once.
     * @param ended filled in
     * @return true if one ended since the last call
     */
    bool takeFinished(Progress& ended);

private:
    mutable std::mutex mutex_;
    Progress progress_;
    bool reported_ = true;
    std::thread thread_;
    std::atomic<bool> cancel_ {false};

    /** Join an ended thread before the next job. */
    void reap();

    /**
     * The pack's thread.
     * @param dir the folder
     * @param entries what goes in it
     * @param compressed through zstd
     */
    void runPack(const std::string& dir, const std::vector<Entry>& entries, bool compressed);

    /**
     * The end of a job.
     * @param ok it went well
     * @param error why not
     */
    void finish(bool ok, const std::string& error);
};

/**
 * The contribution as the panel's loop runs it: loads it, takes the consent and the buttons, follows its recordings
 * and the re-wear between them in eyecam-rec's status, moves each recording into its folder, keeps info.txt, and runs
 * the pack and the delete. Everything it shows goes into a View.
 */
class Session {
public:
    /**
     * Find the contribution to show (the newest in home), and go on with a recording it had started (the panel
     * restarted meanwhile).
     * @param view filled in
     * @param home eyecam-rec's folder
     */
    void load(View& view, const std::string& home);

    /**
     * "I'll help": a new contribution's folder, with the consent's time.
     * @param view the contribution (none yet)
     * @param version the panel's version
     * @param now the time
     * @return false if it couldn't be made (view.error says why)
     */
    bool consent(View& view, const std::string& version, std::time_t now);

    /**
     * "I put it back on": the next step's re-wear counts as done.
     * @param view the contribution
     * @return false if no step waits for one
     */
    bool rewore(View& view);

    /**
     * The command that starts a step's recording, if it can start now (startAllowed, its turn, its re-wear done).
     * @param view the contribution
     * @param eyecam eyecam-rec
     * @param step the step pressed
     * @return "start <protocol>", or "" when it can't
     */
    std::string startCommand(const View& view, const eyecam::View& eyecam, int step) const;

    /**
     * The start went out: follow its recording (info.txt says so, for a restart).
     * @param view the contribution
     * @param step the step
     * @param command the command sent
     * @param sentUnix when (Unix seconds)
     */
    void started(View& view, int step, const std::string& command, double sentUnix);

    /**
     * A reply from eyecam-rec (any: only the start's counts). A refused start ends that try.
     * @param view the contribution
     * @param reply the reply
     * @return true if it was the start's and it was refused
     */
    bool replied(View& view, const eyecam::Reply& reply);

    /**
     * Follow eyecam-rec and the background job (each time the status is read).
     * @param view the contribution
     * @param eyecam eyecam-rec
     * @param nowUnix Unix seconds
     * @param bootNow seconds on a clock that runs through suspend
     * @param log what happened, a line each (for the panel's log; "" when nothing)
     * @return true if the view changed
     */
    bool follow(View& view, const eyecam::View& eyecam, double nowUnix, double bootNow, std::string& log);

    /**
     * Pack it into one file (on the worker).
     * @param view the contribution
     * @return false if it can't start (view.progress says why)
     */
    bool pack(View& view);

    /**
     * Delete its folder (on the worker; not while a recording of it runs).
     * @param view the contribution
     * @return false if it can't
     */
    bool remove(View& view);

private:
    RunWatch run_;
    WearWatch wear_;
    Worker worker_;
    std::string command_;     ///< the start waiting for its reply ("" = none)
    double freeAt_ = -1e9;    ///< when the free space was last read (boot seconds)

    /**
     * Write info.txt (a failure goes to view.error).
     * @param view the contribution
     * @return true if written
     */
    bool save(View& view);

    /**
     * A recording of a step ended.
     * @param view the contribution
     * @param end how
     * @param log what happened
     */
    void ended(View& view, const RunEnd& end, std::string& log);
};

/**
 * A size for the screen: "10.5 GB" (one decimal; MB under 1 GB).
 * @param bytes the size
 * @return the text
 */
std::string sizeText(long long bytes);

}  // namespace contrib
