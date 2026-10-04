// The eye log: starting, stopping and reaping `frameeyeosc --record`.
#include "recorder.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace recorder {

namespace {

/**
 * Create a folder and its parents.
 * @param dir the folder
 * @return true if it exists now
 */
bool makeDirectories(const std::string& dir) {
    for (size_t slash = dir.find('/', 1); ; slash = dir.find('/', slash + 1)) {
        const std::string part = dir.substr(0, slash);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
        if (slash == std::string::npos) break;
    }
    struct stat info {};
    return ::stat(dir.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

/**
 * The last non-empty line of a file, for saying why the child stopped.
 * @param path the file
 * @return the line (at most 160 characters), or ""
 */
std::string lastLine(const std::string& path) {
    std::ifstream file(path);
    std::string line;
    std::string last;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) last = line;
    }
    if (last.size() > 160) last = last.substr(0, 157) + "...";
    return last;
}

/**
 * A file's size in MB, for the log.
 * @param path the file
 * @return the size (0 if it is not there)
 */
double sizeMb(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 ? info.st_size / 1e6 : 0.0;
}

}  // namespace

std::string defaultDir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg != nullptr && xdg[0] == '/') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home != nullptr ? home : ".") + "/.local/share";
    }
    return base + "/frameeyeosc/recordings";
}

std::string shortPath(const std::string& path) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return path;
    const std::string prefix = std::string(home) + "/";
    return path.rfind(prefix, 0) == 0 ? "~/" + path.substr(prefix.size()) : path;
}

std::string fileStamp(std::time_t when) {
    std::tm local {};
    localtime_r(&when, &local);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d_%H-%M-%S", &local);
    return text;
}

std::string elapsedText(double seconds) {
    const long total = static_cast<long>(std::max(0.0, seconds));
    char text[32];
    std::snprintf(text, sizeof(text), "%ld:%02ld", total / 60, total % 60);
    return text;
}

std::string findFrameeyeosc() {
    // Installed next to each other (~/.local/bin, or the unpacked release)
    char self[4096];
    const ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        std::string dir(self);
        dir = dir.substr(0, dir.find_last_of('/'));
        const std::string candidate = dir + "/frameeyeosc";
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    const char* path = std::getenv("PATH");
    std::stringstream dirs(path != nullptr ? path : "");
    std::string dir;
    while (std::getline(dirs, dir, ':')) {
        if (dir.empty() || dir[0] != '/') continue;
        const std::string candidate = dir + "/frameeyeosc";
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return "";
}

std::string uniqueBase(const std::string& dir, const std::string& stamp) {
    const std::string first = dir + "/eyes_" + stamp;
    for (int n = 1;; ++n) {
        const std::string base = n == 1 ? first : first + "_" + std::to_string(n);
        bool taken = false;
        for (const char* extension : {".csv", ".log", ".config.json"}) {
            taken |= ::access((base + extension).c_str(), F_OK) == 0;
        }
        if (!taken) return base;
    }
}

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

void cloexecFrom3() {
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, 3U, ~0U, CLOSE_RANGE_CLOEXEC) == 0) return;
#endif
    const int dir = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) {
        for (int fd = 3; fd < 65536; ++fd) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        return;
    }
    alignas(8) char buffer[4096];
    for (;;) {
        const long n = ::syscall(SYS_getdents64, dir, buffer, sizeof(buffer));
        if (n <= 0) break;
        for (long at = 0; at < n;) {
            const auto* entry = reinterpret_cast<const struct dirent64*>(buffer + at);
            int fd = 0;
            bool number = entry->d_name[0] != '\0';
            for (const char* c = entry->d_name; *c != '\0'; ++c) {
                number &= *c >= '0' && *c <= '9';
                fd = fd * 10 + (*c - '0');
            }
            if (number && fd >= 3 && fd != dir) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            at += entry->d_reclen;
        }
    }
    ::close(dir);
}

Recorder::~Recorder() {
    shutdown();
}

