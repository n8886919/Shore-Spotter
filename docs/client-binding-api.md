# App 的 Client 綁定 API

本文件描述 **1.0／`api_version=2` 現行原始碼契約（2026-10-28）**；BLE 行為沿用 0.7.0-dev。
iOS／Android App 透過 Wi-Fi HTTP 存取 Station，由 Station 以 BLE／LoRa 與 Client 溝通。
Ready 的 GPS 關閉、LoRa 休眠，沒有每 30 秒 LoRa STATE 輪詢；舊 0.6.1 的等待方式不適用。
兩端須使用支援 BLE Ready 的韌體；0.7.0-dev 的既有部署證據見 [BLE 控制與換頻](ble-control-2026-10-27.md)，
1.0 雙端部署、後續 Client UI=6 與驗收界線見 [維護紀錄](t096-maintenance-2026-10-28.md) 最新節。

本文件涵蓋單一 Client 綁定及狀態判斷。完整欄位見 [interface.md](interface.md)，
可匯入的規格見 [綁定 OpenAPI](openapi-client-binding.json)；控制／選頻另見 [控制 OpenAPI](openapi-client-control.json)。

## 請求與回應

`baseUrl` 由使用者選定的 Station IP／hostname 決定，不把特定 Station 或 Client 寫死在 App。
App 必須能連到該 Station 的區域網路；本文不代表已驗收手機的本地網路權限、HTTP 政策或裝置探索。
原生 HTTP client 可使用下列請求；WebView／瀏覽器跨來源存取另需驗證 CORS，不假設韌體已支援。

| 操作 | 請求 |
|---|---|
| 讀取綁定 | `GET /api/whitelist` |
| 指定或替換 Client | `POST /api/whitelist?action=set&id=FB27` |
| 解除綁定 | `POST /api/whitelist?action=clear` |
| 讀取 Client 狀態來源、控制能力與操作快照 | `GET /api/client/control` |

