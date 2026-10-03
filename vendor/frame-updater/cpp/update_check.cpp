// SPDX-License-Identifier: MIT — part of frame-updater by sasaken1102r, shipped under the host app's MIT license
// Implementation of the update checker (see update_check.h).
#include "update_check.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

extern char** environ;

namespace frame_updater {

namespace {

/** A check or a detached start that takes longer than this is stuck and gets killed. */
constexpr int kCheckTimeoutSeconds = 90;
constexpr int kInstallStartTimeoutSeconds = 30;
/** The state file is re-read this often (ms). */
constexpr int kStateReadIntervalMs = 500;
/** A detached install that hasn't written its PID within this many seconds never started. */
constexpr long long kStartGraceSeconds = 60;

/**
 * Get a value from a parsed JSON object.
 * @param map the object
 * @param key the key
 * @return the value, or "" if missing
 */
std::string get(const std::map<std::string, std::string>& map, const std::string& key) {
    const auto it = map.find(key);
    return it == map.end() ? std::string() : it->second;
}

/**
 * Parse the numeric core of a version into up to three numbers.
 * @param text the version ("0.4.0", "v0.4.0-rc1")
 * @param parts receives the numbers (missing ones are 0)
 * @return false if it is not a version
 */
bool parseVersion(const std::string& text, long parts[3]) {
    std::string core = text;
    if (!core.empty() && (core[0] == 'v' || core[0] == 'V')) core.erase(0, 1);
    const size_t suffix = core.find_first_of("-+");
    if (suffix != std::string::npos) core.erase(suffix);
    parts[0] = parts[1] = parts[2] = 0;
    int index = 0;
    size_t digits = 0;
    for (const char c : core) {
        if (c == '.') {
            if (digits == 0 || ++index > 2) return false;
            digits = 0;
        } else if (c >= '0' && c <= '9') {
            if (++digits > 9) return false;
            parts[index] = parts[index] * 10 + (c - '0');
        } else {
            return false;
        }
    }
    return digits > 0;
}

/**
 * Read a whole small file.
 * @param path the file
 * @param text receives the contents
 * @return false if it can't be read
 */
bool readFile(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

/**
 * Take the last non-empty line of a command's output (the script prints one JSON line).
 * @param out the output
 * @return that line
 */
std::string lastLine(const std::string& out) {
    const size_t end = out.find_last_not_of("\r\n ");
    if (end == std::string::npos) return std::string();
    const size_t newline = out.rfind('\n', end);
    const size_t start = newline == std::string::npos ? 0 : newline + 1;
    return out.substr(start, end + 1 - start);
}

/**
 * This boot's ID (/proc/sys/kernel/random/boot_id), read once.
 * @return the ID, or "" if the kernel has none
 */
const std::string& currentBootId() {
    static const std::string id = [] {
        std::string text;
        readFile("/proc/sys/kernel/random/boot_id", text);
        std::string clean;
        for (const char c : text) {
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-') clean += c;
        }
        return clean;
    }();
    return id;
}

/**
 * Decide whether a "running" state file is stale (its install is gone).
 * @param state the parsed state file
 * @return true if nothing is running any more
 */
bool runningIsStale(const std::map<std::string, std::string>& state) {
    // Written before a reboot (a power loss mid-install): its PID may now be any process
    const std::string boot = get(state, "boot_id");
    if (!boot.empty() && !currentBootId().empty() && boot != currentBootId()) return true;
    const std::string pidText = get(state, "pid");
    if (!pidText.empty()) {
        const long pid = std::strtol(pidText.c_str(), nullptr, 10);
        if (pid <= 0) return true;
        // ESRCH: gone. EPERM: another user's process reusing the PID; the install ran as this user
        return ::kill(static_cast<pid_t>(pid), 0) != 0;
    }
    // Written by "install --detach" before the unit started
    const long long updated = std::atoll(get(state, "updated_at").c_str());
    return static_cast<long long>(std::time(nullptr)) - updated > kStartGraceSeconds;
}

/**
 * Skip whitespace in JSON text.
 * @param text the text
 * @param i position, moved past the whitespace
 */
void skipSpace(const std::string& text, size_t& i) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
}

/**
 * Append a Unicode code point as UTF-8.
 * @param out destination
 * @param cp the code point
 */
void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/**
 * Read a JSON string starting at its opening quote.
 * @param text the text
 * @param i position of the quote, moved past the closing quote
 * @param out receives the unescaped string
 * @return false if malformed
 */
bool readString(const std::string& text, size_t& i, std::string& out) {
    if (i >= text.size() || text[i] != '"') return false;
    ++i;
    out.clear();
    while (i < text.size()) {
        const char c = text[i++];
        if (c == '"') return true;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (i >= text.size()) return false;
        const char e = text[i++];
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 > text.size()) return false;
                const unsigned cp = static_cast<unsigned>(std::strtoul(text.substr(i, 4).c_str(), nullptr, 16));
                i += 4;
                appendUtf8(out, cp);  // surrogate pairs are not needed for our files
                break;
            }
            default: out += e; break;  // \" \\ \/
        }
    }
    return false;
}

