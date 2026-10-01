# Station／Client SD 循環記錄

2026-09-23，0.6-dev，LoRa v5／SF10、Client 發送節奏不變。部署結果見 CHANGELOG.md。

Station 插卡開機自動錄製，不需 Wi-Fi、Axiom、Station GPS 定位或 GPS 控制模式。
Client 一般版預設自動待命，僅 GPS 達 Good／OK 時錄製；見下節。
`tbeam-client-trip` 例外：無定位也持續記錄原始 UART hex、Client epoch 與 TX，session 標示
`profile=client-trip-1hz`、`gps_gate=false`；詳見 [field-diagnostic.md](field-diagnostic.md)。
獨立診斷版會依比較階段開關 SD，原始 NMEA 另存內建 Flash，見 [field-diagnostic.md](field-diagnostic.md)。
板載 SD 使用 BLDO1 3.3 V、獨立 HSPI（1 MHz）：SCK 36／MISO 37／MOSI 35／CS 47；
開機會關閉／重新開啟 SD 供電，清掉 USB 重啟後卡片仍 busy 的狀態，不在寫入途中自動重試。
同匯流排 IMU CS 34 保持 HIGH。LoRa 保留原本 global SPI／GPIO12、13、11、10。
一般啟動只掛載既有 FAT 檔案系統，**不自動格式化**；不支援的格式／卡片錯誤會明示。
兩端都啟用循環記錄：空間不足時，只刪除已辨識為本程式的最舊 log，保留卡片其他資料。

## Client GPS 與省電政策

- GPS 有有效位置、最新 epoch 到達未滿 2 秒、至少 6 顆衛星、HDOP 有效且 ≤ 3 才錄製。
  RMC／GGA 同 epoch 的短暫先後到達保留 150 ms 配對時間，不因每個 RMC 暫缺 GGA 就反覆切電。
- 無定位、過期或品質不合格時立即停止接受新記錄；背景排空既有 queue，寫入一筆 `gps_pause` 原因快照，
  同步／關檔／卸載後關閉 **BLDO1 SD 電源**，SPI 輸出改為輸入，避免由訊號線反向供電。
  GPS、LoRa、OLED／環境感測器保持原有行為。排空期間仍會有最後一小段寫入，不能直接切斷未同步檔案。
- 恢復合格後自動開 SD 電、掛載並建立新段；`gps_resume` 包含當下累計 GNSS／TX 計數。
  未錄製期間不保存每個事件，不能還原該期間完整 GPS／TX 時序；恢復快照只能提供累計差額。
- I/O／掛載／供電錯誤會停錄並保留原因，不無限重試耗電。修正問題後用 USB `SD START` 重新待命。
  `SD STOP`／LIST／READ 會解除自動錄製，直到 START 或重開機；START 不繞過 GPS 品質門檻。
- USB STATUS 不開卡電；LIST／READ／明確 FORMAT 可以暫時開電，操作完再關閉。
- 背景 worker 操作 SD；主循環只做有界資料複製／zero-wait queue，無 SD 掛載、刪檔、寫入或同步。
  SD 電源操作也在背景，完整 PMU 交易與主循環讀取／按鍵／OLED／LoRa 電源操作互斥；
  主循環不等鎖，忙時延後；底層 I²C 交易仍使用既有有限逾時。
  閒置 worker 每 200 ms 醒來檢查命令，錄製中每 20 ms 服務；不能宣稱整機零影響或固定省電百分比。

Client 的 `client_event` 記錄 GPS epoch／開始及停止原因、E7 以上精度座標、品質／速度／航向、RMC/GGA、
GNSS 累計錯誤、UART 積壓、2 Hz 實測率、電池電壓、heap、主循環最大間隔，以及每次 TX 開始／結果的
原始 bytes、類型、seq、開始／結果時間與 RadioLib 錯誤碼。沒有保存所有原始 NMEA 字串。
`tx_sent` 僅表示 Client 本地完成發送；是否接收成功仍需對照 Station log。

