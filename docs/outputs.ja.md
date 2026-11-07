# 送り先

[README に戻る](../README.ja.md)

## Steam Link 標準の OSC との違い

https://github.com/user-attachments/assets/c2acaf2a-b644-4235-91f6-81891d831ba3

SteamVR の Steam Link は、自分でもアイトラッキングを OSC で送れます（`LeftEyeX`、`LeftEyeLid` など）。同じ目の記録で比べると、こうなります（SteamVR 2.18.2）。

| | frameeyeosc 0.7.2 | SteamVR 標準の OSC |
|---|---|---|
| 視線 | 両目を少し寄せられる（「左右の目を別々に動かす」。2 m ほど先を見ているくらい） | 両目共通の 1 本 |
| 視線のなめらかさ | なめらかにする処理あり（強さを選べる） | なし（生の値そのまま） |
| まぶた | ふつうの開き具合を目ごとにそろえる。まばたきを保つ処理・左右をそろえる処理あり | 生の開き具合を裏返しただけ（1/254 刻み） |
| まばたき・ウインク | 出る | 出る |
| 見開き | 出る（目のカメラ。SteamOS 0.4.3 でも） | 出ない |
| 瞳孔の大きさ | 出る（目のカメラ。明るいところで小さく、暗いところで大きく） | 出ない |
| 目を細めた動き | 出る（目のカメラ。ユーザー校正のあと） | 出ない |
| 眉 | 0.8.0 から選べる: 目を見開くと上がる | 出ない |
| 準備 | Frame にインストールが必要（目のカメラはふつう sudo なし。だめなときだけ最初に一度 sudo の手順） | インストール不要。SteamVR の設定と VRCFaceTracking のモジュールだけ |

