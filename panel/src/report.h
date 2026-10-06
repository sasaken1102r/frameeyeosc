// Records of calibrations and eye fits ("Recent records" on the Advanced tab's "Having trouble" page, and
// `frameeyeosc-panel --report`). Each eye-camera calibration started from the panel and each eye fit (from the panel,
// or the re-wear fit run by itself when the headset is put on) leaves a folder
// ~/.local/state/frameeyeosc/reports/<kind>_YYYY-MM-DD_HH-MM-SS/ with:
//   summary.json   what ran, how it ended and why (Japanese and English), the versions, the diagnostic code at the
//                  end, the conditions (dashboard, camera and tracker rates, time since the headset was put on) and
//                  the key log lines
//   status.jsonl   one line a second while it ran: frameeyeosc's and eyecam-rec's status.json (compact)
//   logs.txt       the run's window (from kBeforeSec before it) of frameeyeosc's, the panel's and eyecam's journals and
//                  Valve's eye tracker log, merged in time order, each line tagged with its source (at most
//                  kMaxLogBytes)
//   calib_result.json  a calibration's numbers, copied from eyecam's calibration folder (never images, never
//                  calib_samples.csv)
//   report.txt     the same as summary.json, for reading (frameeyeosc-panel --report latest prints it)
// Only the newest kKeep are kept. Nothing is sent anywhere. The logs are read after the run, on a worker thread
// (Writer), with read-only `journalctl --user` calls; the sources can be swapped for tests (Sources). Nothing here
// talks to OpenVR or cairo (report-test).
#pragma once

#include "i18n.h"

#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace report {

/** How many records are kept (the oldest beyond it are deleted when a new one is written). */
constexpr int kKeep = 10;
/** logs.txt is cut to this size (the start and the end are kept, the middle left out). */
constexpr size_t kMaxLogBytes = 300 * 1024;
/** status.jsonl stops growing at this size (a calibration of 18 s writes about 60 KB). */
constexpr size_t kMaxStatusBytes = 200 * 1024;
/** calib_result.json is only copied when it is at most this big. */
constexpr size_t kMaxCalibResultBytes = 512 * 1024;
/** The logs from this long before the run start (the headset being put on, the eye data coming back). */
constexpr double kBeforeSec = 15.0;
/** ...until this long after it ended. */
constexpr double kAfterSec = 2.0;
/** The logs are read this long after the run ended, so the journal has its last lines. */
constexpr double kSettleSec = 1.5;
/** At most this many key lines ("flow") are kept in summary.json. */
constexpr size_t kMaxFlowLines = 16;
/** Valve's eye tracker log: at most this much of its end is read. */
constexpr size_t kMaxValveBytes = 4 * 1024 * 1024;
/** One journal's output is read up to this size. */
constexpr size_t kMaxJournalBytes = 4 * 1024 * 1024;

/** What ran. */
enum class Kind { CalibWear, CalibUser, Fit, Recenter };

/** How it ended. */
enum class Result { Ok, Failed, Partial };

/** Where a log line came from. */
enum class Source { Core, Panel, Eyecam, Valve };

/**
 * @param kind the kind
 * @return its name in folder names and summary.json ("calib-wear", "calib-user", "fit", "recenter")
 */
const char* kindName(Kind kind);

/**
 * @param name a kind's name
 * @param kind where to write it
 * @return true if known
 */
bool parseKind(const std::string& name, Kind& kind);

/**
 * @param result the result
 * @return "ok", "failed" or "partial"
 */
const char* resultName(Result result);

/**
 * @param name a result's name
 * @param result where to write it
 * @return true if known
 */
bool parseResult(const std::string& name, Result& result);

/**
 * @param source the source
 * @return its tag in logs.txt and summary.json ("frameeyeosc", "panel", "eyecam", "valve")
 */
const char* sourceName(Source source);

/**
 * @param name a source's tag
 * @param source where to write it
 * @return true if known
 */
