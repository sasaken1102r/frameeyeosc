// Implementation of the OpenVR connection and overlay.
#include "vr_overlay.h"

#include "gaze_fit.h"
#include "openvr.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

constexpr const char* kDashboardKey = "sasaken.frameeyeosc-panel";
constexpr const char* kDashboardName = "Eye";
// The panel image is shown 2.8 m wide (about the same px-to-meter ratio as v2's 1024px / 2.4m;
// height works out to roughly 1.52m)
constexpr float kDashboardWidthM = 2.8f;
// On shutdown, how long to wait after clearing the overlay before VR_Shutdown (about 36 frames at 90Hz)
constexpr int kShutdownWaitMs = 400;
// Scrolling the panel (the version history, the Advanced tab): px per unit of the scroll events' ydelta. Not measured on the Frame
// yet; the first few events are logged to tune them
constexpr double kSmoothScrollPx = 120.0;   ///< VREvent_ScrollSmooth (a continuous delta)
constexpr double kDiscreteScrollPx = 80.0;  ///< VREvent_ScrollDiscrete (one notch)
constexpr int kScrollLogCount = 12;
// The eye fit's target: a plain overlay fixed to the headset, like the debug dots and the eye capture's light (no sort
// order, no dashboard flags: with those, the dashboard hid the dots even at 1 m), so it shows over the open dashboard
constexpr const char* kTargetKey = "sasaken.frameeyeosc-panel.target";
constexpr const char* kTargetName = "Eye target";
// 0.9 m ahead (gaze_fit::kTargetDistanceM), nearer than the dashboard (about 1.35 m), and 0.15 m wide per meter
// ahead: about 8.6 degrees, as when it was 0.3 m wide 2 m ahead
constexpr double kTargetDistanceM = gaze_fit::kTargetDistanceM;
constexpr float kTargetWidthM = static_cast<float>(0.15 * kTargetDistanceM);
// The debug gaze dots: small plain overlays of their own, like frame-perf-overlay's panel (no sort order, no
// dashboard flags: with those, the dashboard hid them even at 1 m); their width and distance: gaze_dots.h
constexpr const char* kDotKeys[2] = {"sasaken.frameeyeosc-panel.dot0", "sasaken.frameeyeosc-panel.dot1"};
// The eye capture's full-view overlay: a plain overlay like the dots (no sort order, no flags), fixed to the headset
// 0.9 m ahead, closer than the dashboard (about 1.35 m) so it shows over that too, and 4 m wide (square): about 131
// degrees across and as much up and down, more than the headset shows
constexpr const char* kFillKey = "sasaken.frameeyeosc-panel.eyecam-fill";
constexpr const char* kFillName = "Eye capture light";
constexpr double kFillDistanceM = 0.9;
constexpr float kFillWidthM = 4.0f;
// IPDs outside this range (m) are taken as a failed read
constexpr double kIpdMin = 0.045;
constexpr double kIpdMax = 0.085;

/**
 * The transform that puts an overlay `distance` ahead of the headset, turned `yawDeg` right and `pitchDeg` up,
 * facing the eyes. OpenVR's head space has +X right, +Y up and -Z ahead.
 * @param yawDeg degrees to the right
 * @param pitchDeg degrees up
 * @param distance meters
 * @return the transform
 */
vr::HmdMatrix34_t headRelativeTransform(double yawDeg, double pitchDeg, double distance) {
    // Rotate about +Y by -yaw (to the right), then about +X by pitch (up)
    const double yaw = -yawDeg * M_PI / 180.0;
    const double pitch = pitchDeg * M_PI / 180.0;
    const double cy = std::cos(yaw);
    const double sy = std::sin(yaw);
    const double cp = std::cos(pitch);
    const double sp = std::sin(pitch);
    const double rotation[3][3] = {{cy, sy * sp, sy * cp}, {0.0, cp, -sp}, {-sy, cy * sp, cy * cp}};
    vr::HmdMatrix34_t m {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) m.m[row][col] = static_cast<float>(rotation[row][col]);
        // The overlay sits at rotation * (0, 0, -distance)
        m.m[row][3] = static_cast<float>(-distance * rotation[row][2]);
    }
    return m;
}

