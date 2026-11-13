# Shore Spotter Android

Pixel 10 的橫向滿版相機、原生追蹤控制與手機定位 App。App 版本與韌體版本分開管理。目前 source 為 **0.3（versionCode 3）**，本次新增最近 IP；安裝／真機狀態見交接入口。

## 海邊使用

1. 開啟 **Shore Spotter**。首次允許相機與使用期間的**精確位置**；**Station 斷線** 可設定 Station IP，例如 `192.168.1.100`，不加協定。使用可輸入多個小數點的數字鍵盤，按完成即連線；保存後自動重連。
   IP 輸入框下方最多顯示最近成功連線的兩個不同地址，最新在左；點按立即選取並連線。只有通過 Station 識別及完整狀態 poll 後才記錄，輸入／保存／失敗不列入；歷史保留於本機，升級後會從實際成功連線開始累積。
2. 連上後只有一個 **Station 控制** 按鈕，點開同一面板內的控制與資訊；點按面板外的相機預覽即可收合，不影響相機綁定或錄影。未連線不能進入面板，失聯會收起浮層、停止操作並自動重連。
3. **控制**可啟動／停止 Surfer 位置回報，選手動或 GPS 追蹤；手動方向滑桿放開才送出。同一頁可看兩端電量與定位，下方可校正、調整速度或更改 IP。
4. 錄影鍵開始，方形鍵停止保存；旁邊可暫停／繼續，保持同一個影片檔。
5. 右下半透明畫質按鈕調解析度／FPS；錄影中鎖定畫質設定。加粗的倍率滑桿／點按對焦仍可操作，移除 1×／2×／5× 快捷鍵。

右下畫質右側的資源區以兩行顯示：上行接電時為**綠色閃電 icon＋N%**，拔電時為「電量 N%」，接電且滿電也保留閃電與百分比；下行為錄影磁碟的**剩餘 XX.X GB**。提醒直接疊在影像，點按可看處理方法。
目前 source 包含閃電 icon／百分比與 GPS-only；本次 GPS-only APK 尚未安裝，現有手機 APK 未與目前 source 位元組比對，歷史真機驗收不可直接套用。
沒有地圖、雷達、重複的面板標頭、常駐重整、SD 資訊或診斷數值；原 Station 網頁保留作 debug。
Surfer 使用 Heltec T096，App 不呈現不存在的 SD 卡。
狀態採不透明背景：就緒綠 `#7acd9c`、等待沙色、未確認／斷線／錯誤粉紅 `#e99a94`、停用灰色，保留文字與模式勾選。定位尚未取得第一筆時顯示沙色等待，有位置但尚未確認時為粉紅。
完整底板／文字色值以 `FieldStyle.java` 為準；既有實心底板對比為 NEUTRAL 11.24:1、GOOD 6.71:1、WARNING 8.40:1、DANGER 5.24:1、DISABLED 5.42:1。半透明工具不能套用此固定值，日照辨識仍需外測。
上方只有固定寬 112dp 的定位與 128dp 的 Station 入口，底色視覺高度一致為 32dp、觸控區 48dp，沒有整條大背景或儲存 chip；文字變動不推動其他元件。橫向間距 8dp，面板內按鈕垂直間距 12dp。
一般按鈕底板維持 6dp 小圓角長方形。錄影鍵為半透明圓形底板，中心為紅圓／停止方塊，沒有白色外框。
錄影／暫停／倍率／右下畫質／資源區均高 48dp、距底部 26dp 對齊，使用半透明底板；倍率與畫質有白色細框。畫質寬 128dp、距右 172dp；右側兩行資源區寬 144dp、距右 20dp，13sp 文字共用同一個半透明背景，沒有白框。
倍率滑桿列縮至前次寬度的 80%，左側預留錄影／暫停區，並依螢幕寬減 464dp 的剩餘空間夾限；軌道 8dp、拖曳點 28dp。錄影中心紅色 alpha `0xb3`（約 70%）、底板 `0x99`（60%）。展開面板時收起倍率、畫質與資源區，收合後恢復。
右上角 Android 綠點表示相機正在使用，預覽也會亮，不代表已開始錄影。

影片不收音，存至相簿的 `Movies/ShoreSpotter`。不申請麥克風、背景定位或全部檔案權限。
App 前景保持亮屏；離開 App 或鎖屏會停止定位並結束保存錄影，第一版不提供鎖屏錄影。保存已在背景完成時，回前景可重新開始錄影；仍在保存或相機不可用時保持停用。
滿版預覽中央填滿，邊緣會隨螢幕比例裁切；輸出影片使用所選標準解析度。
**錄影暫停與 GPS 追蹤相互獨立**，暫停影片不會停止追蹤器或偷偷切換舵機模式。
使用 App 時忽略手機內建音量加減／靜音鍵（含 App 對話框），不調音量、不當錄影鍵；離開 App 後恢復系統行為。
外接輸入裝置可另行辨識，目前尚未加入藍牙遙控器錄影映射，仍須拿實際遙控器確認事件。

手機需固定在 Station 旁。App 只使用 GPS 衛星定位，不採用基地台／Wi-Fi 網路估算位置，沒有衛星定位時等待 GPS，不自動改用其他定位來源。
每秒最多送一筆新的、未滿 10 秒的 GPS 位置到既有位置 API，核對回覆後才確認；Wi-Fi／熱點仍照常供 Station 連線使用。
手機定位的回報半徑不是實測誤差。App 前景自動持續更新，定位視窗只顯示狀態，沒有停止更新按鈕；離開前景後 Station 仍會保留最後手機位置至重開機。
換家中 Wi-Fi／外出熱點後，依 Station 螢幕更新 IP。App 不更動手機網路、熱點、預設相機或其他 App。

