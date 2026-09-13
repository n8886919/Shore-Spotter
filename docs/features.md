# 韌體功能說明（Features）

目前原始碼版本 **0.6-dev（未發行）**，唯一來源 `include/firmware_version.h`。Client／Server 的開機
OLED 與序列紀錄皆顯示版本；Server 網頁資訊頁及 `health.firmware_version` 顯示
實際運行版本。LoRa 沒有傳送 Client 韌體版本，不能從 Server 版本推定 Client 已更新。
本文件描述原始碼行為；建置、部署與現場驗證狀態另見 [CHANGELOG](../CHANGELOG.md)。

本文件描述 Shore Spotter 韌體的「行為與流程」，分為以下面向：

1. **整體架構** — 角色切分與 build flag
2. **LoRa 無線通訊** — v4 封包、傳送節奏與單一 Client 配對
3. **Client（下水端）** — 定位／遙測／診斷發送與 ATPC
4. **Server（岸上攝影站）** — 接收、方位計算、軌跡、單一 Client 綁定
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

- `src/main.cpp` 用 build flag 分兩種角色：`ROLE_CLIENT`（下水端）、`ROLE_SERVER`（岸上攝影站）；UART 控制端點位於 `src/uart_servo_mode.cpp`。
- 共用 LoRa 封包協定抽在 [../include/protocol.h](../include/protocol.h)；硬體腳位與 RF 參數在 `main.cpp` 上方常數區；手機熱點帳密在 [../include/wifi_config.h](../include/wifi_config.h)。
- 角色由編譯期 `env:tbeam-client` / `env:tbeam-server`決定，未選或同時選兩個角色都會編譯失敗。

# 2. LoRa 無線通訊

- RF 參數、封包格式、msgType、欄位定義詳見 [interface.md](interface.md)。
- **LoRa 協定 v4**：共用標頭 6 bytes，包含識別碼、合併的版本／類型、16-bit `clientId`
  與 16-bit 序號。上下行只使用同一個 `clientId`；不傳群組、獨立來源／目的 ID、
  `payloadLen` 或空白驗證尾碼。保留 RF CRC，依類型嚴格檢查長度與欄位；不接受 v3 封包。
- Client ID 由晶片 MAC 末 2 bytes 衍生，遇 0、廣播或舊保留 ID 時重新映射。
  16-bit ID 並非全球唯一，兩套若撞號仍需處理；不同 ID／白名單不會消除同頻碰撞。

| 類型 | 大小 | 發送節奏與用途 |
|---|---:|---|
| DATA | **17 bytes** | 每 500 ms 排程一次；座標、速度／方向、衛星級距、有效旗標、HDOP、來源年齡 |
| ACK | **11 bytes** | DATA `seq % 8 == 0` 時回覆，名義上每 4 秒一次；確認序號、RSSI／SNR |
| TELEMETRY（TEL） | **11 bytes** | 約每 30 秒一次；電池、溫濕度、精確衛星數 |
| DIAGNOSTIC（DIAG） | **17 bytes** | 約每 30 秒一次；Client GNSS 間隔、UART 積壓、NMEA／TX 錯誤、跳過排程與狀態 |

- **2 Hz 是 RF 發送排程，不代表每秒兩筆新 GNSS 定位**。韌體維持 GPS UART 9600，
  沒有強制模組改成 2 Hz；同一筆位置再次送出時，來源年齡會增加，不會被重新當作新定位。
- DATA、TEL、DIAG 各自使用序號；TEL／DIAG 不消耗 DATA 序號或 ACK 位置。
  Client 只接受自己 ID、預期 DATA 序號、仍在回覆期限內且尚未消耗的 ACK。
- **非阻塞發射／接收**：Client DATA／TEL／DIAG 發射與 Server ACK 均由 DIO1
  TxDone／timeout 推進，完成後恢復 RX；等待 RF 發送期間繼續服務 GNSS 與控制。
