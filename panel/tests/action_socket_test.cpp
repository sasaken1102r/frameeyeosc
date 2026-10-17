// Tests for the actions from outside the panel (action_socket.cpp): what each action does in each state (the exit
// codes), the reply line, the command's side against no panel, a stale socket, a silent and a broken panel, and the
// panel's listening socket with real connections. Sockets are made in the current folder (the build folder under
// ctest), not in /tmp, and removed by the code under test. Built with the panel as action-socket-test; exits non-zero
// on failure.
#include "action_socket.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

/**
 * Record a failed check.
 * @param ok the check
 * @param what what was checked
 * @param line where
 */
void check(bool ok, const char* what, int line) {
    if (ok) return;
    ++gFailures;
    std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
}

#define CHECK(condition) check((condition), #condition, __LINE__)

using namespace action_socket;

/**
 * A socket path of this test's own in the current folder.
 * @param name what it is for
 * @return the path
 */
std::string testPath(const char* name) {
    return std::string("action-socket-test-") + name + "-" + std::to_string(::getpid()) + ".sock";
}

/**
 * A socket address for a path.
 * @param path the path
 * @return the address
 */
sockaddr_un addressOf(const std::string& path) {
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

/**
 * A listening socket that the test drives by hand (a stand-in for a panel that misbehaves).
 * @param path where
 * @return its descriptor
 */
int rawListener(const std::string& path) {
    ::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const sockaddr_un address = addressOf(path);
    ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    ::listen(fd, 4);
    return fd;
}

/**
 * Connect, send a line and read what comes back until the panel closes the connection, polling the server meanwhile.
 * @param server the panel's side
 * @param path its socket
 * @param line what to send (as is)
 * @param code what the caller answers each request it is handed with (unknown ones the server answers itself)...
 * @param text ...and the text
 * @return what came back
 */
std::string exchange(Server& server, const std::string& path, const std::string& line, Code code = Code::Accepted,
                     const char* text = "ok") {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const sockaddr_un address = addressOf(path);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return "(no connection)";
    }
    (void)::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
    ::shutdown(fd, SHUT_WR);
    for (int pass = 0; pass < 50; ++pass) {
        for (const Request& request : server.poll(0.0)) server.answer(request.id, code, text);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::string received;
    char buffer[256];
    ssize_t n = 0;
    while ((n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT)) > 0) received.append(buffer, static_cast<size_t>(n));
    ::close(fd);
    return received;
}

void testNames() {
    Action action = Action::Open;
    CHECK(parseAction("recenter", action) && action == Action::Recenter);
    CHECK(parseAction("fit", action) && action == Action::Fit);
    CHECK(parseAction("stop", action) && action == Action::Stop);
    CHECK(parseAction("open", action) && action == Action::Open);
    CHECK(!parseAction("bogus", action) && !parseAction("", action) && !parseAction("Recenter", action) &&
          !parseAction("fit ", action));
    CHECK(std::string(actionName(Action::Recenter)) == "recenter");
    CHECK(actionNames() == "recenter|fit|stop|open");
}