/**
 * The same turn as headRelativeTransform, at a given position instead of straight along it.
 * @param yawDeg degrees to the right
 * @param pitchDeg degrees up
 * @param x position right (m)
 * @param y position up (m)
 * @param z position back (m)
 * @return the transform
 */
vr::HmdMatrix34_t poseTransform(double yawDeg, double pitchDeg, double x, double y, double z) {
    vr::HmdMatrix34_t m = headRelativeTransform(yawDeg, pitchDeg, 0.0);
    m.m[0][3] = static_cast<float>(x);
    m.m[1][3] = static_cast<float>(y);
    m.m[2][3] = static_cast<float>(z);
    return m;
}

/**
 * Return the overlay error name.
 * @param error the error
 * @return the name (e.g. VROverlayError_None)
 */
const char* overlayErrorName(vr::EVROverlayError error) {
    return vr::VROverlay()->GetOverlayErrorNameFromEnum(error);
}

/**
 * Log an overlay error to stderr unless it's success.
 * @param what what was being done
 * @param error the error
 * @return true on success
 */
bool checkOverlay(const char* what, vr::EVROverlayError error) {
    if (error == vr::VROverlayError_None) return true;
    std::fprintf(stderr, "[VR] %s failed: %s\n", what, overlayErrorName(error));
    return false;
}

/**
 * Log one shutdown step's result, whether it succeeded or failed (to verify order and outcome later).
 * @param what what was done
 * @param error the return value
 */
void logShutdownStep(const char* what, vr::EVROverlayError error) {
    std::fprintf(stderr, "[VR] shutdown: %s -> %s\n", what, overlayErrorName(error));
}

}  // namespace

VrOverlay::VrOverlay() = default;

VrOverlay::~VrOverlay() {
    shutdown();
}

int VrOverlay::findVrserverPid() {
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return -1;
    int found = -1;
    while (const dirent* entry = ::readdir(proc)) {
        const char* name = entry->d_name;
        if (name[0] < '0' || name[0] > '9') continue;
        const std::string commPath = std::string("/proc/") + name + "/comm";
        const int fd = ::open(commPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char comm[64] = {};
        const ssize_t n = ::read(fd, comm, sizeof(comm) - 1);
        ::close(fd);
        if (n > 0 && std::strncmp(comm, "vrserver\n", 9) == 0) {
            found = std::atoi(name);
            break;
        }
    }
    ::closedir(proc);
    return found;
}

VrOverlay::ConnectResult VrOverlay::connect(int width, int height, std::string& message) {
    if (connected_) return ConnectResult::Ok;

    // 1) Check with a Background-type init whether SteamVR is running (don't launch it if not)
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return error == vr::VRInitError_Init_NoServerForBackgroundApp ? ConnectResult::NotRunning
                                                                        : ConnectResult::Error;
    }
    vr::VR_Shutdown();

    // 2) Reconnect as an overlay app
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return ConnectResult::Error;
    }
    connected_ = true;

    // 3) Vulkan (built with the extensions OpenVR requires, on the HMD's GPU)
    std::string vkMessage;
    if (!vulkan_.init(vkMessage)) {
        message = "Vulkan setup failed: " + vkMessage;
        shutdown();
        return ConnectResult::Error;
    }

    // 4) Dashboard panel and thumbnail
    vr::IVROverlay* overlay = vr::VROverlay();
    vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumbnail = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError createError =
        overlay->CreateDashboardOverlay(kDashboardKey, kDashboardName, &main, &thumbnail);
    std::fprintf(stderr, "[VR] CreateDashboardOverlay(%s) -> %s\n", kDashboardKey, overlayErrorName(createError));
    if (createError != vr::VROverlayError_None) {
        message = std::string("CreateDashboardOverlay: ") + overlayErrorName(createError);
        shutdown();
        return ConnectResult::Error;
    }
    dashboardHandle_ = main;
    thumbnailHandle_ = thumbnail;
    panelHeight_ = height;

    checkOverlay("SetOverlayWidthInMeters", overlay->SetOverlayWidthInMeters(main, kDashboardWidthM));
    checkOverlay("SetOverlayInputMethod", overlay->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse));
    // Align mouse coordinates with the image's px
    const vr::HmdVector2_t scale = {{static_cast<float>(width), static_cast<float>(height)}};
    checkOverlay("SetOverlayMouseScale", overlay->SetOverlayMouseScale(main, &scale));
    // Show "close" when hovering the dashboard's icon bar. Pressing it delivers VREvent_OverlayClosed
    const vr::EVROverlayError closeError =
        overlay->SetOverlayFlag(main, vr::VROverlayFlags_EnableControlBarClose, true);
    bool closeEnabled = false;
    const vr::EVROverlayError readError =
        overlay->GetOverlayFlag(main, vr::VROverlayFlags_EnableControlBarClose, &closeEnabled);
    std::fprintf(stderr, "[VR] SetOverlayFlag(EnableControlBarClose) -> %s (read back: %s, %s)\n",
                 overlayErrorName(closeError), overlayErrorName(readError), closeEnabled ? "true" : "false");

    if (!panelTexture_.create(vulkan_, width, height, vkMessage)) {
        message = "Failed to create the panel texture: " + vkMessage;
        shutdown();
        return ConnectResult::Error;
    }

    vrserverPid_ = findVrserverPid();
    lastPanelError_.clear();
    return ConnectResult::Ok;
}