## USB 操作

SD 功能不增加網頁或 OLED 操作。電腦接上 Station USB 即可透過 `tools/read_sd_log.py` 讀取，
Client 也用相同指令。USB 埠須依 MAC 核對（Station 584C／Client E91C），不要只看 ACM 編號。
使用已裝 pyserial 的 Python（本機為 `~/.platformio/penv/bin/python`）：

```sh
python tools/read_sd_log.py --port /dev/ttyACM0 status
python tools/read_sd_log.py --port /dev/ttyACM0 list
python tools/read_sd_log.py --port /dev/ttyACM0 read /logs/檔名.ndjson --output /絕對路徑/場次.ndjson
python tools/read_sd_log.py --port /dev/ttyACM0 start
```

- `status` 讀狀態；`stop` 排空 queue、同步、關檔並卸載；`start` 開新段。
- `list`／`read` 先停止錄製，完成後維持停止；要繼續記錄請執行 `start`。
- `read LAST` 讀本次開機最後開啟的檔案；要讀先前場次，先 list 再指定路徑。
- 本工具在 Linux／POSIX 開啟時不切換 DTR／RTS，並清除 HUPCL，避免 USB/JTAG 意外重啟到下載模式。
  其他終端或作業系統仍可能造成重啟；指令回覆等到 setup 完成。尚未開始錄製的新卡列檔回空清單。
- 傳輸檢查完整檔案大小及 CRC32；失敗保留 `.partial`，成功才建立正式檔名，拒絕覆寫本機檔案。
- 卡片須有可掛載的 FAT。**一般啟動不格式化**；使用者明確同意清空後才可執行
  `python tools/read_sd_log.py --port /dev/ttyACM0 format --erase`，重建單一分割區並強制 FAT32。
  這是快速格式化，不是安全抹除資料；128 GB 卡經 SPI 建立 FAT 可能需數分鐘，完成前勿重啟；
  USB 工具每個格式化階段最多等 15 分鐘，完成後需 `start`。
- 拔卡前執行 `stop`，確認 `state=stopped`。長按 PWR 會要求停止，最多等待 2 秒。
  突然斷電可能損壞 FAT／檔案，不保證僅損失最後 5 秒。
- OTA 啟動會停止錄製；失敗後需重新開始。可回收的空間不足或 I/O 錯誤停止寫卡並保留原因。

USB ASCII 指令為 `SD STATUS/START/STOP/LIST`、`SD READ <path|LAST>`；
格式化必須完整指定 `SD FORMAT FAT32 ERASE`，沒有自動格式化路徑。
回覆使用 `@SD` 前綴；讀檔為 `@SD BEGIN <bytes> <path>`、指定長度原始 bytes、
`@SD END <crc32>`。傳輸期間暫停一般 USB log 輸出，不阻塞 LoRa／Servo 主迴圈。
SD／DIAG 完整回覆後可立即送下一命令；共用 parser 有一筆完整 pending buffer，前一 worker 尚在 cleanup 時
保留命令並停止消耗後續 bytes，釋放串流後再派送，不把 BUSY 文字插入原始檔案 body。
`usb_pending`／`usb_deferred`／`usb_queue_errors` 可檢查背壓；仍是逐筆 request／完整 response，不支援無限管線命令。
SD 錄製中，每次 USB write 採 50 ms 絕對期限，避免電腦未接收時占住寫卡 worker；超時增加
`usb_write_errors`，不停止 SD，也不假稱回覆成功。未錄製的 LIST／READ 維持 3 秒無進度逾時。
格式錯誤附 `rx_len`、`overflow` 與前 32 bytes 的 `rx_hex`，以辨識開機交接／非法輸入，資料不足不猜原因。

## 檔案

