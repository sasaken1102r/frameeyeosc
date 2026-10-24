// Re-fitting by itself when the headset is put on (auto_recenter): straight ahead shifts a little each time the
// headset is put on (up to 6.7° up-down and 3.2° sideways seen between wearings), and it is the one thing measured
// in one wear that holds for the rest of it, so the re-wear fit (the one dot in the middle) is run once the eyes are
// tracked again. "Put on" is read from
// status.json: frameeyeosc's `tracking` goes false a second after the last eye sample, and the Frame's eye server
// stops delivering samples while the headset is off. Nothing here talks to OpenVR or reads files, so it can be
// tested on its own (auto_recenter_test.cpp).
#pragma once

#include <cmath>
#include <string>

namespace auto_recenter {

/** Tracking has to have been off this long (while frameeyeosc ran) for its return to count as putting the headset
 *  on. Taking it off was seen as gaps of 19-344 s; the tracker's own hiccups last 1.2-2.2 s. */
constexpr double kOffSec = 5.0;
/** No update for longer than this is the Frame sleeping: it counts as off (see Watcher::update). */
constexpr double kSleepGapSec = 10.0;
/** Tracking has to have been on this long without a break before the dot shows (the headset is still being
 *  adjusted). */
constexpr double kSettleSec = 3.0;
/** The dashboard has to have been closed this long (and SteamVR connected: the dashboard can't be read before). */
constexpr double kClosedSec = 1.0;
/** SteamVR opens its dashboard by itself a few seconds after it starts, and the user closes it before doing anything
 *  else: a re-center started before that was stopped right after it began (Session::tick, Failure::DashboardOpened).
 *  So after the panel connects to SteamVR, and after the Frame wakes up, a re-center waits until the dashboard has
 *  been open (then closed kClosedSec, as always), this long at most: SteamVR may not open it (the panel restarted by
 *  an update while the headset is worn and the dashboard closed). Seen opening 5.1 s after the panel connected at
 *  boot (2026-10-14 19:55:43.9 -> 19:55:49.0; the panel tries to connect every 3 s, SteamVR's dashboard process had
 *  started 1.3 s before) and 3.2 s after SteamVR left its standby (19:50:16.8 -> 19:50:20.0); 45 s leaves a wide
 *  margin for a slower start (the first after an update), and a dashboard that never comes only delays the re-center
 *  that long. */
constexpr double kStartupDashboardWaitSec = 45.0;
/** A re-center stopped because the dashboard opened anyway is tried again this many more times in a wearing, each once
 *  the dashboard has been closed kClosedSec (the user opened it just as it began, or SteamVR did later than
 *  kStartupDashboardWaitSec); a user who keeps opening it isn't asked again after these. */
constexpr int kDashboardRetries = 2;
/** An eye Valve's eye server reads below this openness (status.json raw.openness) only counts in a fit once the eye
 *  camera's eyelid has taken over for it: the same floor as OPEN_FLOOR in capture.rs. */
constexpr double kOpenFloor = 0.3;
/** ...read that low this long without a break (a blink lasts 70-200 ms; status.json is read every 0.5 s while the
 *  panel is closed). */
constexpr double kShutSec = 1.0;
/** Held back for the eye cameras' warm-up, it starts this long after the warm-up ends (or the eye reads open): from
 *  then frameeyeosc takes the camera's eyelid for an eye it reads closing after CAMERA_DOUBT_GRACE (main.rs), 0.3 s
 *  but at least 8 of the camera's frames (0.53 s at 15 frames a second). */
constexpr double kCameraMarginSec = 1.0;
/** ...and starts anyway this long after it would have started (eyecam-rec's warm-up is 30 s). */
constexpr double kCameraWaitSec = 45.0;

/** What the watcher needs to know, every loop. */
struct Inputs {
    bool enabled = true;         ///< auto_recenter is not "off"
    bool fitted = false;         ///< there is a gaze fit to re-center
    bool locked = false;         ///< a fit key is set on frameeyeosc's command line
    bool running = false;        ///< frameeyeosc is running
    bool tracking = false;       ///< eye data is coming in (status.json)
    bool vrConnected = false;    ///< the panel is connected to SteamVR (while it isn't, the dashboard reads closed
                                 ///< but may open any moment: it counts as open)
    bool dashboardOpen = false;  ///< the SteamVR dashboard is open
    bool fitActive = false;      ///< an eye fit is running
    bool valveShut[2] = {false, false};  ///< Valve's eye server reads the eye below kOpenFloor (left, right)
    bool camerasWarming = false;         ///< the eye cameras' eyelids wait for eyecam-rec's warm-up
                                         ///< (eyecam::recenterAwaitsWarmup)
    bool camerasCalibrating = false;     ///< the eye cameras are calibrating (eyecam::calibrationUnderWay)
};

/** What to do after an update. */
struct Step {
    bool start = false;  ///< start the re-wear fit now
    std::string log;     ///< a line for the log (without "[fit] "), or empty
};

/**
 * Watches for the headset being put on and says when to re-center, once per wearing. Armed at start (the first
 * wearing) and whenever tracking comes back after at least kOffSec off; fires when armed, tracking has been on for
 * kSettleSec, SteamVR connected and the dashboard closed for kClosedSec, and re-centering can run. After the panel
 * connects to SteamVR and after the Frame wakes up, it also waits for SteamVR's own startup dashboard to have opened
 * (and been closed), kStartupDashboardWaitSec at most from the connect or the wake; a re-wear otherwise doesn't. A try
 * the dashboard stopped is tried again, kDashboardRetries times at most in a wearing (fitEnded); any other end is
 * final for the wearing. While Valve's eye server reads an eye shut
 * (it can get stuck at 0 for a whole wear) and the eye cameras are still warming up, no sample would count, so it
 * waits for the warm-up (up to kCameraWaitSec): the eye camera's eyelid counts for that eye after it. While the eye
 * cameras calibrate (the user looking up and down as told), it waits for the calibration to end, however long.
 * Time while frameeyeosc is not running counts for nothing (a restart while worn does not look like putting the
 * headset on); the time since the last update counts for the state seen then, so a suspend right after taking the
 * headset off counts as off.
 */
class Watcher {
public:
    /**
     * Move on.
     * @param now seconds on a clock that keeps running while the Frame sleeps (CLOCK_BOOTTIME): taking the headset
     *            off usually lets the Frame suspend, and that time has to count as off
     * @param in the state now
     * @return whether to start re-centering, and what to log
     */
    Step update(double now, const Inputs& in);

