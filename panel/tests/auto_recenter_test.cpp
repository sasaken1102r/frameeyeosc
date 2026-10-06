// Tests for re-centering by itself when the headset is put on (auto_recenter.cpp): when it arms, when it fires, and
// what keeps it from firing. Built with the panel as auto-recenter-test; exits non-zero on failure.
#include "auto_recenter.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

/**
 * Record a failed check.
 * @param ok the check
 * @param what what was checked
 * @param line where
 */
void check(bool ok, const char* what, int line) {
    if (ok) return;
    ++gFailures;
    std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
}

#define CHECK(condition) check((condition), #condition, __LINE__)

using auto_recenter::Inputs;
using auto_recenter::Watcher;

/** A watcher fed the way the panel's loop feeds it: every 0.25 s, with the state set on `in`. */
struct Run {
    Watcher watcher;
    Inputs in;
    double now = 100.0;
    int starts = 0;                 ///< how many times it said to start
    double startedAt = -1.0;        ///< when it last did
    std::vector<std::string> logs;  ///< every line it logged

    /** A worn headset with a gaze fit, frameeyeosc running and the dashboard closed. */
    Run() {
        in.enabled = true;
        in.fitted = true;
        in.running = true;
        in.tracking = true;
    }

    /**
     * Let time pass with the state as it is.
     * @param seconds how long
     */
    void wait(double seconds) {
        const double end = now + seconds;
        while (now < end - 1e-9) {
            const auto_recenter::Step step = watcher.update(now, in);
            if (!step.log.empty()) logs.push_back(step.log);
            if (step.start) {
                ++starts;
                startedAt = now;
                // The fit runs for a few seconds after it starts, as in the panel
                in.fitActive = true;
            }
            now += 0.25;
        }
    }

    /**
     * Whether a logged line contains a text.
     * @param text the text
     * @return how many lines do
     */
    int logged(const char* text) const {
        int count = 0;
        for (const std::string& line : logs) count += line.find(text) != std::string::npos ? 1 : 0;
        return count;
    }

    /** The re-center that started has finished. */
    void finishFit() { in.fitActive = false; }
};

void testStart() {
    // The first wearing: armed from the start, fires once the eyes have been tracked for 3 s
    Run run;
    run.wait(2.9);
    CHECK(run.starts == 0);
    run.wait(0.5);
    CHECK(run.starts == 1);
    CHECK(run.startedAt >= 103.0 && run.startedAt <= 103.25);
    CHECK(!run.watcher.armed());
    CHECK(run.logged("auto re-center: starting") == 1);
    // Once only
    run.finishFit();
    run.wait(60);
    CHECK(run.starts == 1);
    // Started with the headset off: fires 3 s after it is put on
    Run off;
    off.in.tracking = false;
    off.wait(30);
    CHECK(off.starts == 0 && off.watcher.armed());
    off.in.tracking = true;
    off.wait(2.9);
    CHECK(off.starts == 0);
    off.wait(0.5);
    CHECK(off.starts == 1);
}

void testShortGapIgnored() {
    Run run;
    run.wait(5);
    CHECK(run.starts == 1);
    run.finishFit();
    // Hiccups of 1.2-2.2 s (and anything under 5 s) are not taking the headset off
    for (double gap : {1.25, 2.25, 4.75}) {
        run.in.tracking = false;
        run.wait(gap);
        run.in.tracking = true;
        run.wait(10);
    }
    CHECK(run.starts == 1);
    CHECK(run.logged("put on") == 0);
}

void testLongGapArms() {
    Run run;
    run.wait(5);
    run.finishFit();
    // Taken off for 86 s, put back on
    run.in.tracking = false;
    run.wait(86);
    CHECK(!run.watcher.armed() && run.starts == 1);
    run.in.tracking = true;
    run.wait(0.25);
    CHECK(run.watcher.armed());
    CHECK(run.logged("put on (tracking was off 86.") == 1);
    run.wait(2.5);
    CHECK(run.starts == 1);
    run.wait(1);
    CHECK(run.starts == 2);
    // Exactly the 5 s it takes also counts
    run.finishFit();
    run.in.tracking = false;
    run.wait(auto_recenter::kOffSec);
    run.in.tracking = true;
    run.wait(4);
    CHECK(run.starts == 3);
}

