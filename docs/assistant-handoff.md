# 其他助手交接入口（更新至 2026-11-14）

本頁描述 **1.0（診斷專用建置 1.0-debug），LoRa wire v5、BLE wire v1** 的現行入口。
日期式文件保存當時的測試／部署證據；後續修改不回填為舊版已驗收。
先讀 [AGENTS](../AGENTS.md)、本頁與 [CHANGELOG](../CHANGELOG.md)，再依工作範圍查下方入口。

**11/14 Android GPS-only（尚未安裝手機）：** 手機位置只訂閱 GPS provider，收樣與送出前拒絕非 GPS 來源，
避免較新的 NETWORK 估算位置覆蓋衛星定位；GPS 停用或不支援時等待／顯示原因，不改用其他來源。
5 個 JUnit 案例、APK build／16 KB 對齊通過，lint 0 errors／26 warnings；未修改或燒錄兩端韌體，戶外效果待驗。
產物 SHA-256 與驗證界線見 [App 紀錄](android-app-2026-11-13.md)。

**11/13 追加（584C 已部署）：** [Station Web 逾時與前景恢復](web-recovery-2026-11-13.md)。
修正 abort 未結束造成 HTTP 佇列永久占用，並在切 App／鎖屏回前景時重新讀取；不重送 POST。
戶外時間最後確認為 11/12 23:39～11/13 00:09，開啟回報後重啟的實機根因仍未證實；Servo 問題使用者已解決。
使用者後續授權燒錄，同一產物已 USB application-only 上板，新 boot `352284237`／實際 HTML 核對通過；
現有 WIP 與私人 Wi-Fi 值保留，F035 未更新／重啟、本輪未發 HTTP Servo／Client 指令，開機依既有行為回 179.5°。
BLE 已恢復認領但約 uptime 141 秒才觀察到；SD 文字寫入／同步正常，快照卻停止增加且跳過計數上升，均待查。

**最新追加（Client UI=8 FB27 已部署）：** USER 長按 5 秒放開切換 ACT／SLP，未被 Station 認領也能 ACT；
保留 owner／BLE／boot，RST 用於重啟與換 Station。FB27 新 boot `E4017F95`／UI=8／BLE owner 584C 已確認，
實體按鍵與文字尚待驗收；先前 UI=7 Client ID 已一併上板。主機、build 與部署結果見 [維護紀錄](t096-maintenance-2026-10-28.md)。
下列舊 USER 重啟驗收僅代表當時 UI=6，不代表此版長按切換已完成實體驗收。

**本輪更新（Station 已部署，Client 未部署）：** [BLE 重連查證與修補](ble-reconnect-2026-10-28.md)。
已釐清 NimBLE 忽略傳入 5000、實際使用 30000 ms；改 passive scan／收到 match 即停止掃描並補分段 log。
三項介面需求已實作；Station 窗外關閉與位置回報文字已上板，Client ID UI=7 尚未燒錄。
584C 已 USB application-only 恢復原 Wi-Fi，新 boot `2025037645`／HTML／FB27 STATE／SD 錄製確認；
BLE 仍 searching，實機重連速度尚未驗證。部署細節與未帶入的 WIP 見上述紀錄。

另有本輪追加 **RSSI／SNR 分開 Good／OK／Bad／Off 顯示（11/13 隨同部署至 584C）**，原始數值收進折疊。
Off 為沒有近期 RF 樣本，Station 網頁資料未知／過期顯示 `--`；分級與驗證見 [網頁設計](web-ui-design.md)／CHANGELOG。

**原接續任務：** [10/28 室內驗收與接續任務](indoor-acceptance-handoff-2026-10-28.md)。
包含已通過的使用者操作／API 證據、約 22～61 秒重連觀察、三項待實作介面需求及戶外待驗界線。
先看此頁再安排下一輪，避免重複要求已通過的室內測試。

## 工作目錄與版本

