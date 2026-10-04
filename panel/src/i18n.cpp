// The text tables.
#include "i18n.h"

#include <cstdlib>
#include <fstream>

namespace {

/**
 * Build the Japanese table. Short words; technical terms are replaced with everyday ones
 * (One Euro -> "なめらかさ", deadzone -> "見つめている時の遊び").
 * @return the table
 */
UiText makeJapanese() {
    UiText t {};
    t.title = "目の送信";
    t.badgeSending = "送信中";
    t.badgeWaiting = "目のデータ待ち";
    t.badgePaused = "一時停止中";
    t.badgeNotRunning = "本体が動いていません";
    t.notRunningHint1 = "設定の変更は保存され、";
    t.notRunningHint2 = "本体が起動すると反映されます";
    t.destination = "送り先";
    t.outputVrchatShort = "VRChat";
    t.outputEtvrShort = "VRCFT（ETVR）";
    t.outputLivelinkShort = "VRCFT（LiveLink）";
    t.searchingPc = "PC を探しています…";
    t.modeAuto = "自動（Steam Link の相手）";
    t.modeFixed = "固定";
    t.rateLabel = "送信の回数";
    t.rateFormat = "毎秒 %.0f 回";
    t.trackerRateLabel = "目のデータ";
    t.trackerRateLowHint = "少なめ";
    t.trackerRateSlowHere = "本体の処理が追いついていません";
    t.trackerRateSlowTracker = "Frame から届く数が少なめです";
    t.lidsTitle = "まぶた";
    t.legendRaw = "生の値";
    t.legendSent = "送った値";
    t.left = "左";
    t.right = "右";
    t.gazeTitle = "視線";
    t.leftEye = "左目";
    t.rightEye = "右目";
    t.noEyeData = "目のデータがありません";
    t.errorPrefix = "設定のエラー: ";
    t.sourceErrorPrefix = "目のデータを読めません: ";
    t.dominantEyeLeft = "Frame の設定: 左目だけで追っています";
    t.dominantEyeRight = "Frame の設定: 右目だけで追っています";
    t.errWrite = "設定を書けません: ";
    t.errConfigBroken = "設定ファイルが壊れています。「すべて既定に戻す」で作り直せます";
    t.errAutostart = "自動起動の切り替えに失敗（systemctl）";

    t.tabBasic = "基本";
    t.tabOutput = "送り方";
    t.tabGaze = "視線";
    t.tabGazeFit = "目を合わせる";
    t.tabLids = "まぶた";
    t.tabAdvanced = "詳細";
    t.tabEyecam = "目のカメラ";

    t.on = "オン";
    t.off = "オフ";
    t.locked = "コマンドで固定中";
    t.lowerSmoother = "低いほどなめらか";
    t.capStill = "止まっている時";
    t.capFast = "速い動き";
    t.capChange = "変化の感度";

    t.rowSending = "送信";
    t.hintSending = "止めても本体は動いたまま";
    t.send = "送る";
    t.stop = "止める";
    t.rowOutput = "送り先";
    t.hintOutput = "同期＝ほかの人からの見え方（△はアバターによる）";
    t.outputVrchat = "VRChat に直接";
    t.outputLivelink = "VRCFT（LiveLink）";
    t.outputEtvr = "VRCFT（ETVR）";
    t.outputRecommended = "おすすめ";
    t.outputMarks[0][0] = "見開き ◯";
    t.outputMarks[0][1] = "同期 △";
    t.outputMarks[0][2] = "VRCFT 不要";
    t.outputMarks[1][0] = "見開き ◯";
    t.outputMarks[1][1] = "同期 ◯";
    t.outputMarks[1][2] = "VRCFT 必要";
    t.outputMarks[2][0] = "見開き ×";
    t.outputMarks[2][1] = "同期 ◯";
    t.outputMarks[2][2] = "VRCFT 必要";
    t.rowActiveType = "EyeTrackingActive の型";
    t.hintActiveType = "アバターによっては Float が必要";
    t.activeOff = "送らない";
    t.rowSteamlink = "Steam Link の名前も送る";
    t.hintSteamlink = "Steam Link 向けのアバター用";
    t.steamlinkNoPrefix = "（頭はつけない）";
    t.rowNativeEyes = "VRChat 標準の目も動かす";
    t.hintNativeEyes = "VRCFT 用の値がないアバター用。まぶたが閉じすぎたらオフに";
    t.rowTarget = "送り先の PC";
    t.hintTarget = "自動 = Steam Link の相手";
    t.targetAuto = "自動";
    t.targetFixNow = "今の相手で固定";
    t.targetEnter = "IP を入力";
    t.targetManualFormat = "手動 %s";
    t.hostEntryTitle = "送り先の PC の IP アドレス";
    t.hostEntryHint = "例: 192.168.1.20（ポートは「ポート」の行で）";
    t.hostEntryOk = "決定";
    t.hostEntryCancel = "やめる";
    t.hostErrEmpty = "何も入っていません";
    t.hostErrIpv4 = "IP アドレスの形ではありません（0〜255 の数 4 つを . で区切る）";
    t.rowPort = "ポート";
    t.portDefaultVrchat = "VRChat の既定は 9000";
    t.portDefaultLivelink = "LiveLink の既定は 11111";
    t.portDefaultEtvr = "ETVR の既定は 8889";
    t.vrcftSetupTitle = "PC の VRCFT で準備すること";
    t.vrcftStepsLivelink[0] = "VRCFT の「Module Registry」から LiveLink を入れる";
    t.vrcftStepsLivelink[1] = "ほかの目のモジュール（ETVR など）はオフにする";
    t.vrcftStepsLivelink[2] = "Windows のファイアウォールで UDP 11111 を通す";
    t.vrcftStepsEtvr[0] = "VRCFT の「Module Registry」から ETVR Eye Tracking を入れる";
    t.vrcftStepsEtvr[1] = "ほかの目のモジュール（LiveLink など）はオフにする";
    t.vrcftStepsEtvr[2] = "Windows のファイアウォールで UDP 8889 を通す";
    t.vrcftSetupNote = "パラメーター名や同期のしかたは、VRCFT がアバターに合わせて決めます。";
    t.vrcftNoWide = "この形式では見開きは届きません。";
    t.portReset = "既定に戻す";
    t.rowLanguage = "言語";
    t.rowAutostart = "SteamVR と一緒に起動";
    t.hintAutostart = "次の SteamVR の起動から";
    t.autostartMissing = "準備されていません";
    t.autostartUnknown = "状態を読めません";
    t.resetAll = "すべて既定に戻す";
    t.resetConfirm = "もう一度押すと戻す";
    t.quit = "アプリを終了";
    t.quitConfirm = "もう一度押すと終了";
    t.footer = "押すとすぐ本体に反映され、再起動しても残ります。パネルを終了しても送信は続きます";

    t.rowSmoothing = "スムージング";
    t.hintSmoothing = "オフ = 生の値をそのまま";
    t.rowStrength = "なめらかさ";
    t.strengthLight = "弱";
    t.strengthMedium = "中";
    t.strengthStrong = "強";
    t.custom = "カスタム（下の値）";
    t.rawOnNote = "スムージングがオフです";
    t.rowFine = "細かく変える";
    t.rowDeadzone = "見つめている時の遊び";
    t.hintDeadzone = "これより小さい揺れは無視";
    t.rowHold = "まばたき中は視線を止める";
    t.hintHold = "この値より閉じたら止める";
    t.rowIndependent = "左右の目を別々に動かす";
    t.hintIndependent = "目を合わせてから使うと自然";
    t.hintIndependentOneEye = "片目だけ追跡中は両目が同じ向き";
    t.rowQuality = "不確かな視線を使わない";
    t.hintQuality = "この値より不確かな目は無視";
    t.rowDespike = "一瞬の途切れを消す";
    t.hintDespike = "視線とまぶた。約 11 ms 遅れる";

    t.rowFit = "目を合わせる";
    t.hintFit = "視線とまぶた・約 20 秒";
    t.fitStart = "目を合わせる";
    t.fitAgain = "もう一度合わせる";
    t.fitCenterOnly = "正面だけ合わせ直す";
    t.fitCenterTilt = "正面と傾きを合わせ直す";
    t.fitStop = "やめる";
    t.fitIntro = "ダッシュボードを閉じると始まります。頭は動かさず、点を目で追ってください。最後は 3・2・1 のあと 3 秒間目を閉じ、"
                 "チャイムで開けます";
    t.fitNeedsRunning = "frameeyeosc が動いているときに使えます";
    t.fitLocked = "視線かまぶたの値がコマンドで固定されているので使えません";
    t.fitWaiting = "ダッシュボードを閉じると始まります";
    t.fitHowTo = "頭は動かさず、点を目で追ってください。最後は 3・2・1 のあと 3 秒間目を閉じ、チャイムで開けます。"
                 "ダッシュボードを開くと止まります";
    t.fitWaitingCenter = "正面に点が出ます。頭は動かさず見てください。ダッシュボードを開くと止まります";
    t.fitWaitingTilt = "正面、上、下の順に点が出ます。頭は動かさず、点を目で追ってください。ダッシュボードを開くと止まります";
    t.fitRunningFormat = "測っています: %s（%d / %d）";
    t.fitRetryFormat = "・%d 回目";
    t.fitDone = "合わせました";
    t.fitDoneCenter = "正面を合わせ直しました";
    t.fitDoneTilt = "正面と傾きを合わせ直しました";
    t.fitFitted = "合わせてあります";
    t.fitNotYet = "まだ合わせていません";
    t.fitGazeCenterFormat = "視線の正面: 左右 %s・上下 %s・傾き %s";
    t.fitGazeRangeFormat = "視線の幅: 左右 %s・上 %s・下 %s";
    t.fitGazeNone = "視線: 合わせていません";
    t.fitEyeXFormat = "目ごとの左右: 左 %s・×%s　右 %s・×%s";
    t.fitLidFormat = "まぶた %s: 開 %s・閉 %s・下を見ると %s";
    t.fitLidsNone = "まぶた: 合わせていません（自動で覚えています）";
    t.fitFailed = "合わせられませんでした";
    t.failCancelled = "止めました（ダッシュボードを開くと止まります）";
    t.failWaitTimedOut = "1 分のうちにダッシュボードが閉じられませんでした";
    t.failNotRunning = "frameeyeosc が動いていません";
    t.failNoResult = "frameeyeosc から結果が届きませんでした";
    t.failUnsteadyFormat = "%s の点で視線が落ち着きませんでした（目を閉じていたかも）";
    t.failNotClosed = "目を閉じているのが測れませんでした（3 回）";
    t.failNoMovementFormat = "%s の点で視線がほとんど動きませんでした";
    t.failLidRange = "まぶたの開け閉めの差が小さすぎました";
    t.failWrite = "設定ファイルに書けませんでした";
    t.failDetailSeparator = "・";
    t.failDetailPointFormat = "%sの点";
    t.failDetailSamplesFormat = "使えたサンプル %d/%d";
    t.failDetailSamplesRateFormat = "使えたサンプル %d/%d（%s%d 以上が必要）";
    t.failDetailRateFormat = "毎秒 %.0f・";
    t.failDetailSpreadFormat = "ばらつき %s（%.1f° まで）";
    t.failDetailTriesFormat = "%d 回";
    t.failDetailEyeLeft = "左";
    t.failDetailEyeRight = "右";
    t.failDetailClosedFormat = "%s %.2f（%.2f 未満が必要）";
    t.failDetailMovedFormat = "動いたのは %.1f°（%.1f° 以上が必要）";
    t.failDetailSidewaysLeft = "左目の左右";
    t.failDetailSidewaysRight = "右目の左右";
    t.failDetailLidWhereFormat = "%sのまぶた（%s）";
    t.failDetailLidFormat = "開き %.2f と閉じ %.2f の差 %.2f（%.2f 以上が必要）";
    t.failDetailLidNone = "開き具合が届きませんでした";
    t.pointCenter = "正面";
    t.pointUp = "上";
    t.pointDown = "下";
    t.pointLeft = "左";
    t.pointRight = "右";
    t.pointClosed = "目を閉じる";
    t.targetClose = "3 秒間\n目を閉じて";
    t.targetKeepClosed = "閉じたまま";
    t.targetOpen = "開けて OK";
    t.fitReset = "元に戻す";
    t.fitDetails = "細かく直す";
    t.fitSoundsOn = "♪ 音を鳴らす: オン";
    t.fitSoundsOff = "♪ 音を鳴らす: オフ";
    t.rowAutoRecenter = "被ったとき";
    t.hintAutoRecenter = "被り直したら自動で合わせ直す";
    t.autoRecenterOff = "何もしない";
    t.autoRecenterCenter = "正面だけ";
    t.autoRecenterTilt = "正面と傾き";
    t.rowOffset = "正面の位置";
    t.hintOffset = "＋ は右・上";
    t.rowGain = "動く幅";
    t.hintGain = "大きいほどよく動く";
    t.capLeftRight = "左右";
    t.capUpDown = "上下";
    t.capUp = "上";
    t.capDown = "下";
    t.detailsGaze = "視線";
    t.detailsLids = "まぶた";
    t.rowEyeX = "目ごとの左右";
    t.hintEyeX = "左右の目を別々に動かすときに使う";
    t.rowDownHold = "真下で左右を止める";
    t.hintDownHold = "0 = オフ";
    t.rowTilt = "傾き";
    t.rowEyeLog = "目のログ";
    t.eyeLogRecord = "記録する";
    t.eyeLogStopFormat = "止める %s";
    t.eyeLogWhereFormat = "保存先 %s";
    t.eyeLogLimit = "60 分たつと自動で止まります";
    t.eyeLogFailedFormat = "記録できませんでした: %s";
    t.eyeLogAutoStopped = "60 分たったので止めました";
    t.recordingFormat = "記録中 %s";
    t.hintTilt = "＋ は右が上";
    t.downHoldFormat = "%s° より下";
    t.rowLidFit = "まぶたの読んだ値";
    t.hintLidFit = "Frame の生の開き具合";
    t.capClosed = "閉じ";
    t.capAhead = "正面";
    t.lidFitInUse = "目を合わせた値を使っています";
    t.rowWiden = "見開きやすさ";
    t.hintWiden = "目を合わせた目の見開き";
    t.widenModes[0] = "しない";
    t.widenModes[1] = "控えめ";
    t.widenModes[2] = "ふつう";
    t.widenModes[3] = "出やすい";
    t.widenFollowsLeft = "左目は右目に合わせて見開きます（開き具合が上限に近いため）";
    t.widenFollowsRight = "右目は左目に合わせて見開きます（開き具合が上限に近いため）";
    t.widenNoRoom = "両目とも開き具合が上限に近いため、見開きません";
    t.opennessSaturated = "この SteamOS では開き具合が 1.0 で頭打ちのため、見開きは届きません";
    t.lidMarksFitted = "閉じ・開きは目を合わせた値、見開きは「見開きやすさ」で決まります";
    t.lidMarksUnused = "③④は目を合わせていない目のための目盛りです";

    t.rowCalibration = "自動キャリブレーション";
    t.learnedFormat = "覚えた値 左 %s・右 %s";
    t.notLearned = "まだ覚えていません";
    t.learning = "覚えている最中";
    t.calibrationReset = "リセット";
    t.rowScale = "左右の倍率";
    t.hintScaleAuto = "自動 = キャリブレーションの値";
    t.hintScaleFixed = "固定の倍率を使います";
    t.hintScaleFitted = "目を合わせたあとの微調整";
    t.scaleAuto = "自動";
    t.scaleFixed = "固定";
    t.marksTitle = "目を開け閉めしながら、線を今の値に合わせます（倍率を掛けた後の生の値）";
    t.markClosed = "閉じ";
    t.markOpen = "普通";
    t.markWidenStart = "見開き始め";
    t.markWide = "見開き最大";
    t.rowSync = "左右をそろえる強さ";
    t.hintSync = "0 = そろえない。ウインクは通す";
    t.rowBlink = "まばたきを届ける";
    t.hintBlink = "閉じたまま保つ時間・両目で閉じる";
    t.blinkHold = "保持";
    t.blinkSync = "両目";
    t.rowLidSmooth = "まぶたのなめらかさ";

    t.sectionTools = "調べる道具";
    t.sectionFiles = "ファイルと本体";
    t.updateCheckChip = "起動時と 1 日 1 回確認";
    t.historyButton = "更新履歴";
    t.historyTitle = "更新履歴";
    t.historyClose = "閉じる";
    t.historyMissing = "更新履歴が見つかりません";

    t.eyecamTitle = "目の撮影（開発用）";
    t.eyecamStart = "撮影開始";
    t.eyecamSearching = "目を探しています…";
    t.eyecamStop = "中止";
    t.eyecamErrorTitle = "撮影できませんでした";
    t.eyecamRetry = "もう一度撮影";
    t.eyecamUnknownFormat = "状態: %s";
    t.eyecamFpsFormat = "fps  左 %s / 右 %s";
    t.eyecamNotLocked = "HMD をかぶってください";
    t.eyecamRemainingFormat = "あと %d 秒";
    t.eyecamStepFormat = "ステップ %d / %d";
    t.eyecamSending = "送っています…";
    t.eyecamReplyFailedFormat = "%s が失敗: %s";
    t.eyecamStepNormal = "普通に開けて";
    t.eyecamStepWiden = "見開いて！";
    t.eyecamStepClose = "目を閉じて";
    t.eyecamStepSquint = "目を細めて";
    t.eyecamStepLookUp = "上を見て";
    t.eyecamStepLookDown = "下を見て";
    t.eyecamStepBright = "明るい画面を見て";
    t.eyecamStepDark = "暗い画面を見て";
    t.eyecamStepEnd = "おわり";
    t.eyecamStepLeadIn = "もうすぐ始まるよ";
    t.eyecamLightTitle = "光の注意";
    t.eyecamLightWarning =
        "明るい画面・暗い画面の段では、\n視界全体が白・黒に切り替わります。\n\n"
        "光過敏性てんかんの心配がある人は、光なしで撮ってください。\n\n"
        "途中で気分が悪くなったら、［中止］を押して\nHMD を外してください。";
    t.eyecamStartWithLight = "光ありで始める";
    t.eyecamStartNoLight = "光なしで始める";
    t.eyecamCancel = "やめる";
    t.eyecamNoLight = "光なし";
    t.rowCameraLids = "カメラで瞼を取る";
    t.cameraUseBoth = "使用中：左・右";
    t.cameraUseLeft = "使用中：左だけ（右は Valve の値）";
    t.cameraUseRight = "使用中：右だけ（左は Valve の値）";
    t.cameraUseValve = "Valve の値を使用中";
    t.cameraUseValveFormat = "Valve の値を使用中（%s）";
    t.cameraWhyNotCalibrated = "このかぶりの校正がまだ";
    t.cameraWhyNoCamera = "カメラの値が届いていない";
    t.cameraWhyWarming = "基準を覚えてるところ";
    t.cameraPupilSuffix = "（瞳孔も）";
    t.eyecamWarmingFormat = "普段の目を覚えてるよ（あと %d 秒）";
    t.eyecamLiveOff = "eyecam-rec がカメラの値を読んでいないよ（live がオフ）";
    t.eyecamCalibWearTitle = "目のカメラの校正";
    t.eyecamCalibUserTitle = "ユーザー校正";
    t.eyecamCalibWaiting = "カメラの映像を待っています…";
    t.eyecamCalibErrorTitle = "校正できなかった";
    t.eyecamCalibRetry = "もう一度校正";
    t.eyecamSensitivity = "見開きの感度";
    t.eyecamSensitivityDull = "鈍い";
    t.eyecamSensitivitySharp = "敏感";
    t.setupTitle = "目のカメラの準備（最初の一度だけ）";
    t.setupOptional = "準備しなくても Valve の値で使えるよ";
    t.setupOneLeft = "あと 1 つ";
    t.setupAllDone = "ぜんぶ済んだよ";
    t.setupStepPassword = "パスワードを決める";
    t.setupStepTool = "道具を入れる";
    t.setupStepLearn = "目の動きを覚える";
    t.setupStepDone = "完了";
    t.setupLaterTool = "Konsole でパスワードを打つだけ";
    t.setupLaterLearn = "ボタンを押して 18 秒";
    t.setupLaterDone = "見開きがカメラで出るようになるよ";
    t.setupPasswordLabel = "パスワード：";
    t.setupPasswordSet = "設定済み";
    t.setupAutoChecked = "（自動で確認）";
    t.setupToolLabel = "道具：";
    t.setupToolDone = "入ったよ";
    t.setupLearnLabel = "目の動き：";
    t.setupLearnDone = "覚えたよ";
    t.setupPassPill = "決めたら自動で進むよ";
    t.setupPassBody = "SteamOS のパスワードがまだないみたい。次の ② で 1 回だけ使うから、先に決めてね";
    t.setupPassWhere = "設定画面で";
    t.setupPassPath1 = "Steam の設定";
    t.setupPassPath2 = "開発者";
    t.setupPassPath3 = "ユーザーパスワードを変更";
    t.setupPassKonsole = "Konsole で";
    t.setupPassKonsoleHow = "下のボタン → Enter → 同じパスワードを 2 回打つ";
    t.setupPassMemo = "メモ";
    t.setupPassMemoText = "「開発者」がないときは、システム → 開発者モードをオン";
    t.setupPassButton = "Konsole で passwd を開く";
    t.setupVideo = "動画を開く";
    t.setupVideoNote = "動画は Frame の Chromium で開くよ・開けないとき：";
    t.setupCheckPill = "入るのを待ってるよ";
    t.setupCheckFlow = "Konsole で開く → Enter → パスワード";
    t.setupCheckTyped = "（入力済みで開くよ）";
    t.setupCheckWhat = "何をする";
    t.setupCheckWhatText = "目のカメラの映像を受け取る小さな道具に、その権限だけを付けるよ";
    t.setupCheckPassword = "パスワード";
    t.setupCheckPasswordText = "SteamOS のパスワードを聞かれるよ。打っても画面には出ないよ";
    t.setupCheckSsh = "SSH なら";
    t.setupCheckSshText = "上の 1 行をそのまま打ってね。最後に --uninstall を付けると取り除けるよ";
    t.setupCheckButton = "Konsole で開く";
    t.setupWaitPill = "押すのを待ってるよ";
    t.setupWaitTitle = "HMD をかぶって、準備ができたら押してね";
    t.setupWaitVideo = "カメラの映像";
    t.setupWaitVideoOk = "届いてるよ";
    t.setupWaitVideoNo = "まだ届いてないよ";
    t.setupWaitEyeOk = "見えてるよ";
    t.setupWaitEyeNo = "見えてないよ";
    t.setupWaitButton = "覚えはじめる";
    t.setupWaitHint1 = "押すと 18 秒の校正が始まるよ";
    t.setupWaitHint2 = "画面の指示どおりに目を動かしてね";
    t.setupWaitFoot = "目が見えてないときは押せないよ。SteamVR の視線トラッキングがオンか見てね";
    t.setupLearnPill = "校正中";
    t.setupLearnWidenHint = "思いっきり大きく";
    t.setupChipClose = "閉じる";
    t.setupChipNormal = "普段";
    t.setupChipWiden = "見開く";
    t.setupLeftBefore = "あと";
    t.setupLeftAfter = "秒";
    t.setupLearnStepFormat = "ステップ %d / %d・全部で %d 秒";
    t.setupLearnFoot = "切り替わるときは音でも合図するよ";
    t.setupStop = "やめる";
    t.setupErrorPill = "うまくいかなかった";
    t.setupAgain = "もう一度（18秒）";
    t.setupFailPill = "見開きだけ取れなかった";
    t.setupFailTitle = "見開きがうまく取れなかったよ";
    t.setupFailBody = "見開きは、今回は標準の値で始めるね";
    t.setupFailClosed = "閉じた目";
    t.setupFailNormal = "普段の目";
    t.setupFailWiden = "見開き";
    t.setupFailWidenValue = "標準の値を使うよ";
    t.setupFailProceed = "このまま進む";
    t.setupFailLater = "あとで校正し直しても OK";
    t.setupDoneTitle = "準備できたよ！";
    t.setupDoneBody = "見開きがカメラで VRChat に届くようになったよ。次からはかぶるだけで、毎回の校正はいらないよ";
    t.setupDoneHelp = "困ったときは、この「目のカメラ」タブで";
    t.setupDoneHelp1 = "見開きが出すぎる・出にくい";
    t.setupDoneHelp1Do = "見開きの感度を動かす";
    t.setupDoneHelp2 = "なんか変だな";
    t.setupDoneHelp2Do = "目のカメラの校正（18秒）";
    t.setupDoneButton = "使いはじめる";
    t.setupDoneNote = "押さなくても、次に開いたときはふだんの画面になるよ";
    t.nextTitle = "次にやること・目のカメラ";
    t.nextPass = "SteamOS の設定で決めてね";
    t.nextWait = "ボタンを押すと始まるよ";
    t.nextLearnFormat = "画面の指示どおりに・あと %d 秒";
    t.nextFail = "見開きは標準の値で進めるよ";
    t.nextError = "もう一度やってみてね";
    t.readyTitle = "目のカメラ：準備できたよ";
    t.readyNote = "この表示は少ししたら消えるよ";
    t.lidsFromValve = "Valve の値";
    t.lidsFromCamera = "目のカメラ";
    t.lidsFromCameraLeft = "目のカメラ（左）";
    t.lidsFromCameraRight = "目のカメラ（右）";
    t.camRowState = "いまの状態";
    t.camRowStateHint = "まぶたを何で取っているか";
    t.camLearned = "普段の目を覚えたよ";
    t.camRowLidsHint = "オフ = Valve の値だけ";
    t.camRowSensitivityHint = "見開きの調整はここだけ";
    t.camRowCalib = "違和感があるとき";
    t.camRowCalibHint = "ふだんは要らないよ";
    t.camCalibButton = "目のカメラの校正（18秒）";
    t.camCalibSide1 = "出てくる指示どおりに";
    t.camCalibSide2 = "目を動かしてね";
    t.camRowUser = "細めも送るなら";
    t.camRowUserHint = "やらなくても OK";
    t.camUserButton = "ユーザー校正（最初に 1 回）";
    t.camUserSide1 = "目を細めたのも";
    t.camUserSide2 = "送れるようになるよ";
    t.camUserNeedsCalib = "先に目のカメラの校正をしてね";
    t.camHelpTitle = "こんなときは";
    t.camHelp1 = "見開きが勝手に出る";
    t.camHelp1Do = "「見開きの感度」を鈍いほうへ";
    t.camHelp2 = "見開きが出にくい";
    t.camHelp2Do = "「見開きの感度」を敏感なほうへ";
    t.camHelp3 = "それでもずれてる感じ";
    t.camHelp3Do = "「目のカメラの校正」をしてね";
    t.camHelp4 = "HMD をかぶり直した";
    t.camHelp4Do = "何もしなくて OK（自動で覚え直すよ）";
    t.lidsCamRow = "見開き";
    t.lidsCamRowHint = "いまはカメラで出してるよ";
    t.lidsCamText1 = "強さは「目のカメラ」タブの";
    t.lidsCamText2 = "「見開きの感度」で変えてね";
    t.lidsCamButton = "目のカメラへ";
    t.lidsCamNote = "カメラをオフにすると、ここに「見開きやすさ」（しない〜出やすい）が戻るよ";
    t.lidsCamMarks = "カメラ使用中は見開きの目盛り（見開き始め・最大）は出さないよ";
    t.lidsCamMarksOpen = "カメラ使用中は ③④ を使わないよ";
    t.devTitle = "開発用";
    t.devRecord = "目の撮影";
    t.devRecordHint = "目のカメラの映像を録る";
    t.rowPrefix = "パラメーター名の頭";
    t.prefixNone = "なし";
    t.prefixExample = "例: ";
    t.prefixOther = "今: %s";
    t.rowConfigPath = "設定ファイル";
    t.configPathMismatch = "本体は別の設定ファイルを読んでいます: ";
    t.rowCalibrationPath = "キャリブレーション";
    t.rowStatusPath = "状態ファイル";
    t.rowLockedList = "コマンドで固定中";
    t.hintLockedList = "FRAMEEYEOSC_ARGS の項目";
    t.noneLocked = "なし";
    t.rowCore = "本体";
    t.coreFormat = "PID %d・動いて %s";
    t.notRunning = "動いていません";
    t.hoursMinutesFormat = "%d 時間 %d 分";
    t.minutesFormat = "%d 分";

    t.promptVrchat = "VRChat 向けのおすすめ設定にする？";
    t.promptEtvr = "VRCFaceTracking（ETVR）向けのおすすめ設定にする？";
    t.promptVrchatDetail1 = "視線とまぶたのなめらかさを標準に戻します";
    t.promptVrchatDetail2 = "";
    t.promptEtvrDetail1 = "視線のなめらかさは標準、まぶたのなめらかさは弱めにします";
    t.promptEtvrDetail2 = "（ETVR 側でもまぶたをなめらかにしているため）";
    t.promptLivelink = "VRCFT（LiveLink）向けのおすすめ設定にする？";
    t.promptLivelinkDetail1 = "視線とまぶたのなめらかさを標準に戻します";
    t.promptLivelinkDetail2 = "（LiveLink 側ではなめらかにしないため）";
    t.promptYes = "する";
    t.promptNo = "しない";

    t.rowVersion = "バージョン";
    t.checkedFormat = "・確認 %s";
    t.rowGazeDots = "視線の点を表示";
    t.hintGazeDots = "デバッグ用・送る視線を表示";
    t.dotDistance = "点の距離";
    t.rowUpdateCheck = "新しい版の確認";
    t.hintUpdateCheck = "起動時と 1 日 1 回、GitHub に新しい版がないか見に行きます";
    t.updateUpToDateFormat = "最新版です（%s）";
    t.updateChecking = "新しい版を確かめています…";
    t.updateAvailableFormat = "新しい版 %s があります";
    t.updateButton = "更新する";
    t.updateManual = "ここからは入れられない版です。GitHub から手で更新してね";
    t.updateConfirmFormat = "%s に更新しますか？";
    t.updateConfirmHint = "ダウンロードして入れ替えます。途中でこの画面が閉じて開き直すことがあります";
    t.updateConfirmYes = "更新する";
    t.updateConfirmNo = "やめる";
    t.updateInstallingFormat = "更新中: %s";
    t.updateInstalledFormat = "%s を入れました。開き直すと新しい版になります";
    t.updateInstallFailed = "更新できませんでした（今の版のままです）:";
    t.updateCheckFailed = "新しい版を確かめられませんでした:";
    t.updateCheckNow = "今すぐ確かめる";
    t.updateRetry = "もう一度";
    t.updateDismiss = "閉じる";
    t.updateLogHint = "くわしくは ~/.cache/frameeyeosc/update.log";
    t.stepStart = "準備中";
    t.stepDownload = "ダウンロード中";
    t.stepVerify = "ファイルを確認中";
    t.stepExtract = "展開中";
    t.stepInstall = "入れ替え中";
    t.reasonNetwork = "GitHub につながりません";
    t.reasonRateLimited = "GitHub の回数制限にかかりました。1 時間ほどあとで試してね";
    t.reasonNotFound = "公開されている版がありません";
    t.reasonBadResponse = "GitHub の返事を読めませんでした";
    t.reasonBadVersion = "版の番号を読めませんでした";
    t.reasonBadUrl = "GitHub 以外の場所へ向かったので止めました";
    t.reasonMissingTool = "必要なコマンド（python3）がありません";
    t.reasonNoChecksums = "この版には確認用の SHA256SUMS がありません。手で更新してね";
    t.reasonNoAsset = "この版には入れるファイルがありません";
    t.reasonChecksumMismatch = "ダウンロードしたファイルが壊れています";
    t.reasonUnsafeArchive = "ファイルの中身が安全でないので止めました";
    t.reasonNoInstaller = "ファイルに install.sh がありません";
    t.reasonInstallFailed = "install.sh が失敗しました";
    t.reasonBadArgs = "前回のインストールのオプションを読めません";
    t.reasonBusy = "別の更新が動いています";
    t.reasonNotNewer = "もう最新版です";
    t.reasonDetachFailed = "更新を始められませんでした（systemd-run）";
    t.reasonInterrupted = "更新が途中で止まりました";
    t.reasonIo = "ファイルを書けませんでした";
    t.reasonUpdater = "更新の仕組みが動きませんでした";
    t.reasonOther = "うまくいきませんでした";
    return t;
}

/**
 * Build the English table (same meaning, kept short).
 * @return the table
 */
UiText makeEnglish() {
    UiText t {};
    t.title = "Eye tracking";
    t.badgeSending = "Sending";
    t.badgeWaiting = "Waiting for eye data";
    t.badgePaused = "Paused";
    t.badgeNotRunning = "frameeyeosc is not running";
    t.notRunningHint1 = "Changes are saved and apply";
    t.notRunningHint2 = "when frameeyeosc starts";
    t.destination = "Destination";
    t.outputVrchatShort = "VRChat";
    t.outputEtvrShort = "VRCFT (ETVR)";
    t.outputLivelinkShort = "VRCFT (LiveLink)";
    t.searchingPc = "Looking for the PC…";
    t.modeAuto = "Auto (Steam Link PC)";
    t.modeFixed = "Fixed";
    t.rateLabel = "Send rate";
    t.rateFormat = "%.0f /s";
    t.trackerRateLabel = "Eye data";
    t.trackerRateLowHint = "low";
    t.trackerRateSlowHere = "frameeyeosc can't keep up";
    t.trackerRateSlowTracker = "The Frame itself sends few";
    t.lidsTitle = "Eyelids";
    t.legendRaw = "Raw";
    t.legendSent = "Sent";
    t.left = "L";
    t.right = "R";
    t.gazeTitle = "Gaze";
    t.leftEye = "Left";
    t.rightEye = "Right";
    t.noEyeData = "No eye data";
    t.errorPrefix = "Config error: ";
    t.sourceErrorPrefix = "Can't read eye data: ";
    t.dominantEyeLeft = "Frame setting: tracking the left eye only";
    t.dominantEyeRight = "Frame setting: tracking the right eye only";
    t.errWrite = "Can't save settings: ";
    t.errConfigBroken = "config.json is broken. \"Reset all\" makes a new one";
    t.errAutostart = "Autostart change failed (systemctl)";

    t.tabBasic = "Basic";
    t.tabOutput = "Output";
    t.tabGaze = "Gaze";
    t.tabGazeFit = "Eye fit";
    t.tabLids = "Eyelids";
    t.tabAdvanced = "Advanced";
    t.tabEyecam = "Eye cameras";

    t.on = "On";
    t.off = "Off";
    t.locked = "Locked by command line";
    t.lowerSmoother = "Lower = smoother";
    t.capStill = "When still";
    t.capFast = "Fast moves";
    t.capChange = "Change sensitivity";

    t.rowSending = "Sending";
    t.hintSending = "frameeyeosc keeps running";
    t.send = "Send";
    t.stop = "Pause";
    t.rowOutput = "Send to";
    t.hintOutput = "Sync = how other players see your eyes";
    t.outputVrchat = "VRChat direct";
    t.outputLivelink = "VRCFT (LiveLink)";
    t.outputEtvr = "VRCFT (ETVR)";
    t.outputRecommended = "Recommended";
    t.outputMarks[0][0] = "✓ Wide eyes";
    t.outputMarks[0][1] = "Sync: depends on avatar";
    t.outputMarks[0][2] = "No VRCFT needed";
    t.outputMarks[1][0] = "✓ Wide eyes";
    t.outputMarks[1][1] = "✓ Synced to others";
    t.outputMarks[1][2] = "Needs VRCFT";
    t.outputMarks[2][0] = "✗ No wide eyes";
    t.outputMarks[2][1] = "✓ Synced to others";
    t.outputMarks[2][2] = "Needs VRCFT";
    t.rowActiveType = "EyeTrackingActive type";
    t.hintActiveType = "Some avatars need Float";
    t.activeOff = "Off";
    t.rowSteamlink = "Steam Link names too";
    t.hintSteamlink = "For Steam Link avatars";
    t.steamlinkNoPrefix = " (never prefixed)";
    t.rowNativeEyes = "VRChat's own eyes too";
    t.hintNativeEyes = "For avatars without VRCFT parameters. Turn off if eyelids close too far";
    t.rowTarget = "Target PC";
    t.hintTarget = "Auto = the Steam Link PC";
    t.targetAuto = "Auto";
    t.targetFixNow = "Fix to current PC";
    t.targetEnter = "Enter IP";
    t.targetManualFormat = "Manual %s";
    t.hostEntryTitle = "Target PC's IP address";
    t.hostEntryHint = "e.g. 192.168.1.20 (the port goes in the Port row)";
    t.hostEntryOk = "OK";
    t.hostEntryCancel = "Cancel";
    t.hostErrEmpty = "Nothing entered";
    t.hostErrIpv4 = "Not an IP address (four numbers 0-255 separated by dots)";
    t.rowPort = "Port";
    t.portDefaultVrchat = "VRChat default: 9000";
    t.portDefaultLivelink = "LiveLink default: 11111";
    t.portDefaultEtvr = "ETVR default: 8889";
    t.vrcftSetupTitle = "Set up in VRCFT on the PC";
    t.vrcftStepsLivelink[0] = "Install LiveLink from VRCFT's Module Registry";
    t.vrcftStepsLivelink[1] = "Turn off other eye modules (such as ETVR)";
    t.vrcftStepsLivelink[2] = "Allow UDP 11111 in the Windows firewall";
    t.vrcftStepsEtvr[0] = "Install ETVR Eye Tracking from VRCFT's Module Registry";
    t.vrcftStepsEtvr[1] = "Turn off other eye modules (such as LiveLink)";
    t.vrcftStepsEtvr[2] = "Allow UDP 8889 in the Windows firewall";
    t.vrcftSetupNote = "VRCFT picks the parameter names and how they sync for your avatar, so there is nothing to set here.";
    t.vrcftNoWide = "Wide eyes don't come through in this format.";
    t.portReset = "Default";
    t.rowLanguage = "Language";
    t.rowAutostart = "Start with SteamVR";
    t.hintAutostart = "From the next SteamVR start";
    t.autostartMissing = "Not installed";
    t.autostartUnknown = "Can't read the state";
    t.resetAll = "Reset all";
    t.resetConfirm = "Press again to reset";
    t.quit = "Quit app";
    t.quitConfirm = "Press again to quit";
    t.footer = "Changes apply at once and survive restarts. Sending goes on after you quit the panel";

    t.rowSmoothing = "Smoothing";
    t.hintSmoothing = "Off = send raw values";
    t.rowStrength = "Smoothness";
    t.strengthLight = "Light";
    t.strengthMedium = "Medium";
    t.strengthStrong = "Strong";
    t.custom = "Custom (values below)";
    t.rawOnNote = "Smoothing is off";
    t.rowFine = "Fine tune";
    t.rowDeadzone = "Fixation deadzone";
    t.hintDeadzone = "Smaller wobbles are ignored";
    t.rowHold = "Hold gaze while blinking";
    t.hintHold = "Holds below this openness";
    t.rowIndependent = "Move eyes separately";
    t.hintIndependent = "Natural after an Eye fit";
    t.hintIndependentOneEye = "Both follow the one tracked eye";
    t.rowQuality = "Skip unreliable gaze";
    t.hintQuality = "Ignores an eye less sure than this";
    t.rowDespike = "Remove glitches";
    t.hintDespike = "Gaze and lids, ~11 ms later";

    t.rowFit = "Fit your eyes";
    t.hintFit = "Gaze and eyelids, about 20 s";
    t.fitStart = "Fit my eyes";
    t.fitAgain = "Fit again";
    t.fitCenterOnly = "Re-center only";
    t.fitCenterTilt = "Re-center + tilt";
    t.fitStop = "Stop";
    t.fitIntro = "It starts when you close the dashboard. Keep your head still and follow the dot with your eyes. "
                 "At the end, after 3, 2, 1, close your eyes for 3 seconds and open them at the chime.";
    t.fitNeedsRunning = "Works while frameeyeosc is running";
    t.fitLocked = "Not available: gaze or eyelid values are locked by the command line";
    t.fitWaiting = "Close the dashboard to start";
    t.fitHowTo = "Keep your head still and follow the dot with your eyes. At the end, after 3, 2, 1, close your eyes "
                 "for 3 seconds and open them at the chime. Opening the dashboard stops it.";
    t.fitWaitingCenter = "A dot appears straight ahead. Keep your head still and look at it. Opening the dashboard "
                         "stops it.";
    t.fitWaitingTilt = "A dot appears straight ahead, then up and down. Keep your head still and follow it with "
                       "your eyes. Opening the dashboard stops it.";
    t.fitRunningFormat = "Measuring: %s (%d of %d)";
    t.fitRetryFormat = ", try %d";
    t.fitDone = "Fitted";
    t.fitDoneCenter = "Re-centered";
    t.fitDoneTilt = "Re-centered and leveled";
    t.fitFitted = "Fitted";
    t.fitNotYet = "Not fitted yet";
    t.fitGazeCenterFormat = "Gaze center: L-R %s, U-D %s, tilt %s";
    t.fitGazeRangeFormat = "Gaze range: L-R %s, up %s, down %s";
    t.fitGazeNone = "Gaze: not fitted";
    t.fitEyeXFormat = "Each eye sideways: L %s x%s, R %s x%s";
    t.fitLidFormat = "Eyelid %s: open %s, closed %s, looking down %s";
    t.fitLidsNone = "Eyelids: not fitted (learned automatically)";
    t.fitFailed = "Could not fit";
    t.failCancelled = "Stopped (opening the dashboard stops it)";
    t.failWaitTimedOut = "The dashboard wasn't closed within a minute";
    t.failNotRunning = "frameeyeosc is not running";
    t.failNoResult = "No answer from frameeyeosc";
    t.failUnsteadyFormat = "The gaze wasn't steady at the %s dot (eyes closed?)";
    t.failNotClosed = "Couldn't measure your eyes closed (3 tries)";
    t.failNoMovementFormat = "The gaze hardly moved toward the %s dot";
    t.failLidRange = "The eyelids barely changed between open and closed";
    t.failWrite = "Couldn't write the settings file";
    t.failDetailSeparator = " · ";
    t.failDetailPointFormat = "%s dot";
    t.failDetailSamplesFormat = "%d of %d samples usable";
    t.failDetailSamplesRateFormat = "%d of %d samples usable%s (needs %d)";
    t.failDetailRateFormat = " at %.0f Hz";
    t.failDetailSpreadFormat = "spread %s (max %.1f°)";
    t.failDetailTriesFormat = "%d tries";
    t.failDetailEyeLeft = "L";
    t.failDetailEyeRight = "R";
    t.failDetailClosedFormat = "%s %.2f (needs below %.2f)";
    t.failDetailMovedFormat = "moved %.1f° (needs %.1f°)";
    t.failDetailSidewaysLeft = "left eye sideways";
    t.failDetailSidewaysRight = "right eye sideways";
    t.failDetailLidWhereFormat = "%s eyelid (%s dot)";
    t.failDetailLidFormat = "open %.2f vs closed %.2f, %.2f apart (needs %.2f)";
    t.failDetailLidNone = "no open reading";
    t.pointCenter = "center";
    t.pointUp = "up";
    t.pointDown = "down";
    t.pointLeft = "left";
    t.pointRight = "right";
    t.pointClosed = "eyes closed";
    t.targetClose = "Close your eyes\nfor 3 s";
    t.targetKeepClosed = "Keep them closed";
    t.targetOpen = "Open them";
    t.fitReset = "Reset";
    t.fitDetails = "Fine-tune";
    t.fitSoundsOn = "♪ Sounds: on";
    t.fitSoundsOff = "♪ Sounds: off";
    t.rowAutoRecenter = "When put on";
    t.hintAutoRecenter = "Fits again when put back on";
    t.autoRecenterOff = "Nothing";
    t.autoRecenterCenter = "Re-center";
    t.autoRecenterTilt = "Re-center + tilt";
    t.rowOffset = "Straight ahead";
    t.hintOffset = "+ is right / up";
    t.rowGain = "Range";
    t.hintGain = "Higher moves further";
    t.capLeftRight = "Left-right";
    t.capUpDown = "Up-down";
    t.capUp = "Up";
    t.capDown = "Down";
    t.detailsGaze = "Gaze";
    t.detailsLids = "Eyelids";
    t.rowEyeX = "Each eye sideways";
    t.hintEyeX = "Used when the eyes move separately";
    t.rowDownHold = "Hold sideways far down";
    t.hintDownHold = "0 = off";
    t.rowTilt = "Tilt";
    t.rowEyeLog = "Eye log";
    t.eyeLogRecord = "Record";
    t.eyeLogStopFormat = "Stop %s";
    t.eyeLogWhereFormat = "Saved to %s";
    t.eyeLogLimit = "Stops by itself after 60 minutes";
    t.eyeLogFailedFormat = "Couldn't record: %s";
    t.eyeLogAutoStopped = "Stopped after 60 minutes";
    t.recordingFormat = "Recording %s";
    t.hintTilt = "+ = right up";
    t.downHoldFormat = "Below %s°";
    t.rowLidFit = "Eyelid readings";
    t.hintLidFit = "Frame openness";
    t.capClosed = "Closed";
    t.capAhead = "Ahead";
    t.lidFitInUse = "Using the eye fit";
    t.rowWiden = "Widen";
    t.hintWiden = "For eyes with an eye fit";
    t.widenModes[0] = "Off";
    t.widenModes[1] = "Less";
    t.widenModes[2] = "Normal";
    t.widenModes[3] = "More";
    t.widenFollowsLeft = "The left eye widens with the right (its openness is near the limit)";
    t.widenFollowsRight = "The right eye widens with the left (its openness is near the limit)";
    t.widenNoRoom = "Neither eye widens: both read near the openness limit";
    t.opennessSaturated = "This SteamOS caps eye openness at 1.0, so widening can't come through";
    t.lidMarksFitted = "Closing and opening come from the eye fit, widening from Widen";
    t.lidMarksUnused = "Marks 3 and 4 are for eyes without an eye fit";

    t.rowCalibration = "Auto calibration";
    t.learnedFormat = "Learned L %s / R %s";
    t.notLearned = "Nothing learned yet";
    t.learning = "Learning";
    t.calibrationReset = "Reset";
    t.rowScale = "Eye scales";
    t.hintScaleAuto = "Auto = from calibration";
    t.hintScaleFixed = "Uses fixed scales";
    t.hintScaleFitted = "Fine-tune after the eye fit";
    t.scaleAuto = "Auto";
    t.scaleFixed = "Fixed";
    t.marksTitle = "Blink and open wide, then move the lines to match (raw value after scaling)";
    t.markClosed = "Closed";
    t.markOpen = "Open";
    t.markWidenStart = "Widen start";
    t.markWide = "Widest";
    t.rowSync = "Sync both lids";
    t.hintSync = "0 = off. Winks pass through";
    t.rowBlink = "Make blinks visible";
    t.hintBlink = "Hold closed, close both eyes";
    t.blinkHold = "Hold";
    t.blinkSync = "Both";
    t.rowLidSmooth = "Eyelid smoothing";

    t.sectionTools = "Diagnostics";
    t.sectionFiles = "Files and process";
    t.updateCheckChip = "Check at start and daily";
    t.historyButton = "Version history";
    t.historyTitle = "Version history";
    t.historyClose = "Close";
    t.historyMissing = "Version history not found";

    t.eyecamTitle = "Eye capture (developer)";
    t.eyecamStart = "Start recording";
    t.eyecamSearching = "Looking for the eyes…";
    t.eyecamStop = "Stop";
    t.eyecamErrorTitle = "Recording failed";
    t.eyecamRetry = "Start again";
    t.eyecamUnknownFormat = "State: %s";
    t.eyecamFpsFormat = "fps  L %s / R %s";
    t.eyecamNotLocked = "Put the headset on";
    t.eyecamRemainingFormat = "%d s left";
    t.eyecamStepFormat = "Step %d of %d";
    t.eyecamSending = "Sending…";
    t.eyecamReplyFailedFormat = "%s failed: %s";
    t.eyecamStepNormal = "Open normally";
    t.eyecamStepWiden = "Open wide!";
    t.eyecamStepClose = "Close your eyes";
    t.eyecamStepSquint = "Squint";
    t.eyecamStepLookUp = "Look up";
    t.eyecamStepLookDown = "Look down";
    t.eyecamStepBright = "Look at the bright screen";
    t.eyecamStepDark = "Look at the dark screen";
    t.eyecamStepEnd = "Done";
    t.eyecamStepLeadIn = "Get ready";
    t.eyecamLightTitle = "Light warning";
    t.eyecamLightWarning =
        "During the bright and dark steps your whole view turns white or black.\n\n"
        "If you might be sensitive to light (photosensitive epilepsy), record without light.\n\n"
        "If you feel unwell, press Stop and take the headset off.";
    t.eyecamStartWithLight = "Start with light";
    t.eyecamStartNoLight = "Start without light";
    t.eyecamCancel = "Cancel";
    t.eyecamNoLight = "No light";
    t.rowCameraLids = "Eyelids from the eye cameras";
    t.cameraUseBoth = "In use: left and right";
    t.cameraUseLeft = "In use: left only (right: Valve's values)";
    t.cameraUseRight = "In use: right only (left: Valve's values)";
    t.cameraUseValve = "Using Valve's values";
    t.cameraUseValveFormat = "Using Valve's values (%s)";
    t.cameraWhyNotCalibrated = "not calibrated for this wear";
    t.cameraWhyNoCamera = "no camera values";
    t.cameraWhyWarming = "learning the baseline";
    t.cameraPupilSuffix = " (pupils too)";
    t.eyecamWarmingFormat = "Learning your relaxed eyes (%d s left)";
    t.eyecamLiveOff = "eyecam-rec isn't reading the cameras live (live is off)";
    t.eyecamCalibWearTitle = "Calibrating the cameras";
    t.eyecamCalibUserTitle = "User calibration";
    t.eyecamCalibWaiting = "Waiting for the cameras…";
    t.eyecamCalibErrorTitle = "Calibration failed";
    t.eyecamCalibRetry = "Calibrate again";
    t.eyecamSensitivity = "Widening sensitivity";
    t.eyecamSensitivityDull = "Dull";
    t.eyecamSensitivitySharp = "Sensitive";
    t.setupTitle = "Eye camera setup (just once)";
    t.setupOptional = "Without it, Valve's values still work";
    t.setupOneLeft = "One more";
    t.setupAllDone = "All done";
    t.setupStepPassword = "Set a password";
    t.setupStepTool = "Install the tool";
    t.setupStepLearn = "Learn your eye movements";
    t.setupStepDone = "Done";
    t.setupLaterTool = "Just type your password in Konsole";
    t.setupLaterLearn = "Press the button, 18 s";
    t.setupLaterDone = "Widening will come from the cameras";
    t.setupPasswordLabel = "Password: ";
    t.setupPasswordSet = "set";
    t.setupAutoChecked = "(checked automatically)";
    t.setupToolLabel = "Tool: ";
    t.setupToolDone = "installed";
    t.setupLearnLabel = "Eye movements: ";
    t.setupLearnDone = "learned";
    t.setupPassPill = "Moves on once set";
    t.setupPassBody = "SteamOS has no password yet. Step ② needs it once, so set one first";
    t.setupPassWhere = "In Settings";
    t.setupPassPath1 = "Steam settings";
    t.setupPassPath2 = "Developer";
    t.setupPassPath3 = "Change user password";
    t.setupPassKonsole = "In Konsole";
    t.setupPassKonsoleHow = "The button below → Enter → the same password twice";
    t.setupPassMemo = "Note";
    t.setupPassMemoText = "No \"Developer\"? Turn on System → Developer mode";
    t.setupPassButton = "Open passwd in Konsole";
    t.setupVideo = "Open the video";
    t.setupVideoNote = "The video opens in the Frame's Chromium. If it doesn't: ";
    t.setupCheckPill = "Waiting for it";
    t.setupCheckFlow = "Open in Konsole → Enter → password";
    t.setupCheckTyped = "(it opens typed in)";
    t.setupCheckWhat = "What";
    t.setupCheckWhatText = "Gives a small tool just the right to receive the eye cameras";
    t.setupCheckPassword = "Password";
    t.setupCheckPasswordText = "It asks for the SteamOS password. Typing it shows nothing";
    t.setupCheckSsh = "Over SSH";
    t.setupCheckSshText = "Type the line above. Add --uninstall at the end to remove it";
    t.setupCheckButton = "Open in Konsole";
    t.setupWaitPill = "Waiting for you";
    t.setupWaitTitle = "Put the headset on, then press when ready";
    t.setupWaitVideo = "Camera video";
    t.setupWaitVideoOk = "arriving";
    t.setupWaitVideoNo = "not yet";
    t.setupWaitEyeOk = "seen";
    t.setupWaitEyeNo = "not seen";
    t.setupWaitButton = "Start learning";
    t.setupWaitHint1 = "Starts an 18 s calibration";
    t.setupWaitHint2 = "Move your eyes as the screen says";
    t.setupWaitFoot = "It can't be pressed while the eyes aren't seen. Check SteamVR's eye tracking is on";
    t.setupLearnPill = "Calibrating";
    t.setupLearnWidenHint = "As wide as you can";
    t.setupChipClose = "Close";
    t.setupChipNormal = "Normal";
    t.setupChipWiden = "Wide";
    t.setupLeftBefore = "";
    t.setupLeftAfter = "s left";
    t.setupLearnStepFormat = "Step %d of %d · %d s in all";
    t.setupLearnFoot = "A beep marks each change";
    t.setupStop = "Stop";
    t.setupErrorPill = "Didn't work";
    t.setupAgain = "Again (18 s)";
    t.setupFailPill = "Only widening failed";
    t.setupFailTitle = "Couldn't get your widening";
    t.setupFailBody = "Widening starts from the standard values this time";
    t.setupFailClosed = "Closed eyes";
    t.setupFailNormal = "Relaxed eyes";
    t.setupFailWiden = "Widening";
    t.setupFailWidenValue = "standard values";
    t.setupFailProceed = "Continue";
    t.setupFailLater = "You can calibrate again later";
    t.setupDoneTitle = "All set!";
    t.setupDoneBody = "Widening now reaches VRChat from the cameras. From now on just put the headset on: no calibration each time";
    t.setupDoneHelp = "If something is off, on this Eye cameras tab";
    t.setupDoneHelp1 = "Widening too much or too little";
    t.setupDoneHelp1Do = "Move the widening sensitivity";
    t.setupDoneHelp2 = "Something feels odd";
    t.setupDoneHelp2Do = "Calibrate the cameras (18 s)";
    t.setupDoneButton = "Start using";
    t.setupDoneNote = "Even without pressing, the usual page shows next time";
    t.nextTitle = "Next · Eye cameras";
    t.nextPass = "Set it in the SteamOS settings";
    t.nextWait = "It starts with the button";
    t.nextLearnFormat = "As the screen says · %d s left";
    t.nextFail = "Widening goes on standard values";
    t.nextError = "Try once more";
    t.readyTitle = "Eye cameras: ready";
    t.readyNote = "This goes away in a moment";
    t.lidsFromValve = "Valve's values";
    t.lidsFromCamera = "eye cameras";
    t.lidsFromCameraLeft = "eye cameras (L)";
    t.lidsFromCameraRight = "eye cameras (R)";
    t.camRowState = "Now";
    t.camRowStateHint = "What drives the eyelids";
    t.camLearned = "Learned your relaxed eyes";
    t.camRowLidsHint = "Off = Valve's values only";
    t.camRowSensitivityHint = "The only place to adjust widening";
    t.camRowCalib = "When something feels off";
    t.camRowCalibHint = "Not needed normally";
    t.camCalibButton = "Calibrate the cameras (18 s)";
    t.camCalibSide1 = "Move your eyes as";
    t.camCalibSide2 = "the screen says";
    t.camRowUser = "To send squints too";
    t.camRowUserHint = "Optional";
    t.camUserButton = "User calibration (once)";
    t.camUserSide1 = "Squinting gets";
    t.camUserSide2 = "sent too";
    t.camUserNeedsCalib = "Calibrate the cameras first";
    t.camHelpTitle = "When…";
    t.camHelp1 = "Widening shows by itself";
    t.camHelp1Do = "Move the sensitivity toward Dull";
    t.camHelp2 = "Widening hardly shows";
    t.camHelp2Do = "Move the sensitivity toward Sensitive";
    t.camHelp3 = "Still feels off";
    t.camHelp3Do = "Calibrate the eye cameras";
    t.camHelp4 = "Put the headset back on";
    t.camHelp4Do = "Nothing to do (it relearns by itself)";
    t.lidsCamRow = "Widening";
    t.lidsCamRowHint = "The cameras drive it now";
    t.lidsCamText1 = "Set its strength with the Eye";
    t.lidsCamText2 = "cameras tab's widening sensitivity";
    t.lidsCamButton = "Eye cameras";
    t.lidsCamNote = "With the cameras off, Widen (Off to More) comes back here";
    t.lidsCamMarks = "With the cameras, the widening marks (3 and 4) aren't used";
    t.lidsCamMarksOpen = "Marks 3 and 4 aren't used while the cameras drive the eyelids";
    t.devTitle = "Developer";
    t.devRecord = "Eye recording";
    t.devRecordHint = "Records the eye cameras";
    t.rowPrefix = "Parameter prefix";
    t.prefixNone = "None";
    t.prefixExample = "e.g. ";
    t.prefixOther = "Now: %s";
    t.rowConfigPath = "Config file";
    t.configPathMismatch = "frameeyeosc reads another config file: ";
    t.rowCalibrationPath = "Calibration";
    t.rowStatusPath = "Status file";
    t.rowLockedList = "Locked by command";
    t.hintLockedList = "Set in FRAMEEYEOSC_ARGS";
    t.noneLocked = "None";
    t.rowCore = "frameeyeosc";
    t.coreFormat = "PID %d, up %s";
    t.notRunning = "Not running";
    t.hoursMinutesFormat = "%d h %d min";
    t.minutesFormat = "%d min";

    t.promptVrchat = "Use the recommended settings for VRChat?";
    t.promptEtvr = "Use the recommended settings for VRCFaceTracking (ETVR)?";
    t.promptVrchatDetail1 = "Gaze and eyelid smoothing go back to default";
    t.promptVrchatDetail2 = "";
    t.promptEtvrDetail1 = "Gaze smoothing goes to default, eyelid smoothing gets lighter";
    t.promptEtvrDetail2 = "(ETVR already smooths the eyelids)";
    t.promptLivelink = "Use the recommended settings for VRCFT (LiveLink)?";
    t.promptLivelinkDetail1 = "Gaze and eyelid smoothing go back to default";
    t.promptLivelinkDetail2 = "(the LiveLink module doesn't smooth anything)";
    t.promptYes = "Yes";
    t.promptNo = "No";

    t.rowVersion = "Version";
    t.checkedFormat = " · checked %s";
    t.rowGazeDots = "Show gaze dots";
    t.hintGazeDots = "Debug: the sent gaze";
    t.dotDistance = "Dot distance";
    t.rowUpdateCheck = "Check for updates";
    t.hintUpdateCheck = "Looks on GitHub for a new version at start and once a day";
    t.updateUpToDateFormat = "Up to date (%s)";
    t.updateChecking = "Checking for updates…";
    t.updateAvailableFormat = "Version %s is available";
    t.updateButton = "Update";
    t.updateManual = "This version can't be installed from here. Update by hand from GitHub";
    t.updateConfirmFormat = "Update to %s?";
    t.updateConfirmHint = "It downloads and installs the new version. This panel may close and reopen meanwhile";
    t.updateConfirmYes = "Update";
    t.updateConfirmNo = "Cancel";
    t.updateInstallingFormat = "Updating: %s";
    t.updateInstalledFormat = "%s is installed. Reopen to use it";
    t.updateInstallFailed = "The update failed (nothing was changed):";
    t.updateCheckFailed = "Couldn't check for updates:";
    t.updateCheckNow = "Check now";
    t.updateRetry = "Try again";
    t.updateDismiss = "Close";
    t.updateLogHint = "Details: ~/.cache/frameeyeosc/update.log";
    t.stepStart = "Preparing";
    t.stepDownload = "Downloading";
    t.stepVerify = "Verifying";
    t.stepExtract = "Unpacking";
    t.stepInstall = "Installing";
    t.reasonNetwork = "Can't reach GitHub";
    t.reasonRateLimited = "GitHub's rate limit was hit. Try again in an hour";
    t.reasonNotFound = "No published release";
    t.reasonBadResponse = "Couldn't read GitHub's answer";
    t.reasonBadVersion = "Couldn't read the version number";
    t.reasonBadUrl = "Stopped: the download led outside GitHub";
    t.reasonMissingTool = "A required command (python3) is missing";
    t.reasonNoChecksums = "This release has no SHA256SUMS. Update by hand";
    t.reasonNoAsset = "This release has no file to install";
    t.reasonChecksumMismatch = "The download is corrupt (checksum mismatch)";
    t.reasonUnsafeArchive = "Stopped: the archive has unsafe contents";
    t.reasonNoInstaller = "The archive has no install.sh";
    t.reasonInstallFailed = "install.sh failed";
    t.reasonBadArgs = "The saved install options are invalid";
    t.reasonBusy = "Another update is running";
    t.reasonNotNewer = "Already up to date";
    t.reasonDetachFailed = "Couldn't start the update (systemd-run)";
    t.reasonInterrupted = "The update was interrupted";
    t.reasonIo = "Couldn't write files";
    t.reasonUpdater = "The updater didn't run";
    t.reasonOther = "Something went wrong";
    return t;
}

const UiText kJapanese = makeJapanese();
const UiText kEnglish = makeEnglish();

}  // namespace

