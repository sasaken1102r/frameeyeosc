// The dashboard panel (drawing and hit testing) and its thumbnail. Every color comes from theme.h, so that
// --contrast-report checks what is drawn.
#pragma once

#include "model.h"
#include "ui_state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

class FontSet;
struct Pen;
struct Color;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

/** What a button does. */
enum class PanelAction {
    None,
    Tab,               ///< switch tab (handled inside the panel; arg = tab)
    SetBool,           ///< key = arg != 0
    SetInteger,        ///< key = arg (an integer setting, within its spec)
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
    FitStop,           ///< eye fit: stop the running fit
    SetAutoRecenter,   ///< auto_recenter = kAutoRecenterModes[arg]
    RecordToggle,      ///< start the eye log, or stop it (the caller runs the recorder)
    FitReset,          ///< the fit back to the defaults (fitResetKeys: the gaze fit, lid_fit_*, lid_scale_*)
    FitDetails,        ///< open / close "Fine-tune" (handled inside the panel)
    FitDetailsPage,    ///< show arg (0 gaze, 1 eyelids) under "Fine-tune" (handled inside the panel)
    LidsPage,          ///< show the Eyelids tab's sub-tab arg (LidsPage; handled inside the panel, which opens the tab;
                       ///< the caller remembers it, see lidsPage)
    SetLidWiden,       ///< lid_widen = kLidWidenModes[arg]
    LidPreset,         ///< eyelid smoothing preset arg (0 light, 1 medium, 2 strong)
    NumberSlider,      ///< a setting's slider (key): pressed and dragged inside the panel; the caller takes the value
                       ///< with takeNumberSlider and writes it (SetNumber)
    SetNumber,         ///< key = arg / 1000 (onto its step grid)
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
    EyecamCalib,       ///< a calibration: arg = eyecam::Calib (the panel shows the eye capture tab; the caller sends
                       ///< eyecam::calibCommand)
    EyecamSensitivity, ///< the widening sensitivity slider: pressed and dragged inside the panel; the caller takes
                       ///< the value with takeSensitivity / sensitivityDragging
    SetupKonsole,      ///< the eye cameras' setup: open a Konsole with the command typed in (arg 0 = the tool's install,
                       ///< 1 = passwd; the caller starts it, never from --dump-png)
    SetupVideo,        ///< ...open the setup video (only shown with a video URL)
    SetupProceed,      ///< ...on to the usual page (its "start using", or "continue" with the standard widening)
    EyecamBack,        ///< "Back" on a failed calibration's or recording's error: dismiss it in the panel only
                       ///< (SetupFlow::dismissError; eyecam-rec stays in "error" until the next command)
    DiagOpen,          ///< open the diagnostics page on the Advanced tab (the panel opens it; the caller reads the
                       ///< camera tool's checksum)
    DiagClose,         ///< back to the Advanced tab (handled inside the panel)
    AdvancedScroll,    ///< scroll the Advanced tab's page (or all records) a third of its view, arg -1 up / 1 down
                       ///< (handled inside)
    AdvancedPage,      ///< show the Advanced tab's sub-tab arg (AdvPage; handled inside the panel, which opens the tab;
                       ///< the caller remembers it, see advPage)
    RecordOpen,        ///< open record arg (an index into the records as last drawn; the panel opens its view on the
                       ///< Advanced tab, the caller reads it: recordShown)
    RecordsAll,        ///< open the list of all records (the panel opens it; the caller reads them again)
    RecordBack,        ///< back from a record (to the list it came from) or from all records (handled inside)
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

/** The tabs. Eyecam (the eye cameras) only shows while eyecam-rec runs, before Advanced. */
enum class PanelTab { Basic, Output, Gaze, EyeFit, Lids, Advanced, Eyecam };

/** An icon before a button's label. */
enum class ButtonIcon { None, Terminal, Play };

/**
 * Draws the panel image and finds the button under the laser pointer.
 */
class EyePanel {
public:
    /** Seconds the quit / reset buttons wait for the confirming second press. */
    static constexpr double kConfirmSec = 3.0;
    /** Seconds the sensitivity slider keeps its let-go value at most, waiting for status.json to have it. */
    static constexpr double kSensitivityHoldSec = 3.0;

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
    void setTab(PanelTab tab) {
        if (tab != tab_) advScroll_ = 0.0;
        tab_ = tab;
    }