動画は、目のカメラを使っているときの VRChat のアバターの目です。準備のしかたは [目のカメラ](eyecam.ja.md#目のカメラ) を見てください。

## アバターのパラメーター

VRCFaceTracking の目のパラメータ（`FT/v2/EyeLeftX`、`EyeLidLeft` など）を float で持つアバター。VRChat に直接送るときは、パラメータをビットに詰める「バイナリパラメータ」のアバターには対応していません（目のカメラの瞳孔だけは対応しています。[ビットで受け取るアバターの瞳孔](eyecam.ja.md#ビットで受け取るアバターの瞳孔)）。`EyeTrackingActive` は bool で送ります。これを float で持つアバターは bool が届くと止まるので、送り方タブの「EyeTrackingActive の型」で［Float］を選んでください（`eye_tracking_active`）。［送らない］にすると送りません。SteamVR の Steam Link が自分で送る OSC（`LeftEyeX`、`RightEyeLid` など）に合わせて作ったアバターも、送り方タブの「Steam Link の名前も送る」をオンにすれば動きます（[Steam Link の OSC 向けのアバター](outputs.ja.md#steam-link-の-osc-向けのアバター)）。これらのパラメータを持たないアバターは、VRChat 自身のアイトラッキング入力で目を動かせます（[VRChat のアイトラッキング入力](outputs.ja.md#vrchat-のアイトラッキング入力)）

## VRChat のアイトラッキング入力

`"native_eyes": true`（または `--native-eyes`）にすると、VRChat モードのとき、VRCFT のパラメータに加えて VRChat 自身のアイトラッキング入力も送ります。動くのはアバターの Avatar Descriptor の Eye Look に設定した目とまぶたなので、VRCFT のパラメータを持たないアバターでも、アニメーターに何も足さずに目が動きます。Eye Look を設定済みのアバターなら、アップロードし直す必要もありません。既定ではオフです。送り方タブの「VRChat 標準の目も動かす」で切り替えられます（送り先が「VRChat」のとき）。

- `/tracking/eye/CenterVec`: 送っている視線（なめらかにして、合わせたあとのもの）を向きにしたもの。`independent_eyes` のときは `/tracking/eye/LeftRightVec`
- `/tracking/eye/EyesClosedAmount`: 送っている左右のまぶたを平均した 1 つの値（0 で開く、1 で閉じる）。VRChat が受け取るのは両目で 1 つの値だけで、見開きはありません。ウインクは両目が半分閉じ、見開きはただ開いた目になります。これらを伝えたいときは VRCFT のパラメータを持つアバターを使ってください

VRCFT 向けに作られたアバターでどうなるかは、そのアバターのアニメーター（Tracking Control の Eyes & Eyelids）しだいです。たいていは `EyeTrackingActive` が true のあいだ目がアニメーションに渡され、VRCFT のパラメータに従ったままです。ただ、Eye Look の Eyelids も設定してあるアバターでは、まぶたが 2 倍閉じることがあります。そのアバターでは `native_eyes` をオフにしてください（送り方タブの同じスイッチです）。

これをオンにするときは、SteamVR 自身の Steam Link の OSC はオフのままにしてください。同じ `/tracking/eye/*` に送るので、2 つがぶつかります。

目を見失ったとき、送信を止めたとき、送り先を変えたときは、普通に開いて正面を見ている目を 1 回送ります。この入力には「無効」がなく、VRChat は自分のタイムアウトのあとで目を自動の動きに戻します。

frameeyeosc がこれを送るのは VRChat モードのときだけです。LiveLink・FrameEyeOSC モードでは要りません。VRCFT の目のパラメータを持たないアバターには、VRCFaceTracking 自身が VRChat のアイトラッキング入力を送ります。

アバター側は、Unity で Eye Look を設定しておく必要があります（VRC Avatar Descriptor → Eye Look で［Enable］を押す）。視線は動くのにまばたきをしないときは、たいてい Eyelids が設定されていません:

- Eyes: 「Transforms」に左右の目のボーン。「Rotation States」に、Looking Straight / Up / Down / Left / Right で目がどこまで回るか（プレビューで確かめられます）
- Eyelids: 「Eyelid Type」を Blendshapes に（まぶたをボーンで動かすアバターは Bones）、「Eyelids Mesh」に顔のメッシュ、「Blink」に目を閉じるブレンドシェイプ（`vrc.blink`、`blink`、`Eye_Close` などの名前が多い）

ここを変えたら、アバターをアップロードし直してください。

## VRChat の Face Tracking

`"vrc_face_tracking": true`（または `--vrc-face-tracking`）にすると、VRChat モードのとき、VRChat の Face Tracking 入力 `/tracking/face/v1` にも送ります。Unified Expressions のトラッキングキーごとに 1 バイト、102 バイトの blob が 1 つです。動くのは、アバターの Avatar Descriptor の Face Tracking セクションで対応づけたブレンドシェイプと目のボーンです。VRCFT のパラメータも PC の VRCFaceTracking もいりません。`native_eyes` とちがって目ごとに送るので、ウインク・見開き・細め・瞳も伝わります。既定ではオフです。送り方タブの「Face Tracking にも送る」で切り替えられます（送り先が「VRChat」のとき）。VRChat の Face Tracking は新しい機能なので（VRChat の beta ドキュメント、2026 年 10 月）、それに対応した VRChat と SDK が必要です。

目ごとに送るもの:

- `EyeClosed`: 送っているまぶたから（1 で閉じる、普通に開いた目から上は 0）。`EyeWide`: 普通に開いた目を超えた分
- `EyeLookOut` / `In` / `Up` / `Down`: 送っている視線から（「左右の目を別々に動かす」なら目ごとの、それ以外は共通の視線）。1.0 = 45° で、`v2/EyeLeftX` / `Y` と同じ値です
- 目のカメラがあるとき: `EyeSquint`。瞳はいつもより大きければ `EyeDilation`、小さければ `EyeConstrict`
- `brow_from_widen` のとき: `BrowInnerUp` と `BrowOuterUp`

ほかのキーはすべて 0 です。目を見失ったとき、送信を止めたとき、送り先を変えたときは、すべてのキーが 0 の blob（普通に開いて正面を見ている目）を 1 回送ります。

アバター側の設定（Unity で VRC Avatar Descriptor → Face Tracking の［Enable］を押す）:

- Face Tracking のメッシュに顔のメッシュを足します
- 対応づけるのは frameeyeosc が送るキーだけにします。「Auto-Fill Filter」に `eye` と入れて Auto-Fill すると、目のキーが埋まります。選ばれたものは確かめてください（`EyePupil_...` のような名前も拾うことがあります）。None のままのキーは何も動かさないので、口などに送る 0 は何も変えません。手の表情やほかのアニメーターのレイヤーで動く口は、そのまま動きます
- まぶた: 「Eyelid Type」を Face Tracking に
- 視線: 「Eye Gaze Mode」が Bone Rotation なら、EyeLook のキーで目のボーンが回り、1.0 で Eye Look の Rotation States に届きます。Both はボーンに eyeLook のブレンドシェイプが足されるので、目が回りすぎることがあります
- 瞳には「Use All Tracking Keys」が必要です（SDK 3.10.6-beta.2 ではこのチェックが切り替わらないことがあり、Inspector の Debug モードなら変えられます）
- Perfect Sync（ARKit）の顔の眉: `BrowInnerUpLeft` と `BrowInnerUpRight` をどちらも `browInnerUp` に（VRChat は 2 つのうち大きいほうを使います）、`BrowOuterUpLeft` / `Right` を `browOuterUpLeft` / `Right` に
- Avatar Optimizer: Trace and Optimize の「BlendShape を最適化する」がオンだと、Face Tracking セクションでしか使っていないブレンドシェイプが消されます。VRChat のログに `Avatar Face Tracking Blendshape '...' not found on any mesh` と出て、動きません。このオプションをオフにすると戻ります

ここを変えたら、アバターをアップロードし直してください。

こういうアバターでは blob が `/tracking/eye/*` より優先されるので、セクションのないアバターのために `native_eyes` はオンのままで大丈夫です。ただし、blob ごとに Face Tracking の状態がまるごと入れ替わります。`/tracking/face/v1` に送るほかのプログラム（ほかのフェイストラッカー）と frameeyeosc は、アバターが対応づけたすべてのキーで交互に入れ替わるので、送るのはどちらか 1 つにしてください。

## Steam Link の OSC 向けのアバター

SteamVR の Steam Link（SteamVR 2.18）は、Frame の視線とまぶたを自分でも VRChat に送っています。名前は VRCFaceTracking のものと違い、たとえば `FT/v2/EyeLeftX` ではなく `LeftEyeX` です。こちらに合わせて作ったアバターは、frameeyeosc の VRCFaceTracking の名前では動きません。送り方タブの「Steam Link の名前も送る」をオン（`"steamlink_params": true` または `--steamlink-params`）にすると、frameeyeosc がこの名前でも送ります。VRCFaceTracking の名前のあとに、毎回いっしょに送ります。アバターにないパラメータは VRChat が無視するので、もう片方を送っても困りません。送り先が「VRChat」のときだけです。

中身は frameeyeosc の値（なめらかにして、目を合わせた結果やまばたきの保持なども入ったもの）で、SteamVR 2.18.2 で測った Steam Link の決まりに合わせています。

| パラメータ | 型 | 値 |
|---|---|---|
| `LeftEyeX`、`RightEyeX` | float | 視線の左右。1 = 右に 45°（`EyeLeftX` と同じ） |
| `LeftEyeY`、`RightEyeY` | float | 視線の上下。1 = 45°、**下がプラス**（Steam Link が送るとおりで、`EyeLeftY` とは逆） |
| `LeftEyeLid`、`RightEyeLid` | float | 目がどれだけ閉じているか。0 開いている（普通に開いた目と見開き）、1 閉じている（`EyeLidLeft` とは向きが逆） |
| `LeftEyeLidExpandedSqueeze`、`RightEyeLidExpandedSqueeze` | float | 半分より閉じているあいだ 0.0、ほかは 0.8 |
| `LeftEyeSqueezeToggle`、`RightEyeSqueezeToggle` | int | 半分より閉じているあいだ 1、ほかは 0 |
| `LeftEyeWidenToggle`、`RightEyeWidenToggle` | int | いつも 1（Steam Link が送るとおり） |

- 視線は左右の目で同じもの、「左右の目を別々に動かす」がオンなら少し寄せたものです（Steam Link はいつも同じものを送ります）
- 名前に頭は付けません。`prefix` が何でも、Steam Link と同じ `/avatar/parameters/LeftEyeX` で送ります
- Steam Link の `/tracking/eye/...` と `/sl/...`（VRChat 自身のアイトラッキング用）は送りません
- Steam Link の OSC 送信は、インストールのところに書いたとおり OFF のままにしてください。両方が動いていると、同じパラメータを 2 か所から受け取って、アバターの目を取り合ってしまいます

## VRCFaceTracking（LiveLink）モード

frameeyeosc は、VRCFaceTracking の LiveLink モジュールに Live Link Face の形式（Epic の iPhone アプリ Live Link Face と同じ形式）でも送れます。見開きも伝わるので、まぶたを VRCFaceTracking からもらうアバター（バイナリパラメータのアバターも）で見開きが出ます。LiveLink モジュールは VRCFaceTracking のプロジェクトのモジュール（[VRCFaceTracking/LiveLinkTrackingModule](https://github.com/VRCFaceTracking/LiveLinkTrackingModule)）で、frameeyeosc はその一部ではありません。

1. PC に VRCFaceTracking を入れ、モジュールの一覧から「LiveLink」を追加します。目がこのモジュールから来るように、ほかの目のモジュール（FrameEyeOSC など）は止めるか外してください。UDP 11111 番で受けます
2. パネルの基本タブの「送り先」で「VRCFT（LiveLink）」を選ぶか、`"output": "livelink"`（または `--output livelink`）にします。左の列に「VRCFT（LiveLink）→ …」と出て、送り方タブにこの手順が出ます。送り先の PC はいつもどおり（Steam Link の相手か固定）、ポートは 11111 です
3. Windows Defender ファイアウォールで UDP 11111 番を受けられるようにします。許可がないと何も届きません。つまずきやすいところが 2 つあります:
   - LiveLink モジュールは `VRCFaceTracking.exe` ではなく `VRCFaceTracking.ModuleProcess.exe` の中で動きます。VRCFaceTracking を許可するだけでは届きません
   - Steam Link の付属無線アダプタを使うと、そのネットワークは「識別されていないネットワーク」になり、Windows では「パブリック」扱いです。「プライベート」だけの規則では届きません

   ポート番号で規則を作れば両方に効きます（管理者の PowerShell で）:

   ```powershell
   New-NetFirewallRule -DisplayName "VRCFT LiveLink (UDP 11111)" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 11111 -RemoteAddress LocalSubnet -Profile Private,Public
   ```

注意:

- 送るのは左右それぞれのまぶた・見開き・視線です（ARKit の EyeBlink と EyeWide、目の yaw と pitch）。VRCFaceTracking のまぶたは VRChat モードと同じ値（0 閉じ、0.75 普通に開いた目、1 見開き）になり、視線も同じです。目を細める・口・頭は 0 で送ります。眉も 0 ですが、「見開きで眉も上げる」（`brow_from_widen`）がするのときは BrowInnerUp と BrowOuterUpLeft / Right に入れます。目のカメラを使っているときは、まぶたと見開きにその値が入ります
- LiveLink モジュールは瞳孔を運びません。目のカメラを使っているときは、瞳孔の大きさだけ同じ PC の VRChat に直接送ります（ポート 9000。VRChat モードと同じ `v2/Pupil…` のパラメータで、`prefix` も付きます。1 秒に最大 50 回）。送り方タブの「瞳孔は VRChat に直接送る」（`pupils_to_vrchat`）で止められます
- モジュールは何もなめらかにしないので、frameeyeosc のなめらかさの設定がそのまま効きます
- 送るのは 1 秒に 50 回まで（いつもいちばん新しい値）です。モジュールは 10〜16 ms に 1 つしか読まないので、目のデータを全部（毎秒 90 回以上）送ると、どんどん遅れていきました
- VRCFaceTracking は最後に受け取った値を持ち続けます。なので目のデータが止まったとき（ヘッドセットを外したとき）や、送信を止めたとき・送り先の種類を変えたときは、普通に開いて正面を見た目を 1 回送ります。送信中で目のデータがない間は、それを 1 秒に 2 回送り続けます。モジュールは VRCFaceTracking が読み込んでから 180 秒以内に何か届かないと動き出さないためです（あきらめてしまったら VRCFaceTracking でモジュールを読み込み直してください）。一時停止中は何も送りません
- `eye_tracking_active` と `steamlink_params` は関係ありません。`prefix` も、上の瞳孔にだけ使います。アバターのパラメータは VRCFaceTracking が送ります

## VRCFaceTracking（FrameEyeOSC）モード

FrameEyeOSC は frameeyeosc 専用の VRCFaceTracking モジュールです（リポジトリは別の VRCFT-FrameEyeOSC）。PC に別に入れます。LiveLink モードと同じく見開きが伝わり、さらに目のカメラの細めと瞳孔の大きさも運びます。瞳孔も VRCFaceTracking がアバターごとに合った形（小数か、ビットに詰めた形か）で送るので、アバターごとに「瞳孔の受け取り方」を選ぶ必要がありません。

1. PC で、モジュールの zip（`FrameEyeOSC_<版>.zip`）を[リリースのページ](https://github.com/sasaken1102r/VRCFT-FrameEyeOSC/releases)から落とします。VRCFaceTracking の「Module Registry」で［Install from .zip］を押し、その zip を選びます（モジュールの一覧にはまだありません）。ほかの目のモジュール（LiveLink など）は止めるか外してください。UDP 11112 番で受けます（インストーラーを使わないなら、zip を開いて `FrameEyeOSC.dll` を `%APPDATA%\VRCFaceTracking\CustomLibs` に入れ、VRCFaceTracking を起動し直します）
2. パネルの基本タブの「送り先」で「VRCFT（FrameEyeOSC）」を選ぶか、`"output": "frameeyeosc"`（または `--output frameeyeosc`）にします。左の列に「VRCFT（FrameEyeOSC）→ …」と出て、送り方タブにこの手順が出ます。送り先の PC はいつもどおり（Steam Link の相手か固定）、ポートは 11112 です
3. Windows Defender ファイアウォールで UDP 11112 を通します。上の LiveLink と同じ注意があります（このモジュールも `VRCFaceTracking.ModuleProcess.exe` の中で動きます）:

   ```powershell
   New-NetFirewallRule -DisplayName "VRCFT FrameEyeOSC (UDP 11112)" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 11112 -RemoteAddress LocalSubnet -Profile Private,Public
   ```

注意:

- 1 サンプルを小さな UDP パケット 1 つ（76 バイト。中身は [docs/protocol.md](protocol.md)）で送ります: 左右それぞれの視線、まぶた（VRChat モードと同じ 0 閉じ、0.75 普通に開いた目、1 見開き）、目のカメラを使っているときは細めと瞳孔（mm と開き具合）。VRCFaceTracking のまぶた・視線・細め・瞳孔のパラメータは VRChat モードと同じ値になります
- 目のデータは全部（毎秒 90 回以上）送ります。モジュールは届いた分をまとめて読んでいちばん新しい値を使うので、遅れていきません
- このモードでは瞳孔を VRChat に直接は送らないので、`pupils_to_vrchat` と `pupil_bits` は関係ありません。`prefix`、`eye_tracking_active`、`steamlink_params` も関係ありません
- モジュールは何もなめらかにしないので、frameeyeosc のなめらかさの設定がそのまま効きます
- 送信中で目のデータがない間（ヘッドセットを外したときなど）は、そのことを 1 秒に 2 回モジュールに伝え、モジュールは普通に開いて正面を見た目にします。1 秒何も届かないとき（frameeyeosc を止めたときなど）も同じです。目のデータが止まったときや、送信を止めたとき・送り先の種類を変えたときは、すぐに伝えます

## ほかのアプリ

本体の送り先（VRChat、LiveLink、FrameEyeOSC）のほかに、同じ目のデータをあと 3 つまでのアプリに送れます。FrameEyeOSC モジュールを入れた 2 台目の VRCFaceTracking、中継するアプリ、VRChat の目の入力を読む道具（視線に合わせて描き分ける描画など）、VRCFaceTracking のパラメーター名を読むアプリです。設定ファイルの `extra_outputs` に 1 つずつ書きます:

```json
{
  "extra_outputs": [
    { "name": "Bridge", "format": "feo1", "host": "192.168.0.10", "port": 11113 },
    { "name": "Gaze", "format": "osc", "host": "main", "port": 9015 }
  ]
}
```

| キー | 既定値 | 内容 |
|---|---|---|
| `name` | `"App 1"`、`"App 2"`、… | パネルとログに出すだけの名前。表示できる半角英数字と記号で 16 文字まで |
| `enabled` | `true` | `false` で、書いたまま送らない |
| `format` | （必須） | `"feo1"` か `"osc"`（下） |
| `host` | （必須） | `"main"` は本体の送り先と同じ PC（本体が `"auto"` なら Steam Link の PC）、`"auto"` は Steam Link の PC。それ以外は IP アドレスかホスト名（ポートは付けない） |
| `port` | （必須） | 1〜65535 |
| `bundle` | `false` | `osc` のときだけ: 1 サンプルぶんのメッセージを、1 つずつ別のパケットにせず OSC バンドルにまとめます（タイムタグは「すぐ」、1 つ 1472 バイトまでなので、1 サンプルは UDP パケット 1〜2 つ）。Frame の中で動かす Windows 版 VRChat のように、パケット 1 つごとの処理が重い受け手向けです |
| `max_rate` | `null` | `osc` のときだけ: 1 秒に送るサンプルの数の上限（1〜1000。目のデータは 1 秒に約 90）。超えたぶんは飛ばします。`null` は全部送ります。目のデータが止まったときの最後のメッセージは必ず送ります |

形式は 2 つです:

- `feo1`: FrameEyeOSC のパケット（[docs/protocol.md](protocol.md)）。本体が何を送っていても、FrameEyeOSC モードが送るのと 1 バイトも違わないものです。視線とまぶた、目のカメラを使っていれば細めと瞳孔の大きさ、`brow_from_widen` なら眉も入ります。パケットは 1 回だけ作って、この形式の送り先すべて（と FrameEyeOSC モードの本体）に、同じ通し番号で送ります。目のデータがない間は、FrameEyeOSC モードと同じく「追っていない」を 1 秒に 2 回送ります
- `osc`: ふつうの OSC メッセージ（UDP パケット 1 つに 1 つ。`bundle` ならバンドルにまとめます）。VRChat の目の入力や VRCFaceTracking のパラメーター名を読むアプリ向けです。サンプルごとに、まず VRChat の目の入力として `/tracking/eye/CenterVec`（両目まとめた視線を向きにしたもの。「左右の目を別々に動かす」のときも同じ）と `/tracking/eye/EyesClosedAmount`（両目のまぶたをまとめた 1 つの値。0 開き、1 閉じ）。続けて VRCFaceTracking の v2 のアバターパラメーターを、VRChat モードが送るのと同じ値で、`prefix` の設定にかかわらずいつも `/avatar/parameters/FT/v2/` の下に送ります: `EyeLeftX`/`Y`、`EyeRightX`/`Y`、`EyeLidLeft`/`Right`、`EyeX`/`Y` はいつも、細めと瞳孔（目のカメラを使っているとき）と眉（`brow_from_widen` のとき）はあるときだけ。`EyeTrackingActive`、瞳孔のビット、Steam Link の名前、フェイストラッキングのデータは送りません。目のデータが止まったら、普通に開いて正面を見た目を 1 回送り（細めと眉は 0 に戻します）、データが戻るまで何も送りません。アドレスの一覧は [docs/protocol.md](protocol.md#other-formats-osc) にあります。あとのバージョンでアドレスが増えることはありますが、今あるものは変えません

視線は本体が送るのと同じ値です（目合わせと視線の倍率のあと、1.0 = 45°）。

- 使うのは 3 つまでです。4 つ目から先は送りません
- 書き間違いや知らない形式、この frameeyeosc の知らないキー（新しい frameeyeosc が書いたものなど）がある項目は送らず、どの項目かをエラーに出します（`extra_outputs entry 2: …`。パネルと状態ファイルに出ます）。ファイルのほかの部分はそのまま使います。知らないキーで項目を止めるのはわざとです: 新しいキーは送るものを減らす設定かもしれず、それを読み飛ばした古い frameeyeosc は、選んだより多くを送ってしまいます。`feo1` の項目に `bundle` か `max_rate` を書いても止まります（`false` と `null` なら大丈夫）。パネルではこの 2 つは設定しません。パネルで項目を `feo1` に変えると消えます
- 本体の送り先と同じ住所・ポートのものは送りません（"same as main"）。VRChat に直接送る瞳孔と同じもの（"same as the pupils"）、前の項目と同じもの（"duplicate"）も送りません
- `lan_only` は本体と同じくそれぞれに効きます（[設定](settings.ja.md#設定)）
- 一時停止（`sending`）すると、これも止まります。目のデータが来ている間に、オフにしたり送り先や形式を変えたりすると、前の送り先に最後のメッセージ（「追っていない」か、正面を見た目）を先に送ります
- 本体の目がすでに届いている VRChat に `osc` を向けないでください（直接でも、VRCFaceTracking の LiveLink や FrameEyeOSC 経由でも）。`osc` もアバター（と VRChat 標準の目）を動かすので、2 つがぶつかります。Steam Link 自身の目の OSC が届くポートに送るのも同じで、1 つのポートに送り手が 2 つになります。どちらかを止めてください。同じように、本体が送っている VRCFaceTracking へも中継するアプリに、`feo1` で送らないでください
- それぞれの送り始め・止めたとき・断ったときは、住所と形式つきでログに出ます（`journalctl --user -u frameeyeosc`）。状態ファイルにも並びます（`extras`）

コマンドラインでは、`--extra 形式@ホスト:ポート`（3 回まで）でファイルのリストをまるごと置き換えます。名前は "App 1"、"App 2"、… になり、ファイルからは変えられなくなります。`--no-extras` なら、ファイルに何が書いてあってもほかのアプリには送りません:

```sh
FRAMEEYEOSC_ARGS="--extra feo1@192.168.0.10:11113 --extra osc@main:9015"
```