bool Recorder::start(const std::string& program, const std::string& dir, const std::string& configPath, double now) {
    if (busy()) return false;
    error_.clear();
    if (program.empty()) {
        error_ = "frameeyeosc was not found next to the panel or on the PATH";
        std::fprintf(stderr, "[record] can't start: %s\n", error_.c_str());
        return false;
    }
    if (!makeDirectories(dir)) {
        error_ = "can't create " + shortPath(dir) + ": " + std::strerror(errno);
        std::fprintf(stderr, "[record] can't start: %s\n", error_.c_str());
        return false;
    }
    const std::string base = uniqueBase(dir, fileStamp(std::time(nullptr)));
    csv_ = base + ".csv";
    log_ = base + ".log";
    // The settings in use, to know later what the recording was made with
    {
        std::ifstream in(configPath, std::ios::binary);
        if (in) {
            std::ofstream out(base + ".config.json", std::ios::binary | std::ios::trunc);
            out << in.rdbuf();
        }
    }
    const int logFd = ::open(log_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    const int nullFd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    int pipeFds[2] = {-1, -1};
    if (logFd < 0 || nullFd < 0 || ::pipe2(pipeFds, O_CLOEXEC) != 0) {
        error_ = std::string("can't prepare the recording: ") + std::strerror(errno);
        std::fprintf(stderr, "[record] can't start: %s\n", error_.c_str());
        for (int fd : {logFd, nullFd, pipeFds[0], pipeFds[1]}) {
            if (fd >= 0) ::close(fd);
        }
        return false;
    }
    // Everything the child needs is made before the fork: after it, only async-signal-safe calls
    std::vector<std::string> args = {program, "--record", csv_};
    std::vector<char*> argv;
    for (std::string& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid == 0) {
        // Stopped like Ctrl+C if the panel dies without stopping it; no orphan keeps recording
        ::prctl(PR_SET_PDEATHSIG, SIGINT);
        if (::getppid() != parent) ::_exit(1);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::dup2(nullFd, 0);
        ::dup2(nullFd, 1);
        ::dup2(logFd, 2);
        // Nothing else of the panel's (its lock file, sockets, the GPU) goes to the recorder
        cloexecFrom3();
        ::execv(argv[0], argv.data());
        const int error = errno;
        ssize_t ignored = ::write(pipeFds[1], &error, sizeof(error));
        (void)ignored;
        ::_exit(127);
    }
    ::close(pipeFds[1]);
    ::close(logFd);
    ::close(nullFd);
    if (pid < 0) {
        error_ = std::string("can't start frameeyeosc: ") + std::strerror(errno);
        ::close(pipeFds[0]);
        std::fprintf(stderr, "[record] %s\n", error_.c_str());
        return false;
    }
    // The pipe closes on a successful exec; otherwise the child sends why it failed
    int execError = 0;
    ssize_t got = 0;
    do {
        got = ::read(pipeFds[0], &execError, sizeof(execError));
    } while (got < 0 && errno == EINTR);
    ::close(pipeFds[0]);
    if (got == static_cast<ssize_t>(sizeof(execError))) {
        ::waitpid(pid, nullptr, 0);
        error_ = "can't run " + program + ": " + std::strerror(execError);
        std::fprintf(stderr, "[record] %s\n", error_.c_str());
        return false;
    }
    pid_ = pid;
    stopping_ = false;
    autoStopped_ = false;
    startedAt_ = now;
    std::fprintf(stderr, "[record] started: %s (pid %d, %s)\n", csv_.c_str(), static_cast<int>(pid), program.c_str());
    return true;
}

void Recorder::stop(double now) {
    if (!recording()) return;
    std::fprintf(stderr, "[record] stopping after %s%s\n", elapsedText(now - startedAt_).c_str(),
                 autoStopped_ ? " (the 60-minute limit)" : "");
    ::kill(pid_, SIGINT);
    stopping_ = true;
    stopAt_ = now;
}

bool Recorder::poll(double now) {
    if (!busy()) return false;
    int status = 0;
    const pid_t done = ::waitpid(pid_, &status, WNOHANG);
    if (done == pid_ || (done < 0 && errno == ECHILD)) {
        finished(done == pid_ ? status : 0, now);
        return true;
    }
    if (!stopping_ && now - startedAt_ >= kMaxSec) {
        autoStopped_ = true;
        stop(now);
        return true;
    }
    if (stopping_ && now - stopAt_ >= kStopWaitSec) {
        std::fprintf(stderr, "[record] frameeyeosc did not stop; killing it\n");
        ::kill(pid_, SIGKILL);
        stopAt_ = now + 3600;  // once
    }
    return false;
}

void Recorder::finished(int status, double now) {
    const std::string last = lastLine(log_);
    const bool asked = stopping_;
    pid_ = -1;
    stopping_ = false;
    char how[64];
    if (WIFEXITED(status)) {
        std::snprintf(how, sizeof(how), "exit code %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        std::snprintf(how, sizeof(how), "signal %d", WTERMSIG(status));
    } else {
        std::snprintf(how, sizeof(how), "ended");
    }
    std::fprintf(stderr, "[record] stopped after %s: %s (%.1f MB; %s; %s)\n", elapsedText(now - startedAt_).c_str(),
                 csv_.c_str(), sizeMb(csv_), how, last.empty() ? "no message" : last.c_str());
    if (!asked) {
        // Nobody asked it to stop: say why, from its own last words if it left any
        error_ = last.empty() ? std::string("frameeyeosc stopped (") + how + ")"
                              : (last.rfind("Error: ", 0) == 0 ? last.substr(7) : last);
    }
}

void Recorder::shutdown() {
    if (!busy()) return;
    const auto monotonic = []() {
        timespec now {};
        ::clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<double>(now.tv_sec) + now.tv_nsec / 1e9;
    };
    stop(monotonic());
    const double end = monotonic() + kStopWaitSec;
    while (busy() && monotonic() < end) {
        poll(monotonic());
        if (busy()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (busy()) {
        std::fprintf(stderr, "[record] frameeyeosc did not stop; killing it\n");
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        finished(status, monotonic());
    }
}

View Recorder::view(double now) const {
    View view;
    view.recording = recording();
    view.elapsedSec = view.recording ? now - startedAt_ : 0.0;
    view.path = csv_;
    view.error = error_;
    view.autoStopped = !busy() && autoStopped_;
    return view;
}

}  // namespace recorder
