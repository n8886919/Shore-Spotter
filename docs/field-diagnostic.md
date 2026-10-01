# L76K 外測診斷版（2026-09-24，首次外測完成，重啟問題待查）

目的：保留現有 L76K，分開檢查原始 GNSS 解算、韌體解析、RF／SD 干擾及非預期重啟。
手機 Pixel 10 與 Client 放一起、同步錄手機 GPS；手機是比較參考，並非測量真值。

## 2026-09-24 長時間外測版（Client trip）

`pio run -e tbeam-client-trip` 是獨立環境，保留本次比較的 **1 Hz RMC+GGA**；
一般 Client／Station 仍為 2 Hz，原 `*-diagnostic` 的 P0–P6 比較流程保留。
Trip 固定 phase 7，持續正常 LoRa 發送，不進行 RF／SD 分階段開關；沒有定位不代表會發有效座標。
Station 無需這個 profile，LoRa 封包、功率、GPS 有效性與追蹤／Servo 契約不變。

- **SD 全程錄製**：包含無定位、定位品質不合格與重新恢復；只在此 profile 關閉 SD 的 GPS 省電 gate。
  原始 UART 以 `text_log` 的 `[GNSS_RAW seq=… kind=… hex=…]` 保存，96 bytes 一段；
  kind 1 是收到的 bytes，kind 6 是 backlog guard 丟棄的 bytes。hex 可還原雜訊、CRLF、無效 checksum，
  不會把非 UTF-8 bytes 直接寫壞 JSON。`boot_id`／`ms` 與遞增 seq 可識別重啟和缺段。
  Client epoch、每次 TX 原始 bytes／結果沿用 SD `client_event`，每 10 秒 console 保存實際 SD status。
- 原始 UART 使用既有 16 筆、每筆 256 bytes 的 text queue，zero-wait；仍由 SD worker 批次寫入、5 秒同步。
  超載由 `text_dropped` 與 raw seq 缺口顯示，不改 LoRa 排程。卡錯誤明示停錄，不自行重掛、格式化或覆蓋其他檔案。
  USB STOP／LIST／READ 仍明確停錄；取回後需 START 或重新開機，不會被 trip always policy 擅自恢復。
  錄製中 USB 回覆的單次 write 限 50 ms，主機不接收時以 `usb_write_errors` 明示失敗，避免數秒堵塞寫卡 worker。
- **內建 Flash 保留電源證據**：boot、PMU、關機意圖即時入 queue；解析／UTC snapshot 每 60 秒，
  完整 SD 狀態／recorder health 每 300 秒。原始 UART 與每次 TX 改存 SD，不再大量消耗 Flash。
  真 worker 保守容量模型以每分鐘 80-byte snapshot 加 384-byte health、每五分鐘 1599-byte SD status，
  新增 PMU 原始診斷後，容量模型另保留七個大型開機記錄與每五分鐘 319-byte PMU 記錄獨立 frame；
  `tools/test_trip_logging.py` 的實際 worker 模型為 2888/3072 frames；既有占用、額外按鍵／I2C 故障事件會縮短容量，並非實機保證。
  Flash 滿／錯誤只停止 Flash 保存，不關 RF 或 SD，OLED 顯示實際 SD 與 PowerLog 狀態。
- SD 在 3 秒動畫＋10 秒資訊頁前啟動；GPS UART 設定與 SD 初始化前的資料不在擷取範圍。
  動畫／資訊頁期間持續服務 GPS／PMU／USB；OLED 有界重試，資訊頁每秒更新實際記錄狀態。
  原始 RMC 日期／時間可配對 MCU `ms`；沒有有效 UTC 就保留未知，不使用電腦下載時間冒充量測時間。
- PMU IRQ 改保留 signed 讀取結果，讀錯不觸發按鍵；clear 失敗不重播同一事件。
  真長按的意圖在 OLED／SD drain 前入 queue，保存 raw IRQ、read／clear 錯誤、boot／uptime。
  相同持續 I2C 錯誤每 10 秒彙報；Flash 約 1 秒 flush、SD 約 5 秒 sync 都不保證瞬間斷電零損失。

