# 2026-11-14 兩端部署與 App 0.3 最近 IP

使用者授權「現在可以燒與安裝app」，並確認舵機已斷電、T096已接好；後續要求IP輸入框下方列出最近兩個成功IP供快選。
本輪使用專案Android skill；所有ADB明確指定當次Pixel 10，韌體依當次USB serial核對，沒有操作其他App或全域網路／GPS設定。

## 產物與部署

- 健檢firmware來源60d5d7b，保存於 `~/.local/state/shore-spotter/health-fixes-20261114T011939+0800/`；各hash及私人Wi-Fi fingerprint再核對相符。
- Station 584C先讀回原build `Nov 13 2026 01:13:33`、boot3199757790、F035配對、B／SF9、manual／60°/s。
  SD STOP同步bytes=synced_bytes=2823337，errors=0。讀出metadata及現役application，備份hash與前次部署c8546b0c…相符。
  依目前partition表與兩份OTA CRC／seq3、4確認active app1@0x340000，僅寫此application，不改NVS／partition／SD。
  esptool115200、hash verified、watchdog-reset成功；新build `Nov 14 2026 01:21:02`、boot3950833316。
- T096 F035當次USB VID:PID239A:8071，舊boot60356C6F；保存DIAG STATUS／4筆被拒NMEA。
  1200bps touch時USB斷開造成BrokenPipe，重新辨識同serial已為239A:0071後才DFU，沒有盲重試touch。
  application-only ZIP CRC與manifest核對；DFU回覆Device programmed；重新辨識8071，USB新build `Nov 14 2026 01:21:52`、boot82F0D506。
  Station後續讀回同新boot2196821254、Ready、can_control、BLE connected／claimed、owner584C。沒有自動Start／Test／RF移動驗收。
- Station仍manual、60°/s、command_seq=0，開機UART中位tx_commands=1；舵機斷電，不能宣稱實際回中。
- App原安裝0.1先拉回備份；先更新0.2驗證，再依新增需求更新0.3／versionCode3，兩次拉回APK皆與各自產物hash相符，保留資料。
  App0.3來源commit f14db44，SHA256 `51df3ec533560626ba406a15b9ed2d66cc58780a72996cbc4a98334381d5b8ba`。

## Android新增與驗證

IP欄下方最多兩個不同地址，最新在左，點擊沿用既有連線驗證；Station識別及完整poll成功、目前世代有效才寫入兩個本機prefs。
失敗／部分GET成功／背景／過期不入列，相同IP不重寫。沒有補造第二筆或將舊輸入當成功歷史。

- assembleDebug、5個JUnit、實際方法integration、lint0 errors／26 warnings、4 JNI16KB／zipalign通過。
- Pixel10／Android17：第一個近期IP按鈕在輸入框下出現，點選後回Station控制；結束App後讀回prefs確認保留。
  第二個不同網路IP未真機測；兩筆去重／排序／失敗不入列已有主機回歸。
- 0.2背景錄影離開→保存→回前景→再錄成功，第二段暫停／繼續成功；兩段1080p約30FPS、無音軌。
- 0.3再測同樣背景保存→回前景→再錄，兩段33.3444s／19.7733s，1920×1080、90000/3001FPS、無音軌、MediaStore非pending。
- 只讀／拉取本次App新建影片，測完force-stop本App釋放相機／定位。沒有取得衛星fix；GPS-only不代表戶外定位已驗收。

## SD與已知缺口

新SD `/logs/0000000000000081-0000.ndjson` 匯出147342bytes、CRC32 f27cbc08，20個快照含station_position（當時gnss來源，無有效fix）。
匯出後SD START，讀回新檔recording、文字與bytes／synced_bytes持續增加、errors=0；沒有格式化或刪除卡內紀錄。
USB剛重新開啟時兩次讀到先前錯誤行（rx_hex7d0d／0d），保留原始資料並先讀完pending輸出後取得完整STATUS，匯出CRC成功。

**不能宣稱快照持續性正常**：同一boot samples停在20，sample_dropped持續增加到501；heap_free21048、largest7668，低於SDcapture的32768門檻。
Axiom enabled、state low_memory、requests0、sent_samples0。可解釋當時快照略過，但尚未定位造成佔用的來源，不能當作歷史panic根因。
USBwrite_errors3，後段未增加。外測前仍應處理位置快照缺口；不擅自改Axiom設定或降低記憶體門檻。

## 保存與交接

本機私有證據 `~/.local/state/shore-spotter/deployment-20261114T013211/`：deployment-summary.json、舊APK／Stationapplication及metadata備份、DFU／flashlog、前後API／USB、SD原始匯出、新APK、測試影片／ffprobe、IP對話框截圖與prefs。
位置／私人IP／畫面／憑證／二進位留本機，不提交公開repo。T096前次同板DFUZIP亦核對hash保留作回復參考。
目前規劃與部署界線更新於assistant-handoff及next-session-plan；戶外GPS／RF、跳角與panic、機械動作仍未驗收。
