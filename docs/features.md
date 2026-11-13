# 韌體功能說明（更新至 2026-10-28）

現行原始碼為 **1.0**，診斷專用建置為 **1.0-debug**；唯一來源是
[firmware_version.h](../include/firmware_version.h)。版本採 a.b，同主版本子版要求 Client／Station 互通，
不相容變更須升主版本；這是版本規則，不是已完成混版實機驗收的聲明。
LoRa wire v5／BLE wire v1 與韌體版本分開。

本文件整理現行功能；歷史主機／部署結果見 [CHANGELOG](../CHANGELOG.md)、
[BLE 紀錄](ble-control-2026-10-27.md)、[維護紀錄](t096-maintenance-2026-10-28.md)。
0.7.0-dev／UI=2～4 的既有部署證據保留當時版本；1.0／UI=5 雙端部署與後續 Client UI=6 分列於維護紀錄，
舊版 RF Test／外測不能自動升格為新版驗收。
操作入口見 [function-entrypoints](function-entrypoints.md)，接手須讀 [assistant-handoff](assistant-handoff.md)。

## 角色與架構

- 使用中的 Client 是 T096：nRF52840、UC6580、SX1262＋FEM、160×80 TFT，主流程在 `src/t096_client.cpp`。
  沒有 Wi-Fi／Web／SD／PMU 電流感測；不能套用 T-Beam 的供電／按鍵流程。
- LilyGO T-Beam Supreme 作 Station：ESP32-S3、GNSS、SD、Wi-Fi／Web；`src/main.cpp` 的 `ROLE_STATION`。
  保留的 `ROLE_CLIENT`／`tbeam-client` 是舊 LilyGO Client profile，詳見本文件末段。
- Heltec V4 Station 無 GNSS／SD／PMU／BME280，相關資訊 unknown；有 OLED、FEM 與有界內建 Flash。
  部署需逐板核對，不能由 LilyGO 已更新推定 V4 也更新。腳位與供電見 [hardware](hardware.md)。
- 建置 profile／manifest 與執行期選頻分開，見 [device-config](device-config.md)。
  舊 UART 外部輸入不編入正常 Station；保留其純邏輯測試不代表仍有操作入口。

## T096 開機、BLE 與待機

開機回 Ready，GPS 關閉、LoRa 休眠；未連線時每約 10 秒 BLE 可連線廣播。
廣播間隔不保證首次連線延遲；連線時 Station 約每秒讀狀態。
Station 自動搜尋 NVS 已指定的 Client ID，第一個有效 BLE Claim 固定 Client 本次 boot owner。
一般連線／掃描不算認領；Stop、BLE 斷線、Station 重開均不解除，同一 Station 可重連。
換 Station 前先停用舊 Station 搜尋，設定新目標，再重啟 Client。

| 狀態／命令 | GPS | LoRa | 控制與限制 |
| --- | --- | --- | --- |
| Ready／stop | 關 | 休眠，無定期 STATE／RX | 保留 RAM、boot、序號與 owner，BLE 控制 |
| Tracking／start | 開 | 最新有效 DATA＋STATE／輪替診斷 | Start 必須有匹配 LoRa STATE 才確認 |
| Test／test | 關 | STATE／probe | 用於無線連線測試，不代表 GPS／Servo 驗收 |
| 舊 store API | 關 | 同 Ready | Stop 別名，不進 Storage／SystemOFF |

FreeRTOS tickless RTC 在服務空檔睡眠；BLE、USB、按鍵與各期限影響實際喚醒。
Ready 不定期重試 RF 初始化，等 Start／Test 再啟動；活躍狀態故障可重試，保留原始錯誤，不自動換頻。
舊 Ready 每 30 秒 LoRa／USER 長按 2 秒回 Ready 僅為 [RTC 歷史紀錄](t096-rtc-standby-2026-10-09.md)。

