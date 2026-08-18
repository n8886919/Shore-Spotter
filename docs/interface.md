# 介面規格（Interface）

本文件彙整 Shore Spotter 的兩層資料契約：

1. **下層 — LoRa 封包**（Surfer ↔ 攝影站，RF 二進位格式）
2. **上層 — 攝影站 HTTP API**（攝影站 → 手機監控頁，JSON）
3. **欄位對應表**（封包欄位如何流向 API）

---

# 1. LoRa 封包

採自定義二進位格式，所有欄位 little-endian packed，無 padding。

## RF 參數

| 參數 | 值 |
|---|---|
| 頻率 | 923.2 MHz（台灣合法 AS923）|
| 頻寬 | 125 kHz |
| Spreading Factor | SF9 |
| Coding Rate | 4/5 |
| Sync Word | `0x12` |
| 發射功率 | 17 dBm（ATPC 動態調整 10–22 dBm）|
| 發送間隔 | 1 秒 / 封包（位置），30 秒 / 封包（遙測）|

> **必須兩端一致的是 頻率 / 頻寬 / SF / Sync Word**，不一致就完全解不出封包。
> **Coding Rate 不必一致**：explicit header 模式（RadioLib 預設）會把 payload 的 CR
> 寫在 header 裡，而 header 固定以 4/8 編碼，接收端因此能自動解出任何 CR。
> 所以兩塊板即使 CR 不同也能正常通訊，CR 只決定「這塊板自己發送時」用什麼編碼率。

## 封包類型

| `msgType` | 名稱 | 間隔 | 大小 | 空中時間 | 說明 |
|---|---|---|---|---|---|
| `1` | MSG_DATA | 1 s | 32 B | 246 ms | 位置 + 速度向量 |
| `2` | MSG_ACK | 每 4 包 | 20 B | 185 ms | 岸上端回覆短 ACK（含 ackSeq / RSSI / SNR） |
| `3` | MSG_HELLO | — | — | — | 保留 |
| `4` | MSG_TELEMETRY | 30 s（限定時槽）| 19 B | 185 ms | 電量 + 溫濕度 |

平均通道佔用 = 246 + 185/4 ≈ **292 ms/s（29%）**。

### ACK 為何不是每包都回

ACK 只有 Client 會用到（ATPC 的上行品質來源 + 連線存活判斷），兩者都不需要 1 Hz
解析度。改成每 4 包回一次省下 75% 的 ACK 空中時間，主要是為了未來多 Surfer 時的
通道餘裕。挑選依據是 **client 的 `seq`（`seq % 4 == 0`）而不是 server 端計數器**，
這樣掉包不會讓排程滑掉，兩端也不必同步額外狀態。Client 端的斷線門檻由
`ACK_EVERY_N` 推導，改 N 不需要手動改門檻。

### 遙測時槽

30 s 是 1 s 的整數倍，遙測若「到期就送」會固定壓在位置封包與其 ACK 上，兩包同歸於盡。
因此遙測只在位置封包送出後的**靜默時槽**起送。時槽上下界不是寫死的常數，而是開機時
用 `radio.getTimeOnAir()` 由實際 RF 參數推導（`computeAirtimeBudget()`）：

```
下界 = ToA(DATA) + ToA(ACK) + 80 ms   ≈ 511 ms
上界 = 發送間隔 − ToA(TELEMETRY) − 80 ms ≈ 735 ms
```

所以調整 SF / CR / 封包大小 / 發送間隔都不必手動重算。開機時序列埠會印出實際數值；
若空中時間大到塞不進一個發送週期，會印 `WARNING` 並退回「ACK 結束後隨時可送」。

## 封包結構

所有封包 = `[PacketHeader 11 bytes][Payload][MAC 4 bytes]`

> 結構定義集中於 [../include/protocol.h](../include/protocol.h)，client 與 server 共用。

### PacketHeader（11 bytes）

