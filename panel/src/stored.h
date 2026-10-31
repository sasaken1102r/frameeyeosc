// "Saved data" on the Advanced tab's Files page: what frameeyeosc and eyecam keep on the disk, with each kind's count
// and size and a delete. The eye logs (recorder.h) are counted and deleted here, on a thread of their own, never the
// one being recorded. eyecam's eye-camera recordings (~/eyecam/rec_*) and the eye images its calibrations keep with
// `dev` on are counted and deleted by eyecam-rec, which knows what it is writing: status.json's "stored", and
// `delete rec` / `delete calib_images` on its control socket (refused while it records or calibrates). Nothing here
// talks to OpenVR or cairo, so it can be tested on its own (stored_test.cpp).
#pragma once

#include <ctime>
#include <mutex>
#include <string>
#include <thread>

namespace eyecam {
struct Status;
}

namespace stored {

/** What can be deleted, each with its row. */
enum class Kind { EyeLogs, EyecamRecordings, CalibImages };
constexpr int kKindCount = 3;

/**
 * The kind's word in eyecam-rec's `delete` and its status.json.
 * @param kind the kind
 * @return "rec" / "calib_images", "" for the eye logs (the panel's own)
 */
const char* eyecamName(Kind kind);

/** An eye log written to within this long (s) is being recorded (by the panel, or by hand): it is kept. */
constexpr double kBusySec = 10.0;
/** The eye logs are counted again this often (s) while the page shows. */
constexpr double kRescanSec = 5.0;
/** A delete eyecam-rec answered "ok" to but never showed under way or done is given up on after this long (s). */
constexpr double kEyecamDoneSec = 15.0;

/** How many, and their bytes. */
struct Usage {
    int count = 0;
    long long bytes = 0;
};

/**
 * Whether a file is one of an eye log's three (recorder::uniqueBase: eyes_YYYY-MM-DD_HH-MM-SS, maybe _2, _3...,
 * then .csv, .log or .config.json), and which recording it belongs to.
 * @param name the file's name
 * @return its name without the extension ("eyes_2026-10-31_10-41-01"), or "" if it isn't one
 */
std::string eyeLogStem(const std::string& name);

/**
 * Count the eye logs in a folder: one per recording (its .csv, .log and .config.json together). Those kept by
 * deleteEyeLogs are left out.
 * @param dir the folder (recorder::defaultDir)
 * @param keepBase the recording being made now, its path without the extension ("" = none)
 * @param now the time (files written within kBusySec are kept)
 * @return the count and bytes
 */
Usage eyeLogUsage(const std::string& dir, const std::string& keepBase, std::time_t now);

/**
 * Delete the eye logs in a folder: every eye log's files (regular files only, never links), except the recording
 * being made now and any whose files were written within kBusySec (one recorded by hand). Nothing else in the folder.
 * @param dir the folder
 * @param keepBase the recording being made now, its path without the extension ("" = none)
 * @param now the time
 * @param error why not all of them (the first file that couldn't go), "" if all went
 * @return what went
 */
Usage deleteEyeLogs(const std::string& dir, const std::string& keepBase, std::time_t now, std::string& error);

/** A delete, as its row shows it. */
struct Delete {
    bool running = false;     ///< under way (for eyecam's: sent, and not yet shown done)
    double sentUnix = 0.0;    ///< when it started (Unix seconds)
    bool done = false;        ///< one ended since the panel started
    bool ok = false;          ///< ...and all of it went
    Usage usage;              ///< ...what went
    std::string error;        ///< ...why not all (the first file that couldn't go, or eyecam-rec's reply), "" if
                              ///< it all went
};

/** What the page shows. */
struct View {
    bool eyeLogsCounted = false;  ///< the eye logs were counted once
    Usage eyeLogs;                ///< ...what is there (the one being recorded left out)
    Delete deletes[kKindCount];   ///< each kind's delete
    /**
     * @param kind the kind
     * @return its delete
     */
    Delete& of(Kind kind) { return deletes[static_cast<int>(kind)]; }
    const Delete& of(Kind kind) const { return deletes[static_cast<int>(kind)]; }
};

/**
 * Follow eyecam's deletes in eyecam-rec's status: one sent is done once the status shows a delete of that kind ended
 * after it was sent (and none under way), or given up on after kEyecamDoneSec without showing under way or done.
 * One started elsewhere shows as running while the status says so.
 * @param view the page
 * @param status eyecam-rec's status (fresh)
 * @param nowUnix Unix seconds
 * @return true if the view changed
 */
bool followEyecam(View& view, const eyecam::Status& status, double nowUnix);

/**
 * The eye logs' count and delete, each on a thread of its own so the panel's loop never waits on the disk. One job
 * at a time.
 */
class EyeLogs {
public:
    EyeLogs() = default;
    ~EyeLogs();
    EyeLogs(const EyeLogs&) = delete;
    EyeLogs& operator=(const EyeLogs&) = delete;

    /**
     * Count them (eyeLogUsage), unless a job runs.
     * @param dir the folder
     * @param keepBase the recording being made now ("" = none)
     * @return false if a job runs
     */
    bool count(const std::string& dir, const std::string& keepBase);

    /**
     * Delete them (deleteEyeLogs), then count them again; after the count running now, if one is (take starts it).
     * @param dir the folder
     * @param keepBase the recording being made now ("" = none)
     * @return false if a delete runs or waits already
     */
    bool remove(const std::string& dir, const std::string& keepBase);

    /** @return true while a job runs (or a delete waits for one) */
    bool busy() const;

    /**
     * Take what ended into the view, once, and start a delete that waited for a count. Call every loop.
     * @param view the page
     * @return true if it changed
     */
    bool take(View& view);

private:
    mutable std::mutex mutex_;
    std::thread thread_;
    bool running_ = false;
    bool deleting_ = false;   ///< the job running (or waiting) is a delete
    bool pending_ = false;    ///< a delete waits for the count running now
    std::string pendingDir_;
    std::string pendingKeep_;
    bool counted_ = false;    ///< a count ended, not yet taken
    Usage usage_;
    bool deleted_ = false;    ///< a delete ended, not yet taken
    Usage gone_;
    std::string error_;

    /** Join an ended thread before the next job. */
    void reap();

    /**
     * Start the delete's thread (no job runs).
     * @param dir the folder
     * @param keepBase the recording being made now
     */
    void startRemove(const std::string& dir, const std::string& keepBase);
};

}  // namespace stored