    /** @return true while waiting to re-center for this wearing */
    bool armed() const { return armed_; }

    /**
     * Whether an eye fit ending now would be tried again (see fitEnded).
     * @param byItself it is the re-center this watcher started (not one started from the panel)
     * @param byDashboard it was stopped because the dashboard opened (Failure::DashboardOpened)
     * @return true if fitEnded would arm again
     */
    bool willRetry(bool byItself, bool byDashboard) const {
        return byItself && byDashboard && retries_ < kDashboardRetries;
    }

    /**
     * An eye fit ended. The re-center this watcher started, stopped because the dashboard opened: wait again, to try
     * once the dashboard has been closed kClosedSec, if this wearing has a retry left. Anything else changes nothing
     * (one try per wearing; a fit from the panel is the user's).
     * @param byItself it is the re-center this watcher started
     * @param byDashboard it was stopped because the dashboard opened
     * @return the line for the log (without "[fit] "), or empty
     */
    std::string fitEnded(bool byItself, bool byDashboard);

    /**
     * For the records (report.h): how long the eyes have been tracked without a break, as of the last update.
     * @return seconds, NaN while they aren't (or before the first update with frameeyeosc running)
     */
    double trackedSec() const { return started_ && tracking_ ? onFor_ : NAN; }

    /**
     * ...and how long they weren't before that (the headset off, or the Frame asleep).
     * @return seconds, NaN while they aren't tracked
     */
    double offBeforeSec() const { return started_ && tracking_ ? offFor_ : NAN; }

private:
    bool started_ = false;       ///< update has run once
    bool armed_ = true;          ///< waiting to re-center (the first wearing is armed from the start)
    int retries_ = 0;            ///< tries again this wearing after the dashboard stopped one (fitEnded)
    bool wasRunning_ = false;    ///< frameeyeosc ran at the last update
    bool tracking_ = false;      ///< tracking at the last update while frameeyeosc ran
    double lastNow_ = 0.0;       ///< the last update
    double onFor_ = 0.0;         ///< how long tracking has been on, counting only while frameeyeosc ran...
    double offFor_ = 0.0;        ///< ...or off
    bool dashboardOpen_ = true;  ///< the dashboard at the last update...
    double closedAt_ = 0.0;      ///< ...and when it was last closed
    bool connected_ = false;     ///< SteamVR was connected at the last update
    bool startupWait_ = false;   ///< waiting for SteamVR's startup dashboard to open (kStartupDashboardWaitSec)...
    double startupFrom_ = 0.0;   ///< ...since the connect or the wake...
    const char* startupAfter_ = "";  ///< ...which ("connecting" / "waking up")...
    bool startupLogged_ = false;     ///< ...and a re-center has waited for it (said once)
    bool calibLogged_ = false;   ///< a re-center has waited for the eye cameras' calibration running now (said once)
    double shutSince_ = NAN;     ///< since when an eye has read shut on Valve's eye server (NaN: neither does)
    double holdSince_ = NAN;     ///< since when this wearing's start is held back for the eye cameras (NaN: it isn't)
    double clearSince_ = NAN;    ///< ...and since when that no longer holds (NaN: it still does)

    /**
     * Stop waiting, and say why.
     * @param why the reason for the log
     * @return the log line
     */
    std::string disarm(const char* why);

    /**
     * SteamVR was connected, or the Frame woke up: SteamVR's dashboard is about to open by itself.
     * @param now the time
     * @param after what happened, for the log ("connecting" / "waking up")
     */
    void awaitStartupDashboard(double now, const char* after);
};

}  // namespace auto_recenter