| 欄位 | 型別 | 說明 |
|---|---|---|
| `magic` | `uint8` | 固定 `0x53` |
| `version` | `uint8` | 目前為 `1` |
| `networkId` | `uint8` | 邏輯群組 ID，預設 `0x01` |
| `srcId` | `uint16` | 發送方 ID（ESP32 MAC 末 2 bytes，開機自動衍生）|
| `dstId` | `uint16` | 目標 ID（Server = `0x0010`，廣播 = `0xFFFF`）|
| `msgType` | `uint8` | 見上表 |
| `seq` | `uint16` | 序號，每次發送遞增 |
| `payloadLen` | `uint8` | Payload 長度（bytes）|

### PositionPayload（17 bytes，MSG_DATA 使用）

| 欄位 | 型別 | 說明 |
|---|---|---|
| `latE7` | `int32` | 緯度 × 1e7（定點數，7 位小數）|
| `lonE7` | `int32` | 經度 × 1e7 |
| `fix` | `uint8` | GPS fix（`0`=無效，`1`=有效）|
| `speedCmS` | `uint16` | 瞬間速度，cm/s（由 GPS 計算）|
| `courseDeg10` | `uint16` | 行進方向，0.1° 單位（0–3599）|
| `accelCmS2` | `int16` | 縱向加速度，cm/s²（指數平滑）|
| `satellites` | `uint8` | 使用中衛星數（`0xFF`=未知）—用於 GPS 品質判定 |
| `hdop10` | `uint8` | HDOP × 10（`0xFF`=未知）—用於 GPS 品質判定 |

### TelemetryPayload（4 bytes，MSG_TELEMETRY 使用）

| 欄位 | 型別 | 說明 |
|---|---|---|
| `batteryMv` | `uint16` | 電池電壓 mV（0 = 無資料）|
| `tempC` | `int8` | 溫度 °C（四捨五入整數，`INT8_MIN` = 無感測器）|
| `humidityPct` | `uint8` | 相對濕度 0–100%（`0xFF` = 無感測器）|

### AckPayload（5 bytes，MSG_ACK 使用）

| 欄位 | 型別 | 說明 |
|---|---|---|
| `ackSeq` | `uint16` | 被確認收到的 DATA 封包序號 |
| `rssiDbm10` | `int16` | 接收 RSSI × 10（dBm） |
| `snrDb10` | `int8` | 接收 SNR × 10（dB） |

### MAC（保留）

4 bytes，目前填 0，預留 HMAC 驗證（Stage 2）。

## 白名單機制

Server 維護 `clientWhitelist[]`（最多 16 筆，執行期可透過 API 修改），只接受其中 `srcId` 的封包，其餘靜默丟棄。

開機時先讀 NVS 的白名單；若無資料才回退 `DEFAULT_WHITELIST[]`。

