// Reading and writing config.json.
#include "config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/**
 * Create a folder and its parents (like mkdir -p).
 * @param dir the folder
 * @return true if it exists afterwards
 */
bool makeDirectories(const std::string& dir) {
    for (size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos != dir.size() && dir[pos] != '/') continue;
        const std::string part = dir.substr(0, pos);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

/**
 * Whether a JSON value has the type a key expects.
 * @param spec the key
 * @param value the value
 * @return true if frameeyeosc would accept the type
 */
bool typeMatches(const SettingSpec& spec, const JsonValue& value) {
    switch (spec.type) {
        case SettingType::Bool: return value.isBool();
        case SettingType::Number:
        case SettingType::Integer: return value.isNumber();
        case SettingType::NullableNumber:
        case SettingType::NullableInteger: return value.isNumber() || value.isNull();
        case SettingType::String: return value.isString();
    }
    return false;
}

/**
 * Write text to a file atomically: a temporary file in the same folder, fsync, rename, then fsync the folder.
 * @param path the destination
 * @param text the contents
 * @param error why it failed
 * @return true if written
 */
bool writeFileAtomically(const std::string& path, const std::string& text, std::string& error) {
    const size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    if (slash != std::string::npos && !makeDirectories(dir)) {
        error = "can't create " + dir + ": " + std::strerror(errno);
        return false;
    }
    const std::string temp = path + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "can't write " + temp + ": " + std::strerror(errno);
        return false;
    }
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            error = "write failed: " + std::string(std::strerror(errno));
            ::close(fd);
            ::unlink(temp.c_str());
            return false;
        }
        done += static_cast<size_t>(n);
    }
    // The data must be on disk before the rename, or a crash could leave an empty config.json
    if (::fsync(fd) != 0) {
        error = "fsync failed: " + std::string(std::strerror(errno));
        ::close(fd);
        ::unlink(temp.c_str());
        return false;
    }
    ::close(fd);
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        error = "rename failed: " + std::string(std::strerror(errno));
        ::unlink(temp.c_str());
        return false;
    }
    const int dirFd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd >= 0) {
        ::fsync(dirFd);  // makes the rename itself durable; failing here is harmless
        ::close(dirFd);
    }
    return true;
}

/**
 * A config object with every known key at its default, in the table's order (except the language).
 * @return the object
 */
JsonValue defaultObject() {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    for (const SettingSpec& spec : settingSpecs()) {
        // Left out so the panel keeps following the system language until one is picked
        if (std::string(spec.key) == key::kLanguage) continue;
        root.set(spec.key, defaultValue(spec));
    }
    return root;
}

/**
 * Read a whole file.
 * @param path the file
 * @param text the contents
 * @return false if it could not be opened
 */
bool readWholeFile(const std::string& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    text = buffer.str();
    return true;
}

}  // namespace