void VrOverlay::setPanelScroll(bool on) {
    if (!connected_ || dashboardHandle_ == 0 || on == panelScroll_) return;
    panelScroll_ = on;
    const vr::EVROverlayError error =
        vr::VROverlay()->SetOverlayFlag(dashboardHandle_, vr::VROverlayFlags_SendVRSmoothScrollEvents, on);
    std::fprintf(stderr, "[VR] SetOverlayFlag(SendVRSmoothScrollEvents, %s) -> %s\n", on ? "true" : "false",
                 overlayErrorName(error));
}

void VrOverlay::shutdown() {
    if (!connected_) return;
    vr::IVROverlay* overlay = vr::VROverlay();
    std::fprintf(stderr, "[VR] starting shutdown\n");

    // 1) Clear the texture (so the compositor stops referencing our image)
    if (dashboardHandle_ != 0) {
        logShutdownStep("ClearOverlayTexture(panel)", overlay->ClearOverlayTexture(dashboardHandle_));
        logShutdownStep("ClearOverlayTexture(thumbnail)", overlay->ClearOverlayTexture(thumbnailHandle_));
    }
    if (targetHandle_ != 0) {
        logShutdownStep("HideOverlay(target)", overlay->HideOverlay(targetHandle_));
        logShutdownStep("ClearOverlayTexture(target)", overlay->ClearOverlayTexture(targetHandle_));
    }
    for (uint64_t handle : dotHandles_) {
        if (handle == 0) continue;
        logShutdownStep("HideOverlay(dot)", overlay->HideOverlay(handle));
        logShutdownStep("ClearOverlayTexture(dot)", overlay->ClearOverlayTexture(handle));
    }
    if (fillHandle_ != 0) {
        logShutdownStep("HideOverlay(fill)", overlay->HideOverlay(fillHandle_));
        logShutdownStep("ClearOverlayTexture(fill)", overlay->ClearOverlayTexture(fillHandle_));
    }
    // 2) Destroy the overlays (the thumbnail goes away along with the panel)
    if (dashboardHandle_ != 0) logShutdownStep("DestroyOverlay(panel)", overlay->DestroyOverlay(dashboardHandle_));
    if (targetHandle_ != 0) logShutdownStep("DestroyOverlay(target)", overlay->DestroyOverlay(targetHandle_));
    for (uint64_t& handle : dotHandles_) {
        if (handle != 0) logShutdownStep("DestroyOverlay(dot)", overlay->DestroyOverlay(handle));
        handle = 0;
    }
    dotShown_[0] = dotShown_[1] = false;
    if (fillHandle_ != 0) logShutdownStep("DestroyOverlay(fill)", overlay->DestroyOverlay(fillHandle_));
    fillHandle_ = 0;
    fillShown_ = false;
    dashboardHandle_ = 0;
    thumbnailHandle_ = 0;
    targetHandle_ = 0;
    targetShown_ = false;
    targetPlaced_ = false;

    // 3) Wait a few compositor frames for it to drop the cleared texture
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownWaitMs));
    std::fprintf(stderr, "[VR] shutdown: waited %dms\n", kShutdownWaitMs);

    // 4) Close OpenVR (OpenVR requires that Vulkan images are destroyed only after this)
    vr::VR_Shutdown();
    connected_ = false;
    std::fprintf(stderr, "[VR] shutdown: VR_Shutdown done\n");

    // 5) Destroy the Vulkan images and device
    for (OverlayTexture& texture : dotTextures_) texture.destroy();
    fillTexture_.destroy();
    targetTexture_.destroy();
    thumbnailTexture_.destroy();
    panelTexture_.destroy();
    vulkan_.destroy();
    std::fprintf(stderr, "[VR] shutdown: Vulkan cleaned up\n");
}