/**
 * Skip any JSON value (used for nested objects and arrays we don't read).
 * @param text the text
 * @param i position of the value, moved past it
 * @return false if malformed
 */
bool skipValue(const std::string& text, size_t& i) {
    skipSpace(text, i);
    if (i >= text.size()) return false;
    if (text[i] == '"') {
        std::string ignored;
        return readString(text, i, ignored);
    }
    if (text[i] == '{' || text[i] == '[') {
        int depth = 0;
        while (i < text.size()) {
            const char c = text[i];
            if (c == '"') {
                std::string ignored;
                if (!readString(text, i, ignored)) return false;
                continue;
            }
            if (c == '{' || c == '[') ++depth;
            if (c == '}' || c == ']') {
                if (--depth == 0) {
                    ++i;
                    return true;
                }
            }
            ++i;
        }
        return false;
    }
    while (i < text.size() && text[i] != ',' && text[i] != '}') ++i;
    return true;
}

}  // namespace

bool UpdateStatus::operator==(const UpdateStatus& o) const {
    return state == o.state && checking == o.checking && current == o.current && latest == o.latest &&
           url == o.url && installable == o.installable && reason == o.reason && step == o.step &&
           version == o.version && error == o.error && message == o.message && checkedAt == o.checkedAt &&
           notes == o.notes && notesJa == o.notesJa;
}

std::map<std::string, std::string> parseFlatJson(const std::string& text) {
    std::map<std::string, std::string> result;
    size_t i = 0;
    skipSpace(text, i);
    if (i >= text.size() || text[i] != '{') return {};
    ++i;
    while (true) {
        skipSpace(text, i);
        if (i < text.size() && text[i] == '}') return result;
        std::string key;
        if (!readString(text, i, key)) return {};
        skipSpace(text, i);
        if (i >= text.size() || text[i] != ':') return {};
        ++i;
        skipSpace(text, i);
        if (i >= text.size()) return {};
        if (text[i] == '"') {
            std::string value;
            if (!readString(text, i, value)) return {};
            result[key] = value;
        } else if (text[i] == '{' || text[i] == '[') {
            if (!skipValue(text, i)) return {};
        } else {
            const size_t start = i;
            while (i < text.size() && text[i] != ',' && text[i] != '}' && text[i] != ' ' && text[i] != '\n') ++i;
            result[key] = text.substr(start, i - start);
        }
        skipSpace(text, i);
        if (i >= text.size()) return {};
        if (text[i] == ',') {
            ++i;
            continue;
        }
        if (text[i] == '}') return result;
        return {};
    }
}

int compareVersions(const std::string& a, const std::string& b) {
    long x[3];
    long y[3];
    if (!parseVersion(a, x) || !parseVersion(b, y)) return 0;
    for (int i = 0; i < 3; ++i) {
        if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
    }
    return 0;
}

UpdateChecker::UpdateChecker(UpdaterConfig config) : config_(std::move(config)) {
    const char* cache = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    if (cache != nullptr && cache[0] == '/') {
        cacheDir_ = cache;
    } else {
        cacheDir_ = std::string(home != nullptr ? home : "") + "/.cache";
    }
    cacheDir_ += "/" + config_.app;
    status_.current = config_.currentVersion;
    startedAt_ = static_cast<long long>(std::time(nullptr));
}

UpdateChecker::~UpdateChecker() {
    killChild(check_);
    // "install --detach" only starts the unit and exits within moments; let it finish
    if (installer_.pid > 0) {
        for (int i = 0; i < 50 && !finished(installer_, kInstallStartTimeoutSeconds); ++i) ::usleep(100 * 1000);
        killChild(installer_);
    }
}

