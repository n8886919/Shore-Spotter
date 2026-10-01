# 現場除錯與 log 讀取

2026-09-22 移除網頁除錯分頁、瀏覽器錄製／JSON 匯出與 GPX／Strava。
網頁只保留雷達與資訊。現場完整記錄改由 Station SD 完成，操作見 [SD 記錄](sd-log.md)。

## USB 讀回 SD

插卡開機自動記錄，不需要網路或 GPS 控制模式。接上 USB 後：

```sh
python tools/read_sd_log.py --port /dev/ttyACM0 list
python tools/read_sd_log.py --port /dev/ttyACM0 read /logs/檔名.ndjson --output /絕對路徑/現場.ndjson
python tools/read_sd_log.py --port /dev/ttyACM0 start
```

需 pyserial（本機 `~/.platformio/penv/bin/python` 已具備）。list/read 會先停止、同步並關檔，
讀回核對長度與 CRC32；最後執行 start 恢復記錄。失敗檔保留 `.partial`，不覆寫舊匯出。

## 仍可直接讀取的 HTTP

- `GET /api/status`：狀態、服務時間及最慢 HTTP 請求。
- `GET /api/track`：當前雙端位置／來源／Servo 命令狀態；不含歷史軌跡。
- `GET /api/debug?limit=8`：schema 3、最多一頁封包事件；後續帶回覆的 `boot_id`、`since=next_id`。
- `GET /api/log?from=0`：RAM 文字 ring，依回覆 cursor 增量讀取。
- `GET/POST /api/axiom`：狀態、啟停及 Token 設定；機密只放 JSON body，GET 不回 Token。

完整欄位／分頁／覆寫契約見 [介面](interface.md)。HTTP RAM ring 不是整場記錄；
SD 與 Axiom 是獨立消費者。已保存的 NVS Axiom 設定不因移除網頁而自動停用。

## 判讀界線

- 依 boot ID 分段；檢查 event ID、DATA seq、CRC／拒收原因及 queue/drop/error 計數。
- Client TX 成功不等於 Station 收到；完全未收到的 RF 封包沒有 raw bytes 可存。
- GNSS source epoch、Station 接收、SD 同步、Axiom 上傳與網頁刷新是不同階段。
- Servo angle／target 是軟體命令，沒有實際鏡頭位置回授。
- `_time=null` 為無有效系統 UTC；離線以 boot ID／ms 判讀，不把 1970 當真實時間。
- fsync 及尾端讀回不是斷電／全卡耐受證明；USB CRC 只驗證匯出傳輸一致。

入口盤點見 [功能入口](function-entrypoints.md)。
