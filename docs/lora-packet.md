# LoRa 封包設計

採自定義二進位格式，所有欄位 little-endian packed，無 padding。

---

## RF 參數

| 參數 | 值 |
|---|---|
| 頻率 | 923.2 MHz（台灣合法 AS923）|
| 頻寬 | 125 kHz |
| Spreading Factor | SF9 |
| Coding Rate | 4/7 |
| Sync Word | `0x12` |
| 發射功率 | 17 dBm |
| 發送間隔 | 1 秒 / 封包（位置），10 秒 / 封包（遙測）|

---

## 封包類型

| `msgType` | 名稱 | 間隔 | 說明 |
|---|---|---|---|
| `1` | MSG_DATA | 1 s | 位置 + 速度向量 |
| `2` | MSG_ACK | event-driven | 岸上端收到 DATA 後回覆短 ACK（含 ackSeq / RSSI / SNR） |
| `3` | MSG_HELLO | — | 保留 |
| `4` | MSG_TELEMETRY | 10 s | 電量 + 溫濕度 |

---

## 封包結構

所有封包 = `[PacketHeader 11 bytes][Payload][MAC 4 bytes]`

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

### PositionPayload（15 bytes，MSG_DATA 使用）

| 欄位 | 型別 | 說明 |
|---|---|---|
| `latE7` | `int32` | 緯度 × 1e7（定點數，7 位小數）|
| `lonE7` | `int32` | 經度 × 1e7 |
| `fix` | `uint8` | GPS fix（`0`=無效，`1`=有效）|
| `speedCmS` | `uint16` | 瞬間速度，cm/s（由 GPS 計算）|
| `courseDeg10` | `uint16` | 行進方向，0.1° 單位（0–3599）|
| `accelCmS2` | `int16` | 縱向加速度，cm/s²（指數平滑）|

### TelemetryPayload（5 bytes，MSG_TELEMETRY 使用）

| 欄位 | 型別 | 說明 |
|---|---|---|
| `batteryMv` | `uint16` | 電池電壓 mV（0 = 無資料）|
| `tempC10` | `int16` | 溫度 × 10（0.1°C，`INT16_MIN` = 無感測器）|
| `humidityPct` | `uint8` | 相對濕度 0–100%（`0xFF` = 無感測器）|

### AckPayload（5 bytes，MSG_ACK 使用）

| 欄位 | 型別 | 說明 |
|---|---|---|
| `ackSeq` | `uint16` | 被確認收到的 DATA 封包序號 |
| `rssiDbm10` | `int16` | 接收 RSSI × 10（dBm） |
| `snrDb10` | `int8` | 接收 SNR × 10（dB） |

### MAC（保留）

4 bytes，目前填 0，預留 HMAC 驗證（Stage 2）。

---

## 白名單機制

Server 維護 `clientWhitelist[]`（最多 16 筆，執行期可透過 API 修改），只接受其中 `srcId` 的封包，其餘靜默丟棄。

開機時先讀 NVS 的白名單；若無資料才回退 `DEFAULT_WHITELIST[]`。

擴充多人模式只需將新 Client 的 MAC 末 2 bytes 加入清單即可，詳見 [API 文件](api.md)。

---

## 群組隔離機制

兩層隔離，由粗到細：

| 層級 | 機制 | 說明 |
|---|---|---|
| PHY | `RF_SYNC_WORD = 0x12` | 不同 sync word 的封包在射頻層直接被 SX1262 丟棄，節省 CPU |
| 應用 | `NETWORK_ID` | 軟體層群組 ID，允許同頻段多群組共存 |