VrEvents VrOverlay::pollEvents() {
    VrEvents result;
    if (!connected_) return result;
    vr::VREvent_t event {};
    while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
        if (event.eventType == vr::VREvent_Quit) result.quit = true;
    }
    if (dashboardHandle_ != 0) {
        while (vr::VROverlay()->PollNextOverlayEvent(dashboardHandle_, &event, sizeof(event))) {
            // Mouse coordinates have their origin at the bottom-left, so flip to put it at the top
            const double x = event.data.mouse.x;
            const double y = panelHeight_ - event.data.mouse.y;
            switch (event.eventType) {
                case vr::VREvent_MouseMove: result.pointer.push_back({PointerInput::Type::Move, x, y}); break;
                case vr::VREvent_MouseButtonDown:
                    if (event.data.mouse.button == vr::VRMouseButton_Left) {
                        result.pointer.push_back({PointerInput::Type::Down, x, y});
                    }
                    break;
                case vr::VREvent_MouseButtonUp:
                    if (event.data.mouse.button == vr::VRMouseButton_Left) {
                        result.pointer.push_back({PointerInput::Type::Up, x, y});
                    }
                    break;
                case vr::VREvent_FocusLeave: result.pointer.push_back({PointerInput::Type::Leave, 0, 0}); break;
                // The thumbstick or touchpad while scroll events are on (setPanelScroll). A positive ydelta is
                // scrolling up, like a mouse wheel turned away
                case vr::VREvent_ScrollSmooth:
                case vr::VREvent_ScrollDiscrete: {
                    const bool smooth = event.eventType == vr::VREvent_ScrollSmooth;
                    const double dy = -event.data.scroll.ydelta * (smooth ? kSmoothScrollPx : kDiscreteScrollPx);
                    if (scrollLogs_ < kScrollLogCount) {
                        ++scrollLogs_;
                        std::fprintf(stderr, "[VR] scroll %s: ydelta %.3f, viewportscale %.3f -> %.1f px\n",
                                     smooth ? "smooth" : "discrete", event.data.scroll.ydelta,
                                     event.data.scroll.viewportscale, dy);
                    }
                    if (dy != 0.0) result.pointer.push_back({PointerInput::Type::Scroll, 0, dy});
                    break;
                }
                // The dashboard icon bar's "close" (VROverlayFlags_EnableControlBarClose).
                // This is distinct from SteamVR's own shutdown (VRSystem's VREvent_Quit)
                case vr::VREvent_OverlayClosed:
                    std::fprintf(stderr, "[VR] dashboard \"close\" was pressed\n");
                    result.closeRequested = true;
                    break;
                case vr::VREvent_OverlayShown: std::fprintf(stderr, "[VR] panel shown\n"); break;
                case vr::VREvent_OverlayHidden: std::fprintf(stderr, "[VR] panel hidden\n"); break;
                default: break;
            }
        }
    }
    if (result.quit) {
        std::fprintf(stderr, "[VR] received shutdown notice from SteamVR\n");
        vr::VRSystem()->AcknowledgeQuit_Exiting();
    }
    return result;
}

bool VrOverlay::steamVrAlive() const {
    if (vrserverPid_ <= 0) return true;  // treat as alive if it can't be checked
    return ::kill(vrserverPid_, 0) == 0 || errno == EPERM;
}

