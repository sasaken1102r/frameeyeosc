// On-screen text in Japanese and English. Drawing code never contains text itself; it takes it from here.
// Logs stay in English and are not in this table.
#pragma once

#include <string>

/** Display language. */
enum class Language { Ja, En };

/**
 * The Frame's system language, used while config.json has no language.
 * Japanese if Steam's language setting (the "language" value in ~/.steam/registry.vdf, read only) is Japanese;
 * if that can't be read, Japanese if LC_ALL / LC_MESSAGES / LANG is; English otherwise.
 * Looked up once and remembered.
 * @return the language
 */
Language systemLanguage();

/**
 * All text of the panel. Fields ending in "Format" are printf formats.
 */
struct UiText {
    // Status column
    const char* title;              ///< panel heading
    const char* badgeSending;       ///< sending
    const char* badgeWaiting;       ///< running, sending, but no eye data
    const char* badgePaused;        ///< sending is paused
    const char* badgeNotRunning;    ///< frameeyeosc is not running
    const char* notRunningHint1;    ///< what that means (line 1)
    const char* notRunningHint2;    ///< (line 2)
    const char* destination;        ///< "Destination" label
    const char* outputVrchatShort;  ///< "VRChat" in the destination line
    const char* outputEtvrShort;    ///< "VRCFaceTracking" in the destination line
    const char* outputLivelinkShort;  ///< "VRCFT (LiveLink)" in the destination line
    const char* searchingPc;        ///< auto target not found yet
    const char* modeAuto;           ///< target chosen automatically
    const char* modeFixed;          ///< target fixed
    const char* rateLabel;          ///< messages per second label
    const char* rateFormat;         ///< "%.0f /s"
    const char* trackerRateLabel;   ///< samples from the eye tracker per second (label; the value uses rateFormat)
    const char* trackerRateLowHint;  ///< shown when it is low
    const char* trackerRateSlowHere;     ///< the line below a low rate: frameeyeosc was too slow to take the samples
    const char* trackerRateSlowTracker;  ///< the line below a low rate: the eye tracker itself delivered few
    const char* lidsTitle;          ///< eyelids heading
    const char* legendRaw;          ///< raw value (legend)
    const char* legendSent;         ///< sent value (legend)
    const char* left;               ///< "L"
    const char* right;              ///< "R"
    const char* gazeTitle;          ///< gaze heading
    const char* leftEye;            ///< "Left" over the left eye's gaze pad
    const char* rightEye;
    const char* noEyeData;          ///< no eye data
    const char* errorPrefix;        ///< before frameeyeosc's config_error
    const char* sourceErrorPrefix;  ///< before frameeyeosc's source_error (the eye tracker can't be read)
    const char* dominantEyeLeft;    ///< "Track Dominant Eye Only" is on with the left eye (gaze title line)
    const char* dominantEyeRight;   ///< ...with the right eye
    const char* errWrite;           ///< writing config.json failed
    const char* errConfigBroken;    ///< config.json can't be parsed
    const char* errAutostart;       ///< systemctl enable/disable failed

    // Tabs
    const char* tabBasic;
    const char* tabOutput;
    const char* tabGaze;
    const char* tabGazeFit;
    const char* tabLids;
    const char* tabAdvanced;
    const char* tabEyecam;          ///< the developer tab, only while eyecam-rec runs

    // Shared
    const char* on;
    const char* off;
    const char* locked;             ///< "Locked by command line"
    const char* lowerSmoother;      ///< lower = smoother
    const char* capStill;           ///< One Euro min cutoff caption
    const char* capFast;            ///< One Euro beta caption
    const char* capChange;          ///< One Euro derivative cutoff caption