- **時槽配置**：空中時間由 `radio.getTimeOnAir()` 計算；目前 SF9／BW125／CR4/5
  的 DATA／DIAG 約 165 ms、ACK／TEL 約 145 ms。TEL／DIAG 只在**不回 ACK**的
  DATA 週期、DATA 已成功完成後起送，前後保留 80 ms，必須能在下一個 DATA deadline 前完成。
  兩者同時到期時先送 TEL，DIAG 延後；沒有時槽就延後，不硬塞。
- DATA 排程逾時只處理當前一格，跳過錯失時槽並計數，不連續補發。
  若當前週期容不下 DATA、必要的 ACK 與保護間隔，也直接跳過。
  SF／BW／頻率／Sync Word 改動須同步兩端；調整 RF 參數後需重新檢查時槽預算。
  這是單套的排程協調，沒有多套系統之間的時槽同步或自動換頻。

# 3. Client（下水端）

- 持續讀取 GPS UART 9600；[GNSS 快照收集器](../include/gnss_snapshot.h) 依 NMEA UTC
  epoch 對齊 RMC／GGA，驗證 checksum、定位狀態與數值。位置、品質與速度不借用其他 epoch
  的欄位；只有新 GGA 時可回報位置／品質，但不沿用舊 RMC 的速度。
  等待新 RMC 對應的 GGA 期間，可保留上一筆完整快照及其**原始年齡**；明確無效定位優先撤銷。
- **`fix` 代表現在可用的定位**。兩端皆以 coherent 快照、來源年齡與額外 200 ms
  UART 服務不確定量檢查 `< 2000 ms`；不再以 TinyGPSPlus 曾經有效的旗標判斷。
  若超過 200 ms 未服務 GPS，先丟棄排隊 bytes 並使快照失效，等待更新的 epoch；
  UART 緩衝設為 1024 bytes，不能把排隊的舊 NMEA 當新定位。
- 來源年齡以 NMEA 到達時間回推 UART 序列傳輸時間作為基準，再依 UTC epoch 差值對齊；
  不是 GNSS 與 MCU 時鐘同步，也未量測模組內部定位延遲。DATA 年齡以 10 ms 向上取整，
  未知／超範圍以不可用值編碼；200 ms 不確定量另用於過期判定，不灌進外推位移。
- 座標使用固定原點 24°N／121°E 的兩個 signed 24-bit 差值，共 6 bytes、約 0.1 m
  編碼解析度；**每包獨立解碼**，不是相對前一包。超出協定座標範圍就撤銷 fix。
- 保留同 epoch 的速度／方向與獨立 `velocity_valid`；速度以 0.1 m/s 編碼，低於 0.3 m/s、
  缺欄位或超出可表示範圍時不外推。加速度計算與 `accel_cms2` 欄位已移除。
- DATA 傳衛星級距：未知、≤5、6–7、≥8；保留 HDOP（向上取整至 0.1）。
  精確衛星數改放約每 30 秒的 TEL，僅供診斷；追蹤資格不能拿舊 TEL 數值代替 DATA 級距。
- TEL 另傳電量、溫濕度（BME280，沒感測器則回報未知）；DIAG 記錄 Client 真正量到的
  GNSS epoch 間隔與錯誤／積壓／跳格計數。計數自 Client 開機累計、16-bit 飽和至 65535；
  DIAG 是低頻快照，不是逐筆即時紀錄。
- 電量讀取：透過 AXP2101 PMU，每 5 秒更新。
- **ATPC 自動發射功率控制**（每 15 秒評估）：
  - 收不到 ACK 超過 20 秒 → 功率 +1。（此門檻是 `5 × ACK_EVERY_N × 發送間隔` 推導值，
    ACK 間隔改了會自動跟著調整，不必手動改。）
  - 鏈路強（RSSI > −70 dBm 且 SNR > 8）→ 功率 −1（省電）。
  - 鏈路弱（RSSI < −98 dBm 或 SNR < 2）→ 功率 +1。
  - 結果存 NVS，下次開機沿用。
