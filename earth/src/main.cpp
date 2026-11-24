// M5Stack StopWatch : 1 分で 1 回転する地球
//
// 画面に載せたガラス半球 (直径 58.2mm) の「表面」に地表が見えるように描く。
//
//  - 斜め上 (VIEW_ELEVATION_DEG) から見た視線が半球の表面で屈折して画面に届く経路を
//    起動時に逆算し、各画素に「その視線が半球に入った点」の地表を描く。
//    → 視点から見ると、地球の表面が半球の表面と一致して見える。
//  - 陰影: 斜めからの太陽光、明暗境界、縁の大気、海面の鏡面反射、夜側の都市の灯り
//  - 雲の層: 地表より少し速く回り、地表に影を落とす (太陽側へずらした位置の雲で地表を暗くする)
//
// 高速化:
//  - 各画素の緯度・経度・静的な陰影・雲の影のずれを 8 バイトのテーブルに事前計算 (PSRAM)
//    自転は経度にオフセットを足すだけなので、毎フレームの三角関数は不要
//  - テクスチャは 4x4 タイル配置にしてキャッシュに乗りやすくする
//  - 18 行ずつの帯を内部 RAM で 2 コア並列に描き、DMA 転送と並行して次の帯を描く
//    (M5GFX 標準の PSRAM フレームバッファは経由しない)

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <math.h>
#include <algorithm>
#include <atomic>
#include <lgfx/v1/panel/Panel_CO5300.hpp>

// ---------------- 設定 ----------------
static constexpr uint32_t ROTATION_US       = 60u * 1000u * 1000u;  // 地表 1 回転の時間
static constexpr uint32_t CLOUD_ROTATION_US = 52u * 1000u * 1000u;  // 雲 1 回転の時間 (地表より速く動き、高さが分かる)

// ガラス半球と画面
static constexpr float DOME_DIAMETER_MM    = 58.2f;   // 半球の直径
static constexpr float DISPLAY_DIAMETER_MM = 43.76f;  // 画面の表示部の直径 (466px)
static constexpr float GLASS_N             = 1.50f;   // 半球の屈折率 (中実のガラスを想定)
static constexpr float PIXEL_DEPTH_MM      = 0.7f;    // 半球の底面から画素までの深さ (画面のカバーガラス)

// 視点: 画面の下辺側から、水平に対して VIEW_ELEVATION_DEG の角度で見下ろす (90 = 真上)
static constexpr float VIEW_ELEVATION_DEG  = 60.0f;

// 地球の向き (視点から見て): 地軸を右へ傾ける角度と、北極を手前へ倒す角度
static constexpr float AXIS_TILT_DEG   = 23.4f;
static constexpr float AXIS_TOWARD_DEG = 25.0f;   // 正 = 北極を手前へ (北半球中心), 負 = 南極を手前へ
// 太陽方向 (視点から見て x:右, y:上, z:手前)
static constexpr float SUN_X = -0.80f, SUN_Y = 0.35f, SUN_Z = 0.45f;

static constexpr float CLOUD_ALTITUDE = 0.015f;  // 雲の高さ (地球半径比。影のずれの大きさ、誇張)
static constexpr int   CLOUD_SHADOW   = 150;     // 雲の影の濃さ (0..255)
static constexpr int   CLOUD_RELIEF   = 0;       // 雲の凹凸の陰影の強さ (0 = なし。上げると太陽側の縁が明るく、雲が細切れに見えやすい)

// 起動した瞬間にこの経度が正午 (太陽の正面) になるよう回転の開始位置を合わせる。以後 1 回転ごとに正午
static constexpr float NOON_LONGITUDE_DEG = 139.7f;   // 東経 139.7° (日本・東京)

// 晴れ間: 地表に固定した範囲で雲と雲の影を薄くする (日本が雲に隠れ続けないように)
static constexpr float CLEAR_LAT_DEG = 36.0f, CLEAR_LON_DEG = 138.0f;  // 中心 (日本)
static constexpr float CLEAR_INNER_DEG = 8.0f;    // この半径までは最も薄く
static constexpr float CLEAR_OUTER_DEG = 18.0f;   // この半径から外は通常どおり
static constexpr float CLEAR_MIN = 0.15f;         // 中心での雲の残す割合 (1 = 晴れ間なし)

