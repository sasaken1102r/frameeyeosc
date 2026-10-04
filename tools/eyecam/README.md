# eyecam — Steam Frame の目のカメラを録画する

Valve のアイトラッカーを動かしたまま、IR の目のカメラ映像（左右 400×400、8bit、90 fps）と、
同じ時刻の Valve の推定値（開き具合・視線など、`/dev/shm/eye-server.mmap`）をいっしょに記録する。
まぶた・見開きモデルの学習データ用。常駐モードでは、目の映像からその場で目の開き・見開き・細め・瞳孔を計算して、
frameeyeosc 向けの共有メモリに書く（「ライブ処理」の節）。

- `eyecam-rec` … 録画する本体。ユーザー（steamos）で動かす。root 不要
- `eyecam-grab` … sudo で動かす小さな補助。共有バッファの「ファイル記述子」を rec に渡したら即終了する

使い方は 2 通り:

- **1 回録って終わる**（ターミナルで操作）: `eyecam-rec --cues protocol_widen.txt` → `sudo eyecam-grab`
- **常駐して VR パネルから録る**: `eyecam-rec --serve` を起動して、SteamVR を起動するたびに 1 回 `sudo eyecam-grab`。
  あとはパネルが `ctl.sock` に `start` を送るたびに録画する（仕様は下の「パネル向け仕様」）。その間ずっと、ライブ処理の結果が
  `/run/user/1000/eyecam/live` に出る

## 手順（1 回録って終わる）

### 0. ビルド（済んでいれば不要）

```sh
cd ~/eyecam-src
~/.cargo/bin/cargo build --release
```

録画中の eyecam-rec があるときはビルドしないこと（`pgrep -a eyecam-rec` で確認）。

### 1. 前準備

- SteamVR が動いていて、アイトラッキングが有効になっていること
- **frameeyeosc が動いていること**（Valve の推定値はクライアントが要求している間しか出ない。rec は読むだけで要求しない）

### 2. 録画を待ち受ける（ターミナル 1）

```sh
~/eyecam-src/target/release/eyecam-rec --cues ~/eyecam-src/protocol_widen.txt
```

待ち受けを始めると、次に打つコマンドを表示する。

### 3. バッファを渡す（ターミナル 2、ここだけ sudo）

```sh
sudo /home/steamos/eyecam-src/target/release/eyecam-grab
```

`passed 2 buffer(s) to eyecam-rec ... done` と出て終われば OK。

### 4. ヘッドセットをかぶる

装着を検知（近接センサーが `--prox-min` を **1 秒続けて** 超える）→ 目の映像が流れているのを確認（約 2 秒）→ 録画開始。
`--cues` を付けていれば、開始と同時にビープで合図が始まる（下の表）。
5 秒ごとに左右の fps、Valve サンプルの Hz、書き込み量、空き容量を表示する。
途中で外すと「止まった」と判断して再探索し、かぶり直せば同じセッションに続けて記録する（録画時間は止まらない）。
Ctrl-C で途中終了しても、そこまでのファイルはちゃんと閉じる。

注意: 目のカメラは、ちゃんとかぶっていなくても（レンズの中が映るだけでも）流れることがある。
ロックの条件として「装着中」を見ているのは近接センサーだけなので、録画が始まるのは実際にかぶってからにしてね。

### 5. PC に持ってきて見る

```sh
scp -r steamos@<FrameのIP>:eyecam/rec_2026-10-03_15-00-00 .
pip install numpy pillow
python convert.py rec_2026-10-03_15-00-00          # 30 フレームごとに PNG（png_L/, png_R/）
python convert.py rec_2026-10-03_15-00-00 --mp4    # eye_L.mp4 / eye_R.mp4（ffmpeg が必要）
python convert.py rec_2026-10-03_15-00-00 --dump   # lock_dump.png（枠合わせの確認用）
```

どっちが左目かはバッファに書いてないので、**メモリ上の並び**で決めている: 2 つのカメラのスロットは別々に確保されていて、
アドレスが小さい方のグループ（スロット 0–3）が L、+64 バイト遅れて始まる方（スロット 4–7）が R
（これまでの 6 セッションすべてで、look_up での瞳の動きと虹彩の大きさから確かめた）。決めた理由は meta.txt の
`camera_assignment` に残る。画の見た目でグループ分けした結果が並びと食い違ったら、そこに `WARNING` が付く（自動では入れ替えない）。
万一逆だったら `--swap` を付ける。
raw は無加工で、右目は上下逆さまのまま保存している（convert.py は見る用にだけ上下反転する。180° 回転ではない）。

## 常駐モード（`--serve`）

```sh
# 起動（ターミナルを閉じても動き続けるように）
systemd-run --user --unit eyecam ~/eyecam-src/target/release/eyecam-rec --serve
# または
nohup ~/eyecam-src/target/release/eyecam-rec --serve > /tmp/eyecam-rec.log 2>&1 &
```

1. 起動直後は `waiting_fds`。SteamVR が動いている状態で `sudo /home/steamos/eyecam-src/target/release/eyecam-grab` を 1 回
2. バッファを受け取ると `idle`。以後はパネルから `start` / `stop`
3. アイトラッカーが終了・再起動したり、かぶっているのにバッファが 2 分間まったく更新されなかったりしたら、
   バッファを手放して `waiting_fds` に戻る（grab 用ソケットも作り直す）。道具が入っていれば自動で取り直す。入っていなければもう一度 `sudo eyecam-grab` してね
4. 止めるときは `systemctl --user stop eyecam`（または `kill`）。録画中なら、そのセッションをきちんと閉じてから終わる。
   終了すると status.json に `state: "stopped"` を書き、`ctl.sock` は消える

録画のたびに、プロトコルは `~/eyecam-src/protocol_<名前>.txt` から読む（`start` の引数が名前、省略時は `widen`）。
ロックが生きていれば（スロットが更新され続けていれば）探し直さずにすぐ録画を始め、そうでなければかぶるのを待って探し直す。
映像が `--wait-lock`（既定 300 秒）以内に見つからなければ `error` になる。

## パネル向け仕様（status.json / ctl.sock）

場所は既定で `/run/user/1000/eyecam/`（ディレクトリは 0700、`--serve` / `--fake` とも `--run-dir DIR` で変更可）。
uid 1000（steamos）前提で `/run/user/1000` は固定（`XDG_RUNTIME_DIR` は見ない）。eyecam-grab からバッファを受け取るソケット
`/run/user/1000/eyecam.sock` はこのフォルダの外にあり、`--run-dir` を変えても動かない。

### タブを出す条件

**run dir がある**、**status.json の `state` が `stopped` でない**、**`updated_unix` が 5 秒以内**、の 3 つがそろったとき。

- status.json は **どの状態でも**（`waiting_fds` や `idle` でも）約 10 Hz で書き直す。録画や探索の処理とは別スレッドなので、
  処理中でも止まらない（1 秒以内の鮮度を前提にしてよい）
- きちんと終了したとき（SIGTERM / Ctrl-C）は `state: "stopped"` を書いて終わる（ディレクトリと status.json は残り、ctl.sock は消える）
- 落ちたとき（kill -9 など）は `updated_unix` が古くなるので、5 秒でタブが消える

### status.json

一時ファイルに書いてから rename で置き換える（読みかけが壊れることはない）。1 行の JSON。

```json
{"version":1,"state":"recording","message":"録画中","message_en":"Recording","has_buffers":true,"locked":true,"fps_l":90.000,"fps_r":90.000,
 "step_index":2,"step_count":17,"step_label":"normal","step_remaining_s":3.412,"elapsed_s":6.588,"total_s":84.500,
 "session_dir":"/home/steamos/eyecam/rec_2026-10-04_10-00-00","protocol":"widen","prox":31.000,
 "last_session_aborted":false,"calib_state":6,"recalib_suggested":false,"baseline":"ready","warmup_remaining_s":0.000,
 "calib_saved":true,"widen_sensitivity":0.500,"dev":false,"live":true,"live_ms":1.05,
 "pid":1234,"updated_unix":1791100000.123}
```