    // Basic tab
    const char* rowSending;
    const char* hintSending;
    const char* send;
    const char* stop;
    const char* rowOutput;
    const char* hintOutput;         ///< what "sync" means on the cards
    const char* outputVrchat;       ///< the destination cards' titles
    const char* outputLivelink;
    const char* outputEtvr;
    const char* outputRecommended;  ///< the tag on the LiveLink card
    /** Each card's three lines (VRChat, LiveLink, ETVR): wide eyes, sync, VRCFT. A leading "✓ " or "✗ " is drawn. */
    const char* outputMarks[3][3];
    const char* rowActiveType;      ///< how EyeTrackingActive is sent
    const char* hintActiveType;
    const char* activeOff;
    const char* rowSteamlink;       ///< also send the avatar parameters Steam Link's own OSC sends
    const char* hintSteamlink;
    const char* steamlinkNoPrefix;  ///< after its example address: the prefix is never added
    const char* rowPupils;  ///< LiveLink output: send the pupils straight to VRChat (pupils_to_vrchat)
    const char* hintPupils;  ///< ...why, under it
    const char* pupilsTargetFormat;  ///< ...where they go now ("to %s")
    const char* rowPupilBits;  ///< how the avatar takes pupils: a float or that many bits (pupil_bits)
    const char* pupilBitsFloat;  ///< ...its first segment (0)
    const char* hintPupilBits;  ///< ...under it: how to tell the count
    const char* hintPupilBitsOff;  ///< ...under it while greyed (the pupils don't go straight to VRChat)
    const char* rowNativeEyes;      ///< also send VRChat's own eye tracking input (/tracking/eye/*)
    const char* hintNativeEyes;
    const char* rowTarget;
    const char* hintTarget;
    const char* targetAuto;
    const char* targetFixNow;       ///< "Fix to current PC"
    const char* targetEnter;        ///< "Enter IP"
    const char* targetManualFormat;  ///< "Manual %s": a host set by hand (typed or fixed)
    const char* hostEntryTitle;     ///< the keypad for the target PC (IPv4 only; names go in config.json)
    const char* hostEntryHint;
    const char* hostEntryOk;
    const char* hostEntryCancel;
    const char* hostErrEmpty;
    const char* hostErrIpv4;
    const char* rowPort;
    const char* portDefaultVrchat;  ///< the port row's hint: the default port of the output type
    const char* portDefaultLivelink;
    const char* portDefaultEtvr;
    const char* portReset;          ///< back to default port
    // Output tab, for LiveLink and ETVR: what to set up in VRCFT on the PC
    const char* vrcftSetupTitle;
    const char* vrcftStepsLivelink[3];
    const char* vrcftStepsEtvr[3];
    const char* vrcftSetupNote;     ///< the parameter names and syncing are up to VRCFT
    const char* vrcftNoWide;        ///< ETVR: widened eyes don't come through
    const char* rowLanguage;
    const char* rowAutostart;
    const char* hintAutostart;
    const char* autostartMissing;
    const char* autostartUnknown;
    const char* resetAll;
    const char* resetConfirm;
    const char* quit;
    const char* quitConfirm;
    const char* footer;             ///< changes apply at once

    // Gaze tab
    const char* rowSmoothing;
    const char* hintSmoothing;
    const char* rowStrength;
    const char* strengthLight;
    const char* strengthMedium;
    const char* strengthStrong;
    const char* custom;             ///< values don't match a preset
    const char* rawOnNote;          ///< smoothing is off, so these don't apply
    const char* rowFine;
    const char* rowDeadzone;
    const char* hintDeadzone;
    const char* rowHold;
    const char* hintHold;
    const char* rowIndependent;
    const char* hintIndependent;
    const char* hintIndependentOneEye;  ///< instead, while the Frame tracks one eye alone (both eyes get its gaze)
    const char* rowQuality;
    const char* hintQuality;
    const char* rowDespike;
    const char* hintDespike;