最新工作目錄為 `/home/nolanasd123/Shore-Spotter`。本次文件核對時 HEAD 為
`e88b121`（2026-10-16），後續 MCU 診斷、資訊頁、BLE 與維護 UI 仍含未提交及未追蹤檔。
**只取 HEAD、clone 或舊 worktree 會漏掉最新工作。** 開工前重新看 `git status`／diff，保留 WIP；
舊分享 ZIP 也是當時快照，不能假定包含最新 1.0。

版本唯一來源是 [firmware_version.h](../include/firmware_version.h)。採 a.b，同主版本子版要求雙端互通，不相容變更升主版本。
版本規則不取代實機相容性驗收；相同版本仍可能有不同 build／UI，
核對部署應看產物 hash、裝置身分、新 boot 與版本標記，不能只看版本字串。
私人 Wi-Fi 設定、裝置 manifest、原始位置／USB／HTTP log 與 state 備份不納入公開分享。

## 現行 Client／Station 流程

- 使用中的 Client 是 T096（nRF52840／UC6580／SX1262＋FEM、160×80 TFT），無 Wi-Fi、SD 或 PMU 電流量測。
  LilyGO T-Beam Supreme 作 Station（Wi-Fi／Web、GNSS、SD）；保留的 LilyGO Client profile 不代表仍在使用。
  Heltec V4 的硬體能力與部署須另查，不能由 584C 更新推定 V4 已更新。
- T096 開機回 Ready，GPS 關閉、LoRa 休眠；BLE 未連線時約每 10 秒可連線廣播。
  Station 自動搜尋已指定 ID，第一個有效 **BLE Claim** 取得 Client 本次開機 owner。
  斷線、Stop、Station 重開都不解除 owner，同一 Station 可重連；換 Station 先停舊 Station 搜尋，再重啟 Client。
- Start 開 GPS／LoRa，Test 只開 LoRa，Stop 回 Ready；舊 `store` API 等同 Stop。
  Ready **沒有定期 LoRa STATE／RX 窗口**；保留 RAM、boot、序號與 owner，正常流程不進 SystemOFF。
  活躍狀態仍有 LoRa STATE 與下行窗口；DATA 無 ACK。Client start／stop 不切換 Station Servo 模式。
- Web／App 經 Wi-Fi HTTP 操作 Station。綁定成功、BLE 連線、有效認領、LoRa 通訊與 GPS fix 分開呈現。
  `/api/client/control` 的 `state_source`、`supported`、`can_control` 各有用途；HTTP 202 是 pending。
  Start 要收到匹配 Client boot／owner／command 的 LoRa STATE 才確認並保存選頻；命令期限 65 秒。
  BLE 狀態不能當作收到 LoRa，舊 Ready 每 30 秒回報的等待方式不適用。
- UC6580 的 2 Hz 配置／epoch 與有定位是不同證據。Tracking 最早 RF 時槽 500 ms；
  STATE 與 TEL／DIAG／GNSS_DIAG 會占時槽，不保證每秒兩筆 DATA。最新有效 epoch 才送，失效通知一次。
  三類診斷每 5 秒輪替、各約 15 秒，Ready 不送 TEL。

細節與混版限制見 [BLE 控制](ble-control-2026-10-27.md)、[綁定 API](client-binding-api.md)、
[OpenAPI](openapi-client-control.json) 與 [interface](interface.md)。

## 維護按鍵、顯示與健康診斷

USER 熄屏短按開連線首頁 60 秒，亮屏短按切連線／診斷兩頁並重計時；
按住 1 秒出現倒數，1～未滿 5 秒放開取消，滿 5 秒放開切換 ACT／SLP（未認領亦可 ACT），保留 owner／BLE／boot；RST 才重啟並解除 owner。
本機啟動沿用 runtime 頻道（開機 A），低電保護禁止 ACT，RF 忙碌先等現有交易完成。
開機已按住須先放開；開機／低電自動畫面仍約 5 秒。舊「長按 2 秒回 Ready」已移除。

