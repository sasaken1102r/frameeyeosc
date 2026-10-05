// Tests for the diagnostics page's texts and code (diag.{h,cpp}): SHA-256 and the tool's cached checksum, SteamOS's
// version from os-release, the diagnostic code for each state, the cards' rows in both languages, and reading the
// fields they come from (eyecam-rec's search_detail / last_calib / last_error / prox_min, frameeyeosc's last_error).
// Built as diag-test; exits non-zero on failure.
#include "diag.h"
#include "eyecam.h"
#include "i18n.h"
#include "model.h"
#include "status.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

/**
 * Record a failed check.
 * @param ok the check
 * @param what what was checked
 * @param line where
 */
void check(bool ok, const char* what, int line) {
    if (ok) return;
    ++gFailures;
    std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
}

#define CHECK(condition) check((condition), #condition, __LINE__)

/**
 * Check two strings are the same, printing both when not.
 * @param got what came out
 * @param want what should have
 * @param line where
 */
void same(const std::string& got, const std::string& want, int line) {
    if (got == want) return;
    ++gFailures;
    std::fprintf(stderr, "FAILED line %d:\n  got  \"%s\"\n  want \"%s\"\n", line, got.c_str(), want.c_str());
}

#define SAME(got, want) same((got), (want), __LINE__)

/**
 * A model as on the developer's headset with everything working: frameeyeosc sending to VRChat with both cameras,
 * eyecam-rec idle with the video locked after a look that found the ring, the last calibration fine.
 * @return the model
 */
PanelModel healthy() {
    PanelModel m;
    m.config.exists = true;
    m.config.root.type = JsonValue::Type::Object;
    m.update.current = "0.7.3";
    m.system.steamos = "0.4.3 (20260930.6234839)";
    m.system.grabHash = "294d06c6";
    EyeStatus& s = m.status;
    s.present = s.running = true;
    s.sending = true;
    s.tracking = true;
    s.output = "vrchat";
    s.target = "192.168.0.60:9000";
    s.rate = 90;
    s.trackerRate = 90;
    s.missedRate = 0;
    s.droppedRate = 0;
    s.opennessSaturated = true;
    s.camera.known = s.camera.present = true;
    s.camera.used[0] = s.camera.used[1] = true;
    eyecam::Status& e = m.eyecam.status;
    e.present = true;
    e.state = eyecam::State::Idle;
    e.stateText = "idle";
    e.locked = true;
    e.live = true;
    e.liveMs = 1.6;
    e.hasBuffers = true;
    e.autoGrab = "ok";
    e.prox = 31.2;
    e.proxMin = 20;
    e.hasSearch = true;
    e.hasPupil = true;
    e.pupil[0] = 0.87;
    e.pupil[1] = 0.84;
    e.searchDetail = {true, true, 8, 90.0, 8, true, "", 128};
    eyecam::LastCalib& c = e.lastCalib;
    c.known = c.present = true;
    c.time = "2026-10-05 19:51:03";
    c.ok = true;
    c.message = "校正できた（かぶり）";
    c.messageEn = "Calibrated";
    for (int eye = 0; eye < 2; ++eye) {
        c.pupilFrames[eye] = 470 + eye * 12;
        c.normalFrames[eye] = 486;
        c.pupilX[eye] = 238 + eye * 14;
        c.pupilY[eye] = 201 - eye * 7;
        c.window[eye][0] = 186 - eye * 6;
        c.window[eye][1] = 346 - eye * 6;
    }
    m.eyecam.visible = true;
    return m;
}

/**
 * The healthy model stuck: the headset on, but the proximity sensor reads 2.9, nothing was written in the buffers
 * before the last look, and the right eye kept its previous values at the last calibration.
 * @return the model
 */
PanelModel stuck() {
    PanelModel m = healthy();
    eyecam::Status& e = m.eyecam.status;
    e.locked = false;
    e.search = "not_worn";
    e.prox = 2.9;
    e.pupil[0] = e.pupil[1] = NAN;
    e.searchDetail = {true, true, 0, 0.0, 0, false, "no_candidates", 0};
    e.lastCalib.failedEye = "R";
    e.lastCalib.pupilFrames[1] = 0;
    e.lastCalib.pupilX[1] = e.lastCalib.pupilY[1] = NAN;
    return m;
}

/**
 * A card's row by its label.
 * @param cards the cards
 * @param label the label
 * @return the row (an empty one if there is none)
 */
