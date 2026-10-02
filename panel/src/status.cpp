// Reading status.json.
#include "status.h"

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
/** A status older than this means frameeyeosc is not running (it writes about 10 times a second). */
constexpr double kStaleSec = 3.0;

/**
 * Read a [a, b] array of numbers; missing or null elements become NaN.
 * @param value the JSON value (may be nullptr)
 * @return the pair
 */
Pair readPair(const JsonValue* value) {
    Pair pair {{kNaN, kNaN}};
    if (value == nullptr || !value->isArray()) return pair;
    for (size_t i = 0; i < 2 && i < value->items.size(); ++i) {
        if (value->items[i].isNumber()) pair.v[i] = value->items[i].number;
    }
    return pair;
}

/**
 * A number member, or a fallback.
 * @param object the object
 * @param name the key
 * @param fallback the value if missing
 * @return the number
 */
double readNumber(const JsonValue& object, const char* name, double fallback) {
    const JsonValue* value = object.get(name);
    return value != nullptr && value->isNumber() ? value->number : fallback;
}

/**
 * A boolean member, or false.
 * @param object the object
 * @param name the key
 * @return the value
 */
bool readBool(const JsonValue& object, const char* name) {
    const JsonValue* value = object.get(name);
    return value != nullptr && value->isBool() && value->boolean;
}

/**
 * A string member, or "" (also for null).
 * @param object the object
 * @param name the key
 * @return the value
 */
std::string readText(const JsonValue& object, const char* name) {
    const JsonValue* value = object.get(name);
    return value != nullptr && value->isString() ? value->text : std::string();
}

}  // namespace

bool Pair::valid() const {
    return std::isfinite(v[0]) && std::isfinite(v[1]);
}

bool EyeStatus::isLocked(const std::string& name) const {
    if (!running) return false;
    for (const auto& item : locked) {
        if (item == name) return true;
    }
    return false;
}

std::string defaultStatusPath() {
    // The same rule as frameeyeosc: XDG_RUNTIME_DIR, or /run/user/<uid> when started without it (e.g. over SSH)
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string base = runtime != nullptr && runtime[0] != '\0'
                                 ? std::string(runtime)
                                 : "/run/user/" + std::to_string(::getuid());
    return base + "/frameeyeosc/status.json";
}

double unixNow() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

EyeStatus parseStatus(const std::string& text, double now, bool checkPid) {
    EyeStatus status;
    for (Pair* pair : {&status.openness, &status.opennessScaled, &status.gaze, &status.lids, &status.lidsVrcft,
                       &status.sentGaze, &status.relaxed, &status.scales, &status.rawGazeEye[0],
                       &status.rawGazeEye[1], &status.sentGazeEye[0], &status.sentGazeEye[1]}) {
        *pair = readPair(nullptr);
    }
    JsonValue root;
    if (!parseJson(text, root, status.readError)) return status;
    if (!root.isObject()) {
        status.readError = "status.json is not an object";
        return status;
    }
    status.present = true;
    status.pid = static_cast<int>(readNumber(root, "pid", 0));
    status.time = readNumber(root, "time", 0);
    status.started = readNumber(root, "started", 0);
    status.sending = readBool(root, "sending");
    status.output = readText(root, "output");
    status.targetMode = readText(root, "target_mode");
    status.target = readText(root, "target");
    status.rate = readNumber(root, "rate", 0);
    status.trackerRate = readNumber(root, "tracker_rate", NAN);
    status.tracking = readBool(root, "tracking");

    if (const JsonValue* raw = root.get("raw"); raw != nullptr && raw->isObject()) {
        status.hasRaw = true;
        status.openness = readPair(raw->get("openness"));
        status.opennessScaled = readPair(raw->get("openness_scaled"));
        status.gaze = readPair(raw->get("gaze"));
        status.rawGazeEye[0] = readPair(raw->get("gaze_left"));
        status.rawGazeEye[1] = readPair(raw->get("gaze_right"));
    }
    if (const JsonValue* sent = root.get("sent"); sent != nullptr && sent->isObject()) {
        status.hasSent = true;
        status.lids = readPair(sent->get("lids"));
        status.lidsVrcft = readPair(sent->get("lids_vrcft"));
        status.sentGaze = readPair(sent->get("gaze"));
        status.sentGazeEye[0] = readPair(sent->get("gaze_left"));
        status.sentGazeEye[1] = readPair(sent->get("gaze_right"));
    }
    if (const JsonValue* cal = root.get("calibration"); cal != nullptr && cal->isObject()) {
        status.calibrationEnabled = readBool(*cal, "enabled");
        status.relaxed = readPair(cal->get("relaxed"));
        status.scales = readPair(cal->get("scales"));
        status.learning = readBool(*cal, "learning");
        if (const JsonValue* fitted = cal->get("fitted"); fitted != nullptr && fitted->isArray()) {
            for (size_t eye = 0; eye < 2 && eye < fitted->items.size(); ++eye) {
                status.lidFitted[eye] = fitted->items[eye].isBool() && fitted->items[eye].boolean;
            }
        }
    }
    status.configPath = readText(root, "config_path");
    status.calibrationPath = readText(root, "calibration_path");
    status.configError = readText(root, "config_error");
    status.sourceError = readText(root, "source_error");
    if (const JsonValue* locked = root.get("locked"); locked != nullptr && locked->isArray()) {
        for (const JsonValue& item : locked->items) {
            if (item.isString()) status.locked.push_back(item.text);
        }
    }
    if (const JsonValue* effective = root.get("effective"); effective != nullptr && effective->isObject()) {
        status.effective = *effective;
    }
    if (const JsonValue* capture = root.get("gaze_capture"); capture != nullptr && capture->isObject()) {
        const JsonValue* id = capture->get("id");
        if (id != nullptr && id->isNumber()) {
            GazeCaptureStatus& c = status.capture;
            c.present = true;
            c.id = static_cast<long long>(id->number);
            c.target = readText(*capture, "target");
            c.done = readText(*capture, "state") == "done";
            c.samples = static_cast<int>(readNumber(*capture, "samples", 0));
            c.received = static_cast<int>(readNumber(*capture, "received", 0));
            c.rate = readNumber(*capture, "rate", NAN);
            const double x = readNumber(*capture, "x", NAN);
            const double y = readNumber(*capture, "y", NAN);
            const double spread = readNumber(*capture, "spread", NAN);
            c.hasAverage = std::isfinite(x) && std::isfinite(y) && std::isfinite(spread);
            if (c.hasAverage) {
                c.x = x;
                c.y = y;
                c.spread = spread;
            }
            const double xLeft = readNumber(*capture, "x_left", NAN);
            const double xRight = readNumber(*capture, "x_right", NAN);
            c.hasEyeX = std::isfinite(xLeft) && std::isfinite(xRight);
            if (c.hasEyeX) {
                c.xEye[0] = xLeft;
                c.xEye[1] = xRight;
            }
            const Pair openness = readPair(capture->get("openness"));
            c.hasOpenness = openness.valid();
            if (c.hasOpenness) {
                c.openness[0] = openness.v[0];
                c.openness[1] = openness.v[1];
            }
        }
    }

    const bool fresh = std::fabs(now - status.time) < kStaleSec;
    bool alive = true;
    if (checkPid) alive = status.pid > 0 && (::kill(status.pid, 0) == 0 || errno == EPERM);
    status.running = fresh && alive;
    return status;
}

EyeStatus readStatus(const std::string& path, double now) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        EyeStatus status = parseStatus("", now, false);  // an empty status with every value missing
        status.readError = "no status file";
        return status;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parseStatus(buffer.str(), now, true);
}
