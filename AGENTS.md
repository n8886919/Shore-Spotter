## DOCS
- `include/firmware_version.h` — Client／Station 共用版本號唯一來源；發版時同步 `CHANGELOG.md`
- `CHANGELOG.md` — 從 0.1 開始的版本紀錄（不等同 LoRa 協定版本）
- `docs/interface.md` — LoRa 封包格式 + 攝影站 HTTP API + 欄位對應表
- `docs/features.md` — 韌體功能/流程/狀態/OLED 顯示
- `docs/hardware.md` — 腳位與接線
- `include/protocol.h` — client/station 共用的 LoRa 封包定義
- `include/gnss_snapshot.h` — 最新 epoch RMC／GGA、實測到達期限、診斷年齡與 UART 積壓失效
- `include/lora_schedule.h` — airtime／guard 空檔預算
- `include/client_cadence.h` — 最新有效定位即送、單次失效通知與短暫配對等待
- `include/gnss_rate.h` — 115200／2 Hz 驗證版設定與 5 秒實測 epoch／RMC／GGA 頻率
- `include/station_position.h` — 岸端 30 秒座標平均與 RMS 散布警告，不丟離群點
- `docs/tracking-update-2026-09-19.md` — 已確認行為、主機驗證、實機驗收與未決問題
- `include/packet_diagnostics.h` — 固定容量封包事件與 raw bytes
- `include/packet_rate.h` — 接受 DATA 到達時間的 10 秒滑動 FPS，固定容量、不依賴網頁輪詢
- `docs/sd-log.md` — 兩端 SD 循環記錄、Client GPS gate／關卡電、USB 讀取與驗證界線
- `include/client_sd_policy.h` / `include/client_sd_record.h` — Client 記錄條件及有界事件快照，不改 RF 發送條件
- `include/sd_retention.h` / `src/sd_log.cpp` — 自家 log 辨識、背景空間回收與 SD worker
- `tools/test_client_sd_backend.py` — 實際 Client 記錄產生器／worker／省電與循環回收測試
- `include/http_timing.h` — WebServer 請求前處理／組裝／寫入分段計時，保留最慢請求
- `docs/debug-export.md` — 網頁除錯、錄製匯出與現場判讀
- `docs/link-test-2026-09-13.md` — v4 實機收包證據、GPS／ACK 限制與 HTTP 停頓觀察
- `include/geo_math.h` — 純數學（角度/方位/圓擬合/電量/GPS 分級），不依賴 Arduino
- `include/alerts.h` — 現場提醒的門檻與分級，不依賴 Arduino
- `include/tracking_policy.h` — GPS 定位／外推品質分開與 GPS/UART 模式隔離規則，不依賴 Arduino
- `include/servo_motion.h` — 所有模式共用限速，預設 30°/s，1–90 可調；servospd NVS 單值
- `include/control_cadence.h` — GPS 目標 50 ms 排程，與 Servo 限速器服務獨立
- `include/uart_target.h` — UART 最新目標與 250 ms lease，不執行 PWM
- `include/command_freshness.h` — HTTP／SET2 世代、序號、期限與 LoRa seq 重同步
- `docs/motion-control.md` — 共用控制設定、來源有效性與新 HTTP／UART 契約
- `docs/uart-servo.md` — GPS / UART 獨立控制與鏡頭指南針校正操作
- `include/uart_servo_mode.h` / `src/uart_servo_mode.cpp` — UART 控制端點與輸入生命週期
- `include/uart_set_parser.h` — SET／SET2／SYNC 分行、逾時與非法指令檢查
- `include/loop_metrics.h` — wrap-safe deadline、服務間隔與同步操作耗時
- `include/async_lora_tx.h` — 非阻塞 Client TX 生命週期，可用 fake radio 驗證
- `include/client_binding.h` — 單一 client ID 驗證與舊設定遷移
- `include/magnetic_declination.h` — GPS 位置／日期轉台灣磁偏角
- `include/taiwan_wmm_grid.h` — 產生檔，來源 `tools/make_declination.py`
- `test/` — `pio test -e native` 在筆電上跑的純邏輯測試（數學／提醒／GPS/UART 模式隔離／Servo 控制）
- `include/wifi_config.h` — 手機熱點 SSID/密碼設定（STATION 開機連線用）
- `include/web_ui.h` — 內嵌監控頁
- `docs/web-ui-design.md` — 使用者已確認的網頁設計基準；修改 UI 前必讀，包含版面、燈號、模式顯示與不得擅自恢復的項目
- `include/web_icon.h` — 分頁圖示 PNG（**產生檔，勿手改**）
- `tools/make_icon.py` — 圖示的唯一來源；改設計後重跑會更新 web_icon.h
- `tools/test_servo_slider.js` — `node tools/test_servo_slider.js` 驗證滑桿排程與模式切換
- `tools/test_motion_backend.py` — 擷取實際 HTTP／PWM／GPS gate／NVS 讀寫做主機整合測試
- `tools/test_motion_ui.js` — 資訊頁速度自動保存、GPS 選取門檻與完整頁面模擬
- `tools/test_packet_backend.py` — 實際封包建立／解析與 GNSS 主機整合
- `tools/test_debug_backend.py` / `tools/test_debug_ui.js` — 實際診斷 JSON／log cursor 與網頁錄製匯出
- `docs/health-check-2026-09-08.md` — 最新驗證狀態、歷史測試與尚未處理的問題