| フィールド | 型 | 中身 |
|---|---|---|
| `version` | int | この形式の版。今は 1 |
| `state` | string | `waiting_fds`（grab 待ち）/ `idle`（待機中。バッファを受け取ったらすぐこれになる。HMD をかぶっていない間は `message` が `HMD をかぶってね（目の映像を待ってるよ）`）/ `searching`（`start` を受けて、かぶるのと映像を待っている）/ `recording` / `calibrating`（`calib wear` / `calib user` を受けてから終わるまで。映像待ちの間も `calibrating`）/ `error`（直前の録画か校正が失敗。次の `start` / `calib` まで残る）/ `stopped`（eyecam-rec が終了した） |
| `auto_grab` | string | 自動 grab の状態（「sudo なしで使う」の表）。`--serve` 以外は `""` |
| `grab_outdated` | bool | eyecam-rec の隣の eyecam-grab が、入っている eyecam-grab（`/home/.eyecam/eyecam-grab`）より新しい版（更新で新しい eyecam-grab が届いた）。true なら `install_grab.sh` をもう一度 sudo で実行してもらう。どちらかが無いときは false。版はバイナリに埋め込んだ印 `EYECAM_GRAB_VERSION=<n>;` で比べる（同じソースでもビルドごとにバイトは変わるため。印の無い古いビルドは 1）。`src/bin/eyecam-grab.rs` を変えたら、その番号を上げる（上げ忘れはテストが止める） |

> 安全のための下限: `src/autograb.rs` の `MIN_SAFE_GRAB_VERSION` より古い eyecam-grab は、入っていても自動では起動しない（`auto_grab` が `too_old`、`grab_outdated` も true）。eyecam-grab に安全上の問題が見つかったときだけこの下限を上げる。ふつうの変更は `EYECAM_GRAB_VERSION` を上げるだけ（入れ直しのお知らせが出るが、古い道具のまま動き続ける）。

| `has_buffers` | bool | バッファを持っている（`sudo eyecam-grab` が成功した）。映像が流れているかどうかとは別。`waiting_fds` のときだけ false |
| `message_en` | string | `message` と同じ内容の英語（パネルを英語で表示するとき用）。例: `Put the headset on (waiting for the eye cameras)`、`Recording`、`Saved: rec_…`、`Calibrated`、`Calibrated (couldn't measure widening, using the usual width)`。訳は `src/message_en.rs` にまとめてあり、訳のない文が来たら日本語のまま入る |
| `message` | string | そのまま表示できる日本語の短文。例: `パネルの「目のカメラ」タブで、目のカメラの道具を入れてね`、`HMD をかぶってね（目の映像を待ってるよ）`、`録画中`、`保存した: rec_…`、`保存した（途中で止めた）: rec_…`、`中止した`。`idle` でかぶっていてライブ処理がオンのときは、`見開きの幅がまだわからないので仮の値。一度だけ calib wear をしてね`（`calib_saved` が false）/ `見開きの基準を覚えているところ（目を開けて、ふつうに前を見ていてね）`（`warming`）/ `目の値を出しているよ` |
| `locked` | bool | 目の映像の位置をつかんでいて、今も流れている。**`recording` / `calibrating` 中に false** なら、映像が止まって（HMD を外したなど）探し直している。そのとき `message` は `HMD をかぶってね（目の映像を待ってるよ）`。時間・合図・`step_*` はそのまま進む。`idle` でもライブ処理がオンなら、かぶっている間は true |
| `fps_l`, `fps_r` | number | 録画中の左右のフレームレート（直近 1 秒）。録画中以外と、探し直し中は 0 |
| `step_index` | int | いまのプロトコルの段の番号。**0 から**数えて、段が変わるたびに必ず変わる（パネルは「段 `step_index+1` / `step_count`」と出せる）。全段が終わると `step_count`（このとき `step_label` は `end`）。録画中以外は -1 |
| `step_count` | int | プロトコルの段の数（`searching` / `recording` / `calibrating` のとき）。それ以外は 0 |
| `step_label` | string | `lead_in` / `normal` / `widen` / `close` / `squint` / `look_up` / `look_down` / `bright` / `dark` / `end`（自作プロトコルならそのラベル）。録画中以外は `""` |
| `step_remaining_s` | number | いまの段の残り秒数。`lead_in`（最初の 3 秒）の間はこれでカウントダウンを出せる |
| `elapsed_s`, `total_s` | number | 録画開始からの秒数 / 録画の長さ（プロトコルの長さ + 1.5 秒）。`total_s` は `searching` のときから入る |
| `session_dir` | string | 録画中（と、そのあとの `idle`）のセッションディレクトリ。無ければ `""` |
| `protocol` | string | 録画中（と、そのあと）のプロトコル名 |
| `prox` | number | 近接センサーの値（読めなければ -1）。だいたい 20 を超えるとかぶっている |
| `last_session_aborted` | bool | 直前の録画が最後まで行かなかった（stop など）。その meta.txt は `aborted=1` |
| `calib_state` | int | ビットの和: 1 このかぶりで `calib wear` した / 2 ユーザー校正済み（`calib user`）/ **4 自動の基準線ができた（校正しなくても `eye_wide` などが使える）**。かぶり直す（映像が 10 秒以上止まる）と 1 と 4 は落ちる。**4 は `setup_done` が true になるまで立たない**（最初の準備が済むまで frameeyeosc はカメラを使わない。基準線を覚える計算は裏で続けている） |
| `recalib_suggested` | bool | 今は常に false（基準線がかぶっている間ずっと追従するので、やり直しをすすめる必要がなくなった。互換のため残している） |
| `baseline` | string | 自動の基準線（ふだんの目の開き）: `warming`（覚えているところ。`eye_wide` は 0）/ `ready`。かぶり直すと `warming` に戻る |
| `warmup_remaining_s` | number | `warming` のとき、あと何秒ぶんの「使えるフレーム」（目を開けて、ふつうに前を見ている）が要るか。`ready` なら 0。かぶってから約 35 秒で使えるようになる |
| `calib_saved` | bool | calib.json に見開きの幅を測れた `calib wear` が 1 回以上ある（見開きの幅の履歴がある）。false の間は見開きの幅が仮の値 |
| `setup_done` | bool | 成功した `calib wear` が一度でもある（見開きが取れなかった部分成功も含む）= パネルの最初の準備が済んだ。calib.json の `setup_done` に保存され、ずっと残る（このフィールドがない古い calib.json は、見開きの履歴があれば true） |
| `last_calib_widen` | string | 直近の `calib wear` の見開き: `measured`（測れた）/ `default`（取れなかったので履歴の中央値の幅を使った）/ `""`（まだしていない、この起動で失敗した、または見開きを記録する前の calib.json）。calib.json の `wear.widen` に残るので、再起動しても変わらない |
| `widen_sensitivity` | number | 見開きの感度 0〜1（`set widen_sensitivity`。既定 0.5）。どの state でも入っている |
| `dev` | bool | 開発者モード（settings.json の `"dev": true`、既定 false）。true のときだけ校正のたびの目の映像（`eye_L.raw` など）も残す（下の「settings.json」）。録画（`start`）はこれに関係なく誰でも使える。settings.json を手で書き換えると、再起動しなくても次の書き直しで反映される |
| `live` | bool | ライブ処理がオン（バッファを持っていて `live on`）|
| `live_ms` | number | ライブ処理の 1 フレーム（片目）あたりの時間、直近 1 秒の平均（ms） |
| `pid` | int | デーモンのプロセス ID |
| `updated_unix` | number | 書いた時刻（UNIX 秒） |

各段の長さとラベルは `protocol_widen.txt` のとおり。合図のビープは eyecam-rec が鳴らす（パネルは鳴らさない）。

### ctl.sock

unix stream ソケット（0600）。接続して 1 行（`\n` 終わり）送ると、1 行返ってきて切れる。
接続してきた相手の uid が 1000 でなければ、何も返さずに切る。

**返事はすぐ返る**（ふだん数十 ms。映像を探している最中でも 100 ms 程度）。かぶるのやロックを待ってから返すことはない。
そして **返事が届いた時点で、status.json はもう新しい状態になっている**（`start` → `searching`、`stop` → `idle`）。

