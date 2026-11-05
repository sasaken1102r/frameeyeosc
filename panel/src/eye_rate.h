// The eye cameras' speed as SteamVR sets it (steamvr / eyeTrackingRateMin). SteamOS 0.5.7's SteamVR added the setting
// (default 15, with eyeTrackingRateMax at 90, from frame_hmd's own defaults) and can then run the eye cameras at 15 a
// second, which the left column shows as "Eye data low" with the eye tracker as the cause. Writing 90 there through
// IVRSettings takes effect at once (no SteamVR restart); removing the key gives SteamVR its default back. An older
// SteamVR has no such setting, and then nothing here shows.
//
// What is offered and when lives here, without OpenVR, so it can be tested on its own (eye_rate_test.cpp): the left
// column's line (the offer to set 90, the confirmation, "not taken" when the rate stays low) and which choice the
// Advanced tab's "Having trouble" row shows. VrOverlay reads and writes the setting; the panel's loop feeds the rest.
#pragma once

#include "status.h"

#include <cmath>
#include <string>

namespace eye_rate {

/** The SteamVR settings section and key. */
constexpr const char* kSection = "steamvr";
constexpr const char* kKey = "eyeTrackingRateMin";
/** The rate "Set 90" / "Always 90 a second" writes (the cameras' full speed, eyeTrackingRateMax's default). */
constexpr double kFull = 90.0;
/** After "Set 90" the confirmation shows this long; then, if the eye data is still low, SteamVR didn't take it. */
constexpr double kCheckSec = 5.0;
/** "SteamVR didn't take it" shows this long, then the offer again. */
constexpr double kRefusedSec = 4.0;
/** The setting is read again this often while the panel shows (and right after the panel writes it). */
constexpr double kReadSec = 2.0;

/** The setting as SteamVR has it. */
struct Setting {
    bool exists = false;  ///< this SteamVR has the setting (set, or with a default)
    double min = NAN;     ///< eyeTrackingRateMin (NaN while it doesn't exist)

    /** @return true if the same */
    bool operator==(const Setting& other) const;
    /** @return true if different */
    bool operator!=(const Setting& other) const { return !(*this == other); }
};

/** The Advanced tab's two choices. */
enum class Choice {
    Full,     ///< "Always 90 a second": eyeTrackingRateMin 90 (or more)
    SteamVr,  ///< "Let SteamVR decide": anything lower (its default, or a value set elsewhere)
};

/** What the left column's line under "Eye data" shows besides today's reason. */
enum class Line {
    None,     ///< nothing of this: the reason as always (or no line)
    Offer,    ///< "SteamVR slows the eye cameras" with "Set 90" (pressable)
    Done,     ///< "Set the eye cameras to 90 a second" (for kCheckSec after the press)
    Refused,  ///< "SteamVR didn't take it" (for kRefusedSec)
};

/** The state the panel's loop keeps (PanelModel::eyeRate). */
struct View {
    Setting setting;           ///< as last read (exists false without SteamVR, or before it is read)
    double pressedAt = -1.0;   ///< when "Set 90" was pressed (-1 = not waiting for its check)
    double refusedAt = -1.0;   ///< when SteamVR was found not to have taken it (-1 = not showing that)
    bool refused = false;      ///< ...and the offer stays (even with the setting at 90) until the rate is fine again
    Line line = Line::None;    ///< as of the last update
};

/**
 * The choice the Advanced tab's row shows for a setting: 90 or more is "Always 90 a second".
 * @param setting the setting (it exists)
 * @return the choice
 */
Choice choice(const Setting& setting);

/**
 * Whether the eye data is known to be low now (frameeyeosc running and tracking, below kLowTrackerRate).
 * @param s the status
 * @return true if low
 */
bool low(const EyeStatus& s);

/**
 * Whether the left column offers "Set 90": the eye data is low because of the eye tracker (not frameeyeosc), this
 * SteamVR has the setting, and it is below 90 (or SteamVR didn't take the 90 written a moment ago).
 * @param v the state
 * @param s the status
 * @return true if offered
 */
bool offered(const View& v, const EyeStatus& s);

/**
 * "Set 90" was pressed and written (or the write failed: then "not taken" shows at once).
 * @param v the state
 * @param written the write went through
 * @param now seconds (any monotonic clock, the same as update's)
 */
void pressed(View& v, bool written, double now);

/**
 * Move the state on: the confirmation's time, the check after it (still low because of the eye tracker is "not
 * taken"; low because frameeyeosc is slow then is not), "not taken"'s time, and the line to show.
 * @param v the state
 * @param s the status now
 * @param now seconds
 * @return true if the line changed
 */
bool update(View& v, const EyeStatus& s, double now);

/**
 * Whether the left column keeps the line's row under "Eye data": always while a reason shows, and while the
 * confirmation or "not taken" shows (even with the rate already fine).
 * @param v the state
 * @param cause who was slow now
 * @return true if the row is kept
 */
bool rowShown(const View& v, TrackerRateCause cause);

/**
 * What the panel draws of this, as text, so it is redrawn only when that changes.
 * @param v the state
 * @return the text
 */
std::string signature(const View& v);

}  // namespace eye_rate