void testSettleRestarts() {
    Run run;
    run.in.tracking = false;
    run.wait(1);
    // On for 2 s, lost for a moment (the headset still being adjusted), then on again: 3 s from the return
    run.in.tracking = true;
    run.wait(2);
    run.in.tracking = false;
    run.wait(0.5);
    CHECK(run.logged("eyes lost") == 1);
    run.in.tracking = true;
    const double back = run.now;
    run.wait(2.9);
    CHECK(run.starts == 0);
    run.wait(0.5);
    CHECK(run.starts == 1 && run.startedAt >= back + auto_recenter::kSettleSec);
}

void testDashboardOpenBlocks() {
    Run run;
    run.in.dashboardOpen = true;
    run.wait(20);
    CHECK(run.starts == 0 && run.watcher.armed());
    // Closed: waits a second more
    run.in.dashboardOpen = false;
    run.wait(0.75);
    CHECK(run.starts == 0);
    run.wait(0.5);
    CHECK(run.starts == 1);
    // Opened for a moment just before it would fire: the second starts over
    Run blink;
    blink.wait(2.5);
    blink.in.dashboardOpen = true;
    blink.wait(0.25);
    blink.in.dashboardOpen = false;
    blink.wait(0.75);
    CHECK(blink.starts == 0);
    blink.wait(0.5);
    CHECK(blink.starts == 1);
}

void testNotRunningFreezes() {
    // frameeyeosc restarted (an update) while worn: no dot, however long it took
    Run run;
    run.wait(5);
    run.finishFit();
    run.in.running = false;
    run.in.tracking = false;
    run.wait(40);
    run.in.running = true;
    run.in.tracking = false;  // starting up: no samples yet for a moment
    run.wait(1);
    run.in.tracking = true;
    run.wait(10);
    CHECK(run.starts == 1);
    CHECK(run.logged("put on") == 0);
    // Armed but frameeyeosc stopped: nothing fires, and the settle time doesn't run meanwhile
    Run armed;
    armed.in.running = false;
    armed.wait(30);
    CHECK(armed.starts == 0 && armed.watcher.armed());
    armed.in.running = true;
    armed.wait(2.9);
    CHECK(armed.starts == 0);
    armed.wait(0.5);
    CHECK(armed.starts == 1);
    // The off time only counts while it runs: 3 s off, 60 s stopped, 1 s off again is not a put-on
    Run split;
    split.wait(5);
    split.finishFit();
    split.in.tracking = false;
    split.wait(3);
    split.in.running = false;
    split.wait(60);
    split.in.running = true;
    split.wait(1);
    split.in.tracking = true;
    split.wait(10);
    CHECK(split.starts == 1);
    // ...but 3 + 3 s is
    split.in.tracking = false;
    split.wait(3);
    split.in.running = false;
    split.wait(60);
    split.in.running = true;
    split.wait(3);
    split.in.tracking = true;
    split.wait(4);
    CHECK(split.starts == 2);
}

void testSuspendCountsAsOff() {
    // Taken off, and the Frame slept a minute (no updates at all; the clock keeps running): put on again, it arms.
    // The first status after waking up is the stale one from before, read as "not running"
    Run stale;
    stale.wait(5);
    stale.finishFit();
    stale.in.tracking = false;
    stale.wait(3);
    stale.now += 60;
    stale.in.running = false;
    stale.wait(0.25);
    stale.in.running = true;
    stale.in.tracking = true;
    stale.wait(0.25);
    CHECK(stale.watcher.armed() && stale.logged("put on (tracking was off 63.") == 1);
    stale.wait(4);
    CHECK(stale.starts == 2);
    // ...or the status from before the sleep had not been read again yet (still running, tracking off)
    Run cached;
    cached.wait(5);
    cached.finishFit();
    cached.in.tracking = false;
    cached.wait(3);
    cached.now += 60;
    cached.in.tracking = true;
    cached.wait(4);
    CHECK(cached.starts == 2 && cached.logged("put on (tracking was off 63.") == 1);
    // The last status before sleeping still said "tracking" (read just before the headset came off and the Frame
    // slept), and tracking again after waking: the suspend counts as off, so it arms
    Run stale2;
    stale2.wait(5);
    stale2.finishFit();
    stale2.now += 600;
    stale2.wait(4);
    CHECK(stale2.starts == 2 && stale2.logged("put on (tracking was off 600.") == 1);
    // A gap under 10 s is not sleep: nothing
    Run shortGap;
    shortGap.wait(5);
    shortGap.finishFit();
    shortGap.now += 8;
    shortGap.wait(10);
    CHECK(shortGap.starts == 1 && shortGap.logged("put on") == 0);
}