bool VrOverlay::panelVisible() const {
    return connected_ && dashboardHandle_ != 0 && vr::VROverlay()->IsOverlayVisible(dashboardHandle_);
}

void VrOverlay::showPanel() {
    if (!connected_ || dashboardHandle_ == 0) return;
    // No return value; whether it actually opened shows up later via IsOverlayVisible / VREvent_OverlayShown
    vr::VROverlay()->ShowDashboard(kDashboardKey);
    std::fprintf(stderr, "[VR] called ShowDashboard(%s)\n", kDashboardKey);
}

bool VrOverlay::dashboardVisible() const {
    return connected_ && vr::VROverlay()->IsDashboardVisible();
}

double VrOverlay::userIpdMeters() const {
    if (!connected_) return gaze_fit::kDefaultIpdM;
    vr::ETrackedPropertyError error = vr::TrackedProp_Success;
    const float ipd = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                                    vr::Prop_UserIpdMeters_Float, &error);
    if (error != vr::TrackedProp_Success || !(ipd >= kIpdMin && ipd <= kIpdMax)) {
        std::fprintf(stderr, "[VR] no usable IPD from SteamVR (%s, %.4f); using %.3f m\n",
                     vr::VRSystem()->GetPropErrorNameFromEnum(error), ipd, gaze_fit::kDefaultIpdM);
        return gaze_fit::kDefaultIpdM;
    }
    return ipd;
}

bool VrOverlay::showTarget(double yawDeg, double pitchDeg, const uint8_t* rgba, int size) {
    if (!connected_ || targetFailed_) return false;
    vr::IVROverlay* overlay = vr::VROverlay();
    std::string message;
    if (targetHandle_ == 0) {
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        const vr::EVROverlayError error = overlay->CreateOverlay(kTargetKey, kTargetName, &handle);
        std::fprintf(stderr, "[VR] CreateOverlay(%s) -> %s\n", kTargetKey, overlayErrorName(error));
        if (error != vr::VROverlayError_None) {
            targetFailed_ = true;
            return false;
        }
        targetHandle_ = handle;
        checkOverlay("SetOverlayWidthInMeters(target)", overlay->SetOverlayWidthInMeters(handle, kTargetWidthM));
        std::fprintf(stderr, "[VR] target %.2f m ahead, %.3f m wide (plain overlay, no sort order or flags)\n",
                     kTargetDistanceM, kTargetWidthM);
        if (!targetTexture_.create(vulkan_, size, size, message)) {
            std::fprintf(stderr, "[Vulkan] can't create the target texture: %s\n", message.c_str());
            targetFailed_ = true;
            return false;
        }
    }
    if (rgba != nullptr && !targetTexture_.update(targetHandle_, rgba, message)) {
        std::fprintf(stderr, "[VR] can't send the target: %s\n", message.c_str());
        return false;
    }
    if (!targetPlaced_ || targetYaw_ != yawDeg || targetPitch_ != pitchDeg) {
        const vr::HmdMatrix34_t transform = headRelativeTransform(yawDeg, pitchDeg, kTargetDistanceM);
        const vr::EVROverlayError error =
            overlay->SetOverlayTransformTrackedDeviceRelative(targetHandle_, vr::k_unTrackedDeviceIndex_Hmd, &transform);
        if (!checkOverlay("SetOverlayTransformTrackedDeviceRelative(target)", error)) return false;
        targetYaw_ = yawDeg;
        targetPitch_ = pitchDeg;
        targetPlaced_ = true;
    }
    if (!targetShown_) {
        if (!checkOverlay("ShowOverlay(target)", overlay->ShowOverlay(targetHandle_))) return false;
        targetShown_ = true;
    }
    return true;
}

bool VrOverlay::waitFrameSync(uint32_t timeoutMs) {
    if (!connected_ || frameSyncFailed_) return false;
    const vr::EVROverlayError error = vr::VROverlay()->WaitFrameSync(timeoutMs);
    // A timeout is normal while the compositor skips frames; the loop just goes on
    if (error == vr::VROverlayError_None || error == vr::VROverlayError_TimedOut) return true;
    std::fprintf(stderr, "[VR] WaitFrameSync -> %s; pacing the target with a timer instead\n", overlayErrorName(error));
    frameSyncFailed_ = true;
    return false;
}