    /** @return the Advanced tab's sub-tab (shown whenever the tab is; kept while other tabs show) */
    AdvPage advPage() const { return advPage_; }

    /**
     * Choose the Advanced tab's sub-tab (the one remembered from before, at start; or for --adv-page).
     * @param page the sub-tab
     */
    void setAdvPage(AdvPage page) {
        if (page != advPage_) advScroll_ = 0.0;
        advPage_ = page;
    }

    /** @return the Eyelids tab's sub-tab (shown whenever the tab is; kept while other tabs show) */
    LidsPage lidsPage() const { return lidsPage_; }

    /**
     * Choose the Eyelids tab's sub-tab (the one remembered from before, at start; or for --lids-page).
     * @param page the sub-tab
     */
    void setLidsPage(LidsPage page) { lidsPage_ = page; }

    /**
     * Show a record on the Advanced tab ("Having trouble"), as its "View" does (for --record).
     * @param folder its folder name
     */
    void openRecord(const std::string& folder);

    /** Show all records on the Advanced tab ("Having trouble"), as its button does (for --records-all). */
    void openRecordsAll();

    /** @return the record shown ("" = none; only while the Advanced tab shows) */
    std::string recordShown() const { return tab_ == PanelTab::Advanced ? recordOpen_ : std::string(); }

    /** @return true while all records are listed (and no record is open over them) */
    bool recordsAllOpen() const { return tab_ == PanelTab::Advanced && recordsAllOpen_ && recordOpen_.empty(); }

    /** @return the tab shown (Eyecam falls back to Basic at the next draw once its tab is gone) */
    PanelTab tab() const { return tab_; }

    /**
     * Open or close "Fine-tune" on the Eye fit tab.
     * @param open whether it is open
     */
    void setFitDetails(bool open) { fitDetails_ = open; }

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
     * Open the light warning on the Advanced tab, as its recording's start button does (for --fake-eyecam confirm).
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
     * The widening sensitivity slider was let go of (once per release). It keeps showing that value until
     * status.json has it, its command fails (dropSensitivityHold), or kSensitivityHoldSec pass.
     * @param value where to write the value (0..1, two decimals)
     * @param now monotonic seconds
     * @return true if it was let go of since the last call
     */
    bool takeSensitivity(double& value, double now);

    /** @return true while the slider is dragged */
    bool sensitivityDragging() const { return sensDragging_; }

    /** @return the slider's value while dragged (or as last let go of) */
    double sensitivityValue() const { return sensValue_; }

    /** Show status.json's value again (the command failed). */
    void dropSensitivityHold() { sensHeld_ = false; }

    /**
     * A setting's slider was let go of (once per release).
     * @param name where to write its key
     * @param value where to write the value (on the setting's step grid)
     * @return true if one was let go of since the last call
     */
    bool takeNumberSlider(std::string& name, double& value);

    /**
     * For --sensitivity-drag: the slider as if dragged to a value.
     * @param value 0..1
     */
    void previewSensitivityDrag(double value);

    /**
     * Open the version history on the Advanced tab, with the installed version's row open (the newest one if the
     * changelog doesn't have it) and scrolled to the top. Choosing another tab closes it.
     */
    void openHistory();

    /** Close the version history (the Advanced tab shows its rows again). */
    void closeHistory() { historyOpen_ = false; }

    /** @return true while the version history is open */
    bool historyOpen() const { return historyOpen_; }

    /** Show the diagnostics page on the Advanced tab (in place of its rows; choosing another tab closes it). */
    void openDiag();

    /** Close the diagnostics page. */
    void closeDiag() { diagOpen_ = false; }

    /** @return true while the diagnostics page is shown */
    bool diagOpen() const { return diagOpen_ && tab_ == PanelTab::Advanced; }