    // Eye fit tab
    const char* rowFit;
    const char* hintFit;
    const char* fitStart;            ///< the one big button before any fit
    const char* fitAgain;            ///< the same button once fitted
    const char* fitCenterOnly;       ///< small button: re-center the gaze only...
    const char* fitCenterTilt;       ///< ...or re-center and measure the tilt (as auto_recenter says)
    const char* fitStop;
    const char* fitIntro;            ///< before any fit
    const char* fitNeedsRunning;
    const char* fitLocked;
    const char* fitWaiting;          ///< "Close the dashboard to start"
    const char* fitHowTo;            ///< while waiting / running (full fit)
    const char* fitWaitingCenter;    ///< while waiting / running (re-centering)
    const char* fitWaitingTilt;      ///< while waiting / running (re-centering and the tilt)
    const char* fitRunningFormat;    ///< "Measuring: %s (%d of %d)"
    const char* fitRetryFormat;      ///< appended: ", try %d"
    const char* fitDone;             ///< right after a full fit
    const char* fitDoneCenter;       ///< right after re-centering
    const char* fitDoneTilt;         ///< right after re-centering and the tilt
    const char* fitFitted;           ///< a fit is in config.json
    const char* fitNotYet;           ///< the folded result line before any fit
    const char* fitGazeCenterFormat; ///< "Gaze center: L-R %s, U-D %s, tilt %s"
    const char* fitGazeRangeFormat;  ///< "Gaze range: L-R %s, up %s, down %s"
    const char* fitGazeNone;
    const char* fitEyeXFormat;       ///< "Each eye: L %s x%s, R %s x%s" (zero point, gain)
    const char* fitLidFormat;        ///< "Eyelid %s: open %s, closed %s, looking down %s"
    const char* fitLidsNone;
    const char* fitFailed;
    const char* failCancelled;
    const char* failWaitTimedOut;
    const char* failNotRunning;
    const char* failNoResult;
    const char* failUnsteadyFormat;  ///< %s = the point
    const char* failNotClosed;
    const char* failNoMovementFormat;
    const char* failLidRange;
    const char* failWrite;
    // The numbers behind a failure (one line under it)
    const char* failDetailSeparator;      ///< between the parts ("・")
    const char* failDetailPointFormat;    ///< "%s dot"
    const char* failDetailSamplesFormat;  ///< "%d of %d samples usable" (from frameeyeosc before 0.5.3)
    const char* failDetailSamplesRateFormat;  ///< "%d of %d samples usable%s (needs %d)", %s = failDetailRateFormat
    const char* failDetailRateFormat;     ///< " at %.0f Hz"
    const char* failDetailSpreadFormat;   ///< "spread %s (max %.1f°)"
    const char* failDetailTriesFormat;    ///< "%d tries"
    const char* failDetailEyeLeft;        ///< "L"
    const char* failDetailEyeRight;
    const char* failDetailClosedFormat;   ///< "%s %.2f (needs below %.2f)"
    const char* failDetailMovedFormat;    ///< "moved %.1f° (needs %.1f°)"
    const char* failDetailSidewaysLeft;   ///< "left eye sideways" (its own fit)
    const char* failDetailSidewaysRight;
    const char* failDetailLidWhereFormat;  ///< "%s eyelid, %s dot"
    const char* failDetailLidFormat;      ///< "open %.2f vs shut %.2f, %.2f apart (needs %.2f)"
    const char* failDetailLidNone;        ///< no open reading
    const char* pointCenter;
    const char* pointUp;
    const char* pointDown;
    const char* pointLeft;
    const char* pointRight;
    const char* pointClosed;
    const char* targetClose;         ///< on the target: "Close your eyes\nfor 3 s" (two lines)
    const char* targetKeepClosed;
    const char* targetOpen;
    const char* fitReset;
    const char* fitDetails;          ///< the fold with the values by hand
    const char* fitSoundsOn;         ///< the sound switch, on ("Sounds: on")
    const char* fitSoundsOff;
    const char* rowAutoRecenter;     ///< the row for what runs when the headset is put on...
    const char* hintAutoRecenter;
    const char* autoRecenterOff;     ///< ...and its choices: nothing...
    const char* autoRecenterCenter;  ///< ...re-centering...
    const char* autoRecenterTilt;    ///< ...or re-centering and the tilt
    const char* rowOffset;
    const char* hintOffset;
    const char* rowGain;
    const char* hintGain;
    const char* capLeftRight;
    const char* capUpDown;
    const char* capUp;
    const char* capDown;
    const char* detailsGaze;         ///< "Fine-tune" pages
    const char* detailsLids;
    const char* rowEyeX;             ///< each eye's own sideways zero point and gain
    const char* hintEyeX;
    const char* rowDownHold;         ///< holding the sideways gaze when looking far down
    const char* hintDownHold;
    const char* rowEyeLog;           ///< the eye log row (Advanced tab)
    const char* eyeLogRecord;        ///< its button: start...
    const char* eyeLogStopFormat;    ///< ...and stop, with the time so far ("Stop 1:23")
    const char* eyeLogWhereFormat;   ///< where the files go ("Saved to %s")...
    const char* eyeLogLimit;         ///< ...and that it stops by itself after 60 minutes
    const char* eyeLogFailedFormat;  ///< it could not start or ended by itself ("Couldn't record: %s")
    const char* eyeLogAutoStopped;   ///< the last one was stopped by the 60-minute limit
    const char* recordingFormat;     ///< the mark in the left column while recording ("Recording 1:23")
    const char* rowTilt;             ///< the headset's tilt (gaze_roll_deg), next to the far-down hold
    const char* hintTilt;
    const char* downHoldFormat;      ///< "Below %s°"
    const char* rowLidFit;
    const char* hintLidFit;
    const char* capClosed;
    const char* capAhead;
    const char* lidFitInUse;         ///< Eyelids tab, instead of the learned values
    const char* rowWiden;            ///< how easily a fitted eye widens (lid_widen)...
    const char* hintWiden;
    const char* widenModes[4];       ///< ...off, less, normal, more
    const char* widenFollowsLeft;    ///< the left eye has no room and widens with the right
    const char* widenFollowsRight;
    const char* widenNoRoom;         ///< neither eye has room
    const char* opennessSaturated;   ///< a relaxed open eye reads 1.0 (SteamOS 0.4.3), so widening can't come through
    const char* lidMarksFitted;      ///< above the bars for fitted eyes (the marks folded away)
    const char* lidMarksUnused;      ///< the same, the marks open: 3 and 4 are only for eyes without a fit

    // Lids tab
    const char* rowCalibration;
    const char* learnedFormat;      ///< "Learned L %s / R %s"
    const char* notLearned;
    const char* learning;
    const char* calibrationReset;
    const char* rowScale;
    const char* hintScaleAuto;
    const char* hintScaleFixed;
    const char* hintScaleFitted;    ///< the scales of fitted eyes fine-tune the fit
    const char* scaleAuto;
    const char* scaleFixed;
    const char* marksTitle;         ///< "Open and close your eyes and match the lines"
    const char* markClosed;
    const char* markOpen;
    const char* markWidenStart;
    const char* markWide;
    const char* rowSync;
    const char* hintSync;
    const char* rowBlink;
    const char* hintBlink;
    const char* blinkHold;          ///< label before the blink_hold_ms stepper
    const char* blinkSync;          ///< label before the blink_sync_below stepper
    const char* rowLidSmooth;