| コマンド | 返事 |
|---|---|
| `start` / `start <名前>` | `ok`（受け付けた。state は `searching` → `recording` → `idle` と進む）または `err <理由>`。名前を省くと `widen` |
| `stop` | `ok`（state はすぐ `idle` に戻る。録画中なら、そこまでのセッションを閉じて残し、meta.txt に `aborted=1`。`searching` 中なら取りやめ。校正中なら取りやめて何も保存しない）または `err <理由>` |
| `status` | status.json と同じ JSON 1 行（ふだんはファイルを読めば十分） |
| `calib wear` | `ok`（state は `calibrating` → `idle`。結果は `message` に「校正できた（かぶり）」か、`error` でやり直しの理由）または `err <理由>` |
| `calib user` | 同上。このかぶりの `calib wear` がまだなら `err 先に calib wear をしてね` |
| `live on` / `live off` | `ok`。ライブ処理（共有メモリ `live` の更新）のオン/オフ。既定はオン |
| `set widen_sensitivity <0〜1>` | `ok` または `err widen_sensitivity は 0 から 1 の数で`。見開きの感度（下の「見開きの感度」）。**どの state でもすぐ受け付けてすぐ返る**（スライダーを動かしながら送ってよい）。すぐにライブの値に効き、`~/.config/eyecam/settings.json` に保存される（新しいファイルに書いて rename）。次に起動したときもその値。保存できなければ `err 保存できなかった: …` |

`<名前>` は `~/eyecam-src/protocol_<名前>.txt` のこと。英数字・`_`・`-` だけで 1〜32 文字（それ以外は `err プロトコル名に使えない文字がある`）。

| state | `start` / `calib …` | `stop` | `live on/off` |
|---|---|---|---|
| `waiting_fds` | `err まだバッファを受け取ってない`（予約はしない。grab のあとで送り直す） | `err 録画していない` | `err まだバッファを受け取ってない` |
| `idle` / `error` | `ok`。ファイルが無ければ `err プロトコルが見つからない: protocol_<名前>.txt` | `err 録画していない` | `ok` |
| `searching` / `recording` | `err 録画中` | `ok` | `err 録画中` |
| `calibrating` | `err 校正中` | `ok` | `err 校正中` |

`set widen_sensitivity` は上の表に関係なく、どの state（`waiting_fds` も）でも `ok`。

そのほかの返事: `err 知らないコマンド`、`err 応答がない`（10 秒以内に処理されなかった。ふつうは起きない）。

### 中身なしで試す（`--fake`）

```sh
~/eyecam-src/target/release/eyecam-rec --fake --run-dir /tmp/eyecam-fake
```

fd も root もカメラも使わず、ファイルも書かずに、status.json と ctl.sock を本物と同じ形式で動かす。
5 秒 `waiting_fds` → `idle`。`start` で 2 秒 `searching` → 既定プロトコル（または `start <名前>` のプロトコル）の段を実時間で
`recording`（fps は 90 固定、`locked` は true、ビープなし）→ `idle`。`stop` も本物と同じ（すぐ `idle`、`last_session_aborted` が true）。
`calib wear` / `calib user` も本物と同じ段を実時間で `calibrating` して、終わると `calib_state` が立つ（wear 前の `calib user` は断る）。
`baseline` は `idle` になってから 30 秒 `warming`（`warmup_remaining_s` が減っていく）→ `ready`（`calib_state` に 4）。
`calib_saved` は fake の `calib wear` が終わると true（そのときは `baseline` もすぐ `ready`）。
`set widen_sensitivity` は status.json の `widen_sensitivity` に出るだけで、settings.json には書かない（`dev` だけは本物と同じく settings.json から読んで出す）。
共有メモリ `live` は作らない。
終了すると `state: "stopped"` を書く。`--run-dir` を省くと本物と同じ `/run/user/1000/eyecam/` を使う（本物の `--serve` と同時には動かせない）。

## ライブ処理（目の開き・見開き・細め・瞳孔）

`--serve` は、バッファを持っていてライブ処理がオン（`live on`、既定）なら、録画していないときも目の映像をつかみ続けて、
両目のフレームを毎回（90 Hz）処理し、結果を共有メモリ `/run/user/1000/eyecam/live` に書く。frameeyeosc はそれを読むだけ。
処理は解析エージェントの Python 試作（`analysis/feat2.py`・`mapping.py`・`xwear.py`）の Rust への移植で、
`src/vision.rs`（OpenCV 相当の画像処理）・`src/feat.rs`（特徴量）・`src/live.rs`（出力と校正）・`src/livesvc.rs`（スレッド）にある。

- 処理スレッドは大きいコア（`cpu_capacity` が最大の 3/4 以上：このヘッドセットでは cpu2〜7）に固定し、nice 10 で動く
- 1 フレーム（片目）あたり A720 で約 1.1 ms、X4 で約 0.8 ms。両目 90 Hz で約 0.2 コア（`live_ms` で見られる）
- 処理が追いつかないときは、溜めずにフレームを捨てる

### 共有メモリ `live`（frameeyeosc との約束）

- パス: `/run/user/1000/eyecam/live`（`--run-dir` の中）。ファイルのモードは 0644、ディレクトリは 0700（同じ uid だけ読める）
- 中身: 先頭 200 バイトに下の構造体（リトルエンディアン、`#[repr(C)]`、パディングは表のとおり）。ファイル自体は 4096 バイト
- 読み方: `O_RDONLY` で開いて `mmap(PROT_READ, MAP_SHARED)`。**書かないこと**
- 一貫した読み取り（seqlock）: `seq` を読む（奇数なら書いている途中なので読み直す）→ 構造体をコピー → もう一度 `seq` を読み、
  変わっていたら読み直す。C++ なら `std::atomic_ref<uint32_t>` で acquire 読み、コピーのあとに acquire フェンス
- 確かめること: `magic == 0x4D435945`（バイト列 `"EYCM"`）、`version == 1`、`struct_size >= 200`。版が違えば読まない
- 鮮度: 各目の `t_mono_ns` と `CLOCK_MONOTONIC` の差で判断する（例: 100 ms より古ければ使わない）。`valid == 0` の目は使わない。
  eyecam-rec が終わると `live = 0` と `valid = 0` を書く。落ちたときは書かれないので、鮮度で判断してね
- 書き手が入れ替わったか: `writer_pid` / `writer_start_ns`
- **ファイルは eyecam-rec が動いている間ずっと 4096 バイトのまま**で、縮めたり切り詰めたりはしない（mmap している側が SIGBUS に
  ならないように）。eyecam-rec が起動するときは、同じディレクトリに新しいファイルを作って中身を用意してから `rename` で置き換える
  ので、前の eyecam-rec のファイルを mmap したままの読み手も、古い inode をそのまま読み続けられる（中身はもう更新されない）。
  読み手は `writer_start_ns` が変わったか、鮮度が切れたら、開き直してね（`stat` の inode が変わっていれば新しいファイル）

| オフセット | 型 | 名前 | 中身 |
|---|---|---|---|
| 0 | u32 | `magic` | `0x4D435945`（"EYCM"） |
| 4 | u32 | `version` | 1。並びや意味を変えたら上げる |
| 8 | u32 | `struct_size` | 200 |
| 12 | u32 | `writer_pid` | eyecam-rec のプロセス ID |
| 16 | u64 | `writer_start_ns` | eyecam-rec が起動した時刻（`CLOCK_MONOTONIC` の ns） |
| 24 | u32 | `seq` | seqlock。書いている間は奇数 |
| 28 | u32 | `calib_state` | ビットの和: 1 このかぶりで `calib wear` した / 2 ユーザー校正済み / 4 自動の基準線ができた（status.json と同じ） |
| 32 | u64 | `t_mono_ns` | どちらかの目の最新フレームの撮影時刻（`CLOCK_MONOTONIC` ns） |
| 40 | u64 | `t_cam_raw_ns` | 同じフレームのカメラのタイムスタンプ（`CLOCK_MONOTONIC_RAW` ns） |
| 48 | u32 | `recalib_suggested` | 今は常に 0（互換のため残している） |
| 52 | u32 | `live` | 1 ならライブ処理中（バッファあり・`live on`） |
| 56 | eye[2] | `eyes` | 左（56）、右（128）。各 72 バイト、下の表 |

各目（`eyes[0]` = 左目、`eyes[1]` = 右目。解剖学的な左右。オフセットは目の先頭から）:

