# 版本紀錄

專案從 **0.1** 開始標版。韌體版本的唯一來源是
[`include/firmware_version.h`](include/firmware_version.h)；Client／Server 共用。
一般後續發版依序 0.2、0.3；若只修補既有版本，可用 0.1.1。
版本號不取代 LoRa `PROTO_VERSION`、NVS 格式版本或實機部署驗證。
每次功能、修正、介面或設定行為異動都更新本檔；尚未定版先記在「未發行」，
正式標版時移入對應版本，保留日期、相容性與驗證紀錄。

## 未發行

- 除錯減載：`/api/debug` 與瀏覽器匯出升為 schema 2，boot ID／since 游標只取新增事件，
  每次最多 8 筆；首次歷史分批載入、覆寫與重啟明示，uint32 游標回繞可用。瀏覽器合併
  近期事件供顯示，錄製只保存新增項目，不再每秒重送並重存整份 64 筆歷史。
- 全頁 API 改用單一請求排程（含 body 讀取），待送控制優先、背景同類請求合併，
  排隊／傳輸都有期限；控制序號在真正送出時建立，等待期間的舊世代／過期意圖不重送。
- 新增 `timing.http_detail`：拆分請求前處理（含 accept／解析／派送）、JSON／log 組裝、
  同步 write 與其他耗時，保存最後／最慢請求路徑及短寫統計；無 handler 的輪詢獨立計數。
  LoRa v4 格式及 200 ms GPS 積壓保護維持原定義。
- 除錯減載驗證（2026-09-13）：114／114 native 通過；實際 debug handler 的分頁、
  覆寫、重啟、32 組非法查詢與 log 回繞通過，26 個成功 fixture 回覆皆不超過 8 筆，
  最大 3,372 bytes。控制／封包後端整合與三套 UI 測試通過（滑桿 10／10）；新增完整
  body 序列化、優先序、逾時、無有效 abort、游標回繞，以及模式／角度／速限／α 四條
  操作路徑在外層等待時的期限／boot／epoch 測試。Client、Server、Server OTA build
  全部成功，專案 source 無警告；依賴套件警告仍在。Server OTA 完成，74,656-byte
  網頁與本地一致，下載頁面重跑三套 UI 測試通過；schema 2 與分段計時讀回正常，
  E91C 綁定、30°/s、α 開啟與 UART hold 保留，Client 無須重刷。
- 更新後以實際網頁 JavaScript 接真實 Server 唯讀重測 5 分鐘：916 個完整 HTTP 回覆，
  單次最多 8 事件、最多 1 個 fetch＋body；debug 平均 2,344.6 bytes，較更新前實讀
  縮小約 83.8%。新增 DATA 600、TEL／DIAG 各 10、Server ACK 75，無新增 RF／序號
  缺口；648 個含起始歷史的事件 ID 連續且不重複。GPS backlog 0；HTTP 最大 112.84 ms
  來自初次完整首頁，持續輪詢沒有新增 ≥50 ms 板上請求。仍觀察到 3 次主機請求逾時，
  連帶 2 次除錯排隊逾時及 3 次取樣間隔缺口，後續自動恢復；不能宣稱端到端等待已消除。
  schema 2 JSON 匯出成功，完整結果與證據限制見 [實機重測](docs/link-test-2026-09-13.md)。
- 資訊頁新增「GPS 位置預測（α）」二態開關：預設開啟 α=1，保留等速外推；關閉 α=0
  使用最後收到座標。只影響 GPS 目標估計，保留共用速限、模式、校正與來源資格門檻。
- 新增 GET／POST `/api/track/prediction` 與 servo.prediction_enabled／prediction_alpha；
  POST 檢查既有 epoch／seq／stamp，NVS `gpspredict` 寫入回讀後套用、相同值不重寫。
  UI 自動保存，失敗復原並防止延遲回覆覆蓋較新設定；共用控制上下文拒絕已退役 boot 的回覆。
- 韌體原始碼標示 `0.6-dev`；LoRa 協定升至 **v4**，Client／Server 需一起更新，v3 拒收。
  DATA 由 32 縮為 **17 bytes**，ACK 11、TEL 11、DIAG 17；改用明確 little-endian codec
  與嚴格長度／欄位驗證。移除空 MAC、加速度、group、目的 ID 與 payloadLen，保留 PHY CRC。
- DATA 使用固定原點 24°N／121°E 的 signed24 E6 座標（約 0.1 m 量化），速度
  0.1 m/s／最高 25.4 m/s，保留方向、衛星分級、HDOP、來源 age、fix 與向量有效旗標。
  未知／超界速度停用外推，不偽造數值；精確衛星數移至低頻 TEL。
