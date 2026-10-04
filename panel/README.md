# frameeyeosc-panel

frameeyeosc の設定を、Steam Frame を被ったまま SteamVR のダッシュボードから変えるパネル（C++）。

- パネルがするのは、設定ファイル `config.json` を書き換えることと、状態ファイル `status.json` を読んで表示することだけ。送信は frameeyeosc 本体がする
- パネルを閉じても、落ちても、入れていなくても、本体はそのまま送り続ける
- 本体は設定ファイルを 1 秒に 10 回見ていて、変わったら再起動なしで反映する

## 画面

ダッシュボードの下の並びに「Eye」のアイコン（目の絵）が出る。選ぶとパネル（1200×700 px、幅 2.8 m）が開き、レーザーポインターで押して操作する。

左の列は、どのタブでも出ている今の状態:

- バッジ: 「● 送信中」「○ 目のデータ待ち」「‖ 一時停止中」「✕ 本体が動いていません」
- 送り先（例: `VRChat → 192.168.0.60:9000`、LiveLink なら `VRCFT（LiveLink）→ 192.168.0.60:11111`）と、自動（Steam Link の相手）か固定か
- 毎秒の送信回数と、目のデータ（トラッカーから毎秒届いたサンプル、状態ファイルの `tracker_rate`）。目のデータが 60 未満なら赤で「少なめ」を付ける（毎秒 15 や 46 のことがあった）。そのときはその下に 1 行、遅いのはどちらか: 本体が読みそびれた分（`missed_rate`）を足せば 60 に届く、または 1 つに 1/60 秒以上かかった（`max_processing_ms`）なら「本体の処理が追いついていません」、そうでなければ「Frame から届く数が少なめです」。この行のぶん、まぶたの段が下にずれる
- まぶた: 左右それぞれ、細い灰色の棒が生の値（倍率を掛けた後）、太い色の棒が送った値
- 視線: 枠の中に、輪が生の値、塗った点が送った値。「左右の目を別々に動かす」がオンのときは「左目」「右目」の枠を 2 つ並べ、それぞれの目の生の値（輪、どちらも同じ色）と送った値（点、左は水色・右はオレンジ）、その下に送った x / y を出す。自分から見た向きのまま（左目が左、＋ x は右、＋ y は上）。状態ファイルの `raw` / `sent` の `gaze_left` / `gaze_right` を読む。「Track Dominant Eye Only」の設定（VR Settings > General の詳細、SteamOS 0.4.3）がオンのあいだは、見出しの行の右に「Frame の設定: 右目だけで追っています」（状態ファイルの `dominant_eye`。目ごとの枠のときは凡例の代わりにここに出す）
- いちばん下に赤で 1 行（2 行まで）: パネルが設定を書けなかった、設定ファイルが壊れている、自動起動の切り替えに失敗、本体が目のデータを読めない理由（状態ファイルの `source_error`。「目のデータを読めません: …」と出し、バッジは「目のデータ待ち」のまま）、本体が報告した設定のエラー（この順で 1 つだけ）
- 赤の行が無いときは、新しい版があるあいだ・更新中・入れ終わったあと、いちばん下にお知らせ（「v0.4.1 があります」など）。押すと詳細タブへ

右はタブ:

