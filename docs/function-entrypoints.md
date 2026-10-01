# 功能入口盤點（2026-09-23）

以目前 `src/main.cpp` 路由註冊、`src/uart_servo_mode.cpp`、`src/sd_log.cpp` 及實際內嵌頁面交叉核對。

| 功能 | 現行入口 |
|---|---|
| 雷達／地圖、縮放 | 圖面左上按鈕、pinch／wheel；無 GPS 也可選雷達，不因定位消失而切換視圖 |
| 資訊頁 | 圖面最左上半透明「資訊」，與右側「雷達」間隔 10 px；資訊頁「返回雷達」 |
| 地圖中心 | Station-only／Surfer-only 以該點為中心；雙端以即時中點，歷史軌跡不拉動中心 |
| 雷達方向 | 校正偏移為畫面上方；90°時 E 在上、N 在左，軌跡／目標／Servo 方向一起旋轉 |
| 手動、GPS、UART 模式 | 圖面右上依序「手動／GPS／UART」；`POST /api/servo/mode`，開機手動 |
| 手動角度 | 手動模式顯示雷達／地圖外圈上半圓；左 0°、上 90°、右 180°，數字隨把手顯示，拖動預覽、放開生效；鍵盤方向鍵；`POST /api/servo`；GPS／UART 只保留不可拖曳的圓形角度標記 |
| GPS／LoRa 燈號 | 圖面 Station GPS／Client GPS／LoRa 垂直群組；雷達自適應避開圓圈，地圖固定左側；有效 fix／5 秒內 DATA 連線綠燈，無效紅燈，track 超過 3 秒未更新灰燈 |
| 提醒 | 頁底半透明覆蓋，不佔圖面高度；「▲ 展開提醒／▼ 收合提醒」，展開高度接近下方方位圈 |
| 回中與指南針重校 | 資訊頁；`POST /api/servo/center`、`/api/track/calibrate`；開機有效偏移 90° |
| 共用速限、GPS 預測 α | 資訊頁；`GET/POST /api/servo/settings`、`/api/track/prediction` |
| UART 輸入 | GPIO UART `SET`／`SET2`／`SYNC`；先切到 UART 模式 |
| 單一 Client 綁定 | `GET/POST /api/whitelist`；沒有網頁表單，但 API 可觸發 |
| start／resume／pause | 既有 `POST /api/track/start`、`/resume`、`/pause`；UI 直接切模式 |
| RAM 診斷、文字 log／清除 | `GET /api/debug`、`GET/POST /api/log`；移除網頁分頁但保留 API |
| Axiom 啟停與 Token 設定／清除 | `GET/POST /api/axiom`；沒有 UI，NVS 設定仍有效 |
| SD 循環 log | Station 開機自動錄製；Client GPS Good／OK 才錄、差時排空後關卡電；空間不足刪最舊自家檔；兩端 USB STATUS／START／STOP／LIST／READ，明確 ERASE 才格式化 |
| OTA、PWR、低電量關機 | Arduino OTA、實體 PWR 按鍵與自動電壓門檻 |

已移除的前端功能連同 handler／timer／緩衝邏輯一起移除：除錯錄製／JSON 匯出、Axiom 表單、
GPX／Strava、清除軌跡、Uptime 格式化與雷達圖例。不是只藏按鈕。

保留的「未校正」狀態欄位／防護是相容性狀態；目前正常開機即為有效 90°，沒有操作可將其清成
未校正。未發現其他具備實作、卻沒有 UI／HTTP／USB／UART／按鍵／自動排程入口的運行功能。
此為原始碼與主機回歸盤點，不是所有實體操作的驗收證明。

另有 `geo::fitCircle`、`geo::angleDiff` 純數學 helper 僅由 native 測試使用，目前無韌體運行呼叫；
它們不構成可以操作卻缺少入口的功能。