bool parseSource(const std::string& name, Source& source);

/** One log line. */
struct LogLine {
    double at = 0.0;    ///< when (Unix seconds)
    Source source = Source::Panel;
    std::string text;   ///< the message, without the time and the program's name
};

/** What the run ran under. Unknown numbers are NaN. */
struct Conditions {
    int dashboardOpen = -1;       ///< the SteamVR dashboard was open when it started (1), closed (0), unknown (-1)
    double cameraFps = NAN;       ///< the eye cameras' frames a second (eyecam-rec's cam_fps)
    double trackerRate = NAN;     ///< Valve's eye tracker's samples a second (status.json)
    double trackerMissed = NAN;   ///< ...that frameeyeosc missed
    double sincePutOnSec = NAN;   ///< how long the eyes had been tracked without a break (since putting it on)
    double offBeforeSec = NAN;    ///< how long they weren't before that
    double ipdMm = NAN;           ///< the IPD SteamVR gave (eye fits)
};

/** One record's summary.json. */
struct Summary {
    Kind kind = Kind::Fit;
    std::string trigger = "panel";  ///< "panel" (pressed) or "auto" (the re-wear fit when the headset was put on)
    std::string mode;               ///< eye fits: "full", "center" or "tilt"
    double start = 0.0;             ///< Unix seconds
    double end = 0.0;
    Result result = Result::Ok;
    std::string reason;             ///< why, or what came of it (Japanese)
    std::string reasonEn;           ///< ...in English
    std::string brief;              ///< a short line for the lists (Japanese; "" = none)
    std::string briefEn;
    std::string version;            ///< frameeyeosc's
    std::string steamos;            ///< SteamOS's ("0.4.3 (20260930.6234839)")
    std::string code;               ///< the diagnostic code at the end
    Conditions conditions;
    std::vector<LogLine> flow;      ///< the key log lines, oldest first
    std::string folder;             ///< the record's folder name (not stored: it is the folder)
};

/** A file in a record, as the record view lists it. */
struct FileInfo {
    std::string name;
    long long bytes = 0;
};

/**
 * Where records go: $XDG_STATE_HOME/frameeyeosc/reports, or ~/.local/state/frameeyeosc/reports.
 * @return the folder
 */
std::string defaultDir();

/**
 * A path as shown: the home folder written as "~".
 * @param path the path
 * @return the shorter path
 */
std::string shortPathOf(const std::string& path);

/**
 * A record's folder name.
 * @param kind what ran
 * @param start when it started (Unix seconds; local time in the name)
 * @return "fit_2026-10-06_20-45-09"
 */
std::string folderName(Kind kind, double start);

/**
 * Whether a name is a record's folder (<kind>_YYYY-MM-DD_HH-MM-SS, maybe with _2, _3... after it).
 * @param name the name
 * @return true if it is one
 */
bool isFolderName(const std::string& name);

/**
 * The time part of a folder name, for sorting ("2026-10-06_20-45-09", or "" if it isn't one).
 * @param name the folder name
 * @return the stamp
 */
std::string folderStamp(const std::string& name);

/**
 * Take a journal apart: `journalctl -o short-unix` lines ("1791287099.117852 frame frameeyeosc[28560]: text").
 * A line that doesn't start with a time (a message's next line) keeps the time before it.
 * @param text the output
 * @param source whose
 * @return the lines
 */
std::vector<LogLine> parseJournal(const std::string& text, Source source);

/**
 * The time at the start of a line of Valve's eye tracker log ("Tue Oct 06 2026 20:44:59.045811 [Info] - ...", local).
 * @param line the line
 * @param when where to write it (Unix seconds)
 * @param rest where to write what follows the time (the "[Info] - " left out)
 * @return true if the line starts with one
 */
bool parseValveLine(const std::string& line, double& when, std::string& rest);

/**
 * The lines of Valve's eye tracker log within a window.
 * @param text the log (or its end)
 * @param from Unix seconds
 * @param to Unix seconds
 * @return the lines
 */