原始碼 UI=8（FB27 已部署）首頁五行為 `CLIENT ID xxxx`、sleep／active 實線與選取框、十六進位 owner＋BLE、
GPS／RF 狀態（RF 休眠顯示 OFF）、電壓＋UP；診斷六行為 VERSION 1.0、TX、ERR、SAT、AGE＋HDOP、LAST。
不顯示 USB；真實 BLE MAC 留在 USB 診斷。配對填四位十六進位 Client ID。
完整手勢與畫面限制見 [維護介面](t096-maintenance-2026-10-28.md)。

MCU 溫度是晶片溫度，TEL `humidityPct=254` 明示無濕度，不套環境告警。
接收最高值不等於 Client 全程最高，接收年齡不等於樣本完整年齡；Ready 不送溫度 TEL。
異常 NMEA 保留 4 筆有界 RAM 原始 bytes，可用唯讀 USB `DIAG STATUS`／`DIAG NMEA` 取證，
不寫 Flash。見 [健康診斷](t096-health-diagnostics-2026-10-22.md)。

`charging` 僅表示 USB VBUS；電壓與線性電量估算不證明電流、淨充電或續航。
無 USB 且三次有效 <3.2 V 進低電保護，停止 GPS／新 RF；每 60 秒檢查，三次 ≥3.5 V 才回 Ready。
保護板斷電後 MCU 不能自行定時醒來，恢復須實測。
既有接線為 WOUT → T096 太陽能接口、PCB BAT 留空、電池 → T096 電池接口；
歷史供電／充電觀察見 [電源紀錄](t096-power-validation-2026-10-08.md)。

## RF、設定與記錄

| Runtime 候選 | 頻率 | SF | 共用參數 |
| --- | --- | --- | --- |
| A／0 | 923.2 MHz | 9 | BW125、CR4/5、sync 0x12、wire v5 |
| B／1 | 923.8 MHz | 9 | 同上 |

Ready 且 BLE 已認領才可手動 CAD 掃描／選頻；選擇先存 RAM，Start 經匹配 LoRa STATE 驗證後才保存。
`config/rf-groups.json` 仍有舊 A/SF10、B/SF9 build profiles，**不是現行 runtime SF／候選的唯一來源**。
NVS `rfchannel` 保存 Station 偏好；`gpsclient` 保存接收目標，兩者不轉移 Client owner。
設定／實際硬體相容性需另核對，見 [device-config](device-config.md)。

LilyGO Station SD 開機自動記錄；session header 是開檔當時頻率／SF 快照，同檔切頻看 `[RF]` 事件。
目前部署採 SD，Axiom 停用；T096 正常版無 SD，V4 用有界 Flash。
匯出先核對裝置身分，避免 DTR／RTS／HUPCL 意外 reset；停錄、核對原始長度／CRC、恢復後確認新狀態。
`sample_dropped` 是快照丟棄，不是 RF 漏包，見 [sd-log](sd-log.md)。

## 網頁、Servo 與校正

資訊頁三區：定位資訊／Station Info／Surfer Info；詳細定位、RF、命令及 MCU 數據放診斷。
控制板卡、網頁置中、正常溫濕度／SD／版本常駐顯示已移除；SD error 仍提醒。
ID／頻道點擊開視窗，失聯保留已確認 ID；修改 UI 前必讀 [web-ui-design](web-ui-design.md)。

Youmi 35 kg＋串口控制板固定 M0；LilyGO UART2 9600 8N1、TX43、RX 不配置，
`#00P0500!`～`#00P2500!` 對應 0～359°、中位 179.5°，無 ACK／角度回授。
只留手動／GPS，外部 SET／SET2／SYNC 輸入與 GPS α 已移除。
共用速限預設 30°/s、範圍 1～90；UI 輸入整數，API 可接受有限小數並以 0.001°/s 保存至 NVS `servospd`。GPS 目標每 50 ms 重算。

