# Station Web 逾時與前景恢復修復（2026-11-13，已部署 584C）

## 問題與證據界線

使用者回報戶外 Web 的 Station GPS／Client GPS／LoRa 同時過期，LCD 仍顯示有連接；
之後確認曾切 App／鎖屏。Servo 不動已由使用者解決，不納入本次修復。
戶外時間依最後確認為 **11/12 23:39～11/13 00:09（Asia/Taipei）**；00:12 的 Station boot 已在回家之後。

SD 的 0066／0067 場次顯示，HTTP 計數曾長時間不增加，期間接受的 DATA 仍增加；
0067 的新 Station boot 之後，Client 仍以原 boot／owner 回報，約十分鐘有 1020 筆 raw DATA、58 筆 HTTP。
部分請求約每分鐘成批出現，符合背景頁面暫停的可能性，不能單憑記錄證明瀏覽器或 Station 的唯一根因。
場次之間與回家前仍有記錄缺口，不能推定整段外測 RF／GPS 連續正常。
原始 SD／HTTP／位置資料留在本機私有 state，未納入 repository。

實際嵌入頁面可重現一個獨立缺陷：fetch 或 JSON 本文在 abort 後仍不結束時，
原程式雖拒絕呼叫者的 Promise，卻保留 `httpActive`，之後所有輪詢與控制都持續排隊逾時。
Station 的 `clock_ms` 隨重啟歸零本身不是此缺陷；新 boot 原本就能被接受。
尚未實機重現「開啟回報後重啟 Station」一定造成故障，也未認定為 BLE／LoRa 開啟回報流程的 bug。

## 修正

- 本次交換由自身期限結束，取消不依賴 fetch／本文 Promise 是否結束；遲到回覆隔離。
  正常時仍單筆序列處理完整本文；逾時後有 1 秒冷卻，不承諾強制終止瀏覽器底層 TCP。
- 背景化／pagehide 取消舊讀取、排隊寫入與尚未送出的手勢。前景／pageshow persisted／online 合併重讀狀態，
  等拒絕的呼叫者釋放 busy flag 後才開始，避免被舊 `refreshing` 擋住。
- 恢復只重新讀取；POST 不重試。已送出的請求可能已在 Station 執行，取消不代表撤銷；
  操作未確認時顯示原因，必須根據最新狀態重新明確操作。
  控制意圖增加頁面同步世代，避免等候中的舊操作在恢復後才送出；後端期限與新鮮度 gate 不變。
- 保留 HTTPS 手機定位輔助頁送新位置給背景 opener 的既有流程；只放行該類新位置 POST，
  重新同步丟棄舊待送位置，原 origin／popup 身分／位置驗證不變。
- 清除舊資料新鮮度、重新取得新 boot；不改追蹤／Client 狀態、角度、RF、GPS、BLE 或版面。
  頁面 RAM 留 16 筆截短 HTTP 失敗摘要 `httpFailures`，不保存 query／body／位置或自動上傳。

## 驗證與部署

- `node tools/test_http_recovery.js` **13／13 通過**，使用實際嵌入腳本、虛擬時間與模擬 fetch，涵蓋卡住的 fetch／本文、
  不支援或忽略 abort、遲到回覆與新 boot 隔離、背景計時器延遲、完整本文序列化、優先順序、讀取去重、
  過期排隊控制、POST 不重送、前景／快取恢復／online、手勢取消、HTTPS 手機定位背景 opener、重試頻率與錯誤摘要上限。
- 同一組卡住 fetch 的案例對修復前備份為 2／2 失敗，失敗點均為逾時仍保留 active slot。
- 既有 motion、slider、ring、radar layout、Client control、BLE、Station extensions、debug、MCU temperature UI
  **9 組回歸通過**（其中 slider 10／10）；舊版獨立 UI 模擬器補上 DOM 事件介面，不更動既有驗證內容。
- `SHORE_DEVICE=station-lilygo-584c pio run -e tbeam-station`，使用獨立 build 目錄通過；
  RAM 87,920 bytes、Flash 1,931,614 bytes；application binary 1,932,016 bytes，SHA-256：
  `c8546b0c772d660ed18553e3a00e307aa2ff8be1893bf1e4d484fa1fe6a4e26c`。
  已核對 binary 內完整 HTML 與最後測試來源完全一致，HTML／CSS 與修復前逐字相同；`git diff --check` 通過。
  Build／測試摘要與輸入設定雜湊留在本機私有暫存目錄，未輸出 SSID／密碼。

