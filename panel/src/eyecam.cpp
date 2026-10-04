// The eye capture tab's logic: reading eyecam-rec's status.json, when the tab and the full-view overlay show (and how
// it fades), the light warning before a start, the calibrations (and the note asking for one), the step texts, and
// its control socket.
#include "eyecam.h"

#include "json.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
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
    // (drawn as it is: never anything cairo can't take)
    return value != nullptr && value->isString() ? validUtf8(value->text) : std::string();
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
    if (text == "calibrating") return State::Calibrating;
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
    status.stepRemainingS = status.elapsedS = status.totalS = status.liveMs = status.warmupRemainingS = kNaN;
    status.widenSensitivity = kNaN;
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
    status.messageEn = readText(root, "message_en");
    status.autoGrab = readText(root, "auto_grab");
    const JsonValue* outdated = root.get("grab_outdated");
    status.grabOutdated = outdated != nullptr && outdated->isBool() && outdated->boolean;
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
    // Bits beyond the two known ones are kept, but only the two are used
    status.calibState = std::max(0, readInt(root, "calib_state", 0));
    const JsonValue* recalib = root.get("recalib_suggested");
    status.recalibSuggested = recalib != nullptr && recalib->isBool() && recalib->boolean;
    const JsonValue* live = root.get("live");
    status.live = live != nullptr && live->isBool() && live->boolean;
    status.liveMs = readNumber(root, "live_ms", kNaN);
    // The relaxed eyes learned by itself (missing on an older eyecam-rec, which needs a calibration each wear)
    const JsonValue* baseline = root.get("baseline");
    status.hasBaseline = baseline != nullptr && baseline->isString();
    status.baseline = readText(root, "baseline");
    status.warmupRemainingS = readNumber(root, "warmup_remaining_s", kNaN);
    const JsonValue* saved = root.get("calib_saved");
    status.hasCalibSaved = saved != nullptr && saved->isBool();
    status.calibSaved = status.hasCalibSaved && saved->boolean;
    // The widening sensitivity (missing on an older eyecam-rec: no slider)
    status.widenSensitivity = readNumber(root, "widen_sensitivity", kNaN);
    status.hasWidenSensitivity = std::isfinite(status.widenSensitivity);
    // The setup (missing on an older eyecam-rec: see setupComplete and toolInstalled)
    const JsonValue* buffers = root.get("has_buffers");
    status.hasBuffers = buffers != nullptr && buffers->isBool() && buffers->boolean;
    const JsonValue* setup = root.get("setup_done");
    status.hasSetupDone = setup != nullptr && setup->isBool();
    status.setupDone = status.hasSetupDone && setup->boolean;
    status.lastCalibWiden = readText(root, "last_calib_widen");
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
    // At most kMaxStatusBytes (eyecam-rec writes one short line): a bigger file isn't eyecam-rec's
    std::string text(kMaxStatusBytes + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(file.gcount()));
    if (text.size() > kMaxStatusBytes) {
        Status status = parseStatus("", mtime);
        status.readError = "status.json is too large";
        return status;
    }
    return parseStatus(text, mtime);
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

bool hideLightAtOnce(const View& view, double now) {
    const Status& s = view.status;
    return !tabVisible(s, now) || s.state != State::Recording || age(s, now) > kOverlayStaleSec ||
           (view.busy && view.busyCommand == "stop");
}

Light stepLight(const Light& light, Fill wanted, bool hideNow, double now) {
    Light next = light;
    next.at = now;
    if (hideNow) {
        next.fill = Fill::None;
        next.alpha = 0.0;
        return next;
    }
    // Nothing up: the wanted one (if any) starts from fully clear
    if (light.fill == Fill::None) {
        next.fill = wanted;
        next.alpha = 0.0;
        return next;
    }
    const double dt = std::max(0.0, now - light.at);
    if (light.fill == wanted) {
        // A stalled loop never makes it jump: at most kMaxFadeInStepSec's worth of brightness at a time
        next.alpha = std::min(1.0, light.alpha + std::min(dt, kMaxFadeInStepSec) / kFadeInSec);
        return next;
    }
    // Not wanted any more (its step ended, or the other color is next): out first
    next.alpha = std::max(0.0, light.alpha - dt / kFadeOutSec);
    if (next.alpha <= 0.0) {
        next.fill = wanted;
        next.alpha = 0.0;
    }
    return next;
}