明日前準備：保存既有資料，驗證 trip 韌體與實際 SD 寫入、USB 回讀 CRC、raw seq／掉資料計數，
再由使用者以純電池、實際外殼做移動／接頭／按鍵對照。USB 穩定不代表電池接觸與外殼受壓已排除。
若要驗證接收／追蹤，Station 留岸端同步記錄；Client TX 成功不能替代 Station 收包證據。
部署與實機驗收結果記於 CHANGELOG；未列部署成功前，不把此章的實作當成已燒錄。

2026-09-24 最終 Client E91C trip 已燒錄，app SHA256 `2da2f0df2f3a40be0f4f4f75981a8ecdb798cd3a4c5b73a51a509526b4bdc392`。
實機室內無定位，SD 回讀 162,467 bytes／CRC `791405eb`；raw seq 0–221 連續，220 句正常 NMEA checksum 全部有效，
開機 backlog guard 的 100 bytes 另以 kind 6 保存。30 對連續查詢、4 次 SD 舊檔 CRC／SHA、Flash 18 frames 匯出皆通過，
SD／Flash errors、text／client drops、USB write errors 為 0；驗收結束後 SD 已重新開始。
Flash 保留初版與修正版的受控部署 boot；18 frames 不代表 18 次開機。原型第一次 USB 未接收期間有丟資料，
修正版加入 50 ms 等待上限；這段失敗 evidence 也已保存，不把後續成功測試冒充整趟零錯誤。
USB RX hex 另確認曾收到自身開機文字回送；啟動 console 與未請求的錯誤回覆先獨立保存，正常 request 仍逐筆完整驗證。
USB 供電可能掩蓋電池接觸／瞬降；下一步須拔 USB，以相同電池、實際外殼分開做靜置與移動對照。

## 2026-09-24 13:52 純電池開機後顯示 SHUTDOWN

使用者先手動關機拔 USB，再以電池開機，確認看到 `SHUTDOWN...`。
兩次新 boot（3119320703、1211521128）皆於 uptime **12,782 ms** 記下
`pmu_irq/key` → `shutdown/pwr_long_press`；INTSTS2 分別 0x47／0x07，均有真實 long bit。
電池 4159／4160 mV，VBUS raw [24,93]，I2C read／clear failures 均 0。
SD 兩段及 Flash 的關機意圖一致，SD 正常進入 stopping／同步；這兩次不是只有 OLED 休眠，
也不是由現有證據可指認的電池接觸突然斷電。開機期間的原始按壓手勢仍須由實體重試確認。

原因路徑：舊 boot screen 10 秒期間不讀 PMU key，首個 runtime poll 將啟動期間已累積的 long latch
當成新的關機要求。新修正把啟動手勢與畫面結束後的新按下分開，保留 startup IRQ 證據；
I2C 未知／清除失敗不能當成成功基準。新版已USB部署Client E91C，App 745,968 bytes，
SHA256 `71a689b9b150a9ac0f67c9ae331ecf0b0d2e7c16c24886c384dbc252a72bd2be`；Station未燒錄。
23組電源／boot主機測試及三個build通過。新USBboot實測raw seq 0–139連續、136句正常NMEA checksum通過，
SD／Flash errors與drops均0；舊Flash前31,744 bytes逐byte保留，並保存等待新按壓的startup基準。
這次USB驗收沒有shutdown；純電池／實體按鍵與OLED顯示仍待使用者確認。
較早 09:26 外測的重啟仍不能只靠本次現象判定為同一原因。

後續使用者確認資訊頁還在時重新長按，USB 接著也會直接全暗。舊 guard 的確整段顯示期間
抑制軟體關機；硬體長按可能先斷電。新修正讓首次成功清除的基準只建立一次，其後新的下降沿
在動畫／資訊頁也可操作；初始按壓持續不放不會 armed，不用無 IRQ 猜測已放開。
關機請求返回時不再重畫開機頁；末段短按保留自己的十秒顯示期限。硬體關斷設定與 Station 不變。
這與 18650 無法供電、PMU battery=0 分開驗證；尚未把任一故障歸因於充電一晚或板上黑色殘留。

## 電池偵測與長按設定原始診斷（2026-09-24，已部署 Client E91C）