- RF DATA 每 500 ms 排程、每 8 個 DATA 序號回 ACK；DATA／TEL／DIAG 序號獨立。
  Client TX、Server ACK 非阻塞；過期工作跳過、不積欠，低頻包只在非 ACK 空檔發送。
  嚴格核對 ACK ID、序號與回覆期限，ATPC 避開收 ACK 窗口，RX 失敗可恢復。
- 新增同 epoch RMC／GGA 快照；重複 epoch 不刷新 age，非法定位撤銷快照，UART
  超過 200 ms 未服務則丟棄積壓。追蹤 age 合計來源＋RF airtime＋接收後時間，另保留
  200 ms 不確定度，未滿 2 秒才可用；向量與位置分開判定，α 使用有效向量與合計 age。
  RF 2 Hz 不等於新定位 2 Hz：維持 GPS UART 9600、不強制未知模組命令，實際 GNSS
  更新間隔由診斷回報；GNSS 模組內部延遲未量測。
- 新 Server 預設未綁定，既有 NVS 綁定與控制設定保留；ID 避開保留值。TEL／DIAG／
  拒收包不覆蓋接受 DATA 的位置與訊號統計，事件保留當包實際訊號；DATA 缺口不再計入 TEL。
- 網頁新增唯讀「除錯」頁：封包／來源年齡、Server GNSS、Client 低頻 DIAG、RX／ACK／
  排程錯誤與 raw hex。可加備註、錄製、匯出 JSON 給 AI；有限容量，標示漏採、覆寫、
  重啟與過期資料。新增 `/api/debug`、`X-Log-Boot`、no-store，修正文字游標 uint32 回繞。
  Boot ID 在 PWM 初始化失敗時仍可識別重啟；匯出設定不含 Wi-Fi 帳密。
- `/api/track` 移除加速度，新增 satellite_class、velocity_valid、來源／接收／合計 age；
  未知值回 null。新增 [現場除錯操作](docs/debug-export.md)，協定與 API 以
  [interface.md](docs/interface.md) 為準；原 [提案審核](docs/packet-proposal-review-2026-09-13.md)
  保留為實作前歷史，後續以 1-byte 速度達成 17 bytes。
- 驗證（2026-09-13）：92／92 個 native 測試通過；實際控制／NVS／GPS 預測、NMEA→
  v4 封包、實際 RX 分流／錯誤 ID／重複包不污染追蹤狀態，以及診斷 JSON／環形紀錄／
  游標回繞與 PWM 失敗開機識別主機測試通過。
  10／10 個滑桿案例、完整控制 UI 與除錯錄製／匯出模擬通過，9 段文件 JSON 範例可解析。
  Client、Server、Server OTA 最終 build 全部成功，專案 source 無警告；相依套件仍有
  RadioLib USB CDC 等警告；戶外收包率、實際 GNSS 2 Hz 與機械追蹤效果尚未驗收。
- Server OTA（2026-09-13）成功，重開機 HTTP 讀回 `0.6-dev`／LoRa v4、17／11／11／17
  bytes、500 ms／ACK 每 8 DATA。既有 E91C 綁定與 30°/s 速限保留，α 預設開啟，
  UART waiting／hold、90° 命令位置、PWM 正常且無 motion fault。`/api/debug` 與
  log boot／cursor headers 正常，下載的 68,669-byte 網頁與本地完全一致；以板上頁面
  重跑除錯錄製／匯出主機模擬通過。此階段只更新 Server，Client 隨後另行 USB 更新；
  未另發轉角或啟動追蹤命令，戶外定位／機械追蹤仍待驗。
- Client USB 燒錄（2026-09-13）成功，以 USB／晶片 MAC 確認目標為 E91C，四個 Flash
  區段均通過雜湊驗證。開機紀錄回報 `0.6-dev`、v4 DATA17／ACK11／TEL11、500 ms
  發送與每 8 DATA 回 ACK；PMU／BME280／LoRa 初始化正常。兩段 10 秒觀察皆增加 20
  個成功 TX（約 2 Hz），TX error／skipped／backlog_drops 均 0；GNSS epoch 間隔
  1000 ms，沒有把 RF 2 Hz 當成新定位 2 Hz。當時 Server HTTP 無法連線，Client ACK=0，
  因此尚未完成雙端 v4 收包／ACK 驗證；不推定 Server 離線原因。
- 實機通訊測試（2026-09-13）：Server 上線後唯讀觀察 60 秒，新增 DATA 120、TEL 2、
  DIAG 2、Server ACK 發送 15；DATA 間隔 499–501 ms，無新增序號缺口或無線／解析錯誤。
  188 個去重原始事件由實際 codec 重解成功，1,281 次 API 欄位核對一致。兩端仍無 fix，
  Client GNSS epoch 為 1 Hz；Client ACK 接收尚無直接證據。另記錄一次 HTTP 211.34 ms
  停頓、控制間隔 212 ms 與 Server GPS backlog 丟棄 1 次，未修改韌體。詳見
  [實機通訊測試](docs/link-test-2026-09-13.md)；全程保持 UART hold，未執行機械追蹤。