擴充多人模式只需將新 Client 的 MAC 末 2 bytes 加入清單即可，詳見下方 [POST /api/whitelist](#post-apiwhitelist)。

## 群組隔離機制

兩層隔離，由粗到細：

| 層級 | 機制 | 說明 |
|---|---|---|
| PHY | `RF_SYNC_WORD = 0x12` | 不同 sync word 的封包在射頻層直接被 SX1262 丟棄，節省 CPU |
| 應用 | `NETWORK_ID` | 軟體層群組 ID，允許同頻段多群組共存 |

---

# 2. 攝影站 HTTP API

攝影站以 WiFi station (STA) 模式連線手機熱點。

- 熱點 SSID / 密碼設定於 [../include/wifi_config.h](../include/wifi_config.h)（`WIFI_SSID` / `WIFI_PASSWORD`）
- 攝影站開機會自動連線，IP 由手機分配，開機時顯示於 OLED

以下 endpoint 透過瀏覽器或任何 HTTP client 存取，基底 URL 為攝影站的 IP（例：`http://192.168.x.x`）。

## `GET /`

回傳完整 Web UI（單頁 HTML，內嵌 Servo 控制 + Canvas 極座標雷達圖，無外部 CDN 依賴）。

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
    "speed_cms": 312,
    "course_deg10": 2423,
    "accel_cms2": 15,
    "satellites": 9,
    "hdop": 1.2,
    "last_rx_sec": 1
  },
  "server": {
    "lat": 25.111111,
    "lon": 121.111111,
    "fix": 1,
    "satellites": 7,
    "hdop": 1.5,
    "temp_c": 30.1,
    "humidity_pct": 76,
    "batt_pct": 100,
    "charging": true
  },
  "telemetry": {
    "batt_mv": 3850,
    "temp_c": 28.5,
    "humidity_pct": 72,
    "last_rx_sec": 5
  },
  "servo": {
    "angle": 87.0,
    "target": 87.0,
    "mode": "tracking",
    "calibrated": true,
    "mount_offset_deg": 12.5
  },
  "mag": {
    "online": true,
    "heading": 153.0
  }
}
```

| 欄位 | 說明 |
|---|---|
| `linked` | Client 是否在線（5 秒內有收到封包）|
| `bearing` | 攝影站 → Surfer 方位角（度），無效時為 `-1` |
| `client.fix` | GPS fix 狀態 |
| `client.speed_cms` | Surfer 瞬間速度，cm/s |
| `client.course_deg10` | 行進方向，0.1° 單位 |
| `client.accel_cms2` | 縱向加速度，cm/s²（指數平滑）|
| `client.last_rx_sec` | 距上次收到封包的秒數，失聯時為 `-1` |
| `telemetry.batt_mv` | Surfer 端電池電壓 mV（每 30 s 更新）|
| `telemetry.temp_c` | Surfer 端溫度，`null` = 無感測器 |
| `telemetry.humidity_pct` | Surfer 端濕度 %，`null` = 無感測器 |
| `server.fix` | 攝影站本身的 GPS fix 狀態 |
| `server.temp_c` | 攝影站本機溫度，`null` = 無感測器 |
| `server.humidity_pct` | 攝影站本機濕度，`null` = 無感測器 |
| `servo.angle` | Servo 目前角度（0–180°）；追蹤時受 120°/s 轉速限制，會略微落後 `target` |
| `servo.target` | 追蹤模式下計算出的目標角度（0–180°），以**外推後**的 Surfer 位置計算 |
| `servo.mode` | `idle` / `manual` / `tracking` / `paused` |
| `servo.calibrated` | 是否已鎖定 `mount_offset`（按過 start）|
| `servo.mount_offset_deg` | Servo→世界座標的安裝偏移角，校正後鎖定 |
| `mag.online` | 磁力計（QMC6310）是否在線 |
| `mag.heading` | 攝影站板子的羅盤航向（度），無效時為 `-1` |
| `client.satellites` / `server.satellites` | 雙方使用中衛星數，`-1` = 無效 |
| `client.hdop` / `server.hdop` | 雙方 HDOP，`-1` = 無效（兩端套用相同 Good/Normal/Bad 標準）|
| `server.batt_pct` | 攝影站 18650 電量 %（3.2V=0%、4.15V=100%），`-1` = 無電池/未知 |
| `server.charging` | 攝影站是否接外部電源（USB/Type-C，VBUS 在），用於 ⚡ 指示 |

> 過去 5 分鐘軌跡不再由 API 回傳；前端每秒把 `client`/`server` 當下位置附加到本地陣列（最多 300 點），重整頁會重新累積。

> GPS 品質判定（三級門檻，兩端一致）：**Good** = fix 且 HDOP ≤ 2.0 且 sats ≥ 6；**OK** = fix 且 HDOP ≤ 5.0 且 sats ≥ 4；其餘為 **Bad**。OLED 另用 **Miss** 表示「無衛星/無定位」（LoRa 則為未收到封包）。

## `GET /api/status`

GPS 訊號品質、LoRa 訊號統計、Servo 校正狀態。

**Response**

```json
{
  "server_gps": {
    "fix": 1,
    "satellites": 9,
    "hdop": 1.20
  },
  "lora": {
    "rssi": -85.0,
    "snr": 7.5,
    "rssi_avg": -87.3,
    "snr_avg": 6.9,
    "pkt_rate": 0.98,
    "rx_data": 102,
    "rx_telemetry": 11,
    "rx_drop": 3,
    "drop_rate": 0.026,
    "ack_tx": 102
  },
  "env": {
    "temp_c": 30.1,
    "humidity_pct": 76
  },
  "health": {
    "uptime_s": 5432,
    "heap_free": 188304,
    "heap_min": 173016,
    "reset_reason": "1",
    "rx_error": 0
  },
  "servo": {
    "angle": 87.0,
    "mode": "tracking",
    "calibrated": true,
    "mount_offset_deg": 12.5
  },
  "mag": {
    "online": true,
    "heading": 153.0,
    "calibrated": true,
    "residual_deg": 0.82
  }
}
```

| 欄位 | 說明 |
|---|---|
| `mag.calibrated` | 是否已做過 hard-iron 校正 |
| `mag.residual_deg` | 校正擬合殘差（度），`null` = 未校正 |
| `server_gps.satellites` | 可見衛星數，`-1` = 無效 |
| `server_gps.hdop` | 水平精度因子，數值越小越好，`-1` = 無效 |
| `lora.rssi` | 最近一筆封包 RSSI（dBm）|
| `lora.snr` | 最近一筆封包 SNR（dB）|
| `lora.rssi_avg` | 近 20 筆 RSSI 滾動平均，`null` = 無資料 |
| `lora.snr_avg` | 近 20 筆 SNR 滾動平均，`null` = 無資料 |
| `lora.pkt_rate` | 近 60 秒封包率（封包/秒）|
| `lora.rx_data` | 成功解析的 DATA 封包累計 |
| `lora.rx_telemetry` | 成功解析的 TELEMETRY 封包累計 |
| `lora.rx_drop` | 驗證失敗或白名單不符封包累計 |
| `lora.drop_rate` | `rx_drop/(rx_data+rx_telemetry+rx_drop)` |
| `lora.ack_tx` | ACK 下行封包累計 |
| `env.temp_c` | 攝影站本機溫度 |
| `env.humidity_pct` | 攝影站本機濕度 |
| `health.uptime_s` | 開機秒數 |
| `health.heap_free` | 目前可用 heap |
| `health.heap_min` | 開機後最小可用 heap |
| `health.reset_reason` | ESP 重啟原因代碼 |
| `health.rx_error` | LoRa 接收錯誤碼累計 |
| `servo.angle` | Servo 目前角度（0–180°）|
| `servo.mode` | `idle` / `manual` / `tracking` / `paused` |
| `servo.calibrated` | 是否已鎖定 `mount_offset` |
| `servo.mount_offset_deg` | Servo→世界座標安裝偏移角 |
| `mag.online` | 磁力計（QMC6310）是否在線 |
| `mag.heading` | 攝影站板子羅盤航向（度），`-1` = 無效 |

## `GET /api/whitelist`

列出目前 Client 白名單。

**Response**

```json
{
  "whitelist": ["E91C", "AB12"],
  "count": 2
}
```

## `POST /api/whitelist`

新增、移除或清空白名單。變更會寫入 NVS 並在重啟後保留。

**Query Params**

| 參數 | 值 | 說明 |
|---|---|---|
| `action` | `add` | 新增 ID |
| `action` | `remove` | 移除 ID |
| `action` | `clear` | 清空所有 |
| `id` | `E91C` | 目標 ID（action=add/remove 時必填）|

**範例**

```
POST /api/whitelist?action=add&id=AB12
POST /api/whitelist?action=remove&id=E91C
POST /api/whitelist?action=clear
```

**Response**（成功時回傳更新後的白名單）

```json
{
  "whitelist": ["E91C", "AB12"],
  "count": 2
}
```

## `POST /api/track/calibrate`

用**已知座標的地標**鎖定 `mount_offset`，取代「對著水裡的人拖滑桿」。

```
POST /api/track/calibrate?lat=<地標緯度>&lon=<地標經度>
```

攝影站用自身 GPS 與地標座標算出真方位角，再套用與 `startTracking()` 相同的公式：

```
mount_offset = 地標方位 − heading + 目前 servo 角度
```

**不需要追蹤器在場、不需要 client 封包、不需要第二個人**，只要攝影站自己有 GPS fix。
操作是把地標對到**觀景窗正中央**——400mm 下這是 ±0.05° 的照準，比對著海上的人準一個數量級。

回應：

```json
{"ok":true,"bearing":312.45,"distance_m":1840,"mount_offset_deg":88.3,
 "servo_angle":90.0,"mag_calibrated":true,"warning":""}
