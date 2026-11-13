# 功能入口盤點（更新至 2026-10-28）

適用 **1.0** 現行原始碼；按鍵、網頁與 API 入口見下表。
部署與未完成驗收見 [交接入口](assistant-handoff.md)、[BLE 紀錄](records-index.md#ble-control-2026-10-27)
及 [T096 維護紀錄](records-index.md#t096-maintenance-2026-10-28)。本表不是所有實體操作均已驗收的聲明。

## 日常操作

| 功能 | 現行入口與結果 |
| --- | --- |
| 雷達／地圖、縮放 | 圖面左上按鈕、pinch／wheel；無 GPS 仍可用雷達，不因定位消失切換視圖 |
| 資訊頁 | 圖面左上「資訊」；定位資訊／Station Info／Surfer Info 三區，詳細數值收進診斷 |
| 地圖中心 | Station-only／Surfer-only 以該點為中心；雙端以即時中點，歷史軌跡不拉動中心 |
| 雷達方向 | 依鏡頭校正與磁偏角映射，軌跡／目標／Servo 方向一起旋轉 |
| 手動／GPS | 圖面右上；`POST /api/servo/mode`，開機手動。舊 uart／jetson 回 400 |
| 手動角度 | 完整圓軌道 0～359°，0／359 下、90 左、180 上、270 右；拖動預覽、放開生效，端點不環繞；`POST /api/servo` 保留 raw 角度，UI=359−raw |
| GPS／LoRa 燈號 | GPS 依有效新定位；LoRa 依 90 秒內任一綁定有效 RF，BLE 不算 RF。`lora_fps_10s` 是 10 秒 DATA FPS，track 失聯灰燈 |
| 提醒 | 頁底半透明展開／收合；`GET /api/status.alerts`，含 SD error；正常 SD／溫濕度不常駐資訊頁 |
| Servo 速度 | Station Info 點數字編輯；`GET/POST /api/servo/settings`，1～90°/s、預設 30；UI 整數、API 可接受有限小數，成功保存才關視窗 |
| 指南針校正 | 先切手動、控制板實體置中並等停穩，再於 Station Info 輸入讀數；`POST /api/track/calibrate` 帶 `physical_center=1` 與 epoch／seq／stamp；只存 RAM，同步中位 179.5°並更新世代 |
| 舊軟體回中 | 僅保留 `POST /api/servo/center`，網頁無按鈕；切手動並按速限要求 179.5°，沒有實體到位回授 |
| Station 位置來源 | 定位資訊；`GET/POST /api/station/position`；GNSS／手機／手填，手機快照只存 RAM，不保證背景定位 |
| Client ID | Surfer Info 藍色 ID 點擊編輯、確認才送；`GET/POST /api/whitelist`，容量一台。改目標不轉移 owner，失聯保留已確認 ID |
| Client 啟動／待機 | `GET/POST /api/client/control`，start／stop；202 為 pending。Start 必須有匹配 LoRa STATE 才 confirmed，不能只看 BLE accepted |
| 無線連線測試 | Surfer 診斷的測試／結束測試，test／stop；Test 關 GPS，不保存待選頻道，不代表定位或 Servo 驗收 |
| BLE 自動認領 | Station 自動搜尋目標 ID，Client 首個有效 Claim 取得本次 boot owner；Web／App 透過 Station Wi-Fi HTTP，不直接連 Client BLE |
| LoRa 候選頻道 | Surfer Info 點頻道；`GET/POST /api/client/radio`，Ready 且 BLE claimed 才能 scan／select；A/B 皆 SF9。選擇先存 RAM，Start 的匹配 LoRa STATE 後才存 NVS |
| T096 Ready | 保留 RAM／owner，GPS 關閉、LoRa 休眠，未連線時 BLE 約 10 秒廣播；沒有定期 LoRa STATE／RX。低電保護每 60 秒檢查 |
| T096 USER／RST | USER 短按亮屏 60 秒／切連線與診斷頁；按住 1 秒倒數、1～未滿 5 秒放開取消，滿 5 秒放開 ACT／SLP，未認領可 ACT、保留 owner／BLE。RST 硬體重啟回 Ready（UI=8 FB27 已部署） |
| 換 Station | 停用舊 Station 搜尋，設定新 Station 目標，重啟 Client；核對新 boot／owner，不能只改綁定就宣稱轉移控制權 |

`can_control`、`supported`、`state_source` 分開判讀，詳見 [API](interface.md) 與
[OpenAPI](openapi-client-control.json)。BLE 連線、認領、LoRa 接收、GPS fix 與實體 Servo 動作是不同證據。
畫面詳細規格以 [web-ui-design](web-ui-design.md) 為準。

## 診斷、記錄與維護

| 功能 | 入口／限制 |
| --- | --- |
| MCU 溫度 | Surfer 診斷與 `/api/track.telemetry`；目前值／接收最高／接收年齡，60 秒逾期 unknown；不當環境溫度或電池溫度 |
| T096 原始 NMEA／MCU | 唯讀 USB `DIAG STATUS`、`DIAG NMEA`；`tools/read_t096_diagnostics.py`；4 筆 RAM 證據，重啟會清除 |
| T096 維護 UI=8（FB27 已部署） | 首頁五行 CLIENT ID、sleep／active、owner／BLE、GPS／RF、電壓＋UP；診斷六行 VERSION 1.0、TX、ERR、SAT、AGE＋HDOP、LAST。不是 USB serial 或實測電流 |
| Servo 串口輸出 | M0、9600 8N1、LilyGO TX43、RX 不配置；`/api/track.servo` 保留命令與 UART 診斷，資訊頁控制板卡已移除 |
| RAM 診斷／文字 log | `GET /api/debug`、`GET/POST /api/log`；網頁除錯錄製／JSON 匯出已移除，API 保留 |
| Axiom 設定 | `GET/POST /api/axiom`，無 UI，預設停用；Token 與網路設定為私人資料 |
| SD 循環記錄 | LilyGO Station 自動錄製；`/api/sd/status` 保留，網頁不另輪詢。USB SD STATUS／START／STOP／LIST／READ，ERASE 需明確授權；T096 無 SD |
| RF 記錄 | session header 為開檔 runtime 頻率／SF 快照，`frequency_scope=session_open_snapshot`；同檔切頻看 `[RF]` log |
| V4 Flash | `/api/flash`、USB `DIAG STATUS`／`DIAG READ`／`DIAG ERASE CONFIRM`；有界容量，不等於 SD |
| 更新／電源 | Station Arduino OTA、LilyGO PWR／PMU；T096 application-only DFU／USER ACT-SLP／RST 重啟／低電保護，各板分開核對 |
| 相容追蹤 API | `POST /api/track/start`／`resume`／`pause` 保留；UI 直接切手動／GPS |

## 程式入口

- T096 主流程：`src/t096_client.cpp`；owner／重播：`include/t096_control.h`；BLE codec：`include/ble_control_protocol.h`。
- Station BLE worker／掃頻與命令：`include/station_ble.h`、`include/station_extensions.h`、`include/station_client_link.h`。
- 維護畫面／按鍵：`include/t096_maintenance.h`、`include/t096_power.h`、`include/t096_display.h`。
- 健康診斷／NMEA：`include/mcu_temperature.h`、`include/nmea_evidence.h`、`include/gnss_snapshot.h`。
- Servo：`include/servo_controller.h`、`include/servo_motion.h`、`src/main.cpp`；RF metadata：`include/runtime_radio_profile.h`、`src/sd_log.cpp`。
- 主機回歸清單：`python3 tools/verify_project.py --list`。測試保留的 UART parser／模式隔離不表示現行 Station 還提供外部 UART 控制。

## 已取代的流程

10/9 RTC 文件保留的 Ready 每 30 秒 LoRa 交換、舊 USER 短按 5 秒／長按 2 秒回 Ready，
已由 BLE 與 10/28 維護介面取代。10/10 demo 與 10/11 四區資訊頁是歷史版本。
舊 180° LEDC、SET／SET2／SYNC 外部輸入、GPS α、網頁置中與控制板卡都不是現行操作入口。
純數學 `geo::fitCircle`／`geo::angleDiff` 保留測試用途，不代表有對應使用者功能。