    /** @return true while the Advanced tab shows its sub-tabs and a page of them (nothing open over them) */
    bool advPageShown() const {
        return tab_ == PanelTab::Advanced && !historyOpen_ && !diagOpen_ && recordOpen_.empty() && !recordsAllOpen_;
    }

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

    /**
     * For --adv-scroll: how far the Advanced tab's page is scrolled (kept within the page when drawn).
     * @param px px from the top of the page
     */
    void setAdvancedScroll(double px) { advScroll_ = std::max(0.0, px); }

    /** @return how far the Advanced tab's page is scrolled (px, as last drawn or scrolled) */
    double advancedScroll() const { return advScroll_; }

    /** @return as far as the Advanced tab's page can scroll (px, from the last draw; 0 = it fits) */
    double advancedMaxScroll() const { return advMaxScroll_; }

    /** @return how far the list of all records is scrolled (px) */
    double recordsScroll() const { return listScroll_; }

    /** @return as far as it can scroll (px, from the last draw; 0 = it fits) */
    double recordsMaxScroll() const { return listMaxScroll_; }

    /**
     * @return true while the panel wants the controller's scroll events: the version history is shown, or the
     * Advanced tab's page (or the list of all records) is taller than its view
     */
    bool wantsScroll() const {
        if (tab_ != PanelTab::Advanced) return false;
        if (historyOpen_) return true;
        if (diagOpen_ || !recordOpen_.empty()) return false;
        return recordsAllOpen_ ? listScrollable_ : advScrollable_;
    }

    /**
     * Scroll the version history, the Advanced tab's page or the list of all records (the thumbstick or touchpad;
     * ignored while none is shown or a prompt is open).
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

    /** A usable button as last drawn: its hit and where it is. */
    struct HitArea {
        PanelHit hit;
        double x, y, w, h;
    };

    /**
     * The usable buttons as last drawn (panel-test checks what each screen offers).
     * @return them, in drawing order
     */
    std::vector<HitArea> hitAreas() const;

    /** Mark ⑤ (lid_open_snap) on the Eyelids tab's bars as last drawn. */
    struct SnapMark {
        bool shown = false;                 ///< drawn (the "Fine-tune" page, and the snap not off)
        double x[2] = {NAN, NAN};           ///< each eye's line (px)
        bool faded[2] = {false, false};     ///< the cameras supply that eye, so it doesn't apply there
        double badgeX = NAN;                ///< the ⑤ badge's center (moved aside from ① to ④ where it would overlap)
    };

    /**
     * Mark ⑤ as last drawn (panel-test checks where it goes).
     * @return it
     */
    const SnapMark& snapMark() const { return snapMark_; }

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
    bool diagOpen_ = false;             ///< the diagnostics page is shown on the Advanced tab
    double advScroll_ = 0.0;            ///< px the Advanced tab's page is scrolled (0 again once the tab is left)
    double advMaxScroll_ = 0.0;         ///< as far as it can scroll (from the last draw)
    double advViewH_ = 0.0;             ///< the height it is shown in
    bool advScrollable_ = false;        ///< the last draw showed the page scrolled, with ▲ / ▼ (it didn't fit)
    AdvPage advPage_ = AdvPage::Version;  ///< the Advanced tab's sub-tab
    LidsPage lidsPage_ = LidsPage::Look;  ///< the Eyelids tab's sub-tab
    SnapMark snapMark_;                   ///< mark ⑤ as last drawn
    std::string recordOpen_;            ///< the record shown on the Advanced tab ("" = none)
    bool recordsAllOpen_ = false;       ///< all records are listed on the Advanced tab (a record may be open over them)
    std::vector<std::string> recordNames_;  ///< the records as last drawn (RecordOpen's arg is an index)
    double listScroll_ = 0.0;           ///< px the list of all records is scrolled
    double listMaxScroll_ = 0.0;        ///< as far as it can scroll (from the last draw)
    double listViewH_ = 0.0;            ///< the height it is shown in
    bool listScrollable_ = false;       ///< the last draw showed it scrolled (it didn't fit)
    bool hitClip_ = false;              ///< addButton keeps only what is between hitClipTop_ and hitClipBottom_
    double hitClipTop_ = 0.0;
    double hitClipBottom_ = 0.0;
    bool eyecamTab_ = false;            ///< the eye capture tab is in the tab row (eyecam-rec runs)
    eyecam::State eyecamState_ = eyecam::State::Missing;  ///< the recorder's state as last seen (its start button)
    eyecam::StartConfirm eyecamConfirm_;  ///< the light warning before a start
    bool sensDragging_ = false;         ///< the widening sensitivity slider is held
    double sensValue_ = 0.0;            ///< its value while held, or as last let go of
    bool sensReleased_ = false;         ///< let go of, not taken yet (takeSensitivity)
    bool sensHeld_ = false;             ///< showing sensValue_ until status.json has it
    double sensHoldUntil_ = 0.0;        ///< ...at most until then (monotonic seconds)
    double sensTrackX_ = 0.0;           ///< the slider's track as last drawn
    double sensTrackW_ = 1.0;
    bool numDragging_ = false;          ///< a setting's slider is held (NumberSlider)
    std::string numKey_;                ///< ...which one
    double numValue_ = 0.0;             ///< ...its value while held, or as last let go of
    bool numReleased_ = false;          ///< ...let go of, not taken yet (takeNumberSlider)
    std::map<std::string, std::pair<double, double>> numTrack_;  ///< each one's track (x, width) as last drawn

