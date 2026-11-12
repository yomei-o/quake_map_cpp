# quake_map_cpp — 地震モニター（C++ / WASM）

気象庁が発表した震源・震度と、緊急地震速報を、光る日本地図に出します。
画面はすべて C++ で描いています（地図・観測点・揺れの広がり・一覧・ボタン・緊急地震速報の帯・文字まで）。
ブラウザの JavaScript がやるのは、JSON を取ってきて WASM に渡すことと、指・マウスの動きを渡すことだけです。

**デモ**: https://yomei-o.github.io/quake_map_cpp/

- 右（スマホでは下）の一覧は、気象庁が発表した最近の地震。押すとその地震を選び、揺れの広がりを再生します
- 選んだ地震は、観測点ごとの震度を色で出します。「揺れの広がりを再生」で、震源から P 波（青）と S 波（橙）の輪が広がり、S 波が着いた観測点から順に灯ります
- 新しい地震が発表されると、自動でそれを選んで再生します
- 緊急地震速報を受けると、上に帯（警報は赤、予報は橙）を出し、震源と、発生からの P 波・S 波の輪を描きます
- 「表示テスト」は、選んでいる地震の震源で、いま緊急地震速報が出たことにして表示を見るためのもの（本物ではないと帯に書きます）
- ドラッグで移動、ホイール・2 本指で拡大縮小。観測点にマウスを乗せる（タップする）と名前と震度

> このページは見て楽しむためのものです。防災の判断には、気象庁・テレビ・ラジオ・スマホの緊急速報などの公式の情報を使ってください。

## データ

どれもキー無しで、ブラウザから直接取れます（`Access-Control-Allow-Origin: *`）。

| 何 | どこから | 取り方 |
|---|---|---|
| 発表の一覧（震源・最大震度） | 気象庁 `https://www.jma.go.jp/bosai/quake/data/list.json` | 20 秒ごと |
| 観測点ごとの震度と緯度経度 | 気象庁 `https://www.jma.go.jp/bosai/quake/data/<json>`（「震源・震度情報」の本文） | 選んだ地震だけ |
| 緊急地震速報 | Wolfx `wss://ws-api.wolfx.jp/jma_eew`（気象庁の電文を JSON にしたもの）。開いたときに `https://api.wolfx.jp/jma_eew.json` で直近の 1 件 | WebSocket で届いたとき |

- 揺れの広がりは、P 波 7 km/s・S 波 4 km/s で一定と仮定した近似です（震源の深さは考えて、地表に出た輪を描きます）
- 緊急地震速報は、発生から 3 分たつと消えます（取消は 20 秒出します）

## しくみ

| ファイル | やること |
|---|---|
| `quake.hpp` | 気象庁の一覧・本文と、緊急地震速報の JSON を読む（nlohmann/json）。時刻・震度の扱い |
| `quake_map.hpp` | 画面をまるごと描く。層を 3 つに分け、背景と陸は表示範囲が変わったときだけ、観測点とパネルはデータが変わったときだけ描き直し、毎フレームは揺れの輪・点滅・帯・吹き出しだけ |
| `raster.hpp` | ソフトウェア描画（metro_map_cpp のものに、多角形の塗りと角丸の四角を足した）。文字は stb_truetype |
| `tools/build_basemap.py` | Natural Earth の都道府県の形を間引いて `data/basemap.txt` に |
| `tools/subset_font.py` | Noto Sans JP を JIS 第 1・第 2 水準の文字に絞る（観測点の名前は前もってわからないので、全部入れる。2.1 MB） |
| `wasm/` | ブラウザ用の入口とビルド |
| `main.cpp` | ネイティブで同じ画面を PNG に描く（取っておいた JSON で確かめる用） |

## ビルド

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# list.json と、本文の JSON を置いたフォルダ、緊急地震速報の JSON を渡す → out/q_replay.png, out/q_still.png, out/q_eew.png
build/Release/quake_map.exe data/basemap.txt fonts/NotoSansJP-Bold-jis.ttf out/q 1280 800 list.json <本文のフォルダ> eew.json
sh wasm/build.sh   # docs/qm.js, docs/qm.wasm
```

地図とフォントを作り直すとき:

```sh
python tools/build_basemap.py ne_10m_admin_1_states_provinces.shp > data/basemap.txt
python tools/subset_font.py "NotoSansJP[wght].ttf" fonts/NotoSansJP-Bold-jis.ttf
```

## 出典とライセンス

- コード: BSD-3-Clause（`LICENSE`）
- 震源・震度: 気象庁（出典：気象庁ホームページ）。表示のために加工しています
- 緊急地震速報: Wolfx Project（https://wolfx.jp/ ）が配信する、気象庁の緊急地震速報の電文を JSON にしたもの
- 地図: Natural Earth（パブリックドメイン）を間引いたもの（`data/basemap.txt`）
- 文字: Noto Sans JP（SIL Open Font License 1.1、`fonts/OFL.txt`）。JIS 第 1・第 2 水準の文字に絞り、太さ 700 に固定したもの
- stb_truetype / stb_image_write: パブリックドメイン（または MIT）、nlohmann/json 3.12.0: MIT
