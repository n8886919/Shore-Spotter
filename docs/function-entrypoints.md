# 功能入口盤點（更新至 2026-10-10）

## 2026-10-11 現行控制與介面

正式原始碼已整合 Youmi 35 kg（5–8.4 V、359°）＋串口控制板，固定 **M0**。
Station UART2 **9600 8N1、TX GPIO43、RX 不配置**；`#00P0500!`～`#00P2500!`，無換行。
`include/servo_controller.h` 合併最新目標、去重、最多 50 Hz 非阻塞送出；控制板自行產生 50 Hz PWM。
角度 0～359°，中位 **179.5°／1500 μs**；開機輸出中位、預設手動。沒有 ACK 或實際角度回授。
UART 寫入成功只證明 Station 交給 UART，不能證明控制板接受或 Servo 到位。
只留手動／GPS；舊 115200 SET／SET2／SYNC 外部輸入不再編入正常 Station，`mode=uart/jetson` 回 400。
GPS α UI、預測運算與設定 API 已移除；`/api/track/prediction` 回 404，舊 NVS 鍵不讀寫。
GPS 仍使用兩端有效且新鮮的位置、磁偏角與校正，失效轉完最後有效目標；不自動切換來源。
共用速限預設 30°/s、整數 1～90，NVS `servospd` 保存；GPS 目標每 50 ms 重算。

資訊頁四區：**定位資訊／Station Info／控制板／Surfer Info**。
座標只顯示 Google Maps 連結；手填欄位由上至下經度／緯度／誤差。
卫星顯示無／少／中／多；HDOP 下方說明其為幾何指標，不能換算成已知實際誤差。
手機回報半徑、Station 30 秒 RMS 散布、實際誤差分開；真實誤差需對照已知參考位置。
Station 顯示自有溫濕度與 SD 記錄狀態，無運行時間／Servo 來源／SD 錯誤計數；
Surfer 顯示電池、GPS 開啟／關閉、STATE／DATA 年齡與 RF 品質，無 USB 供電列。
GPS 狀態只依近期 STATE；未知或過期不假裝為關閉。
控制板區顯示 M0、串口、命令角度／脈寬／字串與 UART 計數，實際角度為未知。
磁偏角移到定位資訊；速度點數字開視窗、儲存成功才關閉，指南針整數 0～359、預設 90。
「鏡頭置中」回 179.5°；停穩後填北 0°、東 90°、南 180°、西 270°重新校正（只存 RAM）。
手動完整圓軌道 0～359°，0／359 在下、90 左、180 上、270 右，放開才送、端點不環繞。
UI 刻度延續鏡頭方向反向映射 `ui=359-raw`；控制板區顯示 raw 角度；地理方位另依校正計算。
手機定位權限流程延後、不加複製診斷摘要。

部署目標：LilyGO 584C，B 組 923.8 MHz／SF9、wire v5；OTA 結果另見本次驗證記錄。
主機通過 140 項 native、實際 HTTP／UART／GPS gate／NVS、圓形操作及地圖回歸。
實體 M0 接線、UART 接受、轉向、端點、帶載及戶外 GPS 精度仍待驗收。

## 前版入口盤點（歷史，移除項目以上節為準）

以目前 `src/main.cpp` 路由註冊、`src/uart_servo_mode.cpp`、`src/sd_log.cpp` 及實際內嵌頁面交叉核對。

2026-10-10 [資訊頁 demo](../demo/info-page/README.md) 為獨立 mock，正式網頁／API未改。
新指定 [Youmi＋串口控制板](servo-controller-youmi-2026-10-10.md) 的9600 baud是新輸出需求，
下表UART仍為既有115200輸入；GPS α／舊PWM目前尚未從正式韌體移除。

