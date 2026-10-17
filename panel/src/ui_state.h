// What the panel remembers of itself between starts (not a setting): which sub-tab of the Gaze, Eyelids and Advanced
// tabs was open last, whether the gaze scale's pairs are linked (their locks), the up gains the last whole eye fits
// measured (the fit writes their mean, gaze_fit::meanUpGain), and whether "Get test versions" is on.
// Kept in its own small file, $XDG_STATE_HOME/frameeyeosc/panel.json (~/.local/state/frameeyeosc/panel.json), next to
// the records, so config.json isn't rewritten for it: frameeyeosc never sees it, and "Reset all" leaves it alone.
#pragma once

#include <string>
#include <vector>

/** The Advanced tab's sub-tabs, in the order the segmented control shows them. */
enum class AdvPage { Version, Trouble, Tools, Files };

/** The Eyelids tab's sub-tabs, in the order its segmented control shows them. */
enum class LidsPage { Look, Blinks, Fine };

/** The Gaze tab's sub-tabs, in the order its segmented control shows them. */
enum class GazePage { Motion, Reach, Glitch };

namespace ui_state {

/**
 * Where the file is: $XDG_STATE_HOME/frameeyeosc/panel.json, or ~/.local/state/frameeyeosc/panel.json.
 * @return the path
 */
std::string defaultPath();

/**
 * @param page a sub-tab
 * @return its name in the file ("version", "trouble", "tools", "files")
 */
const char* pageName(AdvPage page);

/**
 * @param name a sub-tab's name
 * @param page where to write it
 * @return true if known
 */
bool parsePage(const std::string& name, AdvPage& page);

/**
 * The sub-tab open last.
 * @param path the file
 * @return it, or Version (the first time, or when the file is missing or broken)
 */
AdvPage readPage(const std::string& path);

/**
 * Remember the sub-tab open now (the file's other members are kept; written through a temporary file and rename).
 * @param path the file (its folder is made if missing)
 * @param page the sub-tab
 * @param error why it failed
 * @return true if written
 */
bool writePage(const std::string& path, AdvPage page, std::string& error);

/**
 * @param page an Eyelids sub-tab
 * @return its name in the file ("look", "blinks", "fine")
 */
const char* lidsPageName(LidsPage page);

/**
 * @param name an Eyelids sub-tab's name
 * @param page where to write it
 * @return true if known
 */
bool parseLidsPage(const std::string& name, LidsPage& page);

/**
 * The Eyelids sub-tab open last.
 * @param path the file
 * @return it, or Look (the first time, or when the file is missing or broken)
 */
LidsPage readLidsPage(const std::string& path);

/**
 * Remember the Eyelids sub-tab open now (as writePage).
 * @param path the file (its folder is made if missing)
 * @param page the sub-tab
 * @param error why it failed
 * @return true if written
 */
bool writeLidsPage(const std::string& path, LidsPage page, std::string& error);

/**
 * @param page a Gaze sub-tab
 * @return its name in the file ("motion", "reach", "glitch")
 */
const char* gazePageName(GazePage page);

/**
 * @param name a Gaze sub-tab's name
 * @param page where to write it
 * @return true if known
 */
bool parseGazePage(const std::string& name, GazePage& page);

/**
 * The Gaze sub-tab open last.
 * @param path the file
 * @return it, or Motion (the first time, or when the file is missing or broken)
 */
GazePage readGazePage(const std::string& path);

/**
 * Remember the Gaze sub-tab open now (as writePage).
 * @param path the file (its folder is made if missing)
 * @param page the sub-tab
 * @param error why it failed
 * @return true if written
 */
bool writeGazePage(const std::string& path, GazePage page, std::string& error);

/**
 * Whether each pair of the gaze scale was left linked (its lock closed): [0] left / right, [1] up / down.
 * @param path the file
 * @param linked where to write them (true, linked, the first time or when the file is missing or broken)
 */
void readGazeScaleLinks(const std::string& path, bool linked[2]);

/**
 * Remember whether each pair of the gaze scale is linked (the file's other members are kept).
 * @param path the file (its folder is made if missing)
 * @param linked [0] left / right, [1] up / down
 * @param error why it failed
 * @return true if written
 */
bool writeGazeScaleLinks(const std::string& path, const bool linked[2], std::string& error);

/**
 * The up gains the last whole eye fits measured ("fit_up_gains"), oldest first.
 * @param path the file
 * @return them (empty the first time, or when the file is missing or broken; numbers outside 0.5..2 are left out)
 */
std::vector<double> readUpGains(const std::string& path);

/**
 * Remember the up gains of the last whole eye fits (the file's other members are kept).
 * @param path the file (its folder is made if missing)
 * @param gains oldest first (empty: forget them, after "Reset" on the Eye fit tab)
 * @param error why it failed
 * @return true if written
 */
bool writeUpGains(const std::string& path, const std::vector<double>& gains, std::string& error);

/**
 * Whether "Get test versions" is on ("update_beta"): update checks then use frame-updater's beta channel.
 * @param path the file
 * @return it (false, off, the first time or when the file is missing or broken)
 */
bool readUpdateBeta(const std::string& path);

/**
 * Remember "Get test versions" (the file's other members are kept).
 * @param path the file (its folder is made if missing)
 * @param on whether it is on
 * @param error why it failed
 * @return true if written
 */
bool writeUpdateBeta(const std::string& path, bool on, std::string& error);

}  // namespace ui_state