namespace {

/**
 * Read Steam's language setting: the first "language" value in ~/.steam/registry.vdf ("japanese" and so on).
 * @return the value, or empty if it can't be read
 */
std::string steamLanguage() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return "";
    std::ifstream file(std::string(home) + "/.steam/registry.vdf");
    std::string line;
    while (std::getline(file, line)) {
        // Format: <tab>"language"<tab>"japanese"
        const std::string key = "\"language\"";
        const size_t at = line.find(key);
        if (at == std::string::npos) continue;
        const size_t open = line.find('"', at + key.size());
        const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
        if (close == std::string::npos) return "";
        return line.substr(open + 1, close - open - 1);
    }
    return "";
}

/**
 * Whether the locale (the first non-empty of LC_ALL, LC_MESSAGES, LANG) is Japanese.
 * @return true if Japanese
 */
bool localeIsJapanese() {
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && value[0] != '\0') return std::string(value).rfind("ja", 0) == 0;
    }
    return false;
}

/**
 * Look up the system language (the body of systemLanguage).
 * @return the language
 */
Language detectSystemLanguage() {
    const std::string steam = steamLanguage();
    if (!steam.empty()) return steam == "japanese" ? Language::Ja : Language::En;
    return localeIsJapanese() ? Language::Ja : Language::En;
}

}  // namespace

