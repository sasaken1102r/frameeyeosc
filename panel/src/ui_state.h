// What the panel remembers of itself between starts (not a setting): which sub-tab of the Advanced tab and of the
// Eyelids tab was open last.
// Kept in its own small file, $XDG_STATE_HOME/frameeyeosc/panel.json (~/.local/state/frameeyeosc/panel.json), next to
// the records, so config.json isn't rewritten for it: frameeyeosc never sees it, and "Reset all" leaves it alone.
#pragma once

#include <string>

/** The Advanced tab's sub-tabs, in the order the segmented control shows them. */
enum class AdvPage { Version, Trouble, Tools, Files };

/** The Eyelids tab's sub-tabs, in the order its segmented control shows them. */
enum class LidsPage { Look, Blinks, Fine };

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

}  // namespace ui_state
