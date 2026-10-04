// What the eye cameras' setup starts outside the panel: a Konsole with the command typed in (the user presses Enter
// and types the password; the panel never runs sudo or passwd itself), the setup video in Chromium, and the
// read-only check for a SteamOS password. Everything here works without OpenVR and cairo (eyecam-test checks the
// argument lists; the spawning is only reached from the VR loop, never from --dump-png).
#pragma once

#include "eyecam.h"
#include "i18n.h"

#include <sys/types.h>

#include <string>
#include <vector>

namespace setup_tools {

/** The command (2) types into its Konsole ($HOME is expanded by bash there). */
constexpr const char* kInstallCommand = "sudo $HOME/.local/lib/eyecam/install_grab.sh";
/** ...as the checklist shows it, to type over SSH. */
constexpr const char* kShownInstallCommand = "sudo ~/.local/lib/eyecam/install_grab.sh";
/** The command (1) types into its Konsole. */
constexpr const char* kPasswdCommand = "passwd";
/** The setup video. None yet: while it is empty, its button and the line under it don't show. */
constexpr const char* kVideoUrl = "";
/** How often the password is checked again while the setup may need it, at first (s)... */
constexpr double kPasswordCheckSec = 4.0;
/** ...doubling while it stays the same, up to this (s). */
constexpr double kPasswordCheckMaxSec = 30.0;
/**
 * What a program started through systemd-run takes along from the panel's environment (only those set): the display
 * and session Konsole needs. The rest comes from the user's systemd, as for the panel itself.
 */
constexpr const char* kSpawnEnvironment[] = {"DISPLAY",         "WAYLAND_DISPLAY", "XAUTHORITY",
                                             "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP",
                                             "DBUS_SESSION_BUS_ADDRESS", "LANG", "LC_ALL"};
/** The longest a password check may take before it counts as unknown (s). */
constexpr double kPasswordTimeoutSec = 5.0;

/**
 * A Konsole that shows one line, waits with the command typed in (bash's read -e -i), runs it on Enter, and waits
 * for Enter before it closes.
 * @param command the command typed in ("sudo $HOME/..." or "passwd")
 * @param language the panel's language (the two lines it says)
 * @return the argument list: konsole -e bash -c '<script>'
 */
std::vector<std::string> konsoleArgv(const std::string& command, Language language);

/**
 * The Konsole a setup button opens, in the panel's language.
 * @param button 0 = the tool's install (2), 1 = passwd (1) (PanelAction::SetupKonsole's arg)
 * @param language the panel's language now
 * @return konsoleArgv for its command
 */
std::vector<std::string> setupKonsoleArgv(int button, Language language);

/**
 * The setup video in the Frame's Chromium (not xdg-open: https opens Discover there).
 * @param url the video
 * @return the argument list: flatpak run org.chromium.Chromium <url>
 */
std::vector<std::string> videoArgv(const std::string& url);

/**
 * The command line that starts a program outside the panel's own service: through "systemd-run --user --collect" as
 * a transient unit of its own, so it outlives a panel restart (the panel is a systemd user service: its cgroup goes
 * with it) while the user types a password into Konsole, taking along the variables of kSpawnEnvironment that are set
 * (-E NAME takes the value from systemd-run's environment). Without systemd-run, the program as it is.
 * @param argv the program and its arguments
 * @param systemdRun systemd-run can be used
 * @param set whether a variable is set in the panel's environment
 * @return the command line
 */
std::vector<std::string> detachedArgv(const std::vector<std::string>& argv, bool systemdRun,
                                      bool (*set)(const char* name));

/**
 * Start a program and leave it (detachedArgv: through systemd-run when there is one): its own session, stdin / stdout /
 * stderr on /dev/null, no other descriptor of the panel's, the panel's environment (DISPLAY: the window shows in the
 * headset), and never waited for (it is reparented to init, so no zombie).
 * @param argv the program and its arguments (looked up in PATH)
 * @param error why it couldn't start (not found, fork failed)
 * @return true if started
 */
bool spawnDetached(const std::vector<std::string>& argv, std::string& error);

/**
 * How long until the password is checked again: kPasswordCheckSec after it changed (or at first), else twice the last
 * wait, up to kPasswordCheckMaxSec.
 * @param last the last wait (s)
 * @param changed the check's answer differs from the one before
 * @return the next wait (s)
 */
double nextPasswordWait(double last, bool changed);

/**
 * "steamos-passwd --has-password" in the background (it only reads: passwd with no input, and whether it asks for
 * the current password), checked while wanted (the panel open on the dashboard at the setup's (1) or (2)): at once, then
 * every kPasswordCheckSec, waiting longer while the answer stays the same (nextPasswordWait, up to
 * kPasswordCheckMaxSec). Forgotten (unknown) while not wanted, so it is checked afresh when it is wanted again. Exit 0 =
 * set, another exit = not set; no steamos-passwd, or no answer in kPasswordTimeoutSec = unknown.
 */
class PasswordCheck {
public:
    PasswordCheck() = default;
    ~PasswordCheck();
    PasswordCheck(const PasswordCheck&) = delete;
    PasswordCheck& operator=(const PasswordCheck&) = delete;

    /**
     * Start a check when one is due, and take its result when it has come. Never waits.
     * @param wanted the setup may still need the password (not done, the tool not in)
     * @param now monotonic seconds
     * @return true if the state changed in this call
     */
    bool tick(bool wanted, double now);

    /** @return the state as last checked */
    eyecam::PasswordState state() const { return state_; }

private:
    pid_t pid_ = -1;
    double startedAt_ = 0.0;
    double nextAt_ = 0.0;
    double wait_ = kPasswordCheckSec;  ///< until the check after the one running or done
    eyecam::PasswordState state_ = eyecam::PasswordState::Unknown;
};

}  // namespace setup_tools