bool lightFading(const Light& light, Fill wanted) {
    if (light.fill != wanted) return true;
    return wanted != Fill::None && light.alpha < 1.0;
}

bool withoutLight(const Status& status) {
    return status.protocol == kNoLightProtocol;
}

std::string startCommand(StartChoice choice) {
    switch (choice) {
        case StartChoice::WithLight: return kStartCommand;
        case StartChoice::WithoutLight: return std::string(kStartCommand) + " " + kNoLightProtocol;
        case StartChoice::Cancel: break;
    }
    return std::string();
}

std::string calibCommand(Calib calib) {
    return calib == Calib::User ? kCalibUserCommand : kCalibWearCommand;
}

bool userCalibAllowed(const Status& status) {
    return (status.calibState & kCalibWearBit) != 0;
}

Run runOfCommand(const std::string& command) {
    if (command == kCalibWearCommand) return Run::CalibWear;
    if (command == kCalibUserCommand) return Run::CalibUser;
    if (command == kStartCommand || command.rfind(std::string(kStartCommand) + " ", 0) == 0) return Run::Recording;
    return Run::None;
}

Run followRun(Run last, const Status& status) {
    switch (status.state) {
        case State::Searching:
        case State::Recording: return Run::Recording;
        case State::Calibrating:
            // eyecam-rec names the calibration as its protocol; the steps say it too (after the countdown)
            if (status.protocol == kCalibUserCommand) return Run::CalibUser;
            if (status.protocol == kCalibWearCommand) return Run::CalibWear;
            switch (parseStep(status.stepLabel)) {
                case Step::Squint:
                case Step::LookUp:
                case Step::LookDown: return Run::CalibUser;
                case Step::Close:
                case Step::Normal:
                case Step::Widen: return Run::CalibWear;
                default: return isCalib(last) ? last : Run::CalibWear;
            }
        default: return last;
    }
}

bool isCalib(Run run) {
    return run == Run::CalibWear || run == Run::CalibUser;
}

bool baselineWarming(const Status& status) {
    return status.hasBaseline && status.baseline == "warming";
}

const std::string& shownMessage(const Status& status, Language language) {
    return language == Language::En && !status.messageEn.empty() ? status.messageEn : status.message;
}

Tool toolState(const Status& status) {
    const std::string& grab = status.autoGrab;
    // Below the safety floor: eyecam-rec won't start it, whatever it held before
    if (grab == "too_old") return Tool::TooOld;
    bool installed = status.hasBuffers;
    switch (status.state) {
        case State::Idle:
        case State::Searching:
        case State::Recording:
        case State::Calibrating: installed = true; break;
        default: break;
    }
    // auto_grab names what eyecam-rec found; without it (an older eyecam-rec) only the buffers say so
    if (!grab.empty() && grab != "missing" && grab != "no_cap" && grab.rfind("unsafe", 0) != 0) installed = true;
    if (!installed) return Tool::Missing;
    return status.grabOutdated ? Tool::Outdated : Tool::Current;
}

bool toolInstalled(const Status& status) {
    const Tool tool = toolState(status);
    return tool == Tool::Outdated || tool == Tool::Current;
}

ToolNotice toolNotice(const Status& status, PasswordState password) {
    if (setupStep(status, password) != SetupStep::Done) return ToolNotice::None;
    switch (toolState(status)) {
        case Tool::Outdated: return ToolNotice::Outdated;
        case Tool::TooOld: return ToolNotice::TooOld;
        default: return ToolNotice::None;
    }
}

bool setupComplete(const Status& status) {
    if (status.hasSetupDone) return status.setupDone;
    // An eyecam-rec before setup_done: any baseline, or a wear calibration saved once, means it is in use already
    return (status.calibState & (kCalibWearBit | kCalibAutoBit)) != 0 || status.calibSaved;
}

SetupStep setupStep(const Status& status, PasswordState password) {
    const Tool tool = toolState(status);
    // Set up needs the tool there (without it the cameras stop at the next SteamVR start); an outdated or too old one
    // is asked for on the usual page instead. An eyecam-rec without auto_grab can't tell (eyecam-grab was always run
    // by hand), so there being set up is enough
    if (setupComplete(status) && (tool != Tool::Missing || status.autoGrab.empty())) return SetupStep::Done;
    // The checklist's (2) is met only by a current tool
    if (tool == Tool::Current) return SetupStep::Learn;
    return password == PasswordState::NotSet ? SetupStep::Password : SetupStep::Tool;
}