void testDecide() {
    State ready;
    ready.running = true;
    ready.tracking = true;
    ready.gazeFitted = true;
    // Both start when the buttons would; the effect says which
    Decision d = decide(Action::Recenter, ready);
    CHECK(d.code == Code::Accepted && d.effect == Effect::StartRecenter && d.text == "started");
    d = decide(Action::Fit, ready);
    CHECK(d.code == Code::Accepted && d.effect == Effect::StartFit);
    // The whole fit needs no earlier fit; the re-center does
    State unfitted = ready;
    unfitted.gazeFitted = false;
    CHECK(decide(Action::Fit, unfitted).code == Code::Accepted);
    d = decide(Action::Recenter, unfitted);
    CHECK(d.code == Code::NotFitted && d.effect == Effect::None);
    // Busy: a fit or a re-center (by itself, say) runs; stop it first. Stop says which it stopped
    State fitting = ready;
    fitting.fitActive = true;
    d = decide(Action::Fit, fitting);
    CHECK(d.code == Code::Busy && d.effect == Effect::None && d.text == "busy (a fit is running; use --action stop)");
    fitting.fitIsRecenter = true;
    d = decide(Action::Recenter, fitting);
    CHECK(d.code == Code::Busy && d.text == "busy (a re-center is running; use --action stop)");
    d = decide(Action::Stop, fitting);
    CHECK(d.code == Code::Accepted && d.effect == Effect::StopFit && d.text == "stopped the re-center");
    fitting.fitIsRecenter = false;
    CHECK(decide(Action::Stop, fitting).text == "stopped the eye fit");
    // Nothing to stop
    d = decide(Action::Stop, ready);
    CHECK(d.code == Code::NothingToStop && d.effect == Effect::None && d.text == "nothing to stop");
    CHECK(decide(Action::Stop, State()).code == Code::NothingToStop);
    // frameeyeosc not running, or no eye data
    State stopped = ready;
    stopped.running = false;
    d = decide(Action::Fit, stopped);
    CHECK(d.code == Code::NoEyeData && d.text == "frameeyeosc is not running");
    State blind = ready;
    blind.tracking = false;
    d = decide(Action::Recenter, blind);
    CHECK(d.code == Code::NoEyeData && d.text == "no eye data from frameeyeosc");
    // Locked by frameeyeosc's command line (the buttons are grey)
    State locked = ready;
    locked.locked = true;
    CHECK(decide(Action::Fit, locked).code == Code::Locked && decide(Action::Recenter, locked).code == Code::Locked);
    // Busy goes before the rest (the panel shows only "Stop" then)
    State busyStopped = fitting;
    busyStopped.running = false;
    CHECK(decide(Action::Fit, busyStopped).code == Code::Busy);
    // Open always opens; in any state
    d = decide(Action::Open, State());
    CHECK(d.code == Code::Accepted && d.effect == Effect::ShowPanel);
    CHECK(decide(Action::Open, fitting).code == Code::Accepted);
    // The fixed numbers other apps rely on
    CHECK(static_cast<int>(Code::Accepted) == 0 && static_cast<int>(Code::Usage) == 1 &&
          static_cast<int>(Code::NotRunning) == 2 && static_cast<int>(Code::NoAnswer) == 3 &&
          static_cast<int>(Code::NoEyeData) == 4 && static_cast<int>(Code::NotFitted) == 5 &&
          static_cast<int>(Code::Busy) == 6 && static_cast<int>(Code::NothingToStop) == 7 &&
          static_cast<int>(Code::Locked) == 8);
}

void testReplyLine() {
    CHECK(formatReply(Code::Busy, "busy (a fit is running; use --action stop)") ==
          "6 busy (a fit is running; use --action stop)\n");
    // Control characters never go out; the line stays within kMaxLine
    CHECK(formatReply(Code::Accepted, "a\nb\tc") == "0 abc\n");
    CHECK(formatReply(Code::Accepted, std::string(500, 'x')).size() == kMaxLine);
    int code = -1;
    std::string text;
    CHECK(parseReply("0 started\n", code, text) && code == 0 && text == "started");
    CHECK(parseReply("7 nothing to stop\r\n", code, text) && code == 7 && text == "nothing to stop");
    CHECK(parseReply("9", code, text) && code == 9 && text.empty());  // a later code from a newer panel
    CHECK(!parseReply("", code, text));
    CHECK(!parseReply("\n", code, text));
    CHECK(!parseReply("ok\n", code, text));
    CHECK(!parseReply("6x busy\n", code, text));
    CHECK(!parseReply("100 too big\n", code, text));
    CHECK(!parseReply("-1 negative\n", code, text));
    CHECK(!parseReply("0 " + std::string(200, 'x'), code, text));
    // Round trip
    CHECK(parseReply(formatReply(Code::Locked, "locked"), code, text) && code == 8 && text == "locked");
    CHECK(resultLine("recenter", "started") == "recenter: started");
    CHECK(resultLine("bad\x1b[2Jname", "x") == "bad[2Jname: x");
}

void testDefaultPath() {
    const char* saved = std::getenv("XDG_RUNTIME_DIR");
    const std::string keep = saved != nullptr ? saved : "";
    ::setenv("XDG_RUNTIME_DIR", "/run/user/4242", 1);
    CHECK(defaultSocketPath() == "/run/user/4242/frameeyeosc-panel.sock");
    if (saved != nullptr) {
        ::setenv("XDG_RUNTIME_DIR", keep.c_str(), 1);
    } else {
        ::unsetenv("XDG_RUNTIME_DIR");
    }
}