| タブ | 項目 |
|---|---|
| 基本 | 送信（送る / 止める）、送り先（3 枚のカード: VRChat に直接 / VRCFT（LiveLink）「おすすめ」 / VRCFT（ETVR）。それぞれ 3 行で 見開き・同期・VRCFT が要るか。選んだカードは太い枠と左上の ✓、コマンドで固定中は灰色）、言語（日本語 / English）、SteamVR と一緒に起動（オン / オフ）、すべて既定に戻す、アプリを終了 |
| 送り方 | 送り先の PC（自動 / 今の相手で固定 / IP を入力。手で決めた host は 3 つ目に「手動 192.168.1.20」と出る）、ポート（− / ＋、既定に戻す。ヒントに送り先の種類の既定）。VRChat に直接のときだけ パラメーター名の頭（/FT / なし、送るアドレスの例）、EyeTrackingActive の型（Bool / Float / 送らない、`eye_tracking_active`）、Steam Link の名前も送る（オン / オフ、`steamlink_params`。Steam Link の OSC 向けのアバター用。送るアドレスの例に「頭はつけない」）、VRChat 標準の目も動かす（オン / オフ、`native_eyes`。VRCFT 用の値がないアバター用）。LiveLink / ETVR のときは「PC の VRCFT で準備すること」の 3 手順と注記。目のカメラがあるときは、LiveLink なら「瞳孔は VRChat に直接送る」（`pupils_to_vrchat`）、VRChat に直接と LiveLink なら「瞳孔の受け取り方」（小数 / 1 / 2 / 3 / 4、`pupil_bits`。PupilDilation1・2・4・8 の bool も送る個数。瞳孔を直接送っていないとき＝LiveLink で `pupils_to_vrchat` がオフか `camera_lids` がオフのときは灰色で押せない） |
| 視線 | スムージング（オン / オフ）、なめらかさ（弱 / 中 / 強）、細かく変える（止まっている時・速い動き・変化の感度の 3 つを − / ＋）、見つめている時の遊び（角度も表示）、まばたき中は視線を止める（オン / オフとしきい値）、左右の目を別々に動かす（Frame が片目だけで追っているあいだは、ヒントが「片目だけ追跡中は両目が同じ向き」になる）、不確かな視線を使わない（オン / オフと上限）、一瞬の途切れを消す（オン / オフ） |
| 目を合わせる | ［目を合わせる］（合わせたあとは［もう一度合わせる］）と小さい［正面だけ合わせ直す］（「被ったとき」が正面と傾きのときは［正面と傾きを合わせ直す］）、「被ったとき」の行（何もしない / 正面だけ / 正面と傾き、`auto_recenter`。既定は正面だけ）、今どうなっているか（始め方・ダッシュボードを閉じると始まります［やめる］・測っています・結果［元に戻す］・失敗の理由）、［♪ 音を鳴らす: オン / オフ］、「細かく直す」（開閉。［視線 / まぶた］で切り替え。視線: 正面の位置・動く幅、真下で左右を止める角度、目ごとの左右の正面の位置・動く幅。まぶた: 目ごとの読んだ値 閉じ / 上 / 正面 / 下 の − / ＋） |
| まぶた | 見た目の調整を全部ここに（案E）。上から: 「いまのまぶた」の帯（目のカメラ（左・右）/ 目のカメラ（左）＋ Valve（右）/ Valve の値。右に「切り替えは「目のカメラ」タブで」か「目のカメラは準備してないよ」）、「見開きの出やすさ」のスライダー 1 本（鈍い ↔ 敏感。両目がカメラなら eyecam-rec の `widen_sensitivity` を `set widen_sensitivity` で送る。片目だけカメラなら同じく感度を送り、Valve の目には近い `lid_widen` の段階も一緒に書く。Valve の値なら `lid_widen` の 4 段階（しない / 控えめ / ふつう / 出やすい）で、押した所に近い止まり位置へ。目を合わせていない目があると下に「目を合わせていない目は、細かく直すの目盛りで決まるよ」。ふつうに開いた目が 1.0 と読まれる SteamOS（`openness_saturated`）でカメラが無いときは灰色にして、「Valve の値だけだと見開きが出ない」の箱と［目のカメラへ］）、まばたきを届ける（保持 ms の − / ＋）、まばたきを両目でそろえる（`blink_sync_below` の − / ＋、0 でオフ）、まぶたのなめらかさ（弱 / 中 / 強。中 = 本体の既定値、どれにも合わなければカスタム）、左右をそろえる強さ（`lid_sync` のスライダー、0.05 刻み）、そのほか［細かく直す ▼］。開くと行の代わりに、自動キャリブレーション（目を合わせていない目で、カメラでないとき）、左右の倍率、生の値の棒と 4 つの目盛り（③④は目を合わせた目とカメラのときは灰色）、なめらかさの 2 つの値（止まっている時・速い動き）。`--lid-marks` で開いて描く |
| 目のカメラ | eyecam-rec が動いているあいだだけ出る（「詳細」の前。出る条件は下の「目の撮影」と同じ）。準備が済むまでは準備のチェックリスト（いまの段階は、満たしていない最初のもの。毎回 status.json とパスワードの確認だけで決める）: ① パスワードを決める（SteamOS にパスワードが無いと分かっているときだけ。`steamos-passwd --has-password` を裏で数秒おきに確かめる。読むだけ。［Konsole で passwd を開く］）② 道具を入れる（［Konsole で開く］で `sudo $HOME/.local/lib/eyecam/install_grab.sh` が入力済みの Konsole を開くだけ。Enter とパスワードは本人で、パネルは sudo も passwd も実行しない。`auto_grab` が ok / trying / waiting_tracker / failed: … か、バッファを持っていれば入っている扱い。`auto_grab` が `too_old`（eyecam の安全のための下限より古く、起動されない）なら入っていない扱い。`grab_outdated` が true（更新で新しい道具が届いた）か too_old なら、見出しを「道具を入れ直す」、その下に「道具が新しくなったよ、入れ直してね」）③ 目の動きを覚える（両目が見えているときだけ［覚えはじめる］で `calib wear`。18 秒の間、ステップのチップ・残り秒・［やめる］をカードの中に出す。見開きだけ取れなかったら［このまま進む］［もう一度（18秒）］）④ 完了（「使いはじめる」）。セットアップ済みでも道具が無くなれば ② に戻り、直れば校正し直さずにふだんの画面へ。道具が古いだけなら ② には戻さず、ふだんの画面のいちばん上にお知らせのカードと［Konsole で開く］（`grab_outdated`: 「道具が新しくなったよ、入れ直してね（今の道具のままでも動くよ）」、`too_old`: 赤いカードで「安全のため、道具を入れ直してね。入れ直すまでカメラは止まってて、まぶたは Valve の値だけで送ってるよ」）、左の列にも同じお知らせ（押すとこのタブ）。eyecam-rec が言わなくなれば自然に消える（`eyecam::toolState` / `toolNotice`。`--fake-eyecam …:outdated`、`waiting:auto=too_old:setup:outdated` で描く）。左の列には、準備が済むまで「次にやること・目のカメラ」、済んだ直後に緑の「目のカメラ：準備できたよ」。済んだあとのふだんの画面: いまの状態（両目・片目・Valve の値とその理由）、カメラで瞼を取る（`camera_lids`）、違和感があるときの［目のカメラの校正（18秒）］、細めも送るときの［ユーザー校正（最初に 1 回）］（どちらも ③ と同じカードで進み、終わると結果のカード）、こんなときは。見開きの出やすさは「まぶた」タブ。`--fake-eyecam` と `--fake-password`、`--fake-camera` で描く |
| 詳細 | バージョン（今の版と最後に確かめた時刻、［今すぐ確かめる］／新しい版があれば［更新する］、確認を 1 回はさむ。更新に失敗したら［もう一度］［閉じる］。その下に「起動時と 1 日 1 回確認 オン / オフ」のチップ、押すと切り替わる。新しい版があるときは、さらにその下にその版の要約を 3 行まで。日本語の画面ではリリースの「日本語:」の段落、無ければ英語の要約。要約の無いリリースなら何も出さない）、［更新履歴］（バージョンの見出しの下、チップの左。下を参照）、「調べる道具」: 視線の点を表示（デバッグ用、オン / オフ）と点の距離（0.3〜2.0 m、0.1 m 刻み。1.2 m くらいより奥だと開いたダッシュボードに隠れる。点がオフの間は灰色）、目のログ、「ファイルと本体」（小さい字）: 設定ファイル・キャリブレーション・状態ファイルの場所、本体の PID と動いている時間、コマンドで固定中の項目と今の値。eyecam-rec が動いているあいだは「開発用」の「目の撮影」（［撮影開始］で下の光の注意。ボタンの右に「目の映像が ~/eyecam/rec_日時/ に残るよ（1 回で約 2.5 GB）」） |

- 更新履歴（`changelog.{h,cpp}`）: 詳細タブの中身と入れ替わって開く。上に「更新履歴」と［閉じる］、その下に版ごとの行を新しい順に並べる。たたんだ行は「0.7.1 · 10/3」とその版の要約を 1 行（はみ出す分は …。要約の段落が無い版は最初の項目）。開くのは 1 行だけで、別の行を押すと前の行は閉じ、開いた行をもう一度押すと閉じる。開いた行は要約の全文と項目を折り返して出す。開いたときは入っている版の行（変更履歴に無ければいちばん新しい版）が開いている。ほかのタブを押すと閉じる
  - 読むもの: 英語の画面は `CHANGELOG.md`、日本語の画面は `CHANGELOG.ja.md`（その版が無ければ `CHANGELOG.md` の英語）。どちらも `## X.Y.Z (YYYY-MM-DD)` の節だけを読み（`## Unreleased` などは飛ばす）、段落と `- ` の項目に分け、バッククォート・リンク（文字だけ残す）・太字を外す。英語は「日本語:」で始まる段落を飛ばした最初の段落を要約にし、項目は最初の文だけ（最初の「. 」か「。」まで。e.g. / i.e. / vs. では切らない）を 2 行まで。日本語は書いてあるまま
  - 探す場所（最初に `CHANGELOG.md` があったフォルダから両方を読む。日本語の画面で `CHANGELOG.md` がどこにも無ければ `CHANGELOG.ja.md` だけのフォルダ）: パネルの実行ファイルと同じフォルダ（配布の tar.gz）→ `…/panel/build` から動かしたときはそのリポジトリ → `~/.local/share/frameeyeosc/`（`install.sh` と `contrib/install-panel.sh` が置く）。開くたびに読み直す。見つからなければ「更新履歴が見つかりません」
  - スクロール: 開いている間だけ、パネルのオーバーレイに `VROverlayFlags_SendVRSmoothScrollEvents` を付け（閉じたら外す）、`VREvent_ScrollSmooth`（と、来たときは `VREvent_ScrollDiscrete`）でスクロールする。`ydelta` 1 あたり 120 px（Discrete は 1 段 80 px）、正の値が上へ。速さと向きは実機でまだ確かめていないので、最初の 12 回は `[VR] scroll smooth: ydelta …` とログに出す。右の ▲ / ▼ は 1 回で見えている高さの 1/3。スクロールは中身の範囲で止まり、行を開いたときはその行（入りきらなければ見出し）が見える位置まで動く。行は見えている範囲で切り取り、押せるのも見えている部分だけ
