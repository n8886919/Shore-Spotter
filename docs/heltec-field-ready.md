# Heltec 外測準備與夜間驗證（2026-10-02）

本次對象是目前接著的 **Heltec V4 Station + T096 Client，group B 923.8 MHz**。
版本 `0.6-dev`／wire v5；A 組923.2 MHz保留。兩塊 LilyGO 未連接，因此只做相容 build，未改燒實體角色。
桌上測試不能取代水上距離、身體遮擋、天線方向或電池量測。

交付前已將本輪桌上Flash紀錄備份並清除，Station重新開機後確認繼續recording、drop/error為0；
T096為Ready、Station手動90°，沒有待執行命令。USB接著時不會因12小時計時自動深眠。
等到早上仍會記少量待命STATE／health，出門前可看`/api/flash`剩餘容量，無需為清除而重刷韌體。

## 出門操作

1. Station 接上原先設定的 Wi-Fi／手機熱點。開 `http://shore-<Station完整Wi-Fi MAC>.local/`；
   本機 manifest 的 `hostname: auto` 不隨 DHCP IP 改名。若熱點手機不能解析 `.local`，使用 Station OLED 的 IP。
   **iPhone／Android 熱點主機端的解析尚未實測，MAC 名稱不會消除平台 mDNS 限制。**
2. 資訊頁「用手機更新位置」開 HTTPS 定位頁並授權，手機放 Station 旁。核對位置時間與精度；
   也可用既有手動輸入位置。Station 只留 RAM，重開需重新提供；鎖屏後定位頁可能暫停。
3. T096 通電預設「停止待命」。按「開始追蹤」後等 STATE 確認（待命每30秒回報，命令不是瞬間送達）。
   室外等 Client GPS 有效；Station／Client 都有有效位置才可選 GPS 控制模式。
   實際追蹤前讓Servo回90°、停穩，依鏡頭實際指南針方向完成校正，再選GPS模式。Client 開始／停止不會改 Servo 模式。
4. 沒有 GPS 時按「LoRa RF 測試」，看實際 probe 收件／counter 缺號／RSSI／SNR。
   此模式沒有位置 DATA，DATA FPS=0 是正常，不能拿它判定 RF 失敗。結束後按「停止待命」。
5. 停止待命還可 Web 啟動；未接 USB 時12小時後進深度收納。深眠後Web不能喚醒；目前配置按鍵wake，nRF52840亦有VBUS上升硬體wake。
   無線充電接收板路徑還需接妥驗證。**充電板未接、封殼外部喚醒尚未完成，先不要把深眠功能當成免開殼方案。**

## 無 SD 時如何保存證據

V4 開機自動使用原本未使用的 SPIFFS 分區（`0xc90000`、`0x360000` bytes=3.375 MiB）記錄，
不需手機保持網頁、網路或筆電。保存逐包 RF bytes／RSSI／SNR／decoder錯誤、boot ID及接收毫秒；
每30秒補一筆 heap、loop最大耗時、接收計數、控制模式與 Station/phone位置來源。
這是 **Station 已觀測到的資料**，無法還原未收到的 Client 原始 GNSS／RF封包。

- OLED 的 `F:recording` 表示本機紀錄中（實際面板仍待目視驗收）。`GET /api/flash` 應為 `state: recording`、`dropped: 0`、`errors: 0`。容量6912 frames，第一格為ownership header。
- 後台 bounded queue，不等待Flash寫完才收下一包；10秒flush或frame填滿就寫。突然斷電可能少最後未flush資料。
- 記滿停止，重開不清除、不循環覆寫。未知分區內容也不自動erase。回家先匯出，再明確清除；出門前確認剩餘空間。
- 不更動NVS、OTA app或coredump分區；之後不可用SPIFFS格式化工具覆蓋此紀錄。

先以 USB serial/MAC 確認是 Station，避免 `/dev/ttyACM*` 插拔換號：

```sh
python3 tools/read_diagnostic_log.py --port <確認過的Station USB port> status
python3 tools/read_diagnostic_log.py --port <確認過的Station USB port> read --output /private/path/trip
# 匯出、檢查 manifest complete/CRC/dropped 後才清除
python3 tools/read_diagnostic_log.py --port <確認過的Station USB port> erase --confirm-erase
```

