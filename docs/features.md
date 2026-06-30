# 韌體功能說明（Features）

本文件描述 Shore Spotter 韌體的「行為與流程」，分為以下面向：

1. **整體架構** — 角色切分與 build flag
2. **LoRa 無線通訊** — 傳送節奏與群組隔離
3. **Client（下水端）** — 定位/遵測發送與 ATPC
4. **Server（岸上攝影站）** — 接收、方位計算、軟跡、白名單
5. **WiFi / Web 監控** — 熱點連線與監控頁
6. **OLED 顯示** — 各畫面模擬
7. **設定持久化（NVS）** — 重啟後保留的設定
8. **健康 / 告警門檻** — 電量、溫濕度、系統健康

封包/HTTP 欄位細節見 [interface.md](interface.md)；硬體腳位見 [hardware.md](hardware.md)。

---

# 1. 整體架構

- 單一 `src/main.cpp`，用 build flag 分兩種角色：`ROLE_CLIENT`（下水端）、`ROLE_SERVER`（岸上攝影站）。
- 共用 LoRa 封包協定抽在 [../include/protocol.h](../include/protocol.h)；硬體腳位與 RF 參數在 `main.cpp` 上方常數區；手機熱點帳密在 [../include/wifi_config.h](../include/wifi_config.h)。
- 角色由編譯期 `env:tbeam-client` / `env:tbeam-server`決定，未選或同時選兩個角色都會編譯失敗。

# 2. LoRa 無線通訊

- RF 參數、封包格式、msgType、欄位定義詳見 [interface.md](interface.md)。
- 傳送節奏：位置封包每 1 秒、遙測每 30 秒；Server 收到 DATA 後即時回 ACK。
- 兩層群組隔離：PHY 層 Sync Word + 應用層 `NETWORK_ID`，允許同頻段多群組共存。
- 節點 ID：開機由晶片 MAC 末 2 bytes 衍生（與 SERVER_ID 衝突時自動 XOR 避開）。

# 3. Client（下水端）

- 持續讀 GPS（UART 9600），每秒送一筆位置封包。
- 速度/加速度：由 GPS 速度計算，加速度做指數平滑（0.8 舊 + 0.2 新）。
- 遙測：每 30 秒送一次電量 + 溫濕度（BME280，沒感測器則回報 N/A）。
- 電量讀取：透過 AXP2101 PMU，每 5 秒更新。
- **ATPC 自動發射功率控制**（每 15 秒評估）：
  - 收不到 ACK 超過 20 秒 → 功率 +1。
  - 鏈路強（RSSI > −70 dBm 且 SNR > 8）→ 功率 −1（省電）。
  - 鏈路弱（RSSI < −98 dBm 或 SNR < 2）→ 功率 +1。
  - 結果存 NVS，下次開機沿用。
- ACK 追蹤：記錄最後 ACK 序號/時間、RSSI/SNR、漏 ACK 次數。
- OLED 平時關閉省電；**短按 PWR 鍵**喚醒螢幕顯示狀態 10 秒後自動關閉（非阻塞，不影響定位/發送）。
- **長按 PWR 鍵** → 顯示關機畫面後由 PMU 斷電。

# 4. Server（岸上攝影站）

- 接收並驗證封包（magic/version/networkId/dstId/payloadLen/白名單）；不符靜默丟棄並計數。
- 收到 DATA → 即時回 ACK、更新 RSSI/SNR 滾動統計（近 20 筆）、封包率（60 秒平均）。
- 自己也讀 GPS，計算「攝影站 → Surfer」方位角（bearing）供雲台追蹤。
- 軌跡緩衝：每 1 秒記一點，循環 buffer 共 300 點（過去 5 分鐘）。
- **白名單**：最多 16 筆，開機從 NVS 載入（無資料用 `DEFAULT_WHITELIST`），可經 API 動態 add/remove/clear 並寫回 NVS。
- **磁力計航向**：板上 QMC6310（I2C bus0）每 200 ms 讀一次 heading，被用來補償攝影站身體旋轉；離線時 heading 視為 0（假設站體固定）。
- **雲台追蹤狀態機**：`manual`（手動拖 slider 對準 surfer）→ `tracking`（按 start）→ `paused`（可 pause/resume）。
  - Servo（IO21，內建 LEDC 50 Hz PWM，500–2500 µs = 0–180°）。
  - start 時鎖定 `mount_offset = bearing − heading − servo_angle`（寫 NVS），之後 `servo_angle = bearing − heading − mount_offset`（clamp 0–180°）。
- 長按 PWR 鍵 → 關機畫面後斷電。

# 5. WiFi / Web 監控（Server）

- STA 模式連手機熱點（帳密在 `wifi_config.h`），逾時 20 秒；連不上仍跑 LoRa，背景自動重連。
- 內嵌單頁 Web UI（Servo 控制 + Canvas 極座標雷達圖，無外部 CDN），前端每 1 秒輪詢 `/api/track`、每 3 秒輪詢 `/api/status`。
- API endpoint 與 JSON 欄位詳見 [interface.md](interface.md)。
- Web UI 頂部狀態列：Link / RX 秒數 / GPS 衛星 / 羅盤 heading / RSSI / Batt / Uptime。
- 雷達圖：以攝影站為中心的 east/north 座標，自動縮放、畫出 surfer 過去 5 分鐘軌跡與 servo 當前/目標指向。

