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

## Station panic：保留 Flash coredump

2026-11-14 主機查核：Station 584C 當時的 SDK 已配置 Flash／ELF／CRC32 coredump，
對應 `firmware.elf` 已連結啟動檢查與 panic writer；**尚未驗證真實 panic 成功保存及解碼**。
使用既有 SDK 與主機工具，不新增韌體 buffer／worker，也不故意觸發 panic。

先保存當時部署的 `firmware.bin`、配對 `firmware.elf` 與 SHA-256，記錄 build／boot／操作時間；
不能用之後重建的 ELF 代替。先匯出 SD，再送 `SD STOP`，等待回覆 `state=stopped`、
`enabled=false`、`bytes=synced_bytes`、`unconfirmed_bytes=0`，並核對錯誤；未同步完成不進 bootloader。
重新辨識 USB 目標、晶片及實際分區表後才讀 Flash，不沿用 `/dev/ttyACM*` 順序。

讀取會重啟並中斷 Station，恢復韌體可能輸出 Servo 中位命令；操作前須確認當下已允許重啟，
且 Servo 斷電或機構處於已確認可安全重啟的狀態。本輪已有 Servo 斷電／重啟授權，不延伸到未來操作。
以下偏移 `0x7f0000`／容量 `0x10000`（64 KiB）**僅限已核對的 584C 分區**，其他板須換成其實際值。

```sh
PY=/home/nolanasd123/.platformio/penv/bin/python
PORT=/dev/serial/by-id/已重新核對的Station
CHIP=esp32s3
OFFSET=0x7f0000
SIZE=0x10000
OUT=/絕對路徑/本次新建的證據目錄
ELF=/絕對路徑/當時部署的firmware.elf
BIN=/絕對路徑/當時部署的firmware.bin
GDB=/home/nolanasd123/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb
mkdir "$OUT"
cp "$BIN" "$OUT/firmware.bin"
cp "$ELF" "$OUT/firmware.elf"
sha256sum "$OUT/firmware.bin" "$OUT/firmware.elf" > "$OUT/firmware.sha256"
"$PY" tools/read_sd_log.py --port "$PORT" stop
# 確認上述同步狀態、目標／分區及重啟邊界後，才執行下一行。
"$PY" -m esptool --chip "$CHIP" --port "$PORT" --after watchdog-reset \
  read-flash "$OFFSET" "$SIZE" "$OUT/coredump-partition.bin"
sha256sum "$OUT/coredump-partition.bin" > "$OUT/coredump.sha256"
"$PY" -m esp_coredump --chip "$CHIP" info_corefile \
  --core "$OUT/coredump-partition.bin" --core-format raw --gdb "$GDB" \
  "$OUT/firmware.elf" > "$OUT/coredump-info.txt" 2>&1
```

先核對原始檔大小等於分區容量；保留完整 raw bin、hash、讀取及解碼輸出，不裁切或覆寫原檔。
全 `0xff`，或首四 byte 小端長度為 `0xffffffff`（SDK 空白標記），表示沒有可解析的當前 dump；
不能據此宣稱從未 panic。其他非法長度／CRC 失敗亦保留原始檔，不當作成功解碼。
離線命令的 `--core` 不可省略，否則工具會再開 serial；本機也未設其線上讀取所需 `IDF_PATH`。
本機已有 esp-coredump 1.16.0、esptool 5.3.0 與 GDB；ROM symbol 有需要時，另指定符合目標的 `--rom-elf`。
讀取完成應確認 Station 已回到正常韌體、新 boot 與 SD recording 狀態；`watchdog-reset` 的重啟不代表新 panic。

目前 SDK 允許下一次 panic 覆寫；boot check 只檢查 CRC，不自動清除。
64 KiB 容量不足、寫入途中斷電或未進 panic handler 的 reset／卡死，都可能沒有有效 dump。
coredump 不是整場追蹤／heap 歷史，跳角、失聯或 Servo 不動仍須配合 SD／RF／API 與實體證據判讀。

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


## Heltec V4 無SD

V4自動使用內建Flash保存逐包RF與Station/phone狀態，狀態入口為`GET /api/flash`。
使用`tools/read_diagnostic_log.py`透過USB匯出，滿即停、不自動覆蓋。容量／操作／實測限制見
[Heltec外測準備](heltec-field-ready.md)。此紀錄與瀏覽器歷史軌跡、Axiom上傳各自獨立。