void VrOverlay::hideTarget() {
    if (!connected_ || targetHandle_ == 0 || !targetShown_) return;
    checkOverlay("HideOverlay(target)", vr::VROverlay()->HideOverlay(targetHandle_));
    targetShown_ = false;
}

bool VrOverlay::showDot(int index, double x, double y, double z, double yawDeg, double pitchDeg, const uint8_t* rgba,
                        int size, bool newImage, double widthM) {
    if (!connected_ || dotFailed_ || index < 0 || index > 1) return false;
    vr::IVROverlay* overlay = vr::VROverlay();
    std::string message;
    uint64_t& handle = dotHandles_[index];
    if (handle == 0) {
        vr::VROverlayHandle_t created = vr::k_ulOverlayHandleInvalid;
        const vr::EVROverlayError error = overlay->CreateOverlay(kDotKeys[index], "Eye gaze dot", &created);
        std::fprintf(stderr, "[VR] CreateOverlay(%s) -> %s\n", kDotKeys[index], overlayErrorName(error));
        if (error != vr::VROverlayError_None || !dotTextures_[index].create(vulkan_, size, size, message)) {
            if (!message.empty()) std::fprintf(stderr, "[Vulkan] can't create a dot texture: %s\n", message.c_str());
            if (error == vr::VROverlayError_None) overlay->DestroyOverlay(created);
            dotFailed_ = true;
            return false;
        }
        handle = created;
        dotWidth_[index] = 0.0;
        newImage = true;
    }
    if (newImage && !dotTextures_[index].update(handle, rgba, message)) {
        std::fprintf(stderr, "[VR] can't send a dot: %s\n", message.c_str());
        return false;
    }
    if (widthM != dotWidth_[index]) {
        if (!checkOverlay("SetOverlayWidthInMeters(dot)",
                          overlay->SetOverlayWidthInMeters(handle, static_cast<float>(widthM)))) {
            return false;
        }
        dotWidth_[index] = widthM;
    }
    const vr::HmdMatrix34_t transform = poseTransform(yawDeg, pitchDeg, x, y, z);
    if (overlay->SetOverlayTransformTrackedDeviceRelative(handle, vr::k_unTrackedDeviceIndex_Hmd, &transform) !=
        vr::VROverlayError_None) {
        return false;
    }
    if (!dotShown_[index]) {
        if (!checkOverlay("ShowOverlay(dot)", overlay->ShowOverlay(handle))) return false;
        dotShown_[index] = true;
    }
    return true;
}

void VrOverlay::hideDot(int index) {
    if (!connected_ || index < 0 || index > 1 || dotHandles_[index] == 0 || !dotShown_[index]) return;
    checkOverlay("HideOverlay(dot)", vr::VROverlay()->HideOverlay(dotHandles_[index]));
    dotShown_[index] = false;
}