新檔為 `/logs/<16 位十六進位遞增序號>-0000.ndjson`，每行一個 JSON。
每段約 32 MiB；開機／重掛載掃描最大序號，使用 exclusive create，不截斷同名檔。
每次開新段前檢查空間，要求空閒至少 **卡片檔案系統的 5%（最低 64 MiB）＋一個 32 MiB 分段**。
不足時依序刪除最舊已關閉的本程式 log，直到足夠；不碰正在寫的檔案及其他檔案。
辨識需同時符合嚴格檔名與第一行 session 標記；只有檔名相似、內容不是本程式的檔案也保留。
FAT 空間檢查失敗／刪檔失敗／無可刪檔案都明確停止，不反覆嘗試寫滿卡。

舊版 `<8 位 boot_id>-<4 位分段>.ndjson` 也可回收：先核對舊 session 格式，再按檔案修改時間排序；
同時間以檔名排序。舊版沒有可靠 UTC／開機順序，這部分不能保證精確時間順序；新序號檔則不依賴 UTC。
停止再開始會建立新段。記錄包含座標，請按私人測試資料保管。

| kind | 內容 |
|---|---|
| `session` | build、韌體／協定／RF 參數、boot ID、記錄政策 |
| `lora_packet` | 每次接收的事件 ID、接收 IRQ 時間、解析／拒收結果、RF 錯誤碼、RSSI／SNR、原始 bytes（最多 255） |
| `station_snapshot` | 每秒 GPS／Client／追蹤命令／電量／記憶體／各服務耗時等詳細快照；本地 schema 3 使用 station_gps／station_gnss／station_average |
| `client_event` | Client GPS epoch、恢復／停錄原因、GNSS 診斷與 TX 開始／結果／raw bytes；僅合格 GPS 錄製期間及最後停錄事件 |
| `state_change` | 相鄰快照間偵測到的連線、模式、GPS、PWM 等狀態變化；不是任意瞬間狀態的完整歷史 |
| `text_log` | 既有 LogTee 文字（長行可拆成多筆），含有限的開機 log 重播 |
| `sd_status` | 每次定期同步／停止前的 SD 計數、耗時與錯誤狀態；該行計數尚未包含這次同步 |

Station 改名不重寫卡上的歷史檔案。USB 工具逐 byte 匯出，因此舊的 `server_snapshot`／schema 2
仍可完整讀回；離線分析須接受兩種 kind，並將舊 `server_gps`／`server_gnss`／`station`
分別對應新 `station_gps`／`station_gnss`／`station_average`。Axiom 上傳另保留 schema 2
相容路徑以保護既有 dataset 欄位上限，詳見 [Axiom 記錄](axiom-log.md)。

原始 CRC 壞包保留，但不送進位置／Servo 解析；壞包的 ID／seq 不可當成可信來源。
未收到的 RF 封包沒有 bytes 可存。其他 radio 讀取失敗不假造 raw 或 RSSI／SNR。
SD 原始 bytes 獨立於網頁 64-event ring；網頁與 Axiom 仍維持原本最多 36-byte raw。

`boot_id + ms`／`sample_ms` 是離線時間基準。未取得有效系統 UTC 時 `_time=null`，
不使用 1970 年假時間；取得 UTC 後只標記後續快照，不回填舊資料。
沒有啟動 Axiom／其他校時來源時，不能假設離線檔案有絕對日期。
開機重播文字的 ms 為重播時間，不是原始每行產生時間。

## 效能與驗證界線

主迴圈只做 bounded value copy／zero-wait queue；JSON、掛載、寫入、同步、讀回、
輪替及關檔皆由 core 0、priority 1 worker 執行。queue 分開保留 32 筆封包、16 段文字、
2 筆快照；Client 另有 32 筆事件 queue；16384-byte worker stack、8192-byte encoder、4096-byte batch。
每秒寫出未滿的 batch；每次 POSIX write 最多 512 bytes，避免 SPI multi-block write 路徑，
但仍用 4096-byte batch 聚集資料。每 5 秒 `fsync`；同步後讀回最後最多 256 bytes 比對。
這是檔案系統讀回證據，不是斷電耐受、全卡健康或 RF 外測保證。