POST 使用 **URL query 參數及空 body**；不要送 JSON body 取代 query。
這些綁定請求不需要 Servo 命令的 `boot_id`／`generation`／`seq`。
目前沒有應用層認證；網路存取界線見 [HTTP 與 OTA 存取](interface.md#http-與-ota-存取)。

Client ID 是 **1–4 碼十六進位字串**，大小寫皆可；App 建議顯示大寫四碼。
`FB27` 只是範例。不要加 `0x`、空白或傳十進位字串；`0000`、`0010`、`FFFF` 保留不可用。
`GET /api/client/control` 的 `client_id` 則是十進位 JSON 整數；`FB27` 對應 `64295`。

讀取／寫入成功皆回 **HTTP 200**、`Content-Type: application/json`：

```json
{"whitelist":["FB27"],"count":1,"capacity":1}
```

解除後回 `{"whitelist":[],"count":0,"capacity":1}`。容量固定一台；成功寫入保存於 Station NVS，
重開機仍保留。實際改綁／解除會回手動模式、停止沿用舊 Client 的位置／遙測／診斷／滾動統計。
同 ID 重送為 no-op，不重置這些狀態，也不重寫 NVS；讀取仍使用 GET。
保存新 ID 後 Station 自動搜尋該 Client；保存 ID 本身不代表已連線或取得控制權。

| HTTP | App 處理 |
|---|---|
| 200 | 顯示「綁定已儲存」，核對回傳 ID；尚不代表 Client 在線 |
| 400 | 檢查 action／ID 格式及保留值；顯示 `error`，不可當成功 |
| 409 | `action=add` 無法加入不同的第二台；替換請用 `set`。實際改綁／解除遇到命令 pending、掃描或 TX 進行中回 `operation_busy`，等操作結束再處理 |
| 503 | BLE 寫入鎖忙碌或 NVS 寫入失敗；綁定與舊 Client 的 RAM 狀態保持不變 |
| 網路逾時／斷線 | 結果未知；先 GET 查目前綁定，再由使用者決定是否重試 |

錯誤 body 為 `{"ok":false,"error":"..."}`，成功 body 沒有 `ok` 欄位。
保留相容操作 `action=add&id=...`、`action=remove&id=...`；remove 不同 ID 不會解除目前綁定。

## App 狀態流程

1. 進入設定頁先 GET 綁定，顯示目前 ID 或「未綁定」。
2. 使用者儲存後送一次 POST `action=set`；HTTP 200 且回傳 ID 一致才標記「已儲存」。
3. 再 GET `/api/client/control`，例如每 5 秒一次；同一 Station 的請求依序送出，前次完成才送下一次。
4. 確認 `api_version=2`、`client_id` 等於目標 ID，再依下表區分「有新鮮狀態」與「可以控制」。
5. 尚未收到新鮮狀態時顯示「已儲存，Client 尚未回報」，保留綁定。Ready 未連線時每約 10 秒廣播，
   Station 連線後約每秒讀狀態；`ble_advertising_ms=10000` 是廣播間隔，不是連線期限。
   沒有回報不能推斷 Client 關機。65 秒是已受理控制命令的期限，不是綁定或 BLE 首次連線期限。

| 欄位 | App 判斷 |
|---|---|
| `supported` | 有可顯示的新鮮 BLE 狀態或 LoRa STATE；不代表已認領或可以發命令 |
| `state_source=ble` | BLE 仍連線，ID／狀態順序有效，最近一次 Client boot 或 uptime 前進距今小於 3000 ms；重讀同一 uptime 不延長期限 |
| `state_source=lora` | 沒有合格的新鮮 BLE 狀態時，使用接收未滿 90000 ms 的 LoRa STATE；已知被其他 Station 擁有時不沿用它 |
| `state_source=none` | `supported=false`、`state=unknown`，`boot_id`／`age_ms`／`battery_mv`／`charging` 為 null |
| `age_ms` | 目前選用來源的年齡；BLE 以 boot／uptime 推進計時，LoRa 以 STATE 接收計時，不是 GPS 定位年齡 |
| `ble.claimed` | 新鮮 BLE 狀態的 owner 與目前 Station ID、Station boot 都相符；單看 `connected` 不足 |
| `can_control` | 沒有 pending／掃描／TX，且有本 Station 的新鮮 BLE 認領，或相符 boot／command／owner 的活躍 Tracking／Test LoRa STATE；特定 action 仍可能因頻道條件回 409 |
| `ready_poll_ms` | 固定 0；Ready 沒有定期 LoRa STATE 或接收窗口 |

BLE 狀態優先於 LoRa 顯示，但 BLE 不刷新 LoRa RF 品質／活動時間。`ble.owner_station_id` 可保留舊值，
不能單靠它判定目前已認領；`ble.state=owned_elsewhere` 表示觀察到其他 Station 的 owner。

改綁不會替 Client 開啟 GPS，也不會移轉其他 Station 已取得的 Client 控制權。
owner 固定於 Client 本次開機；回 Ready、BLE 斷線或 Station 重啟不解除 owner，換 Station 須重啟 Client。
啟動 GPS 使用另外的 `POST /api/client/control?action=start`；其 **HTTP 202 代表 pending**，
App 應記錄回傳 `station_boot_id`＋`operation_id`，之後 GET 比對同一操作，再等 `command=confirmed`。
Start 必須收到匹配 Client boot／owner／command 的 LoRa STATE，並驗證選定頻道後才確認；
BLE accepted 不足以確認 Start。`error`／`timeout` 分開顯示；這是最新操作快照，沒有操作歷史或冪等重送服務。

## 呼叫範例

```sh
# 換成使用者選定的 Station 位址。
STATION_URL='http://192.168.x.x'
curl "$STATION_URL/api/whitelist"
curl -X POST "$STATION_URL/api/whitelist?action=set&id=FB27"
curl "$STATION_URL/api/client/control"
```

OpenAPI 是上述端點的子集，非完整 Station API，也不是韌體上新增的下載路由。
測試 `python3 tools/test_client_binding_api.py` 會編譯實際讀寫 handler／NVS 流程，
驗證回應、格式錯誤、替換／解除、保存失敗與重新載入；不會呼叫實機或更動綁定。
2026-10-27 改綁與 Ready STATE 為當時版本的歷史實機紀錄；0.7.0-dev 的 2026-10-28 部署與驗收界線
見 [BLE 版本紀錄](ble-control-2026-10-27.md)。本次文件核對不新增實機證據；尚未實作或驗收 iOS／Android App。
