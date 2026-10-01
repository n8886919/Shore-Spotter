# 韌體功能說明（Features）

目前原始碼版本 **0.6-dev（未發行）**，唯一來源 `include/firmware_version.h`。Client／Station 的開機
OLED 與序列紀錄皆顯示版本；Station 網頁資訊頁及 `health.firmware_version` 顯示
實際運行版本。LoRa 沒有傳送 Client 韌體版本，不能從 Station 版本推定 Client 已更新。
本文件描述原始碼行為；建置、部署與現場驗證狀態另見 [CHANGELOG](../CHANGELOG.md)。

Station 新增預設關閉的 [Axiom 雲端除錯上傳](axiom-log.md)：透過 `POST /api/axiom` 設定 ingest Token 並啟用後，每秒採樣、約每 5 秒批次上傳。包含定位、LoRa raw events、
GNSS／控制／耗時診斷，設定保留於獨立 NVS。網頁關閉仍持續，忙碌／離線允許丟棄，
不新增 flash log 或更改 Client／控制模式；Token 不經 GET 或除錯匯出讀回。

本文件描述 Shore Spotter 韌體的「行為與流程」，分為以下面向：

1. **整體架構** — 角色切分與 build flag
2. **LoRa 無線通訊** — v5 封包、傳送節奏與單一 Client 配對
3. **Client（下水端）** — 定位／遙測／診斷發送與固定功率
4. **Station（岸上攝影站）** — 接收、方位計算、軌跡、單一 Client 綁定
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

- `src/main.cpp` 用 build flag 分兩種角色：`ROLE_CLIENT`（下水端）、`ROLE_STATION`（岸上攝影站）；UART 控制端點位於 `src/uart_servo_mode.cpp`。
- 共用 LoRa 封包協定抽在 [../include/protocol.h](../include/protocol.h)；硬體腳位與 RF 參數在 `main.cpp` 上方常數區；手機熱點帳密在 [../include/wifi_config.h](../include/wifi_config.h)。
- 角色由編譯期 `env:tbeam-client` / `env:tbeam-station`決定，未選或同時選兩個角色都會編譯失敗。
  另有 group B 的 Heltec V4 Station 與獨立 T096 Client profile；它們不是 group A 的自動換頻版本。

# 2. LoRa 無線通訊

- RF 參數、封包格式、msgType、欄位定義詳見 [interface.md](interface.md)。
- **LoRa 協定 v5**：共用標頭 6 bytes，包含識別碼、合併的版本／類型、16-bit `clientId`
  與 16-bit 序號。上下行只使用同一個 `clientId`；不傳群組、獨立來源／目的 ID、
  `payloadLen` 或空白驗證尾碼。保留 RF CRC，依類型嚴格檢查長度與欄位；不接受 v3 封包。
- Client ID 由晶片 MAC 末 2 bytes 衍生，遇 0、廣播或舊保留 ID 時重新映射。
  16-bit ID 並非全球唯一，兩套若撞號仍需處理；不同 ID／白名單不會消除同頻碰撞。

| 類型 | 大小 | 發送節奏與用途 |
|---|---:|---|
| DATA | **18 bytes** | 最新有效定位即送；失效只通知一次、無 DATA 心跳；座標、速度／方向、衛星級距、有效旗標、HDOP；無來源年齡 |
| TELEMETRY（TEL） | **11 bytes** | 約每 60 秒一次；電池、溫濕度、精確衛星數 |
| DIAGNOSTIC（DIAG） | **17 bytes** | 約每 60 秒一次；Client GNSS 間隔、UART 積壓、NMEA／TX 錯誤、發送延期與狀態 |
| GNSS DIAGNOSTIC | **36 bytes 單包** | 約每 60 秒一包；GPS 資料流、來源年齡與恢復／拒收計數 |
| CLIENT CONTROL / STATE / LINK TEST | **13 / 28 / 18 bytes** | T096 command、STATE rendezvous/confirmation、RF probe；codec 見 interface |

- **GPS 請求 2 Hz，不代表實測達標**。雙端以 115200 baud 設定 RMC＋GGA／500 ms；
  L76K 原廠 >1 Hz 要求單語句，這是已確認採用的雙語句驗證版。每 5 秒觀察 epoch／RMC／GGA
  實際頻率，缺語句與頻率不符明示，不靜默降級。Station 診斷 API 可看三值，Client 序列可看三值，
  Client 低頻 DIAG 額外帶量測完成／三者約 2 Hz 旗標。
