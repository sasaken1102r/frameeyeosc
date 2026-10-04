//! English versions of the Japanese status messages (status.json `message_en`, for the panel's English display).
//! Every message eyecam-rec shows is translated here, in one place, from the Japanese text it builds; the test lists
//! them all. Unknown text comes back unchanged.

/// Fixed messages.
const EXACT: &[(&str, &str)] = &[
    ("待機中", "Idle"),
    ("目の値を出しているよ", "Sending eye values"),
    ("見開きの基準を覚えているところ（目を開けて、ふつうに前を見ていてね）", "Learning your normal eye opening (keep your eyes open and look ahead)"),
    ("見開きの幅がまだわからないので仮の値。一度だけ calib wear をしてね", "Widening width not known yet, using a default. Please calibrate the eye camera once"),
    ("HMD をかぶってね（目の映像を待ってるよ）", "Put the headset on (waiting for the eye cameras)"),
    ("HMD をかぶってね", "Put the headset on"),
    ("sudo eyecam-grab を実行してね", "Run sudo eyecam-grab"),
    ("eyecam-rec は止まっている", "eyecam-rec is stopped"),
    ("録画中", "Recording"),
    ("校正中", "Calibrating"),
    ("中止した", "Cancelled"),
    ("校正を中止した", "Calibration cancelled"),
    ("校正できた（かぶり）", "Calibrated"),
    ("校正できた（見開きは取れなかったので、いつもの幅を使うよ）", "Calibrated (couldn't measure widening, using the usual width)"),
    ("校正できた（ユーザー）", "Calibrated (user)"),
    ("瞳孔の範囲を保存した", "Saved the pupil range"),
    ("瞳孔の範囲は測れなかった", "Couldn't measure the pupil range"),
    ("校正中じゃない", "Not calibrating"),
    ("先に calib wear をしてね", "Calibrate the eye camera first"),
    ("途中で止めた", "Stopped partway"),
    ("校正の計算が終わらなかった", "The calibration didn't finish"),
    ("止めた（途中まで保存）", "Stopped (saved so far)"),
    ("中断した（Ctrl-C / SIGTERM）", "Interrupted (Ctrl-C / SIGTERM)"),
    ("stop で止めた", "Stopped"),
    ("アイトラッカーが終了した。sudo eyecam-grab をもう一度実行してね", "The eye tracker exited. Run sudo eyecam-grab again"),
    ("バッファが更新されなくなった。sudo eyecam-grab をもう一度実行してね", "The buffers stopped updating. Run sudo eyecam-grab again"),
    ("目の映像が見つからなかった（ヘッドセットをかぶってから start してね）", "Couldn't find the eye cameras (put the headset on, then start)"),
    ("ディスクの空きが 1 GB 未満", "Less than 1 GB of disk space left"),
    ("録画時間が終わった", "Recording time is over"),
    ("片目しか映っていない（ちゃんとかぶれてる？）", "Only one eye is visible (is the headset on properly?)"),
    ("目の映像がまだ流れていない", "The eye cameras aren't streaming yet"),
    ("自動でバッファを取りに行くよ（アイトラッキングが始まるのを待ってる）", "Getting the buffers automatically (waiting for eye tracking to start)"),
    ("自動でバッファを取りに行ってる…", "Getting the buffers automatically…"),
    ("視線の向き（Valve）が取れなかった。frameeyeosc が動いているか確かめて、もう一度", "Couldn't get the gaze direction from Valve. Check that frameeyeosc is running and try again"),
    ("視線と目の開きの関係が求められなかった（もう一度）", "Couldn't work out how gaze changes the eye opening (try again)"),
];

/// Per-eye calibration failures, after "左目" / "右目" (the details in [...] follow).
const EYE: &[(&str, &str)] = &[
    ("の瞳がうまく見えなかった（HMD のかぶり方を直して、もう一度）", "couldn't see the pupil well (adjust the headset and try again)"),
    ("を閉じたのが検出できなかった（もう一度、しっかり閉じてね）", "couldn't detect the eye closing (try again and close it firmly)"),
    ("の細めが測れなかった（もう一度）", "couldn't measure the squint (try again)"),
    ("の細めが浅かった（もう一度、しっかり細めてね）", "the squint was too shallow (try again and squint harder)"),
    (": 下を見ても目の開きが変わっていない（もう一度、しっかり下を見てね）", "looking down didn't change the eye opening (try again and look down clearly)"),
    (": 下を見たら下まぶたが上に動いた（検出の失敗かも。もう一度）", "the lower lid moved up when looking down (maybe a detection error; try again)"),
];

/// Prefixes followed by a name or reason that is kept as it is.
const PREFIX: &[(&str, &str)] = &[
    ("保存した（途中で止めた）: ", "Saved (stopped early): "),
    ("保存した: ", "Saved: "),
    ("バッファを受け取れなかった: ", "Couldn't get the buffers: "),
    ("失敗した: ", "Failed: "),
    ("保存できなかった: ", "Couldn't save: "),
    ("プロトコルが見つからない: ", "Protocol not found: "),
];

/// Words inside the [...] details of calibration messages.
fn details(s: &str) -> String {
    s.replace("下を見たとき", "looking down").replace("細め", "squint").replace("普段", "normal").replace('、', ", ")
}

