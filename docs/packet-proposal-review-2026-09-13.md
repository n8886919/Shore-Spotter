# 封包精簡與 2 Hz 提案審核

日期：2026-09-13。**以下保留實作前的審核內容，文中的「目前」指當時 v3；不是現行契約。**
後續採納 1-byte 速度（0.1 m/s、最高 25.4 m/s、255 未知），已在 **0.6-dev** 實作
17-byte v4 DATA、來源 age、向量有效性、獨立 DATA 序號及非阻塞 RF 2 Hz 排程，
另有低頻 Client 診斷與網頁匯出。GNSS 真實 2 Hz 尚待模組實測，沒有硬送更新率命令。
現行格式以 [interface.md](interface.md)／[protocol.h](../include/protocol.h) 為準，
實作與驗證狀態見 [CHANGELOG](../CHANGELOG.md)。後續 Server 已 OTA、Client 已 USB 燒錄；
v4 上行收包與 Server ACK 發送已實測，Client ACK 接收仍待驗；尚未切頻或量測
戶外定位／機械追蹤效果，詳見 [通訊測試](link-test-2026-09-13.md)。

原審核依當時工作目錄（包含未提交變更）、RadioLib／TinyGPSPlus 及 GNSS 原廠文件。

## 結論

**可以精簡，但不核准原 17-byte 提案直接定稿，也不核准只改 500 ms 就宣稱完整定位 2 Hz。**
移除空 MAC、加速度、固定 group／Server ID 是合理刪減。真正影響追蹤的來源資料年齡、
速度有效性和定位狀態，不能為湊 17 bytes 而省略。

我的首選是 **18-byte DATA**：保持約 0.1 m 座標量化、速度／方向、HDOP，壓縮衛星分級，
補上來源 age、fix 與速度向量有效旗標。相比 17 bytes，每包多 20.48 ms；相比目前
32 bytes，仍少 14 bytes、airtime 減少約 24.9%。這筆成本比含糊的追蹤語意值得。

## 逐項裁決

| 提案 | 裁決 | 必要條件／代價 |
|---|---|---|
| 刪 4-byte 驗證尾碼 | 接受 | 現有 MAC 是全零佔位，沒有認證作用；保留 LoRa PHY CRC 與解析檢查。MAC 與 CRC 是不同層 |
| 不做加密／認證 | 接受此專案需求 | 不新增 HMAC；ID、序號、資料域檢查仍用於防止誤配／舊資料，不是資安功能 |
| 刪加速度 | 接受 | 現有外推未使用它；同步刪 API／文件欄位，不假造為 0 |
| 刪速度／方向 | 不建議 | α=1 需要速度向量；只靠位置差分會把定位雜訊與掉包誤差帶入速度 |
| version/type 各 4 bits | 接受 | 協定版本與韌體版本不同；16 個值目前足夠，未知值拒收，無需先建動態 schema |
| 固定 type 長度，刪 payloadLen | 接受 | 按 version/type 精確驗證整包長度，未知／過短／過長拒收 |
| 刪 group、src/dst 改 client_id | 接受目前一對一範圍 | 上行是來源，下行是接收對象；每套實際配對不能沿用同一預設 Client |
| 固定台灣原點，兩個 signed24 E6 | 接受 | 明確範圍、符號延伸、超界處理；沒有定位不能用零 offset 暗示 |
| 5-byte 座標、降低解析度 | 不採用 | 不符合目前維持約 0.1 m 的需求；縮範圍本身不必降低解析度，但還要容納台灣與兩軸精度 |
| 只留「衛星 ≥8」一個 bit | 不採用 | 目前 6–7 顆且 HDOP 合格也可追；顆數多不等於 HDOP 合格或資料新鮮 |
| quality2 取代 fix＋衛星 | 原樣不採用 | Bad 與 fix 不是同一概念；原 enum 數值順序也與提案相反 |
| 17 bytes 是硬上限 | 不採用 | 17→18 有 airtime 成本，但資訊正確性優先；可提供另有取捨的 17-byte 替案 |
| 發送改 2 Hz | 有條件待驗 | GNSS 是否提供完整新定位、ACK／TEL 排程、buffer 與失敗復原都要驗證 |
| 掃描後手動選新頻道 | 可以獨立規劃 | 掃描只提供樣本；單 radio 會中斷接收，切換雙端需要可復原流程 |