- DATA、TEL、DIAG 各自使用序號；GNSS DIAG 每包使用獨立序號，均不消耗 DATA 序號。
- **單向非阻塞發射**：Client 由 DIO1 TxDone／timeout 推進，完成後 standby；Station
  只接收、不回 ACK。Client TX 成功只代表發射完成，收包情況看 Station 序號與 RSSI／SNR。
- **最新有效定位即送**：radio 空閒且 guard 已滿便送，不另加 1 Hz 限制。RMC／GGA
  最多短等 150 ms 配對；同一定位不重傳，忙碌只留最新一筆。有效轉無效通知一次，
  持續無效不送 DATA，恢復直接送新位置；Station 仍用逾時判斷，不依賴一定收到失效通知。
- **SF10 排程**：BW125／CR4/5 下 DATA／DIAG 約 330 ms、GNSS DIAG 約 494 ms、TEL 約 289 ms，
  實際 TxDone 後至少留 80 ms。TEL、DIAG 各約 60 秒，GNSS 單包約 60 秒；沒有定位也送。
  診斷每秒最多一包；有持續 DATA 時交錯，必要時延後一筆定位。同時到期優先 TEL、DIAG、GNSS；
  GNSS 無分頁組裝；收到單包立即替換診斷。失效通知優先於診斷。
- `skipped_slots` 保留舊欄名，計 DATA 因 radio 忙碌／未就緒而延期的工作次數；
  同一次等待只計一次。延期不等於 RF 丟包，DATA 序號不受診斷影響。
- **供電順序**：PMU 先初始化，LoRa 開機與重建都先設定／讀回 ALDO3 3300 mV＋啟用，
  等 10 ms 穩定再存取 SPI。失敗有明確 log，不依賴前次韌體留下的電源設定。
  SF／BW／頻率／Sync Word 改動須同步兩端；**SF9 舊板無法與本版 SF10 通訊**。
  這是單套排程，沒有多套系統同步或自動換頻；尚未新增手機設定入口或 SF 切換功能。

# 3. Client（下水端）

- 持續讀取 GPS UART 115200；[GNSS 快照收集器](../include/gnss_snapshot.h) 依 NMEA UTC
  epoch 對齊 RMC／GGA，驗證 checksum、定位狀態與數值。位置、品質與速度不借用其他 epoch
  的欄位；只有新 GGA 時可回報位置／品質，但不沿用舊 RMC 的速度。
  等待同 epoch 的 GGA 最多 150 ms，缺少品質保持未知；明確無效定位優先撤銷。
- **`fix` 代表有效且持續更新的位置**：Client 本地以新 epoch 首次到達後 <2 秒為門檻，
  Station 遠端以新 DATA 接收後 <2 秒為門檻；重複不刷新期限，推算來源 age 只供診斷。
  UTC、checksum、積壓與明確無效檢查保留。
- 座標使用兩個完整 signed32 E7（10⁻⁷ 度），共 8 bytes，無固定原點；編碼解析度不是實際精度。
- DATA 傳衛星級距：未知、≤5、6–7、≥8；保留 HDOP（向上取整至 0.1）。
  精確衛星數改放約每 60 秒的 TEL，僅供診斷；追蹤資格不能拿舊 TEL 數值代替 DATA 級距。
- TEL 另傳電量、溫濕度（BME280，沒感測器則回報未知）；DIAG 記錄 Client 真正量到的
  GNSS epoch 間隔與錯誤／積壓／延期計數。計數自 Client 開機累計、16-bit 飽和至 65535；
  DIAG 是低頻快照，不是逐筆即時紀錄。
- 電量讀取：透過 AXP2101 PMU，每 5 秒更新。
- **固定發射功率**：Client 固定 **20 dBm**；Station 只接收，初始化保留 **17 dBm** 設定。
  Client 已移除 ATPC，不依 ACK、RSSI／SNR 或經過時間調整功率，也不讀寫舊
  `shorespt_client/txpwr`、`atpc` 設定；舊鍵留存但不生效。上行封包格式保留；SF10 時槽安排見上節。
