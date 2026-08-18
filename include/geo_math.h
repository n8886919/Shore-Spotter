#pragma once
// ---------------------------------------------------------------------------
// Shore Spotter — 純數學工具（不碰硬體、不依賴 Arduino）。
//
// 這裡放的全是「錯了不會有任何錯誤訊息」的函式：角度環繞、方位角、圓擬合、
// 電量換算、GPS 分級。它們決定鏡頭指向哪裡，但在板子上驗證只能靠人站在旁邊
// 轉腳架。抽出來之後 test/ 裡的 native 測試可以在筆電上秒驗。
//
// 唯一的規則：這個檔案不可以 #include <Arduino.h>，也不可以碰任何全域狀態。
// 一旦破例，native 測試就編不起來，這層保護也就沒了。
// ---------------------------------------------------------------------------
#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace geo {

constexpr double kEarthRadiusM = 6371000.0;
// 每緯度一度的公尺數，用於小範圍的等距長方投影。
constexpr double kMetersPerDegLat = 111320.0;

inline float deg2rad(float d) { return d * (float)M_PI / 180.0f; }
inline float rad2deg(float r) { return r * 180.0f / (float)M_PI; }

// 把任意角度收斂到 [0, 360)。
// 用 fmodf 而不是 while 迴圈：while 版本碰到異常大的輸入（例如某個 float
// 運算意外產生 1e9）要跑數百萬次迴圈，在 20 Hz 的追蹤迴圈裡會變成可見的停頓。
inline float normalize360(float a) {
  if (!isfinite(a)) return 0.0f;
  a = fmodf(a, 360.0f);
  return (a < 0.0f) ? a + 360.0f : a;
}

// a - b 的最短有號差值，落在 [-180, 180]。
inline float angleDiff(float a, float b) {
  float d = normalize360(a - b);
  return (d > 180.0f) ? d - 360.0f : d;
}

// 大圓航線的起始方位角（正北為 0，順時針增加）。
inline double computeBearing(double lat1, double lon1, double lat2, double lon2) {
  double phi1 = lat1 * M_PI / 180.0;
  double phi2 = lat2 * M_PI / 180.0;
  double dLon = (lon2 - lon1) * M_PI / 180.0;
  double y = sin(dLon) * cos(phi2);
  double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dLon);
  double brng = atan2(y, x) * 180.0 / M_PI;
  if (brng < 0) brng += 360.0;
  return brng;
}

// 等距長方投影的距離，公尺。本專案的距離都在數公里內，這個近似足夠，
// 而且只用來顯示與觸發警告，不進入指向計算。
inline double equirectDistanceM(double lat1, double lon1, double lat2, double lon2) {
  double eastM = (lon2 - lon1) * M_PI / 180.0 * cos(lat1 * M_PI / 180.0) * kEarthRadiusM;
  double northM = (lat2 - lat1) * M_PI / 180.0 * kEarthRadiusM;
  return sqrt(eastM * eastM + northM * northM);
}

