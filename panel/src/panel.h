// The dashboard panel (drawing and hit testing) and its thumbnail. Every color comes from theme.h, so that
// --contrast-report checks what is drawn.
#pragma once

#include "model.h"

#include <cstdint>
#include <string>
#include <vector>

class FontSet;
struct Pen;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

/** What a button does. */
enum class PanelAction {
    None,
    Tab,               ///< switch tab (handled inside the panel; arg = tab)
    SetBool,           ///< key = arg != 0
    Step,              ///< key += arg * step
    HostAuto,          ///< host = "auto"
    FixHost,           ///< host = the IP frameeyeosc sends to now
    PortDefault,       ///< port = null
    SetOutput,         ///< output = outputOfArg(arg) (0 vrchat, 1 etvr, 2 livelink), port = null, then ask about
                       ///< the recommendation
    SetActiveType,     ///< eye_tracking_active = kActiveTypes[arg] (bool, float, off)
    Preset,            ///< gaze smoothing preset arg (0 light, 1 medium, 2 strong)
    NumberOn,          ///< key = its default, or its onNumber if that is off (for numbers where 0 means off)
    NumberOff,         ///< key = 0
    CalibrationReset,  ///< calibration_reset + 1
    ScaleAuto,         ///< lid_scale_left/right = null
    ScaleFixed,        ///< lid_scale_left/right = the scales in use
    PrefixFt,          ///< prefix = "/FT"
    PrefixNone,        ///< prefix = ""
    Language,          ///< language = arg (0 ja, 1 en)
    AutostartOn,
    AutostartOff,
    PromptYes,         ///< apply the recommendation for output arg (the prompt closes itself)
    PromptNo,
    ResetAll,          ///< only returned on the confirming second press
    Quit,              ///< only returned on the confirming second press
    UpdateCheck,       ///< look for a new release now (ignores the 24 h cache)
    UpdateInstall,     ///< ask whether to install the new release (the caller opens the update prompt)
    UpdateConfirm,     ///< install it (the prompt closes itself)
    UpdateCancel,
    UpdateDismiss,     ///< close the "installed" / "failed" message
    FitStart,          ///< eye fit: the whole fit (the caller starts the session)
    FitCenter,         ///< eye fit: the re-wear fit, as auto_recenter says (re-center only when it is off)
    FitStop,           ///< eye fit: stop waiting for the dashboard to close
    SetAutoRecenter,   ///< auto_recenter = kAutoRecenterModes[arg]
    RecordToggle,      ///< start the eye log, or stop it (the caller runs the recorder)
    FitReset,          ///< the fit back to the defaults (fitResetKeys: the gaze fit, lid_fit_*, lid_scale_*)
    FitDetails,        ///< open / close "Fine-tune" (handled inside the panel)
    FitDetailsPage,    ///< show arg (0 gaze, 1 eyelids) under "Fine-tune" (handled inside the panel)
    LidMarks,          ///< open / close the lid marks on the Eyelids tab for fitted eyes (handled inside the panel)
    SetLidWiden,       ///< lid_widen = kLidWidenModes[arg]
    HostEnter,         ///< open the keypad for the target PC (the caller fills in the host now)
    HostKey,           ///< a keypad key: arg = '0'-'9', '.' or host_entry::kBackspace (handled inside the panel)
    HostOk,            ///< use the typed host (the caller checks and writes it)
    HostCancel,        ///< close the keypad (handled inside the panel)
    HistoryOpen,       ///< open the version history on the Advanced tab (the panel opens it; the caller reads the
                       ///< changelog)
    HistoryClose,      ///< back to the Advanced tab (handled inside the panel)
    HistoryRow,        ///< open version row arg, or close it if it is open (handled inside the panel)
    HistoryScroll,     ///< scroll the version history a third of its height, arg -1 up / 1 down (handled inside)
    EyecamStart,       ///< eye capture tab: open the light warning before a start (handled inside the panel)
    EyecamChoose,      ///< a button in the light warning: arg = eyecam::StartChoice (the panel returns only the two
                       ///< starts; the caller sends eyecam::startCommand to eyecam-rec's socket)
    EyecamStop,        ///< eye capture tab: send "stop"
};

/** A button: its action, the config key it changes and an argument. */
struct PanelHit {
    PanelAction action = PanelAction::None;
    const char* key = nullptr;
    int arg = 0;

    /** @return true if it is the same button */
    bool operator==(const PanelHit& other) const;
    /** @return true if it is another button */
    bool operator!=(const PanelHit& other) const { return !(*this == other); }
};