`packets/texts/samples` 表示已編碼加入寫入流程，不等於每筆已持久化。
`bytes` 是 write 成功接受的位元組；`synced_bytes` 是完成同步及讀回的累計位元組。
`unconfirmed_bytes` 是錯誤時尚未確認或捨棄的尾端資料量，重試不把它算成已同步。
累計 bytes／synced_bytes／unconfirmed_bytes 使用 64-bit，長時間循環記錄不在 4 GiB 回繞。
`overwrite`、`deleted_files/bytes`、`free_bytes_at_open` 顯示循環政策、刪除統計及最近開檔的空間；
Client 另有 `auto_enabled/gps_allowed/card_powered/client_records/client_dropped`。
`*_dropped` 為 queue／採樣略過／錯誤排空計數；I/O 錯誤也可能破壞先前 batch 的尾端，
不能只靠 dropped=0 宣稱整場完整。兩核心仍共用 CPU 資源／記憶體／電源，不能宣稱零影響。

OLED 啟動加入 ALDO1 readback、0x3D／0x3C probe、一次 Wire 重建與 100 kHz 初始化；
無回應就停止顯示寫入並保留錯誤，避免反覆 I²C 錯誤拖慢主迴圈。
`initialized` 只代表初始化／I²C 回應，是否實際亮屏仍需目視確認。

## 主機驗證

`python3 tools/test_sd_backend.py` 編譯實際 worker／encoder，模擬 queue 飽和、離線 UTC、
完整 255-byte CRC raw、JSON 跳脫、停止／重啟／輪替、掛載／開檔／寫入／同步／讀回失敗，
及 batch 中途短寫的已寫／未確認計數（共 13 個情境）。
另驗證 USB 停止／列檔／完整內容與 CRC、路徑拒絕、部分 USB 寫入，及僅明確指令可格式化為 FAT32。
另執行 native、packet/debug/Axiom/motion backend 與 UI 回歸，build Client／Station／Station OTA。
`python3 tools/test_client_sd_backend.py` 使用實際 Client 產生器、SD worker、encoder，覆蓋品質／配對／過期／
millis 回繞、GPS 停錄／恢復、關卡電、queue 飽和、錯誤不重試、舊檔回收與保護其他資料。

## 2026-09-22 實機驗證

Station 584C 已 USB 燒錄並通過 Flash hash 校驗，使用者確認 OLED 已亮；原先 I²C 錯誤的實體根因未定。
原卡無法掛載；經使用者允許清空後，已重建單一 FAT32 並重新掛載，容量 124,415,639,552 bytes。
格式化保留 `FF_MAX_SS` 工作記憶體容量、以 512-byte 批次清 FAT；避免 FatFs 清分割表時越界。

錄製後停止、列檔、USB 下載 `/logs/27e47e48-0000.ndjson`：**122,988 bytes、CRC32 `4b52d1b3`**，
全部行可解析，共 31 筆快照、30 筆文字、7 筆 SD 狀態、各 1 筆 session／state change。
續錄至 135 筆快照、累計 524,933 bytes；30 次同步／尾端讀回、寫入錯誤與各類 dropped 皆為 0。
這段 worker 最大 write 42.830 ms、sync 34.726 ms；主迴圈 snapshot capture 最大 1.093 ms，
worker 最少可用 stack 7,108 bytes。其他觀察窗有快照採樣略過，非每秒絕不漏樣保證。
此次沒有接收到實際 LoRa 封包，逐包／CRC 記錄已通過主機測試，戶外 RF 與 Servo 實體追蹤尚未驗證。
最終白底／圖內控制／底部提醒版 USB 燒錄後恢復錄製；開機約 97 秒時寫入／同步正常，
19 次同步／讀回、2 次快照略過、封包／文字 dropped 與 I/O errors 為 0。
Station 韌體 SHA-256：`45d0ad042ec35ee9bbef8feea57a2bb6501f262a4ebd162b29e73c1c4ad14226`。

