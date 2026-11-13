# 2026-10-28 BLE 重連查證與修補（Station 已部署，Client 未部署）

## 證據與結論

沿用 [室內驗收](indoor-acceptance-handoff-2026-10-28.md) 的 22 秒及 Client UP 61 秒觀察；
不重跑已通過的按鍵、Test／Stop、CAD／保存、Servo 等室內驗收。
初始查證階段沒有重啟、燒錄、發送控制命令或更換頻道；後續授權部署見文末。
唯讀 mDNS／API 確認 hostname 尾碼 584C、韌體 1.0（build Oct 28 2026 01:10:08）、
Station boot 717374395、manual；Client FB27 boot 3281373888，當次 tracking／LoRa 狀態年齡 688 ms，
BLE searching。這是查詢當刻狀態，不證明 BLE 失聯原因。RAM log 只留最近 4096 bytes，
未取得可對齊該次重連的 BLE 分段紀錄；未停 SD 或操作硬體來取證。
現有部署記錄只能證明舊版曾連線，
沒有該次 61 秒的分段原始時間線，**尚未證實根因，也未證實實機延遲已改善**。

已核對本機實際套件來源（不修改套件）：

- Station `tbeam-station` 的 ESP32-S3 `qio_qspi/include/sdkconfig.h` 啟用
  `CONFIG_BT_NIMBLE_ENABLED`／`CONFIG_NIMBLE_ENABLED`，不是 Bluedroid。
- `BLEClient.cpp` 的 NimBLE `connect(address,type,timeoutMs)` **未使用 timeoutMs**，
  以 private `m_connectTimeout=30000` 呼叫 `ble_gap_connect()`，task 最多等待該期限加 1000 ms。
  因此舊呼叫傳入 5000 不能證明實際五秒逾時，也不能據此判定 61 秒根因。
- 該分支正常由 GAP 完成事件釋放等待；task 等待逾時且未連線時呼叫 `ble_gap_conn_cancel()`；
  收到已無等待者的 CONNECT 事件會要求 terminate。這是來源行為，不是實機清理驗收。
  Library 的 connect／GATT 同步操作仍可能延後 worker 處理目標切換；主迴圈 HTTP 不等待它們。
- 舊 Station 使用 active scan，NimBLE 對可掃描 ADV 等 scan response，未收到會延至掃描完成才 callback；
  外層又使用阻塞的兩秒 scan。Client 的 service UUID 與識別 manufacturer data 都在 ADV 主封包，
  名稱才在 scan response，所以識別 Client 不需要 active scan。
- Client 廣播仍每 10000 ms；掃到廣播後建立連線可能仍需等後續廣播。即使取消兩秒等待，
  不能承諾立即連線，也不能把廣播間隔當成完成認領的期限。

## 本次最小修補

1. Station 改 passive scan；非同步啟動原本兩秒掃描，由 worker 每最多 50 ms 等候 queue。
   收到匹配 service／manufacturer／Client ID 的廣播即停止掃描並連線。
   callback 只寫固定容量 match queue，不呼叫 connect／stop 或清除掃描物件。
2. 掃描期間可偵測目標切換；scan start／stop 失敗及超過 2200 ms 未完成分別回報
   `scan_start_failed`／`scan_stop_failed`／`scan_timeout`，
   不拿失敗的掃描結果連線。連線完成後重新檢查目標，有變更就斷線、不認領舊目標。
3. 呼叫明寫 30000 ms，與目前 NimBLE 實際期限一致；未修改 library private 欄位，
   未假稱可在 50 ms 中止 connect，也沒有變更 Client 廣播／owner／GPS／RF。
4. 補 `[BLE]` 階段紀錄：Station uptime、generation、Client ID、phase、elapsed_ms、detail。
   包含 searching、match、connecting／connect_ok／connect_failed、MTU、GATT 開始／結果、Claim 開始／結果。
   match detail 為 callback 到 worker 的毫秒數；MTU detail 為協商值；GATT detail 為
   0 成功／1 無 service／2 無 status characteristic／3 無 command characteristic／4 狀態讀取或 decode 失敗。
   Claim 失敗 detail 是結果碼，並保留既有錯誤名稱；原始 stack 錯誤若有輸出仍沿用 SDK 日誌。
   只記錄狀態轉換及連線嘗試階段，不每秒印成功狀態或每兩秒印無結果掃描。
   全部經既有 `sdk_log` 固定 8×384-byte queue，由主迴圈送 LogTee／SD；overflow 有 dropped 提示。

## 驗證與下一步

已通過 10 組針對性主機回歸：`test_station_ble_discovery.py`、`test_station_ble_backend.py`、
`test_t096_power.py`、`test_t096_ble_control.py`、`test_ble_control_protocol.py`、`test_sdk_log.py`、
`test_ble_ui.js`、`test_client_control_ui.js`、`test_t096_display.py`、`test_motion_ui.js`。新增 discovery 測試直接擷取實際 callback／函式，
覆蓋 ID／service 過濾、首個 match 即返回、換 target、掃描 timeout／start／stop 失敗、舊 queue 與時鐘 wrap。
主機模擬只驗證程式流程，不是 RF 時序驗收。