- まぶたタブの棒は、目を開け閉めしながら目盛りの線を今の値に合わせるためのもの。棒の範囲は 0〜1.2
- 目盛りは「閉じ < 普通 ≦ 見開き始め ≦ 見開き最大」の順を崩さないよう、− / ＋ で動ける範囲を制限している
- なめらかさの 弱 / 中 / 強 は、視線の 3 つの値の組み合わせ（中 = 本体の既定値）

| | 止まっている時（`gaze_min_cutoff`） | 速い動き（`gaze_beta`） | 変化の感度（`gaze_d_cutoff`） |
|---|---|---|---|
| 弱 | 0.5 | 3.0 | 0.8 |
| 中 | 0.3 | 1.5 | 0.5 |
| 強 | 0.2 | 0.8 | 0.3 |

- どれもやわらかく付いていく組み合わせで、弱から強へ、止まっている時がより安定し、素早く目を動かしたあと落ち着くまでが長くなる（60 分の記録 2 本で、動きの 90% に届くまで 弱 167・中 256・強 422 ms と 156・289・433 ms）。強は VRChat で試して選んだ値で、0.6.x までの 強 0.2 / 0.4 / 0.3（すべり続ける、589 ms）と、0.7.0 の試作のきびきびした 0.2 / 1.5 / 2.5（パッと飛ぶ、156 ms）のあいだ。中と弱も、同じ名前の古い組み合わせ（0.6.x までの 弱 1.0 / 1.5 / 1.0・中 0.4 / 0.8 / 0.5）ときびきびした組み合わせのあいだ。設定ファイルの `version` が 1（か無い）なら、パネルが最初に読んだときに 1 回だけ、古い組み合わせとぴったり同じ値を同じ名前の新しい組み合わせに置き換え、そのとき「見つめている時の遊び」が古い既定の 0.02 なら新しい既定の 0.005 にする。それ以外の値はそのまま。そのあと `version` を 2 にする（`migrateGazePresets`）

操作の決まり:

- 押すと、設定ファイルを読み直す → その項目だけ変える → 書く。書いた結果はすぐ画面に出る
- 送り先の種類を変えると、ポートはその種類の既定（VRChat 9000、LiveLink 11111、ETVR 8889）に戻り、「〜向けのおすすめ設定にする？ する / しない」を 1 回だけ聞く。「する」で変わるのは視線のなめらかさ 3 つとまぶたのなめらかさ 2 つだけ（VRChat と LiveLink = すべて既定値、ETVR = 視線は既定値・まぶたは 10.0 / 10.0。ETVR のモジュールがまぶたを自分でもなめらかにしているため。LiveLink のモジュールは何もなめらかにしない）
- 「今の相手で固定」は、本体が今送っている相手の IP を `host` に書く（ポートはそのまま）。本体が動いていない、または相手が見つかっていないときは押せない
- 「IP を入力」（`host_entry.{h,cpp}`）はパネルの上に IPv4 用のテンキー（0〜9・.・⌫、15 文字まで、［決定］［やめる］）を開き、今の `host` が IP アドレスならそれから始める（名前や auto なら空）。パネルの中で描くので `--dump-png` と `--click` で確かめられる。［決定］では空と、4 つの 0〜255 の数を . で区切った形でないものを断って理由を出し、よければ `host` に書いて閉じる。ホスト名は `config.json` の `host` を手で書くときだけ（行には「手動 名前」と出て、［自動］で戻せる）。SteamVR のキーボードは実機で打てなかったので使わない
- 目合わせが止まったとき、理由の下に数字を 1 行出す（`fit_text.{h,cpp}`）: 落ち着かなかった点は使えたサンプル / 45・ばらつき（° と 2.7° まで）・回数、目を閉じる手順は目ごとの開き具合と「正面の 7 割」未満の値、動かなかった点は動いた角度と必要な角度（目ごとの左右なら目）、まぶたの差が小さいときはどの目のどの点か。試すたびに `[fit] center try 2: 128 samples (min 45), spread 3.4° (max 2.7°) -> again` のようにログにも出す
- 「すべて既定に戻す」と「アプリを終了」は、押すと 3 秒間「もう一度押すと〜」になり、その間にもう一度押したときだけ実行する
- 「すべて既定に戻す」は、言語・キャリブレーションのリセットの回数・パネルが知らない項目は残す
- 言語: `config.json` に `language` が無いときは、起動時に 1 回だけ `~/.steam/registry.vdf` の `language` を読み、`japanese` なら日本語、それ以外は英語（読めなければ `LC_ALL` / `LC_MESSAGES` / `LANG`）。こうして決めた言語はファイルに書かず、言語のボタンを押したときだけ保存する
- 「リセット」（キャリブレーション）は `calibration_reset` を 1 増やすだけ。覚え直すのは本体
- 選択状態は色だけで伝えない（✓・塗り・太字。タブは塗りと下向きの印）

## 本体との関係

| ファイル | 場所 | パネル |
|---|---|---|
| 設定 `config.json` | `$XDG_CONFIG_HOME/frameeyeosc/config.json`（無ければ `~/.config/frameeyeosc/config.json`） | 読んで書く（書くのはパネルだけ） |
| 状態 `status.json` | `$XDG_RUNTIME_DIR/frameeyeosc/status.json`（無ければ `/run/user/<uid>/frameeyeosc/status.json`） | 読むだけ |
| 更新 | `~/.local/share/frameeyeosc/frame-update.sh`、`~/.cache/frameeyeosc/`（`update-check.json`・`update-state.json`・`update.log`） | スクリプトを動かし、状態ファイルを読む |
| 変更履歴 `CHANGELOG.md`・`CHANGELOG.ja.md` | パネルの隣、開発ビルドならリポジトリ、`~/.local/share/frameeyeosc/`（上の「更新履歴」を参照） | 更新履歴を開いたときに読むだけ |