static constexpr float EXPOSURE   = 1.45f;  // 地表の明るさの倍率 (最大 2 倍まで)
static constexpr float AMBIENT    = 0.10f;  // 日陰側にも残す明るさ
static constexpr int   BRIGHTNESS = 255;    // 画面の輝度 (0..255)

// ---------------- テクスチャ ----------------
static constexpr int TEX_W = 1024, TEX_H = 512;
extern const uint8_t day_start[]   asm("_binary_data_earth_day_rgb565_start");
extern const uint8_t day_end[]     asm("_binary_data_earth_day_rgb565_end");
extern const uint8_t night_start[] asm("_binary_data_earth_night_gray8_start");
extern const uint8_t night_end[]   asm("_binary_data_earth_night_gray8_end");
extern const uint8_t cloud_start[] asm("_binary_data_earth_cloud_gray8_start");
extern const uint8_t cloud_end[]   asm("_binary_data_earth_cloud_gray8_end");

// PSRAM, 4x4 タイル配置
static uint16_t* texDay   = nullptr;   // 昼の色 (RGB565)
static uint8_t*  texNight = nullptr;   // 夜景
static uint8_t*  texCloud = nullptr;   // 雲 (地表とは別の速さで回す)
// 雲の影・陰影用の縮小マップ (1/2, ぼかし済み)。内部 RAM に置いて PSRAM を読まずに済ませる
static constexpr int SHD_SHIFT = 1;
static constexpr int SHD_W = TEX_W >> SHD_SHIFT, SHD_H = TEX_H >> SHD_SHIFT;
static uint8_t* shadowMap = nullptr;

// 晴れ間の係数 (地表に固定, 1/8 解像度, 0..256)。内部 RAM
static constexpr int CLR_W = TEX_W / 8, CLR_H = TEX_H / 8;
static uint16_t* clearMap = nullptr;

// 縮小マップを読む (u: 0..1023, v: 0..511 のテクセル座標)
static inline int shadowAt(uint32_t u, int v) {
  return shadowMap[(v >> SHD_SHIFT) * SHD_W + (u >> SHD_SHIFT)];
}

// (u:0..1023, v:0..511) → 4x4 タイル配置での添字
static inline uint32_t tileIndex(uint32_t u, uint32_t v) {
  return ((v >> 2) << 12) | ((u >> 2) << 4) | ((v & 3) << 2) | (u & 3);
}

// ---------------- 事前計算テーブル ----------------
// 毎フレームの計算は「テクスチャ × L + 加算色」と、雲・影・鏡面・夜景だけ。
struct Px {
  uint16_t u;    // 経度 (65536 = 一周)
  uint16_t vf;   // [8:0] 緯度の行, [13:9] 縁の大気の強さ, bit14 = 夜側, bit15 = 見えない (黒)
  uint8_t  L;    // 明るさ (128 = 等倍, 255 = 約 2 倍)
  uint8_t  sw;   // 昼側: 鏡面の強さ / 夜側: 夜景の重み
  int8_t   du;   // 雲の影を探す位置のずれ (テクセル)
  int8_t   dv;
};
static constexpr uint16_t VF_NIGHT = 1u << 14, VF_BLANK = 1u << 15;
static uint32_t addTab[32];  // 縁の大気の加算色 (R5,G6,B5 を 10bit ずつ)
static inline uint32_t packAdd(int r5, int g6, int b5) { return (r5 << 20) | (g6 << 10) | b5; }
struct Row { int16_t x0; int16_t len; uint32_t ofs; };

static Px*  lut  = nullptr;
static Row* rows = nullptr;
static int  RD = 0;          // 描画する円の半径 (px)
static uint16_t rotOffset = 0;  // 回転の開始位置 (NOON_LONGITUDE_DEG が正午になる値)

