// M5Stack StopWatch : 1 分で 1 回転する地球
//
// ガラス半球越しに「球が中に浮いている」ように見せるための工夫:
//  - 地球の縁をドームの縁に近づけ、球体の輪郭とガラスの曲面を揃える
//  - 斜め前方からの太陽光 (ランバート + 明暗境界のぼかし) で陰影を付ける
//  - 海面の鏡面ハイライト、縁の大気散乱、外側の大気グロー
//  - 夜側に都市の灯り
//  - IMU の傾きで地球だけを少し動かし、背景の星との間に視差を作る
//
// 高速化:
//  - 各ピクセルの緯度・経度・奥行き (nz) を 4 バイトのテーブルに事前計算 (PSRAM)
//    自転は経度にオフセットを足すだけなので、毎フレームの三角関数は不要
//  - 陰影は内部 RAM の小さな 1 次元テーブルからその場で計算
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
static constexpr uint32_t ROTATION_US   = 60u * 1000u * 1000u;  // 1 回転の時間
static constexpr float    EARTH_RADIUS  = 0.86f;   // 画面半径に対する地球半径
static constexpr float    GLOW_WIDTH    = 0.055f;  // 大気グローの幅 (画面半径比)
static constexpr float    AXIS_TILT_DEG = 23.4f;   // 地軸の傾き (画面内で右へ)
static constexpr float    AXIS_TOWARD_DEG = 18.0f; // 北極をこちらへ倒す角度
static constexpr int      PARALLAX_PX   = 10;      // IMU 視差の最大移動量 (0 で無効)
static constexpr float    PARALLAX_SIGN_X = -1.0f; // 動く向きが逆ならここを反転
static constexpr float    PARALLAX_SIGN_Y = 1.0f;
static constexpr int      STAR_COUNT    = 350;
// 太陽方向 (x:右, y:上, z:手前)
static constexpr float    SUN_X = -0.70f, SUN_Y = 0.30f, SUN_Z = 0.58f;

// ---------------- テクスチャ ----------------
static constexpr int TEX_W = 1024, TEX_H = 512;
extern const uint8_t day_start[]   asm("_binary_data_earth_day_rgb565_start");
extern const uint8_t day_end[]     asm("_binary_data_earth_day_rgb565_end");
extern const uint8_t night_start[] asm("_binary_data_earth_night_gray8_start");
extern const uint8_t night_end[]   asm("_binary_data_earth_night_gray8_end");

static uint16_t* texDay   = nullptr;  // PSRAM, 4x4 タイル配置
static uint8_t*  texNight = nullptr;

// (u:0..1023, v:0..511) → 4x4 タイル配置での添字
static inline uint32_t tileIndex(uint32_t u, uint32_t v) {
  return ((v >> 2) << 12) | ((u >> 2) << 4) | ((v & 3) << 2) | (u & 3);
}

// ---------------- 事前計算テーブル ----------------
// 陰影は時間で変わらないので、毎フレームの計算は
//   色 = テクスチャ × L + add (+ 昼側: 海の鏡面 / 夜側: 夜景)
// だけになるよう、ピクセルごとに事前計算しておく (PSRAM, 8 バイト/画素)。
struct Px {
  uint16_t u;    // 経度 (65536 = 一周)
  uint16_t vf;   // [8:0] 緯度の行, [13:9] 加算色の強さ (縁の大気 / グロー),
                 // bit14 = 夜側, bit15 = 大気グロー (テクスチャなし)
  uint8_t  L;    // テクスチャに掛ける明るさ (0..255)
  uint8_t  sw;   // 昼側: 鏡面の強さ / 夜側: 夜景の重み
};
static constexpr uint16_t VF_NIGHT = 1u << 14, VF_GLOW = 1u << 15;
// 加算色 (R5,G6,B5 を 10bit ずつ詰めたもの)。[0..31] 縁の大気, [32..63] グロー
static uint32_t addTab[64];
static inline uint32_t packAdd(int r5, int g6, int b5) { return (r5 << 20) | (g6 << 10) | b5; }
struct Row { int16_t x0; int16_t len; uint32_t ofs; };