const std::vector<SettingSpec>& settingSpecs() {
    // key, type, default, default text, min, max, step, decimals[, value "On" sets if the default is off]
    static const std::vector<SettingSpec> specs = {
        {key::kVersion, SettingType::Integer, kConfigVersion, "", 1, kConfigVersion, 1, 0},
        {key::kSending, SettingType::Bool, 1, "", 0, 1, 1, 0},
        {key::kOutput, SettingType::String, 0, kOutputVrchat, 0, 0, 0, 0},
        {key::kHost, SettingType::String, 0, "auto", 0, 0, 0, 0},
        {key::kPort, SettingType::NullableInteger, 0, "", 1, 65535, 1, 0},
        {key::kPrefix, SettingType::String, 0, "/FT", 0, 0, 0, 0},
        {key::kEyeTrackingActive, SettingType::String, 0, "bool", 0, 0, 0, 0},
        {key::kSteamlinkParams, SettingType::Bool, 0, "", 0, 1, 1, 0},
        {key::kNativeEyes, SettingType::Bool, 0, "", 0, 1, 1, 0},
        {key::kCameraLids, SettingType::Bool, 1, "", 0, 1, 1, 0},
        {key::kPupilsToVrchat, SettingType::Bool, 1, "", 0, 1, 1, 0},
        {key::kPupilBits, SettingType::Integer, 0, "", 0, 4, 1, 0},
        {key::kRaw, SettingType::Bool, 0, "", 0, 1, 1, 0},
        {key::kGazeMinCutoff, SettingType::Number, 0.3, "", 0.05, 5.0, 0.05, 2},
        {key::kGazeBeta, SettingType::Number, 1.5, "", 0.0, 10.0, 0.1, 2},
        {key::kGazeDCutoff, SettingType::Number, 0.5, "", 0.1, 5.0, 0.1, 2},
        {key::kGazeDeadzone, SettingType::Number, 0.005, "", 0.0, 0.2, 0.005, 3},
        {key::kGazeHoldBelow, SettingType::Number, 0.5, "", 0.05, 1.0, 0.05, 2},
        {key::kIndependentEyes, SettingType::Bool, 0, "", 0, 1, 1, 0},
        {key::kLidMinCutoff, SettingType::Number, 6.0, "", 0.5, 30.0, 0.5, 1},
        {key::kLidBeta, SettingType::Number, 5.0, "", 0.0, 30.0, 0.5, 1},
        {key::kLidClosed, SettingType::Number, 0.30, "", 0.0, 1.5, 0.01, 2},
        {key::kLidOpen, SettingType::Number, 0.80, "", 0.0, 1.5, 0.01, 2},
        {key::kLidWidenStart, SettingType::Number, 0.92, "", 0.0, 1.5, 0.01, 2},
        {key::kLidWide, SettingType::Number, 1.00, "", 0.0, 1.5, 0.01, 2},
        {key::kLidScaleLeft, SettingType::NullableNumber, 0, "", 0.5, 2.0, 0.01, 2},
        {key::kLidScaleRight, SettingType::NullableNumber, 0, "", 0.5, 2.0, 0.01, 2},
        {key::kLidCalibration, SettingType::Bool, 1, "", 0, 1, 1, 0},
        {key::kLidSync, SettingType::Number, 0.4, "", 0.0, 1.0, 0.05, 2},
        {key::kGazeQualityLimit, SettingType::Number, 0.0, "", 0.005, 1.0, 0.005, 3, 0.03},
        {key::kBlinkHoldMs, SettingType::Number, 80, "", 0.0, 300.0, 10.0, 0},
        {key::kDespike, SettingType::Bool, 1, "", 0, 1, 1, 0},
        {key::kBlinkSyncBelow, SettingType::Number, 0.35, "", 0.0, 0.75, 0.05, 2},
        {key::kCameraLidFloor, SettingType::Number, 0.0, "", 0.0, 0.75, 0.05, 2},
        {key::kLidOpenSnap, SettingType::Number, 0.53, "", 0.0, 0.75, 0.01, 2},
        {key::kGazeOffsetX, SettingType::Number, 0.0, "", -0.5, 0.5, 0.005, 3},
        {key::kGazeOffsetY, SettingType::Number, 0.0, "", -0.5, 0.5, 0.005, 3},
        {key::kGazeGainX, SettingType::Number, 1.0, "", 0.5, 2.0, 0.05, 2},
        {key::kGazeGainUp, SettingType::Number, 1.0, "", 0.5, 2.0, 0.05, 2},
        {key::kGazeGainDown, SettingType::Number, 1.0, "", 0.5, 2.0, 0.05, 2},
        {key::kGazeRollDeg, SettingType::Number, 0.0, "", -20.0, 20.0, 0.5, 1},
        {key::kGazeDownHoldXDeg, SettingType::Number, 24, "", 0.0, 45.0, 1.0, 0},
        {key::kGazeDebugDots, SettingType::Bool, 0, "", 0, 1, 1, 0},
        {key::kGazeDebugDotsDistanceM, SettingType::Number, 1.0, "", 0.3, 2.0, 0.1, 1},
        {key::kFitSounds, SettingType::Bool, 1, "", 0, 1, 1, 0},
        {key::kAutoRecenter, SettingType::String, 0, "center", 0, 0, 0, 0},
        {key::kLidWiden, SettingType::String, 0, "normal", 0, 0, 0, 0},
        {key::kGazeOffsetXLeft, SettingType::NullableNumber, 0, "", -0.5, 0.5, 0.005, 3},
        {key::kGazeOffsetXRight, SettingType::NullableNumber, 0, "", -0.5, 0.5, 0.005, 3},
        {key::kGazeGainXLeft, SettingType::NullableNumber, 0, "", 0.5, 2.0, 0.05, 2},
        {key::kGazeGainXRight, SettingType::NullableNumber, 0, "", 0.5, 2.0, 0.05, 2},
        {key::kLidFitClosedLeft, SettingType::NullableNumber, 0, "", 0.0, 1.5, 0.01, 2},
        {key::kLidFitClosedRight, SettingType::NullableNumber, 0, "", 0.0, 1.5, 0.01, 2},
        {key::kLidFitUpLeft, SettingType::NullableNumber, 0, "", 0.0, 1.5, 0.01, 2},
        {key::kLidFitUpRight, SettingType::NullableNumber, 0, "", 0.0, 1.5, 0.01, 2},
        {key::kLidFitOpenLeft, SettingType::NullableNumber, 0, "", 0.0, 1.5, 0.01, 2},
        {key::kLidFitOpenRight, SettingType::NullableNumber, 0, "", 0.0, 1.5, 0.01, 2},
        {key::kLidFitDownLeft, SettingType::NullableNumber, 0, "", 0.0, 1.5, 0.01, 2},
        {key::kLidFitDownRight, SettingType::NullableNumber, 0, "", 0.0, 1.5, 0.01, 2},
        {key::kCalibrationReset, SettingType::Integer, 0, "", 0, 1e9, 1, 0},
        {key::kLanguage, SettingType::String, 0, "ja", 0, 0, 0, 0},
        {key::kUpdateCheck, SettingType::Bool, 1, "", 0, 1, 1, 0},
    };
    return specs;
}

const SettingSpec* findSetting(const std::string& name) {
    for (const SettingSpec& spec : settingSpecs()) {
        if (name == spec.key) return &spec;
    }
    return nullptr;
}