// 描画は内部 SRAM の帯バッファ (BAND_H 行) 2 枚で行い、DMA で直接パネルへ送る。
static constexpr int BAND_H = 18;     // 1 回に描く行数 (偶数。CO5300 は偶数境界での転送が必要)
// インターレース: 毎フレーム 2 行おきに半分の行だけ描き直す (2 行単位は CO5300 の制約)。
// 自転の動きは 1 フレーム 1px 程度なので段差はほぼ見えず、更新回数はほぼ倍になる。
static constexpr bool INTERLACE = true;
static uint16_t* band[2] = { nullptr, nullptr };
static int SW = 0, SH = 0;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float smooth(float e0, float e1, float x) {
  float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}
static inline uint8_t u8f(float v) { return (uint8_t)(clampf(v, 0.f, 1.f) * 255.f + 0.5f); }

struct V3 { float x, y, z; };
static inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static inline V3 norm(V3 a) { return a * (1.f / sqrtf(dot(a, a))); }

// ---- 半球の光学 ----
// 座標 (mm): 原点 = 半球の底面の中心, x = 画面の右, y = 画面の上, z = 画面から手前
static const float DOME_R = DOME_DIAMETER_MM * 0.5f;
static V3 viewDir;   // 視点の方向 (半球から目へ向かう単位ベクトル)

// 半球の表面の点 (qx, qy) に視線が入ったとき、屈折して画素面に届く点 (px, py) を返す
static bool traceDome(float qx, float qy, float& px, float& py) {
  float qz2 = DOME_R * DOME_R - qx * qx - qy * qy;
  if (qz2 <= 0.f) return false;
  V3 q = { qx, qy, sqrtf(qz2) };
  V3 n = q * (1.f / DOME_R);
  V3 d = viewDir * -1.f;            // 入射方向
  float cosi = -dot(d, n);
  if (cosi <= 0.02f) return false;  // 視点から見えない面
  float eta = 1.f / GLASS_N;
  float k = 1.f - eta * eta * (1.f - cosi * cosi);
  V3 t = d * eta + n * (eta * cosi - sqrtf(k));
  if (t.z >= -1e-4f) return false;
  float lam = (q.z + PIXEL_DEPTH_MM) / -t.z;
  px = q.x + lam * t.x;
  py = q.y + lam * t.y;
  return true;
}

// 画素面の点 (tx, ty) に届く視線が半球に入った点を、ニュートン法で求める
static bool solveDome(float tx, float ty, float& qx, float& qy) {
  const float eps = 0.01f;
  for (int it = 0; it < 12; ++it) {
    float px, py, ax, ay, bx, by;
    if (!traceDome(qx, qy, px, py)) return false;
    float ex = px - tx, ey = py - ty;
    if (ex * ex + ey * ey < 1e-6f) return true;
    if (!traceDome(qx + eps, qy, ax, ay) || !traceDome(qx, qy + eps, bx, by)) return false;
    float j11 = (ax - px) / eps, j21 = (ay - py) / eps, j12 = (bx - px) / eps, j22 = (by - py) / eps;
    float det = j11 * j22 - j12 * j21;
    if (fabsf(det) < 1e-6f) return false;
    float sx = ( j22 * ex - j12 * ey) / det;
    float sy = (-j21 * ex + j11 * ey) / det;
    float s = sqrtf(sx * sx + sy * sy), maxStep = DOME_R * 0.2f;
    if (s > maxStep) { sx *= maxStep / s; sy *= maxStep / s; }
    qx -= sx; qy -= sy;
    float r2 = qx * qx + qy * qy, lim = DOME_R * 0.999f;
    if (r2 > lim * lim) { float k = lim / sqrtf(r2); qx *= k; qy *= k; }
  }
  return false;
}

