# M5Stack StopWatch – 回転する地球

M5Stack StopWatch (C152, ESP32-S3 / 1.75" 466×466 AMOLED) に、1 分で 1 回転する地球を表示します。

## ガラス半球の表面に地表を見せる

画面の上に載せたガラス半球（直径 58.2mm）越しに見たとき、地表が半球の表面に貼り付いて見えるように描きます。

- **屈折の逆補正**：斜め上（既定は水平から 60°、画面の下辺側）からの視線が半球の表面で屈折して画面に届く経路を、起動時に画素ごとに逆算します（約 5 秒）。各画素には「その視線が半球に入った点」の地表を描くので、視点から見ると地球の表面と半球の表面が一致します
- **陰影**：斜めからの太陽光、明暗境界のぼかし、縁の大気の青み、海面の鏡面反射
- **夜側**：都市の灯り（雲の下は隠れる）
- **雲**：雲の層を地表より少し速く回し、太陽側へずらした位置の雲で地表に影を落とします
- 画面の外（ベゼル）が見える半球の手前下側は黒くなります。見える範囲は緯度で約 ±60° です

視点の位置が想定から大きくずれると、補正がずれて歪んで見えます。

## テクスチャの準備

NASA の画像を使います（クレジットは末尾の「画像のクレジット」を参照）。

```sh
mkdir -p assets
curl -L -o assets/earth_day.jpg   "https://commons.wikimedia.org/wiki/Special:FilePath/Land_shallow_topo_2048.jpg"
curl -L -o assets/earth_night.jpg "https://commons.wikimedia.org/wiki/Special:FilePath/Earthlights_dmsp.jpg"
curl -L -o assets/earth_clouds_composite.jpg "https://upload.wikimedia.org/wikipedia/commons/6/6b/Land_ocean_ice_cloud_hires.jpg"
python3 tools/make_texture.py --day assets/earth_day.jpg --night assets/earth_night.jpg \
                              --clouds assets/earth_clouds_composite.jpg
```

雲は、雲入りの合成画像（Land_ocean_ice_cloud）と雲なしの昼の画像（Land_shallow_topo）の白さの差から取り出しています。
どちらも同じ Blue Marble の画像なので位置が揃っています。`--clouds` を省くと雲なしになります。
雲は 2002 年の実際の雲の 1 枚の画像で、その模様全体が地表より少し速く回ります。

`tools/make_texture.py` 冒頭の `LAND_GAMMA` / `LAND_KNEE` / `LAND_HIGHLIGHT` で陸地の明るさを調整できます（暗い森林を持ち上げ、明るい砂漠を抑える）。

NASA のサーバー（eoimages.gsfc.nasa.gov）がつながる場合は、次の URL から取得しても同じ画像です。

- 昼: https://eoimages.gsfc.nasa.gov/images/imagerecords/57000/57752/land_shallow_topo_2048.jpg
- 夜: https://eoimages.gsfc.nasa.gov/images/imagerecords/55000/55167/earth_lights_lrg.jpg

正距円筒図法（横:縦 = 2:1）の画像であれば、ほかの画像も使えます。変換には macOS の `sips` を使います。
`python3 tools/make_texture.py --placeholder` を実行すると、手続き生成の仮の大陸で動作を確認できます。

出力ファイル:

- `data/earth_day.rgb565`：1024×512、RGB565
- `data/earth_night.gray8`：1024×512、8 bit 輝度
- `data/earth_cloud.gray8`：1024×512、8 bit 雲の濃さ

いずれもビルド時にファームへ埋め込まれ、起動時に PSRAM へコピーされます。

## ビルドと書き込み

```sh
pio run -t upload && pio device monitor
```

書き込みモードに入れない場合は、電源ボタンを約 2 秒押して緑 LED が点いたら離してから書き込んでください。

## 調整 (`src/main.cpp` 冒頭)

| 定数 | 意味 |
|---|---|
| `ROTATION_US` | 地表 1 回転の時間（初期値 60 秒） |
| `NOON_LONGITUDE_DEG` | 起動した瞬間に正午（太陽の正面）になる経度。既定 139.7 = 日本。以後 1 回転ごとに正午 |
| `CLOUD_ROTATION_US` | 雲 1 回転の時間（初期値 52 秒。地表より速く動き、雲が浮いて見える） |
| `DOME_DIAMETER_MM` | ガラス半球の直径（58.2mm） |
| `DISPLAY_DIAMETER_MM` | 画面の表示部の直径（43.76mm、パネル仕様の値） |
| `GLASS_N` | 半球の屈折率（中実のガラスを想定して 1.50） |
| `PIXEL_DEPTH_MM` | 半球の底面から画素までの深さ（画面のカバーガラス分） |
| `VIEW_ELEVATION_DEG` | 視点の仰角。水平から何度で見下ろすか（90 = 真上、既定 60） |
| `SUN_X/Y/Z` | 太陽の方向（視点から見て）。Z を小さくすると欠けが大きくなり、立体感が強まる |
| `AXIS_TILT_DEG` | 地軸を右へ傾ける角度 |
| `AXIS_TOWARD_DEG` | 北極を手前へ倒す角度。負で南極が手前（既定 25 = 北半球中心。見える範囲は緯度で約 ±60°） |
| `CLOUD_ALTITUDE` | 雲の高さ。大きいほど影が雲から離れる |
| `CLOUD_SHADOW` | 雲の影の濃さ（0..255） |
| `CLOUD_RELIEF` | 雲の凹凸の陰影（0 = なし。上げると雲が細切れに見えやすい） |
| `CLEAR_LAT_DEG` / `CLEAR_LON_DEG` | 晴れ間の中心（既定は日本）。地表に固定した範囲で雲と雲の影を薄くする |
| `CLEAR_INNER_DEG` / `CLEAR_OUTER_DEG` / `CLEAR_MIN` | 晴れ間の大きさと、中心で雲を残す割合（`CLEAR_MIN = 1` で晴れ間なし） |
| `EXPOSURE` | 地表の明るさの倍率（最大 2 倍、既定 1.45） |
| `AMBIENT` | 日陰側にも残す明るさ |
| `BRIGHTNESS` | 画面の輝度（0..255） |
| `INTERLACE` | 2 行おきに半分ずつ描き直す（false で毎フレーム全行） |

## 描画の仕組み

約 20 回/秒 画面を更新します（インターレースで、1 回に半分の行を描き直します）。

- 起動時に、各画素の緯度・経度、静的な陰影（明るさ、大気、鏡面/夜景の重み）、雲の影のずれを 8 バイトのテーブルにまとめます（PSRAM）
- 自転は経度にオフセットを足すだけなので、毎フレームの三角関数は不要です
- 昼の色・夜景・雲を 1 テクセル 32bit にまとめ、4×4 のタイル配置にしてキャッシュに当たりやすくしています
- 雲の影は、内部 RAM に置いた 1/4 の縮小マップから読みます
- M5GFX 標準の PSRAM フレームバッファは使いません。帯ごとに内部 SRAM に 2 コアで描き、DMA でパネルへ直接送ります
- シリアルに fps と描画時間を出力します

## 画像のクレジット

ファームに埋め込んでいる地球のテクスチャは、NASA の画像を縮小・変換したものです。

| 用途 | 画像 | クレジット | 出典 |
|---|---|---|---|
| 昼の地表 | Blue Marble: Land Surface, Shallow Water, and Shaded Topography（`land_shallow_topo_2048.jpg`） | NASA Goddard Space Flight Center Image by Reto Stöckli. Enhancements by Robert Simmon. Data and technical support: MODIS Land Group; MODIS Science Data Support Team; MODIS Atmosphere Group; MODIS Ocean Group. Additional data: USGS EROS Data Center (topography); USGS Terrestrial Remote Sensing Flagstaff Field Center (Antarctica); Defense Meteorological Satellite Program (city lights). | [NASA Visible Earth](https://visibleearth.nasa.gov/images/57752/blue-marble-land-surface-shallow-water-and-shaded-topography) / [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Land_shallow_topo_2048.jpg) |
| 雲 | The Blue Marble: Land Surface, Ocean Color, Sea Ice and Clouds（`Land_ocean_ice_cloud_hires.jpg`。雲だけを取り出して使用） | NASA Goddard Space Flight Center Image by Reto Stöckli. Enhancements by Robert Simmon. | [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Land_ocean_ice_cloud_hires.jpg) |
| 夜の灯り | Earth's City Lights（`Earthlights_dmsp.jpg`） | Data courtesy Marc Imhoff of NASA GSFC and Christopher Elvidge of NOAA NGDC. Image by Craig Mayhew and Robert Simmon, NASA GSFC. | [NASA Visible Earth](https://visibleearth.nasa.gov/images/55167/earths-city-lights) / [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Earthlights_dmsp.jpg) |

NASA が作成した画像は、原則として米国の著作権の対象外です。
NASA のメディア利用ガイドライン（https://www.nasa.gov/nasa-brand-center/images-and-media/ ）に沿って、次の点に注意してください。

- 出典として「Image: NASA」などのクレジットを表記する
- NASA のロゴ（インシグニア等）は使わない
- NASA が製品を推奨・協力しているように見せない

社外に配布・販売する製品に使う前に、上記の出典ページでライセンス表示を確認してください。