/** The tabs, in the order they are shown. Eyecam (developer) only shows while eyecam-rec runs. */
enum class PanelTab { Basic, Output, Gaze, EyeFit, Lids, Advanced, Eyecam };

/**
 * Draws the panel image and finds the button under the laser pointer.
 */
class EyePanel {
public:
    /** Seconds the quit / reset buttons wait for the confirming second press. */
    static constexpr double kConfirmSec = 3.0;

    /**
     * @param fonts the fonts (must outlive the panel)
     */
    explicit EyePanel(const FontSet& fonts);
    ~EyePanel();
    EyePanel(const EyePanel&) = delete;
    EyePanel& operator=(const EyePanel&) = delete;

    /**
     * Draw the panel from the model and the pointer state. Button positions are rebuilt here.
     * @param model what to show
     */
    void render(const PanelModel& model);

    /**
     * The pointer moved.
     * @param x px from the left
     * @param y px from the top
     * @return true if the button under it changed (redraw needed)
     */
    bool pointerMove(double x, double y);

    /**
     * A press. Tabs switch here. Quit and reset only return on a second press within kConfirmSec.
     * While the recommendation prompt is open, only its buttons work.
     * @param x px from the left
     * @param y px from the top
     * @param now monotonic seconds
     * @return the button for the caller to carry out (None if nothing to do)
     */
    PanelHit pointerDown(double x, double y, double now);

    /**
     * The button was released.
     * @return true if a redraw is needed
     */
    bool pointerUp();

    /**
     * The pointer left the panel.
     * @return true if a redraw is needed
     */
    bool pointerLeave();

    /**
     * Expire the quit / reset confirmation.
     * @param now monotonic seconds
     * @return true if a redraw is needed
     */
    bool tick(double now);

    /**
     * Ask once whether to apply the recommended settings of an output type.
     * @param output kOutputVrchat, kOutputEtvr or kOutputLivelink
     */
    void showPrompt(const std::string& output);

    /**
     * Ask once whether to install a new release.
     * @param version the release ("0.4.1")
     */
    void showUpdatePrompt(const std::string& version);

    /** @return true while the recommendation or update prompt is open */
    bool promptOpen() const { return !promptOutput_.empty() || !updatePromptVersion_.empty(); }

    /**
     * Choose the tab.
     * @param tab the tab
     */
    void setTab(PanelTab tab) { tab_ = tab; }

    /** @return the tab shown (Eyecam falls back to Basic at the next draw once its tab is gone) */
    PanelTab tab() const { return tab_; }

    /**
     * Open or close "Fine-tune" on the Eye fit tab.
     * @param open whether it is open
     */
    void setFitDetails(bool open) { fitDetails_ = open; }

    /**
     * Show the lid marks on the Eyelids tab although the eyes are fitted (they are folded away then).
     * @param open whether they show
     */
    void setLidMarks(bool open) { lidMarksOpen_ = open; }

    /**
     * Which values "Fine-tune" shows.
     * @param page 0 = gaze, 1 = eyelids
     */
    void setFitDetailsPage(int page) { fitDetailsPage_ = page; }

    /**
     * Open the keypad for the target PC's IPv4 address (only its buttons work while it is open).
     * @param text what it starts with (the host set now, or "")
     */
    void openHostEntry(const std::string& text);

    /** Close the keypad. */
    void closeHostEntry();

    /** @return true while the keypad is open */
    bool hostEntryOpen() const { return hostEntryOpen_; }

    /**
     * Open the light warning on the eye capture tab, as its start button does (for --fake-eyecam confirm).
     * @param state the recorder's state (only idle and error open it)
     */
    void openEyecamConfirm(eyecam::State state);

    /**
     * Follow eyecam-rec: the light warning closes once its state leaves the one it was opened in, or the tab is
     * gone or not the one shown. Called every loop (render does it too).
     * @param view the recorder as read
     * @return true if the warning closed (redraw)
     */
    bool syncEyecam(const eyecam::View& view);

    /** Close the light warning (the dashboard closed). */
    void closeEyecamConfirm() { eyecamConfirm_.close(); }

    /** @return true while the light warning shows */
    bool eyecamConfirmOpen() const { return eyecamConfirm_.isOpen(); }

    /**
     * Open the version history on the Advanced tab, with the installed version's row open (the newest one if the
     * changelog doesn't have it) and scrolled to the top. Choosing another tab closes it.
     */
    void openHistory();

