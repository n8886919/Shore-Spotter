// ---------------------------------------------------------------------------
// include/geo_math.h 與 include/alerts.h 的單元測試（在筆電上跑）。
//
//   pio test -e native
//
// 這裡測的每一個函式都有同一個特性：算錯了不會有任何錯誤訊息，只會讓鏡頭指向
// 錯的地方，或讓提醒該響的時候沒響。而在板子上驗證它們的唯一辦法是人站在腳架
// 旁邊轉一圈（見 docs/hardware.md 的驗收測試）。
// ---------------------------------------------------------------------------
#include <unity.h>

#include <math.h>

#include "alerts.h"
#include "geo_math.h"

void setUp(void) {}
void tearDown(void) {}

// --- 角度環繞 --------------------------------------------------------------

static void test_normalize360_basic(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.0f, geo::normalize360(0.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.0f, geo::normalize360(360.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 1.0f, geo::normalize360(361.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 359.0f, geo::normalize360(-1.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 350.0f, geo::normalize360(-730.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 123.5f, geo::normalize360(123.5f));
}

// 舊的 while 迴圈版本碰到這種輸入要跑數百萬次；fmodf 版本是常數時間。
// 這個測試存在的意義是「別再改回 while 迴圈」。
static void test_normalize360_extreme_inputs(void) {
  float big = geo::normalize360(1.0e9f);
  TEST_ASSERT_TRUE(big >= 0.0f && big < 360.0f);
  float neg = geo::normalize360(-1.0e9f);
  TEST_ASSERT_TRUE(neg >= 0.0f && neg < 360.0f);
  // NaN / Inf 不能傳染到 servo 指令上
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.0f, geo::normalize360(NAN));
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.0f, geo::normalize360(INFINITY));
}

static void test_angleDiff(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-3, 20.0f, geo::angleDiff(10.0f, 350.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-3, -20.0f, geo::angleDiff(350.0f, 10.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-3, 90.0f, geo::angleDiff(90.0f, 0.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-3, -90.0f, geo::angleDiff(0.0f, 90.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-3, 0.0f, geo::angleDiff(45.0f, 45.0f));
  // 結果一定落在 [-180, 180]
  for (int a = 0; a < 360; a += 7) {
    for (int b = 0; b < 360; b += 11) {
      float d = geo::angleDiff((float)a, (float)b);
      TEST_ASSERT_TRUE(d >= -180.001f && d <= 180.001f);
    }
  }
}

// --- 方位角 ----------------------------------------------------------------

static void test_computeBearing_cardinals(void) {
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.0, geo::computeBearing(0, 0, 1, 0));      // 北
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 90.0, geo::computeBearing(0, 0, 0, 1));     // 東
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 180.0, geo::computeBearing(0, 0, -1, 0));   // 南
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 270.0, geo::computeBearing(0, 0, 0, -1));   // 西
}

static void test_computeBearing_always_positive(void) {
  // 西南方向的 atan2 是負的，必須被折回 [0, 360)。
  double b = geo::computeBearing(25.0, 121.5, 24.9, 121.4);
  TEST_ASSERT_TRUE(b >= 0.0 && b < 360.0);
  TEST_ASSERT_TRUE(b > 180.0 && b < 270.0);
}

static void test_equirect_distance(void) {
  // 赤道上緯度差一度 = R * pi/180
  double oneDeg = geo::kEarthRadiusM * M_PI / 180.0;
  TEST_ASSERT_DOUBLE_WITHIN(1.0, oneDeg, geo::equirectDistanceM(0, 0, 1, 0));
  // 高緯度的經度距離要被 cos(lat) 縮短
  double atEquator = geo::equirectDistanceM(0, 0, 0, 1);
  double at60 = geo::equirectDistanceM(60, 0, 60, 1);
  TEST_ASSERT_DOUBLE_WITHIN(atEquator * 0.01, atEquator * 0.5, at60);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.0, geo::equirectDistanceM(25, 121, 25, 121));
}

// --- 圓擬合（磁力計 hard-iron 校正的核心）---------------------------------

static void test_fitCircle_recovers_known_circle(void) {
  const size_t N = 36;
  float xs[N], ys[N];
  const float CX = 3.0f, CY = -2.0f, R = 5.0f;
  for (size_t i = 0; i < N; i++) {
    float t = (float)i * 2.0f * (float)M_PI / (float)N;
    xs[i] = CX + R * cosf(t);
    ys[i] = CY + R * sinf(t);
  }
  float cx = 0, cy = 0, r = 0;
  TEST_ASSERT_TRUE(geo::fitCircle(xs, ys, 1, N, cx, cy, r));
  TEST_ASSERT_FLOAT_WITHIN(1e-3, CX, cx);
  TEST_ASSERT_FLOAT_WITHIN(1e-3, CY, cy);
  TEST_ASSERT_FLOAT_WITHIN(1e-3, R, r);
}