bool UpdateChecker::spawn(const std::vector<std::string>& args, Child& child) {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) return false;

    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    // Start the script with a clean signal state whatever the panel blocks or ignores
    sigset_t empty;
    sigset_t defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGCHLD);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGHUP);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, "/bin/sh", &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        return false;
    }
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
    child.pid = pid;
    child.fd = fds[0];
    child.eof = false;
    child.out.clear();
    child.started = Clock::now();
    return true;
}

bool UpdateChecker::finished(Child& child, int timeoutSeconds) {
    if (child.pid <= 0) return false;
    char buffer[4096];
    while (!child.eof) {
        const ssize_t n = ::read(child.fd, buffer, sizeof(buffer));
        if (n > 0) {
            if (child.out.size() < 64 * 1024) child.out.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            child.eof = true;
            break;
        }
        if (errno == EINTR) continue;
        break;  // EAGAIN: nothing more for now
    }
    int status = 0;
    const pid_t done = ::waitpid(child.pid, &status, WNOHANG);
    if (done == child.pid || (done < 0 && errno == ECHILD)) {
        // Collect what it wrote just before exiting
        while (!child.eof) {
            const ssize_t n = ::read(child.fd, buffer, sizeof(buffer));
            if (n > 0) {
                if (child.out.size() < 64 * 1024) child.out.append(buffer, static_cast<size_t>(n));
            } else if (!(n < 0 && errno == EINTR)) {
                break;
            }
        }
        ::close(child.fd);
        child.fd = -1;
        child.pid = -1;
        return true;
    }
    if (Clock::now() - child.started > std::chrono::seconds(timeoutSeconds)) {
        killChild(child);
        child.out.clear();
        return true;
    }
    return false;
}