std::vector<LogLine> parseValveLog(const std::string& text, double from, double to);

/**
 * Put several logs together in time order (lines at the same time keep their order).
 * @param logs the logs
 * @return one log
 */
std::vector<LogLine> merge(const std::vector<std::vector<LogLine>>& logs);

/**
 * logs.txt: a line each, "2026-10-06 20:44:59.117 [frameeyeosc] text". Over maxBytes, the start and the end are kept
 * (half each) with a line saying how many were left out.
 * @param lines the merged log
 * @param maxBytes the cap
 * @return the text
 */
std::string logsText(const std::vector<LogLine>& lines, size_t maxBytes = kMaxLogBytes);

/**
 * Whether a line says something about the run (the fit's tries, a calibration's steps, the eye data stopping or
 * coming back, the headset put on): what the record's "flow" shows.
 * @param line the line
 * @return true if it is a key line
 */
bool keyLine(const LogLine& line);

/**
 * The key lines of a log, at most max: the first few and the last ones (where it ended) when there are more.
 * @param lines the merged log
 * @param max how many at most
 * @return them, oldest first
 */
std::vector<LogLine> flowLines(const std::vector<LogLine>& lines, size_t max = kMaxFlowLines);

/**
 * A flow line in the panel's words, where it is one the panel knows (a fit's try, the fit starting, the dot hidden,
 * the eye data back), else the line itself (Valve's without its "[Info] - ").
 * @param t texts
 * @param line the line
 * @return the text
 */
std::string flowText(const UiText& t, const LogLine& line);

/**
 * A source's name as the record shows it ("本体", "パネル", "Valve", "カメラ" / "core", "panel", ...).
 * @param t texts
 * @param source the source
 * @return the name
 */
const char* sourceLabel(const UiText& t, Source source);

/**
 * A kind as shown ("目を合わせる", "正面を合わせ直す", "目のカメラの校正", ...; the re-wear fit's own name by its mode).
 * @param t texts
 * @param summary the record
 * @return the name
 */
std::string kindLabel(const UiText& t, const Summary& summary);

/**
 * A result as shown on its badge ("OK", "失敗", "片目だけ").
 * @param t texts
 * @param result the result
 * @return the word
 */
const char* resultLabel(const UiText& t, Result result);

/**
 * The conditions as one line, "ダッシュボードを開いたまま・カメラ 90 枚/秒・かぶってから 10 秒" (what is known).
 * @param t texts
 * @param summary the record
 * @return the line
 */
std::string conditionsText(const UiText& t, const Summary& summary);

/**
 * summary.json's text.
 * @param summary the record
 * @return the JSON
 */
std::string summaryJson(const Summary& summary);

/**
 * Read summary.json.
 * @param text its text
 * @param summary where to write it (folder is left alone)
 * @return true if it is a record's summary
 */
bool parseSummary(const std::string& text, Summary& summary);

/**
 * report.txt: a header, the result, the versions, the code, the rates, what came before, the flow and the folder
 * (the shape of `frameeyeosc-panel --report latest`).
 * @param t texts (the panel's language when it was written)
 * @param summary the record
 * @param folderShown the folder as shown ("~/.local/state/frameeyeosc/reports/fit_.../")
 * @return the text
 */
std::string reportText(const UiText& t, const Summary& summary, const std::string& folderShown);

/**
 * A JSON text without the white space between its tokens (one line for status.jsonl); "" if it isn't a JSON object.
 * @param text the JSON
 * @return the compact text
 */
std::string compactJson(const std::string& text);

/**
 * Whether a file's text looks like calib_result.json: a JSON object, and no NUL bytes (nothing binary).
 * @param text the file
 * @return true if it may be copied
 */
bool plainJson(const std::string& text);

