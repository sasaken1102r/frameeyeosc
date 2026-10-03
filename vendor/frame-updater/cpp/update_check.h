// SPDX-License-Identifier: MIT — part of frame-updater by sasaken1102r, shipped under the host app's MIT license
// Checks for and installs app updates by running frame-update.sh, without blocking the caller.
// Shared by the Steam Frame panels (copied from the frame-updater repository; see UPSTREAM next to
// the copy). Depends only on the C++17 standard library and POSIX: no drawing, no i18n, no JSON
// library. The panel maps UpdateStatus to its own text (see strings.md in frame-updater).
//
// Use from the panel's main loop:
//   frame_updater::UpdateChecker updater({script, "frameeyeosc", "sasaken1102r/frameeyeosc",
//                                         FRAMEEYEOSC_VERSION, "frameeyeosc-{version}-steamframe-aarch64.tar.gz",
//                                         {"--with-panel"}});
//   every frame:  updater.tick(config.update_check);
//                 if (updater.revision() != drawn) redraw with updater.status();
//   on "Update":  updater.install();
#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace frame_updater {

/** What the checker needs to know about the app. */
struct UpdaterConfig {
    std::string script;                           ///< absolute path of the installed frame-update.sh
    std::string app;                              ///< app name: cache/config folder and unit name ("frameeyeosc")
    std::string repo;                             ///< GitHub "owner/name"
    std::string currentVersion;                   ///< version of the running app ("0.3.0")
    std::string assetPattern;                     ///< release file, {version} for the version
    std::vector<std::string> defaultInstallArgs;  ///< install.sh options when ~/.config/<app>/install-args is missing
    int recheckSeconds = 3600;                    ///< how often tick() asks the script (it reuses GitHub's answer for 24 h)
};

/** Overall state, for choosing what to show. */
enum class UpdateState {
    Unknown,        ///< not checked yet, or checking is off and nothing is known
    UpToDate,       ///< the running version is the newest release
    Available,      ///< a newer release exists (see UpdateStatus::installable)
    Installing,     ///< an install is running (see UpdateStatus::step)
    Installed,      ///< an install finished while this process ran, and it still runs the old version
    CheckFailed,    ///< the last check failed (see UpdateStatus::error)
    InstallFailed,  ///< the last install failed; the current version is unchanged
};

/** Everything the panel may show. */
struct UpdateStatus {
    UpdateState state = UpdateState::Unknown;
    bool checking = false;     ///< a check is running now; the other fields keep the previous answer
    std::string current;       ///< running version
    std::string latest;        ///< newest release ("" if unknown)
    std::string url;           ///< its release page on GitHub
    bool installable = false;  ///< the newer release can be installed from the panel
    std::string reason;        ///< why it can't: "no-checksums" or "no-asset" (update by hand)
    std::string step;          ///< Installing: "start", "download", "verify", "extract" or "install"
    std::string version;       ///< Installing / Installed / InstallFailed: the version being installed
    std::string error;         ///< CheckFailed / InstallFailed: error code (listed in strings.md)
    std::string message;       ///< English detail, for logs
    long long checkedAt = 0;   ///< when GitHub last answered (Unix time; 0 = never). A cached answer keeps its time
    std::string notes;         ///< Available: the new release's summary, one line of plain text ("" if it has none)
    std::string notesJa;       ///< Available: the same in Japanese, if the release text has a "日本語:" paragraph ("" if not)

    bool operator==(const UpdateStatus& other) const;
    bool operator!=(const UpdateStatus& other) const { return !(*this == other); }
};

/**
 * Runs frame-update.sh in the background and keeps an UpdateStatus. Not thread-safe: call every
 * method from the same thread (the panel's main loop). Child processes are reaped in tick().
 */
class UpdateChecker {
public:
    /**
     * @param config the app's settings (script path, repository, version...)
     */
    explicit UpdateChecker(UpdaterConfig config);
    /** Stops a running check. A running install is left alone (it runs in its own systemd unit). */
    ~UpdateChecker();
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    /**
     * Do the periodic work; cheap enough to call every frame. Collects finished child processes,
     * re-reads the install state file twice a second and, when checking is on, starts a check at
     * the first call and every recheckSeconds after that.
     * @param checkEnabled the app's update_check setting
     */
    void tick(bool checkEnabled);

    /**
     * Check now, asking GitHub even if the last answer is recent (the "Check" button). Returns at
     * once; works whether or not automatic checking is on. If an automatic check is running, the
     * forced one starts right after it. status().checking is true until the answer arrives.
     */
    void checkNow();

    /**
     * Start installing the newest release (after the user confirmed). Returns at once; the
     * install runs in the systemd user unit <app>-update and may restart this process.
     * @return false if an install is already running or the script could not be started
     */
    bool install();

    /**
     * Forget a finished or failed install (removes the state file), e.g. when its message is closed. Installed
     * otherwise stays for the life of the process, even after frame-update.sh drops the state file.
     */
    void dismiss();

    /** @return the current status */
    const UpdateStatus& status() const { return status_; }

    /** @return a number that changes whenever status() changes (for redrawing only when needed) */
    std::uint64_t revision() const { return revision_; }

    /** @return the install log (~/.cache/<app>/update.log) */
    std::string logPath() const { return cacheDir_ + "/update.log"; }

private:
    using Clock = std::chrono::steady_clock;

    /** A running frame-update.sh. */
    struct Child {
        pid_t pid = -1;
        int fd = -1;            ///< read end of its stdout
        bool eof = false;
        std::string out;
        Clock::time_point started;
    };

    bool spawn(const std::vector<std::string>& args, Child& child);
    bool finished(Child& child, int timeoutSeconds);
    void killChild(Child& child);
    std::vector<std::string> baseArgs() const;
    void startCheck(bool force);
    void readStateFile();
    void recompute();

    UpdaterConfig config_;
    std::string cacheDir_;
    Child check_;
    Child installer_;
    bool checkedOnce_ = false;
    bool forcePending_ = false;                       ///< checkNow() while another check ran
    Clock::time_point lastCheck_;
    Clock::time_point lastStateRead_;
    bool stateReadOnce_ = false;
    std::map<std::string, std::string> checkResult_;  ///< last output of "check"
    std::map<std::string, std::string> stateFile_;    ///< update-state.json
    std::map<std::string, std::string> installReply_; ///< failure printed by "install --detach" itself
    bool installRequested_ = false;                   ///< install() called, "install --detach" still running
    long long installRequestedAt_ = 0;                ///< when (Unix time)
    long long startedAt_ = 0;                         ///< when this checker was made (Unix time): older "done" is stale
    std::string installedVersion_;                    ///< Installed seen for this version (until dismiss())
    UpdateStatus status_;
    std::uint64_t revision_ = 0;
};

/**
 * Read a flat one-level JSON object ({"a":"x","b":1,"c":true}) into strings. Nested values are
 * skipped. Used for the script's output and files.
 * @param text the JSON text
 * @return key -> value (strings unescaped, numbers and booleans as written); empty if unreadable
 */
std::map<std::string, std::string> parseFlatJson(const std::string& text);

/**
 * Compare two versions by their numeric x.y.z part ("v0.4.0-rc1" counts as 0.4.0).
 * @return -1, 0 or 1; 0 also when either is not a version
 */
int compareVersions(const std::string& a, const std::string& b);

}  // namespace frame_updater
