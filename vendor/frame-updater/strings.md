# 更新の文言（日本語・英語）

各アプリの i18n 表に写して使う。キー名は C++ の `UiText` のフィールド名にそのまま使える形にしてある。`%s` は printf の書式（版の番号など）。
パネルは幅が狭いので、1 行に収まらないときは各アプリで言い回しを縮めてよい（意味は変えない）。

## 設定

| キー | 日本語 | English |
|---|---|---|
| `rowUpdateCheck` | 新しい版の確認 | Check for updates |
| `hintUpdateCheck` | 起動時と 1 日 1 回、GitHub に新しい版がないか見に行きます | Looks on GitHub for a new version at start and once a day |

## 状態の行

| キー | いつ | 日本語 | English |
|---|---|---|---|
| `updateUpToDateFormat` | `UpToDate` | 最新版です（%s） | Up to date (%s) |
| `updateChecking` | `checking` で、まだ答えがない | 新しい版を確かめています… | Checking for updates… |
| `updateAvailableFormat` | `Available` | 新しい版 %s があります | Version %s is available |
| `updateButton` | `Available` で `installable` | 更新する | Update |
| `updateManual` | `Available` で `installable` が false | ここからは入れられない版です。GitHub から手で更新してね | This version can't be installed from here. Update by hand from GitHub |
| `updateReleasePage` | リリースページの URL の前 | リリースページ: | Release page: |
| `updateConfirmFormat` | 「更新する」を押したあとの確認 | %s に更新しますか？ | Update to %s? |
| `updateConfirmHint` | 確認の補足 | ダウンロードして入れ替えます。途中でこの画面が閉じて開き直すことがあります | It downloads and installs the new version. This panel may close and reopen meanwhile |
| `updateConfirmYes` | 確認の実行ボタン | 更新する | Update |
| `updateConfirmNo` | 確認のやめるボタン | やめる | Cancel |
| `updateInstallingFormat` | `Installing`（`%s` は下の手順） | 更新中: %s | Updating: %s |
| `updateInstalledFormat` | `Installed`（古い版がまだ動いている） | %s を入れました。開き直すと新しい版になります | %s is installed. Reopen to use it |
| `updateInstallFailed` | `InstallFailed`（下の理由を続ける） | 更新できませんでした（今の版のままです）: | The update failed (nothing was changed): |
| `updateCheckFailed` | `CheckFailed`（下の理由を続ける） | 新しい版を確かめられませんでした: | Couldn't check for updates: |
| `updateCheckNow` | もう一度確かめるボタン | 今すぐ確かめる | Check now |
| `updateRetry` | `InstallFailed` のやり直しボタン | もう一度 | Try again |
| `updateDismiss` | `Installed` / `InstallFailed` を閉じる | 閉じる | Close |
| `updateLogHint` | 失敗したときの補足 | くわしくは ~/.cache/<アプリ>/update.log | Details: ~/.cache/<app>/update.log |

## 新しい版の要約（`UpdateStatus::notes` / `notesJa`、0.2.0 から）

`Available` のとき、更新の行の下に新しい版の要約を出す（リリースの本文から frame-update.sh が取る。訳さずにそのまま出す）。

- 日本語の画面で `notesJa` が空でなければ `notesJa`、ほかは `notes`
- どちらも空なら何も出さない（前置きの文言も出さない）
- 折り返して 3 行まで（あふれたら最後の行を `…` で切る）

## 更新中の手順（`UpdateStatus::step`）

| step | 日本語 | English |
|---|---|---|
| `start` | 準備中 | Preparing |
| `download` | ダウンロード中 | Downloading |
| `verify` | ファイルを確認中 | Verifying |
| `extract` | 展開中 | Unpacking |
| `install` | 入れ替え中 | Installing |

## 理由（`UpdateStatus::error`）

知らないコードが来たら `other` を出す。

| error | 日本語 | English |
|---|---|---|
| `network` | GitHub につながりません | Can't reach GitHub |
| `rate-limited` | GitHub の回数制限にかかりました。1 時間ほどあとで試してね | GitHub's rate limit was hit. Try again in an hour |
| `not-found` | 公開されている版がありません | No published release |
| `bad-response` | GitHub の返事を読めませんでした | Couldn't read GitHub's answer |
| `bad-version` | 版の番号を読めませんでした | Couldn't read the version number |
| `bad-url` | GitHub 以外の場所へ向かったので止めました | Stopped: the download led outside GitHub |
| `missing-tool` | 必要なコマンド（python3）がありません | A required command (python3) is missing |
| `no-checksums` | この版には確認用の SHA256SUMS がありません。手で更新してね | This release has no SHA256SUMS. Update by hand |
| `no-asset` | この版には入れるファイルがありません | This release has no file to install |
| `checksum-mismatch` | ダウンロードしたファイルが壊れています | The download is corrupt (checksum mismatch) |
| `unsafe-archive` | ファイルの中身が安全でないので止めました | Stopped: the archive has unsafe contents |
| `no-installer` | ファイルに install.sh がありません | The archive has no install.sh |
| `install-failed` | install.sh が失敗しました | install.sh failed |
| `bad-args` | 前回のインストールのオプションを読めません | The saved install options are invalid |
| `busy` | 別の更新が動いています | Another update is running |
| `not-newer` | もう最新版です | Already up to date |
| `detach-failed` | 更新を始められませんでした（systemd-run） | Couldn't start the update (systemd-run) |
| `interrupted` | 更新が途中で止まりました | The update was interrupted |
| `io` | ファイルを書けませんでした | Couldn't write files |
| `usage` / `script-failed` / `spawn-failed` | 更新の仕組みが動きませんでした | The updater didn't run |
| `other` | うまくいきませんでした | Something went wrong |