/** Where the logs come from (swapped for fakes in tests). */
struct Sources {
    /** A user unit's journal from `since` to `until` (Unix seconds) as `journalctl -o short-unix` prints it. */
    std::function<std::string(const std::string& unit, double since, double until)> journal;
    std::string valveLog;    ///< Valve's eye tracker log ("" = none)
    std::string eyecamHome;  ///< eyecam's folder with the calib_* folders ("" = none)
    /** Wait until this Unix time (the journal getting its last lines); tests don't wait. */
    std::function<void(double when)> waitUntil;
};

/**
 * The real ones: `journalctl --user -u UNIT --since @A --until @B -o short-unix --no-pager -q` (read-only),
 * ~/.local/share/Steam/logs/eyetracking.txt and ~/eyecam.
 * @return the sources
 */
Sources systemSources();

/** The units read from the journal, with their source. */
struct Unit {
    const char* name;
    Source source;
};
/** frameeyeosc, the panel and eyecam. */
extern const Unit kUnits[3];

/** A run that has ended, for the writer: everything but the logs. */
struct Pending {
    Summary summary;
    std::string statusLines;  ///< status.jsonl
    std::string calibDir;     ///< eyecam's folder for this calibration ("" = look for it under eyecamHome)
    Language language = Language::Ja;  ///< report.txt's
};

/**
 * Write a record (reads the logs: blocking, so off the panel's thread): its folder (a new name if it is there),
 * summary.json, status.jsonl, logs.txt, calib_result.json and report.txt; then keep only the newest kKeep.
 * @param dir the records' folder (made if missing)
 * @param pending the run
 * @param sources the logs
 * @param folder where to write the folder's name
 * @param error why it failed
 * @return true if written
 */
bool writeRecord(const std::string& dir, Pending pending, const Sources& sources, std::string& folder,
                 std::string& error);

/**
 * Delete all but the newest records (by their folders' names; only folders named as records, only the files in them).
 * @param dir the records' folder
 * @param keep how many to keep
 * @return how many were deleted
 */
int prune(const std::string& dir, int keep = kKeep);

/**
 * The records' folder names, newest first.
 * @param dir the records' folder
 * @return the names
 */
std::vector<std::string> folders(const std::string& dir);

/**
 * The records with their summaries, newest first (a folder without a readable summary.json is left out).
 * @param dir the records' folder
 * @return them
 */
std::vector<Summary> list(const std::string& dir);

/**
 * One record.
 * @param dir the records' folder
 * @param folder its folder name
 * @param summary where to write its summary
 * @param files where to write its files with their sizes (report.txt, logs.txt, status.jsonl, calib_result.json,
 *              summary.json; those there)
 * @return true if its summary.json was read
 */
bool load(const std::string& dir, const std::string& folder, Summary& summary, std::vector<FileInfo>* files = nullptr);

/**
 * A size as shown ("4 KB", "86 KB", "1.2 MB", "512 B").
 * @param bytes the size
 * @return the text
 */
std::string sizeText(long long bytes);

/**
 * `--report list`: a line per record, newest first ("fit_2026-10-06_20-45-09  失敗  目を合わせる  reason").
 * @param t texts
 * @param records the records
 * @return the text ("" for none)
 */
std::string listText(const UiText& t, const std::vector<Summary>& records);

/**
 * `--report latest|list|NAME`: what to print.
 * @param t texts
 * @param dir the records' folder
 * @param what "latest", "list" or a folder name
 * @param out where to write it
 * @return 0, or 1 if there is no such record (out then says so)
 */
int cliReport(const UiText& t, const std::string& dir, const std::string& what, std::string& out);

/**
 * A run being recorded: its start, and status.jsonl a line a second.
 */
class Run {
public:
    /**
     * Start (a run already going is dropped).
     * @param kind what runs
     * @param start Unix seconds
     */
    void begin(Kind kind, double start);

    /** @return true between begin and finish */
    bool active() const { return active_; }

    /** @return the summary being filled in (kind, start; the caller adds trigger, mode, conditions) */
    Summary& summary() { return pending_.summary; }