| 功能 | 現行入口 |
|---|---|
| 雷達／地圖、縮放 | 圖面左上按鈕、pinch／wheel；無 GPS 也可選雷達，不因定位消失而切換視圖 |
| 資訊頁 | 圖面最左上半透明「資訊」，與右側「雷達」間隔 10 px；資訊頁「返回雷達」 |
| 地圖中心 | Station-only／Surfer-only 以該點為中心；雙端以即時中點，歷史軌跡不拉動中心 |
| 雷達方向 | 校正偏移為畫面上方；90°時 E 在上、N 在左，軌跡／目標／Servo 方向一起旋轉 |
| 手動、GPS、UART 模式 | 圖面右上依序「手動／GPS／UART」；`POST /api/servo/mode`，開機手動 |
| 手動角度 | 手動模式顯示雷達／地圖外圈上半圓；左 0°、上 90°、右 180°，數字隨把手顯示，拖動預覽、放開生效；鍵盤方向鍵；`POST /api/servo`；GPS／UART 只保留不可拖曳的圓形角度標記 |
| GPS／LoRa 燈號 | 圖面 Station GPS／Client GPS／LoRa 垂直群組；雷達自適應避開圓圈，地圖固定左側；GPS依有效fix；LoRa依90秒內任一綁定有效RF（含STATE／probe），DATA freshness另保留5秒；track超過3秒未更新灰燈 |
| 提醒 | 頁底半透明覆蓋，不佔圖面高度；「▲ 展開提醒／▼ 收合提醒」，展開高度接近下方方位圈 |
| 回中與指南針重校 | 資訊頁；`POST /api/servo/center`、`/api/track/calibrate`；開機有效偏移 90° |
| 共用速限、GPS 預測 α | 資訊頁；`GET/POST /api/servo/settings`、`/api/track/prediction` |
| Station位置來源 | 資訊頁；`GET/POST /api/station/position`；GNSS／手機／手填，手機快照只留RAM到Station重啟，不保證背景定位 |
| T096啟動／待機／RF測試 | 資訊頁；`GET/POST /api/client/control`；start／stop／test，202只是pending，後續STATE匹配才confirmed；舊store轉Stop |
| T096定時待機／低電檢查 | Ready由上次STATE TX_DONE起約30秒交換指令；取消正常SystemOFF；低電保護每60秒檢查，詳見RTC文件 |
| T096 USER／RST | USER短按亮屏5秒、長按2秒放開回Ready；RST硬體重啟，開機回Ready；不等同T-Beam PWR |
| UART 輸入 | GPIO UART `SET`／`SET2`／`SYNC`；先切到 UART 模式 |
| 單一 Client 綁定 | `GET/POST /api/whitelist`；沒有網頁表單，但 API 可觸發 |
| start／resume／pause | 既有 `POST /api/track/start`、`/resume`、`/pause`；UI 直接切模式 |
| RAM 診斷、文字 log／清除 | `GET /api/debug`、`GET/POST /api/log`；移除網頁分頁但保留 API |
| Axiom 啟停與 Token 設定／清除 | `GET/POST /api/axiom`；沒有 UI，NVS 設定仍有效 |
| SD 循環 log | LilyGO Station開機自動錄製；T-Beam Client GPS Good／OK才錄、差時排空後關卡電；USB SD STATUS／START／STOP／LIST／READ，明確ERASE才格式化；T096正常版無SD |
| V4 Station Flash診斷 | 開機自動存逐包RF／位置脈絡；USB `DIAG STATUS`／`DIAG READ`／`DIAG ERASE CONFIRM`，容量有界，非SD |
| 更新／電源按鍵 | ESP32 Station Arduino OTA、T-Beam PWR／PMU關機；T096用application-only DFU，USER／RTC低電流程與T-Beam不同 |

已移除的前端功能連同 handler／timer／緩衝邏輯一起移除：除錯錄製／JSON 匯出、Axiom 表單、
GPX／Strava、清除軌跡、Uptime 格式化與雷達圖例。不是只藏按鈕。

保留的「未校正」狀態欄位／防護是相容性狀態；目前正常開機即為有效 90°，沒有操作可將其清成
未校正。此表核對主要操作入口，不是所有實體操作的驗收證明；最新驗證與缺口見
[交接入口](assistant-handoff.md)、[RTC驗證](t096-rtc-standby-2026-10-09.md)與[10/10外測](t096-field-validation-2026-10-10.md)。

另有 `geo::fitCircle`、`geo::angleDiff` 純數學 helper 僅由 native 測試使用，目前無韌體運行呼叫；
它們不構成可以操作卻缺少入口的功能。