std::vector<std::string> calibChips(const Status& status, Calib calib) {
    const bool user = calib == Calib::User;
    const size_t count = user ? std::size(kCalibUserSteps) : std::size(kCalibWearSteps);
    // The count with the countdown before them; another protocol gets no chips
    if (status.stepCount != static_cast<int>(count) + 1) return {};
    const char* const* steps = user ? kCalibUserSteps : kCalibWearSteps;
    return std::vector<std::string>(steps, steps + count);
}

Calib calibOf(Run run) {
    return run == Run::CalibUser ? Calib::User : Calib::Wear;
}

PageScreen pageScreen(const View& view) {
    const Status& s = view.status;
    if (s.state == State::Calibrating) return PageScreen::Calibrating;
    if (errorShown(view, false)) return PageScreen::Error;
    if (view.flow.calibResult() != CalibResult::None) return PageScreen::Result;
    return PageScreen::Page;
}

SetupScreen setupScreen(const View& view) {
    const Status& s = view.status;
    if (view.flow.result() == SetupResult::Fail) return SetupScreen::Fail;
    if (view.flow.result() == SetupResult::Done) return SetupScreen::Done;
    switch (setupStep(s, view.password)) {
        case SetupStep::Password: return SetupScreen::Pass;
        case SetupStep::Tool: return SetupScreen::Check;
        case SetupStep::Learn:
            if (s.state == State::Calibrating) return SetupScreen::Learn;
            if (errorShown(view, false)) return SetupScreen::Error;
            return SetupScreen::Wait;
        case SetupStep::Done: break;
    }
    return SetupScreen::Camera;
}

void SetupFlow::follow(const Status& status, double now) {
    const bool complete = setupComplete(status);
    run_ = followRun(run_, status);
    if (status.state == State::Calibrating) {
        if (!calibrating_) {
            // A new calibration: the setup's if the setup wasn't complete before it (or, joined midway, now)
            calibrating_ = true;
            setupCalib_ = !(seen_ ? completeBefore_ : complete);
            result_ = SetupResult::None;
            calibResult_ = CalibResult::None;
            reachedEnd_ = false;
            stopSent_ = false;
        }
        if (status.stepCount > 0 && status.stepIndex >= status.stepCount - 1) reachedEnd_ = true;
    } else {
        if (calibrating_ && !setupCalib_ && status.state == State::Idle && reachedEnd_ && !stopSent_) {
            // One from the usual page ended well (stopped, it ends idle too, but not at its last step or not by
            // the panel's stop)
            calibResult_ = run_ == Run::CalibUser                ? CalibResult::User
                           : status.lastCalibWiden == "default" ? CalibResult::Default
                                                                 : CalibResult::Measured;
        }
        if (calibrating_ && setupCalib_ && status.state == State::Idle && complete) {
            // It ended well (eyecam-rec says setup_done after it): widening fell back to the standard values, or it
            // is done (measured, or an eyecam-rec that doesn't say)
            if (status.lastCalibWiden == "default") {
                result_ = SetupResult::Fail;
            } else {
                result_ = SetupResult::Done;
                doneAt_ = now;
            }
        }
        calibrating_ = false;
        setupCalib_ = false;
        completeBefore_ = complete;
    }
    // Not set up (any more: calib.json removed, or the tool): nothing of an earlier calibration is shown
    if (setupStep(status, PasswordState::Unknown) != SetupStep::Done) {
        result_ = SetupResult::None;
        calibResult_ = CalibResult::None;
        doneAt_ = -1e9;
    }
    // The page's result belongs to the idle right after its run (a recording, an error, a restart: gone)
    if (status.state != State::Idle) calibResult_ = CalibResult::None;
    // A dismissed error is that one error: once eyecam-rec has left it, the next one shows, the same words or not
    if (status.state != State::Error) dismissed_.clear();
    seen_ = true;
}

void SetupFlow::proceed() {
    result_ = SetupResult::None;
    calibResult_ = CalibResult::None;
}

void SetupFlow::closed() {
    if (result_ == SetupResult::Done) result_ = SetupResult::None;
    if (calibResult_ != CalibResult::Default) calibResult_ = CalibResult::None;
}

void SetupFlow::stopSent() {
    if (calibrating_) stopSent_ = true;
}

namespace {

/**
 * Which error this is: its run and its message (both languages).
 * @param run the run that failed
 * @param status the status
 * @return the key
 */
std::string errorKey(Run run, const Status& status) {
    return std::to_string(static_cast<int>(run)) + "\n" + status.message + "\n" + status.messageEn;
}

}  // namespace