先切手動、按控制板實體置中鍵並等停穩，再填指南針讀數；網頁 `physical_center=1` 同步 179.5°
並撤銷舊控制世代，校正只存 RAM，開機有效偏移為 269.5°。
舊無此參數的 API 仍要求軟體精確中位；兩者都沒有實體回授。見 [motion-control](motion-control.md)。

## 最新已有證據與待驗項目

- 10/22：584C／F035 的 MCU USB／LoRa／網頁及 NMEA 匯出紀錄見健康診斷；不能推定 F035 已升級 BLE 版。
- 10/27～28：0.7.0-dev 的 140 native＋46 Python／Node 主機回歸、兩端 build、隔離網頁預覽，
  及 584C／FB27 application-only USB 部署記錄見 [BLE 文件](ble-control-2026-10-27.md)。
  BLE 自動認領、實機 HTML、B 頻道 5 probes 無缺號、Stop 回 Ready 已記錄。
- 10/28：FB27 維護 UI=4 已部署，新 boot `4E9805E0`／標記／BLE 自動認領已記錄；
  Station 保持 manual 179.5°／60°/s。各 UI 版本 hash 與主機回歸見 [維護文件](t096-maintenance-2026-10-28.md)。
- 10/28 最新維護紀錄已記載 **1.0／UI=5 雙端部署**：Client 新 boot `14BA0BDD`、Station boot 1901137112，
  Ready／BLE claimed、SD recording；原始產物 hash 與驗證界線以該紀錄最新節為準。此輪未重做 GPS／RF Test。
- 10/28 後續 UI=6 只將診斷版本行改為 `VERSION 1.0`，紀錄已確認 FB27 新 boot `CF0225A5`／Ready／BLE claimed；
  Station 未重燒，實體文字仍待目視，見維護紀錄的「UI=6 版本標籤」。
- 10/16 健檢已記錄使用者確認 Youmi／控制板實體驗收；10/27 新增的實體置中校正仍待操作驗收。
- 10/28 後續互動驗收：USER 短按／長按取消／放開重啟、Test／Stop、A/B CAD 與啟動後保存、
  兩端重啟後 B 頻道操作、網頁重開及活躍時選頻限制通過；證據分列於
  [室內驗收交接](indoor-acceptance-handoff-2026-10-28.md)。BLE 重連曾到 Client UP 61s，速度尚未通過驗收。
- 新版仍待：多 Station 競爭、長時間重連／Wi-Fi-BLE 共存、待機電流／續航、
  全部 TFT 欄位完整目視、跨 Station 換機及戶外 GPS／RF／完整追蹤。Test 不開 GPS，不能代替定位驗收。

以上為日期式紀錄中的證據；本次文件同步不代表重新執行全套測試、build、部署或實機驗收。

## 接手時的入口與驗證

| 工作範圍 | 入口 |
| --- | --- |
| 操作與 API | [function-entrypoints](function-entrypoints.md)、[interface](interface.md)、[OpenAPI](openapi-client-control.json) |
| Client BLE／owner／按鍵 | `src/t096_client.cpp`、`include/t096_control.h`、`include/ble_control_protocol.h`、`include/t096_power.h`、`include/t096_maintenance.h` |
| Station BLE／CAD／保存 | `include/station_ble.h`、`include/station_extensions.h`、`include/station_client_link.h`、`include/runtime_radio_profile.h` |
| GNSS／健康 | `include/gnss_snapshot.h`、`include/nmea_evidence.h`、`include/mcu_temperature.h`、`tools/read_t096_diagnostics.py` |
| 配置／建置 | [device-config](device-config.md)、[platformio.ini](../platformio.ini)、[platformio-t096.ini](../platformio-t096.ini) |
| USB／記錄 | [sd-log](sd-log.md)、[heltec-field-ready](heltec-field-ready.md) |

