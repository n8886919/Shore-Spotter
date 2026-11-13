# 2026-10-28 室內驗收與接續任務

本頁保留本輪對話的驗收、待修需求與下一步。先讀根目錄 [AGENTS](../AGENTS.md) 及 [助手入口](assistant-handoff.md)。

## 工作位置與範圍

- 工作目錄 `/home/nolanasd123/Shore-Spotter`；交接時 `main`／HEAD `e88b121`。
  大量已修改／未追蹤 WIP 尚未提交，包含先前文件整理及其他 session 的韌體改動。
  **同一工作目錄的 WIP 才包含目前工作，clone HEAD 不足。** 接手先看 status／diff，勿 reset、clean 或覆蓋。
- 使用 LilyGO Station **584C（22604）**、T096 Client **FB27（64295）**；先前部署紀錄為 **1.0／Client UI=6**。
  本輪未重新核對產物 hash 或燒錄；部署證據見 [維護紀錄](t096-maintenance-2026-10-28.md)。
- 使用者操作網頁及按鍵，agent 唯讀查詢 API；Station 測試保持手動。Servo 控制板／PWM 已由使用者確認 PASS，不重測。
- 最後使用者完成「位置回報中禁止選頻、再停止」流程。最後觀察的保存頻道 B、owner 584C；不是下一個 session 的即時狀態。
  重新確認 Station OLED 網址及裝置身分，不把舊 IP、boot、USB 路徑當成固定值。
- 本次僅要求交接。三項介面需求曾要求「幫我記著」，尚未實作；不因交接自行燒錄、重啟、換頻或操作 Servo。

## 已完成室內驗收

「使用者」表示操作／目視回報；「API」表示 agent 當次唯讀快照。沒有完整連續原始 log，不能回推各階段耗時。

| 項目 | 結果／證據 | 界線 |
| --- | --- | --- |
| Ready 自動認領 | 使用者確認 sleep、GPS OFF、RF OFF、OWNER 584C／BLE LINK、Surfer FB27 | 重連速度另列異常 |
| USER 短按 | 切頁、約 60 秒熄屏、再喚醒：第 2 項 PASS | 未逐像素驗收所有欄位 |
| USER 長按取消 | 約 2–3 秒放開，UP 不重置、owner／連線保留：第 3 項 PASS | — |
| USER 重啟 | 滿 5 秒後繼續按不重啟，放開才重啟；先 NONE／ADV，後認領：第 4 項 PASS | 該次未精準量重連時間 |
| 無線測試／停止 | API 當次收 **127 包／漏 0**、confirmed；使用者確認 active、GPS OFF／RF ON，再停止回 sleep、GPS OFF／RF OFF，owner／BLE 保留 | 桌上短時 probe，不是距離／定位驗收 |
| CAD 掃描 | 截圖 A **0／5**、B **1／5**，完成；API candidates error 各 0 | 不是可用率／連線品質 |
| A 啟動／保存 | 使用者「Client 已確認 #4」；API selected=active=saved=0、verified=true | 不需 GNSS fix，不代表定位通過 |
| B 啟動／保存 | 使用者「Client 已確認 #6」；API selected=active=saved=1、verified=true | 同上 |
| Station 重啟 | 使用者確認仍 B、恢復 OWNER 584C／BLE LINK | 約一分鐘及約 22 秒兩次觀察，速度待查 |
| Client 重啟再啟動 | 使用者 UP 61s 才 LINK，再「Client 已確認 #2」；API tracking／confirmed、B active／saved／verified | 功能成功，速度未通過 |
| 網頁關閉 20 秒再開 | FB27、B、sleep／GPS OFF／RF OFF、OWNER 584C／BLE LINK、啟動按鈕可用：使用者 PASS | 網頁同步，不是 BLE 斷線測試 |
| 活躍時選頻限制 | 啟動後掃描／確認選擇不可按，提示需待機，再停止：使用者 PASS | — |

## 優先問題：BLE 重連延遲

### 觀察

1. 只重啟 Station，使用者估計約一分鐘恢復。
2. 再只重啟 Station：**LINK → ADV 約 2 秒、ADV → LINK 約 20 秒**，共約 22 秒。
3. Station 保持開機，只重啟 Client：**UP 61s 才 BLE LINK**。

第三次不能歸因 Station 開機連 Wi-Fi。10 秒是 Client 未連線時的廣播間隔，**不是重連期限，也不能用來合理化一分鐘延遲**。
目前無各階段時間戳／錯誤時間線確認根因。

### 已查來源及推測

- `src/t096_client.cpp`：`Advertising.setIntervalMS(10000,10000)`、`restartOnDisconnect(true)`。
- `include/station_ble.h`：同步 `scan->start(2,false)`，掃描結束才取 match，再 `client->connect(...,5000)`；失敗等 500 ms 再掃。
  **10 秒廣播與 5 秒連線等待可疑**：掃到後仍等掃描結束，可能錯過該次廣播，下一次前就逾時。
  這是時序推測，**未證實是本次 61 秒根因**。