Web／未來 App 經 Station Wi-Fi HTTP 控制，沒有手機直接控制 Client BLE 的產品流程。
`/api/client/control` 為 `api_version=2`，`supported`、`state_source`、`can_control` 各自判讀；
BLE 新鮮度依 Client uptime 推進，重讀相同 bytes 不延長存活。`ready_poll_ms=0`。
HTTP 202 是 pending；以 Station boot／operation ID 追蹤最近操作，confirmed／error／timeout 分開，期限 65 秒。
Start 要匹配 Client boot、owner、command 的實際 LoRa STATE；Test／Stop 可由有效 BLE 狀態確認。
完整契約見 [BLE 控制](ble-control-2026-10-27.md) 與 [OpenAPI](openapi-client-control.json)。

## RF、封包與定位新鮮度

候選 A=923.2 MHz、B=923.8 MHz，兩者皆 SF9／BW125／CR4/5／sync 0x12。
舊 `rf-groups.json` A/SF10、B/SF9 是建置 profile，不代表現行 runtime SF。
Ready 且 BLE claimed 才可手動 CAD／選擇；每候選五次 CAD，掃描後還原頻率，無樣本與錯誤明示。
選擇先在 RAM；Start 收到匹配 LoRa STATE 且 Station 位於選定頻道後才保存 NVS `rfchannel`。
BLE accepted、HTTP 202、Test 回報都不直接保存。不自動跳頻，CAD 無偵測不保證無干擾。

| LoRa 類型 | 大小（含標頭） | T096 發送用途／頻率 |
| --- | --- | --- |
| DATA | 18 bytes | 最新有效定位；失效通知一次，無 DATA 心跳／ACK |
| TEL | 11 bytes | 電壓、MCU 溫度來源標記、衛星數；Tracking 約 15 秒 |
| DIAG | 17 bytes | UART／epoch／TX 計數；Tracking 約 15 秒 |
| GNSS DIAG | 36 bytes | 單包完整來源／fix／時間診斷；Tracking 約 15 秒 |
| CONTROL／STATE／TEST | 13／28／18 bytes | 控制下行／狀態確認／probe，與 DATA 序號分開 |

Tracking 每 5 秒輪替 TEL／DIAG／GNSS DIAG，各約 15 秒；Ready 不定期送 RF。
UC6580 R6 Build3700 的 `CFGNAV,100,100,1000`、RMC/GGA divisor 5 曾回讀並觀察 2 Hz；
有效 fix、epoch 更新率與 Station 收包率是不同證據。最早 RF 時槽 500 ms，STATE／診斷占用時槽，
不保證每秒兩筆 DATA；忙碌只留最新快照，不補歷史位置。

GNSS collector 以 UTC epoch 對齊 RMC/GGA，checksum、定位狀態、數值與積壓皆檢查，
不借用不同 epoch 的品質或速度。可短等 GGA 最多 150 ms；明確無效撤銷、重複不刷新期限。
Client 新 epoch 到達／Station 新 DATA 接收的 2 秒期限決定新鮮度，推算 source age 只作診斷。
E7 是編碼解析度，不是定位精度；缺速度不假造速度。GNSS UTC 停滯／倒退需多個新 epoch 重建時間基準。

DATA 單向非阻塞發射，TX_DONE 只代表送完；SEQ 缺口不能單獨區分 RF 丟失、發送失敗或重啟。
活躍 LoRa STATE 後聽 85 ms，有 preamble/header 才延至最多 800 ms；Station pending LoRa command
在 rendezvous 20 ms 後嘗試一次，STATE 超過 40 ms 未排到就跳過。Ready 沒有此窗口。
實際 wire／欄位驗證以 [interface](interface.md) 與 `include/protocol.h` 為準。

## T096 維護 UI=8（FB27 已部署）、電池與健康診斷

- 首頁五行：`CLIENT ID xxxx`（四位十六進位）、sleep／active 方框與實線、十六進位 owner＋BLE、GPS／RF、電壓＋UP。
  診斷頁六行：VERSION 1.0、TX、ERR、SAT、AGE＋HDOP、LAST。真實 BLE MAC 留在 USB 診斷；RF 休眠顯示 OFF。
- USER 熄屏短按開首頁 60 秒，亮屏短按切頁並重計時；按住滿 1 秒倒數，1～未滿 5 秒放開取消，
  滿 5 秒放開切換 ACT／SLP；未認領亦可 ACT，保留 owner／BLE／boot，RST 才重啟。
  ACT 沿用 runtime 頻道（開機 A），低電保護禁止 ACT；RF 忙碌等交易完成。開機已按住先放開；自動畫面約 5 秒。
