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
9. **電源架構與電量量測** — 2S / UBEC / 18650 的關係
10. **現場提醒（Alerts）** — 監控頁最上方的訊息列
11. **失效回復** — 無線電重建

封包/HTTP 欄位細節見 [interface.md](interface.md)；硬體腳位見 [hardware.md](hardware.md)。

---

# 1. 整體架構

- 單一 `src/main.cpp`，用 build flag 分兩種角色：`ROLE_CLIENT`（下水端）、`ROLE_SERVER`（岸上攝影站）。
- 共用 LoRa 封包協定抽在 [../include/protocol.h](../include/protocol.h)；硬體腳位與 RF 參數在 `main.cpp` 上方常數區；手機熱點帳密在 [../include/wifi_config.h](../include/wifi_config.h)。
- 角色由編譯期 `env:tbeam-client` / `env:tbeam-server`決定，未選或同時選兩個角色都會編譯失敗。

# 2. LoRa 無線通訊

- RF 參數、封包格式、msgType、欄位定義詳見 [interface.md](interface.md)。
- 傳送節奏：位置封包每 1 秒、遙測每 30 秒；Server 每收到 4 包 DATA 回一次 ACK。
- 兩層群組隔離：PHY 層 Sync Word + 應用層 `NETWORK_ID`，允許同頻段多群組共存。
- 節點 ID：開機由晶片 MAC 末 2 bytes 衍生（與 SERVER_ID 衝突時自動 XOR 避開）。
- **時槽配置**：SF9/BW125/CR4-5 下，位置封包空中時間 246 ms、ACK 185 ms，
  ACK 每 4 包回一次 → 平均通道佔用約 292 ms/s（29%）。因此：
  - Client 兩端收送皆為**非阻塞**（DIO1 中斷 + flag），loop 不會停等封包。
  - ACK 每 4 包回一次（依 client `seq` 挑選，掉包不會讓排程滑掉）；Client 的斷線
    與 ATPC 門檻由 `ACK_EVERY_N` 推導，改 N 不必手動改門檻。
  - 遙測（30 s）不能單純「到期就送」——30 s 是 1 s 的整數倍，會固定壓在位置封包
    與其 ACK 上，兩邊同時發射導致雙雙遺失。遙測改為只在位置週期的**靜默時槽**才起送，
    時槽上下界開機時用 `radio.getTimeOnAir()` 推導（見 interface.md），不是寫死常數。
  - **改 SF / BW / 頻率 / Sync Word 必須兩塊板一起燒**；**CR 則不必**——
    explicit header 會帶著 payload 的 CR，接收端自動解，混用 CR 可正常通訊。

# 3. Client（下水端）

- 持續讀 GPS（UART 9600），每秒送一筆位置封包。
- **`fix` 是「現在有沒有定位」，不是「曾經定位過」**：TinyGPSPlus 的 `isValid()`
  一旦為真就永遠為真（失去定位後 `lat/lon/speed/course` 全部凍結在最後一筆，只有
  `age()` 會漲），所以韌體另外檢查 `age() < GPS_FIX_MAX_AGE_MS`（3 秒，約 3 筆 NMEA）。
  沒有這道檢查，天線入水或走進死角時 Server 會拿著一個凍結的舊座標繼續自信地追蹤，
  而且不會有任何徵兆。同一個判定也用在 Server 自己的 GPS 上。
- 速度/加速度：由 GPS 速度計算，加速度做指數平滑（0.8 舊 + 0.2 新）。
- 遙測：每 30 秒送一次電量 + 溫濕度（BME280，沒感測器則回報 N/A）。
- 電量讀取：透過 AXP2101 PMU，每 5 秒更新。
- **ATPC 自動發射功率控制**（每 15 秒評估）：
  - 收不到 ACK 超過 20 秒 → 功率 +1。（此門檻是 `5 × ACK_EVERY_N × 發送間隔` 推導值，
    ACK 間隔改了會自動跟著調整，不必手動改。）
  - 鏈路強（RSSI > −70 dBm 且 SNR > 8）→ 功率 −1（省電）。
  - 鏈路弱（RSSI < −98 dBm 或 SNR < 2）→ 功率 +1。
  - 結果存 NVS，下次開機沿用。