Language systemLanguage() {
    static const Language cached = detectSystemLanguage();
    return cached;
}

const UiText& uiText(Language language) {
    return language == Language::En ? kEnglish : kJapanese;
}

const char* updateStepText(const UiText& t, const std::string& step) {
    if (step == "download") return t.stepDownload;
    if (step == "verify") return t.stepVerify;
    if (step == "extract") return t.stepExtract;
    if (step == "install") return t.stepInstall;
    return t.stepStart;
}

const char* updateReasonText(const UiText& t, const std::string& code) {
    static const struct {
        const char* code;
        const char* UiText::*text;
    } kReasons[] = {
        {"network", &UiText::reasonNetwork},
        {"rate-limited", &UiText::reasonRateLimited},
        {"not-found", &UiText::reasonNotFound},
        {"bad-response", &UiText::reasonBadResponse},
        {"bad-version", &UiText::reasonBadVersion},
        {"bad-url", &UiText::reasonBadUrl},
        {"missing-tool", &UiText::reasonMissingTool},
        {"no-checksums", &UiText::reasonNoChecksums},
        {"no-asset", &UiText::reasonNoAsset},
        {"checksum-mismatch", &UiText::reasonChecksumMismatch},
        {"unsafe-archive", &UiText::reasonUnsafeArchive},
        {"no-installer", &UiText::reasonNoInstaller},
        {"install-failed", &UiText::reasonInstallFailed},
        {"bad-args", &UiText::reasonBadArgs},
        {"busy", &UiText::reasonBusy},
        {"not-newer", &UiText::reasonNotNewer},
        {"detach-failed", &UiText::reasonDetachFailed},
        {"interrupted", &UiText::reasonInterrupted},
        {"io", &UiText::reasonIo},
        {"usage", &UiText::reasonUpdater},
        {"script-failed", &UiText::reasonUpdater},
        {"spawn-failed", &UiText::reasonUpdater},
    };
    for (const auto& reason : kReasons) {
        if (code == reason.code) return t.*reason.text;
    }
    return t.reasonOther;
}

const char* languageCode(Language language) {
    return language == Language::En ? "en" : "ja";
}

bool parseLanguage(const std::string& code, Language& language) {
    if (code == "ja") {
        language = Language::Ja;
        return true;
    }
    if (code == "en") {
        language = Language::En;
        return true;
    }
    return false;
}
