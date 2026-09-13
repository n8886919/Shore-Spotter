# 介面規格（Interface）

本文件彙整 Shore Spotter 的 wire／HTTP 契約及欄位對應：

1. **下層 — LoRa 封包**（Surfer ↔ 攝影站，RF 二進位格式）
2. **上層 — 攝影站 HTTP API**（攝影站 → 手機監控頁，JSON）
3. **欄位對應表**（封包欄位如何流向 API）

---

# 1. LoRa 封包

本節對應 **0.6-dev／LoRa 協定 v4** 的本機實作；板上部署、GNSS 實際更新率及戶外
RF／機構行為仍須實測。v4 與舊 v3 不相容，Client、Server 都須更新；不存在自動降版。
韌體版本與 wire 協定版本分別由 `firmware_version.h`、`protocol.h` 定義。

所有多 byte 整數明訂 **little-endian**，有號數採二補數。C++ 結構只是邏輯欄位，
**不能直接 memcpy 結構當封包**；唯一 wire 定義是 [protocol.h](../include/protocol.h) 的 codec。
接收端先取得實際 RF 包長，再依 version/type 精確比對長度；短包、長包、未知類型／版本、
非法值或矛盾旗標拒收。保留 LoRa PHY CRC，沒有應用 MAC、加密或認證。

## RF 參數與頻率

| 參數 | 值 |
|---|---|
| 中心頻率 | 923.2 MHz，程式固定設定 |
| 頻寬 | 125 kHz |
| Spreading Factor | SF9 |
| Coding Rate | 4/5 |
| Sync Word | `0x12` |
| 前導碼／標頭 | 8 symbols／explicit header，PHY CRC 開啟 |
| 初始發射功率 | 17 dBm；Client ATPC 可調整 10–22 dBm |
| DATA 排程 | 每 500 ms 一個位置發送 slot，RF 目標 2 Hz |
| ACK 排程 | DATA 專用 `seq % 8 == 0`，正常約每 4 秒一次 |
| TELEMETRY／DIAGNOSTIC | 各約每 30 秒到期，等待非 ACK 週期的空檔 |

同組兩端頻率、BW、SF、Sync Word 必須一致。explicit header 帶有發送端的 payload
Coding Rate；本專案兩端發送都使用 4/5。頻率設定不是整套設備的法規／審驗認定。
目前未實作頻道掃描或協商切頻。Client ID 或 Sync Word 的資料過濾不會消除同頻 RF 碰撞。

## 封包類型與空中時間

| type | 名稱 | 大小（含共用標頭） | 空中時間 | 用途 |
|---|---|---:|---:|---|
| 1 | MSG_DATA | 17 B | 164.864 ms | 完整位置、速度向量、定位品質與來源 age |
| 2 | MSG_ACK | 11 B | 144.384 ms | ACK 序號、Server 收到 DATA 的 RSSI／SNR |
| 4 | MSG_TELEMETRY | 11 B | 144.384 ms | 電量、溫濕度與精確衛星數 |
| 5 | MSG_DIAGNOSTIC | 17 B | 164.864 ms | Client 的 GNSS 間隔與執行計數 |

type 3 未實作，也不再把舊 HELLO 當可接受格式。以 RF 2 Hz、每 8 DATA 回 ACK、TEL／DIAG
各 30 秒一次估算，平均空中時間需求約 **37.61%**，含兩個低頻包；不是實測接收率或多套容量保證。

### 不阻塞的發送與時槽

Client 的 DATA／TEL／DIAG 與 Server ACK 都使用 `startTransmit → TxDone／timeout → startReceive`。
軟體 timeout 為各包向上取整的 ToA＋80 ms。Server 若在 DATA RxDone 後超過 50 ms 才能
處理，略過 ACK，不建立待送佇列。Client 僅接受目前期待的 DATA seq、ID 及 ACK 時窗；
重複／逾期 ACK 不更新 alive／ATPC，等待 ACK 時不改發射功率。

DATA、TEL、DIAG 各有獨立序號，TEL 不會消耗 DATA 的 ACK 機會。DATA 以 uint16 遞增並 wrap；
排程落後時跳過錯過的 slot，不補發積欠工作。即使 claim 到 slot，仍須確認下一個 DATA
期限前容得下 `DATA＋必要 ACK＋80 ms`，不夠便跳過，且不先消耗 DATA seq。

TEL／DIAG 只可在已成功送出 DATA、非 ACK 週期、radio 閒置且 RX 已恢復時開始：

```text
起送下界 = 本次 DATA 起送時間＋ceil(DATA ToA)＋80 ms
起送上界 = 下一 DATA 期限−ceil(額外封包 ToA)−80 ms
```

