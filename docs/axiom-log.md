# Axiom 背景除錯上傳

> 2026-09-22：除錯頁與 Axiom 表單已移除；現行使用下列 HTTP API。

Station 專用，**預設關閉**。啟用後不需要保持網頁開著，Station 直接經 Wi-Fi／網際網路
傳到 Axiom；上傳功能本身不要求 Client 參與。當前韌體另有 DIAG 頻率旗標擴充，見介面文件。這是有容量上限、允許遺失的診斷串流。

## 註冊後設定

1. 在 Axiom 建立 **Events dataset**，名稱固定為 `shore-spotter`，選 **US East 1**。
2. 到 **Settings → API tokens → New API token**，使用 **Basic**，Dataset access
   只選該 dataset。需要 ingest API token；不是 Personal Access Token。
3. 由電腦以 `POST /api/axiom` 的 JSON body 設定 `enabled`、`dataset`、`region`、`token`；
   詳見 [API](interface.md#get-apiaxiom--post-apiaxiom)。不得把 Token 放在 URL 或公開命令輸出。
4. 以 `GET /api/axiom` 確認非同步儲存完成、送出筆數及最近成功時間。

Token 留空會沿用既有值；`clear_token=true` 可清除，`enabled=false` 可停用。
設定保留於 NVS。移除網頁不會自動關閉原本已啟用的上傳。
關閉／換設定／OTA 會停止新的上傳；**已在傳送的請求可能完成**。

截至 **2026-09-15**，官方 Personal 方案列出 25 GB 儲存、500 GB／月資料載入、
最多 30 天保留、3 datasets、每 dataset 256 fields；免費額度和保留天數是不同限制。
本機設定不能限制 Axiom 帳單，實際方案與用量以帳號的 Settings → Usage 為準。
來源：[Axiom limits](https://axiom.co/docs/reference/limits)、
[API tokens](https://axiom.co/docs/reference/tokens)、
[資料上傳](https://axiom.co/docs/restapi/ingest)。

## 傳哪些資料

每秒最多一筆 `station_snapshot`，雲端維持 `schema_version = 2`。每筆保留完整結構化欄位，另有英文
`message` 摘要、`level`（`info`／`warn`／`error`）、`client_link` 與 `delta`。正常摘要例如：

```text
uart / waiting | No Client data | Station GPS no_fix | Commanded 90.0 deg
```

- UART 待命與尚未收到 Client 不會單獨當成故障。PWM 不可用／motion fault 為 `error`；
  Client 已收過資料但年齡達 2000 ms、GPS 模式條件不足，或這段區間有新增錯誤為 `warn`。
- `client_link` 為 `not_received`／`receiving`／`stale`，表示 Station 的收包狀態；目前 Client 單向傳送、不接收 ACK。
- Client 收包、Station GNSS 狀態、模式、控制來源、GPS 可用性、PWM 與 motion fault 有變化時，
  同一快照最多額外送一筆 `kind = state_change`，`changes` 陣列保留各欄位的 `from`／`to`。
  不變時只送快照；第一次觀察建立基準，不把未知的舊狀態當成斷線或恢復。
- `delta` 記錄 RF 接收錯誤、控制間隔超過 250 ms、Station GPS checksum 錯誤的增量；
  舊 `ack_errors` 欄位固定為 null，表示不適用，
  `interval_ms` 為兩筆成功編碼快照的本機時間差，不保證剛好 1 秒。累計值仍保留。
  初次觀察、重啟、重新設定或換綁定時增量為 `null`，重新建立基準；uint32 計數與時鐘可回繞。
- 尚未收到 Client／遠端診斷的測量與計數輸出 `null`，`client_gnss.state` 為 `unknown`。
  收過的舊報告仍保留數值與接收年齡，不把它當成本秒量測。
- 摘要、差分與事件編碼都在背景 worker；主迴圈仍只複製固定 1032-byte 主機快照（本版新增岸端平均／GNSS 頻率）。
  狀態事件取決於取樣觀察，短於取樣間隔的變化可能看不到；上傳失敗時事件也可能遺失，不補傳。

在 Axiom Stream 的顯示設定選 `_time`、`level`、`message`、`kind`，關閉 raw event details，
並開啟 severity highlight。詳細欄位仍可展開查詢。只看狀態變化可用：

```text
['shore-spotter']
| where kind == "state_change"
| project _time, level, message, changes
| order by _time desc
```

快照完整欄位包含：

| 區塊 | 內容／用途 |
|---|---|
| 識別 | firmware、build、protocol、node／boot／sample ID、Station `millis()` |
| health | free／minimum／largest heap、重啟原因、Wi-Fi RSSI、雙端電池電壓 |
| station／gnss_rate | 30 秒平均／樣本數／RMS／警告，以及請求與實測 epoch／RMC／GGA Hz；server_gps 座標仍是原始值 |
| server_gps／server_gnss | 雲端 schema 2 相容欄位：Station 定位與 HDOP、新鮮度、epoch 間隔、RMC／GGA／checksum／積壓／重同步 |
| client／client_gnss／client_diag | 最近收到的位置與品質、來源／接收年齡、低頻 GNSS 與 TX 診斷 |
| control | 模式、來源、UART 狀態、軟體角度／目標／限速／速度、GPS 預測、指南針參考、fault |
| timing | 主迴圈、HTTP、LoRa、motion、I2C、OTA 耗時與最慢 HTTP 請求分段 |
| radio | 收件、錯誤、拒收、序號缺口／重同步、radio recovery 計數；ack_enabled=false，舊 ACK 計數為 null |
| events | 每筆快照最多 8 個增量封包事件，保留 id、時間、raw hex、RSSI／SNR／錯誤碼 |
| upload | 丟棄快照數、採樣最長耗時、封包事件游標遺失數 |

使用固定欄位，主機 fixture 計得 **222 個含父層的欄位路徑**（包含狀態事件），低於 256。
Station 改名後，Axiom 專用編碼保留原本 `server_gps`、`server_gnss`、`station` 路徑與
`changes.field = server_gnss` 相容值，避免同一 dataset 保留舊欄位又新增完整新路徑，
導致欄位上限超標；沒有更換 dataset、Token 或既有 NVS 設定。新記錄的 kind 與顯示文字使用
Station，跨版本查詢快照須同時包含歷史 `server_snapshot` 及新的 `station_snapshot`。
本地 SD 使用 schema 3 的 `station_gps`、`station_gnss`、`station_average`，與雲端相容模式分開。
不將 Token、Wi-Fi 帳密、IP／MAC 或整份序列文字 log 放進雲端 JSON；結構化診斷內有
GPS 座標與封包。瀏覽器的文字 log／備註／GPX 錄製仍是原有獨立功能。

`_time` 為 RFC3339 UTC 毫秒：啟用後用 SNTP 校時，背景工作以當下 UTC 扣除快照年齡。
它是 Station 採樣時間的估計，**不是 GNSS 量測時間**。未完成校時不傳送。
使用 `(node_id, boot_id, sample_id, kind)` 識別快照與狀態事件，`(node_id, boot_id, events.id)` 識別封包事件；
node ID 為既有 16-bit 識別碼，並非全球唯一。跨 boot 分段，`sample_ms` 可回繞。
Client 的診斷仍是低頻舊快照：`*_rx_age_ms = null` 代表未收到，不能將初始化的 0 計數
當成實測。GNSS 短年齡／計數有 16-bit 飽和限制，詳見 [debug-export.md](debug-export.md)。
Servo 角度／速度沒有實體回饋。舊版歷史紀錄的 ACK sent 也不代表 Client 確實收到；
目前 ACK 已移除，不再把 ACK 欄位當作連線或錯誤指標。低頻診斷約每 60 秒更新，
在 SF10 下獨立排程，即使無定位也發送，可能延後一筆 DATA。GPS 請求 2 Hz 的雙語句驗證版；
DATA 只送最新有效定位，失效只通知一次，不重傳舊定位。
應同看序號缺口，不能把低於 2 Hz 算成漏包。`skipped_slots` 舊欄名在本版表示 DATA
因 radio 忙碌／未就緒而延期的工作次數，同一次等待只計一次。

## 效能與遺失策略

- loop 每秒只複製固定大小 POD 快照與最多 8 個事件，使用 **零等待**佇列操作。
  JSON 編碼、TLS、DNS、HTTP 與設定的 NVS 寫入都在另一核心的低優先權 FreeRTOS task。
- 全新且停用時不建立背景 task／queue、不校時、不發出 Axiom 請求。第一次設定／啟用後，
  queue（8 × `sizeof(Sample)`，主機測得每筆 1032 bytes，加 FreeRTOS metadata）與 12 KiB task
  stack 保留到重開機；停用會排空 queue、停止 SNTP、釋放 32 KiB batch buffer。
  TLS／網路工作另有動態記憶體成本。板上 `sample_bytes` 會回報實際快照大小。
- 約每 5 秒一批，最多 5 筆快照及其狀態事件（最多 10 筆文件）、32 KiB NDJSON。不是每包 LoRa 都建立一次 HTTPS。
  目前主機 fixture 完整快照約 5.1 KB，含同時七項狀態變化的一組約 5.8 KB；
  真實大小依欄位值及封包數而異，不等於 Axiom 計費量或儲存量。
- queue 固定 8 筆，滿時丟棄新快照；傳送前丟棄超過 15 秒或舊設定世代的快照。
  不寫 flash log、不補傳整場斷線資料。啟用後可能帶上原有 64 筆 ring 的近期封包事件，
  不會占用／推進瀏覽器的事件游標。
- RF IRQ 待處理、前次 loop 超過 20 ms、採樣時 heap 低於 48 KiB 時跳過採樣。
  TLS 啟動前至少保留 100 KiB free heap 與 48 KiB 最大連續區塊；batch 尚未配置時另加 32 KiB。
- HTTP 網路操作 timeout 設 3 秒；DNS／TLS／多次網路操作的總時間可能更長，但不在控制 loop 等待。
  失敗批次丟棄，後續批次按 5、10、20…300 秒退避；429／503 的 `Retry-After` 秒數或
  HTTP-date 會延長等待，最大 24 小時。400／401／403／404／413／422 停止上傳，需修正後重新儲存。
- 只有 HTTP 200 且回覆 `ingested`／`failed` 合計吻合才計算實際成功筆數。
  部分失敗、超長／無效回覆、連線結果不明不會被當作整批成功；失敗批次不重送。
  網路失敗仍可能已有部分資料到達 Axiom，`failed_samples` 表示本機未確認成功的筆數。
- API 保留既有計數名稱：`sent_samples`／`failed_samples` 依 Axiom 回覆計算文件筆數，
  包含快照與狀態事件；`dropped_samples` 合計略過／排空的快照及已編碼卻未確認收件的文件，
  不能直接用來算快照丟失率。
  `event_lost` 另表示採樣游標落後原有 packet ring 的事件數，兩者不可相加當成封包遺失數。

這些措施限制上傳工作量；兩個核心仍共用記憶體、Wi-Fi 與 flash，不能宣稱完全零影響。
**2026-09-15 已完成 schema 2 Station OTA 更新**，板上 build `Sep 15 2026 19:44:40`，
Token／啟用／綁定／控制設定保留。約 30 秒實機觀察中 HTTP 200、確認收件 6→36 筆，
失敗／丟棄均 0；worker 最低剩餘 stack 6,100 bytes，control gap max 123 ms。
Station 維持 UART waiting／hold、90°、PWM 正常。收件證據來自 Station 驗證 Axiom ingest 回覆；
未直接查詢雲端文件內容，也未測試戶外 GPS／LoRa 負載、實體追蹤或長時間啟用性能。
實機啟用後，應比較關閉／開啟相同長度區間的 `timing` 增量、GPS backlog、RF errors、
heap、`capture_max_us`、`worker_stack_free_min`，再決定是否提高取樣率。

## 設定與 API

設定在 Station 的獨立 NVS namespace `axiom_log`、key `config`，單一有版本 magic 的 blob。
讀不到、長度不符或驗證失敗時回到預設關閉。寫入後讀回確認，失敗維持暫停並顯示錯誤。
Token 寫入裝置 NVS；現有網頁是熱點內 HTTP，雲端上傳則使用驗證 CA／hostname 的 HTTPS。
禁止 TLS insecure fallback 與自動 redirect；目的地只能是 Axiom 官方 US／EU edge。

API 契約見 [interface.md](interface.md#get-apiaxiom--post-apiaxiom)。
網頁 GET／POST 與其他 API 共用全頁請求佇列，Servo 控制優先。

## 驗證指令

```sh
python3 tools/test_axiom_backend.py
node tools/test_axiom_ui.js
pio test -e native
pio run -e tbeam-client -e tbeam-station -e tbeam-station-ota
```

後端測試編譯實際 worker、encoder 與 HTTP handler，以 fake Wi-Fi／TLS／NVS／FreeRTOS
驗證預設關閉、成功／部分失敗／超時／限流、佇列上限與過期、Token 保留／清除、
設定失敗、OTA、記憶體不足與傳送中關閉。需要已安裝的 PlatformIO ESP32 cJSON header
和 Linux `libcjson` runtime。這些是主機模擬，不是實機 TLS／時序證據。