| オフセット | 型 | 名前 | 中身 |
|---|---|---|---|
| 0 | u64 | `t_mono_ns` | この目の最新フレームの撮影時刻（`CLOCK_MONOTONIC` ns。カメラの `MONOTONIC_RAW` から、毎秒測る差で換算） |
| 8 | u64 | `t_cam_raw_ns` | そのフレームのカメラのタイムスタンプ（`CLOCK_MONOTONIC_RAW` ns） |
| 16 | u64 | `frame_count` | この目で処理したフレーム数（eyecam-rec 起動から） |
| 24 | u32 | `valid` | 1 なら下の値はこのフレームのもの（瞳、閉じたまぶた、まぶたのどれかが見えた）。0 なら使わない |
| 28 | u32 | `closed` | 1 なら目を閉じている |
| 32 | f32 | `eye_lid` | VRCFT の目の開き: 0 閉じ、0.75 普段の開き（自動の基準線）、1 しっかり見開き |
| 36 | f32 | `eye_wide` | 見開き 0..1（既定の感度 0.5 で、基準線から見開きの幅の 32.5% までは 0、60% で 0.5、87.5% で 1。0.3 を超えるのは 0.5 超えが 100 ms 続いてから） |
| 40 | f32 | `eye_squint` | 細め 0..1（視線の上下による開きの変化を補正した開きから） |
| 44 | f32 | `pupil_ratio` | 瞳孔の直径 / 虹彩の半径。明るいところで約 0.6、暗いところで約 1.05 |
| 48 | f32 | `pupil_mm` | `pupil_ratio` × 5.9 mm（虹彩の直径を 11.8 mm と仮定） |
| 52 | f32 | `pupil_dilation` | その人の瞳孔の範囲の中での 0..1（測ってなければ 0.6〜1.05 で） |
| 56 | f32 | `confidence` | 0..1。瞳・まぶたがどれだけ見えたか × 基準の状態（基準線ができていて見開きの幅の履歴あり 1.0、履歴なしで仮の幅 0.7、基準線を覚えている途中 0.3） |
| 60 | f32 | `skin_up` | デバッグ用の生の特徴: 瞳の中心 − 上まぶたの皮膚の線（虹彩の半径単位） |
| 64 | f32 | `aperture` | デバッグ用: 下まぶた − 上まぶた（虹彩の半径単位） |
| 68 | u32 | （予約） | 0 |

値の性質:

- どの値も 1 フレームごとの特徴量の **直近 5 フレームの中央値**（外れ値よけ）から計算していて、それ以上のなめらかにする処理はしていない
  （EMA などは frameeyeosc 側で）。瞳孔は見えなかったフレームでは直前の値のまま
- `closed` の判定は校正なしで動く: 虹彩があるはずの箱が、目を開けているときの長期の明るさの 1.3 倍より明るい、かつ 3 フレーム続けて瞳が見えない
- 閉じている間と、開いてから 80 ms は `eye_wide` と `eye_squint` は 0
- **校正なしで動く（既定）**。普段の目の開き（上まぶたの皮膚の線の高さと、まぶたの間の開き。どちらもピクセル）は、かぶってから
  「使えるフレーム」（瞳が見えていて、閉じていなくて、Valve の視線の上下がその人のいつもの角度（直近 1 分の中央値）から ±15° 以内）
  30 秒ぶんの **最頻値**（中央値ではない。見開き・よそ見・まばたきは裾にしかならない）。かぶってから約 35 秒で `baseline` が `ready` に
  なり、それまで `eye_wide` は 0（`eye_squint` も 0、`eye_lid` は開き具合では下がらない）
- そのあとも基準線はゆっくり追従する（時定数 60 秒、`eye_wide` < 0.2 のフレームだけ、同じ視線の条件。開きは `eye_squint` < 0.2 も）。
  HMD が少しずれても基準がずれたままにならない（15 秒の校正は 1 分もたたずに古くなっていた）。速く追従する（20 秒）と、下を見たときに
  まぶたが下がるのを追いかけて逆に誤検出が増えた
- 見開きの幅と閉じた開き（虹彩の半径の単位）は、**これまでに成功した `calib wear` の中央値**（履歴）× いまの虹彩の半径。履歴がまだないときは
  既定値（見開き 0.19、開き 0.88）を使い、`calib_saved` が false になる。虹彩の半径はかぶってから 10 秒は履歴の中央値を使う
- 視線が 0.3 秒以内に 8° 以上下がったら、0.3 秒 `eye_wide` = 0（下を見ると上まぶたが遅れてついてくるので、その間だけ皮膚の線が
  瞳から離れて見える。残っていた誤検出の主な原因）
- `eye_wide` は基準線から見開きの幅のある割合までを不感帯にして、そこから直線で 0→1（割合は見開きの感度で決まる。既定の 0.5 では
  32.5% から上がり始めて、60% で 0.5、87.5% で 1）。さらに、0.5 を
  100 ms（9 フレーム）続けて超えるまでは 0.3 で頭打ちにし、一度出たら 0.3 を下回るまでそのまま出す（ヒステリシス）。上を見たときや
  皮膚の線の一瞬のゆれで見開きが出るのを防ぐため（以前は幅の 25% から 75% で 0→1、0.5 は幅の半分）
- 瞳もまぶたも見えないのに閉じてもいないとき（細めでまつげが瞳にかかったとき）は `eye_squint` = 1
- **`calib user` がまだのときは `eye_squint` は常に 0、`eye_lid` は目の開き具合では下がらない**（閉じたら 0、ふだん 0.75、見開きで
  1 まで上がるだけ）。視線の上下で開きが変わる分を補正できないうちは、開きだけで細めと判断しない
- 虹彩の箱がまぶたの皮膚くらい明るいときに見つかった「瞳」は、開いているときの瞳と暗さ・見えている割合・大きさ（0.45〜1.3 倍）が合わなければ
  捨てる（閉じたまぶたのしわやまつげの線を瞳と間違えて、閉眼が途切れるのを防ぐ）。明るい光でも箱はこのくらい明るくなるが、
  そのときの瞳は小さく（いつもの 0.55〜0.65 倍）なるだけで暗さも見え方も変わらない。しわは中が 24〜28 階調明るく、半分も見えず、
  大きさは 0.23〜0.42 倍。以前は大きさ ±30% で切っていて、bright の段の 26% が閉眼になっていた
- 瞳孔の大きさは、瞳が丸ごと見えているフレームだけで更新する: 64 本の光線の 75% 以上が縁に届き、形（b/a）がいつもの値から
  ±0.08 以内（まぶたがかかると楕円がつぶれる）、まばたきのあと 300 ms 以上たっている。それ以外のフレームは直前の値のまま
- 左右の瞳孔は一緒に動くので、同じ瞬間の左右の差から「いつもの左右差」（直近 60 秒の中央値。目ごとに虹彩の半径で割るので
  少しずれる。録画では L が 0.4〜1.2 mm 大きい）を引いて、1.5 mm を超えたら、自分の直近 10 秒からより外れている方の目を
  直前の値のまま・`confidence` 半分にする。serve のログに 30 秒ごと、食い違った回数・比べた回数・いつもの左右差が出る
- 校正セッション 5 本での食い違い（比べたペアのうち）: 前 0.0 / 0.2 / 0.8 / 3.6 / 2.4% → 後 0.0 / 0.9 / 0.2 / 0.0 / 0.0%
  （食い違いは、まばたきのあとや段の切り替わりなど、まぶたが瞳にかかる瞬間に集中していた）

### settings.json

`~/.config/eyecam/settings.json`。なければ既定値。知らないキーは無視し、範囲外・型ちがいの値は既定値になる。

```json
{
  "version": 1,
  "widen_sensitivity": 0.5,
  "dev": false
}
```

- `widen_sensitivity`: 見開きの感度 0〜1（パネルの `set widen_sensitivity` が書く。下の「見開きの感度」）。書くときは `dev` はそのまま残す
- `dev`: 開発者モード（既定 false）。**手で書き換える**（ctl のコマンドはない）。true のときだけ、校正（`calib wear` / `calib user`）でも
  録画と同じく目の映像（`eye_L.raw`, `eye_R.raw`, `headers.bin`, `lock_dump.bin`）を残す。status.json の `dev` に出る。
  起動中に書き換えても、status.json には次の書き直しで、校正には次の校正から効く