- 書き方: 同じフォルダの `config.json.tmp` に書く → `fsync` → `rename`（→ フォルダも `fsync`）。読み込んだ JSON の中身を書き換えて書き戻すので、パネルが知らないキーも消えない。ファイルが無いときは、全部の項目を既定値で書いたファイルを作る
- 設定ファイルが壊れた JSON のときは、ふつうのボタンでは書かない（中身を失わないため）。「設定ファイルが壊れています」と赤で出る。「すべて既定に戻す」だけは、壊れたファイルを `config.json.broken` に写してから既定値で作り直す
- 状態ファイルが無い、`time` が 3 秒より古い、`pid` のプロセスがいない、のどれかなら「本体が動いていません」。それでも設定は変えられる（本体が起動したら反映される）
- 本体のコマンドラインで指定した項目（状態の `locked`）は、グレーにして鍵の印と「コマンドで固定中」を出し、値は状態の `effective` から出す。押せない
- 本体が別の設定ファイル（`--config`）を読んでいるときは、詳細タブに赤で出す
- 状態ファイルは、パネルが**開いている間だけ** 1 秒に 10 回読む。設定ファイルは同じときに更新時刻と大きさだけ見て、変わっていたら読み直す。表示に出る値（丸めた値）が変わったときだけ描き直す
- 閉じている間はどちらのファイルも読まず、描かない（更新の状態ファイルと、目を合わせている間は別。下を参照）
- 更新の確認だけは閉じている間も動く: 起動時と 1 時間ごとに `frame-update.sh check` を裏で動かす（GitHub に行くのは、スクリプトが覚えている答えが 24 時間より古いときだけ。前回失敗していたら 1 時間）。`update_check` が false なら動かさない。［今すぐ確かめる］は 24 時間を待たずに `--force` で確かめ、`update_check` が false でも使える
- ［更新する］→ 確認で「更新する」を押すと `frame-update.sh install --detach` を動かす。スクリプトが SHA256SUMS で確かめてから、新しい版の `install.sh` を前回と同じオプション（`~/.config/frameeyeosc/install-args`、無ければ `--with-panel`）で、systemd のユーザーユニット `frameeyeosc-update` の中で実行する。`install.sh` がパネルを再起動しても更新は続く。パネルは `update-state.json` を 0.5 秒ごとに読んで進み具合を出す。これはパネルが動いている間ずっと（閉じている間も）続く
- 目を合わせる（`gaze_fit.{h,cpp}`）: ［目を合わせる］か［正面だけ合わせ直す］で始め、「ダッシュボードを閉じると始まります」と出して待つ（1 分まで、［やめる］で中止）。`IsDashboardVisible` でダッシュボードが閉じたのを見たら、頭に固定した目印（`target.cpp` の絵、別のオーバーレイ `sasaken.frameeyeosc-panel.target`）を出す。点は正面・上・下・左・右の順で、1 点 2.5 秒（`kPointSec`、これ 1 つで速さが変わる）。点から点へ 0.35 秒でなめらかに動き、0.5 秒後に `config.json` の `gaze_capture` に `{"id": 次の番号, "target": "center", "seconds": 2, "skip": 0.3}` などを書く（本体は 2 秒測り、最初の 0.3 秒は捨てる）。輪は 2.5 秒かけて減り、測っている間だけ点の下に残りの秒数（2・1）を出す。本体が状態ファイルの `gaze_capture` で同じ id の `done` を返したら次へ。使えたサンプルが、届いたサンプル（本体が `received` で返す、目を閉じていたものも含む）の 6 割未満か 12 未満（ただし求めるのは 45 まで。ふつうの回数では 0.5.2 と同じ）（`received` を返さない 0.5.3 より前の本体なら 45 未満）、またはばらつきが 0.06 を超えたら同じ点を 3 回までやり直す（トラッカーの回数は毎秒 90〜136 のことも 15 のこともあるので、決まった数にしない）。5 点が終わったら視線の計算を確かめてから、目印に「3 秒間 目を閉じて」と 3・2・1 を出してから `target: "closed"` を 3 秒（最初の 0.5 秒は捨てる）頼み（目を閉じたサンプルも使い、視線は使わない）、左右とも正面の開き具合の 7 割より下なら「開けて OK」を 1.5 秒出して終わり。8 秒答えが無い・本体が止まった・ダッシュボードが開いた・点がほとんど動かなかった・目が閉じなかった・まぶたの開け閉めの差が 0.1 未満だったら止めて理由を出す。終わったら正面の位置・幅・目ごとの左右（`gaze_offset_x_left/right`・`gaze_gain_x_left/right`）・目ごとのまぶたの 4 つの値を書く（正面だけのときは正面の位置と目ごとの左右の 0 点だけ）。目ごとの左右は、HMD の `Prop_UserIpdMeters_Float`（読めないか 45〜85 mm の外なら 63 mm）から、2 m 先の点をその目から見た角度 atan2(2·sinθ ± IPD/2, 2·cosθ)（左目 +、右目 −、+ は右）に合わせる
- 視線の点（デバッグ用、`gaze_dots.{h,cpp}`）: `gaze_debug_dots` がオンの間、本体は処理したサンプルごとに送っている視線を 40 バイトのデータグラム（`FEOD`・版 1・フラグ・時刻・視線 6 つ、リトルエンディアン。`src/dots.rs`）で状態ファイルのフォルダの `gaze-dots.sock` に送る。パネルはそこに Unix データグラムソケットを開き（古いファイルは消してから）、届いた最新の 1 つで点を置く。まとめた視線なら 1 つ（明るい灰色）、`independent_eyes` なら目ごとに 2 つ（左は水色、右はオレンジ。明るさも違う）で、それぞれの目の位置（IPD の半分ずつ左右）からその目の向きに 1 m 先（下を参照）。点は別々のオーバーレイ（`sasaken.frameeyeosc-panel.dot0/1`、幅 3.5 cm）で、絵は 1 回だけ送り、あとは位置（transform）だけ動かす。点の距離（目からその目の向きに沿って）は `gaze_debug_dots_distance_m`（既定 1.0 m、0.3〜2.0 m。パネルだけが使い、書き換えるとすぐ反映）で、ダッシュボードを開いても閉じても同じなので跳ばない。点は何も付けない普通のオーバーレイ（frame-perf-overlay のパネルと同じ）。並び順（`SetOverlaySortOrder`）や `VisibleInDashboard` を付けると 1 m でもダッシュボードに隠れた。普通のオーバーレイなら、約 1.35 m 先のパネルの上に 0.45・0.8・1.0 m で見え、1.2 m では頭の位置によって隠れた。幅は距離に比例させて（2 m なら 3.5 cm、1 m で 1.75 cm）、見た目の大きさ（約 1°）は変えない。向きは同じ光線の上なので、左右の点が指す方向は変わらない。ただし両目が寄って交わる点（例えば 2 m 先の目印を見ているとき）より手前に出すので、左右の点はその分離れて見える（1 m なら IPD の半分、約 3 cm）。1 つにまとめた点（`independent_eyes` がオフ）は両目の真ん中から出すので 1 つのまま。点がオンの間は、ダッシュボードを閉じていても設定ファイルを読むので、スイッチと距離はすぐ反映する。距離が変わるたびに距離と幅をログに出す。オンの間は 1/90 秒おきに回る。1 秒届かなければ隠す。目を合わせている間とオフにしたときはソケットを閉じて点を隠す
- 目合わせの音（`sounds.{h,cpp}`）: 起動時に 7 つの音を正弦波 / 三角波と短い立ち上がり・減衰で作り（48 kHz モノラル 16 bit、ピーク −14 dBFS）、`$XDG_RUNTIME_DIR/frameeyeosc/sounds/` に WAV で書く。鳴らす場面は目印の見た目に合わせる: 点がすべり終わって止まったら pop、測れたら pip、測り直しなら buzz、「目を閉じて」の 3・2・1 が変わるたびに tick、目を閉じた手順が終わって「開けて OK」になったら open、終わったら done、止まったら fail。`fit_sounds` が false なら鳴らさない
- 目を合わせている間だけは、ダッシュボードが閉じていても 1/90 秒おきに回って目印を毎回描き直し、状態ファイルを 1 秒に 10 回読む。目印 1 枚を描くのは約 0.4 ms（`--target-bench`）。目印を隠したとき、描いた回数と 1 秒あたりの回数をログに出す。終わるか止まったら目印を隠し、元の読み方に戻る
- 目印は `CreateOverlay`（ダッシュボードのオーバーレイではない）で最初に要るときに作り、`SetOverlayTransformTrackedDeviceRelative(HMD)` で 2 m 先、幅 0.3 m に置く（正面、上下 15°、左右 20°）。作ったあとは隠したまま残し、終了時にパネルと同じ順で片付ける
- 点の計算: 正面 = 正面の点の平均。幅 = 目標の角度（45° を 1 とした値）÷（その点の平均 − 正面の平均）、左右は両側の平均、0.5〜2 に丸める。点が目標の 1/4 も正しい向きに動かなかったら失敗にする（向きが逆のときもここで止まる）
- 仕組みは frame-updater の共通部品（`vendor/frame-updater/`、C++ は `update_check.{h,cpp}`）。ここでは書き換えない。画面の文言は `vendor/frame-updater/strings.md` のまま（版の行の見出し「バージョン」と「・確認 %s」だけはこのパネルのもの）

