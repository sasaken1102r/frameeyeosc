// frameeyeosc-panel: a SteamVR dashboard panel for frameeyeosc's settings. It only writes config.json and reads
// status.json; frameeyeosc keeps sending when the panel is closed, crashes or is not installed.
#include "action_socket.h"
#include "auto_recenter.h"
#include "autostart.h"
#include "config.h"
#include "draw.h"
#include "eyecam.h"
#include "eye_preview.h"
#include "fit_text.h"
#include "frame_apps.h"
#include "gaze_dots.h"
#include "gaze_fit.h"
#include "host_entry.h"
#include "i18n.h"
#include "model.h"
#include "panel.h"
#include "recorder.h"
#include "report.h"
#include "setup_tools.h"
#include "sounds.h"
#include "status.h"
#include "target.h"
#include "theme.h"
#include "ui_state.h"
#include "vr_overlay.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ctime>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t gStopRequested = 0;
volatile std::sig_atomic_t gShowRequested = 0;  ///< SIGUSR1 (from a second launch) opens the panel

/** Exit code after "Quit" or the dashboard's "close" (systemd's RestartPreventExitStatus=). */
constexpr int kExitCodeUserQuit = 3;

constexpr int kThumbnailSize = 256;       ///< dashboard thumbnail edge (px)
constexpr double kPanelPollSec = 0.033;   ///< event polling while the panel is visible
constexpr double kClosedPollSec = 0.25;   ///< event polling while it is not
constexpr double kAppsRefreshSec = 5.0;   ///< the other Frame apps' states are looked at again this often while shown
constexpr double kFitPollSec = 1.0 / 90;  ///< every display frame while the eye fit's target is up (without frame sync)
constexpr uint32_t kFrameSyncTimeoutMs = 50;  ///< the longest wait for the compositor's next frame
constexpr double kStatusReadSec = 0.1;    ///< status.json is read this often while the panel is visible...
/** ...and this often while it is closed and only watching for the headset being put on (auto_recenter) */
constexpr double kWatchStatusReadSec = 0.5;
constexpr double kUpdateSettleSec = 60.0; ///< --update-live: longest wait for a check or an install to finish
/** eyecam-rec's status.json is read this often while it runs (its tab shows, or its light is up)... */
constexpr double kEyecamReadSec = 0.1;
/** ...and this often otherwise, only to notice it starting (one failed open a second while it isn't there) */
constexpr double kEyecamIdleReadSec = 1.0;

/** The command line. */
struct Options {
    enum class Mode { Overlay, Print, DumpPng, Probe, SwitchAway, ContrastReport, PlaySound, Report, ReportMake, Help,
                      Version };
    Mode mode = Mode::Overlay;
    std::string configPath;
    std::string statusPath;
    std::string pngPath;
    std::string thumbnailPngPath;
    int thumbnailSize = 256;
    std::string targetPngPath;    ///< --target-png: the eye fit's target image
    std::string targetStyle = "dot";
    std::string dotPngPath;       ///< --dot-png: a debug gaze dot image
    std::string playSound;        ///< --play-sound: play one eye fit cue and exit
    std::string dotKind = "both";
    bool dotRing = false;         ///< --dot-ring: the ring (the gaze before the gaze scale) instead of the dot
    int targetSeconds = 3;
    double targetProgress = 0.7;
    int targetBench = 0;          ///< --target-bench N: time drawing the target N times
    bool fitDetails = false;      ///< --fit-details: "Fine-tune" open on the Eye fit tab
    int fitDetailsPage = 0;       ///< --fit-details lids: its eyelid page
    std::string lidsPage;         ///< --lids-page (--lid-marks: fine): the Eyelids tab's sub-tab (look, blinks, fine,
                                  ///< brows)
    std::string gazePage;         ///< --gaze-page: the Gaze tab's sub-tab (motion, reach, glitch)
    bool history = false;         ///< --history: the version history open (on the Advanced tab)
    std::string historyOpen;      ///< --history-open: the version whose row is open ("" = the installed one)
    double historyScroll = -1.0;  ///< --history-scroll: px it is scrolled (-1 = as opened)
    double advScroll = 0.0;       ///< --adv-scroll: px the Advanced tab's page is scrolled
    std::string changelogDir;     ///< --changelog-dir: read CHANGELOG*.md from here only
    bool diag = false;            ///< --diag: the diagnostics page open (on the Advanced tab)
    std::string advPage;          ///< --adv-page: the Advanced tab's sub-tab (version, trouble, tools, files)
    std::string record;           ///< --record: a record shown on the Advanced tab (its folder name, or latest)
    bool recordsAll = false;      ///< --records-all: all records listed on the Advanced tab
    bool fakeRecords = false;     ///< --fake-records: made-up records instead of the folder's
    bool fakeSaved = false;       ///< --fake-saved: a failed fit's or calibration's record was just saved (its bar)
    std::string appsSheet;        ///< --apps-sheet: the other Frame apps' list, or a confirmation over it
    bool previewSheet = false;    ///< --preview-sheet: the eye preview open...
    std::string previewSpec;      ///< ...as SOURCE[:nomirror][:nocamera][:frozen] ("" = as it opens)
    std::string fakePreview;      ///< --fake-preview: made-up preview values and pictures ("ok" or a state)
    double fakePreviewBrows = NAN;  ///< --fake-preview-brows: brows sent at this level in them (NaN: none)
    int previewBench = 0;         ///< --preview-bench N: time N draws of the panel with the eye preview open
    std::string fakeAppsState;    ///< --fake-apps-state: made-up states of the other apps (KEY=STATE,...)
    std::string fakeAppsLaunch;   ///< --fake-apps-launch: why Konsole didn't open (on the confirmation)
    int fakeAppsExtra = 0;        ///< --fake-apps-extra N: made-up apps without an icon added to the list
    std::string report;           ///< --report: latest, list or a record's folder name
    std::string reportsDir;       ///< --reports-dir: the records' folder (default ~/.local/state/frameeyeosc/reports)
    std::vector<std::string> reportMake;  ///< --report-make KIND START END RESULT REASON (a test hook)
    std::string language;         ///< for --dump-png: overrides the config language (ja / en)
    PanelTab tab = PanelTab::Basic;
    bool previewQuit = false;
    bool previewReset = false;
    bool previewUpdatePrompt = false;
    double switchAwaySec = 3.0;
    // Fake states for --dump-png (any of them draws a made-up state instead of reading the files)
    bool fake = false;
    bool fakeNotRunning = false;
    bool fakePaused = false;
    bool fakeNoTracking = false;
    bool fakeSlowTracker = false; ///< --fake-slow-tracker: the eye tracker delivers 15 samples a second
    bool fakeEtvr = false;
    bool fakeLivelink = false;
    bool fakeModule = false;
    bool fakePupilsOff = false;   ///< --fake-pupils-off: pupils_to_vrchat off
    int fakePupilBits = -1;       ///< --fake-pupil-bits N: pupil_bits (-1 = as the file says)
    bool fakeFixed = false;
    bool fakeTargetNull = false;
    bool fakeLocked = false;
    bool fakeConfigError = false;
    bool fakeSourceError = false;
    bool fakeCoreError = false;   ///< --fake-core-error: frameeyeosc's last_error
    bool fakeBroken = false;
    bool fakeWriteError = false;
    bool fakeModuleLinkError = false;  ///< --fake-module-link-error: the Output tab's module link couldn't open Chromium
    bool fakeCustom = false;
    bool fakeIndependent = false; ///< --fake-independent: independent_eyes on, the gaze pad per eye
    bool fakeV1 = false;          ///< --fake-v1: eye_behavior 1 (the eyes as up to 0.7.5)
    std::string fakeDominantEye;  ///< --fake-dominant-eye: "left" / "right" ("Track Dominant Eye Only")
    bool fakeOpennessSaturated = false;  ///< --fake-openness-saturated: a relaxed open eye reads 1.0
    std::string fakePrompt;       ///< vrchat / etvr / livelink
    std::string fakeUpdate;       ///< a made-up update state (see printUsage)
    std::string fakeUpdateNotes;  ///< --fake-update-notes: the new release's summary ("both", "en" or "long")
    bool updateBeta = false;      ///< --update-beta: "Get test versions" on (instead of panel.json's)
    std::string fakeFit;          ///< a made-up eye fit state (see printUsage)
    std::string fakeRecord;       ///< a made-up eye log state: "recording", "failed" or "autostopped"
    std::string fakeWiden;        ///< lid_widen in the made-up config ("" = the default)
    bool fakeNoWiden = false;     ///< --fake-no-widen: widen off (no widening at all)
    bool fakeBrows = false;       ///< --fake-brows: brow_from_widen on (brows raised with widening)
    std::vector<double> fakeGazeScale;  ///< --fake-gaze-scale: gaze_scale_left, _right, _up, _down (empty = defaults)
    bool fakeGazeDots = false;    ///< --fake-gaze-dots: gaze_debug_dots on
    bool fakeRingsOff = false;    ///< --fake-rings-off: gaze_debug_rings off
    std::string fakeEyecam;       ///< a made-up eyecam-rec state (see printUsage and parseFakeEyecam)
    std::string fakeCamera;       ///< --fake-camera: the eye cameras as frameeyeosc reports them (see printUsage)
    std::string fakePassword = "set";  ///< --fake-password: the setup's password check
    std::string fakeContrib;      ///< --fake-contrib: "Record for squint training" on a made-up screen (fakeContrib)
    double sensitivityDrag = -1;  ///< --sensitivity-drag: the slider as if dragged there (-1 = not)
    std::string eyecamDir;        ///< --eyecam-dir: eyecam-rec's folder (status.json, ctl.sock) instead of the default
    std::string fillPngPath;      ///< --eyecam-fill-png: the eye capture's full-view light image
    std::string fillKind = "bright";  ///< --eyecam-fill bright|dark
    bool updateLive = false;      ///< --dump-png: run the real update checker (and wait for it after clicks)
    std::vector<std::pair<double, double>> clicks;  ///< --click X,Y: presses carried out before the PNG is drawn
    Autostart fakeAutostart = Autostart::Disabled;
};

/**
 * Mark the process for a clean stop (SIGTERM / SIGINT).
 * @param signal the signal (unused)
 */
void onSignal(int /*signal*/) {
    gStopRequested = 1;
}

/**
 * Mark a request to open the panel (SIGUSR1 from a second launch).
 * @param signal the signal (unused)
 */
void onShowSignal(int /*signal*/) {
    gShowRequested = 1;
}

/** Install the SIGTERM / SIGINT / SIGUSR1 handlers. */
void installSignalHandlers() {
    struct sigaction show {};
    show.sa_handler = onShowSignal;
    sigemptyset(&show.sa_mask);
    sigaction(SIGUSR1, &show, nullptr);

    struct sigaction action {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}

/**
 * Monotonic time in seconds.
 * @return seconds
 */
double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/**
 * Seconds since boot, counting the time the Frame was suspended (CLOCK_BOOTTIME; the monotonic clock stops then).
 * @return seconds
 */
double bootSeconds() {
    timespec now {};
    ::clock_gettime(CLOCK_BOOTTIME, &now);
    return static_cast<double>(now.tv_sec) + now.tv_nsec / 1e9;
}

/**
 * Sleep until a stop or show request, or until the time is up.
 * @param seconds how long to wait
 */
void sleepInterruptible(double seconds) {
    const double end = nowSeconds() + seconds;
    while (!gStopRequested && !gShowRequested) {
        const double left = end - nowSeconds();
        if (left <= 0) break;
        std::this_thread::sleep_for(std::chrono::duration<double>(std::fmin(left, 0.5)));
    }
}

/** Print the usage. */
void printUsage() {
    std::printf(
        "Usage: frameeyeosc-panel [options]\n"
        "  (none)                Show the panel on the SteamVR dashboard and stay resident (waits for SteamVR)\n"
        "                        If one is already running, open its panel and exit\n"
        "  --print               Without OpenVR: print what the panel sees (config, status, autostart)\n"
        "  --dump-png PATH       Without OpenVR: draw the panel to a PNG (from the real files) and exit\n"
        "  --thumbnail-png PATH  Draw the dashboard thumbnail (the launcher icon) to a PNG\n"
        "      --thumbnail-size N  Its edge length (default 256)\n"
        "  --play-sound NAME     Play one eye fit cue (pop, pip, buzz, tick, open, done, fail) and exit\n"
        "  --dot-png PATH        Draw a debug gaze dot to a PNG\n"
        "      --dot-kind both|left|right  Which one (default both)\n"
        "      --dot-ring        The ring (the gaze before the gaze scale) instead of the dot\n"
        "  --target-png PATH     Draw the eye fit's target (the head-locked dot) to a PNG\n"
        "      --target-style dot|close|keep|open  The dot, or the eyes-shut step (words from --language)\n"
        "      --target-seconds N  The countdown on it (default 3; 0 = none)\n"
        "      --target-progress F  How much of its ring is left, 0..1 (default 0.7)\n"
        "      --target-bench N  Also draw it N times and print how long one takes, then one gaze point\n"
        "                        paced at 90 frames/s\n"
        "      --language ja|en  Draw in this language instead of the config's\n"
        "      --tab basic|output|gaze|eyefit|lids|advanced|eyecam  Draw this tab (eyecam: only while eyecam-rec runs,\n"
        "                        or with --fake-eyecam)\n"
        "      --fit-details [gaze|lids]  Open \"Fine-tune\" on the Eye fit tab (default: its gaze page)\n"
        "      --lids-page look|blinks|fine|brows  The Eyelids tab's sub-tab (default look)\n"
        "      --gaze-page motion|reach|glitch  The Gaze tab's sub-tab (default motion)\n"
        "      --lid-marks       The same as --lids-page fine (the lid marks)\n"
        "      --history         Open the version history (Advanced tab)\n"
        "      --history-open VERSION  ...with this version's row open instead of the installed one\n"
        "      --history-scroll PX  ...scrolled this far (kept within the list)\n"
        "      --diag            Open the diagnostics page (Advanced tab)\n"
        "      --adv-page version|trouble|tools|files  The Advanced tab's sub-tab (default version)\n"
        "      --adv-scroll PX   Scroll the Advanced tab's page this far (kept within the page; with --tab advanced)\n"
        "      --record NAME|latest  Show a record (its folder name) on the Advanced tab\n"
        "      --records-all     List all records on the Advanced tab\n"
        "      --fake-records    Made-up records (ten) instead of the records' folder; --record N shows the Nth (0 newest)\n"
        "      --fake-saved      A failed fit's or calibration's log was just saved: the bar on its failure screen\n"
        "      --apps-sheet list|confirm[:KEY]|menu  The other Frame apps' list (Advanced > Version), or the confirmation\n"
        "                        over it for an app (KEY: its installer short name; default the first not installed)\n"
        "                        or for the installer's menu\n"
        "      --fake-apps-state KEY=STATE,...  Made-up states of the other apps (KEY keyboard, mic, perf, aux or menu;\n"
        "                        STATE missing, installed, running or busy: its Konsole open). With --fake or this,\n"
        "                        the built-in list with keyboard and perf missing, mic running and aux installed;\n"
        "                        otherwise as installed now (the list fetched before; never fetched here)\n"
        "      --fake-apps-launch busy|nodisplay|nokonsole|failed  Why Konsole didn't open (with --apps-sheet confirm)\n"
        "      --fake-apps-extra N  Add N made-up apps without an icon to the list (it scrolls past four or five)\n"
        "      --preview-sheet [SOURCE[:nomirror][:nocamera][:frozen]]  The eye preview open (SOURCE sent, raw or\n"
        "                        camera; default sent, in a mirror, with the camera pictures). Its pictures are read from\n"
        "                        eyecam-rec's preview file in --eyecam-dir when it is there (eyecam-rec --replay\n"
        "                        --preview), else made up with --fake-preview\n"
        "      --fake-preview [ok|noeyecam|nopicture|closed|nodata]  Made-up preview values and grey pictures (not an\n"
        "                        eye), with the eye cameras set up: all there (ok), eyecam-rec not running, no picture\n"
        "                        yet, the left eye read closed, or no samples from frameeyeosc. Implies --fake\n"
        "      --fake-preview-brows LEVEL  Brows sent in the made-up preview values, both at LEVEL (0 at rest ..\n"
        "                        1 raised all the way). Implies --fake-preview (ok unless given) and --fake-brows\n"
        "      --preview-bench N  Time N draws of the panel with the eye preview open (with --preview-sheet)\n"
        "      --changelog-dir DIR  Read CHANGELOG.md / CHANGELOG.ja.md from DIR instead of next to the binary,\n"
        "                        the checkout (panel/build) or ~/.local/share/frameeyeosc\n"
        "      --preview-quit    Show \"press again to quit\"\n"
        "      --preview-reset   Show \"press again to reset\"\n"
        "      --preview-update-prompt  Show the \"update to ...?\" question (with --fake-update available)\n"
        "      --fake            Draw a made-up state (running, sending to VRChat) instead of the files.\n"
        "                        Each --fake-* below implies --fake\n"
        "      --fake-not-running / --fake-paused / --fake-no-tracking / --fake-etvr / --fake-livelink / --fake-fixed\n"
        "      --fake-frameeyeosc  Sending to VRCFT's FrameEyeOSC module\n"
        "      --fake-target-null  Auto target not found yet\n"
        "      --fake-pupils-off  pupils_to_vrchat off (the Output tab's row for LiveLink with the eye cameras)\n"
        "      --fake-pupil-bits N  pupil_bits 0..4 (the Output tab's \"pupils as bits\" row, with the eye cameras)\n"
        "      --fake-slow-tracker  The eye tracker delivers only 15 samples a second\n"
        "      --fake-locked     Some keys locked by the command line\n"
        "      --fake-config-error  frameeyeosc reports a config error\n"
        "      --fake-source-error  frameeyeosc can't read the eye tracker (an unsupported shared-memory version)\n"
        "      --fake-core-error  frameeyeosc logged a problem 2 minutes ago (its last_error)\n"
        "      --fake-broken     config.json can't be parsed\n"
        "      --fake-write-error  The panel failed to write config.json\n"
        "      --fake-module-link-error  The Output tab's module link couldn't open the browser (with --fake-frameeyeosc)\n"
        "      --fake-custom     Gaze smoothing values that match no preset\n"
        "      --fake-independent  Move eyes separately (the left column shows each eye's gaze)\n"
        "      --fake-v1         The eyes move as up to 0.7.5 (eye_behavior 1)\n"
        "      --fake-gaze-scale L,R,U,D  The gaze scale (gaze_scale_left, _right, _up, _down; 0.05..2). A pair whose\n"
        "                        two values differ shows its lock open\n"
        "      --fake-gaze-dots  The debug gaze dots on (gaze_debug_dots; the Advanced tab's Debug tools)\n"
        "      --fake-rings-off  The dots' rings off (gaze_debug_rings)\n"
        "      --fake-dominant-eye left|right  \"Track Dominant Eye Only\" is on with that eye\n"
        "      --fake-openness-saturated  A relaxed open eye reads 1.0 (SteamOS 0.4.3), so widening can't come through\n"
        "      --fake-prompt vrchat|etvr|livelink|frameeyeosc  The recommended-settings question\n"
        "      --fake-autostart on|off|missing|unknown\n"
        "      --fake-update checking|uptodate|available|manual|installing|installed|checkfailed|installfailed|\n"
        "                    test|testbadge|onbeta|back|backmanual\n"
        "                        A made-up update state (the version row on the Advanced tab). test: the new version\n"
        "                        is a test version; testbadge: up to date and a newer test version exists (the badge\n"
        "                        while \"Get test versions\" is off); onbeta: running test version 0.7.11-beta.2;\n"
        "                        back: on 0.7.11-beta.2 with it off, going back to 0.7.10 is offered (backmanual:\n"
        "                        the same without SHA256SUMS, so not from the panel)\n"
        "      --update-beta     \"Get test versions\" on (with --dump-png; else as panel.json says)\n"
        "      --fake-update-notes both|en|long  With --fake-update available|manual, the new release's summary:\n"
        "                        English and Japanese, English only, or both cut at 300 characters\n"
        "      --fake-fit running|running-center|running-closed|done|done-center|done-standin|\n"
        "                 done-center-standin|fitted|fitted-gaze|\n"
        "                 failed-unsteady|failed-notclosed|failed-movement|failed-lidrange|failed-cancelled|\n"
        "                 failed-left|failed-opened|failed-opened-retry|failed-noresult\n"
        "                        A made-up eye fit (Eye fit tab; failed-opened-retry: a re-wear fit the dashboard\n"
        "                        stopped, tried again once it is closed)\n"
        "      --fake-record recording|failed|autostopped  A made-up eye log (Advanced tab, and the left column)\n"
        "      --fake-widen off|low|normal|high  lid_widen in the made-up settings\n"
        "      --fake-no-widen   widen off in the made-up settings (no eye widens; the Eyelids tab's \"Widening\")\n"
        "      --fake-brows      brow_from_widen on in the made-up settings (\"Widen raises brows\" and its\n"
        "                        strength on the Eyelids tab's \"Brows\")\n"
        "      --fake-eyecam waiting|idle|confirm|searching|recording:LABEL|calibrating[:LABEL]|error|calib-error\n"
        "                        A made-up eyecam-rec for the eye cameras tab (confirm: idle with the recording's\n"
        "                        light warning open, on the Advanced tab; LABEL: lead_in, normal, widen, close,\n"
        "                        squint, look_up, look_down, bright, dark, end (recording: that label's first step\n"
        "                        of eyecam's protocol, with its step chips); calibrating without a label: waiting\n"
        "                        for the video; calib-error: a failed user calibration). Flags after it, each with\n"
        "                        \":\": unlocked (the cameras lost the eyes), nolight (recording without the light),\n"
        "                        user (calibrating / calib-error: the user's calibration; wear: this wear's),\n"
        "                        calib=N (calib_state 0..7, bit 2 a baseline learned by itself; default 0, 1 for a\n"
        "                        failed user calibration), warming=N (learning the relaxed eyes, N s left), ready\n"
        "                        (learned them; both mark an eyecam-rec that learns by itself), saved (calib_saved),\n"
        "                        recalib (recalib_suggested), nolive (not reading the cameras live), auto=VALUE\n"
        "                        (auto_grab), outdated (grab_outdated: an update brought a new tool), sens=V\n"
        "                        (widen_sensitivity 0..1), veto=on|off (lid_veto.on; without it, an eyecam-rec\n"
        "                        before it), buffers (has_buffers), setup /\n"
        "                        nosetup (setup_done true / false; without either, an eyecam-rec before it),\n"
        "                        widen=measured|default (last_calib_widen), done / fail (the setup's calibration\n"
        "                        just ended: the checklist's done screen, or widening on the standard values),\n"
        "                        page=measured|default|user (a calibration from the usual page just ended: its\n"
        "                        card; with setup),\n"
        "                        msg / msgja (eyecam-rec's message with message_en / Japanese only),\n"
        "                        pupil=L,R (pupil_l / pupil_r: each 0..1 or null; without it, an eyecam-rec before\n"
        "                        them), failed=L|R|LR|mixed (calib_failed_eye, with eyecam-rec's message: L / R a\n"
        "                        \"calib wear\" that went through without that eye, with done or page=...; LR both\n"
        "                        eyes' pupils not seen, mixed a different reason per eye, with calib-error; with\n"
        "                        user too, a failed user calibration's message for each eye), prov\n"
        "                        (failed=L|R: provisional values, not the previous ones), lookdown=L|R|LR (with\n"
        "                        page=user: the user's calibration went through with the standard look-down\n"
        "                        correction on that eye, or both), lookprev=L|R|LR (likewise, with the previous\n"
        "                        one kept; with lookdown=R and lookprev=L, one eye each),\n"
        "                        search=not_worn|no_video|one_eye (unlocked, and why: the proximity sensor says\n"
        "                        the headset is off, no eye video, or only one eye's; not_worn reads 12),\n"
        "                        prox=V (the proximity reading), blocks=N (changed blocks before the last look;\n"
        "                        the look itself follows the state: the ring of 8, or where search=... stops),\n"
        "                        lasterror (eyecam-rec's last error, 3 minutes ago), camfps=N (cam_fps: the eye\n"
        "                        cameras deliver N frames a second, 1..500; without it, an eyecam-rec before\n"
        "                        cam_fps. The calibration messages' counts follow it, with eyecam-rec's note under\n"
        "                        60). The last calibration follows setup, failed=... and calib-error\n"
        "      --sensitivity-drag V  Draw the widening sensitivity slider as if dragged to V (with sens=...)\n"
        "      --fake-password set|unset|unknown  The setup's password check (default: set)\n"
        "      --fake-contrib STATE  \"Record for squint training\" open on a made-up screen (the eye cameras set up\n"
        "                        and idle unless --fake-eyecam says otherwise): consent, deleted, eyewear,\n"
        "                        ready, nospace, running, failed, incomplete, truncated, valve (not usable:\n"
        "                        a file missing, a raw file cut short, Valve's values missing), done (just\n"
        "                        recorded), list (\"Recordings\"), packing, packed, delete, deleting (with --adv-page tools: its\n"
        "                        row on Debug tools, the sheet closed; none for the row before the first)\n"
        "      --fake-camera both|left|right|uncalibrated|absent|error|off  The eye cameras as frameeyeosc reports\n"
        "                        them (off: camera_lids off)\n"
        "  --eyecam-fill-png PATH  Draw the eye capture's full-view light (with --language) to a PNG\n"
        "      --eyecam-fill bright|dark  Which one (default bright)\n"
        "  --eyecam-dir DIR      eyecam-rec's folder: status.json and ctl.sock (default $XDG_RUNTIME_DIR/eyecam)\n"
        "      --update-live     Run the real update checker: check first, and after each --click wait for the\n"
        "                        check or install it started (installs really happen; for testing with a fake GitHub)\n"
        "      --click X,Y       Press the panel at X,Y first (repeatable; writes --config; not with --fake)\n"
        "  --report latest|list|NAME  Print the newest record's report.txt, a line per record, or that record's\n"
        "                        report (records of calibrations and eye fits; works over SSH, without SteamVR)\n"
        "      --reports-dir DIR  The records' folder (default ~/.local/state/frameeyeosc/reports)\n"
        "  --report-make KIND START END RESULT REASON  For testing: write a record of that window from today's logs\n"
        "                        (KIND fit|recenter|calib-wear|calib-user, START / END \"YYYY-MM-DD HH:MM:SS\",\n"
        "                        RESULT ok|failed|partial|stopped) into --reports-dir, and print its folder\n"
        "  --contrast-report     Print the WCAG contrast ratio of every color pair on screen\n"
        "  --probe               Diagnostics: connect to SteamVR as a background app and describe the resident panel\n"
        "  --probe-switch-away [S]  Diagnostics: switch the dashboard to a temporary overlay for S s (default 3)\n"
        "  --version             Print the version\n"
        "  --action recenter|fit|stop|relearn|open  Ask the running panel to start the gaze re-center, start the eye\n"
        "                        fit, stop the fit or re-center running, relearn the relaxed eyes on the eye cameras,\n"
        "                        or open the panel (as its buttons do; the dashboard may stay closed). Never starts the\n"
        "                        panel. Prints one line to stderr and exits with:\n"
        "                        0 accepted (started / stopped / shown), 1 unknown action or bad arguments (any usage\n"
        "                        error of this program exits 1), 2 panel not running, 3 no answer from the panel,\n"
        "                        4 frameeyeosc not running, 5 re-center before any gaze fit, 6 busy: an eye fit is\n"
        "                        running (stop it first), 7 nothing to stop, 8 the fit's values are locked by\n"
        "                        frameeyeosc's command line, 9 no eye data (the headset off?), 10 busy: a re-center is\n"
        "                        running, 11 relearn: the eye cameras aren't in use or ready\n"
        "  --config PATH         Config file (default ~/.config/frameeyeosc/config.json)\n"
        "  --status PATH         Status file (default $XDG_RUNTIME_DIR/frameeyeosc/status.json)\n");
}

/**
 * Take --preview-sheet's value apart: sent, raw or camera, then any of :nomirror, :nocamera, :frozen ("" = as it opens).
 * @param text the value
 * @param source which values drive the drawn eyes
 * @param mirror as in a mirror
 * @param camera with the eye cameras' pictures
 * @param frozen stopped
 * @return false if something in it is unknown
 */
bool parsePreviewSpec(const std::string& text, eye_preview::Source& source, bool& mirror, bool& camera, bool& frozen) {
    source = eye_preview::Source::Sent;
    mirror = true;
    camera = true;
    frozen = false;
    if (text.empty()) return true;
    size_t from = 0;
    for (bool first = true;; first = false) {
        const size_t colon = text.find(':', from);
        const std::string part = text.substr(from, colon == std::string::npos ? std::string::npos : colon - from);
        if (first && (part == "sent" || part == "raw" || part == "camera")) {
            source = part == "sent" ? eye_preview::Source::Sent
                     : part == "raw" ? eye_preview::Source::Raw
                                     : eye_preview::Source::Camera;
        } else if (!first && part == "nomirror") {
            mirror = false;
        } else if (!first && part == "nocamera") {
            camera = false;
        } else if (!first && part == "frozen") {
            frozen = true;
        } else {
            return false;
        }
        if (colon == std::string::npos) return true;
        from = colon + 1;
    }
}

/** --fake-contrib's screens (fakeContrib). */
const std::string kFakeContribStates[] = {"none",    "consent", "deleted",    "eyewear", "ready",   "nospace",
                                          "running", "failed",  "incomplete", "truncated", "valve", "done",
                                          "list",    "packing", "packed",     "delete",  "deleting"};

/** --fake-eyecam, taken apart. */
struct FakeEyecam {
    std::string state;      ///< waiting, idle, confirm, searching, recording, calibrating, error, calib-error
    std::string label;      ///< recording's and calibrating's step label ("" = none)
    bool unlocked = false;  ///< the cameras lost the eyes
    bool noLight = false;   ///< the recording runs without the light
    int calibUser = -1;     ///< 1 = the user's calibration, 0 = this wear's, -1 = from the label
    int calibState = -1;    ///< calib_state (-1 = the state's default)
    bool recalib = false;   ///< recalib_suggested
    bool noLive = false;    ///< not reading the cameras live
    std::string autoGrab;   ///< auto_grab ("" = an eyecam-rec without it)
    bool outdated = false;  ///< grab_outdated
    std::string baseline;   ///< "warming" or "ready" ("" = an eyecam-rec that doesn't learn the baseline by itself)
    double warmupS = -1.0;  ///< warmup_remaining_s while warming (-1 = none)
    bool saved = false;     ///< calib_saved
    double sens = -1.0;     ///< widen_sensitivity (-1 = an eyecam-rec without it)
    int veto = -1;          ///< lid_veto.on: 1 / 0 (-1 = an eyecam-rec without it)
    bool buffers = false;   ///< has_buffers
    int setup = -1;         ///< setup_done: 1 / 0 (-1 = an eyecam-rec before it)
    std::string widen;      ///< last_calib_widen ("" = none)
    std::string result;     ///< "done" / "fail": the setup's calibration just ended that way ("" = no)
    std::string page;       ///< "measured" / "default" / "user": one from the usual page just ended ("" = no)
    int message = 0;        ///< 1 = a message with message_en, 2 = a message without it (an older eyecam-rec)
    bool hasPupil = false;  ///< pupil_l / pupil_r given (an eyecam-rec that says whether it finds the pupils)
    double pupil[2] = {0.0, 0.0};  ///< ...their values (NaN = null)
    std::string failed;     ///< "L" / "R" / "LR" / "mixed": calib_failed_eye and its message ("" = none)
    bool provisional = false;  ///< failed=L|R: provisional values (no earlier ones)
    std::string lookDown;   ///< "L" / "R" / "LR": a user calibration on the standard look-down correction there
    std::string lookPrevious;  ///< ...on the previous one kept there (an eye in lookDown too is on the standard one)
    std::string search;     ///< search: why the video isn't found (not_worn / no_video / one_eye; "" = an older
                            ///< eyecam-rec without it, or locked)
    double prox = -1.0;     ///< prox=V: the proximity reading (-1 = as the search says)
    int blocks = -1;        ///< blocks=N: changed blocks before the last look (-1 = as the search says)
    bool lastError = false; ///< lasterror: eyecam-rec's last error
    double camFps = -1.0;   ///< camfps=N: the cameras' frame rate (-1 = an eyecam-rec before cam_fps)
    bool noPreview = false; ///< nopreview: an eyecam-rec without the eye preview (no "preview" in its status)
};

/**
 * Parse pupil=L,R: each a share 0..1 or "null".
 * @param text after "pupil="
 * @param fake where to write
 * @return false if it isn't two of them
 */
bool parseFakePupil(const std::string& text, FakeEyecam& fake) {
    const size_t comma = text.find(',');
    if (comma == std::string::npos) return false;
    const std::string values[2] = {text.substr(0, comma), text.substr(comma + 1)};
    for (int eye = 0; eye < 2; ++eye) {
        const std::string& value = values[eye];
        if (value == "null") {
            fake.pupil[eye] = std::numeric_limits<double>::quiet_NaN();
        } else if (!value.empty() && value.find_first_not_of("0123456789.") == std::string::npos) {
            fake.pupil[eye] = std::atof(value.c_str());
        } else {
            return false;
        }
    }
    fake.hasPupil = true;
    return true;
}

/**
 * Take --fake-eyecam apart: STATE[:LABEL][:flag]..., where a label only follows recording (which needs one) and
 * calibrating.
 * @param text the option's value
 * @param fake where to write
 * @return false if something in it is unknown
 */
bool parseFakeEyecam(const std::string& text, FakeEyecam& fake) {
    fake = FakeEyecam();
    std::vector<std::string> parts;
    for (size_t from = 0;;) {
        const size_t colon = text.find(':', from);
        parts.push_back(text.substr(from, colon == std::string::npos ? std::string::npos : colon - from));
        if (colon == std::string::npos) break;
        from = colon + 1;
    }
    fake.state = parts[0];
    static const char* const kStates[] = {"waiting", "idle",        "confirm", "searching",
                                          "recording", "calibrating", "error",   "calib-error"};
    if (std::find(std::begin(kStates), std::end(kStates), fake.state) == std::end(kStates)) return false;
    size_t next = 1;
    const bool takesLabel = fake.state == "recording" || fake.state == "calibrating";
    if (takesLabel && next < parts.size() && eyecam::parseStep(parts[next]) != eyecam::Step::Unknown) {
        fake.label = parts[next++];
    }
    if (fake.state == "recording" && fake.label.empty()) return false;
    for (; next < parts.size(); ++next) {
        const std::string& flag = parts[next];
        if (flag == "unlocked") {
            fake.unlocked = true;
        } else if (flag == "nolight") {
            fake.noLight = true;
        } else if (flag == "user" || flag == "wear") {
            fake.calibUser = flag == "user" ? 1 : 0;
        } else if (flag == "recalib") {
            fake.recalib = true;
        } else if (flag == "nolive") {
            fake.noLive = true;
        } else if (flag.rfind("auto=", 0) == 0 && flag.size() > 5) {
            fake.autoGrab = flag.substr(5);
        } else if (flag.rfind("warming=", 0) == 0 && flag.size() > 8 &&
                   flag.find_first_not_of("0123456789.", 8) == std::string::npos) {
            fake.baseline = "warming";
            fake.warmupS = std::atof(flag.c_str() + 8);
        } else if (flag == "msg" || flag == "msgja") {
            fake.message = flag == "msg" ? 1 : 2;
        } else if (flag == "buffers") {
            fake.buffers = true;
        } else if (flag == "outdated") {
            fake.outdated = true;
        } else if (flag == "setup" || flag == "nosetup") {
            fake.setup = flag == "setup" ? 1 : 0;
        } else if (flag == "widen=measured" || flag == "widen=default") {
            fake.widen = flag.substr(6);
        } else if (flag == "done" || flag == "fail") {
            fake.result = flag;
        } else if (flag == "page=measured" || flag == "page=default" || flag == "page=user") {
            fake.page = flag.substr(5);
        } else if (flag.rfind("pupil=", 0) == 0) {
            if (!parseFakePupil(flag.substr(6), fake)) return false;
        } else if (flag == "failed=L" || flag == "failed=R" || flag == "failed=LR" || flag == "failed=mixed") {
            fake.failed = flag.substr(7);
        } else if (flag == "lookdown=L" || flag == "lookdown=R" || flag == "lookdown=LR") {
            fake.lookDown = flag.substr(9);
        } else if (flag == "lookprev=L" || flag == "lookprev=R" || flag == "lookprev=LR") {
            fake.lookPrevious = flag.substr(9);
        } else if (flag == "prov") {
            fake.provisional = true;
        } else if (flag == "search=not_worn" || flag == "search=no_video" || flag == "search=one_eye") {
            fake.search = flag.substr(7);
            fake.unlocked = true;
        } else if (flag.rfind("prox=", 0) == 0 && flag.size() > 5 &&
                   flag.find_first_not_of("0123456789.", 5) == std::string::npos) {
            fake.prox = std::atof(flag.c_str() + 5);
        } else if (flag.rfind("blocks=", 0) == 0 && flag.size() > 7 &&
                   flag.find_first_not_of("0123456789", 7) == std::string::npos) {
            fake.blocks = std::atoi(flag.c_str() + 7);
        } else if (flag == "lasterror") {
            fake.lastError = true;
        } else if (flag.rfind("camfps=", 0) == 0 && flag.size() > 7 &&
                   flag.find_first_not_of("0123456789.", 7) == std::string::npos) {
            fake.camFps = std::atof(flag.c_str() + 7);
            if (fake.camFps < 1 || fake.camFps > 500) return false;
        } else if (flag == "ready") {
            fake.baseline = "ready";
        } else if (flag == "saved") {
            fake.saved = true;
        } else if (flag == "nopreview") {
            fake.noPreview = true;
        } else if (flag.rfind("sens=", 0) == 0 && flag.size() > 5 &&
                   flag.find_first_not_of("0123456789.", 5) == std::string::npos) {
            fake.sens = std::atof(flag.c_str() + 5);
        } else if (flag == "veto=on" || flag == "veto=off") {
            fake.veto = flag == "veto=on" ? 1 : 0;
        } else if (flag.rfind("calib=", 0) == 0 && flag.size() == 7 && flag[6] >= '0' && flag[6] <= '7') {
            fake.calibState = flag[6] - '0';
        } else {
            return false;
        }
    }
    return true;
}

/**
 * Take --fake-apps-state apart: KEY=STATE pairs separated by commas. KEY is an installer short name or a name of the
 * built-in list, or "menu" (the installer's menu, only busy); STATE missing, installed, running or busy (not installed
 * and its Konsole still open).
 * @param text the option's value
 * @param states where to add each app's state (by its name)
 * @param busy where to add the short names shown busy ("" = the menu)
 * @return false if a pair can't be read
 */
bool parseFakeApps(const std::string& text, std::map<std::string, frame_apps::AppState>& states,
                   std::vector<std::string>& busy) {
    using frame_apps::AppState;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string pair = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? text.size() + 1 : comma + 1;
        const size_t equals = pair.find('=');
        if (equals == std::string::npos) return false;
        const std::string key = pair.substr(0, equals);
        const std::string state = pair.substr(equals + 1);
        if (key == "menu") {
            if (state != "busy") return false;
            busy.push_back("");
            continue;
        }
        const frame_apps::AppInfo* app = frame_apps::findApp(key);
        if (app == nullptr) return false;
        if (state == "missing" || state == "busy") {
            states[app->name] = AppState::Missing;
        } else if (state == "installed") {
            states[app->name] = AppState::Installed;
        } else if (state == "running") {
            states[app->name] = AppState::Running;
        } else {
            return false;
        }
        if (state == "busy") busy.push_back(app->key);
    }
    return true;
}