static void buildLut() {
  const float el = VIEW_ELEVATION_DEG * (float)M_PI / 180.f;
  viewDir = { 0.f, -cosf(el), sinf(el) };                  // 画面の下辺側から見る
  // 視点から見た基底 (右, 上, 手前)
  const V3 vR = { 1.f, 0.f, 0.f };
  const V3 vZ = viewDir;
  const V3 vU = cross(vZ, vR);

  // 地軸 a と経度の基準 e0 (正面), e1 (東) — 視点の基底で表す
  const float tz = AXIS_TILT_DEG * (float)M_PI / 180.f;
  const float tx = AXIS_TOWARD_DEG * (float)M_PI / 180.f;
  const float ax = sinf(tz) * cosf(tx), ay = cosf(tz) * cosf(tx), az = sinf(tx);
  V3 A = { ax, ay, az };
  V3 E0 = norm(V3{ 0.f, 0.f, 1.f } + A * -az);
  V3 E1 = cross(A, E0);

  V3 Ls = norm({ SUN_X, SUN_Y, SUN_Z });
  V3 H  = norm(Ls + V3{ 0.f, 0.f, 1.f });

  for (int q = 0; q < 32; ++q) {
    float a = q / 31.f;
    addTab[q] = packAdd((int)(a * 80 / 8), (int)(a * 150 / 4), (int)(a * 255 / 8));
  }

  auto lonLat = [&](V3 n, float& u16, float& vrow) {
    float lat = asinf(clampf(dot(n, A), -1.f, 1.f));
    float lon = atan2f(dot(n, E1), dot(n, E0));
    u16  = (lon + (float)M_PI) / (2.f * (float)M_PI) * 65536.f;
    vrow = ((float)M_PI * 0.5f - lat) / (float)M_PI * TEX_H;
  };

  // 太陽の真下の点の経度 (画面上の位置) と、正午にしたい経度の差 = 回転の開始位置
  {
    float uSun, vSun;
    lonLat(Ls, uSun, vSun);
    float uNoon = (NOON_LONGITUDE_DEG + 180.f) / 360.f * 65536.f;
    rotOffset = (uint16_t)(int32_t)(uSun - uNoon);
  }

  const float pitch = DISPLAY_DIAMETER_MM / SW;   // mm/px
  RD = SW / 2;
  const int nRows = RD * 2;
  rows = (Row*)heap_caps_malloc(sizeof(Row) * nRows, MALLOC_CAP_INTERNAL);
  size_t total = 0;
  for (int j = 0; j < nRows; ++j) {
    float dy = j - RD + 0.5f;
    int w = (int)sqrtf(fmaxf(0.f, (float)RD * RD - dy * dy));
    rows[j] = { (int16_t)-w, (int16_t)(w * 2), (uint32_t)total };
    total += w * 2;
  }
  lut = (Px*)heap_caps_malloc(sizeof(Px) * total, MALLOC_CAP_SPIRAM);
  Serial.printf("LUT %u px, %u KB\n", (unsigned)total, (unsigned)(sizeof(Px) * total / 1024));

  uint32_t t0 = millis(), blank = 0;
  for (int j = 0; j < nRows; ++j) {
    Px* p = lut + rows[j].ofs;
    bool have = false;
    float qx = 0, qy = 0;
    for (int i = 0; i < rows[j].len; ++i, ++p) {
      float sx = rows[j].x0 + i + 0.5f, sy = j - RD + 0.5f;   // 画面座標 (px, 下が正)
      float tx = sx * pitch, ty = -sy * pitch;                  // 画素面 (mm, 上が正)
      // 初期値: 前の画素の解、なければ真上から見た近似 (中心は約 1.5 倍に拡大)
      bool ok = false;
      for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
        if (!have || attempt > 0) {
          float k = attempt == 2 ? 1.0f : 1.35f;
          qx = tx * k; qy = ty * k + (attempt == 1 ? DOME_R * 0.15f : 0.f);
        }
        ok = solveDome(tx, ty, qx, qy);
        if (!ok) have = false;
      }
      if (!ok) { *p = { 0, VF_BLANK, 0, 0, 0, 0 }; ++blank; continue; }
      have = true;

      // 半球の表面の法線 = 地表の法線 → 視点の基底へ
      V3 nd = norm({ qx, qy, sqrtf(fmaxf(0.f, DOME_R * DOME_R - qx * qx - qy * qy)) });
      V3 n  = { dot(nd, vR), dot(nd, vU), dot(nd, vZ) };

      float uf, vfF;
      lonLat(n, uf, vfF);
      uint16_t u = (uint16_t)(int32_t)uf;
      int v = (int)vfF;
      v = v < 0 ? 0 : (v >= TEX_H ? TEX_H - 1 : v);

      float ndl  = dot(n, Ls);
      float nv   = clampf(n.z, 0.f, 1.f);                       // 視線との角度
      float dayT = smooth(-0.08f, 0.12f, ndl);
      float lam  = fmaxf(ndl, 0.f);
      float light = (0.03f + dayT * (AMBIENT + (1.f - AMBIENT) * powf(lam, 0.75f))) * EXPOSURE;
      float night = (1.f - smooth(-0.15f, 0.03f, ndl)) * (0.5f + 0.5f * nv);
      float ndh  = fmaxf(dot(n, H), 0.f);
      float spec = (0.80f * powf(ndh, 80.f) + 0.10f * powf(ndh, 8.f)) * dayT;
      float rim  = powf(1.f - nv, 2.5f) * 0.95f * smooth(-0.25f, 0.30f, ndl);

      // 画面の縁は暗くぼかす (表示部の境目を目立たせない)
      float edge = smooth(1.0f, 0.95f, sqrtf(sx * sx + sy * sy) / RD);
      float L = clampf(light, 0.f, 2.f) * (1.f - rim) * edge;
      rim *= edge; night *= edge; spec *= edge;

      // 雲の影: 太陽の方へ地表に沿って進んだ位置の雲が影を落とす
      int8_t du = 0, dv = 0;
      V3 ts = Ls + n * -ndl;                                     // 太陽方向の接線成分
      float tl = sqrtf(dot(ts, ts));
      if (ndl > 0.f && tl > 1e-4f) {
        float ang = clampf(CLOUD_ALTITUDE * tl / fmaxf(ndl, 0.15f), 0.f, 0.15f);
        V3 n2 = norm(n + ts * (ang / tl));
        float u2, v2;
        lonLat(n2, u2, v2);
        float ddu = (u2 - uf) / 64.f;                            // テクセル
        if (ddu > 512.f) ddu -= 1024.f;
        if (ddu < -512.f) ddu += 1024.f;
        du = (int8_t)clampf(roundf(ddu), -127.f, 127.f);
        dv = (int8_t)clampf(roundf(v2 - vfF), -127.f, 127.f);
      }

      bool nightSide = night > spec;
      uint16_t q = (uint16_t)(clampf(rim, 0.f, 1.f) * 31.f + 0.5f);
      uint16_t vf = (uint16_t)v | (q << 9) | (nightSide ? VF_NIGHT : 0);
      *p = { u, vf, (uint8_t)fminf(255.f, L * 128.f), u8f(nightSide ? night : spec), du, dv };
    }
  }
  Serial.printf("dome mapping %u ms, %u px not visible\n", (unsigned)(millis() - t0), (unsigned)blank);
}

