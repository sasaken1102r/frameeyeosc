// "Record for squint training" (Advanced tab, Debug tools): one press records eyecam's protocol for the training data
// of its half-shut check (the eyelid veto's CNN) and keeps it for the user to send to the developer. The first time
// (and whenever the consent's text changes) it asks for the consent and whether glasses or prescription inserts are
// worn; then one recording of protocol_contrib.txt (about 93 s, no full-view light: closes, squints, widening, looking
// up, down, left and right). eyecam-rec records it into ~/eyecam/rec_<date>/ as always (with its cues.csv, valve.csv,
// frames.csv and meta.txt); once it has ended, and has those four, the panel moves it into ~/eyecam/contrib/, which
// gathers every recording made this way, and adds a section about it to that folder's info.txt (the settings then,
// the minutes since the one before and whether the headset was taken off in between, from eyecam-rec's count; no
// personal data). The folder can be packed into one file and deleted from the panel. Everything here works without
// OpenVR and cairo (contrib-test).
#pragma once

#include "eyecam.h"

#include <atomic>
#include <cmath>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace contrib {

/** The protocol recorded (eyecam's protocol_contrib.txt). */
constexpr const char* kProtocol = "contrib";
/** How long its steps take (s; the lead-in included). */
constexpr double kProtocolSec = 93.0;
/** eyecam-rec records this much past a protocol's last step (s), for the end beep. */
constexpr double kTailSec = 1.5;
/** The consent's text version: a newer one asks for the consent again. */
constexpr int kConsentVersion = 1;
/** The folder in ~/eyecam that gathers the recordings. */
constexpr const char* kFolderName = "contrib";
/** Its file about them (a section added for each event). */
constexpr const char* kInfoFile = "info.txt";
/** Where a recording that didn't finish (or can't be used) goes in it: never packed, deleted with it. */
constexpr const char* kAbortedDir = "aborted";
/** The files every recording needs for training (the labels come from cues.csv and Valve's openness). */
constexpr const char* kRequiredFiles[] = {"eye_L.raw", "eye_R.raw", "frames.csv", "valve.csv", "cues.csv", "meta.txt"};
/** A raw frame's size when meta.txt doesn't say (frame_bytes: 400x400 grey, or 512x400 with --full-width). */
constexpr long long kRawFrameBytes = 160000;
/** The eye with fewer frames needs at least this share of the other's: both cameras run at the same rate (0.999 to
 * 1.000 in 119 recordings and calibrations, at 15 fps too); a ring locked on part of one camera's slots gives one
 * eye a quarter, a half or three quarters of the frames. */
constexpr double kEvenShare = 0.9;
/** At least this share of the frames needs a usable Valve sample near it (the labels use Valve's openness)... */
constexpr double kValveShare = 0.9;
/** ...within this long of its camera time (s; Valve's samples come about 90 times a second, on the same clock). */
constexpr double kValvePairSec = 0.05;
/** What eyecam-rec writes per frame and eye (a 400x400 grey frame, its header, its rows), as eyecam-rec estimates. */
constexpr double kFrameBytes = 160300.0;
/** The eye cameras' usual rate, for the estimate while eyecam-rec doesn't say. */
constexpr double kDefaultFps = 90.0;
/** Space left free after a recording (eyecam-rec stops one with less than 1 GiB left), and after a pack. */
constexpr long long kMarginBytes = 1LL << 30;
/** zstd's level for the pack: on the Frame's eye video -3 gave 0.62 of the size in 7.7 s for 2 GB, -6 0.58 in 20 s,
 * -9 0.56 in 43 s, -12 0.55 in 90 s (zip -6: 0.60 in 166 s, gzip -1: 0.62 in 19 s). */
constexpr int kZstdLevel = 6;
/** The pack is at most this share of the recordings (0.58 measured at kZstdLevel). */
constexpr double kPackShare = 0.7;
/** A start that eyecam-rec doesn't show running within this long (s) didn't start. */
constexpr double kStartGraceSec = 8.0;
/** eyecam-rec silent (its status not fresh) this long (s) ends a recording as failed. */
constexpr double kGoneSec = 10.0;

/** Glasses or prescription inserts in the headset (asked with the consent; the training looks at them apart). */
enum class Eyewear { Unasked, None, Glasses, Inserts, NoAnswer };