/**
 * What the other Frame apps' list needs to know about this one: its name (left out of the list), and no apps it uses.
 * @return the config (the user's home and cache)
 */
frame_apps::ManagerConfig appsManagerConfig() {
    return frame_apps::ManagerConfig {"frameeyeosc", {}};
}

/**
 * The other Frame apps for --dump-png: made up (--fake, --fake-apps-state: the built-in list with keyboard and perf
 * missing, mic running and aux installed, then the option's states) or as installed now (the list fetched before by
 * any app; never fetched here), and --fake-apps-extra's made-up apps after them.
 * @param options the command line
 * @return the list as the panel shows it
 */
AppsView dumpApps(const Options& options) {
    using frame_apps::AppState;
    frame_apps::ManagerConfig config = appsManagerConfig();
    const bool fake = options.fake || !options.fakeAppsState.empty();
    if (fake) config.fetch.cacheDir.clear();  // the built-in list, the same on every machine
    frame_apps::AppsManager apps(config);
    if (fake) {
        std::map<std::string, AppState> states = {{"frame-jp-keyboard", AppState::Missing},
                                                  {"frame-mic-tuner", AppState::Running},
                                                  {"frame-perf-overlay", AppState::Missing},
                                                  {"frame-aux-shortcuts", AppState::Installed}};
        std::vector<std::string> busy;
        parseFakeApps(options.fakeAppsState, states, busy);
        apps.setForPreview(states, busy);
    } else {
        // Which apps run comes from systemctl, a moment later
        apps.refreshNow();
        for (int i = 0; i < 50 && !apps.tick(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    AppsView view;
    view.entries = apps.entries();
    view.menuBusy = apps.busy("");
    for (int i = 0; i < options.fakeAppsExtra; ++i) {
        frame_apps::Entry extra;
        const std::string n = std::to_string(i + 1);
        extra.app = {"frame-example-" + n, "example", "Ex" + n, "[今後のアプリの説明がここに入ります]",
                     "[A future app's description goes here]", "frame-example.service", ".local/bin/frame-example",
                     "frame-example-128.png", ""};
        extra.state = AppState::Missing;
        view.entries.push_back(extra);
    }
    return view;
}

/**
 * Parse the command line.
 * @param argc argument count
 * @param argv arguments
 * @param options where to write
 * @return true if valid
 */
bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasNext = i + 1 < argc;
        if (arg == "--print") {
            options.mode = Options::Mode::Print;
        } else if (arg == "--dump-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.pngPath = argv[++i];
        } else if (arg == "--thumbnail-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.thumbnailPngPath = argv[++i];
        } else if (arg == "--target-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.targetPngPath = argv[++i];
        } else if (arg == "--play-sound" && hasNext) {
            options.mode = Options::Mode::PlaySound;
            options.playSound = argv[++i];
            sounds::Cue cue = sounds::Cue::Pop;
            if (!sounds::parse(options.playSound, cue)) {
                std::fprintf(stderr, "--play-sound: unknown cue %s\n", options.playSound.c_str());
                return false;
            }
        } else if (arg == "--dot-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.dotPngPath = argv[++i];
        } else if (arg == "--dot-ring") {
            options.dotRing = true;
        } else if (arg == "--dot-kind" && hasNext) {
            options.dotKind = argv[++i];
            if (options.dotKind != "both" && options.dotKind != "left" && options.dotKind != "right") {
                std::fprintf(stderr, "--dot-kind must be both, left or right: %s\n", options.dotKind.c_str());
                return false;
            }
        } else if (arg == "--target-style" && hasNext) {
            options.targetStyle = argv[++i];
            if (options.targetStyle != "dot" && options.targetStyle != "close" && options.targetStyle != "keep" &&
                options.targetStyle != "open") {
                std::fprintf(stderr, "--target-style must be dot, close, keep or open: %s\n", options.targetStyle.c_str());
                return false;
            }
        } else if (arg == "--target-bench" && hasNext) {
            options.targetBench = std::max(0, std::min(100000, std::atoi(argv[++i])));
        } else if (arg == "--lid-marks") {
            options.lidsPage = "fine";
        } else if (arg == "--lids-page" && hasNext) {
            options.lidsPage = argv[++i];
            LidsPage page;
            if (!ui_state::parseLidsPage(options.lidsPage, page)) {
                std::fprintf(stderr, "--lids-page must be look, blinks, fine or brows: %s\n", options.lidsPage.c_str());
                return false;
            }
        } else if (arg == "--history") {
            options.history = true;
        } else if (arg == "--history-open" && hasNext) {
            options.history = true;
            options.historyOpen = argv[++i];
        } else if (arg == "--history-scroll" && hasNext) {
            options.history = true;
            options.historyScroll = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--diag") {
            options.diag = true;
        } else if (arg == "--adv-scroll" && hasNext) {
            options.advScroll = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--gaze-page" && hasNext) {
            options.gazePage = argv[++i];
            GazePage page;
            if (!ui_state::parseGazePage(options.gazePage, page)) {
                std::fprintf(stderr, "--gaze-page must be motion, reach or glitch: %s\n", options.gazePage.c_str());
                return false;
            }
        } else if (arg == "--adv-page" && hasNext) {
            options.advPage = argv[++i];
            AdvPage page;
            if (!ui_state::parsePage(options.advPage, page)) {
                std::fprintf(stderr, "--adv-page must be version, trouble, tools or files: %s\n", options.advPage.c_str());
                return false;
            }
        } else if (arg == "--record" && hasNext) {
            options.record = argv[++i];
        } else if (arg == "--records-all") {
            options.recordsAll = true;
        } else if (arg == "--fake-records") {
            options.fakeRecords = true;
        } else if (arg == "--fake-saved") {
            options.fake = options.fakeSaved = true;
        } else if (arg == "--preview-sheet") {
            options.previewSheet = true;
            // (its value is optional)
            if (hasNext && std::strncmp(argv[i + 1], "--", 2) != 0) {
                options.previewSpec = argv[++i];
                eye_preview::Source source;
                bool mirror = true;
                bool camera = true;
                bool frozen = false;
                if (!parsePreviewSpec(options.previewSpec, source, mirror, camera, frozen)) {
                    std::fprintf(stderr,
                                 "--preview-sheet takes sent, raw or camera, then :nomirror, :nocamera, :frozen: %s\n",
                                 options.previewSpec.c_str());
                    return false;
                }
            }
        } else if (arg == "--fake-preview") {
            options.fakePreview = "ok";
            if (hasNext && std::strncmp(argv[i + 1], "--", 2) != 0) {
                options.fakePreview = argv[++i];
                const std::string& state = options.fakePreview;
                if (state != "ok" && state != "noeyecam" && state != "nopicture" && state != "closed" &&
                    state != "nodata") {
                    std::fprintf(stderr, "--fake-preview must be ok, noeyecam, nopicture, closed or nodata: %s\n",
                                 state.c_str());
                    return false;
                }
            }
            options.fake = true;
        } else if (arg == "--fake-preview-brows" && hasNext) {
            const double level = std::atof(argv[++i]);
            if (!(level >= 0.0 && level <= 1.0)) {
                std::fprintf(stderr, "--fake-preview-brows takes a level from 0 to 1: %s\n", argv[i]);
                return false;
            }
            options.fakePreviewBrows = level;
            // (a --fake-preview STATE given too, before or after, is kept)
            if (options.fakePreview.empty()) options.fakePreview = "ok";
            options.fake = options.fakeBrows = true;
        } else if (arg == "--preview-bench" && hasNext) {
            options.previewBench = std::max(0, std::min(10000, std::atoi(argv[++i])));
        } else if (arg == "--apps-sheet" && hasNext) {
            options.appsSheet = argv[++i];
            const std::string& sheet = options.appsSheet;
            const bool confirm = sheet.rfind("confirm:", 0) == 0 && frame_apps::isValidKey(sheet.substr(8));
            if (sheet != "list" && sheet != "confirm" && sheet != "menu" && !confirm) {
                std::fprintf(stderr, "--apps-sheet must be list, confirm, confirm:KEY or menu: %s\n", sheet.c_str());
                return false;
            }
        } else if (arg == "--fake-apps-state" && hasNext) {
            options.fakeAppsState = argv[++i];
            std::map<std::string, frame_apps::AppState> states;
            std::vector<std::string> busy;
            if (!parseFakeApps(options.fakeAppsState, states, busy)) {
                std::fprintf(stderr,
                             "--fake-apps-state takes KEY=STATE pairs (KEY keyboard, mic, perf, aux or menu; STATE "
                             "missing, installed, running or busy): %s\n",
                             options.fakeAppsState.c_str());
                return false;
            }
        } else if (arg == "--fake-apps-launch" && hasNext) {
            options.fakeAppsLaunch = argv[++i];
            const std::string& launch = options.fakeAppsLaunch;
            if (launch != "busy" && launch != "nodisplay" && launch != "nokonsole" && launch != "failed") {
                std::fprintf(stderr, "--fake-apps-launch must be busy, nodisplay, nokonsole or failed: %s\n",
                             launch.c_str());
                return false;
            }
        } else if (arg == "--fake-apps-extra" && hasNext) {
            options.fakeAppsExtra = std::max(0, std::min(20, std::atoi(argv[++i])));
        } else if (arg == "--report" && hasNext) {
            options.mode = Options::Mode::Report;
            options.report = argv[++i];
        } else if (arg == "--reports-dir" && hasNext) {
            options.reportsDir = argv[++i];
        } else if (arg == "--report-make" && i + 5 < argc) {
            options.mode = Options::Mode::ReportMake;
            for (int k = 0; k < 5; ++k) options.reportMake.push_back(argv[++i]);
        } else if (arg == "--changelog-dir" && hasNext) {
            options.changelogDir = argv[++i];
        } else if (arg == "--fit-details") {
            options.fitDetails = true;
            if (hasNext && (std::string(argv[i + 1]) == "gaze" || std::string(argv[i + 1]) == "lids")) {
                options.fitDetailsPage = std::string(argv[++i]) == "lids" ? 1 : 0;
            }
        } else if (arg == "--target-seconds" && hasNext) {
            options.targetSeconds = std::max(0, std::min(99, std::atoi(argv[++i])));
        } else if (arg == "--target-progress" && hasNext) {
            options.targetProgress = std::max(0.0, std::min(1.0, std::atof(argv[++i])));
        } else if (arg == "--thumbnail-size" && hasNext) {
            options.thumbnailSize = std::max(16, std::min(1024, std::atoi(argv[++i])));
        } else if (arg == "--language" && hasNext) {
            options.language = argv[++i];
            Language check;
            if (!parseLanguage(options.language, check)) {
                std::fprintf(stderr, "--language must be ja or en: %s\n", options.language.c_str());
                return false;
            }
        } else if (arg == "--tab" && hasNext) {
            const std::string tab = argv[++i];
            if (tab == "basic") {
                options.tab = PanelTab::Basic;
            } else if (tab == "output") {
                options.tab = PanelTab::Output;
            } else if (tab == "gaze") {
                options.tab = PanelTab::Gaze;
            } else if (tab == "eyefit") {
                options.tab = PanelTab::EyeFit;
            } else if (tab == "lids") {
                options.tab = PanelTab::Lids;
            } else if (tab == "advanced") {
                options.tab = PanelTab::Advanced;
            } else if (tab == "eyecam") {
                options.tab = PanelTab::Eyecam;
            } else {
                std::fprintf(stderr, "--tab must be basic, output, gaze, eyefit, lids, advanced or eyecam: %s\n",
                             tab.c_str());
                return false;
            }
        } else if (arg == "--preview-quit") {
            options.previewQuit = true;
        } else if (arg == "--preview-reset") {
            options.previewReset = true;
        } else if (arg == "--preview-update-prompt") {
            options.previewUpdatePrompt = true;
        } else if (arg == "--fake") {
            options.fake = true;
        } else if (arg == "--fake-not-running") {
            options.fake = options.fakeNotRunning = true;
        } else if (arg == "--fake-paused") {
            options.fake = options.fakePaused = true;
        } else if (arg == "--fake-slow-tracker") {
            options.fake = options.fakeSlowTracker = true;
        } else if (arg == "--fake-no-tracking") {
            options.fake = options.fakeNoTracking = true;
        } else if (arg == "--fake-etvr") {
            options.fake = options.fakeEtvr = true;
        } else if (arg == "--fake-livelink") {
            options.fake = options.fakeLivelink = true;
        } else if (arg == "--fake-frameeyeosc") {
            options.fake = options.fakeModule = true;
        } else if (arg == "--fake-pupils-off") {
            options.fake = options.fakePupilsOff = true;
        } else if (arg == "--fake-pupil-bits" && hasNext) {
            const std::string value = argv[++i];
            if (value.size() != 1 || value[0] < '0' || value[0] > '4') {
                std::fprintf(stderr, "--fake-pupil-bits takes 0 to 4, not %s\n", value.c_str());
                return false;
            }
            options.fake = true;
            options.fakePupilBits = value[0] - '0';
        } else if (arg == "--fake-fixed") {
            options.fake = options.fakeFixed = true;
        } else if (arg == "--fake-target-null") {
            options.fake = options.fakeTargetNull = true;
        } else if (arg == "--fake-locked") {
            options.fake = options.fakeLocked = true;
        } else if (arg == "--fake-config-error") {
            options.fake = options.fakeConfigError = true;
        } else if (arg == "--fake-core-error") {
            options.fake = options.fakeCoreError = true;
        } else if (arg == "--fake-source-error") {
            options.fake = options.fakeSourceError = true;
        } else if (arg == "--fake-broken") {
            options.fake = options.fakeBroken = true;
        } else if (arg == "--fake-write-error") {
            options.fake = options.fakeWriteError = true;
        } else if (arg == "--fake-module-link-error") {
            options.fake = options.fakeModuleLinkError = true;
        } else if (arg == "--fake-custom") {
            options.fake = options.fakeCustom = true;
        } else if (arg == "--fake-independent") {
            options.fake = options.fakeIndependent = true;
        } else if (arg == "--fake-v1") {
            options.fake = options.fakeV1 = true;
        } else if (arg == "--fake-no-widen") {
            options.fake = options.fakeNoWiden = true;
        } else if (arg == "--fake-brows") {
            options.fake = options.fakeBrows = true;
        } else if (arg == "--fake-gaze-dots") {
            options.fake = options.fakeGazeDots = true;
        } else if (arg == "--fake-rings-off") {
            options.fake = options.fakeRingsOff = true;
        } else if (arg == "--fake-gaze-scale" && hasNext) {
            const std::string value = argv[++i];
            std::vector<double> scales;
            size_t start = 0;
            while (start <= value.size()) {
                const size_t comma = value.find(',', start);
                const std::string part = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                char* end = nullptr;
                const double scale = std::strtod(part.c_str(), &end);
                if (part.empty() || end == nullptr || *end != '\0' || !(scale >= 0.05 && scale <= 2.0)) break;
                scales.push_back(scale);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            if (scales.size() != 4 || std::count(value.begin(), value.end(), ',') != 3) {
                std::fprintf(stderr, "--fake-gaze-scale takes four values 0.05..2 (L,R,U,D), not %s\n", value.c_str());
                return false;
            }
            options.fake = true;
            options.fakeGazeScale = scales;
        } else if (arg == "--fake-dominant-eye" && hasNext) {
            options.fakeDominantEye = argv[++i];
            if (options.fakeDominantEye != "left" && options.fakeDominantEye != "right") {
                std::fprintf(stderr, "--fake-dominant-eye must be left or right: %s\n",
                             options.fakeDominantEye.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-openness-saturated") {
            options.fake = options.fakeOpennessSaturated = true;
        } else if (arg == "--fake-prompt" && hasNext) {
            options.fakePrompt = argv[++i];
            if (options.fakePrompt != kOutputVrchat && options.fakePrompt != kOutputEtvr &&
                options.fakePrompt != kOutputLivelink && options.fakePrompt != kOutputModule) {
                std::fprintf(stderr, "--fake-prompt must be vrchat, etvr, livelink or frameeyeosc: %s\n",
                             options.fakePrompt.c_str());
                return false;
            }
        } else if (arg == "--fake-autostart" && hasNext) {
            const std::string state = argv[++i];
            if (state == "on") {
                options.fakeAutostart = Autostart::Enabled;
            } else if (state == "off") {
                options.fakeAutostart = Autostart::Disabled;
            } else if (state == "missing") {
                options.fakeAutostart = Autostart::Missing;
            } else if (state == "unknown") {
                options.fakeAutostart = Autostart::Unknown;
            } else {
                std::fprintf(stderr, "--fake-autostart must be on, off, missing or unknown: %s\n", state.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-update" && hasNext) {
            options.fakeUpdate = argv[++i];
            static const char* const kStates[] = {"checking",  "uptodate",  "available",   "manual",
                                                  "installing", "installed", "checkfailed", "installfailed",
                                                  "test",       "testbadge", "onbeta",    "back",
                                                  "backmanual"};
            if (std::find(std::begin(kStates), std::end(kStates), options.fakeUpdate) == std::end(kStates)) {
                std::fprintf(stderr, "--fake-update: unknown state %s\n", options.fakeUpdate.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--update-beta") {
            options.updateBeta = true;
        } else if (arg == "--fake-update-notes" && hasNext) {
            options.fakeUpdateNotes = argv[++i];
            if (options.fakeUpdateNotes != "both" && options.fakeUpdateNotes != "en" && options.fakeUpdateNotes != "long") {
                std::fprintf(stderr, "--fake-update-notes must be both, en or long: %s\n", options.fakeUpdateNotes.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-widen" && hasNext) {
            options.fakeWiden = argv[++i];
            if (std::find(std::begin(kLidWidenModes), std::end(kLidWidenModes), options.fakeWiden) ==
                std::end(kLidWidenModes)) {
                std::fprintf(stderr, "--fake-widen must be off, low, normal or high: %s\n", options.fakeWiden.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-record" && hasNext) {
            options.fakeRecord = argv[++i];
            if (options.fakeRecord != "recording" && options.fakeRecord != "failed" &&
                options.fakeRecord != "autostopped") {
                std::fprintf(stderr, "--fake-record must be recording, failed or autostopped: %s\n",
                             options.fakeRecord.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-eyecam" && hasNext) {
            options.fakeEyecam = argv[++i];
            FakeEyecam fake;
            if (!parseFakeEyecam(options.fakeEyecam, fake)) {
                std::fprintf(stderr,
                             "--fake-eyecam must be waiting, idle, confirm, searching, recording:LABEL, "
                             "calibrating[:LABEL], error or calib-error, with known flags: %s\n",
                             options.fakeEyecam.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-password" && hasNext) {
            options.fakePassword = argv[++i];
            if (options.fakePassword != "set" && options.fakePassword != "unset" && options.fakePassword != "unknown") {
                std::fprintf(stderr, "--fake-password must be set, unset or unknown: %s\n", options.fakePassword.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-contrib" && hasNext) {
            options.fakeContrib = argv[++i];
            if (std::find(std::begin(kFakeContribStates), std::end(kFakeContribStates), options.fakeContrib) ==
                std::end(kFakeContribStates)) {
                std::fprintf(stderr, "--fake-contrib must be one of the states in --help: %s\n",
                             options.fakeContrib.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--sensitivity-drag" && hasNext) {
            options.sensitivityDrag = std::atof(argv[++i]);
        } else if (arg == "--fake-camera" && hasNext) {
            options.fakeCamera = argv[++i];
            static const char* const kCameras[] = {"both", "left", "right", "uncalibrated", "absent", "error", "off"};
            if (std::find(std::begin(kCameras), std::end(kCameras), options.fakeCamera) == std::end(kCameras)) {
                std::fprintf(stderr, "--fake-camera must be both, left, right, uncalibrated, absent, error or off: %s\n",
                             options.fakeCamera.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--eyecam-dir" && hasNext) {
            options.eyecamDir = argv[++i];
        } else if (arg == "--eyecam-fill-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.fillPngPath = argv[++i];
        } else if (arg == "--eyecam-fill" && hasNext) {
            options.fillKind = argv[++i];
            if (options.fillKind != "bright" && options.fillKind != "dark") {
                std::fprintf(stderr, "--eyecam-fill must be bright or dark: %s\n", options.fillKind.c_str());
                return false;
            }
        } else if (arg == "--fake-fit" && hasNext) {
            options.fakeFit = argv[++i];
            static const char* const kFitStates[] = {
                "running",         "running-center",   "running-closed",   "done",
                "done-center",     "done-standin",     "done-center-standin", "fitted",
                "fitted-gaze",     "failed-unsteady",  "failed-notclosed", "failed-movement",
                "failed-lidrange", "failed-cancelled", "failed-left",      "failed-opened",
                "failed-opened-retry", "failed-noresult"};
            if (std::find(std::begin(kFitStates), std::end(kFitStates), options.fakeFit) == std::end(kFitStates)) {
                std::fprintf(stderr, "--fake-fit: unknown state %s\n", options.fakeFit.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--update-live") {
            options.updateLive = true;
        } else if (arg == "--version") {
            options.mode = Options::Mode::Version;
        } else if (arg == "--click" && hasNext) {
            double x = 0;
            double y = 0;
            if (std::sscanf(argv[++i], "%lf,%lf", &x, &y) != 2) {
                std::fprintf(stderr, "--click needs X,Y: %s\n", argv[i]);
                return false;
            }
            options.clicks.emplace_back(x, y);
        } else if (arg == "--contrast-report") {
            options.mode = Options::Mode::ContrastReport;
        } else if (arg == "--probe") {
            options.mode = Options::Mode::Probe;
        } else if (arg == "--probe-switch-away") {
            options.mode = Options::Mode::SwitchAway;
            if (hasNext && argv[i + 1][0] != '-') options.switchAwaySec = std::max(0.5, std::atof(argv[++i]));
        } else if (arg == "--config" && hasNext) {
            options.configPath = argv[++i];
        } else if (arg == "--status" && hasNext) {
            options.statusPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            options.mode = Options::Mode::Help;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    if (options.fake && options.updateLive) {
        std::fprintf(stderr, "--update-live works on the real files, not with --fake\n");
        return false;
    }
    if (options.fake && !options.clicks.empty()) {
        std::fprintf(stderr, "--click works on the real files, not with --fake\n");
        return false;
    }
    if (options.configPath.empty()) options.configPath = defaultConfigPath();
    if (options.statusPath.empty()) options.statusPath = defaultStatusPath();
    if (options.eyecamDir.empty()) options.eyecamDir = eyecam::defaultDir();
    if (options.reportsDir.empty()) options.reportsDir = report::defaultDir();
    return true;
}

/**
 * The panel language from config.json, or the Frame's system language if it has none (or an unknown one).
 * @param config the config
 * @return the language
 */
Language configLanguage(const ConfigFile& config) {
    Language language = systemLanguage();
    const JsonValue* written = config.root.get(key::kLanguage);
    if (written != nullptr && written->isString()) parseLanguage(written->text, language);
    return language;
}

/**
 * The update checker's settings: frame-update.sh as install.sh puts it, this release's tarball name and the
 * install.sh option used when ~/.config/frameeyeosc/install-args is missing (0.3.x did not write it).
 * @param beta "Get test versions" (panel.json): the beta channel; off, the stable one, which also looks at the beta
 *             channel to tell whether a newer test version exists (the badge)
 * @return the settings
 */
frame_updater::UpdaterConfig updaterConfig(bool beta) {
    frame_updater::UpdaterConfig c;
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    const std::string dataHome =
        data != nullptr && data[0] == '/' ? std::string(data) : std::string(home != nullptr ? home : "") + "/.local/share";
    c.script = dataHome + "/frameeyeosc/frame-update.sh";
    c.app = "frameeyeosc";
    c.repo = "sasaken1102r/frameeyeosc";
    c.currentVersion = FRAMEEYEOSC_VERSION;
    c.assetPattern = "frameeyeosc-{version}-steamframe-aarch64.tar.gz";
    // The update button is only in the panel, so whoever uses it has the panel installed
    c.defaultInstallArgs = {"--with-panel"};
    c.channel = beta ? "beta" : "stable";
    c.watchTestVersions = true;
    return c;
}

/**
 * A made-up update state for --fake-update.
 * @param state the state name
 * @param notes --fake-update-notes: "both", "en", "long" or "" (no summary)
 * @return the status
 */
frame_updater::UpdateStatus fakeUpdate(const std::string& state, const std::string& notes) {
    using frame_updater::UpdateState;
    frame_updater::UpdateStatus u;
    u.current = FRAMEEYEOSC_VERSION;
    u.latest = u.current;
    u.url = "https://github.com/sasaken1102r/frameeyeosc/releases/latest";
    u.checkedAt = static_cast<long long>(std::time(nullptr)) - 600;
    if (state == "checking") {
        u.checking = true;
        u.checkedAt = 0;
    } else if (state == "uptodate") {
        u.state = UpdateState::UpToDate;
    } else if (state == "available" || state == "manual") {
        u.state = UpdateState::Available;
        u.latest = "9.9.9";
        u.installable = state == "available";
        if (!u.installable) u.reason = "no-checksums";
        // What frame-update.sh takes from the release text (0.7.1's CHANGELOG section)
        if (notes == "both" || notes == "en") {
            u.notes = "Eye data at the full rate while Steam Link streams, no stray widening on SteamOS 0.4.3, and two "
                      "opt-ins for avatars not made for VRCFaceTracking: Steam Link's parameter names and VRChat's own "
                      "eye tracking.";
        }
        if (notes == "both") {
            u.notesJa = "Steam Link で配信中でも目のデータが全部届くように。SteamOS 0.4.3 で勝手に見開かないように。"
                        "VRCFaceTracking 用じゃないアバター向けに、Steam Link の名前で送る機能と、VRChat 標準の目も動かす"
                        "機能を追加。";
        }
        if (notes == "long") {
            // As long as frame-update.sh lets them be (300 characters with the "…")
            std::string en;
            while (en.size() < 299) en += "A very long summary that goes on and on. ";
            u.notes = en.substr(0, 299) + "…";
            for (int i = 0; i < 299; ++i) u.notesJa += "長";
            u.notesJa += "…";
        }
    } else if (state == "test") {
        u.state = UpdateState::Available;
        u.latest = "9.9.9-beta.1";
        u.prerelease = true;
        u.installable = true;
    } else if (state == "testbadge") {
        u.state = UpdateState::UpToDate;
        u.testVersion = "9.9.9-beta.1";
    } else if (state == "back" || state == "backmanual") {
        // "Get test versions" turned off on a test version: the older regular version is offered going back to
        u.state = UpdateState::Available;
        u.current = "0.7.11-beta.2";
        u.latest = "0.7.10";
        u.installable = state == "back";
        if (!u.installable) u.reason = "no-checksums";
        u.backToStable = true;
        u.notes = "The summary of the older version, not shown.";
    } else if (state == "onbeta") {
        u.state = UpdateState::UpToDate;
        u.current = "0.7.11-beta.2";
        u.latest = u.current;
        u.prerelease = true;
    } else if (state == "installing") {
        u.state = UpdateState::Installing;
        u.step = "download";
        u.version = "9.9.9";
    } else if (state == "installed") {
        u.state = UpdateState::Installed;
        u.version = "9.9.9";
    } else if (state == "checkfailed") {
        u.state = UpdateState::CheckFailed;
        u.error = "network";
    } else if (state == "installfailed") {
        u.state = UpdateState::InstallFailed;
        u.version = "9.9.9";
        u.error = "checksum-mismatch";
    }
    return u;
}

/**
 * A made-up eyecam-rec for --fake-eyecam, written just now (so its tab shows).
 * @param text the option (see parseFakeEyecam; confirm is idle, and the caller opens the light warning)
 * @return the view
 */
eyecam::View fakeEyecam(const std::string& text) {
    FakeEyecam fake;
    parseFakeEyecam(text, fake);
    const std::string& state = fake.state;
    eyecam::View view;
    eyecam::Status& s = view.status;
    s.present = true;
    s.mtime = unixNow();
    s.protocol = "default";
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    s.fpsL = s.fpsR = s.stepRemainingS = s.elapsedS = s.totalS = s.warmupRemainingS = nan;
    // The headset on, the cameras read live and not calibrated yet, unless the flags say otherwise
    s.locked = !fake.unlocked;
    s.live = !fake.noLive;
    s.liveMs = s.live ? 4.2 : nan;
    s.calibState = 0;
    if (state == "waiting") {
        s.stateText = "waiting_fds";
        s.message = fake.autoGrab.empty() ? "eyecam-grab からカメラのバッファを待っています"
                                          : "アイトラッカーが始まるのを待ってるよ";
        s.autoGrab = fake.autoGrab;
        s.locked = false;
        s.live = false;
    } else if (state == "idle" || state == "confirm") {
        s.stateText = "idle";
    } else if (state == "searching") {
        s.stateText = "searching";
        s.fpsL = 29.8;
        s.fpsR = 30.1;
    } else if (state == "error") {
        s.stateText = "error";
        s.message = "右のカメラの映像が 3 秒届きません";
        view.lastRun = eyecam::Run::Recording;
        // A failed command too, to see where it goes
        view.hasReply = true;
        view.reply.command = "start";
        view.reply.error = "not idle";
    } else if (state == "calib-error") {
        // eyecam-rec's reason is Japanese only
        const bool wear = fake.calibUser == 0;
        s.stateText = "error";
        s.message = wear ? "右目: 目を閉じても目の開きが変わっていない（もう一度、しっかり閉じてね）"
                         : "左目: 下を見ても目の開きが変わっていない（もう一度、しっかり下を見てね）";
        s.calibState = wear ? 0 : eyecam::kCalibWearBit;
        view.lastRun = wear ? eyecam::Run::CalibWear : eyecam::Run::CalibUser;
    } else if (state == "calibrating") {
        // At that step of the calibration: this wear's (18 s) or the user's (18 s)
        static const struct {
            const char* label;
            double seconds;
        } kWear[] = {{"lead_in", 3}, {"close", 2}, {"normal", 5}, {"widen", 3}, {"normal", 2}, {"widen", 3}},
          kUser[] = {{"lead_in", 3}, {"squint", 5}, {"look_up", 5}, {"look_down", 5}};
        const eyecam::Step step = eyecam::parseStep(fake.label);
        const bool user = fake.calibUser == 1 ||
                          (fake.calibUser < 0 && (step == eyecam::Step::Squint || step == eyecam::Step::LookUp ||
                                                  step == eyecam::Step::LookDown));
        s.stateText = "calibrating";
        s.protocol = user ? eyecam::kCalibUserCommand : eyecam::kCalibWearCommand;
        s.calibState = user ? eyecam::kCalibWearBit : 0;
        view.lastRun = user ? eyecam::Run::CalibUser : eyecam::Run::CalibWear;
        s.stepLabel = fake.label;
        if (!fake.label.empty()) {
            const auto* steps = user ? kUser : kWear;
            const int count = user ? static_cast<int>(std::size(kUser)) : static_cast<int>(std::size(kWear));
            s.stepCount = count;
            s.stepIndex = 0;
            double before = 0.0;
            s.totalS = 0.0;
            for (int i = 0; i < count; ++i) s.totalS += steps[i].seconds;
            while (s.stepIndex < count - 1 && steps[s.stepIndex].label != fake.label) before += steps[s.stepIndex++].seconds;
            s.stepRemainingS = 1.4;
            s.elapsedS = before + steps[s.stepIndex].seconds - s.stepRemainingS;
            s.fpsL = 30.0;
            s.fpsR = 29.9;
        }
    } else {
        // recording:<label>, at that label's first step of eyecam's default protocol (protocol_widen.txt: 17 steps,
        // 83 s; without the light protocol_widen_nolight.txt, its first 14), with its steps as the panel reads them
        // from that file (the chips); "end" after the last
        static const struct {
            const char* label;
            double seconds;
        } kSteps[] = {
            {"lead_in", 3}, {"close", 2},     {"normal", 5}, {"widen", 5},  {"close", 2},  {"normal", 5},
            {"widen", 5},   {"close", 2},     {"normal", 5}, {"widen", 5},  {"squint", 5}, {"look_up", 5},
            {"look_down", 5}, {"normal", 5}, {"bright", 8}, {"dark", 8},   {"bright", 8},
        };
        const int count = fake.noLight ? 14 : static_cast<int>(std::size(kSteps));
        s.stateText = "recording";
        s.stepLabel = fake.label;
        s.protocol = fake.noLight ? eyecam::kNoLightProtocol : "widen";
        view.protocol.name = s.protocol;
        s.stepCount = count;
        s.stepIndex = 0;
        double before = 0.0;
        while (s.stepIndex < count && kSteps[s.stepIndex].label != fake.label) before += kSteps[s.stepIndex++].seconds;
        double total = 0.0;
        for (int i = 0; i < count; ++i) {
            view.protocol.steps.push_back(kSteps[i].label);
            total += kSteps[i].seconds;
        }
        s.stepRemainingS = s.stepIndex < count ? 3.4 : 0.0;
        // (eyecam-rec records 1.5 s past the last step)
        s.totalS = total + 1.5;
        s.elapsedS = s.stepIndex < count ? before + kSteps[s.stepIndex].seconds - s.stepRemainingS : total;
        s.fpsL = 30.0;
        s.fpsR = 29.9;
        s.sessionDir = "/home/steamos/eyecam/2026-10-03_12-00-00";
        view.lastRun = eyecam::Run::Recording;
    }
    if (fake.calibState >= 0) s.calibState = fake.calibState;
    s.recalibSuggested = fake.recalib;
    // An eyecam-rec that learns the relaxed eyes by itself writes the baseline and calib_saved
    if (!fake.baseline.empty()) {
        s.hasBaseline = s.hasCalibSaved = true;
        s.baseline = fake.baseline;
        if (fake.warmupS >= 0) s.warmupRemainingS = fake.warmupS;
    }
    if (fake.saved) s.hasCalibSaved = true;
    s.calibSaved = fake.saved;
    if (fake.sens >= 0) {
        s.hasWidenSensitivity = true;
        s.widenSensitivity = fake.sens;
    }
    if (fake.veto >= 0) {
        s.hasLidVeto = true;
        s.lidVeto = fake.veto == 1;
    }
    if (!fake.autoGrab.empty()) s.autoGrab = fake.autoGrab;
    s.grabOutdated = fake.outdated;
    // Why the video isn't found (and the proximity reading that goes with it)
    s.hasSearch = !fake.search.empty();
    s.search = fake.search;
    s.prox = fake.search == "not_worn" ? 12.0 : (fake.search.empty() ? nan : 31.0);
    if (fake.message > 0) {
        s.message = "校正できた（かぶり）";
        s.messageEn = fake.message == 1 ? "Calibrated (this wear)" : "";
    }
    // The cameras' frame rate (while locked), and the calibration counts at it: eyecam-rec's 5.4 s of normal steps
    // (486 frames at 90 fps), what it needs of them (about a second's worth: 90 at 90 fps, 15 at 15), and its note when
    // the rate is low. At 90 the made-up messages are the ones they always were.
    const double camFps = fake.camFps > 0 ? fake.camFps : 90.0;
    s.hasCamFps = fake.camFps > 0;
    s.camFps[0] = s.camFps[1] = s.hasCamFps && s.locked && s.live ? fake.camFps : nan;
    const int normalFrames = static_cast<int>(std::lround(5.4 * camFps));
    const auto seenOf = [&](double at90) { return std::max(1L, std::lround(at90 * camFps / 90.0)); };
    const int needed = std::max({12, static_cast<int>(std::lround(camFps)), static_cast<int>(std::ceil(0.18 * normalFrames))});
    const auto counts = [&](double at90) { return std::to_string(seenOf(at90)) + "/" + std::to_string(normalFrames); };
    const std::string needJa = "、" + std::to_string(needed) + " 必要]";
    const std::string needEn = ", " + std::to_string(needed) + " needed]";
    const std::string slowJa = camFps < 60 ? "。カメラの映像が毎秒 " + std::to_string(std::lround(camFps)) + " 枚しか届いていない" : "";
    const std::string slowEn =
        camFps < 60 ? ". The eye cameras deliver only " + std::to_string(std::lround(camFps)) + " frames a second" : "";
    const std::string bothJa = "両目の瞳がうまく見えなかった（HMD のかぶり方を直して、もう一度）[左 " + counts(12) + "・右 " +
                               counts(30) + needJa + slowJa;
    const std::string bothEn = "Both eyes: couldn't see the pupil well (adjust the headset and try again) [L " + counts(12) +
                               ", R " + counts(30) + needEn + slowEn;
    // The pupils, and an eye a "calib wear" failed on (with eyecam-rec's messages for it)
    s.hasPupil = fake.hasPupil;
    s.pupil[0] = fake.hasPupil ? fake.pupil[0] : nan;
    s.pupil[1] = fake.hasPupil ? fake.pupil[1] : nan;
    if (fake.failed == "LR" || fake.failed == "mixed") {
        s.calibFailedEye = "LR";
        if (fake.failed == "LR") {
            s.message = bothJa;
            s.messageEn = bothEn;
        } else if (fake.calibUser == 1) {
            // A user calibration's: a different reason per eye, one of eyecam-rec's longest messages
            s.message = "左目: 下を見ても目の開きが変わっていない（もう一度、しっかり下を見てね）。"
                        "右目のまぶたの線が見つからなかった（HMD のかぶり方を直して、もう一度）[100/600、150 必要]";
            s.messageEn = "Left eye: looking down didn't change the eye opening (try again and look down clearly). "
                          "Right eye: couldn't find the eyelid lines (adjust the headset and try again) "
                          "[100/600, 150 needed]";
        } else {
            s.message = "左目の瞳がうまく見えなかった（HMD のかぶり方を直して、もう一度）[" + counts(12) + needJa +
                        "。右目の上まぶたの線が見つからなかった（HMD のかぶり方を直して、もう一度）[" + counts(40) + needJa +
                        slowJa;
            s.messageEn = "Left eye: couldn't see the pupil well (adjust the headset and try again) [" + counts(12) +
                          needEn + ". Right eye: couldn't find the upper eyelid line (adjust the headset and try again) [" +
                          counts(40) + needEn + slowEn;
        }
        if (state == "calib-error") {
            const bool user = fake.calibUser == 1 && fake.failed == "mixed";
            s.calibState = fake.calibState >= 0 ? fake.calibState : (user ? eyecam::kCalibWearBit : 0);
            view.lastRun = user ? eyecam::Run::CalibUser : eyecam::Run::CalibWear;
        }
    } else if (!fake.failed.empty()) {
        // It went through: the other eye's new values, this one's earlier (or provisional) ones; widening on the
        // standard values too with fail or page=default
        s.calibFailedEye = fake.failed;
        const bool left = fake.failed == "L";
        const bool widenDefault = fake.result == "fail" || fake.page == "default" || fake.widen == "default";
        s.message = std::string("校正できた（") + (left ? "左目" : "右目") + "は瞳がうまく見えなかったので、" +
                    (fake.provisional ? "仮の値" : "前の値") + "を使うよ" +
                    (widenDefault ? "。見開きは取れなかったので、いつもの幅を使うよ" : "") + "）[" + counts(12) + needJa;
        s.messageEn = std::string("Calibrated (") + (left ? "Left eye" : "Right eye") +
                      ": couldn't see the pupil well, using " +
                      (fake.provisional ? "provisional values" : "its previous values") +
                      (widenDefault ? ". Couldn't measure widening, using the usual width" : "") + ") [" + counts(12) +
                      needEn;
    }
    if (!fake.lookDown.empty() || !fake.lookPrevious.empty()) {
        // A user calibration that went through with the standard look-down correction or the previous one kept
        // (eyecam-rec's message: one note for the eyes, or one for each when they differ)
        const auto bits = [](const std::string& eyes) {
            return eyes == "LR"  ? (eyecam::kLeftEyeBit | eyecam::kRightEyeBit)
                   : eyes == "L" ? eyecam::kLeftEyeBit
                   : eyes == "R" ? eyecam::kRightEyeBit
                                 : 0;
        };
        const int standard = bits(fake.lookDown);
        const int previous = bits(fake.lookPrevious) & ~standard;
        const auto noteJa = [&](int bit) {
            return std::string(previous & bit ? "下を見たときの補正を前の値のままにしたよ"
                                              : "下を見たときの補正を標準の値にしたよ");
        };
        const auto noteEn = [&](int bit) {
            return std::string(previous & bit ? "keeping the previous correction for looking down"
                                              : "using the standard correction for looking down");
        };
        const int eyes = standard | previous;
        const int left = eyecam::kLeftEyeBit;
        const int right = eyecam::kRightEyeBit;
        if (eyes == (left | right) && noteJa(left) != noteJa(right)) {
            s.message = "校正できた（左目は" + noteJa(left) + "。右目は" + noteJa(right) + "）";
            s.messageEn = "Calibrated (Left eye: " + noteEn(left) + ". Right eye: " + noteEn(right) + ")";
        } else {
            const bool both = eyes == (left | right);
            const int bit = eyes == right ? right : left;
            s.message = std::string("校正できた（") + (both ? "両目とも" : bit == left ? "左目は" : "右目は") +
                        noteJa(bit) + "）";
            s.messageEn = std::string("Calibrated (") + (both ? "Both eyes" : bit == left ? "Left eye" : "Right eye") +
                          ": " + noteEn(bit) + ")";
        }
        s.hasUserLookDownDefault = true;
        s.userLookDownDefault = standard;
        s.hasUserLookDownPrevious = true;
        s.userLookDownPrevious = previous;
    }
    if (fake.message == 2) s.messageEn.clear();
    // The diagnostics: the proximity threshold, the last look (as eyecam-rec's own --fake makes it), the last wear
    // calibration and the last error
    s.proxMin = 20.0;
    if (fake.prox >= 0) {
        s.prox = fake.prox;
    } else if (state != "waiting" && !std::isfinite(s.prox)) {
        s.prox = 31.0;  // (worn)
    }
    if (state != "waiting") {
        eyecam::SearchDetail& d = s.searchDetail;
        if (fake.search == "one_eye") {
            d = {true, true, 4, 90.0, 4, false, "one_eye", 64};
        } else if (fake.search == "not_worn" || fake.search == "no_video") {
            d = {true, true, 0, 0.0, 0, false, "no_candidates", fake.search == "no_video" ? 3 : 0};
        } else {
            d = {true, true, 8, 90.0, 8, true, "", 128};
        }
        if (fake.blocks >= 0) d.changedBlocks = fake.blocks;
    }
    {
        eyecam::LastCalib& c = s.lastCalib;
        const bool failedWear = state == "calib-error" && fake.calibUser != 1;
        c.known = true;
        c.present = fake.setup == 1 || !fake.failed.empty() || failedWear;
        c.time = "2026-10-05 19:51:03";
        c.ok = fake.failed != "LR" && fake.failed != "mixed" && !failedWear;
        c.failedEye = c.ok ? (fake.failed == "L" || fake.failed == "R" ? fake.failed : "") : "LR";
        c.message = c.ok ? (c.failedEye.empty() ? std::string("校正できた（かぶり）") : s.message) : s.message;
        c.messageEn = c.ok ? (c.failedEye.empty() ? std::string("Calibrated") : s.messageEn) : s.messageEn;
        if (failedWear && fake.failed.empty()) {
            c.message = bothJa;
            c.messageEn = bothEn;
        }
        for (int eye = 0; eye < 2; ++eye) {
            const bool lost = !c.ok || c.failedEye == (eye == 0 ? "L" : "R");
            c.pupilFrames[eye] = static_cast<double>(seenOf(lost ? (eye == 0 ? 12 : 30) : (eye == 0 ? 470 : 482)));
            c.normalFrames[eye] = normalFrames;
            c.pupilX[eye] = lost ? nan : (eye == 0 ? 238 : 252);
            c.pupilY[eye] = lost ? nan : (eye == 0 ? 201 : 194);
            c.window[eye][0] = eye == 0 ? 186 : 180;
            c.window[eye][1] = eye == 0 ? 346 : 340;
        }
    }
    if (fake.lastError || state == "calib-error" || state == "error") {
        s.lastError = state == "error" || state == "calib-error" ? s.message : "右のカメラの映像が 3 秒届きません";
        s.lastErrorEn = state == "error" || state == "calib-error" ? s.messageEn : "";
        s.lastErrorUnix = unixNow() - 180;
    }
    // A failed calibration keeps the buffers (eyecam-rec writes has_buffers in every state)
    s.hasBuffers = fake.buffers || state == "calib-error";
    // An eyecam-rec that makes the eye preview's pictures (not making them now)
    s.hasPreview = !fake.noPreview;
    s.preview = false;
    // ...and that keeps a drift log alongside the eye log (off)
    s.hasDriftlog = true;
    s.hasSetupDone = fake.setup >= 0;
    s.setupDone = fake.setup == 1;
    s.lastCalibWiden = fake.result == "fail" ? "default" : (fake.result == "done" ? "measured" : fake.widen);
    s.state = eyecam::parseState(s.stateText);
    view.visible = eyecam::tabVisible(s, unixNow());
    // The setup's calibration that just ended: followed as the loop would, from calibrating to this status
    if (!fake.result.empty()) {
        eyecam::Status calibrating = s;
        calibrating.state = eyecam::State::Calibrating;
        calibrating.hasSetupDone = true;
        calibrating.setupDone = false;
        view.flow.follow(calibrating, nowSeconds());
        // eyecam-rec says setup_done after either (the standard widening is saved too)
        s.hasSetupDone = s.setupDone = true;
        view.flow.follow(s, nowSeconds());
        view.readyNotice = view.flow.readyNotice(nowSeconds());
        view.lastRun = eyecam::Run::CalibWear;
    }
    // One from the usual page that just ended: followed from its last step to this status
    if (!fake.page.empty()) {
        const bool user = fake.page == "user";
        eyecam::Status calibrating = s;
        calibrating.state = eyecam::State::Calibrating;
        calibrating.protocol = user ? eyecam::kCalibUserCommand : eyecam::kCalibWearCommand;
        calibrating.stepCount = user ? 4 : 6;
        calibrating.stepIndex = calibrating.stepCount - 1;
        view.flow.follow(s, nowSeconds());
        view.flow.follow(calibrating, nowSeconds());
        if (!user) s.lastCalibWiden = fake.page;
        view.flow.follow(s, nowSeconds());
        view.lastRun = user ? eyecam::Run::CalibUser : eyecam::Run::CalibWear;
    }
    return view;
}

/**
 * --fake-contrib: "Record for squint training" on one of its screens (kFakeContribStates), made up: nothing is read or
 * written. The eye cameras set up and idle, unless --fake-eyecam gave eyecam-rec already; recording for "running".
 * @param m the model
 * @param state the screen
 * @param eyecamGiven --fake-eyecam was given
 */
void fakeContrib(PanelModel& m, const std::string& state, bool eyecamGiven) {
    contrib::View& c = m.contrib;
    c.loaded = true;
    c.home = "/home/steamos/eyecam";
    c.dir = c.home + "/" + contrib::kFolderName;
    c.freeBytes = state == "nospace" ? 3'100'000'000LL : 23'200'000'000LL;
    c.fps = 90.0;
    if (!eyecamGiven) m.eyecam = fakeEyecam("idle:setup:auto=ok:buffers:ready:saved");
    if (state == "none" || state == "consent" || state == "deleted" || state == "eyewear") {
        c.deleted = state == "deleted";
        return;
    }
    c.exists = true;
    c.info.consentVersion = contrib::kConsentVersion;
    c.info.consent = "2026-10-25 14:02:10 +0900";
    c.info.eyewear = contrib::Eyewear::None;
    // Three recordings so far (and a fourth just now on the end's screen)
    const char* const recs[] = {"rec_2026-10-23_21-14-05", "rec_2026-10-24_22-30-41", "rec_2026-10-25_14-03-12",
                                "rec_2026-10-25_14-20-55"};
    const bool justDone = state == "done" || state == "packing" || state == "packed" || state == "delete";
    for (int i = 0; i < (justDone ? 4 : 3); ++i) {
        contrib::Recording r;
        r.rec = recs[i];
        r.protocol = contrib::kProtocol;
        r.bytes = contrib::estimateBytes(90.0) - 4'000'000;
        c.info.recordings.push_back(r);
    }
    c.totalBytes = contrib::recordedBytes(c.info);
    c.justDone = justDone;
    if (state == "running") {
        m.eyecam = fakeEyecam("recording:squint");
        // (protocol_contrib.txt's steps: the second squint, 35 s in)
        eyecam::Status& s = m.eyecam.status;
        s.protocol = contrib::kProtocol;
        m.eyecam.protocol.name = s.protocol;
        m.eyecam.protocol.steps = {"lead_in", "normal", "close",  "normal", "squint",  "normal",    "widen",
                                   "close",   "normal", "squint", "normal", "widen",   "close",     "normal",
                                   "squint",  "normal", "widen",  "look_up", "look_down", "look_left", "normal",
                                   "look_right", "normal"};
        s.stepCount = static_cast<int>(m.eyecam.protocol.steps.size());
        s.stepIndex = 9;
        s.stepLabel = "squint";
        s.totalS = contrib::kProtocolSec + contrib::kTailSec;
        s.elapsedS = 36.6;
        s.stepRemainingS = 2.4;
        c.running = true;
    }
    if (state == "failed") {
        c.failure = {true, contrib::Why::Stopped, "", "", "aborted/rec_2026-10-25_14-20-55"};
    }
    if (state == "incomplete") {
        c.failure = {true, contrib::Why::Incomplete, "cues.csv", "cues.csv", "aborted/rec_2026-10-25_14-20-55"};
    }
    if (state == "truncated") {
        c.failure = {true, contrib::Why::Truncated, "eye_R.raw", "eye_R.raw", "aborted/rec_2026-10-25_14-20-55"};
    }
    if (state == "valve") {
        c.failure = {true, contrib::Why::Valve, "42%", "42%", "aborted/rec_2026-10-25_14-20-55"};
    }
    if (state == "packing") {
        c.progress.job = contrib::Job::Pack;
        c.progress.running = true;
        c.progress.total = c.totalBytes;
        c.progress.done = c.totalBytes * 45 / 100;
    }
    if (state == "packed") {
        c.info.packed = "contrib_2026-10-25_14-25-02.tar.zst";
        c.info.packedCount = 4;
        c.info.packedBytes = c.totalBytes * 58 / 100;
        c.totalBytes += c.info.packedBytes;
    }
    if (state == "list") c.incomplete = {"rec_2026-10-24_22-30-41"};
    if (state == "deleting") {
        c.progress.job = contrib::Job::Delete;
        c.progress.running = true;
    }
}

/**
 * Copy where a command to eyecam-rec is (waiting, or its reply) into the view the panel draws.
 * @param view the view
 * @param control the control socket
 */
void syncEyecamControl(eyecam::View& view, const eyecam::Control& control) {
    view.busy = control.busy();
    view.busyCommand = control.busy() ? control.command() : std::string();
    view.hasReply = control.hasReply();
    if (control.hasReply()) view.reply = control.reply();
}

/**
 * What the eye log's drift link looks at now.
 * @param recording the eye log
 * @param model the panel's model (eyecam-rec as last read, and the settings)
 * @return the inputs
 */
recorder::DriftInputs driftInputs(const recorder::View& recording, const PanelModel& model) {
    recorder::DriftInputs in;
    in.recording = recording.recording;
    in.stem = recorder::driftStem(recording.path);
    in.eyecamRunning = model.eyecam.visible;
    in.camerasInUse = eyecam::inUse(model.eyecam, SettingsView(model).flag(key::kCameraLids));
    in.hasDriftlog = model.eyecam.status.hasDriftlog;
    in.driftlogOn = model.eyecam.status.driftlogOn;
    in.driftlogPath = model.eyecam.status.driftlogPath;
    in.now = nowSeconds();
    return in;
}

/**
 * eyecam-rec's answer to one of the drift link's commands: in the journal, and for "driftlog on" in the recording's
 * .log too (its file, so the two can be paired, or why there is none), if it was for that recording.
 * @param reply the answer
 * @param eyeLog the eye log
 */
void driftReplied(const eyecam::Reply& reply, recorder::Recorder& eyeLog) {
    std::fprintf(stderr, "[record] %s -> %s%s\n", reply.command.c_str(), reply.ok ? "ok " : "err ",
                 reply.ok ? reply.detail.c_str() : reply.error.c_str());
    if (reply.command != "driftlog on " + recorder::driftStem(eyeLog.view(nowSeconds()).path)) return;
    eyeLog.note(reply.ok ? "eyecam drift log: " + reply.detail : "eyecam drift log not started: " + reply.error);
}

/**
 * Send one of the drift link's commands to eyecam-rec's control socket (its reply comes to driftReplied; at once if it
 * can't be reached).
 * @param control the drift log's own connection
 * @param eyecamDir eyecam-rec's run folder
 * @param command the command
 * @param eyeLog the eye log
 */
void sendDrift(eyecam::Control& control, const std::string& eyecamDir, const std::string& command,
               recorder::Recorder& eyeLog) {
    const std::string socket = eyecamDir + "/ctl.sock";
    std::fprintf(stderr, "[record] sending \"%s\" to %s\n", command.c_str(), socket.c_str());
    if (!control.send(socket, command, nowSeconds()) && !control.busy()) driftReplied(control.reply(), eyeLog);
}

/**
 * A setting's slider let go of: its value as a SetNumber press.
 * @param panel the panel
 * @param apply applies the press
 */
template <typename Apply>
void takeSliders(EyePanel& panel, Apply apply) {
    std::string name;
    double value = 0.0;
    if (!panel.takeNumberSlider(name, value)) return;
    const SettingSpec* spec = findSetting(name);
    if (spec == nullptr) return;
    std::fprintf(stderr, "[action] %s slid to %.3f\n", name.c_str(), value);
    apply(PanelHit {PanelAction::SetNumber, spec->key, static_cast<int>(std::lround(value * 1000))});
}

/**
 * A made-up model for --dump-png (--fake*), so every state can be checked without frameeyeosc.
 * @param options the command line
 * @return the model
 */
PanelModel fakeModel(const Options& options) {
    PanelModel m;
    m.configPath = options.configPath;
    m.statusPath = options.statusPath;
    m.config.exists = true;
    m.config.root.type = JsonValue::Type::Object;
    JsonValue& root = m.config.root;
    if (options.fakeEtvr) root.set(key::kOutput, JsonValue::makeString(kOutputEtvr));
    if (options.fakeLivelink) root.set(key::kOutput, JsonValue::makeString(kOutputLivelink));
    if (options.fakeModule) root.set(key::kOutput, JsonValue::makeString(kOutputModule));
    if (options.fakePupilsOff) root.set(key::kPupilsToVrchat, JsonValue::makeBool(false));
    if (options.fakePupilBits >= 0) root.set(key::kPupilBits, JsonValue::makeNumber(options.fakePupilBits, true));
    if (options.fakeFixed) root.set(key::kHost, JsonValue::makeString("192.168.0.60"));
    if (!options.fakeWiden.empty()) root.set(key::kLidWiden, JsonValue::makeString(options.fakeWiden));
    if (options.fakeNoWiden) root.set(key::kWiden, JsonValue::makeBool(false));
    if (options.fakeBrows) root.set(key::kBrowFromWiden, JsonValue::makeBool(true));
    if (options.fakePaused) root.set(key::kSending, JsonValue::makeBool(false));
    if (options.fakeCustom) {
        root.set(key::kGazeMinCutoff, JsonValue::makeNumber(0.3));
        root.set(key::kGazeBeta, JsonValue::makeNumber(0.9));
        root.set(key::kGazeDeadzone, JsonValue::makeNumber(0.045));
        root.set(key::kLidScaleLeft, JsonValue::makeNumber(1.03));
        root.set(key::kLidScaleRight, JsonValue::makeNumber(0.97));
        root.set(key::kGazeHoldBelow, JsonValue::makeNumber(0.0));
        root.set(key::kPort, JsonValue::makeNumber(9001, true));
        root.set(key::kPrefix, JsonValue::makeString(""));
        root.set(key::kSteamlinkParams, JsonValue::makeBool(true));
        root.set(key::kNativeEyes, JsonValue::makeBool(true));
    }
    if (options.fakeBroken) m.config.error = "expected , or } between members (near character 212)";
    if (options.fakeIndependent) root.set(key::kIndependentEyes, JsonValue::makeBool(true));
    if (options.fakeV1) root.set(key::kEyeBehavior, JsonValue::makeNumber(1, true));
    if (options.fakeGazeDots) root.set(key::kGazeDebugDots, JsonValue::makeBool(true));
    if (options.fakeRingsOff) root.set(key::kGazeDebugRings, JsonValue::makeBool(false));
    for (size_t i = 0; i < options.fakeGazeScale.size() && i < 4; ++i) {
        root.set(kGazeScaleKeys[i / 2][i % 2], JsonValue::makeNumber(options.fakeGazeScale[i]));
    }
    if (options.fakeRecord == "recording") {
        m.recording.recording = true;
        m.recording.elapsedSec = 83.4;
    } else if (options.fakeRecord == "failed") {
        m.recording.error = "can't open /dev/shm/eye-server.mmap: No such file or directory (os error 2)";
    } else if (options.fakeRecord == "autostopped") {
        m.recording.autoStopped = true;
    }
    if (!options.fakeFit.empty()) {
        using gaze_fit::Failure;
        using gaze_fit::Phase;
        using gaze_fit::Point;
        gaze_fit::View& fit = m.fit;
        const std::string& state = options.fakeFit;
        fit.mode = state.find("center") != std::string::npos ? gaze_fit::Mode::Center : gaze_fit::Mode::Full;
        fit.count = gaze_fit::pointCount(fit.mode);
        // The fit in config.json: the zero point of the live run on 2026-09-28, the mean up gain of five wears on
        // 2026-10-09, and eyelids like the worn recording
        const bool saved = state.rfind("done", 0) == 0 || state.rfind("fitted", 0) == 0;
        if (saved) {
            root.set(key::kGazeOffsetX, JsonValue::makeNumber(0.012));
            root.set(key::kGazeOffsetY, JsonValue::makeNumber(-0.02));
            root.set(key::kGazeGainUp, JsonValue::makeNumber(0.88));
        }
        if (saved && state != "fitted-gaze") {
            const double readings[2][4] = {{0.15, 0.93, 0.92, 0.77}, {0.26, 0.86, 0.81, 0.75}};
            for (int eye = 0; eye < 2; ++eye) {
                for (int i = 0; i < 4; ++i) root.set(kLidFitKeys[eye][i], JsonValue::makeNumber(readings[eye][i]));
            }
        }
        if (state == "running") {
            fit.phase = Phase::Capturing;
            fit.index = 2;
            fit.point = Point::Down;
            fit.attempt = 2;
        } else if (state == "running-center") {
            fit.phase = Phase::Capturing;
        } else if (state == "running-closed") {
            fit.phase = Phase::Settling;
            fit.index = 5;
            fit.point = Point::Closed;
        } else if (state.rfind("done", 0) == 0) {
            fit.phase = Phase::Done;
            // The right eye stuck shut on Valve's side, seen open by the eye camera (2026-10-11)
            fit.notes.standIn[1] = state.find("standin") != std::string::npos;
        } else if (saved) {
            fit.phase = Phase::Idle;
        } else {
            fit.phase = Phase::Failed;
            fit.failure = state == "failed-unsteady"    ? Failure::Unsteady
                          : state == "failed-notclosed" ? Failure::NotClosed
                          : state == "failed-movement"  ? Failure::NoMovement
                          : state == "failed-lidrange"  ? Failure::NoLidRange
                          : state == "failed-cancelled" ? Failure::Cancelled
                          : state == "failed-left"      ? Failure::Left
                          : state.rfind("failed-opened", 0) == 0 ? Failure::DashboardOpened
                                                        : Failure::NoResult;
            m.fitRetry = state == "failed-opened-retry";
            fit.point = state == "failed-movement" ? Point::Down : Point::Left;
            // Made-up numbers behind it
            gaze_fit::FailureDetail& d = fit.detail;
            if (fit.failure == Failure::Unsteady) {
                fit.point = Point::Center;
                d.tries = 3;
                d.last.samples = 10;
                d.last.received = 26;
                d.last.rateHz = 15.2;
                d.last.spread = 3.4 / gaze_fit::kFullScaleDeg;
            } else if (fit.failure == Failure::NotClosed) {
                fit.point = Point::Closed;
                d.tries = 3;
                d.last.samples = 200;
                d.last.openness[0] = 0.62;
                d.last.openness[1] = 0.40;
                d.closedBelow[0] = 0.56;
                d.closedBelow[1] = 0.53;
            } else if (fit.failure == Failure::NoMovement) {
                d.movedDeg = 2.1;
                d.neededDeg = 3.75;
            } else if (fit.failure == Failure::NoLidRange) {
                d.eye = 1;
                d.lidPoint = Point::Down;
                d.lidOpen = 0.30;
                d.lidClosed = 0.25;
            }
        }
    }

    EyeStatus& s = m.status;
    if (!options.fakeNotRunning) {
        const double now = unixNow();
        const bool etvr = options.fakeEtvr;
        s.present = true;
        s.running = true;
        s.pid = 12345;
        s.time = now;
        s.started = now - 4980;
        s.sending = !options.fakePaused;
        s.output = etvr                   ? kOutputEtvr
                   : options.fakeLivelink ? kOutputLivelink
                   : options.fakeModule   ? kOutputModule
                                          : kOutputVrchat;
        s.targetMode = options.fakeFixed ? "fixed" : "auto";
        if (!options.fakeTargetNull) {
            s.target = std::string("192.168.0.60:") +
                       (etvr                   ? "8889"
                        : options.fakeLivelink ? "11111"
                        : options.fakeModule   ? "11112"
                                               : (options.fakeCustom ? "9001" : "9000"));
        }
        s.trackerRate = options.fakeSlowTracker ? 15.0 : 89.6;
        s.missedRate = 0;
        s.maxProcessingMs = 1.4;
        s.droppedRate = 0;
        s.rate = options.fakePaused ? 0.0 : s.trackerRate;
        s.tracking = !options.fakeNoTracking && !options.fakeSourceError;
        if (s.tracking) {
            s.hasRaw = true;
            s.openness = {{0.81, 0.79}};
            s.opennessScaled = {{0.84, 0.77}};
            s.gaze = {{0.22, -0.14}};
            s.hasSent = true;
            s.lidsVrcft = {{0.75, 0.72}};
            s.lids = etvr ? Pair {{1.0, 0.96}} : s.lidsVrcft;
            s.sentGaze = {{0.17, -0.10}};
            // Each eye turned in a little (left eye right of the combined gaze, right eye left of it)
            s.rawGazeEye[0] = {{0.27, -0.14}};
            s.rawGazeEye[1] = {{0.17, -0.14}};
            s.sentGazeEye[0] = options.fakeIndependent ? Pair {{0.21, -0.10}} : s.sentGaze;
            s.sentGazeEye[1] = options.fakeIndependent ? Pair {{0.13, -0.10}} : s.sentGaze;
        }
        s.calibrationEnabled = true;
        s.relaxed = {{0.78, 0.82}};
        // What frameeyeosc applies: a fixed scale, else 1 for a fitted eye, else the learned one
        {
            const double learned[2] = {1.026, 0.976};
            const FitInConfig fitted = fitInConfig(SettingsView(m));
            const char* fixedKeys[2] = {key::kLidScaleLeft, key::kLidScaleRight};
            for (int eye = 0; eye < 2; ++eye) {
                const double fixed = m.config.number(fixedKeys[eye]);
                s.scales.v[eye] = std::isfinite(fixed) ? fixed : (fitted.lidsFitted[eye] ? 1.0 : learned[eye]);
            }
        }
        s.learning = true;
        s.configPath = options.configPath;
        s.calibrationPath = "/home/steamos/.config/frameeyeosc/calibration";
        if (options.fakeConfigError) s.configError = "lid_closed must be below lid_open";
        if (options.fakeSourceError) {
            s.sourceError = "unsupported eye shared-memory version 6; supported: 4, 5";
        }
        if (options.fakeCoreError) {
            s.lastError = "Can't send OSC to 192.168.0.60:9000 yet (Network is unreachable (os error 101)); retrying "
                          "every 5 s";
            s.lastErrorTime = now - 120;
        }
        s.dominantEye = options.fakeDominantEye;
        // LiveLink: the pupils go straight to VRChat on the same PC (while the setting is on)
        if (options.fakeLivelink && m.config.flag(key::kPupilsToVrchat)) s.pupilTarget = "192.168.0.60:9000";
        s.opennessSaturated = options.fakeOpennessSaturated;
        if (!options.fakeCamera.empty()) {
            const std::string& camera = options.fakeCamera;
            CameraStatus& c = s.camera;
            c.known = true;
            c.present = camera != "absent";
            c.calibState = camera == "uncalibrated" ? 0 : (camera == "error" ? 1 : 3);
            c.used[0] = camera == "both" || camera == "left";
            c.used[1] = camera == "both" || camera == "right";
            c.pupilUsed[0] = c.pupilUsed[1] = camera == "both";
            if (camera == "error") c.error = "camera frames are stale";
            if (camera == "off") root.set(key::kCameraLids, JsonValue::makeBool(false));
            if (c.used[0] || c.used[1]) {
                s.squint = {{c.used[0] ? 0.12 : NAN, c.used[1] ? 0.08 : NAN}};
                if (camera == "both") s.pupilDilation = 0.46;
            }
        }
        s.effective = root;
        if (options.fakeLocked) {
            s.locked = {key::kOutput,          key::kPort,        key::kRaw,         key::kLidOpen,
                        key::kIndependentEyes, key::kGazeOffsetY, key::kSteamlinkParams, key::kNativeEyes,
                        key::kCameraLids, key::kPupilsToVrchat, key::kPupilBits};
            s.effective.set(key::kOutput, JsonValue::makeString(options.fakeLivelink ? kOutputLivelink : kOutputVrchat));
            s.effective.set(key::kPort, JsonValue::makeNumber(9123, true));
            s.effective.set(key::kRaw, JsonValue::makeBool(true));
            s.effective.set(key::kLidOpen, JsonValue::makeNumber(0.78));
            s.effective.set(key::kIndependentEyes, JsonValue::makeBool(true));
            s.effective.set(key::kGazeOffsetY, JsonValue::makeNumber(-0.05));
            s.effective.set(key::kSteamlinkParams, JsonValue::makeBool(true));
            s.effective.set(key::kNativeEyes, JsonValue::makeBool(true));
            s.effective.set(key::kCameraLids, JsonValue::makeBool(true));
            s.effective.set(key::kPupilsToVrchat, JsonValue::makeBool(false));
            s.effective.set(key::kPupilBits, JsonValue::makeNumber(3, true));
        }
    } else {
        s.readError = "no status file";
    }
    m.eyecamDir = options.eyecamDir;
    if (!options.fakeEyecam.empty()) {
        m.eyecam = fakeEyecam(options.fakeEyecam);
    } else if (!options.fakePreview.empty() && options.fakePreview != "noeyecam") {
        // The eye preview's pictures need the eye cameras set up and in use
        m.eyecam = fakeEyecam("idle:setup:auto=ok:buffers:ready:saved");
    }
    if (!options.fakePreview.empty()) {
        eye_preview::fake(m.preview, eye_preview::monotonicNs());
        if (std::isfinite(options.fakePreviewBrows)) eye_preview::fakeBrows(m.preview, options.fakePreviewBrows);
        const std::string& state = options.fakePreview;
        if (state == "nopicture") {
            m.preview.hasPictures = false;
        } else if (state == "closed") {
            // The left eye shut: the eye camera reads it closed and finds no pupil, the eyelid goes out at 0
            gaze_dots::PacketEye& left = m.preview.packet.eyes[0];
            left.lid = left.cameraLid = 0.0f;
            left.openness = left.opennessScaled = 0.04f;
            left.cameraClosed = true;
            left.lidFrom = 0;
            eye_preview::Picture& picture = m.preview.pictures.eyes[0];
            picture.closed = true;
            picture.pupil = false;
        } else if (state == "nodata") {
            m.preview.hasPacket = false;
        }
    }
    if (!options.fakeContrib.empty()) fakeContrib(m, options.fakeContrib, !options.fakeEyecam.empty());
    m.eyecam.password = options.fakePassword == "unset"   ? eyecam::PasswordState::NotSet
                        : options.fakePassword == "unknown" ? eyecam::PasswordState::Unknown
                                                            : eyecam::PasswordState::Set;
    m.autostart.autostart = options.fakeAutostart;
    m.language = configLanguage(m.config);
    if (options.fakeWriteError) m.panelError = "rename failed: Read-only file system";
    if (options.fakeModuleLinkError) m.moduleLinkError = "flatpak not found";
    m.update = fakeUpdate(options.fakeUpdate, options.fakeUpdateNotes);
    if (options.fakeUpdate.empty()) m.update.state = frame_updater::UpdateState::UpToDate;
    // The diagnostics page's SteamOS and camera tool, as on the developer's headset
    m.system.steamos = "0.4.3 (20260930.6234839)";
    m.system.grabHash = "294d06c6";
    return m;
}

void applyHit(const PanelHit& hit, PanelModel& model, EyePanel& panel, AutostartWorker& autostart,
              frame_updater::UpdateChecker* updater, gaze_fit::Session* fit, const VrOverlay* vr,
              recorder::Recorder* eyeLog, eyecam::Control* eyecamControl, contrib::Session* contribSession = nullptr);
std::string targetLabel(const UiText& t, gaze_fit::TargetStyle style);

/**
 * --update-live: tick the update checker until its check or install is over (or time runs out).
 * @param updater the checker
 * @param enabled the update_check setting
 */
void settleUpdater(frame_updater::UpdateChecker& updater, bool enabled) {
    const double end = nowSeconds() + kUpdateSettleSec;
    // Give a just-started install a moment to write its state file
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    while (nowSeconds() < end) {
        updater.tick(enabled);
        const auto& u = updater.status();
        if (!u.checking && u.state != frame_updater::UpdateState::Installing) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

/**
 * Read the version history for the panel's language (when it is opened, and again if the language changes while
 * it is open).
 * @param model the model (its changelogDirs; history is written)
 */
/**
 * What the diagnostics page reads itself: SteamOS's version (once) and the camera tool's checksums (again only when a
 * file changed): install.sh's copy, and the installed one eyecam-rec runs when it can be read (no root needed).
 * @param model where to put them
 */
void readSystem(PanelModel& model) {
    static diag::FileHash grabHash;
    static diag::FileHash installedHash;
    if (model.system.steamos.empty()) model.system.steamos = diag::readSteamos();
    model.system.grabHash = grabHash.get(diag::defaultGrabPath());
    model.system.grabUnreadable = grabHash.unreadable();
    model.system.installedHash = installedHash.get(diag::kInstalledGrabPath);
}

void loadHistory(PanelModel& model) {
    model.history = changelog::load(model.changelogDirs, model.language == Language::Ja);
    std::fprintf(stderr, "[history] %zu versions from %s\n", model.history.entries.size(),
                 model.history.found ? model.history.dir.c_str() : "nowhere (no CHANGELOG.md found)");
}

/**
 * A local time written "YYYY-MM-DD HH:MM:SS" (or Unix seconds).
 * @param text the time
 * @param when where to write it
 * @return true if it could be read
 */
bool parseLocalTime(const std::string& text, double& when) {
    std::tm tm {};
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                    &tm.tm_sec) == 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = -1;
        when = static_cast<double>(std::mktime(&tm));
        return true;
    }
    char* end = nullptr;
    when = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' && when > 0;
}

/**
 * The records again (when "Having trouble" or all records show, and when one was written).
 * @param model the model (records.list)
 */
void loadRecords(PanelModel& model) {
    model.records.list = report::list(model.records.dir);
}

/**
 * Read a record for its view.
 * @param model the model (records.opened, openedFound, files)
 * @param folder its folder name
 */
void openRecordView(PanelModel& model, const std::string& folder) {
    model.records.opened = report::Summary();
    model.records.files.clear();
    model.records.openedFound = report::load(model.records.dir, folder, model.records.opened, &model.records.files);
    std::fprintf(stderr, "[report] %s: %s\n", folder.c_str(), model.records.openedFound ? "shown" : "not found");
}

/**
 * Made-up records for --fake-records: ten, newest first, the second a failed eye fit with its key lines (as on the
 * developer's headset on 2026-10-06).
 * @return them
 */
std::vector<report::Summary> fakeRecordList() {
    using report::Kind;
    using report::Result;
    using report::Source;
    const auto at = [](const char* text) {
        double when = 0;
        parseLocalTime(text, when);
        return when;
    };
    const auto make = [&](Kind kind, const char* start, double seconds, Result result, const char* brief,
                          const char* briefEn, const char* reason, const char* reasonEn) {
        report::Summary s;
        s.kind = kind;
        s.start = at(start);
        s.end = s.start + seconds;
        s.result = result;
        s.brief = brief;
        s.briefEn = briefEn;
        s.reason = reason;
        s.reasonEn = reasonEn;
        s.version = FRAMEEYEOSC_VERSION;
        s.steamos = "0.4.3 (20260930.6234839)";
        s.code = "OK·P24·B128·G1·C1·FW·V2";
        s.mode = kind == Kind::Fit ? "full" : kind == Kind::Recenter ? "center" : "";
        s.conditions.dashboardOpen = 1;
        s.conditions.cameraFps = 90;
        s.conditions.trackerRate = 90;
        s.conditions.trackerMissed = 0;
        s.folder = report::folderName(kind, s.start);
        return s;
    };
    std::vector<report::Summary> list;
    list.push_back(make(Kind::Fit, "2026-10-06 20:46:02", 21.4, Result::Ok, "", "",
                        "合わせました。視線の正面: 左右 +0.021・上下 -0.013",
                        "Fitted. Gaze center: L-R +0.021, U-D -0.013"));
    {
        report::Summary s = make(Kind::Fit, "2026-10-06 20:45:09", 7.9, Result::Failed,
                                 "正面の点で視線が落ち着きませんでした", "The gaze wasn't steady at the center dot",
                                 "正面 の点で視線が落ち着きませんでした（目を閉じていたかも） 正面の点: 使えたサンプル "
                                 "0/153（毎秒 90・45 以上が必要）・ばらつき —（2.7° まで）・3 回",
                                 "The gaze wasn't steady at the center dot (eyes closed?) Center dot: 0 of 153 samples "
                                 "usable at 90 Hz (needs 45) · spread — (max 2.7°) · 3 tries");
        s.conditions.sincePutOnSec = 10;
        s.conditions.offBeforeSec = 201.3;
        s.conditions.ipdMm = 69.6;
        const double t0 = s.start;
        s.flow = {
            {t0 - 10.2, Source::Core, "Eye tracking resumed after 202.4 s"},
            {t0 - 10.1, Source::Panel, "[fit] put on (tracking was off 201.3 s): re-centering once the eyes settle"},
            {t0 - 10.2, Source::Valve, "[Info] - HMD on, starting eye tracking"},
            {t0, Source::Panel, "[fit] eye fit, IPD 69.6 mm, dashboard open"},
            {t0 + 2.6, Source::Panel,
             "[fit] center try 1: 0 of 153 samples usable at 90 Hz (needs 45), no gaze average -> again"},
            {t0 + 5.3, Source::Panel,
             "[fit] center try 2: 0 of 153 samples usable at 90 Hz (needs 45), no gaze average -> again"},
            {t0 + 7.9, Source::Panel,
             "[fit] center try 3: 0 of 153 samples usable at 90 Hz (needs 45), no gaze average -> failed"},
            {t0 + 7.9, Source::Panel, "[fit] target up 7.9 s: 944 frames (119 per second), 902 pictures drawn"},
        };
        std::stable_sort(s.flow.begin(), s.flow.end(),
                         [](const report::LogLine& a, const report::LogLine& b) { return a.at < b.at; });
        list.push_back(s);
    }
    list.push_back(make(Kind::CalibWear, "2026-10-06 19:43:02", 21.0, Result::Ok, "カメラ 72 枚/秒", "cameras 72 fps",
                        "校正できた", "Calibrated"));
    {
        report::Summary s = make(Kind::Recenter, "2026-10-06 19:40:11", 3.2, Result::Ok, "", "", "真ん中で合わせ直しました",
                                 "Re-centered");
        s.trigger = "auto";
        s.conditions.dashboardOpen = 0;
        list.push_back(s);
    }
    list.push_back(make(Kind::CalibWear, "2026-10-06 18:12:40", 20.5, Result::Partial,
                        "右目は瞳がうまく見えなかったので、前の値を使うよ", "Right eye: previous values",
                        "校正できた（右目は瞳がうまく見えなかったので、前の値を使うよ）[12/486、90 必要]",
                        "Calibrated (the right eye's pupil wasn't seen well: its previous values are used) [12/486, 90 "
                        "needed]"));
    list.push_back(make(Kind::CalibWear, "2026-10-06 18:11:02", 20.1, Result::Failed, "両目の瞳がうまく見えなかった",
                        "Both eyes: couldn't see the pupil well",
                        "両目の瞳がうまく見えなかった（HMD のかぶり方を直して、もう一度）[左 2/81・右 5/81、15 必要]",
                        "Both eyes: couldn't see the pupil well (adjust the headset and try again) [L 2/81, R 5/81, 15 "
                        "needed]"));
    list.push_back(make(Kind::CalibUser, "2026-10-05 22:30:15", 19.0, Result::Ok, "", "", "校正できた（ユーザー）",
                        "Calibrated (user)"));
    list.push_back(make(Kind::Fit, "2026-10-05 22:28:40", 20.9, Result::Ok, "", "", "合わせました", "Fitted"));
    list.push_back(make(Kind::Recenter, "2026-10-05 21:05:03", 2.9, Result::Stopped, "止めました", "Stopped", "止めました",
                        "Stopped"));
    list.push_back(make(Kind::CalibWear, "2026-10-05 21:01:44", 20.3, Result::Ok, "カメラ 90 枚/秒", "cameras 90 fps",
                        "校正できた", "Calibrated"));
    return list;
}

/**
 * --dump-png / --thumbnail-png: draw without OpenVR and save PNGs.
 * @param options the command line
 * @return exit code
 */
int runDumpPng(const Options& options) {
    FontSet fonts;
    fonts.load(kFontPath, kBoldFontPath);
    if (!options.pngPath.empty()) {
        PanelModel model;
        if (options.fake) {
            model = fakeModel(options);
        } else {
            model.configPath = options.configPath;
            model.statusPath = options.statusPath;
            model.config = readConfigFile(options.configPath);
            model.status = readStatus(options.statusPath, unixNow());
            model.autostart.autostart = readAutostart();
            model.language = configLanguage(model.config);
            // eyecam-rec as it is now (its tab shows only while it runs; --eyecam-dir to try it on a test folder)
            model.eyecamDir = options.eyecamDir;
            model.eyecam.status = eyecam::readStatus(model.eyecamDir);
            eyecam::followProtocol(model.eyecam, eyecam::libDir());
            model.eyecam.visible = eyecam::tabVisible(model.eyecam.status, unixNow());
            model.eyecam.lastRun = eyecam::followRun(model.eyecam.lastRun, model.eyecam.status);
            model.update.current = FRAMEEYEOSC_VERSION;
            readSystem(model);
        }
        if (!options.language.empty()) parseLanguage(options.language, model.language);
        model.changelogDirs =
            options.changelogDir.empty() ? changelog::defaultDirs() : std::vector<std::string> {options.changelogDir};
        // The records: made up, or the folder's
        model.records.dir = options.reportsDir;
        model.records.list = options.fakeRecords ? fakeRecordList() : report::list(model.records.dir);
        if (options.fakeSaved) {
            model.records.lastFit = "fit_2026-10-06_20-45-09";
            model.records.lastCalib = "calib-wear_2026-10-06_18-11-02";
        }
        model.apps = dumpApps(options);
        EyePanel panel(fonts);
        panel.setTab(options.tab);
        if (AdvPage page; ui_state::parsePage(options.advPage, page)) panel.setAdvPage(page);
        if (LidsPage page; ui_state::parseLidsPage(options.lidsPage, page)) panel.setLidsPage(page);
        if (GazePage page; ui_state::parseGazePage(options.gazePage, page)) panel.setGazePage(page);
        // The gaze scale's locks as at a start with them left closed: open where the two values differ
        {
            const SettingsView view(model);
            for (int pair = 0; pair < 2; ++pair) {
                panel.setGazeScaleLinked(pair, gazeScaleLinkShown(true, view.number(kGazeScaleKeys[pair][0]),
                                                                  view.number(kGazeScaleKeys[pair][1])));
            }
        }
        if (options.recordsAll) panel.openRecordsAll();
        if (!options.record.empty()) {
            std::string folder = options.record;
            if (folder == "latest") folder = model.records.list.empty() ? "" : model.records.list.front().folder;
            if (options.fakeRecords) {
                // --record N: the Nth made-up one
                const size_t index = static_cast<size_t>(std::max(0, std::atoi(options.record.c_str())));
                if (index < model.records.list.size()) {
                    folder = model.records.list[index].folder;
                    model.records.opened = model.records.list[index];
                    model.records.openedFound = true;
                    model.records.files = {{"report.txt", 4 * 1024}, {"logs.txt", 86 * 1024},
                                           {"status.jsonl", 31 * 1024}, {"summary.json", 2 * 1024}};
                }
            } else {
                openRecordView(model, folder);
            }
            panel.openRecord(folder);
        }
        panel.setFitDetails(options.fitDetails);
        panel.setFitDetailsPage(options.fitDetailsPage);
        if (options.diag) panel.openDiag();
        panel.setAdvancedScroll(options.advScroll);
        if (options.history) {
            loadHistory(model);
            panel.openHistory();
            if (!options.historyOpen.empty()) panel.setHistoryRow(options.historyOpen);
            if (options.historyScroll >= 0) panel.setHistoryScroll(options.historyScroll);
        }
        if (options.fakeEyecam.rfind("confirm", 0) == 0) panel.openEyecamConfirm(model.eyecam.status.state);
        // (with --adv-page tools: its row on Debug tools, the sheet closed)
        if (!options.fakeContrib.empty() && options.advPage != "tools") {
            const std::string& state = options.fakeContrib;
            panel.openContrib(state == "list" || state == "deleting" ? contrib::Page::Manage : contrib::Page::Record);
            panel.setContribAgreed(state == "eyewear");
            if (state == "delete") panel.openContribDelete();
        }
        if (!options.appsSheet.empty()) {
            const std::string& sheet = options.appsSheet;
            panel.openAppsList();
            if (sheet == "menu") {
                panel.openAppsConfirm("");
            } else if (sheet.rfind("confirm", 0) == 0) {
                // confirm: the first app not installed (or the first one)
                std::string key = sheet.size() > 8 ? sheet.substr(8) : std::string();
                for (const frame_apps::Entry& entry : model.apps.entries) {
                    if (key.empty() && entry.state == frame_apps::AppState::Missing) key = entry.app.key;
                }
                if (key.empty() && !model.apps.entries.empty()) key = model.apps.entries.front().app.key;
                panel.openAppsConfirm(key);
            }
            const std::string& launch = options.fakeAppsLaunch;
            if (!launch.empty() && sheet != "list") {
                using frame_apps::LaunchResult;
                panel.setAppsLaunchResult(launch == "busy"        ? LaunchResult::Busy
                                          : launch == "nodisplay" ? LaunchResult::NoDisplay
                                          : launch == "nokonsole" ? LaunchResult::NoKonsole
                                                                  : LaunchResult::Failed);
            }
        }
        if (options.previewSheet) {
            eye_preview::Source source = eye_preview::Source::Sent;
            bool mirror = true;
            bool camera = true;
            bool frozen = false;
            parsePreviewSpec(options.previewSpec, source, mirror, camera, frozen);
            panel.openPreview();
            panel.setPreviewOptions(source, mirror, camera, frozen);
            // The pictures from eyecam-rec's preview file when it is there (eyecam-rec --replay --preview DIR, with
            // --eyecam-dir DIR); a file of another kind says why
            eye_preview::PictureReader reader;
            std::string why;
            const std::string path = model.eyecamDir + "/" + eye_preview::kFileName;
            switch (reader.read(path, model.preview.pictures, why)) {
                case eye_preview::ReadResult::Read:
                    model.preview.hasPictures = true;
                    model.preview.nowNs = eye_preview::monotonicNs();
                    std::fprintf(stderr, "[preview] pictures from %s\n", path.c_str());
                    break;
                case eye_preview::ReadResult::Invalid:
                    std::fprintf(stderr, "[preview] %s: %s\n", path.c_str(), why.c_str());
                    break;
                default: break;
            }
        }
        if (options.previewQuit) panel.armQuitForPreview();
        if (options.previewReset) panel.armResetForPreview();
        if (!options.fakePrompt.empty()) panel.showPrompt(options.fakePrompt);
        if (options.previewUpdatePrompt) panel.showUpdatePrompt(model.update.latest, model.update.backToStable);
        panel.setUpdateBeta(options.updateBeta);
        std::unique_ptr<frame_updater::UpdateChecker> updater;
        const bool updateCheck = model.config.flag(key::kUpdateCheck);
        if (options.updateLive) {
            updater = std::make_unique<frame_updater::UpdateChecker>(updaterConfig(panel.updateBeta()));
            updater->tick(updateCheck);
            settleUpdater(*updater, updateCheck);
            model.update = updater->status();
        }
        // Presses as the laser pointer would make them (hit areas and config writes, without a headset)
        AutostartWorker idleAutostart;  // never started: autostart presses are only logged
        eyecam::Control eyecamControl;  // the eye capture tab's buttons really talk to --eyecam-dir's socket
        for (const auto& click : options.clicks) {
            panel.render(model);
            const PanelHit hit = panel.pointerDown(click.first, click.second, nowSeconds());
            std::printf("click %.0f,%.0f -> action %d key %s arg %d\n", click.first, click.second,
                        static_cast<int>(hit.action), hit.key != nullptr ? hit.key : "-", hit.arg);
            if (hit.action != PanelAction::Quit) {
                applyHit(hit, model, panel, idleAutostart, updater.get(), nullptr, nullptr, nullptr, &eyecamControl);
            }
            // The other apps' "Open in Konsole": only said here (nothing is fetched or opened from --dump-png)
            if (hit.action == PanelAction::AppsLaunch) {
                std::printf("apps: would open Konsole with: %s\n", panel.appsConfirmCommand(model).c_str());
            }
            // Wait for eyecam-rec's reply, and show the folder as it is after it
            if (hit.action == PanelAction::EyecamChoose || hit.action == PanelAction::EyecamStop ||
                hit.action == PanelAction::EyecamCalib || hit.action == PanelAction::EyecamRelearn ||
                hit.action == PanelAction::EyecamLidVeto) {
                while (eyecamControl.busy()) {
                    eyecamControl.poll(nowSeconds());
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                const eyecam::Reply& r = eyecamControl.reply();
                std::printf("eyecam: %s -> %s%s\n", r.command.c_str(), r.ok ? "ok" : "err ", r.error.c_str());
                syncEyecamControl(model.eyecam, eyecamControl);
                if (r.ok && eyecam::runOfCommand(r.command) != eyecam::Run::None) {
                    model.eyecam.lastRun = eyecam::runOfCommand(r.command);
                }
                model.eyecam.status = eyecam::readStatus(model.eyecamDir);
                eyecam::followProtocol(model.eyecam, eyecam::libDir());
                model.eyecam.visible = eyecam::tabVisible(model.eyecam.status, unixNow());
                model.eyecam.lastRun = eyecam::followRun(model.eyecam.lastRun, model.eyecam.status);
            }
            panel.pointerUp();
            takeSliders(panel, [&](const PanelHit& slid) {
                applyHit(slid, model, panel, idleAutostart, updater.get(), nullptr, nullptr, nullptr, &eyecamControl);
            });
            double sensitivity = 0.0;
            if (panel.takeSensitivity(sensitivity, nowSeconds())) {
                const std::string command = eyecam::sensitivityCommand(sensitivity);
                std::fprintf(stderr, "[eyecam] sending \"%s\" to %s/ctl.sock\n", command.c_str(),
                             model.eyecamDir.c_str());
                eyecamControl.send(model.eyecamDir + "/ctl.sock", command, nowSeconds());
                while (eyecamControl.busy()) {
                    eyecamControl.poll(nowSeconds());
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                const eyecam::Reply& r = eyecamControl.reply();
                std::printf("eyecam: %s -> %s%s\n", r.command.c_str(), r.ok ? "ok" : "err ", r.error.c_str());
                if (!r.ok) panel.dropSensitivityHold();
                syncEyecamControl(model.eyecam, eyecamControl);
                model.eyecam.status = eyecam::readStatus(model.eyecamDir);
                model.eyecam.visible = eyecam::tabVisible(model.eyecam.status, unixNow());
            }
            if (updater) {
                settleUpdater(*updater, model.config.flag(key::kUpdateCheck));
                model.update = updater->status();
            }
        }
        if (updater) {
            const auto& u = model.update;
            std::printf("update: state %d, checking %d, current %s, latest %s, installable %d, step %s, version %s, "
                        "error %s, checked_at %lld\n",
                        static_cast<int>(u.state), u.checking, u.current.c_str(), u.latest.c_str(), u.installable,
                        u.step.c_str(), u.version.c_str(), u.error.c_str(), u.checkedAt);
        }
        panel.pointerLeave();
        // (after the pointer left: leaving lets go of the slider)
        if (options.sensitivityDrag >= 0) panel.previewSensitivityDrag(options.sensitivityDrag);
        panel.render(model);
        if (options.previewBench > 0 && panel.previewOpen()) {
            // What one draw of the panel costs with the eye preview open (the VR loop draws it about 30 times a second
            // while the eyes move)
            const double start = nowSeconds();
            for (int i = 0; i < options.previewBench; ++i) {
                model.preview.packet.gaze[0] = static_cast<float>(0.2 * std::sin(i * 0.1));
                panel.render(model);
                panel.toRgba();
            }
            const double each = (nowSeconds() - start) / options.previewBench;
            std::printf("preview: %.2f ms per draw (and its RGBA copy) over %d draws\n", each * 1000,
                        options.previewBench);
        }
        if (!panel.writePng(options.pngPath)) {
            std::fprintf(stderr, "Could not write the PNG: %s\n", options.pngPath.c_str());
            return 1;
        }
        std::printf("Wrote %s (%dx%d)\n", options.pngPath.c_str(), panel.width(), panel.height());
    }
    if (!options.dotPngPath.empty()) {
        const DotKind kind = options.dotKind == "left"    ? DotKind::Left
                             : options.dotKind == "right" ? DotKind::Right
                                                          : DotKind::Both;
        std::vector<uint8_t> rgba;
        if (options.dotRing) {
            renderGazeRing(kind, rgba, options.dotPngPath);
        } else {
            renderGazeDot(kind, rgba, options.dotPngPath);
        }
        std::printf("Wrote %s (%dx%d)\n", options.dotPngPath.c_str(), kDotImageSize, kDotImageSize);
    }
    if (!options.targetPngPath.empty()) {
        Language language = Language::Ja;
        if (!options.language.empty()) parseLanguage(options.language, language);
        const UiText& t = uiText(language);
        using gaze_fit::TargetStyle;
        const std::string& name = options.targetStyle;
        const TargetStyle style = name == "close"  ? TargetStyle::CloseEyes
                                  : name == "keep" ? TargetStyle::KeepClosed
                                  : name == "open" ? TargetStyle::OpenEyes
                                                   : TargetStyle::Dot;
        const std::string label = targetLabel(t, style);
        std::vector<uint8_t> rgba;
        renderTarget(fonts, style, label, options.targetSeconds, options.targetProgress, rgba, options.targetPngPath);
        std::printf("Wrote %s (%dx%d)\n", options.targetPngPath.c_str(), kTargetImageSize, kTargetImageSize);
        if (options.targetBench > 0) {
            // What one frame of the target costs to draw (the VR loop draws one per display frame while it is up)
            double start = nowSeconds();
            for (int i = 0; i < options.targetBench; ++i) {
                renderTarget(fonts, style, label, 3, 1.0 - static_cast<double>(i) / options.targetBench, rgba);
            }
            const double each = (nowSeconds() - start) / options.targetBench;
            std::printf("target: %.3f ms per frame over %d frames (%.0f frames/s possible)\n", each * 1000,
                        options.targetBench, 1.0 / each);
            // As the VR loop draws it: one gaze point's ring running down over kPointSec at 90 frames/s, paced to
            // frame deadlines (as WaitFrameSync would), drawing only when the picture changes
            TargetPainter painter;
            const double frame = 1.0 / 90;
            const int frames = static_cast<int>(std::lround(gaze_fit::kPointSec / frame));
            int drawn = 0;
            double busy = 0.0;
            start = nowSeconds();
            for (int i = 0; i < frames; ++i) {
                const double workStart = nowSeconds();
                const double left = gaze_fit::kPointSec - i * frame;
                const int seconds = left <= gaze_fit::kCaptureSec ? static_cast<int>(std::ceil(left - 1e-9)) : 0;
                if (painter.paint(fonts, style, label, seconds, left / gaze_fit::kPointSec)) ++drawn;
                busy += nowSeconds() - workStart;
                std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(std::chrono::duration<double>(start + (i + 1) * frame))));
            }
            const double took = nowSeconds() - start;
            std::printf("paced: %d frames in %.3f s (%.1f frames/s), %d pictures drawn, %.3f ms busy per frame\n",
                        frames, took, frames / took, drawn, busy / frames * 1000);
        }
    }
    if (!options.fillPngPath.empty()) {
        Language language = Language::Ja;
        if (!options.language.empty()) parseLanguage(options.language, language);
        const std::string label = eyecam::instruction(uiText(language), options.fillKind);
        std::vector<uint8_t> rgba;
        renderFill(fonts, options.fillKind == "bright", label, rgba, options.fillPngPath);
        std::printf("Wrote %s (%dx%d)\n", options.fillPngPath.c_str(), kFillImageSize, kFillImageSize);
    }
    if (!options.thumbnailPngPath.empty()) {
        std::vector<uint8_t> rgba;
        renderThumbnail(fonts, options.thumbnailSize, rgba, options.thumbnailPngPath);
        std::printf("Wrote %s (%dx%d)\n", options.thumbnailPngPath.c_str(), options.thumbnailSize,
                    options.thumbnailSize);
    }
    return 0;
}

/**
 * --report: a record's report.txt (or the list of them), in the config's language, for reading over SSH.
 * @param options the command line
 * @return exit code (1 if there is no such record)
 */
int runReport(const Options& options) {
    const UiText& t = uiText(configLanguage(readConfigFile(options.configPath)));
    std::string out;
    const int code = report::cliReport(t, options.reportsDir, options.report, out);
    std::fputs(out.c_str(), code == 0 ? stdout : stderr);
    return code;
}

/**
 * --report-make: a record of a window from today's logs, as the panel writes one after a run (for testing the
 * records against the real journal and Valve's log; nothing else is read).
 * @param options the command line
 * @return exit code
 */
int runReportMake(const Options& options) {
    const std::vector<std::string>& a = options.reportMake;
    report::Pending pending;
    report::Summary& s = pending.summary;
    if (a.size() != 5 || !report::parseKind(a[0], s.kind) || !parseLocalTime(a[1], s.start) ||
        !parseLocalTime(a[2], s.end) || !report::parseResult(a[3], s.result) || s.end < s.start) {
        std::fprintf(stderr, "--report-make KIND START END RESULT REASON: KIND fit|recenter|calib-wear|calib-user, "
                             "START / END \"YYYY-MM-DD HH:MM:SS\", RESULT ok|failed|partial|stopped\n");
        return 1;
    }
    s.mode = s.kind == report::Kind::Fit ? "full" : s.kind == report::Kind::Recenter ? "center" : "";
    s.reason = s.reasonEn = a[4];
    s.version = FRAMEEYEOSC_VERSION;
    s.steamos = diag::readSteamos();
    const ConfigFile config = readConfigFile(options.configPath);
    pending.language = configLanguage(config);
    std::string folder;
    std::string error;
    if (!report::writeRecord(options.reportsDir, pending, report::systemSources(), folder, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        if (folder.empty()) return 1;
    }
    std::printf("%s/%s\n", options.reportsDir.c_str(), folder.c_str());
    return 0;
}

/**
 * --print: what the panel sees, for checking over SSH.
 * @param options the command line
 * @return exit code (1 if the config is broken)
 */
int runPrint(const Options& options) {
    const ConfigFile config = readConfigFile(options.configPath);
    std::printf("config: %s (%s)\n", options.configPath.c_str(),
                !config.exists ? "missing, all defaults" : (config.error.empty() ? "ok" : config.error.c_str()));
    for (const SettingSpec& spec : settingSpecs()) {
        const JsonValue value = config.value(spec.key);
        JsonValue wrapper;
        wrapper.type = JsonValue::Type::Object;
        wrapper.set(spec.key, value);
        std::string line = writeJson(wrapper);
        // Keep only the "key": value line
        const size_t start = line.find('"');
        const size_t end = line.find('\n', start);
        std::printf("  %s\n", line.substr(start, end - start).c_str());
    }
    const EyeStatus status = readStatus(options.statusPath, unixNow());
    std::printf("status: %s (%s)\n", options.statusPath.c_str(),
                !status.present ? status.readError.c_str() : (status.running ? "running" : "stale or pid gone"));
    if (status.present) {
        std::printf("  pid %d, age %.2f s, sending %s, output %s, target %s (%s), rate %.1f, tracking %s\n", status.pid,
                    unixNow() - status.time, status.sending ? "yes" : "no", status.output.c_str(),
                    status.target.empty() ? "null" : status.target.c_str(), status.targetMode.c_str(), status.rate,
                    status.tracking ? "yes" : "no");
        std::printf("  raw openness_scaled %.3f %.3f, gaze %.3f %.3f; sent lids %.3f %.3f, gaze %.3f %.3f\n",
                    status.opennessScaled.v[0], status.opennessScaled.v[1], status.gaze.v[0], status.gaze.v[1],
                    status.lids.v[0], status.lids.v[1], status.sentGaze.v[0], status.sentGaze.v[1]);
        std::printf("  calibration enabled %s, relaxed %.3f %.3f, scales %.3f %.3f, learning %s\n",
                    status.calibrationEnabled ? "yes" : "no", status.relaxed.v[0], status.relaxed.v[1],
                    status.scales.v[0], status.scales.v[1], status.learning ? "yes" : "no");
        std::string locked;
        for (const auto& name : status.locked) locked += " " + name;
        std::printf("  locked:%s\n  config_error: %s\n  source_error: %s\n  config_path: %s\n",
                    locked.empty() ? " (none)" : locked.c_str(),
                    status.configError.empty() ? "null" : status.configError.c_str(),
                    status.sourceError.empty() ? "null" : status.sourceError.c_str(), status.configPath.c_str());
        std::printf("  dominant_eye: %s, openness_saturated: %s\n",
                    status.dominantEye.empty() ? "null" : status.dominantEye.c_str(),
                    status.opennessSaturated ? "true" : "false");
        std::printf("  eye data %.0f/s, missed %.0f/s, longest sample %.1f ms, dropped %.0f/s\n", status.trackerRate,
                    status.missedRate, status.maxProcessingMs, status.droppedRate);
    }
    const Autostart autostart = readAutostart();
    std::printf("autostart (%s): %s\n", kServiceName,
                autostart == Autostart::Enabled    ? "enabled"
                : autostart == Autostart::Disabled ? "disabled"
                : autostart == Autostart::Missing  ? "not installed"
                                                   : "unknown");
    return config.error.empty() ? 0 : 1;
}

/**
 * A short text of what the status column shows, rounded so that the panel is only redrawn when something
 * visible changes.
 * @param s the status
 * @return the text
 */
std::string statusSignature(const EyeStatus& s) {
    char text[1024];
    std::snprintf(text, sizeof(text),
                  "%d|%d|%d|%s|%s|%s|%.0f|%d|%d|%.2f %.2f %.2f %.2f|%.2f %.2f %.2f %.2f|%.2f %.2f|%.2f %.2f|%.2f %.2f|%d|%d|%d",
                  s.running, s.pid, s.sending, s.output.c_str(), s.targetMode.c_str(), s.target.c_str(), s.rate,
                  s.tracking, s.hasRaw, s.opennessScaled.v[0], s.opennessScaled.v[1], s.gaze.v[0], s.gaze.v[1],
                  s.lids.v[0], s.lids.v[1], s.sentGaze.v[0], s.sentGaze.v[1], s.relaxed.v[0], s.relaxed.v[1],
                  s.scales.v[0], s.scales.v[1], s.lidsVrcft.v[0], s.lidsVrcft.v[1], s.learning, s.calibrationEnabled,
                  static_cast<int>((s.time - s.started) / 60));
    std::string signature = text;
    signature += "|" + std::to_string(static_cast<int>(trackerRateCause(s)));
    char eyes[128];
    std::snprintf(eyes, sizeof(eyes), "|%.2f %.2f %.2f %.2f|%.2f %.2f %.2f %.2f", s.rawGazeEye[0].v[0],
                  s.rawGazeEye[0].v[1], s.rawGazeEye[1].v[0], s.rawGazeEye[1].v[1], s.sentGazeEye[0].v[0],
                  s.sentGazeEye[0].v[1], s.sentGazeEye[1].v[0], s.sentGazeEye[1].v[1]);
    signature += eyes;
    const CameraStatus& c = s.camera;
    signature += "|" + std::to_string(c.known) + std::to_string(c.present) + std::to_string(c.calibState) +
                 std::to_string(c.recalibSuggested) + std::to_string(c.used[0]) + std::to_string(c.used[1]) +
                 std::to_string(c.pupilUsed[0]) + std::to_string(c.pupilUsed[1]) + c.error;
    signature += "|" + s.pupilTarget;
    signature += "|" + s.configError + "|" + s.sourceError + "|" + s.dominantEye + "|" +
                 (s.opennessSaturated ? "saturated" : "") + "|" + s.configPath + "|" + s.calibrationPath + "|";
    for (const auto& name : s.locked) signature += name + ",";
    if (s.effective.isObject()) signature += writeJson(s.effective);
    return signature;
}


/**
 * Write a change to config.json and keep the model in step; a failure is shown in the panel.
 * @param model the model (config and error are updated)
 * @param change edits the object
 * @return true if written
 */
bool writeConfig(PanelModel& model, const std::function<void(JsonValue&)>& change) {
    std::string error;
    const bool ok = updateConfigFile(model.configPath, change, error);
    if (ok) {
        model.panelError.clear();
        model.panelErrorBroken = false;
    } else {
        std::fprintf(stderr, "[config] %s\n", error.c_str());
        model.panelError = error;
        model.panelErrorBroken = !model.config.error.empty();
    }
    model.config = readConfigFile(model.configPath);
    model.language = configLanguage(model.config);
    return ok;
}

/**
 * Ask frameeyeosc for a gaze capture: gaze_capture = {"id": the next id, "target": target, "seconds": how long,
 * "skip": how much of the start to skip}.
 * @param model the model
 * @param target the target name
 * @param seconds how long it lasts
 * @param skip how much of its start is skipped
 * @param lastId the last id asked for (updated)
 * @return the new id, or 0 if the write failed
 */
long long writeCaptureRequest(PanelModel& model, const char* target, double seconds, double skip, long long& lastId) {
    long long id = 0;
    const std::string name = target;
    const long long last = lastId;
    const bool ok = writeConfig(model, [&id, name, last, seconds, skip](JsonValue& root) {
        // Always a new id, also after a restart of the panel or a hand-edited file
        const JsonValue* old = root.get(key::kGazeCapture);
        const JsonValue* oldId = old != nullptr && old->isObject() ? old->get("id") : nullptr;
        const long long written = oldId != nullptr && oldId->isNumber() ? static_cast<long long>(oldId->number) : 0;
        id = std::max(written, last) + 1;
        JsonValue request;
        request.type = JsonValue::Type::Object;
        request.set("id", JsonValue::makeNumber(static_cast<double>(id), true));
        request.set("target", JsonValue::makeString(name));
        request.set("seconds", JsonValue::makeNumber(seconds));
        request.set("skip", JsonValue::makeNumber(skip));
        root.set(key::kGazeCapture, request);
    });
    if (!ok) return 0;
    lastId = id;
    std::fprintf(stderr, "[fit] asked for gaze capture %lld (%s, %.1f s)\n", id, target, seconds);
    return id;
}

/**
 * Bring an older config.json up to date, once (see migrateLidScales, migrateGazePresets, migrateEyeOffsets,
 * migrateFitPolicy and migrateLidOpenSnap), and read it again.
 * @param model the model (its config is re-read after a write)
 */
void migrateConfig(PanelModel& model) {
    if (!model.config.exists || !model.config.error.empty() || !configNeedsMigration(model.config.root)) return;
    std::string lidLog;
    std::string gazeLog;
    std::string snapLog;
    std::string eyeLog;
    std::string fitLog;
    bool lids = false;
    bool gaze = false;
    bool snap = false;
    bool eyes = false;
    bool fitPolicy = false;
    std::string error;
    const bool ok = updateConfigFile(
        model.configPath,
        [&](JsonValue& root) {
            lids = migrateLidScales(root, lidLog);
            gaze = migrateGazePresets(root, gazeLog);
            eyes = migrateEyeOffsets(root, eyeLog);
            fitPolicy = migrateFitPolicy(root, fitLog);
            snap = migrateLidOpenSnap(root, snapLog);
        },
        error);
    if (!ok) {
        std::fprintf(stderr, "[config] could not bring config.json up to date: %s\n", error.c_str());
        return;
    }
    if (lids) {
        std::fprintf(stderr, "[config] from before 0.6.0: lid_widen = \"normal\"%s%s\n", lidLog.empty() ? "" : "; ",
                     lidLog.c_str());
    }
    if (gaze) {
        std::fprintf(stderr, "[config] gaze presets from before version 2: %s\n",
                     gazeLog.empty() ? "own values, kept" : gazeLog.c_str());
    }
    if (eyes && !eyeLog.empty()) std::fprintf(stderr, "[config] %s\n", eyeLog.c_str());
    if (fitPolicy) {
        std::fprintf(stderr, "[config] eye fit from before version 4 (only the zero point and the up gain are kept): %s\n",
                     fitLog.empty() ? "nothing else was set" : fitLog.c_str());
    }
    if (snap) std::fprintf(stderr, "[config] %s\n", snapLog.c_str());
    model.config = readConfigFile(model.configPath);
}

/**
 * Write an eye fit's result (see applyFitValues) and log it.
 * @param model the model
 * @param values the result
 * @param mode the mode
 * @return true if written
 */
bool writeFitValues(PanelModel& model, const gaze_fit::Values& values, gaze_fit::Mode mode) {
    const bool full = mode == gaze_fit::Mode::Full;
    if (full) {
        std::fprintf(stderr, "[fit] done: offset %+.3f %+.3f, tilt %+.1f deg, gains %.2f %.2f %.2f\n", values.offsetX,
                     values.offsetY, values.rollDeg, values.gainX, values.gainUp, values.gainDown);
    } else {
        std::fprintf(stderr, "[fit] done: offset %+.3f %+.3f (the rest unchanged)\n", values.offsetX, values.offsetY);
    }
    if (full && values.hasLids) {
        for (int eye = 0; eye < 2; ++eye) {
            if (values.lidsKept[eye]) {
                std::fprintf(stderr, "[fit] %s eyelid: kept as it was (Valve's openness stuck shut, the eye camera "
                             "saw it open)\n", eye == 0 ? "left" : "right");
                continue;
            }
            std::fprintf(stderr, "[fit] %s eyelid: closed %.3f, up %.3f, ahead %.3f, down %.3f\n",
                         eye == 0 ? "left" : "right", values.lidClosed[eye], values.lidUp[eye], values.lidOpen[eye],
                         values.lidDown[eye]);
        }
    }
    return writeConfig(model, [values, mode](JsonValue& root) { applyFitValues(root, values, mode); });
}

/**
 * The words on the target for the eyes-shut step.
 * @param t texts
 * @param style the target's look
 * @return the words (empty for the dot)
 */
std::string targetLabel(const UiText& t, gaze_fit::TargetStyle style) {
    switch (style) {
        case gaze_fit::TargetStyle::Dot: return "";
        case gaze_fit::TargetStyle::CloseEyes: return t.targetClose;
        case gaze_fit::TargetStyle::KeepClosed: return t.targetKeepClosed;
        case gaze_fit::TargetStyle::OpenEyes: return t.targetOpen;
    }
    return "";
}

/**
 * Why a typed host can't be used, in words.
 * @param t texts
 * @param problem what is wrong
 * @return the message
 */
std::string hostErrorText(const UiText& t, host_entry::HostError problem) {
    using host_entry::HostError;
    switch (problem) {
        case HostError::None: return "";
        case HostError::Empty: return t.hostErrEmpty;
        case HostError::Ipv4: return t.hostErrIpv4;
    }
    return "";
}

/**
 * Start an eye fit session: the first target shows at once, the dashboard open or closed.
 * @param fit the session
 * @param mode the whole fit or re-centering only
 * @param view the settings (the fit now)
 * @param vr the connection to SteamVR, for the IPD in the log (null: the default)
 */
void startFit(gaze_fit::Session& fit, gaze_fit::Mode mode, const SettingsView& view, const VrOverlay* vr) {
    const double ipd = vr != nullptr ? vr->userIpdMeters() : gaze_fit::kDefaultIpdM;
    const char* name = mode == gaze_fit::Mode::Full ? "eye fit" : "re-center";
    std::fprintf(stderr, "[fit] %s, IPD %.1f mm, dashboard %s\n", name, ipd * 1000,
                 vr != nullptr && vr->dashboardVisible() ? "open" : "closed");
    // The whole fit writes the mean up gain of the last few whole fits (panel.json keeps them)
    const std::vector<double> upGains =
        mode == gaze_fit::Mode::Full ? ui_state::readUpGains(ui_state::defaultPath()) : std::vector<double>();
    fit.start(mode, fitInConfig(view).values, nowSeconds(), upGains);
}

/**
 * Carry out a button press: re-read config.json, change keys, write it back; or ask the autostart worker.
 * @param hit the button
 * @param model the model (config and error are updated)
 * @param panel the panel (to open the recommendation prompt)
 * @param autostart the autostart worker
 * @param updater the update checker (null in --dump-png without --update-live)
 * @param fit the eye fit session (null in --dump-png)
 * @param vr the connection to SteamVR, for the IPD (null in --dump-png)
 * @param eyeLog the eye log (null in --dump-png)
 * @param eyecamControl eyecam-rec's control socket (null: its buttons do nothing)
 * @param contribSession "Record for squint training" (null in --dump-png: its buttons only say what they would do)
 */
void applyHit(const PanelHit& hit, PanelModel& model, EyePanel& panel, AutostartWorker& autostart,
              frame_updater::UpdateChecker* updater, gaze_fit::Session* fit, const VrOverlay* vr,
              recorder::Recorder* eyeLog, eyecam::Control* eyecamControl, contrib::Session* contribSession) {
    const SettingsView view(model);
    std::function<void(JsonValue&)> change;
    std::string openPrompt;
    bool reset = false;
    switch (hit.action) {
        case PanelAction::None:
        case PanelAction::Tab:
        case PanelAction::Quit: return;
        case PanelAction::PromptNo: std::fprintf(stderr, "[action] recommended settings: no\n"); return;
        case PanelAction::AutostartOn:
        case PanelAction::AutostartOff:
            std::fprintf(stderr, "[action] start with SteamVR: %s\n", hit.action == PanelAction::AutostartOn ? "on" : "off");
            autostart.request(hit.action == PanelAction::AutostartOn);
            return;
        case PanelAction::SetBool: {
            const std::string name = hit.key;
            const bool value = hit.arg != 0;
            change = [name, value](JsonValue& root) { root.set(name, JsonValue::makeBool(value)); };
            break;
        }
        case PanelAction::SetInteger: {
            const SettingSpec* spec = findSetting(hit.key);
            if (spec == nullptr || view.locked(hit.key) || hit.arg < spec->min || hit.arg > spec->max) return;
            const std::string name = hit.key;
            const double value = hit.arg;
            if (std::fabs(view.number(name) - value) < 1e-12) return;
            change = [name, value](JsonValue& root) { root.set(name, JsonValue::makeNumber(value, true)); };
            break;
        }
        case PanelAction::Step: {
            const SettingSpec* spec = findSetting(hit.key);
            if (spec == nullptr) return;
            const std::string name = hit.key;
            // The gaze scale: with its pair linked, the other one goes to the same value
            if (const int pair = gazeScalePair(name); pair >= 0) {
                const std::vector<SettingChange> changes =
                    gazeScaleStep(view, name, hit.arg, panel.gazeScaleLinked(pair));
                if (changes.empty()) return;
                change = [changes](JsonValue& root) {
                    for (const SettingChange& c : changes) root.set(c.key, c.value);
                };
                break;
            }
            double current = view.number(name);
            if (name == key::kPort && !std::isfinite(current)) current = view.port();
            if ((name == key::kLidScaleLeft || name == key::kLidScaleRight) && !std::isfinite(current)) {
                const double scale = model.status.scales.v[name == key::kLidScaleLeft ? 0 : 1];
                current = std::isfinite(scale) ? scale : 1.0;
            }
            double low = -1e9;
            double high = 1e9;
            lidMarkBounds(name, view, low, high);
            const double next = stepValue(*spec, current, hit.arg, low, high);
            if (std::fabs(next - current) < 1e-12) return;
            const bool integer = spec->type == SettingType::Integer || spec->type == SettingType::NullableInteger;
            change = [name, next, integer](JsonValue& root) { root.set(name, JsonValue::makeNumber(next, integer)); };
            break;
        }
        case PanelAction::HostAuto:
            change = [](JsonValue& root) { root.set(key::kHost, JsonValue::makeString("auto")); };
            break;
        case PanelAction::FixHost: {
            const std::string host = hostOfTarget(model.status.target);
            if (host.empty()) return;
            change = [host](JsonValue& root) { root.set(key::kHost, JsonValue::makeString(host)); };
            break;
        }
        case PanelAction::PortDefault:
            change = [](JsonValue& root) { root.set(key::kPort, JsonValue::makeNull()); };
            break;
        case PanelAction::SetOutput: {
            const std::string output = outputOfArg(hit.arg);
            if (view.text(key::kOutput) == output) return;
            // A new output type starts from its own default port
            change = [output](JsonValue& root) {
                root.set(key::kOutput, JsonValue::makeString(output));
                root.set(key::kPort, JsonValue::makeNull());
            };
            openPrompt = output;
            model.moduleLinkError.clear();  // (it was about the output before)
            break;
        }
        case PanelAction::SetActiveType: {
            if (hit.arg < 0 || hit.arg > 2) return;
            const std::string type = kActiveTypes[hit.arg];
            change = [type](JsonValue& root) { root.set(key::kEyeTrackingActive, JsonValue::makeString(type)); };
            break;
        }
        case PanelAction::RecordToggle:
            if (eyeLog == nullptr) return;
            if (eyeLog->recording()) {
                eyeLog->stop(nowSeconds());
            } else if (!eyeLog->busy()) {
                eyeLog->start(recorder::findFrameeyeosc(), recorder::defaultDir(), model.configPath, nowSeconds());
            }
            model.recording = eyeLog->view(nowSeconds());
            return;
        case PanelAction::SetLidWiden: {
            if (hit.arg < 0 || hit.arg > 3) return;
            const std::string mode = kLidWidenModes[hit.arg];
            change = [mode](JsonValue& root) { root.set(key::kLidWiden, JsonValue::makeString(mode)); };
            break;
        }
        case PanelAction::SetAutoRecenter: {
            if (hit.arg < 0 || hit.arg > 1) return;
            const std::string mode = kAutoRecenterModes[hit.arg];
            change = [mode](JsonValue& root) { root.set(key::kAutoRecenter, JsonValue::makeString(mode)); };
            break;
        }
        case PanelAction::Preset: {
            if (hit.arg < 0 || hit.arg > 2) return;
            const GazePreset preset = gazePresets()[hit.arg];
            change = [preset](JsonValue& root) {
                root.set(key::kGazeMinCutoff, JsonValue::makeNumber(preset.minCutoff));
                root.set(key::kGazeBeta, JsonValue::makeNumber(preset.beta));
                root.set(key::kGazeDCutoff, JsonValue::makeNumber(preset.dCutoff));
            };
            break;
        }
        case PanelAction::LidPreset: {
            if (hit.arg < 0 || hit.arg > 2) return;
            const LidPreset preset = lidPresets()[hit.arg];
            change = [preset](JsonValue& root) {
                root.set(key::kLidMinCutoff, JsonValue::makeNumber(preset.minCutoff));
                root.set(key::kLidBeta, JsonValue::makeNumber(preset.beta));
            };
            break;
        }
        case PanelAction::SetNumber: {
            const SettingSpec* spec = findSetting(hit.key);
            if (spec == nullptr || view.locked(hit.key)) return;
            const std::string name = hit.key;
            const double next = snapValue(*spec, hit.arg / 1000.0);
            if (std::fabs(next - view.number(name)) < 1e-12) return;
            change = [name, next](JsonValue& root) { root.set(name, JsonValue::makeNumber(next)); };
            break;
        }
        case PanelAction::NumberOn: {
            const SettingSpec* spec = findSetting(hit.key);
            if (spec == nullptr) return;
            const std::string name = hit.key;
            const double value = spec->defaultNumber > 0 ? spec->defaultNumber : spec->onNumber;
            change = [name, value](JsonValue& root) { root.set(name, JsonValue::makeNumber(value)); };
            break;
        }
        case PanelAction::NumberOff: {
            const std::string name = hit.key;
            change = [name](JsonValue& root) { root.set(name, JsonValue::makeNumber(0.0)); };
            break;
        }
        case PanelAction::CalibrationReset:
            change = [](JsonValue& root) {
                const JsonValue* current = root.get(key::kCalibrationReset);
                const double count = current != nullptr && current->isNumber() ? current->number : 0.0;
                root.set(key::kCalibrationReset, JsonValue::makeNumber(std::floor(count) + 1, true));
            };
            break;
        case PanelAction::ScaleAuto: {
            const bool left = !view.locked(key::kLidScaleLeft);
            const bool right = !view.locked(key::kLidScaleRight);
            change = [left, right](JsonValue& root) {
                if (left) root.set(key::kLidScaleLeft, JsonValue::makeNull());
                if (right) root.set(key::kLidScaleRight, JsonValue::makeNull());
            };
            break;
        }
        case PanelAction::ScaleFixed: {
            // Start from the scales in use, so the eyelids don't jump
            const SettingSpec* spec = findSetting(key::kLidScaleLeft);
            double values[2];
            for (int eye = 0; eye < 2; ++eye) {
                const double scale = model.status.running ? model.status.scales.v[eye] : NAN;
                values[eye] = roundToDecimals(*spec, std::isfinite(scale) ? scale : 1.0);
            }
            const bool left = !view.locked(key::kLidScaleLeft);
            const bool right = !view.locked(key::kLidScaleRight);
            change = [left, right, values](JsonValue& root) {
                if (left) root.set(key::kLidScaleLeft, JsonValue::makeNumber(values[0]));
                if (right) root.set(key::kLidScaleRight, JsonValue::makeNumber(values[1]));
            };
            break;
        }
        case PanelAction::PrefixFt:
            change = [](JsonValue& root) { root.set(key::kPrefix, JsonValue::makeString("/FT")); };
            break;
        case PanelAction::PrefixNone:
            change = [](JsonValue& root) { root.set(key::kPrefix, JsonValue::makeString("")); };
            break;
        case PanelAction::Language: {
            const std::string code = hit.arg == 1 ? "en" : "ja";
            change = [code](JsonValue& root) { root.set(key::kLanguage, JsonValue::makeString(code)); };
            break;
        }
        case PanelAction::PromptYes: {
            const std::vector<SettingChange> changes =
                recommendedSettings(outputOfArg(hit.arg), view);
            change = [changes](JsonValue& root) {
                for (const SettingChange& item : changes) root.set(item.key, item.value);
            };
            break;
        }
        case PanelAction::ResetAll: reset = true; break;
        case PanelAction::UpdateCheck:
            std::fprintf(stderr, "[update] check now\n");
            if (updater != nullptr) updater->checkNow();
            return;
        case PanelAction::UpdateInstall: {
            // "Update", or "Try again" after a failed install
            const frame_updater::UpdateStatus& u = model.update;
            if (u.state == frame_updater::UpdateState::Available && u.installable) {
                panel.showUpdatePrompt(u.latest, u.backToStable);
            } else if (u.state == frame_updater::UpdateState::InstallFailed) {
                // Trying again to go back: the version tried is below the running one
                const std::string target = u.version.empty() ? u.latest : u.version;
                panel.showUpdatePrompt(target, frame_updater::compareVersions(target, u.current) < 0);
            }
            return;
        }
        case PanelAction::UpdateConfirm:
            std::fprintf(stderr, "[update] installing %s (log: %s)\n", model.update.latest.c_str(),
                         updater != nullptr ? updater->logPath().c_str() : "-");
            if (updater != nullptr && !updater->install()) std::fprintf(stderr, "[update] could not start it\n");
            return;
        case PanelAction::UpdateCancel: std::fprintf(stderr, "[update] install: no\n"); return;
        case PanelAction::UpdateDismiss:
            if (updater != nullptr) updater->dismiss();
            return;
        case PanelAction::UpdateBeta:
            // Switched in the panel; the loop saves it to panel.json
            std::fprintf(stderr, "[update] test versions %s\n", panel.updateBeta() ? "on" : "off");
            if (updater != nullptr) updater->setChannel(panel.updateBeta() ? "beta" : "stable");
            return;
        case PanelAction::FitStart:
        case PanelAction::FitCenter: {
            const bool full = hit.action == PanelAction::FitStart;
            std::fprintf(stderr, "[fit] %s: started from the panel\n", full ? "eye fit" : "re-wear fit");
            if (fit != nullptr) startFit(*fit, full ? gaze_fit::Mode::Full : gaze_fit::Mode::Center, view, vr);
            return;
        }
        case PanelAction::FitDetails:
        case PanelAction::FitDetailsPage:
        case PanelAction::HostKey:
        case PanelAction::HostCancel:
        case PanelAction::HistoryClose:
        case PanelAction::DiagClose:
        case PanelAction::HistoryRow:
        case PanelAction::HistoryScroll:
        case PanelAction::AdvancedScroll:
        case PanelAction::EyecamStart:
        case PanelAction::NumberSlider:                // the loop writes the slider's value (SetNumber)
        case PanelAction::EyecamSensitivity: return;  // the loop sends the slider's value
        case PanelAction::SetupBegin: return;  // handled inside the panel
        case PanelAction::SetupProceed:
            std::fprintf(stderr, "[setup] on to the eye cameras' page\n");
            model.eyecam.flow.proceed();
            return;
        case PanelAction::SetupKonsole:
        case PanelAction::SetupVideo:
        case PanelAction::OpenModulePage: {
            // Konsole with the command typed in (the user presses Enter and types the password: the panel never runs
            // sudo or passwd), or the video or the module's page in Chromium. Only in VR: --dump-png (vr null) only
            // says what it would run
            const std::vector<std::string> argv =
                hit.action == PanelAction::SetupVideo       ? setup_tools::videoArgv(setup_tools::kVideoUrl)
                : hit.action == PanelAction::OpenModulePage ? setup_tools::videoArgv(setup_tools::kModulePageUrl)
                                                            : setup_tools::setupKonsoleArgv(hit.arg, model.language);
            std::string line;
            for (const std::string& arg : argv) line += (line.empty() ? "" : " | ") + arg;
            if (vr == nullptr) {
                std::printf("setup: would start %s\n", line.c_str());
                return;
            }
            // The module link's failure shows under the link on the Output tab, the setup's on the setup's screens
            const bool modulePage = hit.action == PanelAction::OpenModulePage;
            const char* const tag = modulePage ? "[output]" : "[setup]";
            std::string error;
            if (setup_tools::spawnDetached(argv, error)) {
                std::fprintf(stderr, "%s started %s\n", tag, line.c_str());
                noteSpawnResult(model, modulePage, "");
            } else {
                std::fprintf(stderr, "%s couldn't start %s: %s\n", tag, argv[0].c_str(), error.c_str());
                noteSpawnResult(model, modulePage, modulePage ? error : argv[0] + ": " + error);
            }
            return;
        }
        case PanelAction::HistoryOpen:
            loadHistory(model);
            return;
        case PanelAction::GazeScaleLink: {
            // Closed again: the second value takes the first (right := left, down := up)
            std::fprintf(stderr, "[action] gaze scale %s %s\n", hit.arg == 0 ? "left / right" : "up / down",
                         panel.gazeScaleLinked(hit.arg) ? "linked" : "unlinked");
            if (!panel.gazeScaleLinked(hit.arg)) return;
            const std::vector<SettingChange> changes = gazeScaleRelink(view, hit.arg);
            if (changes.empty()) return;
            change = [changes](JsonValue& root) {
                for (const SettingChange& c : changes) root.set(c.key, c.value);
            };
            break;
        }
        case PanelAction::AdvancedPage:
        case PanelAction::GazePage:
        case PanelAction::LidsPage:
        case PanelAction::RecordBack: return;  // handled inside the panel
        case PanelAction::AppsClose:
        case PanelAction::AppsInstall:
        case PanelAction::AppsMenu:
        case PanelAction::AppsScroll:
        case PanelAction::AppsCancel: return;  // handled inside the panel
        case PanelAction::AppsOpen:
        case PanelAction::AppsLaunch: return;  // the loop: frame_apps::AppsManager (fetchIfDue, openInstaller)
        case PanelAction::PreviewOpen:
        case PanelAction::PreviewClose:
        case PanelAction::PreviewSource:
        case PanelAction::PreviewMirror:
        case PanelAction::PreviewCamera:
        case PanelAction::PreviewFreeze: return;  // handled inside the panel; the loop follows previewOpen
        case PanelAction::RecordOpen: openRecordView(model, panel.recordShown()); return;
        case PanelAction::RecordsAll: loadRecords(model); return;
        case PanelAction::DiagOpen:
            readSystem(model);
            std::fprintf(stderr, "[diag] opened: %s\n", diag::code(model).c_str());
            return;
        case PanelAction::HostEnter: {
            // Start from the IP address set now (a host name can only be changed in config.json)
            const std::string host = view.text(key::kHost);
            const bool ip = !host.empty() && host.find_first_not_of("0123456789.") == std::string::npos;
            panel.openHostEntry(ip ? host : "");
            return;
        }
        case PanelAction::HostOk: {
            const std::string host = panel.hostEntryText();
            const host_entry::HostError problem = host_entry::checkHost(host);
            if (problem != host_entry::HostError::None) {
                std::fprintf(stderr, "[action] typed host \"%s\" can't be used (%d)\n", host.c_str(),
                             static_cast<int>(problem));
                panel.setHostEntryError(hostErrorText(uiText(model.language), problem));
                return;
            }
            panel.closeHostEntry();
            std::fprintf(stderr, "[action] host typed: %s\n", host.c_str());
            change = [host](JsonValue& root) { root.set(key::kHost, JsonValue::makeString(host)); };
            break;
        }
        case PanelAction::EyecamBack:
            std::fprintf(stderr, "[eyecam] error dismissed: %s\n", model.eyecam.status.message.c_str());
            model.eyecam.flow.dismissError(model.eyecam.lastRun, model.eyecam.status);
            return;
        case PanelAction::EyecamChoose:
        case PanelAction::EyecamStop:
        case PanelAction::EyecamCalib:
        case PanelAction::EyecamRelearn:
        case PanelAction::EyecamLidVeto: {
            // One line to its socket ("start" or "start widen_nolight" from the light warning, "stop", a calibration,
            // "relearn", or the eyelid veto's switch, which then shows as status.json says); the reply is read by the
            // loop (eyecam::Control never waits)
            if (eyecamControl == nullptr) return;
            const std::string command =
                hit.action == PanelAction::EyecamStop      ? std::string("stop")
                : hit.action == PanelAction::EyecamRelearn ? std::string(eyecam::kRelearnCommand)
                : hit.action == PanelAction::EyecamLidVeto ? eyecam::lidVetoCommand(hit.arg != 0)
                : hit.action == PanelAction::EyecamCalib   ? eyecam::calibCommand(static_cast<eyecam::Calib>(hit.arg))
                                                           : eyecam::startCommand(static_cast<eyecam::StartChoice>(hit.arg));
            if (command.empty()) return;
            const std::string socket = model.eyecamDir + "/ctl.sock";
            std::fprintf(stderr, "[eyecam] sending \"%s\" to %s\n", command.c_str(), socket.c_str());
            if (!eyecamControl->send(socket, command, nowSeconds()) && !eyecamControl->busy()) {
                std::fprintf(stderr, "[eyecam] %s failed: %s\n", command.c_str(), eyecamControl->reply().error.c_str());
            } else if (hit.action == PanelAction::EyecamStop) {
                // A calibration stopped from here ends idle like one done: no result after it
                model.eyecam.flow.stopSent();
            }
            syncEyecamControl(model.eyecam, *eyecamControl);
            return;
        }
        case PanelAction::ContribClose:
        case PanelAction::ContribAgree:
        case PanelAction::ContribDelete:
        case PanelAction::ContribDeleteCancel: return;  // handled inside the panel
        case PanelAction::ContribOpen:
            // Read from the disk the first time (the loop reads it at its start and follows it from then on); each
            // opening starts afresh: the last recording's end and failure are not shown again
            if (contribSession != nullptr && !model.contrib.loaded) {
                contribSession->load(model.contrib, contrib::defaultHome());
            }
            model.contrib.justDone = false;
            model.contrib.failure = contrib::Failure();
            model.contrib.deleted = false;
            std::fprintf(stderr, "[contrib] opened (%s): %d recording(s) in %s\n",
                         hit.arg == 0 ? "record" : "recordings",
                         static_cast<int>(model.contrib.info.recordings.size()),
                         recorder::shortPath(model.contrib.dir).c_str());
            return;
        case PanelAction::ContribEyewear: {
            const contrib::Eyewear eyewear = static_cast<contrib::Eyewear>(hit.arg);
            if (contribSession == nullptr) {
                std::printf("contrib: would write the consent (eyewear %s)\n", contrib::eyewearName(eyewear));
                return;
            }
            if (contribSession->consent(model.contrib, eyewear, std::time(nullptr))) {
                std::fprintf(stderr, "[contrib] consent given (version %d, eyewear %s)\n", contrib::kConsentVersion,
                             contrib::eyewearName(eyewear));
            } else {
                std::fprintf(stderr, "[contrib] couldn't write the consent: %s\n", model.contrib.error.c_str());
            }
            return;
        }
        case PanelAction::ContribStart: {
            // protocol_contrib.txt to eyecam-rec's socket (its reply and its run are followed by the loop)
            if (contribSession == nullptr || eyecamControl == nullptr) {
                std::printf("contrib: would send \"start %s\"\n", contrib::kProtocol);
                return;
            }
            const std::string command = contribSession->startCommand(model.contrib, model.eyecam);
            if (command.empty()) {
                std::fprintf(stderr, "[contrib] can't start now\n");
                return;
            }
            // What the recording's section says that eyecam-rec's status doesn't
            static diag::FileHash recHash;
            contrib::Snapshot snapshot;
            snapshot.appVersion = FRAMEEYEOSC_VERSION;
            snapshot.eyecamRec = recHash.get(eyecam::libDir() + "/eyecam-rec");
            snapshot.widen = view.flag(key::kWiden);
            snapshot.cameraLids = view.flag(key::kCameraLids);
            const std::string socket = model.eyecamDir + "/ctl.sock";
            std::fprintf(stderr, "[contrib] sending \"%s\" to %s\n", command.c_str(), socket.c_str());
            const bool sent = eyecamControl->send(socket, command, nowSeconds());
            contribSession->started(model.contrib, model.eyecam, snapshot, command, unixNow());
            if (!sent && !eyecamControl->busy()) {
                std::fprintf(stderr, "[contrib] %s failed: %s\n", command.c_str(),
                             eyecamControl->reply().error.c_str());
                contribSession->replied(model.contrib, eyecamControl->reply());
            }
            syncEyecamControl(model.eyecam, *eyecamControl);
            return;
        }
        case PanelAction::ContribPack:
            if (contribSession == nullptr) {
                std::printf("contrib: would pack %s\n", model.contrib.dir.c_str());
                return;
            }
            if (contribSession->pack(model.contrib, std::time(nullptr))) {
                std::fprintf(stderr, "[contrib] packing %s\n", recorder::shortPath(model.contrib.dir).c_str());
            } else {
                std::fprintf(stderr, "[contrib] can't pack: %s\n", model.contrib.progress.error.c_str());
            }
            return;
        case PanelAction::ContribDeleteConfirm:
            if (contribSession == nullptr) {
                std::printf("contrib: would delete %s\n", model.contrib.dir.c_str());
                return;
            }
            if (contribSession->remove(model.contrib)) {
                std::fprintf(stderr, "[contrib] deleting %s\n", recorder::shortPath(model.contrib.dir).c_str());
            } else {
                std::fprintf(stderr, "[contrib] can't delete %s\n", model.contrib.dir.c_str());
            }
            return;
        case PanelAction::FitStop:
            std::fprintf(stderr, "[fit] stopped\n");
            if (fit != nullptr) fit->cancel();
            return;
        case PanelAction::FitReset: {
            std::vector<std::string> unlocked;
            for (const std::string& name : fitResetKeys()) {
                if (!view.locked(name)) unlocked.push_back(name);
            }
            change = [unlocked](JsonValue& root) {
                for (const std::string& name : unlocked) root.set(name, defaultValue(*findSetting(name)));
            };
            break;
        }
    }

    std::string error;
    bool ok = false;
    if (reset) {
        std::fprintf(stderr, "[action] reset all settings\n");
        ok = resetConfigFile(model.configPath, error);
    } else {
        std::fprintf(stderr, "[action] %d %s %d\n", static_cast<int>(hit.action), hit.key != nullptr ? hit.key : "-",
                     hit.arg);
        ok = updateConfigFile(model.configPath, change, error);
    }
    if (ok) {
        model.panelError.clear();
        model.panelErrorBroken = false;
        if (!openPrompt.empty()) panel.showPrompt(openPrompt);
        // The eye fit starts over: the up gains of the fits before don't count any more (not in --dump-png)
        if (hit.action == PanelAction::FitReset && fit != nullptr) {
            std::string stateError;
            if (!ui_state::writeUpGains(ui_state::defaultPath(), {}, stateError)) {
                std::fprintf(stderr, "[panel] %s\n", stateError.c_str());
            }
        }
    } else {
        std::fprintf(stderr, "[config] %s\n", error.c_str());
        model.panelError = error;
        model.panelErrorBroken = !model.config.error.empty();
    }
    model.config = readConfigFile(model.configPath);
    model.language = configLanguage(model.config);
}

/**
 * Where the eye fit's sound files go: a "sounds" folder next to the status file.
 * @param statusPath the status file
 * @return the folder
 */
std::string soundsDir(const std::string& statusPath) {
    const size_t slash = statusPath.find_last_of('/');
    return (slash == std::string::npos ? std::string(".") : statusPath.substr(0, slash)) + "/sounds";
}

/**
 * --play-sound: play one cue and wait for it (for listening while tuning).
 * @param options the command line
 * @return exit code
 */
int runPlaySound(const Options& options) {
    sounds::Cue cue = sounds::Cue::Pop;
    sounds::parse(options.playSound, cue);
    sounds::Player player;
    if (!player.init(soundsDir(options.statusPath)) || player.program().empty()) return 1;
    player.play(cue);
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        player.reap();
    }
    return 0;
}

/**
 * Where the single-instance lock lives ($XDG_RUNTIME_DIR, or /run/user/<uid>).
 * @return the path
 */
std::string lockFilePath() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] != '\0') return std::string(runtime) + "/frameeyeosc-panel.lock";
    const std::string userRuntime = "/run/user/" + std::to_string(::getuid());
    if (::access(userRuntime.c_str(), W_OK) == 0) return userRuntime + "/frameeyeosc-panel.lock";
    return "/tmp/frameeyeosc-panel-" + std::to_string(::getuid()) + ".lock";
}

/**
 * Take the single-instance lock and write our PID into it (the file stays open; the OS drops the lock on exit).
 * @param lockFd the open lock file if taken
 * @param holderPid the resident instance's PID if not taken (0 if unreadable)
 * @return true if taken (or locking is unavailable and we should just start)
 */
bool acquireInstanceLock(int& lockFd, pid_t& holderPid) {
    lockFd = -1;
    holderPid = 0;
    const std::string path = lockFilePath();
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        std::fprintf(stderr, "[start] can't open the lock file %s; starting without the single-instance check\n",
                     path.c_str());
        return true;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        const std::string pid = std::to_string(::getpid()) + "\n";
        if (::ftruncate(fd, 0) != 0 || ::pwrite(fd, pid.data(), pid.size(), 0) < 0) {
            std::fprintf(stderr, "[start] could not write the PID into the lock file\n");
        }
        lockFd = fd;
        return true;
    }
    // The resident instance may have just taken the lock and not written its PID yet
    for (int attempt = 0; attempt < 10 && holderPid <= 0; ++attempt) {
        char buffer[32] = {};
        const ssize_t n = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) holderPid = static_cast<pid_t>(std::atol(buffer));
        if (holderPid <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::close(fd);
    return false;
}

/**
 * Whether this process was started by systemd as frameeyeosc-panel.service. INVOCATION_ID alone is not enough:
 * the Frame's Steam client runs as steam.service and passes INVOCATION_ID on to what it launches, so the
 * cgroup is checked too.
 * @return true if started by our own unit
 */
bool startedByOwnService() {
    const char* invocation = std::getenv("INVOCATION_ID");
    if (invocation == nullptr || invocation[0] == '\0') return false;
    const std::string suffix = "/frameeyeosc-panel.service";
    std::ifstream cgroup("/proc/self/cgroup");
    std::string line;
    while (std::getline(cgroup, line)) {
        if (line.size() >= suffix.size() && line.compare(line.size() - suffix.size(), std::string::npos, suffix) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Log what the update checker found, once per change (not the "checking" flag alone).
 * @param u the status
 */
void logUpdate(const frame_updater::UpdateStatus& u) {
    static std::string last;
    std::string line;
    switch (u.state) {
        case frame_updater::UpdateState::Unknown: return;
        case frame_updater::UpdateState::UpToDate:
            line = "up to date (" + u.current + ")" + (u.testVersion.empty() ? "" : "; test version " + u.testVersion);
            break;
        case frame_updater::UpdateState::Available:
            line = (u.backToStable ? "can go back to the regular version " : "new release ") + u.latest +
                   (u.prerelease ? " (a test version)" : "") + (u.installable ? "" : " (not installable: " + u.reason + ")");
            break;
        case frame_updater::UpdateState::Installing: line = "installing " + u.version + ": " + u.step; break;
        case frame_updater::UpdateState::Installed: line = "installed " + u.version + "; reopen to use it"; break;
        case frame_updater::UpdateState::CheckFailed: line = "check failed (" + u.error + "): " + u.message; break;
        case frame_updater::UpdateState::InstallFailed: line = "install failed (" + u.error + "): " + u.message; break;
    }
    if (line == last) return;
    last = line;
    std::fprintf(stderr, "[update] %s\n", line.c_str());
}

/**
 * A small file's text (status.json), for the records' status.jsonl.
 * @param path the file
 * @return its text ("" if it can't be read or is over 256 KB)
 */
std::string readSmallFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::string text(256 * 1024 + 1, '\0');
    in.read(&text[0], static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(in.gcount()));
    return text.size() > 256 * 1024 ? std::string() : text;
}

/**
 * What a calibration or a fit runs under, for its record.
 * @param model the model (frameeyeosc's and eyecam-rec's status)
 * @param vr the connection to SteamVR (the dashboard)
 * @param recenter the put-on watcher (how long the eyes have been tracked)
 * @return the conditions
 */
report::Conditions runConditions(const PanelModel& model, const VrOverlay& vr, const auto_recenter::Watcher& recenter) {
    report::Conditions c;
    c.dashboardOpen = vr.dashboardVisible() ? 1 : 0;
    c.cameraFps = model.eyecam.visible ? eyecam::cameraFps(model.eyecam.status) : NAN;
    c.trackerRate = model.status.running ? model.status.trackerRate : NAN;
    c.trackerMissed = model.status.running ? model.status.missedRate : NAN;
    c.sincePutOnSec = recenter.trackedSec();
    c.offBeforeSec = recenter.offBeforeSec();
    return c;
}

/**
 * A short line from eyecam-rec's message: up to its first bracket ("両目の瞳がうまく見えなかった").
 * @param message the message
 * @return the start of it
 */
std::string briefOf(const std::string& message) {
    size_t cut = message.size();
    for (const char* mark : {"（", "(", "[", "。"}) cut = std::min(cut, message.find(mark));
    std::string brief = message.substr(0, cut);
    while (!brief.empty() && brief.back() == ' ') brief.pop_back();
    return brief;
}

/**
 * A run still going when the panel closes, for its record: stopped, "パネルを閉じた".
 * @param s the record's summary
 */
void fillClosedResult(report::Summary& s) {
    s.result = report::Result::Stopped;
    s.reason = s.brief = uiText(Language::Ja).recordPanelClosed;
    s.reasonEn = s.briefEn = uiText(Language::En).recordPanelClosed;
}

/**
 * How a calibration ended, for its record: from eyecam-rec's status after it (its message in both languages, one eye
 * on its earlier values as "partial"), why it never ran, or that eyecam-rec stopped answering. The panel's "Stop" is
 * "stopped", not a failure.
 * @param s the record's summary
 * @param es eyecam-rec's status after it
 * @param stopped the panel's "Stop" ended it
 * @param refused eyecam-rec's answer when it refused the command ("" = it took it)
 * @param silent eyecam-rec's status went stale for CalibWatch::kSilentSec (its status says nothing then)
 */
void fillCalibResult(report::Summary& s, const eyecam::Status& es, bool stopped, const std::string& refused,
                     bool silent) {
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    s.result = report::Result::Failed;
    if (!refused.empty()) {
        char text[512];
        std::snprintf(text, sizeof(text), ja.recordNoStartFormat, refused.c_str());
        s.reason = s.brief = text;
        std::snprintf(text, sizeof(text), en.recordNoStartFormat, refused.c_str());
        s.reasonEn = s.briefEn = text;
        return;
    }
    if (stopped) {
        s.result = report::Result::Stopped;
        s.reason = s.brief = ja.recordStopped;
        s.reasonEn = s.briefEn = en.recordStopped;
        return;
    }
    if (silent) {
        s.reason = s.brief = ja.recordCalibSilent;
        s.reasonEn = s.briefEn = en.recordCalibSilent;
        return;
    }
    const std::string messageEn = es.messageEn.empty() ? es.message : es.messageEn;
    s.reason = es.message;
    s.reasonEn = messageEn;
    if (es.state == eyecam::State::Error || es.state != eyecam::State::Idle) {
        if (s.reason.empty()) s.reason = ja.eyecamCalibErrorTitle;
        if (s.reasonEn.empty()) s.reasonEn = en.eyecamCalibErrorTitle;
        s.brief = briefOf(s.reason);
        s.briefEn = briefOf(s.reasonEn);
        return;
    }
    // Idle after it: done, or (this wear's) one eye on its earlier values, or neither eye
    const std::string& eye = es.calibFailedEye;
    if (s.kind == report::Kind::CalibWear && eye == "LR") {
        s.brief = briefOf(s.reason);
        s.briefEn = briefOf(s.reasonEn);
        return;
    }
    if (s.kind == report::Kind::CalibWear && (eye == "L" || eye == "R")) {
        s.result = report::Result::Partial;
        s.brief = briefOf(s.reason);
        s.briefEn = briefOf(s.reasonEn);
        return;
    }
    s.result = report::Result::Ok;
    if (std::isfinite(s.conditions.cameraFps)) {
        char text[64];
        std::snprintf(text, sizeof(text), ja.condCameraFormat, s.conditions.cameraFps);
        s.brief = text;
        std::snprintf(text, sizeof(text), en.condCameraFormat, s.conditions.cameraFps);
        s.briefEn = text;
    }
}

/**
 * Stay resident as a dashboard overlay. Waits for SteamVR, exits quietly when it quits.
 * If another instance runs, asks it to open its panel and exits (no VR_Init).
 * @param options the command line
 * @return exit code
 */
int runOverlay(const Options& options) {
    int lockFd = -1;
    pid_t holderPid = 0;
    if (!acquireInstanceLock(lockFd, holderPid)) {
        // Started by systemd (Restart=always) while another instance runs: exit without signalling, with the
        // code systemd does not restart on, so the panel does not pop open every 5 s
        if (startedByOwnService()) {
            std::fprintf(stderr, "[start] already running (PID %d); started by the service, so exiting\n",
                         static_cast<int>(holderPid));
            return kExitCodeUserQuit;
        }
        if (holderPid > 0 && ::kill(holderPid, SIGUSR1) == 0) {
            std::fprintf(stderr, "[start] already running (PID %d); opening its panel\n", static_cast<int>(holderPid));
            return 0;
        }
        std::fprintf(stderr, "[start] seems to be running already, but could not signal it (PID %d)\n",
                     static_cast<int>(holderPid));
        return 1;
    }

    PanelModel model;
    model.configPath = options.configPath;
    model.statusPath = options.statusPath;
    model.eyecamDir = options.eyecamDir;
    model.changelogDirs =
        options.changelogDir.empty() ? changelog::defaultDirs() : std::vector<std::string> {options.changelogDir};
    model.config = readConfigFile(model.configPath);
    model.language = configLanguage(model.config);
    std::fprintf(stderr, "[start] config %s, status %s\n", model.configPath.c_str(), model.statusPath.c_str());
    if (!model.config.error.empty()) std::fprintf(stderr, "[config] broken: %s\n", model.config.error.c_str());
    migrateConfig(model);
    model.system.steamos = diag::readSteamos();
    FontSet fonts;
    fonts.load(kFontPath, kBoldFontPath);
    EyePanel panel(fonts);
    // The Advanced and Eyelids tabs' sub-tabs open last time (their own file: config.json isn't touched for them), and
    // the records
    const std::string uiStatePath = ui_state::defaultPath();
    AdvPage savedPage = ui_state::readPage(uiStatePath);
    panel.setAdvPage(savedPage);
    LidsPage savedLidsPage = ui_state::readLidsPage(uiStatePath);
    panel.setLidsPage(savedLidsPage);
    GazePage savedGazePage = ui_state::readGazePage(uiStatePath);
    panel.setGazePage(savedGazePage);
    // The gaze scale's locks as left, but open where the two values differ (a closed lock means they are the same)
    bool savedLinks[2] = {true, true};
    ui_state::readGazeScaleLinks(uiStatePath, savedLinks);
    for (int pair = 0; pair < 2; ++pair) {
        savedLinks[pair] = gazeScaleLinkShown(savedLinks[pair], model.config.number(kGazeScaleKeys[pair][0]),
                                              model.config.number(kGazeScaleKeys[pair][1]));
        panel.setGazeScaleLinked(pair, savedLinks[pair]);
    }
    model.records.dir = options.reportsDir;
    AutostartWorker autostart;
    autostart.start();
    // "Get test versions", as left
    bool savedBeta = ui_state::readUpdateBeta(uiStatePath);
    panel.setUpdateBeta(savedBeta);
    frame_updater::UpdateChecker updater(updaterConfig(savedBeta));
    std::fprintf(stderr, "[update] frameeyeosc-panel %s, checking %s, %s channel\n", FRAMEEYEOSC_VERSION,
                 model.config.flag(key::kUpdateCheck) ? "on" : "off", savedBeta ? "beta" : "stable");
    // The other Frame apps (the bottom of "Version"): which are installed and run, and their list from the site, fetched
    // only while update checks are on (at most hourly, shared by the apps; else the last one or the built-in one)
    frame_apps::AppsManager apps(appsManagerConfig());
    apps.refreshNow();
    apps.setFetchEnabled(model.config.flag(key::kUpdateCheck));
    if (apps.fetchIfDue()) std::fprintf(stderr, "[apps] fetching the list of the other Frame apps\n");
    model.apps.entries = apps.entries();
    model.apps.menuBusy = apps.busy("");
    std::uint64_t drawnApps = apps.revision();
    std::string lastAppsError;
    VrOverlay vr;

    // Wait for SteamVR
    std::string lastMessage;
    while (!gStopRequested) {
        std::string message;
        const VrOverlay::ConnectResult result = vr.connect(panel.width(), panel.height(), message);
        if (result == VrOverlay::ConnectResult::Ok) break;
        if (message != lastMessage) {
            if (result == VrOverlay::ConnectResult::NotRunning) {
                std::fprintf(stderr, "[VR] SteamVR is not running; retrying every 3 s\n");
            } else {
                std::fprintf(stderr, "[VR] connect failed: %s (retrying in 3 s)\n", message.c_str());
            }
            lastMessage = message;
        }
        sleepInterruptible(3.0);
        if (gShowRequested) {
            gShowRequested = 0;
            std::fprintf(stderr, "[start] asked to open the panel, but not connected to SteamVR\n");
        }
    }
    if (gStopRequested) {
        autostart.stop();
        if (lockFd >= 0) ::close(lockFd);
        return 0;
    }
    std::fprintf(stderr, "[VR] connected to SteamVR\n");
    {
        std::vector<uint8_t> thumbnail;
        renderThumbnail(fonts, kThumbnailSize, thumbnail);
        vr.submitThumbnail(thumbnail.data(), kThumbnailSize);
        // Give the panel a first image right away (no empty frame the first time it is chosen)
        model.status = readStatus(model.statusPath, unixNow());
        panel.render(model);
        vr.submitPanel(panel.toRgba().data());
        vr.logOverlayState("after connecting");
    }

    // Requests from `frameeyeosc-panel --action` (action_socket.h), taken from now on: the actions need SteamVR (the
    // targets, the dashboard), so before this the command finds no socket and says the panel isn't running
    action_socket::Server actionServer;
    // ...and "relearn" on eyecam-rec's control socket for them (a connection of its own, so the Eye cameras tab's
    // replies stay its own), answered once eyecam-rec has
    eyecam::Control relearnControl;
    int relearnRequest = 0;  // the request waiting for it (0: none)
    {
        std::string error;
        const std::string path = action_socket::defaultSocketPath();
        if (actionServer.open(path, error)) {
            std::fprintf(stderr, "[action] listening on %s\n", path.c_str());
        } else {
            std::fprintf(stderr, "[action] not listening: %s\n", error.c_str());
        }
    }

    gaze_fit::Session fit;
    // The records of calibrations and eye fits (report.h): each run's start and its status a second, then the logs
    // read and written on the writer's own thread after it
    report::Writer writer(model.records.dir, report::systemSources());
    report::Run fitRun;
    report::Run calibRun;
    std::string fitTrigger = "panel";  // "auto" for the re-wear fit started by itself, "command" for --action
    bool fitRetryShown = false;        // the last fit was a re-wear fit the dashboard stopped, to be tried again
    std::string calibCommand;          // the calibration's command, to know its reply
    report::CalibWatch calibWatch;     // ...until it ends
    double lastRunSample = 0.0;
    bool recordsWereShown = false;     // "Having trouble" (or all records) showed at the last loop
    // The eye log (frameeyeosc --record as a child), and what the panel last drew of it
    recorder::Recorder eyeLog;
    std::string drawnRecording;
    // ...and eyecam's drift log alongside it, on a control socket connection of its own
    recorder::DriftLink driftLink;
    eyecam::Control driftControl;
    // Re-centering by itself when the headset is put on (auto_recenter); armed now for the first wearing
    auto_recenter::Watcher recenter;
    long long lastCaptureId = 0;
    // The eye fit's sound cues: files written once now, played while the fit runs (if fit_sounds is on)
    sounds::Player player;
    player.init(soundsDir(model.statusPath));
    sounds::FitCues cues;
    TargetPainter targetPainter;
    bool targetUp = false;          // shown this loop: then the loop waits for the compositor's next frame
    int targetFrames = 0;           // frames since the target came up, to log the rate...
    int targetDraws = 0;            // ...and how many of them drew a new picture
    double targetShownAt = 0.0;
    double loggedYaw = NAN;         // where the target was last logged as settled
    double loggedPitch = NAN;
    std::string lastFitState;
    // The developer tab "Eye capture": eyecam-rec's status file and control socket, and the full-view light during its
    // bright and dark steps (the picture drawn, and the one the overlay has)
    eyecam::Control eyecamControl;
    eyecam::SensitivitySender sensitivitySender;  // the widening sensitivity slider's value on its way
    setup_tools::PasswordCheck passwordCheck;     // the eye cameras' setup: is there a SteamOS password
    double lastEyecamRead = -1e9;
    std::string drawnEyecam;
    std::string loggedEyecam;
    std::vector<uint8_t> fillImage;
    std::string fillDrawn;          // what fillImage shows ("bright|<text>"), "" before the first
    std::string fillSent;           // what the overlay was last sent, "" to send again
    eyecam::Light light;            // the light as shown: which one, and its alpha as it fades in and out
    eyecam::Fill lightWanted = eyecam::Fill::None;  // the one fillFor wanted last time (to log a fade-out once)
    bool lightMoving = false;       // fading in or out: the loop runs every display frame
    // "Record for squint training": its folder in ~/eyecam
    contrib::Session contribSession;
    contribSession.load(model.contrib, contrib::defaultHome());
    if (model.contrib.exists) {
        std::fprintf(stderr, "[contrib] %s: %d recording(s)\n", recorder::shortPath(model.contrib.dir).c_str(),
                     static_cast<int>(model.contrib.info.recordings.size()));
    }
    // The debug gaze dots: their socket, the three dot images and the three ring images after them (drawn once), which
    // image each overlay has (0 and 1 the dots, 2 and 3 the rings: the gaze before the user's gaze scale)
    gaze_dots::Receiver dots;
    std::vector<uint8_t> dotImages[6];
    for (int kind = 0; kind < 3; ++kind) {
        renderGazeDot(static_cast<DotKind>(kind), dotImages[kind]);
        renderGazeRing(static_cast<DotKind>(kind), dotImages[3 + kind]);
    }
    constexpr int kDotOverlays = VrOverlay::kDots;
    int dotImageShown[kDotOverlays] = {-1, -1, -1, -1};
    bool dotsVisible[kDotOverlays] = {false, false, false, false};
    bool dotsOpenFailed = false;
    bool dotsWanted = false;        // the dots are on (their overlays shown); the socket may be open for the preview too
    // The eye preview: frameeyeosc's samples come on the dots' socket (bound while its sheet shows), eyecam-rec's
    // pictures from its preview file, asked for with "preview on" every second while they show (a control socket
    // connection of their own, so the eye cameras tab's replies stay its own)
    eye_preview::PictureReader previewReader;
    eye_preview::Asker previewAsker;
    eyecam::Control previewControl;
    bool previewShown = false;      // the sheet showed at the last loop
    double lastPreviewSample = -1e9;
    std::string previewLogged;      // the last line logged about the pictures (each said once)
    bool previewOnSent = false;     // "preview on" went out (and no "off" since), for the log
    eye_preview::DrawPacer previewPacer;  // the sheet drawn for its samples and pictures about 30 times a second
    double lastDotAt = 0.0;
    double dotIpd = gaze_fit::kDefaultIpdM;
    double dotDistance = 0.0;       // how far along the rays the dots are (gaze_debug_dots_distance_m), 0 until set
    uint64_t drawnAutostart = autostart.snapshot(model.autostart);
    uint64_t drawnUpdate = 0;
    std::string lastSignature;
    std::string drawnDiag;          // what the diagnostics page showed last
    std::string drawnCode;          // the diagnostic code the Advanced tab showed last
    double lastDiagRead = -1e9;
    std::string lastStamp = configStamp(model.configPath);
    bool lastRunning = false;
    double lastStatusRead = -1e9;
    bool dirty = true;
    bool wasVisible = false;
    bool firstSubmit = true;
    bool calmDrawn = false;         // the Eye fit tab's running view was drawn (it then stays still, see below)
    bool userQuit = false;
    const auto submitRecord = [&](report::Pending pending) {
        report::Summary& s = pending.summary;
        s.version = FRAMEEYEOSC_VERSION;
        s.steamos = model.system.steamos;
        s.code = diag::code(model);
        pending.language = model.language;
        std::fprintf(stderr, "[report] %s %s (%.1f s): writing its record\n", report::kindName(s.kind),
                     report::resultName(s.result), s.end - s.start);
        writer.submit(std::move(pending));
    };
    // A calibration's end, from eyecam-rec's status now
    const auto finishCalib = [&](const std::string& refused) {
        report::Pending pending = calibRun.finish(unixNow());
        fillCalibResult(pending.summary, model.eyecam.status, calibWatch.stopped(), refused, calibWatch.silent());
        calibWatch.end();
        submitRecord(std::move(pending));
    };
    while (!gStopRequested && !userQuit) {
        if (gShowRequested) {
            gShowRequested = 0;
            std::fprintf(stderr, "[start] a second launch asked to open the panel\n");
            vr.showPanel();
        }
        const VrEvents events = vr.pollEvents();
        // SteamVR quitting exits with 0; the dashboard's "close" is the user quitting, like "Quit" (code 3)
        if (events.quit) break;
        if (events.closeRequested) {
            std::fprintf(stderr, "[VR] quitting from the dashboard's \"close\"\n");
            userQuit = true;
            break;
        }
        if (!vr.steamVrAlive()) {
            std::fprintf(stderr, "[VR] vrserver is gone; quitting\n");
            break;
        }

        // Requests from `--action`: taken in now, answered below once the files are read (read for them even when
        // nothing else reads them, so the answer goes by the state now)
        const std::vector<action_socket::Request> requests = actionServer.poll(nowSeconds());
        for (const std::string& line : actionServer.takeLog()) std::fprintf(stderr, "[action] %s\n", line.c_str());

        // Files are only read while the panel is visible, or while an eye fit runs (the dashboard open or closed)
        const bool visible = vr.panelVisible();
        autostart.setActive(visible);
        const bool fitting = fit.active();
        // (also while the debug dots are on, so their switch and distance apply without opening the dashboard; and
        // every kWatchStatusReadSec while there is a fit to re-center when the headset is put on)
        const bool often = visible || fitting || dots.isOpen();
        const bool watching =
            autoRecenter(model.config) != AutoRecenter::Off && fitInConfig(SettingsView(model)).gazeFitted;
        const double readEvery = often ? kStatusReadSec : kWatchStatusReadSec;
        const bool asked = !requests.empty();
        if ((often || watching || asked) &&
            (asked || (visible && !wasVisible) || nowSeconds() >= lastStatusRead + readEvery)) {
            lastStatusRead = nowSeconds();
            model.status = readStatus(model.statusPath, unixNow());
            if (model.status.running != lastRunning) {
                if (model.status.running) {
                    std::fprintf(stderr, "[status] frameeyeosc is running (PID %d)\n", model.status.pid);
                } else {
                    std::fprintf(stderr, "[status] frameeyeosc is not running (%s)\n",
                                 model.status.present ? "stale status" : model.status.readError.c_str());
                }
                lastRunning = model.status.running;
            }
            const std::string signature = statusSignature(model.status);
            if (signature != lastSignature) {
                lastSignature = signature;
                dirty = true;
            }
            const std::string stamp = configStamp(model.configPath);
            if (stamp != lastStamp) {
                model.config = readConfigFile(model.configPath);
                // An old file put back (a backup, say) is brought up to date the same way
                migrateConfig(model);
                lastStamp = configStamp(model.configPath);
                model.language = configLanguage(model.config);
                // The version history follows the language
                if (panel.historyOpen() && model.history.japanese != (model.language == Language::Ja)) {
                    loadHistory(model);
                }
                dirty = true;
            }
            // The module link's failure goes with the output it was for (changed in the file or on the command line)
            if (!model.moduleLinkError.empty() && SettingsView(model).text(key::kOutput) != kOutputModule) {
                model.moduleLinkError.clear();
                dirty = true;
            }
        }

        bool pointerDirty = false;  // the pointer changed something on the panel (hover, a press)
        for (const PointerInput& input : events.pointer) {
            bool changed = false;
            switch (input.type) {
                case PointerInput::Type::Move: changed = panel.pointerMove(input.x, input.y); break;
                case PointerInput::Type::Down: {
                    const PanelTab tabBefore = panel.tab();
                    const PanelHit hit = panel.pointerDown(input.x, input.y, nowSeconds());
                    // Leaving the tab stops a fit (the tab bar is the one thing left to press besides "Stop")
                    if (panel.tab() != tabBefore && fit.active()) {
                        std::fprintf(stderr, "[fit] stopped: another tab chosen\n");
                        fit.cancel(gaze_fit::Failure::Left);
                    }
                    if (hit.action == PanelAction::Quit) {
                        std::fprintf(stderr, "[VR] quitting from the panel's \"Quit\"\n");
                        userQuit = true;
                    } else {
                        applyHit(hit, model, panel, autostart, &updater, &fit, &vr, &eyeLog, &eyecamControl,
                                 &contribSession);
                        lastStamp = configStamp(model.configPath);
                        // The other apps' list opened: looked at now, and fetched if it is due
                        if (hit.action == PanelAction::AppsOpen) {
                            apps.refreshNow();
                            if (apps.fetchIfDue()) std::fprintf(stderr, "[apps] list opened: fetching it from the site\n");
                        }
                        // ...and "Open in Konsole" on its confirmation: the installer, as typed by hand
                        if (hit.action == PanelAction::AppsLaunch) {
                            const std::string key = panel.appsConfirmKey();
                            const frame_apps::LaunchResult result = apps.openInstaller(
                                key, model.language == Language::Ja ? frame_apps::Lang::Ja : frame_apps::Lang::En);
                            panel.setAppsLaunchResult(result);
                            std::fprintf(stderr, "[apps] Konsole with the installer (%s): %s\n",
                                         key.empty() ? "menu" : key.c_str(),
                                         result == frame_apps::LaunchResult::Started ? "opened" : "couldn't open");
                        }
                        // A calibration sent to eyecam-rec: its record starts (one still open ends first)
                        if (hit.action == PanelAction::EyecamCalib && eyecamControl.busy()) {
                            if (calibRun.active()) finishCalib("");
                            const bool user = static_cast<eyecam::Calib>(hit.arg) == eyecam::Calib::User;
                            calibRun.begin(user ? report::Kind::CalibUser : report::Kind::CalibWear, unixNow());
                            calibRun.summary().conditions = runConditions(model, vr, recenter);
                            calibCommand = eyecamControl.command();
                            calibWatch.start(nowSeconds());
                            model.records.lastCalib.clear();
                        }
                        if (hit.action == PanelAction::EyecamStop) calibWatch.stop();
                    }
                    changed = true;
                    break;
                }
                case PointerInput::Type::Up: changed = panel.pointerUp(); break;
                case PointerInput::Type::Leave: changed = panel.pointerLeave(); break;
                case PointerInput::Type::Scroll: changed = panel.scroll(input.y); break;
            }
            dirty |= changed;
            pointerDirty |= changed;
        }
        if (userQuit) break;
        // The thumbstick scrolls the panel only while the version history, or a taller Advanced tab, is shown
        vr.setPanelScroll(panel.wantsScroll());
        dirty |= panel.tick(nowSeconds());
        const uint64_t autostartVersion = autostart.snapshot(model.autostart);
        if (autostartVersion != drawnAutostart) {
            drawnAutostart = autostartVersion;
            dirty = true;
        }
        // The other Frame apps: a Konsole closing, systemctl's answer and the list's download reaped; while they are on
        // screen, looked at again every few seconds
        apps.setFetchEnabled(model.config.flag(key::kUpdateCheck));
        if (visible && panel.appsShown()) apps.refreshIfStale(kAppsRefreshSec);
        if (apps.tick() || apps.revision() != drawnApps) {
            drawnApps = apps.revision();
            model.apps.entries = apps.entries();
            model.apps.menuBusy = apps.busy("");
            dirty = true;
        }
        if (apps.lastError() != lastAppsError) {
            lastAppsError = apps.lastError();
            if (!lastAppsError.empty()) std::fprintf(stderr, "[apps] list: %s\n", lastAppsError.c_str());
        }
        // Checks at start and then hourly (frame-update.sh reuses GitHub's answer for 55 minutes), even while closed
        updater.tick(model.config.flag(key::kUpdateCheck));
        if (updater.revision() != drawnUpdate) {
            drawnUpdate = updater.revision();
            model.update = updater.status();
            logUpdate(model.update);
            dirty = true;
        }

        // The eye log: reaped when it ends, stopped at its limit; redrawn when its time or state changes
        {
            eyeLog.poll(nowSeconds());
            model.recording = eyeLog.view(nowSeconds());
            const recorder::View& r = model.recording;
            const std::string shown = (r.recording ? recorder::elapsedText(r.elapsedSec) : std::string("-")) + r.error;
            // eyecam's drift log alongside it while the eye cameras are in use (see recorder::DriftLink)
            std::string command;
            if (driftLink.next(driftInputs(r, model), driftControl.busy(), command)) {
                sendDrift(driftControl, model.eyecamDir, command, eyeLog);
            }
            if (driftControl.poll(nowSeconds())) driftReplied(driftControl.reply(), eyeLog);
            if (shown != drawnRecording) {
                drawnRecording = shown;
                dirty = true;
            }
        }

        const bool dashboardOpen = vr.dashboardVisible();
        // Re-centering by itself once the headset is put on and the eyes have settled: the same one-dot fit as
        // "Re-center only", once per wearing (see auto_recenter.h)
        {
            const SettingsView view(model);
            auto_recenter::Inputs in;
            in.enabled = autoRecenter(model.config) != AutoRecenter::Off;
            in.fitted = fitInConfig(view).gazeFitted;
            in.locked = fitKeysLocked(view);
            in.running = model.status.running;
            in.tracking = model.status.tracking;
            in.vrConnected = vr.connected();
            in.dashboardOpen = dashboardOpen;
            in.fitActive = fit.active();
            // An eye Valve's eye server reads shut (stuck at 0 for a whole wear at times) counts only with the eye
            // camera's eyelid, which waits for eyecam-rec's warm-up after the headset is put on (and never comes with
            // the cameras switched off)
            for (int eye = 0; eye < 2; ++eye) {
                in.valveShut[eye] = model.status.hasRaw && model.status.openness.v[eye] < auto_recenter::kOpenFloor;
            }
            in.camerasWarming = eyecam::recenterAwaitsWarmup(model.eyecam, view.flag(key::kCameraLids));
            in.camerasCalibrating = eyecam::calibrationUnderWay(model.eyecam);
            // On a clock that runs through suspend: taking the headset off usually lets the Frame sleep
            const auto_recenter::Step step = recenter.update(bootSeconds(), in);
            if (!step.log.empty()) std::fprintf(stderr, "[fit] %s\n", step.log.c_str());
            if (step.start) {
                fitTrigger = "auto";
                startFit(fit, gaze_fit::Mode::Center, view, &vr);
            }
        }

        // ...and from `--action`, each as if its button were pressed, with the button's conditions (action_socket.h)
        for (const action_socket::Request& request : requests) {
            using action_socket::Effect;
            const SettingsView view(model);
            action_socket::State state;
            state.running = model.status.running;
            state.tracking = model.status.tracking;
            state.locked = fitKeysLocked(view);
            state.gazeFitted = fitInConfig(view).gazeFitted;
            state.fitActive = fit.active();
            state.fitIsRecenter = fit.view().mode == gaze_fit::Mode::Center;
            if (request.action == action_socket::Action::Relearn) {
                // eyecam-rec's status as it is now (it is read once a second while nothing runs)
                model.eyecam.status = eyecam::readStatus(model.eyecamDir);
                lastEyecamRead = nowSeconds();
                const bool lids = view.flag(key::kCameraLids);
                const bool warming = eyecam::baselineWarming(model.eyecam.status);
                state.camerasReady =
                    eyecam::relearnAllowed(eyecam::tabVisible(model.eyecam.status, unixNow()), model.eyecam.status, lids,
                                           relearnUse(cameraUse(model.status, lids, warming)));
            }
            // (eyecam-rec's status is read every kEyecamReadSec while it runs, and a calibration the panel sent counts
            // from the moment it went out)
            state.camerasCalibrating = eyecam::calibrationUnderWay(model.eyecam);
            action_socket::Decision decision = action_socket::decide(request.action, state);
            std::fprintf(stderr, "[action] %s (from PID %d): %s\n", action_socket::actionName(request.action),
                         request.peerPid, decision.text.c_str());
            switch (decision.effect) {
                case Effect::StartRecenter:
                case Effect::StartFit:
                    // With the dashboard open, onto this panel's Eye fit tab as if pressed there (its "Stop" shows,
                    // and another page showing would stop the fit). Closed, it stays closed: the targets show without
                    // it, as for the automatic re-center, and opening it stops the fit
                    if (dashboardOpen) {
                        panel.setTab(PanelTab::EyeFit);
                        if (!visible) vr.showPanel();
                        dirty = true;
                    }
                    fitTrigger = "command";
                    startFit(fit, decision.effect == Effect::StartFit ? gaze_fit::Mode::Full : gaze_fit::Mode::Center,
                             view, &vr);
                    break;
                case Effect::StopFit:
                    std::fprintf(stderr, "[fit] stopped\n");
                    fit.cancel();
                    break;
                case Effect::ShowPanel: vr.showPanel(); break;
                case Effect::Relearn: {
                    // Answered once eyecam-rec has (below), unless it can't even be asked
                    const std::string socket = model.eyecamDir + "/ctl.sock";
                    if (relearnRequest != 0 || relearnControl.busy()) {
                        decision = {action_socket::Code::NoCameras, "eye cameras not ready (a relearn is on its way)",
                                    Effect::None};
                    } else if (relearnControl.send(socket, eyecam::kRelearnCommand, nowSeconds(),
                                                   action_socket::kEyecamReplySec)) {
                        std::fprintf(stderr, "[eyecam] sending \"%s\" to %s (--action)\n", eyecam::kRelearnCommand,
                                     socket.c_str());
                        relearnRequest = request.id;
                        continue;
                    } else {
                        decision = action_socket::relearnAnswer(false, relearnControl.reply().error);
                    }
                    std::fprintf(stderr, "[action] relearn: %s\n", decision.text.c_str());
                    break;
                }
                case Effect::None: break;
            }
            actionServer.answer(request.id, decision.code, decision.text);
        }
        // ...and the relearn's, once eyecam-rec has answered (or kEyecamReplySec has gone by)
        if (relearnControl.poll(nowSeconds()) && relearnRequest != 0) {
            const eyecam::Reply& r = relearnControl.reply();
            const action_socket::Decision answer = action_socket::relearnAnswer(r.ok, r.error);
            std::fprintf(stderr, "[eyecam] %s -> %s%s; [action] relearn: %s\n", r.command.c_str(), r.ok ? "ok" : "err ",
                         r.error.c_str(), answer.text.c_str());
            actionServer.answer(relearnRequest, answer.code, answer.text);
            relearnRequest = 0;
            // (the Eye cameras tab's countdown follows from eyecam-rec's status, read again now)
            lastEyecamRead = -1e9;
        }

        // A fit started (from the panel, by itself or from --action): its record starts
        if (fit.active() && !fitRun.active()) {
            const gaze_fit::View started = fit.view();
            fitRun.begin(started.mode == gaze_fit::Mode::Full ? report::Kind::Fit : report::Kind::Recenter, unixNow());
            report::Summary& s = fitRun.summary();
            s.trigger = fitTrigger;
            s.mode = started.mode == gaze_fit::Mode::Full ? "full" : "center";
            s.conditions = runConditions(model, vr, recenter);
            s.conditions.ipdMm = vr.userIpdMeters() * 1000;
            model.records.lastFit.clear();
            fitRetryShown = false;
        }
        // A re-wear fit stopped because the dashboard opened anyway (auto_recenter waits for SteamVR's own startup
        // dashboard): auto_recenter tries again once it is closed, kDashboardRetries times at most in a wearing
        const auto stoppedByDashboard = [](const gaze_fit::View& v) {
            return v.phase == gaze_fit::Phase::Failed && v.failure == gaze_fit::Failure::DashboardOpened;
        };
        fitTrigger = "panel";

        // The eye fit: requests and results go through config.json and status.json; the target shows over the open
        // dashboard too (it is nearer)
        {
            gaze_fit::Dashboard dashboard;
            dashboard.open = dashboardOpen;
            dashboard.panelShown = visible;
            const gaze_fit::Actions actions = fit.tick(nowSeconds(), dashboard, model.status);
            // One "[fit]" line each (a try's numbers, and at the end the gains)
            for (size_t start = 0; start < actions.log.size();) {
                const size_t end = std::min(actions.log.find('\n', start), actions.log.size());
                std::fprintf(stderr, "[fit] %s\n", actions.log.substr(start, end - start).c_str());
                start = end + 1;
            }
            // Cues follow what the target shows (a dot arriving, the countdown, the eyes-shut ending)
            const bool soundsOn = model.config.flag(key::kFitSounds);
            // ...but no "failed" sound for a re-wear fit the dashboard stopped that will be tried again
            const bool retrying = fitRun.active() &&
                                  recenter.willRetry(fitRun.summary().trigger == "auto", stoppedByDashboard(fit.view()));
            for (sounds::Cue cue : cues.update(fit.view(), actions)) {
                if (soundsOn && !(retrying && cue == sounds::Cue::Fail)) player.play(cue);
            }
            player.reap();
            if (actions.writeCapture) {
                const long long id =
                    writeCaptureRequest(model, actions.target, actions.captureSec, actions.skipSec, lastCaptureId);
                if (id != 0) {
                    fit.captureSent(id, nowSeconds());
                } else {
                    fit.writeFailed();
                }
                lastStamp = configStamp(model.configPath);
            }
            if (actions.writeValues) {
                const gaze_fit::View done = fit.view();
                if (!writeFitValues(model, actions.values, done.mode)) {
                    fit.writeFailed();
                } else if (done.mode == gaze_fit::Mode::Full && std::isfinite(done.notes.gainUp)) {
                    // This fit's up gain joins the ones the next whole fit averages
                    std::string error;
                    const std::vector<double> gains =
                        gaze_fit::nextUpGains(ui_state::readUpGains(uiStatePath), done.notes.gainUp);
                    if (!ui_state::writeUpGains(uiStatePath, gains, error)) {
                        std::fprintf(stderr, "[panel] %s\n", error.c_str());
                    }
                }
                lastStamp = configStamp(model.configPath);
            }
            targetUp = false;
            if (actions.showTarget) {
                // Every display frame, so the ring runs down and the dot glides smoothly; the picture is only drawn
                // and sent when it changed
                const std::string label = targetLabel(uiText(model.language), actions.style);
                const bool drawn =
                    targetPainter.paint(fonts, actions.style, label, actions.seconds, actions.progress);
                targetUp = vr.showTarget(actions.yawDeg, actions.pitchDeg,
                                         drawn ? targetPainter.rgba().data() : nullptr, kTargetImageSize);
                if (drawn && !targetUp) targetPainter.reset();
                if (targetFrames++ == 0) targetShownAt = nowSeconds();
                if (drawn) ++targetDraws;
                if (actions.arrived && (actions.yawDeg != loggedYaw || actions.pitchDeg != loggedPitch)) {
                    std::fprintf(stderr, "[VR] target at %.0f deg right, %.0f deg up\n", actions.yawDeg,
                                 actions.pitchDeg);
                    loggedYaw = actions.yawDeg;
                    loggedPitch = actions.pitchDeg;
                }
            } else {
                vr.hideTarget();
                if (targetFrames > 0) {
                    const double seconds = nowSeconds() - targetShownAt;
                    std::fprintf(stderr, "[fit] target up %.1f s: %d frames (%.0f per second), %d pictures drawn\n",
                                 seconds, targetFrames, targetFrames / std::max(seconds, 0.001), targetDraws);
                    targetFrames = 0;
                    targetDraws = 0;
                    loggedYaw = loggedPitch = NAN;
                    targetPainter.reset();
                }
            }
            const gaze_fit::View view = fit.view();
            char state[96];
            std::snprintf(state, sizeof(state), "phase %d, point %d/%d (%s), try %d, failure %d",
                          static_cast<int>(view.phase), view.index + 1, view.count,
                          gaze_fit::target(view.point).name, view.attempt, static_cast<int>(view.failure));
            if (state != lastFitState) {
                if (!lastFitState.empty() || view.phase != gaze_fit::Phase::Idle) std::fprintf(stderr, "[fit] %s\n", state);
                lastFitState = state;
                model.fit = view;
                dirty = true;
            }
        }
        // ...and ends with it (done, failed or stopped)
        if (!fit.active() && fitRun.active()) {
            const gaze_fit::View ended = fit.view();
            const bool byItself = fitRun.summary().trigger == "auto";
            const bool byDashboard = stoppedByDashboard(ended);
            const bool retry = recenter.willRetry(byItself, byDashboard);
            report::Pending pending = fitRun.finish(unixNow());
            fillFitResult(pending.summary, ended);
            if (retry) {
                // Still "stopped" (grey, not a failure), and its reason says it is tried again
                pending.summary.reason += std::string("。") + uiText(Language::Ja).fitRetryAfterDashboard;
                pending.summary.reasonEn += std::string(". ") + uiText(Language::En).fitRetryAfterDashboard;
            }
            submitRecord(std::move(pending));
            if (const std::string line = recenter.fitEnded(byItself, byDashboard); !line.empty()) {
                std::fprintf(stderr, "[fit] %s\n", line.c_str());
            }
            fitRetryShown = retry;
        }
        // The Eye fit tab says it is tried again while it still will be (not once the wearing's re-center is off)
        if (const bool retryShown = fitRetryShown && recenter.armed(); retryShown != model.fitRetry) {
            model.fitRetry = retryShown;
            dirty = true;
        }

        // The debug gaze dots: while the switch is on (and no eye fit runs), move a dot to where the gaze frameeyeosc
        // sends points, as each sample arrives. Only the transform changes; each dot's image is sent once. The socket
        // is also open while the eye preview shows (frameeyeosc sends while it is there), without the dots
        const bool previewing = visible && panel.previewOpen();
        {
            const bool wanted = model.config.flag(key::kGazeDebugDots) && !fit.active();
            dotsWanted = wanted;
            const bool listen = wanted || previewing;
            const auto hideDot = [&](int index) {
                vr.hideDot(index);
                dotsVisible[index] = false;
            };
            const auto hideDots = [&]() {
                for (int i = 0; i < kDotOverlays; ++i) hideDot(i);
            };
            const auto anyDot = [&]() {
                return std::any_of(std::begin(dotsVisible), std::end(dotsVisible), [](bool shown) { return shown; });
            };
            // One distance, dashboard open or closed (near enough to show over it; nothing jumps)
            const double wantedDistance = gaze_dots::dotDistance(model.config.number(key::kGazeDebugDotsDistanceM));
            if (dots.isOpen() && wanted && wantedDistance != dotDistance) {
                std::fprintf(stderr, "[dots] at %.2f m, %.1f mm wide (plain overlays, no sort order or flags)\n",
                             wantedDistance, gaze_dots::dotWidth(wantedDistance) * 1000);
                dotDistance = wantedDistance;
            }
            if (listen && !dots.isOpen() && !dotsOpenFailed) {
                const size_t slash = model.statusPath.find_last_of('/');
                const std::string dir = slash == std::string::npos ? "." : model.statusPath.substr(0, slash);
                std::string error;
                if (dots.open(dir + "/" + gaze_dots::kSocketName, error)) {
                    dotIpd = vr.userIpdMeters();
                    // Set the distance now, so a packet in this same pass isn't placed at 0 m with the default width
                    dotDistance = wantedDistance;
                    if (wanted) {
                        std::fprintf(stderr, "[dots] at %.2f m, %.1f mm wide (plain overlays, no sort order or flags)\n",
                                     wantedDistance, gaze_dots::dotWidth(wantedDistance) * 1000);
                    }
                    std::fprintf(stderr, "[dots] listening in %s (IPD %.1f mm)%s\n", dir.c_str(), dotIpd * 1000,
                                 wanted ? "" : " for the eye preview");
                } else {
                    std::fprintf(stderr, "[dots] %s\n", error.c_str());
                    dotsOpenFailed = true;
                }
            } else if (!listen && (dots.isOpen() || dotsOpenFailed)) {
                if (dots.isOpen()) std::fprintf(stderr, "[dots] off\n");
                dotDistance = 0.0;
                dots.close();
                hideDots();
                dotsOpenFailed = false;
            }
            // Listening for the eye preview alone: no dots. The rings switched off (gaze_debug_rings): no rings, at
            // once rather than at the next sample
            if (!wanted && anyDot()) hideDots();
            const bool wantRings = model.config.flag(key::kGazeDebugRings);
            if (!wantRings && (dotsVisible[2] || dotsVisible[3])) {
                hideDot(2);
                hideDot(3);
            }
            gaze_dots::Packet packet;
            const bool got = dots.isOpen() && dots.poll(packet);
            if (got && previewing) {
                model.preview.packet = packet;
                model.preview.hasPacket = true;
                lastPreviewSample = nowSeconds();
                previewPacer.changed();
            }
            if (got && wanted) {
                lastDotAt = nowSeconds();
                // A ring (index 2 or 3) is the same eye's dot, kRingWidthScale times as wide, with a clear middle
                const auto place = [&](int index, int eye, DotKind kind, float x, float y) {
                    const bool ring = index >= 2;
                    const gaze_dots::Pose pose = gaze_dots::dotPose(x, y, eye, dotIpd, dotDistance);
                    const int image = static_cast<int>(kind) + (ring ? 3 : 0);
                    const double width = gaze_dots::dotWidth(dotDistance) * (ring ? gaze_dots::kRingWidthScale : 1.0);
                    dotsVisible[index] = vr.showDot(index, pose.position.x, pose.position.y, pose.position.z,
                                                    pose.yawDeg, pose.pitchDeg, dotImages[image].data(),
                                                    kDotImageSize, dotImageShown[index] != image, width);
                    if (dotsVisible[index]) dotImageShown[index] = image;
                };
                // The dots show the gaze as sent; while the user's gaze scale is in use, the rings show it before
                // the scale (where the eyes look). Without it the two are the same, so no rings; nor with the rings
                // switched off (only the dots, as up to 0.7.7)
                const float* gazes[2] = {packet.gaze, packet.unscaledGaze};
                for (int ring = 0; ring < 2; ++ring) {
                    const float* gaze = gazes[ring];
                    const int at = ring * 2;
                    if (ring == 1 && (!packet.scaled || !wantRings)) {
                        hideDot(at);
                        hideDot(at + 1);
                    } else if (packet.independent) {
                        place(at, -1, DotKind::Left, gaze[0], gaze[1]);
                        place(at + 1, 1, DotKind::Right, gaze[2], gaze[3]);
                    } else {
                        place(at, 0, DotKind::Both, gaze[4], gaze[5]);
                        hideDot(at + 1);
                    }
                }
            } else if (anyDot() && nowSeconds() - lastDotAt > gaze_dots::kStaleSec) {
                // No samples (frameeyeosc stopped, or the eyes aren't tracked): no stale dots
                hideDots();
            }
        }

        // The eye preview while its sheet shows (the dashboard open): the samples as they come (none for
        // kPacketStaleSec: none), eyecam-rec's pictures while they can show (asked for every kRenewSec, and "off" once
        // they stop), drawn at the panel's usual pace while either comes in (eye_preview::DrawPacer: not at every
        // sample while the debug dots run the loop at every display frame). Closed, it holds nothing
        {
            const double now = nowSeconds();
            if (previewing != previewShown) {
                std::fprintf(stderr, "[preview] %s\n", previewing ? "open" : "closed");
                previewShown = previewing;
                if (!previewing) {
                    model.preview = eye_preview::View();
                    previewReader.close();
                    previewLogged.clear();
                }
            }
            if (model.preview.hasPacket && now - lastPreviewSample > eye_preview::kPacketStaleSec) {
                model.preview.hasPacket = false;
                dirty = true;
            }
            // A refused "preview on" shows why for a while, then the switch can ask again
            if (eye_preview::refusalOver(model.preview, now)) dirty = true;
            const eye_preview::CameraState camera = eye_preview::cameraState(
                model.eyecam, SettingsView(model).flag(key::kCameraLids), model.preview.refused);
            // The eye cameras can't give pictures (now): the switch goes off, and only pressing it again brings them
            // back (not eyecam-rec coming back by itself)
            if (camera != eye_preview::CameraState::Available && panel.previewPictures()) {
                panel.stopPreviewPictures();
                dirty = true;
            }
            const bool pictures = previewing && panel.previewPictures() && camera == eye_preview::CameraState::Available;
            std::string command;
            if (previewAsker.next(pictures, previewControl.busy(), now, command)) {
                const std::string socket = model.eyecamDir + "/ctl.sock";
                // (said when it starts and when it stops, not each time it is renewed)
                if (command == "preview off" || !previewOnSent) {
                    std::fprintf(stderr, "[preview] sending \"%s\" to %s%s\n", command.c_str(), socket.c_str(),
                                 command == "preview off" ? "" : " (again every second while the pictures show)");
                }
                previewOnSent = command != "preview off";
                previewControl.send(socket, command, now);
            }
            if (previewControl.poll(now) || (previewControl.hasReply() && !previewControl.busy())) {
                // ("preview on" refused, or not answered in time: no pictures, said once, why shown for kRefusedSec)
                const eyecam::Reply& reply = previewControl.reply();
                if (reply.command == "preview on" && eye_preview::tookReply(model.preview, reply.ok, now)) {
                    if (!reply.ok) std::fprintf(stderr, "[preview] \"preview on\" refused: %s\n", reply.error.c_str());
                    dirty = true;
                }
            }
            if (pictures) {
                std::string why;
                const std::string path = model.eyecamDir + "/" + eye_preview::kFileName;
                const eye_preview::ReadResult read = previewReader.read(path, model.preview.pictures, why);
                if (read == eye_preview::ReadResult::Read) {
                    if (!model.preview.hasPictures) std::fprintf(stderr, "[preview] pictures from %s\n", path.c_str());
                    model.preview.hasPictures = true;
                    previewPacer.changed();
                }
                const std::string line = read == eye_preview::ReadResult::Invalid ? why : std::string("read");
                if (line != previewLogged) {
                    if (read == eye_preview::ReadResult::Invalid) std::fprintf(stderr, "[preview] %s: %s\n", path.c_str(), why.c_str());
                    previewLogged = line;
                }
            } else if (model.preview.hasPictures) {
                // The pictures turned off (the switch, or the eye cameras): none kept
                model.preview.hasPictures = false;
                model.preview.pictures = eye_preview::Pictures();
                previewReader.close();
                dirty = true;
            }
            if (previewPacer.due(now)) dirty = true;
            // (the pictures' age, at the draw)
            if (previewing) model.preview.nowNs = eye_preview::monotonicNs();
        }

        // The developer tab "Eye capture" while eyecam-rec runs, dashboard open or closed: its status file read often
        // while it runs (once a second otherwise, to notice it), the reply to "start" / "stop", the light warning
        // before a start, and the full-view light during the bright and dark steps: faded in and, when its step ends,
        // out; hidden at once when recording ends, "stop" is sent, the file goes stale or the tab goes (and destroyed
        // with the other overlays at shutdown)
        {
            const bool running = model.eyecam.visible || vr.fillShown() || eyecamControl.busy();
            bool statusRead = false;
            if (nowSeconds() >= lastEyecamRead + (running ? kEyecamReadSec : kEyecamIdleReadSec)) {
                lastEyecamRead = nowSeconds();
                model.eyecam.status = eyecam::readStatus(model.eyecamDir);
                // (the recording's steps for its chips, read from eyecam's protocol file once a run)
                eyecam::followProtocol(model.eyecam, eyecam::libDir());
                model.eyecam.lastRun = eyecam::followRun(model.eyecam.lastRun, model.eyecam.status);
                statusRead = true;
            }
            const double now = unixNow();
            const eyecam::Status& s = model.eyecam.status;
            const bool shown = eyecam::tabVisible(s, now);
            if (shown != model.eyecam.visible) {
                std::fprintf(stderr, "[eyecam] tab %s (%s)\n", shown ? "shown" : "gone",
                             !s.present ? s.readError.c_str() : (shown ? s.stateText.c_str() : "stopped or stale"));
                model.eyecam.visible = shown;
            }
            if (eyecamControl.poll(nowSeconds())) {
                const eyecam::Reply& r = eyecamControl.reply();
                std::fprintf(stderr, "[eyecam] %s -> %s%s\n", r.command.c_str(), r.ok ? "ok" : "err ", r.error.c_str());
                // A run it took (a calibration's error then reads as the calibration's)
                if (r.ok && eyecam::runOfCommand(r.command) != eyecam::Run::None) {
                    model.eyecam.lastRun = eyecam::runOfCommand(r.command);
                }
                // A calibration it refused: its record ends here
                if (calibRun.active() && r.command == calibCommand && calibWatch.replied(r.ok)) {
                    finishCalib(r.error.empty() ? std::string("err") : r.error);
                }
                // A recording for squint training it refused: that try ends
                if (contribSession.replied(model.contrib, r)) {
                    std::fprintf(stderr, "[contrib] start refused: %s\n", r.error.c_str());
                    dirty = true;
                }
                // A sensitivity it refused: the slider shows the file's value again (the reply shows like others)
                if (!r.ok && eyecam::isSensitivityCommand(r.command)) {
                    panel.dropSensitivityHold();
                    dirty = true;
                }
            }
            // A setting's slider let go of: written like a press (and the widening slider's Valve eye, below)
            takeSliders(panel, [&](const PanelHit& slid) {
                applyHit(slid, model, panel, autostart, &updater, &fit, &vr, &eyeLog, &eyecamControl);
                dirty = true;
            });
            // The widening sensitivity slider: its value goes out when let go of, and a few times a second while
            // dragged, one command at a time
            {
                double value = 0.0;
                if (panel.takeSensitivity(value, nowSeconds())) {
                    sensitivitySender.released(value);
                    // One eye on the cameras: the other (Valve's values) takes lid_widen's nearest level with it
                    const SettingsView view(model);
                    if (widenSlider(view, model.status).control == WidenControl::Mixed && !view.locked(key::kLidWiden)) {
                        applyHit({PanelAction::SetLidWiden, key::kLidWiden, widenLevelAt(value)}, model, panel, autostart,
                                 &updater, &fit, &vr, &eyeLog, &eyecamControl);
                    }
                } else if (panel.sensitivityDragging()) {
                    sensitivitySender.dragged(panel.sensitivityValue(), nowSeconds());
                }
                std::string command;
                if (sensitivitySender.next(eyecamControl.busy(), nowSeconds(), command)) {
                    const std::string socket = model.eyecamDir + "/ctl.sock";
                    std::fprintf(stderr, "[eyecam] sending \"%s\" to %s\n", command.c_str(), socket.c_str());
                    if (!eyecamControl.send(socket, command, nowSeconds()) && !eyecamControl.busy()) {
                        std::fprintf(stderr, "[eyecam] %s failed: %s\n", command.c_str(),
                                     eyecamControl.reply().error.c_str());
                        panel.dropSensitivityHold();
                    }
                    dirty = true;
                }
            }
            syncEyecamControl(model.eyecam, eyecamControl);
            // "Record for squint training": its recording's end (checked and moved into its folder; watched with the
            // dashboard closed too), and the pack or the delete
            if (statusRead) {
                std::string log;
                if (contribSession.follow(model.contrib, model.eyecam, now, bootSeconds(), log)) dirty = true;
                for (size_t start = 0; start < log.size();) {
                    const size_t end = std::min(log.find('\n', start), log.size());
                    std::fprintf(stderr, "[contrib] %s\n", log.substr(start, end - start).c_str());
                    start = end + 1;
                }
            }
            // The calibration's record: where eyecam keeps it, the cameras' rate during it, and its end once eyecam-rec
            // leaves "calibrating" (or never got there)
            if (calibRun.active()) {
                if (s.state == eyecam::State::Calibrating) {
                    calibRun.calibDir(s.sessionDir);
                    const double fps = eyecam::cameraFps(s);
                    if (std::isfinite(fps)) calibRun.summary().conditions.cameraFps = fps;
                }
                // (only a fresh status.json counts: eyecam-rec gone mid-calibration ends it after kSilentSec)
                const bool fresh = s.present && eyecam::age(s, now) <= eyecam::kVisibleSec;
                const bool calibrating = s.state == eyecam::State::Calibrating;
                if (calibWatch.follow(calibrating, fresh, eyecamControl.busy(), nowSeconds())) {
                    if (calibWatch.silent()) {
                        std::fprintf(stderr, "[eyecam] no fresh status for %.0f s: the calibration's record ends\n",
                                     report::CalibWatch::kSilentSec);
                    }
                    finishCalib("");
                }
            }
            // The setup: the password (only while it may still be needed: at (1) or (2), the tool not in), how its
            // calibration ended, and the short "ready" in the left column after it
            {
                // (only while the panel shows on the dashboard: nobody reads it otherwise)
                const bool wanted = visible && shown &&
                                    eyecam::setupStep(s, eyecam::PasswordState::Unknown) == eyecam::SetupStep::Tool;
                if (passwordCheck.tick(wanted, nowSeconds())) {
                    std::fprintf(stderr, "[setup] password: %s\n",
                                 passwordCheck.state() == eyecam::PasswordState::Set      ? "set"
                                 : passwordCheck.state() == eyecam::PasswordState::NotSet ? "not set"
                                                                                           : "unknown");
                }
                model.eyecam.password = passwordCheck.state();
                const eyecam::SetupResult before = model.eyecam.flow.result();
                const eyecam::CalibResult pageBefore = model.eyecam.flow.calibResult();
                model.eyecam.flow.follow(s, nowSeconds());
                if (model.eyecam.flow.result() != before && model.eyecam.flow.result() != eyecam::SetupResult::None) {
                    std::fprintf(stderr, "[setup] calibration ended: %s\n",
                                 model.eyecam.flow.result() == eyecam::SetupResult::Done ? "done" : "standard widening");
                }
                const eyecam::CalibResult page = model.eyecam.flow.calibResult();
                if (page != pageBefore && page != eyecam::CalibResult::None) {
                    std::fprintf(stderr, "[eyecam] calibration ended: %s\n",
                                 page == eyecam::CalibResult::User      ? "user"
                                 : page == eyecam::CalibResult::Default ? "standard widening"
                                                                        : "measured");
                }
                model.eyecam.readyNotice = model.eyecam.flow.readyNotice(nowSeconds());
            }
            const std::string state = shown ? s.stateText + " " + std::to_string(s.stepIndex) + " " + s.stepLabel : "";
            if (state != loggedEyecam) {
                if (shown) {
                    std::fprintf(stderr, "[eyecam] %s, step %d/%d %s%s%s\n", s.stateText.c_str(), s.stepIndex,
                                 s.stepCount, s.stepLabel.c_str(), s.message.empty() ? "" : ": ", s.message.c_str());
                }
                loggedEyecam = state;
            }
            const std::string signature = eyecam::signature(model.eyecam);
            if (signature != drawnEyecam) {
                drawnEyecam = signature;
                dirty = true;
            }
            // The light fades in and out on a step change while recording goes on; anything else (stale file, tab
            // gone, recording over, "stop" sent) takes it away at once
            if (panel.syncEyecam(model.eyecam)) dirty = true;
            const eyecam::Fill wanted = eyecam::fillFor(s, now);
            const bool hideNow = eyecam::hideLightAtOnce(model.eyecam, now);
            const eyecam::Light before = light;
            light = eyecam::stepLight(light, wanted, hideNow, nowSeconds());
            if (light.fill == eyecam::Fill::None) {
                if (vr.fillShown()) {
                    vr.hideFill();
                    const char* why = "faded out";
                    if (hideNow) {
                        why = !shown                                          ? "tab gone"
                              : eyecam::age(s, now) > eyecam::kOverlayStaleSec ? "status file stale"
                              : s.state != eyecam::State::Recording           ? "not recording"
                                                                              : "stop sent";
                    }
                    std::fprintf(stderr, "[eyecam] light off (%s)\n", why);
                }
                fillSent.clear();
            } else {
                if (before.fill == light.fill && wanted != light.fill && lightWanted == light.fill) {
                    std::fprintf(stderr, "[eyecam] light fading out (step changed)\n");
                }
                const bool bright = light.fill == eyecam::Fill::Bright;
                // The light's own step (while it fades out, the status already has the next one)
                const std::string label = eyecam::instruction(uiText(model.language), bright ? "bright" : "dark");
                const std::string key = std::string(bright ? "bright|" : "dark|") + label;
                if (key != fillDrawn) {
                    renderFill(fonts, bright, label, fillImage);
                    fillDrawn = key;
                }
                const bool fresh = key != fillSent;
                if (vr.showFill(fresh ? fillImage.data() : nullptr, kFillImageSize, light.alpha)) {
                    if (fresh) std::fprintf(stderr, "[eyecam] light on: %s, fading in\n", bright ? "bright" : "dark");
                    fillSent = key;
                } else {
                    fillSent.clear();
                }
            }
            lightWanted = wanted;
            lightMoving = eyecam::lightFading(light, wanted);
        }

        // A run's status a second (status.jsonl)
        if ((fitRun.active() || calibRun.active()) && unixNow() >= lastRunSample + 1.0) {
            lastRunSample = unixNow();
            const std::string core = readSmallFile(model.statusPath);
            const std::string eyecamText = readSmallFile(model.eyecamDir + "/status.json");
            fitRun.sample(lastRunSample, core, eyecamText);
            calibRun.sample(lastRunSample, core, eyecamText);
        }
        // A record written: the failure screens' bar links to it, the lists show it
        {
            std::string folder;
            report::Kind kind = report::Kind::Fit;
            while (writer.poll(folder, kind)) {
                const bool calib = kind == report::Kind::CalibWear || kind == report::Kind::CalibUser;
                (calib ? model.records.lastCalib : model.records.lastFit) = folder;
                loadRecords(model);
                if (panel.recordShown() == folder) openRecordView(model, folder);
                dirty = true;
            }
        }
        // "Having trouble" (or all records) shown: the records read again
        {
            const bool shown = visible && ((panel.advPageShown() && panel.advPage() == AdvPage::Trouble) ||
                                           panel.recordsAllOpen());
            if (shown && !recordsWereShown) {
                loadRecords(model);
                dirty = true;
            }
            recordsWereShown = shown;
        }
        // The Advanced tab's sub-tab, remembered for the next start
        if (panel.advPage() != savedPage) {
            savedPage = panel.advPage();
            std::string error;
            if (!ui_state::writePage(uiStatePath, savedPage, error)) std::fprintf(stderr, "[panel] %s\n", error.c_str());
        }
        // ...and the Eyelids tab's
        if (panel.lidsPage() != savedLidsPage) {
            savedLidsPage = panel.lidsPage();
            std::string error;
            if (!ui_state::writeLidsPage(uiStatePath, savedLidsPage, error)) {
                std::fprintf(stderr, "[panel] %s\n", error.c_str());
            }
        }
        // ...and the Gaze tab's
        if (panel.gazePage() != savedGazePage) {
            savedGazePage = panel.gazePage();
            std::string error;
            if (!ui_state::writeGazePage(uiStatePath, savedGazePage, error)) {
                std::fprintf(stderr, "[panel] %s\n", error.c_str());
            }
        }
        // ...and the gaze scale's locks
        if (panel.gazeScaleLinked(0) != savedLinks[0] || panel.gazeScaleLinked(1) != savedLinks[1]) {
            savedLinks[0] = panel.gazeScaleLinked(0);
            savedLinks[1] = panel.gazeScaleLinked(1);
            std::string error;
            if (!ui_state::writeGazeScaleLinks(uiStatePath, savedLinks, error)) {
                std::fprintf(stderr, "[panel] %s\n", error.c_str());
            }
        }
        // ...and "Get test versions" (the checker was switched when it was pressed)
        if (panel.updateBeta() != savedBeta) {
            savedBeta = panel.updateBeta();
            std::string error;
            if (!ui_state::writeUpdateBeta(uiStatePath, savedBeta, error)) std::fprintf(stderr, "[panel] %s\n", error.c_str());
        }

        // The diagnostics page: redrawn when anything on it changes (the tool's checksum is looked at again too, which
        // costs a stat unless the file changed)
        if (visible && panel.diagOpen() && nowSeconds() >= lastDiagRead + kStatusReadSec) {
            lastDiagRead = nowSeconds();
            readSystem(model);
            const std::string signature = diag::signature(uiText(model.language), model);
            if (signature != drawnDiag) {
                drawnDiag = signature;
                dirty = true;
            }
        }

        // The Advanced tab's diagnostic code: worked out again and compared (it reads more of the status files than the
        // eye cameras' signature covers: the changed blocks, the last calibration, the proximity reading)
        if (visible && panel.advPageShown() && panel.advPage() == AdvPage::Trouble) {
            const std::string code = diag::code(model);
            if (code != drawnCode) {
                drawnCode = code;
                dirty = true;
            }
        }

        // Draw only while visible, and only when something changed. While a fit runs on its tab, the panel is behind
        // the dots: drawn once and then only for the pointer, so nothing on it moves (the left column's numbers, the
        // steps) and the loop keeps to the display's frames; what changed meanwhile is drawn when the fit ends
        const bool calm = fit.active() && panel.tab() == PanelTab::EyeFit;
        if (!calm) calmDrawn = false;
        if (visible && (dirty || !wasVisible) && (!calm || !calmDrawn || pointerDirty || !wasVisible)) {
            panel.render(model);
            vr.submitPanel(panel.toRgba().data());
            dirty = false;
            calmDrawn = calm;
            previewPacer.drawn(nowSeconds());
            if (firstSubmit) {
                firstSubmit = false;
                vr.logOverlayState("after the first draw");
            }
        }
        // The IP keypad and the eye capture's light warning don't stay open behind a closed dashboard
        if (!visible && wasVisible) {
            panel.closeHostEntry();
            // ...nor the eye preview (its pictures stop with it)
            panel.closePreview();
            panel.closeEyecamConfirm();
            // ...nor the other apps' confirmation (back to their list)
            panel.closeAppsConfirm();
            // ...nor the setup's done screen ("next time it shows the usual page")
            model.eyecam.flow.closed();
        }
        wasVisible = visible;
        // Every display frame while the fit's target or the debug dots are up: with the target, paced by the
        // compositor itself (a fixed sleep plus the loop's work fell behind the display, and the ring stuttered)
        // (the dots only while packets arrive: with none for kStaleSec, the usual slow poll)
        const bool dotsLive = dots.isOpen() && dotsWanted && nowSeconds() - lastDotAt <= gaze_dots::kStaleSec;
        // (and while the eye capture's light fades, so its alpha steps are small)
        const bool everyFrame = fit.active() || dotsLive || lightMoving;
        // While eyecam-rec runs, as often as with the panel open, so the light follows its steps closely
        const bool eyecamLive = model.eyecam.visible || vr.fillShown() || eyecamControl.busy();
        if (!(targetUp && vr.waitFrameSync(kFrameSyncTimeoutMs))) {
            sleepInterruptible(everyFrame ? kFitPollSec : ((visible || eyecamLive) ? kPanelPollSec : kClosedPollSec));
        }
    }

    // The same shutdown for SIGTERM / SIGINT, SteamVR quitting, vrserver gone, "close" and "Quit"; a recording is
    // stopped (and its file written out) first. The eye capture's light goes first of all (shutdown also destroys it),
    // and the --action socket with it (a command from now on hears the panel isn't running, not a timeout)
    actionServer.close();
    if (vr.fillShown()) std::fprintf(stderr, "[eyecam] light off (quitting)\n");
    vr.hideFill();
    eyeLog.shutdown();
    // ...and eyecam's drift log with it (a command still out answered first; each waited for at most a second)
    {
        const auto settle = [&]() {
            const double end = nowSeconds() + 1.0;
            while (driftControl.busy() && nowSeconds() < end) {
                if (driftControl.poll(nowSeconds())) {
                    driftReplied(driftControl.reply(), eyeLog);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        };
        settle();
        std::string command;
        if (driftLink.next(driftInputs(eyeLog.view(nowSeconds()), model), driftControl.busy(), command)) {
            sendDrift(driftControl, model.eyecamDir, command, eyeLog);
            settle();
        }
    }
    vr.shutdown();
    autostart.stop();
    // A fit or a calibration still going: its record, stopped by the panel closing, written before the panel exits
    // (the writer writes all that is queued before its thread ends)
    if (fitRun.active()) {
        report::Pending pending = fitRun.finish(unixNow());
        fillClosedResult(pending.summary);
        submitRecord(std::move(pending));
    }
    if (calibRun.active()) {
        report::Pending pending = calibRun.finish(unixNow());
        fillClosedResult(pending.summary);
        calibWatch.end();
        submitRecord(std::move(pending));
    }
    writer.finish();
    std::fprintf(stderr, "[VR] done\n");
    if (lockFd >= 0) ::close(lockFd);
    return userQuit ? kExitCodeUserQuit : 0;
}

}  // namespace

/**
 * Entry point.
 * @param argc argument count
 * @param argv arguments
 * @return exit code
 */
int main(int argc, char** argv) {
    // Line-buffered stderr so journald gets each line at once
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    installSignalHandlers();

    // `--action NAME` talks to the running panel and exits with its own codes (1 for bad arguments, not the usage's 2)
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (std::find(args.begin(), args.end(), "--action") != args.end()) {
        std::string line;
        const int code =
            action_socket::runCommand(args, action_socket::defaultSocketPath(), action_socket::kReplyTimeoutMs, line);
        std::fprintf(stderr, "%s\n", line.c_str());
        return code;
    }

    // Every usage error exits 1 (2 is --action's "panel not running")
    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return 1;
    }
    switch (options.mode) {
        case Options::Mode::Help: printUsage(); return 0;
        case Options::Mode::Version: std::printf("frameeyeosc-panel %s\n", FRAMEEYEOSC_VERSION); return 0;
        case Options::Mode::Print: return runPrint(options);
        case Options::Mode::DumpPng: return runDumpPng(options);
        case Options::Mode::Probe: return VrOverlay::probe();
        case Options::Mode::SwitchAway: return VrOverlay::switchAway(options.switchAwaySec);
        case Options::Mode::ContrastReport: return printContrastReport();
        case Options::Mode::PlaySound: return runPlaySound(options);
        case Options::Mode::Report: return runReport(options);
        case Options::Mode::ReportMake: return runReportMake(options);
        case Options::Mode::Overlay: break;
    }
    return runOverlay(options);
}