```

`warning` 會在兩種情況有值：磁力計還沒做 hard-iron 校正（那這個 offset 換場地就失效），
或地標距離 < 300 m（近地標會把攝影站自身的定位誤差放大成方位誤差：1 m 誤差在 100 m
是 0.6°，在 1 km 只有 0.06°）。

錯誤：`400`（缺參數 / 座標超出範圍）、`409`（`need server GPS fix`）。

## `/api/log`

攝影站的滾動執行紀錄。韌體把所有 log 同時寫到 USB 序列埠和一個 **4 KB 環形緩衝**，
網頁「紀錄」分頁可直接看——機器架在沙灘腳架上時不可能接筆電讀序列埠，而
**espota 只上傳韌體、不提供任何 log**。

```
GET  /api/log?from=<絕對位移>   取得該位移之後的新內容
POST /api/log                   清除緩衝
```

回應是 **plain text**（不是 JSON，這樣 log 內容不必跳脫），簿記放在兩個自訂 header：

| Header | 說明 |
|---|---|
| `X-Log-Next` | 本次回傳結束時的絕對位移，下次帶進 `from` 即可只取增量 |
| `X-Log-Dropped` | `1` = 你要的位移已滾出 4 KB 視窗（或裝置重開、位移倒退），已自動跳到目前最舊的位置 |

位移是單調遞增的總位元組數，所以前端輪詢只會拿到新內容。裝置重開後 `logTotal` 歸零、
位移倒退，此時一樣回 `X-Log-Dropped: 1` 並從頭給起。

> 前端只在「紀錄」分頁可見時才輪詢（每 2 秒），因為 ESP32 的 WebServer 一次只服務一個連線。

## `/api/mag/calibrate`

磁力計 hard-iron 校正。

```
POST /api/mag/calibrate              開始收樣本
POST /api/mag/calibrate?action=cancel 中止
GET  /api/mag/calibrate              查詢進度
```

開始後把**整台機器水平慢慢轉一整圈**。韌體以 20 Hz 取樣，每當向量轉過 2° 收一筆
（最多 180 筆），並以 36 個 10° 的分格統計涵蓋率；收滿 34/36 格就自動做最小平方圓擬合，
圓心即為 hard-iron 偏移，寫入 NVS。逾時 120 秒。

```json
{"state":"done","online":true,"coverage_pct":100,"samples":178,"calibrated":true,
 "residual_deg":0.82,"field_gauss":0.3714,"offset_x":0.0213,"offset_y":-0.0147,
 "heading":47.2,"error":""}