- ACK 追蹤：記錄最後 ACK 序號/時間、RSSI/SNR、漏 ACK 次數。
- OLED 平時進入睡眠省電；**短按 PWR 鍵**喚醒螢幕顯示狀態 10 秒後自動睡回去（非阻塞，不影響定位/發送）。
  - 睡眠是 `u8g2.setPowerSave(1)`（關顯示與升壓電路），不是只清畫面 —— 只清畫面的話
    像素熄了但控制器還在跑，是毫安等級的常態消耗。
  - **電源軌 ALDO1 刻意維持供電**：同一條軌供電給 I2C bus-0 上的 BME280，而下水端每
    5 秒要讀一次溫濕度（也是進水提醒的資料來源），關掉會連感測器一起失去。
- **長按 PWR 鍵** → 顯示關機畫面後由 PMU 斷電。

# 4. Server（岸上攝影站）

- 接收並驗證封包（magic/version/networkId/dstId/payloadLen/白名單）；不符靜默丟棄並計數。
- 收到 DATA → 更新 RSSI/SNR 滾動統計（近 20 筆）、封包率（60 秒平均）；`seq % 4 == 0` 時回 ACK。
- 自己也讀 GPS，計算「攝影站 → Surfer」方位角（bearing）供雲台追蹤。
- 軌跡緩衝：每 1 秒記一點，循環 buffer 共 300 點（過去 5 分鐘）。
- **白名單**：最多 16 筆，開機從 NVS 載入（無資料用 `DEFAULT_WHITELIST`），可經 API 動態 add/remove/clear 並寫回 NVS。
- **磁力計航向**：板上 QMC6310（I2C bus0）每 200 ms 讀一次 heading，被用來補償攝影站身體旋轉；離線時 heading 視為 0（假設站體固定）。
- **雲台追蹤狀態機**：`manual`（手動拖 slider 對準 surfer）→ `tracking`（按 start）→ `paused`（可 pause/resume）。
  - Servo（IO21，內建 LEDC 333 Hz / 14-bit PWM，500–2500 µs = 0–180°）。
  - 從上方看，Servo 角度增加為逆時針、羅盤 bearing 增加為順時針；start 時鎖定 `mount_offset = bearing − heading + servo_angle`（寫 NVS），之後 `servo_angle = heading + mount_offset − bearing`（clamp 0–180°）。
  - **追蹤迴圈為 20 Hz（每 50 ms），與封包到達脫鉤**：不是「收到封包才動一次」，
    因此掉一包也不會整整一秒不動。
  - **位置外推（dead reckoning）**：封包 1 Hz，拿到時已經過期最多 1 秒。改用
    payload 既有的 `speed_cms` / `course_deg10` 沿速度向量外推目前位置（等速模型，
    不含加速度——`accel_cms2` 是 GPS 速度的高度平滑微分，平方項會在起乘瞬間嚴重過衝）。
    速度低於 0.3 m/s 不外推（此時 GPS 航向是雜訊）；外推最多 2 秒，超過就凍結，
    避免斷線後鏡頭一路飄走。
  - **轉速限制 120°/s**：單筆 GPS 跳點或從遠處 resume 時，不會用 servo 全速
    （約 400°/s）甩鏡頭。因此 `servo.angle`（實際）會略微落後 `servo.target`（目標）。
  - 註：servo 瞄準的是**外推後**的位置，而 `/api/track` 的 `client.lat/lon` 與
    `bearing` 仍是**原始收到值**，兩者在高速時可能差幾公尺（雷達圖上約數 %）。
  - **追蹤期間 heading 凍結**：進入追蹤時快照一次 heading 後就固定使用，磁力計的
    即時雜訊（servo 電流、震動、濾波漣漪）不會進入控制迴路。只有當即時值偏離快照
    超過 2°（死區）才視為「腳架真的被轉動」並重新快照——重新快照會把誤差歸零、
    自然落在死區內，等於免費得到遲滯，不會在門檻上抖動。
    GPS 提升到 ~1 m 後磁力計才是指向誤差的主要來源，所以這件事有感。