/**
 * The name info.txt writes.
 * @param eyewear the answer
 * @return "none", "glasses", "inserts", "no_answer" ("" for Unasked)
 */
const char* eyewearName(Eyewear eyewear);

/**
 * Read a name info.txt wrote.
 * @param name the name
 * @return the answer (Unasked for anything else)
 */
Eyewear parseEyewear(const std::string& name);

/** One recording's section in info.txt. */
struct Recording {
    std::string rec;                 ///< its folder in the contribution's ("rec_YYYY-MM-DD_HH-MM-SS")
    std::string protocol;            ///< the protocol recorded
    std::string start;               ///< when its start was sent (local "YYYY-MM-DD HH:MM:SS +zzzz")
    double startUnix = 0.0;          ///< ...in Unix seconds
    double endUnix = 0.0;            ///< when the panel saw it ended
    long long bytes = 0;             ///< its folder's size
    std::string appVersion;          ///< frameeyeosc's version
    std::string eyecamRec;           ///< the first 8 hex of eyecam-rec's SHA-256 ("" = unknown; it has no git hash)
    std::string lidVeto;             ///< eyecam's eyelid veto: "on", "off", "unknown" (an eyecam-rec before it)
    std::string widen;               ///< frameeyeosc's widening (`widen`): "on" / "off"
    std::string widenSensitivity;    ///< eyecam's ("0.50"; "unknown")
    std::string cameraLids;          ///< the eye cameras switched on for the eyelids (`camera_lids`): "on" / "off"
    std::string eyewear;             ///< eyewearName at the time
    double fps = NAN;                ///< the eye cameras' frames a second at the start (NaN = unknown)
    std::string sincePrevious;       ///< minutes since the previous recording ended ("12.5"; "none" for the first)
    std::string headsetOff;          ///< the headset taken off since then: "yes", "no", "unknown", "none" (the first)
    long long offCountStart = -1;    ///< eyecam-rec's headset_off_count at the start (-1 = unknown)
    long long offCountEnd = -1;      ///< ...and at the end
    double eyecamStarted = 0.0;      ///< eyecam-rec's started_unix (the count is per run; 0 = unknown)
    std::string calib;               ///< the calibration's result copied beside it ("<rec>.calib_result.json"; "none")
    std::string userCalib;           ///< eyecam's calib.json copied beside it ("<rec>.user_calib.json"; "none")
    std::string settings;            ///< eyecam's settings.json copied beside it ("<rec>.settings.json"; "none")
};

/** info.txt as read: the consent and every recording. */
struct Info {
    int consentVersion = 0;          ///< the consent's text version agreed to (0 = none)
    std::string consent;             ///< when (local time)
    Eyewear eyewear = Eyewear::Unasked;
    std::vector<Recording> recordings;
    std::vector<std::string> aborted;  ///< tries that didn't finish or lacked a file ("aborted/rec_... why")
    std::string packed;              ///< the last pack's file in the folder ("" = none)
    int packedCount = 0;             ///< ...how many recordings it holds
    long long packedBytes = 0;
};

/**
 * Whether the consent is to be asked (again): none, an older text's, or no answer about glasses yet.
 * @param info the file as read
 * @return true to ask
 */
bool consentNeeded(const Info& info);

/**
 * What a recording takes on the disk (eyecam-rec's own estimate: both eyes, every frame).
 * @param fps the eye cameras' frame rate (kDefaultFps when not a number above 0)
 * @return bytes
 */
long long estimateBytes(double fps);

/**
 * Whether a recording fits: its estimate and kMarginBytes are free (or the free space isn't known).
 * @param freeBytes the free space (-1 = unknown)
 * @param fps the eye cameras' frame rate
 * @return true if it fits
 */
bool fits(long long freeBytes, double fps);

/**
 * The recordings' size.
 * @param info the file as read
 * @return bytes
 */
long long recordedBytes(const Info& info);

/**
 * What packing needs free: kPackShare of the recordings (all of them without zstd) and kMarginBytes.
 * @param info the file as read
 * @param compressed packed with zstd
 * @return bytes
 */
long long packNeedBytes(const Info& info, bool compressed);

/**
 * The top of a new info.txt: what it is, and its format.
 * @return the text
 */
std::string headerText();

/**
 * A consent's section.
 * @param time when (local)
 * @param version the text's version
 * @param eyewear the answer about glasses
 * @return the text
 */
std::string consentSection(const std::string& time, int version, Eyewear eyewear);