### 校正

| コマンド | 段（ビープも録画と同じ） | 長さ | わかること |
|---|---|---|---|
| `calib wear`（**しなくてもよい**。最初に一度だけはしてほしい） | lead_in 3 → close 2 → normal 5 → widen 3 → normal 2 → widen 3 | 18 秒 | 普段の開き（基準線の出発点にする。そのあとは追従する）・見開きの幅と閉じた開き（履歴に足す）・虹彩の半径 |
| `calib user`（一度だけ） | lead_in 3 → squint 5 → look_up 5 → look_down 5 | 18 秒 | 視線の上下と目の開きの関係（2 次式、測った範囲で頭打ち）・細めの深さ |

- 各段の最初の 0.8 秒（反応の遅れ）は使わない
- `calib user` は、同じかぶりで `calib wear` が済んでいないと断る。視線は Valve の値を使うので、frameeyeosc が動いている必要がある
- **`calib wear` で閉じる・普段は取れたのに見開きが取れなかったとき（見開きのフレームが足りない、幅が 0.05 未満）は、失敗にしない**。
  普段の開き・閉じた開きはこの校正の値を使い、見開きの幅は履歴の中央値（なければ既定の 0.19）。この回の幅は履歴に入れない。
  `message` は「校正できた（見開きは取れなかったので、いつもの幅を使うよ）」、`last_calib_widen` は `default`、`calib_state` の 1 は立つ
- やり直しになる場合（`state` が `error` になり、`message` に理由。例「左目: 下を見ても目の開きが変わっていない（もう一度、しっかり下を見てね）」）:
  普段の瞳が見えない・閉じが検出できない、下を見たときの開きが普段の 0.95 倍より大きい、下を見たら下まぶたが上に動いた、
  細めが浅い（閉じ〜普段の 85% 以上）
- 左右の変化の差（下を見たとき・細めたとき）が虹彩の半径の 0.2 倍以上なら **警告だけ**（やり直しにはしない）。
  `message` に「校正できた（ユーザー）。注意: 左右の差が大きい [下を見たとき 0.39、細め 0.08]」のように出て、calib.json の
  `user.warnings` に残る（問題のない録画でも 4 本中 2 本でこうなったため）
- 瞳孔の範囲は校正の段に入れていない。bright と dark の段がある録画（`start widen` など）を最後まで録ると、その間の瞳孔から
  （明るいときの 5 パーセンタイル〜暗いときの 95 パーセンタイル）保存する
- **校正のたびに（失敗しても、止めても）`~/eyecam/calib_YYYY-MM-DD_HH-MM-SS/` に保存する。ただし目の映像は残さない**:
  ふだんは小さなテキストだけ（`calib_result.json`, `calib_samples.csv`, `cues.csv`, `frames.csv`, `valve.csv`, `meta.txt`。合わせて 1 MB ほど）で、
  `eye_L.raw`, `eye_R.raw`, `headers.bin`, `lock_dump.bin` は最初から書かない（meta.txt に `images=none …`）。
  settings.json で `"dev": true` のときだけ、録画と同じ形式で映像も残す（約 0.5 GB / 回、meta.txt に `images=kept`）。
  中身は `calib_result.json`（`ok`・`message`・目ごとの中間値: 虹彩の半径、普段/見開きの skin_up と見開きの幅、普段/閉じの開き、
  瞳が見えたフレーム数、user なら細めの深さや下を見たときの開きの比、それぞれのしきい値・できた校正の値）と
  `calib_samples.csv`（当てはめに使った各フレーム: 目・時刻・段・瞳の有無・skin_up・開き・瞳孔・視線・下まぶた・R）。
  `dev` で残した校正は `eyecam-rec --replay ~/eyecam/calib_… --out x.csv` でそのまま流し直せる。serve のログにも目ごとの見開きの幅としきい値が 1 行ずつ出る
- 保存先: `~/.config/eyecam/calib.json`（user の校正、最後のかぶりの校正、`history`: 成功した `calib wear` ごとの目ごとの見開きの幅 `step`・
  閉じた開き `gap`（虹彩の半径の単位）・虹彩の半径 `r_px`。新しい 100 回分）。`history` がない calib.json は、起動したときに
  `~/eyecam/calib_*/calib_result.json`（成功した wear のもの）から作って保存する（なければ最後のかぶりの校正を 1 件目にする）。
  このヘッドセットの今の中央値は見開きの幅 L 0.22 / R 0.20。serve の起動ログに履歴の中央値が出る
- 映像が 10 秒以上止まったら（HMD を外したら）新しいかぶりとみなして、かぶりの校正と学習した値（レンズの端、虹彩の半径、開いた目の明るさ、
  基準線）を捨てて、もう一度覚える

### 試作（Python）との一致

`--replay --compat` で、試作の run2.py と同じ手順（レンズ端は 200 フレームおきの平均画像、探索用の虹彩半径は 25 フレームおきの中央値、
400 フレームごとに追跡をやり直す）で 5 セッション × 両目（約 7.6 万フレーム）を処理し、
`eyecam_features_5sess_2026-10-04_01-11-53.csv` と比べた結果:

| 特徴 | 差の中央値 | 1 px 以内（box は 1 階調以内） |
|---|---|---|
| 瞳が見つかったか | 違うのは 1 セッション 0〜15 フレーム | — |
| 瞳の中心・長径・短径 | 0（同じ値） | 99.99〜100% |
| 虹彩の半径 | 0 | 99.98〜100% |
| 上まぶたの皮膚の線 | 0.000〜0.001 px | 98.5〜99.8% |
| 上まぶた（縁） | 0.000〜0.002 px | 98.4〜99.7% |
| 下まぶた | 0.003〜0.009 px | 99.0〜99.9% |
| 虹彩の箱の明るさ | 0 | 98.9〜100% |

GaussianBlur とオープニングは OpenCV とビット単位で同じ（`vision::tests::matches_opencv_on_a_test_image`）。残りの差は、
瞳のまわりを消す楕円の塗り方（OpenCV の多角形近似とこちらの内外判定）の違いから来ている。

### 録画を通して確かめる（`--replay`）

```sh
# 試作と同じ手順の特徴量（一致の確認用）
eyecam-rec --replay ~/eyecam/rec_2026-10-03_23-50-19 --compat --out compat.csv
# ライブ処理をそのまま通す。参照セッションから user 校正を作り、別のセッションを最初の 4 段（15 秒）で wear 校正して評価
eyecam-rec --replay ~/eyecam/rec_A --fit-user calib_A.json --out full_A.csv
eyecam-rec --replay ~/eyecam/rec_B --user-calib calib_A.json --calib-block 4 --out test_A_B.csv
# 校正なし: 1 回目で基準線を覚えてから最初から流し直す（セッションの前に 30 秒かぶっていたことにする）
eyecam-rec --replay ~/eyecam/rec_B --user-calib calib_with_history.json --prewarm --out test_auto.csv
# 評価用のつまみ: --no-track（追従しない）、--no-gaze-drop（下を見たときの保持なし）、--wide-curve 0.25,0.5（見開きの曲線）、--widen-sensitivity 0.8（感度で曲線を決める）
python eval_live.py DIR   # DIR の test_r*_t*.csv を xwear.py と同じ指標で集計
```

L/R が逆に録られた古いセッション（`slot_camera=1,1,1,1,0,0,0,0`）は自動で入れ替えて読む（`--swap` で反転）。

### 精度（5 セッション・4 かぶり、ライブ処理をそのまま通した結果）

xwear.py と同じ設計: かぶりごとに参照セッション（1, 3, 4, 5）から user 校正を作り、別のかぶりのセッションを最初の 4 段（約 15 秒、
lead_in・close・normal・widen）で wear 校正して、残りの段（step >= 4）で評価。参照 × テスト × 目 = 30 通りの平均（カッコ内は最小〜最大）。
右の列は Python 試作の同じ条件（`block`・`gated`・`soft`、xsum_base.txt）。`eye_wide` は Python と同じ EMA 0.35 をかけた値。