bool VrOverlay::showFill(const uint8_t* rgba, int size, double alpha) {
    if (!connected_ || fillFailed_) return false;
    vr::IVROverlay* overlay = vr::VROverlay();
    std::string message;
    if (fillHandle_ == 0) {
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        const vr::EVROverlayError error = overlay->CreateOverlay(kFillKey, kFillName, &handle);
        std::fprintf(stderr, "[VR] CreateOverlay(%s) -> %s\n", kFillKey, overlayErrorName(error));
        if (error != vr::VROverlayError_None || !fillTexture_.create(vulkan_, size, size, message)) {
            if (!message.empty()) std::fprintf(stderr, "[Vulkan] can't create the fill texture: %s\n", message.c_str());
            if (error == vr::VROverlayError_None) overlay->DestroyOverlay(handle);
            fillFailed_ = true;
            return false;
        }
        fillHandle_ = handle;
        checkOverlay("SetOverlayWidthInMeters(fill)", overlay->SetOverlayWidthInMeters(handle, kFillWidthM));
        // Straight ahead, facing the eyes, moving with the head
        const vr::HmdMatrix34_t transform = headRelativeTransform(0.0, 0.0, kFillDistanceM);
        checkOverlay("SetOverlayTransformTrackedDeviceRelative(fill)",
                     overlay->SetOverlayTransformTrackedDeviceRelative(handle, vr::k_unTrackedDeviceIndex_Hmd,
                                                                       &transform));
        std::fprintf(stderr, "[VR] fill overlay %.1f m ahead, %.1f m wide\n", kFillDistanceM, kFillWidthM);
        fillAlpha_ = -1.0f;
    }
    if (rgba != nullptr && !fillTexture_.update(fillHandle_, rgba, message)) {
        std::fprintf(stderr, "[VR] can't send the fill: %s\n", message.c_str());
        return false;
    }
    // The alpha before ShowOverlay: a fade-in starts from clear, never from the last alpha
    const float a = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    if (a != fillAlpha_) {
        // Every step of a fade (logged only when it fails)
        if (checkOverlay("SetOverlayAlpha(fill)", overlay->SetOverlayAlpha(fillHandle_, a))) {
            fillAlpha_ = a;
        } else {
            fillAlpha_ = -1.0f;
            if (!fillShown_) return false;  // never shown at an alpha that wasn't set
        }
    }
    if (!fillShown_) {
        if (!checkOverlay("ShowOverlay(fill)", overlay->ShowOverlay(fillHandle_))) return false;
        fillShown_ = true;
    }
    return true;
}

void VrOverlay::hideFill() {
    if (!connected_ || fillHandle_ == 0 || !fillShown_) return;
    checkOverlay("HideOverlay(fill)", vr::VROverlay()->HideOverlay(fillHandle_));
    fillShown_ = false;
}

bool VrOverlay::submitThumbnail(const uint8_t* rgba, int size) {
    if (!connected_ || thumbnailHandle_ == 0) return false;
    std::string message;
    if (!thumbnailTexture_.ready() && !thumbnailTexture_.create(vulkan_, size, size, message)) {
        std::fprintf(stderr, "[Vulkan] can't create the thumbnail texture: %s\n", message.c_str());
        return false;
    }
    if (!thumbnailTexture_.update(thumbnailHandle_, rgba, message)) {
        std::fprintf(stderr, "[VR] can't send the thumbnail: %s\n", message.c_str());
        return false;
    }
    return true;
}

bool VrOverlay::submitPanel(const uint8_t* rgba) {
    if (!connected_ || dashboardHandle_ == 0 || !panelTexture_.ready()) return false;
    std::string message;
    const bool ok = panelTexture_.update(dashboardHandle_, rgba, message);
    // Don't spam the same error every time
    if (message != lastPanelError_) {
        if (!ok) std::fprintf(stderr, "[VR] can't send the panel: %s\n", message.c_str());
        lastPanelError_ = message;
    }
    return ok;
}

void VrOverlay::logOverlayState(const char* when) const {
    if (!connected_) return;
    vr::IVROverlay* overlay = vr::VROverlay();
    vr::VROverlayHandle_t found = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError findError = overlay->FindOverlay(kDashboardKey, &found);
    uint32_t w = 0;
    uint32_t h = 0;
    const vr::EVROverlayError sizeError = overlay->GetOverlayTextureSize(dashboardHandle_, &w, &h);
    uint32_t tw = 0;
    uint32_t th = 0;
    const vr::EVROverlayError thumbError = overlay->GetOverlayTextureSize(thumbnailHandle_, &tw, &th);
    std::fprintf(stderr,
                 "[VR] check (%s): FindOverlay(%s) -> %s (same handle: %s) panel texture %ux%u (%s) "
                 "thumbnail texture %ux%u (%s) panel visible: %s dashboard: %s\n",
                 when, kDashboardKey, overlayErrorName(findError), found == dashboardHandle_ ? "yes" : "no", w, h,
                 overlayErrorName(sizeError), tw, th, overlayErrorName(thumbError),
                 overlay->IsOverlayVisible(dashboardHandle_) ? "yes" : "no",
                 overlay->IsDashboardVisible() ? "open" : "closed");
}