- OLED 平時進入睡眠省電；**短按 PWR 鍵**喚醒螢幕顯示狀態 10 秒後自動睡回去（非阻塞，不影響定位/發送）。
  - 睡眠是 `u8g2.setPowerSave(1)`（關顯示與升壓電路），不是只清畫面 —— 只清畫面的話
    像素熄了但控制器還在跑，是毫安等級的常態消耗。
  - **電源軌 ALDO1 刻意維持供電**：同一條軌供電給 I2C bus-0 上的 BME280，而下水端每
    5 秒要讀一次溫濕度（也是進水提醒的資料來源），關掉會連感測器一起失去。
- **長按 PWR 鍵** → 顯示關機畫面後由 PMU 斷電。

# 4. Station（岸上攝影站）

## 4.0 Heltec V4、手機位置與 T096（2026-10-02 USB 桌上部署）

- Heltec V4 Station 是 group B 923.8 MHz profile；無 PMU／SD／BME280／內建 GNSS，battery/charging
  都是 unknown。SSD1306、Servo GPIO4、FEM 和 auto GC1109/KCT8103L probe 的腳位見
  [hardware.md](hardware.md)。0 dBm 是 SX1262 chip drive；FEM enabled 並非天線端 RF 輸出實測。
- Station 可保留使用者明確更新的 phone 位置到 RAM；重開即遺失。Station HTTP 頁不自行取得瀏覽器
  定位，HTTPS helper 背景化／鎖屏可能停止，所以只有最後成功 snapshot 與 age，沒有背景定位保證。
- T096 只有在送出 STATE 後才開 800 ms RX；Station 有 pending command 才在 rendezvous 20 ms 後嘗試
  一次下行，STATE 過 120 ms 未排到即跳過。HTTP 202 是 pending，僅後續匹配 boot/station/command/state
  的 STATE 才 confirmed；TX failure/timeout 都回 RX。
- T096 `start`／`stop`／`store` 只改 Client state；`test` 只送 RF probe。它們不改 Station Servo 或
  GPS/UART mode，也不以 fake GPS 產生 DATA、方位或追蹤。Ready 12 小時後才請求 Storage/SystemOFF，USB
  VBUS 在時抑制關機。USB VBUS 狀態回報已確認；無線充電板未接，充電喚醒、深眠電流與續航待實測。

- 接收時保留實際 RF 長度，驗證 magic、v5、類型、固定長度、`clientId` 白名單與數值／保留位元。
  格式、長度、綁定、序號與 RF 錯誤分別計數，並寫入 64 筆結構化事件環形紀錄。
- 收到 DATA → 更新 RSSI/SNR 滾動統計（近 20 筆）、**約 5 秒視窗的 DATA 封包率**；
  距最後 DATA 超過 5 秒，`pkt_rate` 回報 0。此值是 RF 接收率，不是新 GNSS 定位率。
  接收處理後直接恢復 RX；不建立 ACK 或其他下行封包。
- 自己也讀 GPS，使用 **30 秒新鮮新 epoch 的座標平均**計算「攝影站 → Surfer」方位角。
  原始座標另保留，RMS >3 m 只警告、不丟離群點；平均不能修正所有共同偏差，亦非真實精度證明。
  仍由當前原始定位有效性／新 epoch 到達期限決定追蹤資格。適用固定攝影站，移動腳架後平均需時間跟上。
- Station 每次初始化／重建開啟 SX1262 boosted RX gain，失敗明示；實際收訊改善待 RF 對照測試。
- Station 不保存軌跡；瀏覽器每秒嘗試累積有效且不同的位置，只保存及顯示最近 5 分鐘。GPX／Strava 操作已移除；
  完整逐包與狀態 log 存 SD；兩端皆可循環回收最舊自家 log。Client 僅在新鮮 Good／OK GPS 時記錄，
  失效時背景排空並關卡電，LoRa 行為不變。USB 讀取與條件見 [SD 記錄](sd-log.md)。
  錄影時使用另一支手機保持網頁前景與螢幕亮著；切背景、鎖屏或斷線可能漏點，重整頁面會清空軌跡。
- DATA 拒絕重複／倒退序號，支援 uint16 回繞；滿 3 秒未接受 DATA 後，第一包有效位置重建基準並記錄原因。
  無效位置不重建，無 boot ID／握手，不能辨別重啟與失聯。
  `sequence_gaps` 只由 DATA 的前進序號推估缺包，TEL／DIAG 不影響此計數；重同步另計。