void SetupFlow::dismissError(Run run, const Status& status) {
    if (status.state == State::Error) dismissed_ = errorKey(run, status);
}

bool SetupFlow::errorDismissed(Run run, const Status& status) const {
    return status.state == State::Error && !dismissed_.empty() && dismissed_ == errorKey(run, status);
}

bool errorShown(const View& view, bool recording) {
    const bool run = recording ? view.lastRun == Run::Recording : isCalib(view.lastRun);
    return view.status.state == State::Error && run && !view.flow.errorDismissed(view.lastRun, view.status);
}

bool SetupFlow::readyNotice(double now) const {
    return now >= doneAt_ && now < doneAt_ + kReadyNoticeSec;
}

std::string sensitivityCommand(double value) {
    if (!std::isfinite(value)) return std::string();
    char text[64];
    std::snprintf(text, sizeof(text), "set widen_sensitivity %.2f", std::clamp(value, 0.0, 1.0));
    return text;
}

void SensitivitySender::released(double value) {
    pending_ = true;
    value_ = value;
}

void SensitivitySender::dragged(double value, double now) {
    if (sent_ && (now < lastAt_ + kSensitivitySendSec || std::fabs(value - lastValue_) < 0.005)) return;
    pending_ = true;
    value_ = value;
}

bool SensitivitySender::next(bool busy, double now, std::string& command) {
    if (!pending_ || busy) return false;
    pending_ = false;
    command = sensitivityCommand(value_);
    if (command.empty()) return false;
    sent_ = true;
    lastValue_ = value_;
    lastAt_ = now;
    return true;
}

bool isSensitivityCommand(const std::string& command) {
    return command.rfind("set widen_sensitivity", 0) == 0;
}

bool StartConfirm::open(State state) {
    if (state != State::Idle && state != State::Error) return false;
    open_ = true;
    openedIn_ = state;
    return true;
}

bool StartConfirm::sync(State state, bool tabShown) {
    if (!open_ || (tabShown && state == openedIn_)) return false;
    open_ = false;
    return true;
}

std::string StartConfirm::choose(StartChoice choice) {
    open_ = false;
    return startCommand(choice);
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
    // (its reason is drawn: valid UTF-8 only)
    std::string text = validUtf8(line);
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
           view.reply.error + "|" + s.stateText + "|" + s.message + "|" + s.messageEn + "|" + std::to_string(s.locked) + "|" +
           rounded(s.fpsL, 0.1) + "|" + rounded(s.fpsR, 0.1) + "|" + std::to_string(s.stepIndex) + "|" +
           std::to_string(s.stepCount) + "|" + s.stepLabel + "|" +
           (std::isfinite(s.stepRemainingS) ? std::to_string(static_cast<long>(std::ceil(s.stepRemainingS - 1e-9)))
                                            : std::string("-")) +
           "|" + rounded(progress, 0.002) + "|" + rounded(s.elapsedS, 1.0) + "|" + rounded(s.totalS, 1.0) + "|" +
           s.protocol + "|" + std::to_string(s.calibState) + std::to_string(s.recalibSuggested) +
           std::to_string(s.live) + "|" + std::to_string(static_cast<int>(view.lastRun)) + "|" +
           std::to_string(s.hasBaseline) + s.baseline + "|" +
           (std::isfinite(s.warmupRemainingS)
                ? std::to_string(static_cast<long>(std::ceil(std::max(0.0, s.warmupRemainingS) - 1e-9)))
                : std::string("-")) +
           "|" + std::to_string(s.hasCalibSaved) + std::to_string(s.calibSaved) + "|" +
           rounded(s.widenSensitivity, 0.01) + "|" + std::to_string(s.hasBuffers) + std::to_string(s.hasSetupDone) +
           std::to_string(s.setupDone) + s.lastCalibWiden + "|" + s.autoGrab + std::to_string(s.grabOutdated) + "|" +
           std::to_string(static_cast<int>(view.password)) + std::to_string(static_cast<int>(view.flow.result())) +
           std::to_string(static_cast<int>(view.flow.calibResult())) +
           std::to_string(view.readyNotice) + "|" + view.spawnError;
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
                finish(parseReply(received_.substr(0, utf8Prefix(received_, 80)), command_));
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

bool needsManualGrab(const Status& s) {
    return !toolInstalled(s);
}

}  // namespace eyecam