// ---------------- 描画 ----------------
struct Frame { uint16_t rot, rotC; };
struct Job { Frame f; uint16_t* buf; int start, count, yEnd; };
static volatile Job job;
static std::atomic<int> nextRow;   // 2 コアで行を取り合う (負荷の偏りをなくす)

// 帯の k 行目 (バッファ内) が画面の何行目か
static inline int rowOf(int start, int k) {
  return INTERLACE ? start + 4 * (k >> 1) + (k & 1) : start + k;
}

static inline uint16_t swap16(uint32_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

// 帯の行 (バッファ内 0..count-1) を nextRow から取って描く (5/6/5 ビットのまま計算)
static void IRAM_ATTR renderRows(const Frame& f, uint16_t* buf, int start, int count, int yEnd) {
  const uint16_t rot = f.rot, rotC = f.rotC;
  for (int k; (k = nextRow.fetch_add(1, std::memory_order_relaxed)) < count; ) {
    const int y = rowOf(start, k);
    if (y >= yEnd) continue;
    uint16_t* line = buf + k * SW;
    memset(line, 0, SW * 2);

    int j = y - (SH / 2 - RD);
    if (j < 0 || j >= RD * 2) continue;
    const Row& rw = rows[j];
    const Px* p = lut + rw.ofs;
    uint16_t* dst = line + SW / 2 + rw.x0;

    for (int i = 0; i < rw.len; ++i, ++p, ++dst) {
      const uint32_t vf = p->vf;
      if (vf & VF_BLANK) continue;
      const uint32_t v = vf & 511;

      const uint32_t ug = (uint16_t)(p->u - rot) >> 6;    // 地表の経度 (テクセル)
      const uint32_t ti = tileIndex(ug, v);
      const int clr = clearMap[(v >> 3) * CLR_W + (ug >> 3)];   // 晴れ間 (256 = 通常)
      const uint32_t t = texDay[ti];
      const int r = t >> 11, g = (t >> 5) & 63, b = t & 31;
      const int L = p->L;
      int sr = (r * L) >> 7, sg = (g * L) >> 7, sb = (b * L) >> 7;

      const uint32_t uc = (uint16_t)(p->u - rotC) >> 6;    // 雲の経度 (地表より速く回る)
      const int a = (texCloud[tileIndex(uc, v)] * clr) >> 8;   // 雲の濃さ
      const int sw = p->sw;
      int s = 0;                                           // 太陽側の雲の濃さ (影と雲の陰影に使う)

      if (vf & VF_NIGHT) {
        if (sw) {                                          // 夜景 (雲があると隠れる)
          int n = (texNight[ti] * sw * (255 - a)) >> 16;   // 0..255
          sr += n >> 3; sg += (n * 3) >> 4; sb += (n * 3) >> 6;
        }
      } else {
        // 雲の影 (縮小マップから): 太陽の方向へずらした位置の雲
        int sv = (int)v + p->dv;
        sv = sv < 0 ? 0 : (sv > TEX_H - 1 ? TEX_H - 1 : sv);
        s = (shadowAt((uc + p->du) & (TEX_W - 1), sv) * clr) >> 8;
        const int k = 256 - ((s * CLOUD_SHADOW) >> 8);
        sr = (sr * k) >> 8; sg = (sg * k) >> 8; sb = (sb * k) >> 8;
        if (sw && b > r + 2 && b * 2 >= g) {               // 海だけ鏡面反射 (雲の下は弱める)
          int s2 = (sw * (255 - a)) >> 8;
          sr += (s2 * 25) >> 8; sg += (s2 * 53) >> 8; sb += (s2 * 29) >> 8;
        }
      }
      // 雲: 白く光る。濃さを高さとみなし、太陽側に面した所を明るく、反対側を暗くして厚みを出す
      if (a) {
        int cL = L + (L >> 3);                             // 128 = 等倍
        if (CLOUD_RELIEF && !(vf & VF_NIGHT)) {
          // ぼかした雲の濃さで比べる: 自分の位置 (sc) より太陽側 (s) が薄ければ太陽に面した側
          const int sc = shadowAt(uc, v);
          cL += (L * (sc - s) * CLOUD_RELIEF) >> 15;
          cL = cL < (L >> 2) ? (L >> 2) : cL;
        }
        if (cL > 128) cL = 128 + ((cL - 128) >> 2);        // 雲は白飛びしにくく
        int cr = (31 * cL) >> 7, cg = (63 * cL) >> 7, cb = (31 * cL) >> 7;
        cr = cr > 31 ? 31 : cr; cg = cg > 63 ? 63 : cg; cb = cb > 31 ? 31 : cb;
        sr += ((cr - sr) * a) >> 8; sg += ((cg - sg) * a) >> 8; sb += ((cb - sb) * a) >> 8;
      }
      // 縁の大気
      const uint32_t ad = addTab[(vf >> 9) & 31];
      sr += ad >> 20; sg += (ad >> 10) & 1023; sb += ad & 1023;

      sr = sr > 31 ? 31 : (sr < 0 ? 0 : sr);
      sg = sg > 63 ? 63 : (sg < 0 ? 0 : sg);
      sb = sb > 31 ? 31 : (sb < 0 ? 0 : sb);
      *dst = swap16((sr << 11) | (sg << 5) | sb);
    }
  }
}

static SemaphoreHandle_t semStart, semDone;

static void workerTask(void*) {
  for (;;) {
    xSemaphoreTake(semStart, portMAX_DELAY);
    Frame f = { job.f.rot, job.f.rotC };
    renderRows(f, job.buf, job.start, job.count, job.yEnd);
    xSemaphoreGive(semDone);
  }
}

// M5GFX は StopWatch で PSRAM フレームバッファ付きパネルを使う。
// 帯ごとに直接転送したいので、その内側の実パネルに差し替える。
struct FbPanelAccess : lgfx::Panel_AMOLED_Framebuffer {
  static lgfx::Panel_AMOLED* raw(lgfx::Panel_AMOLED_Framebuffer* p) {
    return p->*(&FbPanelAccess::_panel);
  }
};

static void useDirectPanel() {
  if (M5.getBoard() != m5::board_t::board_M5StopWatch) return;
  auto fbp = static_cast<lgfx::Panel_AMOLED_Framebuffer*>(M5.Display.getPanel());
  auto raw = FbPanelAccess::raw(fbp);
  if (!raw) return;
  M5.Display.setPanel(raw);
  Serial.println("direct panel");
}

static void loadTextures() {
  // フラッシュ上の埋め込みデータ (行順) を PSRAM の 4x4 タイル配置へ
  const uint16_t* d = (const uint16_t*)day_start;
  const size_t nd = (day_end - day_start) / 2, nn = night_end - night_start, nc = cloud_end - cloud_start;
  for (uint32_t v = 0; v < (uint32_t)TEX_H; ++v) {
    for (uint32_t u = 0; u < (uint32_t)TEX_W; ++u) {
      size_t s = (size_t)v * TEX_W + u;
      const uint32_t t = tileIndex(u, v);
      texDay[t]   = s < nd ? d[s] : 0;
      texNight[t] = s < nn ? night_start[s] : 0;
      texCloud[t] = s < nc ? cloud_start[s] : 0;
    }
  }
}

static void buildShadowMap() {
  // 縮小 (平均) → 3x3 のぼかしを 2 回
  const int F = 1 << SHD_SHIFT;
  uint8_t* tmp = (uint8_t*)heap_caps_malloc(SHD_W * SHD_H, MALLOC_CAP_SPIRAM);
  const size_t nc = cloud_end - cloud_start;
  for (int y = 0; y < SHD_H; ++y)
    for (int x = 0; x < SHD_W; ++x) {
      int sum = 0;
      for (int j = 0; j < F; ++j)
        for (int i = 0; i < F; ++i) {
          size_t s = (size_t)(y * F + j) * TEX_W + x * F + i;
          sum += s < nc ? cloud_start[s] : 0;
        }
      shadowMap[y * SHD_W + x] = sum / (F * F);
    }
  for (int pass = 0; pass < 2; ++pass) {
    memcpy(tmp, shadowMap, SHD_W * SHD_H);
    for (int y = 0; y < SHD_H; ++y)
      for (int x = 0; x < SHD_W; ++x) {
        int sum = 0;
        for (int j = -1; j <= 1; ++j)
          for (int i = -1; i <= 1; ++i) {
            int yy = std::min(std::max(y + j, 0), SHD_H - 1), xx = (x + i + SHD_W) % SHD_W;
            sum += tmp[yy * SHD_W + xx];
          }
        shadowMap[y * SHD_W + x] = sum / 9;
      }
  }
  heap_caps_free(tmp);
}

static void buildClearMap() {
  const float d2r = (float)M_PI / 180.f;
  const float cl = CLEAR_LAT_DEG * d2r, co = CLEAR_LON_DEG * d2r;
  for (int y = 0; y < CLR_H; ++y)
    for (int x = 0; x < CLR_W; ++x) {
      float lat = (90.f - (y + 0.5f) * 180.f / CLR_H) * d2r;
      float lon = ((x + 0.5f) * 360.f / CLR_W - 180.f) * d2r;
      float c = sinf(lat) * sinf(cl) + cosf(lat) * cosf(cl) * cosf(lon - co);
      float dist = acosf(clampf(c, -1.f, 1.f)) / d2r;           // 中心からの角度
      float k = CLEAR_MIN + (1.f - CLEAR_MIN) * smooth(CLEAR_INNER_DEG, CLEAR_OUTER_DEG, dist);
      clearMap[y * CLR_W + x] = (uint16_t)(k * 256.f + 0.5f);
    }
}

// ---------------- setup / loop ----------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.display();
  useDirectPanel();
  M5.Display.setBrightness(BRIGHTNESS);
  M5.Display.fillScreen(TFT_BLACK);

  SW = M5.Display.width();
  SH = M5.Display.height();
  Serial.printf("display %dx%d\n", SW, SH);

  texDay   = (uint16_t*)heap_caps_malloc(TEX_W * TEX_H * 2, MALLOC_CAP_SPIRAM);
  texNight = (uint8_t*)heap_caps_malloc(TEX_W * TEX_H, MALLOC_CAP_SPIRAM);
  texCloud = (uint8_t*)heap_caps_malloc(TEX_W * TEX_H, MALLOC_CAP_SPIRAM);
  shadowMap = (uint8_t*)heap_caps_malloc(SHD_W * SHD_H, MALLOC_CAP_INTERNAL);
  clearMap  = (uint16_t*)heap_caps_malloc(CLR_W * CLR_H * 2, MALLOC_CAP_INTERNAL);
  for (int i = 0; i < 2; ++i)
    band[i] = (uint16_t*)heap_caps_malloc(SW * BAND_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!texDay || !texNight || !texCloud || !shadowMap || !clearMap || !band[0] || !band[1]) {
    M5.Display.drawString("alloc failed", 10, SH / 2);
    for (;;) delay(1000);
  }

  loadTextures();
  buildShadowMap();
  buildClearMap();
  buildLut();

  semStart = xSemaphoreCreateBinary();
  semDone = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(workerTask, "render0", 4096, nullptr, 5, nullptr, 0);
}

