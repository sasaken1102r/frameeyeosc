// The eye fit's words on the panel.
#include "fit_text.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

/**
 * printf into a string.
 * @param format the format
 * @param args its values
 * @return the text
 */
template <typename... Args>
std::string format(const char* format, Args... args) {
    char text[320];
    std::snprintf(text, sizeof(text), format, args...);
    return text;
}

/**
 * Join parts with the language's separator.
 * @param t texts
 * @param parts the parts
 * @return "a・b・c"
 */
std::string join(const UiText& t, const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) out += t.failDetailSeparator;
        out += part;
    }
    return out;
}

/**
 * "where: what", with the first letter capitalized (for English).
 * @param where the place
 * @param what the numbers
 * @return the line
 */
std::string line(const std::string& where, const std::string& what) {
    std::string out = where + ": " + what;
    if (!out.empty() && std::islower(static_cast<unsigned char>(out[0]))) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

}  // namespace

const char* pointName(const UiText& t, gaze_fit::Point point) {
    switch (point) {
        case gaze_fit::Point::Center: return t.pointCenter;
        case gaze_fit::Point::Up: return t.pointUp;
        case gaze_fit::Point::Down: return t.pointDown;
        case gaze_fit::Point::Left: return t.pointLeft;
        case gaze_fit::Point::Right: return t.pointRight;
        case gaze_fit::Point::Closed: return t.pointClosed;
    }
    return "";
}

std::string failureText(const UiText& t, const gaze_fit::View& fit) {
    using gaze_fit::Failure;
    switch (fit.failure) {
        case Failure::None: return "";
        case Failure::Cancelled: return t.failCancelled;
        case Failure::Left: return t.failLeft;
        case Failure::DashboardOpened: return t.failDashboardOpened;
        case Failure::NotRunning: return t.failNotRunning;
        case Failure::NoResult: return t.failNoResult;
        case Failure::Unsteady: return format(t.failUnsteadyFormat, pointName(t, fit.point));
        case Failure::NotClosed: return t.failNotClosed;
        case Failure::NoMovement: return format(t.failNoMovementFormat, pointName(t, fit.point));
        case Failure::NoLidRange: return t.failLidRange;
        case Failure::WriteFailed: return t.failWrite;
    }
    return "";
}

std::string failureDetailText(const UiText& t, const gaze_fit::View& fit) {
    using gaze_fit::Failure;
    const gaze_fit::FailureDetail& d = fit.detail;
    const std::string dot = format(t.failDetailPointFormat, pointName(t, fit.point));
    // "26/26 usable at 15 a second, 16 needed", or from an older frameeyeosc "30/45"
    std::string samples;
    if (d.last.received > 0) {
        const std::string rate = std::isfinite(d.last.rateHz) ? format(t.failDetailRateFormat, d.last.rateHz) : "";
        samples = format(t.failDetailSamplesRateFormat, d.last.samples, d.last.received, rate.c_str(),
                         gaze_fit::samplesNeeded(d.last));
    } else {
        samples = format(t.failDetailSamplesFormat, d.last.samples, gaze_fit::kMinSamples);
    }
    const std::string tries = format(t.failDetailTriesFormat, d.tries);
    const char* eyes[2] = {t.failDetailEyeLeft, t.failDetailEyeRight};
    switch (fit.failure) {
        case Failure::Unsteady: {
            const bool known = d.last.samples > 0 && std::isfinite(d.last.spread);
            const std::string spread = known ? format("%.1f°", d.last.spread * gaze_fit::kFullScaleDeg) : "—";
            return line(dot, join(t, {samples, format(t.failDetailSpreadFormat, spread.c_str(),
                                                        gaze_fit::kMaxSpread * gaze_fit::kFullScaleDeg),
                                      tries}));
        }
        case Failure::NotClosed: {
            std::vector<std::string> parts;
            if (d.last.samples < gaze_fit::samplesNeeded(d.last)) parts.push_back(samples);
            for (int eye = 0; eye < 2; ++eye) {
                parts.push_back(format(t.failDetailClosedFormat, eyes[eye], d.last.openness[eye], d.closedBelow[eye]));
            }
            parts.push_back(tries);
            return line(pointName(t, gaze_fit::Point::Closed), join(t, parts));
        }
        case Failure::NoMovement: {
            if (!std::isfinite(d.movedDeg)) return "";
            const std::string where = d.eye < 0 ? dot : (d.eye == 0 ? t.failDetailSidewaysLeft : t.failDetailSidewaysRight);
            return line(where, format(t.failDetailMovedFormat, d.movedDeg, d.neededDeg));
        }
        case Failure::NoLidRange: {
            if (d.eye < 0) return "";
            const std::string where = format(t.failDetailLidWhereFormat, eyes[d.eye], pointName(t, d.lidPoint));
            if (!std::isfinite(d.lidOpen)) return line(where, t.failDetailLidNone);
            return line(where, format(t.failDetailLidFormat, d.lidOpen, d.lidClosed, d.lidOpen - d.lidClosed,
                                      gaze_fit::kMinLidRange));
        }
        default: return "";
    }
}

bool fitStopped(const gaze_fit::View& fit) {
    using gaze_fit::Failure;
    if (fit.phase != gaze_fit::Phase::Failed) return false;
    return fit.failure == Failure::Cancelled || fit.failure == Failure::Left || fit.failure == Failure::DashboardOpened;
}

void fillFitResult(report::Summary& s, const gaze_fit::View& fit) {
    const UiText* tables[2] = {&uiText(Language::Ja), &uiText(Language::En)};
    if (fit.phase == gaze_fit::Phase::Done) {
        s.result = report::Result::Ok;
        for (int i = 0; i < 2; ++i) {
            const UiText& t = *tables[i];
            const char* title = fit.mode == gaze_fit::Mode::Center ? t.fitDoneCenter
                                : fit.mode == gaze_fit::Mode::Tilt ? t.fitDoneTilt
                                                                   : t.fitDone;
            const std::string line = format(t.fitGazeCenterFormat, format("%+.3f", fit.values.offsetX).c_str(),
                                            format("%+.3f", fit.values.offsetY).c_str(),
                                            format("%+.1f°", fit.values.rollDeg).c_str());
            (i == 0 ? s.reason : s.reasonEn) = std::string(title) + t.condSeparator + line;
        }
        return;
    }
    const bool stopped = fitStopped(fit);
    s.result = stopped ? report::Result::Stopped : report::Result::Failed;
    for (int i = 0; i < 2; ++i) {
        const UiText& t = *tables[i];
        const std::string why = failureText(t, fit);
        // Stopped: why only (there are no numbers behind it)
        const std::string detail = stopped ? std::string() : failureDetailText(t, fit);
        (i == 0 ? s.brief : s.briefEn) = why;
        (i == 0 ? s.reason : s.reasonEn) = why + (detail.empty() ? "" : " " + detail);
    }
}