- 長按 PWR 鍵 → 關機畫面後斷電。
- **OLED 位址依板子版本自動判定**：T-Beam Supreme 有 QMC6310U（磁力計 0x1C → 螢幕 0x3C）
  與 QMC6310N（磁力計 0x3C → 螢幕 **0x3D**）兩種版本。`initMag()` 先探測 0x1C，探到哪個
  位址就回推螢幕位址。寫死 0x3C 在 N 版板子上會讓 u8g2 把畫面資料寫進磁力計的暫存器 ——
  螢幕全黑而且沒有任何錯誤訊息。開機 log 會印出實際使用的兩個位址。

## 4.1 校正（各做一次即可，一個人就能完成）

兩個校正互相獨立，但要一起做才有意義：

**磁力計 hard-iron**（`POST /api/mag/calibrate`，網頁資訊頁有按鈕）\
把整台機器**順時針**慢慢水平轉一整圈，韌體對水平面兩軸做最小平方圓擬合，圓心存 NVS。
不用對準任何東西、不需要 GPS fix。校正值跟著板子走，換浪點不用重做。
回報**擬合殘差**（<1° = 安裝乾淨）與**磁場強度**（台灣應接近 0.37 G）當品質指標。

這一圈同時判斷**板子怎麼擺**：三軸中轉一圈幾乎不變的那一軸就是垂直軸，另兩軸即水平面
（結果顯示在「水平面軸」）。所以板子平放、立起、側立都可以，不必再遷就水平放置；
代價是**換擺法後必須重做校正**，而且**旋轉方向決定 heading 的正負**（轉反了 heading 會
反向，log 會用磁傾角交叉檢查並警告）。

**地標校正**（`POST /api/track/calibrate?lat&lon`）\
servo 設 90°，從觀景窗把 1 km 外的地標對到畫面正中央，貼上座標。400mm 下是 ±0.05°
的照準。不需要追蹤器在場、不需要第二個人。

這是**唯一**鎖定 `mount_offset` 的方式。曾經做過的「朝向法」「方位角法」（操作者手動
輸入方位）已移除：手工對方位是 2~5° 誤差，還多一個磁北／真北搞錯就靜靜偏 4~5° 的風險，
只為了省 30 秒照準。副作用是校正**需要 GPS fix 與看得見的地標**，室內做不了。

精度 ~0.1°（1 km 地標 + 觀景窗照準），而且整個流程不涉及羅盤與磁偏角。
鎖定時 heading 取**最近 5 秒的向量平均**（25 筆 @ 5 Hz），避免把某一瞬間的雜訊寫成永久
常數——但平均只對零均值雜訊有效，hard-iron、servo 鋼齒輪、傾斜這些是確定性誤差，
平均多久都不會消失。「靜置一段時間」也不能取代這一步：靜置只降雜訊，不會提供任何
「鏡頭指向哪裡」的資訊，而那正是 `mount_offset` 需要的。

**選跟浪區同方向的地標。** 磁力計軌跡的橢圓誤差是方位的 sin2θ 函數，`mount_offset` 只
吸收了校正姿勢那一點，洩漏到指向的是兩個姿勢的差：上限 `2 × ellipse_deg × |sin(Δθ)|`。
Δθ=0 時完全抵銷，90° 時最壞。`/api/track` 的 `pose_delta_deg` / `pose_err_deg` 會即時
報這個值。