- ACK 追蹤：預期序號與回覆期限由 DATA 排程建立；記錄 ACK 接收／拒絕次數、最後接收時間與 RSSI／SNR。
- OLED 平時進入睡眠省電；**短按 PWR 鍵**喚醒螢幕顯示狀態 10 秒後自動睡回去（非阻塞，不影響定位/發送）。
  - 睡眠是 `u8g2.setPowerSave(1)`（關顯示與升壓電路），不是只清畫面 —— 只清畫面的話
    像素熄了但控制器還在跑，是毫安等級的常態消耗。
  - **電源軌 ALDO1 刻意維持供電**：同一條軌供電給 I2C bus-0 上的 BME280，而下水端每
    5 秒要讀一次溫濕度（也是進水提醒的資料來源），關掉會連感測器一起失去。
- **長按 PWR 鍵** → 顯示關機畫面後由 PMU 斷電。

# 4. Server（岸上攝影站）

- 接收時保留實際 RF 長度，驗證 magic、v4、類型、固定長度、`clientId` 白名單與數值／保留位元。
  格式、長度、綁定、序號與 RF 錯誤分別計數，並寫入 64 筆結構化事件環形紀錄。
- 收到 DATA → 更新 RSSI/SNR 滾動統計（近 20 筆）、**約 5 秒視窗的 DATA 封包率**；
  距最後 DATA 超過 5 秒，`pkt_rate` 回報 0。此值是 RF 接收率，不是新 GNSS 定位率。
  DATA `seq % 8 == 0` 時回 ACK。
  ACK 非阻塞，TxDone／timeout 才恢復 RX，空中傳送時持續服務 Servo／UART。
  自 RxDone 超過 50 ms 才處理的 ACK 直接略過；失敗／逾時會計數且嘗試恢復 RX。
- 自己也讀 GPS，計算「攝影站 → Surfer」方位角（bearing）供雲台追蹤。
- Server 不保存軌跡；瀏覽器每秒嘗試累積有效且不同的位置，加入新點時移除兩小時前
  的資料供 GPX 匯出，雷達／地圖只畫最近 5 分鐘。重整頁面會清空瀏覽器軌跡。
- DATA 拒絕重複／倒退序號，支援 uint16 回繞；超過 2.5 秒未接受 DATA 可重同步。
  `sequence_gaps` 只由 DATA 的前進序號推估缺包，TEL／DIAG 不影響此計數；重同步另計。
- **單一 Client**：只接受綁定 ID 的 DATA／TEL／DIAG，NVS 保存 `gpsclient`。
  新裝預設 **0 = 未綁定**，不接受任何 Client 定位；有效舊綁定會保留。
  舊 `wl` 只遷移第一個有效 ID；刻意清空不恢復預設。API 的 add 不覆寫不同 client，
  set 可明確替換；成功變更時撤銷自動控制並清空舊 client 的資料、濕度基準與滾動統計。
  v4 線上封包只帶一個 `clientId`，上行表示發送者，下行表示 ACK 對象；現階段容量固定為 1。
