---
name: shore-spotter-android
description: Develop, install, inspect, and verify the Shore Spotter Android camera and tracking-control app on an authorized phone using Mobile MCP and ADB. Apply to this app and its Station API integration, not unrelated phone apps or firmware deployment.
---

# Shore Spotter Android

操作對象固定為 `tw.shorespotter.app`。先確認本次使用者要看 UI、開發／安裝、錄影驗收，或實際操作硬體；skill 本身不擴張授權。沿用同一 session 已有的有效授權，不重複要求確認。

## 專案與產品

- 從專案根目錄先讀 `AGENTS.md`、`android/README.md`，依需要讀 `docs/interface.md`、`docs/web-ui-design.md`；以目前 checkout／WIP 為準。歷史真機紀錄從 `docs/records-index.md` 查回。
- 使用者情境是到海邊趕快下水：相機滿版，錄影／暫停／縮放，單一 Station 按鈕展開控制／資訊；點按面板外相機預覽可收合，不重新綁定相機或改變錄影。提醒直接在影像，沒有長篇常駐說明。
- 狀態用不透明底板表示，保留粗體文字／模式勾選；就緒綠 `#7acd9c`、未確認／斷線／錯誤粉紅 `#e99a94`，第一筆定位等待為沙色，不能只改字色。紅綠已提高飽和度，完整色值與對比見 `android/README.md`，修改色值後重算，不沿用舊值；日照辨識須與室內畫面分開驗證。
- 頂列無整條背景，上方底板同高 32dp／觸控區 48dp；只留固定寬 112／128dp 的定位／Station，沒有儲存 chip，文字變動不推動位置。橫向間距 8dp、面板按鈕垂直間距 12dp。
- 一般按鈕底板維持 6dp 小圓角長方形；錄影鍵例外為半透明圓形底板，中心保留紅圓／停止方塊，沒有白色外框。
- 錄影／暫停／倍率／右下畫質／資源區均高 48dp、距底部 26dp 對齊；相機工具底板保留半透明，只有倍率／畫質有白色細框。畫質寬 128dp／距右 172dp，右側資源區寬 144dp／距右 20dp。錄影中心紅色 alpha `0xb3`、底板 `0x99`。倍率只有數字與 8dp 軌道／28dp 拖曳點，沒有 1×／2×／5×；滑桿列縮至前次寬度 80%，左側預留錄影／暫停區，再以 `max(200dp, 螢幕寬 - 464dp)` 夾限；展開面板時隱藏倍率／畫質／資源區，收合後恢復。
- 右下資源區以 13sp 上下兩行共用同一個半透明背景，沒有白框：上行接電顯示綠色閃電 icon＋N%，拔電顯示「電量 N%」，接電且滿電也保留閃電與百分比；下行剩餘 GB，不再用「充電中／已充滿」取代百分比。未連線的「Station 斷線」只開 IP 設定，不能進控制／資訊；失聯停用並收起浮層。Surfer 是 Heltec T096；兩端 SD、地圖／雷達、封包／log／原始診斷留在既有網頁，不搬進 App。
- 目前 source 包含閃電 icon／百分比、GPS-only 與最近 IP；0.3 已安裝的驗證界線以交接為準，當前狀態先看 `docs/assistant-handoff.md`／`android/README.md`。使用者收起手機時停止 ADB／Mobile MCP 與安裝；歷史真機驗收不可套用新 APK。
- 手機固定在 Station 旁提供位置。影片暫停不切追蹤模式；GPS 追蹤與錄影生命週期分開。
- 使用 App 時吞掉手機內建音量鍵，含對話框，不改音量、不當錄影鍵；外接裝置另辨識，目前未加遙控錄影。按鍵驗收只在本 App 前景，不隨意改全域音訊或配對設定。

## 連線與日常手機範圍

