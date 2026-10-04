# analysis（記録用）

src/live.rs の決め方（虹彩の半径で割る、閉じた目の判定、見開きの段、かぶり直しをまたいだ評価など）を選んだときの、
解析スクリプトの記録。xwear.py（かぶり直しをまたいだ評価）と mapping.py（VRCFT の値への写し方）が live.rs のもとになっている。

**そのままでは動かない**: tab.py が使う `ana0.py`（録画セッションの読み込み）と、sheets.py が使う `viz2.py`（フレームに
検出結果を描く）は残っていない。tab.py を読み込むスクリプト（boxes / evalx / lat / mapping / scat / series / sheets / xwear）は
どれも動かないので、考え方と数式を読むためのものとして置いている。録画を読むだけなら、一つ上の convert.py と eval_live.py が動く。

録画（`rec_*`）には目の映像が入るので、解析は Frame か自分の PC の中だけでして、どこにも上げないでね。