## 0.5 — 2026-09-09

- 所有模式改為單一實際時間限速器，預設 30°/s；移除加速度、減速度、jerk、deadband
  與 Ruckig／等加速度規劃器。只處理最新目標，保留固定角度範圍與長停頓步幅保護。
- 唯一速限移至資訊頁，1–90°/s、修改自動保存，移動中與任何模式都可改；NVS 單一
  `servospd` 值、寫入回讀後生效，重複相同值不重寫。舊 motioncfg 忽略，首次採 30。
- 開機仍歸中 90°，完成初始化後預設 UART。GPS 只有 Server、Client 都為新鮮 Good／OK
  才可選；mode／start／resume 都在後端檢查。Bad／Miss／過期拒絕並保留原模式。
- GPS 追蹤門檻同步接受 Good／OK；指南針校正、有效磁偏角與 2 秒穩定期仍必要。
- 移除所有 Servo 進階／頻率／OLED 調試介面與獨立 log 網頁；OLED 固定分段刷新。
  settings API 改為單一 speed 自動保存，移除 diagnostics API、control_elapsed 與舊運動欄位。
- 49 個 native、實際 HTTP／PWM／GPS gate／NVS 主機整合、10 個滑桿與完整 UI 模擬通過。
  三環境 build 與 Server OTA 成功，板上版本 `0.5`、預設 UART、速限 30、PWM 正常。
- 新版頁面 47,155 bytes 與本地一致，以下載頁面重跑 UI／滑桿測試通過。室內 GPS 不可用，
  實機 mode=gps／start 都回 409 並保留 UART；diagnostics API 已回 404。
- 實機自動保存 29.5°/s，再次 OTA 重啟後讀回 29.5；最後恢復並保存 30°/s。
  E91C 綁定保留，Client 協定未變、未重刷；未另發轉角命令。實體抖動與戶外 GPS 尚未驗收。

## 0.4 — 2026-09-09

- Servo 取消 20 Hz 軟體節流，使用微秒實際經過時間推進；GPS 目標重算保留 20 Hz，
  所有模式仍共用控制設定與最新目標。長停頓單次最多推進 50 ms，不累積、不補送。
- 修正反向煞車交接丟失更新區間剩餘時間，避免後續軌跡依更新頻率而延後。
- 脈寬保留小數微秒至最後轉成 14-bit PWM；只有 duty 改變才寫入，硬體 PWM 維持 333 Hz。
- UI 改顯示即時推進與不限頻診斷；status 的 `timing.control_elapsed` 取代 `control_20hz`，
  提供微秒精度服務間隔、超過 5／50 ms 計數與 PWM 寫入／略過重複計數。
- 參數開關、NVS v2、指南針 RAM 校正及 HTTP／UART／LoRa 目標協定保持相容。
  Client 不需為此重刷。升級後需重整網頁。
- 60 個 native 測試、實際 HTTP／PWM 後端整合模擬、10 個滑桿案例與完整參數／診斷頁
  模擬通過；三環境 build 成功，Server OTA 成功、板上回報 `0.4`，下載 UI 與本地一致。
- 升級後還原使用者 RAM 參數：速度／加速度／減速度 30／30／30，jerk 120 與死區 1
  保留數值但關閉；OLED 維持暫停，未寫 NVS。Client 綁定 E91C 保留，Client 未重刷。
  還原後另收到新的調參，最後讀取 speed=90、其餘相同；保留該新設定。
- 20 次板上 status 取樣包含手動移動命令：控制服務平均 0.118 ms、最大 73.100 ms；
  HTTP 最大 73.04 ms，PWM／規劃無故障。這是命令／服務時序，未量測實體減振效果。
  平均間隔包含同一 loop 內多次服務，不等同有效 PWM 頻率；同步 HTTP 仍可能造成停頓。

## 0.3 — 2026-09-09

- UI 左 0°／右 180° 改對應內部 180°／0°，目前角度與目標文字同步反轉；HTTP／UART
  與 GPS／指南針內部座標保持相容。滑桿拖動只預覽，放開才送出，取消手勢不提交。
- Servo 滑桿下方加入可展開參數面板；移除左右行程欄位，保留固定 0–180° 命令範圍。
- 速度、加速度、減速度、jerk、deadband 五項數值與啟用開關；關閉會跳過該限制。
  jerk 開啟用 S 曲線、關閉用等加速度軌跡；只有限速時等速，運動限制全關時直接採用目標。