## ビルド（Frame 上）

必要なもの（SteamOS に入っている）: cmake、ninja、g++、pkg-config、cairo、freetype2、Vulkan のヘッダとローダー（`vulkan` の pkg-config）、SteamVR（`/opt/steamvr/bin/linuxarm64/libopenvr_api.so`）。
`openvr.h` は `third_party/openvr/` に同梱（OpenVR SDK 2.15.6、BSD-3-Clause。`third_party/openvr/LICENSE`）。

PC から送ってビルドする例（Git Bash で、このフォルダから）:

```sh
tar --exclude=build --exclude=out -cf - . | ssh steamos@<Frame の IP> 'mkdir -p ~/frameeyeosc-panel && tar -xf - -C ~/frameeyeosc-panel'
ssh steamos@<Frame の IP> 'cd ~/frameeyeosc-panel && cmake -G Ninja -S . -B build && ninja -C build'
```

実行ファイルは `build/frameeyeosc-panel`。OpenVR のライブラリの場所は rpath に入っているので、別の場所にコピーしても動く。`-Wall -Wextra` で警告ゼロ。

## 手動で動かす

```sh
./build/frameeyeosc-panel          # ダッシュボードにパネルを出して常駐（Ctrl+C で終了）
```

- SteamVR が起動していなければ 3 秒おきに待つ（SteamVR を勝手に起動はしない）
- SteamVR が終わる（`VREvent_Quit`）と静かに終わる（終了コード 0）
- すでに常駐しているときにもう一度起動すると、SteamVR にはつながず、常駐しているほうに SIGUSR1 を送ってすぐ終わる。常駐側はダッシュボードを開いてパネルを出す
- 常駐の見分けは `$XDG_RUNTIME_DIR/frameeyeosc-panel.lock` のロック（flock）と、そこに書いた PID

確認用のオプション（`--probe` 系以外は OpenVR なしで動く）:

```sh
./build/frameeyeosc-panel --print                 # 設定・状態・自動起動を、パネルが読んだとおりに表示
./build/frameeyeosc-panel --dump-png out/panel_2026-09-27_00-00-00.png --tab lids --language en
./build/frameeyeosc-panel --dump-png out/output_2026-09-30_00-00-00.png --tab output --fake-livelink   # 送り方タブ（LiveLink のとき）
./build/frameeyeosc-panel --dump-png out/etvr_2026-09-27_00-00-00.png --fake-etvr --fake-prompt etvr
./build/frameeyeosc-panel --dump-png out/t_2026-09-27_00-00-00.png --config /tmp/t/config.json --click 883,148
./build/frameeyeosc-panel --thumbnail-png out/thumbnail_2026-09-27_00-00-00.png --thumbnail-size 256
./build/frameeyeosc-panel --dump-png out/update_2026-09-27_00-00-00.png --fake-update available --tab advanced
./build/frameeyeosc-panel --dump-png out/history_2026-10-03_00-00-00.png --fake --history --language en   # 更新履歴（入っている版が開く）
./build/frameeyeosc-panel --dump-png out/history_2026-10-03_00-00-01.png --fake --history-open 0.5.0 --history-scroll 200
./build/frameeyeosc-panel --dump-png out/fit_2026-09-28_00-00-00.png --tab eyefit --fake-fit fitted --fit-details   # 目を合わせるタブの各状態
./build/frameeyeosc-panel --dump-png out/eyes_2026-09-28_00-00-00.png --tab gaze --fake-independent   # 左の列の視線を目ごとに
./build/frameeyeosc-panel --target-png out/target_2026-09-28_00-00-00.png --target-style close --target-seconds 2 --target-bench 900
./build/gaze-fit-test                            # 目合わせの計算と手順のテスト（OpenVR なし）
./build/gaze-dots-test                           # 視線の点のデータ・位置・ソケットのテスト（OpenVR なし）
./build/text-test                                # テンキーで打った送り先の IP の確認と、目合わせの失敗の文（日本語・英語）のテスト
./build/sounds-test                              # 目合わせの音の WAV と、どの場面でどの音かのテスト（鳴らさない）
./build/changelog-test                           # 更新履歴の読み方（英語と日本語の形、「日本語:」を飛ばす、最初の文、マークダウン、英語で補う）のテスト
./build/eyecam-test                              # 目の撮影タブ（状態ファイル、出す・消す条件、指示の文、全面の光とそのフェード、光の注意と送るコマンド、/tmp の偽ソケット）のテスト
./build/frameeyeosc-panel --dump-png out/eyecam_2026-10-03_00-00-00.png --fake-eyecam recording:widen --tab eyecam
./build/frameeyeosc-panel --dump-png out/eyecam-confirm_2026-10-04_00-00-00.png --fake-eyecam confirm --tab eyecam
./build/frameeyeosc-panel --eyecam-fill-png out/fill_2026-10-03_00-00-00.png --eyecam-fill dark --language en
./build/frameeyeosc-panel --play-sound open      # 音を 1 つ鳴らして聞く（pop / pip / buzz / tick / open / done / fail）
./build/frameeyeosc-panel --dot-png out/dot_2026-09-28_00-00-00.png --dot-kind left
./build/frameeyeosc-panel --version               # 版（Cargo.toml から）
./build/frameeyeosc-panel --contrast-report       # 色の組み合わせごとのコントラスト比と合否
./build/frameeyeosc-panel --probe                 # 常駐しているパネルを SteamVR 経由で探して状態を出す
./build/frameeyeosc-panel --probe-switch-away 3   # ダッシュボードを一時的な別のオーバーレイに切り替える（閉じたときの確認用）
```