- 開機立即顯示 `CLIENT BOOTING`；USER 切換完成顯示 `ACT STARTED`／`ENTERED SLP` 約 2 秒，失敗顯示低電／RF 原因。
- 共用 160×16 row buffer；首頁每列 16 px、診斷 13 px，6×8 字體、最多 26 字；診斷底部 2 px 清黑。
  500 ms 更新，RF 忙碌讓位；背光期限獨立，SLPOUT 120 ms 非阻塞。操作細節見 [維護文件](t096-maintenance-2026-10-28.md)。
- ADC 3.0 V reference、12 bit、390k/100k 分壓；RF idle 才取樣，開 divider 等 10 ms、丟首次並平均 4 次。
  0 或超出 2.5～4.5 V 為 unknown；Ready 健康時 30 秒、活躍／低電警告 1 秒、保護期間 60 秒取樣。
- 三次有效 <3.2 V 且無 USB 進低電保護，停止 GPS／新 RF；三次 ≥3.5 V 才回 Ready，不自動開 GPS。
  USB 出現或 unknown 不能自行解除既有保護；保護板斷電後 MCU 無法定時醒來，恢復須實測。
- Ready 電壓／VBUS 經 BLE 狀態，活躍 LoRa STATE／TEL 也有電壓。`charging` 僅表示 USB VBUS；
  百分比是 3.2～4.15 V 線性估算，電壓／USB blocked_ms 不證明淨充電電流、電池容量或續航。
- MCU 約每 30 秒機會式取樣，TEL `humidityPct=254` 表示晶片溫度／無濕度，環境欄位 null。
  接收 60 秒過期時目前值 unknown，接收最高值另保留；接收年齡不是樣本完整年齡，Ready 不送溫度 TEL。
- GNSS 拒收原因與 4 筆有界 RAM 原始 NMEA bytes 可用唯讀 USB `DIAG STATUS`／`DIAG NMEA` 匯出，
  含截斷／覆寫計數，不寫 Flash；重啟清除。見 [健康診斷](t096-health-diagnostics-2026-10-22.md)。

## Station 接收、位置與 Servo

Station 驗證 RF 原始長度、magic／wire／type、綁定 ID、數值與保留位元，分開計格式／序號／RF 錯誤。
DATA 拒絕重複／倒退，16-bit wrap 支援；3 秒無 DATA 後以有效新位置重建基準並記原因。
64 筆 packet event 保留 raw bytes；`lora.rssi/snr` 取最近有效綁定 RF，90 秒逾期；
`rssi_avg/snr_avg` 才是近 20 筆 DATA 平均。`pkt_rate` 舊約 5 秒計數與
`lora_fps_10s` 的 10 秒滑動 FPS 不混用。恢復 RX 不發 DATA ACK，控制下行另走 STATE 窗口。

Station GNSS 使用 30 秒新鮮 epoch 平均，保留原始點，RMS >3 m 警告但不丟離群點；
資格仍看當前定位新鮮度。可明確改用手機／手填，phone 快照僅存 RAM，無背景／鎖屏定位保證。
平均不能修正共同偏差，手機半徑、RMS、HDOP 與對已知位置的誤差分開。

單一綁定 `gpsclient` 保存 NVS；新裝未指定為 0。有效舊 `wl` 僅首次遷移；明確清空不回復預設。
改綁回手動並清舊資料／統計；命令／掃描／TX 忙碌拒絕改綁，保存失敗保留原值；同 ID 是 no-op。
設定成功不代表已認領／定位，見 [綁定 API](client-binding-api.md)。

Youmi＋串口控制板固定 M0，LilyGO TX43、9600 8N1、RX 不配置；0～359°、中位 179.5°／1500 μs。
控制板產生 PWM；命令 `#00P0500!`～`#00P2500!`，合併最新目標／脈寬去重、最多 50 Hz 非阻塞輸出。
共用微秒實際時間限速，預設 30°/s、範圍 1～90；UI 輸入整數，API 可接受有限小數，
以 0.001°/s 保存。長停頓最多採計 50 ms；UART 短寫明示故障。
只留手動／GPS，內部保留 paused；`mode=uart/jetson` 回 400，GPS α API 回 404，沒有位置外推。
GPS 目標每 50 ms 重算，需要兩端新鮮有效位置、校正、磁偏角；失效轉完最後有效目標，不自動換來源。
UART 成功沒有 ACK／角度回授，首次中位無法保證物理限速。詳見 [motion-control](motion-control.md)。