    /**
     * The slider's value at a pointer position (on its track, two decimals).
     * @param x px from the left
     * @return 0..1
     */
    double sensitivityAt(double x) const;

    /**
     * Find the usable button at a point.
     * @param x px from the left
     * @param y px from the top
     * @return the button, or action None
     */
    PanelHit hitTest(double x, double y) const;

    /**
     * Register a button's hit area. While hitClip_ is on (a scrolled view), only the part inside it, and nothing if
     * too little of it shows.
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
     * The Eye fit tab while a fit runs: "look at the dot", how, and a big "Stop" (the dots show in front of it).
     * @param pen drawing tools
     * @param t texts
     * @param fit the fit
     */
    void drawEyeFitRunning(const Pen& pen, const UiText& t, const gaze_fit::View& fit);

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
     * The Eyelids tab: its sub-tabs, and under them the one chosen.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param view the settings shown
     */
    void drawLids(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view);

    /**
     * The Eyelids tab's "Look": where the eyelids come from now, how easily widening shows (one row saying it can't
     * on a SteamOS that caps openness, without the cameras), the smoothing presets and the lid sync slider.
     * @param pen drawing tools
     * @param t texts
     * @param m the model
     * @param v the settings shown
     * @param top where it starts
     */
    void drawLidsLook(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v, double top);

    /**
     * The Eyelids tab's "Blinks & squint": the blink hold, blinking both eyes together and the lowest eyelid when
     * narrowed, each with its note beside it.
     * @param pen drawing tools
     * @param t texts
     * @param m the model
     * @param v the settings shown
     * @param top where it starts
     */
    void drawLidsBlinks(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v, double top);

    /**
     * The Advanced tab.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param view the settings shown
     */
    void drawAdvanced(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view);

    /**
     * The Advanced tab's sub-tabs: a segmented control across the card (a dot after "Version" while a new release is
     * available).
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param y its top
     * @param h its height
     */
    void drawSubTabs(const Pen& pen, const UiText& t, const PanelModel& model, double y, double h);

    /**
     * A tab's sub-tabs: a segmented control across the card, one text size for all (the largest at which each fits).
     * @param pen drawing tools
     * @param y its top
     * @param h its height
     * @param pages the sub-tabs: their labels and buttons
     * @param selected the one shown (its button can't be pressed)
     * @param dot the one with a dot after its label (-1 = none)
     */
    void drawPageSwitch(const Pen& pen, double y, double h, const std::vector<Option>& pages, int selected, int dot);