僅 Client diagnostic／trip 新增 `pmu_battery` JSON，Flash kind 2 與 USB／SD text 同步提交。
`init` 階段在 PMU 初始化後、radio 初始化前；`runtime` 首次於主迴圈取樣，其後每 300 秒一次。
初始化剛啟用 ADC 的讀值不保證已穩定；需與後續 runtime 對照。此記錄不改 `cachedBatteryMv`、
LoRa 電量欄位、低電／PKEY 判斷、充電／BATFET／DCDC 設定；一般 Client 與所有 Station 不新增讀取。

- `init` 陣列依序為既有 battery detection、VBUS ADC、battery ADC、system ADC enable 呼叫的回傳值。
  1＝原呼叫成功、0＝呼叫失敗、-1＝未執行；不是電池存在值，也不等於設定 readback 成功。
- `raw` 順序固定為 hex `00、01、30、34、35、68、22、27`；成功保留 0–255，-2＝未讀。
  -100＝register pointer 寫入長度不符，-101…-105＝I2C TX status，-200＝RX 短讀，-201＝沒有可讀 byte，-202＝read 無有效 byte。
  `read_fail` bit0–7 對應這八個寄存器；`online=false` 或 `lock=false` 時未讀，不把它當成電池不存在。
- `00` bit3 是 PMU 電池存在、bit5 是 VBUS good；`30` bit0 為電池 ADC enable，`68` bit0 為偵測 enable。
  `01` bit3 是 VINDPM，不能當作 USB 存在；`22/27` 用於核對硬體長按關機 enable／時序，僅讀不改。
- `34/35` 依高、低位順序相鄰讀取；AXP2101 V1.4 記載高位 mask 0x3f，現用 XPowers 取電池電壓為 0x1f，
  保留 raw 供離線檢視，此次不修改既有 library 解碼。一般單顆鋰電池電壓範圍兩者相同。
- 每組使用 Client 既有 PMU／SD rail lock 一次 try，讀完即釋放再輸出；依序取樣，不宣稱原子快照。
  專用 helper 檢查短讀後直接 `read()`，避免 XPowers `readBytes()` 的 Stream 等待；
  沿用既有 10 ms I2C transaction timeout，`elapsed_ms` 保存整組實际耗時，故障時仍可能增加 loop 延遲。