## 建置

本機已有 Android SDK 36.1、JDK 21、Gradle wrapper，Android Studio 不必開啟。
本機 `local.properties` 設定 `sdk.dir`，不納入版本控制。

```sh
cd /home/nolanasd123/Shore-Spotter/android
JAVA_HOME=/opt/android-studio/jbr ./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
python3 tools/check_16kb.py app/build/outputs/apk/debug/app-debug.apk
```

產物 `app/build/outputs/apk/debug/app-debug.apk`；獨立套件 `tw.shorespotter.app`，不覆蓋舊 My Application。
CameraX 1.6.2。FPS 是鏡頭目標範圍，實際影片仍需驗證；不把手機官方相機功能當作第三方 App 保證。
動作沿用 Station API 的開機識別／世代／序號／2 秒有效期；失聯或切回前景後不重送舊操作。
目前部署／未驗收界線見 [交接入口](../docs/assistant-handoff.md) 與 [後續規劃](../docs/next-session-plan.md)。
過往 UI／影片／API 驗收見 [歷史 App 紀錄索引](../docs/records-index.md#android-app-2026-11-13)。

## 定位、控制與驗證契約

GPS-only 在收樣與送出前拒絕 network／fused／passive 或缺少來源；GPS provider 停用時仍保留 listener，重新啟用可恢復，停用時清除 App 最新位置。
Android 回報的精度半徑不是實測誤差；GPS 信號不足時等待衛星定位，不自動改來源。
依 monotonic 時鐘拒絕重複／倒退／未滿條件的新樣本，超過 10 秒不送；位置 POST 必須回覆相同 source／valid／座標／timestamp 才確認。
App 背景化會停止定位、撤銷舊世代並結束保存錄影；Station 仍保留最後手機 snapshot 至重開機。

HTTP 由單一 executor 序列化，連線／讀取各有 2 秒期限，回應上限 64 KiB。控制只由手勢產生，捕捉 2 秒意圖期限、boot／epoch；執行時 GET 新上下文，核對同世代後送 seq+1／stamp。
poll 失敗或狀態逾 5 秒會撤銷尚未送出的舊意圖；worker 偵測失敗後即封鎖同世代操作，不等 UI 回呼，舊世代失敗不影響新連線。重啟或換世代同樣撤銷舊意圖，POST 不重送；Surfer HTTP 202 僅受理，繼續核對 Station boot＋operation ID 的 confirmed／error／timeout。
UART 或命令角度沒有實體回授；只看 API 成功不算舵機驗收。

11/14 先前 0.1 GPS-only 基準：JUnit 三組共 5 案例、assembleDebug、4 個 64 位元 JNI 的 16 KB ELF／APK 對齊及 zipalign 通過；lint 0 errors／26 warnings。
APK SHA-256 `7e2fbf19faf1edd3c91c2297cfd241917ba91b2c05d8ae91c753de56496bfb0c`；尚未安裝或外測，不需重燒 Station／Client。

## Agent 操作

使用專案內的 [$shore-spotter-android](../.agents/skills/shore-spotter-android/SKILL.md)；沿用 Mobile MCP／ADB，不另開專用遠控服務。
包含只讀裝置／Station 檢查工具、App 前景截圖檢查與真機驗收流程。Skill 不會縮小 ADB 本身的整機權限，agent 必須遵守日常手機與硬體操作範圍。

### 0.2 待安裝驗證

0.2 保留 GPS-only，修正背景錄影保存後回前景無法重新錄影，以及失聯後已排隊的舵機／Surfer 操作未撤銷。
`python3 tools/test_android_lifecycle.py`（專案根目錄）抽取實際 Java 方法，模擬 executor／UI 回呼順序；由 `tools/verify_project.py` 自動納入。需要 JDK 17+（優先 `JAVA_HOME`／PATH，亦可使用既有 Android Studio JBR），不連手機或 Station。
覆蓋背景／前景 Finalize、仍在保存、相機不可用，以及 poll 失敗後 UI 回呼前後撤銷、freshness 期限、舊世代失敗、重連新手勢、boot／epoch／2 秒期限。這是主機回歸，不能替代 CameraX、實際 Wi-Fi、GPS／戶外與機械驗收；新 APK 尚未安裝。

0.2 主機檢查：上述 integration 回歸、5 個 JUnit、assembleDebug 通過，lint 0 errors／26 warnings；4 個 64 位元 JNI 的 16 KB ELF／APK 對齊及 zipalign 通過。APK SHA-256 `9f5cb1f551452739ce3e21d02869f9cdfff64885a47f1ddae6e822919b2d8681`；產物與明日驗收步驟見 [交接入口](../docs/assistant-handoff.md)。

### 0.3 最近 IP 主機驗證

`tools/test_android_lifecycle.py` 另驗證實際 poll／歷史寫入方法：最近兩筆排序與去重、相同地址不重寫、歷史儲存，以及任一 GET 失敗、過期世代、背景回覆不記錄。保留數字鍵盤、完成即連線與取消／外部點按不提交的既有流程。
assembleDebug、5 個 JUnit、上述 integration 回歸、16 KB ELF／APK 對齊與 zipalign 通過；lint 0 errors／26 warnings。
APK SHA-256 `51df3ec533560626ba406a15b9ed2d66cc58780a72996cbc4a98334381d5b8ba`；本項僅為主機驗證，安裝與手機 UI 驗收另記於交接入口。