## RULES:
+ 網頁修改先讀 `docs/web-ui-design.md`；使用者最新明確指示優先，只改已授權範圍，並同步規格與驗證紀錄。不得因美化／重構自行重排控制、恢復已移除功能或改變追蹤／Servo 行為。
+ 功能、修正、介面或設定行為異動須同步記錄 `CHANGELOG.md`；未定版先放「未發行」，標版時歸入對應版本並記錄驗證／部署狀態。
+ build 與驗證由 agent 執行，輸出以結果摘要為主；使用者明確要求不 build 時不得編譯。
+ 使用中文回答,也可以中文夾雜,不用將專有名詞刻意翻譯

## 協作原則：保持批判態度
- 主動檢查使用者與 agent 自己的假設；不因為某個方向被提出、先前建議過或已經實作，就把它當成正確答案。發現反例或更好的方向時，直接說明證據與理由。
- 先看整套 GPS／LoRa／追蹤流程與相依關係，再提出有優先順序的一致方案；不要只隨每個提問零碎增加建議。
- 清楚區分已知事實、推測、主機測試、實機與外測結果。資料不足就指出缺口，不把設定寫入、成功 build 或接收封包當成精度與穩定性的證明。
- 現階段優先簡單、穩定、方便 debug，讓問題可見並學習原因。保留原始資料與失敗原因，避免靜默降級、掩蓋異常，或為低機率情況堆疊複雜狀態機；耗電優化後置。
- 已確認的工作自主完成並驗證；新發現若需要改變尚未確認的產品行為，先提出取捨，不以假設安全或可靠為理由自行擴大範圍。

## 外測診斷
- `docs/field-diagnostic.md` — 無筆電外測流程、內建 Flash 邊界、UTC／原始資料與 USB 匯出。
- `include/field_diagnostic.h` / `include/diagnostic_store_codec.h` / `src/diagnostic_store.cpp` — 診斷版專用，不改正常版 2 Hz。
- `include/trip_log.h` / `tools/test_trip_logging.py` — Client trip 1 Hz 長時間記錄；SD 全程 raw UART、Flash 稀疏電源／狀態，正常版 GPS gate 不變。
- `include/power_irq.h` / `tools/test_power_irq_backend.py` — signed PMU IRQ／VBUS、clear failure 去重與動作前關機證據。
- `tools/test_pmu_battery_diagnostic.py` — Client 診斷版唯讀 PMU 原始取樣、短讀／鎖忙／初始化回傳值與資料完整性。
- `include/client_boot_animation.h` / `tools/preview_client_boot_animation.py` — Client 3 秒 OLED 開機動畫與實際 U8g2 像素預覽；不代表 GPS 狀態
- `tools/test_usb_completion.py` — 實際 SD／DIAG worker 完成回覆後立即下一指令的回歸。
- `tools/test_sd_usb_budget.py` — 錄製中 USB 背壓的有界等待、寫卡不中斷與非法輸入原始證據。