- **雲台控制**：`manual / gps / uart / paused`。開機 Servo 90°、預設 UART 等待新指令。
  GPS／UART 互斥；只有 UART 模式開啟 UART 接收控制。切換清除舊目標與 UART session。
  GPS 按鈕與 API 要求兩端新鮮 Good／OK（衛星 ≥6、HDOP ≤3；Client 使用即時衛星級距）；追蹤另需校正與
  連續可用 2 秒，失效保持，不自動切換來源。
  UART 接受 SET／SET2，250 ms 無有效指令保持；新 SET 可恢復，不要求 GPS／校正。
  Manual／pause／OTA／關機撤銷自動控制；start／resume 不修改校正。
  舊 HTTP `mode=jetson` 接受為 UART 別名，回應統一為 `uart`。
  - Servo（IO21，內建 LEDC 333 Hz / 14-bit PWM，500–2500 µs = 0–180°）。
  - 從上方看，Servo 角度增加為逆時針、羅盤 bearing 增加為順時針；指南針校正時鎖定 `mount_offset = compass_bearing + servo_angle`（只存 RAM），之後 `servo_angle = mount_offset + declination − true_bearing`（clamp 0–180°）。
  - **GPS 目標重算為 20 Hz（每 50 ms），與封包到達脫鉤**：α 開啟時，可在有效封包之間外推。
  - **位置外推（dead reckoning）**：DATA 排程每 500 ms，使用同一快照且有效的速度／方向
    做等速預測，不含加速度；API 將 wire 的 0.1 m/s 速度還原為 `speed_cms`，方向為 `course_deg10`。
    缺少有效速度向量、速度低於 0.3 m/s 或定位過期時不外推。
    外推時間為 **來源年齡＋DATA airtime＋自 RxDone 經過時間**，最多 2 秒，仍須通過新鮮度門檻。
  - **年齡與 freshness**：`source_age_ms` 是 Client 發送前估計，`rx_age_ms` 是自 RF 收到後
    經過的時間；`sample_age_ms` 為兩者再加 airtime 的合計。Client 資料須滿足
    `sample_age_ms + 200 < 2000`，Server 自身 GNSS 也採來源年齡＋200 ms 的 `< 2000` 門檻。
    額外 200 ms 只降低過期判定的容忍度，不增加預測位移；內部 GNSS 延遲仍未量測。
    API `fix`／`velocity_valid` 依目前年齡重新判斷，不會因持續收到同一筆舊座標而永久有效。
    速度／方向數值本身可保留最後值，使用者仍須檢查 `velocity_valid`。
  - **GPS 位置預測 α 開關**：資訊頁勾選為 α=1（預設），取消為 α=0，
    使用最後收到座標；切換自動保存至 NVS，重開機沿用。關閉後 Servo 仍按共用速限追目標，
    不會暫停追蹤；手動／UART 不受此設定影響。這是位置預測開關，不是 PID 或馬達速限比例。
  - **所有模式只共用最高速度**：預設 30°/s，資訊頁可調 1–90°/s，修改自動寫入
    NVS 單一速度值，之後開機沿用；舊多參數設定不帶入。移動中也可改速，不換模式或目標。
    按微秒實際時間限制每次位移，Servo 無固定輸出頻率；GPS 目標另每 50 ms 重算。
    保留小數微秒精度到 14-bit duty，只有數值變化才寫入。加速度／jerk／死區規劃已移除。
    命令範圍固定 0–180°；長停頓最多採計 50 ms，不補送歷史目標。來源失效立即保持。
    API 無機械回饋，首次回 90° 無法保證物理限速；命令移動時禁止指南針校正。
    詳見 [共用控制器](motion-control.md)。
  - 註：α 開啟時 servo 瞄準的是**外推後**的位置，而 `/api/track` 的 `client.lat/lon` 與
    `bearing` 仍是**原始收到值**，兩者在高速時可能差幾公尺（雷達圖上約數 %）。
- 長按 PWR／低電量關機先切手動、撤銷控制，再顯示畫面與要求 PMU 斷電。
- OLED 位址獨立偵測：優先檢查 0x3D，否則使用 0x3C；不再依賴磁力計初始化。
  保留兩種板子版本的螢幕支援。

## 4.1 鏡頭指南針校正

1. 切到手動，固定腳架並等鏡頭停穩。
2. 輸入鏡頭普通磁針指南針讀數：北 0°、東 90°、南 180°、西 270°。
3. 選「GPS」。UART 模式不需要此校正。

`POST /api/track/calibrate?bearing=<0..359.999>` 只保存 RAM 參考，校正本身不需 GPS／地標。
板上磁力計初始化、取樣、旋轉補償、hard-iron 校正頁與 API 已移除。
不再讀寫 mount*／mag* NVS。腳架轉動、重新架設、移動指南針或重開機都需要重校。

磁針輸入為磁北，GPS 與雷達加入 WMM2025 磁偏角（東正西負）；依攝影站 GPS 位置與
UTC 日期自動查表，適用北緯 18–28°、東經 116–124°、2025–2029 年。
GPS 位置／日期無效或超出範圍時，GPS 模式保持，UART 模式不受影響。
磁偏角補償不能消除指南針附近金屬／馬達的局部磁場干擾。