/**
 * A recording's section.
 * @param r the recording
 * @return the text
 */
std::string recordingSection(const Recording& r);

/**
 * A try that didn't finish (or lacked a file).
 * @param name where it went ("aborted/rec_...")
 * @param why why (English)
 * @return the text
 */
std::string abortedSection(const std::string& name, const std::string& why);

/**
 * A pack.
 * @param file its name
 * @param count the recordings in it
 * @param bytes its size
 * @return the text
 */
std::string packedSection(const std::string& file, int count, long long bytes);

/**
 * Read info.txt: "[section]" lines, "key=value" lines after them, "#" comments; unknown keys and odd lines skipped.
 * The last consent counts.
 * @param text the text
 * @param info filled in
 */
void parseInfo(const std::string& text, Info& info);

/**
 * Whether a name is eyecam-rec's recording folder: "rec_YYYY-MM-DD_HH-MM-SS".
 * @param name the name
 * @return true if it is
 */
bool isRecordingName(const std::string& name);

/**
 * Whether a name is a pack's: "contrib_YYYY-MM-DD_HH-MM-SS.tar.zst" (or ".tar").
 * @param name the name
 * @return true if it is
 */
bool isPackName(const std::string& name);

/**
 * A local time as info.txt writes it.
 * @param when the time
 * @param zone with the time zone's offset (" +0900")
 * @return "YYYY-MM-DD HH:MM:SS" (with the offset)
 */
std::string localTime(std::time_t when, bool zone);

/**
 * Where eyecam-rec records ($HOME/eyecam); the contribution's folder is kFolderName in it.
 * @return the folder
 */
std::string defaultHome();

/**
 * Where eyecam keeps its user calibration and settings ($HOME/.config/eyecam: calib.json, settings.json).
 * @return the folder
 */
std::string defaultConfigDir();

/**
 * Add text to the end of the folder's info.txt (made with headerText first), on the disk before it returns.
 * @param dir the folder
 * @param text the text
 * @param error why not
 * @return true if written
 */
bool appendInfo(const std::string& dir, const std::string& text, std::string& error);

/**
 * The free space where a folder is (what an unprivileged user can fill).
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
 * The files a recording lacks (kRequiredFiles).
 * @param dir its folder
 * @return their names (empty: all there)
 */
std::vector<std::string> missingFiles(const std::string& dir);

/** Whether a recording can be used for training. */
enum class Check {
    Ok,
    Missing,    ///< a file of kRequiredFiles isn't there
    NoVideo,    ///< an eye has no frame at all (its camera's video didn't come)
    Uneven,     ///< one eye has under kEvenShare of the other's frames (its camera's video came only in part)
    Truncated,  ///< an eye's raw file doesn't hold as many frames as frames.csv lists for it (the disk ran full)
    Valve,      ///< under kValveShare of the frames have a usable Valve sample near them (Valve's tracking off?)
};

/** checkRecording's answer. */
struct CheckResult {
    Check check = Check::Ok;
    std::string detail;  ///< the files missing, the eyes without frames ("L", "R", "LR"), each eye's frames
                         ///< ("8490/2124", left/right), the raw files cut short ("eye_L.raw"), or the share paired
                         ///< ("42%")
};

/**
 * Whether a recording can be used for training, from its CSV files and its raw files' sizes alone (never the
 * frames): all of kRequiredFiles there; frames of both eyes in frames.csv, the fewer at least kEvenShare of the
 * more; each eye's raw file exactly as many
 * frames (meta.txt's frame_bytes each, else kRawFrameBytes) as frames.csv has rows for that eye; and at least
 * kValveShare of frames.csv's frames with a usable Valve sample in valve.csv (producer_state 1, both openness values
 * numbers) within kValvePairSec of their t_cam (sample_time is on the same clock).
 * @param dir its folder
 * @return the answer
 */
CheckResult checkRecording(const std::string& dir);

/**
 * Move a recording eyecam-rec has finished into the contribution's folder (aborted/ for one that didn't finish or
 * lacks a file). Only one of eyecam-rec's own folders, right in home ("rec_<date>"), is moved.
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

/**
 * The newest calibration result eyecam keeps in home (calib_<date>/calib_result.json, by name: they are dated).
 * @param home eyecam-rec's folder
 * @return its text ("" = none)
 */
std::string newestCalibResult(const std::string& home);

