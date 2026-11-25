// "What's new" introductions: when a release adds something to a tab (or one of its sub-tabs), the first time the user
// opens that tab after updating, the panel shows a short card about it, once (EyePanel::drawIntro). Each intro has an
// id kept in panel.json ("intros_seen", ui_state) once shown; a fresh install (no panel.json, no config.json, no
// records yet) marks every intro of this version seen, so a new user isn't greeted with a pile of them.
// To add one: a row at the end of kIntros in intro.cpp, with a new id (never reuse one), its tab and sub-tab, the version
// it came in, the text in both languages and, if it has a "Try it" button, the row it points to (Row).
#pragma once

#include "i18n.h"

#include <string>
#include <vector>

/** The tabs (panel.h). */
enum class PanelTab : int;

namespace intro {

/** The row an intro's "Try it" points to: it is shown (its tab open, nothing over it) and outlined for a moment. */
enum class Row { None, GazeSource };

/** One introduction. */
struct Intro {
    const char* id;     ///< kept in panel.json once shown (never reused)
    PanelTab tab;       ///< the tab it is about
    int page;           ///< that tab's sub-tab (its AdvPage / LidsPage / GazePage / OutputPage as int), -1 = any
    const char* since;  ///< the version it came in (for the record; the tests check it isn't newer than this one)
    const char* titleJa;
    const char* bodyJa;  ///< paragraphs separated by '\n', each wrapped to the card
    const char* titleEn;
    const char* bodyEn;
    Row row;  ///< Row::None: only "Got it"
};

/** @return every intro, oldest first */
const std::vector<Intro>& all();

/** @return every intro's id (what a fresh install marks seen) */
std::vector<std::string> allIds();

/**
 * @param id an id
 * @return its intro, or nullptr
 */
const Intro* find(const std::string& id);

/**
 * The intro to show now: the first not yet seen for this tab and sub-tab.
 * @param tab the tab open
 * @param page its sub-tab (as int; -1 for a tab without them)
 * @param seen the ids already shown
 * @return it, or nullptr
 */
const Intro* due(PanelTab tab, int page, const std::vector<std::string>& seen);

/**
 * Whether this looks like a fresh install, whose intros are all taken as seen: the panel has no panel.json yet, and
 * frameeyeosc no config.json and no records (an update from before the intros may have neither panel.json nor the
 * intros' member, but has a config.json or records once anything was set or fitted).
 * @param panelStateExists panel.json is there
 * @param configExists config.json is there
 * @param hasRecords any record (calibration, eye fit) is there
 * @return true if every intro should count as seen
 */
bool freshInstall(bool panelStateExists, bool configExists, bool hasRecords);

/**
 * @param intro an intro
 * @param language the language
 * @return its title
 */
const char* title(const Intro& intro, Language language);

/**
 * @param intro an intro
 * @param language the language
 * @return its body (paragraphs separated by '\n')
 */
const char* body(const Intro& intro, Language language);

}  // namespace intro