# 5. WiFi / Web 監控（Server）

- STA 模式連手機熱點（帳密在 `wifi_config.h`），逾時 20 秒；連不上仍跑 LoRa，背景自動重連。
- 內嵌單頁 Web UI 分為**雷達／資訊／除錯**，無外部 CDN；前端每 1 秒輪詢 `/api/track`、
  每 3 秒輪詢 `/api/status`。所有 API 共用單一請求排程，含回應 body 讀取；待送控制優先，
  背景同類請求合併，忙碌時輪詢可延後。網頁輪詢頻率不等於 RF 或 GNSS 更新頻率。
- 手動滑桿拖動只預覽，放開才送出；UI 0–180° 對應內部 180–0°。在途請求最多一筆，
  待送只保留最新已放開目標；模式切換清除待送並等待在途完成。pointercancel 不送出。
  10 個案例由 `node tools/test_servo_slider.js` 驗證。
- OLED 運行畫面仍分 8 段傳輸、段間服務控制，固定正常刷新；不提供 OLED 刷新開關。
- Server 支援 PlatformIO `espota` Wi-Fi 韌體更新；OTA 開始時暫停追蹤，完成後自動重開。OTA 本身未設密碼，以手機熱點的存取控制作為網路邊界。
- API endpoint 與 JSON 欄位詳見 [interface.md](interface.md)。
- **除錯與匯出**：除錯頁在前景、或已開始瀏覽器錄製時，最多每秒串行讀取
  `/api/debug` 與 `/api/log`，與全頁其他 API 排程共用；沒有錄製時離開除錯頁便停止這兩項輪詢。
  錄製最長 10 分鐘／1200 筆／5 MiB，任一上限即停止；漏採、Server 重開機、事件／文字覆寫
  與讀取失敗皆有標記。開始／停止錄製只操作瀏覽器，不切換追蹤模式或清除 Server log。
- JSON 匯出包含備註、每次取樣時間與各端點接收時間、版本／RF 設定、track／status、
  GNSS／Client DIAG、封包事件與文字增量。除錯 API／匯出 schema 為 2；每次最多 8 個
  新事件，首次歷史分批補齊，畫面在瀏覽器合併近期 64 筆。游標覆寫／重啟都有提示，
  匯出不停止錄製、不清除現存紀錄。
  手機鎖屏／背景執行可能漏採，重整頁面會遺失；詳見 [除錯匯出](debug-export.md)。
- 除錯頁將 **Server 本機 GNSS**、**Client 約每 30 秒的 DIAG**、**Server 依封包年齡推估的
  Client 更新間隔**分開顯示。DIAG 90 秒內標示仍在有效期但非即時，超過則只供歷史參考；
  原始欄位會隨 JSON 保留。另顯示綁定 ID／未綁定，以及最近因綁定不符拒收的 ID。
- **執行紀錄**：USB 序列埠與 4 KB 環形緩衝保留；`/api/log?from=<offset>` 回傳文字增量，
  `X-Log-Next`／`X-Log-Dropped`／`X-Log-Boot` 提供游標、遺失與開機識別。
  瀏覽器文字畫面最多保留最近 32768 UTF-16 字元（約 64 KiB），截短會提示。
- **RX 每分鐘一筆統計**：收包不逐包印文字，避免快速洗掉 4 KB 環形緩衝中的控制事件；
  累積後每 60 秒印一行：收到位置數／seq 範圍、ACK、drop/err（含最後的錯誤碼）、
  RSSI 與 SNR 的 min/avg/max、雙方 GPS、距離/方位、servo 角度與模式。
  百分比使用收到的 DATA 加上 DATA 序號推估缺包作分母；TEL／DIAG 使用獨立序號，
  不再灌入 DATA 分母。序號缺口仍不能區分 RF 遺失、發射失敗與重啟等原因，需搭配
  Client DIAG、Server 錯誤計數與事件紀錄判讀；API `drop_rate` 是拒收計數比例，並非空中丟包率。
  4 KB 能保留多久取決於各類 log 的頻率，追蹤期間還會產生每 3 秒一筆的追蹤紀錄。