同次開機稍後曾停止於 `write_failed_or_card_full`：332 筆快照、1,283,584 bytes write、
1,283,273 bytes 已同步、3,638 bytes 未確認／捨棄尾端，68 次同步後累計 1 次 I/O 錯誤。
錯誤發生時已關檔卸載，後續 UI 更新前以 USB 查到；不能將上述短期通過視為長時間 SD 穩定性證明。
錯誤名稱合併短寫／卡滿，未證明是實際容量已滿，也未定位卡片、供電或 SPI 驅動哪一層。

2026-09-23 凌晨改用最多 512-byte write 後，錄製 2 分鐘通過：
`/logs/0552be78-0000.ndjson` 共 **460,157 bytes、CRC32 `aa0e2c96`**，USB 下載與全部 JSON 行解析成功；
包含 118 筆 snapshot、85 筆文字、25 筆 SD 狀態及 session／state change 各 1 筆。
26 次同步／讀回，I/O errors=0、packet/text dropped=0、sample skipped=2；
worker 最大 batch write 49.675 ms、sync 31.977 ms，主迴圈 capture 最大 1.107 ms。
下載後已開新段並恢復錄製。此為短期改善證據，尚未重跑整場戶外或長時間卡片測試。
最新 Station 韌體 SHA-256：`89046cc8e18705618a048fbad47e019c0021223bd86c23a403f04c8a1cbca888`。

## 2026-09-23 循環記錄版部署

Client E91C 已 USB 燒錄並通過 hash 校驗。現有卡可掛載，容量 31,104,958,464 bytes，
尚無 `/logs` 時列檔回空清單；沒有格式化或為驗收繞過 GPS 品質門檻。
最後 boot ID 為 2884068359，`overwrite=true`、`auto_enabled=true`、`gps_allowed=false`、
`card_powered=false`、`errors=0`。USB 列檔暫時開電，結束後關電並重新 START 待命。
Linux USB 工具連續開啟兩次也已確認 boot ID 不變。
GNSS 曾觀察到 2 Hz epoch／RMC／GGA，但當時無合格定位；更新率不代表定位品質。
Client 實際 GPS／TX 記錄、戶外 RF、整機電流與滿卡回收仍未做實機驗收；回收與停錄恢復已通過主機測試。

Station OTA 接受邀請後，TCP 3233 回連未到達 Linux；其後使用者接上 Station USB，已改 USB 燒錄
584C 並通過 build／hash 校驗。燒錄前舊場次 173,237 bytes 全部同步，未格式化記憶卡。
新 boot ID 2154012129，自動建立 `/logs/0000000000000001-0000.ndjson`，
停止後 111,304 bytes 全部同步；USB 讀回 CRC32 `457bb3be`、全部 JSON 行可解析。
內容有 27 筆 Station 快照、35 筆文字、3 筆 LoRa 事件、7 筆 SD 狀態及 session／state change 各 1 筆。
I/O errors=0、packet/text dropped=0、sample skipped=4；快照略過不等同 RF 丟包。
讀回後已 START，恢復 `/logs/0000000000000002-0000.ndjson` 記錄、`overwrite=true`，
累計 bytes=139,482、synced_bytes=135,872（仍在錄製，尾端等待定期同步）、errors=0。
實機 HTML 與目前來源一致，track API 正常。滿卡回收仍是主機測試證據，沒有把實卡寫滿或做戶外追蹤驗收。

- 已燒錄 Client SHA-256：`e472c613f211b98f46c1fc1a4fd584b78139f59fc013166d2347bab28dd42e92`。
- 已燒錄 Station USB SHA-256：`139927a14937df2e2ab481f6f4a5522df7d23d496a55263da991e4a63182cda8`。
- 同功能 Station OTA build SHA-256（此次未 OTA 部署）：`548301d97468b3664940ca9eb77c8ccd3b1551e5d98fa07333bcc83bf173fd5c`。