## 必須先修正的問題

### 1. 接收年齡不是定位年齡（P1）

`buildDataPacket()` 允許來源位置 age 未滿 2 秒，Server 的 `haveBearingFix()`／
`gpsModeAvailable()` 再以收到後未滿 2 秒為門檻。最差可用到接近 **4 秒＋airtime**
之前的資料；每 500 ms 重發同一筆定位只會得到新封包序號，不會變成新定位。

`predictClientPos()` 也只算 `millis() - lastRxMs`，漏掉建立封包前的定位年齡與 RF 傳送時間。
17-byte 包在此設定下約需 165 ms，這段延遲本身就不能忽略。α 開關不修正這個既有缺口。

建議傳來源 age：建立封包時向上取整為 10 ms 單位，接收端加傳送耗時與接收後經過時間，
合計到期限便停用；排隊過久須重建或放棄封包。不能以飽和後的 age 冒充新鮮資料。
TinyGPS 的 age 是解析更新後時間，仍不是精確 GNSS 測量 epoch；若未做 epoch 對齊，
必須保留這項限制，不能宣稱完整端到端時間同步。

### 2. 速度向量的有效性未定義（P1）

目前 `buildDataPacket()` 對 speed／course 只有 `isValid()`，沒有 `.age()`。
如果 GGA 繼續、RMC 停止，可形成「新位置＋舊速度／方向」，Server 卻繼續外推。
course 缺漏初始化為 0，也會被誤解為向北。

新增 `velocity_valid`：速度、方向都有效、新鮮，且與位置時間可接受地一致時才置 1。
無效時只停用外推，仍能使用有效位置；speed 與 course 的數值域、未知值也要明訂。
方向 0–3599 代表 0–359.9°，不能接受剩餘 12-bit 數值後直接當角度。

### 3. GPS 分級與 fix 不能互相替代（P1）

目前 `geo::SigLevel` 是 GOOD=0、OK=1、BAD=2、MISS=3；原提案 00 無效、01 差、10 OK、
11 Good 正好反向，**不能直接 cast**。Bad 可能是有 fix 但品質差，也可能沒有 fix；
雷達、GPX、方位計算仍使用獨立 `client.fix`，不能由 Bad 無歧義還原。

既有追蹤門檻：Good 為 fix、衛星 ≥8、HDOP ≤1.5；OK 為 fix、衛星 ≥6、HDOP ≤3。
只保留「八顆以上」會失去 OK 類別，並且無法取代 HDOP。建議使用 2-bit 衛星分級
（未知／0–5／6–7／≥8），另外保留 fix 與 velocity_valid，Server 繼續判斷門檻。

### 4. 遙測會吃掉 ACK 序號（P1）

目前 DATA 與 TEL 共用 `txSeq++`，Server 以 `seq % ACK_EVERY_N == 0` 決定 ACK。
若改成 N=8 且 2 Hz，以下情況會漏掉整次 ACK：

```text
DATA 0 → ACK
DATA 1..7
TEL 8
DATA 9..16 → 到 16 才再次 ACK
```

兩次 ACK 相隔 15 個位置週期，約 **7.5 秒**，不是固定 4 秒。
「TEL 只放非 ACK 週期」仍無法避免 DATA7 後的 TEL8。現有 N=4 也有同樣缺口；
DATA loss 統計使用 seq span，因此把遙測序號一起算進期望位置包數。

改版應讓 DATA 有獨立序號；ACK 回覆 DATA seq，TEL 另定序號用途。ACK 接收端需核對
自己的 ID、確實期待回覆的近期 DATA seq 與時間窗口，再更新 alive／ATPC。
現有 ACK 處理只記錄 ackSeq，沒有完整核對。

