# frameeyeosc

Steam Frame のアイトラッキング（視線とまぶたの開き具合）を、VRCFaceTracking 形式のアバターパラメータとして OSC で VRChat に送るツールです。ヘッドセット上でバックグラウンドのサービスとして動き、Steam Link でストリーミングしている PC 版 VRChat で使えます。PC の VRCFaceTracking に送って、ほかのトラッカーとまとめることもできます。

[English](README.md)

https://github.com/user-attachments/assets/f8969485-161b-40d4-b9e4-689dee6d1955

[konsti219/frameeyeosc](https://github.com/konsti219/frameeyeosc) をフォークしたものです。Steam Frame のまぶたのデータは公開 API からは取れないのですが、元のプロジェクトが内部の共有メモリ（`/dev/shm/eye-server.mmap`）から読めることを見つけてくれたおかげで、このツールを作ることができました。

目のカメラの映像の取り出し方は、Curtis English さんの [FrameEyeCameraFeed](https://github.com/Curtis-VL/FrameEyeCameraFeed)（MIT）から移植しました。

## このフォークで足したもの

- 送り先の PC は自動で見つけます。Steam Link でつないでいる PC に送るので、アドレスを調べて設定する必要はありません。付属の無線アダプタを使っているときはその直通回線で送るので、家のネットワークの状態にも左右されません
- 視線とまぶたを One Euro フィルタでなめらかにしています。何かを見つめているときの細かい揺れは小さなデッドゾーンで止め、目を閉じている間は視線を固定します。Frame は目を開けた瞬間に視線が跳ねるためです
- 両目に同じ視線を送ります。Frame は左右の目の視線がそれぞれ勝手に揺れるので、そのまま送るとアバターの目がピクピクします。左右別々にしたいときは `--independent-eyes` を付けてください
- まぶたの値を VRCFT の基準（0 で閉じる、0.75 で普通、1 で見開き）に合わせています。Frame の値は、目を閉じ続けても 0.2 前後までしか下がらず、普通に開いているときも 0.75〜0.9 くらいでふらつきます
- まぶたは使っているうちに自動で調整されます。左右それぞれの普段の開き具合を覚えるので、顔や被り方のせいで片目だけ開いて見える人でも、アバターでは揃って見えます。ウインクはそのまま伝わります
- サービスとして常駐し、SteamVR と一緒に起動します。止まっても自動で再起動します
- 設定はファイルに置き、動いたまま反映します。SteamVR のダッシュボードに出すパネル（入れなくてもよい）で、被ったまま変えられます
- VRCFaceTracking 用の ETVR Tracking Module が読む形式でも送れます（[VRCFaceTracking（ETVR）モード](#vrcfacetrackingetvrモード)）。VRCFaceTracking の LiveLink モジュール向けに Live Link Face の形式でも送れて、こちらは見開きも伝わります（[VRCFaceTracking（LiveLink）モード](#vrcfacetrackinglivelinkモード)）
- 使いたい人は、Frame の目のカメラも使えます（一緒に入る道具 eyecam を使います）。SteamOS 0.4.3 でも見開きが届くようになり、目を細めた動きと瞳孔の大きさも送れます。パネルで最初に一度だけ準備がいります（[目のカメラ](#目のカメラ)）

## 必要なもの

- 開発者モードを有効にして SSH で入れる Steam Frame（設定 → システム → 開発者モードを有効化、開発者の項目でパスワードを設定）。SSH を有効にすると、同じネットワークにいてパスワードを知っている人は誰でもヘッドセットに入れるので、推測されにくいパスワードにしてください
- Steam Link でストリーミングしている PC 版 VRChat（Action Menu → Options → OSC → Enabled）
- VRCFaceTracking の目のパラメータ（`FT/v2/EyeLeftX`、`EyeLidLeft` など）を float で持つアバター。VRChat に直接送るときは、パラメータをビットに詰める「バイナリパラメータ」のアバターには対応していません（目のカメラの瞳孔だけは対応しています。[ビットで受け取るアバターの瞳孔](#ビットで受け取るアバターの瞳孔)）。`EyeTrackingActive` は bool で送ります。これを float で持つアバターは bool が届くと止まるので、送り方タブの「EyeTrackingActive の型」で［Float］を選んでください（`eye_tracking_active`）。［送らない］にすると送りません。SteamVR の Steam Link が自分で送る OSC（`LeftEyeX`、`RightEyeLid` など）に合わせて作ったアバターも、送り方タブの「Steam Link の名前も送る」をオンにすれば動きます（[Steam Link の OSC 向けのアバター](#steam-link-の-osc-向けのアバター)）。これらのパラメータを持たないアバターは、VRChat 自身のアイトラッキング入力で目を動かせます（[VRChat のアイトラッキング入力](#vrchat-のアイトラッキング入力)）
- VRCFaceTracking（ETVR）モードで使うときは、PC に VRCFaceTracking と ETVR Tracking Module。LiveLink モードなら VRCFaceTracking と LiveLink モジュール

## インストール

### いちばんかんたん：Frame の中だけで入れる（おすすめ）

PC は要りません。Frame の Konsole（画面下のバーの ＋ →「プログラムを起動」→ Konsole）で次のコマンドを入力して Enter を押し、メニューで **1**（frameeyeosc）を選びます。ダッシュボードのパネルも入れるかは、途中で聞かれます。

```sh
curl -fsSL https://frame.sasaken1102s.net | sh
```

- 最初の 1 回だけ、Steam 設定 → システム →「開発者モードを有効化」をオンにしておきます（オフだと ＋ の一覧に Konsole が出ません）
- ほかのアプリ（frame-jp-keyboard・frame-mic-tuner・frame-perf-overlay）も同じメニューから一緒に入れられます
- 更新は、同じコマンドで同じ番号を選ぶだけ。アンインストールはメニューの `u` から
- くわしい手順と動画：https://frame.sasaken1102s.net
- 質問なしで入れるなら `curl -fsSL https://frame.sasaken1102s.net | sh -s -- install eye`

入るもの・オプションは、下の「PC から入れる」と同じです（中で `install.sh` を実行しています）。インストールしたら、下の Steam Link の OSC 送信を OFF にするのも忘れずに。

### PC から入れる

リリースページから tar.gz をダウンロードして、ヘッドセットにコピーします。PC からなら例えば:

```sh
scp frameeyeosc-*-steamframe-aarch64.tar.gz steamos@<ヘッドセットのIP>:
```

そのあとヘッドセット上で（`ssh steamos@<ヘッドセットのIP>`）:

```sh
tar xzf frameeyeosc-*-steamframe-aarch64.tar.gz
cd frameeyeosc
./install.sh               # frameeyeosc だけ
./install.sh --with-panel  # frameeyeosc とダッシュボードのパネル
```

sudo は要りません。全部ホームフォルダ（`~/.local/bin`、`~/.local/lib/eyecam`、`~/.config`、`~/.local/share`）に入るので、SteamOS を更新しても消えません。更新するときも同じコマンドです。`--with-panel` を付けないときは、入っているパネルはそのまま残ります。

リリースには目のカメラの道具 eyecam も入っていて、`~/.local/lib/eyecam` に入ります。初めて入れたときに、ユーザーサービス（`eyecam.service`）として有効にして動かします。更新では、動いていれば再起動するだけで、自分でオフにしたものはオフのままにします（[止める・取り除く](#止める取り除く)）。パネルで目のカメラの準備をするまでは待っているだけで、何も変わりません（[目のカメラ](#目のカメラ)）。sudo がいるのはその準備の 1 回だけで、それも自分で実行します。`install.sh` が sudo を使うことはありません。

インストールしたら、PC 側で Steam Link の OSC 送信を OFF にしてください（SteamVR の設定 → Steam Link → OSC）。Steam Link もスムージングなしの目のデータを VRChat に送っているので、両方が動いているとアバターの目を2つのデータが取り合ってしまいます。ETVR モードと LiveLink モードでも同じです（アバターの目を動かすのが VRCFaceTracking になるだけです）。

#### 0.4.0 より前の版から更新するとき

0.4.0 より前の版には更新の仕組みがないので、0.4.0 へは一度だけ手で更新します。新しい tar.gz を上と同じ手順でコピーして広げ、`./install.sh --with-panel`（パネルがいらなければ `./install.sh`）を実行するだけです。`~/.config/frameeyeosc/env` と学習したまぶたの値はそのまま使われ、サービスも新しい版で起動し直します。

`env` の `FRAMEEYEOSC_ARGS` に書いたオプションは、今までどおり効きます。ただし、そこに書いた項目はパネルでは「コマンドで固定中」になって変えられません。パネルで変えたい項目は `env` から消して、`systemctl --user restart frameeyeosc` してください（値はパネルで設定し直します）。

削除は `./install.sh --uninstall`（パネルも消えます。設定と学習値も消すなら `--purge` を付ける）。

#### パネルから更新する（0.4.0 から）

0.4.0 からは、パネルの「更新する」で更新できます。パネルの「詳細」に、入っている版が出ます。パネルは起動時と、その後 1 日 1 回まで、GitHub に新しい版がないか確かめます。1 日 1 回なのは確認がうまくいっている間で、失敗したときは 1 時間後にもう一度確かめます。「今すぐ確かめる」を押すとその場で確かめます。新しい版があれば、詳細のバージョンの行の下にその版の要約が出ます（日本語の要約があるリリースなら、日本語の画面ではそちら）。「更新する」で、ダウンロードしてリリースの `SHA256SUMS` と照らし合わせ、前回と同じオプション（`~/.config/frameeyeosc/install-args` に残っています）でその `install.sh` を実行します。本体とパネルは新しい版で起動し直します。`install.sh` を実行する前に失敗したときは何も変わりません。ログは `~/.cache/frameeyeosc/update.log` です。毎日の確認は「新しい版の確認」をオフにすると止まります（「今すぐ確かめる」は使えます）。更新そのものは、ボタンを押したときにしか行いません。

`SHA256SUMS` は同じリリースに付いているチェックサムで、署名ではありません。ダウンロードが壊れていたり途中で切れていたりするのは見つけられますが、GitHub 上でリリースごと差し替えられたものは見つけられません（チェックサムも一緒に差し替わるため）。

## パネル

`./install.sh --with-panel` で、SteamVR のダッシュボードに「Eye」のパネルが入ります。次に SteamVR を起動したときから一緒に起動します。すぐ開きたいときは、ダッシュボードの「プログラムを起動」（＋）から「frameeyeosc パネル」を選んでください。

| 基本 | 送り方 |
|---|---|
| ![基本のタブ](docs/images/panel-basic-ja_2026-10-01_02-00-00.png) | ![送り方のタブ](docs/images/panel-output-ja_2026-10-01_02-00-00.png) |
| **視線** | **目を合わせる** |
| ![視線のタブ](docs/images/panel-gaze-ja_2026-10-01_02-00-00.png) | ![目を合わせるタブ](docs/images/panel-eyefit-ja_2026-10-01_02-00-00.png) |
| **まぶた** | **詳細** |
| ![まぶたのタブ](docs/images/panel-lids-ja_2026-10-01_02-00-00.png) | ![詳細のタブ](docs/images/panel-advanced-ja_2026-10-01_02-00-00.png) |

- 左の列には、いつでも今の状態が出ます: 送信中か止めているか、送り先、毎秒の送信回数、目のデータが毎秒何回来ているか（60 未満は赤で「少なめ」。その下に、遅いのが本体か Frame か）、左右のまぶたと視線（生の値と送った値）、設定のエラー。「Track Dominant Eye Only」の設定がオンのあいだは、視線の見出しの行に、Frame がどちらの目で追っているかが出ます（「Frame の設定: 右目だけで追っています」）
- 基本: 送信の一時停止、送り先（3 枚のカード: VRChat に直接・VRCFT（LiveLink、おすすめ）・VRCFT（ETVR）。それぞれ見開きが届くか、ほかの人からの見え方、VRCFaceTracking が要るかを表示）、言語（日本語 / English）、SteamVR と一緒に起動、すべて既定に戻す、アプリを終了
- 送り方: 送り先の PC（自動、今送っている PC で固定、または入力。下を参照）とポート。VRChat に直接のときはパラメーター名の頭、EyeTrackingActive の型、Steam Link の名前も送るか、VRChat 標準の目も動かすか、LiveLink と ETVR のときは PC の VRCFaceTracking で準備すること。LiveLink で目のカメラを使っているときは「瞳孔は VRChat に直接送る」（`pupils_to_vrchat`）も。目のカメラを使っていて VRChat に直接か LiveLink のときは「瞳孔の受け取り方」（`pupil_bits`）も
- 視線: スムージングのオン / オフ、なめらかさの弱 / 中 / 強と 3 つの値、見つめている時の遊び、まばたき中は視線を止める、左右の目を別々に動かす、不確かな視線を使わない、一瞬の途切れを消す
- 目を合わせる: ボタン 1 つで視線とまぶたを約 20 秒で合わせる（[目を合わせる](#目を合わせる) を参照）、正面の合わせ直し、被ったときに自動で合わせ直すもの（「被ったとき」: 何もしない・正面だけ・正面と傾き）、結果と［元に戻す］、「細かく直す」の中で値を手で直す
- まぶた: いまのまぶたがどこから来ているか（目のカメラ・片目だけ目のカメラ・Valve の値）、「見開きの出やすさ」（鈍い〜敏感のスライダー 1 本。目のカメラのときはその感度、Valve の値のときは目を合わせた目の 4 段階（しない / 控えめ / ふつう / 出やすい、`lid_widen`）。SteamOS 0.4.3 で目のカメラがないときは、ふつうに開いた目がもう 1.0 と読まれて見開きが届かないので灰色になり、目のカメラタブへのボタンが出ます）、まばたきを届ける（閉じたまま保つ時間）、まばたきを両目でそろえる（`blink_sync_below`）、まぶたのなめらかさ（弱 / 中 / 強）、左右をそろえる強さ（`lid_sync`）。残りは「そのほか」の［細かく直す］の中: 自動キャリブレーションと覚えた値、左右の倍率、左右の今の開き具合の上に重ねた 4 つの目盛り（目を閉じたり見開いたりしながら合わせる。③④は目を合わせた目とカメラの目では灰色）、なめらかさの 2 つの値
- 目のカメラ: eyecam が動いているあいだ出ます。準備が済むまでは準備のチェックリスト（[目のカメラ](#目のカメラ)）。済んだあとは、いまの状態、「カメラで瞼を取る」のオン / オフ、違和感があるときの「目のカメラの校正（18秒）」、目を細めた動きも送りたいときの「ユーザー校正（最初に 1 回）」、こんなときはどうするか
- 詳細: 版の表示と更新の確認・更新（自動の確認のオン / オフも）と「更新履歴」（版ごとの要約と変更点、新しい順。開くのは 1 つずつで、スティックか ▲ / ▼ でスクロール）、調べる道具（視線の点を表示と点の距離、目のログの記録。下を参照）、ファイルと本体（ファイルの場所、本体の PID、コマンドで固定中の項目）。eyecam が動いているあいだは「開発用」の「目の撮影」も出ます。目の処理を調整するための、目のカメラの録画で、目の映像が `~/eyecam/rec_…/` に残ります（1 回で約 2.5 GB。[panel/README.md](panel/README.md) を参照）

パネルは `config.json` を書き、状態ファイルを読みます。既定の言語を決めるために、起動時に 1 回だけ Steam の `~/.steam/registry.vdf` の `language` の行も読みます（読むだけ）。更新には `~/.local/share/frameeyeosc/frame-update.sh` を使います（上を参照）。閉じても、終了しても、入れていなくても frameeyeosc は送り続けます。ダッシュボードで開いていない間は何も描きません。そのとき読むのは、更新の確認を動かすほかは、更新の状態ファイル（`~/.cache/frameeyeosc/update-state.json`）を 1 秒に 2 回ほどだけです。ただし目を合わせている間は、終わるまで状態ファイルも読み、ダッシュボードを閉じた状態で点を出します。デバッグ用の視線の点をオンにしている間も、そのソケットで受け取り、サンプルが届いている間は 1 秒に約 90 回点を動かします。このときは状態ファイルも読み、`config.json` が変わったかを 1 秒に 10 回確かめます。「被ったとき」が「何もしない」以外で視線を合わせてあるときも、ヘッドセットを被ったのに気づくため、ダッシュボードを閉じている間 0.5 秒ごとに状態ファイルを読みます。「SteamVR と一緒に起動」は、パネルの systemd ユーザーサービス（`frameeyeosc-panel.service`）を有効 / 無効にします。ビルド方法や確認用のオプションは [panel/README.md](panel/README.md) にあります。

### 送り先の PC を手で決める

「自動」は Steam Link の接続先の PC に送ります。違う PC に送っているときは、送り方タブの「送り先の PC」で［IP を入力］を押してください。テンキーが開くので、PC の IPv4 アドレス（例: `192.168.1.20`）を打って［決定］を押します。ポートは付けず、「ポート」の行で設定します。パネルが `config.json` の `host` に書き、行には「手動 192.168.1.20」と出ます。［自動］で元に戻ります。アドレスではなくホスト名にしたいときは、`config.json` の `host` を手で書いてください。行には「手動」と名前が出て、［自動］で戻せるのは同じです。

## 設定

設定は `~/.config/frameeyeosc/config.json` にあります。パネルが書きますが、手で書いてもかまいません。frameeyeosc は 1 秒に 10 回このファイルを見て、変わっていたら再起動せずに反映します。書いていない項目は既定値、知らない項目は無視します。ファイルが壊れていたり値が範囲外だったりしたときは、前の設定のまま動き続け、エラーを出します（パネルと状態ファイルに出ます）。

```json
{ "output": "vrchat", "gaze_min_cutoff": 0.3, "lid_sync": 0.6 }
```

| キー | オプション | 既定値 | 内容 |
|---|---|---|---|
| `sending` | | `true` | `false` で送信を一時停止（VRChat モードでは `EyeTrackingActive` の「無効」を `eye_tracking_active` の型で 1 回送る。LiveLink モードでは普通に開いて正面を見た目を 1 回送る） |
| `output` | `--output` | `"vrchat"` | `"vrchat"` は VRChat にアバターパラメータを送る、`"etvr"` は VRCFaceTracking の ETVR Tracking Module に送る、`"livelink"` は VRCFaceTracking の LiveLink モジュールに Live Link Face の形式で送る |
| `host` | `--target` | `"auto"` | `"auto"` は Steam Link の接続先 PC。それ以外は IP アドレスかホスト名（ポートは付けない） |
| `port` | `--port`、`--target` | `null` | `null` は `vrchat` なら 9000、`etvr` なら 8889、`livelink` なら 11111 |
| `prefix` | `--prefix` | `"/FT"` | パラメータ名の頭。`""` で頭なし。LiveLink モードでは、VRChat に直接送る瞳孔（`pupils_to_vrchat`）にだけ使う |
| `eye_tracking_active` | `--eye-tracking-active` | `"bool"` | VRChat モードで `EyeTrackingActive` をどう送るか: `"bool"`（true / false）、`"float"`（1.0 / 0.0。アバターによってはこちらが必要）、`"off"`（送らない。止めたときや目を見失ったときの 1 回の「無効」も送らない）。ETVR モードと LiveLink モードではもともと送らない |
| `steamlink_params` | `--steamlink-params` | `false` | VRChat モードで、SteamVR の Steam Link が自分の OSC で送るアバターのパラメータ（`LeftEyeX`、`RightEyeLid` など）も送る。それに合わせて作ったアバター用（[Steam Link の OSC 向けのアバター](#steam-link-の-osc-向けのアバター)）。頭（`prefix`）は付けない。ETVR モードと LiveLink モードでは使わない |
| `native_eyes` | `--native-eyes` | `false` | VRChat モードで、VRChat 自身のアイトラッキング入力（`/tracking/eye/*`）も送る。VRCFT のパラメータを持たないアバターの目が動く（[VRChat のアイトラッキング入力](#vrchat-のアイトラッキング入力)）。送り方タブにスイッチがある |
| `camera_lids` | `--no-camera-lids` | `true` | 準備が済んでいれば、目のカメラの値を使う（[目のカメラ](#目のカメラ)）: 普通に開いた目から上のまぶた（見開き）、目を細めた動き、瞳孔の大きさ。`false` で Valve の値だけ。パネルが動いていれば、eyecam-rec にも映像からの計算を止めさせる（`live off`。eyecam 自体は動き続ける。[止める・取り除く](#止める取り除く)）。目のカメラタブの「カメラで瞼を取る」 |
| `pupils_to_vrchat` | `--no-pupils-to-vrchat` | `true` | LiveLink モードで、目のカメラの瞳孔の大きさを VRChat に直接送る（同じ PC のポート 9000）。LiveLink モジュールは瞳孔を運ばないため。ほかのモードでは関係ない |
| `pupil_bits` | `--pupil-bits` | `0` | 目のカメラの瞳孔を VRChat に送るとき、開き具合をこの個数の bool のパラメータ（`v2/PupilDilation1`、`2`、`4`、`8`）でも送る。ビットで受け取るアバター用。`0` で小数だけ。1〜4。[ビットで受け取るアバターの瞳孔](#ビットで受け取るアバターの瞳孔) を参照。送り方タブの「瞳孔の受け取り方」 |
| `raw` | `--raw` | `false` | スムージングしない。時間を使う処理（途切れ消し、視線を止める、品質チェック、閉じたまま保つ、真下で左右を止める）もしない |
| `gaze_min_cutoff` | `--gaze-min-cutoff` | `0.3` | 下げるほど止まっている時の視線が安定（その分遅れる） |
| `gaze_beta` | `--gaze-beta` | `1.5` | 上げるほど素早い視線の動きに遅れず付いていき、動いたあと早く落ち着く |
| `gaze_d_cutoff` | `--gaze-d-cutoff` | `0.5` | 下げるほど、トラッキングのノイズで視線のフィルタがゆるみにくく、素早い目の動きにもパッと飛ばずにやわらかく付いていく |
| `gaze_deadzone` | `--gaze-deadzone` | `0.005` | これより小さい視線の変化は無視（1.0＝45°）。視線は目が止まった所からこの分だけ手前で止まることがある |
| `gaze_hold_below` | `--gaze-hold-below` | `0.5` | どちらかの目の開き具合がこれより小さい間は視線を止める。`0` で無効 |
| `independent_eyes` | `--independent-eyes` | `false` | 共通の視線ではなく、左右それぞれの視線を送る。目を合わせると目ごとの左右も合うので、自然に見える。「Track Dominant Eye Only」の設定がオンのあいだは、どちらでも両目とも追っている目の視線になる |
| `gaze_quality_limit` | `--gaze-quality-limit` | `0`（オフ） | 念のための安全策: Frame が出す視線の不確かさ（共分散）がこれ（例: `0.03`）より大きい目の視線は使わない。片目だけならもう片方の目で両目を動かし、両目ともなら視線を止める。まぶたには影響しない。きちんと合ったヘッドセットでは測って差が出なかった。不確かさが上がるのはほぼ目を閉じかけている間だけで、そこは `gaze_hold_below` がもう視線を止めているため |
| `despike` | `--no-despike` | `true` | 視線と開き具合の 1 サンプルだけの途切れを消す（3 サンプルの中央値。全体が約 11 ms 遅れる） |
| `lid_min_cutoff` / `lid_beta` | `--lid-min-cutoff` / `--lid-beta` | `6.0` / `5.0` | まぶたのなめらかさ（視線と同じ考え方） |
| `lid_closed` / `lid_open` / `lid_widen_start` / `lid_wide` | `--lid-closed` など | `0.30` / `0.80` / `0.92` / `1.00` | Frame の開き具合を「閉じ／普通／見開き」にどう対応させるか |
| `lid_widen` | `--lid-widen` | `"normal"` | 目を合わせた目の見開きやすさ: `"off"`・`"low"`・`"normal"`・`"high"`（[目を合わせる](#目を合わせる) を参照）。目を合わせていない目は `lid_widen_start` / `lid_wide` を使う |
| `lid_scale_left` / `lid_scale_right` | `--lid-scale-left` / `--lid-scale-right` | `null`（学習値） | 学習値の代わりに固定の倍率を使う。目を合わせた目では、合わせたあとの微調整になる: 0.9 でその目が 1 割閉じ気味に読まれる（閉じやすく、見開きにくくなる）。`null` は 1.0。目を合わせ直すと（全部のとき）と［元に戻す］で `null` に戻る |
| `lid_calibration` | `--no-lid-calibration` | `true` | まぶたを学習する |
| `lid_sync` | `--lid-sync` | `0.4` | 左右のまぶたの小さな差を揃える。大きな差（ウインク）はそのまま。`0` で無効 |
| `blink_hold_ms` | `--blink-hold-ms` | `80` | 目が閉じたら、少なくともこの時間は完全に閉じた値を送る。短いまばたきもほかの人に届くように。`0` で無効 |
| `blink_sync_below` | `--blink-sync-below` | `0.35` | 片目が閉じていて、もう片方がこれより小さい（VRCFT の値）とき、両目とも閉じて送る。もう片方が開いているウインクはそのまま。`0` で無効 |
| `gaze_offset_x` / `gaze_offset_y` | `--gaze-offset-x` / `--gaze-offset-y` | `0` / `0` | 正面とみなす視線。-0.5〜0.5（1.0＝45°、＋ は右・上）。目を合わせると決まる |
| `gaze_gain_x` / `gaze_gain_up` / `gaze_gain_down` | `--gaze-gain-x` / `--gaze-gain-up` / `--gaze-gain-down` | `1.0` | そこから左右・上・下にどれだけ動かすか。0.5〜2。目を合わせると決まる |
| `gaze_roll_deg` | `--gaze-roll-deg` | `0` | ヘッドセットが何度傾いているか（-20〜20。＋ は右を見ると上に読む向き）。正面を中心に、ゲインの前に傾きを戻す。左右の目と両目の視線どれにも効く。目を合わせると決まる |
| `gaze_offset_x_left` / `_right`、`gaze_gain_x_left` / `_right` | `--gaze-offset-x-left` など | `null` | 目ごとの左右の 0 点と幅。目ごとの視線（`independent_eyes`）に使う。目を合わせると、2 m 先を見るときそれぞれの目が本当に向く角度（少し寄り目になる）に合うよう決まる。`null` = `gaze_offset_x` / `gaze_gain_x` を使う。上下は Frame が両目で共有しているので目ごとの値はない |
| `gaze_down_hold_x_deg` | `--gaze-down-hold-x-deg` | `24` | 真下を見ると、Frame の左右の視線が跳ねます（右へ 19° くらい）。この角度より下を見ている間は、左右の視線（両目とまとめた視線）を、その手前の値へ寄せます（さらに 10° 下で完全に止める）。角度はトラッカーのそのままの値（正面の位置・幅をかける前）。上下は変えない。`0` で無効 |
| `gaze_debug_dots` | | `false` | デバッグ用: 送っている視線の向きに、1 m 先（`gaze_debug_dots_distance_m`）に小さい点を出す（`independent_eyes` のときは目ごとに、それぞれの目から。左は水色、右はオレンジ）。アバターに届いている視線が見えます。オンの間は、frameeyeosc が処理したサンプルを 1 つずつ状態ファイルのフォルダの Unix ソケットでパネルに渡します。ヘッドセットの外には出さず、オフのときは渡しません。目を合わせている間は隠します |
| `lid_fit_closed_left` 〜 `lid_fit_down_right` | | `null` | 目ごとの Frame の開き具合: 目を閉じたとき、上・正面・下を見て開いているとき（`closed` / `up` / `open` / `down`、`_left` / `_right`）。目を合わせると決まる。`null` = まだ合わせていない。合わせた目は、自動キャリブレーションの代わりにこれを使い、下を見ただけでは閉じない。`lid_scale_*` はそのあとの微調整になる |
| `calibration_reset` | | `0` | 増やすと、まぶたの学習をやり直す |
| `language` | | Steam の言語 | パネルの言語。`"ja"` か `"en"`。書いていないときは、Steam の言語が日本語なら日本語、それ以外なら英語 |
| `gaze_debug_dots_distance_m` | | `1.0` | 視線の点を何 m 先に出すか（0.3〜2.0 m。詳細タブの「点の距離」）。ダッシュボードを開いても閉じても同じ。1.2 m くらいより奥だと、開いたダッシュボードに隠れる。パネルが使い、frameeyeosc 本体は使わない |
| `fit_sounds` | | `true` | 目を合わせている間、パネルが短い音を鳴らす。frameeyeosc 本体は使わない |
| `auto_recenter` | | `"center"` | 視線を合わせてあるとき、ヘッドセットを被るたびにパネルが 1 回だけ自動で合わせ直すもの: `"center"` 正面だけ（点 1 つ・2.5 秒）、`"tilt"` 正面と傾き（正面・上・下の点、約 7.5 秒）、`"off"` 何もしない。［もう一度合わせる］の隣のボタンも同じものを手で始める（`"off"` のときは正面だけ）。frameeyeosc 本体は使わない |
| `update_check` | | `true` | パネルが起動時と 1 日 1 回（確認に失敗したときは 1 時間後）、GitHub に新しい版がないか確かめる。frameeyeosc 本体は使わない |

コマンドラインのオプションは、このファイルより優先されます。オプションは `~/.config/frameeyeosc/env` に書き、`systemctl --user restart frameeyeosc` で反映します:

```sh
FRAMEEYEOSC_ARGS="--gaze-min-cutoff 0.3 --lid-sync 0.6"
```

ここで指定した項目はファイルからは変えられず、パネルでは「コマンドで固定中」と出ます。すべてのオプション（別の設定ファイルを使う `--config` など）は `~/.local/bin/frameeyeosc --help` で確認できます。

## VRChat のアイトラッキング入力

`"native_eyes": true`（または `--native-eyes`）にすると、VRChat モードのとき、VRCFT のパラメータに加えて VRChat 自身のアイトラッキング入力も送ります。動くのはアバターの Avatar Descriptor の Eye Look に設定した目とまぶたなので、VRCFT のパラメータを持たないアバターでも、アニメーターに何も足さずに目が動きます。Eye Look を設定済みのアバターなら、アップロードし直す必要もありません。既定ではオフです。送り方タブの「VRChat 標準の目も動かす」で切り替えられます（送り先が「VRChat」のとき）。

- `/tracking/eye/CenterVec`: 送っている視線（なめらかにして、合わせたあとのもの）を向きにしたもの。`independent_eyes` のときは `/tracking/eye/LeftRightVec`
- `/tracking/eye/EyesClosedAmount`: 送っている左右のまぶたを平均した 1 つの値（0 で開く、1 で閉じる）。VRChat が受け取るのは両目で 1 つの値だけで、見開きはありません。ウインクは両目が半分閉じ、見開きはただ開いた目になります。これらを伝えたいときは VRCFT のパラメータを持つアバターを使ってください

VRCFT 向けに作られたアバターでどうなるかは、そのアバターのアニメーター（Tracking Control の Eyes & Eyelids）しだいです。たいていは `EyeTrackingActive` が true のあいだ目がアニメーションに渡され、VRCFT のパラメータに従ったままです。ただ、Eye Look の Eyelids も設定してあるアバターでは、まぶたが 2 倍閉じることがあります。そのアバターでは `native_eyes` をオフにしてください（送り方タブの同じスイッチです）。

これをオンにするときは、SteamVR 自身の Steam Link の OSC はオフのままにしてください。同じ `/tracking/eye/*` に送るので、2 つがぶつかります。

目を見失ったとき、送信を止めたとき、送り先を変えたときは、普通に開いて正面を見ている目を 1 回送ります。この入力には「無効」がなく、VRChat は自分のタイムアウトのあとで目を自動の動きに戻します。

frameeyeosc がこれを送るのは VRChat モードのときだけです。ETVR モードと LiveLink モードでは要りません。VRCFT の目のパラメータを持たないアバターには、VRCFaceTracking 自身が VRChat のアイトラッキング入力を送ります。

アバター側は、Unity で Eye Look を設定しておく必要があります（VRC Avatar Descriptor → Eye Look で［Enable］を押す）。視線は動くのにまばたきをしないときは、たいてい Eyelids が設定されていません:

- Eyes: 「Transforms」に左右の目のボーン。「Rotation States」に、Looking Straight / Up / Down / Left / Right で目がどこまで回るか（プレビューで確かめられます）
- Eyelids: 「Eyelid Type」を Blendshapes に（まぶたをボーンで動かすアバターは Bones）、「Eyelids Mesh」に顔のメッシュ、「Blink」に目を閉じるブレンドシェイプ（`vrc.blink`、`blink`、`Eye_Close` などの名前が多い）

ここを変えたら、アバターをアップロードし直してください。

## 状態ファイル

frameeyeosc は 1 秒に 10 回、今の様子を `$XDG_RUNTIME_DIR/frameeyeosc/status.json`（ふつうは `/run/user/1000/frameeyeosc/status.json`）に書きます。中身は、送信中か、送り先、毎秒の送信回数、目のトラッカーから毎秒届くサンプルの数（`tracker_rate`）、frameeyeosc がそれに追いついているか（`missed_rate`: 直近 1 秒に目のトラッカーが出したのに読めなかった数、`max_processing_ms`: 直近 1 秒でサンプル 1 つにいちばん長くかかった時間、`dropped_rate`: 直近 1 秒にネットワークが混んでいて捨てた送信の数）、最新の生の値と送った値、キャリブレーション、今効いている設定、コマンドで固定中の項目、設定のエラー、目のデータを読めないときはその理由（`source_error`）、「Track Dominant Eye Only」の設定がオンのあいだ Frame が追っている目（`dominant_eye`: `"left"` か `"right"`。オフなら `null`）、ふつうに開いた目が 1.0 と読まれて見開きが届かないかどうか（`openness_saturated`）、目のカメラ（`camera`: 値が届いているか、目ごとに使っているか `used` / `pupil_used`、読めないときはその理由。eyecam が動いていなければ `null`）、LiveLink モードで瞳孔を送っている先（`pupil_target`）、最後の目合わせの測定です。パネルはこれを読んで表示します。フォルダは本人しか読めず、メモリの上にあって再起動すると消えます。残るのは最新の値だけです。

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

- 視線は左右の目で同じもの、「左右の目を別々に動かす」がオンならそれぞれの目のものです（Steam Link はいつも同じものを送ります）
- 名前に頭は付けません。`prefix` が何でも、Steam Link と同じ `/avatar/parameters/LeftEyeX` で送ります
- Steam Link の `/tracking/eye/...` と `/sl/...`（VRChat 自身のアイトラッキング用）は送りません
- Steam Link の OSC 送信は、インストールのところに書いたとおり OFF のままにしてください。両方が動いていると、同じパラメータを 2 か所から受け取って、アバターの目を取り合ってしまいます

## VRCFaceTracking（ETVR）モード

frameeyeosc は、VRCFaceTracking 用の ETVR Tracking Module が読む形式で送れます。アバターを動かすのは VRCFaceTracking になるので、口のトラッカーなど、ほかのトラッカーと 1 つにまとめられます。ETVR Tracking Module は別のプロジェクトのモジュール（[EyeTrackVR/ETVRTrackingModule](https://github.com/EyeTrackVR/ETVRTrackingModule)）で、frameeyeosc はその一部ではありません。

1. PC に VRCFaceTracking を入れ、モジュールの一覧から ETVR Tracking Module を追加します。既定では UDP 8889 番で受けます
2. パネルの基本タブの「送り先」で「VRCFT（ETVR）」を選ぶか、`"output": "etvr"`（または `--output etvr`）にします。送り先の PC はいつもどおり（Steam Link の相手か固定）、ポートは 8889 です

注意:

- 送るのは `EyeLeftX`・`EyeLeftY`・`EyeRightX`・`EyeRightY`・`EyeLidLeft`・`EyeLidRight` の 6 個です。`EyeX` / `EyeY` は送りません。これを受け取るとモジュールが片目用の読み方に切り替わり、送っていないまぶたの値を読むので、まぶたが開いたまま動かなくなるためです
- モジュールはまぶたの 1.0 を「普通に開いた目」として扱うので、このモードでは見開きは伝わりません（1.0 で止めます）
- モジュールもまぶたを自分でなめらかにしています。パネルで切り替えると、frameeyeosc 側のまぶたのなめらかさを弱めるか聞かれます。そのなめらかさのせいで、`blink_hold_ms` の間閉じて送ってもアバターでは閉じきらないことがあります。短いまばたきが半目に見えるときは、120 くらいに上げてください
- VRCFaceTracking を起動してからモジュールの準備ができるまで、2 分近くウィンドウが「応答なし」になることがあります。壊れてはいないので、そのまま待ってください
- PC で UDP 8889 番の受信が許可されている必要があります。VRCFaceTracking の ModuleProcess には、たいてい最初から受信の許可が入っています

## VRCFaceTracking（LiveLink）モード

frameeyeosc は、VRCFaceTracking の LiveLink モジュールに Live Link Face の形式（Epic の iPhone アプリ Live Link Face と同じ形式）でも送れます。ETVR モードと違って見開きも伝わるので、まぶたを VRCFaceTracking からもらうアバター（バイナリパラメータのアバターも）で見開きが出ます。LiveLink モジュールは VRCFaceTracking のプロジェクトのモジュール（[VRCFaceTracking/LiveLinkTrackingModule](https://github.com/VRCFaceTracking/LiveLinkTrackingModule)）で、frameeyeosc はその一部ではありません。

1. PC に VRCFaceTracking を入れ、モジュールの一覧から「LiveLink」を追加します。目がこのモジュールから来るように、ほかの目のモジュール（ETVR Tracking Module など）は止めるか外してください。UDP 11111 番で受けます
2. パネルの基本タブの「送り先」で「VRCFT（LiveLink）」を選ぶか、`"output": "livelink"`（または `--output livelink`）にします。左の列に「VRCFT（LiveLink）→ …」と出て、送り方タブにこの手順が出ます。送り先の PC はいつもどおり（Steam Link の相手か固定）、ポートは 11111 です
3. Windows Defender ファイアウォールで UDP 11111 番を受けられるようにします。許可がないと何も届きません。つまずきやすいところが 2 つあります:
   - LiveLink モジュールは `VRCFaceTracking.exe` ではなく `VRCFaceTracking.ModuleProcess.exe` の中で動きます。VRCFaceTracking を許可するだけでは届きません
   - Steam Link の付属無線アダプタを使うと、そのネットワークは「識別されていないネットワーク」になり、Windows では「パブリック」扱いです。「プライベート」だけの規則では届きません

   ポート番号で規則を作れば両方に効きます（管理者の PowerShell で）:

   ```powershell
   New-NetFirewallRule -DisplayName "VRCFT LiveLink (UDP 11111)" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 11111 -RemoteAddress LocalSubnet -Profile Private,Public
   ```

注意:

- 送るのは左右それぞれのまぶた・見開き・視線です（ARKit の EyeBlink と EyeWide、目の yaw と pitch）。VRCFaceTracking のまぶたは VRChat モードと同じ値（0 閉じ、0.75 普通に開いた目、1 見開き）になり、視線も同じです。目を細める・口・眉・頭は 0 で送ります。目のカメラを使っているときは、まぶたと見開きにその値が入ります
- LiveLink モジュールは瞳孔を運びません。目のカメラを使っているときは、瞳孔の大きさだけ同じ PC の VRChat に直接送ります（ポート 9000。VRChat モードと同じ `v2/Pupil…` のパラメータで、`prefix` も付きます。1 秒に最大 50 回）。送り方タブの「瞳孔は VRChat に直接送る」（`pupils_to_vrchat`）で止められます
- モジュールは何もなめらかにしないので、frameeyeosc のなめらかさの設定がそのまま効きます
- 送るのは 1 秒に 50 回まで（いつもいちばん新しい値）です。モジュールは 10〜16 ms に 1 つしか読まないので、目のデータを全部（毎秒 90 回以上）送ると、どんどん遅れていきました
- VRCFaceTracking は最後に受け取った値を持ち続けます。なので目のデータが止まったとき（ヘッドセットを外したとき）や、送信を止めたとき・送り先の種類を変えたときは、普通に開いて正面を見た目を 1 回送ります。送信中で目のデータがない間は、それを 1 秒に 2 回送り続けます。モジュールは VRCFaceTracking が読み込んでから 180 秒以内に何か届かないと動き出さないためです（あきらめてしまったら VRCFaceTracking でモジュールを読み込み直してください）。一時停止中は何も送りません
- `eye_tracking_active` と `steamlink_params` は関係ありません。`prefix` も、上の瞳孔にだけ使います。アバターのパラメータは VRCFaceTracking が送ります

## 目を合わせる

アバターの目が少しずれる（下を向きすぎる、下を見るとまぶたが閉じる、など）ときは、パネルの「目を合わせる」タブで合わせられます。［目を合わせる］を押して、ダッシュボードを閉じてください:

1. 正面、上 15°、下 15°、左 20°、右 20° の順に、2.5 秒ずつ点が出ます。頭は動かさず、点を目で追ってください。点のまわりの輪が減っていき、測っている間は残りの秒数（2・1）が点の下に出ます
2. 続いて目印に「3 秒間 目を閉じて」と出て 3・2・1 と数えます。数え終わったら目を閉じ、チャイムが鳴るまで 3 秒そのまま閉じていてください。「開けて OK」と出たら終わりです

全部で約 20 秒です。ダッシュボードを開くと止まります。手順ごとに小さな音が鳴るので、パネルを見なくても進み具合がわかります: 点が止まったら「ポッ」、測れたら「ピッ」、測り直しは低い「ブッ」、3・2・1 で「カチ」、目を開けていいときにチャイム、終わったら上がっていくチャイム（止まったときは下がる 2 音）。タブの「♪ 音を鳴らす」で消せます。視線が落ち着かなかった点（最後の手順では目が閉じていなかったとき）は、3 回まで測り直します。結果はタブに出て、それからはボタンが［もう一度合わせる］になり、［元に戻す］で合わせる前に戻ります。正面は被るたびに少しずれるので、視線を合わせてあれば、被ったときにパネルが自動で測り直します: ヘッドセットを被って 3 秒ほどすると（ダッシュボードを閉じているとき）、正面に 2.5 秒点が出るので、見てください。動く幅・傾き・まぶたはそのままです。タブの「被ったとき」の行で、これ（正面だけ、既定）・正面と傾き（正面、上、下の順に 2.5 秒ずつ点が出て、傾きも測り直す）・何もしない を選べ（`auto_recenter`）、［もう一度合わせる］の隣のボタンで同じものを手でも始められます（「何もしない」のときは正面だけ）。正面だけが既定なのは、この測り方の傾きが測るたびに ±5° ほどばらつき（同じ被り方の 3 分の間に +3.1°、-8.9°、+2.6°、+0.6°）、直す分と同じくらいずれを足してしまうためです。値は「細かく直す」の中で手でも直せます。

合わせると決まるもの:

- 視線: 正面の位置（`gaze_offset_x` / `gaze_offset_y`）と、左右・上・下にどれだけ動かすか（`gaze_gain_x`・`gaze_gain_up`・`gaze_gain_down`）。15° 上を見たら 15° 上として送ります
- ヘッドセットの傾き（`gaze_roll_deg`）: 下の点から上の点への動きがどれだけ斜めかで測ります。ヘッドセットが傾いていると、横を見ただけで視線が上下にも動きます（8° 傾いていると、20° 横で 3° ほど）。被り直すたびに +1.1°〜+8.4° と変わっていました。左右の点を結んだ線から見た傾きは合わせるたびにばらついたので（続けて 3 回合わせて、左右から -3.8°・+2.7°・-2.1°、上下から +2.0°・+2.4°・+1.7°）、ログに並べて出すだけにしています。frameeyeosc が正面を中心に、ゲインの前にこの分だけ戻します
- 目ごとの左右（「左右の目を別々に動かす」用。`gaze_offset_x_left/right`・`gaze_gain_x_left/right`）: 点は 2 m 先なので、それぞれの目が点へ向く本当の角度は、両目の真ん中から見た角度とは違います。真ん中から見て正面の点でも、左目は 0.9° ほど右、右目は 0.9° ほど左を向きます（目の間が 63 mm のとき）。SteamVR が持っている目の間の距離（無ければ 63 mm）で、目ごとにその角度へ合わせるので、アバターの目も自然に寄ります
- まぶた: 目ごとの、閉じたとき・上・正面・下を見て開いているときの開き具合（`lid_fit_*`）。Frame は下を見るだけで開き具合を少なく読みます（20° 下で 3 割ほど）。合わせた目は、見ている向きでふつうの開き具合と比べるので、下を見ただけでは閉じません。ただし正面より開いているとは予想しません（合わせると上や下を見たほうが開いて読まれることがよくありますが、1 時間の記録ではそうなっておらず、普通に開いた目が 1 時間に何百回も半分閉じて送られていました）。上下 15° の点より先は、そこで測った値のままにします。閉じた値から開いた値までの 3 割より下で「閉じた」になります。合わせた目には自動キャリブレーションを使わず、まぶたの目盛りの代わりにこれを使います。それでも片目だけ開きすぎ・閉じすぎに見えるときは、まぶたタブの「左右の倍率」で合わせたあとの微調整ができます（`lid_scale_*`、0.9 で 1 割閉じ気味）。全部を合わせ直すと 1.0 に戻ります。記録で試すと、下を見ている目が 3 分の 1 閉じて送られた回数が 35 から 5 に減り、完全に閉じて送られたまばたきも増えました（60 回中 50 → 53）
- 見開きは測れません。目を見開いても Frame の開き具合は 0.05 ほどしか上がらず（2 回の合わせで +0.019 / -0.009 と +0.048 / +0.047）、1.000 で頭打ちになるためです。そこで合わせた目は、まぶたタブの「見開きやすさ」（`lid_widen`）で、見ている向きで予想されるその目の開き具合から数えて見開きます: 「控えめ」は 0.10 上から（0.18 上で最大）、「ふつう」は 0.07 上から（0.14 上で最大）、「出やすい」は 0.04 上から（0.10 上で最大）、「しない」は見開きません。ふつうに開いた目も少し上に揺れます。これまでの記録では、うっかり目に見えて見開いた（4 割以上）時間が、控えめ 2.4%・ふつう 6.3%・出やすい 14.9% でした。正面の値が高く 1.000 までに余地のない目（ある人の左目は 0.945）は、もう片方の目に合わせて見開きます。両目とも余地がなければ見開きません。見開きは 0.25 秒続いてから出します。まばたきの直前に Frame の開き具合が一瞬跳ね上がる（1.000 まで行くことも）ことがよくあり、そのままだと閉じる前に一瞬見開いて見えるためです。SteamOS 0.4.3 からは、ふつうに開いた目がもう 1.000 と読まれるので、見開きは届きません（[うまく動かないとき](#うまく動かないとき) を参照）

点はヘッドセットに固定して 2 m 先に出し、ダッシュボードが閉じている間だけ見えます。しくみ: パネルが `config.json` に `gaze_capture` の依頼を書き、frameeyeosc がトラッカーの視線と目ごとの開き具合を、パネルが頼んだ長さだけ平均して（点は 2 秒で最初の 0.3 秒を除く、目を閉じる手順は 3 秒で最初の 0.5 秒を除く。最後の手順以外では目を閉じているサンプルも除く）、届いたサンプルの数と毎秒の回数と一緒に状態ファイルで返し、パネルがその平均から設定を計算します。点は、届いたサンプルの 6 割以上（12 以上、ただし 45 を超えては求めない）が使えて、視線のばらつきが 2.7° 以内なら使います。トラッカーの回数は決まっていないためです（Steam Link で送っている間は毎秒 90〜136、15 のこともありました）。測った値はジャーナルにも残ります（`journalctl --user -u frameeyeosc`）。既定値のままなら何も変わりません。

## キャリブレーション

まぶたのキャリブレーションは自動です。被ってから20秒は学習せず、そのあと10秒ほどで左右それぞれの普段の開き具合をつかみ、以降はゆっくり追従します（直近10分くらいを重視）。少し目を細めた程度ではほとんど動きません。学習値は1分ごとに `~/.config/frameeyeosc/calibration` に保存され、次回はそこから始まります。やり直したいときは、パネルの「リセット」を押してください（または `calibration_reset` を増やす）。

## 目のカメラ

Frame には、目ごとに赤外線のカメラが付いています。frameeyeosc と一緒に入る道具 eyecam は、SteamVR のアイトラッキングが動いているあいだその映像を読み、目がどれくらい開いているか、見開いているか、細めているか、瞳孔の大きさを出します。frameeyeosc はそれを送る値に混ぜます。使うかどうかは自由で、使わなければ今までどおり Valve の値を送ります。

できるようになること:

- 見開きが届きます。Valve の開き具合では見開きが出ない SteamOS 0.4.3 でもです（[うまく動かないとき](#うまく動かないとき)）。普通に開いた目から上はカメラの値を使い、閉じるところ・半分閉じたところ・まばたきは今までどおり Valve の値です。目ごとに決めるので、カメラが今使えない目は Valve の値になります
- 目を細めた動きを送ります（VRChat モードの `v2/EyeSquintLeft` / `EyeSquintRight` / `EyeSquint`）。下のユーザー校正をしたあとです
- 瞳孔の大きさを送ります（VRChat モードの `v2/PupilDilation` と `v2/PupilDiameterLeft` / `PupilDiameterRight` / `PupilDiameter`。LiveLink モードでは VRChat に直接。[VRCFaceTracking（LiveLink）モード](#vrcfacetrackinglivelinkモード)）。ETVR モードでは、細めも瞳孔も送りません

準備が済めば、かぶるたびにすることはありません。かぶってから、目を開けてふつうに前を見ている約 35 秒ぶんでふだんの目を覚え、それまでは Valve の値を使います。かぶり直すと、また覚え直します。

### 準備

最初に一度だけ、パネルの「目のカメラ」タブで準備します。チェックリストになっていて、できたところから自動で次に進みます。

1. パスワードを決める: SteamOS にまだパスワードがないときだけです（2 で使うため）。Steam の設定 → 開発者 → ユーザーパスワードを変更、または［Konsole で passwd を開く］
2. 道具を入れる: ［Konsole で開く］を押すと、`sudo ~/.local/lib/eyecam/install_grab.sh` が入力された Konsole が開きます。Enter を押して、SteamOS のパスワードを打ちます（SSH なら同じ 1 行を打ちます）。パネルが自分で sudo を実行することはありません。そのあと、`install_grab.sh` が出す、入れた eyecam-grab の sha256 を確かめてください（[入れた道具を確かめる](#入れた道具を確かめる)）。更新で新しい道具が届いたときは、目のカメラタブ（と左の列）に「道具が新しくなったよ、入れ直してね」と出ます。そこから同じ［Konsole で開く］で入れ直してください。それまでも今の道具のままで動きます。ただし eyecam が安全のために古い道具を使わないと決めたとき（安全のための下限より古いとき）だけは、入れ直すまでカメラが止まり、まぶたは Valve の値だけで送ります。お知らせにもそう出ます。準備が済む前なら、この段階が「道具を入れ直す」としてまた出ます
3. 目の動きを覚える: HMD をかぶって両目が見えている状態で［覚えはじめる］を押し、画面の指示どおりに 18 秒目を動かします（閉じる・ふつうに開ける・見開く…。切り替わるたびにピッと鳴ります）。見開きだけうまく取れなかったときは、標準の値のまま進めて、あとで校正し直せます

済んだあとのタブでは、いまどの値でまぶたを動かしているかと、カメラを止めるスイッチ（「カメラで瞼を取る」、`camera_lids`）、違和感があるときの「目のカメラの校正（18秒）」、目を細めた動きを送るのに要る「ユーザー校正（最初に 1 回）」（18 秒: 細める・上を見る・下を見る）が出ます。見開きの出やすさは、まぶたタブの「見開きの出やすさ」で変えます。

### ビットで受け取るアバターの瞳孔

アバターによっては、瞳孔の開き具合を 1 つの小数（`v2/PupilDilation`）ではなく、VRCFaceTracking と同じように、いくつかの bool のパラメータに分けて受け取ります: `v2/PupilDilation1`、`v2/PupilDilation2`、`v2/PupilDilation4`（と `v2/PupilDilation8`）。そういうアバターでは、送り方タブの「瞳孔の受け取り方」（`pupil_bits`）でその個数を選んでください。`PupilDilation1`・`2`・`4` があるなら 3 です。frameeyeosc は VRCFaceTracking と同じ計算でそのビットも送ります。小数もそのまま送ります（アバターにない名前は無視されます）。送るのは変わったビットだけで、1 秒に 1 回は全部を送り直します。瞳孔を VRChat に送るとき全部に効きます: VRChat モードと、LiveLink モードで「瞳孔は VRChat に直接送る」がオンのときです。

この設定は、どのアバターでも同じ 1 つの値です。frameeyeosc からは、いまどのアバターを着ていて、どんなパラメータがあるかが見えないためです（VRChat がそれを教えるのは PC の中のプログラムだけです。OSCQuery が 127.0.0.1 でしか聞いていません）。使うアバターに合わせて選び、ふつうの小数のアバターに戻ったら「小数」に戻してください。個数は、アバターのパラメータの中から `PupilDilation1`、`PupilDilation2`… を探して数えます（Unity のアバターの Expression Parameters か、VRCFaceTracking がアバターごとに出すパラメータの一覧で見られます）。

### sudo がいるわけと、入るもの

SteamOS では、ほかのプログラムからカメラのバッファを受け取ることは、同じユーザーのプログラムどうしでもできません。できるのは、そのための権限（`CAP_SYS_PTRACE`）を持つプログラムだけです。そこで `install_grab.sh` は、小さなプログラム `eyecam-grab` を 1 つだけ `/home/.eyecam/eyecam-grab` に写し、そのファイルにだけこの権限を付けます。ファイルもフォルダも root のものなので、sudo なしでは書き換えられません。書き換えたファイルからは権限が消えます。`/home` に置くのは、SteamOS を更新しても消えない場所だからです。root で常駐するものはなく、sudoers も変えません。ずっと動いている `eyecam-rec` は自分のユーザーで動き（ユーザーサービス `eyecam.service`）、アイトラッキングが動いているときに `eyecam-grab` を起動します。

`eyecam-grab` がすること（約 200 行、`tools/eyecam/src/bin/eyecam-grab.rs`）:

- Valve のアイトラッキングのプロセスを、実行ファイルの場所とユーザー（1000）で探し（見つからないか 2 つ以上なら止める）、その中の目のカメラのバッファ（`udmabuf`）だけを選んで複製し、ユーザー 1000 のものだと確かめた eyecam-rec のソケットに渡して、すぐ終わります
- アイトラッキングを止める・アタッチする・シグナルを送る・メモリを読み書きする、はしません。バッファを自分で読むことも、ファイルを書くことも、常駐することもしません
- 権限を持つプログラムは安全なモードで動き（`LD_PRELOAD` などは効きません）、環境変数も自分のユーザー ID も見ません。どのプログラムが頼んだかは確かめません。`/run/user/1000/eyecam.sock` で待っているユーザー 1000 のプログラムに渡します（確かめるのは、ソケットとその先のプログラムがユーザー 1000 のものであることだけ）。ふつうは eyecam-rec ですが、eyecam-grab を入れると、あなたのユーザーで動くプログラムならどれでも、これを通して目のカメラの映像を受け取れるようになります。そういうプログラムは、もともとあなたのファイルも読めます。ほかのユーザーのプログラムには渡しません

eyecam-rec は、受け取ったバッファも Valve の目のデータも読み取り専用で開きます。root で動くことはありません。くわしくは [tools/eyecam/README.md](tools/eyecam/README.md) にあります。

### 入れた道具を確かめる

`install_grab.sh` が写すのは `~/.local/lib/eyecam/eyecam-grab` で、このフォルダーはあなたのユーザーで動くプログラムならどれでも書き換えられます。なので、権限を付けたのがリリースのファイルそのものか確かめてください。`install_grab.sh` は入れたファイルの sha256 を出します（「インストールした」のすぐ上の、`/home/.eyecam/eyecam-grab` で終わる行）。それを、リリースの tar.gz の中の `eyecam/SHA256SUMS` の `eyecam-grab` の行と比べます:

```sh
tar -xzOf frameeyeosc-*-steamframe-aarch64.tar.gz frameeyeosc/eyecam/SHA256SUMS
```

tar.gz は GitHub のリリースから取ってください（tar.gz 自体のチェックサムは、そのリリースの `SHA256SUMS` にあります）。同じ一覧は `~/.local/lib/eyecam/SHA256SUMS` にも入りますが、同じ書き換えられるフォルダーにあるので、信じるのはリリースのほうです。違っていたら、道具を取り除いて（`sudo ~/.local/lib/eyecam/install_grab.sh --uninstall`）、frameeyeosc を入れ直してください。

### 止める・取り除く

- カメラを使うのをやめるには、目のカメラタブの「カメラで瞼を取る」をオフにします（`"camera_lids": false`）。Valve の値だけを送るようになり、パネルは eyecam-rec にも映像から目の値を出すのをやめさせます（`live off`。パネルが動いているあいだは、eyecam-rec が再起動するたびに言い直します）。eyecam-rec 自体は動き続け、カメラのバッファを持ったまま、状態ファイルを書きます
- eyecam を完全に止めるには: `systemctl --user disable --now eyecam`。更新してもオフのままです（`install.sh` が eyecam.service を有効にするのは初めて入れたときだけで、そのあとは動いていれば再起動するだけです）。また使うときは `systemctl --user enable --now eyecam`
- 権限の付いたコピーを取り除くには: `sudo ~/.local/lib/eyecam/install_grab.sh --uninstall`。`./install.sh --uninstall` は `~/.local/lib/eyecam` を消すので、その前にやってください（あとからでも、コピーが残っていれば、消すための `sudo rm` のコマンドを出します）
- `./install.sh --uninstall` は frameeyeosc と一緒に eyecam も取り除き、`--purge` を付けると設定と校正（`~/.config/eyecam`）も消します。`~/eyecam` の校正のファイルと開発用の録画（[プライバシー](#プライバシー)）は残すので、要らなければ自分で消してください

## うまく動かないとき

- ログ: `journalctl --user -u frameeyeosc -f`（パネルは `journalctl --user -u frameeyeosc-panel -f`）
- `No Steam Link connection found; waiting for one`: Steam Link がまだつながっていません。または送り先の PC を固定してください
- `Can't send OSC to ... yet (Network is unreachable)` や `Sending OSC to ... failed (...)`: ネットワークがまだつながっていない（起動直後の Wi-Fi など）か、PC に届きません。frameeyeosc は動き続けて送り直し、送れるようになると `... works again` と出ます
- 左の列の「目のデータ」が少なめ（赤、毎秒 60 未満。90 のはずが 46 や 15 のことがありました）、または目を合わせると最初の点でサンプルが足りずに止まる: 回数の下の行に、どちらが遅いかが出ます。「本体の処理が追いついていません」なら、目のトラッカーが出したサンプルを frameeyeosc が読みそびれたか、1 つに時間がかかりすぎています。「Frame から届く数が少なめです」なら、目のトラッカーから届く数そのものが少なく、frameeyeosc は全部読めています。少なめが 10 秒続くと、数字を 1 行ログに残します（`journalctl --user -u frameeyeosc` に `Eye data has been low for 10 s: …`）。その行と、PC から Steam Link で送っていたかどうかを知らせてください
- ログに `Dropped … datagrams to … the network was too busy to take them at once`: ネットワーク（Steam Link の映像でいっぱいの Wi-Fi など）が送る速さに追いつかなかったので、その分を捨てました。frameeyeosc は送り終わるのを待たないので、目のデータは止まらずに届きます。この行は 1 分に 1 回までです
- ログに `Sending OSC to ...` と出ているのにアバターが反応しない: VRChat の OSC が有効かを確認したうえで、Windows のファイアウォールを確認してください。VRChat の受信許可は「パブリック」だけになっていることが多く、「プライベート」の家のネットワークから届く OSC は止められます。許可の対象が `launch.exe` ではなく `VRChat.exe` になっているかにも注意してください。範囲をしぼって許可するには（管理者の PowerShell で）:
  ```powershell
  New-NetFirewallRule -DisplayName "VRChat OSC (LAN UDP 9000)" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 9000 -RemoteAddress LocalSubnet -Program "C:\Program Files (x86)\Steam\steamapps\common\VRChat\VRChat.exe" -Profile Private,Public
  ```
  「プライベート」と「パブリック」の両方にしてあるのは、家のネットワークは「プライベート」のことが多い一方で、付属の無線アダプタは Windows では別のネットワークとして見え、たいてい「パブリック」になっているからです
- SteamOS 0.4.3 で目が見開かず、まぶたタブに「開き具合が 1.0 で頭打ち」と出る: SteamOS 0.4.3 から、Frame はふつうに開いた目を上限の 1.000 と読みます（ある人の左目は、両目を開いているあいだ、それまで中央値 0.754、0.4.3 では 1.000 が 75〜93%）。それより上がないので、見開きを読み取れません。frameeyeosc は版ではなく読んだ値から判断し（両目を開いている直近 1 分で、どちらかの目が 1.000 のサンプルが半分を超える。[状態ファイル](#状態ファイル) の `openness_saturated`）、まぶたタブにそう出します。のちの SteamOS で変われば、表示は自然に消えます（アップデートの再起動のあとなど、次に frameeyeosc が起動したときから。見る向きで割合が上下するので、一度そう判断したら frameeyeosc が動いているあいだはそのままです）。そのあいだはどのまぶたも「普通に開いた」より上では送らないので、目を合わせていない目（1.000 が目盛り④より先になる）がずっと見開いて見えることもありません。まばたきと閉じるのは今までどおりで、「見開きやすさ」の設定も、また見開きが届くようになったときのためにそのまま残ります。[目のカメラ](#目のカメラ) を使っている目は、カメラの値で見開きます
- 両目がいつも同じ向きを見る、片目の視線がもう片方の目についていく: 「Track Dominant Eye Only」の設定（VR Settings > General の詳細。SteamOS 0.4.3）がオンです。Frame はその目だけを追い、両目にその目の視線を渡します（まぶたはそれぞれの目のまま）。パネルの左の列にも出ます（「Frame の設定: 右目だけで追っています」）。両目を追うには、この設定をオフにしてください。frameeyeosc はこの設定を読むだけで、変えません
- パネルに「目のデータを読めません: …」と出る: frameeyeosc は動いていますが、アイトラッキングのデータを読めていません。1 秒ごとに読み直します（理由は `journalctl --user -u frameeyeosc` にも 1 回だけ出ます）。ヘッドセットの起動直後（`… No such file or directory`）なら問題ありません。アイトラッキングがまだ起動していないだけです。「unsupported eye shared-memory version」なら、SteamOS の更新で frameeyeosc が読む形式が変わっています（[免責事項](#免責事項) を参照）
- ヘッドセットを外していると何も動かない: 正常です。Frame は被っている間しか目を追いません
- パネルに「本体が動いていません」と出る: `systemctl --user status frameeyeosc` を確認してください。パネルで変えた設定は保存されていて、動き出したら反映されます

## 既知の問題

- VRChat に直接送るときは、パラメータをビットに詰める「バイナリパラメータ」の VRCFT アバターには対応していません（目のカメラの瞳孔だけは `pupil_bits` で対応）。ETVR モードと LiveLink モードでは、アバター側は VRCFaceTracking しだいです。見開きも伝えたいときは LiveLink モードを使ってください

## プライバシー

- 視線とまぶたの値（目のカメラを使っていれば、目を細めた動きと瞳孔の大きさも）は、あなたの PC にだけ送ります: 上の送り先と、LiveLink モードで目のカメラを使っているときは瞳孔のために同じ PC のもう 1 か所、VRChat のポート 9000（状態ファイルの `pupil_target`）です。テレメトリはなく、インターネットにも接続しません
- パネルは、起動時と 1 日 1 回まで（確認に失敗したときは 1 時間後に）、GitHub（`api.github.com`）に最新のリリースを問い合わせます（「新しい版の確認」がオフなら問い合わせません）。ふつうの Web アクセスと同じく、GitHub には IP アドレスが見えます。ほかには何も送らず、ダウンロードも GitHub からだけです
- ディスクに書くもの:
  - 設定（`~/.config/frameeyeosc/config.json`）と、左右それぞれの目の「普段の開き具合」の学習値2つ（`~/.config/frameeyeosc/calibration`）
  - `install.sh` が置くもの: 更新のスクリプト `~/.local/share/frameeyeosc/frame-update.sh`、パネルの更新履歴が読む変更履歴（同じ場所の `CHANGELOG.md`・`CHANGELOG.ja.md`）、インストールのオプション `~/.config/frameeyeosc/install-args`
  - 更新の確認と更新が `~/.cache/frameeyeosc/` に書くもの: `update-check.json`（GitHub の前回の答え）、`update-state.json`（前回の更新の進み具合）、`update.log`（前回の更新のログ）、作業フォルダ `update/`（毎回空にします。写した更新のスクリプトだけ残ります）、確認や更新の最中だけの `update.lock/` フォルダ

  - パネルが `$XDG_RUNTIME_DIR/frameeyeosc/` に書くもの（メモリの上にあり、再起動で消える）: 目を合わせるときの音のファイル `sounds/`（起動時に書く）
  - eyecam（目のカメラ）が書くもの: 設定と校正 `~/.config/eyecam/`（`settings.json` に見開きの感度、`calib.json` に校正で測った値）、メモリの上（再起動で消える）の決まった場所: `/run/user/1000/eyecam/` に状態ファイル・操作用のソケット・frameeyeosc が読む目の値、そのフォルダーの隣に eyecam-grab がバッファを渡すソケット `/run/user/1000/eyecam.sock`。systemd のジャーナル（`journalctl --user -u eyecam`）に、30 秒ごとに 1 行（処理したフレームの数、1 フレームの時間、左右の瞳孔のふだんの差）と、校正のたびに測った値の報告。校正では目の映像を残しません（開発者モードを除く）。校正のたびに（準備のときもあとからも、失敗したり止めたりしても）、`~/eyecam/calib_YYYY-MM-DD_HH-MM-SS/` に小さなテキストのファイルだけ（1 MB ほど: 結果、フレームごとに出した値、フレームの時刻、Valve の値）を残します。eyecam の開発者モード（`~/.config/eyecam/settings.json` の `"dev": true`。手で書き換えるもので、最初はオフ）のときだけ、その 18 秒の目のカメラの映像も残します（1 回で約 0.5 GB）。詳細タブの開発用の「目の撮影」は、いつも目の映像を `~/eyecam/rec_YYYY-MM-DD_HH-MM-SS/` に残します（1 回で約 2.5 GB。始める前にパネルにも出ます）。どれもヘッドセットの外には出しません。要らなくなったら消してください

  上の eyecam の校正のファイル・録画・ジャーナルの行のほかは、目のデータは保存しません。ただし目を合わせたときに測った値（視線の平均の向きとばらつき、目ごとの開き具合の平均）は、1 回ごとに 1 行 systemd のジャーナルに残り、まぶたの読んだ値は `config.json` に残ります。最新の目の値は状態ファイルにありますが、これはメモリの上にあって本人しか読めず、1 秒に 10 回上書きされます。履歴は残しません
- OSC は暗号化されないので、同じネットワーク上の他の機器から読める可能性があります
- 「視線の点を表示」がオンの間は、送っている視線を Unix ソケット（状態ファイルのフォルダの `gaze-dots.sock`）でパネルにも渡します。ヘッドセットの外には出ず、保存もしません

## 免責事項

- 自己責任でお使いください。このフォークでの変更は AI（Claude Opus 5.5）を使って作りました。ユニットテストと自分の Steam Frame で動作は確かめていますが、あなたの環境で何か起きても責任は取れません。使う前にコードを自分の目で確認してください。本ソフトウェアは無保証です（[LICENSE](LICENSE) を参照）
- ヘッドセットのアイトラッキングが使っている、公開されていない共有メモリの形式（バージョン4と、SteamOS 0.4.3 からのバージョン5）を読んでいます。SteamOS の更新でこの形式が変わると、frameeyeosc が対応するまで目のデータを送れません。そのあいだも frameeyeosc は動き続けて 1 秒ごとに読み直し、パネルには「目のデータを読めません: unsupported eye shared-memory version …」と出ます
- frameeyeosc は root 権限を使わず、SteamOS のファイルや設定は変更しません。書き込むのは、アイトラッキングの共有メモリにある「次のサンプルをください」という合図だけです。共有メモリのロックも、アイトラッキングの本来の利用側と同じ手順で取ります。パネルが書くのは frameeyeosc の設定ファイルだけで、ほかには eyecam-rec に命令を送ります。目のカメラの準備では、Konsole を開くのと、SteamOS にパスワードがあるかの確認（`steamos-passwd --has-password`。読むだけ）もします。目のカメラを使うときだけ、sudo の手順が 1 回あり、それは自分で実行します。これで `/home/.eyecam` に、権限を 1 つだけ持つ root のプログラムが 1 つ入ります（[目のカメラ](#目のカメラ)）
- eyecam は、アイトラッキングが中で使っているバッファから目のカメラの映像を読みます。これも公開されていないものです。SteamOS の更新で読めなくなることがあり、そのときは frameeyeosc は Valve の値を送ります
- Valve の非公開の内部データを読むことは、リバースエンジニアリングを制限している Steam 利用規約に触れる可能性があります。使うかどうかはご自身で判断してください
- 非公式のプロジェクトで、Valve Corporation、VRChat Inc.、VRCFaceTracking プロジェクト、EyeTrackVR プロジェクトとは関係なく、承認も受けていません。Steam、Steam Frame、SteamVR、Steam Link は Valve Corporation の商標、VRChat は VRChat Inc. の商標です。対応製品を示す目的でのみ名前を使っています

## 開発

ビルドとテストはヘッドセット上で行います（本体はヘッドセットの glibc に、パネルは SteamVR の OpenVR ライブラリにリンクする必要があるため。`scripts/package.sh` を参照）:

```sh
cargo test --release
cmake -G Ninja -S panel -B panel/build && ninja -C panel/build
panel/build/gaze-fit-test   # パネルの目合わせの計算
scripts/package.sh   # 両方入った dist/frameeyeosc-<version>-steamframe-aarch64.tar.gz と dist/SHA256SUMS を作る
```

`vendor/frame-updater/` は、私の Steam Frame 用アプリで共通の更新の仕組みのコピーです。ここでは書き換えないでください。`MANIFEST.sha256` と違っていると `scripts/package.sh` が止まります。

`tools/eyecam/` は、目のカメラのツール eyecam（`eyecam-rec` と `eyecam-grab`）のコピーです。eyecam は別のブランチで作っているので、これもここでは書き換えないでください。更新するときは、取り込みたいコミットからこのフォルダーを取り出して（`git rm -rq tools/eyecam && git checkout <コミット> -- tools/eyecam`。先の `git rm` で、新しい版でなくなったファイルも消える）、メッセージにそのコミットのハッシュを書いてコミットします。`Cargo.toml` と `Cargo.lock` は別で、ルートの Cargo のビルドには入りません。`scripts/package.sh` が別にビルドしてテストします。

リリースを公開するときは、2 つとも添付します。`SHA256SUMS` が無いリリースは、パネルの「更新する」では入れず、手で更新してもらう表示になります。リリースの本文は `CHANGELOG.md` のその版の節（見出しを除く）です。節は英語の要約 1 段落で始め、次に `日本語: ` で始まる日本語の要約の段落、そのあとに箇条書きを置きます。古い版のパネル（0.7.1 から）は、その版が出ているあいだ、要約を出します（日本語の画面では日本語の段落。マークダウンを外し、300 文字まで）:

```sh
gh release create v0.4.0 --title v0.4.0 --notes-file notes.md
gh release upload v0.4.0 dist/frameeyeosc-0.4.0-steamframe-aarch64.tar.gz dist/SHA256SUMS
```

リリースのたびに、パネルに出す短い日本語の更新履歴 `CHANGELOG.ja.md` にもその版の節を足します。見出しは `CHANGELOG.md` と同じにし、要約は `CHANGELOG.md` の `日本語:` の段落と同じにして、そのあとに変更点をやさしい言葉で並べます。

目の処理を実データで調整するときは、アイトラッカーの生の値を記録してから再生します（記録中は何も送らないので、サービスと並べて動かせます）。再生すると、今の設定と、同じ設定から 0.4.0 の処理を外したものの指標を並べて出します。設定はいつもどおり `config.json` とオプションから読みます。記録は個人のデータなので、リポジトリに入れないでください。

```sh
frameeyeosc --record ~/eyes.csv              # Ctrl+C（または SIGINT / SIGTERM）で止める。止めたところまで全部書かれる
frameeyeosc --replay ~/eyes.csv --blink-hold-ms 120 --replay-out ~/processed.csv   # 処理後の値を CSV にも書く
```

同じ記録は、遊びながらパネルからも取れます: 詳細タブの「目のログ」の［記録する］で始まり、ボタンは［止める 1:23］に変わります。その間、左の列にも赤く「記録中 1:23」と出ます。60 分たつか、パネルが終わると止まります。ファイルは `~/.local/share/frameeyeosc/recordings/` に、`eyes_YYYY-MM-DD_HH-MM-SS.csv`（1 分で約 2.3 MB）、そのとき使っていた `config.json` を `eyes_….config.json`、記録係のメッセージを `eyes_….log` として保存します。ヘッドセットの外には出しません。要らなくなったら消してください。

eyecam が動いているあいだは、記録の各行の最後に目のカメラの値も入ります（`cam_l_*` / `cam_r_*`: 新しいか・有効か、閉じているか、まぶた、見開き、細め、瞳孔の mm と開き具合、確かさ、何 ms 前の値か。そのあと `cam_calib_state` と `cam_live`。値がないあいだは空欄）。前の記録も、前の読み手もそのまま使えます。このような記録を replay すると、frameeyeosc と同じようにカメラの値を混ぜ、報告にカメラの値があった割合と、カメラあり・なしでの見開いて送った割合が足されます。

## ライセンス

MIT。[LICENSE](LICENSE) を参照してください。元の作品は konsti219 によるものです。`vendor/frame-updater/` はほかの人のコードではなく、sasaken1102r が自分の Steam Frame アプリで共通に使っている更新の仕組みを、このリポジトリの MIT ライセンスのもとで写したものです。`tools/eyecam/` も sasaken1102r のもので同じライセンスですが、Curtis English さんの FrameEyeCameraFeed（MIT、[tools/eyecam/NOTICE](tools/eyecam/NOTICE)）の一部を移植しています。同梱している Rust のライブラリ、パネルが使っている OpenVR SDK のヘッダのライセンスと FrameEyeCameraFeed の表示は [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) に、変更履歴は [CHANGELOG.md](CHANGELOG.md) にあります。

## 謝辞

まぶたのデータのありかを見つけて frameeyeosc を公開してくれた konsti219 さんに感謝します。このフォークはその成果の上に作っています。目のカメラの映像の見つけ方を FrameEyeCameraFeed で示してくれた Curtis English さんにも感謝します。eyecam はその上に作っています。
