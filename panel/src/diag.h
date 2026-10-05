// The diagnostics page (the Advanced tab's "Diagnostics"): what a stuck user sends as one screenshot. Four cards
// (versions, the eye data, the eye cameras, the last calibration) of short label / value rows, and a diagnostic code to
// type into a reply. Everything shown is put together here as text, from frameeyeosc's and eyecam-rec's status files
// and two things the panel reads itself (SteamOS's version, the camera tool's checksum), so it can be tested without
// cairo (diag-test). The panel only draws it.
//
// The diagnostic code (diag::code), its parts joined with "·":
//   S  the eye video: OK flowing / NW not worn (the proximity sensor says off, nothing found) / NV no video (worn, nothing
//      found) / OE one eye only / SR searching, no reason yet / LO live processing off / WF waiting for the tool to hand
//      over the camera buffers / NR eyecam-rec not running
//   P  the proximity reading, rounded ("P3"; "P-" unknown)
//   B  the 64 KiB blocks of the camera buffers that changed before the last look ("B0": nothing is written; "B-" no
//      look yet, or an older eyecam-rec)
//   G  the camera tool: G1 it handed over the buffers (has_buffers, or auto_grab "ok"), else G0
//   C  the last "calib wear": C1 went through, C1L / C1R without that eye (it kept its previous values), C0 failed,
//      C- none
//   F  frameeyeosc: F1 sending / FW waiting for eye data / FT no target found / FP paused / FE an error (the eye data
//      can't be read, or the config) / F0 not running
// e.g. "OK·P31·B128·G1·C1·F1" (all well), "NW·P3·B0·G1·C1R·F1" (stuck: not worn by the sensor, nothing written).
#pragma once

#include "i18n.h"

#include <string>
#include <vector>

struct PanelModel;

namespace diag {

/** What the panel reads for the page itself (not in the status files). */
struct System {
    std::string steamos;   ///< "0.4.3 (20260930.6234839)" ("" = unknown)
    std::string grabHash;  ///< the first 8 hex of the installed camera tool's SHA-256 ("" = not there)
};

/** One row: a label and its value. */
struct Row {
    std::string label;
    std::string value;
    bool bad = false;  ///< a problem now (drawn in red)
};

/** One card: a title and its rows. */
struct Card {
    std::string title;
    std::vector<Row> rows;
};

/**
 * SteamOS's version from /etc/os-release's text: VERSION_ID and BUILD_ID.
 * @param text the file's text
 * @return "0.4.3 (20260930.6234839)", "0.4.3" without a build, "" without a version
 */
std::string steamosVersion(const std::string& text);

/**
 * SteamOS's version as installed.
 * @param path os-release (default /etc/os-release)
 * @return as steamosVersion ("" if it can't be read)
 */
std::string readSteamos(const std::string& path = "/etc/os-release");

/**
 * SHA-256 of some bytes.
 * @param bytes the bytes
 * @return 64 lowercase hex digits
 */
std::string sha256Hex(const std::string& bytes);

/** Where install.sh puts the camera tool the setup installs ($HOME/.local/lib/eyecam/eyecam-grab). */
std::string defaultGrabPath();

/**
 * The first 8 hex of a file's SHA-256, worked out again only when its size or modification time changes.
 */
class FileHash {
public:
    /**
     * @param path the file
     * @return 8 hex digits, or "" if it can't be read
     */
    const std::string& get(const std::string& path);

private:
    std::string path_;
    long long size_ = -1;
    long long mtimeNs_ = -1;
    std::string hash_;
};

/**
 * The diagnostic code (see the top of this file).
 * @param m the model
 * @return e.g. "NW·P3·B0·G1·C1R·F1"
 */
std::string code(const PanelModel& m);

/**
 * The four cards.
 * @param t the texts
 * @param m the model (its system for the versions)
 * @return versions, eye data, eye cameras, last calibration
 */
std::vector<Card> cards(const UiText& t, const PanelModel& m);

/**
 * Everything the page shows as one string, so the panel redraws only when it changes.
 * @param t the texts
 * @param m the model
 * @return the code and every row
 */
std::string signature(const UiText& t, const PanelModel& m);

}  // namespace diag
