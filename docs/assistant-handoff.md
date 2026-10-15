# 其他助手交接入口（2026-10-10）

2026-10-16 更新：E91C／F035 現況、USB SDK log 修正及實機 CRC 回歸見
[深度健檢](health-check-2026-10-16.md)。使用者已確認 Youmi＋控制板 Servo 實體驗收完成；
熱點與 GNSS 留待會面處理。以下各日期段落保留當時驗證邊界。

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

部署目標：LilyGO 584C，B 組 923.8 MHz／SF9、wire v5；已 application OTA 更新，HTML 比對、M0 輸出與持續 STATE 通過；見 [部署驗證](servo-ui-deployment-2026-10-11.md)。
主機通過 140 項 native、實際 HTTP／UART／GPS gate／NVS、圓形操作及地圖回歸。
實體 M0 接線、UART 接受、轉向、端點、帶載及戶外 GPS 精度仍待驗收。

## 先前分享／demo 階段記錄（歷史）

目前版本為 **0.6-dev／未發行、LoRa wire v5**。先讀本文件，再讀根目錄
[AGENTS.md](../AGENTS.md)；功能契約以 [features.md](features.md)、[interface.md](interface.md)、
[hardware.md](hardware.md) 和實際程式交叉確認。日期式驗證文件保留當時結果，不全部代表現行行為。

## 2026-10-10 本次新增：Youmi 控制板與 UI demo

使用者指定未來固定 **Youmi 35 kg、5–8.4 V、359°＋雙路串口控制板**。
控制板圖片提供 9600 baud、`#<00|01>P<0500..2500>!`、50 Hz PWM；使用者確認 8.4 V、
共地、TX/RX交叉、手動燈亮、旋鈕接近一整圈。GPIO／M0或M1、脈寬角度校準、TX輸出電平與ACK仍未知。
先讀 [新控制板契約](servo-controller-youmi-2026-10-10.md)，不要再以 GXServo／直接 PWM 當產品方向。
正式程式／部署尚未迁移：仍是 180°LEDC，115200 UART 是外部**輸入**，不是此板的 9600**輸出**。
使用者已確認不需外部UART輸入，產品只留手動／GPS；demo已移除，正式韌體待遷移。

[資訊頁 demo](../demo/info-page/README.md) 是獨立模擬頁，無網路 API、實機命令或持久化；
三卡為定位資訊／Station Info／Surfer Info，含待機／定位／RF／SD情境與輸入視窗；
另依使用者確認加入完整圓形0～359°軌道（端點不環繞、放開才套用）。
手機定位權限流程本次延後，不加複製診斷摘要；GPS α 只在 demo 移除，正式功能尚未刪除。
沒有 build／燒錄或實機 Servo 試轉。先前製作的分享 ZIP 是此輪之前快照，不包含新 demo／硬體需求；
若分享最新專案應重新準備快照，不要把舊 ZIP 當完整最新內容。

## 目前原始碼在哪裡

最新工作資料夾是 `/home/nolanasd123/Shore-Spotter`，含尚未提交的韌體與文件。
基底 commit 為 `726ab76604d76eccc44db64736be2b97fecc7b95`（2026-10-04）；
聊天附掛的舊 worktree `1f3a/Shore-Spotter` 停留在該基底，沒有後續 RTC／SF9／電源與顯示修正。
**只分享 Git commit／clone／舊 worktree 會漏掉最新工作。** 不要用 reset／checkout 覆蓋累積的 WIP。

本次分享 ZIP 是最新工作檔案快照，包含 tracked 及非忽略的 untracked 檔案；
不是 `git archive HEAD`。`SNAPSHOT.json` 列出基底、檔案 SHA-256 與排除項目。
不附 `.git`、編譯產物、真實 Wi-Fi 帳密、私人裝置 manifest、原始路線與執行期 log；
文件副本的私人網路／完整硬體識別資訊會遮蔽，程式檔保持原內容。原主機的私人資料不隨快照搬移。
本次只整理文件與分享包，沒有重新 build／燒錄或改變裝置設定。

## 現在使用的硬體與角色

| 裝置 | 角色與能力 | 本輪狀態 |
|---|---|---|
| Heltec T096，Client F035／61493 | nRF52840、UC6580 GNSS、SX1262＋FEM、160×80 TFT；正常版無 SD、無 Wi-Fi／Web | B 組 SF9；RTC 待機修正版已於 10/10 application-only DFU 部署 |
| LilyGO T-Beam Supreme，識別尾碼 E91C | Station；ESP32-S3、GNSS、SD、Web／OTA | 10/6 改 Station；10/7 SF9、10/9 新 Client 控制已部署；本次外測來源 |
| Heltec V4 Station | ESP32-S3、SX1262＋FEM、OLED；裸板無 GNSS／SD／AXP2101 | 可接同一 Client；以內建 Flash 存逐包 RF／位置脈絡；不能由 LilyGO 的版本推定這台也已更新 |