- IP 快取在 DHCP 換位址時會更新（比對 `IPAddress` 而非只在空值時寫入），
  避免 WiFi 未斷線但續約換 IP 時 OLED 一直顯示舊位址。
- 雷達頁顯示模式／追蹤或保持狀態，資訊頁顯示連線、接收年齡、GPS 品質、RSSI／SNR、電量及 uptime。
- **GPS 品質顯示**：一般操作頁將 HDOP 換算成 `HDOP × 2.5 m` 的粗略「預期精度」，
  並非模組實測誤差。Server 衛星顯示少／普通／好；Client 顯示 DATA 的**未知／≤5／6–7／≥8 顆**。
  Client 精確顆數僅由 TEL 低頻更新，API `satellites` 在未知或 TEL 超過 90 秒時為 `null`，
  另提供 `satellites_age_ms`。UI 不將級距下界冒充精確數字，GPS 品質／追蹤門檻也不使用舊 TEL 顆數。
- 雷達圖：以攝影站為中心的 east/north 座標，自動縮放、畫出 surfer 過去 5 分鐘軌跡與 servo 當前/目標指向。
- 地圖使用瀏覽器載入的 OpenStreetMap 圖磚，需要網路；韌體內的操作頁與雷達可本地使用。

# 6. OLED 顯示內容

SH1106 128×64。以下為模擬畫面（`<...>` 為動態數值）。

## Client 開機畫面（亮 10 秒後關閉省電）

開機時自動顯示一次；之後可隨時**短按 PWR 鍵**再亮 10 秒（內容相同，數值即時刷新）。

```
+--------------------+
| SHORE SPOTTER v<version> |
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
| SHORE SPOTTER v<version> |   | SHORE SPOTTER v<version> |
| SERVER booting...  |   |--------------------|
|                    |   | WiFi connected     |   失敗 -> WiFi FAILED
|                    |   | <手機分配的 IP>     |   失敗 -> see wifi_config.h
+--------------------+   +--------------------+
```

## Server 運行畫面（正常排程每 500 ms 刷新單一綁定 Client）

左半 = Server 面板\
右半 = Client 面板

```
+------------------+------------------+
|      Server    | V |  Client XXXX    |
|----------------+---+----------------|
|   26.9C / 41%  |T/H|  26.9C / 41%   |
|      Good      |GPS|    OK          |
|    ⚡ 87%      |BAT|    72%         |
|                |   |  LoRa: Normal  |
|             {wifi_status}           |
+------------------+------------------+
```

+ `Client XXXX` 顯示綁定的十六進位 ID；未綁定時顯示 `Unbound`。
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

- Server（namespace `shorespotter`）：單一 client `gpsclient`（UShort，0 = 未綁定；舊 wl 僅首次遷移）。
- 唯一速限以 `shorespotter/servospd` 單一 uint32 保存（毫度／秒）；資訊頁修改即寫入並回讀確認，
  相同值不重寫，重啟沿用；舊 motioncfg 不讀取。
- GPS 預測開關以 `shorespotter/gpspredict` 保存，0 = α 關、1 = α 開；未設定預設開啟，
  保存失敗不修改執行中的設定。
- 鏡頭校正只存 RAM，板上校正停用。舊 mount*／mag* 鍵忽略，不清除整個 NVS 區域。
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

## 執行時間診斷

`/api/status.timing` 保存控制最大服務間隔，以及 loop、HTTP、BME280、PMU、OLED、LoRa、
OTA 的 last/max 耗時與 ≥50 ms 次數，另有 UART 晚服務／丟棄 bytes／拒絕指令數。
不逐次寫 log。Wire 與 PMUWire 單筆交易 timeout 為 10 ms；整段操作可含多筆交易。
同步 WebServer 仍可能阻塞，這批提供實機定位依據，並不宣稱 250 ms 期限已獲保證。
週期 deadline 使用差值比較處理 millis 溢位，控制在同步 I/O 工作之間被服務。
