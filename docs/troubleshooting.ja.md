# うまく動かないとき

[README に戻る](../README.ja.md)

- ログ: `journalctl --user -u frameeyeosc -f`（パネルは `journalctl --user -u frameeyeosc-panel -f`）
- 困って相談するとき: パネルの「詳細」タブの「困ったとき」にある「診断」の行の［開く］（目のカメラのタブで映像が見つからない理由が出ているときは、その横の［診断を見る］）を押して、そのページのスクショを送ってください。版、目のデータ、目のカメラが映像を最後に探したときの様子、最後の校正と、返信に書ける短い「診断コード」（例 `NW·P3·B0·G1·C1R·F1`。読み方は [panel/README.md](../panel/README.md#診断コード)）が 1 枚に出ます。送り先の PC を固定していると、そのスクショに LAN の IP アドレスが写り、ファイルの場所（パス）も写ることがあります。校正や目合わせが失敗したときは、その記録も手がかりになります: 失敗の画面の［記録を見る］か、SSH で `.local/bin/frameeyeosc-panel --report latest`（理由と、そのときのログの主な行が出ます。送り先を固定していると、ログにも PC の LAN の IP アドレスが出ることがあります）
- `No Steam Link connection found; waiting for one`: Steam Link がまだつながっていません。または送り先の PC を固定してください
- `Can't send OSC to ... yet (Network is unreachable)` や `Sending OSC to ... failed (...)`: ネットワークがまだつながっていない（起動直後の Wi-Fi など）か、PC に届きません。frameeyeosc は動き続けて送り直し、送れるようになると `... works again` と出ます
- 左の列の「目のデータ」が少なめ（赤、毎秒 60 未満。90 のはずが 46 や 15 のことがありました）、または目を合わせると最初の点でサンプルが足りずに止まる: 回数の下の行に、どちらが遅いかが出ます。「本体の処理が追いついていません」なら、目のトラッカーが出したサンプルを frameeyeosc が読みそびれたか、1 つに時間がかかりすぎています。「Frame から届く数が少なめです」なら、目のトラッカーから届く数そのものが少なく、frameeyeosc は全部読めています。SteamOS 0.5.7 の SteamVR は目のカメラを毎秒 15 で動かすことがあり（SteamVR の `eyeTrackingRateMin` の設定）、そのときは代わりに「SteamVR が目のカメラを遅くしています」と出て、横の［90 にする］ですぐ毎秒 90 に戻せます（「詳細」›「困ったとき」の「目のカメラの速さ」でも同じことができ、SteamVR に任せるにも戻せます）。少なめが 10 秒続くと、数字を 1 行ログに残します（`journalctl --user -u frameeyeosc` に `Eye data has been low for 10 s: …`）。その行と、PC から Steam Link で送っていたかどうかを知らせてください
- ログに `Dropped … datagrams to … the network was too busy to take them at once`: ネットワーク（Steam Link の映像でいっぱいの Wi-Fi など）が送る速さに追いつかなかったので、その分を捨てました。frameeyeosc は送り終わるのを待たないので、目のデータは止まらずに届きます。この行は 1 分に 1 回までです
- ログに `Sending OSC to ...` と出ているのにアバターが反応しない: VRChat の OSC が有効かを確認したうえで、Windows のファイアウォールを確認してください。VRChat の受信許可は「パブリック」だけになっていることが多く、「プライベート」の家のネットワークから届く OSC は止められます。許可の対象が `launch.exe` ではなく `VRChat.exe` になっているかにも注意してください。範囲をしぼって許可するには（管理者の PowerShell で）:
  ```powershell
  New-NetFirewallRule -DisplayName "VRChat OSC (LAN UDP 9000)" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 9000 -RemoteAddress LocalSubnet -Program "C:\Program Files (x86)\Steam\steamapps\common\VRChat\VRChat.exe" -Profile Private,Public
  ```
  「プライベート」と「パブリック」の両方にしてあるのは、家のネットワークは「プライベート」のことが多い一方で、付属の無線アダプタは Windows では別のネットワークとして見え、たいてい「パブリック」になっているからです
- SteamOS 0.4.3 で目が見開かず、まぶたタブに「開き具合が 1.0 で頭打ち」と出る: SteamOS 0.4.3 から、Frame はふつうに開いた目を上限の 1.000 と読みます（ある人の左目は、両目を開いているあいだ、それまで中央値 0.754、0.4.3 では 1.000 が 75〜93%）。それより上がないので、見開きを読み取れません。frameeyeosc は版ではなく読んだ値から判断し（両目を開いている直近 1 分で、どちらかの目が 1.000 のサンプルが半分を超える。[状態ファイル](settings.ja.md#状態ファイル) の `openness_saturated`）、まぶたタブにそう出します。のちの SteamOS で変われば、表示は自然に消えます（アップデートの再起動のあとなど、次に frameeyeosc が起動したときから。見る向きで割合が上下するので、一度そう判断したら frameeyeosc が動いているあいだはそのままです）。そのあいだはどのまぶたも「普通に開いた」より上では送らないので、目を合わせていない目（1.000 が見開きと読まれる）がずっと見開いて見えることもありません。まばたきと閉じるのは今までどおりで、「見開きやすさ」の設定も、また見開きが届くようになったときのためにそのまま残ります。[目のカメラ](eyecam.ja.md#目のカメラ) を使っている目は、カメラの値で見開きます
- 両目がいつも同じ向きを見る、片目の視線がもう片方の目についていく: 「Track Dominant Eye Only」の設定（VR Settings > General の詳細。SteamOS 0.4.3）がオンです。Frame はその目だけを追い、両目にその目の視線を渡します（まぶたはそれぞれの目のまま）。パネルの左の列にも出ます（「Frame の設定: 右目だけで追っています」）。両目を追うには、この設定をオフにしてください。frameeyeosc はこの設定を読むだけで、変えません
- パネルに「目のデータを読めません: …」と出る: frameeyeosc は動いていますが、アイトラッキングのデータを読めていません。1 秒ごとに読み直します（理由は `journalctl --user -u frameeyeosc` にも 1 回だけ出ます）。ヘッドセットの起動直後（`… No such file or directory`）なら問題ありません。アイトラッキングがまだ起動していないだけです。「unsupported eye shared-memory version」なら、SteamOS の更新で frameeyeosc が読む形式が変わっています（[免責事項](../README.ja.md#免責事項) を参照）
- ヘッドセットを外していると何も動かない: 正常です。Frame は被っている間しか目を追いません
- パネルに「本体が動いていません」と出る: `systemctl --user status frameeyeosc` を確認してください。パネルで変えた設定は保存されていて、動き出したら反映されます