/**
 * Whether the headset was taken off between two recordings, from eyecam-rec's count: only within one run of it
 * (the same started_unix), with both counts known.
 * @param before the earlier recording
 * @param offCountStart the count at the later one's start (-1 = unknown)
 * @param eyecamStarted eyecam-rec's started_unix then (0 = unknown)
 * @return "yes", "no" or "unknown"
 */
std::string headsetOffBetween(const Recording& before, long long offCountStart, double eyecamStarted);

/** How a recording ended. */
enum class Outcome {
    Running,  ///< not yet
    Done,     ///< it ran its full time
    Stopped,  ///< stopped early (the user's "Stop", the eyes lost): its folder (if any) holds part of it
    Failed,   ///< eyecam-rec failed, refused, went away, or never started it
};

/** Why a try didn't finish (the screen says it). */
enum class Why {
    None, Stopped, Error, Refused, Gone, NoStart, Missing, Move, Incomplete, NoVideo, Uneven, Truncated, Valve
};

/** A recording's end. */
struct RunEnd {
    Outcome outcome = Outcome::Running;
    std::string dir;        ///< its folder ("" = none was made)
    Why why = Why::None;
    std::string message;    ///< eyecam-rec's (Japanese) or the reply's reason
    std::string messageEn;  ///< eyecam-rec's in English ("" = none)
};

/**
 * Follows a recording from the moment its start went out until eyecam-rec has ended it, from status.json alone.
 */
class RunWatch {
public:
    /**
     * A start was sent.
     * @param protocol its protocol
     * @param sentUnix when (Unix seconds)
     */
    void begin(const std::string& protocol, double sentUnix);

    /** It ended (or was given up). */
    void end() { active_ = false; }

    /** @return true while following one */
    bool active() const { return active_; }

    /**
     * Follow it. It runs while eyecam-rec searches or records this protocol; it ended once eyecam-rec is idle or in
     * error again after showing it running, or with a recording folder made after the start. Idle with the folder:
     * Done, or Stopped when eyecam-rec says it ended early (last_session_aborted); idle without one: Stopped
     * (stopped while it searched). In error: Failed. Not running kStartGraceSec after the start, or eyecam-rec silent
     * kGoneSec: Failed.
     * @param s eyecam-rec's status
     * @param fresh it is fresh (eyecam::tabVisible)
     * @param nowUnix Unix seconds
     * @param sessionMtime the modification time of s.sessionDir (Unix seconds; NaN when it isn't there)
     * @return how it stands
     */
    RunEnd follow(const eyecam::Status& s, bool fresh, double nowUnix, double sessionMtime);

private:
    bool active_ = false;
    std::string protocol_;
    double sent_ = 0.0;
    bool seen_ = false;         ///< eyecam-rec was seen running it
    double staleSince_ = -1.0;  ///< since when its status isn't fresh (-1 = it is)
};

/** What a try that didn't finish left for the screen. */
struct Failure {
    bool shown = false;
    Why why = Why::None;
    std::string message;    ///< eyecam-rec's (Japanese), the reason, or checkRecording's detail
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
    int count = 0;            ///< the recordings packed
    std::vector<std::string> incomplete;  ///< recordings it left out (checkRecording said no)
};

/** The contribution as the panel shows it (PanelModel::contrib; the loop fills it in). */
struct View {
    bool loaded = false;         ///< looked for since the panel started
    std::string home;            ///< eyecam-rec's folder (defaultHome)
    std::string dir;             ///< the contribution's folder (home/kFolderName)
    std::string configDir;       ///< eyecam's (defaultConfigDir): its calib.json and settings.json are copied
    bool exists = false;         ///< ...is there
    Info info;                   ///< its info.txt as read
    long long totalBytes = 0;    ///< everything in it (recordings, aborted/, a pack), as last counted
    std::string error;           ///< info.txt couldn't be written, a folder couldn't be made (English; "" = none)
    long long freeBytes = -1;    ///< free space there (-1 = unknown)
    double fps = kDefaultFps;    ///< the eye cameras' rate for the estimate
    bool running = false;        ///< a recording of it runs
    bool justDone = false;       ///< one has just been recorded (the end's screen, until the sheet is opened again)
    Failure failure;             ///< the last try, when it didn't finish
    std::vector<std::string> incomplete;  ///< recordings the last pack left out (checkRecording said no)
    Progress progress;           ///< packing or deleting
    bool compress = true;        ///< zstd is there (the pack is .tar.zst; else .tar)
    bool deleted = false;        ///< it was just deleted (the first screen says so)
};

