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
/** How often the password is checked again while the setup may need it (s). */
constexpr double kPasswordCheckSec = 4.0;
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
 * Start a program and leave it: its own session, stdin / stdout / stderr on /dev/null, the panel's environment
 * (DISPLAY: the window shows in the headset), and never waited for (it is reparented to init, so no zombie).
 * @param argv the program and its arguments (looked up in PATH)
 * @param error why it couldn't start (not found, fork failed)
 * @return true if started
 */
bool spawnDetached(const std::vector<std::string>& argv, std::string& error);

/**
 * "steamos-passwd --has-password" in the background (it only reads: passwd with no input, and whether it asks for
 * the current password), checked again every kPasswordCheckSec while wanted. Exit 0 = set, another exit = not set;
 * no steamos-passwd, or no answer in kPasswordTimeoutSec = unknown.
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
    eyecam::PasswordState state_ = eyecam::PasswordState::Unknown;
};

}  // namespace setup_tools