void loop() {
  static uint32_t frames = 0, t0 = millis();
  static uint64_t tRen = 0;

  uint64_t now = esp_timer_get_time();
  Frame f;
  f.rot  = (uint16_t)(rotOffset + (((now % ROTATION_US) << 16) / ROTATION_US));
  f.rotC = (uint16_t)(rotOffset + (((now % CLOUD_ROTATION_US) << 16) / CLOUD_ROTATION_US));

  static int phase = 0;
  if (INTERLACE) phase ^= 1;
  const int span = INTERLACE ? BAND_H * 2 : BAND_H;   // 1 帯が覆う画面の行数

  M5.Display.startWrite();
  int bi = 0;
  for (int y = 0; y < SH; y += span, bi ^= 1) {
    const int yEnd = std::min(y + span, SH);
    const int start = y + (INTERLACE ? 2 * phase : 0);
    const int count = INTERLACE ? BAND_H : yEnd - y;
    uint64_t ta = esp_timer_get_time();
    // core0 とこのコアで帯の行を取り合って描く
    job.f.rot = f.rot; job.f.rotC = f.rotC;
    job.buf = band[bi]; job.start = start; job.count = count; job.yEnd = yEnd;
    nextRow.store(0, std::memory_order_release);
    xSemaphoreGive(semStart);
    renderRows(f, band[bi], start, count, yEnd);
    xSemaphoreTake(semDone, portMAX_DELAY);
    tRen += esp_timer_get_time() - ta;
    // 前の DMA 完了を待ってから送る (もう一方のバッファはその間に描く)
    if (INTERLACE) {
      for (int k = 0; k < count; k += 2) {
        int yy = rowOf(start, k);
        if (yy < yEnd) M5.Display.pushImageDMA(0, yy, SW, 2, band[bi] + k * SW);
      }
    } else {
      M5.Display.pushImageDMA(0, y, SW, count, band[bi]);
    }
  }
  M5.Display.waitDMA();
  M5.Display.endWrite();

  if (++frames == 100) {
    uint32_t t = millis();
    Serial.printf("%.1f fps  render %u us/frame\n", 100000.f / (t - t0), (unsigned)(tRen / 100));
    frames = 0; t0 = t; tRen = 0;
  }
}