    // Advanced tab
    const char* sectionTools;       ///< section titles: the gaze dots and the eye log...
    const char* sectionFiles;       ///< ...and the file locations and the process
    const char* updateCheckChip;    ///< the automatic update check, as a chip in the version row ("... On")
    const char* historyButton;      ///< the version row's button that opens the version history
    const char* historyTitle;       ///< the version history's title
    const char* historyClose;       ///< its button back to the Advanced tab
    const char* historyMissing;     ///< no CHANGELOG.md (or CHANGELOG.ja.md) was found
    // The diagnostics page (diag.h; the Advanced tab's "Diagnostics")
    const char* diagButton;              ///< opens the diagnostics page (Advanced tab; eye cameras' "no video")
    const char* diagTitle;               ///< the diagnostics page's title
    const char* diagSub;                 ///< ...the line under it
    const char* diagCodeLabel;           ///< over the diagnostic code
    const char* diagBack;                ///< back to the Advanced tab
    const char* diagCardVersions;        ///< the cards' titles
    const char* diagCardEyeData;
    const char* diagCardCameras;
    const char* diagCardCalib;
    const char* diagRowTool;             ///< the rows' labels
    const char* diagRowOutput;
    const char* diagRowGaze;
    const char* diagRowSend;
    const char* diagRowCap;
    const char* diagRowLids;
    const char* diagRowCoreError;
    const char* diagRowState;
    const char* diagRowSearch;
    const char* diagRowBlocks;
    const char* diagRowProx;
    const char* diagRowPupil;
    const char* diagRowLoad;
    const char* diagRowWhen;
    const char* diagRowResult;
    const char* diagRowPupilFrames;
    const char* diagRowPupilAt;
    const char* diagRowWindow;
    const char* diagRowLastError;
    const char* diagToolOutdated;        ///< the tool: an update brought a newer one
    const char* diagToolMissing;         ///< ...not found where install.sh puts it
    const char* diagModeAuto;            ///< the target PC: Steam Link's
    const char* diagModeFixed;           ///< ...fixed (the host follows)
    const char* diagRateFormat;          ///< a rate ("%.0f": a second)
    const char* diagMissedFormat;        ///< samples frameeyeosc missed in the last second
    const char* diagDroppedFormat;       ///< datagrams dropped in the last second
    const char* diagPaused;              ///< sending paused
    const char* diagCapOn;               ///< a relaxed open eye reads 1.0 (SteamOS 0.4.3)
    const char* diagCapOff;
    const char* diagLidsBoth;            ///< where the eyelids come from
    const char* diagLidsLeft;
    const char* diagLidsRight;
    const char* diagLidsValve;
    const char* diagNone;                ///< no error
    const char* diagCoreNotRunning;      ///< frameeyeosc isn't running
    const char* diagCoreNoTarget;        ///< no target PC found (Steam Link's)
    const char* diagWithTimeFormat;      ///< when a message came, and the message ("%s · %s")
    const char* diagVideoSlotsFormat;    ///< the eye video flows (%d: the ring's slots)
    const char* diagVideo;               ///< ...without the slots
    const char* diagNotWorn;             ///< not found: the proximity sensor says the headset is off
    const char* diagNoVideo;             ///< not found though worn
    const char* diagOneEyeOnly;          ///< only one eye's video
    const char* diagSearching;           ///< searching, nothing said yet
    const char* diagLiveOff;             ///< live processing off
    const char* diagWaitingTool;         ///< waiting for the tool to hand over the buffers
    const char* diagNoRecorder;          ///< eyecam-rec isn't running
    const char* diagError;               ///< eyecam-rec is in "error"
    const char* diagCandidatesFormat;    ///< the last look: candidate frames
    const char* diagHzFormat;            ///< ...how often they were rewritten
    const char* diagSlotsFormat;         ///< ...the ring's slots
    const char* diagBothEyes;
    const char* diagOneEye;
    const char* diagStopNoCandidates;    ///< ...where it stopped
    const char* diagStopNotRefreshing;
    const char* diagStopFewSlots;
    const char* diagChangedFormat;       ///< the 64 KiB blocks changed before the last look
    const char* diagNotLooked;           ///< no look yet
    const char* diagUnreadable;          ///< the proximity sensor can't be read
    const char* diagEyesFormat;          ///< a value per eye ("L %s · R %s")
    const char* diagMsFormat;            ///< live processing time per frame
    const char* diagOk;                  ///< the last calibration went through
    const char* diagFailedFormat;        ///< ...failed (%s: why)
    const char* diagPreviousLeft;        ///< ...without the left eye (it kept its previous values)
    const char* diagPreviousRight;
    const char* diagSameAsResult;        ///< eyecam-rec's last error is the last calibration's result
    const char* diagNoCalib;             ///< no calibration yet