**以上為程式修復階段的主機驗證，當時未燒錄／重啟或送控制指令。**
後續使用者明確授權「燒錄」，部署同一已驗證產物，結果如下；保留工作目錄 WIP 與私有設定。
實機待驗：保持 Client 位置回報，依序測 Station 重啟、手機切 App／鎖屏與回前景，
確認三燈依新鮮資料恢復，且不產生重複控制命令；目前不能宣稱戶外問題已驗收通過。

## 使用者授權後部署（2026-11-13）

- 核對 USB VID:PID `303a:1001`、serial／MAC `48:CA:43:57:58:4C`、hostname `shore-48ca4357584c`，
  舊 boot `767967084`。產物與測試來源／設定雜湊一致，Wi-Fi SSID／密碼值及 binary 中的值
  與先前成功部署版一致；未在輸出或文件列出敏感值。
- USB `SD STOP` 完成：11,769,855 bytes 全部同步，`unconfirmed_bytes=0`、`usb_pending=false`。
  讀回分割表／NVS／otadata，與 build 分割表相同，確認目前 app1 位於 `0x340000`、大小 `0x330000`。
- 備份的大筆 stub 讀取在相同區段中斷；改採 ROM 短讀完成 3,342,336-byte application 備份，
  `verify-flash` MD5 核對通過。舊 image 的 SHA-256 與上次成功部署 `f0e99243…bdeb90` 相同。
  此為 USB 備份工具的觀察，不能當作戶外故障的根因。
- 僅寫入 app1 application，未擦除 NVS／partition／其他 app。寫入 1,932,016 bytes，
  SHA-256 同上 `c8546b0c…a4e26c`；Flash hash 驗證通過，`watchdog-reset` 重啟。
- 原網路恢復，韌體 1.0、build `Nov 13 2026 01:13:33`，新 boot **352284237**。
  實際 HTML **94,594 bytes**，去除外圍空白後與已測試來源逐 byte 相同；包含本次 HTTP 恢復與先前 RSSI／SNR 分級 WIP。
  手機／WebView 需重新整理一次才會執行新版腳本。
- Client 綁定 F035／61493、B／923.8 MHz 保留；Client boot **1614113903** 未變。
  最終 BLE connected／claimed、owner 584C、Ready、`can_control=true`、age 51 ms。
  **這次自動重連較慢**：首輪 90 秒等待未完成認領，約 Station uptime 141 秒首次觀察到已連線／認領。
  這不是精確重連耗時量測，也不表示 BLE 重連速度改善已驗收。
- Servo 由重啟前 manual／259°，依既有開機行為回 manual／179.5°；速限 60°/s 保留。
  最終 `command_seq=0`、UART `tx_commands=1`（開機中位）；本輪未送 HTTP Servo／Client 操作指令，未更新或重啟 Client。

### SD 驗收限制與後續

SD 自動恢復 `/logs/000000000000006f-0000.ndjson`、recording／enabled，I/O errors、USB write errors 均 0。
最後兩次取樣的 texts **184→185**、bytes **496,785→497,743**、synced bytes **496,471→497,569**，
`unconfirmed_bytes=0`，證明文字持續寫入與同步。

但 **samples 114→114、sample_dropped 增加 3（累計 145）**，不能宣稱快照記錄已完整恢復。
`src/sd_log.cpp::captureDue` 在 busy 或 free heap <32,768 bytes 時跳過快照；
當時 API 顯示 heap_free 27,152 bytes，但 API 取樣與快照判定時點不同，尚未區分忙碌與 heap gate 的實際占比。
本輪核對 main／SD／SDK log backend 與上次成功部署來源相同，沒有調低門檻或靜默忽略失敗。
後續需分別查 BLE 重連延遲與 SD 快照跳過；戶外／背景恢復仍待實機驗收。

原始 API、Flash／ROM 備份與校驗 log、產物與驗證腳本保存於本機私有
`~/.local/state/shore-spotter/web-recovery-deploy-20261113T011754+0800/`；未 commit／push。