> **為什麼這樣就不用每次校正**：`mount_offset` 本來就存在 NVS 且跨重開機有效
> （`/api/track/resume` 用的就是舊值）。它之所以看起來每次都得重做，是因為未校正的
> heading 是被扭曲的——換個方位架設誤差就跑掉幾十度。修好 hard-iron，`mount_offset`
> 才真的是常數，之後每次下水只要 resume。

# 5. WiFi / Web 監控（Server）

- STA 模式連手機熱點（帳密在 `wifi_config.h`），逾時 20 秒；連不上仍跑 LoRa，背景自動重連。
- 內嵌單頁 Web UI（Servo 控制 + Canvas 極座標雷達圖，無外部 CDN），前端每 1 秒輪詢 `/api/track`、每 3 秒輪詢 `/api/status`。
- Server 支援 PlatformIO `espota` Wi-Fi 韌體更新；OTA 開始時暫停追蹤，完成後自動重開。OTA 本身未設密碼，以手機熱點的存取控制作為網路邊界。
- API endpoint 與 JSON 欄位詳見 [interface.md](interface.md)。
- **執行紀錄**：所有 log 同時輸出到 USB 序列埠與一個 4 KB 環形緩衝，`http://<站IP>/log`
  可即時檢視（每 2 秒抓增量，僅在瀏覽器分頁在前景時輪詢）。到了海邊不必接筆電就能除錯；
  espota 本身只上傳韌體，不提供 log。紀錄是**獨立網址**而非 Web UI 的分頁——由「資訊」
  頁的按鈕另開分頁，看 log 就不必放掉雷達畫面。
- **RX 每分鐘一筆統計**：收包不再一包一行。一包一行會在約 16 秒內把 4 KB 環形緩衝洗完，
  等於你打開紀錄想看的 `[SERVO]` / `[MAGCAL]` / `[TRACK]` 一定已經被沖掉。改成累積後
  每 60 秒印一行：收到/應收（用 seq 差算掉包率）、ACK、drop/err（含最後的錯誤碼）、
  RSSI 與 SNR 的 min/avg/max、雙方 GPS、距離/方位、heading、servo 角度與模式。
  這樣同一個 4 KB 大約能留 30 分鐘以上的歷史。
- IP 快取在 DHCP 換位址時會更新（比對 `IPAddress` 而非只在空值時寫入），
  避免 WiFi 未斷線但續約換 IP 時 OLED 一直顯示舊位址。
- Web UI 頂部狀態列：Link / RX 秒數 / GPS 衛星 / 羅盤 heading / RSSI / Batt / Uptime。
- **GPS 品質用操作者看得懂的說法**：Web UI 不再顯示 HDOP 與衛星數字。HDOP 換算成
  **預期精度（公尺）**＝`HDOP × 2.5 m`（2.5 m 是 ATGM336H 這類單頻消費級模組常用的
  1σ UERE），衛星數改成 **少／普通／好** 三段（≥8 好、≥6 普通、其餘少，門檻與
  Good/OK/Bad 判定一致）。封包欄位仍是 HDOP 與衛星數（`protocol.h` 沒動），只有顯示改寫。
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
+ `LoRa` & `GPS` 狀態為四級：`Good` / `OK` / `Bad` / `Miss`（`Miss` = 沒訊號/沒連線/無衛星）\
  GPS
  + Good = fix 且 HDOP≤1.5 且 sats≥8
  + OK = fix 且 HDOP≤3 且 sats≥6
  + Bad = 有收到衛星但無可用定位
  + Miss = 沒衛星且無定位。

  LoRa：依最近一筆 RSSI/SNR 分 Good/OK/Bad/Miss。
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

- Server（namespace `shorespotter`）：白名單 `wl`、雲台安裝偏移 `mountoff`、校正旗標 `mountcal`、
  磁力計水平面軸 `magaxa`/`magaxb`、hard-iron 偏移 `magoa`/`magob`、旗標 `magcal`、
  品質 `magres`/`magfld`、版本 `magver`（現為 2；舊的 `magx`/`magy` 為 v1，升版後自動失效）。
