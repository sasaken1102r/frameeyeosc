#include "eye_rate.h"

#include <cstdio>

namespace eye_rate {

bool Setting::operator==(const Setting& other) const {
    if (exists != other.exists) return false;
    if (!exists) return true;
    return min == other.min || (std::isnan(min) && std::isnan(other.min));
}

Choice choice(const Setting& setting) {
    return setting.min >= kFull ? Choice::Full : Choice::SteamVr;
}

bool low(const EyeStatus& s) {
    return s.running && s.tracking && std::isfinite(s.trackerRate) && s.trackerRate < kLowTrackerRate;
}

bool offered(const View& v, const EyeStatus& s) {
    if (trackerRateCause(s) != TrackerRateCause::Tracker || !v.setting.exists) return false;
    return v.refused || !(v.setting.min >= kFull);
}

void pressed(View& v, bool written, double now) {
    if (written) {
        v.pressedAt = now;
        v.refusedAt = -1.0;
    } else {
        v.pressedAt = -1.0;
        v.refusedAt = now;
        v.refused = true;
    }
}

bool update(View& v, const EyeStatus& s, double now) {
    const Line before = v.line;
    // The check once the confirmation has shown its time: still low is SteamVR not taking it
    if (v.pressedAt >= 0 && now - v.pressedAt >= kCheckSec) {
        if (low(s)) {
            v.refusedAt = v.pressedAt + kCheckSec;
            v.refused = true;
        }
        v.pressedAt = -1.0;
    }
    if (v.refusedAt >= 0 && now - v.refusedAt >= kRefusedSec) v.refusedAt = -1.0;
    // Fine again (or nothing to tell): the next low rate starts afresh
    if (!low(s) && v.refusedAt < 0) v.refused = false;
    if (v.pressedAt >= 0) {
        v.line = Line::Done;
    } else if (v.refusedAt >= 0) {
        v.line = Line::Refused;
    } else {
        v.line = offered(v, s) ? Line::Offer : Line::None;
    }
    return v.line != before;
}

bool rowShown(const View& v, TrackerRateCause cause) {
    return cause != TrackerRateCause::None || v.line == Line::Done || v.line == Line::Refused;
}

std::string signature(const View& v) {
    char text[64];
    std::snprintf(text, sizeof(text), "%d|%.3f|%d", v.setting.exists ? 1 : 0, v.setting.exists ? v.setting.min : 0.0,
                  static_cast<int>(v.line));
    return text;
}

}  // namespace eye_rate