### 5. 2 Hz GNSS 不能只改 timer（P1）

目前 GPS UART 為 9600 baud，程式沒有送出 GNSS 更新率設定。TinyGPSPlus 從 RMC
取速度／方向，從 GGA 取衛星／HDOP。

L76K 原廠 PCAS02 文件列出 500 ms＝2 Hz，但間隔小於 1000 ms 時要求 115200 baud
及單一 NMEA 輸出類型。只保留 RMC 或 GGA 都會缺少目前需要的部分資料；因此不能僅憑
命令表保證完整定位 2 Hz。板型還可能使用 M10，不能對所有模組硬送 PCAS 命令。
依據：[Quectel L76K Protocol v1.1，第 23 頁](https://files.waveshare.com/upload/d/dd/Quectel_L76K_GNSS_Protocol_Specification_V1.1.pdf)。

先識別模組／韌體並錄製 NMEA，確認不同測量 epoch、位置、速度／方向與品質欄位都依需求更新。
若沿用文件以外的句型組合，需要實機證據。網頁位置輪詢目前仍是 1 Hz，也不會隨 LoRa 自動加倍。

## 審核後首選：18-byte DATA（尚未定稿）

多 byte 整數明訂 little-endian，方向與旗標用 mask／shift，不使用 C++ bitfield ABI。

| offset | bytes | 欄位 | 規則 |
|---|---:|---|---|
| 0 | 1 | magic | 0x53 |
| 1 | 1 | version/type | 高 4 bits 協定版本、低 4 bits 訊息類型 |
| 2–3 | 2 | client_id | 上行來源；下行指定 Client |
| 4–5 | 2 | data_seq | DATA 專用遞增 uint16，允許 wrap |
| 6–8 | 3 | lat_delta_e6 | 緯度相對 24°N 的 signed24，單位 10⁻⁶ 度 |
| 9–11 | 3 | lon_delta_e6 | 經度相對 121°E 的 signed24，單位 10⁻⁶ 度 |
| 12–13 | 2 | speed_cms | cm/s；velocity_valid=0 時不參與預測 |
| 14–15 | 2 | course_and_flags | bits0–11 course；12–13 sat_class；14 fix_fresh；15 velocity_valid |
| 16 | 1 | hdop10 | HDOP×10，255 未知 |
| 17 | 1 | source_age_10ms | 0–254 為向上取整的來源 age，255 表示未知／過期，不可追蹤 |

sat_class：0 未知、1 為 0–5 顆、2 為 6–7 顆、3 為 ≥8 顆。分級也須建立於新鮮衛星資料。
fix_fresh=1 表示發送端認定有新鮮位置；接收端仍要重新檢查合計年齡，不能永久相信旗標。
HDOP 的未知、來源過期與不合格值必須令 GPS gate 失效。具體欄位時間一致性仍是實作前置條件。

精確衛星數可以另放低頻 TEL 供診斷；不需要為展示顆數增加每個位置包。
固定 offset 與實際玉山座標無關，只是共享算術原點；沒有動態原點同步工作。
這個範圍是緯度 15.611392–32.388607、經度 112.611392–129.388607。
E6 每格南北約 0.111 m、台灣緯度東西約 0.10 m，屬於原要求的約 0.1 m；
四捨五入誤差至多半格，但 GPS 本身精度並不因此變成 0.1 m。
目前 v3 使用 E7、量化約 0.01 m；E6 是本次討論選定的約 0.1 m，仍有十倍量化間距差異。
可編碼範圍也不等於現有磁偏角表／追蹤功能的有效範圍。

若一定維持 17 bytes，可把 HDOP 的 1 byte 換成 age，16-bit 方向欄位改為
course12＋quality2＋fix1＋velocity_valid1，HDOP 與精確衛星數放 12-byte TEL。
10-byte 與 12-byte TEL 在這組 RF 參數同為 144.384 ms；但此方案把品質政策放到 Client，
兩端須共用固定分級定義，即時原始品質診斷也變成低頻。**我優先選 18 bytes，降低政策耦合。**

ACK 可保留 11 bytes、基本 TEL 10 bytes。另建議 ACK 的 int8 SNR 從 ×10 改為
0.25 dB 單位，對齊 SX126x 原生值；目前 ×10 的 int8 只能表示 −12.8～12.7 dB，
`buildAckPacket()` 又未飽和轉型。改單位不增加 byte，但 ATPC／顯示／文件需同步。

## Airtime 與共存

以目前 SF9、BW125 kHz、CR4/5、preamble8、explicit header、PHY CRC 計算。
byte 數指交給 radio 的應用包長，CRC 等開銷已包含於 airtime 計算。
依本機 RadioLib SX126x 的公式，這組參數可簡化為：

```text
T(n) = [20.25 + 5 × ceil((8n + 8) / 36)] × 4.096 ms
```

| 方案 | DATA／ACK／TEL bytes | DATA airtime | 平均 RF 發送時間需求 |
|---|---|---:|---:|
| 目前 1 Hz；理想每 4 DATA 回 ACK | 32／20／19 | 246.784 ms | 約 29.93% |
| 原提案 1 Hz；每 4 DATA 回 ACK | 17／11／10 | 164.864 ms | 約 20.58% |
| 原提案 2 Hz；每 8 DATA 回 ACK | 17／11／10 | 164.864 ms | 約 37.06% |
| 修正版 1 Hz；每 4 DATA 回 ACK | 18／11／10 | 185.344 ms | 約 22.63% |
| 修正版 2 Hz；每 8 DATA 回 ACK | 18／11／10 | 185.344 ms | 約 41.16% |

均含 TEL 每 30 秒一次；假定成功且 ACK 週期已正確實現，沒有額外掃描／重試／控制訊息。
現行共用序號可能漏 ACK，故目前列是名目需求，並非現場量測占用率。
這些平均值不代表每個 deadline 都排得下，也不是成功接收率或法規限額。
兩套未協調的 17-byte／2 Hz 系統即使名目總需求約 74%，仍可能嚴重碰撞；不需到 100% 才互相影響。
不同 client_id／group／sync word 不能隔離同頻 RF 傳送。

13–17 bytes 在此組參數同一階，18–21 bytes 是下一階；這不是 K byte 一檔的固定規則，
改 SF／BW／CR 後階梯和時間都會不同。可考慮偶爾合併 TEL，但必須重新算邊界與排程，
不為省一包額外建立通用動態欄位框架。

### 500 ms 的排程缺口

原有 guard 每段 80 ms，17-byte DATA＋ACK＋TEL＋兩段 guard 共 **613.632 ms**，
超過 500 ms。即使 TEL 只放非 ACK 週期，可開始 TEL 的窗口也僅約
244.864–275.616 ms，寬 30.752 ms；18-byte 修正版窗口縮為約 10.272 ms。
因此若堅持 2 Hz，TEL 獨立包的排程需要實際重設，不能照搬現有 fallback。

`computeAirtimeBudget()` 算不下時會放寬窗口到整個 interval，這可跨下次 deadline。
Client 的 blocking transmit 在失敗時等待約 5 倍 airtime，Server ACK 也有較長 timeout；
正常平均算得下不代表故障時仍每 500 ms 到點。要定義過期工作跳過、不積欠補發，並驗證 RX 恢復。

提高 UART 至 115200 後，1024-byte buffer 在滿線速約 89 ms 填滿，而 TX 正常阻塞約
165–185 ms。受限 NMEA 未必有那麼大量，因此不是已證實溢位；需量實際 burst 與遺失句子。
`RadioSequence` 重同步至少相隔 500 ms 的規則，也須驗證 499／500／501 ms 抖動邊界。

## 格式、配對與切頻的必要界線

- 長度要在截斷 RF buffer **之前**驗證。目前 `validateHeader()`／ACK 接受至少指定長度，
  接收路徑又可能先截短，不能直接移植成 v4 strict parser。端點、短包、長包、未知 type 必須拒收。
- signed24 差值要先判斷範圍再編碼，接收要 sign extension；不能取低 24 bits 後讓超界位置回繞。
  零 offset 是 24N121E，不能當無定位。不得因刪 fix 而產生看似有效的假位置。
- 現在 NETWORK_ID／SERVER_ID 已是固定常數，刪除不會憑空失去本來不存在的每站唯一性。
  但所有新 Server 都預設綁 E91C，若同時啟用便可能搶回同一 Client；必須實際確認每套配對。
  MAC 後 16 bits 的 nodeId 不是全域唯一，還需處理 0／FFFF 保留值與 ID 衝突。
- 換協定預期 v3／v4 互不接受，兩端協調更新並保留回復方式。無需為此先建多版本自動協商系統。
- 頻道掃描應標為「CAD 偵測比例、RSSI 統計、取樣時間」，不是「保證乾淨」或完整占用率。
  `getRSSI()` 預設是上一包，環境 RSSI 要用合適 RX 狀態下的 `getRSSI(false)`。
  單 radio 掃描期間收不到正常位置，必須整合 IRQ／ACK、停止依舊資料追蹤，完成或失敗回原頻率與 RX。
- 手動選頻只省掉自動選擇政策；雙端仍需交易識別、確認、重試與失敗後會合方式。
  「看到一包就保存、看不到就退回」在非對稱丟包下可能兩端永久分離。最後確認遺失、單邊重啟都要測。
  `recoverRadio()` 目前回編譯期頻率，加入設定後 boot／recovery 都須還原已確認頻道。

## 實作順序與驗收

1. 本輪 α 開關先作為單獨變更；只比較是否外推，不能拿它證明新封包或 2 Hz 已完成。
2. 封包另批實作：先定義 age／velocity／fix／分級語意、DATA 序號、精確解析與 API 變更。
   golden bytes、signed24 端點、超界／短長包、錯 ID、未知版本、seq wrap／重開機必須通過。
3. NMEA replay 覆蓋 RMC 單獨中斷、GGA 單獨中斷、舊速度、重送同一 epoch、品質過期。
   先以 1 Hz 驗證新協定與追蹤；再確認模組的真實 2 Hz 能力。
4. fake radio／clock 測 DATA／TEL／ACK 各相位、timeout／TxDone 遺失、RX 失敗、HTTP 延遲、
   millis wrap 與跨 slot；板上 DIO1／NMEA 時間戳另驗證真正頻率、UART 遺失及超時恢復。
5. 切頻獨立一批：每步丟包／重複／延遲、單邊斷電、radio recovery 後都能重新會合。
6. 最後作戶外單套與兩套共存測試；記錄獨立 DATA seq 成功率、最長失聯、ACK 間隔和實體指向。
   主機測試及編譯不能代替 GNSS／RF／機械證據。

## 原始碼證據

- [目前 wire v3](../include/protocol.h)：Header、DATA／TEL／ACK、MAC 佔位。
- [主程式](../src/main.cpp)：`buildDataPacket`、`predictClientPos`、`gpsModeAvailable`、
  `computeAirtimeBudget`、`validateHeader`、`logRxSummary`、`recoverRadio` 及 Client ACK 接收。
- [GPS 分級](../include/geo_math.h) 與 [追蹤 gate](../include/tracking_policy.h)。
- [序號與重同步](../include/command_freshness.h)、[單一配對](../include/client_binding.h)。
- 本機 `.pio/libdeps/tbeam-client/TinyGPSPlus/src/TinyGPS++.cpp` 的 RMC／GGA commit 分支。
- 本機 `.pio/libdeps/tbeam-client/RadioLib/src/modules/SX126x/SX126x.cpp` 的 airtime、
  blocking transmit、SNR、RSSI、CAD；[RadioLib SX126x API](https://jgromes.github.io/RadioLib/class_s_x126x.html)。