- 使用者指定的是日常手機。先列裝置並核對型號／系統及選定 adb ID，所有命令都明確 `-s <device>`；重連後重新辨識。無線偵錯 IP 不等於 Station IP，不把歷史 IP／port 寫死。
- Mobile MCP 在電腦端執行，現有註冊名 `shore-android-mobile`，`MOBILEMCP_DISABLE_TELEMETRY=1`。先發現實際可用工具；目前 session 未載入時，可用既有 MCP client 或 ADB 完成本 App 的必要操作，不自行重啟 Codex。
- 無線配對需要使用者提供的當次配對碼與配對 port；可用 adb mDNS 找 port。配對碼不存檔，沒有授權就不自行配對。
- ADB/MCP 有整機能力，操作範圍靠 agent 約束：只安裝／啟動／結束本 App、處理本 App 必要相機與精確位置權限、操作其 UI、驗證其新建影片。保持其他 App、個人內容、網路、鎖定與系統設定。
- 不自行解除螢幕鎖、不掃描通知／照片／聯絡人。截圖、UI tree 或觸控前確認前景是本 App；鎖定或其他 App 在前景時停止這些操作，請使用者解鎖或在既有授權內只啟動本 App。
- 使用 [scripts/probe.py](scripts/probe.py) 做唯讀辨識；它只印本 App／Station 必要欄位、排除座標，非本 App 前景會拒絕截圖：
  `python3 <skill>/scripts/probe.py --device <authorized-adb-id> [--station-ip <private-ip>] [--screenshot <new-file.png>]`

## 畫面操作與驗收

- UI 操作優先用 Mobile MCP：讀當下元素，再用 ref 點按。版面改變就重讀；不沿用上一版座標。錄影／停止／暫停／繼續用其 accessibility label；Station 按鈕展收控制與資訊整合的同一頁浮層。
- IP 欄只輸入 IP；由程式加 HTTP。沿用已確認當日 Station 地址與實機 hostname，不猜測手機熱點網段。
- IP 使用數字鍵盤且可輸入多個點，完成即連線。鍵盤出現／消失會移動對話框；等版面穩定後重讀元素，不沿用切換前 ref／座標，以免點到鍵盤或系統導覽。
- 只看 UI 或錄影測試時，不觸碰位置回報啟停、GPS／手動模式、方向滑桿、校正或速度。這些會改變設備狀態；實際操作必須在本次人類授權範圍。
- 硬體控制驗收先核對 Station／Surfer 身分與初始模式；位置回報啟停與實體舵機測試分開。結束時恢復已約定初始狀態，不自動追加模式切換或校正。
- 錄影確認同檔暫停／繼續、展收控制／資訊整合浮層不中斷、縮放、保存；只讀／拉取本次 App 已知新建的 `Movies/ShoreSpotter` 檔案，不列整個相簿。
- 用 ffprobe 核對解析度、實際 FPS、時長、無音軌；不能把畫質選單當成輸出證據。未測所有格式就明說。

## 開發與控制契約

- `android/` 是獨立 Gradle 專案；本機可用 JDK 21 與 SDK 36.1，無須啟動或重裝 Android Studio。保留本機 `local.properties`、WIP 與既有 app 資料，安裝更新不清除資料。
- 依變更跑相關 JUnit、assembleDebug／lint；功能變更同步 CHANGELOG／規格。Native UI 不代表要重編／燒錄 Station 或 Client。
- Motion 必須保留新鮮 boot／epoch、seq、stamp 與使用者意圖 2 秒期限；重新開機、重連或模式世代改變撤銷舊操作，POST 不自動重送。
- Surfer 的 HTTP 202 只受理；比對 Station boot＋operation ID 的 confirmed／error／timeout，不能只看 BLE connected、HTTP 200 或設定保存。
- 手機定位前景固定自動更新，沒有停止／開始更新按鈕；只送新且未滿 10 秒樣本，確認 API 回覆相同資料才標成功。離開前景後 Station 仍留最後 snapshot 到重開機，不擅自切 GNSS。
- 16 KB 相容性須跑 `android/tools/check_16kb.py` 與 SDK zipalign `-c -P 16 4`。這台 Android 17 即使 kernel PAGE_SIZE=4096 也曾提示舊 JNI 對齊問題；不靠隱藏警告或改手機相容模式處理。
- 收尾時停止本次測試錄影；未要求持續運行時結束本 App 釋放相機／定位，不動其他 App 或全域省電設定。報告分開說主機、App 真機／API、影片、戶外與機械證據。