- `--dump-png` は今の設定ファイルと状態ファイルで描く。`--config PATH`・`--status PATH` で別のファイルを読める
- `--fake` か `--fake-*` を付けると、ファイルを読まずに作り物の状態で描く: `--fake-not-running`・`--fake-paused`・`--fake-no-tracking`・`--fake-etvr`・`--fake-livelink`・`--fake-fixed`・`--fake-target-null`・`--fake-locked`・`--fake-config-error`・`--fake-source-error`・`--fake-dominant-eye left|right`・`--fake-openness-saturated`・`--fake-broken`・`--fake-write-error`・`--fake-custom`・`--fake-prompt vrchat|etvr|livelink`・`--fake-autostart on|off|missing|unknown`。`--preview-quit`・`--preview-reset` で「もう一度押すと〜」の見た目
- 更新の見た目は `--fake-update checking|uptodate|available|manual|installing|installed|checkfailed|installfailed`。`--preview-update-prompt`（`--fake-update available` と一緒に）で更新の確認。`--fake-update-notes both|en|long`（`available`・`manual` と一緒に）で新しい版の要約: 英語と日本語、英語だけ、どちらも 300 文字の長さ
- `--update-live` を付けると本物の更新の仕組みを動かす: 最初に確認し、`--click` のあとは始まった確認や更新が終わるまで待ってから描く。更新は本当に行われるので、偽の GitHub（`FRAME_UPDATE_API_URL`・`FRAME_UPDATE_ALLOW_INSECURE=1`）と別の `HOME` で試す
- 更新履歴は `--history` で開き、`--history-open 版` でその版の行を開き、`--history-scroll px` でスクロールして描く（どちらも `--history` を含む。スクロールは中身の範囲に収める）。変更履歴は上の場所から探す。`--changelog-dir DIR` でそのフォルダだけを見る（無いフォルダなら「見つかりません」の見た目）。`--fake` とも一緒に使える
- `--click X,Y`（何回でも）は、描く前にその座標を押したことにする。当たり判定と設定ファイルの書き込みをヘッドセットなしで確かめる用（`--fake` とは一緒に使えない。`--config` の設定ファイルを本当に書き換えるので、試すときは別の場所を指定する）
- 目の撮影タブ（下）は `--fake-eyecam waiting|idle|confirm|searching|recording:ラベル|error` と `--tab eyecam` で描く（`confirm` は idle で光の注意を開いたところ。`recording:ラベル:unlocked` でカメラが目を見失ったとき＝「HMD をかぶってください」、`recording:ラベル:nolight` で光なしのプロトコル＝「光なし」。ラベルは lead_in・normal・widen・close・squint・look_up・look_down・bright・dark・end。recording は 9 ステップ・120 秒のうちのそのステップの数字が入る。error は失敗した返事も出す）。`--eyecam-fill-png PATH`（`--eyecam-fill bright|dark`、`--language`）で全面の光の画像。`--eyecam-dir DIR` で eyecam-rec のフォルダ（`status.json` と `ctl.sock`）を別の場所にする。`--fake` なしの `--dump-png` はそこを読み、`--click` で［撮影開始］→［光ありで始める］／［光なしで始める］や［中止］を押すとそのソケットに本当に送って返事を待つ（試すときは一時フォルダに偽の `status.json` とソケットを置く。本物の `/run/user/1000/eyecam/` には何も作らない）
- `--probe` は Background 型でつなぐだけで、オーバーレイも Vulkan も作らない。`FindOverlay`・名前・幅・閉じるボタン・表示中か・`GetOverlayTextureSize` を出す
- `contrib/icons/frameeyeosc-panel-{48,128,256}.png` は `--thumbnail-png` で書き出したもの（ダッシュボードのサムネイルと同じ絵）

### 目の撮影（開発用、eyecam-rec）

目のカメラの録画ツール eyecam-rec（`tools/eyecam`）が動いている間だけ出る。今は自分のタブではなく、「詳細」タブの「開発用」の行と、「目のカメラ」タブ（上の表）に分かれている。下の「タブ」は、その行と撮影中の画面のこと。

