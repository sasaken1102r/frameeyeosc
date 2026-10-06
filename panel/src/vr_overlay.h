// Connection to OpenVR, the dashboard panel (plus its thumbnail), and the eye fit's head-locked target.
#pragma once

#include "vk_texture.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * A pointer action on the dashboard panel (coordinates are px, origin at the image's top-left). For Scroll, y is
 * how far to scroll in px (positive shows what is further down) and x is 0.
 */
struct PointerInput {
    enum class Type { Move, Down, Up, Leave, Scroll };
    Type type;
    double x = 0.0;
    double y = 0.0;
};

/** Result of pollEvents(). */
struct VrEvents {
    bool quit = false;                  ///< SteamVR asked us to quit (VREvent_Quit; SteamVR itself is shutting down)
    bool closeRequested = false;        ///< the dashboard icon's "close" was pressed (VREvent_OverlayClosed)
    std::vector<PointerInput> pointer;  ///< actions on the panel (in arrival order)
};

/**
 * Wraps the connection to OpenVR as an overlay app.
 * Never changes SteamVR settings; only creates and shows a dashboard panel, and during an eye fit a small target
 * overlay fixed to the headset (and the debug gaze dots, and the eye capture's full-view light, each its own overlay).
 * Images are sent as Vulkan textures via SetOverlayTexture rather than SetOverlayRaw.
 */
class VrOverlay {
public:
    /** Result of connect(). */
    enum class ConnectResult {
        Ok,          ///< connected
        NotRunning,  ///< SteamVR isn't running (wait and retry)
        Error,       ///< any other failure
    };

    VrOverlay();
    ~VrOverlay();
    VrOverlay(const VrOverlay&) = delete;
    VrOverlay& operator=(const VrOverlay&) = delete;

    /**
     * If SteamVR is running, connect as an overlay app and set up Vulkan and the dashboard panel.
     * Checks first with a Background-type init to avoid accidentally launching SteamVR if it
     * isn't running.
     * @param width panel image width (px)
     * @param height panel image height (px)
     * @param message reason for failure
     * @return the connection result
     */
    ConnectResult connect(int width, int height, std::string& message);

    /**
     * Tear down in a safe order: clear the texture -> destroy the overlay -> wait a few compositor
     * frames -> VR_Shutdown -> destroy the Vulkan images and device. Every API return value is
     * logged.
     */
    void shutdown();

    /**
     * Process pending events. Answers SteamVR's shutdown (VREvent_Quit) with AcknowledgeQuit_Exiting.
     * @return quit request and panel pointer actions
     */
    VrEvents pollEvents();

    /**
     * Whether the vrserver process found at connect time is still alive.
     * @return true if alive (or if it can't be determined)
     */
    bool steamVrAlive() const;

    /**
     * Whether the panel (this app selected on the dashboard) is currently visible.
     * @return true if visible
     */
    bool panelVisible() const;

    /**
     * Open the dashboard and show this app's panel (IVROverlay::ShowDashboard).
     */
    void showPanel();

    /**
     * Ask for the controller's scroll events on the panel (VROverlayFlags_SendVRSmoothScrollEvents), only while
     * something on it scrolls, so the other tabs get the dashboard's usual input. Logged when it changes.
     * @param on whether to get them
     */
    void setPanelScroll(bool on);

    /**
     * Whether the SteamVR dashboard is open (showing any overlay, not only this panel).
     * @return true if open
     */
    bool dashboardVisible() const;

    /**
     * The distance between the eyes that SteamVR uses (Prop_UserIpdMeters_Float of the headset).
     * @return meters, or gaze_fit::kDefaultIpdM when SteamVR doesn't say or the value looks wrong
     */
    double userIpdMeters() const;


    /**
     * Show the eye fit's target: an overlay of its own (not on the dashboard), fixed to the headset
     * gaze_fit::kTargetDistanceM (0.9 m) ahead in the given direction, nearer than the dashboard so it shows over it
     * when that is open. Created the first time it is needed and kept, hidden, until shutdown.
     * @param yawDeg degrees to the right of straight ahead
     * @param pitchDeg degrees up
     * @param rgba a new image, non-premultiplied RGBA; nullptr keeps the last one
     * @param size its edge length (px)
     * @return true if it is shown
     */
    bool showTarget(double yawDeg, double pitchDeg, const uint8_t* rgba, int size);

    /**
     * Wait for the compositor's next frame (IVROverlay::WaitFrameSync), to draw the target once per display frame.
     * @param timeoutMs the longest wait
     * @return false if it can't be used (then the caller sleeps instead); logged once
     */
    bool waitFrameSync(uint32_t timeoutMs);

    /** Hide the target (nothing happens if it was never created). */
    void hideTarget();