準時的 500 ms 週期內，TEL 起送窗口為 DATA 起送後 **245–275 ms**，DIAG 為 **245–255 ms**。
兩者同時到期時 TEL 優先，DIAG 等另一個可用週期；不會放寬 guard 或跨下個期限硬送。
因此「30 秒」是到期後排送間隔，忙碌／失敗時可能延後。RX／TX 錯誤有計數、重試與 radio recovery。

## 共用標頭（6 bytes）

| offset | bytes | wire 欄位 | 規則 |
|---|---:|---|---|
| 0 | 1 | magic | `0x53` |
| 1 | 1 | version/type | 高 4 bits 為版本 4，低 4 bits 為 type |
| 2–3 | 2 | client_id | 上行表示來源；下行表示接收對象 |
| 4–5 | 2 | seq | DATA 專用遞增序號；TEL／DIAG 各自遞增；ACK 標頭 seq 使用被回覆的 DATA seq |

不再傳 networkId、獨立 srcId／dstId、payloadLen、空白 MAC。長度由 type 決定。
wire ID 0／FFFF 不可用；白名單還保留舊 Server ID `0010` 的禁用規則。

## DATA（17 bytes＝標頭 6＋定位資料 11）

| offset | bytes | wire 欄位 | 編碼／未知值 |
|---|---:|---|---|
| 6–8 | 3 | lat_delta_e6 | signed24，`round(lat×10⁶)−24000000` |
| 9–11 | 3 | lon_delta_e6 | signed24，`round(lon×10⁶)−121000000` |
| 12 | 1 | speedDmS | 0–254 為 0–25.4 m/s，0.1 m/s 單位；255 未知／超界 |
| 13–14 | 2 | course_and_flags | 低 12 bits 方向、接續衛星分級與兩個旗標，見下表 |
| 15 | 1 | hdop10 | HDOP×10 **向上取整**，0–254；255 未知／超界 |
| 16 | 1 | age10ms | 來源 age／10 **向上取整**，0–254；255 未知／超界，不可追蹤 |

| course_and_flags bits | 內容 |
|---|---|
| 0–11 | courseDeg10：0–3599＝0–359.9°；4095 未知；3600–4094 非法 |
| 12–13 | satelliteClass：0 未知、1＝0–5 顆、2＝6–7 顆、3＝≥8 顆 |
| 14 | fix：發送端認定位置有效且新鮮 |
| 15 | velocityValid：可外推的速度向量；必須 fix 有效、速度已知且 ≥0.3 m/s、方向已知 |

固定原點為 **24°N、121°E**，不是每次架設點，不需交換原點。範圍：北緯
15.611392–32.388607°、東經112.611392–129.388607°；每格南北約0.111 m、台灣緯度
東西約0.10 m。這是編碼解析度，不是 GPS 實際精度或 LoRa 通訊距離，也不擴大磁偏角表範圍。
超界不得截取低24 bits回繞；位置不可用時 fix=0。發送端無可用座標時寫零差值，**fix=0 的
零差值不能解讀成真的位於原點**。速度精度較 v3 的 cm/s 降為0.1 m/s；加速度已移除。

### 同 epoch 快照與新鮮度