- **出る条件**: `$XDG_RUNTIME_DIR/eyecam/status.json`（無ければ `/run/user/<uid>/eyecam/`）があり、`state` が `stopped` でなく、5 秒以内に書かれている（ファイルの更新時刻）。消えたら、このタブを見ていたときは「基本」に戻る。読むのは動いている間 0.1 秒おき、それ以外は 1 秒に 1 回（ファイルが無ければ開けないだけ）。ダッシュボードが閉じていても読む
- **状態ごとの表示**（大きな字。ヘッドセットの中で読む）: `waiting_fds`（道具待ち）はここには出さず、目のカメラタブが扱う（道具が無ければ準備の ② で、一度だけ実行するコマンド `sudo $HOME/.local/lib/eyecam/install_grab.sh` を入力済みの Konsole を開くだけ。パネルは sudo も eyecam-grab も実行しない）。`idle` は大きな［撮影開始］。`searching` は「目を探しています…」と左右の fps と［中止］。`recording` はそのステップの指示をとても大きく（普通に開けて・見開いて！・目を閉じて・目を細めて・上を見て・下を見て・明るい画面を見て・暗い画面を見て・おわり）、ステップの残り秒、「ステップ i / n」（`step_index` は 0 から数えるものとして +1 で出す。`protocol` が `widen_nolight` のときはその横に「光なし」）、全体の進み具合のバー（`elapsed_s / total_s`）、fps、［中止］。`error` はメッセージと［もう一度撮影］。`message` は空でなければいつも下に出し、送ったコマンドの返事が `err` ならそれも赤で出す。ステップの音は eyecam-rec が鳴らすので、パネルは鳴らさない
- **光の注意**: ［撮影開始］（と `error` の［もう一度撮影］）はすぐには始めず、タブの中に赤い「光の注意」（警告の三角と赤い枠）を出す: 「明るい画面・暗い画面の段では、視界全体が白・黒に切り替わります。光過敏性てんかんの心配がある人は、光なしで撮ってください。途中で気分が悪くなったら、［中止］を押して HMD を外してください。」。箱の下に「撮影すると ~/eyecam/rec_日時/ に目の映像が残るよ（1 回で約 2.5 GB）。いらなくなったら消してね」。ボタンは［光ありで始める］（`start`）・［光なしで始める］（`start widen_nolight`。明るい・暗いの段が無いプロトコル）・［やめる］（元の画面に戻る）。どちらの始めるも同じ見た目で、どちらかを勧めない。開いたときの状態（idle か error）から変わったとき、eyecam-rec のタブが消えたとき、別のタブを選んだとき、ダッシュボードを閉じたときは閉じる（`eyecam::StartConfirm`）
- **操作**: 光の注意の始めるボタンは `ctl.sock`（unix stream）に `start` か `start widen_nolight`、［中止］は `stop` を 1 行送る。ノンブロッキングでつないで送り、返事（`ok` か `err 理由`）はループで読む。2 秒で返事が無ければあきらめる。つながらないときもその理由を出す。描画のループは待たない
- **全面の光**: `recording` で `step_label` が `bright` か `dark` の間だけ、ダッシュボードとは別のオーバーレイ（`sasaken.frameeyeosc-panel.eyecam-fill`）を出す。ヘッドセットに固定（`SetOverlayTransformTrackedDeviceRelative`、HMD）で正面 0.9 m、幅 4 m の正方形（左右・上下とも約 131°）、真っ白か真っ黒で、真ん中に指示を薄く出す（1024 px 四方、ステップか言語が変わったときだけ描き直す）
  - **フェード**: 出るときは `SetOverlayAlpha` を 0 から 1 へ 0.7 秒かけて上げる（0 にしてから `ShowOverlay`）。録画が続いたままステップが変わったときは 0.5 秒かけて 0 へ下げてから隠す。明るい↔暗いが続くときも白から黒へ一気に変えず、今の光を 0.5 秒で消してから次の光を 0.7 秒で出す。アルファは前の周からの経過時間で動かす（`eyecam::stepLight`。1 回に進める時間は 0.1 秒まで、なのでループが詰まっても一気に明るくならない）。フェードの間はループを表示のフレームごと（約 90 Hz）に回す
  - **すぐ隠す**: 状態ファイルが 1 秒より古い・タブが消えた・録画していない（止まった・失敗した・idle に戻った）・［中止］を送った（返事を待たずに）のどれかなら、フェードせずにその周で隠す（`eyecam::hideLightAtOnce`）。パネルの終了時（どの終わり方でも）は真っ先に隠し、終了処理で `DestroyOverlay` する。プロセスが落ちたときは SteamVR が消す

## ＋（プログラムを起動）から使う

Frame の Steam は XDG の `.desktop` を読んで「プログラムを起動」（＋）の一覧を作る。配布の tar.gz から入れるときは、トップの `./install.sh --with-panel` を使う（こちらは自動起動も有効にする）。ソースからビルドしたときは、Frame 上でこのフォルダから（sudo 不要。自動起動は有効にしない）:

```sh
sh contrib/install-panel.sh
```

入るもの:

- `~/.local/bin/frameeyeosc-panel`（systemd のサービスもこれを使う）
- `~/.local/share/applications/frameeyeosc-panel.desktop`（`Exec` を実行ファイルの絶対パスにしたもの）
- `~/.local/share/icons/hicolor/{48x48,128x128,256x256}/apps/frameeyeosc-panel.png`
- `~/.local/share/frameeyeosc/CHANGELOG.md`・`CHANGELOG.ja.md`（リポジトリにあれば。更新履歴が読む）
- `~/.config/systemd/user/frameeyeosc-panel.service`（置いて `daemon-reload` するだけ。enable はしない）

常駐を終わらせたいときは、ダッシュボードの「Eye」アイコンにホバーして「閉じる」、またはパネルの「アプリを終了」（どちらも終了コード 3）。

削除:

```sh
systemctl --user disable frameeyeosc-panel.service
rm -f ~/.local/bin/frameeyeosc-panel ~/.local/share/applications/frameeyeosc-panel.desktop ~/.config/systemd/user/frameeyeosc-panel.service
rm -f ~/.local/share/icons/hicolor/{48x48,128x128,256x256}/apps/frameeyeosc-panel.png
systemctl --user daemon-reload
```

## 自動起動（systemd ユーザーサービス）

パネルの「SteamVR と一緒に起動」で切り替える（先に `install-panel.sh` でユニットを入れておく。入っていなければグレーで「準備されていません」）。

- オン = `systemctl --user enable frameeyeosc-panel.service`、オフ = `disable`。`start` はしない（次に SteamVR が起動したときから効く）
- 今の状態は `systemctl --user is-enabled` で読む（パネルを開いた直後と、開いている間 5 秒おき）
- ユニットは `After`・`PartOf`・`WantedBy=steamvr.service`、`Restart=always`、`RestartPreventExitStatus=3`、`SuccessExitStatus=3`
- サービスから起動されたのに常駐がすでにいるとき（手で起動したものが残っているなど）は、SIGUSR1 を送らずに終了コード 3 で終わる（5 秒ごとにパネルが開き続けないように）。「サービスから起動された」は、`INVOCATION_ID` があり、かつ `/proc/self/cgroup` が `.../frameeyeosc-panel.service` のときだけとみなす（Frame の Steam 自体が `steam.service` で動いていて、＋から起動した子にも `INVOCATION_ID` が引き継がれるため）
- ログ: `journalctl --user -u frameeyeosc-panel -f`

## 色とアクセシビリティ

- 色は `src/theme.h` の 1 か所にまとめてある。`--contrast-report` が同じ定義から WCAG 2.x のコントラスト比を計算する
- 背景は GitHub ダーク系（`#0d1117` / `#161b22` / `#21262d` / `#30363d`）、アクセントは `#e27dfd`
- 文字は大きさによらず 4.5:1 以上、部品の枠・選択状態・図は 3:1 以上。押せないボタンの文字は WCAG では例外だが 3:1 を目安にする
- 「本体が動いていません」のバッジは、赤い文字だと 4.35:1 で足りないので、文字は白に近い色で ✕ だけ赤
- 目盛りの線は、色の棒の上でも暗い地の上でも見えるよう、明るい線の両側に暗い縁を付けている
- 2026-09-27 の結果: 35 組すべて合格。いちばん低いのは「コマンドで固定中の選択肢の枠」`#6e7681` / `#21262d` の 3.31:1

## 守っていること