    /**
     * Show debug gaze dot `index` (0 or 1): an overlay of its own, fixed to the headset at a pose (head space,
     * m and degrees). Created the first time it is needed. Only the transform changes from frame to frame; the
     * image is sent when `newImage` is true (or the dot is new).
     * @param index 0 or 1
     * @param x right (m)
     * @param y up (m)
     * @param z back (m; ahead is negative)
     * @param yawDeg turned right, facing back toward the eyes
     * @param pitchDeg turned up
     * @param rgba the image, non-premultiplied RGBA
     * @param size its edge length (px)
     * @param newImage the image changed since the last call
     * @param widthM its width (m; set when it changes, to keep its angular size at another distance)
     * @return true if it is shown
     */
    bool showDot(int index, double x, double y, double z, double yawDeg, double pitchDeg, const uint8_t* rgba, int size,
                 bool newImage, double widthM);

    /**
     * Hide a debug gaze dot.
     * @param index 0 or 1
     */
    void hideDot(int index);

    /**
     * Show the eye capture's full-view overlay: an overlay of its own (not on the dashboard, and nothing shared with
     * the panel's), fixed to the headset straight ahead (kFillDistanceM) and wide enough to fill the view
     * (kFillWidthM), white or black during the recorder's bright and dark steps. Created the first time it is
     * needed; destroyed by shutdown().
     * @param rgba a new image, non-premultiplied RGBA; nullptr keeps the last one
     * @param size its edge length (px)
     * @param alpha how opaque (0..1, SetOverlayAlpha; set before it is first shown, so it never flashes up)
     * @return true if it is shown
     */
    bool showFill(const uint8_t* rgba, int size, double alpha);

    /** Hide the full-view overlay (nothing happens if it isn't shown). */
    void hideFill();

    /** @return true while the full-view overlay is shown */
    bool fillShown() const { return fillShown_; }

    /**
     * Send the dashboard thumbnail image (once, right after connecting).
     * @param rgba non-premultiplied RGBA
     * @param size side length in px
     * @return true if it was sent
     */
    bool submitThumbnail(const uint8_t* rgba, int size);

    /**
     * Send the panel image.
     * @param rgba non-premultiplied 8-bit RGBA (sized as given to connect)
     * @return true if it was sent
     */
    bool submitPanel(const uint8_t* rgba);

    /**
     * Diagnostic: look up the overlay again by key and log its texture size and so on.
     * @param when what point this check is at (for logging)
     */
    void logOverlayState(const char* when) const;

    /**
     * Diagnostic (--probe): connect as Background type, look up the running instance's dashboard
     * overlay by key, and print whether it's visible, its flags, and its texture size to stdout.
     * Creates neither an overlay nor Vulkan.
     * @return 0 if found, 1 if SteamVR isn't running or the overlay wasn't found
     */
    static int probe();

    /**
     * Diagnostic (--probe-switch-away): create a temporary empty dashboard overlay and switch to
     * it with ShowDashboard, putting the running instance's Eye panel into the not-visible (closed)
     * state. Removes it again after a few seconds.
     * @param seconds how long to keep it switched away
     * @return 0 on success
     */
    static int switchAway(double seconds);

private:
    bool connected_ = false;
    uint64_t dashboardHandle_ = 0;  ///< vr::VROverlayHandle_t (the dashboard panel)
    uint64_t thumbnailHandle_ = 0;  ///< the dashboard thumbnail
    uint64_t targetHandle_ = 0;     ///< the eye fit's target (0 until first needed)
    bool targetShown_ = false;
    bool frameSyncFailed_ = false;  ///< WaitFrameSync gave an error other than a timeout
    bool targetPlaced_ = false;     ///< its transform was set, to targetYaw_ / targetPitch_
    double targetYaw_ = 0.0;
    double targetPitch_ = 0.0;
    bool targetFailed_ = false;     ///< creating it failed; not tried again
    uint64_t dotHandles_[2] = {0, 0};  ///< the debug gaze dots (0 until first needed)
    bool dotShown_[2] = {false, false};
    double dotWidth_[2] = {0.0, 0.0};  ///< the width last set (m)
    bool dotFailed_ = false;          ///< creating one failed; not tried again
    uint64_t fillHandle_ = 0;         ///< the eye capture's full-view overlay (0 until first needed)
    bool fillShown_ = false;
    bool fillFailed_ = false;         ///< creating it failed; not tried again
    float fillAlpha_ = -1.0f;         ///< the alpha last set on it (-1 = not set yet)
    int panelHeight_ = 0;
    bool panelScroll_ = false;      ///< the panel asked for scroll events
    int scrollLogs_ = 0;            ///< scroll events logged so far (the first few, to tune the speed)
    int vrserverPid_ = -1;
    std::string lastPanelError_;

    VulkanContext vulkan_;
    OverlayTexture panelTexture_;
    OverlayTexture thumbnailTexture_;
    OverlayTexture targetTexture_;
    OverlayTexture dotTextures_[2];
    OverlayTexture fillTexture_;

    /**
     * Scan /proc once to find the vrserver PID.
     * @return the PID found, or -1 if not found
     */
    static int findVrserverPid();
};