| 指標 | Rust（ライブ処理） | Python 試作 |
|---|---|---|
| widen と normal の AUROC（skin_up） | 0.968（0.918〜0.997） | 0.971 |
| eye_wide > 0.5 の検出率（widen 中） | 0.888（0.585〜0.991） | 0.888 |
| eye_wide > 0.5 の誤検出率（normal・上下・明暗） | 0.024（0.010〜0.062） | 0.023 |
| 誤検出の回数（回/分） | 5.5 | 5.5 |
| 閉眼の正解率（close 中） | 0.891（0.556〜1.000） | 0.888 |
| 細めの正解率（squint 中、eye_squint > 0.5） | 0.731（0.011〜0.942） | 0.767 |
| 開いているときに閉眼と出た割合 | 0.001 | 0.001 |
| 下を見たとき細め扱いしない割合 | 0.835 | 0.793 |
| 3 クラス（閉・細・開）の平均正解率 | 0.872 | 0.883 |
| 瞳孔: 暗い vs 明るいの AUROC | 0.994（0.987〜1.000） | 0.993 |
| 瞳孔の直径（明るい / 暗い） | 3.69 / 6.00 mm | 3.69 / 6.00 mm |
| pupil_dilation（明るい / 暗い） | 0.10 / 0.80 | 0.09 / 0.79 |

上の表は最初の移植時（見開きの曲線が幅の半分で 0.5、校正した値を固定）の結果。

### 校正なし（既定）と見開きの曲線の比較

プロトコルは、テストと別のかぶりの wear 校正だけを履歴にして（見開きの幅はその中央値）、`--prewarm` で基準線を覚えてから流した
5 セッション × 両目（見開きの値は参照セッションに依らないので 15 通りでも同じ数字）。自由使用は `rec_2026-10-04_03-45-15`
（合図なしで普段どおり 3 分、意図した見開きなし）。履歴は直前までの Frame の wear 校正 6 回。自動の基準線は覚えたあと（約 35 秒以降）だけ数えた。
検出率・誤検出率はフレーム単位で `eye_wide` > 0.5（エンジンの出力そのまま、EMA なし）。

| 条件 | widen 検出率 | 誤検出率（中立の段） | 誤検出（回/分） | 自由使用 L / R（100 ms 以上の回/分） |
|---|---|---|---|---|
| 以前（曲線 50%、校正を固定） | 0.891 | 1.79% | 5.6 | 0.66 / 1.32 |
| 不感帯 40% + ヒステリシス、校正を固定 | 0.793 | 0.66% | 2.8 | 0.00 / 0.99 |
| 校正で出発 + 追従 | 0.656 | 0.22% | 0.7 | 0.00 / 1.32 |
| 校正なし: 自動の基準線 + 追従 + 下を見たときの保持（曲線 40%〜100%） | 0.632 | 0.21% | 0.7 | 0.00 / 0.41 |
| 同、追従なし | 0.642 | 0.19% | 0.7 | 0.00 / 0.00 |
| 同、下を見たときの保持なし | 0.676 | 0.70% | 2.4 | 1.25 / 0.41 |
| 同、曲線を幅の半分で 0.5 に | 0.791 | 0.72% | 2.0 | 2.08 / 0.41 |
| 同、曲線を幅の 60% で 0.5（30%〜90%） | 0.745 | 0.28% | 1.1 | 0.83 / 0.41 |
| 同、60% で 0.5（35%〜85%） | 0.743 | 0.28% | 1.1 | 0.83 / 0.41 |

- 曲線を書いていない行は 40%〜100%（70% で 0.5）で測った
- 閉眼の正解率 0.994、bright/dark 中に閉眼と出た割合 0.003、瞳孔 AUROC 0.992 はどの条件でも同じ
- この長さ（1〜3 分）では追従のあり/なしの差はほとんどない（追従は、かぶっている間に HMD がずれていくのに備えるもの）

### 見開きの感度（`widen_sensitivity`）

パネルから `set widen_sensitivity <0〜1>` で変えられる。見開きの幅を step として、`eye_wide` が上がり始める点（不感帯の端）と 0.5 に
なる点を、0 と 1 のあいだで直線に動かす（1 になるのは「上がり始め + 2 ×（0.5 の点 − 上がり始め）」）。ヒステリシス（0.5 超えが
100 ms 続くまで 0.3 で頭打ち）と下を見たときの保持は感度に関係なくそのまま。

| 感度 | 上がり始め | 0.5 | 1 | widen 検出率 | 誤検出率 | 誤検出（回/分） | 自由使用 L / R（回/分） |
|---|---|---|---|---|---|---|---|
| 0（いちばん厳しい） | 40% | 70% | 100% | 0.632 | 0.21% | 0.7 | 0.00 / 0.41 |
| **0.5（既定）** | 32.5% | 60% | 87.5% | 約 0.745 | 約 0.28% | 約 1.1 | 約 0.83 / 0.41 |
| 1（いちばん敏感） | 25% | 50% | 75% | 0.791 | 0.72% | 2.0 | 2.08 / 0.41 |

（校正なし・自動の基準線 + 追従 + 下を見たときの保持で、上の比較と同じ評価。0.5 の行は、0.5 の点が同じ 60% で上がり始めが 30% と 35% の
2 つの曲線を測った値（0.745 / 0.743 で、ほかもほぼ同じ）で、その間にある。）感度を上げるほど見開きを取りこぼさなくなるが、
普段の何気ない目の動きでも見開きが出やすくなる。

途中で直したこと: 最初は虹彩の半径を校正後も推定し続けていて、セッション 1・2 の右目で校正中（67.9 px）とその後（63.4 px）が 7% ずれ、
見開きの誤検出率が 0.11 になっていた。今は校正した半径をそのかぶりの間固定している（試作の block 校正と同じ）。

`calib user` の検査は、参照 4 セッション中 3 つで引っかかった（オフラインの評価では警告だけにして続行）:
左右の差（下を見たとき 0.39 と 0.33。これを受けて左右の差は警告だけにした）、右目で下を見ても開きが変わらない
（1.54 / 普段 1.47。本当に下まぶたの検出が失敗している例で、今もやり直しになる）。

## sudo なしで使う（自動 grab と systemd）

毎回の `sudo eyecam-grab` をなくすための、一度だけの設定。

### 1. eyecam-grab をインストールする（一度だけ sudo）

```sh
cd ~/eyecam-src
sudo ./install_grab.sh                        # 隣の eyecam-grab（なければ ./target/release/eyecam-grab）を入れる
sudo ~/.local/lib/eyecam/install_grab.sh      # install_user.sh で入れたもの（隣の eyecam-grab が入る）
sudo ./install_grab.sh target-next/release/eyecam-grab   # ビルドしたものを指定するとき
```

`/home/.eyecam/eyecam-grab`（ファイルもディレクトリも root 所有・0755）にコピーして、`setcap cap_sys_ptrace=ep` を付け、
`getcap` で確かめて sha256 を表示する。

- **なぜ /home か**: SteamOS の `/var` は A/B スロットごとの 256 MB のパーティション（var-A / var-B）で、OS のアップデートで
  スロットが切り替わると中身が見えなくなりうる。アップデートしても残るのは `/home` パーティションだけなので、その中の、ユーザーが
  書き込めない root のディレクトリに置く（`/home` は nosuid ではないので、ファイル能力が効く）
- 取り除く: `sudo ./install_grab.sh --uninstall`
- 更新: eyecam-grab を作り直したら、同じコマンドをもう一度（中身を確かめてから）。書き換わったファイルは能力が消えるので、
  ユーザーが勝手に差し替えたり書き換えたりしたものが能力を持つことはない

### 2. eyecam-rec --serve が自分で grab する

`--serve` は waiting_fds のとき、インストール済みのコピーを確かめて（root 所有・root 以外は書けない・root のディレクトリ・
シンボリックリンクではない・`security.capability` に CAP_SYS_PTRACE の permitted と effective がある）、アイトラッカーが動いていれば
5 秒おきに起動する。grab から受け取るのは、root から（手動の sudo）か、自分が起動したその子プロセス（SO_PEERCRED の pid が一致）
からの接続だけ。status.json の `auto_grab`:

| 値 | 意味 |
|---|---|
| `missing` | インストールされていない（`message` は「パネルの「目のカメラ」タブで、目のカメラの道具を入れてね」。手で `sudo eyecam-grab` してもいい） |
| `no_cap` | 置いてあるが能力が付いていない |
| `unsafe: …` | root 所有でない・書き込める人がいる、など。使わない |
| `waiting_tracker` | アイトラッカー（SteamVR のアイトラッキング）が始まるのを待っている |
| `trying` | 起動した |
| `ok` | 自動の grab からバッファを受け取った |
| `failed: …` | 失敗（理由つき）。5 秒後にまたやる |

アイトラッカーが再起動したときも、自分で取り直す。

### 3. systemd で常駐させる（sudo なし）

```sh
cd ~/eyecam-src
./install_user.sh                 # ~/.local/lib/eyecam に eyecam-rec・eyecam-grab・install_grab.sh・protocol_*.txt を入れて、eyecam.service を有効にして起動
./install_user.sh --bin target-next/release   # ビルド先を指定
./install_user.sh --uninstall     # 止めて取り除く
journalctl --user -u eyecam -f    # ログ
```

`eyecam.service`（`systemctl --user`）は `~/.local/lib/eyecam/eyecam-rec --serve` を `Restart=on-failure` で動かす。
更新は同じ `./install_user.sh`（動いている eyecam-rec は名前の付け替えで置き換えてから再起動する）。プロトコルは
`eyecam-rec` と同じディレクトリの `protocol_<名前>.txt` を先に探し、無ければ `~/eyecam-src` を見る。

### セキュリティの考え方

- root で常駐するデーモンも、sudoers の書き換えも使わない。特権は「eyecam-grab という 1 ファイルが持つ CAP_SYS_PTRACE だけ」
- CAP_SYS_PTRACE があれば、`kernel.yama.ptrace_scope=1` の下でも、ほかのプロセスの fd を `pidfd_getfd` で写せる。eyecam-grab が
  それを使うのは、身元を確かめた Valve の eyetracking（exe のパスと uid 1000）の udmabuf だけ。渡す相手は
  「`/run/user/1000/eyecam.sock` で待っている uid 1000 のプロセス」で、それが eyecam-rec かどうかまでは確かめない
  （同じユーザーのほかのプログラムがそのソケットを作れば、そちらが受け取れる）。アタッチ・停止・メモリの読み書きはしない
- 能力つきのプログラムは secure-exec で動く（LD_PRELOAD などの環境変数は効かない）。eyecam-grab は環境変数も自分の uid も見ない
- ファイルは root 所有でユーザーは書き換えられず、書き換えれば能力は消える。どのユーザーのプロセスでも実行はできるが、
  できることは「uid 1000 のソケットへ eyetracking のバッファ（目のカメラの映像が通る）を渡す」ことだけ。つまり steamos
  ユーザーとして動くプログラムなら、目の映像を受け取れる
- 入れ直し・取り除きでは、ファイルを置き換える前に `setcap -r` で能力を外す（古いコピーが別の名前で能力を持ったまま残らないように）
- install_grab.sh が入れるのは `~/.local/lib/eyecam/eyecam-grab`（steamos が書き換えられる場所）。steamos ユーザーとして動く
  悪いプログラムがあれば、sudo の前にこのファイルを差し替えられる（install_grab.sh 自体も同じ場所にあるので、隣にハッシュの
  一覧を置いても守れない）。気になるときは、install_grab.sh が最初に出す sha256 を、リリースに書かれた値と比べてから
  パスワードを打つ

## ビープの合図（`--cues`、常駐モードの録画・校正も同じ）

| 合図 | 意味 |
|---|---|
| 短 1 回（高い音） | lead_in（準備。これから始まる） |
| 短 1 回 | normal（ふつうに開ける） |
| 短 2 回 | widen（見開く） |
| 短 3 回 | close（閉じる） |
| 長 1 回（中くらいの高さ） | squint（細める） |
| 長 1 回（高い音） | look_up（上を見る） |
| 長 1 回（低い音） | look_down（下を見る） |
| 短 2 回（とても高い音） | bright（明るいものを見る） |
| 短 2 回（低い音） | dark（暗いものを見る） |
| 長 2 回（低い音） | 終わり |

`protocol_widen.txt`（83 秒）: lead_in 3 秒 → (close 2 秒, normal 5 秒, widen 5 秒) × 3 → squint 5 秒 → look_up 5 秒 → look_down 5 秒
→ normal 5 秒 → 瞳孔チェック: bright 8 秒 → dark 8 秒 → bright 8 秒。

瞳孔チェックのあいだは目を開けたまま、bright の合図で明るい画面（白っぽい SteamVR ホームやダッシュボードなど）を、
dark の合図で暗い画面（暗いシーン、ダッシュボードを閉じた真っ暗な状態など）を見る。目は閉じないでね（瞳孔の大きさの変化を見るため）。
自分で作るときは 1 行に `秒数 ラベル`（`#` 以降はコメント）。上の表にないラベルは長 1 回（中）。
`--seconds` を省くと、録画時間はプロトコルの長さ + 1.5 秒になる。
音は `pw-play`（なければ `paplay` / `aplay`）で鳴らす。音量は `--volume 0..1`（既定 0.25）、鳴らさず記録だけなら `--no-beep`。

## できるファイル（`~/eyecam/rec_YYYY-MM-DD_HH-MM-SS/`）

| ファイル | 中身 |
|---|---|
| `eye_L.raw`, `eye_R.raw` | 400×400 の 8bit グレーを記録順にベタ書き（1 フレーム 160000 バイト、ヘッダなし、無反転） |
| `frames.csv` | 1 フレーム 1 行: `index, eye, eye_index, slot, t_cam, t_raw, t_copy, valve_seq`。`eye_index` がその目の raw の何枚目か。`t_cam` はカメラのタイムスタンプ（スロットのヘッダに入っている）、`t_raw` はスロットが書き換わり始めた時刻、`t_copy` は読み終えた時刻 |
| `headers.bin` | フレーム直前の 64 バイトヘッダの生データ（frames.csv と同じ順）。先頭 8 バイトが `t_cam`（u64、ns）、残りは今のところ 0 |
| `valve.csv` | Valve のサンプル 1 件 1 行: `valve_seq, t_seen, sample_time, producer_state, sample_flag, open_l/r, gaze_l/r, cov, fix, pre_l/r, precov, extra_0..7` |
| `cues.csv` | 合図ごとに `index, t_raw, t_rel, label, seconds, beep` |
| `meta.txt` | ロック時の状態（バッファ、スロット位置、どのスロットがどの目か、スロットごとの枠合わせ、プロトコル、Valve の shm 版など）と終了時の集計。最後まで録れなかったとき（stop、Ctrl-C、映像が戻らない、ディスク不足、トラッカー終了）は `aborted=1`、最後まで録れたら `aborted=0` |
| `lock_dump.bin` | ロックした瞬間のリング周辺の生メモリ（約 2 MB）。枠合わせやヘッダの形をあとで検証する用 |
| `calib_result.json`, `calib_samples.csv` | 校正のときだけ（`calib_YYYY-MM-DD_HH-MM-SS/`）。「ライブ処理 › 校正」参照 |

**プライバシー**: 目の映像（`eye_L.raw`, `eye_R.raw`）とそれを含みうる生データ（`headers.bin`, `lock_dump.bin`）が残るのは、自分で始めた録画（`start`）だけ。
校正は settings.json で `"dev": true` にしていない限り映像を書かず、小さなテキスト（結果の値・フレームの時刻・Valve の推定値）だけを残す。
常駐中は systemd の journal（`journalctl --user -u eyecam`）にも書く: 30 秒ごとの処理の集計（左右の瞳孔の差を mm で含む）と、
校正のたびの結果（目ごとの測った値）。映像は書かない。どれも Frame の中だけで、どこにも送らない。

時刻はぜんぶ CLOCK_MONOTONIC_RAW の秒。Valve の `sample_time` も、カメラの `t_cam` も同じ時計なので、そのまま突き合わせられる
（`t_cam` はスロットが書き換わり始めてから約 6.5 ms 後の値。読み終えるのはその約 4 ms 後）。
`frames.csv` の `valve_seq` は、そのフレームを書いた時点で最後に見た Valve サンプル。