    /** Close the version history (the Advanced tab shows its rows again). */
    void closeHistory() { historyOpen_ = false; }

    /** @return true while the version history is open */
    bool historyOpen() const { return historyOpen_; }

    /**
     * For --history-open: open this version's row instead (without scrolling to it).
     * @param version "0.5.0"
     */
    void setHistoryRow(const std::string& version);

    /**
     * For --history-scroll: how far the version history is scrolled (kept within the list when drawn).
     * @param px px from the top of the list
     */
    void setHistoryScroll(double px);

    /** @return true while the panel wants the controller's scroll events (the version history is shown) */
    bool wantsScroll() const { return historyOpen_ && tab_ == PanelTab::Advanced; }

    /**
     * Scroll the version history (the thumbstick or touchpad; ignored while it is not shown or a prompt is open).
     * @param dy px; positive moves the list up (shows what is further down)
     * @return true if it moved (redraw needed)
     */
    bool scroll(double dy);

    /** @return what is typed */
    const std::string& hostEntryText() const { return hostEntryText_; }


    /**
     * Show why the typed host can't be used (cleared by the next key).
     * @param message the message ("" for none)
     */
    void setHostEntryError(const std::string& message) { hostEntryError_ = message; }

    /**
     * The image as un-premultiplied RGBA for OpenVR.
     * @return width() * height() * 4 bytes
     */
    const std::vector<uint8_t>& toRgba();

    /**
     * Save the image as PNG.
     * @param path where to save it
     * @return true if saved
     */
    bool writePng(const std::string& path) const;

    /** For --dump-png: show "press again to quit". */
    void armQuitForPreview();
    /** For --dump-png: show "press again to reset". */
    void armResetForPreview();

    /** @return the image width (px) */
    int width() const;
    /** @return the image height (px) */
    int height() const;

private:
    /** Hit area of one button. */
    struct Button {
        PanelHit hit;
        double x, y, w, h;
        bool usable;
    };

    /** One choice of a segmented control. */
    struct Option {
        std::string label;
        PanelHit hit;
        bool usable = true;
    };

    const FontSet& fonts_;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    std::vector<uint8_t> rgba_;
    std::vector<Button> buttons_;
    PanelHit hover_;
    PanelHit pressed_;
    PanelTab tab_ = PanelTab::Basic;
    bool fitDetails_ = false;  ///< "Fine-tune" is open on the Eye fit tab
    int fitDetailsPage_ = 0;   ///< what "Fine-tune" shows: 0 = gaze, 1 = eyelids
    bool lidMarksOpen_ = false;  ///< the lid marks show on the Eyelids tab although the eyes are fitted
    bool quitArmed_ = false;
    double quitArmedUntil_ = 0.0;
    bool resetArmed_ = false;
    double resetArmedUntil_ = 0.0;
    std::string promptOutput_;  ///< the output type the prompt asks about; empty = no prompt
    std::string updatePromptVersion_;  ///< the release the update prompt asks about; empty = no prompt
    bool hostEntryOpen_ = false;  ///< the keypad for the target PC is open
    std::string hostEntryText_;   ///< what is typed in it
    std::string hostEntryError_;  ///< why it can't be used, shown under it
    bool historyOpen_ = false;          ///< the version history is shown on the Advanced tab
    bool historyRowPending_ = false;    ///< open the installed version's row at the next draw
    bool historyScrollSet_ = false;     ///< --history-scroll gave the scroll; don't move it to the open row
    bool historyReveal_ = false;        ///< scroll the open row into view at the next draw
    std::string historyRow_;            ///< the version whose row is open ("" = none)
    std::vector<std::string> historyVersions_;  ///< the rows as last drawn (HistoryRow's arg is an index)
    double historyScroll_ = 0.0;        ///< px the list is scrolled
    double historyMaxScroll_ = 0.0;     ///< as far as it can scroll (from the last draw)
    double historyViewH_ = 0.0;         ///< the height it is shown in (from the last draw)
    bool eyecamTab_ = false;            ///< the eye capture tab is in the tab row (eyecam-rec runs)
    eyecam::State eyecamState_ = eyecam::State::Missing;  ///< the recorder's state as last seen (its start button)
    eyecam::StartConfirm eyecamConfirm_;  ///< the light warning before a start

    /**
     * Find the usable button at a point.
     * @param x px from the left
     * @param y px from the top
     * @return the button, or action None
     */
    PanelHit hitTest(double x, double y) const;