    /**
     * A page drawn into a view: as it is when it fits; else a little narrower beside ▲ / ▼, clipped to the view and
     * scrolled (only what shows can be pressed).
     * @param pen drawing tools
     * @param viewTop the view's top
     * @param viewBottom its bottom
     * @param scroll px scrolled (kept within the page)
     * @param maxScroll where to write how far it can scroll
     * @param viewH where to write the view's height
     * @param scrollable where to write whether it scrolls
     * @param page draws the page from its top to a right edge and returns its height
     */
    void drawScrolled(const Pen& pen, double viewTop, double viewBottom, double& scroll, double& maxScroll,
                      double& viewH, bool& scrollable, const std::function<double(double top, double right)>& page);

    /**
     * The Advanced tab's sub-tab shown, as a page.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param view the settings shown
     * @param top the page's top (moved up by the scroll)
     * @param right the rows' right edge
     * @return the page's height
     */
    double drawAdvancedPage(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view,
                            double top, double right);

    /**
     * "Version": the version (large), when it was last checked and what came of it, the update button and the new
     * release's summary, and under them "Version history", the automatic check's chip and "Check now".
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param view the settings shown
     * @param top the page's top
     * @param right its right edge
     * @return its height
     */
    double drawVersionPage(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view,
                           double top, double right);

    /**
     * "Having trouble": the diagnostics row (its code and "Open"), the newest three records, how to read them over
     * SSH and "All records".
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param top the page's top
     * @param right its right edge
     * @return its height
     */
    double drawTroublePage(const Pen& pen, const UiText& t, const PanelModel& model, double top, double right);

    /**
     * "Debug tools": the gaze dots and their distance, the eye log, and the eye capture while eyecam-rec runs.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param view the settings shown
     * @param top the page's top
     * @param right its right edge
     * @return its height
     */
    double drawToolsPage(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view,
                         double top, double right);

    /**
     * "Files": the files' places, frameeyeosc's PID and how long it has run, and what the command line locks.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param top the page's top
     * @param right its right edge
     * @return its height
     */
    double drawFilesPage(const Pen& pen, const UiText& t, const PanelModel& model, double top, double right);

    /**
     * One record as a row: when, what, a short line, its result's badge and "View".
     * @param pen drawing tools
     * @param t texts
     * @param record the record
     * @param x0 left
     * @param x1 right
     * @param y top
     * @param h height
     */
    void drawRecordRow(const Pen& pen, const UiText& t, const report::Summary& record, double x0, double x1, double y,
                       double h);

    /**
     * A result's badge ("OK" green, "失敗" red, "片目だけ" accent), right-aligned.
     * @param pen drawing tools
     * @param t texts
     * @param result the result
     * @param right its right edge
     * @param cy its middle
     * @param size the text size
     * @return its width
     */
    double drawResultBadge(const Pen& pen, const UiText& t, report::Result result, double right, double cy, double size);

    /**
     * A record in place of the Advanced tab: "‹ Back", what ran and when, the conditions, the result's badge, the reason
     * in a box, the key log lines, and the files with their sizes, the diagnostic code and the folder.
     * @param pen drawing tools
     * @param t texts
     * @param model the model (records.opened)
     */
    void drawRecord(const Pen& pen, const UiText& t, const PanelModel& model);

    /**
     * All records in place of the Advanced tab: "‹ Back", the title, and a row each (scrolled when they don't fit).
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     */
    void drawRecordsAll(const Pen& pen, const UiText& t, const PanelModel& model);

    /**
     * The bar on a failed fit or calibration: its log was saved, where to find it, and "View record". Measured
     * (draw false) or drawn.
     * @param pen drawing tools
     * @param t texts
     * @param folder the record
     * @param calib a calibration's (else a fit's)
     * @param x0 left
     * @param x1 right
     * @param y top
     * @param draw draw it (else only measure)
     * @return its height
     */
    double drawRecordBar(const Pen& pen, const UiText& t, const std::string& folder, bool calib, double x0, double x1,
                         double y, bool draw = true);

