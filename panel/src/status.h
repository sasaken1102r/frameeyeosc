// frameeyeosc's status file (status.json), written about 10 times a second while it runs. The panel only reads it,
// and only while the panel is open.
#pragma once

#include "json.h"

#include <cmath>
#include <string>
#include <vector>

/** A pair of numbers (left and right, or x and y). NaN when missing. */
struct Pair {
    double v[2];

    /** @return true if both numbers are there */
    bool valid() const;
};

/** The latest gaze capture frameeyeosc reports (asked for by the panel's eye fit). */
struct GazeCaptureStatus {
    bool present = false;
    long long id = 0;
    std::string target;
    bool done = false;        ///< finished ("done"), else still capturing ("running")
    bool hasAverage = false;  ///< x, y and spread are there (done with samples)
    double x = 0.0;           ///< raw combined gaze, before the zero point and gains
    double y = 0.0;
    double spread = 0.0;
    bool hasEyeX = false;      ///< xEye is there (done with samples, not the eyes-shut step)
    double xEye[2] = {0.0, 0.0};  ///< each eye's own raw sideways gaze, left / right
    bool hasOpenness = false;  ///< openness is there (done with samples)
    double openness[2] = {0.0, 0.0};  ///< each eye's average Frame openness, before any scale
    int samples = 0;
    int received = 0;          ///< samples that came in after the skipped start (0 from frameeyeosc before 0.5.3)
    double rate = NAN;         ///< the tracker's rate over the capture (samples a second)
};

/** What the panel knows about frameeyeosc from status.json. */
struct EyeStatus {
    bool present = false;   ///< the file was read and parsed
    bool running = false;   ///< present, written less than 3 s ago, and its pid is alive
    std::string readError;  ///< why the file could not be used (for the log)

    int pid = 0;
    double time = 0.0;      ///< when it was written (Unix seconds)
    double started = 0.0;   ///< when frameeyeosc started (Unix seconds)
    bool sending = false;
    std::string output;     ///< "vrchat" / "etvr" / "livelink"
    std::string targetMode; ///< "auto" / "fixed"
    std::string target;     ///< "IP:PORT"; empty while the Steam Link PC is not found
    double rate = 0.0;      ///< samples sent in the last second
    double trackerRate = NAN;  ///< samples from the eye tracker in the last second (NaN before 0.5.3)
    bool tracking = false;  ///< eye data is coming in

    bool hasRaw = false;
    Pair openness {};        ///< Frame openness, left / right
    Pair opennessScaled {};  ///< after the per-eye scale (compared with the four lid marks)
    Pair gaze {};            ///< combined gaze x / y (-1..1), before smoothing
    bool hasSent = false;
    Pair lids {};            ///< what was sent (0..1 in the output's scale)
    Pair lidsVrcft {};       ///< the same in VRCFT's scale (0.75 = relaxed)
    Pair sentGaze {};        ///< gaze sent, x / y
    Pair rawGazeEye[2] {};   ///< each eye's own gaze before smoothing (left, right), x / y
    Pair sentGazeEye[2] {};  ///< each eye's gaze as sent (the combined one unless independent_eyes)

    bool calibrationEnabled = false;
    Pair relaxed {};         ///< learned relaxed openness per eye (NaN = not learned)
    Pair scales {};          ///< the scale in use per eye
    bool learning = false;
    bool lidFitted[2] = {false, false};  ///< each eye uses the eye fit (no learning, no scale)

    std::string configPath;
    std::string calibrationPath;
    std::string configError;          ///< one line from frameeyeosc; empty if none
    std::string sourceError;          ///< why frameeyeosc can't read the eye tracker (it keeps retrying); empty if it can
    std::vector<std::string> locked;  ///< config keys set on the command line
    JsonValue effective;              ///< the settings in effect (config keys)
    GazeCaptureStatus capture;        ///< the latest gaze capture

    /**
     * Whether a config key is set on frameeyeosc's command line (only trusted while it runs).
     * @param name the key
     * @return true if locked
     */
    bool isLocked(const std::string& name) const;
};

/**
 * The default place of status.json ($XDG_RUNTIME_DIR/frameeyeosc, or /run/user/<uid>/frameeyeosc).
 * @return the path
 */
std::string defaultStatusPath();

/**
 * Read status.json.
 * @param path the file
 * @param now the current Unix time in seconds (to tell a stale file)
 * @return the status (present = false if missing or unreadable)
 */
EyeStatus readStatus(const std::string& path, double now);

/**
 * Parse the text of a status file (split out for --status tests and fake states).
 * @param text the JSON text
 * @param now the current Unix time in seconds
 * @param checkPid whether to check that the pid is alive
 * @return the status
 */
EyeStatus parseStatus(const std::string& text, double now, bool checkPid);

/**
 * The current Unix time in seconds.
 * @return seconds since 1970
 */
double unixNow();
