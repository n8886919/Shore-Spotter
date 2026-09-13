## DOCS
- `include/firmware_version.h` — Client／Server 共用版本號唯一來源；發版時同步 `CHANGELOG.md`
- `CHANGELOG.md` — 從 0.1 開始的版本紀錄（不等同 LoRa 協定版本）
- `docs/interface.md` — LoRa 封包格式 + 攝影站 HTTP API + 欄位對應表
- `docs/features.md` — 韌體功能/流程/狀態/OLED 顯示
- `docs/hardware.md` — 腳位與接線
- `include/protocol.h` — client/server 共用的 LoRa 封包定義
- `include/gnss_snapshot.h` — 同 epoch RMC／GGA、來源年齡與 UART 積壓失效
- `include/lora_schedule.h` — 500 ms 無線時槽、略過過期工作與 ACK 回覆窗口
- `include/packet_diagnostics.h` — 固定容量封包事件與 raw bytes
- `include/http_timing.h` — WebServer 請求前處理／組裝／寫入分段計時，保留最慢請求
- `docs/debug-export.md` — 網頁除錯、錄製匯出與現場判讀
- `docs/link-test-2026-09-13.md` — v4 實機收包證據、GPS／ACK 限制與 HTTP 停頓觀察
- `include/geo_math.h` — 純數學（角度/方位/圓擬合/電量/GPS 分級），不依賴 Arduino
- `include/alerts.h` — 現場提醒的門檻與分級，不依賴 Arduino
- `include/tracking_policy.h` — GPS 品質與 GPS/UART 模式隔離規則，不依賴 Arduino
- `include/servo_motion.h` — 所有模式共用限速，預設 30°/s，1–90 可調；servospd NVS 單值
- `include/control_cadence.h` — GPS 目標 50 ms 排程，與 Servo 限速器服務獨立
- `include/uart_target.h` — UART 最新目標與 250 ms lease，不執行 PWM
- `include/command_freshness.h` — HTTP／SET2 世代、序號、期限與 LoRa seq 重同步
- `docs/motion-control.md` — 共用控制設定、來源有效性與新 HTTP／UART 契約
- `docs/uart-servo.md` — GPS / UART 獨立控制與鏡頭指南針校正操作
- `include/uart_servo_mode.h` / `src/uart_servo_mode.cpp` — UART 控制端點與輸入生命週期
- `include/uart_set_parser.h` — SET／SET2／SYNC 分行、逾時與非法指令檢查
- `include/loop_metrics.h` — wrap-safe deadline、服務間隔與同步操作耗時
- `include/async_lora_ack.h` — 非阻塞 ACK 生命週期，可用 fake radio 驗證
- `include/client_binding.h` — 單一 client ID 驗證與舊設定遷移
- `include/magnetic_declination.h` — GPS 位置／日期轉台灣磁偏角
- `include/taiwan_wmm_grid.h` — 產生檔，來源 `tools/make_declination.py`
- `test/` — `pio test -e native` 在筆電上跑的純邏輯測試（數學／提醒／GPS/UART 模式隔離／Servo 控制）
- `include/wifi_config.h` — 手機熱點 SSID/密碼設定（SERVER 開機連線用）
- `include/web_ui.h` — 內嵌監控頁
- `include/web_icon.h` — 分頁圖示 PNG（**產生檔，勿手改**）
- `tools/make_icon.py` — 圖示的唯一來源；改設計後重跑會更新 web_icon.h
- `tools/test_servo_slider.js` — `node tools/test_servo_slider.js` 驗證滑桿排程與模式切換
- `tools/test_motion_backend.py` — 擷取實際 HTTP／PWM／GPS gate／NVS 讀寫做主機整合測試
- `tools/test_motion_ui.js` — 資訊頁速度自動保存、GPS 選取門檻與完整頁面模擬
- `tools/test_packet_backend.py` — 實際封包建立／解析與 GNSS 主機整合
- `tools/test_debug_backend.py` / `tools/test_debug_ui.js` — 實際診斷 JSON／log cursor 與網頁錄製匯出
- `docs/health-check-2026-09-08.md` — 最新驗證狀態、歷史測試與尚未處理的問題

## RULES:
+ 功能、修正、介面或設定行為異動須同步記錄 `CHANGELOG.md`；未定版先放「未發行」，標版時歸入對應版本並記錄驗證／部署狀態。
+ build 與驗證由 agent 執行，輸出以結果摘要為主；使用者明確要求不 build 時不得編譯。
+ 使用中文回答,也可以中文夾雜,不用將專有名詞刻意翻譯