void testCommandWithoutPanel() {
    std::string line;
    // Bad arguments and unknown actions: 1, without connecting
    CHECK(runCommand({"--action"}, "unused.sock", 100, line) == 1);
    CHECK(line == "action: bad arguments (usage: frameeyeosc-panel --action recenter|fit|stop|open)");
    CHECK(runCommand({"--action", "fit", "extra"}, "unused.sock", 100, line) == 1);
    CHECK(runCommand({"--version", "--action", "fit"}, "unused.sock", 100, line) == 1);
    CHECK(runCommand({"--action", "bogus"}, "unused.sock", 100, line) == 1);
    CHECK(line == "bogus: unknown action (use recenter|fit|stop|open)");
    // No socket file: the panel isn't running (or is one without this socket)
    const std::string path = testPath("none");
    ::unlink(path.c_str());
    CHECK(runCommand({"--action", "recenter"}, path, 500, line) == 2);
    CHECK(line == "recenter: panel not running");
    CHECK(runCommand({"--action", "fit"}, "", 500, line) == 2);  // no runtime folder
    // A socket file nobody listens on (a panel that crashed): also not running
    const int stale = rawListener(path);
    ::close(stale);
    CHECK(::access(path.c_str(), F_OK) == 0);
    CHECK(runCommand({"--action", "stop"}, path, 500, line) == 2);
    CHECK(line == "stop: panel not running");
    // ...and a new panel replaces it
    Server server;
    std::string error;
    CHECK(server.open(path, error));
    server.close();
    CHECK(::access(path.c_str(), F_OK) != 0);
}

void testSilentAndBrokenPanel() {
    std::string line;
    // Listening but never answering: 3 after the timeout (the connection itself goes through the backlog)
    const std::string path = testPath("silent");
    const int silent = rawListener(path);
    const auto start = std::chrono::steady_clock::now();
    CHECK(runCommand({"--action", "fit"}, path, 200, line) == 3);
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(waited >= 0.15 && waited < 1.5);
    CHECK(line == "fit: no answer from the panel");
    ::close(silent);
    ::unlink(path.c_str());
    // Answering something that isn't a reply: 3
    const std::string brokenPath = testPath("broken");
    const int broken = rawListener(brokenPath);
    std::thread panel([broken]() {
        const int client = ::accept(broken, nullptr, nullptr);
        char buffer[64];
        (void)::recv(client, buffer, sizeof(buffer), 0);
        const char junk[] = "hello there\n";
        (void)::send(client, junk, sizeof(junk) - 1, MSG_NOSIGNAL);
        ::close(client);
    });
    CHECK(runCommand({"--action", "recenter"}, brokenPath, 1000, line) == 3);
    CHECK(line == "recenter: no answer from the panel (not a reply)");
    panel.join();
    ::close(broken);
    ::unlink(brokenPath.c_str());
}

/**
 * Run the command on its own thread against a server polled here, the server answering with decide() for a state.
 * @param server the panel's side, open on path
 * @param path its socket
 * @param action the action's name
 * @param state the panel's state
 * @param line set to the command's line
 * @param effect set to what the panel would have done
 * @return the command's exit code
 */
