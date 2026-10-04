// The eye cameras' setup outside the panel: Konsole and Chromium started on their own, and the password check.
#include "setup_tools.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

extern char** environ;

namespace setup_tools {

namespace {

/**
 * Whether a program can be found in PATH (or is a path that can be run).
 * @param name the program
 * @return true if found
 */
bool inPath(const std::string& name) {
    if (name.find('/') != std::string::npos) return ::access(name.c_str(), X_OK) == 0;
    const char* path = std::getenv("PATH");
    const std::string dirs = path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t from = 0;
    while (from <= dirs.size()) {
        const size_t colon = std::min(dirs.find(':', from), dirs.size());
        const std::string dir = dirs.substr(from, colon - from);
        if (!dir.empty() && ::access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        from = colon + 1;
    }
    return false;
}

}  // namespace

std::vector<std::string> konsoleArgv(const std::string& command, Language language) {
    const bool ja = language == Language::Ja;
    const std::string say = ja ? "Enter を押すと実行するよ（パスワードを聞かれるよ）"
                               : "Press Enter to run it (it asks for your password)";
    const std::string close = ja ? "Enter で閉じるよ" : "Press Enter to close";
    const std::string script = "echo \"" + say + "\"; read -e -p \"$ \" -i \"" + command +
                               "\" c && eval \"$c\"; echo; read -p \"" + close + "\" _";
    return {"konsole", "-e", "bash", "-c", script};
}

std::vector<std::string> setupKonsoleArgv(int button, Language language) {
    return konsoleArgv(button == 1 ? kPasswdCommand : kInstallCommand, language);
}

std::vector<std::string> videoArgv(const std::string& url) {
    return {"flatpak", "run", "org.chromium.Chromium", url};
}

bool spawnDetached(const std::vector<std::string>& argv, std::string& error) {
    if (argv.empty()) {
        error = "nothing to run";
        return false;
    }
    if (!inPath(argv[0])) {
        error = argv[0] + " not found";
        return false;
    }
    // Everything the children need, made before forking (only async-signal-safe calls after it)
    std::vector<char*> args;
    for (const std::string& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    const pid_t child = ::fork();
    if (child < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        // A session of its own, then once more so the program is init's child and never the panel's zombie
        ::setsid();
        const pid_t grandchild = ::fork();
        if (grandchild != 0) ::_exit(grandchild < 0 ? 1 : 0);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            ::dup2(null, 0);
            ::dup2(null, 1);
            ::dup2(null, 2);
            if (null > 2) ::close(null);
        }
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "fork failed in the child";
        return false;
    }
    return true;
}

PasswordCheck::~PasswordCheck() {
    if (pid_ > 0) {
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, nullptr, 0);
    }
}

bool PasswordCheck::tick(bool wanted, double now) {
    const eyecam::PasswordState before = state_;
    if (pid_ > 0) {
        int status = 0;
        const pid_t done = ::waitpid(pid_, &status, WNOHANG);
        if (done == pid_) {
            pid_ = -1;
            state_ = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? eyecam::PasswordState::Set
                     : WIFEXITED(status) && WEXITSTATUS(status) != 127 ? eyecam::PasswordState::NotSet
                                                                       : eyecam::PasswordState::Unknown;
        } else if (now >= startedAt_ + kPasswordTimeoutSec) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
            pid_ = -1;
            state_ = eyecam::PasswordState::Unknown;
        }
    } else if (!wanted) {
        // Not kept while it doesn't matter: when the step comes back (the tool removed), it is checked again at once
        state_ = eyecam::PasswordState::Unknown;
        nextAt_ = now;
    } else if (now >= nextAt_) {
        nextAt_ = now + kPasswordCheckSec;
        if (!inPath("steamos-passwd")) {
            state_ = eyecam::PasswordState::Unknown;
        } else {
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
            posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
            posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
            char name[] = "steamos-passwd";
            char flag[] = "--has-password";
            char* args[] = {name, flag, nullptr};
            pid_t pid = -1;
            if (posix_spawnp(&pid, name, &actions, nullptr, args, environ) == 0) {
                pid_ = pid;
                startedAt_ = now;
            }
            posix_spawn_file_actions_destroy(&actions);
        }
    }
    return state_ != before;
}

}  // namespace setup_tools
