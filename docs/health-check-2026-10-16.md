# 專案深度健檢（2026-10-16）

維持目前持續開發的專案狀態，本輪沒有正式發版或要求重整為交接專案。
後續依使用者指示修正 USB 匯出並提交目前開發狀態；最新部署結果見「USB 修正實機回歸」。
使用者已確認 Youmi＋控制板 Servo 實體驗收完成；熱點與 GNSS 留待會面處理。
檢查以 `/home/nolanasd123/Shore-Spotter` 的實際 WIP 為準；聊天 worktree 停在較舊 HEAD，
不可拿它代表目前正在使用的功能。測試、build 與燒錄證據在本機私人 validation 目錄，
原始外測座標、帳密、Flash/NVS 備份不納入 Git。

範圍：Git／工具鏈、全套既有 host regression、五種板型 build、GNSS→LoRa→Station→Servo
主要路徑與失效處理、RF／SD／UI 證據界線，以及 E91C／F035 更新後的實機觀察。
這不是形式化證明；主機測試與桌上 RF 成功不能取代 GNSS、Servo、電源與戶外驗收。

## 有優先順序的發現

| 優先 | 發現與影響 | 本次處理／後續 |
|---|---|---|
| 已納入本輪提交 | 健檢起點的 Git HEAD `726ab76` 是 10/4 基底；當時主目錄有 72 個 changed/untracked 檔案，只 clone 舊 HEAD 會漏掉現行功能 | 依使用者後續指示，將目前 SF9、RTC、TFT、電源、Youmi 與 USB 修正一併提交並推送；仍維持 0.6-dev，私人設定留在本機 |
| P1 | 新電腦解析 `espressif32@55.3.39` 失敗；本機其實預裝 pioarduino | 已改為同一版本的明確 release URL；空白 PlatformIO core 的 platform 安裝通過 |
| P1 | E91C 板上原為 10/9 的 180° PWM 版本，最新原始碼為 359° M0 串口版；兩者皆顯示 `0.6-dev` | 每台核對 build、profile、binary hash；本次部署驗證見下方，不以共用版本字串推定相同映像 |
| 已修正 | Wi-Fi 未連線時，SDK 錯誤訊息插入 USB SD 檔案串流，標準匯出連續兩次 CRC 失敗 | SDK 改經固定佇列與 LogTee；E91C 實機離線重試期間，同一舊檔連續兩次標準匯出與新 session 匯出均通過 CRC，SDK 訊息仍保留 |
| P1 | 10/10 帶殼 GNSS 外測首次有效位置約等 61 分鐘，位置跳動，速度旗標全無效 | 保留為後續首要診斷項目；需要原始 RMC/GGA、相同擺位及開殼／天線對照，不能先判 RF 或天線故障 |
| 已驗收 | Youmi UART 無 ACK／角度回授；目前數字是命令值 | 使用者於本輪確認 Youmi＋控制板 Servo 實體驗收完成；這是使用者實機確認，不是 UART 計數推論 |
| P2 | 三支 host 測試依賴 `.pio/libdeps`，空專案直接跑會缺檔 | 統一 `verify_project.py --prepare` 先裝依賴，失敗時不回報全綠 |
| P2 | `^` 依賴使新專案選到 RadioLib 7.8.1，舊 Station 快取為 7.7.1 | 固定此次解析版本，兩端重新 build／實機核對；仍非 bit-for-bit 可重現 build 保證 |
| P2 | README／日期文件保留舊 PWM／UART／SystemOFF 敘述，易被當成現在 | README 增加本文件入口、修正明顯衝突；其他日期文件須按歷史段落讀，契約以 features/interface/hardware 與程式交叉確認 |
| P2 | GNSS 的 `no_position_sentences`／RMC/GGA 0 Hz 容易被讀成模組沒有輸出，但實際計數在時間與 epoch 驗證通過後才增加 | 本次有設定回覆、最近 UART／sentence age 43 ms，卻有 missing_time 38、accepted epoch 0；應區分 raw 收到、解析拒絕、有效 epoch 三層，不在缺 raw NMEA 時判模組失聯 |
| P2 | 尚無 CI；大部分測試以 fake hardware 或擷取真實函式執行 | 共同開發後將統一入口接到 CI；任何 RF、電源、UART／SPI 初始化變更仍需實機 |

