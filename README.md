# tamatama

M5Stack StopWatch（C152）の画面に、1 分で 1 回転する地球を表示するプロジェクトです。
表面のガラス半球越しに、球が中に浮いているように見えることを狙っています。

## 構成

```
tamatama/
├── earth/   ファームウェア（PlatformIO）
└── case/    StopWatch 本体とケースの 3D データ
```

### earth/ — ファームウェア

| パス | 内容 |
|---|---|
| `platformio.ini` | ビルド設定（M5Unified / M5GFX、16MB Flash、OPI PSRAM） |
| `src/main.cpp` | 地球の描画プログラム |
| `tools/make_texture.py` | 地球の画像をファームに埋め込む形式へ変換するスクリプト |
| `assets/` | 元画像（NASA の昼の画像と夜の灯りの画像） |
| `data/` | 変換後のテクスチャ。ビルド時にファームへ埋め込まれる |

詳しい手順・調整方法・描画の仕組みは [earth/README.md](earth/README.md) を参照してください。

### case/ — 3D データ

| ファイル | 内容 | 外形寸法 (mm) |
|---|---|---|
| `StopWatch.stl` | StopWatch のモデル | 15.0 × 52.0 × 234.3 |
| `StopWatch_body.stl` | StopWatch 本体 | 15.0 × 52.0 × 52.0 |
| `StopWatch_case.stl` | ケース | 60.8 × 60.8 × 19.5 |
| `tamatama.sc` | SolidCraft の設計ファイル（`.bak` はそのバックアップ） | — |

## クイックスタート

必要なもの: M5Stack StopWatch、USB-C ケーブル、PlatformIO、macOS（テクスチャ変換に `sips` を使用）

```sh
cd earth

# 1. テクスチャを用意（初回のみ）
mkdir -p assets
curl -L -o assets/earth_day.jpg   "https://commons.wikimedia.org/wiki/Special:FilePath/Land_shallow_topo_2048.jpg"
curl -L -o assets/earth_night.jpg "https://commons.wikimedia.org/wiki/Special:FilePath/Earthlights_dmsp.jpg"
python3 tools/make_texture.py --day assets/earth_day.jpg --night assets/earth_night.jpg

# 2. ビルドして書き込む
pio run -t upload
pio device monitor   # fps などのログを確認
```

書き込みモードに入れない場合は、電源ボタンを約 2 秒押して緑 LED が点いたら離してから書き込んでください。

## 主な仕様

- 表示: 468×468（1.75 インチ円形 AMOLED、CO5300）
- 1 回転 60 秒、約 20fps
- 陰影は斜め前からの太陽光で付けています。海面の鏡面反射、縁の大気、外側の大気グロー、夜側の都市の灯りも描きます
- 本体を傾けると、IMU で地球が少し動き、背景の星との間に視差が出ます

## クレジット

地球のテクスチャには NASA の画像（Blue Marble、Earth's City Lights）を使っています。
クレジットの全文と利用上の注意は [earth/README.md の「画像のクレジット」](earth/README.md#画像のクレジット) を参照してください。