    /**
     * A line of status.jsonl, at most one a second (status.jsonl stops at kMaxStatusBytes).
     * @param when Unix seconds
     * @param coreStatus frameeyeosc's status.json ("" = none)
     * @param eyecamStatus eyecam-rec's ("" = none)
     * @return true if a line was added
     */
    bool sample(double when, const std::string& coreStatus, const std::string& eyecamStatus);

    /**
     * Make a note of eyecam's folder for this calibration (status.json's session_dir while it calibrates).
     * @param dir the folder ("" is ignored)
     */
    void calibDir(const std::string& dir) {
        if (!dir.empty()) pending_.calibDir = dir;
    }

    /**
     * End it.
     * @param end Unix seconds
     * @return the run for the writer (summary's end set; the caller fills in the result)
     */
    Pending finish(double end);

private:
    bool active_ = false;
    double lastSample_ = -1e18;
    Pending pending_;
};

/**
 * Follows a calibration sent to eyecam-rec until it ends, for its record (the panel's loop feeds it eyecam-rec's state):
 * it ends when eyecam-rec leaves "calibrating", is refused, or never starts calibrating within kStartSec of its reply.
 */
class CalibWatch {
public:
    /** How long after the command a calibration that never shows as "calibrating" counts as over (s). */
    static constexpr double kStartSec = 10.0;

    /**
     * The command went to eyecam-rec.
     * @param now monotonic seconds
     */
    void start(double now) {
        active_ = true;
        seen_ = false;
        stopped_ = false;
        sentAt_ = now;
    }

    /** The panel's "Stop" was pressed (while one is followed). */
    void stop() {
        if (active_) stopped_ = true;
    }

    /**
     * eyecam-rec's state, each time it is read.
     * @param calibrating it says "calibrating"
     * @param waitingReply the command's reply hasn't come yet
     * @param now monotonic seconds
     * @return true if the calibration just ended (followed no more)
     */
    bool follow(bool calibrating, bool waitingReply, double now) {
        if (!active_) return false;
        if (calibrating) {
            seen_ = true;
            return false;
        }
        if (!seen_ && (waitingReply || now - sentAt_ <= kStartSec)) return false;
        active_ = false;
        return true;
    }

    /**
     * The command's reply.
     * @param ok eyecam-rec took it
     * @return true if it was refused (the calibration ended before it began; followed no more)
     */
    bool replied(bool ok) {
        if (!active_ || ok || seen_) return false;
        active_ = false;
        return true;
    }

    /** Followed no more (another calibration replaces it). */
    void end() { active_ = false; }

    /** @return true while one is followed */
    bool active() const { return active_; }

    /** @return true if the panel's "Stop" ended it */
    bool stopped() const { return stopped_; }

private:
    bool active_ = false;
    bool seen_ = false;      ///< eyecam-rec was seen calibrating
    bool stopped_ = false;   ///< the panel sent "stop"
    double sentAt_ = 0.0;
};

/**
 * Writes records on its own thread (one after the other), so the panel's loop never waits for the logs.
 */
class Writer {
public:
    /**
     * @param dir the records' folder
     * @param sources where the logs come from
     */
    Writer(std::string dir, Sources sources);
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /**
     * Queue a record.
     * @param pending the run
     */
    void submit(Pending pending);

    /**
     * A record written since the last call (never waits).
     * @param folder its folder name
     * @param kind its kind
     * @return true if there was one
     */
    bool poll(std::string& folder, Kind& kind);

    /** @return true while records wait or are being written */
    bool busy();

    /** @return the records' folder */
    const std::string& dir() const { return dir_; }

private:
    /** A written record. */
    struct Done {
        std::string folder;
        Kind kind;
    };
    std::string dir_;
    Sources sources_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Pending> queue_;
    std::deque<Done> done_;
    bool working_ = false;
    bool stop_ = false;
    std::thread thread_;

    /** The thread: writes what is queued until stopped (what is queued then is still written). */
    void loop();
};

}  // namespace report