    // The eye cameras tab (eyecam-rec's state and controls; its recording is on the Advanced tab)
    const char* eyecamTitle;           ///< the title over the tab
    const char* eyecamStart;           ///< the big button that sends "start"
    const char* eyecamSearching;       ///< searching: looking for the eyes
    const char* eyecamStop;            ///< the button that sends "stop"
    const char* eyecamErrorTitle;      ///< error: the heading over the recorder's message
    const char* eyecamRetry;           ///< error: start again
    const char* eyecamUnknownFormat;   ///< a state this panel doesn't know ("State: %s")
    const char* eyecamFpsFormat;       ///< "fps  L %s / R %s"
    const char* eyecamNotLocked;       ///< while recording without camera frames (headset off): put it on
    // Why the eyes' video isn't found, as eyecam-rec says (status.json "search"; eyecam::searchText), in place of the
    // call to put the headset on and under the setup's (3)
    const char* searchNotWornFormat;   ///< the proximity sensor says the headset is off (%d: its reading)
    const char* searchNotWorn;         ///< ...without a reading
    const char* searchNoVideo;         ///< worn, but no eye video (eye tracking off in SteamVR?)
    const char* searchOneEye;          ///< only one eye's video
    const char* eyecamRemainingFormat; ///< seconds left of the step ("%d s left")
    const char* eyecamStepFormat;      ///< "Step %d of %d"
    const char* eyecamSending;         ///< a command waits for its reply
    const char* eyecamReplyFailedFormat;  ///< the last command failed ("Couldn't %s: %s")
    const char* eyecamStepNormal;      ///< the instruction of each step label
    const char* eyecamStepWiden;
    const char* eyecamStepClose;
    const char* eyecamStepSquint;
    const char* eyecamStepLookUp;
    const char* eyecamStepLookDown;
    const char* eyecamStepBright;
    const char* eyecamStepDark;
    const char* eyecamStepEnd;
    const char* eyecamStepLeadIn;  ///< the countdown before the first step
    const char* eyecamLightTitle;      ///< the light warning before a start: its red title
    const char* eyecamStorageNote;     ///< ...under it: where the video goes, how big, and to delete it
    const char* eyecamStorageRow;      ///< the same, shorter, next to the recording's button
    const char* eyecamLightWarning;    ///< ...the warning ("\n" breaks a line, "\n\n" between paragraphs)
    const char* eyecamStartWithLight;  ///< ...sends "start"
    const char* eyecamStartNoLight;    ///< ...sends "start widen_nolight"
    const char* eyecamCancel;          ///< ...back without starting
    const char* eyecamNoLight;         ///< by the step number while recording without the light
    // ...the eye cameras for frameeyeosc (their usual page): the camera_lids switch and what frameeyeosc uses now
    const char* rowCameraLids;         ///< the camera_lids switch
    const char* cameraUseBoth;         ///< the cameras drive both eyes
    const char* cameraUseLeft;         ///< ...only the left one
    const char* cameraUseRight;        ///< ...only the right one
    const char* cameraUseValve;        ///< Valve's values (camera_lids off, or no reason given)
    const char* cameraUseValveFormat;  ///< Valve's values, and why ("... (%s)")
    const char* cameraWhyNotCalibrated;  ///< ...not calibrated for this wear
    const char* cameraWhyNoCamera;     ///< ...no live camera values reach frameeyeosc
    const char* cameraWhyWarming;      ///< ...eyecam-rec is still learning the relaxed eyes
    const char* cameraPupilSuffix;     ///< after "in use" when the pupils come from the cameras too
    const char* eyecamWarmingFormat;   ///< by the cameras' title while learning the relaxed eyes ("... %d s left")
    const char* eyecamLiveOff;         ///< eyecam-rec doesn't read the cameras live (nothing to calibrate for)
    // ...while calibrating, and after a failed calibration
    const char* eyecamCalibWearTitle;  ///< the title while calibrating for this wear
    const char* eyecamCalibUserTitle;  ///< ...for the user
    const char* eyecamCalibWaiting;    ///< calibrating, before the first step (waiting for the video)
    const char* eyecamCalibErrorTitle; ///< error after a calibration: the heading over the recorder's message
    const char* eyecamCalibRetry;      ///< ...the same calibration again
    const char* eyecamSensitivity;     ///< the widening sensitivity slider (eye cameras)...
    const char* eyecamSensitivityDull; ///< ...its left end
    const char* eyecamSensitivitySharp;  ///< ...its right end
    // The eye cameras' setup checklist, their usual page, the Eyelids tab's widening row, the developer recording
    const char* setupTitle;  ///< the eye cameras tab while not set up: the checklist title
    const char* setupOptional;  ///< ...at its right at (1) and (2)
    const char* setupOneLeft;  ///< ...at (3)
    const char* setupAllDone;  ///< ...when done (green)
    const char* setupStepPassword;  ///< step (1)
    const char* setupStepTool;  ///< step (2)
    const char* setupStepToolAgain;  ///< ...when an update brought a new tool (grab_outdated)
    const char* setupToolUpdated;  ///< ...the line under its title, and in the left column
    const char* toolNoticeOutdated;  ///< set up, the tool outdated: the usual page's card
    const char* eyecamBack;          ///< "Back" on a calibration's or recording's error
    const char* camCalibNeedsLids;   ///< the page's calibrations while camera_lids is off (greyed)
    const char* eyecamUserNeedsWear; ///< a failed user calibration that can't be retried (the headset put back on)
    const char* toolNoticeTooOld;    ///< set up, the tool below the safety floor: the usual page's card (stronger)
    const char* nextToolOutdated;    ///< ...the left column's card, under "install again"
    const char* nextToolTooOld;
    const char* setupStepLearn;  ///< step (3)
    const char* setupStepDone;  ///< step (4)
    const char* setupLaterTool;  ///< a step still to come, after its name
    const char* setupLaterLearn;
    const char* setupLaterDone;
    const char* setupPasswordLabel;  ///< a step done: its name...
    const char* setupPasswordSet;  ///< ...and what it is (green)
    const char* setupAutoChecked;  ///< ...and how it was found
    const char* setupToolLabel;
    const char* setupToolDone;
    const char* setupLearnLabel;
    const char* setupLearnDone;
    const char* setupPassPill;  ///< (1): the pill at the right of its title
    const char* setupPassBody;  ///< (1): why
    const char* setupPassWhere;  ///< (1): rows: where...
    const char* setupPassPath1;  ///< ...the settings path
    const char* setupPassPath2;
    const char* setupPassPath3;
    const char* setupPassKonsole;  ///< ...with Konsole
    const char* setupPassKonsoleHow;
    const char* setupPassMemo;  ///< ...a note
    const char* setupPassMemoText;
    const char* setupPassButton;  ///< ...its button
    const char* setupVideo;  ///< the video button (only with a video URL)
    const char* setupVideoNote;  ///< ...the line under it, before the URL
    const char* setupCheckPill;  ///< (2): the pill
    const char* setupCheckFlow;  ///< (2): how, before the command
    const char* setupCheckTyped;  ///< ...after it
    const char* setupCheckWhat;  ///< (2): rows
    const char* setupCheckWhatText;
    const char* setupCheckPassword;
    const char* setupCheckPasswordText;
    const char* setupCheckSsh;
    const char* setupCheckSshText;
    const char* setupCheckButton;  ///< (2): its button
    const char* setupWaitPill;  ///< (3) before its button: the pill
    const char* setupWaitTitle;  ///< ...the big line
    const char* setupWaitVideo;  ///< ...rows: the camera video...
    const char* setupWaitVideoOk;
    const char* setupWaitVideoNo;
    const char* setupWaitEyeOk;  ///< ...each eye
    const char* setupWaitEyeNo;
    const char* setupWaitEyeNoPupil;  ///< ...the video there, the pupil not found (eyecam::EyeSight::NoPupil)
    const char* setupWaitEyeWeak;  ///< ...the pupil found only some of the time (eyecam::EyeSight::Weak)
    const char* setupWaitButton;  ///< ...the button ("calib wear")
    const char* setupWaitHint1;  ///< ...beside it
    const char* setupWaitHint2;
    const char* setupWaitFoot;  ///< ...under it (also under a failed one's buttons while the video isn't there)
    const char* setupWaitNoPupil;  ///< ...instead, while an eye's pupil isn't found: put the headset on again
    const char* setupErrorEyes;  ///< (3) failed: before the eyes eyecam-rec's message names
    const char* setupLearnPill;  ///< (3) calibrating: the pill
    const char* setupLearnWidenHint;  ///< ...beside the instruction to widen
    const char* setupChipClose;  ///< ...the steps as chips
    const char* setupChipNormal;
    const char* setupChipWiden;
    const char* setupChipSquint;  ///< ...the user calibration's
    const char* setupChipLookUp;
    const char* setupChipLookDown;
    const char* setupLeftBefore;  ///< ...the seconds left: before the number...
    const char* setupLeftAfter;  ///< ...and after it
    const char* setupLearnStepFormat;  ///< ..."Step %d of %d · %d s in all"
    const char* setupLearnFoot;  ///< ...under it
    const char* setupStop;  ///< ...stop
    const char* setupErrorPill;  ///< (3) failed: the pill
    const char* setupAgain;  ///< (3) failed or standard widening: once more
    const char* setupFailPill;  ///< (3) standard widening: the pill
    const char* setupFailTitle;
    const char* setupFailBody;
    const char* setupFailClosed;
    const char* setupFailNormal;
    const char* setupFailWiden;
    const char* setupFailWidenValue;
    const char* setupFailProceed;  ///< ...on with them
    const char* setupFailLater;
    const char* setupDoneTitle;  ///< done: the card
    const char* setupDoneBody;
    const char* setupDoneHelp;  ///< ...what to do if something is off
    const char* setupDoneHelp1;
    const char* setupDoneHelp1Do;
    const char* setupDoneHelp2;
    const char* setupDoneHelp2Do;
    const char* setupDoneButton;
    const char* setupDoneNote;
    const char* calibDonePill;  ///< a calibration from the usual page ended well: the card's pill
    const char* calibDoneTitle;  ///< ...this wear's
    const char* calibDoneBody;
    const char* calibUserDoneTitle;  ///< ...the user's
    const char* calibUserDoneBody;
    const char* calibDoneButton;  ///< ...back to the page
    const char* partialPill;  ///< "calib wear" went through without one eye (calib_failed_eye L / R): the pill
    const char* partialEyeLeft;  ///< ...the eye in its title (a name that starts a sentence)
    const char* partialEyeRight;
    const char* partialTitlePrevFormat;  ///< ..."%s" (the eye) uses its earlier values
    const char* partialTitleProvFormat;  ///< ...or provisional ones (there were no earlier ones)
    const char* partialSetupBody;  ///< ...the setup's: set up, but calibrate again
    const char* partialPageBody;  ///< ...from the usual page
    const char* partialNew;  ///< ...rows: the eye that went through
    const char* partialPrev;  ///< ...the other one: its earlier values
    const char* partialProv;  ///< ...or provisional ones
    const char* nextTitle;  ///< the left column while not set up: its card
    const char* nextPass;  ///< ...what (1) is about
    const char* nextWait;  ///< ...(3) before its button
    const char* nextLearnFormat;  ///< ...(3) calibrating ("%d s left")
    const char* nextFail;  ///< ...(3) standard widening
    const char* nextError;  ///< ...(3) failed
    const char* readyTitle;  ///< the left column right after the setup (green)
    const char* readyNote;
    const char* lidsFromValve;  ///< by the left column's eyelid title: where they come from
    const char* lidsFromCamera;
    const char* lidsFromCameraLeft;
    const char* lidsFromCameraRight;
    const char* camRowState;  ///< the eye cameras page: rows (title, hint)
    const char* camRowStateHint;
    const char* camLearned;  ///< ...the baseline learned (green pill)
    const char* camRowLidsHint;
    const char* camRowCalib;
    const char* camRowCalibHint;
    const char* camCalibButton;
    const char* camCalibSide1;
    const char* camCalibSide2;
    const char* camRowUser;
    const char* camRowUserHint;
    const char* camUserButton;
    const char* camUserSide1;
    const char* camUserSide2;
    const char* camUserNeedsCalib;  ///< ...instead while it can't be pressed
    // The sentence at the bottom of the eye cameras' page (cameraLine)
    const char* camLineBothVrchat;
    const char* camLineBoth;
    const char* camLineLeft;
    const char* camLineRight;
    const char* camLineWarmingFormat;
    const char* camLineWarming;
    const char* camLinePutOn;
    const char* camLineOff;
    const char* camHelpTitle;  ///< the "when..." box
    const char* camHelp1;
    const char* camHelp1Do;
    const char* camHelp2;
    const char* camHelp2Do;
    const char* camHelp3;
    const char* camHelp3Do;
    const char* lidsCamButton;
    const char* lidsCamMarks;  ///< ...the folded marks' line
    const char* lidsNowLabel;  ///< the Eyelids tab (案E): the band at its top, "now"
    const char* lidsNowBoth;  ///< ...from both cameras
    const char* lidsNowLeft;  ///< ...the left camera, Valve for the right
    const char* lidsNowRight;  ///< ...the right camera, Valve for the left
    const char* lidsNowValve;  ///< ...Valve's values
    const char* lidsNowSwitch;  ///< ...at its right: where to switch
    const char* lidsNowNoCamera;  ///< ...no eye cameras set up
    const char* rowWidenEase;  ///< the one widening slider
    const char* widenHintCamera;  ///< ...its hint: the cameras
    const char* widenHintMixed;  ///< ...one eye on the cameras
    const char* widenHintValve;  ///< ...Valve's values
    const char* widenHintSaturated;  ///< ...nothing to drive
    const char* widenNoteCamera;  ///< ...the line under it: the cameras
    const char* widenNoteValve;  ///< ...Valve's values (four stops)
    const char* widenNoteUnfitted;  ///< ...an eye without an eye fit
    const char* widenSaturated1;  ///< ...on a SteamOS that caps openness: the box
    const char* widenSaturated2;
    const char* blinkHint;  ///< the blink row: its hint
    const char* blinkHoldCaption;  ///< ...beside the hold
    const char* rowBlinkBoth;  ///< blink_sync_below
    const char* blinkBothHint;  ///< ...its hint
    const char* blinkBothNote;  ///< ...beside it
    const char* lidSmoothHint;  ///< the eyelid smoothing presets: hint
    const char* syncOff;  ///< the lid sync slider: its left end
    const char* syncStrong;  ///< ...its right end
    const char* syncHint;  ///< ...the row hint
    const char* rowOther;  ///< the row that opens Fine-tune
    const char* otherHint;  ///< ...its hint
    const char* otherText1;  ///< ...beside its button
    const char* otherText2;
    const char* detailsTitle;  ///< Fine-tune open: its title
    const char* detailsClose;  ///< ...the button back
    const char* camPupilLine;  ///< the eye cameras page: under "Now" while the pupils go straight to VRChat
    const char* camWidenNotice;  ///< ...the pointer to the Eyelids tab
    const char* camWidenButton;  ///< ...its button
    const char* calibCardTitle;  ///< a calibration from the page: the card title
    const char* calibResultTitle;  ///< ...how it ended: the card title
    const char* calibWearNote;  ///< ...at the right of the heading (this wear)
    const char* lidsCamMarksOpen;  ///< ...the same with the marks open  ///< ...at the bottom of the tab
    const char* devTitle;  ///< Advanced tab: the developer section (eye recording)
    const char* devRecord;  ///< ...its row
    const char* devRecordHint;
    const char* rowPrefix;
    const char* prefixNone;
    const char* prefixExample;      ///< "e.g." before an OSC address
    const char* prefixOther;        ///< "Now: %s"
    const char* rowConfigPath;
    const char* configPathMismatch; ///< frameeyeosc reads another config file
    const char* rowCalibrationPath;
    const char* rowStatusPath;
    const char* rowLockedList;
    const char* hintLockedList;
    const char* noneLocked;
    const char* rowCore;
    const char* coreFormat;         ///< "PID %d, up %s"
    const char* notRunning;
    const char* hoursMinutesFormat; ///< "%d h %d min"
    const char* minutesFormat;      ///< "%d min"