/** Which way the sheet was opened. */
enum class Page {
    Record,  ///< "Record": the consent if needed, then the recording and its end
    Manage,  ///< "Recordings": the end's screen (how many, the folder, pack, delete)
};

/** What the sheet shows. */
enum class Screen {
    Consent,   ///< what is recorded and how it is used, and "Agree"
    Eyewear,   ///< glasses or inserts?
    Ready,     ///< what to do, and "Start"
    Running,   ///< the recording (the eye capture's run)
    Failed,    ///< it didn't finish: why, and again
    Done,      ///< recorded: how many, the folder, how to send, pack, delete
    Deleting,  ///< being deleted
};

/**
 * The sheet's screen.
 * @param view the contribution
 * @param page how it was opened
 * @param agreed "Agree" was pressed (the question about glasses follows)
 * @return the screen
 */
Screen screen(const View& view, Page page, bool agreed);

/**
 * Whether "Start" can be pressed: the consent given, eyecam-rec there and idle (or in error), set up, no command
 * waiting, nothing running or being packed, the recording fits, and frameeyeosc running with eye data coming in
 * (Valve's samples, which a recording needs for its labels, only come while frameeyeosc asks for them: without them
 * it would end in aborted/ after its 93 s).
 * @param view the contribution
 * @param eyecam eyecam-rec
 * @param eyeData frameeyeosc runs and eye data comes in (EyeStatus running and tracking)
 * @return true if it can
 */
bool startAllowed(const View& view, const eyecam::View& eyecam, bool eyeData);

/** One file or folder in the pack. */
struct Entry {
    std::string path;    ///< on the disk
    std::string name;    ///< in the archive ("contrib/rec_.../eye_L.raw"; folders end in "/")
    long long size = 0;  ///< bytes (0 for a folder)
    std::time_t mtime = 0;
    bool folder = false;
};

/**
 * What the pack holds: the folder, info.txt, and each recording checkRecording passes (its files sorted by name) with
 * the copies beside it (calibration result, user calibration, settings). Not aborted/ or a pack; a recording that
 * doesn't pass is left out and named.
 * @param dir the contribution's folder
 * @param info its info.txt as read
 * @param entries filled in
 * @param incomplete the recordings left out
 * @param error why not (a name too long, the folder unreadable)
 * @return true if it can be packed
 */
bool packEntries(const std::string& dir, const Info& info, std::vector<Entry>& entries,
                 std::vector<std::string>& incomplete, std::string& error);

/**
 * A tar (ustar) header for an entry: sizes of 8 GiB and more in GNU's base-256.
 * @param entry the entry (its name under 100 bytes)
 * @return the 512 bytes, or "" for a name too long
 */
std::string tarHeader(const Entry& entry);

/**
 * A pack's file name.
 * @param when when it is made
 * @param compressed with zstd
 * @return "contrib_YYYY-MM-DD_HH-MM-SS.tar.zst" (or ".tar")
 */
std::string packName(std::time_t when, bool compressed);

/**
 * Whether zstd can be run (in PATH).
 * @return true if it can
 */
bool zstdAvailable();

/**
 * Packs or deletes the contribution's folder on a thread of its own, at a low priority, so the panel's loop never
 * waits for it. The pack is written in the folder as "<name>.part" and renamed once complete; a failed one is
 * removed. The recordings themselves are never touched by a pack.
 */
class Worker {
public:
    Worker() = default;
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /**
     * Pack into one file in the folder (tar, through zstd at kZstdLevel when compressed), checking each recording
     * again first (packEntries, on the thread: it reads their CSV files).
     * @param dir the folder
     * @param info its info.txt as read
     * @param name the file's name (packName)
     * @param compressed through zstd
     * @return false if a job runs
     */
    bool pack(const std::string& dir, const Info& info, const std::string& name, bool compressed);

    /**
     * Delete the contribution's folder (only home/kFolderName itself, a real folder).
     * @param home eyecam-rec's folder
     * @param dir the folder
     * @return false if a job runs, or the folder isn't that one
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
     * @param info its info.txt as read
     * @param name the file's name
     * @param compressed through zstd
     */
    void runPack(const std::string& dir, const Info& info, const std::string& name, bool compressed);