目前來源檔模式掃描未發現常見 token／private-key；真實 Wi-Fi 與 device manifest 未被 Git 追蹤，
這兩條路徑的 Git history 亦無提交。這是有限的模式檢查，不是完整歷史秘密掃描。
共享原始碼不附真實帳密、私人裝置清單、原始定位 log、Flash/NVS 備份或編譯產物。

## 本輪修正

- pioarduino 平台使用明確的 55.03.39 release URL，修正新環境的 UnknownPackageError。
- 固定此次解析的應用 library 版本和 native platform；Station 的 RadioLib 由本機舊快取
  7.7.1 對齊 7.8.1，因此需重新 build 與 RF 驗證。
- 新增 `requirements-dev.txt`、`.venv/` ignore 與 `tools/verify_project.py`。
  三支測試需要讀真實 XPowers/ST7735 source，統一入口會先檢查或安裝所需依賴。
- OTA 的私人 LAN 預設目標換為不可用 placeholder；必須顯式指定已核對的目標。
- 修正 README 的一段舊 PWM 敘述、助手入口中已過期的 UART 模式列表。
- GPS／追蹤／Servo／電源產品行為、協定與版本號維持現況；未為分享而重構、標版或推送遠端。

## 如何重跑驗證

Linux 主機本輪為 Python 3.11、Node 22.23.2、PlatformIO Core 6.2.0，需 g++。
Windows/macOS 新機尚未實測；空白 core 只另驗證 platform 的取得，沒有宣稱整台空白 OS 已驗證。

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-dev.txt
python tools/verify_project.py --prepare
```

入口執行 native 與 36 支 Python/Node 測試；`--prepare` 只準備第三方 libraries，
不開 serial、不上傳。後續可省略 `--prepare`。native 內含 140 test cases。
Firmware build 另外執行，Wi-Fi 檔案由 `include/wifi_config.h.example` 複製，
裝置設定由 `config/devices.example.json` 複製為 `.local.json` 並填入實際身分。

```sh
SHORE_DEVICE=station-lilygo-e91c pio run -e tbeam-station
SHORE_DEVICE=t096-client pio run -e t096-client
```

上面 profile 名稱屬本機設定；別人的 manifest 必須自行建立相應項目。
通用 LilyGO 預設為 A 組，本次 E91C／F035 必須透過 manifest 選 B 組 SF9。
既有 NVS 綁定優先於 manifest 預設值，實際配對看 `/api/whitelist` 與新鮮 STATE。

## 主要流程檢查

- GNSS snapshot 以同 epoch 的 RMC/GGA 組合、保留失敗原因與積壓診斷；相同 epoch 不重傳，
  invalid 轉換只通知一次。DATA 不帶 source age，Station 的位置 freshness 只能觀察接收後年齡。
- T096 Ready 約每 30 秒 STATE／聽命令，Tracking 才開 GPS。HTTP 202 是 pending，
  STATE 匹配才 confirmed；Start/Stop 不改 Station Servo 模式。每個 Client boot 只接受首個控制 Station。
- Servo 只留手動／GPS，Youmi 359°、TX43、9600 8N1、固定 M0。命令合併／去重、最多 50 Hz；
  共用限速 30°/s 預設、1～90 NVS。失效 GPS 走完最後目標後保持，不自動改來源。
- UART 發送計數不代表實際角度，開機中位 179.5°是命令；首個絕對位置命令沒有已知實際起點。
  完整行程不環繞，UI `359-raw` 與實際轉向需現場核對。
- SD packet_dropped、sample_dropped、RF 序號缺口分屬不同層；現有歷史快照遺失不能叫 RF 漏包。
  卡上已有舊外測資料，剩餘空間約 100 MB；本輪恢復錄製觸發既有循環回收，
  自動刪除 1 個舊 log、1,402,796 bytes。本次沒有格式化或另行刪檔。
  分享實體卡前自行選擇是否改用另一張卡；若需要保留歷史資料，應先複製。
- 修正前 USB 匯出只透過 `LogTee` 的 `usbTransferActive()` 暫停專案 log；SDK 的 console 輸出
  不經這一層。`WiFi.reconnect()` 失敗訊息可插入 `@SD BEGIN`／`END` 之間，
  原有 fake Serial 測試沒有模擬這個輸出來源。本輪已新增 SDK callback 與實際 LogTee 的測試，
  不能在正式讀取工具中直接丟棄任意文字來掩蓋 CRC 失敗。
- 既有 Web/OTA/LoRa 無存取控制是已記錄的設計取捨；本次沒有自行增加驗證流程。

## 本輪驗證與部署

- Host：140/140 native，36/36 Python/Node；統一入口回報 37/37 suites。初次空專案缺 library
  的三支測試在準備依賴後通過；沒有略過失敗項目。
- Build：tbeam-station、t096-client、heltec-v4-station、tbeam-client、tbeam-client-trip 五種均成功。
  依賴固定後再 build 本次 Station／Client 也成功。V4 有一個 project unused-variable warning，
  其餘主要 warning 來自 SDK/library；沒有以 warning-free 描述結果。
- 靜態容量：Station Flash 49.2%、RAM 25.2%；T096 Flash 15.0%、RAM 4.4%。
  這是 link-time 用量，不能代表動態 heap／stack 或長時間穩定性。
- 初始實機：E91C 為 10/9 build、SF9／923.8 MHz、綁定 F035，舊 PWM 手動 90°；Axiom disabled／無 token。
- T096 最終 application-only DFU 成功，套件 CRC/manifest 驗證，保留 bootloader/SoftDevice。
  新 boot 與 Station 接收的 STATE 相符；兩端仍 wire v5、B 組 SF9。
- 先在現有網路驗證新版 Station：application OTA 成功，新 boot、Youmi profile、TX43／9600／M0、
  手動 179.5°／1500 μs。HTML 與 `include/web_ui.h` bytes 完全相同，舊 prediction API 回 404，
  NVS 綁定 F035、速限 30°/s、Axiom disabled／無 token保留。
- 桌上 RF：61.1 秒新增 109 probes，total 110、missing 0、max gap 1000 ms；
  Test 17.758 秒、Start 4.730 秒、Stop 1.110 秒均由 STATE confirmed。Client／Station boot 在測試內不變。
  最後 Client Ready，Station manual；UART 僅開機中位一筆、errors 0，沒有額外 Servo 試轉。
- GNSS 20 秒短測：UC6580 可回讀 PDTINFO／CFGNAV／CFGMSG，但 4 個觀測窗 epoch/RMC/GGA 均 0 Hz。
  接收端 snapshot `no_epoch`、missing_or_invalid_time 38、rejected_sentences 54、checksum_errors 1、
  last_byte/last_sentence age 43 ms；backlog_drops/TX errors 0。這是未取得可用 epoch，
  不是已證明天線、硬體或解析器故障。raw RMC/GGA 未保存，時間欄位實際內容仍需下一輪直接觀察。
- SD：1,208,513 bytes、CRC32 `a824a8ca` 匯出成功，727 行 JSON 可解析，150 個 packet events；
  SD errors/packet_dropped 0，sample_dropped 5、deleted_files 0。快照遺失已保留，未說成全資料零遺失。
- 回復資料：前 64 KiB boot／partition／NVS 已備份；晶片 digest 核對舊 10/9 application 完全相符，
  已有可回復的舊映像。完整 8 MiB read-flash 在兩種 baud 都中斷；沒有完整備份成功的宣稱，
  未執行 erase_flash，更新走 application OTA。

最終專用熱點版本 OTA 已完成，新 Station boot `707245342`，build `Oct 16 2026 02:32:17`；
session 記錄 wire v5／SF9／923.8 MHz，收到 F035 同一新 Client boot `332061302` 的 4 筆 Ready STATE。
Wi-Fi 設定使用使用者指定的專用熱點，但本輪開機顯示連線失敗、持續重試；
新熱點的 Web 連線尚未驗證，不能把先前網路上的 HTTP 驗證算成新熱點已成功。
密碼僅保存在忽略的本機 `wifi_config.h` 及私人驗證目錄；主目錄本機設定已同步，原設定已私下備份。

最終 SD 標準匯出連續兩次因 SDK console 混入而 CRC 失敗，原始 `.partial` 保留。
第三次另存完整 USB 串流 491,771 bytes，精確識別兩行 `wifi:sta is connecting, return error`，
離線移除共 98 bytes 後得到 491,673 bytes、CRC32 `54d15928`，與板端宣告一致，260 行 JSON 全可解析。
此為保留原始證據的鑑別與復原，**不算正常匯出驗證通過**；未更改原始 SD 檔或正式讀取工具。
最後確認 SD 已恢復 recording，同 boot，errors／packet_dropped／unconfirmed_bytes 0，
sample_dropped 6、deleted_files 1；最長一次 SD write 約 1.414 秒，不能宣稱完全沒有 worker 停頓。

第一輪映像（Station 後續由下方 USB 修正版取代）：

| 產物 | bytes | SHA-256 |
|---|---:|---|
| E91C Station application | 1,645,952 | `9f645de5087783d598bc449faf3272eb5e46d93ce147cf128ab81d4aeb0e2d29` |
| T096 DFU ZIP | 122,951 | `4239c29bacb039c19a06908b83a29b3ef4a025813bc3f47411832a6ee777f2b4` |
| T096 application（ZIP 內） | 122,148 | `1b59288c3a3d839f086c717ef3b13eab687c807b02278b57d43c593c9fb0a989` |

這些仍是 `0.6-dev` 的本次驗證映像，不另建正式 release。

## USB 修正實機回歸

- 修正：ESP-IDF 的 vprintf callback 僅格式化並寫入有鎖的固定佇列，主迴圈再送入 LogTee。
  匯出期間不直接寫 USB，SDK 診斷仍進入紀錄；8 筆／每筆最多 383 bytes，溢位與截斷可見。
  ROM panic／第三方直接 printf 不在此 hook 保證範圍。正式讀取工具保持原始長度／CRC 驗證。
- Host：140/140 native＋37/37 Python/Node，統一入口 38/38 suites。
- Build：修正後 tbeam-station、heltec-v4-station、tbeam-client 三種 ESP32 角色均通過。
  新測試涵蓋實際 callback／LogTee 的 SD 與 DIAG 隔離、一般輸出、並行 producer、溢位、截斷。
- E91C application-only USB 更新與晶片 hash 驗證成功。更新前讀回 partition／NVS／otadata，
  核對 active app0 `0x10000`；只寫應用映像，沒有重寫 bootloader、NVS 或 OTA 選擇資料。
  新 boot `641061680`，build `Oct 16 2026 03:00:27`，Youmi M0、SF9、F035 綁定保持。
- Wi-Fi 維持離線重試：舊檔 491,673 bytes，連續兩次用原本 `read_sd_log.Device.read` 匯出成功，
  各約 6.9 秒，CRC32 都為 `54d15928`，與修正前板端宣告一致；沒有移除任何串流文字。
- 新 session 匯出 251,612 bytes、CRC32 `018f55a1`、152 行 JSON，包含 13 筆 SDK Wi-Fi 錯誤，
  證明診斷沒有因 USB 修正被整體關閉；2 筆 F035 Ready STATE 的 Client boot 仍是 `332061302`。
  F035 程式沒有異動，本輪未重燒 Client。
- 最後 SD recording，errors／packet_dropped／sample_dropped／unconfirmed_bytes 皆 0。
  本 boot 既有循環回收刪除 1 個舊檔、19,106,829 bytes；沒有格式化或手動刪檔。
- 最新 E91C 映像 1,646,768 bytes，SHA-256
  `32469c9c5d145effc0a83e4d11b1dc0a4b6ca0f826c7fa39a2cbe554135a400f`。

## 後續優先工作

1. 熱點／GNSS（使用者指定留待會面）：確認熱點連線；同地點／同朝向做帶殼與開殼對照，收 raw RMC/GGA、首次 fix、有效比例、散布。
   10/10 約 61 分鐘才有效定位與速度旗標全 false 尚未定因；不能先歸咎 LoRa 或單獨判天線故障。
2. Servo：使用者已確認實體驗收完成；後續配合有效 GNSS 驗證整條 GPS 追蹤鏈。
3. 測試：把現有統一入口接入 CI；RF/SPI/UART 初始化與 PMU 變更仍保留實機檢查。
4. 外測／電源：海邊 RF、整機電流／續航、低電恢復與防水，分別記錄，避免用桌上結果代替。