    /**
     * Register a button's hit area.
     * @param hit the button
     * @param x left
     * @param y top
     * @param w width
     * @param h height
     * @param usable whether it can be pressed
     */
    void addButton(PanelHit hit, double x, double y, double w, double h, bool usable = true);

    /**
     * The pointer state of a button.
     * @param hit the button
     * @return 0 = idle, 1 = hovered, 2 = pressed
     */
    int pointerState(const PanelHit& hit) const;

    /**
     * The always-visible status column (left).
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     */
    void drawStatus(const Pen& pen, const UiText& t, const PanelModel& model);

    /**
     * The tab row (the eye capture tab last, only while eyecamTab_).
     * @param pen drawing tools
     * @param t texts
     */
    void drawTabs(const Pen& pen, const UiText& t);

    /**
     * The Basic tab.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param view the settings shown
     */
    void drawBasic(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view);

    /**
     * The Basic tab's destination: three cards (VRChat directly, VRCFT LiveLink, VRCFT ETVR), each with what it
     * carries in three short lines.
     * @param pen drawing tools
     * @param t texts
     * @param view the settings shown
     * @param y top
     * @return the height used
     */
    double drawOutputCards(const Pen& pen, const UiText& t, const SettingsView& view, double y);

    /**
     * The Output tab: the target PC and port, then the VRChat-only rows (parameter prefix, EyeTrackingActive type, Steam Link's names, VRChat's own eye tracking),
     * or what to set up in VRCFT for LiveLink and ETVR.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param view the settings shown
     */
    void drawOutput(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view);

    /**
     * The Gaze tab.
     * @param pen drawing tools
     * @param t texts
     * @param model what is shown (the status, for the one-eye hint)
     * @param view the settings shown
     */
    void drawGaze(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view);

    /**
     * The Eye fit tab: one button for the whole fit (gaze and eyelids), re-centering, the result, and the values
     * by hand under "Fine-tune".
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param view the settings shown
     */
    void drawEyeFit(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view);

    /**
     * "Fine-tune" on the Eye fit tab, gaze page: the zero point, the gains, the far-down hold and each eye's own
     * sideways values.
     * @param pen drawing tools
     * @param t texts
     * @param view the settings shown
     * @param saved the fit in config.json
     * @param busy a fit is running (nothing can be changed)
     * @param y top
     */
    void drawEyeFitGaze(const Pen& pen, const UiText& t, const SettingsView& view, const FitInConfig& saved, bool busy,
                        double y);

    /**
     * "Fine-tune" on the Eye fit tab, eyelid page: each eye's four lid readings.
     * @param pen drawing tools
     * @param t texts
     * @param view the settings shown
     * @param saved the fit in config.json
     * @param busy a fit is running (nothing can be changed)
     * @param y top
     */
    void drawEyeFitLids(const Pen& pen, const UiText& t, const SettingsView& view, const FitInConfig& saved, bool busy,
                        double y);

    /**
     * A plain button (or an accent one) with a centered label.
     * @param pen drawing tools
     * @param x left
     * @param y top
     * @param w width
     * @param h height
     * @param label the label
     * @param hit what it does
     * @param usable whether it can be pressed
     * @param accent accent fill
     * @param textSize the label's size (smaller if it doesn't fit)
     */
    void drawButton(const Pen& pen, double x, double y, double w, double h, const std::string& label,
                    const PanelHit& hit, bool usable, bool accent, double textSize = 19);

    /**
     * The Eyelids tab.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param view the settings shown
     */
    void drawLids(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view);

    /**
     * The Advanced tab.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param view the settings shown
     */
    void drawAdvanced(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view);

    /**
     * The version history in place of the Advanced tab: a title row with "Close", one row per version (newest
     * first; the open one shows its summary and items), clipped to the card and scrolled, and ▲ / ▼ on the right.
     * @param pen drawing tools
     * @param t texts
     * @param model the model (the changelog, and the installed version)
     */
    void drawHistory(const Pen& pen, const UiText& t, const PanelModel& model);

    /**
     * The eye capture tab (developer): what eyecam-rec is doing, in large type to read in the headset. Waiting for
     * the camera buffers: the command to run over SSH. Idle: a big "Start". Searching: the fps. Recording: the
     * step's instruction, the seconds left, the step number, a progress bar over the whole run, the fps and
     * "Stop" (and "No light" by the step number for the protocol without the light). Error: the message and "Start
     * again". "Start" and "Start again" open the light warning in its place (drawEyecamConfirm). The recorder's
     * message and a failed command's reply under it.
     * @param pen drawing tools
     * @param t texts
     * @param model the model (its eyecam view)
     */
    void drawEyecam(const Pen& pen, const UiText& t, const PanelModel& model);