// 實際呼叫端傳的是交錯的三軸緩衝區（magCalRaw[N][3]）的兩個欄位，stride=3。
static void test_fitCircle_interleaved_stride(void) {
  const size_t N = 40;
  float raw[N][3];
  const float CX = -0.12f, CY = 0.31f, R = 0.37f;  // 接近真實的地磁量級（高斯）
  for (size_t i = 0; i < N; i++) {
    float t = (float)i * 2.0f * (float)M_PI / (float)N;
    raw[i][0] = 9.9f;                  // 垂直軸：擬合時不該被用到
    raw[i][1] = CX + R * cosf(t);
    raw[i][2] = CY + R * sinf(t);
  }
  float cx = 0, cy = 0, r = 0;
  TEST_ASSERT_TRUE(geo::fitCircle(&raw[0][1], &raw[0][2], 3, N, cx, cy, r));
  TEST_ASSERT_FLOAT_WITHIN(1e-4, CX, cx);
  TEST_ASSERT_FLOAT_WITHIN(1e-4, CY, cy);
  TEST_ASSERT_FLOAT_WITHIN(1e-4, R, r);
}

static void test_fitCircle_rejects_bad_input(void) {
  float xs[6] = {0, 1, 2, 3, 4, 5}, ys[6] = {0, 1, 2, 3, 4, 5};
  float cx, cy, r;
  // 樣本太少
  TEST_ASSERT_FALSE(geo::fitCircle(xs, ys, 1, 6, cx, cy, r));

  // 共線 —— 這是「根本沒轉」的情況，必須失敗而不是回一個亂數圓心
  const size_t N = 20;
  float lx[N], ly[N];
  for (size_t i = 0; i < N; i++) { lx[i] = (float)i; ly[i] = 2.0f * (float)i; }
  TEST_ASSERT_FALSE(geo::fitCircle(lx, ly, 1, N, cx, cy, r));
}

// 只轉了四分之一圈也要能擬合出正確的圓（真正擋住半吊子校正的是涵蓋率檢查，
// 不是這裡；這個測試確認擬合本身不會在部分弧上崩掉）。
static void test_fitCircle_partial_arc(void) {
  const size_t N = 15;
  float xs[N], ys[N];
  const float CX = 1.5f, CY = 2.5f, R = 4.0f;
  for (size_t i = 0; i < N; i++) {
    float t = (float)i * (float)M_PI / 2.0f / (float)N;
    xs[i] = CX + R * cosf(t);
    ys[i] = CY + R * sinf(t);
  }
  float cx = 0, cy = 0, r = 0;
  TEST_ASSERT_TRUE(geo::fitCircle(xs, ys, 1, N, cx, cy, r));
  TEST_ASSERT_FLOAT_WITHIN(1e-2, CX, cx);
  TEST_ASSERT_FLOAT_WITHIN(1e-2, CY, cy);
  TEST_ASSERT_FLOAT_WITHIN(1e-2, R, r);
}

// --- 電量 ------------------------------------------------------------------

static void test_batteryPercent(void) {
  TEST_ASSERT_EQUAL_UINT8(0, geo::batteryPercent(3200));
  TEST_ASSERT_EQUAL_UINT8(0, geo::batteryPercent(3000));
  TEST_ASSERT_EQUAL_UINT8(100, geo::batteryPercent(4150));
  TEST_ASSERT_EQUAL_UINT8(100, geo::batteryPercent(4200));
  TEST_ASSERT_EQUAL_UINT8(50, geo::batteryPercent(3675));
  // 單調遞增
  uint8_t prev = 0;
  for (uint16_t mv = 3200; mv <= 4150; mv += 10) {
    uint8_t p = geo::batteryPercent(mv);
    TEST_ASSERT_TRUE(p >= prev);
    prev = p;
  }
}

// --- GPS / LoRa 分級 -------------------------------------------------------

static void test_gpsSignal_grades(void) {
  TEST_ASSERT_EQUAL(geo::SIG_MISS, geo::gpsSignal(false, 0, 99.9f));
  TEST_ASSERT_EQUAL(geo::SIG_BAD, geo::gpsSignal(false, 6, 1.0f));   // 有衛星沒定位
  TEST_ASSERT_EQUAL(geo::SIG_BAD, geo::gpsSignal(true, 3, 1.0f));    // 衛星太少
  TEST_ASSERT_EQUAL(geo::SIG_GOOD, geo::gpsSignal(true, 9, 1.2f));
  TEST_ASSERT_EQUAL(geo::SIG_OK, geo::gpsSignal(true, 7, 2.0f));
  TEST_ASSERT_EQUAL(geo::SIG_OK, geo::gpsSignal(true, 9, 2.0f));     // 衛星夠但 HDOP 普通
  TEST_ASSERT_EQUAL(geo::SIG_BAD, geo::gpsSignal(true, 9, 5.0f));    // HDOP 太差
}