// 最小平方圓擬合。把 (x-cx)^2 + (y-cy)^2 = r^2 改寫成
//   x^2 + y^2 = a*x + b*y + c,  a = 2cx, b = 2cy, c = r^2 - cx^2 - cy^2
// 之後對 (a, b, c) 是線性的，所以是一次 3x3 解而不是疊代擬合 —— 便宜到可以在
// 轉完那一瞬間直接在 ESP32 上跑完。
// stride 以 float 為單位，所以兩個座標可以是同一個交錯三軸陣列的兩個欄位，
// 不必先複製出來。
inline bool fitCircle(const float *xs, const float *ys, size_t stride, size_t n,
                      float &cx, float &cy, float &r) {
  if (n < 8) return false;
  double Sx = 0, Sy = 0, Sxx = 0, Syy = 0, Sxy = 0, Sz = 0, Sxz = 0, Syz = 0;
  for (size_t i = 0; i < n; i++) {
    double x = xs[i * stride], y = ys[i * stride], z = x * x + y * y;
    Sx += x; Sy += y; Sxx += x * x; Syy += y * y; Sxy += x * y;
    Sz += z; Sxz += x * z; Syz += y * z;
  }
  double m[3][4] = {{Sxx, Sxy, Sx, Sxz},
                    {Sxy, Syy, Sy, Syz},
                    {Sx,  Sy,  (double)n, Sz}};
  for (int col = 0; col < 3; col++) {  // Gauss-Jordan，含 partial pivoting
    int piv = col;
    for (int i = col + 1; i < 3; i++) {
      if (fabs(m[i][col]) > fabs(m[piv][col])) piv = i;
    }
    if (fabs(m[piv][col]) < 1e-12) return false;  // 退化：根本沒轉
    if (piv != col) {
      for (int k = 0; k < 4; k++) {
        double t = m[col][k]; m[col][k] = m[piv][k]; m[piv][k] = t;
      }
    }
    for (int i = 0; i < 3; i++) {
      if (i == col) continue;
      double f = m[i][col] / m[col][col];
      for (int k = col; k < 4; k++) m[i][k] -= f * m[col][k];
    }
  }
  double a = m[0][3] / m[0][0];
  double b = m[1][3] / m[1][1];
  double c = m[2][3] / m[2][2];
  cx = (float)(a / 2.0);
  cy = (float)(b / 2.0);
  double rr = c + (double)cx * cx + (double)cy * cy;
  if (rr <= 0) return false;
  r = (float)sqrt(rr);
  return true;
}

// --- 電量 -----------------------------------------------------------------
// 單顆鋰電池的線性刻度：BATT_EMPTY_MV 為 0%，BATT_PCT_FULL_MV 為 100%。
// web_ui.h 的 battPct() 是同一條刻度，兩邊改動必須同步。
constexpr uint16_t kBattEmptyMv = 3200;
constexpr uint16_t kBattFullMv = 4150;

inline uint8_t batteryPercent(uint16_t mv) {
  if (mv <= kBattEmptyMv) return 0;
  if (mv >= kBattFullMv) return 100;
  return (uint8_t)lround((mv - kBattEmptyMv) * 100.0f /
                         (kBattFullMv - kBattEmptyMv));
}

// --- GPS 分級 --------------------------------------------------------------
// Server OLED 與 Web UI 共用同一組門檻（web_ui.h 的 gpsGrade()）。
enum SigLevel : uint8_t { SIG_GOOD = 0, SIG_OK = 1, SIG_BAD = 2, SIG_MISS = 3 };

inline const char *sig4Text(SigLevel s) {
  switch (s) {
    case SIG_GOOD: return "Good";
    case SIG_OK:   return "OK";
    case SIG_BAD:  return "Bad";
    default:       return "Miss";
  }
}

//   Good : 有定位 & HDOP <= 1.5 & 衛星 >= 8
//   OK   : 有定位 & HDOP <= 3.0 & 衛星 >= 6
//   Bad  : 收得到衛星但定位不堪用
//   Miss : 完全沒訊號
inline SigLevel gpsSignal(bool fix, int sats, float hdop) {
  if (sats <= 0 && !fix) return SIG_MISS;
  if (!fix || sats < 4) return SIG_BAD;
  if (hdop <= 1.5f && sats >= 8) return SIG_GOOD;
  if (hdop <= 3.0f && sats >= 6) return SIG_OK;
  return SIG_BAD;
}

// LoRa 分級，取自最後一包的 RSSI/SNR；「完全沒連線」由呼叫端對應到 Miss。
inline SigLevel loraSignal(float rssi, float snr) {
  if (snr >= 7.0f && rssi >= -105.0f) return SIG_GOOD;
  if (snr >= 0.0f) return SIG_OK;
  return SIG_BAD;
}

}  // namespace geo