int roundTrip(Server& server, const std::string& path, const std::string& action, const State& state,
              std::string& line, Effect& effect) {
    std::atomic<int> code {-1};
    std::thread command([&]() { code = runCommand({"--action", action}, path, 2000, line); });
    effect = Effect::None;
    // As the panel's loop does: poll, decide, do, answer (a few passes until the request has come)
    for (int pass = 0; pass < 400 && code < 0; ++pass) {
        for (const Request& request : server.poll(0.0)) {
            const Decision decision = decide(request.action, state);
            effect = decision.effect;
            CHECK(request.peerPid == ::getpid());
            server.answer(request.id, decision.code, decision.text);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    command.join();
    return code;
}

void testServer() {
    const std::string path = testPath("panel");
    Server server;
    std::string error;
    CHECK(server.open(path, error));
    CHECK(server.isOpen());
    // Only the user can reach it
    struct stat info {};
    CHECK(::stat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode) && (info.st_mode & 0777) == 0600);
    CHECK(info.st_uid == ::getuid());

    State ready;
    ready.running = true;
    ready.tracking = true;
    ready.gazeFitted = true;
    std::string line;
    Effect effect = Effect::None;
    CHECK(roundTrip(server, path, "recenter", ready, line, effect) == 0);
    CHECK(line == "recenter: started" && effect == Effect::StartRecenter);
    CHECK(roundTrip(server, path, "fit", ready, line, effect) == 0 && effect == Effect::StartFit);
    // Busy
    State fitting = ready;
    fitting.fitActive = true;
    CHECK(roundTrip(server, path, "fit", fitting, line, effect) == 6);
    CHECK(line == "fit: busy (a fit is running; use --action stop)" && effect == Effect::None);
    // Stop with nothing running, then with a fit running
    CHECK(roundTrip(server, path, "stop", ready, line, effect) == 7);
    CHECK(line == "stop: nothing to stop");
    CHECK(roundTrip(server, path, "stop", fitting, line, effect) == 0 && effect == Effect::StopFit);
    CHECK(line == "stop: stopped the eye fit");
    // Not fitted, frameeyeosc not running, locked
    State unfitted = ready;
    unfitted.gazeFitted = false;
    CHECK(roundTrip(server, path, "recenter", unfitted, line, effect) == 5);
    CHECK(line == "recenter: no gaze fit yet (use --action fit first)");
    CHECK(roundTrip(server, path, "fit", State(), line, effect) == 4);
    CHECK(line == "fit: frameeyeosc is not running");
    State locked = ready;
    locked.locked = true;
    CHECK(roundTrip(server, path, "recenter", locked, line, effect) == 8);
    CHECK(roundTrip(server, path, "open", State(), line, effect) == 0 && effect == Effect::ShowPanel);
    CHECK(line == "open: panel shown");

    // On the wire: an unknown action is answered by the server itself (a newer command, an older panel), and never
    // handed to the caller
    CHECK(exchange(server, path, "bogus\n", Code::Accepted, "should not") == "1 unknown action\n");
    CHECK(exchange(server, path, "stop\r\n", Code::NothingToStop, "nothing to stop") == "7 nothing to stop\n");
    // Too long, or no line at all: closed without an answer
    CHECK(exchange(server, path, std::string(300, 'a') + "\n").empty());
    CHECK(exchange(server, path, "fit").empty());
    // Two requests in one line's place: only the first line counts
    CHECK(exchange(server, path, "open\nfit\n", Code::Accepted, "panel shown") == "0 panel shown\n");
    for (const std::string& entry : server.takeLog()) std::printf("  server: %s\n", entry.c_str());

    // A connection that sends nothing is dropped after kRequestTimeoutSec
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const sockaddr_un address = addressOf(path);
        CHECK(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(server.poll(100.0).empty());
        CHECK(server.poll(100.0 + kRequestTimeoutSec + 0.1).empty());
        char byte = 0;
        CHECK(::recv(fd, &byte, 1, 0) == 0);  // closed by the panel
        ::close(fd);
        const std::vector<std::string> log = server.takeLog();
        CHECK(log.size() == 1 && log[0] == "dropped a connection: no request in time");
    }
    // A command that gave up before the answer: the answer goes nowhere, and nothing breaks
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const sockaddr_un address = addressOf(path);
        CHECK(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        (void)::send(fd, "fit\n", 4, MSG_NOSIGNAL);
        ::close(fd);
        std::vector<Request> requests;
        for (int pass = 0; pass < 50 && requests.empty(); ++pass) {
            requests = server.poll(0.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(requests.size() == 1 && requests[0].action == Action::Fit);
        if (!requests.empty()) server.answer(requests[0].id, Code::Accepted, "started");
    }

    // Closed: the file goes, and the command hears the panel isn't running
    server.close();
    CHECK(!server.isOpen());
    CHECK(::access(path.c_str(), F_OK) != 0);
    CHECK(runCommand({"--action", "fit"}, path, 500, line) == 2);

    // Something at the path that isn't a socket is left alone (not listening)
    { std::ofstream(path) << "not a socket\n"; }
    CHECK(!server.open(path, error));
    CHECK(error.find("is not a socket") != std::string::npos);
    CHECK(::access(path.c_str(), F_OK) == 0);
    ::unlink(path.c_str());  // the test's own file
    // A path that doesn't fit, or none
    CHECK(!server.open(std::string(200, 'p'), error));
    CHECK(!server.open("", error));
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testNames();
    testDecide();
    testReplyLine();
    testDefaultPath();
    testCommandWithoutPanel();
    testSilentAndBrokenPanel();
    testServer();
    if (gFailures == 0) std::printf("action-socket-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