static void test_loraSignal_grades(void) {
  TEST_ASSERT_EQUAL(geo::SIG_GOOD, geo::loraSignal(-90.0f, 9.0f));
  TEST_ASSERT_EQUAL(geo::SIG_OK, geo::loraSignal(-110.0f, 9.0f));  // RSSI 拖下來
  TEST_ASSERT_EQUAL(geo::SIG_OK, geo::loraSignal(-90.0f, 3.0f));
  TEST_ASSERT_EQUAL(geo::SIG_BAD, geo::loraSignal(-120.0f, -5.0f));
}

// --- 現場提醒的門檻 --------------------------------------------------------

static void test_battery_alert_levels(void) {
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::batteryLevel(-1));   // 沒電池讀數不算異常
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::batteryLevel(100));
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::batteryLevel(21));
  TEST_ASSERT_EQUAL(alerts::WARN, alerts::batteryLevel(20));
  TEST_ASSERT_EQUAL(alerts::WARN, alerts::batteryLevel(11));
  TEST_ASSERT_EQUAL(alerts::ERROR, alerts::batteryLevel(10));
  TEST_ASSERT_EQUAL(alerts::ERROR, alerts::batteryLevel(0));
}

static void test_temperature_alert_levels(void) {
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::temperatureLevel(70, false));  // 沒感測器
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::temperatureLevel(30, true));
  TEST_ASSERT_EQUAL(alerts::WARN, alerts::temperatureLevel(50, true));
  TEST_ASSERT_EQUAL(alerts::WARN, alerts::temperatureLevel(59, true));
  TEST_ASSERT_EQUAL(alerts::ERROR, alerts::temperatureLevel(60, true));
}

// 進水偵測是這組提醒裡最容易寫錯的一個：絕對門檻單獨用會在海邊整天誤報。
static void test_humidity_alert_uses_baseline(void) {
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::humidityLevel(-1, 50));  // 沒感測器

  // 海邊空氣本來就 82%，而且開機時就這麼高 -> 不該報
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::humidityLevel(82, 80));
  // 同樣是 82%，但下水前只有 60% -> 正在滲水
  TEST_ASSERT_EQUAL(alerts::WARN, alerts::humidityLevel(82, 60));
  // 升幅夠但絕對值還很低 -> 不報（例如早晨的溫差）
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::humidityLevel(70, 50));
  // 絕對值破表 -> 不管基準是多少都是 error
  TEST_ASSERT_EQUAL(alerts::ERROR, alerts::humidityLevel(95, 90));
  TEST_ASSERT_EQUAL(alerts::ERROR, alerts::humidityLevel(90, -1));
  // 還沒有基準時，只靠絕對門檻
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::humidityLevel(85, -1));
}

static void test_link_alert_levels(void) {
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::linkLevel(-1));   // 從沒連上，另有訊息
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::linkLevel(0));
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::linkLevel(9));
  TEST_ASSERT_EQUAL(alerts::WARN, alerts::linkLevel(10));
  TEST_ASSERT_EQUAL(alerts::WARN, alerts::linkLevel(29));
  TEST_ASSERT_EQUAL(alerts::ERROR, alerts::linkLevel(30));
}

static void test_pose_alert_level(void) {
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::poseLevel(-1.0f));  // 無法判斷
  TEST_ASSERT_EQUAL(alerts::NONE, alerts::poseLevel(0.5f));
  TEST_ASSERT_EQUAL(alerts::WARN, alerts::poseLevel(2.0f));
  TEST_ASSERT_EQUAL(alerts::WARN, alerts::poseLevel(8.0f));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_normalize360_basic);
  RUN_TEST(test_normalize360_extreme_inputs);
  RUN_TEST(test_angleDiff);
  RUN_TEST(test_computeBearing_cardinals);
  RUN_TEST(test_computeBearing_always_positive);
  RUN_TEST(test_equirect_distance);
  RUN_TEST(test_fitCircle_recovers_known_circle);
  RUN_TEST(test_fitCircle_interleaved_stride);
  RUN_TEST(test_fitCircle_rejects_bad_input);
  RUN_TEST(test_fitCircle_partial_arc);
  RUN_TEST(test_batteryPercent);
  RUN_TEST(test_gpsSignal_grades);
  RUN_TEST(test_loraSignal_grades);
  RUN_TEST(test_battery_alert_levels);
  RUN_TEST(test_temperature_alert_levels);
  RUN_TEST(test_humidity_alert_uses_baseline);
  RUN_TEST(test_link_alert_levels);
  RUN_TEST(test_pose_alert_level);
  return UNITY_END();
}