隔離 mock 瀏覽器驗證窗內點選保持開窗、窗外點擊關閉、再開回到原 B 頻道；無取消按鈕。
實際 CSS viewport 回報 218 px，無水平溢出；TFT 使用實際 formatter 與 Adafruit 原字型的 mock
確認 `CLIENT ID FB27` 未截字。兩者均不代表實機更新或 TFT 目視驗收。

Station `tbeam-station` 與 Client `t096-client` build 均通過（Station 最後增量 45.10 秒、
Client 373.58 秒）。既有 SDK／USB CDC warnings 留在完整 log，並非零警告建置。
`git diff --check` 通過；此主機驗證階段未 commit／push／燒錄。
完整 log、修改前檔案、來源／產物 hash 與 mock 預覽保存於本機私人 state `ble-reconnect-20261028`。

部署前重新核對 Station 584C／Client FB27 的 serial、MAC、Client ID、目前 boot 與明確授權。
本機私人 manifest 的 Client 預設仍是 F035，不能直接將其當作 FB27 配對證據；
本輪 build 用既有 manifest 進行編譯驗證，不把產物視為已批准燒錄。實際部署前核對 NVS 與 build 預設。

取得部署授權後，僅針對尚未通過的重連計時：分別記錄 Station 單獨重啟、Client 單獨重啟，
對齊 `[BLE]` uptime 與 Client boot／UP，量 searching→match→connect→GATT→Claim。
先與使用者約定延遲目標與樣本數；記錄失敗嘗試及最慢值，不能只報最後成功。
目前不能用 22／61 秒舊觀察計算改善百分比。

## Station 部署與恢復（2026-10-28，已完成）

使用者明確授權「先燒 st」，Client 不更新。部署前核對 mDNS hostname 584C、Station boot
717374395、手動 179.5°／60°/s、FB27／B；SD 已同步且 unconfirmed_bytes=0。
使用已驗證的 BLE／彈窗產物（不納入其後另有的 RF 顯示 WIP），application SHA-256
`ca20408a1b93383791154babe9430ca8a4690f96effd7eed8e927ff016bb1552`。
OTA 回覆 OK／Success，但重啟後網路未恢復，**不可視為部署驗證通過**。

追查確認部署前漏核對 Wi-Fi 設定：先前實機建置使用私人的部署設定，之後還原工作區設定；
本輪待燒產物使用工作區的另一 SSID。已核對保存的部署設定與先前成功 application 一致
（敏感值不寫入文件）。已在隔離來源目錄以原部署 Wi-Fi 重建，保留工作區設定與 WIP，
並在 Station USB 接入後核對 serial／MAC，完成 application-only 恢復與下列驗證。
這是本輪部署疏漏，不是使用者需重新授權的功能變更。

恢復版 Station build 已通過；已再次比對原部署 Wi-Fi 值存在於產物，HTML 與本次已核准
BLE／彈窗產物逐 byte 一致，未帶入後續 RF 顯示 WIP。SHA-256：
`f0e99243ed57958cf49204a76cee7a28e49fd01c5bdef7e62fab109be8bdeb90`。產物與 `recover_usb.py` 留在私人 state 的 `deploy-station` 子目錄。
使用者確認 USB 已接後，核對 VID:PID `303a:1001`、serial／MAC `48:CA:43:57:58:4C`。
先停 SD 並確認同步、無 USB pending；讀回 partition／otadata，核對目前 app1 位於 `0x340000`，
僅覆寫該 application，未擦除 NVS／partition。Flash hash 驗證通過。
RTS reset 後 API／USB 指令未回覆，改用 esptool `read-mac --after watchdog-reset`，隨後原網路恢復。

恢復後唯讀驗證：

- Station hostname 584C，韌體 1.0、build `Oct 28 2026 02:28:31`，新 boot `2025037645`。
- 實機 HTML 88,824 bytes，去除外圍空白後與核准產物逐 byte 相同；包含頻道窗外關閉及位置回報文字。
- whitelist 僅 FB27；Client boot 仍 `3281373888`，LoRa STATE tracking／age 862 ms，B／923.8 MHz。
  這證明新鮮 STATE，未取得新鮮定位 DATA，不能據此宣稱 GNSS 已定位。
- Servo manual／179.5°／60°/s 保留；未發送動作或模式變更指令。
- SD recording，errors／packet_dropped／text_dropped／unconfirmed_bytes 均 0；
  兩次取樣間 samples 78→91、readback_checks 23→26。`sample_dropped=42` 未增加，保留此觀察，不稱全程零遺失。
- BLE 在 Station uptime 約 141 秒仍 searching，尚未取得新的成功連線／Claim 時間線；
  **修補已部署，但重連改善尚未驗證**。不把 LoRa 可控制當成 BLE 已恢復。

Client UI=7、後續 RF 顯示 WIP 及工作區另行更新的私人 Wi-Fi 設定均未納入此次部署。
未重跑已通過的室內操作，未 commit／push。原始 API、USB、Flash／watchdog log 與產物 metadata
保存於上述私人 state 的 `deploy-station` 子目錄。