void UpdateChecker::killChild(Child& child) {
    if (child.pid > 0) {
        ::kill(child.pid, SIGKILL);
        while (::waitpid(child.pid, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
    if (child.fd >= 0) ::close(child.fd);
    child.pid = -1;
    child.fd = -1;
}

std::vector<std::string> UpdateChecker::baseArgs() const {
    return {"sh",     config_.script, "--app",   config_.app,
            "--repo", config_.repo,   "--current", config_.currentVersion,
            "--asset", config_.assetPattern};
}

void UpdateChecker::startCheck(bool force) {
    if (check_.pid > 0) return;
    lastCheck_ = Clock::now();
    checkedOnce_ = true;
    std::vector<std::string> args = baseArgs();
    if (force) args.push_back("--force");
    args.push_back("check");
    if (!spawn(args, check_)) {
        checkResult_ = {{"status", "error"}, {"error", "spawn-failed"}, {"message", "cannot run " + config_.script}};
    }
}

void UpdateChecker::checkNow() {
    if (check_.pid > 0) {
        forcePending_ = true;  // the running check may answer from the cache; ask again after it
    } else {
        startCheck(true);
    }
    recompute();
}

bool UpdateChecker::install() {
    if (installer_.pid > 0 || status_.state == UpdateState::Installing) return false;
    std::vector<std::string> args = baseArgs();
    for (const auto& arg : config_.defaultInstallArgs) {
        args.push_back("--install-arg");
        args.push_back(arg);
    }
    args.push_back("--detach");
    args.push_back("install");
    installReply_.clear();
    installedVersion_.clear();
    if (!spawn(args, installer_)) {
        installReply_ = {{"state", "failed"}, {"error", "spawn-failed"}, {"message", "cannot run " + config_.script}};
        recompute();
        return false;
    }
    installRequested_ = true;
    installRequestedAt_ = static_cast<long long>(std::time(nullptr));
    recompute();
    return true;
}

void UpdateChecker::dismiss() {
    if (status_.state == UpdateState::Installing) return;
    ::unlink((cacheDir_ + "/update-state.json").c_str());
    stateFile_.clear();
    installReply_.clear();
    installedVersion_.clear();
    recompute();
}

void UpdateChecker::readStateFile() {
    std::string text;
    stateFile_ = readFile(cacheDir_ + "/update-state.json", text) ? parseFlatJson(text)
                                                                  : std::map<std::string, std::string>();
    lastStateRead_ = Clock::now();
    stateReadOnce_ = true;
}

void UpdateChecker::tick(bool checkEnabled) {
    const auto now = Clock::now();
    if (check_.pid > 0 && finished(check_, kCheckTimeoutSeconds)) {
        auto result = parseFlatJson(lastLine(check_.out));
        if (result.empty()) {
            result = {{"status", "error"}, {"error", "script-failed"}, {"message", "no answer from " + config_.script}};
        }
        checkResult_ = std::move(result);
    }
    if (forcePending_ && check_.pid <= 0) {
        forcePending_ = false;
        startCheck(true);
    }
    if (installer_.pid > 0 && finished(installer_, kInstallStartTimeoutSeconds)) {
        auto reply = parseFlatJson(lastLine(installer_.out));
        if (reply.empty()) {
            reply = {{"state", "failed"}, {"error", "script-failed"}, {"message", "no answer from " + config_.script}};
        }
        // A failure here happened before the state file was written (busy, systemd-run missing...)
        if (get(reply, "state") == "failed") installReply_ = std::move(reply);
        installRequested_ = false;
        readStateFile();
    }
    if (checkEnabled && check_.pid <= 0 &&
        (!checkedOnce_ || now - lastCheck_ >= std::chrono::seconds(config_.recheckSeconds))) {
        startCheck(false);
    }
    if (!stateReadOnce_ || now - lastStateRead_ >= std::chrono::milliseconds(kStateReadIntervalMs)) {
        readStateFile();
    }
    recompute();
}

void UpdateChecker::recompute() {
    UpdateStatus next;
    next.current = config_.currentVersion;
    next.checking = check_.pid > 0 || forcePending_;
    next.checkedAt = std::atoll(get(checkResult_, "checked_at").c_str());

    // The check's answer is the base; an install in progress or just finished overrides it
    const std::string checkStatus = get(checkResult_, "status");
    next.latest = get(checkResult_, "latest");
    next.url = get(checkResult_, "url");
    if (checkStatus == "up-to-date") {
        next.state = UpdateState::UpToDate;
    } else if (checkStatus == "update-available") {
        next.state = UpdateState::Available;
        next.installable = get(checkResult_, "installable") == "true";
        next.reason = get(checkResult_, "reason");
        next.notes = get(checkResult_, "notes");
        next.notesJa = get(checkResult_, "notes_ja");
    } else if (checkStatus == "error") {
        next.state = UpdateState::CheckFailed;
        next.error = get(checkResult_, "error");
        next.message = get(checkResult_, "message");
    }

    // The install may have just finished: read the file again before calling it interrupted
    if (get(stateFile_, "state") == "running" && runningIsStale(stateFile_)) readStateFile();
    const std::string state = get(stateFile_, "state");
    const std::string version = get(stateFile_, "version");
    const bool running = state == "running" && !runningIsStale(stateFile_);
    // Until "install --detach" writes the state file, it still holds the previous install
    const bool requested =
        installRequested_ && std::atoll(get(stateFile_, "updated_at").c_str()) < installRequestedAt_;
    if (running || requested) {
        next.state = UpdateState::Installing;
        next.step = running ? get(stateFile_, "step") : "start";
        next.version = running ? version : std::string();
        next.error.clear();
        next.message.clear();
    } else if (!installReply_.empty()) {
        next.state = UpdateState::InstallFailed;
        next.error = get(installReply_, "error");
        next.message = get(installReply_, "message");
    } else if (state == "running") {
        // The install died without recording why (killed, power off)
        next.state = UpdateState::InstallFailed;
        next.version = version;
        next.error = "interrupted";
        next.message = "the update was interrupted";
    } else if (state == "done" && compareVersions(version, config_.currentVersion) > 0 &&
               std::atoll(get(stateFile_, "updated_at").c_str()) >= startedAt_) {
        // Installed while this process ran: it still runs the old version until it restarts, however long that
        // takes. A "done" from before it started is old news (it was restarted, or a version was installed by hand)
        installedVersion_ = version;
        next.state = UpdateState::Installed;
        next.version = version;
        next.error.clear();
        next.message.clear();
    } else if (!installedVersion_.empty() && state != "failed") {
        // Seen before; frame-update.sh may have removed the state file since
        next.state = UpdateState::Installed;
        next.version = installedVersion_;
        next.error.clear();
        next.message.clear();
    } else if (state == "failed" && (version.empty() || compareVersions(version, config_.currentVersion) > 0)) {
        next.state = UpdateState::InstallFailed;
        next.version = version;
        next.error = get(stateFile_, "error");
        next.message = get(stateFile_, "message");
    }

    if (next != status_) {
        status_ = std::move(next);
        ++revision_;
    }
}

}  // namespace frame_updater