/// The English for one Japanese message.
pub fn message_en(ja: &str) -> String {
    if let Some(rest) = ja.strip_prefix("（fake）") {
        return format!("(fake) {}", message_en(rest));
    }
    if let Some(&(_, en)) = EXACT.iter().find(|(j, _)| *j == ja) {
        return en.to_string();
    }
    // "<reason>（途中まで保存: <session>）"
    if let Some(body) = ja.strip_suffix('）')
        && let Some((head, name)) = body.rsplit_once("（途中まで保存: ")
    {
        return format!("{} (saved so far: {name})", message_en(head));
    }
    if let Some(rest) = ja.strip_prefix("校正できた（ユーザー）。注意: ") {
        // The warnings (joined with 、) are the left/right difference check's.
        return format!("Calibrated (user). Note: {}", details(&rest.replace("左右の差が大きい", "Large left/right difference")));
    }
    if let Some(rest) = ja.strip_prefix("左右の差が大きい") {
        return format!("Large left/right difference{}", details(rest));
    }
    if let Some(rest) = ja.strip_prefix("自動でバッファを取れなかった: ") {
        let why = rest.strip_suffix("（sudo eyecam-grab でもいい）").unwrap_or(rest);
        return format!("Couldn't get the buffers automatically: {why} (sudo eyecam-grab also works)");
    }
    if let Some((name, e)) = ja.split_once(".txt の書き方がおかしい: ") {
        return format!("{name}.txt is malformed: {e}");
    }
    for (eye_ja, eye_en) in [("左目", "Left eye"), ("右目", "Right eye")] {
        if let Some(rest) = ja.strip_prefix(eye_ja) {
            let (body, detail) = match rest.find('[') {
                Some(i) => (&rest[..i], format!(" {}", details(&rest[i..]))),
                None => (rest, String::new()),
            };
            if let Some(&(_, en)) = EYE.iter().find(|(j, _)| *j == body) {
                return format!("{eye_en}: {en}{detail}");
            }
        }
    }
    for (j, en) in PREFIX {
        if let Some(rest) = ja.strip_prefix(j) {
            return format!("{en}{rest}");
        }
    }
    ja.to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn has_japanese(s: &str) -> bool {
        s.chars().any(|c| matches!(c as u32, 0x3040..=0x30FF | 0x4E00..=0x9FFF | 0xFF01..=0xFF60))
    }

    #[test]
    fn every_message_has_english() {
        let all = [
            "待機中",
            "目の値を出しているよ",
            "見開きの基準を覚えているところ（目を開けて、ふつうに前を見ていてね）",
            "見開きの幅がまだわからないので仮の値。一度だけ calib wear をしてね",
            "HMD をかぶってね（目の映像を待ってるよ）",
            "（fake）HMD をかぶってね（目の映像を待ってるよ）",
            "（fake）sudo eyecam-grab を実行してね",
            "（fake）校正できた（かぶり）",
            "（fake）保存した（途中で止めた）: rec_fake",
            "sudo eyecam-grab を実行してね",
            "eyecam-rec は止まっている",
            "録画中",
            "校正中",
            "中止した",
            "校正を中止した",
            "校正できた（かぶり）",
            "校正できた（見開きは取れなかったので、いつもの幅を使うよ）",
            "校正できた（ユーザー）",
            "校正できた（ユーザー）。注意: 左右の差が大きい [下を見たとき 0.39、細め 0.08]",
            "瞳孔の範囲を保存した",
            "瞳孔の範囲は測れなかった",
            "先に calib wear をしてね",
            "保存した: rec_2026-10-04_10-00-00",
            "保存した（途中で止めた）: rec_2026-10-04_10-00-00",
            "目の映像が見つからなかった（ヘッドセットをかぶってから start してね）（途中まで保存: rec_x）",
            "ディスクの空きが 1 GB 未満",
            "失敗した: disk I/O error",
            "バッファを受け取れなかった: bad message",
            "自動でバッファを取りに行くよ（アイトラッキングが始まるのを待ってる）",
            "自動でバッファを取りに行ってる…",
            "自動でバッファを取れなかった: pidfd_getfd: Operation not permitted（sudo eyecam-grab でもいい）",
            "片目しか映っていない（ちゃんとかぶれてる？）",
            "目の映像がまだ流れていない",
            "アイトラッカーが終了した。sudo eyecam-grab をもう一度実行してね",
            "バッファが更新されなくなった。sudo eyecam-grab をもう一度実行してね",
            "校正の計算が終わらなかった",
            "左目の瞳がうまく見えなかった（HMD のかぶり方を直して、もう一度）",
            "右目を閉じたのが検出できなかった（もう一度、しっかり閉じてね）",
            "左目の細めが測れなかった（もう一度）",
            "右目の細めが浅かった（もう一度、しっかり細めてね）[f_sq 0.91]",
            "左目: 下を見ても目の開きが変わっていない（もう一度、しっかり下を見てね）[1.54 / 普段 1.47]",
            "右目: 下を見たら下まぶたが上に動いた（検出の失敗かも。もう一度）[240.0 px / 普段 250.0 px]",
            "視線の向き（Valve）が取れなかった。frameeyeosc が動いているか確かめて、もう一度",
            "視線と目の開きの関係が求められなかった（もう一度）",
            "protocol_x.txt の書き方がおかしい: line 3",
            "プロトコルが見つからない: protocol_x.txt",
        ];
        for ja in all {
            let en = message_en(ja);
            assert!(!has_japanese(&en), "{ja} -> {en}");
        }
        assert_eq!(message_en("校正できた（かぶり）"), "Calibrated");
        assert_eq!(message_en("保存した: rec_a"), "Saved: rec_a");
        assert_eq!(
            message_en("左目: 下を見ても目の開きが変わっていない（もう一度、しっかり下を見てね）[1.54 / 普段 1.47]"),
            "Left eye: looking down didn't change the eye opening (try again and look down clearly) [1.54 / normal 1.47]"
        );
        assert_eq!(message_en(""), "");
    }
}