會產出 `.bin` 原frames、`.ndjson` 解碼與 `.manifest.json` 完整性報告。工具不透過DTR／RTS重置ESP32，
frame／傳輸CRC、分boot序號、drops及解碼錯誤均保留。下載成功不等於全程RF零丟包。

**Axiom目前未啟用**：新V4沒有ingest token；本機只有既有query token，沒有把它冒充裝置上傳金鑰。
舊Axiom非阻塞上傳程式與HTTP設定入口保留；取得原Station ingest設定或新的限dataset ingest token後，
可依 [axiom-log.md](axiom-log.md) 啟用，並以成功上傳計數／雲端讀回驗證。Flash目前負責離線保留，沒有新增Flash自動補傳雲端。

## 本輪修正與已知限制

- GNSS供電：T096 GNSS load switch上游是Vext；追蹤同時開Vext與GNSS_EN，待命才關。
  `$PDTINFO` 實讀 `UC6580I-00 / R6.0.0.0Build3700`。
- GNSS更新率：這版receiver拒絕第三參數0／100，接受`CFGNAV,100,100,1000`。
  搭配GGA/RMC divisor5，回讀一致；連續串口量到新UTC epoch/RMC/GGA為2.00 Hz、500 ms。
  10 Hz是內部navigation，2 Hz是輸出。另試`CFGNAV,500,500,1000`也明確回FAIL、回讀維持1000 ms，
  所以未採未支援的原生2 Hz設定。未寫CFGSAVE、未改星系。室內仍`no_fix`、0顆衛星；戶外fix尚待確認。
- RF排程：STATE後85 ms listen，有preamble/header才延長至最多800 ms；Station於20–40 ms間嘗試一次命令。
  沒有命令時不再白等800 ms。每500 ms最早發送一次，DATA獨立seq、只送最新epoch；STATE與診斷會占用時槽。
  「距上次STATE」是約5秒（運行）／30秒（待命）的控制回報年齡，**本來就不是0.5秒定位週期**。
- 遙測：T096補回既有TEL／DIAG／GNSS_DIAG，每5秒輪流送一種，每種約15秒；未知ADC電量仍為unknown。
  `skipped_slots`沿用忙碌延期episode計數，不是RF缺包或精確遺失epoch數。
- USB：nRF的CDC write在host連線但不讀取時會等buffer；改為完整短log可容納才寫、否則計數略過。
  USB日誌丟棄與RF丟包分開；生產追蹤不依賴USB開著。
- 新Client boot清除Station舊即時fix／GNSS診斷／probe統計與命令序號基準；Station累計RF計數保留。
  實機重新燒入T096後，Station的GNSS／DIAG由received切回false、command_id回0、舊satellite資料清空；
  再開始追蹤收到的新診斷epoch_interval_ms=500、rate bits已確認2 Hz。
  本輪GNSS開機後rejected_sentences=16／checksum_errors=1，後續觀察未再增加；未把它寫成GNSS零錯誤。

## 驗證記錄

- Native 139/139；33支Python/JS host回歸全部通過，包含實際函式、Flash worker／USB、DATA新epoch／invalid transition／獨立seq、boundedCDC。
- V4及T096 build／USB部署成功；LilyGO Client／Station相容build亦成功，未連接實體板。V4 app寫入經esptool hash驗證；T096 application DFU保留bootloader。
- V4第一次匯出22/22 frames、decode/CRC/drop/error均0。重燒後原11,264 bytes逐byte保留，新boot追加紀錄。
- 最後備份648/648 frames（331,776 bytes）；CRC、decode、序號缺口與7個boot的drop均0，
  前幾次匯出的既有bytes保持一致。匯出期間新增1 frame，未包含於這份固定起始範圍備份。
  原始備份SHA256：`a9a563ecdd8abd553044998b5e0024fe45f948b02faf1aa9b58400e8a69da179`。