static Px*  lut  = nullptr;
static Row* rows = nullptr;
static int  R = 0, RG = 0;   // 地球半径, 地球 + グローの半径 (px)

// 描画は内部 SRAM の帯バッファ (BAND_H 行) 2 枚で行い、DMA で直接パネルへ送る。
static constexpr int BAND_H = 18;     // 偶数 (CO5300 は偶数境界での転送が必要)
static uint16_t* band[2] = { nullptr, nullptr };
static int SW = 0, SH = 0;

struct Star { uint16_t x, y; uint16_t c; };
static Star*     stars = nullptr;
static uint16_t* starRow = nullptr;   // y 行の最初の星の添字 (SH+1 個)

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float smooth(float e0, float e1, float x) {
  float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}
static inline uint16_t rgb565(int r, int g, int b) {
  r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline uint8_t u8f(float v) { return (uint8_t)(clampf(v, 0.f, 1.f) * 255.f + 0.5f); }

static void buildLut() {
  const float half = SW * 0.5f;
  R  = (int)(half * EARTH_RADIUS);
  const int G = (int)(half * GLOW_WIDTH);
  RG = R + G;

  // 地軸 a と経度の基準 e0 (正面), e1 (東)
  const float tz = AXIS_TILT_DEG * (float)M_PI / 180.f;
  const float tx = AXIS_TOWARD_DEG * (float)M_PI / 180.f;
  const float ax = sinf(tz) * cosf(tx), ay = cosf(tz) * cosf(tx), az = sinf(tx);
  float e0x = -az * ax, e0y = -az * ay, e0z = 1.f - az * az;  // Z - (Z.a)a
  float n0 = sqrtf(e0x * e0x + e0y * e0y + e0z * e0z);
  e0x /= n0; e0y /= n0; e0z /= n0;
  const float e1x = ay * e0z - az * e0y, e1y = az * e0x - ax * e0z, e1z = ax * e0y - ay * e0x;

  float lx = SUN_X, ly = SUN_Y, lz = SUN_Z;
  float ln = sqrtf(lx * lx + ly * ly + lz * lz);
  lx /= ln; ly /= ln; lz /= ln;
  float hx = lx, hy = ly, hz = lz + 1.f;   // ハーフベクトル (視線 = +Z)
  float hn = sqrtf(hx * hx + hy * hy + hz * hz);
  hx /= hn; hy /= hn; hz /= hn;
  float l2 = sqrtf(lx * lx + ly * ly);     // 画面内の太陽方向 (グロー用)

  for (int q = 0; q < 32; ++q) {
    float a = q / 31.f;
    addTab[q]      = packAdd((int)(a * 80 / 8), (int)(a * 140 / 4), (int)(a * 255 / 8));
    addTab[q + 32] = packAdd((int)(a * 90 / 8), (int)(a * 160 / 4), (int)(a * 255 / 8));
  }

  const int nRows = RG * 2 + 1;
  rows = (Row*)heap_caps_malloc(sizeof(Row) * nRows, MALLOC_CAP_INTERNAL);
  size_t total = 0;
  for (int j = 0; j < nRows; ++j) {
    float dy = j - RG + 0.5f;
    int w = (int)sqrtf(fmaxf(0.f, (float)RG * RG - dy * dy));
    rows[j] = { (int16_t)-w, (int16_t)(w * 2), (uint32_t)total };
    total += w * 2;
  }
  lut = (Px*)heap_caps_malloc(sizeof(Px) * total, MALLOC_CAP_SPIRAM);
  Serial.printf("LUT %u px, %u KB\n", (unsigned)total, (unsigned)(sizeof(Px) * total / 1024));

  for (int j = 0; j < nRows; ++j) {
    Px* p = lut + rows[j].ofs;
    for (int i = 0; i < rows[j].len; ++i, ++p) {
      float px = rows[j].x0 + i + 0.5f;
      float py = j - RG + 0.5f;
      float d  = sqrtf(px * px + py * py);
      if (d >= R) {
        // 大気グロー: 外へ向かって減衰、太陽側ほど明るい (背景の星に加算)
        float t = 1.f - clampf((d - R) / G, 0.f, 1.f);
        float sunSide = l2 > 0 ? (px * lx - py * ly) / (d * l2) : 0.f;
        float a = t * t * (0.30f + 0.55f * smooth(-0.6f, 0.8f, sunSide));
        uint16_t q = (uint16_t)(clampf(a, 0.f, 1.f) * 31.f + 0.5f);
        *p = { 0, (uint16_t)(VF_GLOW | (q << 9)), 0, 0 };
        continue;
      }
      // 球面上の法線 (視点座標)
      float nx = px / R, ny = -py / R;
      float nz = sqrtf(fmaxf(0.f, 1.f - nx * nx - ny * ny));
      // 地球座標へ
      float sLat = clampf(nx * ax + ny * ay + nz * az, -1.f, 1.f);
      float lat  = asinf(sLat);
      float lon  = atan2f(nx * e1x + ny * e1y + nz * e1z, nx * e0x + ny * e0y + nz * e0z);
      uint16_t u = (uint16_t)(int32_t)((lon + (float)M_PI) / (2.f * (float)M_PI) * 65536.f);
      int v = (int)(((float)M_PI * 0.5f - lat) / (float)M_PI * TEX_H);
      v = v < 0 ? 0 : (v >= TEX_H ? TEX_H - 1 : v);

      float ndl  = nx * lx + ny * ly + nz * lz;
      float dayT = smooth(-0.10f, 0.15f, ndl);
      float lam  = fmaxf(ndl, 0.f);
      float light = 0.03f + dayT * (0.22f + 0.85f * powf(lam, 0.8f));
      float night = (1.f - smooth(-0.18f, 0.04f, ndl)) * (0.55f + 0.45f * nz);
      float ndh  = fmaxf(nx * hx + ny * hy + nz * hz, 0.f);
      float spec = (0.85f * powf(ndh, 70.f) + 0.12f * powf(ndh, 8.f)) * dayT;
      float rim  = powf(1.f - nz, 2.2f) * 0.9f * smooth(-0.30f, 0.35f, ndl);

      // c*light を haze へ rim だけ寄せる = c*(light*(1-rim)) + haze*rim
      float L = clampf(light, 0.f, 1.f) * (1.f - rim);
      bool nightSide = night > spec;
      uint16_t q = (uint16_t)(clampf(rim, 0.f, 1.f) * 31.f + 0.5f);
      uint16_t vf = (uint16_t)v | (q << 9) | (nightSide ? VF_NIGHT : 0);
      *p = { u, vf, (uint8_t)fminf(255.f, L * 256.f), u8f(nightSide ? night : spec) };
    }
  }
}

static void buildStars() {
  stars = (Star*)heap_caps_malloc(sizeof(Star) * STAR_COUNT * 3, MALLOC_CAP_INTERNAL);
  starRow = (uint16_t*)heap_caps_calloc(SH + 1, 2, MALLOC_CAP_INTERNAL);
  int n = 0;
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  for (int k = 0; k < STAR_COUNT; ++k) {
    int x = rnd() % SW, y = rnd() % SH;
    int b = 40 + (int)(powf((rnd() & 0xFFFF) / 65535.f, 3.f) * 215.f);
    int tint = rnd() % 3;
    int r = tint == 0 ? b : b * 85 / 100, g = b * 92 / 100, bl = tint == 2 ? b * 80 / 100 : b;
    stars[n++] = { (uint16_t)x, (uint16_t)y, __builtin_bswap16(rgb565(r, g, bl)) };
    if (b > 200 && x + 1 < SW && y + 1 < SH) {  // 明るい星は少し大きく
      uint16_t dim = __builtin_bswap16(rgb565(r / 3, g / 3, bl / 3));
      stars[n++] = { (uint16_t)(x + 1), (uint16_t)y, dim };
      stars[n++] = { (uint16_t)x, (uint16_t)(y + 1), dim };
    }
  }
  // y で並べ替えて行ごとの索引を作る
  std::sort(stars, stars + n, [](const Star& a, const Star& b) { return a.y < b.y; });
  int k = 0;
  for (int y = 0; y <= SH; ++y) {
    while (k < n && stars[k].y < y) ++k;
    starRow[y] = k;
  }
}

// ---------------- 描画 ----------------
struct Frame { int cx, cy; uint16_t rot; };
struct Job { Frame f; uint16_t* buf; int bandY, y1; };
static volatile Job job;
static std::atomic<int> nextRow;   // 2 コアで行を取り合う (負荷の偏りをなくす)

static inline uint16_t swap16(uint32_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

// 帯 (bandY 行目から y1 行目の手前まで) の行を nextRow から取って描く (5/6/5 ビットのまま計算)
static void IRAM_ATTR renderRows(const Frame& f, uint16_t* buf, int bandY, int y1) {
  const uint16_t rot = f.rot;
  for (int y; (y = nextRow.fetch_add(1, std::memory_order_relaxed)) < y1; ) {
    uint16_t* line = buf + (y - bandY) * SW;
    memset(line, 0, SW * 2);
    for (int k = starRow[y]; k < starRow[y + 1]; ++k) line[stars[k].x] = stars[k].c;

    int j = y - (f.cy - RG);
    if (j < 0 || j > RG * 2) continue;
    const Row& rw = rows[j];
    int x = f.cx + rw.x0;
    int i0 = 0, i1 = rw.len;
    if (x < 0) i0 = -x;
    if (x + i1 > SW) i1 = SW - x;
    const Px* p = lut + rw.ofs + i0;
    uint16_t* dst = line + x + i0;

    for (int i = i0; i < i1; ++i, ++p, ++dst) {
      const uint32_t vf = p->vf;
      const uint32_t ad = addTab[((vf >> 9) & 31) | ((vf >> 10) & 32)];  // グローは 32..63
      uint32_t R5 = ad >> 20, G6 = (ad >> 10) & 1023, B5 = ad & 1023;

      if (vf & VF_GLOW) {  // 大気グロー: 背景 (星) に加算
        uint32_t bg = swap16(*dst);
        R5 += bg >> 11; G6 += (bg >> 5) & 63; B5 += bg & 31;
      } else {
        const uint32_t ti = tileIndex((uint16_t)(p->u - rot) >> 6, vf & 511);
        const uint32_t t = texDay[ti];
        const uint32_t r = t >> 11, g = (t >> 5) & 63, b = t & 31;
        const uint32_t L = p->L;
        R5 += (r * L) >> 8; G6 += (g * L) >> 8; B5 += (b * L) >> 8;
        const uint32_t sw = p->sw;
        if (sw) {
          if (vf & VF_NIGHT) {                     // 夜景 (暖色)
            uint32_t n = texNight[ti] * sw;        // 0..65025
            R5 += n >> 11; G6 += (n * 3) >> 12; B5 += (n * 3) >> 14;
          } else if (b > r + 2 && b * 2 >= g) {    // 海だけ鏡面反射
            R5 += (sw * 25) >> 8; G6 += (sw * 53) >> 8; B5 += (sw * 29) >> 8;
          }
        }
      }
      R5 = R5 > 31 ? 31 : R5; G6 = G6 > 63 ? 63 : G6; B5 = B5 > 31 ? 31 : B5;
      *dst = swap16((R5 << 11) | (G6 << 5) | B5);
    }
  }
}

static SemaphoreHandle_t semStart, semDone;
static volatile uint64_t tCore0 = 0;

static void workerTask(void*) {
  for (;;) {
    xSemaphoreTake(semStart, portMAX_DELAY);
    Frame f = { job.f.cx, job.f.cy, job.f.rot };
    uint64_t a = esp_timer_get_time();
    renderRows(f, job.buf, job.bandY, job.y1);
    tCore0 += esp_timer_get_time() - a;
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
  const size_t nd = (day_end - day_start) / 2, nn = night_end - night_start;
  for (uint32_t v = 0; v < (uint32_t)TEX_H; ++v) {
    for (uint32_t u = 0; u < (uint32_t)TEX_W; ++u) {
      size_t s = (size_t)v * TEX_W + u;
      uint32_t t = tileIndex(u, v);
      texDay[t]   = s < nd ? d[s] : 0;
      texNight[t] = s < nn ? night_start[s] : 0;
    }
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
  M5.Display.setBrightness(200);
  M5.Display.fillScreen(TFT_BLACK);

  SW = M5.Display.width();
  SH = M5.Display.height();
  Serial.printf("display %dx%d\n", SW, SH);

  texDay = (uint16_t*)heap_caps_malloc(TEX_W * TEX_H * 2, MALLOC_CAP_SPIRAM);
  texNight = (uint8_t*)heap_caps_malloc(TEX_W * TEX_H, MALLOC_CAP_SPIRAM);
  for (int i = 0; i < 2; ++i)
    band[i] = (uint16_t*)heap_caps_malloc(SW * BAND_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!texDay || !texNight || !band[0] || !band[1]) {
    M5.Display.drawString("alloc failed", 10, SH / 2);
    for (;;) delay(1000);
  }

  loadTextures();
  buildStars();
  buildLut();

  semStart = xSemaphoreCreateBinary();
  semDone = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(workerTask, "render0", 4096, nullptr, 5, nullptr, 0);
}

void loop() {
  static float offX = 0, offY = 0;
  static uint32_t frames = 0, t0 = millis();
  static uint64_t tRen = 0, tWait = 0, tCore1 = 0;

  M5.update();
  if (PARALLAX_PX > 0 && M5.Imu.isEnabled()) {
    float ax, ay, az;
    if (M5.Imu.getAccel(&ax, &ay, &az)) {
      float tx = clampf(ax * PARALLAX_SIGN_X, -1.f, 1.f) * PARALLAX_PX * 1.6f;
      float ty = clampf(ay * PARALLAX_SIGN_Y, -1.f, 1.f) * PARALLAX_PX * 1.6f;
      offX += (clampf(tx, -PARALLAX_PX, PARALLAX_PX) - offX) * 0.15f;
      offY += (clampf(ty, -PARALLAX_PX, PARALLAX_PX) - offY) * 0.15f;
    }
  }

  uint64_t us = esp_timer_get_time() % ROTATION_US;
  Frame f;
  f.cx  = SW / 2 + (int)lroundf(offX);
  f.cy  = SH / 2 + (int)lroundf(offY);
  f.rot = (uint16_t)((us << 16) / ROTATION_US);

  M5.Display.startWrite();
  int bi = 0;
  for (int y = 0; y < SH; y += BAND_H, bi ^= 1) {
    int h = SH - y < BAND_H ? SH - y : BAND_H;
    uint64_t ta = esp_timer_get_time();
    // core0 とこのコアで帯の行を取り合って描く
    job.f.cx = f.cx; job.f.cy = f.cy; job.f.rot = f.rot;
    job.buf = band[bi]; job.bandY = y; job.y1 = y + h;
    nextRow.store(y, std::memory_order_release);
    xSemaphoreGive(semStart);
    uint64_t tc = esp_timer_get_time();
    renderRows(f, band[bi], y, y + h);
    tCore1 += esp_timer_get_time() - tc;
    xSemaphoreTake(semDone, portMAX_DELAY);
    uint64_t tb = esp_timer_get_time();
    // 前の帯の DMA 完了を待ってから送る (もう一方のバッファはその間に描く)
    M5.Display.pushImageDMA(0, y, SW, h, band[bi]);
    tRen += tb - ta; tWait += esp_timer_get_time() - tb;
  }
  M5.Display.waitDMA();
  M5.Display.endWrite();

  if (++frames == 100) {
    uint32_t t = millis();
    Serial.printf("%.1f fps  render %u us (core0 %u, core1 %u)  dma-wait %u us  cpu %d MHz\n",
                  100000.f / (t - t0), (unsigned)(tRen / 100), (unsigned)(tCore0 / 100),
                  (unsigned)(tCore1 / 100), (unsigned)(tWait / 100), getCpuFrequencyMhz());
    frames = 0; t0 = t; tRen = tWait = tCore1 = 0; tCore0 = 0;
  }
}
