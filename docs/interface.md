# 介面規格（Interface）

本文件彙整 Shore Spotter 的 wire／HTTP 契約及欄位對應：

1. **下層 — LoRa 封包**（Surfer ↔ 攝影站，RF 二進位格式）
2. **上層 — 攝影站 HTTP API**（攝影站 → 手機監控頁，JSON）
3. **欄位對應表**（封包欄位如何流向 API）

2026-09-23 未發行版將岸端角色統一命名為 **Station**：編譯環境為 `tbeam-station`／
`tbeam-station-ota`、角色旗標為 `ROLE_STATION`。`/api/track.station` 是岸端即時位置與感測器；
30 秒平均另用 `station_average`，避免兩種資料共用同名 key。升級後須重整內嵌網頁，外部 API
消費端也須更新原 `server`／`server_gps` 欄位。NVS 設定鍵與 LoRa 封包內容不因改名更動。
僅 Axiom 雲端保留明確的歷史 schema 2 欄位相容，見 [Axiom 記錄](axiom-log.md)。

---

# 1. LoRa 封包

本節對應 **0.6-dev／LoRa 協定 v5** 的本機實作；板上部署、GNSS 實際更新率及戶外
RF／機構行為仍須實測。v5 與舊 v4／v3 不相容，Client、Station 都須更新；不存在自動降版。
韌體版本與 wire 協定版本分別由 `firmware_version.h`、`protocol.h` 定義。

所有多 byte 整數明訂 **little-endian**，有號數採二補數。C++ 結構只是邏輯欄位，
**不能直接 memcpy 結構當封包**；唯一 wire 定義是 [protocol.h](../include/protocol.h) 的 codec。
接收端先取得實際 RF 包長，再依 version/type 精確比對長度；短包、長包、未知類型／版本、
非法值或矛盾旗標拒收。保留 LoRa PHY CRC，沒有應用 MAC、加密或認證。

## RF 參數與頻率

Station SD 記錄與 USB 狀態、開始／停止、列檔／讀回及檔案格式見
[SD 記錄](sd-log.md)。不依賴 GPS 控制模式，LoRa 契約保持不變。

| 參數 | 值 |
|---|---|
| 中心頻率 | 923.2 MHz，程式固定設定 |
| 頻寬 | 125 kHz |
| Spreading Factor | SF10 |
| Coding Rate | 4/5 |
| Sync Word | `0x12` |
| 前導碼／標頭 | 8 symbols／explicit header，PHY CRC 開啟 |
| 固定發射功率 | Client 20 dBm；Station 僅接收（初始化保留 17 dBm 設定）；ATPC 已移除 |
| DATA 排程 | 最新有效定位即送，受 airtime＋80 ms guard 限制；失效通知一次，無 DATA 心跳 |
| ACK | 已移除，無下行回覆 |
| TELEMETRY／DIAGNOSTIC | 各約每 60 秒獨立發送；GNSS DIAG 約 60 秒一包，可延後定位 |

同組兩端頻率、BW、SF、Sync Word 必須一致。explicit header 帶有發送端的 payload
Coding Rate；本專案 Client 發送使用 4/5。頻率設定不是整套設備的法規／審驗認定。
目前未實作頻道掃描或協商切頻。Client ID 或 Sync Word 的資料過濾不會消除同頻 RF 碰撞。
開機與無線電重建均使用各角色固定功率；Client 不讀寫舊 `shorespt_client/txpwr`、
`atpc` NVS 鍵，也不清除它們。固定功率不隨接收品質調整；目前 SF10／無 ACK 排程如下。

## 封包類型與空中時間

| type | 名稱 | 大小（含共用標頭） | 空中時間 | 用途 |
|---|---|---:|---:|---|
| 1 | MSG_DATA | 18 B | 329.728 ms | 完整 E7 位置、速度向量與定位品質；無來源 age |
| 4 | MSG_TELEMETRY | 11 B | 288.768 ms | 電量、溫濕度與精確衛星數 |
| 5 | MSG_DIAGNOSTIC | 17 B | 329.728 ms | Client 的 GNSS 間隔與執行計數 |
| 6 | MSG_GNSS_DIAGNOSTIC | 36 B | 493.568 ms | 單包完整 GPS 資料流／診斷年齡快照 |

type 2（舊 ACK）與 type 3（舊 HELLO）不接受。wire 升為 v5；所有類型標頭均用新版本，
Client／Station 必須一起更新，舊格式拒收，無自動降版。SF9 舊板也無法與 SF10 通訊。
2 Hz 是 GPS 請求值，實際 DATA 頻率由有效新定位、RF 占用與診斷共同決定，不能由設定推定收包率。

### 不阻塞的發送與空檔

Client 使用 `startTransmit → TxDone／timeout → finishTransmit`，完成後回到 standby，
不開啟 RX 或等待 ACK。Station 只接收。軟體 timeout 為各包向上取整的 ToA＋80 ms。
無線傳送期間仍服務 GNSS；Station 接收後繼續服務 Servo／UART。

Client 觀察最新同 epoch 快照，新的有效定位且 radio 空閒、guard 已滿即可發送，不設 1 Hz 上限。
RMC／GGA 尚未配對時最多短等 150 ms；新 GGA 可提供位置但不借舊 RMC 速度。
收集器直接提供最新 epoch；缺少的同 epoch 欄位保持未知，已送的 epoch 不重傳。
忙碌時只保留最新快照，不排隊補歷史位置；發送端座標必須可編碼且該新 epoch 首次到達後未滿 2 秒；推算 source age 不參與門檻。
有效→失效只送一包 fix=0 DATA；開機尚未定位及持續無效期間不送 DATA 心跳。
恢復的第一筆有效位置就是恢復通知。通知可能漏收，Station 仍按最後接受新位置後的本機經過時間（2 秒）判斷，不能只靠通知。
本機發射失敗仍消耗該筆發送機會，避免緊密重試；沒有 ACK／舊定位重傳。

DATA、TEL、DIAG 各有獨立序號，GNSS DIAG 每包使用獨立 seq；診斷不消耗 DATA seq。
SF10 的 DATA／DIAG 約 330 ms，GNSS DIAG 約 494 ms，TEL 約 289 ms；所有包在實際 TxDone 後
至少留 80 ms guard。TEL／DIAG 約 60 秒一次；GNSS 每包約 60 秒取樣一次。
診斷不依賴有效定位：無 fix 時仍送。每秒最多一包診斷，同時到期優先 TEL、DIAG、GNSS DIAG；
有持續待送 DATA 時與 DATA 交錯，必要時延後一筆定位。失效通知優先於診斷。
GNSS 診斷單包即可解碼，無分頁組裝／舊序號等待；診斷不刷新位置接收時間。

`skipped_slots` 保留舊欄位名稱；本版表示 DATA 因 radio 忙碌／未就緒而延期的工作次數，
同一次等待只加一次，之後只送最新快照。無新 epoch、診斷等待均不算 RF 失敗。
Client 序列摘要另列 `diagnostic_tx`。`pkt_rate` 低於 2 Hz 不代表丟包，需看 DATA 序號缺口。

雙端開機先初始化 PMU；每次 LoRa 初始化／復原前均設定 ALDO3 3300 mV、啟用並讀回確認，
短等 10 ms 後才開始 SPI／radio.begin。PMU 不可用、寫入或讀回失敗不繼續 radio 初始化。
寄存器讀回不是電壓實測；仍須實機確認供電與距離。

## 共用標頭（6 bytes）

| offset | bytes | wire 欄位 | 規則 |
|---|---:|---|---|
| 0 | 1 | magic | `0x53` |
| 1 | 1 | version/type | 高 4 bits 為版本 5，低 4 bits 為 type |
| 2–3 | 2 | client_id | 上行表示來源；下行表示接收對象 |
| 4–5 | 2 | seq | DATA 專用遞增序號；TEL／DIAG 各自遞增；GNSS DIAG 每包使用獨立 seq |

不再傳 networkId、獨立 srcId／dstId、payloadLen、空白 MAC。長度由 type 決定。
wire ID 0／FFFF 不可用；白名單還保留舊 Station ID `0010` 的禁用規則。

## DATA（18 bytes＝標頭 6＋定位資料 12）

| offset | bytes | wire 欄位 | 編碼／未知值 |
|---|---:|---|---|
| 6–9 | 4 | latE7 | int32，round(lat×10⁷)，範圍 −900000000..900000000 |
| 10–13 | 4 | lonE7 | int32，round(lon×10⁷)，範圍 −1800000000..1800000000 |
| 14 | 1 | speedDmS | 0–254＝0–25.4 m/s；255 未知／超界 |
| 15–16 | 2 | course_and_flags | 方向／衛星級距／有效旗標，見下表 |
| 17 | 1 | hdop10 | HDOP×10 向上取整，0–254；255 未知／超界 |

| course_and_flags bits | 內容 |
|---|---|
| 0–11 | courseDeg10：0–3599；4095 未知；3600–4094 非法 |
| 12–13 | satelliteClass：0 未知、1＝0–5 顆、2＝6–7 顆、3＝≥8 顆 |
| 14 | fix：發送端認定位置有效且新 epoch 到達後未逾時 |
| 15 | velocityValid：同 epoch 向量有效，fix 有效、速度已知且 ≥0.3 m/s、方向已知 |