- **單一 Client**：只接受綁定 ID 的 DATA／TEL／DIAG，NVS 保存 `gpsclient`。
  新裝預設 **0 = 未綁定**，不接受任何 Client 定位；有效舊綁定會保留。
  舊 `wl` 只遷移第一個有效 ID；刻意清空不恢復預設。API 的 add 不覆寫不同 client，
  set 可明確替換；成功變更時撤銷自動控制並清空舊 client 的資料、濕度基準與滾動統計。
  v5 線上封包只帶一個 `clientId`，上行表示發送者；目前沒有下行；現階段容量固定為 1。
- **雲台控制**：`manual / gps / uart / paused`。開機 Servo 90°、預設手動模式。
  GPS／UART 互斥；只有 UART 模式開啟 UART 接收控制。切換清除舊目標與 UART session。
  GPS 按鈕與 API 要求兩端持續更新的有效位置，品質差只警告；追蹤另需校正與
  有效磁偏角。條件恢復即於下個控制週期追蹤，不再要求連續可用 2 秒；失效停止更新／外推目標，按速限轉完最後目標再停住，不自動切換來源。
  UART 接受 SET／SET2，250 ms 無有效指令保持；新 SET 可恢復，不要求 GPS／校正。
  Manual／pause／OTA／關機撤銷自動控制；start／resume 不修改校正。
  舊 HTTP `mode=jetson` 接受為 UART 別名，回應統一為 `uart`。
  - Servo（IO21，內建 LEDC 333 Hz / 14-bit PWM，500–2500 µs = 0–180°）。
  - 從上方看，Servo 角度增加為逆時針、羅盤 bearing 增加為順時針；指南針校正時鎖定 `mount_offset = compass_bearing + servo_angle`（只存 RAM），之後 `servo_angle = mount_offset + declination − true_bearing`（clamp 0–180°）。
  - **GPS 目標重算為 20 Hz（每 50 ms），與封包到達脫鉤**：α 開啟時，可在有效封包之間外推。
  - **位置外推（dead reckoning）**：DATA 由最新有效 epoch 觸發，GPS 請求 2 Hz，使用同一快照且有效的速度／方向
    做等速預測，不含加速度；API 將 wire 的 0.1 m/s 速度還原為 `speed_cms`，方向為 `course_deg10`。
    缺少有效速度向量、速度低於 0.3 m/s 或定位過期時不外推。
    外推只用 **自 RxDone 經過時間**，且兩端衛星 ≥6、HDOP ≤3，速度有效；品質差仍直接追位置。
  - **更新期限**：Client DATA 的來源／合計年齡為 null，僅 `rx_age_ms` 用於 2 秒逾時。
    岸端本地用新 epoch 首次到達後時間；NMEA 年齡估算保留診斷，不 gate 追蹤。
    原始速度／方向保留，`velocity_valid` 與 `prediction_active` 分別表示向量有效與控制是否使用。
  - **GPS 位置預測 α 開關**：資訊頁勾選為 α=1，取消為 α=0（預設），
    使用最後收到座標；切換自動保存至 NVS，重開機沿用。關閉後 Servo 仍按共用速限追目標，
    不會暫停追蹤；手動／UART 不受此設定影響。這是位置預測開關，不是 PID 或馬達速限比例。
  - **所有模式只共用最高速度**：預設 30°/s，資訊頁可調 1–90°/s，修改自動寫入
    NVS 單一速度值，之後開機沿用；舊多參數設定不帶入。移動中也可改速，不換模式或目標。
    按微秒實際時間限制每次位移，Servo 無固定輸出頻率；GPS 目標另每 50 ms 重算。
    保留小數微秒精度到 14-bit duty，只有數值變化才寫入。加速度／jerk／死區規劃已移除。
    命令範圍固定 0–180°；長停頓最多採計 50 ms，不補送歷史目標。UART 來源失效立即保持；GPS 失效轉完最後有效目標後保持，手動暫停／模式切換仍立即撤銷。
    API 無機械回饋，首次回 90° 無法保證物理限速；指南針校正採用送達時的命令角度。
    詳見 [共用控制器](motion-control.md)。
  - 註：α 開啟時 servo 瞄準的是**外推後**的位置，而 `/api/track` 的 `client.lat/lon` 與
    `bearing` 仍是**原始收到值**，兩者在高速時可能差幾公尺（雷達圖上約數 %）。
- 長按 PWR／低電量關機先切手動、撤銷控制，再顯示畫面與要求 PMU 斷電。
- OLED 位址獨立偵測：優先檢查 0x3D，否則使用 0x3C；不再依賴磁力計初始化。
  保留兩種板子版本的螢幕支援。

