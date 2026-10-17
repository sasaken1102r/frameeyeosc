// Actions from outside the panel (`frameeyeosc-panel --action NAME`): the decision, the line protocol, the command's
// side and the panel's listening socket. See action_socket.h.
#include "action_socket.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace action_socket {

namespace {

/** The actions and their names, in the order of the usage line. */
struct Named {
    Action action;
    const char* name;
};
constexpr Named kActions[] = {
    {Action::Recenter, "recenter"},
    {Action::Fit, "fit"},
    {Action::Stop, "stop"},
    {Action::Relearn, "relearn"},
    {Action::Open, "open"},
};

/** The longest action name echoed back in a line (whatever was typed). */
constexpr size_t kMaxNameEcho = 32;
/** Codes a reply may carry: the table's and later ones (a newer panel), never more than a shell's exit status. */
constexpr int kMaxReplyCode = 99;

/**
 * Keep printable ASCII only (a reply or an echoed name never carries control characters into a log or a HUD).
 * @param text the text
 * @param limit the longest result
 * @return the text kept
 */
std::string printable(const std::string& text, size_t limit) {
    std::string out;
    for (const char c : text) {
        if (out.size() >= limit) break;
        if (c >= 0x20 && c <= 0x7e) out += c;
    }
    return out;
}

/**
 * Monotonic milliseconds.
 * @return milliseconds
 */
long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/**
 * Fill a socket address.
 * @param path the socket's path
 * @param address set to it
 * @return false if the path doesn't fit
 */
bool socketAddress(const std::string& path, sockaddr_un& address) {
    address = {};
    if (path.empty() || path.size() >= sizeof(address.sun_path)) return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

/** The text the command prints when nothing listens. */
constexpr const char* kNotRunningText = "panel not running";
/** ...and when no proper answer came. */
constexpr const char* kNoAnswerText = "no answer from the panel";

}  // namespace

bool parseAction(const std::string& name, Action& action) {
    for (const Named& named : kActions) {
        if (name == named.name) {
            action = named.action;
            return true;
        }
    }
    return false;
}

const char* actionName(Action action) {
    for (const Named& named : kActions) {
        if (named.action == action) return named.name;
    }
    return "?";
}

std::string actionNames() {
    std::string names;
    for (const Named& named : kActions) names += (names.empty() ? "" : "|") + std::string(named.name);
    return names;
}

Decision decide(Action action, const State& state) {
    switch (action) {
        case Action::Stop:
            if (!state.fitActive) return {Code::NothingToStop, "nothing to stop", Effect::None};
            return {Code::Accepted, std::string("stopped the ") + (state.fitIsRecenter ? "re-center" : "eye fit"),
                    Effect::StopFit};
        case Action::Open: return {Code::Accepted, "panel shown", Effect::ShowPanel};
        case Action::Recenter:
        case Action::Fit:
        case Action::Relearn:
            // While a fit runs the panel shows only its "Stop", so a new one waits for that first (and the eyelids
            // aren't relearnt while a fit measures them)
            if (state.fitActive && state.fitIsRecenter) {
                return {Code::BusyRecenter, "busy (a re-center is running; use --action stop)", Effect::None};
            }
            if (state.fitActive) return {Code::BusyFit, "busy (a fit is running; use --action stop)", Effect::None};
            // The buttons are grey while frameeyeosc doesn't run; without eye data (the headset off, most often)
            // nothing could be measured
            if (!state.running) return {Code::NoFrameeyeosc, "frameeyeosc is not running", Effect::None};
            if (!state.tracking) return {Code::NoEyeData, "no eye data (is the headset on?)", Effect::None};
            if (action == Action::Relearn) {
                if (!state.camerasReady) {
                    return {Code::NoCameras, "eye cameras not in use or not ready (see the panel's Eye cameras tab)",
                            Effect::None};
                }
                return {Code::Accepted, "started", Effect::Relearn};
            }
            if (state.locked) {
                return {Code::Locked, "locked (the fit's values are set on frameeyeosc's command line)", Effect::None};
            }
            // The re-center moves the fitted zero point; with nothing fitted the whole fit comes first (the automatic
            // re-center after putting the headset on waits for one too)
            if (action == Action::Recenter && !state.gazeFitted) {
                return {Code::NotFitted, "no gaze fit yet (use --action fit first)", Effect::None};
            }
            return {Code::Accepted, "started", action == Action::Fit ? Effect::StartFit : Effect::StartRecenter};
    }
    return {Code::Usage, "unknown action", Effect::None};
}

Decision relearnAnswer(bool ok, const std::string& error) {
    if (ok) return {Code::Accepted, "started (learning your relaxed eyes, about 30 s)", Effect::None};
    // eyecam-rec's reasons are Japanese (its own English is in status.json): the usual ones named here
    const char* why = error.find("映像") != std::string::npos      ? "no eye video"
                      : error.find("校正中") != std::string::npos   ? "calibrating"
                      : error.find("録画中") != std::string::npos   ? "recording"
                      : error.find("目の値を出していない") != std::string::npos ? "not reading the cameras"
                      : error.find("知らないコマンド") != std::string::npos ? "eyecam-rec too old for it"
                      : error.find("no reply") != std::string::npos ? "no answer from eyecam-rec"
                                                                    : "eyecam-rec said no";
    return {Code::NoCameras, std::string("eye cameras not ready (") + why + ")", Effect::None};
}

std::string formatReply(Code code, const std::string& text) {
    const std::string head = std::to_string(static_cast<int>(code));
    // The code, a space, the text and the newline within kMaxLine
    return head + " " + printable(text, kMaxLine - head.size() - 2) + "\n";
}

bool parseReply(const std::string& line, int& code, std::string& text) {
    std::string body = line;
    if (!body.empty() && body.back() == '\n') body.pop_back();
    if (!body.empty() && body.back() == '\r') body.pop_back();
    if (body.empty() || body.size() >= kMaxLine) return false;
    size_t digits = 0;
    while (digits < body.size() && body[digits] >= '0' && body[digits] <= '9') ++digits;
    if (digits == 0 || digits > 2 || (digits < body.size() && body[digits] != ' ')) return false;
    code = std::atoi(body.substr(0, digits).c_str());
    if (code < 0 || code > kMaxReplyCode) return false;
    text = digits < body.size() ? printable(body.substr(digits + 1), kMaxLine) : std::string();
    return true;
}

std::string resultLine(const std::string& action, const std::string& text) {
    const std::string name = printable(action, kMaxNameEcho);
    return (name.empty() ? std::string("action") : name) + ": " + text;
}

std::string defaultSocketPath() {
    // The same folder as the single-instance lock, but never /tmp (the socket must stay the user's own)
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] != '\0') return std::string(runtime) + "/" + kSocketName;
    const std::string userRuntime = "/run/user/" + std::to_string(::getuid());
    if (::access(userRuntime.c_str(), W_OK) == 0) return userRuntime + "/" + kSocketName;
    return "";
}

