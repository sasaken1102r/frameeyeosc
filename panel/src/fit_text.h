// The eye fit's words on the panel: point names, why a fit stopped, and the numbers behind it, and how it ended for
// its record. Kept apart from the drawing so they are tested on their own (text_test.cpp).
#pragma once

#include "gaze_fit.h"
#include "i18n.h"
#include "report.h"
#include "status.h"

#include <string>
#include <vector>

/**
 * The name of an eye fit point.
 * @param t texts
 * @param point the point
 * @return the name ("正面" / "center")
 */
const char* pointName(const UiText& t, gaze_fit::Point point);

/**
 * Why an eye fit stopped, in words.
 * @param t texts
 * @param fit the session
 * @return the text ("" if it did not fail)
 */
std::string failureText(const UiText& t, const gaze_fit::View& fit);

/**
 * The numbers behind a failure, one line: where, what was measured against what was needed, and the tries.
 * @param t texts
 * @param fit the session
 * @return the line, e.g. "正面の点: 使えたサンプル 30/45・ばらつき 3.4°（2.7° まで）・3 回" ("" if there are none)
 */
std::string failureDetailText(const UiText& t, const gaze_fit::View& fit);

/**
 * After a fit, the note on each eye the eye camera stood in for (gaze_fit::Notes::standIn): Valve read it stuck at 0,
 * the camera saw it open, and a whole fit kept its lid fit. One line per eye, split by '\n' ("" for none, or if the fit
 * isn't done).
 * @param t texts
 * @param fit the session
 * @return the note
 */
std::string standInText(const UiText& t, const gaze_fit::View& fit);

/**
 * The Eye fit tab's card after a fit: its one line under "Fitted", what is fitted (fitCardBoth, fitCardGazeOnly, the
 * gaze and one eye's eyelid, the eyelids only, one eye's eyelid only; a first whole fit with an eye the eye camera
 * stood in for writes only the other eye's lid fit). The values themselves are under "Fine-tune".
 * @param t texts
 * @param gaze the gaze is fitted (FitInConfig::gazeFitted)
 * @param lids whether each eye's lid fit is all there (FitInConfig::lidsFitted)
 * @return the line (nullptr with nothing fitted)
 */
const char* fitCardLine(const UiText& t, bool gaze, const bool lids[2]);

/**
 * The Eye fit tab's card line (and the fit record's words) when the last whole fit kept the eyeball's old map, made
 * none, or kept one eye's ("partial": "右目の眼球の写し方は前のままだよ（点からずれてたので）。..."): why, with the eye ("眼球の写し方は前のままだよ（右目が点からずれてたので）。\nもう一度合わせてね": the card
 * starts a line at the line break, the record has a space there).
 * @param t texts
 * @param map what the last whole fit did to the eyeball's maps (status.json "eyeball_map")
 * @return the line ("" when it fitted one, or nothing is known)
 */
std::string eyeballMapLine(const UiText& t, const EyeballMapStatus& map);

/**
 * Whether a fit was stopped rather than failed: "Stop", another tab or page, the dashboard opened. Nothing went
 * wrong, and nothing was changed.
 * @param fit the session
 * @return true if stopped
 */
bool fitStopped(const gaze_fit::View& fit);

/**
 * How an eye fit ended, for its record (Japanese and English): done with its gaze center (and standInText), stopped
 * (fitStopped; not a failure, as on the tab) with why, or failed with why and the numbers behind it.
 * @param s the record's summary
 * @param fit the session as it ended
 */
void fillFitResult(report::Summary& s, const gaze_fit::View& fit);