## 4.1 鏡頭指南針校正

1. 固定腳架，按「回到 90°」，切為手動並依共用速限回中，等鏡頭停穩。
2. 輸入鏡頭普通磁針指南針讀數：北 0°、東 90°、南 180°、西 270°；其他方向填實際讀數。
3. 按「校正」，需要追蹤時再選「GPS」。UART 模式不需要此校正。

`POST /api/track/calibrate?bearing=<0..359.999>` 只保存 RAM 參考，僅接受命令位置與目標
均為 90° 且不再移動；非 90°／移動中點擊會通知，後端重查並回 409，保留原校正。
不需兩端 GPS，請求保留 HTTP 排程與新鮮度檢查。校正不切模式或解除 PWM 故障；
角度是命令而非實體回饋，仍需確認鏡頭與磁針停穩。GPS 後續重算使用新參考。
板上磁力計初始化、取樣、旋轉補償、hard-iron 校正頁與 API 已移除。
不再讀寫 mount*／mag* NVS。重開機採用 90°有效偏移；腳架方向不同時可重新校正。

磁針輸入為磁北，GPS 與雷達加入 WMM2025 磁偏角（東正西負）；依攝影站 GPS 位置與
UTC 日期自動查表，適用北緯 18–28°、東經 116–124°、2025–2029 年。
GPS 位置／日期無效或超出範圍時，GPS 不更新目標，最多轉完最後有效目標；UART 不受影響。
磁偏角補償不能消除指南針附近金屬／馬達的局部磁場干擾。

# 5. WiFi / Web 監控（Station）

- STA 模式連手機熱點（帳密在 `wifi_config.h`），逾時 20 秒；連不上仍跑 LoRa，背景自動重連。
- 內嵌單頁 Web UI 分為**雷達／資訊**，無外部 CDN；前端每 1 秒輪詢 `/api/track`、
  每 3 秒輪詢 `/api/status`。所有 API 共用單一請求排程，含回應 body 讀取；待送控制優先，
  背景同類請求合併，忙碌時輪詢可延後。網頁輪詢頻率不等於 RF 或 GNSS 更新頻率。
- 雷達採白底、深綠格線／方位／波束；GPS 標籤採白底深色文字。
- 手動角度使用雷達最外圈上半圓軌道與把手，地圖亦保留；GPS／UART 模式隱藏軌道、刻度與拖曳提示，
  保留顯示目前命令角度的圓形標記，但不能拖曳。
  刻度固定左 0°、上 90°、右 180°；UI 0–180° 對應內部 180–0°，波束另依校正計算實際方位。
  拖動只預覽、放開才送出。在途請求最多一筆，
  待送只保留最新已放開目標；模式切換清除待送並等待在途完成。pointercancel 不送出。
  指令排程由 `node tools/test_servo_slider.js`、外圈手勢／取消／端點由 `node tools/test_servo_ring.js` 驗證。
  雷達不要求有效 GPS；無定位時保留格線與手動外圈，不畫虛構位置。Surfer-only 以 Surfer 為中心，GPS 追蹤門檻不變。
  圖面滿版，左上「資訊」與「雷達／地圖」間隔 10 px；右上依序「手動／GPS／UART」。角度數字顯示在外圈把手上。
  頂部按鈕字級 18 px、高 46 px（約原版 1.5 倍）；分段按鈕只有兩端圓角，窄螢幕保持單排。
  提醒整區半透明疊在頁底，不佔雷達高度；展開上緣依下方 NESW 方位圈安排，過長清單可捲動。
  Station GPS／Client GPS／LoRa 由上而下排成燈號群組；前兩者依有效 fix、LoRa 依 90 秒內接受過綁定 Client 任一有效封包顯示綠／紅。
  只有遙測而尚無新 DATA 時，連線提示顯示「收到遙測，等待 DATA」；定位與 GPS 追蹤的新鮮度門檻不變。
  LoRa 顯示 Station 最近 10 秒接受 DATA 包數 ÷ 10 的 FPS，獨立於 RF 連線燈；不計拒收／診斷包或瀏覽器輪詢。
  等待資料或網頁 track 超過 3 秒未更新顯示灰燈，不把網頁失聯冒充 GPS／LoRa 故障。
  雷達依可用長寬選擇燈號位置與圓心，避開外圈及把手；地圖燈號固定左側。
