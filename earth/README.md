# M5Stack StopWatch – 回転する地球

M5Stack StopWatch (C152, ESP32-S3 / 1.75" 466×466 AMOLED) に、1 分で 1 回転する地球を表示します。

## ガラス半球越しの立体感のための工夫

- 地球の半径を画面の 86% にして、球の輪郭をドームの縁に近づけています
- 斜め前から光を当てています（拡散光 + 明暗境界のぼかし + 海面の鏡面ハイライト）
- 地球の縁に大気の青み、外側に大気のグローを付けています
- 夜側に都市の灯り（夜景テクスチャがある場合）を表示します
- 本体を傾けると IMU で地球だけが最大 10px 動き、背景の星との間に視差が生まれます

## テクスチャの準備

NASA の画像を使います（クレジットは末尾の「画像のクレジット」を参照）。

```sh
mkdir -p assets
curl -L -o assets/earth_day.jpg   "https://commons.wikimedia.org/wiki/Special:FilePath/Land_shallow_topo_2048.jpg"
curl -L -o assets/earth_night.jpg "https://commons.wikimedia.org/wiki/Special:FilePath/Earthlights_dmsp.jpg"
python3 tools/make_texture.py --day assets/earth_day.jpg --night assets/earth_night.jpg
```

NASA のサーバー（eoimages.gsfc.nasa.gov）がつながる場合は、次の URL から取得しても同じ画像です。

- 昼: https://eoimages.gsfc.nasa.gov/images/imagerecords/57000/57752/land_shallow_topo_2048.jpg
- 夜: https://eoimages.gsfc.nasa.gov/images/imagerecords/55000/55167/earth_lights_lrg.jpg

正距円筒図法（横:縦 = 2:1）の画像であれば、ほかの画像も使えます。変換には macOS の `sips` を使います。
`python3 tools/make_texture.py --placeholder` を実行すると、手続き生成の仮の大陸で動作を確認できます。

出力ファイル:

- `data/earth_day.rgb565`：1024×512、RGB565
- `data/earth_night.gray8`：1024×512、8 bit 輝度

どちらもビルド時にファームへ埋め込まれ、起動時に PSRAM へコピーされます。

## ビルドと書き込み

```sh
pio run -t upload && pio device monitor
```

書き込みモードに入れない場合は、電源ボタンを約 2 秒押して緑 LED が点いたら離してから書き込んでください。

## 調整 (`src/main.cpp` 冒頭)

| 定数 | 意味 |
|---|---|
| `ROTATION_US` | 1 回転の時間（初期値 60 秒） |
| `EARTH_RADIUS` | 画面半径に対する地球の大きさ。ドームの見え方に合わせて調整 |
| `SUN_X/Y/Z` | 太陽の方向。Z を小さくすると欠けが大きくなり、立体感が強まる |
| `AXIS_TILT_DEG` / `AXIS_TOWARD_DEG` | 地軸の傾き |
| `PARALLAX_PX` | IMU 視差の量（0 で無効） |
| `PARALLAX_SIGN_X/Y` | 傾けたときに地球が逆に動く場合は符号を反転 |

## 描画の仕組み

約 20fps で動作します（自転による移動は中心付近で 1 フレームあたり約 1px）。

- 起動時に、各ピクセルの緯度・経度と静的な陰影（明るさ、大気の強さ、鏡面/夜景の重み）を 6 バイトのテーブルにまとめます（PSRAM）
- 自転は経度にオフセットを足すだけなので、毎フレームの処理は「テクスチャ × 明るさ + 加算」だけです
- テクスチャは 4×4 のタイル配置にして、キャッシュに当たりやすくしています
- M5GFX 標準の PSRAM フレームバッファは使いません。18 行ずつの帯を内部 SRAM に 2 コアで描き、DMA でパネルへ直接送ります
  （転送している間に次の帯を描きます）
- シリアルに fps と描画時間を出力します

## 画像のクレジット

ファームに埋め込んでいる地球のテクスチャは、NASA の画像を縮小・変換したものです。

| 用途 | 画像 | クレジット | 出典 |
|---|---|---|---|
| 昼の地表 | Blue Marble: Land Surface, Shallow Water, and Shaded Topography（`land_shallow_topo_2048.jpg`） | NASA Goddard Space Flight Center Image by Reto Stöckli. Enhancements by Robert Simmon. Data and technical support: MODIS Land Group; MODIS Science Data Support Team; MODIS Atmosphere Group; MODIS Ocean Group. Additional data: USGS EROS Data Center (topography); USGS Terrestrial Remote Sensing Flagstaff Field Center (Antarctica); Defense Meteorological Satellite Program (city lights). | [NASA Visible Earth](https://visibleearth.nasa.gov/images/57752/blue-marble-land-surface-shallow-water-and-shaded-topography) / [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Land_shallow_topo_2048.jpg) |
| 夜の灯り | Earth's City Lights（`Earthlights_dmsp.jpg`） | Data courtesy Marc Imhoff of NASA GSFC and Christopher Elvidge of NOAA NGDC. Image by Craig Mayhew and Robert Simmon, NASA GSFC. | [NASA Visible Earth](https://visibleearth.nasa.gov/images/55167/earths-city-lights) / [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Earthlights_dmsp.jpg) |

NASA が作成した画像は、原則として米国の著作権の対象外です。
NASA のメディア利用ガイドライン（https://www.nasa.gov/nasa-brand-center/images-and-media/ ）に沿って、次の点に注意してください。

- 出典として「Image: NASA」などのクレジットを表記する
- NASA のロゴ（インシグニア等）は使わない
- NASA が製品を推奨・協力しているように見せない

社外に配布・販売する製品に使う前に、上記の出典ページでライセンス表示を確認してください。
