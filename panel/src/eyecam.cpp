// The eye capture tab's logic: reading eyecam-rec's status.json, when the tab and the full-view overlay show, the
// step texts, and its control socket.
#include "eyecam.h"

#include "json.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace eyecam {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
/** A reply longer than this is not a reply. */
constexpr size_t kMaxReply = 1024;

/**
 * A number member, or a fallback.
 * @param object the object
 * @param name the key
 * @param fallback the value if missing or not a number
 * @return the number
 */
double readNumber(const JsonValue& object, const char* name, double fallback) {
    const JsonValue* value = object.get(name);
    return value != nullptr && value->isNumber() ? value->number : fallback;
}

/**
 * A whole number member, or a fallback.
 * @param object the object
 * @param name the key
 * @param fallback the value if missing, not a number or out of range
 * @return the number
 */
int readInt(const JsonValue& object, const char* name, int fallback) {
    const double value = readNumber(object, name, kNaN);
    if (!std::isfinite(value) || value < -1e6 || value > 1e6) return fallback;
    return static_cast<int>(std::lround(value));
}

/**
 * A string member, or "".
 * @param object the object
 * @param name the key
 * @return the text
 */
std::string readText(const JsonValue& object, const char* name) {
    const JsonValue* value = object.get(name);
    return value != nullptr && value->isString() ? value->text : std::string();
}

}  // namespace

std::string defaultDir() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string base = runtime != nullptr && runtime[0] != '\0' ? std::string(runtime)
                                                                      : "/run/user/" + std::to_string(::getuid());
    return base + "/eyecam";
}

State parseState(const std::string& text) {
    if (text.empty()) return State::Missing;
    if (text == "waiting_fds") return State::WaitingFds;
    if (text == "idle") return State::Idle;
    if (text == "searching") return State::Searching;
    if (text == "recording") return State::Recording;
    if (text == "error") return State::Error;
    if (text == "stopped") return State::Stopped;
    return State::Unknown;
}

Step parseStep(const std::string& label) {
    static const struct {
        const char* label;
        Step step;
    } kSteps[] = {
        {"lead_in", Step::LeadIn}, {"normal", Step::Normal}, {"widen", Step::Widen},       {"close", Step::Close},
        {"squint", Step::Squint}, {"look_up", Step::LookUp},   {"look_down", Step::LookDown},
        {"bright", Step::Bright}, {"dark", Step::Dark},         {"end", Step::End},
    };
    for (const auto& item : kSteps) {
        if (label == item.label) return item.step;
    }
    return Step::Unknown;
}

Status parseStatus(const std::string& text, double mtime) {
    Status status;
    status.mtime = mtime;
    status.fpsL = status.fpsR = kNaN;
    status.stepRemainingS = status.elapsedS = status.totalS = kNaN;
    JsonValue root;
    if (!parseJson(text, root, status.readError)) return status;
    if (!root.isObject()) {
        status.readError = "status.json is not an object";
        return status;
    }
    status.present = true;
    status.stateText = readText(root, "state");
    status.state = parseState(status.stateText);
    // A file without a state is still the recorder's: shown as an unknown state, not as no recorder
    if (status.state == State::Missing) status.state = State::Unknown;
    status.message = readText(root, "message");
    const JsonValue* locked = root.get("locked");
    status.locked = locked != nullptr && locked->isBool() && locked->boolean;
    status.fpsL = readNumber(root, "fps_l", kNaN);
    status.fpsR = readNumber(root, "fps_r", kNaN);
    status.stepIndex = readInt(root, "step_index", -1);
    status.stepCount = readInt(root, "step_count", -1);
    status.stepLabel = readText(root, "step_label");
    status.stepRemainingS = readNumber(root, "step_remaining_s", kNaN);
    status.elapsedS = readNumber(root, "elapsed_s", kNaN);
    status.totalS = readNumber(root, "total_s", kNaN);
    status.sessionDir = readText(root, "session_dir");
    status.protocol = readText(root, "protocol");
    return status;
}

Status readStatus(const std::string& dir) {
    const std::string path = dir + "/status.json";
    // The file is replaced by a rename, so the open file and its time are one version of it
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Status status = parseStatus("", 0.0);
        status.readError = "no status file";
        return status;
    }
    struct stat info {};
    double mtime = 0.0;
    if (::stat(path.c_str(), &info) == 0) mtime = info.st_mtim.tv_sec + info.st_mtim.tv_nsec / 1e9;
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parseStatus(buffer.str(), mtime);
}

double age(const Status& status, double now) {
    return std::fabs(now - status.mtime);
}

bool tabVisible(const Status& status, double now) {
    return status.present && status.state != State::Stopped && status.state != State::Missing &&
           age(status, now) <= kVisibleSec;
}

Fill fillFor(const Status& status, double now) {
    if (!tabVisible(status, now) || status.state != State::Recording || age(status, now) > kOverlayStaleSec) {
        return Fill::None;
    }
    switch (parseStep(status.stepLabel)) {
        case Step::Bright: return Fill::Bright;
        case Step::Dark: return Fill::Dark;
        default: return Fill::None;
    }
}