- OLED 運行畫面仍分 8 段傳輸、段間服務控制，固定正常刷新；不提供 OLED 刷新開關。
- Station 支援 PlatformIO `espota` Wi-Fi 韌體更新；OTA 開始時暫停追蹤，完成後自動重開。OTA 本身未設密碼，以手機熱點的存取控制作為網路邊界。
- API endpoint 與 JSON 欄位詳見 [interface.md](interface.md)。
- **除錯與匯出**：網頁除錯／錄製／JSON 匯出已移除。HTTP schema 2 診斷與 log cursor 保留，
  詳細離線資料改存 SD，經 USB 核對大小與 CRC32 讀回，見 [SD 記錄](sd-log.md)。
  GNSS、低頻 Client DIAG 與實際收包頻率仍分開保存在診斷欄位。
- Client 單包 GPS 資料流診斷：收到完整快照即顯示來源年齡、UART／NMEA／
  epoch前進距取樣時間、原始fix與時間恢復／拒收計數；舊Client顯示未收到，90秒過期明示。
  GPS UTC停滯／倒退後須多個有效新epoch及至少2秒一致節奏才重新建立時間基準，
  重複或突發積壓不能直接刷新定位；詳見 [介面契約](interface.md)。
- **執行紀錄**：USB 序列埠與 4 KB 環形緩衝保留；`/api/log?from=<offset>` 回傳文字增量，
  `X-Log-Next`／`X-Log-Dropped`／`X-Log-Boot` 提供游標、遺失與開機識別。
- **RX 每分鐘一筆統計**：收包不逐包印文字，避免快速洗掉 4 KB 環形緩衝中的控制事件；
  累積後每 60 秒印一行：收到位置數／seq 範圍、drop/err（含最後的錯誤碼）、
  RSSI 與 SNR 的 min/avg/max、雙方 GPS、距離/方位、servo 角度與模式。
  百分比使用收到的 DATA 加上 DATA 序號推估缺包作分母；TEL／DIAG 使用獨立序號，
  不再灌入 DATA 分母。序號缺口仍不能區分 RF 遺失、發射失敗與重啟等原因，需搭配
  Client DIAG、Station 錯誤計數與事件紀錄判讀；API `drop_rate` 是拒收計數比例，並非空中丟包率。
  4 KB 能保留多久取決於各類 log 的頻率，追蹤期間還會產生每 3 秒一筆的追蹤紀錄。
- IP 快取在 DHCP 換位址時會更新（比對 `IPAddress` 而非只在空值時寫入），
  避免 WiFi 未斷線但續約換 IP 時 OLED 一直顯示舊位址。
- 雷達頁顯示模式／追蹤或保持狀態，資訊頁顯示連線、接收年齡、GPS 品質、RSSI／SNR、電量與校正偏移；Uptime 留在診斷 API。
- **GPS 品質顯示**：一般操作頁將 HDOP 換算成 `HDOP × 2.5 m` 的粗略「預期精度」，
  並非模組實測誤差。Station 衛星顯示少／普通／好；Client 顯示 DATA 的**未知／≤5／6–7／≥8 顆**。
  Client 精確顆數僅由 TEL 低頻更新，API `satellites` 在未知或 TEL 超過 90 秒時為 `null`，
  另提供 `satellites_age_ms`。UI 不將級距下界冒充精確數字，GPS 品質／追蹤門檻也不使用舊 TEL 顆數。
- 雷達圖：以攝影站為中心的 east/north 座標，自動縮放、畫出 surfer 過去 5 分鐘軌跡與 servo 當前/目標指向。
- 地圖使用瀏覽器載入的 OpenStreetMap 圖磚，需要網路；韌體內的操作頁與雷達可本地使用。

# 6. OLED 顯示內容

SH1106 128×64。以下為模擬畫面（`<...>` 為動態數值）。

## Client 開機畫面（3 秒動畫，接 10 秒資訊）

開機先播放 3 秒單色雷達掃描、浪形展開與 SHORE SPOTTER 字標動畫，再顯示資訊 10 秒後讓面板睡眠。
動畫是品牌圖案，不代表已取得 GPS 定位；短按喚醒只顯示資訊 10 秒，不重播動畫。
啟動畫面期間繼續服務 GPS／PMU，診斷版並處理 USB／Flash，Trip SD 背景記錄持續。
開機第一次完整讀取／清除只建立按鍵基準，不執行殘留事件；之後再次按下形成的新下降沿，
在動畫／資訊頁期間也可接受短按喚醒／長按關機。持续按住原來的開機鍵，不會產生新的下降沿而誤關機。
資訊頁末段短按會保留從該次短按起算的十秒顯示期限；未操作時仍按原定時程熄屏。

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