`protocol_widen.txt` 1 回（約 85 秒）で約 2.5 GB、2 分の録画で約 3.3 GB（90 fps × 2 眼の見積もり）。空きが 1 GB を切ったら止める。

### L と R が逆のセッション（a417dab までのビルド）

a417dab までは、画が左寄りのカメラを L にしていた（FrameEyeCameraFeed の規則）。これは顔の位置しだいで逆になり、
実際に `rec_2026-10-04_00-41-34` は `slot_camera=1,1,1,1,0,0,0,0` で、eye_L.raw に右目が入っている。
`slot_camera` が `1,1,1,1,0,0,0,0` のセッションは逆なので、`fix_swap.py` で直せる（まずコピーで試してね）:

```sh
python fix_swap.py rec_2026-10-04_00-41-34
```

eye_L.raw と eye_R.raw を名前の付け替えで入れ替え、frames.csv の eye 列の L/R を入れ替えて、meta.txt に `repaired_swap=1` を書く。
2 回目は断る（`--force` で入れ替え直し）。

### 古いセッション（09fbf3d までのビルドで録ったもの）

枠合わせのバグで、**スロット 0–3 の目（ふつうは L）が 64 列ずれて保存されている**。左端 64 列が欠けて、右端 64 列はパディング（真っ黒）。
復元はできない（欠けた列はファイルに入っていない）。スロット 4–7 の目は正しい。
そのころの `frames.csv` の `h0..h15` も、L 側はピクセル、R 側はタイムスタンプを float として読んだだけで意味はない。
見分け方: `meta.txt` に `framing=per_slot` が無ければ古い。convert.py も注意を出す。

## オプション

```
--serve            常駐モード（上の説明）
--fake             中身なしの常駐モード（パネル開発用。--serve を含む）
--run-dir DIR      status.json と ctl.sock の置き場所（既定 /run/user/1000/eyecam）
--seconds N        録画時間（既定 120、プロトコルがあればその長さ + 1.5）
--out DIR          保存先（既定 ~/eyecam）
--cues FILE        1 回録るモードで、ビープの合図を鳴らして cues.csv に記録
--swap             L と R の割り当てを入れ替える（ふつうは要らない）
--wait-grab N      eyecam-grab を待つ秒数（既定 600。--serve は無期限）
--wait-lock N      映像が見つかるまで待つ秒数（既定 300、外して再探索するときも同じ）
--prox-min V       近接センサーがこの値を 1 秒続けて超えたら装着中とみなす（既定 20）
--allow-one-eye    片目しか流れていなくても録る
--full-width       512 バイトの行のまま保存（パディング込み。枠合わせを疑うとき用）
--no-beep / --volume V
```

## セキュリティ: root で動く部分がすること・しないこと

`eyecam-grab`（`src/bin/eyecam-grab.rs`、約 200 行、依存は libc だけ）を sudo で動かす前に読んでね。

**すること**
1. `/run/user/1000/eyecam.sock` が uid 1000 所有のソケットか確かめる（シンボリックリンクは拒否）
2. `/proc` から、実行ファイルがちょうど `/opt/steamvr/tools/eyetracking/bin/linuxarm64/eyetracking` で、uid が全部 1000 のプロセスを 1 つだけ探す（0 個・複数なら中止）
3. そのプロセスの fd のうち、fdinfo に `exp_name: udmabuf` とある DMA-BUF だけを選ぶ（今は 16 MiB と 32 MiB の 2 本）
4. `pidfd_open` + `pidfd_getfd` でその fd だけを複製する（root が要るのはここだけ。`kernel.yama.ptrace_scope=1` のため）。複製した fd が同じバッファかもう一度確かめる
5. ソケットに接続し、相手のプロセスが uid 1000 か `SO_PEERCRED` で確かめてから、fd と小さなヘッダ（本数・サイズ・pid）を渡して終了

**しないこと**
- アイトラッカーを止める・アタッチする・ptrace する・シグナルを送る・メモリ（`/proc/<pid>/mem`）を読み書きする
- ファイルを書く、バッファを自分で読む・mmap する、それ以外の fd を渡す
- 常駐する、サービスになる

`eyecam-rec` 側も、知らない相手（root 以外）からの grab 用接続、uid 1000 以外からの ctl 接続は無視する。
受け取ったバッファは **読み取り専用**（`PROT_READ`）で mmap し、書き込みも `DMA_BUF_IOCTL_SYNC` もしない。
`eye-server.mmap` も読み取り専用で開く（`metadata_requested` などは一切書かない）。常駐モードでも root にはならない。

注意: sudo で入れる元の `eyecam-grab` は steamos が書き換えられる場所にある（`~/.local/lib/eyecam` や `target/release`）。
sudo で動かすのは、自分でビルドしたもの、またはリリースのものを中身を確認したうえで。install_grab.sh は入れる前に sha256 を出すので、
自分でビルドしたなら `sha256sum target/release/eyecam-grab` をビルド直後に控えておいて比べる。

`eyecam-rec` が grab からの接続を受けるのは、root か自分で起動した子プロセス（`SO_PEERCRED` の pid）からだけ。ただし逆向き
（eyecam-grab が渡す相手）は uid しか確かめないので、同じユーザーのほかのプログラムが先にソケットを作れば受け取れる。

## しくみのメモ

- 目のカメラは V4L2 デバイスではなく、XRService が eyetracking に渡す udmabuf の中を通る。1 フレームは 512 バイト行 × 400 行で、
  実際の画は 400 列（400..511 列はパディングで 0）。スロット 0–3 が片目、4–7 がもう片目で、各スロットは「64 バイトのヘッダ + フレーム」、
  同じ目の中のスロット間隔は 262208 バイト（256 KiB + 64）。**2 つ目の目のグループは、その間隔から予想される位置より 64 バイト後ろから始まる**
  （別々に確保されているらしい。初めての実機録画で判明）
- リングの位置は毎回変わるので探す。探し方は FrameEyeCameraFeed の `framestream.c`（MIT、Curtis English。`NOTICE` 参照）の移植で、
  装着中・20% 以上が明るい・1 秒間更新が続く、の 3 条件がそろったときだけロックする
- 正確な先頭位置は eyecam 独自で、**スロットごと**に決める: ピクセルは黒くても 0 にならず（黒レベル約 4）毎フレーム変わる一方、
  パディング列と行間のすき間は 0 のまま変わらない。その境目がぴったり合う位置を先頭にする（パディング削除がずれない／ヘッダが先頭の直前に来る）
- ヘッダの先頭 8 バイトはカメラのタイムスタンプ（u64、CLOCK_MONOTONIC_RAW の ns。90 fps で 11.11 ms 間隔）。残り 56 バイトは今のところ 0
- カメラは CPU キャッシュを通さずに書くので、読む直前にその範囲だけ `dc civac`（clean + invalidate）してから読む。
  `framestream.c` と同じやり方。clean が先なので誰かの書き込みを消すことはなく、データも変えない
- 新しいフレームは、スロットのサンプル指紋か 64 バイトヘッダが変わったことで検出し、1 ポーリング（2.5 ms）変化が止まってからコピー、
  コピー中に変わっていたら捨ててやり直す（書きかけのフレームを録らないため）

## まだ確かめていないこと

- （確認済み）スロットごとの枠合わせは、9f033c2 以降の実機の 5 セッションすべてで `framing_shift=[0, 0, 0, 0, 64, 64, 64, 64]` になった
- メモリ上の並びでの L/R の判定を、実機のロックで動かすこと（判定のしかたは 6 セッションの記録と合成テストで確認済み）
- 常駐モードで実際にバッファを受け取ってからの動き（idle → searching → recording、HMD を外したときの `locked=false`、
  トラッカー再起動で waiting_fds に戻る）。受け取る前の waiting_fds と、`--fake` での状態遷移・返事・更新頻度は確認済み
- ライブ処理を実機のバッファで動かすこと（共有メモリへの書き込み、`calib wear` / `calib user`、CPU）。同じ処理スレッドに録画済みの
  フレームを流すテスト（`livesvc::tests::worker_on_a_session`）では、かぶり校正・共有メモリ・終了時の後始末まで確認済み