std::string instruction(const UiText& t, const std::string& label) {
    switch (parseStep(label)) {
        case Step::LeadIn: return t.eyecamStepLeadIn;
        case Step::Normal: return t.eyecamStepNormal;
        case Step::Widen: return t.eyecamStepWiden;
        case Step::Close: return t.eyecamStepClose;
        case Step::Squint: return t.eyecamStepSquint;
        case Step::LookUp: return t.eyecamStepLookUp;
        case Step::LookDown: return t.eyecamStepLookDown;
        case Step::Bright: return t.eyecamStepBright;
        case Step::Dark: return t.eyecamStepDark;
        case Step::End: return t.eyecamStepEnd;
        case Step::Unknown: break;
    }
    return label;
}

Reply parseReply(const std::string& line, const std::string& command) {
    std::string text = line;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    Reply reply;
    reply.command = command;
    if (text == "ok") {
        reply.ok = true;
        return reply;
    }
    if (text == "err" || text.rfind("err ", 0) == 0) {
        reply.error = text.size() > 4 ? text.substr(4) : std::string("(no reason)");
        return reply;
    }
    reply.error = text.empty() ? "empty reply" : "unexpected reply: " + text;
    return reply;
}

std::string signature(const View& view) {
    const Status& s = view.status;
    // As drawn: whole seconds left, the progress bar in steps of 0.2 %, fps to 0.1
    const auto rounded = [](double value, double step) {
        return std::isfinite(value) ? std::to_string(std::lround(value / step)) : std::string("-");
    };
    const double progress = std::isfinite(s.elapsedS) && std::isfinite(s.totalS) && s.totalS > 0
                                ? s.elapsedS / s.totalS
                                : kNaN;
    return std::to_string(view.visible) + "|" + std::to_string(view.busy) + "|" + view.busyCommand + "|" +
           std::to_string(view.hasReply) + std::to_string(view.reply.ok) + view.reply.command + "|" +
           view.reply.error + "|" + s.stateText + "|" + s.message + "|" + std::to_string(s.locked) + "|" +
           rounded(s.fpsL, 0.1) + "|" + rounded(s.fpsR, 0.1) + "|" + std::to_string(s.stepIndex) + "|" +
           std::to_string(s.stepCount) + "|" + s.stepLabel + "|" +
           (std::isfinite(s.stepRemainingS) ? std::to_string(static_cast<long>(std::ceil(s.stepRemainingS - 1e-9)))
                                            : std::string("-")) +
           "|" + rounded(progress, 0.002) + "|" + rounded(s.elapsedS, 1.0) + "|" + rounded(s.totalS, 1.0);
}

Control::~Control() {
    if (fd_ >= 0) ::close(fd_);
}

void Control::finish(const Reply& reply) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    received_.clear();
    reply_ = reply;
    hasReply_ = true;
}

bool Control::send(const std::string& socketPath, const std::string& command, double now, double timeoutSec) {
    if (busy()) return false;
    command_ = command;
    received_.clear();
    Reply failed;
    failed.command = command;
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(address.sun_path)) {
        failed.error = "socket path too long: " + socketPath;
        finish(failed);
        return false;
    }
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        failed.error = std::string("socket: ") + std::strerror(errno);
        finish(failed);
        return false;
    }
    // A unix socket connects at once, or says no (no recorder: ENOENT / ECONNREFUSED; its queue full: EAGAIN)
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        failed.error = "connect " + socketPath + ": " + std::strerror(errno);
        ::close(fd);
        finish(failed);
        return false;
    }
    const std::string line = command + "\n";
    const ssize_t sent = ::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(line.size())) {
        failed.error = sent < 0 ? std::string("send: ") + std::strerror(errno) : std::string("send: short write");
        ::close(fd);
        finish(failed);
        return false;
    }
    fd_ = fd;
    deadline_ = now + timeoutSec;
    return true;
}

bool Control::poll(double now) {
    if (fd_ < 0) return false;
    char buffer[256];
    while (true) {
        const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), 0);
        if (n > 0) {
            received_.append(buffer, static_cast<size_t>(n));
            const size_t newline = received_.find('\n');
            if (newline != std::string::npos) {
                finish(parseReply(received_.substr(0, newline), command_));
                return true;
            }
            if (received_.size() > kMaxReply) {
                finish(parseReply(received_.substr(0, 80), command_));
                return true;
            }
            continue;
        }
        if (n == 0) {
            // Closed: a reply without its newline still counts
            Reply reply = parseReply(received_, command_);
            if (received_.empty()) reply.error = "closed without a reply";
            finish(reply);
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        Reply failed;
        failed.command = command_;
        failed.error = std::string("recv: ") + std::strerror(errno);
        finish(failed);
        return true;
    }
    if (now >= deadline_) {
        Reply failed;
        failed.command = command_;
        failed.error = "no reply in time";
        finish(failed);
        return true;
    }
    return false;
}

}  // namespace eyecam