Result request(const std::string& socketPath, const std::string& action, int timeoutMs) {
    Result result;
    sockaddr_un address {};
    if (!socketAddress(socketPath, address)) {
        result.code = static_cast<int>(Code::NotRunning);
        result.text = kNotRunningText;
        return result;
    }
    // Non-blocking, so a panel whose backlog is full can't hold the command past its timeout (a Unix socket's
    // connect then fails with EAGAIN at once instead of waiting)
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        result.text = std::string(kNoAnswerText) + " (socket: " + std::strerror(errno) + ")";
        return result;
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int error = errno;
        ::close(fd);
        if (error == EAGAIN) {
            result.text = std::string(kNoAnswerText) + " (it is busy)";
        } else {
            // No file (not running, or a panel without this socket: 0.7.9 and before), a file nobody listens on (it
            // crashed), or not ours
            result.code = static_cast<int>(Code::NotRunning);
            result.text = kNotRunningText;
        }
        return result;
    }
    const std::string line = printable(action, kMaxLine - 1) + "\n";
    if (::send(fd, line.data(), line.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(line.size())) {
        ::close(fd);
        result.text = kNoAnswerText;
        return result;
    }
    // One line back, within the timeout
    const long long deadline = nowMs() + std::max(0, timeoutMs);
    std::string received;
    bool complete = false;
    while (!complete && received.size() < kMaxLine) {
        const long long left = deadline - nowMs();
        if (left <= 0) break;
        pollfd waiting {fd, POLLIN, 0};
        const int ready = ::poll(&waiting, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        char buffer[kMaxLine];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) break;  // closed without (the rest of) a line
        received.append(buffer, static_cast<size_t>(n));
        complete = received.find('\n') != std::string::npos;
    }
    ::close(fd);
    int code = 0;
    std::string text;
    if (complete && parseReply(received.substr(0, received.find('\n')), code, text)) {
        result.code = code;
        result.text = text;
        return result;
    }
    result.text = received.empty() ? kNoAnswerText : std::string(kNoAnswerText) + " (not a reply)";
    return result;
}

int runCommand(const std::vector<std::string>& args, const std::string& socketPath, int timeoutMs, std::string& line) {
    if (args.size() != 2 || args[0] != "--action") {
        line = "action: bad arguments (usage: frameeyeosc-panel --action " + actionNames() + ")";
        return static_cast<int>(Code::Usage);
    }
    Action action = Action::Open;
    if (!parseAction(args[1], action)) {
        line = resultLine(args[1], "unknown action (use " + actionNames() + ")");
        return static_cast<int>(Code::Usage);
    }
    const Result result = request(socketPath, args[1], timeoutMs);
    line = resultLine(args[1], result.text.empty() ? std::string("code ") + std::to_string(result.code) : result.text);
    return result.code;
}