    /**
     * The light warning before a start: a red "Light warning" title with a warning sign, the warning in a red box,
     * "Start with light" / "Start without light" and "Cancel".
     * @param pen drawing tools
     * @param t texts
     * @param view the recorder (a command on its way greys out the two starts)
     * @param y the top under the section title
     */
    void drawEyecamConfirm(const Pen& pen, const UiText& t, const eyecam::View& view, double y);

    /**
     * The recommendation prompt over everything (only its buttons stay usable).
     * @param pen drawing tools
     * @param t texts
     */
    void drawPrompt(const Pen& pen, const UiText& t);

    /**
     * The keypad for the target PC over everything (only its buttons stay usable).
     * @param pen drawing tools
     * @param t texts
     */
    void drawHostEntry(const Pen& pen, const UiText& t);

    /**
     * The version row and its button (Advanced tab): the running version, the check result, install progress, and
     * the switch for the automatic check as a chip under the texts, and the new release's summary under all that.
     * @param pen drawing tools
     * @param t texts
     * @param u the update status
     * @param notes the new release's summary in the panel's language ("" = none; see updateNotes)
     * @param checkOn whether the automatic check (update_check) is on
     * @param y row top
     * @return the row's height
     */
    double drawUpdateRow(const Pen& pen, const UiText& t, const frame_updater::UpdateStatus& u,
                         const std::string& notes, bool checkOn, double y);

    /**
     * A notice at the bottom of the status column while a new release is available, installing or installed.
     * Pressing it opens the Advanced tab.
     * @param pen drawing tools
     * @param t texts
     * @param u the update status
     * @param x0 left
     * @param x1 right
     */
    void drawUpdateNotice(const Pen& pen, const UiText& t, const frame_updater::UpdateStatus& u, double x0, double x1);

    /**
     * A section title across the content (muted, with a line under it).
     * @param pen drawing tools
     * @param y top
     * @param title the title
     * @return the height used
     */
    double drawSectionTitle(const Pen& pen, double y, const std::string& title);

    /**
     * A row's title on the left, with a hint or the "locked" note under it.
     * @param pen drawing tools
     * @param t texts
     * @param y row top
     * @param h row height
     * @param title the title
     * @param hint the hint (may be empty)
     * @param locked show the lock note instead of the hint
     */
    void drawRowLabel(const Pen& pen, const UiText& t, double y, double h, const std::string& title,
                      const std::string& hint, bool locked);

    /**
     * A pill with 2 or more choices; the chosen one gets the accent fill, a check mark and bold text.
     * @param pen drawing tools
     * @param x left
     * @param y top
     * @param w width
     * @param h height
     * @param options the choices
     * @param selected the chosen index, -1 if none
     * @param size text size
     * @param locked locked by the command line (gray, shows the value, can't be pressed)
     */
    void drawSegmented(const Pen& pen, double x, double y, double w, double h, const std::vector<Option>& options,
                       int selected, double size, bool locked = false);

    /**
     * A − value ＋ control for one config key.
     * @param pen drawing tools
     * @param x left
     * @param y top
     * @param w width
     * @param h height
     * @param name the key (its step and range come from the key table)
     * @param value the value now
     * @param text the value as shown
     * @param usable whether it can be changed now
     * @param locked locked by the command line
     * @param low extra lower bound
     * @param high extra upper bound
     */
    void drawStepper(const Pen& pen, double x, double y, double w, double h, const char* name, double value,
                     const std::string& text, bool usable, bool locked, double low = -1e9, double high = 1e9);

    /**
     * A caption above a stepper, optionally led by a numbered circle (the lid marks).
     * @param pen drawing tools
     * @param x left
     * @param baseline text baseline
     * @param text the caption
     * @param number 1..4 for a numbered circle, 0 for none
     * @param locked add a padlock (the value is locked by the command line)
     */
    void drawCaption(const Pen& pen, double x, double baseline, const std::string& text, int number, bool locked);
};

/**
 * Draw the dashboard thumbnail (an eye and "Eye"). The launcher icons in contrib/icons are the same picture.
 * @param fonts the fonts
 * @param size edge length in px
 * @param rgba where to write un-premultiplied RGBA
 * @param pngPath also save a PNG here if not empty
 */
void renderThumbnail(const FontSet& fonts, int size, std::vector<uint8_t>& rgba, const std::string& pngPath = "");
