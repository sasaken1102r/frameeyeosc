# frameeyeosc

Steam Frame のアイトラッキング（視線とまぶたの開き具合）を、VRCFaceTracking 形式のアバターパラメータとして OSC で VRChat に送るツールです。ヘッドセット上でバックグラウンドのサービスとして動き、Steam Link でストリーミングしている PC 版 VRChat で使えます。PC の VRCFaceTracking に送って、ほかのトラッカーとまとめることもできます。

[English](README.md)

https://github.com/user-attachments/assets/f8969485-161b-40d4-b9e4-689dee6d1955

[konsti219/frameeyeosc](https://github.com/konsti219/frameeyeosc) をフォークしたものです。Steam Frame のまぶたのデータは公開 API からは取れないのですが、元のプロジェクトが内部の共有メモリ（`/dev/shm/eye-server.mmap`）から読めることを見つけてくれたおかげで、このツールを作ることができました。

目のカメラの映像の取り出し方は、Curtis English さんの [FrameEyeCameraFeed](https://github.com/Curtis-VL/FrameEyeCameraFeed)（MIT）から移植しました。

## できること

- Steam Link でつないでいる PC を自動で見つけて、目のデータを送ります
- 視線とまぶたをなめらかにして、アバターの目がピクピクしないようにします
- まぶたは使っているうちに自動で調整され、まばたきとウインクも伝わります
- 送り先は、VRChat に直接・VRCFaceTracking（LiveLink モジュールか、専用の FrameEyeOSC モジュール）から選べます
- 使いたい人は、Frame の目のカメラで見開き・細め・瞳孔の大きさも送れます
- SteamVR のダッシュボードのパネルで、かぶったまま設定を変えられます

くわしくは [できることのくわしい説明](docs/features.ja.md) を、Steam Link 標準の OSC との比べは [送り先](docs/outputs.ja.md#steam-link-標準の-osc-との違い) を見てください。

## 必要なもの

- 開発者モードを有効にして SSH で入れる Steam Frame（設定 → システム → 開発者モードを有効化、開発者の項目でパスワードを設定）。SSH を有効にすると、同じネットワークにいてパスワードを知っている人は誰でもヘッドセットに入れるので、推測されにくいパスワードにしてください
- Steam Link でストリーミングしている PC 版 VRChat（Action Menu → Options → OSC → Enabled）
- VRCFaceTracking の目のパラメータ（`FT/v2/EyeLeftX`、`EyeLidLeft` など）を float で持つアバター（くわしくは [アバターのパラメーター](docs/outputs.ja.md#アバターのパラメーター)）
- VRCFaceTracking（LiveLink）モードで使うときは、PC に VRCFaceTracking と LiveLink モジュール。FrameEyeOSC モードなら VRCFaceTracking と FrameEyeOSC モジュール

## インストール

### いちばんかんたん：Frame の中だけで入れる（おすすめ）

PC は要りません。Frame の Konsole（画面下のバーの ＋ →「プログラムを起動」→ Konsole）で次のコマンドを入力して Enter を押し、メニューで **1**（frameeyeosc）を選びます。ダッシュボードのパネルも入れるかは、途中で聞かれます。

```sh
curl -fsSL https://frame.sasaken1102s.net | sh
```

https://github.com/user-attachments/assets/712e7370-df43-460d-a6bd-0b18e00cca7d

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

入る場所と、一緒に入る目のカメラの道具 eyecam のことは [入る場所](docs/update.ja.md#入る場所) を見てください。

インストールしたら、PC 側で Steam Link の OSC 送信を OFF にしてください（SteamVR の設定 → Steam Link → OSC）。Steam Link もスムージングなしの目のデータを VRChat に送っているので、両方が動いているとアバターの目を2つのデータが取り合ってしまいます。LiveLink・FrameEyeOSC モードでも同じです（アバターの目を動かすのが VRCFaceTracking になるだけです）。

## はじめの使い方

1. ダッシュボードの「プログラムを起動」（＋）から「frameeyeosc パネル」を開きます（次からは SteamVR と一緒に起動します）
2. 「基本」タブで送り先を選びます: VRChat に直接・VRCFT（LiveLink）・VRCFT（FrameEyeOSC）。VRCFT のときは、PC で準備することが「送り方」タブに出ます
3. アバターの目がずれるときは、「目を合わせる」タブの［目を合わせる］を押して、出てくる点を目で追います（約 20 秒。[目を合わせる](docs/eye-fit.ja.md#目を合わせる)）
4. 見開きや細めも送りたいときは、「目のカメラ」タブで最初に一度だけ準備します（約 2 分。[目のカメラ](docs/eyecam.ja.md)）

パネルの各タブの説明は [パネル](docs/panel.ja.md)、設定ファイルとオプションは [設定と状態ファイル](docs/settings.ja.md) にあります。

## 更新する・前の版に戻す

- パネルの「詳細」›「バージョン」の［更新する］で更新できます。インストールと同じコマンドでも更新できます
- テスト版は、同じページの「テスト版も受け取る」をオンにすると届きます
- 前の版に戻す手順は [前の版に戻す](docs/update.ja.md#前の版に戻す) にあります

## うまく動かないとき

- 相談するときは、パネルの「詳細」›「困ったとき」の「診断」の［開く］を押して、そのページのスクショを送ってください
- ログ: `journalctl --user -u frameeyeosc -f`（パネルは `journalctl --user -u frameeyeosc-panel -f`）
- アバターの目が動かないときは、PC の Steam Link の OSC 送信がオフか、VRChat の OSC が有効か、Windows のファイアウォールで止められていないかを見てください
- 左の列の「目のデータ」が赤（少なめ）のときは、横の［90 にする］で戻せることがあります（SteamOS 0.5.7）

ほかのことは [うまく動かないとき](docs/troubleshooting.ja.md) にあります。

## プライバシー

- 目のデータは、あなたが決めた送り先にだけ送ります。テレメトリはありません
- パネルがインターネットにつなぐのは、新しい版の確認（GitHub と frame.sasaken1102s.net。「新しい版の確認」をオフにすると止まります）と、送り方タブのモジュールのアドレスを押したときだけです
- 目のカメラの映像は、自分で録ったとき（「目の撮影」「細めの学習用に撮る」）と開発者モードのときだけ Frame に残します。どこにも送りません
- 送るのは暗号化しない UDP で、家のネットワークで使う前提です

くわしくは [プライバシー](docs/privacy.ja.md) を見てください。

## 既知の問題

- VRChat に直接送るときは、パラメータをビットに詰める「バイナリパラメータ」の VRCFT アバターには対応していません（目のカメラの瞳孔だけは `pupil_bits` で対応）。LiveLink・FrameEyeOSC モードでは、アバター側は VRCFaceTracking しだいです

## 免責事項

- 自己責任でお使いください。このフォークでの変更は AI（Claude Opus 5.5）を使って作りました。ユニットテストと自分の Steam Frame で動作は確かめていますが、あなたの環境で何か起きても責任は取れません。使う前にコードを自分の目で確認してください。本ソフトウェアは無保証です（[LICENSE](LICENSE) を参照）
- ヘッドセットのアイトラッキングが使っている、公開されていない共有メモリの形式（バージョン4と、SteamOS 0.4.3 からのバージョン5）を読んでいます。SteamOS の更新でこの形式が変わると、frameeyeosc が対応するまで目のデータを送れません。そのあいだも frameeyeosc は動き続けて 1 秒ごとに読み直し、パネルには「目のデータを読めません: unsupported eye shared-memory version …」と出ます
- frameeyeosc は root 権限を使わず、SteamOS のファイルや設定は変更しません。書き込むのは、アイトラッキングの共有メモリにある「次のサンプルをください」という合図だけです。共有メモリのロックも、アイトラッキングの本来の利用側と同じ手順で取ります。パネルが書くのは frameeyeosc の設定ファイルと、自分の記録（校正と目合わせの記録、目のログ、細めの学習用の録画）だけで、ほかには eyecam-rec に命令を送ります。消すのは、新しい 10 件より古い校正と目合わせの記録と、あなたが［消す］を押したものだけです: 詳細タブの「データ」の目のログ（記録中の分は残します）と、「細めの学習用に撮る」の録画。「データ」の目のカメラの録画と校正の目の画像は、eyecam-rec に頼んで消してもらいます（校正の結果は残ります）。目のカメラの準備では、Konsole を開くのと、SteamOS にパスワードがあるかの確認（`steamos-passwd --has-password`。読むだけ）もします。目のカメラは、ふつうは Valve の dsp_service に自分のユーザーのままつないで、バッファを読み取り専用でもらいます（何も書き込みません）。それができないときだけ、sudo の手順が 1 回あり、それは自分で実行します。これで `/home/.eyecam` に、権限を 1 つだけ持つ root のプログラムが 1 つ入ります（[目のカメラ](docs/eyecam.ja.md#目のカメラ)）
- eyecam は、アイトラッキングが中で使っているバッファから目のカメラの映像を読みます。これも公開されていないものです。SteamOS の更新で読めなくなることがあり、そのときは frameeyeosc は Valve の値を送ります
- Valve の非公開の内部データを読むことは、リバースエンジニアリングを制限している Steam 利用規約に触れる可能性があります。使うかどうかはご自身で判断してください
- 非公式のプロジェクトで、Valve Corporation、VRChat Inc.、VRCFaceTracking プロジェクト、EyeTrackVR プロジェクトとは関係なく、承認も受けていません。Steam、Steam Frame、SteamVR、Steam Link は Valve Corporation の商標、VRChat は VRChat Inc. の商標です。対応製品を示す目的でのみ名前を使っています

## もっとくわしく

- [できることのくわしい説明](docs/features.ja.md)
- [インストールと更新・前の版に戻す](docs/update.ja.md)
- [パネル](docs/panel.ja.md)
- [送り先](docs/outputs.ja.md)（VRChat、VRCFaceTracking、ほかのアプリ）
- [目を合わせる・キャリブレーション](docs/eye-fit.ja.md)
- [目のカメラ](docs/eyecam.ja.md)
- [設定と状態ファイル](docs/settings.ja.md)
- [うまく動かないとき](docs/troubleshooting.ja.md)
- [プライバシー](docs/privacy.ja.md)
- [開発](docs/development.ja.md)

## ライセンス

MIT。[LICENSE](LICENSE) を参照してください。元の作品は konsti219 によるものです。`vendor/frame-updater/` はほかの人のコードではなく、sasaken1102r が自分の Steam Frame アプリで共通に使っている更新の仕組みを、このリポジトリの MIT ライセンスのもとで写したものです。`tools/eyecam/` も sasaken1102r のもので同じライセンスですが、Curtis English さんの FrameEyeCameraFeed（MIT、[tools/eyecam/NOTICE](tools/eyecam/NOTICE)）の一部を移植しています。同梱している Rust のライブラリ、パネルが使っている OpenVR SDK のヘッダのライセンスと FrameEyeCameraFeed の表示は [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) に、変更履歴は [CHANGELOG.md](CHANGELOG.md) にあります。

## 謝辞

まぶたのデータのありかを見つけて frameeyeosc を公開してくれた konsti219 さんに感謝します。このフォークはその成果の上に作っています。目のカメラの映像の見つけ方を FrameEyeCameraFeed で示してくれた Curtis English さんにも感謝します。eyecam はその上に作っています。
