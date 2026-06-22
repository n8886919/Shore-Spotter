# 攝影站端 API

攝影站以 WiFi AP 模式運作。

- `SSID = ShoreSpotter_<MAC末4碼>`
- `Password = nolan1234`

以下 endpoint 透過瀏覽器或任何 HTTP client 存取，基底 URL 為 `http://192.168.4.1`。

---

## `GET /`

回傳完整 Web UI（單頁 HTML，內嵌 Leaflet.js 地圖）。

---

## `GET /api/track`

即時狀態與軌跡，前端每 2 秒輪詢一次。

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
    "last_rx_sec": 1
  },
  "server": {
    "lat": 25.111111,
    "lon": 121.111111,
    "fix": 1,
    "temp_c": 30.1,
    "humidity_pct": 76
  },
  "telemetry": {
    "batt_mv": 3850,
    "temp_c": 28.5,
    "humidity_pct": 72,
    "last_rx_sec": 5
  },
  "history": [
    { "lat": 25.123456, "lon": 121.123456 },
    { "lat": 25.123490, "lon": 121.123510 }
  ]
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
| `telemetry.batt_mv` | Surfer 端電池電壓 mV（每 10 s 更新）|
| `telemetry.temp_c` | Surfer 端溫度，`null` = 無感測器 |
| `telemetry.humidity_pct` | Surfer 端濕度 %，`null` = 無感測器 |
| `server.fix` | 攝影站本身的 GPS fix 狀態 |
| `server.temp_c` | 攝影站本機溫度，`null` = 無感測器 |
| `server.humidity_pct` | 攝影站本機濕度，`null` = 無感測器 |
| `history` | 過去 2 小時軌跡（每 10 秒一點，最多 720 點，循環 buffer）|

---

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
  "servo_origin_deg": 123.0
}
```

| 欄位 | 說明 |
|---|---|
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
| `servo_origin_deg` | 目前 Servo 方位校正基準角 |

---

## `GET /api/whitelist`

列出目前 Client 白名單。

**Response**

```json
{
  "whitelist": ["E91C", "AB12"],
  "count": 2
}
```

---

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

---

## `POST /api/calibrate/servo`

Servo 方位角校正。步驟：手動置中 Servo → 放好攝影站 → 讀取羅盤方向 → 呼叫此 endpoint。

> TODO：目前需手動輸入羅盤讀數；未來整合 QMC5883L 後可自動讀取。

**Query Params**

| 參數 | 值 |
|---|---|
| `heading` | 目前羅盤讀數（度，0–360）|

**Response**

```json
{ "ok": true, "servo_origin_deg": 123.0 }
```
