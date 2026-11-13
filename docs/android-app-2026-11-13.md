# Android 相機／控制／定位整合（2026-11-13）

## 已確認需求

- 實測 Pixel 10，實際系統回讀 Android 17／API 37。
- 橫向、相機滿版、控制浮層按需展開；主要操作錄影／縮放／控制，不常駐說明或診斷小字。
- 不收音，解析度與 FPS 在 App 內選擇，錄影時禁止切換規格。
- 一般按鈕底板為小圓角長方形；錄影鍵為半透明圓形底板／中心紅圓，錄影中改停止方塊，沒有白色外框；獨立暫停／繼續鍵，同一影片檔不中斷分檔。
- 手機固定於 Station 旁，前景持續更新位置；家中 Station 連家裡 Wi-Fi，外出連手機熱點。
- Station IP 欄只顯示／輸入 IP，不要求 `http://`；內部仍使用現有 HTTP API。
- IP 改數字鍵盤，可輸入 IPv4 的多個點；鍵盤完成即連線，橫向不使用全螢幕文字編輯。
- 日常手機：只安裝／操作本次獨立 App，保留其他 App、資料與系統設定。

## 實作

入口 [android/README](../android/README.md)，App 套件 `tw.shorespotter.app`／版本 `0.1`。
相機使用 CameraX 1.6.2，錄影存入 MediaStore 的 Movies/ShoreSpotter，沒有 audio track 開啟呼叫或麥克風權限。
全螢幕預覽使用中央填滿；影片仍為所選標準格式。倍率以鏡頭實際提供範圍為準，移除 1／2／5 倍捷徑，保留滑桿與點按對焦。
滑桿為 8dp 軌道／28dp 拖曳點／48dp 觸控列。滑桿列縮至前次寬度的 80%，從左側 148dp 起預留錄影／暫停區，再以 `max(200dp, 螢幕寬 - 464dp)` 夾限寬度。

海邊狀態由 `FieldStyle` 共用不透明背景：就緒綠 `#7acd9c`、等待沙色、未確認／斷線／錯誤粉紅 `#e99a94`、停用灰，文字粗體並保留模式勾選；紅綠依最新指示提高飽和度。
依目前 source 的 sRGB 相對亮度重算文字／實心底板對比：NEUTRAL 11.24:1、GOOD 6.71:1、WARNING 8.40:1、DANGER 5.24:1、DISABLED 5.42:1。
定位有新鮮 Station 確認時為綠色，尚未取得第一筆位置為沙色等待，有位置但未確認為粉紅；提醒與錄影狀態文字也用對應實心底色。控制面板不透明，不受背後相機明暗干擾。
一般按鈕／底板維持小圓角長方形，`FieldStyle` 圓角為 6dp；錄影鍵例外使用圓形底板，中心為紅圓／停止方塊，沒有白色外框。
相機工具底板保留半透明：倍率／暫停／畫質／資源區 alpha `0xb3`（約 70%），錄影底板 `0x99`（60%），中心紅色 `0xb3`（約 70%）。
頂列只留固定寬 112dp 的定位與 128dp 的 Station 入口，視覺底板 32dp、觸控區 48dp，沒有整條大背景或儲存 chip；文字變動不推動位置。橫向間距 8dp，面板按鈕垂直間距 12dp。
右下 `phonePower` 整合兩行資源：上行接電時為綠色閃電 icon＋N%，拔電時為「電量 N%」，接電且滿電也保留閃電與百分比；下行剩餘 GB。文字 13sp、共用同一個半透明背景，沒有白框。畫質固定寬 128dp、距右 172dp；右側資源區固定寬 144dp、距右 20dp，兩者相隔 8dp。
錄影／暫停／倍率／畫質／資源區均高 48dp、距底部 26dp 對齊；只有倍率與畫質加不透明白色細框。開面板時倍率／畫質／資源區一起收起，面板收合後恢復。
半透明工具的實際文字對比會隨背後影像變化，不套用實心底板的固定對比值。

原生控制直接呼叫 Station API，不載入或注入原網頁。主畫面只留一個 Station 入口，控制與資訊整合於同一個可捲動浮層；點按面板外的相機預覽可收合，展收不重新綁定相機、不改錄影生命週期。
控制保留 Surfer 位置回報啟停、GPS／手動模式與手動角度；資訊只留兩端電量、定位、現場校正、速度與 IP。
刪除 App 中的地圖／雷達／原始 log、封包測試、重複標頭／重整及所有兩端 SD 資訊；原網頁 debug 保持可用。
底部電源狀態從 Android battery 讀取；最新需求以綠色閃電 icon 表示接電，旁邊仍顯示 N%，不再顯示「充電中／已充滿」文字。
右下資源區第二行顯示本機錄影磁碟 StatFs 的可用空間「剩餘 XX.X GB」（十進位 GB），不讀其他 App 或檔案內容。
最新閃電 icon／百分比改動尚未部署到手機。本輪手機收起後停止 ADB／Mobile MCP 操作及安裝，待下次接續部署；下方歷史真機紀錄不代表這項最新顯示已驗收。
原連線／控制／資訊三入口合併：未連線顯示「Station 斷線」，只開 IP 設定；連上顯示「Station 控制」，才可開控制／資訊。
連線資格取自完整三份 API 輪詢、未失敗且 5 秒內的新資料；失聯會停用操作並收起浮層，持續自動重連。
定位確認仍由獨立定位狀態呈現，不用定位 POST 成功取代控制 API 的連線狀態。
Surfer 電量以與 geo_math.h 相同的 3200–4150 mV 公式估計並標註，Station 百分比取自 API。
提醒疊在影像，點按可看當下處理方式；與手機來源無關的 Station GNSS rate／scatter 及 debug SD 訊息不在 App 顯示。