- 首輪900.66秒 RF／Web 並行測試：1,641個 probe、counter缺號0、Client boot不變；前450秒USB連著但不讀。
  同時出現31次HTTP timeout，另保留失敗紀錄；不能只用RF成功宣稱網頁正常。
  Flash匯出215/215 frames，CRC／decode／drop／error均0。跨首輪前後共1,715筆同boot probe，
  counter缺號0，接收間隔median500 ms、p951,013 ms、max1,763 ms（含命令交換）。
  測得約723 frames/hour，以空容量粗估9.5小時；這是RF test資料量，不保證所有追蹤／錯誤量都相同。
  250 ms空連線修正後再測600.1秒：1,089個probe、counter缺號0、最大probe間隔1,014 ms；
  仍有3次HTTP timeout。104次額外分段HTTP量測雖全數完成，等待response header最長2.969秒，
  當時板上handler/loop最大耗時沒有增加；接著進行V4 Wi-Fi modem sleep關閉對照，同網路的20次Ping平均62.7→7.1 ms；HTTP分段測試開啟省電104次、關閉後116次皆完成，
  response header最長2.969→0.123秒。此對照支持V4關閉modem sleep以減少延遲，耗電增加未量測；
  T096不使用Wi-Fi，不受這項設定影響。相同RF負載600.08秒：1,088個probe、缺號0、最大間隔1,015 ms；0次4秒API timeout，
  但最慢成功API仍3.057秒。因此沒有將「零timeout」當成低延遲保證。
  額外8次空連線分段實驗中，7次約0.19–0.22秒、1次約2.75秒；較慢樣本TCP total_retrans=1，
  其餘0。後續改為：先給新連線100 ms等待首批bytes，之後若已有別的連線排隊便讓位；沒有排隊時仍用250 ms期限。
  這只清理尚未開始HTTP的空連線，保留已有內容與SSE。
  最後80次空連線並行壓測：首40次39成功／1次4秒timeout，成功中位0.097秒、最大2.380秒；
  再40次全成功，中位0.047秒、最大2.377秒，其中1次有Client TCP重傳。
  沒有因此宣稱網路延遲已根治；失敗樣本也保留，無法將所有長等待單歸因於Wi-Fi或HTTP程式。
  最終版本持續600.15秒：1,089個probe、counter缺號0、最大間隔1,014 ms，Client boot不變；
  前300秒USB連著但不讀。期間有2次4秒API timeout、最慢成功control請求2.505秒；
  這輪也包含上述空連線壓測，不把兩次逾時直接歸因於單一因素。Flash queue/drop/error均正常。
  結束後stop命令2.152秒由Client STATE確認，回Ready、手動90°。
- 已重現TCP空連線讓WebServer後續請求延後6.9秒，RF／主迴圈仍正常；Station新增250 ms無內容連線期限。
  關閉modem sleep前實測5次相同情境，HTTP回應0.183–0.217秒，idle清理計數從0增至5。
  最終RF並行中再測空連線：一次0.201秒、一次2.486秒，後者未通過0.75秒主機延遲門檻；
  兩次idle清理計數都有增加，但不能據此宣稱所有端到端延遲都已解決，亦未把超標隱藏。已收到請求內容或SSE不套用此期限；`timing.http_idle_closed`可觀測次數。

已部署產物 SHA256（0.6-dev）：

- V4 app：`fd281a5b136988106ae6b10ee26fba46f52e82a7874f23dbd857f39ad236627b`
- T096 application DFU zip：`9ad7234a2f7cb6bd761286bede81c727116c336c4c4b40c3ba112303e5a88bab`

原始serial、API、flash、manifest保存在本機私人validation目錄，不提交家中位置、網路資料或憑證。

## 下次有硬體條件時的最小測試

- 室外靜置取得T096真fix，再核對GNSS500ms、實際DATA到達間隔／缺號；先陸上，再相同距離的岸／水對照。
- 天線先保持垂直，記錄身體前／側／背與Station架高，不同姿勢各一段；不要同時改SF／功率／天線而無法比較。
- 接收器5 V走板上充電輸入，不可直接接到電池座；接妥後量整機SystemOFF與未充電時的反向漏電，
  並實際試放上／移開充電座的喚醒。
  未確定外部wake前不封死殼。建議只用放上充電座喚醒；睡眠仍由Web或12小時計時，避免斷續充電誤關機。
  nRF52840 原生支援 VBUS 上升喚醒（見 [硬體筆記](hardware.md)）；先核對5 V接法與實測，
  若現有路徑不能可靠喚醒，才加接按鍵喚醒腳的磁簧／磁力開關，不先做充電手勢辨識。
- 1000／2500mAh無法只憑板子型號保證數月；用整機量測電流估算，另保留電池自放電與可用容量餘量。