# 6. OLED 顯示內容

SH1106 128×64。以下為模擬畫面（`<...>` 為動態數值）。

## Client 開機畫面（亮 10 秒後關閉省電）

開機時自動顯示一次；之後可隨時**短按 PWR 鍵**再亮 10 秒（內容相同，數值即時刷新）。

```
+--------------------+
| SHORE SPOTTER      |
|--------------------|
| ID: <MAC末2碼>      |
| Batt: <%>%      ⚡ |   ⚡ = 接著 USB/充電中
| Temp: <x.x>C       |   無感測器 -> Temp: N/A
| Hum:  <x>%         |   無感測器 -> Hum:  N/A
+--------------------+
```

## Server 開機畫面

```
  開機中                WiFi 結果（顯示 2 秒）
+--------------------+   +--------------------+
| SHORE SPOTTER      |   | SHORE SPOTTER      |
| SERVER booting...  |   |--------------------|
|                    |   | WiFi connected     |   失敗 -> WiFi FAILED
|                    |   | <手機分配的 IP>     |   失敗 -> see wifi_config.h
+--------------------+   +--------------------+
```

## Server 運行畫面（每5秒刷新並輪下個白名單資訊）

左半 = Server 面板\
右半 = Client 面板

```
+------------------+------------------+
|      Server    | V |  Client-n/N    |
|----------------+---+----------------|
|   26.9C / 41%  |T/H|  26.9C / 41%   |
|      Good      |GPS|    OK          |
|    ⚡ 87%      |BAT|    72%         |
|                |   |  LoRa: Normal  |
|             {wifi_status}           |
+------------------+------------------+
```

+ `Client-n/N` 中 n 代表白名單編號（1 起算）、N 代表白名單總數；無白名單時顯示 `Client -/-`。
+ `LoRa` & `GPS` 狀態為四級：`Good` / `OK` / `Bad` / `Miss`（`Miss` = 沒訊號/沒連線/無衛星）。
  + GPS：Good = fix 且 HDOP≤2 且 sats≥6；OK = fix 且 HDOP≤5 且 sats≥4；Bad = 有收到衛星但無可用定位；Miss = 沒衛星且無定位。
  + LoRa：依最近一筆 RSSI/SNR 分 Good/OK/Bad；連線中斷（未收到封包）= Miss。
+ 溫濕度無資料顯示 `--.-C/--%`
+ `BAT` 顯示電量百分比（左=Server 自身 18650、右=選定 Client 遙測），無資料顯示 `--%`；Server 接著 USB/充電時左側顯示 ⚡。
+ wifi_status
  + 連上熱點,顯示IP
  + 還沒連上熱點顯示 `reconnecting to <SSID> in <n>s`
  + 正在嘗試顯示 `WiFi connecting: <SSID> ...`


## 關機畫面（兩種角色共用）

```
+--------------------+
|                    |
|     SHUTDOWN...    |   1.5 秒後 PMU 斷電
|                    |
+--------------------+
```

# 7. 設定持久化（NVS）

- Server（namespace `shorespotter`）：白名單 `wl`、雲台安裝偏移 `mountoff`、校正旗標 `mountcal`。
- Client（namespace `shorespt_client`）：發射功率 `txpwr`、ATPC 開關 `atpc`。

# 8. 健康 / 告警門檻

- 電量百分比：單顆鋰電 **3.2V = 0%、4.2V = 100%**（線性），OLED 與網頁皆以 % 顯示。
- 低電量自動關機（Server / Client 皆有）：偵測到 **< 3.2V** 時顯示「LOW BATTERY」後由 PMU 斷電；**開機**時若已過低則直接顯示後關機（無法開機）。為避免誤動作，**接著 USB（VBUS 在）時不關機**、且讀值 < 2.5V（視為無/異常電池）時忽略；執行中需連續兩次低讀值才關機。
- 充電指示：偵測到外部電源（USB/Type-C）時顯示 ⚡（OLED 與網頁「站」電量）。
- 溫度警告 50.0°C、濕度警告 90%。
- 健康監控：uptime、可用 heap、最小 heap、reset 原因、RX 錯誤計數。

# 9. 電源架構與電量量測

```
2S 鋰電 ──┬─ UBEC 6V ─────────→ Servo
          └─ Type-C 變壓 ──→ Server USB-C（PMU 視為 VBUS）
Server：USB-C(來自2S) + 板載 18650（備援，像手機插著電使用）
Client：板載 18650
```

- **板上 AXP2101 只量得到 18650（單 cell）**；那顆 2S 對 Server 而言只是接在 USB-C 的電源（PMU 看到 VBUS），**無法讀其電量**。
- 因此網頁/OLED 的「站」電量是 18650（備援）：Type-C 正常時會接近滿並顯示 ⚡；**Type-C 失效時改吃 18650**，過低即自動關機（保險）。
- 「接著 Type-C 仍回報 18650 電壓」是正常現象（`getBattVoltage()` 永遠讀 cell）。
