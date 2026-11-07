# 開発

[README に戻る](../README.ja.md)

ビルドとテストはヘッドセット上で行います（本体は glibc に、パネルは SteamVR の OpenVR ライブラリにリンクする必要があるため。`scripts/package.sh` を参照）。`scripts/package.sh` は、前の SteamOS でも起動するように、Rust のプログラムを古い glibc のコンテナ（Debian bookworm、podman を使う）でビルドします:

```sh
cargo test --release
cmake -G Ninja -S panel -B panel/build && ninja -C panel/build
panel/build/gaze-fit-test   # パネルの目合わせの計算
scripts/package.sh   # 両方入った dist/frameeyeosc-<version>-steamframe-aarch64.tar.gz と dist/SHA256SUMS を作る
```

`vendor/frame-updater/` は、私の Steam Frame 用アプリで共通の更新の仕組みのコピー、`vendor/frame-apps/` は「詳細」›「バージョン」に出す私のほかの Frame アプリの一覧のコピーです。ここでは書き換えないでください。どちらも `MANIFEST.sha256` と違っていると `scripts/package.sh` が止まります。

`tools/eyecam/` は、目のカメラのツール eyecam（`eyecam-rec` と `eyecam-grab`）のコピーです。eyecam は別のブランチで作っているので、これもここでは書き換えないでください。更新するときは、取り込みたいコミットからこのフォルダーを取り出して（`git rm -rq tools/eyecam && git checkout <コミット> -- tools/eyecam`。先の `git rm` で、新しい版でなくなったファイルも消える）、メッセージにそのコミットのハッシュを書いてコミットします。`Cargo.toml` と `Cargo.lock` は別で、ルートの Cargo のビルドには入りません。`scripts/package.sh` が別にビルドしてテストします。

リリースを公開するときは、2 つとも添付します。`SHA256SUMS` が無いリリースは、パネルの「更新する」では入れず、手で更新してもらう表示になります。リリースの本文は `CHANGELOG.md` のその版の節（見出しを除く）です。節は英語の要約 1 段落で始め、次に `日本語: ` で始まる日本語の要約の段落、そのあとに箇条書きを置きます。古い版のパネル（0.7.1 から）は、その版が出ているあいだ、要約を出します（日本語の画面では日本語の段落。マークダウンを外し、300 文字まで）:

```sh
gh release create v0.4.0 --title v0.4.0 --notes-file notes.md
gh release upload v0.4.0 dist/frameeyeosc-0.4.0-steamframe-aarch64.tar.gz dist/SHA256SUMS
```

リリースのたびに、パネルに出す短い日本語の更新履歴 `CHANGELOG.ja.md` にもその版の節を足します。見出しは `CHANGELOG.md` と同じにし、要約は `CHANGELOG.md` の `日本語:` の段落と同じにして、そのあとに変更点をやさしい言葉で並べます。

目の処理を実データで調整するときは、アイトラッカーの生の値を記録してから再生します（記録中は何も送らないので、サービスと並べて動かせます）。再生すると、今の設定と、同じ設定から 0.4.0 の処理を外したものの指標を並べて出します。設定はいつもどおり `config.json` とオプションから読みます。記録は個人のデータなので、リポジトリに入れないでください。

```sh
~/.local/bin/frameeyeosc --record ~/eyes.csv              # Ctrl+C（または SIGINT / SIGTERM）で止める。止めたところまで全部書かれる
~/.local/bin/frameeyeosc --replay ~/eyes.csv --blink-hold-ms 120 --replay-out ~/processed.csv   # 処理後の値を CSV にも書く
```

同じ記録は、遊びながらパネルからも取れます: 詳細タブの「動作確認」の「目のログ」の［記録する］で始まり、ボタンは［止める 1:23］に変わります。その間、左の列にも赤く「記録中 1:23」と出ます。60 分たつか、パネルが終わると止まります。ファイルは `~/.local/share/frameeyeosc/recordings/` に、`eyes_YYYY-MM-DD_HH-MM-SS.csv`（1 分で約 2.3 MB）、そのとき使っていた `config.json` を `eyes_….config.json`、記録係のメッセージを `eyes_….log` として保存します。[目のカメラ](eyecam.ja.md#目のカメラ)を使っているときは、記録している間、eyecam のドリフトの記録も一緒に取ります: 数字だけ（映像なし）、目ごとに 1 秒 1 行（瞳を見つけた位置、比べている基準の値、出した値、Valve の開き具合と視線の上下。1 時間で約 1.3 MB）を、記録と同じ名前の `~/eyecam/drift/eyes_YYYY-MM-DD_HH-MM-SS.eyecam.csv` に。どのファイルかは記録の `.log` に書きます。ヘッドセットの外には出しません。要らなくなったら消してください。

eyecam が動いているあいだは、記録の各行の最後に目のカメラの値も入ります（`cam_l_*` / `cam_r_*`: 新しいか・有効か、閉じているか、まぶた、見開き、細め、瞳孔の mm と開き具合、確かさ、何 ms 前の値か。そのあと `cam_calib_state` と `cam_live`。値がないあいだは空欄）、最後に `cam_eyes_anatomical`（カメラの値がある行は `1`）。この列は `cam_l_*` が左目だという印です。0.7.2〜0.7.5 の eyecam は右目のカメラを L と呼んでいたので、この列がない記録（その版のカメラの列のある記録）は `cam_l_*` が右目、`cam_r_*` が左目です。replay はこれを入れ替えて読み、それぞれの値を本当の目に当てます。前の記録も、前の読み手もそのまま使えます。このような記録を replay すると、frameeyeosc と同じようにカメラの値を混ぜ、報告にカメラの値があった割合と、カメラあり・なしでの見開いて送った割合が足されます。