- 已讀本機 ESP32 BLE library：`BLEScan::start(duration,bool)` 會等掃描結束；`BLEClient` 有不同 stack 分支，
  接手須核對實際 build 分支及 timeout／取消連線行為。不要直接修改 PlatformIO 套件作正式修補。
- `src/main.cpp`：Station 先連 Wi-Fi（timeout 20000 ms），另有 2 秒顯示停留，才啟動 BLE worker。
  只能解釋 Station 自己重啟時的一部分等待。

### 下一步

1. 確認當下韌體／boot、WIP 差異，讀取現有記錄，區分 searching、match、connecting、timeout、GATT、Claim。
2. 若證據不足，補有界階段計時與失敗原因；不要只看最終 connected 就推定中間正常。
3. 驗證廣播／掃描／連線期限交互作用及 timeout cleanup，再選最小修補。
   廣播改快會改耗電，不能未確認就變更；單純延長 timeout 也須考慮 target 切換、worker 可用性及取消。
4. 修補後做相應 host 回歸／build，部署前重核身分與授權，再重複實測各階段耗時。
   Host 測試不能證明 BLE 時序改善，使用者尚未約定延遲驗收目標。

入口：`include/station_ble.h`、`include/station_extensions.h`、`src/t096_client.cpp`。
回歸入口：`tools/test_station_ble_backend.py`、`tools/test_t096_ble_control.py`、`tools/test_ble_control_protocol.py`。

## 三項需求：已記錄，尚未實作

修改網頁先讀 [web-ui-design](web-ui-design.md)，同步規格／CHANGELOG／相關驗證；不重排頁面或改控制行為。

1. **Client 識別顯示**：使用者要求「Cl mac 應顯示 xx:xx:xx:xx:FB:27，現在顯示的會讓人誤會」。
   目前為實際 BLE MAC，而 Client ID 來自 `clientIdFromFicr()`，BLE 位址來自 `sd_ble_gap_addr_get()`。
   不可強改真實位址尾碼仍稱 MAC；需釐清顯示與標籤，其他 Client 依自己的 ID，不硬編 FB27。
2. **LoRa 頻道彈窗**：移除「取消」，點彈窗外部關閉；保留「確認選擇」。
   外部關閉不送出未確認選擇，點彈窗內不能誤關閉。
3. **控制按鈕文字**：「開啟 GPS／關閉 GPS」改為 **「開啟位置回報／關閉位置回報」**。
   僅文字需求，不代表變更 Start／Stop 的 GPS、RF 或 owner 行為。

三項也曾依使用者要求存入本機 memory extension notes；本頁已完整載明，不必依賴 memory 才能接續。

## 未測與戶外接續

- 使用者有「LoRa32 V4」，只推測不支援；**板型／韌體／IP／功能均未核對**。
  雙 Station 搶占／換 owner 為「未測，第二台韌體待確認」，不是硬體不支援，也不是 PASS。
- 未重新驗證 SD 匯出／CRC、溫度、電流／續航、長時間 Wi-Fi/BLE 共存及覆蓋。
- 戶外接續：GNSS 靜止／走動的新鮮定位與速度、手機對照、距離／身體遮擋 RF、超出 BLE 範圍控制、GPS／相機整體追蹤。
  絕對偏差、散布、資料年齡、收包分開記，手機不是測量真值。
- **Ready 無 LoRa RX 窗口**：遠端 Stop 後不能假設能靠 LoRa 再 Start，需回到 BLE 可控制範圍。
  Test 收包與 confirmed 不能代替 GNSS fix、定位精度或完整追蹤。

## 交接動作界線

此次只新增／同步文件及交接連結；未修程式、build、部署、commit 或 push。
先前文件整理的五組 host 回歸見 CHANGELOG，不是此次重跑，也不是延遲已修復的證據。

## 接續實作更新（2026-10-28，Station 已部署、Client 未部署）

以上保存交接當時狀態。本輪已開始 [BLE 重連查證與最小修補](ble-reconnect-2026-10-28.md)：
實際 stack 為 NimBLE，5000 參數未被採用，內部使用 30000 ms；故原「五秒逾時」只是假說且不適用此分支。
改 passive scan／match 即結束掃描，補各階段時序證據；尚不能確認 61 秒根因或實機改善。

三項需求已實作：使用者本輪確認 `CLIENT ID FB27`（動態 ID，UI=7）；頻道彈窗點窗外關閉、移除取消；
按鈕改為「開啟位置回報／關閉位置回報」。行為與驗證見新紀錄及網頁規格。
保留既有 WIP，未重跑已通過的室內操作。後續依「先燒 st」授權完成 584C application-only 更新與
Wi-Fi 恢復，新 boot／HTML／FB27 STATE／SD 錄製已確認；BLE 仍 searching，重連改善尚未驗證。
Client UI=7 未燒錄，未發送 Servo 控制指令；部署完整經過見上述 BLE 重連紀錄。