JsonValue defaultValue(const SettingSpec& spec) {
    switch (spec.type) {
        case SettingType::Bool: return JsonValue::makeBool(spec.defaultNumber != 0);
        case SettingType::Number: return JsonValue::makeNumber(spec.defaultNumber);
        case SettingType::Integer: return JsonValue::makeNumber(spec.defaultNumber, true);
        case SettingType::NullableNumber:
        case SettingType::NullableInteger: return JsonValue::makeNull();
        case SettingType::String: return JsonValue::makeString(spec.defaultText);
    }
    return JsonValue::makeNull();
}

JsonValue ConfigFile::value(const std::string& name) const {
    const SettingSpec* spec = findSetting(name);
    const JsonValue* found = root.get(name);
    if (spec == nullptr) return found != nullptr ? *found : JsonValue();
    if (found != nullptr && typeMatches(*spec, *found)) return *found;
    return defaultValue(*spec);
}

double ConfigFile::number(const std::string& name) const {
    const JsonValue v = value(name);
    if (v.isNumber()) return v.number;
    if (v.isBool()) return v.boolean ? 1.0 : 0.0;
    return kNaN;
}

bool ConfigFile::flag(const std::string& name) const {
    const JsonValue v = value(name);
    return v.isBool() && v.boolean;
}

std::string ConfigFile::text(const std::string& name) const {
    const JsonValue v = value(name);
    return v.isString() ? v.text : std::string();
}

std::string defaultConfigPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg != nullptr && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home != nullptr ? home : ".") + "/.config";
    }
    return base + "/frameeyeosc/config.json";
}

ConfigFile readConfigFile(const std::string& path) {
    ConfigFile config;
    config.root.type = JsonValue::Type::Object;
    std::string text;
    if (!readWholeFile(path, text)) return config;  // a missing file means "all defaults"
    config.exists = true;
    JsonValue root;
    std::string error;
    if (!parseJson(text, root, error)) {
        config.error = error;
        return config;
    }
    if (!root.isObject()) {
        config.error = "the top level must be an object { ... }";
        return config;
    }
    config.root = std::move(root);
    return config;
}

std::string configStamp(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return "0";
    return std::to_string(info.st_mtim.tv_sec) + "." + std::to_string(info.st_mtim.tv_nsec) + ":" +
           std::to_string(info.st_size) + ":" + std::to_string(info.st_ino);
}

bool updateConfigFile(const std::string& path, const std::function<void(JsonValue&)>& change, std::string& error) {
    ConfigFile current = readConfigFile(path);
    if (!current.error.empty()) {
        error = "config.json is broken (" + current.error + ")";
        return false;
    }
    JsonValue root = current.exists ? current.root : defaultObject();
    change(root);
    if (!writeFileAtomically(path, writeJson(root), error)) return false;
    std::fprintf(stderr, "[config] wrote %s\n", path.c_str());
    return true;
}

bool resetConfigFile(const std::string& path, std::string& error) {
    ConfigFile current = readConfigFile(path);
    JsonValue root = defaultObject();
    if (current.exists && current.error.empty()) {
        // Keep what "reset" should not touch: the panel language, the calibration counter and unknown keys
        root = current.root;
        for (const SettingSpec& spec : settingSpecs()) {
            const std::string name = spec.key;
            if (name == key::kLanguage || name == key::kCalibrationReset) continue;
            root.set(name, defaultValue(spec));
        }
    } else if (!current.error.empty()) {
        std::string text;
        std::string backupError;
        if (readWholeFile(path, text) && writeFileAtomically(path + ".broken", text, backupError)) {
            std::fprintf(stderr, "[config] kept the broken file as %s.broken\n", path.c_str());
        } else {
            error = "can't back up the broken config.json: " + backupError;
            return false;
        }
    }
    if (!writeFileAtomically(path, writeJson(root), error)) return false;
    std::fprintf(stderr, "[config] reset %s to defaults\n", path.c_str());
    return true;
}

double roundToDecimals(const SettingSpec& spec, double value) {
    const double scale = std::pow(10.0, spec.decimals);
    return std::round(value * scale) / scale;
}

double snapValue(const SettingSpec& spec, double value) {
    const double snapped = spec.min + std::round((value - spec.min) / spec.step) * spec.step;
    return roundToDecimals(spec, std::fmin(spec.max, std::fmax(spec.min, snapped)));
}

double stepValue(const SettingSpec& spec, double current, int direction, double low, double high) {
    const double lower = std::fmax(spec.min, low);
    const double upper = std::fmin(spec.max, high);
    if (!std::isfinite(current)) current = spec.defaultNumber;
    // Move one step and snap onto the step grid (a hand-written 0.37 goes to 0.40 or 0.35)
    double next = std::round((current + direction * spec.step) / spec.step) * spec.step;
    if ((direction > 0 && next <= current + 1e-9) || (direction < 0 && next >= current - 1e-9)) {
        next += direction * spec.step;
    }
    next = std::fmin(upper, std::fmax(lower, next));
    return roundToDecimals(spec, next);
}
