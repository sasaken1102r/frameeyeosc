# 設定と状態ファイル

[README に戻る](../README.ja.md)

## 設定

設定は `~/.config/frameeyeosc/config.json` にあります。パネルが書きますが、手で書いてもかまいません。frameeyeosc は 1 秒に 10 回このファイルを見て、変わっていたら再起動せずに反映します。書いていない項目は既定値、知らない項目は無視します。ファイルが壊れていたり値が範囲外だったりしたときは、前の設定のまま動き続け、エラーを出します（パネルと状態ファイルに出ます）。

```json
{ "output": "vrchat", "gaze_min_cutoff": 0.3, "lid_sync": 0.6 }
```

| キー | オプション | 既定値 | 内容 |
|---|---|---|---|
| `sending` | | `true` | `false` で送信を一時停止（VRChat モードでは `EyeTrackingActive` の「無効」を `eye_tracking_active` の型で 1 回送る。LiveLink モードでは普通に開いて正面を見た目を 1 回送る） |
| `output` | `--output` | `"vrchat"` | `"vrchat"` は VRChat にアバターパラメータを送る、`"livelink"` は VRCFaceTracking の LiveLink モジュールに Live Link Face の形式で送る、`"frameeyeosc"` は VRCFaceTracking の FrameEyeOSC モジュールに送る。0.10 でなくした ETVR モードの `"etvr"` は `"vrchat"` として読む（[前の版に戻す](update.ja.md#前の版に戻す)） |
| `host` | `--target` | `"auto"` | `"auto"` は Steam Link の接続先 PC。それ以外は IP アドレスかホスト名（ポートは付けない） |
| `port` | `--port`、`--target` | `null` | `null` は `vrchat` なら 9000、`livelink` なら 11111、`frameeyeosc` なら 11112 |
| `prefix` | `--prefix` | `"/FT"` | パラメータ名の頭。`""` で頭なし。LiveLink モードでは、VRChat に直接送る瞳孔（`pupils_to_vrchat`）にだけ使う |
| `lan_only` | `--no-lan-only` | `true` | 家のネットワークの中にだけ送る。送り先（本体の送り先、VRChat に直接送る瞳孔、[ほかのアプリ](outputs.ja.md#ほかのアプリ)のそれぞれ）は、ループバック（127.x、::1）、プライベート（10.x、172.16〜31.x、192.168.x、IPv6 の fc00::/7）、リンクローカル（169.254.x、fe80::）か、Tailscale などの VPN が使う 100.64〜127.x（VPN と表示）でないといけない。それ以外の住所（公開の住所）には送らず、ログに出し、パネルに出す。`false` なら公開の住所にも送り、「公開」と表示する。マルチキャストとブロードキャストの住所にはどちらでも送らない。Steam Link の PC（`"auto"`）は、Steam Link が使う住所を確かめるまでは断らず、ログに出すだけ |
| `extra_outputs` | `--extra`、`--no-extras` | `[]` | 目のデータのほかの送り先（3 つまで）。[ほかのアプリ](outputs.ja.md#ほかのアプリ)を見てください |
| `eye_tracking_active` | `--eye-tracking-active` | `"bool"` | VRChat モードで `EyeTrackingActive` をどう送るか: `"bool"`（true / false）、`"float"`（1.0 / 0.0。アバターによってはこちらが必要）、`"off"`（送らない。止めたときや目を見失ったときの 1 回の「無効」も送らない）。ほかのモードではもともと送らない |
| `steamlink_params` | `--steamlink-params` | `false` | VRChat モードで、SteamVR の Steam Link が自分の OSC で送るアバターのパラメータ（`LeftEyeX`、`RightEyeLid` など）も送る。それに合わせて作ったアバター用（[Steam Link の OSC 向けのアバター](outputs.ja.md#steam-link-の-osc-向けのアバター)）。頭（`prefix`）は付けない。ほかのモードでは使わない |
| `native_eyes` | `--native-eyes` | `false` | VRChat モードで、VRChat 自身のアイトラッキング入力（`/tracking/eye/*`）も送る。VRCFT のパラメータを持たないアバターの目が動く（[VRChat のアイトラッキング入力](outputs.ja.md#vrchat-のアイトラッキング入力)）。送り方タブにスイッチがある |
| `vrc_face_tracking` | `--vrc-face-tracking` | `false` | VRChat モードで、VRChat の Face Tracking 入力（`/tracking/face/v1`）にも送る。目ごとのまぶた・見開き・視線と、目のカメラの細めと瞳、`brow_from_widen` で上げた眉を送る。VRChat の Face Tracking を設定したアバター用（[VRChat の Face Tracking](outputs.ja.md#vrchat-の-face-tracking)）。送り方タブの「Face Tracking にも送る」 |
| `camera_lids` | `--no-camera-lids` | `true` | 準備が済んでいれば、目のカメラの値を使う（[目のカメラ](eyecam.ja.md#目のカメラ)）: 普通に開いた目から上のまぶた（見開き）と、カメラが閉じている・細めても開いていると見た目のまぶた、目を細めた動き、瞳孔の大きさ。`false` で Valve の値だけ。frameeyeosc が eyecam-rec にも映像からの計算を止めさせる（`live off`。eyecam 自体は動き続ける。[止める・取り除く](eyecam.ja.md#止める取り除く)）。「基本」タブの「目のカメラ」の「使う / 使わない」 |
| `pupils_to_vrchat` | `--no-pupils-to-vrchat` | `true` | LiveLink モードで、目のカメラの瞳孔の大きさを VRChat に直接送る（同じ PC のポート 9000）。LiveLink モジュールは瞳孔を運ばないため。ほかのモードでは関係ない |
| `pupil_bits` | `--pupil-bits` | `0` | 目のカメラの瞳孔を VRChat に送るとき、開き具合をこの個数の bool のパラメータ（`v2/PupilDilation1`、`2`、`4`、`8`）でも送る。ビットで受け取るアバター用。`0` で小数だけ。1〜4。[ビットで受け取るアバターの瞳孔](eyecam.ja.md#ビットで受け取るアバターの瞳孔) を参照。送り方タブの「瞳孔の受け取り方」 |
| `raw` | `--raw` | `false` | スムージングしない。時間を使う処理（途切れ消し、視線を止める、品質チェック、閉じたまま保つ、真下で左右を止める）もしない |
| `gaze_min_cutoff` | `--gaze-min-cutoff` | `0.3` | 下げるほど止まっている時の視線が安定（その分遅れる） |
| `gaze_beta` | `--gaze-beta` | `1.5` | 上げるほど素早い視線の動きに遅れず付いていき、動いたあと早く落ち着く |
| `gaze_d_cutoff` | `--gaze-d-cutoff` | `0.5` | 下げるほど、トラッキングのノイズで視線のフィルタがゆるみにくく、素早い目の動きにもパッと飛ばずにやわらかく付いていく |
| `gaze_deadzone` | `--gaze-deadzone` | `0.005` | これより小さい視線の変化は無視（1.0＝45°）。視線は目が止まった所からこの分だけ手前で止まることがある |
| `gaze_hold_below` | `--gaze-hold-below` | `0.5` | どちらかの目の開き具合がこれより小さい間は視線を止める。`0` で無効。片目だけ下回っているときは 1 秒まで（ウインク）止め、それより長いとその目の読みは誤りとみなして、もう片方の目で視線を動かす |
| `independent_eyes` | `--independent-eyes` | `false` | 両目に同じ視線を送る代わりに、共通の左右の向きから両目を決まった 2°（2 m ほど先を見ているくらい）だけ寄せる。上下は目ごと、視線が不確かな目はもう片方の目のものを使う。Frame の目ごとの左右の値は使わない: 0.9 m 先の点（両目の差 4.4°）を見ても差が 0.1〜17.3° と読め、録画中も 1° 未満と 4° 超えのあいだを 1 分に 20〜32 回行き来していた（アバターが寄り目になり、p95 で 13〜19°）。わざと寄り目にしたときは見せる: 両目が 0.7 以上開き、15° より深く下を見ていないまま、Valve の左右差が 20° 以上を 0.5 秒（または 25° 以上を 0.25 秒）続けたら通し、15° 未満が 0.15 秒続いたら戻す。15° 未満から 2.5 秒以内に上がったときだけで、まばたきのすぐあと（0.3 秒）は数えない。Valve がずっと寄り気味に読むかぶりでは、3 つの線をまとめて、直近 60 秒の中央値より 10° 上まで上げる。「Track Dominant Eye Only」の設定がオンのあいだは、どちらでも両目とも追っている目の視線になる |
| `own_eye_gaze` | `--own-eye-gaze` | `0` | `independent_eyes` のとき、Valve の目ごとの左右の視線をどれだけ使うか、`0`〜`1`。`0` で上の寄せた視線、`1` で目ごとの値をそのまま（上のとおり大きく揺れるので、ぶれる）、あいだはその割合だけ寄せる。まぶた・なめらかさ・止めるのはそのまま、視線が不確かな目はもう片方の目のものを使う。視線タブの「動く範囲」の「目ごとの視線」 |
| `gaze_quality_limit` | `--gaze-quality-limit` | `0`（オフ） | 念のための安全策: Frame が出す視線の不確かさ（共分散）がこれ（例: `0.03`）より大きい目の視線は使わない。片目だけならもう片方の目で両目を動かし、両目ともなら視線を止める。まぶたには影響しない。きちんと合ったヘッドセットでは測って差が出なかった。不確かさが上がるのはほぼ目を閉じかけている間だけで、そこは `gaze_hold_below` がもう視線を止めているため |
| `despike` | `--no-despike` | `true` | 視線と開き具合の 1 サンプルだけの途切れを消す（3 サンプルの中央値。全体が約 11 ms 遅れる） |
| `lid_min_cutoff` / `lid_beta` | `--lid-min-cutoff` / `--lid-beta` | `6.0` / `5.0` | まぶたのなめらかさ（視線と同じ考え方） |
| `lid_closed` / `lid_open` / `lid_widen_start` / `lid_wide` | `--lid-closed` など | `0.30` / `0.80` / `0.92` / `1.00` | Frame の開き具合を「閉じ／普通／見開き」にどう対応させるか |
| `lid_widen` | `--lid-widen` | `"normal"` | 目を合わせた目の見開きやすさ: `"off"`・`"low"`・`"normal"`・`"high"`（[目を合わせる](eye-fit.ja.md#目を合わせる) を参照）。目を合わせていない目は `lid_widen_start` / `lid_wide` を使う |
| `widen` | `--no-widen` | `true` | そもそも見開くか。`false` で見開かない: 目のカメラ・`lid_widen`・まぶたの目盛りが何と言っても、まぶたはふつうに開いた所より上に行かない（VRChat の `EyeLid` は 0.75 まで、VRCFT の LiveLink・FrameEyeOSC モジュールでは `EyeWide` が 0）。閉じる・まばたき・目のカメラの細めはそのまま。まぶたタブの「見開き」（する / しない） |
| `brow_from_widen` | `--brow-from-widen` | `false` | 目を見開くと、アバターの眉も上げる。目のカメラは眉を見ていないので、見開きが 15 % を超えた目の眉を上げ、いちばん見開いて `brow_strength` まで。`widen` が `false` でも上げる。閉じかけ・閉じている目の眉はそのまま、ウインクでは閉じた目の眉が開いた目の眉に合わせる。眉を下げることはしない。VRChat に直接なら `v2/BrowExpressionLeft` / `Right` / `BrowExpression`・`v2/BrowUpLeft` / `Right` / `BrowUp`・`v2/BrowInnerUp`・`v2/BrowOuterUp`（止まるときに 1 回 0）、LiveLink は `BrowInnerUp` と `BrowOuterUpLeft` / `Right`、FrameEyeOSC はパケットの眉の部分（108 バイト）。眉のパラメーターがあるアバターで動く。まぶたタブの「見開きで眉も上げる」 |
| `brow_strength` | `--brow-strength` | `0.8` | `brow_from_widen` でいちばん見開いたときの眉の上がり方、`0`〜`1`。まぶたタブの「眉の上がり方」 |
| `lid_scale_left` / `lid_scale_right` | `--lid-scale-left` / `--lid-scale-right` | `null`（学習値） | 学習値の代わりに固定の倍率を使う。目を合わせた目では、合わせたあとの微調整になる: 0.9 でその目が 1 割閉じ気味に読まれる（閉じやすく、見開きにくくなる）。`null` は 1.0。目を合わせ直すと（全部のとき）と［元に戻す］で `null` に戻る |
| `lid_calibration` | `--no-lid-calibration` | `true` | まぶたを学習する |
| `lid_sync` | `--lid-sync` | `0.4` | 左右のまぶたの小さな差を揃える。大きな差（ウインク）はそのまま。`0` で無効 |
| `blink_hold_ms` | `--blink-hold-ms` | `80` | 目が閉じたら、少なくともこの時間は完全に閉じた値を送る。短いまばたきもほかの人に届くように。`0` で無効 |
| `blink_sync_below` | `--blink-sync-below` | `0.35` | 片目が閉じていて、もう片方がこれより小さい（VRCFT の値）とき、両目とも閉じて送る。もう片方が開いているウインクはそのまま。`0` で無効 |
| `camera_lid_floor` | `--camera-lid-floor` | `0` | [目のカメラ](eyecam.ja.md#目のカメラ)が目を開いていると見ているあいだ、まぶたをこれより下げずに送る（VRCFT の値、0〜0.75）。強く細めても細めた目に見え、残りは細めのパラメータが受け持つ。まばたきと、カメラが閉じていると見た目はちゃんと閉じる。カメラの値がない目は変わらない。`0` で無効。まぶたタブの「細めたときのまぶたの下限」 |
| `camera_close_gate` | `--no-camera-close-gate` | `true` | [目のカメラ](eyecam.ja.md#目のカメラ)の「閉じた」が片目だけのときの扱い。もう片方の目のカメラも 0.15 秒以内に閉じたと見たか、Valve の開き具合でもう片方の目が 0.5 未満ならまばたきとして、0.2 秒続いたらウインクとして使う。それまでは、カメラが最後に開いていると見たまぶたのままにするので、開いた目をカメラが短く「閉じた」と読んでも（瞳が見つからないときなど）目は閉じない。Valve の視線が 18° より下を向いているあいだは、Valve の開き具合でどちらかの目が 0.25 未満（本当のまばたき）でないかぎり、カメラの「閉じた」は使わない（下を見ると上まぶたが瞳にかぶさり、両目のカメラが閉じたと読むため。開いた目の Valve のまぶたも 0.5 のあたりを行き来する）。「下を見ている」と「そこでのまばたき」は、どちらも最後のサンプルから 0.15 秒保つ。`false` = 前と同じく、カメラの「閉じた」をすぐ使う |
| `camera_closed_doubt` | `--no-camera-closed-doubt` | `true` | [目のカメラ](eyecam.ja.md#目のカメラ)があるとき、Valve がその目を開いていると自信を持って読んでいるのに、カメラの「閉じた」がまばたきより長く続いたら、開いているとみなす（寝転ぶと、カメラが映像の端で瞳を見失うことがあるため）。`false` = みなさない。閉じた目を Valve が開いていると読むヘッドセット向け（SteamOS 0.4.3 のある Frame では、ウインクしている右目を Valve が自信を持って 1.00 と読み、ウインクが 0.5 秒で開いてしまった）。横を大きく向いたときの確かめはどちらでも行う |
| `lid_open_snap` | `--lid-open-snap` | `0.53` | [目のカメラ](eyecam.ja.md#目のカメラ)のまぶたがない目で、送るまぶた（VRCFT の値、0〜0.75。目を合わせた目では左右の倍率 `lid_scale_*` はこのあとにかけるので、微調整はそのまま効く。目合わせのない目では倍率がその目の値を決めているので、倍率のあと）がこれより開いていたら、ふつうに開いた目（0.75）までなめらかに持ち上げる。この値から 0.75 までの半分のところで 0.75 に届く。既定は、開いた値が 1.000 の目合わせで開き具合 0.80 が来るところ。`0.75` で無効。開いた値に対する割合だったころの 0.75 より大きい値は、一度だけ置き換える（0.80 → 0.53）。まぶたタブの「まばたき・細め」の「ほぼ開いてたら全開にする」 |
| `camera_lid_open_snap` | `--camera-lid-open-snap` | `0.63` | 同じことを[目のカメラ](eyecam.ja.md#目のカメラ)のまぶたがある目で: 送るまぶた（VRCFT の値、0〜0.75）がこれより開いていたら、ふつうに開いた目までなめらかに持ち上げる。開いた目の小さな下がりは開いた目で送る。見開きと、これより下の細め・まばたきはそのまま。`0.75` で無効（0.9.0 までの動き）。まぶたタブの「まばたき・細め」の「ほぼ開いてたら全開にする」の「Valve の目」の隣の「カメラの目」 |
| `gaze_offset_x` / `gaze_offset_y` | `--gaze-offset-x` / `--gaze-offset-y` | `0` / `0` | 正面とみなす視線。-0.5〜0.5（1.0＝45°、＋ は右・上）。目を合わせると決まる |
| `gaze_gain_x` / `gaze_gain_up` / `gaze_gain_down` | `--gaze-gain-x` / `--gaze-gain-up` / `--gaze-gain-down` | `1.0` | そこから左右・上・下にどれだけ動かすか。0.5〜2。全部を合わせると、上は直近 5 回の平均になり、左右と下は 1 に戻る |
| `gaze_scale_left` / `gaze_scale_right` / `gaze_scale_up` / `gaze_scale_down` | `--gaze-scale-left` など | `1.0` | 正面から左・右・上・下へ、視線がどこまで動くかの自分で決める倍率。0.05〜2.0。目合わせとフィルターのあとで両目まとめた視線に掛ける（左右の目は同じだけ動く）。目合わせでは変わらない。視線タブの「動く範囲」の「視線の倍率」 |
| `gaze_roll_deg` | `--gaze-roll-deg` | `0` | ヘッドセットが何度傾いているか（-20〜20。＋ は右を見ると上に読む向き）。正面を中心に、ゲインの前に傾きを戻す。左右の目と両目の視線どれにも効く。目合わせではもう測らない: 全部を合わせると 0 になり、0.7.7 より前の config.json も一度だけ 0 にする（パネルが行う。`version` 4） |
| `gaze_offset_x_left` / `_right`、`gaze_gain_x_left` / `_right` | `--gaze-offset-x-left` など | `null` | 目ごとの左右の 0 点と幅。もう片方の目の視線が不確かで、この目の視線で代わりをするときだけ使う。目合わせではもう決めない: 全部を合わせると `null` になり、0.7.7 より前の config.json も一度だけ `null` にする（パネルが行う。`version` 4）。`null` = `gaze_offset_x` / `gaze_gain_x` を使う。上下は Frame が両目で共有しているので目ごとの値はない |
| `gaze_down_hold_x_deg` | `--gaze-down-hold-x-deg` | `24` | 真下を見ると、Frame の左右の視線が跳ねます（右へ 19° くらい）。この角度より下を見ている間は、左右の視線（両目とまとめた視線）を、その手前の値へ寄せます（さらに 10° 下で完全に止める）。角度はトラッカーのそのままの値（正面の位置・幅をかける前）。上下は変えない。`0` で無効 |
| `gaze_debug_dots` | | `false` | デバッグ用: 送っている視線の向きに、1 m 先（`gaze_debug_dots_distance_m`）に小さい点を出す（`independent_eyes` のときは目ごとに、それぞれの目から。左は水色、右はオレンジ）。アバターに届いている視線が見えます。視線の倍率を使っている（どれかが 100 % でない）あいだは、倍率を掛ける前の視線（目が実際に見ている向き）も同じ色の輪で出します（同じ向きなら点が輪の中に入る）。オンの間は、frameeyeosc が処理したサンプルを 1 つずつ状態ファイルのフォルダの Unix ソケットでパネルに渡します。ヘッドセットの外には出さず、オフのときは渡しません。目を合わせている間は隠します |
| `lid_fit_closed_left` 〜 `lid_fit_down_right` | | `null` | 目ごとの Frame の開き具合: 目を閉じたとき、上・正面・下を見て開いているとき（`closed` / `up` / `open` / `down`、`_left` / `_right`）。目を合わせると決まる。`null` = まだ合わせていない。合わせた目は、自動キャリブレーションの代わりにこれを使い、下を見ただけでは閉じない。`lid_scale_*` はそのあとの微調整になる |
| `calibration_reset` | | `0` | 増やすと、まぶたの学習をやり直す |
| `language` | | Steam の言語 | パネルの言語。`"ja"` か `"en"`。書いていないときは、Steam の言語が日本語なら日本語、それ以外なら英語 |
| `gaze_debug_dots_distance_m` | | `1.0` | 視線の点を何 m 先に出すか（0.3〜2.0 m。詳細タブの「動作確認」の「点の距離」）。ダッシュボードを開いても閉じても同じ。1.2 m くらいより奥だと、開いたダッシュボードに隠れる。パネルが使い、frameeyeosc 本体は使わない |
| `gaze_debug_rings` | | `true` | 視線の点といっしょに、倍率を掛ける前の視線の輪を出すか（詳細タブの「動作確認」の「倍率の前の向き（輪）」）。オフなら点だけ。パネルが使い、frameeyeosc 本体は使わない |
| `fit_sounds` | | `true` | 目を合わせている間、パネルが短い音を鳴らす。frameeyeosc 本体は使わない |
| `auto_recenter` | | `"center"` | 視線を合わせてあるとき、ヘッドセットを被るたびにパネルが 1 回だけ自動で合わせ直すもの: `"center"` 真ん中だけ（真ん中の点 1 つ・2.5 秒）、`"off"` 何もしない。0.7.7 より前の `"tilt"` は `"center"` として読む（一度だけ書き換える）。［もう一度合わせる］の隣のボタンは、どちらでも真ん中だけ合わせ直す。frameeyeosc 本体は使わない |
| `update_check` | | `true` | パネルが起動時と 1 時間に 1 回、GitHub に新しい版がないか確かめ、frame.sasaken1102s.net から私のほかの Frame アプリの一覧を取ってくる（「詳細」›「バージョン」。プライバシーの項を参照）。frameeyeosc 本体は使わない |

コマンドラインのオプションは、このファイルより優先されます。オプションは `~/.config/frameeyeosc/env` に書き、`systemctl --user restart frameeyeosc` で反映します:

```sh
FRAMEEYEOSC_ARGS="--gaze-min-cutoff 0.3 --lid-sync 0.6"
```

ここで指定した項目はファイルからは変えられず、パネルでは「コマンドで固定中」と出ます。すべてのオプション（別の設定ファイルを使う `--config` など）は `~/.local/bin/frameeyeosc --help` で確認できます。

## 状態ファイル

frameeyeosc は 1 秒に 10 回、今の様子を `$XDG_RUNTIME_DIR/frameeyeosc/status.json`（ふつうは `/run/user/1000/frameeyeosc/status.json`）に書きます。中身は、送信中か、送り先、毎秒の送信回数、目のトラッカーから毎秒届くサンプルの数（`tracker_rate`）、frameeyeosc がそれに追いついているか（`missed_rate`: 直近 1 秒に目のトラッカーが出したのに読めなかった数、`max_processing_ms`: 直近 1 秒でサンプル 1 つにいちばん長くかかった時間、`dropped_rate`: 直近 1 秒にネットワークが混んでいて捨てた送信の数）、最新の生の値と送った値、キャリブレーション、今効いている設定、コマンドで固定中の項目、設定のエラー、目のデータを読めないときはその理由（`source_error`）、「Track Dominant Eye Only」の設定がオンのあいだ Frame が追っている目（`dominant_eye`: `"left"` か `"right"`。オフなら `null`）、ふつうに開いた目が 1.0 と読まれて見開きが届かないかどうか（`openness_saturated`）、目のカメラ（`camera`: 値が届いているか、目ごとに使っているか `used` / `pupil_used`、読めないときはその理由。eyecam が動いていなければ `null`）、LiveLink モードで瞳孔を送っている先（`pupil_target`）、`lan_only`、本体と瞳孔の送り先の住所の種類（`net` / `pupil_net`: `"lan"`、`"vpn"`、`"public"`）と送らない理由（`refused` / `pupil_refused`。例 `"public address"`）、[ほかのアプリ](outputs.ja.md#ほかのアプリ)のそれぞれ（`extras`: 項目の中身、送り先、`net`、送った回数、送らない理由 `refused` / `last_error`）、frameeyeosc が最後にログに出した問題とその時刻（`last_error`: `text` と `time`。直ったあとも残る。例 "No Steam Link connection found; waiting for one"。まだ無ければ `null`）、最後の目合わせの測定です。パネルはこれを読んで表示します。フォルダもファイルも本人しか読めず、メモリの上にあって再起動すると消えます。残るのは最新の値だけです。状態ファイルは frameeyeosc 自身のためのもので、版によって中身が変わります。ほかのプログラムとの約束ではありません（約束は [docs/protocol.md](protocol.md) のパケットです）。