所有 HTTP 序列化、有連線／讀取期限及回應容量限制。控制只因使用者手勢送出；
提交時捕捉控制 boot／epoch／2 秒意圖期限，執行時重新 GET 上下文，核對同 boot／epoch 後才帶 seq+1／stamp POST。
過期、重連、模式世代或重新開機都撤銷原意圖；POST 不自動重試，HTTP 200 也不代表實體到位。
Surfer 啟停先核對新鮮 can_control／Client ID／兩端 boot，HTTP 202 僅受理，繼續比對 Station boot＋operation ID 的 confirmed／error／timeout。
不把 BLE accepted 當作位置回報／LoRa 已確認；GPS 模式資格仍由後端 gps_available 決定。

使用 Android 精確位置，最多每秒上傳一筆**新樣本**；依 monotonic 時鐘拒絕快取超過 10 秒、重複或倒退樣本。
手機定位改為前景固定自動更新，移除停止／開始更新與其 gate；定位視窗只有狀態。
11/14 起只訂閱 GPS provider；收樣與送出前都拒絕 network／fused／passive 或沒有來源的定位，避免較新的網路估算座標覆蓋 GPS。
GPS provider 存在但停用時仍保留 listener，以便前景重新啟用後恢復；停用時清除 App 最新位置並提示開啟 GPS，無 provider 時提示不支援，不自動改用其他來源。
POST 回覆需要 `source=phone`、`valid=true`，座標／timestamp 與提交一致才確認。
畫面背景化會停止取樣、丟棄未送舊世代、結束保存錄影；不提供背景／鎖屏服務。
現有 Station 最後手機 snapshot 保留至重開機，App 停止不會把舊 snapshot 偽裝新樣本或自動切換來源。

本 App 工作未修改或燒錄 Client／Station 韌體，未送 Servo／校正／速度／配對／換頻指令。
原生位置回報啟停做了真機驗證，已恢復 Surfer Ready；Station 保持 manual 179.5°，沒有進入 GPS 追蹤或實體轉動測試。

`PhoneVolumeKeys` 依 InputDevice 的內建／外接分類攔住手機音量加減／靜音，MainActivity 的 onKeyDown／onKeyUp 與 AppDialogBuilder 對話框共用規則。
不做全域設定，不把手機音量鍵當成錄影鍵；外接輸入不混為手機側鍵，但本版尚未加入藍牙遙控器錄影映射。

## 工具與隱私範圍

- 電腦端安裝 Mobile MCP `@mobilenext/mobile-mcp@1.0.17`，註冊名稱 `shore-android-mobile`，停用 MCP telemetry。
  只用於本次 App 的安裝／啟動／畫面驗證；沒有在手機安裝額外常駐遠控 App。
- `mobile-android-design` skill 取自 `wshobson/agents` 的 `plugins/ui-design/skills/mobile-android-design`；
  使用其原生 Android 可及性／觸控／狀態呈現指引。使用者的極簡、滿版要求優先於通用範例。
- 配對碼只供當次無線 ADB 配對，不寫入專案或文件；私人座標不放公開驗證摘要。
- 建立專案版 [$shore-spotter-android](../.agents/skills/shore-spotter-android/SKILL.md)，沿用現有 Mobile MCP，
  包含只讀辨識與前景檢查工具；不新增手機常駐服務，也不把 skill 流程限制誤稱為 ADB 權限隔離。

## 驗證狀態

- 11/14 GPS-only 變更：三組 JUnit 共 5 個測試案例通過，涵蓋新鮮度邊界、較新 NETWORK 點仍被拒絕、其他／缺少來源拒絕，及既有控制 lease／位址規則。
  assembleDebug 通過；lint 0 errors／26 warnings；4 個 64 位元 JNI 的 16 KB ELF／APK 對齊及 zipalign 通過。
  APK SHA-256 `7e2fbf19faf1edd3c91c2297cfd241917ba91b2c05d8ae91c753de56496bfb0c`；尚未安裝手機，衛星重新取得、戶外精度與追蹤效果尚待實測。
- 最新閃電 icon／百分比版本 build、3 組 JUnit 通過；lint 為 0 errors／26 warnings。
  APK 的 16 KB ELF／ZIP 對齊與 zipalign 檢查通過；依使用者指示，這版尚未安裝或做真機顯示驗收。
  單元測試覆蓋私有 Station 位址、重複／倒退／過期位置，以及重連／重啟／世代改變／2 秒到期的控制意圖與 uint32 序號 wrap。