GNSS UART 仍為9600 baud，未送出強制2 Hz命令。RF每500 ms可以重送同一個定位epoch；
不能由包數推論每秒有兩筆新定位。L76K高速NMEA的限制與實測前置條件見
[原廠協定，第23頁](https://files.waveshare.com/upload/d/dd/Quectel_L76K_GNSS_Protocol_Specification_V1.1.pdf)。

[gnss_snapshot.h](../include/gnss_snapshot.h) 驗證NMEA checksum，以相同UTC epoch配對RMC／GGA：
新GGA可單獨提供位置／品質，但無速度；最新只有有效RMC且GGA尚未到時，暫用上一個有GGA的
一致快照及其原age，不把兩個epoch欄位混合。最新無效RMC／GGA不會被舊好資料覆蓋。
重複epoch不刷新age；過期快照由新鮮度門檻停用。UART服務間隔>200 ms即丟棄積壓bytes及
半句，等待新的epoch；支援跨午夜，倒退UTC需明確重設collector基線。

`age_basis="nmea_epoch_aligned_arrival"`：source age依最早句子到達、UART序列化與UTC間隔
估計；**GNSS內部定位／輸出延遲未量測，測量時鐘未同步**。另有200 ms本地緩衝不確定量，
只用在停用門檻，不增加推估移動距離：

```text
Client：source_age＋200 ms < 2000 ms 才標示 fresh
Server：sample_age = wire source_age＋ceil(DATA ToA)＋收到後經過時間
GPS gate：sample_age＋200 ms < 2000 ms
α=1 外推使用 sample_age；α=0 使用收到的位置
```

衛星／HDOP取自同一快照，沒有新鮮GGA品質不能追蹤。Good仍為≥8顆且HDOP≤1.5；
OK為≥6顆且HDOP≤3，兩者均要求有效位置與新鮮度。兩端都合格、已校正且磁偏角有效，
再連續穩定2秒才追蹤。DATA序號在連線時拒絕重複／倒退；間斷≥2500 ms後需兩個前進候選，
間隔至少250 ms且小於2000 ms才重同步；沒有Client boot/session ID。

## ACK（11 bytes＝標頭6＋資料5）

| offset | bytes | 欄位 | 編碼 |
|---|---:|---|---|
| 6–7 | 2 | ackSeq | 確認的DATA seq |
| 8–9 | 2 | rssiDbm10 | int16，RSSI×10，dBm |
| 10 | 1 | snrQuarterDb | int8，0.25 dB單位，−32～31.75 dB |

RSSI／SNR是Server收到上行DATA的量測，回Client供ATPC；SNR已由v3的×10改成原生四分之一dB。

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
| 14–15 | 2 | skippedSlots | Client跳過的DATA slot次數 |
| 16 | 1 | status | bits0..5依序為haveEpoch、fix、velocityValid、haveGga、haveRmc、ageUncertaintySet；bits6..7須0 |

uint16欄位發送時飽和至65535，計數自Client開機累計；不會wrap成小值。此包沒有Client boot ID，
不能僅靠計數下降判定重啟。status是低頻快照，不能取代DATA的即時gate；也不增加DATA包長。

## 單一 Client 綁定

Server只接受綁定的client_id。NVS `gpsclient`保留既有綁定；若尚無該鍵，遷移舊`wl`第一個
有效ID，舊空集合保持未綁定。**全新Server預設未綁定（0）**，需從網頁指定自己的Client。
ID取自ESP32 MAC末16 bits，並非全域唯一；多套仍須確認沒有重號、沒有多台Server誤綁同一Client。

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
三個分頁：**雷達**（雷達／地圖與手動／GPS／UART）、**資訊**（共用最高速度、α、遙測、校正、軌跡匯出）及**除錯**（唯讀狀態、記錄與JSON匯出）。
OpenStreetMap 底圖由瀏覽器連網載入；頁面程式與雷達不依賴外部 CDN，底圖則需要網路。

### 全頁共用 HTTP 請求排程

內嵌頁面所有 API 請求共用一個排程；同一頁同時只執行一個請求，直到回應內容讀取完成
才開放下一個。等待中的優先順序為控制 POST、track／設定讀取、status、除錯／文字 log；
不會中斷已開始的請求。相同背景讀取以 key 合併，避免輪詢累積成重複佇列。

背景讀取的排隊期限與開始傳輸後的 timeout 各為 2 秒。控制命令從加入佇列起保留原始
2 秒期限，必要時先更新控制上下文；過期或控制世代已變更的等待命令不會延後重播。
timeout 會嘗試取消傳輸；若瀏覽器未能結束 fetch／內容讀取，排程仍保留該位置，避免
再開第二個重疊請求。這是單一網頁的行為，不限制其他瀏覽器、分頁或外部 HTTP client，
也不包含外部地圖圖磚載入；Server 的同步 WebServer 仍可能受網路等待影響。

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
    "source_age_ms": 80,
    "sample_age_ms": 345,
    "age_basis": "nmea_epoch_aligned_arrival",
    "last_rx_sec": 0
  },
  "server": {
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
| `linked` | Client是否在線（5秒內有收到DATA）；不代表GPS仍新鮮 |
| `bearing` | 攝影站 → Surfer 方位角（度），無效時為 `-1` |
| `client.fix` | 已通過來源age＋airtime＋接收年齡＋200 ms不確定量門檻的fix，0表示目前不可用 |
| `client.speed_cms` | 速度換算為cm/s，10 cm/s一格；未知為`null`，是否可外推另看velocity_valid |
| `client.course_deg10` | 行進方向0.1°；未知為`null`，不可只靠非null判斷仍新鮮 |
| `client.velocity_valid` | 位置仍新鮮且速度向量可外推；false不必然代表位置不可用 |
| `client.satellite_class` | DATA即時衛星分級：0未知、1為0–5、2為6–7、3為≥8，不是假造精確顆數 |
| `client.satellites` / `satellites_age_ms` | 低頻TEL精確顆數／距收到TEL的ms；無資料或顆數未知為null，TEL滿90秒也不再呈現顆數 |
| `client.rx_age_ms` / `source_age_ms` / `sample_age_ms` | 接收年齡／封包攜帶的來源age／兩者加DATA airtime；未知null，sample_age不含200 ms不確定量 |
| `client.age_basis` | 固定`nmea_epoch_aligned_arrival`，是估計時間基準，非GNSS測量時鐘同步 |
| `client.last_rx_sec` | 距上次收到DATA的秒數；從未收到為`-1`，失聯後仍累計 |
| `telemetry.batt_mv` | Surfer端電池電壓mV（約每30秒排送，忙碌時延後）|
| `telemetry.temp_c` | Surfer 端溫度，`null` = 無感測器 |
| `telemetry.humidity_pct` | Surfer 端濕度 %，`null` = 無感測器 |
| `server.fix` | 攝影站本身的 GPS fix 狀態 |
| `server.temp_c` | 攝影站本機溫度，`null` = 無感測器 |
| `server.humidity_pct` | 攝影站本機濕度，`null` = 無感測器 |
| `servo.angle` | Servo 目前命令角度（0–180°）；GPS／UART／手動共用不限頻微秒軌跡，只套用共用速限、預設 30°/s，移動期間落後 `target`，不是機械位置回饋 |
| `servo.target` | 追蹤目標角度（0–180°）；GPS 的 α 開啟時以外推位置計算，關閉時以最後收到位置計算 |
| `servo.mode` | `manual` / `gps` / `uart` / `paused` |
| `servo.source` | 實際控制來源：`manual` / `gps` / `uart` / `hold` |
| `servo.gps_available` | 兩端 GPS 均為新鮮 Good／OK，可選擇 GPS 模式；Bad／Miss／過期為 false |
| `servo.gps_usable` / `servo.gps_ready` | 另滿足校正與磁偏角／已通過 2 秒穩定期 |
| `servo.uart_state` / `servo.uart_ready` | inactive / waiting / tracking / watchdog_hold；是否有新鮮 SET |
| `servo.north_reference` | 指南針輸入基準，固定 `magnetic` |
| `servo.declination_deg` | 磁北轉真北需加的角度（東正西負），`null` = 位置／日期或模型不可用 |
| `servo.calibrated` | 是否已完成鏡頭指南針校正|
| `servo.mount_offset_deg` | Servo→磁北座標的安裝偏移角，校正後鎖定於 RAM |
| `server.satellites` | Server本機TinyGPS顯示資訊，`-1`未知；真正追蹤gate另用同epoch collector |
| `client.hdop` / `server.hdop` | Client未知為`null`，Server顯示資訊未知為`-1`；Client為向上量化的即時DATA值 |
| `server.batt_pct` | 攝影站 18650 電量 %（3.2V=0%、4.15V=100%），`-1` = 無電池/未知 |
| `server.charging` | 攝影站是否接外部電源（USB/Type-C，VBUS 在），用於 ⚡ 指示 |

> API 不回傳軌跡。前端每秒嘗試加入有效且不同的 `client`/`server` 位置，加入新點時
> 清掉兩小時前的資料供 GPX 匯出；雷達／地圖只畫最近 5 分鐘。沒有固定 300 點的
> Server 緩衝，重整頁面會重新累積。

> GPS 品質判定（四級，韌體與監控頁共用同一組門檻，見 `geo::gpsSignal()` 與 web_ui.h 的 `gpsGrade()`）：
> **Good** = 有定位 且 HDOP ≤ 1.5 且 sats ≥ 8；**OK** = 有定位 且 HDOP ≤ 3.0 且 sats ≥ 6；
> **Bad** = 收得到衛星但定位不堪用（無定位／sats < 4／達不到 OK）；**Miss** = 完全沒訊號。
> LoRa 的 **Miss** 表示未收到封包。

> Client即時品質由DATA的衛星分級與HDOP判斷；精確衛星數另由低頻TEL提供，不能當成每500 ms更新的數值。
> Web UI的預期精度是HDOP換算的提示，不是實測誤差界限；GPIO輸出與Servo角度也不是機械回授。

## `GET /api/status`

GPS 訊號品質、LoRa 訊號統計、Servo 校正狀態。

**Response**

```json
{
  "server_gps": {
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
    "ack_tx": 26,
    "ack_busy": false,
    "ack_errors": 0,
    "ack_skipped": 0,
    "ack_last_error": 0
  },
  "env": {
    "temp_c": 30,
    "humidity_pct": 76
  },
  "health": {
    "firmware_version": "0.6-dev",
    "protocol_version": 4,
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
| `mount_uncal` | warn | GPS 模式且未校正 | GPS 保持，需切手動校正 |
| `uart_wait` | warn | UART 模式且無有效 SET | 維持最後輸出角度 |

**濕度為什麼要看基準而不是絕對值**：海邊空氣本來就 80% 起跳，封盒時關進潮濕空氣是常態，
只用絕對門檻會整天誤報。進水真正的特徵是「相對開機值單調上升」，所以兩條規則並用：
絕對值破 90% 直接判定，其餘看相對開機第一筆讀數的升幅。

| 欄位 | 說明 |
|---|---|
| `server_gps.satellites` | 定位使用中的衛星數，`-1` = 無效 |
| `server_gps.hdop` | 水平精度因子，數值越小越好，`-1` = 無效 |
| `lora.rssi` | 最近一筆位置封包 RSSI（dBm）|
| `lora.snr` | 最近一筆位置封包 SNR（dB）|
| `lora.rssi_avg` | 近 20 筆 RSSI 滾動平均，`null` = 無資料 |
| `lora.snr_avg` | 近 20 筆 SNR 滾動平均，`null` = 無資料 |
| `lora.pkt_rate` | DATA的60秒統計；首次完成前0，距最後DATA滿5秒回0；不是GNSS epoch更新率 |
| `lora.rx_data` | 成功解析的 DATA 封包累計 |
| `lora.rx_telemetry` / `lora.rx_diagnostic` | 成功接受的TEL／DIAG累計 |
| `lora.rx_drop` | 驗證失敗或白名單不符封包累計 |
| `lora.drop_rate` | 拒收比例 `rx_drop/(rx_data+rx_telemetry+rx_diagnostic+rx_drop)`，不包含完全沒收到的封包 |
| `lora.ack_tx` | 收到 TxDone 且 finishTransmit 成功的 ACK 累計 |
| `lora.ack_busy` | ACK 正在空中傳送 |
| `lora.ack_errors` / `lora.ack_last_error` | ACK 傳送失敗／逾時累計，以及最後 RadioLib 錯誤碼 |
| `lora.ack_skipped` | RxDone 後超過 50 ms 才能處理而略過的 ACK 數 |
| `env.temp_c` | 攝影站本機溫度 |
| `env.humidity_pct` | 攝影站本機濕度 |
| `health.firmware_version` | Server 正在執行的專案版本字串（目前 `0.6-dev`），不是 Client 版本或 LoRa 協定版本 |
| `health.protocol_version` | Server使用的LoRa wire版本，目前4 |
| `health.uptime_s` | 開機秒數 |
| `health.heap_free` | 目前可用 heap |
| `health.heap_min` | 開機後最小可用 heap |
| `health.reset_reason` | ESP 重啟原因代碼 |
| `health.rx_error` | LoRa 接收錯誤碼累計 |
| `servo.angle` | Servo 目前輸出的命令角度（0–180°），不是機械回授量測 |
| `servo.target` | 追蹤時的目標角度 |
| `servo.mode` | `manual` / `gps` / `uart` / `paused` |
| `servo.source` | 實際控制來源：`manual` / `gps` / `uart` / `hold` |
| `servo.gps_available` | 兩端 GPS 均為新鮮 Good／OK，可選擇 GPS 模式；Bad／Miss／過期為 false |
| `servo.gps_usable` / `servo.gps_ready` | 另滿足校正與磁偏角／已通過 2 秒穩定期 |
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

此欄位由 `/api/status` 提供，除錯頁取用最近的 status 快照；`/api/debug` 不重複附帶。
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
上下文，也不會啟用追蹤、修改設定、清除事件或文字紀錄。回應 `schema_version: 2`；
事件改為每次最多 8 筆的增量分頁，升級 Server 後需重整網頁。

| 查詢參數 | 規則 |
|---|---|
| `boot_id` / `since` | 兩者必須同時提供或同時省略；十進位 uint32（0..4294967295），不接受空值、符號、空白、小數或混雜字元 |
| `limit` | 每頁事件數 1..8，省略時為 8；非法文字、0 或超過 8 回 400 |

初次使用 `GET /api/debug` 或 `GET /api/debug?limit=8`。後續請求帶上回應的 `boot_id`
與 `events.next_id`，例如 `/api/debug?boot_id=1234567&since=14&limit=8`；`since` 指已消費
的最後事件 ID。缺少配對參數或參數格式／範圍錯誤回 HTTP 400 JSON error，且不改變事件。

| 頂層欄位 | 內容 |
|---|---|
| `schema_version` | 除錯JSON格式版本，目前2，與LoRa版本不同 |
| `firmware_version` / `build` / `protocol_version` | Server韌體版本、編譯日期時間字串、wire版本4 |
| `boot_id` / `clock_ms` | Server本次開機識別／millis時間；不可直接當UTC |
| `config` | 目前RF、包長、週期、綁定與GNSS設定，見下表 |
| `gps` | **Server本機**GNSS解析／epoch統計，不是Client測量值 |
| `client_diagnostic` | 從低頻DIAG取得的Client狀態，另帶接收年齡與fresh |
| `counters` | Server接收、拒收、ACK、序號間隔與估計來源更新統計 |
| `events` | 64筆Server環形紀錄的游標資訊與本頁事件，每次最多8筆 |
| `limitations` | 明列未量測GNSS內部延遲、GNSS與RF頻率獨立、Client診斷低頻、Servo角度非機械回授 |

`config`包含：`rf_frequency_mhz`、`bw_khz`、`sf`、`cr`（分母5，代表4/5）、
`data_bytes`、`ack_bytes`、`telemetry_bytes`、`diagnostic_bytes`、`send_interval_ms`、
`ack_every_n`、`gnss_baud`、`bound_client_id`、`gnss_age_uncertainty_ms`，以及
`data_airtime_ms`／`ack_airtime_ms`／`telemetry_airtime_ms`／`diagnostic_airtime_ms`。
ID在JSON中為十進位整數，0表示未綁定；ToA以向上取整的ms提供。

| `gps`欄位 | 說明 |
|---|---|
| `scope` | 固定`server_local` |
| `age_basis` / `measurement_clock_synchronized` | `nmea_epoch_aligned_arrival`／false |
| `last_epoch_interval_ms` | 最新接受的不同UTC epoch間隔；0表示尚未觀察到間隔 |
| `source_age_ms` / `epoch_ms_of_day` | 當前回傳快照的age／UTC日內ms；無快照null |
| `fix` / `have_rmc` / `have_gga` | 本機fix新鮮度／該快照具有的句型 |
| `epochs` / `rmc` / `gga` | 接受的新epoch／RMC／GGA計數 |
| `checksum_errors` / `rejected_sentences` | checksum錯誤／拒收句數；前者包含於後者，不可相加 |
| `ignored_sentences` | checksum正確但不需收集的其他句型，例如GSV |
| `backwards_epochs` / `duplicate_epochs` | 倒退epoch／同類句型同epoch重複計數；正常RMC＋GGA配對不算重複 |
| `backlog_drops` | 本機因UART服務間隔過長而丟棄緩衝的次數 |

`client_diagnostic`包含`received`、`rx_age_ms`、`fresh`及wire資料的
`epoch_interval_ms`、`backlog_drops`、`nmea_errors`、`tx_errors`、`skipped_slots`、`status_bits`。
未收到時資料欄位為null；接收未滿90秒才`fresh=true`，**fresh表示低頻診斷快照仍在顯示期限，
不是位置仍可追蹤**。`counter_encoding`為`uint16_saturating_since_client_boot`；不能從這個
沒有Client boot ID的封包聲稱已可靠識別Client重開機。

`counters`欄位：

- 接收：`rx_data`、`rx_telemetry`、`rx_diagnostic`、`radio_errors`。
- 拒收：`rejected_length`、`rejected_format`、`rejected_binding`、`rejected_sequence`。
- 序號：`sequence_gaps`、`sequence_resyncs`；重同步不把Client重啟跳號當成數萬包丟失。
- DATA資格：`invalid_fix_packets`、`invalid_velocity_packets`，不等於整包解析失敗。
- ACK／復原：`ack_sent`、`ack_errors`、`ack_skipped`、`radio_recoveries`。
- 時間：`last_data_interval_ms`、`max_data_interval_ms`、`inferred_source_updates`、`inferred_source_interval_ms`。

`inferred_source_*`由收到的來源age推估，是Server推論值；辨識Client GNSS真實epoch間隔
應參考低頻DIAG及實機NMEA，不能把RF2Hz直接當GNSS2Hz。累計計數主要跨綁定保留，
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
| `id` / `ms` | 本次Server開機內的事件序號／millis時間 |
| `kind` | `data`、`telemetry`、`diagnostic`、`length`、`format`、`binding`、`sequence`、`radio_error`、`ack_error`或`ack_skipped` |
| `client_id` / `seq` / `length` | 可解析時的設備／序號及原始包長；未取得的數值可能為0 |
| `source_age_ms` | DATA內的來源age，不含接收後時間；沒有可用值為null |
| `rssi_dbm` / `snr_db` | RF接收量測；沒有原始RF資料的事件為null |
| `code` | RadioLib錯誤碼等事件碼，正常通常0 |
| `flags` | bit0為DATA fix、bit1為velocityValid；不是wire的course_and_flags原值 |
| `raw_hex` | 最多17 bytes原始RF資料的小寫hex；超長包只留前17 bytes，真實長度看length |

### 網頁除錯記錄與JSON匯出

除錯頁共用前述全頁 HTTP 排程，每秒最多啟動一次取樣，依序讀一頁 `/api/debug` 與增量
`/api/log`，忙碌時延後；不因 `more=true` 立即連發所有歷史頁。首次開頁面的 64 筆保留
歷史需分批補齊。每次附上最近的 track／status 快照與各自取得時間，並非四個 API 同時
取得。顯示除錯頁或啟動記錄時才取樣；錄製期間可以切到本站其他分頁。

開始記錄後最多 10 分鐘、1200 筆或 5 MiB **樣本資料**，任一上限到達便停止；JSON 縮排
與目前快照可能讓下載檔稍大。頁面文字顯示最多 32768 字元。瀏覽器背景／鎖屏可能延遲、
漏採；相鄰記錄間隔超過 2500 ms 會留下缺口資訊，另記錄逾時、讀取失敗與覆寫／重啟提示。
記錄存在瀏覽器記憶體，重新整理不會恢復；停止錄製後，目前快照仍可更新。

匯出檔為瀏覽器產生的`shorespotter_debug_<時間>.json`，不是另一個Server endpoint。
根結構為`schema_version:2`、`kind:"shore_spotter_diagnostics"`，包含：

- `exported_at`、`firmware_version`、`protocol_version`、`notes`（最多2000字元）、`config`。
- `event_identity:["boot_id","id"]`：事件識別／去重所用的欄位。
- `evidence_limits`：記錄頻率／背景分頁／來源age／機械回授的限制。
- `recording`：開始／停止時間、停止原因、筆數／大小／上限、gap／error計數與log丟失／重啟觀察。
- `current`：最近track、status、debug與接收時間、文字log、cursor／boot、errors／warnings；`current.events`另保留已合併／去重的最近最多64筆事件。
- `samples`：記錄期間的快照及各自取得時間、增量log、errors／warnings；`sample.events`包含該次新增事件與boot／next_id／more／reset／dropped。

`sample.debug.events`及`current.debug.events`只保留環形紀錄與分頁 metadata，不含`items`；
raw hex只在對應的事件陣列中保存，避免每筆樣本重複存整段歷史。分析時不能假設每筆
樣本各有完整64筆事件，應跨樣本合併、按boot分段、依事件ID去重，並檢查`more`、
`reset`與`dropped`。`event_backlog`表示仍在分批讀取，不列入錄製的gap計數。

HTTP分段耗時從`sample.status.timing.http_detail`或`current.status.timing.http_detail`
取得。匯出不發送控制命令，也不清除Server的事件或文字log；操作說明見
[現場除錯與匯出](debug-export.md)。

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
空集合也是合法設定，不會在重啟後恢復預設 ID。實際變更後回到手動並清除舊client狀態；全新Server預設未綁定，已有NVS不被覆蓋。

**Query Params**

| 參數 | 值 | 說明 |
|---|---|---|
| `action` | `set` | 明確指定／替換唯一 ID |
| `action` | `add` | 未綁定時新增；已綁定相同 ID 為 no-op，不同 ID 回 409 |
| `action` | `remove` | 移除 ID |
| `action` | `clear` | 清空所有 |
| `id` | `AB12` | 1–4 碼十六進位，set/add/remove 必填；0、SERVER_ID、FFFF 不可用 |

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
| moving | 最新目標尚未抵達；移動中禁止指南針校正，但可調整共用速限 |
| speed_limit_deg_s | 所有模式共用最高速度，預設 30、可調 1–90°/s |
| prediction_enabled / prediction_alpha | 0.6-dev：GPS位置預測 bool／0 或 1，預設 true／1；只影響 GPS 目標估計 |
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

## `POST /api/track/calibrate`

輸入鏡頭上普通磁針指南針的讀數（磁北），北 0°、東 90°、南 180°、西 270°。

```text
POST /api/track/calibrate?bearing=90
```

只接受有限十進位數字 `0 <= bearing < 360`。不需要 server/client GPS fix，
不再接受地標 lat/lon。先選 Manual 或 pause，等鏡頭與腳架停穩再讀指南針。
手動運動尚未完成時回 409，網頁也會暫停校正按鈕；仍需自行確認實際鏡頭與磁針已停穩。

```text
mount_offset = compass_bearing + servo_angle
```

只有固定腳架的鏡頭磁針參考，不使用板上磁力計。不讀寫 mount*／mag* NVS；
重開機、腳架轉動或重新架設後必須重校。

GPS 追蹤使用 `servo_angle = mount_offset + declination - true_bearing`。
磁偏角東正西負，依攝影站 GPS 位置與 UTC 日期使用 WMM2025 離線表，適用北緯 18–28°、
東經 116–124°、2025–2029 年、海平面。日期未知／超過 60 秒未更新、位置失效或超出模型
範圍時，`declination_deg=null`，GPS 模式保持；UART 模式不受影響。校正本身不需 GPS。
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

錯誤：400（缺少 bearing、非法數字、超出範圍），409（GPS／UART 控制中，需先切手動），
503（PWM 不可用）。校正不移動 Servo；start/resume 也不覆寫校正。

## `/api/log`

攝影站的滾動執行紀錄。韌體把所有 log 同時寫到 USB 序列埠和一個 **4 KB 環形緩衝**，
0.5移除獨立`/log`網頁；0.6-dev新增除錯分頁，並使用此文字API收集增量紀錄。

> 收包**不是**一包一行：那樣 4 KB 會在約 16 秒內被洗完。RX 改成累積後每 60 秒一行統計
> （`[SERVER] RX 60s`，含DATA序號缺口、RSSI/SNR的min/avg/max、雙方
> GPS、距離方位、servo 與模式），所以同一個緩衝大約能留 30 分鐘以上。

```
GET  /api/log?from=<絕對位移>   取得該位移之後的新內容
POST /api/log                   清除緩衝
```

回應是 **plain text**，帶 `Cache-Control: no-store`；增量讀取資訊放在三個自訂header：

| Header | 說明 |
|---|---|
| `X-Log-Boot` | 目前Server boot ID；變更時應重設前端cursor，不能只靠位移判斷重啟 |
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
  "default_enabled": true,
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

NVS `shorespotter/gpspredict` 保存 uint32 0／1；缺值、非法值預設開啟，回讀確認成功才套用。
重複相同已保存值不重寫。參數非法／多餘回 400，上下文不符回 409，保存失敗回 503 並保留 RAM 值。
此開關與v4封包共存；外推使用v4的來源age估計＋RF airtime＋接收年齡，但不把200 ms不確定量加入位移。GNSS內部延遲仍未量測。

## `POST /api/servo/mode`

`?mode=manual|gps|uart`，三者互斥，開機預設 UART；舊 `mode=auto` 回 400。
選 GPS 要求 `gps_available=true`，Server／Client 任一 Bad、Miss、過期或品質缺漏時
回 409，保留原模式與目標。條件在伺服器執行當下檢查，不能靠舊按鈕狀態繞過。
相容舊呼叫端的 `mode=jetson` 輸入，視為 `uart`；此 API、status／track 與 resume
的模式回應一律使用 `uart`。UART 舊 `SET <毫度>` 保留，另支援 SYNC／SET2。

- manual：撤銷所有自動控制，維持最後 PWM 角度，可使用 slider。
- gps：只用 GPS；指南針與磁偏角有效、兩端 fix／品質未滿 2 秒、衛星 ≥6、HDOP ≤3，
  Good／OK 條件連續 2 秒後追蹤。訊號變差則保持，不切 UART。
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

start 選 GPS；resume 恢復上次選取的 GPS／UART（開機預設 UART）。選 GPS 的所有路徑
都會再次檢查兩端 Good／OK 且資料新鮮，否則回 409 並保留原模式。沿用 RAM 校正，
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
| lat_delta_e6／lon_delta_e6 | DATA | `client.lat`／`client.lon` | signed24符號延伸、加固定原點、÷10⁶；必須同看fix |
| fix | DATA | `client.fix` | 再套用source＋airtime＋RX age＋200 ms門檻，不直接永久照搬bit |
| speedDmS | DATA | `client.speed_cms` | ×10 cm/s；255→null，解析度10 cm/s |
| courseDeg10 | DATA | `client.course_deg10` | 保留0.1°單位；4095→null |
| velocityValid | DATA | `client.velocity_valid` | 再要求Client fix仍新鮮；決定是否可外推 |
| satelliteClass | DATA | `client.satellite_class` | 0未知、1為0–5、2為6–7、3為≥8；即時gate使用此分級 |
| hdop10 | DATA | `client.hdop` | ÷10；255→null，發送端向上量化 |
| age10ms | DATA | `client.source_age_ms` | ×10；255→null，不隨API輪詢增加 |
| source age＋ToA＋RX age | Server推算 | `client.sample_age_ms` | 當前估計定位年齡，不含另外用於gate的200 ms不確定量 |
| DATA RxDone時間 | Server | `client.rx_age_ms`／`last_rx_sec` | 距接收的ms／秒，不等於來源測量年齡 |
| batteryMv | TEL | `telemetry.batt_mv` | 直接mV；未知0 |
| tempC／humidityPct | TEL | `telemetry.temp_c`／`humidity_pct` | 整數°C／%；未知→null |
| satellites | TEL | `client.satellites` | 精確低頻顆數；未知或TEL滿90秒→null，不作即時gate |
| TEL RxDone時間 | Server | `client.satellites_age_ms`／`telemetry.last_rx_sec` | TEL接收年齡，不代表衛星數精確測量時間 |
| epochIntervalMs等 | DIAG | `/api/debug.client_diagnostic` | Client低頻解析／排程快照，帶獨立接收年齡 |
| RSSI／SNR | Server收到DATA時的radio量測 | `/api/status.lora.rssi`／`snr` | dBm／dB，不是Server從ACK反解 |
| rssiDbm10／snrQuarterDb | ACK下行 | 無Server解碼欄位 | Client分別÷10／÷4，供ATPC |
| uint16 DATA seq | DATA | `/api/debug`序號與event欄位 | 只由DATA使用，TEL／DIAG不占它的序號 |

加速度已從封包和API移除，不回假0。`server.*`來自本機GNSS／感測器；其中既有衛星／HDOP
顯示欄位仍由TinyGPS提供，真正gate使用同epoch collector。`servo.*`是命令／追蹤狀態，
沒有實際機械角度回授。軌跡由瀏覽器累積，API不保存整段軌跡。

`bearing`由攝影站位置與收到的Client位置計算；α=1的`servo.target`使用速度向量外推後
的位置，因此兩者可能不同。α=0不外推，兩者仍經鏡頭校正、磁偏角與0–180°限制換算。
