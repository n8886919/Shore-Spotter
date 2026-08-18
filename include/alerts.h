#pragma once
// ---------------------------------------------------------------------------
// Shore Spotter — 現場提醒的門檻與分級（不碰硬體，可在 native 測試）。
//
// 監控頁最上方那條訊息列的判斷邏輯。使用者是站在沙灘上的非工程人員，所以：
//   * 門檻集中在這裡，不散在各處的 if 裡；
//   * 分級只有兩級（warn / error），因為現場只有兩種反應：「留意」和「現在處理」；
//   * 訊息文字寫在 main.cpp 的 buildAlertsJson()，那裡才拿得到要代入的數字。
//
// 對應文件：docs/interface.md 的 GET /api/status。
// ---------------------------------------------------------------------------
#include <stdint.h>

namespace alerts {

enum Level : uint8_t { NONE = 0, WARN = 1, ERROR = 2 };

inline const char *levelText(Level l) {
  return l == ERROR ? "error" : (l == WARN ? "warn" : "none");
}

// --- 電量 -----------------------------------------------------------------
// 20% 開始提醒（還有時間換電池），10% 是即將自動關機（BATT_SHUTDOWN_MV）。
constexpr uint8_t kBattWarnPct = 20;
constexpr uint8_t kBattErrorPct = 10;

inline Level batteryLevel(int pct) {
  if (pct < 0) return NONE;  // 讀不到電量不算異常（例如純 USB 供電）
  if (pct <= kBattErrorPct) return ERROR;
  if (pct <= kBattWarnPct) return WARN;
  return NONE;
}

// --- 溫度 -----------------------------------------------------------------
// 黑色防水盒曬在沙灘上很容易破 50°C；60°C 以上鋰電池開始有風險，
// 而且 GPS 與 LoRa 模組的規格上限多半就在這附近。
constexpr int kTempWarnC = 50;
constexpr int kTempErrorC = 60;

inline Level temperatureLevel(int tempC, bool haveSensor) {
  if (!haveSensor) return NONE;
  if (tempC >= kTempErrorC) return ERROR;
  if (tempC >= kTempWarnC) return WARN;
  return NONE;
}

// --- 濕度（進水偵測）------------------------------------------------------
// 絕對門檻單獨用會誤報：海邊空氣本來就 80% 起跳，封盒時關進潮濕空氣是常態。
// 進水真正的特徵是「相對開機時單調上升」，所以兩條規則並用：
//   * 絕對 >= kHumErrorPct        -> 幾乎確定進水
//   * 比基準高 kHumRiseWarnPct 且已經偏高 -> 可能正在滲水
// 基準是開機後收到的第一筆濕度（baselinePct < 0 表示還沒有基準）。
constexpr uint8_t kHumErrorPct = 90;
constexpr uint8_t kHumWarnPct = 80;
constexpr uint8_t kHumRiseWarnPct = 15;

inline Level humidityLevel(int humPct, int baselinePct) {
  if (humPct < 0) return NONE;  // 沒有感測器
  if (humPct >= kHumErrorPct) return ERROR;
  if (baselinePct >= 0 && humPct >= kHumWarnPct &&
      (humPct - baselinePct) >= kHumRiseWarnPct) {
    return WARN;
  }
  return NONE;
}

// --- LoRa 連線 -------------------------------------------------------------
// 位置封包是 1 Hz。10 秒沒消息已經是明顯異常，30 秒等於追蹤停擺。
constexpr uint32_t kLinkWarnSec = 10;
constexpr uint32_t kLinkErrorSec = 30;

inline Level linkLevel(int32_t sinceRxSec) {
  if (sinceRxSec < 0) return NONE;
  if ((uint32_t)sinceRxSec >= kLinkErrorSec) return ERROR;
  if ((uint32_t)sinceRxSec >= kLinkWarnSec) return WARN;
  return NONE;
}

// --- 站體被轉動後的瞄準誤差 ------------------------------------------------
// 望遠端 2 度已經足以讓人跑出畫面外。
constexpr float kPoseErrWarnDeg = 2.0f;

inline Level poseLevel(float poseErrDeg) {
  return (poseErrDeg >= kPoseErrWarnDeg) ? WARN : NONE;
}

}  // namespace alerts
