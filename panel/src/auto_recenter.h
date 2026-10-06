// Re-fitting by itself when the headset is put on (auto_recenter): straight ahead shifts a little each time the
// headset is put on (up to 6.7° up-down and 3.2° sideways seen between wearings), and so does its tilt, while the
// gains stay within a few %, so the re-wear fit (straight ahead, and the side dots for the tilt, or the one dot only)
// is run once the eyes are tracked again. "Put on" is read from
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
/** The dashboard has to have been closed this long. */
constexpr double kClosedSec = 1.0;

/** What the watcher needs to know, every loop. */
struct Inputs {
    bool enabled = true;         ///< auto_recenter is not "off"
    bool fitted = false;         ///< there is a gaze fit to re-center
    bool locked = false;         ///< a fit key is set on frameeyeosc's command line
    bool running = false;        ///< frameeyeosc is running
    bool tracking = false;       ///< eye data is coming in (status.json)
    bool dashboardOpen = false;  ///< the SteamVR dashboard is open
    bool fitActive = false;      ///< an eye fit is waiting or running
};

/** What to do after an update. */
struct Step {
    bool start = false;  ///< start the re-wear fit now
    std::string log;     ///< a line for the log (without "[fit] "), or empty
};

/**
 * Watches for the headset being put on and says when to re-center, once per wearing. Armed at start (the first
 * wearing) and whenever tracking comes back after at least kOffSec off; fires when armed, tracking has been on for
 * kSettleSec, the dashboard closed for kClosedSec, and re-centering can run. Time while frameeyeosc is not running
 * counts for nothing (a restart while worn does not look like putting the headset on); the time since the last
 * update counts for the state seen then, so a suspend right after taking the headset off counts as off.
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
    bool wasRunning_ = false;    ///< frameeyeosc ran at the last update
    bool tracking_ = false;      ///< tracking at the last update while frameeyeosc ran
    double lastNow_ = 0.0;       ///< the last update
    double onFor_ = 0.0;         ///< how long tracking has been on, counting only while frameeyeosc ran...
    double offFor_ = 0.0;        ///< ...or off
    bool dashboardOpen_ = true;  ///< the dashboard at the last update...
    double closedAt_ = 0.0;      ///< ...and when it was last closed

    /**
     * Stop waiting, and say why.
     * @param why the reason for the log
     * @return the log line
     */
    std::string disarm(const char* why);
};

}  // namespace auto_recenter