### 鏡頭校正

先切手動、按控制板實體置中鍵並等停穩，再輸入指南針整數 0～359°。
網頁送 `physical_center=1` 與新鮮 epoch／seq／stamp，表示操作者確認；後端要求 manual、輸出正常、
無 motion fault、限速器停止且命令跟上。成功以 179.5°同步軟體中位、更新控制世代並保存 RAM 參考。
未帶此參數的舊 API 要求軟體命令／限速器／目標都已精確為 179.5°且停止，網頁無回中按鈕。
校正不需 GPS、不清故障、不自行切 GPS。開機以指南針東 90°＋中位形成偏移 269.5°。
板上磁力計停用，不讀寫 mount*／mag*；位置／日期超出磁偏角模型範圍時不更新 GPS 目標。
磁偏角不能消除馬達附近局部磁場干擾；HDOP 不能換算為已知實際誤差。

## Wi-Fi／Web 與提醒

STA 連手機熱點，首次等待 20 秒，失聯背景重連；本機雷達／控制頁無 CDN，地圖圖磚需要網路。
雷達／資訊頁共用單請求排程，track 約每秒、status 約每 3 秒，忙碌可延後；控制優先、同類背景合併。
HTTP 讀取與 MCU loop／I2C 耗時會影響服務，輪詢率不等於 RF／GNSS 頻率。

資訊頁三區為定位資訊／Station Info／Surfer Info；詳細定位／RF／命令／MCU 數據收進診斷。
控制板卡、網頁置中、正常溫濕度／SD／版本常駐顯示已移除；速度成功只更新值／關閉視窗。
Surfer ID／頻道點擊編輯，ID 取消或頻道彈窗點外部關閉不送出；失聯保留已確認 ID，僅明確 `client_id=0` 顯示未配對。
無線連線測試有結束操作。手機權限流程延後、不加複製診斷摘要；詳細規格見 [web-ui-design](web-ui-design.md)。

手動完整圓軌道 0～359°：0／359 下、90 左、180 上、270 右，UI=359−raw，端點不環繞。
拖動只預覽、放開生效，pointercancel 不送；GPS 模式標記不可拖曳。沒有定位仍可用雷達／手動控制。
GPS 燈依新鮮有效 fix，LoRa 燈依 90 秒內綁定有效 RF；BLE 不刷新 RF。
track 超過 3 秒未更新為灰燈。瀏覽器保存最近 5 分鐘軌跡，重整清除、背景／斷線可能漏點。