先檢查本機設定是否存在，只在缺少時由 `.example` 建立，避免覆蓋私人 Wi-Fi／manifest。
`device_config.py check/plan` 是設定檢查，不能取代 USB 身分及硬體驗收。

```sh
git status --short
python3 tools/device_config.py check
python3 tools/verify_project.py --list
# 全套主機回歸；--prepare 會準備相依套件，不燒錄或操作硬體
python3 tools/verify_project.py --prepare
# 建置不等於部署；使用獨立輸出目錄
PLATFORMIO_BUILD_DIR=/tmp/shore-t096-build pio run -c platformio-t096.ini -e t096-client
# Station 依實際板型／私人 manifest 選擇 SHORE_DEVICE 與 environment
```

T096 platform 為 `nordicnrf52@10.10.0`，RadioLib 已固定 `7.8.1`，不再是 `^7.1.2` 浮動範圍。
10/16 建置修正與乾淨環境驗證界線見 [健檢](health-check-2026-10-16.md)，仍非跨 OS／逐 bit 重現保證。
燒錄每台都重新核對 serial／MAC／Client ID，不用 `/dev/ttyACM*` 編號猜角色。

## 歷史證據索引（2026-10-07～10-10）

| 項目 | 已觀察結果 | 限制／文件 |
|---|---|---|
| SF9 更新，10/7 | 同步更新 E91C／F035；桌上 110 probes 無 counter 缺號、SD CRC 核對 | 不能外推海面／遠距；[部署紀錄](sf9-field-pair-2026-10-07.md) |
| TFT，10/6 | 修正 Heltec plugin offset 與 SPIM2 初始化；使用者確認文字不再斜著錯位 | 歷史部署；[控制筆記](t096-controls-2026-10-05.md) |
| WOUT 充電，10/7–8 | 正確接線紅燈亮；放座末段中位 4.088 V，離座後觀察；歷史電池接手通過 | 不是電流／容量量測；[電源紀錄](t096-power-validation-2026-10-08.md) |
| RTC，10/10 | 修正睡眠後 TX 前需明確 standby；純電池 120.944 秒、5 筆增量 STATE、同 boot；Start 30.776 秒、Stop 5.399 秒 confirmed | USER／放座切換與低電恢復尚待本版驗收；整機電流／續航未量；[RTC 紀錄](t096-rtc-standby-2026-10-09.md) |
| 帶殼內建 GNSS，10/10 | 首次確認 Tracking 後約 61 分鐘才見有效位置；161 DATA seq 0–160 全收，但速度旗標全無效 | **定位與速度品質未通過外測**，不直接判天線故障；[本次外測分析](t096-field-validation-2026-10-10.md) |
| Servo／防水／海面 | 有控制程式與歷史主機／桌上證據 | 沒有本次下水、防水壓力、海面 RF／定位或完整實體追蹤驗收 |

上表保留當時結果與限制；不是現行 1.0 的驗收表。
主機測試、build、部署、使用者目視、電流量測與戶外實測要分開記。
過去 native 139/139 與相關回歸／雙端 build 結果見日期式文件；本次文件整理未重新執行它們。
HTTP 曾有讀取逾時，成功命令確認不能代表 HTTP 完全穩定；USB blocked_ms 不能代表真實睡眠電流。


## 私人歷史證據與未決問題

10/10 的 GPX／KML、SD 匯出、統計與報告留在本機 state `field-validation-20261010`；
後續各次私人 hash manifest／USB／HTTP 證據位置見對應日期式文件，不納入 Git。
10/10 的長時間無 fix／速度失效是當時觀察，不能直接判定天線故障，也不能當作目前即時 GNSS 狀態。
外接 GNSS 天線／硬體選路必須依實際板版、接頭與當前改裝狀況重新核對，舊規劃不當作現況。
手機對照不是測量真值；位置偏差、散布、GNSS 新鮮度與 RF 接收率分開評估。