    // Recommendation prompt
    const char* promptVrchat;
    const char* promptEtvr;
    const char* promptVrchatDetail1;
    const char* promptVrchatDetail2;
    const char* promptEtvrDetail1;
    const char* promptEtvrDetail2;
    const char* promptLivelink;
    const char* promptLivelinkDetail1;
    const char* promptLivelinkDetail2;
    const char* promptYes;
    const char* promptNo;

    // Updates. The texts are vendor/frame-updater/strings.md word for word (same key names); rowVersion and
    // checkedFormat are this panel's own (the label of the version row on the Advanced tab)
    const char* rowVersion;
    const char* checkedFormat;          ///< after the version under the row label: "・確認 %s" (time or date)
    const char* rowGazeDots;         ///< the debug gaze dots switch
    const char* hintGazeDots;
    const char* dotDistance;         ///< next to its stepper ("Dot distance")
    const char* rowUpdateCheck;
    const char* hintUpdateCheck;
    const char* updateUpToDateFormat;   ///< "Up to date (%s)"
    const char* updateChecking;
    const char* updateAvailableFormat;  ///< "Version %s is available"
    const char* updateButton;
    const char* updateManual;           ///< the release can't be installed from the panel
    const char* updateConfirmFormat;    ///< "Update to %s?"
    const char* updateConfirmHint;
    const char* updateConfirmYes;
    const char* updateConfirmNo;
    const char* updateInstallingFormat; ///< "Updating: %s" (a step below)
    const char* updateInstalledFormat;  ///< "%s is installed. Reopen to use it"
    const char* updateInstallFailed;    ///< followed by the reason
    const char* updateCheckFailed;      ///< followed by the reason
    const char* updateCheckNow;
    const char* updateRetry;
    const char* updateDismiss;
    const char* updateLogHint;
    // Install steps (UpdateStatus::step)
    const char* stepStart;
    const char* stepDownload;
    const char* stepVerify;
    const char* stepExtract;
    const char* stepInstall;
    // Why a check or an install failed (UpdateStatus::error)
    const char* reasonNetwork;
    const char* reasonRateLimited;
    const char* reasonNotFound;
    const char* reasonBadResponse;
    const char* reasonBadVersion;
    const char* reasonBadUrl;
    const char* reasonMissingTool;
    const char* reasonNoChecksums;
    const char* reasonNoAsset;
    const char* reasonChecksumMismatch;
    const char* reasonUnsafeArchive;
    const char* reasonNoInstaller;
    const char* reasonInstallFailed;
    const char* reasonBadArgs;
    const char* reasonBusy;
    const char* reasonNotNewer;
    const char* reasonDetachFailed;
    const char* reasonInterrupted;
    const char* reasonIo;
    const char* reasonUpdater;          ///< usage, script-failed, spawn-failed
    const char* reasonOther;            ///< any other code
};

/**
 * The text table of a language.
 * @param language the language
 * @return the table (valid for the whole program)
 */
const UiText& uiText(Language language);

/**
 * The text of an install step.
 * @param t the text table
 * @param step UpdateStatus::step ("download" and so on)
 * @return the text
 */
const char* updateStepText(const UiText& t, const std::string& step);

/**
 * The text of an update error code.
 * @param t the text table
 * @param code UpdateStatus::error ("network" and so on); unknown codes get a general text
 * @return the text
 */
const char* updateReasonText(const UiText& t, const std::string& code);

/**
 * The language name written in config.json.
 * @param language the language
 * @return "ja" / "en"
 */
const char* languageCode(Language language);

/**
 * Read a language name from config.json.
 * @param code "ja" / "en"
 * @param language where to write it
 * @return true if the name is known
 */
bool parseLanguage(const std::string& code, Language& language);