提醒在頁底，`/api/status.alerts` 提供；SD `state=error` 為 warn，主動 stop／mounting／unsupported 不報錯。
環境溫濕度門檻見 [alerts](interface.md#alerts--現場提醒)，T096 MCU 不套環境告警；高濕警告不能替代實體進水檢查。
GPX／Strava／網頁除錯錄製／JSON 匯出已移除，診斷 API 與文字 log cursor 保留。

## 保存、記錄與故障診斷

- Station NVS：`gpsclient`、`servospd`、經驗證的 `rfchannel`；校正、手機位置與 Client owner 不持久化。
  舊 gpspred5／gpspredict／mount*／mag* 不讀寫，也不為遷移清空整區 NVS。
- LilyGO Station 開機 SD 循環錄製；session header 為開檔 runtime 頻率／SF 快照，
  `frequency_scope=session_open_snapshot`，同檔切頻看 `[RF]`。T096 無 SD；保留 T-Beam Client 的 GPS gate 見 [sd-log](sd-log.md)。
- V4 將 RF bytes 與位置脈絡存有界 Flash，滿即停止、不自動刪除；見 [heltec-field-ready](heltec-field-ready.md)。
- USB 匯出核對原始長度與 CRC。ESP-IDF SDK log 透過固定佇列交主迴圈 LogTee，不直接污染匯出封裝；
  截斷／溢位可見，不宣稱可攔 ROM panic。快照 drops 不等於 RF 漏包。
- Axiom API 預設關閉，需獨立 Token，背景採樣／批次上傳可能因忙碌／斷網遺失；見 [axiom-log](axiom-log.md)。
- Station 無線電恢復會回目前工作頻道；TX/RX 錯誤與恢復結果可查 log。T096 Ready 不週期重試 RF。
- `/api/status.timing` 保留 loop／HTTP／I2C／OLED／RF 等 last/max；Wire 單筆 10 ms 不代表整個請求 10 ms。
  不以主機回歸宣稱所有實機期限達標；USB／HTTP／完整追蹤證據需分開。
- Station OTA 會暫停追蹤、完成重啟；網路存取界線見 [interface](interface.md#http-與-ota-存取)。
  T096 為 application-only DFU，每次先核對 serial／MAC／ID；韌體更新不是硬體驗收。

## 歷史與板型專用資料

10/9 RTC、10/10 demo／外測、10/11 四區 UI 及 0.7 部署結果保留於日期式文件。
舊 180° LEDC／115200 UART 外部輸入／90°回中不作現行操作說明。
下列 T-Beam Client／OLED 資料只適用其板型，T096 的 USER、TFT 與低電保護以上文為準。

# 保留的 T-Beam Client profile（非目前使用中的 T096）

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
- **固定發射功率**：Client 固定 **20 dBm**；LilyGO Station 初始化 **17 dBm**，對 T096 另有控制下行。
  Client 已移除 ATPC，不依 ACK、RSSI／SNR 或經過時間調整功率，也不讀寫舊
  `shorespt_client/txpwr`、`atpc` 設定；舊鍵留存但不生效。上行封包格式保留；各組 airtime／時槽安排見上節。
- OLED 平時進入睡眠省電；**短按 PWR 鍵**喚醒螢幕顯示狀態 10 秒後自動睡回去（非阻塞，不影響定位/發送）。
  - 睡眠是 `u8g2.setPowerSave(1)`（關顯示與升壓電路），不是只清畫面 —— 只清畫面的話
    像素熄了但控制器還在跑，是毫安等級的常態消耗。
  - **電源軌 ALDO1 刻意維持供電**：同一條軌供電給 I2C bus-0 上的 BME280，而下水端每
    5 秒要讀一次溫濕度（也是進水提醒的資料來源），關掉會連感測器一起失去。
- **長按 PWR 鍵** → 顯示關機畫面後由 PMU 斷電。

# T-Beam／Station OLED（T096 TFT 見上方維護介面）

SH1106 128×64。以下為模擬畫面（`<...>` 為動態數值）。

## 保留的 T-Beam Client 開機畫面（3 秒動畫，接 10 秒資訊）

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
+ `LoRa` & `GPS` 狀態為四級：`Good` / `OK` / `Bad` / `Miss`；LoRa取最近接受的任何綁定Client有效RF封包，90秒未收到才Miss，沒有GPS DATA也能分級。\
  GPS
  + Good = fix 且 HDOP≤1.5 且 sats≥8
  + OK = fix 且 HDOP≤3 且 sats≥6
  + Bad = 有收到衛星但無可用定位
  + Miss = 沒衛星且無定位。

  LoRa：最近有效DATA/TEL/DIAG/GNSS/STATE/probe的SNR≥7且RSSI≥−105為Good；其餘SNR≥0為OK、<0為Bad；90秒過期為Miss。不再顯示TEL替代字。
+ 溫濕度無資料顯示 `--.-C/--%`；T096 MCU marker 不填環境欄位，BLE 不刷新 LoRa 品質。
+ `BAT` 顯示電量百分比（左=Station 自身 18650、右=選定 Client 遙測），無資料顯示 `--%`；Station 接著 USB/充電時左側顯示 ⚡。
+ wifi_status
  + 連上熱點,顯示IP
  + 還沒連上熱點顯示 `reconnecting to <SSID> in <n>s`
  + 正在嘗試顯示 `WiFi connecting: <SSID> ...`


## T-Beam PMU 關機畫面（不適用 T096）

```
+--------------------+
|                    |
|     SHUTDOWN...    |   1.5 秒後 PMU 斷電
|                    |
+--------------------+
```