判讀界線：PMU 報 battery absent 與讀取失败現在可分開，但兩者都不能單獨證明電芯電壓、接點導通或負載能力。
硬體長按可能在 SD drain／OLED 準備完成前關斷；仍須結合 shutdown event、PMU on/off source與使用者手勢判斷。
依據：[AXP2101 V1.4](https://github.com/lewisxhe/XPowersLib/blob/master/datasheet/AXP2101_Datasheet_V1.4_en.pdf)。

此次 E91C Trip 實機 USB 驗收 boot `4153263253`：init／runtime 均 online、取得 lock、read_fail=0、耗時4ms，
原四個初始化呼叫均成功；raw 依序為 `[32,21,13,1,110,1,6,20]` 與 `[32,21,13,1,112,1,6,20]`。
偵測及 ADC 已啟用但 PMU 報 battery absent，VBAT ADC 366／368mV 不等於電芯實測電壓。
硬體長按關斷設定為啟用／power-off／6秒，long IRQ 1.5秒，此次僅讀未改。
28組 power 主機測試與三個 build 通過；App SHA256 `e5ea4ae30919643e435a276d9898bf9ea37c2b1a42bdae52798d1d692a3dbc1c`，
app-only 燒錄前後完整 Flash 校驗且舊記錄保留，Station未燒錄。新boot SD/Flash errors及drops=0。
使用者懷疑原圖板面有燒斷痕跡，照片不能確認銅箔連通；已停止後續通電測試並同步停止SD。
資訊頁內新按壓的實體OLED驗收延後，電池供電仍須檢查電池座／連接／保護／PMU路徑，不能據照片定因。

## 不用帶筆電的記錄方式

診斷韌體把原始 UART bytes 與解析 snapshot 存進板上 **1.5 MiB Flash**，回家用 USB 取出。
關閉 SD 不會關掉此記錄。資料不以 PSRAM 作唯一保存，也不經 LoRa 或 Axiom 傳送。
只使用現有 `spiffs` 分區（`0x670000`、`0x180000` bytes），不修改分區表、NVS、OTA 或 coredump。
不掛載／格式化檔案系統；全空白可初始化，遇到未知內容拒寫並顯示 `unknown`，保留原資料。

- 32 筆零等候 queue；背景 worker 合併多筆至 512-byte frame，CRC32 加寫後讀回。
- 一個 ownership header 加最多 3071 個 data frames；一般約每秒 flush，容量依實際資料量計算。
- 實際 codec 的容量模型（1 Hz、原始 UART 160–256 B/s、snapshot／TX／SD status）：
  20 分鐘約 0.8–0.9 MB，30 分鐘約 1.2–1.3 MB；**先以清空後開機起算 20–25 分鐘、最多 30 分鐘**規劃。
  這不是實機容量保證；模組若沒有遵守 1 Hz／雙句或已有舊紀錄，可錄時間會縮短。
- 滿了顯示 `full` 並停止，**不循環覆寫**。重新上電繼續追加，不自動清空。
- 每 frame 保存 boot ID、序號、累積 dropped；重新開機掃描保留可用 frames，損坏 frame 不阻斷後續解析。
- 突然重啟可能損失 queue／尚未寫入的尾端；1 秒為正常排程 flush 目標，並非斷電零損失保證。
- Flash 寫入仍可能影響 MCU 排程；記錄 `write_max_us`、main loop gap、UART backlog drops。
  全部 RF／SD 比較階段持續使用相同 Flash recorder，實機延遲、供電干擾仍須量測。

短按 Client 電源鍵喚醒 OLED，底下兩行顯示 `DIAG P… RF… SD…` 及 `Flash:recording/full/error/unknown`。
`RF`／`SD` 是目標狀態，SD 關閉須等背景排空及關卡電；原始記錄內另有實際 SD 狀態。
未知／錯誤／滿容量時中止測試階段，P6 恢復 RF 開、SD 關；OLED 保留錯誤，不會假裝完成。

## 自動比較流程（Client）

診斷版使用 **115200 baud、1 Hz RMC+GGA**。一般版仍為原本的 2 Hz 驗證設定。
設定命令不代表 GNSS 已接受；用原始句子與 5 秒實測 RMC／GGA／epoch rate 判定。
不改 GNSS 星系設定，不把天線標示當成接收器實際啟用的星系。

| 階段 | 時間 | RF 發送 | SD | 用途 |
|---|---:|---|---|---|
| P0 暖機 | 等有效定位持續約 30 秒 | 關 | 關 | 避免 RF 干擾初次定位、保留冷啟動資料 |
| P1 | 3 分鐘 | 關 | 關 | 基準 |
| P2 | 3 分鐘 | 開 | 關 | 與 P1 比較 RF 影響 |
| P3 | 3 分鐘 | 開 | 開 | 與 P2／P4 比較 SD 影響 |
| P4 | 3 分鐘 | 開 | 關 | 關閉 SD 後重測 |
| P5 完成 | 持續至關機／滿容量 | 開 | 關 | 仍記錄，回家可匯出 |

建議這一趟先把手機與 Client 放在同一開闊位置，固定擺放方向完成 P0–P4；
P5 後再一起走一段 3–5 分鐘的路。全程錄手機 GPS。這樣不會把不同路段的建物遮蔽誤認為 RF／SD 效果，
也同時取得行走軌跡；總時長以開機起算 20–25 分鐘為目標。

暖機須有效新鮮 RMC+GGA、至少 6 衛星、HDOP ≤3；容許配對短暫間隔，超過 2 秒無合格點則重新計時。
如果一直沒有合格定位，維持 P0，原始無定位輸出同樣保存。
非預期重啟會新增 boot/reset/PMU 記錄，重新由 P0 開始；不把重啟前後當成連續同一階段。
RF 關是停止新增發送，已開始的 TX 先正常結束，晶片留 standby；**不是切掉 RF 電源**。
SD 開階段仍遵守現有 GPS 品質 gate，卡故障／掉定位可能使實際 SD 尚未開始，分析以 SD 狀態為準。

Station 診斷版固定 P255，持續接收與既有 SD 記錄，另存自己的原始 GNSS／snapshot；不執行 Client 階段。
Station OLED 左下 `F:…` 顯示 Flash 狀態。
這趟獨立 Client 診斷不必帶 Station；原始證據直接存於 Client Flash，RF 階段不依賴 ACK。
若另帶 Station 作收包對照，保持開機手動模式。P0／P1 刻意不發送，畫面暫無 Client DATA 是預期行為。

## 保存的證據與時間

1. 原始 UART bytes：含 CRLF、不良 checksum、雜訊；每筆最多 128 bytes，長句分段，不先過濾好定位。
2. 被 backlog guard 丟棄的 bytes 另標 `discarded_backlog`；UART 硬體 FIFO 已溢出的資料無法重建。
3. 每秒解析 snapshot（80 bytes）：原始 double 座標、速度、航向、HDOP、衛星、fix／句型旗標、epoch 與 source／arrival age。
4. 同 epoch、checksum 有效 RMC 的完整 UTC 日期／時間；沒有對得上的日期就標未知。
   `boot_ms` 為 MCU 單調計時，不冒充 UTC；下載電腦時間只作 `host_received_utc`。
5. boot ID、ESP reset reason 數值、PMU power-on／power-off source 原值、phase、電池、heap、loop gap、backlog。
6. 每個 Client TX 事件與原始 wire bytes；每 10 秒完整 SD status，以有序 UTF-8 chunks 保存。

RMC 日期只接受 2000–2099，完整日期／時間不跨 epoch 借用。UTCvalid 不等於定位準確。
PMU source 為暫存器證據，不能单靠一個 bit 斷言本次重啟原因；仍需與 ESP reset reason、時間線對照。
開機 GNSS UART 設定前的輸出不在捕捉範圍；診斷 Client 的 3 秒動畫及 10 秒資訊頁期間持續服務 UART。

## Build 與回家匯出

```sh
pio run -e tbeam-client-diagnostic -e tbeam-station-diagnostic
python3 tools/read_diagnostic_log.py --port /dev/ttyACM1 status
python3 tools/read_diagnostic_log.py --port /dev/ttyACM1 read --output /private/path/client-diagnostic
```

Python 須已安裝 pyserial；本機可使用 `~/.platformio/penv/bin/python`。
`ttyACM1` 只是範例，操作前以 MAC 辨識板子。工具避免切換 DTR／RTS，清除 HUPCL；不要同時開另一個 serial monitor。
輸出 `.bin` 原始 frame、`.ndjson` 解碼資料、`.manifest.json` 完整性報告。CRC／遺失 frame 可按 index 重試；
Flash 本身損壞、序號缺口或 dropped 仍保留可得證據並以非零結束，不能把成功下載誤當完整外測。
讀取不停止記錄；本次只匯出開始時已存在的 frame 範圍，尾端新增資料可再讀一次。

只有資料已另外保存、確定要清除此診斷區時才執行：

```sh
python3 tools/read_diagnostic_log.py --port /dev/ttyACM1 erase --confirm-erase
```

此命令清除整個診斷分區，普通開機及 read/status 均不會清除。清除後重新上電才開始一趟乾淨的測試時間線。
清除後會立即繼續記錄，也不重置測試階段；目前沒有 USB STOP／START 命令。
桌上驗收完成後先匯出，再清除自己的驗收紀錄；接著拔掉 USB、長按電源鍵正常關機，到空地才重新開機。
不可插著 USB 留在家中持續開機，否則會消耗外測容量。關機最後一筆是否完整須靠讀回確認，不能保證突然斷電零損失。
一般版不啟用 Flash recorder；診斷環境沒有另建 OTA profile。建置成功不代表已燒錄／Flash 實機寫入或外測已驗證。

## 本輪驗證（2026-09-24）

- native：129／129；SD worker／Client SD 與 retention、packet／motion／debug／Axiom backend 及既有 UI 回歸通過。
- Flash 實際 worker：17 種主機情境加非診斷 stubs，含掉電尾端／未知分區／滿容量／queue drops／USB 斷線。
- USB 匯出：14 項測試，真實 C++ codec 產 fixture；CRC、重試、跨 boot、UTC、原始 bytes 留存。
- 擷取實際共用 USB parser／LogTee：SD／DIAG 輸出互斥、pending command 保留、overflow／timeout／millis wrap 通過。
- 擷取實際 configureGps／serviceGps：一般 2 Hz／診斷 1 Hz 命令與 checksum、原始 UART／backlog 分流通過。
  另驗 UTC 日期／午夜／同 epoch 配對、80-byte 編碼、30 秒暖機、各 phase／abort／計時 wrap。
- 真 codec 容量模型 12 情境，包含 recorder-health；30 分鐘皆低於 1.5 MiB，60 分鐘皆超出，非實機耐久測試。
- Client、Station、Station OTA、兩端 diagnostic 共 5 個韌體環境 build 通過；僅第三方 SDK／RadioLib 既有警告。
  上述為建置／主機驗證；後續 Client USB 部署與室內實測另列於下方，尚未重新外測。
  診斷 binary／ELF／分區與 SHA256 manifest 留存於私人 `shore-spotter-debug/2026-09-24/diagnostic-build/`。

## Client 部署與室內採集（2026-09-24）

- 僅 Client E91C 燒入 `tbeam-client-diagnostic`，共用版號 `0.6-dev`；Station 新版尚未部署。
- 韌體 739,424 bytes，SHA256 `a3afdf51a2a9f48905793df34bf60021a766b193fcf36631bf2eea005b070662`。
- 燒錄前保存 7 個既有 SD 外測 log，合計 11,011,880 bytes；SD 未格式化。
  完整 8 MiB Flash 先備份，ROM 分塊讀取後以裝置整體 digest 核對。
  預設 esptool stub 在特定區段讀取斷流；ROM 與 legacy stub 可讀回，不能據此判定硬體故障。
- 原診斷區只有 `0x6ffe00–0x6fffff` 共 512 bytes 為零，其餘全 FF，來源未知。
  原樣已備份；本次人工清除其所在 4 KiB sector，整個診斷區空白 digest 驗證成功，保留韌體未知內容拒寫機制。
- 僅寫 app0 `0x10000`，更新後整顆 Flash 與預期內容 digest 一致；bootloader／分區／NVS／OTA／其餘內容保留。
- 初次 USB 匯出 35／35 frames、17,920 bytes，CRC32 `ae56bac4`；全部 frame CRC／順序／解碼通過。
  35 RMC＋36 GGA checksum 全過，實測到達間隔 999–1001 ms，兩句 UTC epoch 間隔皆 1000 ms。
  35 筆有效 UTC snapshot 與原始 RMC 日期／epoch／age 完全一致。
- 當時無定位（RMC V、GGA fix 0），phase 0、RF／SD target 關、實際 SD 停止且卡電關、TX 0。
  dropped／errors／corrupt／UART backlog 0，loop gap 最大 35 ms，Flash write 最大 2656 μs。
- 一次明確標記的工具受控重啟後，匯出 81 frames、41,472 bytes，先前 35 frames 逐 byte 保留。
  新 boot 重新 P0，Flash 正常追加；drops／errors／corrupt 為 0，Flash write 最大 2681 μs。
- 原始備份、兩次 binary／NDJSON／完整性 manifest、部署與重啟操作紀錄保留在私人
  `shore-spotter-debug/2026-09-24/client-deployment/`。桌上驗收資料匯出後才清除，以準備外測。

以上證明室內原始資料採集、1 Hz、USB 匯出與受控重啟保存；不代表定位精度、完整 P1–P5／RF／SD 干擾、
電池實際正常關機尾端保存或外測穩定性已驗證。日後燒錄仍須重新確認 MAC、備份既有資料並核對實機記錄狀態。

## 首次外測讀回（2026-09-24）

Client E91C 僅讀回資料，未重燒、重置或清除。Flash 1702／1702 frames、871,424 bytes，CRC32 `c8ae6860`，
逐 frame CRC、USB retry／error、持久化 dropped／corrupt／sequence gap 均 0。
P3 新增兩份 SD log 合計 290,040 bytes，大小／CRC／742 行 JSON 全部通過；最終正常長按關機事件也已保存。
完整匯出不代表非預期重啟前未落地尾端為零損失。

- 手機 1,123 點、每秒一點；約前 15 分鐘停留，其後移動。手機是濾波後參考，沒有 accuracy／HDOP，不是真值。
- 依 GNSS UTC 精確配對 1,077 點：停留中位距離 1.20 m／P95 3.31 m；移動中位 8.46 m／P95 22.33 m。
  移動時首次重啟前 P95 11.83 m、重啟後 31.73 m；最大相差 39.07 m。不同速度／路段與重啟混在一起，不能直接定因。
- P1–P4 各 180 秒均完成，也進入 P5；與手機距離 P95 依序 3.33／1.90／1.28／0.75 m。
  沒有重現開 RF／SD 後的數十公尺偏移；同時存在暖機／衛星變化，不能宣稱完全排除干擾或證明 1 Hz 已修好精度。
- 1,124 筆有效 snapshot 與同 epoch 原始 NMEA 座標一致，772 個 fix DATA 的 E7 座標也全數吻合。
  774 DATA seq 連續，另外兩筆是 no-fix 通知；所有 813 TX started／sent 成對，未驗 Station 接收。
- 移動期間有 3 次非預期 MCU boot；使用者確認未手動重開。全部 `reset_reason=1`，PMU raw on=1／off=2。
  後兩次前完整 boot 無 TX、SD 未供電／寫入；不能把 TX 高峰或 SD 寫入當必要原因。
  電池取樣约 4.13 V 不排除瞬降；PMU latch 可能是舊狀態。整晶片 reset 記號也不能排除特殊 RTC／PMU watchdog。
- 兩次約 474 ms 主 loop 停頓觸發 200 ms backlog guard；兩處原始 GNSS 每秒完整、實際 discarded bytes 為 0。
  P3 暫停 GPS gate 428 ms，因此產生兩個 SD 段；另一處在 SD 已關的 P5。兩筆 no-fix 後約 419／420 ms 出現有效 DATA TX完成，非 Station 顯示時長。
  SD write 最大 62.288 ms 為整批合計，sync 57.624 ms、Flash write 3.620 ms，尚不足以定因。
  短按喚醒 OLED 的初始化至少有 400 ms 固定延遲，是相符的程式候選；缺少按鍵時點，不能直接指認這兩次事件。

優先在家排查電池固定／供電與 EN(RST)／按鍵受壓，再追 PMU／I2C，保留 L76K 與目前 1 Hz 作比較基準。
I2C 讀錯誤可能被庫轉成 PKEY IRQ 的程式弱點另待修正與驗證；未將它定為本次重啟原因。
私人證據與分析保存在 `shore-spotter-debug/2026-09-24/field-test/`，定位座標及完整原始紀錄不納入專案 Git。

後續主機故障注入共 11 案例通過：實際 XPowersLib 的 INTSTS2 read -1 會同時誤判 short／long，
Client dispatch 先喚醒 OLED（至少 400 ms 固定 delay），才進入記錄 shutdown intent 的 helper。
IRQ clear 失敗也能重播短按；此為程式路徑驗證，尚無實機 signed I2C error 證據，未認定為外測根因。
使用者補充約 09:26 開始收拾設備、可能動到，與重啟時段吻合，接觸／按鍵／EN 受壓仍優先。
已保留原韌體啟動 USB 靜置／SD 讀取／查詢負載的有界監測，主機留存增量 Flash 與 console；
Flash 滿後不清除，僅剩 host status／console，不能以 USB 穩定排除電池或移動接觸故障。
原始主機案例與實機監測進度在私人 `shore-spotter-debug/2026-09-24/bench-test/`，不視為已修正或已部署。

USB 留接測試另發現完成回覆時序問題：`SD LIST END` 送出後，worker 尚未 unmount／關卡電及釋放 usbActive；
主機立即送下一命令時，parser 可能靜默丟棄。實際 worker/parser 的主機排程測試已重現 READ／DIAG 丟棄；
實機曾 LIST 成功但 READ 無 header 逾時，boot／SD errors 不變，不能誤當關機或 SD 內容損毀。
暫以主機每次發命令前等待 500 ms 作對照，首次 121,423 bytes 回讀 CRC／SHA 一致；未更動裝置韌體，
固定延遲並非協定完成保證，保留逾時／暫停負載與原始證據。之後產品仍需修正回覆完成與下一命令接受的契約。