- 所有模式共用 50 ms／最高 20 Hz 更新，不補送停頓期間的歷史命令；PWM 維持 333 Hz。
- OLED 運行畫面拆成 8 段傳輸，段間服務控制；可暫停／恢復刷新、清除 20 Hz 統計做對照。
- 設定 NVS 升至 v2：舊加速度同時作為初始減速度、開關預設開啟、舊左右限制移除；
  明確儲存才寫入新版格式。settings POST 改為五值＋五開關，升級後需重整網頁。
- 57 個 native 測試與主機控制整合模擬通過；10 個放開送出／反轉／取消／模式隔離案例
  及完整設定頁／診斷開關模擬通過。三環境 build 與 **Server OTA 上傳成功**，板上
  回報 `0.3`，新版頁面與本地完全一致，五項預設與開關、PWM、Client 綁定正常。
- OLED 開啟／暫停／再開啟實機對照：控制間隔平均 50.10／50.02／50.08 ms，最大
  55／55／56 ms；漏過週期均為 0。OLED 單段最長 4.66／0／5.24 ms，最後恢復刷新。
  三段皆保持手動 90°；本次不包含實體轉動或減振量測。Client 未重刷。
- 軸心鬆動仍存在，尚未量測機構振動，不宣稱軟體已解決抖動。

## 0.2 — 2026-09-09

- GPS／UART／手動共用速度的預設值與可設定上限由 30°/s 提高為 **90°/s**。
- 網頁設定欄位、HTTP 驗證訊息及操作文件同步更新。
- 加減速度 30°/s²、jerk 120°/s³、1° deadband 與角度範圍維持原值；
  短行程不一定達到速度上限，開機首次 90° 的既有限制仍適用。
- 保持 NVS 格式相容，先前保存的較低速度仍可載入，不覆寫其他使用者參數。
- 53 個 native 測試、11 個滑桿案例、完整設定頁模擬與 8 個 API JSON 範例檢查通過；
  包含雙向 90°/s 速限、加速度／jerk 有界與舊設定讀回。三個韌體環境 build 通過。
- **Server OTA 上傳成功**，API 已讀回版本 `0.2`、speed=90、acceleration=30、jerk=120、
  deadband=1；開機 manual 90°、PWM 正常、Client 綁定保留。板上頁面與本地新版一致，
  以下載頁面重跑網頁案例通過。Client 未重刷；本次未量測實際機械速度／抖動。

## 0.1 — 2026-09-09

以現有功能作為首次標版基準：

### 功能

- GPS／UART／手動共用 S 曲線控制器，最高 30°/s、預設 1° deadband。
- 共用控制設定可先套用 RAM，再明確保存至 NVS；指南針校正只保留 RAM。
- 手動選擇 GPS／UART 互斥模式，單一 GPS client 綁定與台灣磁偏角換算。
- 最新目標覆寫、HTTP 世代／序號／期限、相容 UART SET 並提供 SET2。
- 非阻塞 LoRa ACK、服務間隔與操作耗時診斷。
- 新增 Client／Server 開機 OLED、序列紀錄與 Server API／網頁版本顯示。

### 文件與相容性

- 建立版本唯一來源、版本紀錄與 API／OLED／網頁版本顯示說明。
- 修正硬體接線文件：Servo 實際直接使用 2S、2000 mAh、35C 電池，不經 UBEC。
- HTTP 動作需要世代、序號與板上時間；手動角度 API 不再自動切換模式。
  從先前韌體升級後，瀏覽器需重新整理；舊 UART `SET` 保持相容。

本次標版不調整 Servo 控制參數、LoRa wire 格式或 NVS 格式。

### 驗證與部署（2026-09-09）

- Client、Server、Server OTA 三個環境編譯通過，產物已確認包含 `0.1` 版本資訊。
- 11 個滑桿案例、完整設定頁模擬、版本顯示與舊韌體回應相容檢查通過；
  8 個 API JSON 範例與本地文件連結檢查通過。
- 共用控制後端先前已通過 52 個 native 測試；本次只加版本識別，未重跑控制測試。
- **0.1 未單獨上傳 Client／Server**：當時 Server 無法連線、USB 未接；Server 後續
  直接更新為 0.2。更早共用後端的 OTA 成功紀錄屬於未標版韌體。

### 已知限制

- Servo 移動時抖動、固定時偶爾抖一下，原因尚未確認；目前沒有動態供電或機構振動量測。
  此版本不宣稱已修復抖動。
- 開機第一次歸中 90° 沒有起始位置回授，無法保證物理速度／平滑度。
- 舊 UART `SET` 與 LoRa 封包缺少完整來源時間資訊，無法保證排除所有傳送前積壓資料。

實機部署與尚未驗收項目見 [健檢紀錄](docs/health-check-2026-09-08.md)。