## Station 開機畫面

```
  開機中                WiFi 結果（顯示 2 秒）
+--------------------+   +--------------------+
| SHORE SPOTTER v<version> |   | SHORE SPOTTER v<version> |
| STATION booting...  |   |--------------------|
|                    |   | WiFi connected     |   失敗 -> WiFi FAILED
|                    |   | <手機分配的 IP>     |   失敗 -> see wifi_config.h
+--------------------+   +--------------------+
```

## Station 運行畫面（單一綁定 Client，畫面刷新獨立於發送頻率）

左半 = Station 面板\
右半 = Client 面板

```
+------------------+------------------+
|      Station    | V |  Client XXXX    |
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
+ `BAT` 顯示電量百分比（左=Station 自身 18650、右=選定 Client 遙測），無資料顯示 `--%`；Station 接著 USB/充電時左側顯示 ⚡。
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

- Station（namespace `shorespotter`）：單一 client `gpsclient`（UShort，0 = 未綁定；舊 wl 僅首次遷移）。
- 唯一速限以 `shorespotter/servospd` 單一 uint32 保存（毫度／秒）；資訊頁修改即寫入並回讀確認，
  相同值不重寫，重啟沿用；舊 motioncfg 不讀取。
- GPS 預測開關以 `shorespotter/gpspred5` 保存，0 = α 關、1 = α 開；未設定預設關閉，舊 gpspredict 鍵不沿用，
  保存失敗不修改執行中的設定。
- 開機使用 90°有效鏡頭校正；重新校正只存 RAM，重開機恢復 90°，板上校正停用。舊 mount*／mag* 鍵忽略，不清除整個 NVS 區域。
- Client 舊 namespace `shorespt_client` 的 `txpwr`／`atpc` 不再讀寫，也不清除；功率由韌體固定為 20 dBm。

# 8. 健康 / 告警門檻

- 電量百分比：單顆鋰電 **3.2V = 0%、4.15V = 100%**（線性），OLED 與網頁皆以 % 顯示。
- 低電量自動關機（Station / Client 皆有）：偵測到 **< 3.2V** 時顯示「LOW BATTERY」後由 PMU 斷電；**開機**時若已過低則直接顯示後關機（無法開機）。為避免誤動作，**接著 USB（VBUS 在）時不關機**、且讀值 < 2.5V（視為無/異常電池）時忽略；執行中需連續兩次低讀值才關機。
- 充電指示：偵測到外部電源（USB/Type-C）時顯示 ⚡（OLED 與網頁「站」電量）。
- 健康監控：uptime、可用 heap、最小 heap、reset 原因、RX 錯誤計數。

# 9. 電源架構與電量量測

```
2S 鋰電 ──┬─ UBEC 6V ─────────→ Servo
          └─ Type-C 變壓 ──→ Station USB-C（PMU 視為 VBUS）
Station：USB-C(來自2S) + 板載 18650（備援，像手機插著電使用）
Client：板載 18650
```

- **板上 AXP2101 只量得到 18650（單 cell）**；那顆 2S 對 Station 而言只是接在 USB-C 的電源（PMU 看到 VBUS），**無法讀其電量**。
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

基準值是 Station 端記的：下水端的基準來自收到的第一筆遙測，站體的基準來自開機後第一筆
BME280 讀數。

# 11. 失效回復

- **無線電重建**：SX1262 若因 SPI 干擾或狀態機卡住而停止工作，原本兩端都只會印一行
  log 然後安靜地永遠壞下去 —— 下水端平時螢幕是關的，完全沒有外部徵兆。現在：
  - Client 連續 `RADIO_TX_FAIL_LIMIT`（5）次送不出去 → 重新 `initRadio()`，
    重新確認 ALDO3 3300 mV／啟用，套用 Client 固定 20 dBm 並掛回 DIO1 中斷。
  - Station 連續 `RADIO_RX_ERR_LIMIT`（30）次讀取錯誤 → 同樣重建並恢復 RX（17 dBm 初始化設定不代表實際發送）。門檻比 Client 高很多，
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
