// frameeyeosc-panel: a SteamVR dashboard panel for frameeyeosc's settings. It only writes config.json and reads
// status.json; frameeyeosc keeps sending when the panel is closed, crashes or is not installed.
#include "auto_recenter.h"
#include "autostart.h"
#include "config.h"
#include "draw.h"
#include "eyecam.h"
#include "gaze_dots.h"
#include "gaze_fit.h"
#include "host_entry.h"
#include "i18n.h"
#include "model.h"
#include "panel.h"
#include "recorder.h"
#include "setup_tools.h"
#include "sounds.h"
#include "status.h"
#include "target.h"
#include "theme.h"
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
    enum class Mode { Overlay, Print, DumpPng, Probe, SwitchAway, ContrastReport, PlaySound, Help, Version };
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
    int targetSeconds = 3;
    double targetProgress = 0.7;
    int targetBench = 0;          ///< --target-bench N: time drawing the target N times
    bool fitDetails = false;      ///< --fit-details: "Fine-tune" open on the Eye fit tab
    int fitDetailsPage = 0;       ///< --fit-details lids: its eyelid page
    bool lidMarks = false;        ///< --lid-marks: the lid marks open on the Eyelids tab
    bool history = false;         ///< --history: the version history open (on the Advanced tab)
    std::string historyOpen;      ///< --history-open: the version whose row is open ("" = the installed one)
    double historyScroll = -1.0;  ///< --history-scroll: px it is scrolled (-1 = as opened)
    double advScroll = 0.0;       ///< --adv-scroll: px the Advanced tab's page is scrolled
    std::string changelogDir;     ///< --changelog-dir: read CHANGELOG*.md from here only
    bool diag = false;            ///< --diag: the diagnostics page open (on the Advanced tab)
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
    bool fakeCustom = false;
    bool fakeIndependent = false; ///< --fake-independent: independent_eyes on, the gaze pad per eye
    std::string fakeDominantEye;  ///< --fake-dominant-eye: "left" / "right" ("Track Dominant Eye Only")
    bool fakeOpennessSaturated = false;  ///< --fake-openness-saturated: a relaxed open eye reads 1.0
    std::string fakePrompt;       ///< vrchat / etvr / livelink
    std::string fakeUpdate;       ///< a made-up update state (see printUsage)
    std::string fakeUpdateNotes;  ///< --fake-update-notes: the new release's summary ("both", "en" or "long")
    std::string fakeFit;          ///< a made-up eye fit state (see printUsage)
    std::string fakeRecord;       ///< a made-up eye log state: "recording", "failed" or "autostopped"
    std::string fakeWiden;        ///< lid_widen in the made-up config ("" = the default)
    std::string fakeEyecam;       ///< a made-up eyecam-rec state (see printUsage and parseFakeEyecam)
    std::string fakeCamera;       ///< --fake-camera: the eye cameras as frameeyeosc reports them (see printUsage)
    std::string fakePassword = "set";  ///< --fake-password: the setup's password check
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
        "      --lid-marks       Show the lid marks on the Eyelids tab although the eyes are fitted\n"
        "      --history         Open the version history (Advanced tab)\n"
        "      --history-open VERSION  ...with this version's row open instead of the installed one\n"
        "      --history-scroll PX  ...scrolled this far (kept within the list)\n"
        "      --diag            Open the diagnostics page (Advanced tab)\n"
        "      --adv-scroll PX   Scroll the Advanced tab's page this far (kept within the page; with --tab advanced)\n"
        "      --changelog-dir DIR  Read CHANGELOG.md / CHANGELOG.ja.md from DIR instead of next to the binary,\n"
        "                        the checkout (panel/build) or ~/.local/share/frameeyeosc\n"
        "      --preview-quit    Show \"press again to quit\"\n"
        "      --preview-reset   Show \"press again to reset\"\n"
        "      --preview-update-prompt  Show the \"update to ...?\" question (with --fake-update available)\n"
        "      --fake            Draw a made-up state (running, sending to VRChat) instead of the files.\n"
        "                        Each --fake-* below implies --fake\n"
        "      --fake-not-running / --fake-paused / --fake-no-tracking / --fake-etvr / --fake-livelink / --fake-fixed\n"
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
        "      --fake-custom     Gaze smoothing values that match no preset\n"
        "      --fake-independent  Move eyes separately (the left column shows each eye's gaze)\n"
        "      --fake-dominant-eye left|right  \"Track Dominant Eye Only\" is on with that eye\n"
        "      --fake-openness-saturated  A relaxed open eye reads 1.0 (SteamOS 0.4.3), so widening can't come through\n"
        "      --fake-prompt vrchat|etvr|livelink  The recommended-settings question\n"
        "      --fake-autostart on|off|missing|unknown\n"
        "      --fake-update checking|uptodate|available|manual|installing|installed|checkfailed|installfailed\n"
        "                        A made-up update state (the version row on the Advanced tab)\n"
        "      --fake-update-notes both|en|long  With --fake-update available|manual, the new release's summary:\n"
        "                        English and Japanese, English only, or both cut at 300 characters\n"
        "      --fake-fit running|running-center|running-tilt|running-closed|done|done-center|done-tilt|\n"
        "                 fitted|fitted-gaze|\n"
        "                 failed-unsteady|failed-notclosed|failed-movement|failed-lidrange|failed-cancelled|\n"
        "                 failed-left|failed-opened|failed-noresult  A made-up eye fit (Eye fit tab)\n"
        "      --fake-record recording|failed|autostopped  A made-up eye log (Advanced tab, and the left column)\n"
        "      --fake-widen off|low|normal|high  lid_widen in the made-up settings\n"
        "      --fake-eyecam waiting|idle|confirm|searching|recording:LABEL|calibrating[:LABEL]|error|calib-error\n"
        "                        A made-up eyecam-rec for the eye cameras tab (confirm: idle with the recording's\n"
        "                        light warning open, on the Advanced tab; LABEL: lead_in, normal, widen, close,\n"
        "                        squint, look_up, look_down, bright, dark, end; calibrating without a label: waiting\n"
        "                        for the video; calib-error: a failed user calibration). Flags after it, each with\n"
        "                        \":\": unlocked (the cameras lost the eyes), nolight (recording without the light),\n"
        "                        user (calibrating / calib-error: the user's calibration; wear: this wear's),\n"
        "                        calib=N (calib_state 0..7, bit 2 a baseline learned by itself; default 0, 1 for a\n"
        "                        failed user calibration), warming=N (learning the relaxed eyes, N s left), ready\n"
        "                        (learned them; both mark an eyecam-rec that learns by itself), saved (calib_saved),\n"
        "                        recalib (recalib_suggested), nolive (not reading the cameras live), auto=VALUE\n"
        "                        (auto_grab), outdated (grab_outdated: an update brought a new tool), sens=V\n"
        "                        (widen_sensitivity 0..1), buffers (has_buffers), setup /\n"
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
        "                        (failed=L|R: provisional values, not the previous ones),\n"
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
        "      --fake-camera both|left|right|uncalibrated|absent|error|off  The eye cameras as frameeyeosc reports\n"
        "                        them (off: camera_lids off)\n"
        "  --eyecam-fill-png PATH  Draw the eye capture's full-view light (with --language) to a PNG\n"
        "      --eyecam-fill bright|dark  Which one (default bright)\n"
        "  --eyecam-dir DIR      eyecam-rec's folder: status.json and ctl.sock (default $XDG_RUNTIME_DIR/eyecam)\n"
        "      --update-live     Run the real update checker: check first, and after each --click wait for the\n"
        "                        check or install it started (installs really happen; for testing with a fake GitHub)\n"
        "      --click X,Y       Press the panel at X,Y first (repeatable; writes --config; not with --fake)\n"
        "  --contrast-report     Print the WCAG contrast ratio of every color pair on screen\n"
        "  --probe               Diagnostics: connect to SteamVR as a background app and describe the resident panel\n"
        "  --probe-switch-away [S]  Diagnostics: switch the dashboard to a temporary overlay for S s (default 3)\n"
        "  --version             Print the version\n"
        "  --config PATH         Config file (default ~/.config/frameeyeosc/config.json)\n"
        "  --status PATH         Status file (default $XDG_RUNTIME_DIR/frameeyeosc/status.json)\n");
}

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
    std::string search;     ///< search: why the video isn't found (not_worn / no_video / one_eye; "" = an older
                            ///< eyecam-rec without it, or locked)
    double prox = -1.0;     ///< prox=V: the proximity reading (-1 = as the search says)
    int blocks = -1;        ///< blocks=N: changed blocks before the last look (-1 = as the search says)
    bool lastError = false; ///< lasterror: eyecam-rec's last error
    double camFps = -1.0;   ///< camfps=N: the cameras' frame rate (-1 = an eyecam-rec before cam_fps)
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
        } else if (flag.rfind("sens=", 0) == 0 && flag.size() > 5 &&
                   flag.find_first_not_of("0123456789.", 5) == std::string::npos) {
            fake.sens = std::atof(flag.c_str() + 5);
        } else if (flag.rfind("calib=", 0) == 0 && flag.size() == 7 && flag[6] >= '0' && flag[6] <= '7') {
            fake.calibState = flag[6] - '0';
        } else {
            return false;
        }
    }
    return true;
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
            options.lidMarks = true;
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
        } else if (arg == "--fake-custom") {
            options.fake = options.fakeCustom = true;
        } else if (arg == "--fake-independent") {
            options.fake = options.fakeIndependent = true;
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
                options.fakePrompt != kOutputLivelink) {
                std::fprintf(stderr, "--fake-prompt must be vrchat, etvr or livelink: %s\n", options.fakePrompt.c_str());
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
                                                  "installing", "installed", "checkfailed", "installfailed"};
            if (std::find(std::begin(kStates), std::end(kStates), options.fakeUpdate) == std::end(kStates)) {
                std::fprintf(stderr, "--fake-update: unknown state %s\n", options.fakeUpdate.c_str());
                return false;
            }
            options.fake = true;
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
                "running",          "running-center",  "running-tilt",     "running-closed",  "done",
                "done-center",      "done-tilt",       "fitted",           "fitted-gaze",     "failed-unsteady",
                "failed-notclosed", "failed-movement", "failed-lidrange",  "failed-cancelled", "failed-left",
                "failed-opened",    "failed-noresult"};
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
 * @return the settings
 */