```

| 欄位 | 說明 |
|---|---|
| `state` | `idle` / `collecting` / `done` / `failed` |
| `coverage_pct` | 轉圈涵蓋率（僅 `collecting` 時有意義）|
| `residual_deg` | 擬合殘差換算成 heading 誤差。**< 1° = 安裝乾淨**；好幾度代表 soft-iron（servo 鋼齒輪），該把板子移遠 |
| `field_gauss` | 擬合出的水平磁場強度。台灣應該接近 **0.37 G**，差太多代表有強烈局部干擾 |
| `error` | `field over range — board is too close to the servo` / `no rotation detected` / `incomplete turn` / `circle fit failed` |

> 沒有 hard-iron 校正時，heading 是真實方位角被正弦扭曲後的結果（板上 18650 的鍍鎳鋼殼
> 就足以造成 20~30° 且**隨面向而變**的誤差）。這正是為什麼未校正時 `mount_offset` 換個
> 方位架設就失效、每次都得重新對準。校正過後它才是真正的常數。

## `POST /api/servo`

手動設定 Servo 角度（用來對準 surfer）。追蹤中（`mode=tracking`）會被拒絕，需先按暫停。

**Query Params**

| 參數 | 值 |
|---|---|
| `angle` | 目標角度（0–180）|

**Response**

```json
{ "ok": true, "angle": 87.0 }
```

錯誤時回 `409`（`pause tracking first`）、`400`（`missing angle param`），或在 LEDC/PWM 初始化失敗時回 `503`（`servo PWM unavailable`）。

## `POST /api/track/start`

鎖定目前對準狀態為校正基準（計算 `mount_offset`）並進入自動追蹤。需 server 與 client 都有 GPS fix。

**Response**

```json
{ "ok": true, "mount_offset_deg": 12.5 }
```

無法校正時回 `409`（`need server+client GPS fix`）；PWM 不可用時回 `503`（`servo PWM unavailable`）。

## `POST /api/track/pause`

暫停追蹤，Servo 維持當前角度，不再自動更新。

```json
{ "ok": true, "mode": "paused" }
```

## `POST /api/track/resume`

以現有校正恢復自動追蹤（無需重新對準）。未校正時回 `409`；PWM 不可用時回 `503`。

```json
{ "ok": true, "mode": "tracking" }
```

## `POST /api/track/stop`

回到手動控制（保留校正結果）。

```json
{ "ok": true, "mode": "manual" }
```

---

# 3. 欄位對應表（封包 → API）

攝影站收到 LoRa 封包後，解析並轉成 HTTP API 的 JSON。以下為主要欄位的對應關係：

| LoRa 封包欄位 | 來源封包 | API JSON 欄位 | 轉換 |
|---|---|---|---|
| `latE7` | PositionPayload | `client.lat` | ÷ 1e7 |
| `lonE7` | PositionPayload | `client.lon` | ÷ 1e7 |
| `fix` | PositionPayload | `client.fix` | 直接 |
| `speedCmS` | PositionPayload | `client.speed_cms` | 直接 |
| `courseDeg10` | PositionPayload | `client.course_deg10` | 直接 |
| `accelCmS2` | PositionPayload | `client.accel_cms2` | 直接 |
| `batteryMv` | TelemetryPayload | `telemetry.batt_mv` | 直接 |
| `tempC` | TelemetryPayload | `telemetry.temp_c` | 直接整數（`INT8_MIN` → `null`）|
| `humidityPct` | TelemetryPayload | `telemetry.humidity_pct` | 直接（`0xFF` → `null`）|
| `rssiDbm10` / `snrDb10` | AckPayload（下行）| `lora.rssi` / `lora.snr` | ÷ 10（攝影站本地量測）|

> `bearing` 不是封包欄位，而是攝影站用「自身 GPS」與「`client.lat/lon`」即時計算；`server.*` 來自攝影站本機 GPS 與感測器；過去 5 分鐘軌跡不在此 API，而是前端用每秒的 `client/server` 位置自行累積（5 分鐘 / 300 點）。`servo.*` 為 Servo 追蹤狀態，`mag.heading` 由板上 QMC6310 磁力計提供，三者皆攝影站本地產生。

> **`bearing` 與 `servo.target` 為何可能不一致**：`client.lat/lon` 與 `bearing` 用的是
> **原始收到的**位置；servo 用的是沿速度向量**外推後**的位置（見 features.md §4）。
> 高速時兩者可差數公尺 / 數度，這是預期行為，不是校正跑掉。