    /**
     * The end of a job.
     * @param ok it went well
     * @param error why not
     */
    void finish(bool ok, const std::string& error);
};

/** What the panel knows at a start that eyecam-rec's status doesn't say (for the recording's section). */
struct Snapshot {
    std::string appVersion;  ///< frameeyeosc's version
    std::string eyecamRec;   ///< the first 8 hex of the installed eyecam-rec's SHA-256 ("" = unknown)
    bool widen = true;       ///< frameeyeosc's `widen`
    bool cameraLids = true;  ///< `camera_lids`
};

/**
 * The contribution as the panel's loop runs it: loads it, takes the consent and the start, follows the recording in
 * eyecam-rec's status, checks its files and moves it into the folder, adds its section to info.txt, and runs the
 * pack and the delete. Everything it shows goes into a View.
 */
class Session {
public:
    /**
     * Read the folder (home/kFolderName) and its info.txt.
     * @param view filled in
     * @param home eyecam-rec's folder
     * @param configDir eyecam's settings' folder
     */
    void load(View& view, const std::string& home, const std::string& configDir = defaultConfigDir());

    /**
     * Read the folder and its info.txt again (the sheet opened: it may have been changed or deleted by hand), keeping
     * the recording followed and the job: only what is on the disk (exists, info, its size, the free space).
     * @param view the contribution
     */
    void refresh(View& view);

    /**
     * The consent and the answer about glasses: the folder (made if needed), and their section in info.txt.
     * @param view the contribution
     * @param eyewear the answer
     * @param now the time
     * @return false if it couldn't be written (view.error says why)
     */
    bool consent(View& view, Eyewear eyewear, std::time_t now);

    /**
     * The command that starts a recording, if it can start now (startAllowed).
     * @param view the contribution
     * @param eyecam eyecam-rec
     * @param eyeData frameeyeosc runs and eye data comes in
     * @return "start contrib", or "" when it can't
     */
    std::string startCommand(const View& view, const eyecam::View& eyecam, bool eyeData) const;

    /**
     * The start went out: follow its recording, with what its section will say about the time of the start.
     * @param view the contribution
     * @param eyecam eyecam-rec (its status now: the eyelid veto, the widening sensitivity, its count)
     * @param snapshot what the panel knows
     * @param command the command sent
     * @param sentUnix when (Unix seconds)
     */
    void started(View& view, const eyecam::View& eyecam, const Snapshot& snapshot, const std::string& command,
                 double sentUnix);

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
     * @param bootNow seconds on a clock that runs through suspend (how often the free space is read again)
     * @param log what happened, a line each (for the panel's log; "" when nothing)
     * @return true if the view changed
     */
    bool follow(View& view, const eyecam::View& eyecam, double nowUnix, double bootNow, std::string& log);

    /**
     * Pack the folder into one file (on the worker), when the space it needs is free (packNeedBytes; an earlier pack
     * is already taken from the free space, and goes only once the new one is complete).
     * @param view the contribution
     * @param now the time (the file's name)
     * @return false if it can't start (view.progress says why)
     */
    bool pack(View& view, std::time_t now);

    /**
     * Delete the folder (on the worker; not while a recording runs).
     * @param view the contribution
     * @return false if it can't
     */
    bool remove(View& view);

private:
    RunWatch run_;
    Worker worker_;
    std::string command_;     ///< the start waiting for its reply ("" = none)
    Recording pending_;       ///< the recording running, as its section will say
    std::string calibText_;   ///< the calibration result at its start
    std::string userCalibText_;  ///< ...eyecam's calib.json then
    std::string settingsText_;   ///< ...and its settings.json
    double freeAt_ = -1e9;    ///< when the free space was last read (boot seconds)

    /**
     * Add a section to info.txt and read it again (a failure goes to view.error).
     * @param view the contribution
     * @param text the section
     * @return true if written
     */
    bool append(View& view, const std::string& text);

    /**
     * The recording ended.
     * @param view the contribution
     * @param end how
     * @param eyecam eyecam-rec (its count at the end)
     * @param nowUnix when (Unix seconds)
     * @param log what happened
     */
    void ended(View& view, const RunEnd& end, const eyecam::View& eyecam, double nowUnix, std::string& log);
};

/**
 * A size for the screen: "10.5 GB" (one decimal; MB under 1 GB).
 * @param bytes the size
 * @return the text
 */
std::string sizeText(long long bytes);

}  // namespace contrib