    /**
     * ▲ / ▼ at the right edge of a scrolled view (a third of the view per press) and a thin bar between them with
     * where the view is.
     * @param pen drawing tools
     * @param action the buttons' action (arg -1 up, 1 down)
     * @param x the column's left
     * @param viewTop the view's top
     * @param viewBottom the view's bottom
     * @param scroll px scrolled
     * @param maxScroll as far as it can scroll
     * @param contentH the content's height
     */
    void drawScrollBar(const Pen& pen, PanelAction action, double x, double viewTop, double viewBottom, double scroll,
                       double maxScroll, double contentH);

    /**
     * The version history in place of the Advanced tab: a title row with "Close", one row per version (newest
     * first; the open one shows its summary and items), clipped to the card and scrolled, and ▲ / ▼ on the right.
     * @param pen drawing tools
     * @param t texts
     * @param model the model (the changelog, and the installed version)
     */
    void drawHistory(const Pen& pen, const UiText& t, const PanelModel& model);

    /**
     * The diagnostics page in place of the Advanced tab: the title and what it is for, the diagnostic code and "Back",
     * then four cards of label / value rows (diag::cards). A value that doesn't fit beside its label goes under it
     * (two lines at most, one if the card would overflow, smaller type last).
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     */
    void drawDiag(const Pen& pen, const UiText& t, const PanelModel& model);

    /**
     * A small pill that opens the diagnostics page.
     * @param pen drawing tools
     * @param t texts
     * @param right its right edge
     * @param top its top
     * @param h its height
     * @param size its text size
     * @param draw draw it (else only measure)
     * @return its width
     */
    double drawDiagChip(const Pen& pen, const UiText& t, double right, double top, double h, double size,
                        bool draw = true);

    /**
     * The eye cameras tab: the setup checklist until it is done (drawSetup), then their page (drawCameraPage); a
     * calibration from that page and how it ended show as the setup's card (drawPageCalib), its failure as drawRun.
     * @param pen drawing tools
     * @param t texts
     * @param model the model (its eyecam view)
     * @param view the settings shown
     */
    void drawEyecam(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view);

    /**
     * What eyecam-rec runs, in large type to read in the headset: the light warning before a recording, searching,
     * recording and calibrating (the step's instruction, the seconds left, the step number, a progress bar, the fps
     * and "Stop"; "No light" or the calibration's name by the step number), and a failed calibration or recording
     * with "again". The recorder's message and a failed command's reply under it. (The recording's on the Advanced
     * tab, the calibrations' on the eye cameras tab.)
     * @param pen drawing tools
     * @param t texts
     * @param model the model (its eyecam view)
     */
    void drawRun(const Pen& pen, const UiText& t, const PanelModel& model);

    /**
     * The setup checklist: (1) a password, (2) the tool, (3) the eye movements, (4) done. Steps done in a row each
     * (green), the current one in a card (setupCard), those to come muted under it.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param screen which one (not Camera)
     */
    void drawSetup(const Pen& pen, const UiText& t, const PanelModel& model, eyecam::SetupScreen screen);

    /**
     * A calibration from the usual page, and how it ended, in the setup's card: titled with the calibration instead of
     * (3), without the checklist around it.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param screen Learn (calibrating), Fail (the standard widening) or Calibrated
     */
    void drawPageCalib(const Pen& pen, const UiText& t, const PanelModel& model, eyecam::SetupScreen screen);

    /**
     * The current step's card in the checklist (or a calibration's from the usual page), measured (draw false) or
     * drawn.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param screen which step and how
     * @param x0 its text's left
     * @param x1 its text's right
     * @param top its top
     * @param draw draw it (else only measure)
     * @param page from the usual page: titled with the calibration (this wear's or the user's) instead of (3)
     * @return its height
     */
    double setupCard(const Pen& pen, const UiText& t, const PanelModel& model, eyecam::SetupScreen screen, double x0,
                     double x1, double top, bool draw, bool page = false);

    /**
     * The eye cameras' usual page: what drives the eyelids now, camera_lids, the widening sensitivity, a calibration
     * when something feels off, the user's own (optional), and what to do when.
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param view the settings shown
     */
    void drawCameraPage(const Pen& pen, const UiText& t, const PanelModel& model, const SettingsView& view);