diag::Row rowOf(const std::vector<diag::Card>& cards, const std::string& label) {
    for (const diag::Card& card : cards) {
        for (const diag::Row& row : card.rows) {
            if (row.label == label) return row;
        }
    }
    std::fprintf(stderr, "no row \"%s\"\n", label.c_str());
    return {};
}

void testSha256() {
    SAME(diag::sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    SAME(diag::sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 bytes: the padding needs a second block
    SAME(diag::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    SAME(diag::sha256Hex("The quick brown fox jumps over the lazy dog"),
         "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    SAME(diag::sha256Hex(std::string(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void testFileHash() {
    char path[] = "/tmp/diag-test-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) return;
    close(fd);
    {
        std::ofstream file(path, std::ios::binary);
        file << "abc";
    }
    diag::FileHash hash;
    SAME(hash.get(path), "ba7816bf");
    SAME(hash.get(path), "ba7816bf");
    // Rewritten (another size): worked out again
    {
        std::ofstream file(path, std::ios::binary);
        file << "The quick brown fox jumps over the lazy dog";
    }
    SAME(hash.get(path), "d7a8fbb3");
    CHECK(!hash.unreadable());
    // There but not readable (permissions): "", and unreadable() says so; readable again (a chmod: the same size and
    // modification time), it is worked out again (root reads it anyway)
    if (geteuid() != 0) {
        CHECK(chmod(path, 0) == 0);
        SAME(hash.get(path), "");
        CHECK(hash.unreadable());
        CHECK(chmod(path, 0600) == 0);
        SAME(hash.get(path), "d7a8fbb3");
        CHECK(!hash.unreadable());
        CHECK(chmod(path, 0) == 0);
        SAME(hash.get(path), "");
        CHECK(hash.unreadable());
    }
    std::remove(path);
    SAME(hash.get(path), "");
    CHECK(!hash.unreadable());
    SAME(hash.get("/nonexistent/eyecam-grab"), "");
}

void testSteamos() {
    SAME(diag::steamosVersion("NAME=\"SteamOS\"\nBUILD_ID=20260930.6234839\nVARIANT_ID=\"vr\"\nVERSION_ID=0.4.3\n"),
         "0.4.3 (20260930.6234839)");
    SAME(diag::steamosVersion("VERSION_ID=\"3.7.13\"\r\n"), "3.7.13");
    SAME(diag::steamosVersion("BUILD_ID=1\n"), "");
    SAME(diag::steamosVersion(""), "");
    SAME(diag::readSteamos("/nonexistent/os-release"), "");
}

void testCode() {
    SAME(diag::code(healthy()), "OK·P31·B128·G1·C1·F1");
    SAME(diag::code(stuck()), "NW·P3·B0·G1·C1R·F1");
    {
        // The video not there although worn; one eye only; searching before the first look (an older eyecam-rec)
        PanelModel m = stuck();
        m.eyecam.status.search = "no_video";
        m.eyecam.status.prox = 30.6;
        SAME(diag::code(m), "NV·P31·B0·G1·C1R·F1");
        m.eyecam.status.search = "one_eye";
        m.eyecam.status.searchDetail = {true, true, 4, 90.0, 4, false, "one_eye", 64};
        SAME(diag::code(m), "OE·P31·B64·G1·C1R·F1");
        m.eyecam.status.search.clear();
        m.eyecam.status.searchDetail = {};
        m.eyecam.status.prox = -1;
        SAME(diag::code(m), "SR·P-·B-·G1·C1R·F1");
        m.eyecam.status.live = false;
        SAME(diag::code(m), "LO·P-·B-·G1·C1R·F1");
    }
    {
        // A failed calibration, and none yet
        PanelModel m = healthy();
        m.eyecam.status.lastCalib.ok = false;
        m.eyecam.status.lastCalib.failedEye = "LR";
        SAME(diag::code(m), "OK·P31·B128·G1·C0·F1");
        m.eyecam.status.lastCalib.present = false;
        SAME(diag::code(m), "OK·P31·B128·G1·C-·F1");
    }
    {
        // Waiting for the tool; eyecam-rec not running at all
        PanelModel m = healthy();
        eyecam::Status& e = m.eyecam.status;
        e.state = eyecam::State::WaitingFds;
        e.locked = false;
        e.hasBuffers = false;
        e.autoGrab = "waiting_tracker";
        SAME(diag::code(m), "WF·P31·B128·G0·C1·F1");
        m.eyecam.visible = false;
        SAME(diag::code(m), "NR·P-·B-·G0·C-·F1");
    }
    {
        // frameeyeosc: waiting for eye data, no target, paused, an error, not running
        PanelModel m = healthy();
        m.status.tracking = false;
        SAME(diag::code(m), "OK·P31·B128·G1·C1·FW");
        m.status.target.clear();
        SAME(diag::code(m), "OK·P31·B128·G1·C1·FT");
        m.status.sending = false;
        SAME(diag::code(m), "OK·P31·B128·G1·C1·FP");
        m.status.sourceError = "unsupported eye shared-memory version 6; supported: 4, 5";
        SAME(diag::code(m), "OK·P31·B128·G1·C1·FE");
        m.status.running = false;
        SAME(diag::code(m), "OK·P31·B128·G1·C1·F0");
    }
}

void testCards() {
    const UiText& ja = uiText(Language::Ja);
    const UiText& en = uiText(Language::En);
    {
        const std::vector<diag::Card> cards = diag::cards(ja, healthy());
        CHECK(cards.size() == 4);
        SAME(cards[0].title, "版");
        SAME(rowOf(cards, "frameeyeosc").value, "0.7.3");
        SAME(rowOf(cards, "SteamOS").value, "0.4.3 (20260930.6234839)");
        SAME(rowOf(cards, ja.diagRowTool).value, "ok · 294d06c6");
        SAME(rowOf(cards, ja.diagRowOutput).value, "VRChat に直接 · 自動");
        SAME(rowOf(cards, ja.diagRowGaze).value, "90 回/秒 · 取りこぼし 0");
        SAME(rowOf(cards, ja.diagRowSend).value, "90 回/秒 · 捨てた 0");
        SAME(rowOf(cards, ja.diagRowCap).value, "あり（0.4.3）");
        SAME(rowOf(cards, ja.diagRowLids).value, "両目カメラ");
        SAME(rowOf(cards, ja.diagRowCoreError).value, "なし");
        SAME(rowOf(cards, ja.diagRowState).value, "映像あり（8 スロット）");
        SAME(rowOf(cards, ja.diagRowSearch).value, "候補 8 · 毎秒 90 · 8 スロット · 両目");
        SAME(rowOf(cards, ja.diagRowBlocks).value, "128");
        SAME(rowOf(cards, ja.diagRowProx).value, "31.2 / 20");
        SAME(rowOf(cards, ja.diagRowPupil).value, "左 87% · 右 84%");
        SAME(rowOf(cards, ja.diagRowLoad).value, "1.6 ms/枚");
        SAME(rowOf(cards, ja.diagRowWhen).value, "10/05 19:51");
        SAME(rowOf(cards, ja.diagRowResult).value, "OK");
        SAME(rowOf(cards, ja.diagRowPupilFrames).value, "左 470/486 · 右 482/486");
        SAME(rowOf(cards, ja.diagRowPupilAt).value, "左 (238, 201) · 右 (252, 194)");
        SAME(rowOf(cards, ja.diagRowWindow).value, "左 186–346 · 右 180–340");
        SAME(rowOf(cards, ja.diagRowLastError).value, "なし");
        for (const diag::Card& card : cards) {
            for (const diag::Row& row : card.rows) CHECK(!row.bad);
        }
    }
    {
        // Stuck, in English
        const std::vector<diag::Card> cards = diag::cards(en, stuck());
        const diag::Row state = rowOf(cards, en.diagRowState);
        SAME(state.value, "not worn (sensor)");
        CHECK(state.bad);
        SAME(rowOf(cards, en.diagRowSearch).value, "0 candidates · stopped (no candidates)");
        // (nothing written in the buffers: in red)
        const diag::Row blocks = rowOf(cards, en.diagRowBlocks);
        SAME(blocks.value, "0");
        CHECK(blocks.bad);
        SAME(rowOf(cards, en.diagRowProx).value, "2.9 / 20");
        SAME(rowOf(cards, en.diagRowPupil).value, "L — · R —");
        SAME(rowOf(cards, en.diagRowResult).value, "right eye: previous values");
        SAME(rowOf(cards, en.diagRowPupilFrames).value, "L 470/486 · R 0/486");
        SAME(rowOf(cards, en.diagRowPupilAt).value, "L (238, 201) · R —");
        SAME(rowOf(cards, en.diagRowWindow).value, "L 186–346 · R 180–340");
    }
    {
        // A failed calibration (eyecam-rec's reason in the panel's language), eyecam-rec's and frameeyeosc's last errors
        PanelModel m = healthy();
        eyecam::Status& e = m.eyecam.status;
        e.lastCalib.ok = false;
        e.lastCalib.failedEye = "LR";
        e.lastCalib.message = "両目の瞳がうまく見えなかった（HMD のかぶり方を直して、もう一度）[左 12/486・右 30/486、90 必要]";
        e.lastCalib.messageEn = "Both eyes: couldn't see the pupil well (adjust the headset and try again) "
                                "[L 12/486, R 30/486, 90 needed]";
        e.lastError = e.lastCalib.message;
        e.lastErrorEn = e.lastCalib.messageEn;
        e.lastErrorUnix = 1791200000;
        m.status.lastError = "No Steam Link connection found; waiting for one";
        m.status.lastErrorTime = 1791200000;
        const std::vector<diag::Card> jaCards = diag::cards(ja, m);
        const diag::Row result = rowOf(jaCards, ja.diagRowResult);
        CHECK(result.bad);
        CHECK(result.value.rfind("失敗: 両目の瞳がうまく見えなかった", 0) == 0);
        m.language = Language::En;
        const std::vector<diag::Card> enCards = diag::cards(en, m);
        CHECK(rowOf(enCards, en.diagRowResult).value.rfind("failed: Both eyes: couldn't see the pupil well", 0) == 0);
        // (the local time it happened first; the failed calibration's message is in the result already)
        const std::string last = rowOf(enCards, en.diagRowLastError).value;
        CHECK(last.size() > 14 && last.substr(2, 1) == "/" && last.find(" · same as the result") == 11);
        const diag::Row core = rowOf(enCards, en.diagRowCoreError);
        CHECK(core.value.find(" · No Steam Link connection found; waiting for one") == 11 && !core.bad);
        // Another error than the calibration's: as it is
        m.eyecam.status.lastErrorEn = "The right camera's video hasn't come for 3 s";
        m.eyecam.status.lastError = "右のカメラの映像が 3 秒届きません";
        CHECK(rowOf(diag::cards(en, m), en.diagRowLastError).value.find(" · The right camera's video") == 11);
        // What is wrong now comes before the last error, in red
        m.status.target.clear();
        const diag::Row noTarget = rowOf(diag::cards(en, m), en.diagRowCoreError);
        SAME(noTarget.value, "no target found");
        CHECK(noTarget.bad);
        m.status.running = false;
        const std::vector<diag::Card> down = diag::cards(ja, m);
        SAME(rowOf(down, ja.diagRowCoreError).value, "本体が動いていない");
        SAME(rowOf(down, ja.diagRowGaze).value, "—");
    }
    {
        // No eyecam-rec: its rows say so, nothing made up
        PanelModel m = healthy();
        m.eyecam.visible = false;
        const std::vector<diag::Card> cards = diag::cards(ja, m);
        SAME(rowOf(cards, ja.diagRowState).value, "eyecam-rec が動いていない");
        SAME(rowOf(cards, ja.diagRowSearch).value, "—");
        SAME(rowOf(cards, ja.diagRowWhen).value, "—");
        SAME(rowOf(cards, ja.diagRowTool).value, "— · 294d06c6");
        // The tool not installed where install.sh puts it, or not readable
        m.system.grabHash.clear();
        SAME(rowOf(diag::cards(en, m), en.diagRowTool).value, "— · no file");
        m.system.grabUnreadable = true;
        SAME(rowOf(diag::cards(en, m), en.diagRowTool).value, "— · unreadable");
        SAME(rowOf(diag::cards(ja, m), ja.diagRowTool).value, "— · 読めない");
    }
    {
        // The installed copy eyecam-rec runs: its checksum when it can be read, install.sh's beside it when that differs
        PanelModel m = healthy();
        m.system.installedHash = "294d06c6";
        SAME(rowOf(diag::cards(ja, m), ja.diagRowTool).value, "ok · 294d06c6");
        m.system.installedHash = "1a2b3c4d";
        SAME(rowOf(diag::cards(ja, m), ja.diagRowTool).value, "ok · 1a2b3c4d（同梱 294d06c6）");
        SAME(rowOf(diag::cards(en, m), en.diagRowTool).value, "ok · 1a2b3c4d (bundled 294d06c6)");
        m.system.grabHash.clear();
        m.system.grabUnreadable = true;
        SAME(rowOf(diag::cards(en, m), en.diagRowTool).value, "ok · 1a2b3c4d");
    }
    {
        // Candidates in separate buffers: where it stopped, without a refresh rate (it didn't measure one)
        PanelModel m = stuck();
        m.eyecam.status.searchDetail = {true, true, 3, 0.0, 0, false, "split_buffers", 12};
        SAME(rowOf(diag::cards(ja, m), ja.diagRowSearch).value, "候補 3 · 止まった（候補が別々のバッファ）");
        SAME(rowOf(diag::cards(en, m), en.diagRowSearch).value, "3 candidates · stopped (candidates in separate buffers)");
    }
    {
        // An older eyecam-rec (no search_detail, no last_calib) against one that hasn't looked or calibrated yet
        PanelModel m = healthy();
        m.eyecam.status.searchDetail = {};
        m.eyecam.status.lastCalib = {};
        SAME(rowOf(diag::cards(ja, m), ja.diagRowSearch).value, "—");
        SAME(rowOf(diag::cards(ja, m), ja.diagRowWhen).value, "—");
        m.eyecam.status.searchDetail.known = m.eyecam.status.lastCalib.known = true;
        SAME(rowOf(diag::cards(ja, m), ja.diagRowSearch).value, "まだ見てない");
        SAME(rowOf(diag::cards(ja, m), ja.diagRowWhen).value, "まだ");
        // Rates never show "-0"
        m.status.missedRate = -0.0;
        SAME(rowOf(diag::cards(en, m), en.diagRowGaze).value, "90/s · missed 0");
    }
    {
        // The signature follows what is shown
        const PanelModel a = healthy();
        PanelModel b = healthy();
        CHECK(diag::signature(ja, a) == diag::signature(ja, b));
        b.eyecam.status.prox = 30.0;
        CHECK(diag::signature(ja, a) != diag::signature(ja, b));
    }
}

void testTexts() {
    // Every text of the page, in both languages
    const char* UiText::*const fields[] = {
        &UiText::diagButton, &UiText::diagTitle, &UiText::diagSub, &UiText::diagCodeLabel, &UiText::diagBack,
        &UiText::diagCardVersions, &UiText::diagCardEyeData, &UiText::diagCardCameras, &UiText::diagCardCalib,
        &UiText::diagRowTool, &UiText::diagRowOutput, &UiText::diagRowGaze, &UiText::diagRowSend, &UiText::diagRowCap,
        &UiText::diagRowLids, &UiText::diagRowCoreError, &UiText::diagRowState, &UiText::diagRowSearch,
        &UiText::diagRowProx, &UiText::diagRowPupil, &UiText::diagRowLoad, &UiText::diagRowWhen, &UiText::diagRowResult,
        &UiText::diagRowPupilFrames, &UiText::diagRowPupilAt, &UiText::diagRowWindow, &UiText::diagRowBlocks,
        &UiText::diagRowLastError, &UiText::diagSameAsResult, &UiText::diagToolOutdated,
        &UiText::diagToolMissing, &UiText::diagToolBundledFormat, &UiText::diagModeAuto, &UiText::diagModeFixed,
        &UiText::diagRateFormat,
        &UiText::diagMissedFormat, &UiText::diagDroppedFormat, &UiText::diagPaused, &UiText::diagCapOn,
        &UiText::diagCapOff, &UiText::diagLidsBoth, &UiText::diagLidsLeft, &UiText::diagLidsRight,
        &UiText::diagLidsValve, &UiText::diagNone, &UiText::diagCoreNotRunning, &UiText::diagCoreNoTarget,
        &UiText::diagWithTimeFormat, &UiText::diagVideoSlotsFormat, &UiText::diagVideo, &UiText::diagNotWorn,
        &UiText::diagNoVideo, &UiText::diagOneEyeOnly, &UiText::diagSearching, &UiText::diagLiveOff,
        &UiText::diagWaitingTool, &UiText::diagNoRecorder, &UiText::diagError, &UiText::diagCandidatesFormat,
        &UiText::diagHzFormat, &UiText::diagSlotsFormat, &UiText::diagBothEyes, &UiText::diagOneEye,
        &UiText::diagStopNoCandidates, &UiText::diagStopSplitBuffers, &UiText::diagStopNotRefreshing,
        &UiText::diagStopFewSlots,
        &UiText::diagChangedFormat, &UiText::diagNotLooked, &UiText::diagUnreadable, &UiText::diagEyesFormat,
        &UiText::diagMsFormat, &UiText::diagOk, &UiText::diagFailedFormat, &UiText::diagPreviousLeft,
        &UiText::diagPreviousRight, &UiText::diagNoCalib};
    CHECK(std::size(fields) == 77);
    for (const Language language : {Language::Ja, Language::En}) {
        const UiText& t = uiText(language);
        for (const char* UiText::*field : fields) CHECK(t.*field != nullptr && (t.*field)[0] != '\0');
    }
}

void testParse() {
    // eyecam-rec's line, as its Status::to_json writes it
    const std::string line =
        "{\"version\":1,\"state\":\"idle\",\"message\":\"\",\"message_en\":\"\",\"has_buffers\":true,\"auto_grab\":\"ok\","
        "\"locked\":false,\"prox\":2.900,\"search\":\"not_worn\",\"pupil_l\":null,\"pupil_r\":null,\"prox_min\":20.000,"
        "\"search_detail\":{\"candidates\":4,\"refresh_hz\":65.000,\"slots\":4,\"both_eyes\":false,"
        "\"stopped_at\":\"one_eye\",\"changed_blocks\":64,\"unix\":1791200000.000},"
        "\"last_calib\":{\"time\":\"2026-10-05 19:51:03\",\"ok\":true,\"failed_eye\":\"R\",\"message\":\"校正できた\","
        "\"message_en\":\"Calibrated\",\"pupil_frames\":[486.000,0.000],\"normal_frames\":[486.000,486.000],"
        "\"pupil_x\":[240.500,null],\"pupil_y\":[201.000,null],\"window\":[[186.000,346.000],[180.000,340.000]]},"
        "\"last_error\":\"右目を閉じたのが検出できなかった\",\"last_error_en\":\"Right eye: couldn't detect the eye closing\","
        "\"last_error_unix\":1791200001.500,\"pid\":7,\"updated_unix\":1791200002.000}";
    const eyecam::Status s = eyecam::parseStatus(line, 1791200002.0);
    CHECK(s.present);
    CHECK(s.proxMin == 20.0);
    const eyecam::SearchDetail& d = s.searchDetail;
    CHECK(d.known && d.present && d.candidates == 4 && d.refreshHz == 65.0 && d.slots == 4 && !d.bothEyes);
    SAME(d.stoppedAt, "one_eye");
    CHECK(d.changedBlocks == 64);
    const eyecam::LastCalib& c = s.lastCalib;
    CHECK(c.present && c.ok);
    SAME(c.time, "2026-10-05 19:51:03");
    SAME(c.failedEye, "R");
    SAME(c.messageEn, "Calibrated");
    CHECK(c.pupilFrames[0] == 486 && c.pupilFrames[1] == 0 && c.normalFrames[1] == 486);
    CHECK(c.pupilX[0] == 240.5 && std::isnan(c.pupilX[1]) && c.pupilY[0] == 201 && std::isnan(c.pupilY[1]));
    CHECK(c.window[0][0] == 186 && c.window[0][1] == 346 && c.window[1][0] == 180 && c.window[1][1] == 340);
    SAME(s.lastErrorEn, "Right eye: couldn't detect the eye closing");
    CHECK(s.lastErrorUnix == 1791200001.5);
    // An older eyecam-rec (or none yet): not there
    const eyecam::Status old = eyecam::parseStatus("{\"state\":\"idle\",\"search_detail\":null,\"last_calib\":null}", 0);
    CHECK(old.present && !old.searchDetail.present && !old.lastCalib.present && std::isnan(old.proxMin));
    CHECK(old.searchDetail.known && old.lastCalib.known);
    const eyecam::Status older = eyecam::parseStatus("{\"state\":\"idle\"}", 0);
    CHECK(!older.searchDetail.known && !older.lastCalib.known);
    CHECK(old.lastError.empty() && old.lastErrorUnix == 0);

    // frameeyeosc's last_error
    const EyeStatus with = parseStatus(
        "{\"version\":1,\"pid\":1,\"time\":1791200000,\"started\":1791199000,\"sending\":true,\"output\":\"vrchat\","
        "\"last_error\":{\"text\":\"No Steam Link connection found; waiting for one\",\"time\":1791199500.25}}",
        1791200000, false);
    SAME(with.lastError, "No Steam Link connection found; waiting for one");
    CHECK(with.lastErrorTime == 1791199500.25);
    const EyeStatus without =
        parseStatus("{\"version\":1,\"pid\":1,\"time\":1791200000,\"last_error\":null}", 1791200000, false);
    CHECK(without.lastError.empty() && without.lastErrorTime == 0);
}

}  // namespace

int main() {
    testSha256();
    testFileHash();
    testSteamos();
    testCode();
    testCards();
    testTexts();
    testParse();
    if (gFailures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("diag-test: all checks passed\n");
    return 0;
}