void testDisabledDisarms() {
    // Switched off: disarmed without firing, and put-ons don't arm it
    Run run;
    run.in.enabled = false;
    run.wait(10);
    CHECK(run.starts == 0 && !run.watcher.armed());
    CHECK(run.logged("skipped (switched off)") == 1);
    run.in.tracking = false;
    run.wait(20);
    run.in.tracking = true;
    run.wait(10);
    CHECK(run.starts == 0 && !run.watcher.armed() && run.logged("put on") == 0);
    // Switched on again: from the next put-on
    run.in.enabled = true;
    run.wait(10);
    CHECK(run.starts == 0);
    run.in.tracking = false;
    run.wait(20);
    run.in.tracking = true;
    run.wait(4);
    CHECK(run.starts == 1);
    // No fit yet, or the fit values locked: nothing to do
    Run unfitted;
    unfitted.in.fitted = false;
    unfitted.wait(10);
    CHECK(unfitted.starts == 0 && unfitted.logged("no gaze fit") == 1);
    Run locked;
    locked.in.locked = true;
    locked.wait(10);
    CHECK(locked.starts == 0 && locked.logged("locked") == 1);
    // A fit started by hand while armed covers this wearing
    Run byHand;
    byHand.wait(1);
    byHand.in.fitActive = true;
    byHand.wait(20);
    byHand.in.fitActive = false;
    byHand.wait(10);
    CHECK(byHand.starts == 0 && byHand.logged("an eye fit was started") == 1);
}

void testOneShotPerPutOn() {
    // The re-center failed or was stopped: no retry until the headset is put on again
    Run run;
    run.wait(4);
    CHECK(run.starts == 1);
    run.finishFit();
    run.wait(120);
    CHECK(run.starts == 1);
    run.in.tracking = false;
    run.wait(19);
    run.in.tracking = true;
    run.wait(4);
    CHECK(run.starts == 2);
    run.finishFit();
    run.wait(120);
    CHECK(run.starts == 2);
    // One line per event, not one per update
    CHECK(run.logged("auto re-center: starting") == 2);
    CHECK(run.logged("put on") == 1);
    CHECK(run.logs.size() == 3);
}

void testWornFor() {
    // For the records: how long the eyes have been tracked, and how long they weren't before that
    Run run;
    CHECK(std::isnan(run.watcher.trackedSec()) && std::isnan(run.watcher.offBeforeSec()));  // nothing seen yet
    run.wait(6);
    CHECK(std::fabs(run.watcher.trackedSec() - 5.75) < 1e-6);
    run.in.tracking = false;
    run.wait(30);
    CHECK(std::isnan(run.watcher.trackedSec()) && std::isnan(run.watcher.offBeforeSec()));
    run.in.tracking = true;
    run.wait(10);
    CHECK(std::fabs(run.watcher.trackedSec() - 9.75) < 1e-6);
    CHECK(std::fabs(run.watcher.offBeforeSec() - 30.0) < 1e-6);
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testStart();
    testShortGapIgnored();
    testLongGapArms();
    testSettleRestarts();
    testDashboardOpenBlocks();
    testNotRunningFreezes();
    testSuspendCountsAsOff();
    testDisabledDisarms();
    testOneShotPerPutOn();
    testWornFor();
    if (gFailures == 0) std::printf("auto-recenter-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