完整經緯度取消固定原點，E7 是編碼解析度，不代表實際 GNSS 達到公分精度，也不擴大
磁偏角表的適用範圍。fix=0 時發送端寫零座標，零不能當成有效目標。超界有效位置拒收。
沒有 DATA 來源年齡欄位；速度／方向保留供診斷，velocityValid 不代表品質允許控制外推。

### 同 epoch 快照與新鮮度

雙端請求 L76K **115200 baud／500 ms（2 Hz）／RMC＋GGA**。先在 9600 發切換 baud 命令，
再於 115200 重送相同命令以涵蓋 MCU 重啟但 GNSS 仍供電的情況；只設定本次運行，不寫 GNSS flash。
**這是驗證版，不是原廠保證組合**：L76K 原廠對 >1 Hz 要求 115200 且只開一種 NMEA 語句。
本專案追蹤需要 RMC＋GGA，依確認選擇先實測雙語句；不靜默降回 1 Hz、不用缺欄位冒充完整定位。
每 5 秒以計數差量測 epoch／RMC／GGA Hz；Station 診斷 API 與 Axiom 有三個實測值，Client 序列有
完整三值，低頻 DIAG bits6／7 提供整體驗證結果。最後一次 epoch 間隔不能單獨證明持續 2 Hz。
原廠限制見 [Quectel L76K 協定，第 22–23 頁](https://forums.quectel.com/uploads/short-url/kmb3zNuV2SldThkOOJoNg9th40S.pdf)。

[gnss_snapshot.h](../include/gnss_snapshot.h) 驗證NMEA checksum，以相同UTC epoch配對RMC／GGA：
新 GGA 可單獨提供位置／品質，但無速度；只有新 RMC 時品質保持未知，不借上一個 epoch。
最新明確無效的 RMC／GGA 優先撤銷位置；重複 epoch 不刷新首次到達時間。
UART 服務間隔 >200 ms 仍丟棄積壓／半句，等待更新 epoch。UTC 倒退保留既有 paced recovery：
至少 3 個有效新 epoch，UTC／本機皆跨越 2 秒、每步 100–2000 ms 且間隔誤差 ≤200 ms，才重建。
來源年齡 anchor 的偏移／恢復仍可診斷，但不再用推算值決定是否追蹤。

```text
Client／岸端本地 GPS：fix 有效，最新 epoch 首次到達後 <2000 ms
Station 遠端位置：fix 有效，最後接受 DATA 後 <2000 ms
α=0（預設）：直接使用收到的位置
α=1（選用）：只以接收後經過時間外推，還須通過兩端品質與向量檢查
```

GNSS `source_age_ms`／`age_basis=nmea_epoch_aligned_arrival` 只保留診斷；未知模組內部延遲
不當成精確量測。Station 本地 `arrival_age_ms`／`freshness_basis=new_epoch_arrival` 是實際門檻；
Client API `age_basis=rx_elapsed_only`，`source_age_ms`／`sample_age_ms` 為 null，`rx_age_ms` 保留。
沒有跨裝置對時／PPS。品質差或未知仍追有效新位置並警告；外推才要求兩端衛星 ≥6、HDOP ≤3，
同 epoch 速度方向有效。明確失效／逾時後按速限轉完最後目標，不繼續外推。
DATA 連續時拒絕重複／倒退；距最後接受 DATA ≥3000 ms，第一包 fix=1 直接重建序號並記錄原因。
fix=0 不能重建；無 boot ID／握手，不區分重啟與失聯。

## 舊 ACK（type 2，已移除）

目前單向傳送不再建立或解析 ACK。type 2 保留編號，收到會視為未知格式拒收。
RSSI／SNR 仍由 Station 接收時量測，供網頁與雲端診斷；Client 不接收收件確認。

## TELEMETRY（11 bytes＝標頭6＋資料5）

| offset | bytes | 欄位 | 編碼／未知值 |
|---|---:|---|---|
| 6–7 | 2 | batteryMv | uint16 mV；0未知 |
| 8 | 1 | tempC | int8整數°C；−128未知 |
| 9 | 1 | humidityPct | uint8 0–100%；255未知，其餘值拒收 |
| 10 | 1 | satellites | 精確衛星數；255未知，僅低頻診斷，不參與即時追蹤門檻 |

## DIAGNOSTIC（17 bytes＝標頭6＋資料11）

| offset | bytes | 欄位 | 編碼 |
|---|---:|---|---|
| 6–7 | 2 | epochIntervalMs | 最新接受GNSS epoch的間隔ms；初始0 |
| 8–9 | 2 | backlogDrops | Client UART積壓丟棄次數 |
| 10–11 | 2 | nmeaErrors | Client NMEA拒收句數；正常略過GSV等不算錯誤 |
| 12–13 | 2 | txErrors | Client TX錯誤次數 |
| 14–15 | 2 | skippedSlots | Client DATA因radio忙碌／未就緒而延期的工作次數；同一次等待只計一次 |
| 16 | 1 | status | bits0..5依序為haveEpoch、fix、velocityValid、haveGga、haveRmc、ageUncertaintySet；bit6為5秒頻率量測已完成，bit7為epoch／RMC／GGA均觀察到約2 Hz |

uint16欄位發送時飽和至65535，計數自Client開機累計；不會wrap成小值。此包沒有Client boot ID，
bit7=1而bit6=0為矛盾旗標，拒收；約2 Hz的判準是三者均在1.6–2.4 Hz，並非長時間驗收。
不能僅靠計數下降判定重啟。status是低頻快照，不能取代DATA的即時gate；也不增加DATA包長。


## GNSS_DIAGNOSTIC（type 6，36 bytes）

單包完整快照，獨立 seq，每 60 秒找空檔發送；無 page 欄位或組裝狀態。
接收端先檢查版本、長度與綁定，再直接替換報告，Client 重啟後不等待舊報告到期。
診斷不控制 Servo，也不刷新 DATA 接收時間。`rx_age_ms` 從此包收到時起算。

| offset | 欄位 |
|---|---|
| 0–5 | 共用標頭 |
| 6–9 | sourceAgeMs uint32（推算值，僅診斷） |
| 10–13 | utcMs uint32，UTC 當日毫秒 |
| 14–15／16–17／18–19 | byteAgeMs／sentenceAgeMs／advanceAgeMs，uint16 |
| 20–21／22–23 | epochs／timeResyncs，uint16 |
| 24–25／26–27／28–29 | missingOrInvalidTime／backwardEpochs／duplicateEpochs，uint16 |
| 30–31／32–33 | rejectedSentences／checksumErrors，uint16 |
| 34 | flags bits0..4：sample、raw fix、RMC、GGA、recovering；其餘保留為 0 |
| 35 | satellites，255 未知 |

uint32 年齡未知為 UINT32_MAX；uint16 年齡 65535 未知、65534 表示 ≥65534 ms；
計數飽和至 65535。UTC 非未知時須 <86400000。狀態以 UART／語句／epoch 推進與 fix 判斷，
不把推算來源年齡偏移判成新位置失效。`pages_mask` 舊 API 欄位為 null，設定 pages=1。

## 單一 Client 綁定

Station只接受綁定的client_id。NVS `gpsclient`保留既有綁定；若尚無該鍵，遷移舊`wl`第一個
有效ID，舊空集合保持未綁定。**全新Station預設未綁定（0）**，需從網頁指定自己的Client。
ID取自ESP32 MAC末16 bits，並非全域唯一；多套仍須確認沒有重號、沒有多台Station誤綁同一Client。

變更／清空綁定會回手動並清除舊Client位置、遙測、診斷、濕度基準與滾動統計；NVS寫入失敗
不更換RAM狀態。`add`不能擴成第二個Client。ID、序號與CRC用來配對／拒收錯誤資料，沒有認證能力。

## HTTP 與 OTA 存取

HTTP API與OTA均未設應用層認證／密碼，同熱點可連線裝置可存取；HTTP控制上下文只防止
過期／重複命令，不是登入認證。封包精簡沒有改變既有手動／GPS／UART控制授權語意。

---

# 2. 攝影站 HTTP API

攝影站以 WiFi station (STA) 模式連線手機熱點。

- 熱點 SSID / 密碼設定於 [../include/wifi_config.h](../include/wifi_config.h)（`WIFI_SSID` / `WIFI_PASSWORD`）
- 攝影站開機會自動連線，IP 由手機分配，開機時顯示於 OLED

以下 endpoint 透過瀏覽器或任何 HTTP client 存取，基底 URL 為攝影站的 IP（例：`http://192.168.x.x`）。

## `GET /`

回傳完整 Web UI（單頁 HTML，內嵌 Servo 控制 + Canvas 極座標雷達圖，無外部 CDN 依賴）。
兩個分頁：**雷達**（雷達／地圖、手動／GPS／UART、底部 slider）與 **資訊**（共用最高速度、α、遙測、校正偏移與重新校正）。
OpenStreetMap 底圖由瀏覽器連網載入；頁面程式與雷達不依賴外部 CDN，底圖則需要網路。

### 全頁共用 HTTP 請求排程

內嵌頁面所有 API 請求共用一個排程；同一頁同時只執行一個請求，直到回應內容讀取完成
才開放下一個。等待中的優先順序為控制 POST、track／設定讀取、status、除錯／文字 log；
不會中斷已開始的請求。相同背景讀取以 key 合併，避免輪詢累積成重複佇列。

背景讀取的排隊期限與開始傳輸後的 timeout 各為 2 秒。控制命令從加入佇列起保留原始
2 秒期限，必要時先更新控制上下文；過期或控制世代已變更的等待命令不會延後重播。
timeout 會嘗試取消傳輸；若瀏覽器未能結束 fetch／內容讀取，排程仍保留該位置，避免
再開第二個重疊請求。這是單一網頁的行為，不限制其他瀏覽器、分頁或外部 HTTP client，
也不包含外部地圖圖磚載入；Station 的同步 WebServer 仍可能受網路等待影響。

## 分頁圖示 `GET /icon-16.png` · `/icon-32.png` · `/icon-192.png` · `/favicon.ico`

回 `image/png`，帶 `Cache-Control: public, max-age=604800`。

| 端點 | 尺寸 | 位元組 | 用途 |
|---|---|---|---|
| `/icon-16.png` | 16×16 | 389 | 分頁列（1× DPI）|
| `/icon-32.png` | 32×32 | 538 | 分頁列（2× DPI）、書籤 |
| `/icon-192.png` | 192×192 | 2120 | PWA 加到主畫面、`apple-touch-icon` |
| `/favicon.ico` | 32×32 | — | 同 `/icon-32.png`；瀏覽器認的是 Content-Type 不是副檔名 |

合計約 3 KB flash。全部是**調色盤 PNG**（整張圖只有 6 種顏色，比 RGBA 小 3–5 倍），
嵌在韌體裡由攝影站自己供應，離線一樣看得到。

`/favicon.ico` 一定要有 handler：沒有的話瀏覽器自動發出的那個請求會落到 `onNotFound`
的 `302 -> /`，於是**每開一次頁面就多抓一次完整的 44 KB HTML**。監控頁的 `<head>` 也
明確列出了尺寸，讓瀏覽器直接挑對，不必先去試 `/favicon.ico`。

> 圖示的來源是 [`tools/make_icon.py`](../tools/make_icon.py)，它產生
> `include/web_icon.h`。圖示是用幾何繪製的，所以 repo 裡**不另外放一份 SVG** ——
> 兩份副本遲早會走岔。要改設計就改那個腳本裡的常數再重跑。
>
> 設計上畫的是這台機器本身而不是一朵浪：半圓 = servo 的 0–180° 行程、橘扇形 = 目前
> 瞄準、紅點 = 衝浪者、白點 = 攝影站，顏色與監控頁雷達畫面一致。理由是 16px 下浪會
> 糊成一團，而且那是任何衝浪 app 都長的樣子。
>
> manifest 的 `icons` 刻意**不宣告** `purpose:"maskable"`：紅點靠近圖磚邊緣，
> Android 的圓形遮罩會把它切掉。

## `GET /api/track`

即時狀態，前端每 1 秒輪詢一次（輕量，不含軌跡）。過去 5 分鐘軌跡改由前端自行累積這些即時點，攝影站不再儲存。

**Response**

```json
{
  "linked": true,
  "lora_fps_10s": 2.0,
  "bearing": 242.3,
  "client": {
    "lat": 25.123456,
    "lon": 121.123456,
    "fix": 1,
    "velocity_valid": true,
    "speed_cms": 310,
    "course_deg10": 2423,
    "satellite_class": 3,
    "satellites": 9,
    "satellites_age_ms": 5000,
    "hdop": 1.2,
    "rx_age_ms": 100,
    "source_age_ms": null,
    "sample_age_ms": null,
    "age_basis": "rx_elapsed_only",
    "last_rx_sec": 0
  },
  "station": {
    "lat": 25.111111,
    "lon": 121.111111,
    "fix": 1,
    "satellites": 7,
    "hdop": 1.5,
    "temp_c": 30,
    "humidity_pct": 76,
    "batt_pct": 100,
    "charging": true
  },
  "telemetry": {
    "batt_mv": 3850,
    "temp_c": 28,
    "humidity_pct": 72,
    "last_rx_sec": 5
  },
  "servo": {
    "angle": 87.0,
    "target": 87.0,
    "mode": "gps",
    "source": "gps",
    "calibrated": true,
    "calibration_ready": false,
    "mount_offset_deg": 12.5,
    "north_reference": "magnetic",
    "declination_deg": -5.04,
    "gps_ready": true,
    "uart_state": "inactive",
    "uart_ready": false,
    "pwm_ok": true,
    "moving": false,
    "velocity_deg_s": 0,
    "motion_fault": false,
    "rejected_commands": 0,
    "rejected_gps_sequence": 0,
    "control_boot_id": 12345678,
    "control_epoch": 87654321,
    "command_seq": 0,
    "clock_ms": 123456,
    "gps_available": true,
    "gps_usable": true,
    "speed_limit_deg_s": 30,
    "prediction_enabled": true,
    "prediction_alpha": 1
  }
}
```

| 欄位 | 說明 |
|---|---|
| `linked` / `data_fresh` | 5 秒內接受過 DATA；不代表 GPS 仍新鮮，不再用於判斷有沒有收到低頻遙測 |
| `rf_alive` | 90 秒內接受過綁定 Client 的 DATA／TEL／DIAG／GNSS 任一封包；錯誤、拒收或未綁定來源不刷新；不延長追蹤期限 |
| `rf_age_ms` | 距最後接受綁定 Client 任一有效封包的 ms；從未收到為 null |
| `lora_fps_10s` | Station 最近 10,000 ms 實際接受的 DATA 封包數 ÷ 10，單位 FPS；不計 TEL／DIAG、CRC 錯誤或拒收包。固定時間窗，開機未滿 10 秒也除以 10；斷流自然降至 0，與 5 秒 linked 門檻分開 |
| `bearing` | 攝影站 → Surfer 方位角（度），無效時為 `-1` |
| `client.fix` | 已通過來源age＋airtime＋接收年齡＋200 ms不確定量門檻的fix，0表示目前不可用 |
| `client.speed_cms` | 速度換算為cm/s，10 cm/s一格；未知為`null`，是否可外推另看velocity_valid |
| `client.course_deg10` | 行進方向0.1°；未知為`null`，不可只靠非null判斷仍新鮮 |
| `client.velocity_valid` | 位置仍新鮮且速度向量可外推；false不必然代表位置不可用 |
| `client.satellite_class` | DATA即時衛星分級：0未知、1為0–5、2為6–7、3為≥8，不是假造精確顆數 |
| `client.satellites` / `satellites_age_ms` | 低頻TEL精確顆數／距收到TEL的ms；無資料或顆數未知為null，TEL滿90秒也不再呈現顆數 |
| `client.rx_age_ms` / `source_age_ms` / `sample_age_ms` | 接收後時間／null／null；v5 不量測完整定位延遲 |
| `client.age_basis` | 固定`nmea_epoch_aligned_arrival`，是估計時間基準，非GNSS測量時鐘同步 |
| `client.last_rx_sec` | 距上次收到DATA的秒數；從未收到為`-1`，失聯後仍累計 |
| `telemetry.batt_mv` | Surfer端電池電壓mV（約每60秒排送，忙碌時延後）|
| `telemetry.temp_c` | Surfer 端溫度，`null` = 無感測器 |
| `telemetry.humidity_pct` | Surfer 端濕度 %，`null` = 無感測器 |
| `station.fix` | 攝影站本身的 GPS fix 狀態 |
| `station.temp_c` | 攝影站本機溫度，`null` = 無感測器 |
| `station.humidity_pct` | 攝影站本機濕度，`null` = 無感測器 |
| `servo.angle` | Servo 目前命令角度（0–180°）；GPS／UART／手動共用不限頻微秒軌跡，只套用共用速限、預設 30°/s，移動期間落後 `target`，不是機械位置回饋 |
| `servo.target` | 追蹤目標角度（0–180°）；GPS 的 α 開啟時以外推位置計算，關閉時以最後收到位置計算 |
| `servo.mode` | `manual` / `gps` / `uart` / `paused` |
| `servo.source` | 實際控制來源：`manual` / `gps` / `uart` / `hold` |
| `servo.gps_available` | 兩端持續更新且定位有效即可選 GPS；品質差仍可用，定位無效／逾時為 false |
| `servo.gps_usable` / `servo.gps_ready` | 另滿足校正與磁偏角／GPS 模式、PWM 正常且 GPS 使用條件成立；無額外等待 |
| `servo.uart_state` / `servo.uart_ready` | inactive / waiting / tracking / watchdog_hold；是否有新鮮 SET |
| `servo.north_reference` | 指南針輸入基準，固定 `magnetic` |
| `servo.declination_deg` | 磁北轉真北需加的角度（東正西負），`null` = 位置／日期或模型不可用 |
| `servo.calibrated` | 是否已有有效校正參考；開機 90°預設參考即為 true|
| `servo.calibration_ready` | 命令位置、限速器位置及目標均精確為 90°，且沒有命令移動；不是機械位置回饋 |
| `servo.mount_offset_deg` | Servo→磁北座標的安裝偏移角，開機預設 90°，重新校正只存 RAM |
| `station.satellites` | Station本機TinyGPS顯示資訊，`-1`未知；真正追蹤gate另用同epoch collector |
| `client.hdop` / `station.hdop` | Client未知為`null`，Station顯示資訊未知為`-1`；Client為向上量化的即時DATA值 |
| `station.batt_pct` | 攝影站 18650 電量 %（3.2V=0%、4.15V=100%），`-1` = 無電池/未知 |
| `station.charging` | 攝影站是否接外部電源（USB/Type-C，VBUS 在），用於 ⚡ 指示 |

> API 不回傳軌跡。前端每秒嘗試加入有效且不同的位置，只保留／顯示最近 5 分鐘；
> 重整頁面會重新累積，背景／鎖屏可能漏點。GPX／Strava 與清除軌跡操作已移除。
> SD 逐包及詳細狀態不依賴瀏覽器，見 [SD 記錄](sd-log.md)。

> GPS 品質判定（四級，韌體與監控頁共用同一組門檻，見 `geo::gpsSignal()` 與 web_ui.h 的 `gpsGrade()`）：
> **Good** = 有定位 且 HDOP ≤ 1.5 且 sats ≥ 8；**OK** = 有定位 且 HDOP ≤ 3.0 且 sats ≥ 6；
> **Bad** = 收得到衛星但定位不堪用（無定位／sats < 4／達不到 OK）；**Miss** = 完全沒訊號。
> LoRa 的 **Miss** 表示未收到封包。

> Client即時品質由DATA的衛星分級與HDOP判斷；精確衛星數另由低頻TEL提供，不能當成逐次DATA更新的數值。
> Web UI的預期精度是HDOP換算的提示，不是實測誤差界限；GPIO輸出與Servo角度也不是機械回授。

## `GET /api/status`

GPS 訊號品質、LoRa 訊號統計、Servo 校正狀態。

**Response**

```json
{
  "station_gps": {
    "fix": 1,
    "satellites": 9,
    "hdop": 1.2
  },
  "lora": {
    "rssi": -85.0,
    "snr": 7.5,
    "rssi_avg": -87.3,
    "snr_avg": 6.9,
    "pkt_rate": 1.98,
    "rx_data": 102,
    "rx_telemetry": 11,
    "rx_diagnostic": 10,
    "rx_drop": 3,
    "drop_rate": 0.024,
    "ack_enabled": false,
    "ack_tx": null,
    "ack_busy": false,
    "ack_errors": null,
    "ack_skipped": null,
    "ack_last_error": null
  },
  "env": {
    "temp_c": 30,
    "humidity_pct": 76
  },
  "health": {
    "firmware_version": "0.6-dev",
    "protocol_version": 5,
    "uptime_s": 5432,
    "heap_free": 188304,
    "heap_min": 173016,
    "reset_reason": "1",
    "rx_error": 0
  },
  "servo": {
    "angle": 87.0,
    "target": 88.2,
    "mode": "gps",
    "source": "gps",
    "calibrated": true,
    "pwm_ok": true,
    "mount_offset_deg": 12.5,
    "north_reference": "magnetic",
    "declination_deg": -5.04,
    "gps_ready": true,
    "uart_state": "inactive",
    "uart_ready": false,
    "moving": false,
    "velocity_deg_s": 0,
    "motion_fault": false,
    "rejected_commands": 0,
    "rejected_gps_sequence": 0,
    "control_boot_id": 12345678,
    "control_epoch": 87654321,
    "command_seq": 0,
    "clock_ms": 123456,
    "gps_available": true,
    "gps_usable": true,
    "speed_limit_deg_s": 30,
    "prediction_enabled": true,
    "prediction_alpha": 1
  },
  "alerts": [
    {
      "id": "client_water",
      "level": "error",
      "title": "追蹤器可能已經進水",
      "detail": "防水盒裡的濕度到了 92%。請立刻請衝浪者上岸，把裝置擦乾並檢查防水圈有沒有夾到東西。"
    }
  ],
  "timing": {
    "control_gap_max_ms": 37,
    "control_gap_over_250ms": 0,
    "loop": {
      "last_ms": 1.98,
      "max_ms": 59.81,
      "over_50ms": 1
    },
    "http": {
      "last_ms": 1.58,
      "max_ms": 9.76,
      "over_50ms": 0
    },
    "bme280": {
      "last_ms": 0.77,
      "max_ms": 1.87,
      "over_50ms": 0
    },
    "pmu": {
      "last_ms": 1.48,
      "max_ms": 4.52,
      "over_50ms": 0
    },
    "oled": {
      "last_ms": 34.78,
      "max_ms": 36.42,
      "over_50ms": 0
    },
    "lora": {
      "last_ms": 0.0,
      "max_ms": 7.78,
      "over_50ms": 0
    },
    "ota": {
      "last_ms": 0.09,
      "max_ms": 0.38,
      "over_50ms": 0
    },
    "uart_late_polls": 0,
    "uart_discarded_bytes": 0,
    "uart_rejected_commands": 0,
    "motion": {
      "last_ms": 0.01,
      "max_ms": 0.5,
      "over_50ms": 0
    }
  }
}
```

> `servo` 區塊與 [`GET /api/track`](#get-apitrack) **完全相同**（同一個
> `appendServoJson()`）。原本兩個端點各寫一份、欄位還不一致，前端得靠兩個端點
> 拼一份狀態；現在改哪一邊都不會走岔。

### `alerts` — 現場提醒

GPS 定位／連線／未校正的操作提醒只在 GPS 模式顯示；UART 不因 GPS 品質不足報停止追蹤。

給站在沙灘上的人看的訊息，不是給工程師看的。判斷與措辭都在韌體端
（`main.cpp` 的 `appendAlertsJson()`，門檻在 [`include/alerts.h`](../include/alerts.h)），
監控頁只負責畫成最上方那條訊息列 —— 這樣之後 OLED 要顯示同一組提醒不必再實作一次規則。

陣列已依嚴重度排序（`error` 在前）。沒有任何異常時是空陣列。

| 欄位 | 說明 |
|---|---|
| `id` | 穩定識別字，前端可用來去重或記住「已讀」|
| `level` | `error` = 現在就要處理／`warn` = 留意 |
| `title` | 一句話說發生什麼事 |
| `detail` | 說明 + 該做什麼，內含當下的實際數值 |

目前定義的提醒：

| `id` | 等級 | 觸發條件 | 意思 |
|---|---|---|---|
| `client_water` | error | 盒內濕度 ≥ 90% | 追蹤器幾乎確定進水 |
| `client_water` | warn | 濕度 ≥ 80% **且**比基準高 ≥ 15 點 | 可能正在滲水 |
| `client_batt` | error / warn | 電量 ≤ 10% / ≤ 20% | 追蹤器快沒電 |
| `client_temp` | error / warn | 溫度 ≥ 60°C / ≥ 50°C | 追蹤器過熱 |
| `client_gps` | warn | 客端 GPS 分級為 Bad/Miss | 鏡頭可能追錯位置 |
| `client_link` | error / warn | 未收到封包 ≥ 30 s / ≥ 10 s | 失聯 |
| `client_never` | warn | 開機後從未收到封包 | 沒開機或不在白名單 |
| `srv_water` | warn | 站體濕度同上規則 | 攝影站受潮 |
| `srv_batt` | error / warn | 同上（充電中不觸發）| 攝影站快沒電 |
| `srv_temp` | error / warn | 同上 | 攝影站過熱 |
| `srv_gps` | warn | 站體 GPS 分級 Bad/Miss | 方位計算會偏 |
| `servo_fault` | error | `servoPwmReady == false` | 雲台沒有反應 |
| `mount_uncal` | warn | GPS 模式且未校正 | GPS 保持，至資訊頁輸入指南針角度校正 |
| `uart_wait` | warn | UART 模式且無有效 SET | 維持最後輸出角度 |

**濕度為什麼要看基準而不是絕對值**：海邊空氣本來就 80% 起跳，封盒時關進潮濕空氣是常態，
只用絕對門檻會整天誤報。進水真正的特徵是「相對開機值單調上升」，所以兩條規則並用：
絕對值破 90% 直接判定，其餘看相對開機第一筆讀數的升幅。

| 欄位 | 說明 |
|---|---|
| `station_gps.satellites` | 定位使用中的衛星數，`-1` = 無效 |
| `station_gps.hdop` | 水平精度因子，數值越小越好，`-1` = 無效 |
| `lora.rssi` | 最近一筆位置封包 RSSI（dBm）|
| `lora.snr` | 最近一筆位置封包 SNR（dB）|
| `lora.rssi_avg` | 近 20 筆 RSSI 滾動平均，`null` = 無資料 |
| `lora.snr_avg` | 近 20 筆 SNR 滾動平均，`null` = 無資料 |
| `lora.pkt_rate` | DATA的60秒統計；首次完成前0，距最後DATA滿5秒回0；不是GNSS epoch更新率 |
| `lora.rx_data` | 成功解析的 DATA 封包累計 |
| `lora.rx_telemetry` / `lora.rx_diagnostic` | 成功接受的TEL／DIAG累計 |
| `lora.rx_drop` | 驗證失敗或白名單不符封包累計 |
| `lora.drop_rate` | 拒收比例 `rx_drop/(rx_data+rx_telemetry+rx_diagnostic+rx_drop)`，不包含完全沒收到的封包 |
| `lora.ack_enabled` | 固定 false，已移除 ACK |
| `lora.ack_busy` | 固定 false，沒有 ACK 傳送中狀態 |
| `lora.ack_tx` / `ack_errors` / `ack_last_error` / `ack_skipped` | 舊欄位保留為 null，表示不適用 |
| `env.temp_c` | 攝影站本機溫度 |
| `env.humidity_pct` | 攝影站本機濕度 |
| `health.firmware_version` | Station 正在執行的專案版本字串（目前 `0.6-dev`），不是 Client 版本或 LoRa 協定版本 |
| `health.protocol_version` | Station使用的LoRa wire版本，目前4 |
| `health.uptime_s` | 開機秒數 |
| `health.heap_free` | 目前可用 heap |
| `health.heap_min` | 開機後最小可用 heap |
| `health.reset_reason` | ESP 重啟原因代碼 |
| `health.rx_error` | LoRa 接收錯誤碼累計 |
| `servo.angle` | Servo 目前輸出的命令角度（0–180°），不是機械回授量測 |
| `servo.target` | 追蹤時的目標角度 |
| `servo.mode` | `manual` / `gps` / `uart` / `paused` |
| `servo.source` | 實際控制來源：`manual` / `gps` / `uart` / `hold` |
| `servo.gps_available` | 兩端持續更新且定位有效即可選 GPS；品質差仍可用，定位無效／逾時為 false |
| `servo.gps_usable` / `servo.gps_ready` | 另滿足校正與磁偏角／GPS 模式、PWM 正常且 GPS 使用條件成立；無額外等待 |
| `servo.uart_state` / `servo.uart_ready` | inactive / waiting / tracking / watchdog_hold；是否有新鮮 SET |
| `servo.north_reference` | 指南針輸入基準，固定 `magnetic` |
| `servo.declination_deg` | 磁北轉真北需加的角度（東正西負），`null` = 位置／日期或模型不可用 |
| `servo.calibrated` | 是否已鎖定 `mount_offset` |
| `servo.pwm_ok` | LEDC PWM 是否正常；`false` 代表雲台完全無法控制 |
| `servo.mount_offset_deg` | Servo→磁北座標安裝偏移角，只存 RAM |
| `alerts[]` | 現場提醒，見上一節 |

### `timing` — 同步操作與服務間隔

`GET /api/status` 另包含 timing，數值自開機累計，不逐次寫 log：

| 欄位 | 含義 |
|---|---|
| `timing.control_gap_max_ms` | 相鄰兩次 serviceControl 呼叫的最大間隔；第一筆不計開機等待 |
| `timing.control_gap_over_250ms` | 服務間隔 ≥250 ms 的次數 |
| `timing.loop/http/bme280/pmu/oled/lora/ota.last_ms` | 該同步工作最近一次耗時（ms） |
| 上述各項 `.max_ms` / `.over_50ms` | 最大耗時／耗時 ≥50 ms 次數 |
| `timing.uart_late_polls` | UART 因 ≥250 ms 未服務而丟棄緩衝的次數 |
| `timing.uart_discarded_bytes` | 上述丟棄的 bytes 累計 |
| `timing.uart_rejected_commands` | 非 SET、格式／範圍錯誤、過期半行、同步邊界丟棄等拒絕行數 |

HTTP 內呼叫的 PMU 讀取也計入 HTTP 耗時；各項不是互斥、不能加總。HTTP／loop 本次
耗時需等返回後才更新，狀態回應讀到的可能是上一筆。I2C 每筆交易 timeout 10 ms，
一次感測器操作可含多筆交易；WebServer 仍同步，這些設定不保證控制期限。

### `timing.http_detail` — 已完成 HTTP 請求的分段耗時

此欄位由 `/api/status` 提供，SD／HTTP 分析可取用 status 快照；`/api/debug` 不重複附帶。
它量測板上一次 `handleClient()` 呼叫，並將進入請求處理的呼叫與未進入者分開統計。
回應只看得到先前已完成的請求，不能包含自己尚未完成的同步寫入時間。

| 欄位 | 說明 |
|---|---|
| `requests` | 已進入請求處理、且 `handleClient()` 已返回的次數 |
| `polls_without_request` | 未進入請求處理的呼叫次數，包含閒置、未完成／被拒絕的解析及解析錯誤回覆 |
| `slow_requests` | 上述已完成請求 `total_ms >= 50` 的次數 |
| `max_poll_without_request_ms` | 未進入請求處理之單次呼叫的最大耗時，不混入 `max`／`slowest` |
| `max` | 各分段的歷來最大值：`total_ms`、`pre_handler_ms`、`build_ms`、`write_ms`、`other_ms`；可能來自不同請求，不能相加 |
| `last` / `slowest` | 最近／歷來最慢的已完成請求；尚無已完成請求時為 `null` |

`last`、`slowest` 的欄位如下；時間由微秒換算為 ms，JSON 保留三位小數。

| 欄位 | 說明 |
|---|---|
| `route` | 路徑分類：`root`、`track`、`status`、`debug`、`log`、`control`、`other` |
| `total_ms` | 該次完整 `handleClient()` 呼叫耗時 |
| `pre_handler_ms` | 進入處理函式前的接受連線、解析／派送工作；不等於單純解析耗時 |
| `build_ms` | 明確包覆的回覆組裝時間，目前為 track／status／debug 的 JSON 與 log 文字 |
| `write_ms` | 同步寫入呼叫耗時；包含其本機等待，不能當成純網路 RTT 或 Client 收到資料的時間 |
| `other_ms` | 其餘處理／返回工作；未單獨包覆的回覆組裝也包含於此 |
| `bytes_written` | 寫入接口實際回傳的 bytes，包含經該接口送出的標頭與內容；不是 JSON 本文大小 |
| `short_writes` | 寫入接口回傳長度少於要求長度的次數，不等於 RF 丟包數 |

`root` 對應 `/`；track／status／debug／log 對應同名 `/api/...`。`control` 包含
`/api/servo`、其子路徑、`/api/track/` 子路徑與 `/api/whitelist`，不依 GET／POST 分類。
單筆 `total_ms` 由四個互斥分段組成，可用來辨別解析／派送、組裝資料、同步寫入或其他
工作的占比；舊 `timing.http` 仍涵蓋整次呼叫，不能再與這些分段相加。這些數值不含手機
排隊、尚未進入本次呼叫的傳輸等待等完整端到端時間，也不表示物理控制期限已獲保證。

## `GET /api/debug`

唯讀除錯快照，`Content-Type: application/json`、`Cache-Control: no-store`。不需要控制
上下文，也不會啟用追蹤、修改設定、清除事件或文字紀錄。回應 `schema_version: 3`；
事件改為每次最多 8 筆的增量分頁，升級 Station 後需重整網頁。

| 查詢參數 | 規則 |
|---|---|
| `boot_id` / `since` | 兩者必須同時提供或同時省略；十進位 uint32（0..4294967295），不接受空值、符號、空白、小數或混雜字元 |
| `limit` | 每頁事件數 1..8，省略時為 8；非法文字、0 或超過 8 回 400 |

初次使用 `GET /api/debug` 或 `GET /api/debug?limit=8`。後續請求帶上回應的 `boot_id`
與 `events.next_id`，例如 `/api/debug?boot_id=1234567&since=14&limit=8`；`since` 指已消費
的最後事件 ID。缺少配對參數或參數格式／範圍錯誤回 HTTP 400 JSON error，且不改變事件。

| 頂層欄位 | 內容 |
|---|---|
| `schema_version` | 除錯JSON格式版本，目前3，與LoRa版本不同；岸端scope為station_local，平均位置欄位為station_average |
| `firmware_version` / `build` / `protocol_version` | Station韌體版本、編譯日期時間字串、wire版本4 |
| `boot_id` / `clock_ms` | Station本次開機識別／millis時間；不可直接當UTC |
| `config` | 目前RF、包長、週期、綁定與GNSS設定，見下表 |
| `gps` | **Station本機**GNSS解析／epoch統計，不是Client測量值 |
| `client_diagnostic` | 從低頻DIAG取得的Client狀態，另帶接收年齡與fresh |
| `counters` | Station接收、拒收、序號間隔與估計來源更新統計；舊ACK欄位為null |
| `events` | 64筆Station環形紀錄的游標資訊與本頁事件，每次最多8筆 |
| `limitations` | 明列未量測GNSS內部延遲、GNSS與RF頻率獨立、Client診斷低頻、Servo角度非機械回授 |

`config`包含：`rf_frequency_mhz`、`bw_khz`、`sf`、`cr`（分母5，代表4/5）、
`data_bytes`、`ack_bytes`、`telemetry_bytes`、`diagnostic_bytes`、`gnss_diagnostic_bytes`（36）、
`gnss_diagnostic_pages`（1）、`send_interval_ms`（500，**請求的 GPS 間隔，非 DATA 定時器**）、
`send_mode`（`latest_valid_fix`）、`status_heartbeat_ms`（0）、`diagnostic_interval_ms`（60000）、
`rx_boosted_gain`（true）、
`ack_every_n`、`gnss_baud`、`bound_client_id`、`gnss_age_uncertainty_ms`，以及
`data_airtime_ms`／`ack_airtime_ms`／`telemetry_airtime_ms`／`diagnostic_airtime_ms`。
`ack_enabled=false`；舊 `ack_bytes`／`ack_every_n`／`ack_airtime_ms` 均為0（功能不存在）。
ID在JSON中為十進位整數，0表示未綁定；ToA以向上取整的ms提供。

| `gps`欄位 | 說明 |
|---|---|
| `scope` | 固定`station_local` |
| `requested_hz` / `observed_hz` / `rmc_hz` / `gga_hz` | 請求2 Hz／最近完整5秒視窗實測值，量測前null |
| `rate_state` | measuring、no_position_sentences、missing_rmc、missing_gga、rate_mismatch、observed_2hz |
| `age_basis` / `measurement_clock_synchronized` | `nmea_epoch_aligned_arrival`／false |
| `last_epoch_interval_ms` | 最新接受的不同UTC epoch間隔；0表示尚未觀察到間隔 |
| `source_age_ms` / `epoch_ms_of_day` | 當前回傳快照的age／UTC日內ms；無快照null |
| `fix` / `have_rmc` / `have_gga` | 本機fix新鮮度／該快照具有的句型 |
| `epochs` / `rmc` / `gga` | 接受的新epoch／RMC／GGA計數 |
| `checksum_errors` / `rejected_sentences` | checksum錯誤／拒收句數；前者包含於後者，不可相加 |
| `ignored_sentences` | checksum正確但不需收集的其他句型，例如GSV |
| `backwards_epochs` / `duplicate_epochs` | 倒退epoch／同類句型同epoch重複計數；正常RMC＋GGA配對不算重複 |
| `backlog_drops` | 本機因UART服務間隔過長而丟棄緩衝的次數 |

`/api/track` 與 `/api/debug` 的平均位置欄位為 `station_average`：`window_ms`（30000）、`samples`、
`raw_lat`／`raw_lon`、`mean_lat`／`mean_lon`、`rms_m`、`warning`、`warning_rms_m`（3）。
平均只收新鮮新 epoch，RMS >3 m 只警告不丟點；無樣本時平均與RMS為null。原始座標保留，
追蹤幾何使用平均，但仍以當前原始定位品質／年齡判斷是否可用，平均不延長有效期。
`servo.finishing_last_gps_target` 表示 GPS 失效後仍按速限轉往最後目標；不再外推，抵達後停止。
手動暫停、模式切換與 PWM 故障仍會撤銷移動。

`client_diagnostic`包含`received`、`rx_age_ms`、`fresh`及wire資料的
`epoch_interval_ms`、`backlog_drops`、`nmea_errors`、`tx_errors`、`skipped_slots`、`status_bits`。
未收到時資料欄位為null；接收未滿90秒才`fresh=true`，**fresh表示低頻診斷快照仍在顯示期限，
不是位置仍可追蹤**。`counter_encoding`為`uint16_saturating_since_client_boot`；不能從這個
沒有Client boot ID的封包聲稱已可靠識別Client重開機。

`gps.stream`提供本機資料流快照；`client_gnss`提供type 6收齊結果，包含`received`、
`fresh`、`rx_age_ms`、`pages_mask`與`snapshot`。尚未收齊時snapshot=null；缺頁不覆寫
先前snapshot，pages_mask只代表目前組裝進度，不能代表snapshot的新鮮度。
`fresh`是從page 0接收後未滿90秒的**顯示期限**，不是GPS定位有效期。

stream／snapshot共有：`state`、`source_age_ms`、`epoch_ms_of_day`、`last_byte_age_ms`、
`last_sentence_age_ms`、`last_advance_age_ms`、`raw_fix`、`recovering`、`satellites`、
`status_bits`、`epochs`、`time_resyncs`、`missing_or_invalid_time`、`backwards_epochs`、
`duplicate_epochs`、`rejected_sentences`、`checksum_errors`、`age16_saturation_ms`（65534）、
`counter_max`（65535）。未知age／UTC／衛星與無快照raw_fix為null。
`state`依序判斷no_uart、no_nmea（兩者2秒未更新）、recovering、no_epoch、stale_epoch
（新 epoch 超過2秒未推進）、no_fix、fresh_fix；定位品質分級只限制選用的速度外推。
本地 API／匯出使用 schema 3；歷史 schema 2 沒有的新欄位應視為未知，不能補零當成實測。

`counters`欄位：

- 接收：`rx_data`、`rx_telemetry`、`rx_diagnostic`、`rx_gnss_diagnostic`（頁數）、`radio_errors`。
- 拒收：`rejected_length`、`rejected_format`、`rejected_binding`、`rejected_sequence`。
- 序號：`sequence_gaps`、`sequence_resyncs`；重同步不把Client重啟跳號當成數萬包丟失。
- DATA資格：`invalid_fix_packets`、`invalid_velocity_packets`，不等於整包解析失敗。
- 復原：`radio_recoveries`；`config.ack_enabled=false`，舊 `ack_sent`／`ack_errors`／`ack_skipped` 為null。
- 時間：`last_data_interval_ms`、`max_data_interval_ms`、`inferred_source_updates`、`inferred_source_interval_ms`。

`inferred_source_*` 在 v5 固定 null，不再由 RF 到達時間推估 GPS 頻率；辨識 Client GNSS 真實 epoch 間隔
應參考低頻DIAG及實機NMEA，不能把RF發送上限當成GNSS更新頻率。累計計數主要跨綁定保留，
目前Client狀態／滾動窗口則在變更綁定時清除；離線分析同時記錄boot_id和bound_client_id。

### `events`封包紀錄

`events.capacity=64`，`total`為累計寫入數，`overwritten`為已覆寫數；計數與事件 ID 是
uint32，正常回繞時仍可繼續使用游標。這是有限環形緩衝，不是永久記錄。

| 分頁欄位 | 說明 |
|---|---|
| `items` | 本頁事件，依舊到新排列，數量不超過 `limit`；可能為空 |
| `next_id` | 本頁最後回傳的事件 ID；沒有新事件時保留已追上的游標，不直接跳過尚未傳出的項目 |
| `more` | 回應建立時尚有已保留事件待下一頁讀取；新到事件仍可能出現在後續請求 |
| `reset` | 本次重新建立讀取起點：首次、boot 不符、游標落後已覆寫資料或無效未來游標 |
| `dropped` | 同 boot 的游標已落後保留範圍，或為無效未來游標；需要從最舊保留事件重讀 |

未提供游標或 `boot_id` 不符時，從**現存最舊事件**分批回傳，`reset=true`、
`dropped=false`；首次看到 `overwritten>0` 不代表這次讀取遺失。相同 boot 的有效游標
只回傳 `since` 之後的事件，`reset=false`、`dropped=false`。相同 boot 但游標落後／無效
則 `reset=true`、`dropped=true`，從現存最舊事件重新開始。新 boot 的空緩衝回
`items:[]`、`next_id:0`、`more:false`；ID 回繞後的 0 也是有效游標，不能用真假值判斷
是否已初始化。用 `(boot_id, id)` 識別／去重事件，依 uint32 回繞規則判斷進度。

例如保留 ID 7..70、`limit=8`：首次回 7..14、`next_id=14`、`more=true`，後續 `since=14`
回 15..22。追到 70 後再次 `since=70`，未新增事件便回空陣列、`next_id=70`、`more=false`。

每個 item：

| 欄位 | 說明 |
|---|---|
| `id` / `ms` | 本次Station開機內的事件序號／millis時間 |
| `kind` | `data`、`telemetry`、`diagnostic`、`gnss_diagnostic`、`length`、`format`、`binding`、`sequence`、`radio_error`；`ack_error`／`ack_skipped` 僅為舊版歷史事件 |
| `client_id` / `seq` / `length` | 可解析時的設備／序號及原始包長；未取得的數值可能為0 |
| `source_age_ms` | v5 DATA 不含此欄，固定 null；GNSS 診斷推算值另列 |
| `rssi_dbm` / `snr_db` | RF接收量測；沒有原始RF資料的事件為null |
| `code` | RadioLib錯誤碼等事件碼，正常通常0 |
| `flags` | bit0為DATA fix、bit1為velocityValid；不是wire的course_and_flags原值 |
| `raw_hex` | 最多36 bytes原始RF資料的小寫hex；超長包只留前36 bytes，真實長度看length |

### 診斷入口

2026-09-22 移除網頁除錯分頁、瀏覽器錄製與 JSON 匯出；也移除相關背景輪詢。
上述 `/api/debug`、`/api/log` 的 cursor／schema 契約保留，可由電腦直接讀取。
現場完整 log 改存 SD 並透過 USB 匯出，見 [SD 記錄](sd-log.md)。

## `GET /api/axiom` / `POST /api/axiom`

Station 專用的可選 Axiom 上傳，預設關閉。設定和上傳狀態由 GET 讀取，`Cache-Control: no-store`。
雲端 log schema 2 新增英文 `message`／`level`、區間 `delta` 與獨立 `state_change` 文件。
`sent_samples`／`failed_samples` 為收件文件數（含快照與狀態事件）；`dropped_samples` 混合
略過的快照與未確認收件文件，不能直接視為快照丟失率。
網頁不提供 Dataset／區域選項，儲存時固定送出 `shore-spotter`／`us`；API 仍保留兩欄位。
清除 Token 時沿用已存目的地並停用上傳。
回覆只含 `token_set`，不回傳 Token。上傳資料 schema 與容量／失敗策略見
[axiom-log.md](axiom-log.md)。本功能不更改 LoRa、UART 或 Servo 控制設定。

POST 必須是 **`Content-Type: application/json`、有 Content-Length 的 JSON body**；
不接受 URL query parameters。body 上限 768 bytes：

```json
{"enabled":true,"dataset":"shore-spotter","region":"us","token":"YOUR_INGEST_API_TOKEN","clear_token":false}
```

- `enabled` 必填 boolean；`dataset` 必填，1–63 個英數字、`-`、`_`。
- `region` 必填 `us` 或 `eu`，對應 `us-east-1.aws.edge.axiom.co`／`eu-central-1.aws.edge.axiom.co`。
- `token` 選填，最多 255 個無空白 ASCII 字元；缺省或空字串保留舊 Token。
- `clear_token: true` 清除 Token，必須同時關閉，否則缺 Token 驗證失敗。
- 成功接收回 `202`，**不代表已寫入 NVS 或上傳成功**。輪詢 GET，依 `boot_id`／`save_id`
  確認 `saving=false` 且無 `storage_error`；設備重啟後需重新確認設定。
- 無效設定／格式 `400`；前一筆設定尚未完成 `409`；背景 task 無法啟動 `503`。
- GET 狀態：`enabled`、`dataset`、`region`、`token_set`、`state`、`saving`、`in_flight`、
  `boot_id`、`save_id`、`queue_depth`／`queue_capacity`、`sample_ms`／`flush_ms`、`sample_bytes`、
  `sent_samples`、`dropped_samples`、`failed_samples`、`sent_bytes`、`requests`、
  `capture_last_us`／`capture_max_us`、`request_ms`、`worker_stack_free_min`、
  `retry_in_ms`、`http_status`、`transport_error`、`last_success_age_ms`。
  計數自 Station 本次開機起累計；`sent_bytes` 僅計整批確認成功的 payload bytes。
- `state` 為 `disabled`、`saving`、`idle`、`waiting_wifi`、`waiting_time`、`low_memory`、
  `waiting_batch`、`uploading`、`backoff`、`rejected`、`storage_error`、`task_error`、`ota_paused`。
  關閉後 `in_flight=true` 表示先前已開始的請求仍在完成中。

## `GET /api/whitelist`

列出單一 GPS client 綁定；端點名稱沿用，容量固定為 1。

**Response**

```json
{
  "whitelist": [
    "AB12"
  ],
  "count": 1,
  "capacity": 1
}
```

## `POST /api/whitelist`

設定、移除或清空單一 client。變更會檢查 NVS 寫入結果並在重啟後保留；
空集合也是合法設定，不會在重啟後恢復預設 ID。實際變更後回到手動並清除舊client狀態；全新Station預設未綁定，已有NVS不被覆蓋。

**Query Params**

| 參數 | 值 | 說明 |
|---|---|---|
| `action` | `set` | 明確指定／替換唯一 ID |
| `action` | `add` | 未綁定時新增；已綁定相同 ID 為 no-op，不同 ID 回 409 |
| `action` | `remove` | 移除 ID |
| `action` | `clear` | 清空所有 |
| `id` | `AB12` | 1–4 碼十六進位，set/add/remove 必填；0、STATION_ID、FFFF 不可用 |

**範例**

```
POST /api/whitelist?action=set&id=AB12
POST /api/whitelist?action=remove&id=AB12
POST /api/whitelist?action=clear
```

**Response**（成功時回傳更新後的白名單）

```json
{
  "whitelist": [
    "AB12"
  ],
  "count": 1,
  "capacity": 1
}
```

綁定錯誤回應：400（參數非法）、409（add 第二個 ID）、503（NVS 寫入失敗）。
移除不同於綁定的有效 ID 不改變現有綁定。

## 共用運動狀態欄位

status／track 的 `servo` 都增加下列欄位：

| 欄位 | 含義 |
|---|---|
| moving | 最新目標尚未抵達；不可校正，仍可調整共用速限 |
| speed_limit_deg_s | 所有模式共用最高速度，預設 30、可調 1–90°/s |
| prediction_enabled / prediction_alpha | 0.6-dev：GPS位置預測 bool／0 或 1，預設 false／0；只影響 GPS 目標估計，另有 prediction_active 表示實際使用 |
| velocity_deg_s | 命令速度，非機械量測 |
| motion_fault | 規劃／輸出故障鎖定，需重新開機 |
| control_boot_id / control_epoch | 開機識別／目前控制世代 |
| command_seq / clock_ms | 已消耗的 HTTP 命令序號／ESP32 時間 |
| rejected_commands | HTTP 上下文缺漏、過期、舊世代或順序拒絕次數 |
| rejected_gps_sequence | DATA 重複／倒退序號或等待重同步候選次數 |

`timing.motion` 使用既有 last_ms／max_ms／over_50ms 格式，量測共用限速與輸出耗時。
`motion_fault` 也會以同名 ERROR alert 顯示（若 PWM 故障則沿用 servo_fault）。

## 控制請求的有效性（2026-09-09）

以下所有 `/api/servo`、`/api/servo/mode`、`/api/servo/settings`，以及
`/api/track/start`／`resume`／`pause`／`calibrate`／`prediction` 的 POST 都必須帶：
`epoch=<control_epoch>&seq=<下一個序號>&stamp=<clock_ms>`。
數值取自最近 GET status／track 的 servo 欄位，stamp 使用 ESP32 時間。
距 stamp 滿 2000 ms、未來時間、舊世代、重複／倒退序號或缺欄位回 409。
通過新鮮度檢查就消耗序號，後續參數驗證失敗也不能重用該序號。
新版網頁自動帶入，更新後須重新整理。下方簡寫的 API URL 均需補上這三個參數。
這是命令有效性檢查，不是登入認證；同熱點裝置仍可取得上下文。
完整設定、來源期限、序號重同步及 UART SET2 見 [共用控制器](motion-control.md)。

## `POST /api/servo/center`

資訊頁「回到 90°」。要求新鮮 epoch／seq／stamp，先檢查 PWM／motion fault；失敗回 503，
保留原模式與目標。成功一次切為 manual、更新控制世代並撤銷 GPS／UART／待完成 GPS 目標，
透過共用限速器要求 90°，不直接寫 PWM、不清除指南針校正或速度設定、不自動恢復追蹤。
回覆為共用控制 JSON：`ok`、`mode=manual`、`angle`、`target=90` 與新的控制上下文。
200 代表回中目標已接受，並不代表實體鏡頭已到位；以 `servo.calibration_ready` 判斷命令到位，
並目視確認停穩。過期／重播／舊世代請求回 409，不改變控制狀態。

## `POST /api/track/calibrate`

輸入鏡頭上普通磁針指南針的讀數（磁北），北 0°、東 90°、南 180°、西 270°。

```text
POST /api/track/calibrate?bearing=90
```

只接受有限十進位數字 `0 <= bearing < 360`，最多 16 個字元，不接受科學記號或地標 lat/lon。
僅在 Servo 命令位置、限速器位置及目標均精確為 90°，且沒有命令移動時接受。
非 90°／尚未抵達／已排入離開 90° 的目標皆回 409，保留原 RAM 參考；不以顯示四捨五入判斷。
每次成功覆蓋 RAM 參考，校正不需 GPS／磁偏角，也不切模式或解除 PWM 故障。

網頁按鈕保留可點，非 90° 或其他控制操作處理中會跳出通知；後端於請求送達再次檢查，
避免狀態過期造成誤校正。請求仍需 epoch／seq／stamp，失敗顯示原因，不自動重送。
角度是軟體命令，不是機械位置回饋，操作者仍需等鏡頭與磁針實際停穩。

```text
mount_offset = compass_bearing + servo_angle
```

只有固定腳架的鏡頭磁針參考，不使用板上磁力計。不讀寫 mount*／mag* NVS；
重開機恢復 90°有效偏移；腳架方向與該參考不同時重新校正。

GPS 追蹤使用 `servo_angle = mount_offset + declination - true_bearing`。
磁偏角東正西負，依攝影站 GPS 位置與 UTC 日期使用 WMM2025 離線表，適用北緯 18–28°、
東經 116–124°、2025–2029 年、海平面。日期未知／超過 60 秒未更新、位置失效或超出模型
範圍時，`declination_deg=null`，GPS 不更新目標、最多轉完最後有效目標；UART 不受影響。磁偏角是否可用不影響校正資格。
雷達使用同一磁北轉真北換算，無有效補償時不畫鏡頭方位線。

```json
{
  "ok": true,
  "method": "compass",
  "bearing": 90.0,
  "mount_offset_deg": 180.0,
  "servo_angle": 90.0
}
```

錯誤：400（缺少 bearing、非法數字、超出範圍），409（非 90°／仍有命令移動，或命令上下文缺漏、過期、序號／世代失效）。
校正只更新 RAM 參考，不切模式或直接寫 PWM；不解除 PWM 故障。GPS 追蹤會在後續目標
重算使用新參考，因此可能調整轉角。UART／手動目標不變；start/resume 不覆寫校正。

## `/api/log`

攝影站的滾動執行紀錄。韌體把所有 log 同時寫到 USB 序列埠和一個 **4 KB 環形緩衝**，
0.5移除獨立`/log`網頁；0.6-dev 曾加入除錯分頁，2026-09-22 再移除；此文字 API 保留。

> 收包**不是**一包一行：那樣 4 KB 會在約 16 秒內被洗完。RX 改成累積後每 60 秒一行統計
> （`[STATION] RX 60s`，含DATA序號缺口、RSSI/SNR的min/avg/max、雙方
> GPS、距離方位、servo 與模式），所以同一個緩衝大約能留 30 分鐘以上。

```
GET  /api/log?from=<絕對位移>   取得該位移之後的新內容
POST /api/log                   清除緩衝
```

回應是 **plain text**，帶 `Cache-Control: no-store`；增量讀取資訊放在三個自訂header：

| Header | 說明 |
|---|---|
| `X-Log-Boot` | 目前Station boot ID；變更時應重設前端cursor，不能只靠位移判斷重啟 |
| `X-Log-Next` | 本次回傳結束時的絕對位移，下次帶進 `from` 即可只取增量 |
| `X-Log-Dropped` | `1` = 你要的位移已滾出 4 KB 視窗（或裝置重開、位移倒退），已自動跳到目前最舊的位置 |

位移是單調遞增的總位元組數，所以前端輪詢只會拿到新內容。裝置重開後 `logTotal` 歸零、
位移倒退，此時一樣回 `X-Log-Dropped: 1` 並從頭給起。

## `POST /api/servo`

`?angle=0..180` 手動設定目標角度；必須已處於 manual，否則回 409，不能用延遲角度請求
切回手動。目標需在固定 0..180° 範圍內。所有模式在下次控制服務採用最新目標，以微秒實際經過時間推進、不設固定更新頻率，只套用共用最高速度限制。HTTP 立即回覆，不等待抵達；新的請求只取代目標，不排隊。
`angle` 為回覆當下的命令角度，`target` 為接受的目標（以毫度精度保存）。
status／track 的 `servo.angle` 可用來觀察命令角度進度，但不是實測機械位置。
空字串、NaN/Inf、超出範圍及混雜字元回 400，PWM 不可用回 503。

```json
{
  "ok": true,
  "angle": 90.0,
  "target": 87.0,
  "mode": "manual",
  "control_boot_id": 12345678,
  "control_epoch": 87654321,
  "command_seq": 0,
  "clock_ms": 123456
}
```

## `GET /api/servo/settings` / `POST /api/servo/settings`

GET 回傳唯一速度設定、允許範圍與控制上下文：

```json
{
  "ok": true,
  "speed": 30,
  "min_speed": 1,
  "max_speed": 90,
  "default_speed": 30,
  "control_boot_id": 12345678,
  "control_epoch": 87654321,
  "command_seq": 0,
  "clock_ms": 123456
}
```

POST 使用 `?speed=1..90`，修改即自動保存並生效；任何模式、移動中都可使用，不切換
模式、不清除目標。不再接受 action、啟用開關、加速度、jerk、deadband 或其他多餘參數。
輸入非法回 400；上下文過期回 409；NVS 保存失敗回 503，保留原有 RAM 速限。
保存使用單一 uint32 `shorespotter/servospd`（毫度／秒），回讀確認後套用，相同已保存值
不重寫。重開機讀回；首次／無有效設定為 30，忽略舊 motioncfg 多參數設定。
詳見 [保存規則](motion-control.md#設定套用與保存)。

0.5 已移除 `/api/servo/diagnostics`、`timing.control_elapsed` 與 OLED 暫停／時序調試介面。
UI 角度仍使用 `180 - raw`，HTTP／UART 使用內部角度。升級後需重新整理網頁。

## `GET /api/track/prediction` / `POST /api/track/prediction`（0.6-dev）

GET 回傳 GPS 預測設定與控制上下文，並標示 `Cache-Control: no-store`：

```json
{
  "ok": true,
  "enabled": true,
  "alpha": 1,
  "default_enabled": false,
  "control_boot_id": 12345678,
  "control_epoch": 87654321,
  "command_seq": 0,
  "clock_ms": 123456
}
```

POST 僅接受 `enabled=0` 或 `enabled=1` 加 epoch／seq／stamp，成功回相同結構。
`/api/track` 與 `/api/status` 的 servo 物件同步增加 `prediction_enabled`／`prediction_alpha`。
開啟沿既有速度向量外推，關閉直接使用最後收到座標；只有 GPS 來源套用，其他模式可預先保存。
這不是馬達限速或 PID 比例。切換保留模式、校正與共用速限，下次 GPS 50 ms 排程生效，
不直接寫 PWM，也不改定位資格門檻。

NVS `shorespotter/gpspred5` 保存 uint32 0／1；缺值、非法值預設關閉，舊 gpspredict 鍵不沿用，回讀確認成功才套用。
重複相同已保存值不重寫。參數非法／多餘回 400，上下文不符回 409，保存失敗回 503 並保留 RAM 值。
v5 外推僅用接收後經過時間，且兩端品質／同 epoch 速度須合格；GNSS 內部延遲未量測。預設 α=0；`prediction_active` 區分實際外推與保存的開關設定。

## `POST /api/servo/mode`

`?mode=manual|gps|uart`，三者互斥，開機預設手動；舊 `mode=auto` 回 400。
選 GPS 要求 `gps_available=true`，Station／Client 任一 Bad、Miss、過期或品質缺漏時
回 409，保留原模式與目標。條件在伺服器執行當下檢查，不能靠舊按鈕狀態繞過。
相容舊呼叫端的 `mode=jetson` 輸入，視為 `uart`；此 API、status／track 與 resume
的模式回應一律使用 `uart`。UART 舊 `SET <毫度>` 保留，另支援 SYNC／SET2。

- manual：撤銷所有自動控制，維持最後 PWM 角度，可使用 slider。
- gps：只用 GPS；指南針與磁偏角有效、兩端 fix／品質未滿 2 秒、衛星 ≥6、HDOP ≤3，
  有效新定位恢復即於下個控制週期追蹤。定位無效／逾時停止更新／外推目標，轉完最後有效目標後保持，不切 UART。
- uart：只用 UART SET，不要求 GPS／校正；250 ms 無有效 SET 保持，新指令可直接恢復。

切換清除舊目標／UART session；只有 UART 模式開 UART。重複選同模式不重置。
每次開機 Servo 90°、完成初始化後 UART；PWM 不可用回 503。模式切換保留 RAM 指南針參考。

```json
{
  "ok": true,
  "mode": "uart"
}
```

## `POST /api/track/start` / `POST /api/track/resume`

start 選 GPS；resume 恢復上次選取的 GPS／UART（尚未選過自動來源時，resume 候選為 UART；開機模式為手動）。選 GPS 的所有路徑
都會再次檢查兩端定位有效且資料持續更新，否則回 409 並保留原模式。沿用 RAM 校正，
UART session 重新進入後需新的完整 SET。PWM 不可用回 503；成功回實際 mode。

## `POST /api/track/pause`

撤銷 GPS 與 UART，維持最後輸出角度，回 `{"ok":true,"mode":"paused"}`。
手動調整需先明確選 manual；resume 保留校正。

## 已移除入口

`/api/track/stop` 與 `/api/mag/calibrate` 均回 404 JSON，沒有控制／校正效果。
停止全部追蹤請選 manual。UART 不再接受 ARM／STOP，接受 SET／SET2（另有 SYNC）。


---

# 3. 欄位對應表（封包 → API）

| wire／來源欄位 | 封包 | API欄位 | 轉換／限制 |
|---|---|---|---|
| latE7／lonE7 | DATA | `client.lat`／`client.lon` | signed32 ÷10⁷；必須同看fix |
| fix | DATA | `client.fix` | 再套用source＋airtime＋RX age＋200 ms門檻，不直接永久照搬bit |
| speedDmS | DATA | `client.speed_cms` | ×10 cm/s；255→null，解析度10 cm/s |
| courseDeg10 | DATA | `client.course_deg10` | 保留0.1°單位；4095→null |
| velocityValid | DATA | `client.velocity_valid` | 再要求Client fix仍新鮮；決定是否可外推 |
| satelliteClass | DATA | `client.satellite_class` | 0未知、1為0–5、2為6–7、3為≥8；即時gate使用此分級 |
| hdop10 | DATA | `client.hdop` | ÷10；255→null，發送端向上量化 |
| 無 | v5 DATA | `client.source_age_ms` | 固定 null，來源延遲未量測 |
| 無 | v5 DATA | `client.sample_age_ms` | 固定 null，不再合成推算定位年齡 |
| DATA RxDone時間 | Station | `client.rx_age_ms`／`last_rx_sec` | 距接收的ms／秒，不等於來源測量年齡 |
| batteryMv | TEL | `telemetry.batt_mv` | 直接mV；未知0 |
| tempC／humidityPct | TEL | `telemetry.temp_c`／`humidity_pct` | 整數°C／%；未知→null |
| satellites | TEL | `client.satellites` | 精確低頻顆數；未知或TEL滿90秒→null，不作即時gate |
| TEL RxDone時間 | Station | `client.satellites_age_ms`／`telemetry.last_rx_sec` | TEL接收年齡，不代表衛星數精確測量時間 |
| epochIntervalMs等 | DIAG | `/api/debug.client_diagnostic` | Client低頻解析／排程快照，帶獨立接收年齡 |
| RSSI／SNR | Station收到DATA時的radio量測 | `/api/status.lora.rssi`／`snr` | dBm／dB，不是Station從ACK反解 |
| ACK | 已移除 | `ack_enabled=false` | 不再傳回RSSI／SNR；Station仍保留上行接收品質 |
| uint16 DATA seq | DATA | `/api/debug`序號與event欄位 | 只由DATA使用，TEL／DIAG不占它的序號 |

加速度已從封包和API移除，不回假0。`station.*`來自本機GNSS／感測器；其中既有衛星／HDOP
顯示欄位仍由TinyGPS提供，真正gate使用同epoch collector。`servo.*`是命令／追蹤狀態，
沒有實際機械角度回授。軌跡由瀏覽器累積，API不保存整段軌跡。

`bearing`由攝影站位置與收到的Client位置計算；α=1的`servo.target`使用速度向量外推後
的位置，因此兩者可能不同。α=0不外推，兩者仍經鏡頭校正、磁偏角與0–180°限制換算。