Server::~Server() {
    close();
}

bool Server::open(const std::string& path, std::string& error) {
    close();
    sockaddr_un address {};
    if (!socketAddress(path, address)) {
        error = path.empty() ? "no runtime folder for the socket" : "socket path too long: " + path;
        return false;
    }
    // Only the instance holding the lock gets here, so a socket left there is a crashed panel's. Anything that isn't a
    // socket is not ours to remove
    struct stat existing {};
    if (::lstat(path.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            error = path + " is there and is not a socket; not listening";
            return false;
        }
        ::unlink(path.c_str());
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        error = "bind " + path + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    // 0600 right after bind. The runtime folder is the user's own (0700), so nobody else can reach the socket in
    // between either (umask isn't touched: it is the whole process's, and other threads write files)
    struct stat bound {};
    if (::chmod(path.c_str(), 0600) != 0 || ::stat(path.c_str(), &bound) != 0 ||
        ::listen(fd, static_cast<int>(kMaxClients)) != 0) {
        error = "listen " + path + ": " + std::strerror(errno);
        ::close(fd);
        ::unlink(path.c_str());
        return false;
    }
    fd_ = fd;
    path_ = path;
    inode_ = static_cast<unsigned long long>(bound.st_ino);
    return true;
}

void Server::close() {
    for (Client& client : clients_) {
        if (client.fd >= 0) ::close(client.fd);
    }
    clients_.clear();
    if (fd_ < 0) return;
    ::close(fd_);
    fd_ = -1;
    // Removed only if it is still the file this panel bound (not one a later panel put there)
    struct stat now {};
    if (::lstat(path_.c_str(), &now) == 0 && static_cast<unsigned long long>(now.st_ino) == inode_) {
        ::unlink(path_.c_str());
    }
}

std::vector<Request> Server::poll(double now) {
    std::vector<Request> requests;
    if (fd_ < 0) return requests;
    for (;;) {
        const int client = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0) break;  // EAGAIN: no one else waiting
        ucred peer {};
        socklen_t size = sizeof(peer);
        if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::getuid()) {
            log_.push_back("refused a connection from another user (uid " + std::to_string(peer.uid) + ")");
            ::close(client);
            continue;
        }
        if (clients_.size() >= kMaxClients) {
            log_.push_back("too many requests waiting; closed one");
            ::close(client);
            continue;
        }
        Client added;
        added.id = nextId_++;
        added.fd = client;
        added.pid = static_cast<int>(peer.pid);
        added.since = now;
        clients_.push_back(added);
    }
    for (Client& client : clients_) {
        if (client.fd < 0 || client.taken) continue;
        bool closed = false;
        char buffer[kMaxLine];
        for (;;) {
            const ssize_t n = ::recv(client.fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                client.received.append(buffer, static_cast<size_t>(n));
                if (client.received.size() > kMaxLine) break;
                continue;
            }
            if (n == 0) {
                closed = true;
            } else if (errno == EINTR) {
                continue;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                closed = true;
            }
            break;
        }
        const size_t end = client.received.find('\n');
        if (end == std::string::npos || end + 1 > kMaxLine) {
            // No whole line: too long, given up on, gone, or still to come
            std::string why;
            if (end != std::string::npos || client.received.size() >= kMaxLine) {
                why = "a request too long";
            } else if (now - client.since > kRequestTimeoutSec) {
                why = "no request in time";
            } else if (!closed) {
                continue;
            }
            if (!why.empty()) log_.push_back("dropped a connection: " + why);
            ::close(client.fd);
            client.fd = -1;
            continue;
        }
        std::string line = client.received.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Action action = Action::Open;
        if (!parseAction(line, action)) {
            log_.push_back("unknown action \"" + printable(line, kMaxNameEcho) + "\" (PID " + std::to_string(client.pid) +
                           ")");
            reply(client, Code::Usage, "unknown action");
            continue;
        }
        client.taken = true;
        requests.push_back({client.id, action, client.pid});
    }
    prune();
    return requests;
}

void Server::answer(int id, Code code, const std::string& text) {
    for (Client& client : clients_) {
        if (client.id == id && client.fd >= 0) reply(client, code, text);
    }
    prune();
}

std::vector<std::string> Server::takeLog() {
    std::vector<std::string> lines;
    lines.swap(log_);
    return lines;
}

void Server::reply(Client& client, Code code, const std::string& text) {
    const std::string line = formatReply(code, text);
    // A fresh connection's buffer takes one short line at once; if the client has gone, there is nobody to tell
    (void)::send(client.fd, line.data(), line.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    ::close(client.fd);
    client.fd = -1;
}

void Server::prune() {
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [](const Client& c) { return c.fd < 0; }),
                   clients_.end());
}

}  // namespace action_socket