現場 USB 埠與 IP 會改變。燒錄前核對實際 MAC／USB serial／Client ID，不用 `/dev/ttyACM*` 編號猜板子。
多 Station 可以收同一 Client；T096 每次開機由第一個有效命令的 Station 取得控制權，換控制 Station 要重啟 Client。
配對 NVS 與編譯 manifest 是不同證據，改 manifest 不會覆蓋既有 NVS。

## 現行流程與不能誤讀的欄位

- **日常兩種狀態：啟動 GPS＝Tracking；待機＝Ready。** Ready 保留 RAM／boot／序號／控制權，
  GPS 關閉、背光熄滅、SX1262 空閒 warm-start sleep／FEM 關閉，MCU 用 FreeRTOS tickless RTC 進 SystemON sleep。
  每輪 STATE TX_DONE 後約 30 秒再發 STATE，之後聽 85 ms；偵測 preamble/header 才延長至最多 800 ms。
  Station 下令須等這個窗口，202 是 pending，匹配的後續 STATE 才是 confirmed。
  純電池啟動實測 30.776 秒；正常約一輪、約 31 秒，並非保證的 30 秒硬期限。命令逾時 65 秒。
- **Tracking 才開 GNSS。** UC6580 輸出配置／epoch 已觀察到 2 Hz；RF 最早時槽 500 ms。
  STATE 與診斷占時槽，不保證每秒 2 筆 DATA。DATA 沒有 ACK，最新有效 epoch 才送，失效通知一次。
  TEL／DIAG／GNSS_DIAG 在 Tracking 每 5 秒輪替一種，各約 15 秒；Ready 不送這些診斷。
- `test` 是無 GPS 的 RF probe 診斷；舊 `store` API 轉 Stop／Ready。正常流程已取消 12 小時 SystemOFF／Storage。
  開機回 Ready，不自行啟動 GPS。舊版 SystemOFF／USER／BLE OTA 記錄是歷史，不能套用到現行 RTC 流程。
- Station Servo 的手動／GPS 與 Client 的 Ready／Tracking 是不同狀態；Client start／stop 不切 Station 模式。外部 UART 輸入已於 10/11 移除。
- `charging` **只表示 USB VBUS**；WOUT 走太陽能接口不會因此變 true。紅色充電燈、ADC 電壓趨勢、
  實際淨充電電流分別是不同證據。百分比為 3.2–4.15 V 線性估算，非校準的 SOC。
- 接線：無線 PCB **WOUT → T096 太陽能接口，PCB BAT 留空，電池 → T096 電池接口**。
  使用者提供 1,000 mAh、4.2 V 過充／2.7 V 過放保護板。韌體三次有效 <3.2 V、無 USB 時停止 GPS／新 RF；
  每 60 秒檢查，三次 ≥3.5 V 回 Ready。保護板斷電後 MCU 無法定時醒來，恢復仍待實測。

完整待機時序、USB／ADC 造成的提前喚醒和實機限制見 [RTC 待機驗證](t096-rtc-standby-2026-10-09.md)。

## RF 與記錄

| 組別 | 頻率 | SF | 共用參數 |
|---|---:|---:|---|
| A | 923.2 MHz | 10 | BW125 kHz、CR4/5（RadioLib 數值 5）、sync 0x12、wire v5 |
| B，本輪 T096／Station | 923.8 MHz | 9 | 同上；DATA 18 B、STATE 28 B |

唯一 RF 設定來源為 [config/rf-groups.json](../config/rf-groups.json)。SF9 與 SF10 不互通，
改一台不能遠端更新其他 Station。頻率／chip drive／FEM 設定不是法規符合性或天線端功率實測。

使用者已選 **Station SD 記錄、Axiom 停用，不需 log server key**。LilyGO 開機自動記錄；
V4 沒有 SD，改用有限容量 Flash。T096 正常版沒有 SD／全程原始 NMEA log，不能把 T-Beam Client 的 SD 功能套上來。
匯出前核對身分，避免 DTR／RTS／HUPCL 重置；停止錄製、CRC 核對、完成後恢復並確認新鮮狀態。
`sample_dropped` 是狀態快照遺失，不是 RF 漏包；判 RF 使用原始 packet events／DATA 序號／CRC。

## 已有證據與仍未通過項目

| 項目 | 已觀察結果 | 限制／文件 |
|---|---|---|
| SF9 更新，10/7 | 同步更新 E91C／F035；桌上 110 probes 無 counter 缺號、SD CRC 核對 | 不能外推海面／遠距；[部署紀錄](sf9-field-pair-2026-10-07.md) |
| TFT，10/6 | 修正 Heltec plugin offset 與 SPIM2 初始化；使用者確認文字不再斜著錯位 | 歷史部署；[控制筆記](t096-controls-2026-10-05.md) |
| WOUT 充電，10/7–8 | 正確接線紅燈亮；放座末段中位 4.088 V，離座後觀察；歷史電池接手通過 | 不是電流／容量量測；[電源紀錄](t096-power-validation-2026-10-08.md) |
| RTC，10/10 | 修正睡眠後 TX 前需明確 standby；純電池 120.944 秒、5 筆增量 STATE、同 boot；Start 30.776 秒、Stop 5.399 秒 confirmed | USER／放座切換與低電恢復尚待本版驗收；整機電流／續航未量；[RTC 紀錄](t096-rtc-standby-2026-10-09.md) |
| 帶殼內建 GNSS，10/10 | 首次確認 Tracking 後約 61 分鐘才見有效位置；161 DATA seq 0–160 全收，但速度旗標全無效 | **定位與速度品質未通過外測**，不直接判天線故障；[本次外測分析](t096-field-validation-2026-10-10.md) |
| Servo／防水／海面 | 有控制程式與歷史主機／桌上證據 | 沒有本次下水、防水壓力、海面 RF／定位或完整實體追蹤驗收 |