- eyecam-rec の `status.json`（64 KB まで）と `ctl.sock` の返事の文字列は、描く前に正しい UTF-8 にする（壊れたバイトは U+FFFD。切るときは文字の境目で）。eyecam-rec に送るのは決まったコマンドだけ（`start`、`stop`、`calib wear` / `calib user`、`set widen_sensitivity 0.00`〜`1.00`、`live on` / `live off`）
- 書くのは設定ファイル（と、壊れていたときの `config.json.broken`）だけ。ほかに作るのは、視線の点がオンの間の `gaze-dots.sock`（状態ファイルのフォルダ、閉じるときに消す）と、起動時に書く目合わせの音 `sounds/*.wav`（状態ファイルのフォルダ、7 つ）だけ。アイトラッキングの共有メモリ・カメラ・GPIO・sysfs・`/persist` には触らない。sudo を使わない
- 外部コマンドは `systemctl --user` と `/bin/sh ~/.local/share/frameeyeosc/frame-update.sh`、目を合わせるときの音の `pw-play`（無ければ `paplay`、`aplay`。どれも無ければ鳴らさない。`/usr/bin`・`/bin` を先に探し、PATH の空や相対のフォルダは使わない）、目のカメラの準備の 3 つだけ:
  - `konsole -e bash -c '<script>'`（準備の ①・② のボタンを押したときだけ）。`<script>` はコードの中の定数（`setup_tools::kInstallCommand` = `sudo $HOME/.local/lib/eyecam/install_grab.sh`、`kPasswdCommand` = `passwd`）と、パネルの言語の 2 行の案内（これも定数）だけから組み立てる。状態ファイルや設定から来た文字列は入らない。Konsole は入力済みで開くだけで、Enter とパスワードは本人（パネルは sudo も passwd も実行しない）。`systemd-run --user --collect` で別の一時ユニットとして起動し（パネルが再起動しても閉じない。DISPLAY などの変数だけ `-E` で渡す）、無ければ直接。どちらも stdin / stdout / stderr を捨て、3 以上の fd は渡さず、待たない
  - `steamos-passwd --has-password`（読むだけ。パネルがダッシュボードに出ていて準備が ①・② のあいだだけ。答えが同じあいだは 4 → 30 秒と間をあける。5 秒で答えなければ SIGKILL）
  - `flatpak run org.chromium.Chromium <動画の URL>`（準備の動画のボタン。URL がまだ無いので、今は出さず、起動もしない）音は `posix_spawn` で出力を捨てて起動し、待たずにループで片付ける（同時に 2 つまで）。音量や PipeWire / WirePlumber の設定には触らない。どちらも固定の引数で呼び、コマンドの文字列を組み立ててシェルに渡すことはしない。`systemctl` は 2 秒（enable / disable は 5 秒）、更新の確認は 90 秒で終わらなければ SIGKILL、どの場合も `waitpid` で片付ける
- 更新のスクリプトが書くのは `~/.cache/frameeyeosc/` だけ。新しい版を入れるのは［更新する］を押して確認したときだけ
- `systemctl` はワーカースレッドで実行する（ポインターへの応答 33 ms おきを止めない）。SIGTERM・SIGINT・SIGUSR1 はメインスレッドで受ける
- パネルを閉じている間は描かない。読むのは更新の状態ファイル（`~/.cache/frameeyeosc/update-state.json`、1 秒に 2 回ほど）と eyecam-rec の `status.json`（1 秒に 1 回。動いている間は 0.1 秒おき）だけで、実行するコマンドは更新の確認（起動時と 1 時間ごと）だけ。ほかはイベントを 0.25 秒おきに見るだけ。例外は自分で始めた目合わせの間だけ（上を参照）
- 目合わせの目印は、ダッシュボードが閉じている間だけ出す。ダッシュボードが開いたら、その場で隠して止める

負荷（2026-09-27 に Frame で実測、`/proc/<pid>/stat` と `/proc/<pid>/io`）:

| 状態 | CPU | 読み込みの回数 |
|---|---|---|
| パネルを閉じている | 10 秒で 1 tick（10 ms） | 0 |
| パネルが開いていて、値が 1 秒に 10 回変わる | 10 秒で 45 tick（1 コアの約 4.5%） | 1 秒に約 24 回 |

## 画像の送り方と終了処理

- パネルとサムネイルの画像は Vulkan の `VkImage` を `IVROverlay::SetOverlayTexture` で渡す（`SetOverlayRaw` は差し替えのたびに画像が無い瞬間ができてちらつくので使わない）。画像は 2 枚を交互に使う
- 見えているときだけ、変化があったときだけ描く。つないだ直後に 1 枚入れておき、初めて選ばれたときに画像が無い瞬間を作らない
- 別のプロセスから `GetOverlayImageData` を呼ぶと、Vulkan のテクスチャが入ったオーバーレイでは呼んだ側が落ちることがあるので、`--probe` では `GetOverlayTextureSize` だけを使う

終了処理の順番（SIGTERM / SIGINT・`VREvent_Quit`・vrserver の消滅・「閉じる」・「アプリを終了」のどれでも同じ。各手順の結果はログに `[VR] shutdown: ...` と出る）:

1. `ClearOverlayTexture`（パネル・サムネイル）
2. `DestroyOverlay`（パネル。サムネイルは一緒に消える）
3. 400 ms 待つ（コンポジタに外したテクスチャを手放してもらう）
4. `VR_Shutdown`
5. Vulkan の画像・デバイス・インスタンスを壊す（OpenVR の決まりで `VR_Shutdown` の後）
6. 自動起動のワーカースレッドを止める

## ファイル

| ファイル | 役割 |
|---|---|
| `src/main.cpp` | コマンドライン、常駐のループ、ボタンの処理（設定ファイルへの書き込み）、二重起動、確認用オプション |
| `src/panel.*` | パネルの描画とボタンの当たり判定（描くたびに配置を作り直す）、サムネイル（目の絵） |
| `src/model.*` | 表示に使う値の組み立て（コマンドで固定中なら `effective`）、なめらかさの 3 段階、おすすめ設定、目盛りの順番 |
| `src/config.*` | `config.json` の項目の表（型・既定値・範囲・刻み）、読み書き（一時ファイル → fsync → rename） |
| `src/status.*` | `status.json` の読み込みと、本体が動いているかの判断 |
| `src/eyecam.*` | 目の撮影タブ（開発用）: eyecam-rec の `status.json` の読み込み、タブと全面の光を出す条件と光のフェード、光の注意（始める前の確認と送るコマンド）、指示の文、`ctl.sock` への送信 |
| `src/autostart.*` | `systemctl --user` で自動起動を読む・切り替えるワーカースレッド |
| `src/command.*` | fork＋execvp・パイプ・タイムアウト・waitpid |
| `src/theme.*` | 色の定義と WCAG のコントラスト比（`--contrast-report`） |
| `../vendor/frame-updater/` | 更新の確認・更新の共通部品（frame-updater の `sync.sh` で写したもの）。CMake が `cpp/update_check.cpp` を一緒にビルドする |
| `src/i18n.*` | 画面の文言（日本語・英語）。ログは英語 |
| `src/vr_overlay.*` | OpenVR の接続、ダッシュボードのオーバーレイ、イベント、終了処理、`--probe` |
| `src/vk_texture.*`・`src/draw.*`・`src/json.*` | Vulkan の画像、描画の部品、JSON の読み書き |
| `contrib/` | `.desktop`・`.service`・アイコン・`install-panel.sh` |
| `third_party/openvr/` | `openvr.h`（OpenVR SDK 2.15.6）と、そのライセンス（BSD-3-Clause） |