frame_updater::UpdaterConfig updaterConfig() {
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
        // recording:<label>, at that step of the default run (9 steps, 120 s)
        static const char* const kLabels[] = {"normal",    "widen",  "close", "squint", "look_up",
                                              "look_down", "bright", "dark",  "end"};
        s.stateText = "recording";
        s.stepLabel = fake.label;
        if (fake.noLight) s.protocol = eyecam::kNoLightProtocol;
        s.stepCount = static_cast<int>(std::size(kLabels));
        s.stepIndex = static_cast<int>(std::find(std::begin(kLabels), std::end(kLabels), s.stepLabel) -
                                       std::begin(kLabels));
        s.stepRemainingS = 3.4;
        s.totalS = 120.0;
        s.elapsedS = s.stepIndex * 13.0 + 9.6;
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
    if (options.fakePupilsOff) root.set(key::kPupilsToVrchat, JsonValue::makeBool(false));
    if (options.fakePupilBits >= 0) root.set(key::kPupilBits, JsonValue::makeNumber(options.fakePupilBits, true));
    if (options.fakeFixed) root.set(key::kHost, JsonValue::makeString("192.168.0.60"));
    if (!options.fakeWiden.empty()) root.set(key::kLidWiden, JsonValue::makeString(options.fakeWiden));
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
        fit.mode = state.find("center") != std::string::npos ? gaze_fit::Mode::Center
                   : state.find("tilt") != std::string::npos ? gaze_fit::Mode::Tilt
                                                              : gaze_fit::Mode::Full;
        fit.count = gaze_fit::pointCount(fit.mode);
        // The fit in config.json: the gaze of the live run on 2026-09-28, and eyelids like the worn recording
        const bool saved = state.rfind("done", 0) == 0 || state.rfind("fitted", 0) == 0;
        if (saved) {
            root.set(key::kGazeOffsetX, JsonValue::makeNumber(0.012));
            root.set(key::kGazeOffsetY, JsonValue::makeNumber(-0.02));
            root.set(key::kGazeGainX, JsonValue::makeNumber(0.93));
            root.set(key::kGazeGainUp, JsonValue::makeNumber(0.9));
            root.set(key::kGazeGainDown, JsonValue::makeNumber(0.88));
            root.set(key::kGazeRollDeg, JsonValue::makeNumber(6.7));
            // Each eye's own sideways fit (made-up numbers)
            root.set(key::kGazeOffsetXLeft, JsonValue::makeNumber(0.031));
            root.set(key::kGazeOffsetXRight, JsonValue::makeNumber(-0.006));
            root.set(key::kGazeGainXLeft, JsonValue::makeNumber(0.95));
            root.set(key::kGazeGainXRight, JsonValue::makeNumber(0.9));
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
        } else if (state == "running-tilt") {
            fit.phase = Phase::Settling;
            fit.index = 1;
            fit.point = Point::Up;
        } else if (state == "running-closed") {
            fit.phase = Phase::Settling;
            fit.index = 5;
            fit.point = Point::Closed;
        } else if (state.rfind("done", 0) == 0) {
            fit.phase = Phase::Done;
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
                          : state == "failed-opened"    ? Failure::DashboardOpened
                                                        : Failure::NoResult;
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
        s.output = etvr ? kOutputEtvr : (options.fakeLivelink ? kOutputLivelink : kOutputVrchat);
        s.targetMode = options.fakeFixed ? "fixed" : "auto";
        if (!options.fakeTargetNull) {
            s.target = std::string("192.168.0.60:") +
                       (etvr ? "8889" : options.fakeLivelink ? "11111" : (options.fakeCustom ? "9001" : "9000"));
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
    if (!options.fakeEyecam.empty()) m.eyecam = fakeEyecam(options.fakeEyecam);
    m.eyecam.password = options.fakePassword == "unset"   ? eyecam::PasswordState::NotSet
                        : options.fakePassword == "unknown" ? eyecam::PasswordState::Unknown
                                                            : eyecam::PasswordState::Set;
    m.autostart.autostart = options.fakeAutostart;
    m.language = configLanguage(m.config);
    if (options.fakeWriteError) m.panelError = "rename failed: Read-only file system";
    m.update = fakeUpdate(options.fakeUpdate, options.fakeUpdateNotes);
    if (options.fakeUpdate.empty()) m.update.state = frame_updater::UpdateState::UpToDate;
    // The diagnostics page's SteamOS and camera tool, as on the developer's headset
    m.system.steamos = "0.4.3 (20260930.6234839)";
    m.system.grabHash = "294d06c6";
    return m;
}

void applyHit(const PanelHit& hit, PanelModel& model, EyePanel& panel, AutostartWorker& autostart,
              frame_updater::UpdateChecker* updater, gaze_fit::Session* fit, const VrOverlay* vr,
              recorder::Recorder* eyeLog, eyecam::Control* eyecamControl);
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
            model.eyecam.visible = eyecam::tabVisible(model.eyecam.status, unixNow());
            model.eyecam.lastRun = eyecam::followRun(model.eyecam.lastRun, model.eyecam.status);
            model.update.current = FRAMEEYEOSC_VERSION;
            readSystem(model);
        }
        if (!options.language.empty()) parseLanguage(options.language, model.language);
        model.changelogDirs =
            options.changelogDir.empty() ? changelog::defaultDirs() : std::vector<std::string> {options.changelogDir};
        EyePanel panel(fonts);
        panel.setTab(options.tab);
        panel.setFitDetails(options.fitDetails);
        panel.setFitDetailsPage(options.fitDetailsPage);
        panel.setLidMarks(options.lidMarks);
        if (options.diag) panel.openDiag();
        panel.setAdvancedScroll(options.advScroll);
        if (options.history) {
            loadHistory(model);
            panel.openHistory();
            if (!options.historyOpen.empty()) panel.setHistoryRow(options.historyOpen);
            if (options.historyScroll >= 0) panel.setHistoryScroll(options.historyScroll);
        }
        if (options.fakeEyecam.rfind("confirm", 0) == 0) panel.openEyecamConfirm(model.eyecam.status.state);
        if (options.previewQuit) panel.armQuitForPreview();
        if (options.previewReset) panel.armResetForPreview();
        if (!options.fakePrompt.empty()) panel.showPrompt(options.fakePrompt);
        if (options.previewUpdatePrompt) panel.showUpdatePrompt(model.update.latest);
        std::unique_ptr<frame_updater::UpdateChecker> updater;
        const bool updateCheck = model.config.flag(key::kUpdateCheck);
        if (options.updateLive) {
            updater = std::make_unique<frame_updater::UpdateChecker>(updaterConfig());
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
            // Wait for eyecam-rec's reply, and show the folder as it is after it
            if (hit.action == PanelAction::EyecamChoose || hit.action == PanelAction::EyecamStop ||
                hit.action == PanelAction::EyecamCalib) {
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
        renderGazeDot(kind, rgba, options.dotPngPath);
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
 * Bring an older config.json up to date, once (see migrateLidScales and migrateGazePresets), and read it again.
 * @param model the model (its config is re-read after a write)
 */
void migrateConfig(PanelModel& model) {
    if (!model.config.exists || !model.config.error.empty() || !configNeedsMigration(model.config.root)) return;
    std::string lidLog;
    std::string gazeLog;
    bool lids = false;
    bool gaze = false;
    std::string error;
    const bool ok = updateConfigFile(
        model.configPath,
        [&](JsonValue& root) {
            lids = migrateLidScales(root, lidLog);
            gaze = migrateGazePresets(root, gazeLog);
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
    std::fprintf(stderr, "[fit] done: offset %+.3f %+.3f, tilt %+.1f deg, gains %.2f %.2f %.2f%s\n", values.offsetX,
                 values.offsetY, values.rollDeg, values.gainX, values.gainUp, values.gainDown,
                 full ? "" : " (the rest unchanged)");
    if (full && values.hasLids) {
        for (int eye = 0; eye < 2; ++eye) {
            std::fprintf(stderr, "[fit] %s eyelid: closed %.3f, up %.3f, ahead %.3f, down %.3f\n",
                         eye == 0 ? "left" : "right", values.lidClosed[eye], values.lidUp[eye], values.lidOpen[eye],
                         values.lidDown[eye]);
        }
    }
    if (values.hasEyeX) {
        std::fprintf(stderr, "[fit] each eye sideways: left %+.3f x%.2f, right %+.3f x%.2f\n", values.eyeOffsetX[0],
                     values.eyeGainX[0], values.eyeOffsetX[1], values.eyeGainX[1]);
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
 * @param mode the whole fit, re-centering only, or re-centering and the tilt
 * @param view the settings (the fit now)
 * @param vr the connection to SteamVR, for the IPD (null: the default)
 */
void startFit(gaze_fit::Session& fit, gaze_fit::Mode mode, const SettingsView& view, const VrOverlay* vr) {
    const double ipd = vr != nullptr ? vr->userIpdMeters() : gaze_fit::kDefaultIpdM;
    const char* name = mode == gaze_fit::Mode::Full     ? "eye fit"
                       : mode == gaze_fit::Mode::Center ? "re-center"
                                                        : "re-center and tilt";
    std::fprintf(stderr, "[fit] %s, IPD %.1f mm, dashboard %s\n", name, ipd * 1000,
                 vr != nullptr && vr->dashboardVisible() ? "open" : "closed");
    fit.start(mode, fitInConfig(view).values, nowSeconds(), ipd);
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
 */
void applyHit(const PanelHit& hit, PanelModel& model, EyePanel& panel, AutostartWorker& autostart,
              frame_updater::UpdateChecker* updater, gaze_fit::Session* fit, const VrOverlay* vr,
              recorder::Recorder* eyeLog, eyecam::Control* eyecamControl) {
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
            if (hit.arg < 0 || hit.arg > 2) return;
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
                panel.showUpdatePrompt(u.latest);
            } else if (u.state == frame_updater::UpdateState::InstallFailed) {
                panel.showUpdatePrompt(u.version.empty() ? u.latest : u.version);
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
        case PanelAction::FitStart:
        case PanelAction::FitCenter: {
            const bool full = hit.action == PanelAction::FitStart;
            std::fprintf(stderr, "[fit] %s: started from the panel\n", full ? "eye fit" : "re-wear fit");
            const gaze_fit::Mode mode = full ? gaze_fit::Mode::Full : rewearMode(autoRecenter(model.config));
            if (fit != nullptr) startFit(*fit, mode, view, vr);
            return;
        }
        case PanelAction::FitDetails:
        case PanelAction::FitDetailsPage:
        case PanelAction::LidMarks:
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
        case PanelAction::SetupProceed:
            std::fprintf(stderr, "[setup] on to the eye cameras' page\n");
            model.eyecam.flow.proceed();
            return;
        case PanelAction::SetupKonsole:
        case PanelAction::SetupVideo: {
            // Konsole with the command typed in (the user presses Enter and types the password: the panel never runs
            // sudo or passwd), or the video in Chromium. Only in VR: --dump-png (vr null) only says what it would run
            const std::vector<std::string> argv =
                hit.action == PanelAction::SetupVideo
                    ? setup_tools::videoArgv(setup_tools::kVideoUrl)
                    : setup_tools::setupKonsoleArgv(hit.arg, model.language);
            std::string line;
            for (const std::string& arg : argv) line += (line.empty() ? "" : " | ") + arg;
            if (vr == nullptr) {
                std::printf("setup: would start %s\n", line.c_str());
                return;
            }
            std::string error;
            if (setup_tools::spawnDetached(argv, error)) {
                std::fprintf(stderr, "[setup] started %s\n", line.c_str());
                model.eyecam.spawnError.clear();
            } else {
                std::fprintf(stderr, "[setup] couldn't start %s: %s\n", argv[0].c_str(), error.c_str());
                model.eyecam.spawnError = argv[0] + ": " + error;
            }
            return;
        }
        case PanelAction::HistoryOpen:
            loadHistory(model);
            return;
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
        case PanelAction::EyecamCalib: {
            // One line to its socket ("start" or "start widen_nolight" from the light warning, "stop", or a
            // calibration); the reply is read by the loop (eyecam::Control never waits)
            if (eyecamControl == nullptr) return;
            const std::string command =
                hit.action == PanelAction::EyecamStop    ? std::string("stop")
                : hit.action == PanelAction::EyecamCalib ? eyecam::calibCommand(static_cast<eyecam::Calib>(hit.arg))
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
        case frame_updater::UpdateState::UpToDate: line = "up to date (" + u.current + ")"; break;
        case frame_updater::UpdateState::Available:
            line = "new release " + u.latest + (u.installable ? "" : " (not installable: " + u.reason + ")");
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
    AutostartWorker autostart;
    autostart.start();
    frame_updater::UpdateChecker updater(updaterConfig());
    std::fprintf(stderr, "[update] frameeyeosc-panel %s, checking %s\n", FRAMEEYEOSC_VERSION,
                 model.config.flag(key::kUpdateCheck) ? "on" : "off");
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

    gaze_fit::Session fit;
    // The eye log (frameeyeosc --record as a child), and what the panel last drew of it
    recorder::Recorder eyeLog;
    std::string drawnRecording;
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
    // The debug gaze dots: their socket, the three dot images (drawn once), which image each overlay has
    gaze_dots::Receiver dots;
    std::vector<uint8_t> dotImages[3];
    for (int kind = 0; kind < 3; ++kind) renderGazeDot(static_cast<DotKind>(kind), dotImages[kind]);
    int dotImageShown[2] = {-1, -1};
    bool dotsVisible[2] = {false, false};
    bool dotsOpenFailed = false;
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
        if ((often || watching) && ((visible && !wasVisible) || nowSeconds() >= lastStatusRead + readEvery)) {
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
                        applyHit(hit, model, panel, autostart, &updater, &fit, &vr, &eyeLog, &eyecamControl);
                        lastStamp = configStamp(model.configPath);
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
            in.dashboardOpen = dashboardOpen;
            in.fitActive = fit.active();
            // On a clock that runs through suspend: taking the headset off usually lets the Frame sleep
            const auto_recenter::Step step = recenter.update(bootSeconds(), in);
            if (!step.log.empty()) std::fprintf(stderr, "[fit] %s\n", step.log.c_str());
            if (step.start) startFit(fit, rewearMode(autoRecenter(model.config)), view, &vr);
        }

        // The eye fit: requests and results go through config.json and status.json; the target shows over the open
        // dashboard too (it is nearer)
        {
            gaze_fit::Dashboard dashboard;
            dashboard.open = dashboardOpen;
            dashboard.panelShown = visible;
            const gaze_fit::Actions actions = fit.tick(nowSeconds(), dashboard, model.status);
            // One "[fit]" line each (a try's numbers, and at the end the tilt)
            for (size_t start = 0; start < actions.log.size();) {
                const size_t end = std::min(actions.log.find('\n', start), actions.log.size());
                std::fprintf(stderr, "[fit] %s\n", actions.log.substr(start, end - start).c_str());
                start = end + 1;
            }
            // Cues follow what the target shows (a dot arriving, the countdown, the eyes-shut ending)
            const bool soundsOn = model.config.flag(key::kFitSounds);
            for (sounds::Cue cue : cues.update(fit.view(), actions)) {
                if (soundsOn) player.play(cue);
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
                if (!writeFitValues(model, actions.values, fit.view().mode)) fit.writeFailed();
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

        // The debug gaze dots: while the switch is on (and no eye fit runs), move a dot to where the gaze frameeyeosc
        // sends points, as each sample arrives. Only the transform changes; each dot's image is sent once
        {
            const bool wanted = model.config.flag(key::kGazeDebugDots) && !fit.active();
            const auto hideDots = [&]() {
                for (int i = 0; i < 2; ++i) {
                    vr.hideDot(i);
                    dotsVisible[i] = false;
                }
            };
            // One distance, dashboard open or closed (near enough to show over it; nothing jumps)
            const double wantedDistance = gaze_dots::dotDistance(model.config.number(key::kGazeDebugDotsDistanceM));
            if (dots.isOpen() && wantedDistance != dotDistance) {
                std::fprintf(stderr, "[dots] at %.2f m, %.1f mm wide (plain overlays, no sort order or flags)\n",
                             wantedDistance, gaze_dots::dotWidth(wantedDistance) * 1000);
                dotDistance = wantedDistance;
            }
            if (wanted && !dots.isOpen() && !dotsOpenFailed) {
                const size_t slash = model.statusPath.find_last_of('/');
                const std::string dir = slash == std::string::npos ? "." : model.statusPath.substr(0, slash);
                std::string error;
                if (dots.open(dir + "/" + gaze_dots::kSocketName, error)) {
                    dotIpd = vr.userIpdMeters();
                    // Set the distance now, so a packet in this same pass isn't placed at 0 m with the default width
                    dotDistance = wantedDistance;
                    std::fprintf(stderr, "[dots] at %.2f m, %.1f mm wide (plain overlays, no sort order or flags)\n",
                                 wantedDistance, gaze_dots::dotWidth(wantedDistance) * 1000);
                    std::fprintf(stderr, "[dots] listening in %s (IPD %.1f mm)\n", dir.c_str(), dotIpd * 1000);
                } else {
                    std::fprintf(stderr, "[dots] %s\n", error.c_str());
                    dotsOpenFailed = true;
                }
            } else if (!wanted && (dots.isOpen() || dotsOpenFailed)) {
                if (dots.isOpen()) std::fprintf(stderr, "[dots] off\n");
                dotDistance = 0.0;
                dots.close();
                hideDots();
                dotsOpenFailed = false;
            }
            gaze_dots::Packet packet;
            if (dots.isOpen() && dots.poll(packet)) {
                lastDotAt = nowSeconds();
                const auto place = [&](int index, int eye, DotKind kind, float x, float y) {
                    const gaze_dots::Pose pose = gaze_dots::dotPose(x, y, eye, dotIpd, dotDistance);
                    const int image = static_cast<int>(kind);
                    dotsVisible[index] = vr.showDot(index, pose.position.x, pose.position.y, pose.position.z,
                                                    pose.yawDeg, pose.pitchDeg, dotImages[image].data(),
                                                    kDotImageSize, dotImageShown[index] != image,
                                                    gaze_dots::dotWidth(dotDistance));
                    if (dotsVisible[index]) dotImageShown[index] = image;
                };
                if (packet.independent) {
                    place(0, -1, DotKind::Left, packet.gaze[0], packet.gaze[1]);
                    place(1, 1, DotKind::Right, packet.gaze[2], packet.gaze[3]);
                } else {
                    place(0, 0, DotKind::Both, packet.gaze[4], packet.gaze[5]);
                    vr.hideDot(1);
                    dotsVisible[1] = false;
                }
            } else if ((dotsVisible[0] || dotsVisible[1]) && nowSeconds() - lastDotAt > gaze_dots::kStaleSec) {
                // No samples (frameeyeosc stopped, or the eyes aren't tracked): no stale dots
                hideDots();
            }
        }

        // The developer tab "Eye capture" while eyecam-rec runs, dashboard open or closed: its status file read often
        // while it runs (once a second otherwise, to notice it), the reply to "start" / "stop", the light warning
        // before a start, and the full-view light during the bright and dark steps: faded in and, when its step ends,
        // out; hidden at once when recording ends, "stop" is sent, the file goes stale or the tab goes (and destroyed
        // with the other overlays at shutdown)
        {
            const bool running = model.eyecam.visible || vr.fillShown() || eyecamControl.busy();
            if (nowSeconds() >= lastEyecamRead + (running ? kEyecamReadSec : kEyecamIdleReadSec)) {
                lastEyecamRead = nowSeconds();
                model.eyecam.status = eyecam::readStatus(model.eyecamDir);
                model.eyecam.lastRun = eyecam::followRun(model.eyecam.lastRun, model.eyecam.status);
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
        if (visible && panel.tab() == PanelTab::Advanced && !panel.diagOpen() && !panel.historyOpen()) {
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
            if (firstSubmit) {
                firstSubmit = false;
                vr.logOverlayState("after the first draw");
            }
        }
        // The IP keypad and the eye capture's light warning don't stay open behind a closed dashboard
        if (!visible && wasVisible) {
            panel.closeHostEntry();
            panel.closeEyecamConfirm();
            // ...nor the setup's done screen ("next time it shows the usual page")
            model.eyecam.flow.closed();
        }
        wasVisible = visible;
        // Every display frame while the fit's target or the debug dots are up: with the target, paced by the
        // compositor itself (a fixed sleep plus the loop's work fell behind the display, and the ring stuttered)
        // (the dots only while packets arrive: with none for kStaleSec, the usual slow poll)
        const bool dotsLive = dots.isOpen() && nowSeconds() - lastDotAt <= gaze_dots::kStaleSec;
        // (and while the eye capture's light fades, so its alpha steps are small)
        const bool everyFrame = fit.active() || dotsLive || lightMoving;
        // While eyecam-rec runs, as often as with the panel open, so the light follows its steps closely
        const bool eyecamLive = model.eyecam.visible || vr.fillShown() || eyecamControl.busy();
        if (!(targetUp && vr.waitFrameSync(kFrameSyncTimeoutMs))) {
            sleepInterruptible(everyFrame ? kFitPollSec : ((visible || eyecamLive) ? kPanelPollSec : kClosedPollSec));
        }
    }

    // The same shutdown for SIGTERM / SIGINT, SteamVR quitting, vrserver gone, "close" and "Quit"; a recording is
    // stopped (and its file written out) first. The eye capture's light goes first of all (shutdown also destroys it)
    if (vr.fillShown()) std::fprintf(stderr, "[eyecam] light off (quitting)\n");
    vr.hideFill();
    eyeLog.shutdown();
    vr.shutdown();
    autostart.stop();
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

    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return 2;
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
        case Options::Mode::Overlay: break;
    }
    return runOverlay(options);
}