主機測試、build、部署、使用者目視、電流量測與戶外實測要分開記。
過去 native 139/139 與相關回歸／雙端 build 結果見日期式文件；本次文件整理未重新執行它們。
HTTP 曾有讀取逾時，成功命令確認不能代表 HTTP 完全穩定；USB blocked_ms 不能代表真實睡眠電流。

## 下一步已知方向

使用者傾向改外接 GNSS 天線，**尚未改硬體或選定天線型號**。Heltec 預設 LDS 路徑，
需依官方選路圖移焊電阻才能啟用 GNSS IPEX；GNSS／LoRa 接頭不同，主動／被動、供電與 L1／L5 相容性仍要核對。
見 [hardware.md](hardware.md#t096-外接-gnss-天線2026-10-10-規劃)。

下一輪優先同地點、同韌體、固定朝向的帶殼／開殼／外接天線對照，記首次定位與有效比例、固定點散布。
若持續異常，需 T096 原始 RMC／GGA／衛星訊號診斷，分辨模組與 parser。
本次手機與 Client／Station 是否全程同位置攜帶、吃飯時 Client 擺位仍未確認；手機是對照，不是測量真值。
先讓 GPS 失效原因可見，再做省電優化，不用平滑／道路吸附掩蓋原始跳動。

## 接手時的入口與驗證

| 要看什麼 | 檔案 |
|---|---|
| 規則／版本／現行入口 | [AGENTS](../AGENTS.md)、[CHANGELOG](../CHANGELOG.md)、[function-entrypoints](function-entrypoints.md) |
| T096 主流程／控制時序 | [t096_client.cpp](../src/t096_client.cpp)、[client_control.h](../include/client_control.h)、[t096_control.h](../include/t096_control.h) |
| RTC／電池／TFT | [t096_idle.h](../include/t096_idle.h)、[t096_power.h](../include/t096_power.h)、[t096_display.h](../include/t096_display.h) |
| Station 下行／位置來源 | [station_extensions.h](../include/station_extensions.h)、[main.cpp](../src/main.cpp) |
| GNSS／wire／RF 活動 | [gnss_snapshot.h](../include/gnss_snapshot.h)、[protocol.h](../include/protocol.h)、[rf_reception.h](../include/rf_reception.h) |
| 配置／建置 | [device-config.md](device-config.md)、[platformio.ini](../platformio.ini)、[platformio-t096.ini](../platformio-t096.ini) |
| UI 改動前必讀 | [web-ui-design.md](web-ui-design.md) |
| 匯出／記錄限制 | [sd-log.md](sd-log.md)、[heltec-field-ready.md](heltec-field-ready.md) |

可先在自己的環境準備本機設定，不沿用別人的 Wi-Fi 密碼或 USB 埠：

```sh
cp include/wifi_config.h.example include/wifi_config.h
cp config/devices.example.json config/devices.local.json
# 填自己的網路與已核對裝置；先閱讀 docs/device-config.md。
python3 tools/device_config.py check
pio test -e native
python3 tools/test_t096_idle.py
python3 tools/test_t096_radio_lifecycle.py
python3 tools/test_t096_power.py
python3 tools/test_t096_gnss_uart.py
python3 tools/test_t096_display.py
python3 tools/test_t096_position.py
python3 tools/test_station_downlink.py
node tools/test_station_extensions_ui.js
pio run -c platformio-t096.ini -e t096-client
```

Station 依實際板型和 manifest 選 `tbeam-station` 或 `heltec-v4-station`，不要因預設 group 相同就認定配對正確。
以上是接手用驗證命令，不代表本次已執行；build 不會自動驗證戶外 GPS／RF。
T096 平台為 nordicnrf52 10.10.0；`lib_deps` 的 RadioLib `^7.1.2` 是浮動範圍，
本次修正時實際解析到 7.8.1。重新解析依賴要記錄版本、驗證睡眠→standby→TX 契約，不能只靠 0.6-dev 名字認定同一 binary。

## 私人證據與共享範圍

原主機 `.local/state/shore-spotter/field-validation-20261010/` 保留 10/10 原始 GPX／KML、
三份 SD 匯出、解析腳本、CSV／JSON、路線地圖與 report。電源／RTC 原始證據另在日期式 validation 資料夾。
這些不在分享包內；本文件的統計摘要不取代原始證據。需要逐點重分析時由使用者另行提供。
沒有替使用者向其他助手傳送、雲端上傳、commit／push、公開路線或部署新韌體。