- APK 已透過 Mobile MCP 安裝至 Pixel 10／Android 17。主畫面實際 UI bounds 為 2424×1080，
  native panel 已檢查不含地圖／雷達／重整／SD；沒有網頁容器。後續合併為單一 Station 入口，控制／資訊整合在同一個浮層，沒有分頁。
- App 相機、使用期間精確位置與網路權限已核對；沒有麥克風、媒體全讀、背景位置或背景錄影服務。
- 錄影 1：1920×1080、平均約 29.79 fps、57.062 秒，H.264，只有 video stream、無 audio stream。
  真機暫停／繼續、展收介面後同一檔案成功保存。
- 錄影 2：3840×2160、平均約 59.32 fps、22.807 秒，H.264，無 audio stream。
  本次 native 控制／資訊切換、2×→1×縮放、暫停／繼續保持錄影，停止後保存成功。
  FPS 為 CameraX 目標與實際 VFR 分開記錄；不是所有光線、鏡頭或溫度下固定 60 fps 的保證。
- 手機位置已由 App POST 並由實機 Station 讀回 `source=phone`／`valid=true`，
  多次 timestamp 持續前進；家中 Android 回報半徑約 100 m，不能當作戶外精度驗收。
- Native「開啟位置回報」通過：Station 目前 boot 內 operation 1，command=confirmed／Client Tracking。
  Native「關閉位置回報」通過：operation 2，command=confirmed／Client Ready。
  前後 Station 均 manual 179.5°，沒有操作舵機或自動進入 GPS。
- 16 KB 相容性：初版舊 CameraX 1.3.1 的 image_processing JNI ELF LOAD 只有 4096 對齊，引發 Android 17 debug 提示。
  更新 CameraX 1.6.2／compile SDK 36.1 後，4 個 64 位元 JNI 的 ELF LOAD 與 APK ZIP offset 均為 16384 對齊，
  `tools/check_16kb.py`、`zipalign -c -P 16 4` 通過，手機重新開啟沒有該提示。
  Pixel `getconf PAGE_SIZE` 實為 4096，未宣稱真正 16 KB kernel 完整驗收；未調手機相容模式。
- 已恢復 App 1080p／30fps、1×、收起面板、停止錄影，Station 與 Surfer 如上述原狀態。
  測試影片保留在手機 Movies/ShoreSpotter，副本與 ffprobe／build 證據保存在本機私人 state 目錄，不上傳雲端。
- 單頁整合後真機確認「充電中 · 剩餘 35.4 GB」、單一 Station 入口與面板捲動。
  只將 App 地址暫指同一 Station 的關閉 port，已確認顯示 Station 斷線、收起面板且只能開 IP 設定；
  恢復原 IP 後重新顯示 Station 控制，Station 維持 manual 179.5°、Surfer Ready，沒有發送硬體操作。
- Skill 唯讀工具已核對實際裝置／Station 與本 App 前景截圖；stub 測試確認不同 package／相似前綴在截圖前遭拒。
- IP 欄真機確認數字鍵盤與小數點鍵，完整三個點的 IPv4 輸入通過；系統 EditorInfo 回讀為 Number、IME_ACTION_DONE、NO_EXTRACT_UI。
- 低彩度背景、取消定位暫停與加長倍率滑桿已 build／安裝；真機主畫面與定位對話框輸入音量加減，媒體／鈴聲音量及靜音模式保持原值、未啟動錄影。
  定位視窗確認沒有停止／開始更新。滑桿／面板的後續真機紀錄見本機私人驗收證據。
- 閃電 icon 修改前，真機確認兩行共用資源背景；錄影中暫停／繼續、點面板外收合、相機工具隱藏／恢復與停止保存通過。
  較早一次錄影測試曾回報 `ERROR_NO_VALID_DATA`（8）；原因未確認，完整等待安裝完成後重測成功。這些紀錄屬先前 APK，不能代替最新 icon 的真機驗收。

## 未驗與戶外接續

- 尚未驗證戶外定位誤差、Client 真實 GNSS fix／RF 距離、多小時錄影熱穩定、GPS 追蹤的實際機械動作。
- 低彩度配色與半透明工具尚未驗證真實海邊日照；側鍵以 ADB 虛擬內建事件驗證，實體手機夾／藍牙遙控器尚未驗收。
  前景重新開啟系統定位服務的 listener 缺口已修正，未為測試切換日常手機的全域定位開關。
- 夜間室內 API／短片成功不能代替上述驗收。24 fps／720p／480p 選項可選，但未逐組錄影測試。
- 首次 UI 查詢碰到鎖定畫面，已停止該畫面操作並移除暫存截圖；沒有解除或修改螢幕鎖。
- 換到手機熱點後 Station IP 可能改變，只改 App 連線 IP；保持手機在腳架旁，先確認新位置送達，再使用 GPS 追蹤。