- Client（namespace `shorespt_client`）：發射功率 `txpwr`、ATPC 開關 `atpc`。

# 8. 健康 / 告警門檻

- 電量百分比：單顆鋰電 **3.2V = 0%、4.15V = 100%**（線性），OLED 與網頁皆以 % 顯示。
- 低電量自動關機（Server / Client 皆有）：偵測到 **< 3.2V** 時顯示「LOW BATTERY」後由 PMU 斷電；**開機**時若已過低則直接顯示後關機（無法開機）。為避免誤動作，**接著 USB（VBUS 在）時不關機**、且讀值 < 2.5V（視為無/異常電池）時忽略；執行中需連續兩次低讀值才關機。
- 充電指示：偵測到外部電源（USB/Type-C）時顯示 ⚡（OLED 與網頁「站」電量）。
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

# 10. 現場提醒（Alerts）

監控頁最上方那條訊息列。讀它的人站在沙灘上、多半不是工程師、手上可能還拿著相機，
所以每一則都寫成「發生什麼事 + 現在該做什麼」，不出現 HDOP、dBm、mV 這類名詞。

**判斷全部在韌體端**（`main.cpp` 的 `appendAlertsJson()`，門檻在
[../include/alerts.h](../include/alerts.h)），監控頁只負責畫。這樣之後 OLED 要顯示同一組
提醒時，不必再把規則實作第二次 —— 兩份規則遲早會走岔，而走岔的那天不會有人發現。

- 隨 `GET /api/status` 一起回（3 秒一次），不另開端點：ESP32 的 WebServer 一次只服務
  一個連線，多一個輪詢端點就是多一份延遲。
- 陣列已依嚴重度排序，`error` 在前；沒有異常時是空陣列，訊息列整條隱藏。
- 完整的提醒清單與觸發條件見 [interface.md 的 alerts 一節](interface.md#alerts--現場提醒)。

**進水偵測為什麼不只看絕對濕度**：海邊空氣本來就 80% 起跳，封盒時關進潮濕空氣是常態，
只用絕對門檻會整天誤報 —— 而一個整天誤報的警告等於沒有警告。進水真正的特徵是「相對
開機值單調上升」，所以兩條規則並用：

| 規則 | 判定 |
|---|---|
| 濕度 ≥ 90% | error，幾乎確定進水 |
| 濕度 ≥ 80% **且**比開機第一筆高 ≥ 15 個百分點 | warn，可能正在滲水 |

基準值是 Server 端記的：下水端的基準來自收到的第一筆遙測，站體的基準來自開機後第一筆
BME280 讀數。

# 11. 失效回復

- **無線電重建**：SX1262 若因 SPI 干擾或狀態機卡住而停止工作，原本兩端都只會印一行
  log 然後安靜地永遠壞下去 —— 下水端平時螢幕是關的，完全沒有外部徵兆。現在：
  - Client 連續 `RADIO_TX_FAIL_LIMIT`（5）次送不出去 → 重新 `initRadio()`，
    並把 ATPC 收斂到的發射功率與 DIO1 中斷重新掛回去。
  - Server 連續 `RADIO_RX_ERR_LIMIT`（30）次讀取錯誤 → 同樣重建。門檻比 Client 高很多，
    因為 CRC 錯誤在距離極限本來就會出現；這裡要抓的是「卡住之後每次都回同一個錯」。
  - 兩次重建之間至少間隔 `RADIO_RECOVER_MIN_MS`（30 秒），避免重建失敗時變成每個 loop
    都重建一次。
- **GPS 過期判定**：見第 3 節的 `GPS_FIX_MAX_AGE_MS`。
- **WiFi**：斷線後每 5 秒自動重連，LoRa 追蹤不受影響（本來就不依賴 WiFi）。