    /**
     * The Eyelids tab's widening slider ("見開きの出やすさ"): dull at the left, sensitive and the value at the right.
     * For the cameras (both eyes, or one) it is eyecam-rec's sensitivity (held, its own value, and after it is let go
     * of until status.json has it); for Valve's values lid_widen's four stops (a press goes to the nearest one, its
     * level's name at the right); greyed when nothing can be driven.
     * @param pen drawing tools
     * @param t texts
     * @param m the model (eyecam-rec's status)
     * @param v the settings shown
     * @param widen what it drives (widenSlider)
     * @param x0 left
     * @param x1 right
     * @param y top
     * @param h height
     */
    void drawWidenSlider(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v,
                         const WidenSlider& widen, double x0, double x1, double y, double h);

    /**
     * A setting's slider over its whole range: the low end's word at the left, the high end's and the value at the
     * right. Held, it follows the pointer (numberShown); let go of, the caller writes it.
     * @param pen drawing tools
     * @param name the setting
     * @param value its value (numberShown)
     * @param low the left end's word
     * @param high the right end's word
     * @param x0 left
     * @param x1 right
     * @param y top
     * @param h height
     * @param usable it can be moved (not locked)
     */
    void drawNumberSlider(const Pen& pen, const char* name, double value, const char* low, const char* high, double x0,
                          double x1, double y, double h, bool usable);

    /**
     * A setting's value as its slider shows it: its own while held.
     * @param name the setting
     * @param value the setting's value
     * @return the value to show
     */
    double numberShown(const char* name, double value) const;

    /**
     * A setting's value at a pointer position on its slider's track (on its step grid).
     * @param name the setting
     * @param x px from the left
     * @return the value (NaN if its slider wasn't drawn)
     */
    double numberAt(const std::string& name, double x) const;

    /**
     * The Eyelids tab's "Fine-tune": a line on what decides the eyelids, the auto calibration (eyes without a fit,
     * not from the cameras), the per-eye scales, the openness bars with the four marks and their values, and the
     * smoothing values.
     * @param pen drawing tools
     * @param t texts
     * @param m the model
     * @param v the settings shown
     * @param top where it starts
     */
    void drawLidsDetails(const Pen& pen, const UiText& t, const PanelModel& m, const SettingsView& v, double top);

    /**
     * A button with an icon before its label (centered together).
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
     * @param icon the icon
     */
    void drawIconButton(const Pen& pen, double x, double y, double w, double h, const std::string& label,
                        const PanelHit& hit, bool usable, bool accent, double textSize, ButtonIcon icon);

    /**
     * At the bottom of the status column while the eye cameras aren't set up: what to do next (a button to their
     * tab); right after the setup, that they are ready (green, for a short while).
     * @param pen drawing tools
     * @param t texts
     * @param model the model
     * @param screen the eye cameras' screen (eyecam::setupScreen)
     * @param x0 left
     * @param x1 right
     */
    void drawSetupNotice(const Pen& pen, const UiText& t, const PanelModel& model, eyecam::SetupScreen screen,
                         double x0, double x1);




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
     * @param lineRight where the line under it stops (0 = the content's right edge)
     * @return the height used
     */
    double drawSectionTitle(const Pen& pen, double y, const std::string& title, double lineRight = 0);

    /**
     * A row's title on the left, with a hint or the "locked" note under it.
     * @param pen drawing tools
     * @param t texts
     * @param y row top
     * @param h row height
     * @param title the title
     * @param hint the hint (may be empty)
     * @param locked show the lock note instead of the hint
     * @param indent how far right of the usual place (room for a numbered circle before it)
     */
    void drawRowLabel(const Pen& pen, const UiText& t, double y, double h, const std::string& title,
                      const std::string& hint, bool locked, double indent = 0);

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
     * @param outline its outline while usable (nullptr: the usual border)
     */
    void drawStepper(const Pen& pen, double x, double y, double w, double h, const char* name, double value,
                     const std::string& text, bool usable, bool locked, double low = -1e9, double high = 1e9,
                     const Color* outline = nullptr);

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