int VrOverlay::probe() {
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        std::printf("Can't connect to SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        return 1;
    }
    vr::IVROverlay* overlay = vr::VROverlay();
    int code = 1;
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError findError = overlay ? overlay->FindOverlay(kDashboardKey, &handle)
                                                  : vr::VROverlayError_RequestFailed;
    std::printf("FindOverlay(%s) -> %s\n", kDashboardKey, overlay ? overlayErrorName(findError) : "no IVROverlay");
    if (overlay != nullptr && findError == vr::VROverlayError_None) {
        code = 0;
        char name[128] = {};
        overlay->GetOverlayName(handle, name, sizeof(name), nullptr);
        bool closeFlag = false;
        overlay->GetOverlayFlag(handle, vr::VROverlayFlags_EnableControlBarClose, &closeFlag);
        float widthM = 0.0f;
        overlay->GetOverlayWidthInMeters(handle, &widthM);
        std::printf("  name: %s  width: %.2fm  close button: %s  panel visible: %s  dashboard: %s\n", name, widthM,
                    closeFlag ? "yes" : "no", overlay->IsOverlayVisible(handle) ? "yes" : "no",
                    overlay->IsDashboardVisible() ? "open" : "closed");
        uint32_t w = 0;
        uint32_t h = 0;
        const vr::EVROverlayError sizeError = overlay->GetOverlayTextureSize(handle, &w, &h);
        std::printf("  GetOverlayTextureSize -> %s (%ux%u)\n", overlayErrorName(sizeError), w, h);
        // Don't use GetOverlayImageData: as tested on 2026-09-27, calling it on an overlay holding a
        // Vulkan texture crashed the calling process (this one) with SIGSEGV (SteamVR itself was fine).
        // Confirm the image was set by checking its size instead.
    }
    vr::VR_Shutdown();
    return code;
}

int VrOverlay::switchAway(double seconds) {
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        std::printf("Can't connect to SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        return 1;
    }
    vr::VR_Shutdown();
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) {
        std::printf("Can't connect as an overlay app: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        return 1;
    }
    vr::IVROverlay* overlay = vr::VROverlay();
    constexpr const char* kAwayKey = "sasaken.frameeyeosc-panel.probe-away";
    vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumbnail = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError createError = overlay->CreateDashboardOverlay(kAwayKey, "Probe", &main, &thumbnail);
    std::printf("CreateDashboardOverlay(%s) -> %s\n", kAwayKey, overlayErrorName(createError));
    int code = 1;
    if (createError == vr::VROverlayError_None) {
        vr::VROverlayHandle_t eye = vr::k_ulOverlayHandleInvalid;
        overlay->FindOverlay(kDashboardKey, &eye);
        const auto eyeVisible = [&]() {
            return eye != vr::k_ulOverlayHandleInvalid && overlay->IsOverlayVisible(eye) ? "yes" : "no";
        };
        std::printf("Before switching: Eye panel visible: %s\n", eyeVisible());
        // The dashboard wouldn't switch to an overlay with no image set, so give it an icon PNG
        // (read from a file; doesn't use SetOverlayRaw's shared memory)
        char exe[4096] = {};
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        std::string icon = n > 0 ? std::string(exe, static_cast<size_t>(n)) : std::string();
        icon = icon.substr(0, icon.find_last_of('/')) + "/../contrib/icons/frameeyeosc-panel-256.png";
        char resolved[4096] = {};
        if (::realpath(icon.c_str(), resolved) != nullptr) {
            std::printf("SetOverlayFromFile(%s) -> %s\n", resolved,
                        overlayErrorName(overlay->SetOverlayFromFile(main, resolved)));
        }
        overlay->SetOverlayWidthInMeters(main, 1.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        overlay->ShowDashboard(kAwayKey);
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        std::printf("After switching: Eye panel visible: %s  temporary overlay visible: %s\n", eyeVisible(),
                    overlay->IsOverlayVisible(main) ? "yes" : "no");
        std::printf("DestroyOverlay -> %s\n", overlayErrorName(overlay->DestroyOverlay(main)));
        code = 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    vr::VR_Shutdown();
    return code;
}
