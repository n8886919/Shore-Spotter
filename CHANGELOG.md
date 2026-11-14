# 版本紀錄

專案從 **0.1** 開始標版。韌體版本的唯一來源是
[`include/firmware_version.h`](include/firmware_version.h)；Client／Station 共用。
從 1.0 起固定使用 **a.b**：同主版本 a 的所有子版本 b，Client／Station 必須互相相容；
不相容變更必須提升 a。一般版本不加 dev；`FIELD_DIAGNOSTIC` 專用建置顯示 `1.0-debug`，
debug 是建置標記，不是第三段版本號。舊版歷史與當時實測字串保留原樣。
版本號不取代 LoRa `PROTO_VERSION`、NVS 格式版本或實機部署驗證。
每次功能、修正、介面或設定行為異動都更新本檔；尚未定版先記在「未發行」，
正式標版時移入對應版本，保留日期、相容性與驗證紀錄。

## 未發行

- **2026-11-14 下午 Station 部署／對照**：使用者確認Servo斷電，584C停用Axiom並保留Token，重啟前後heap約29→52～55 KB、位置快照恢復。Station最終build `Nov 14 2026 16:52:02`／source `c0dbd44` application-only hash通過；SD實際CRC匯出117筆全部含UTC／岸端位置／UART計數。Surfer啟停、App0.3錄影／背景恢復與API組合測試、51／51主機及Station build通過；仍有採樣略過與一次USB匯出失敗紀錄，不宣稱整場／機械／戶外通過。已保存CRC正確的歷史coredump，但缺匹配ELF，未定案panic根因；現役配對ELF已保存，詳見交接入口。

- **2026-11-14 SD 校時解除 Axiom 相依**：實機停用 Axiom 後713筆快照 UTC 全為 null；將既有 SNTP 啟動移至 Station，Wi-Fi 首次連線時啟動一次，Axiom 停用／OTA 不再停止共用時鐘。沒有網路或尚未校時仍保留 boot／sample_ms 與 null，不虛構 UTC；不新增自行實作的重試／worker。

- **2026-11-14 Station 使用情境留證**：沿用 SD／Axiom 既有快照，追加控制 epoch／序號與現役 Youmi UART 的送出、暫緩、錯誤、命令 pulse；明示無實體回授。固定 Sample 1240→1264 bytes，無新增 queue／worker／重試，不改追蹤／Servo 行為。實際 producer／encoder 回歸先紅後綠，雲端 schema 欄位238／256；完整驗證與部署狀態見交接入口。補充既有 SDK Flash coredump 的保存／離線解析流程，尚未以故意 panic 驗收。

- **2026-11-14 實機部署**：使用者確認舵機斷電；584C application-only 燒錄及 hash 成功，build `Nov 14 2026 01:21:02`；F035 application-only DFU 成功，build `Nov 14 2026 01:21:52`，新 boot／Ready／BLE 認領確認。保留設定與 SD；新記錄 CRC 匯出含 `station_position`。Pixel 10 先安裝 0.2 驗證錄影恢復，再更新 **0.3**，拉回 APK hash 相符，最近 IP 按鈕真機點選連線成功。戶外 GPS-only、跳角／panic 與實際機械驗收仍待完成；歷史「未部署」敘述為當時階段狀態，現況見交接入口。

- **2026-11-14 Android 0.3 最近 Station IP**：IP 輸入框下列出最近成功連線的最多兩個不同地址，最新在左，點按直接沿用原連線流程；只在 Station 識別及完整狀態 poll 成功、目前世代仍有效時寫入本機歷史，失敗／過期／背景回覆不記錄。versionCode=3；主機回歸（排序／去重／失敗／過期回覆）、5 個 JUnit、assembleDebug、lint（0 errors／26 warnings）、16 KB／zipalign 通過；本項主機驗證不代表已安裝／真機驗收。

- **2026-11-14 健檢修正整合與次日交接**：六項修正分別提交，完整主機回歸 **51／51**（native 140／140）與
  LilyGO Station／T096 build 通過；T096 DFU ZIP CRC／application-only 核對通過。
  APK／兩端 bin／elf／ZIP、hash 與來源 commit 已存本機獨立產物目錄；更新交接與出門前分段驗收。
  **未安裝 App、未燒錄、未操作舵機；panic 根因與戶外可靠性尚未驗證**。詳見交接入口與階段紀錄索引。

- **2026-11-14 Android 0.2 健檢修正（待安裝）**：保留 GPS-only，poll 失敗先在 worker 封鎖同世代操作，
  UI 再撤銷世代與 busy；5 秒 freshness 過期及送出前／GET 後均檢查，舊世代失敗不影響新連線。
  保留 boot／epoch／seq／2 秒期限與 POST 不重送。錄影在背景完成保存後，回前景恢復錄影鍵；
  仍在保存、相機未就緒或已出錯時保持停用。App versionCode=2／versionName=0.2。
  新增 `tools/test_android_lifecycle.py`，實際方法的兩個舊碼失敗案例修正後通過，並測 callback 順序／重連／過期；
  自動納入主機回歸，需 JDK 17+。5 個 JUnit、assembleDebug、lint（0 errors／26 warnings）、16 KB／zipalign 通過。
  APK SHA-256 `9f5cb1f551452739ce3e21d02869f9cdfff64885a47f1ddae6e822919b2d8681`，尚未安裝／真機／外測；產物位置見交接入口。

- **2026-11-14 Station 記錄來源與 T096 DATA 修正**：SD／Axiom 的板載 GNSS freshness 改由自身 fix／arrival age 決定，
  schema 2／3 追加 `station_position` 保存實際選用岸端 source、valid、座標、接收年齡與手機 timestamp／accuracy；
  保留既有欄位及手機位置保留規則，不改追蹤／Servo。固定 Sample 主機大小 1200→1240 bytes，無新 worker／動態配置。
  T096 速度超出 wire 範圍時保留有效位置並標示速度未知；編碼故障記既有錯誤、不空增 DATA 序號或重試同 epoch。
  wire v5／BLE v1／正常 2 Hz 不變。兩項新增回歸先在舊碼失敗，修正後六組相關主機回歸通過；尚未部署或外測。

- **2026-11-14 部署設定防呆與換機說明**：manifest 的 check／plan／build loader 共用板型、角色與 environment 對照，
  拒絕未知／native／錯誤板型或角色；保留現有十種 firmware profiles。換 Station 流程更正為 RST 重啟，
  USER 長按 ACT／SLP 不清 owner。新增負例在修正前 15 個 subcases 失敗，修正後 11 組設定回歸通過，
  本機四台 manifest 檢查通過；沒有 build、燒錄或硬體操作。

- **2026-11-14 Session checkpoint 與紀錄整理**：保存目前 firmware／Android GPS-only，正式外殼改於使用者更正的 `~/freecad/Shore-Spotter-Case/` 獨立 Git 提交，
  以 `cad/reference.json` 對照 commit／檔案 hash，移除誤納入的 Pocket 診斷副本。
  新增實驗紀錄及下一 session 精簡規劃；已結束階段文件先提交，再移出工作目錄。
  以固定 commit／原路徑／SHA-256／附件索引及 `tools/read_record.py` 讀取、匯出、核對，保留現行規格與待辦。
  checkpoint 前完整主機回歸 50／50 組通過；未操作手機、燒錄或驅動舵機。

- **2026-11-14 Android 手機定位只採 GPS**：移除 NETWORK provider 訂閱，收樣與送出前都拒絕非 GPS 來源，
  避免較新的基地台／Wi-Fi 估算位置覆蓋衛星定位；GPS 停用／不支援時顯示等待或原因，不自動改用其他來源。
  保留前景自動更新、最多 1 Hz／10 秒 freshness 與 Station 相同快照確認，Wi-Fi／熱點連線用途不變。
  三組 JUnit 共 5 案例、assembleDebug、16 KB ELF／APK 對齊與 zipalign 通過，lint 0 errors／26 warnings。
  尚未安裝手機或完成戶外驗收，未修改或燒錄 Station／Client，詳見 [App 紀錄](docs/records-index.md#android-app-2026-11-13)。

- **2026-11-13 Android 整合 App 0.1**：新增獨立 `tw.shorespotter.app`，Pixel 10 橫向滿版相機、
  無聲錄影／同檔暫停繼續、可調解析度／FPS、縮放／對焦，錄影不因展收原生控制／資訊而中斷。
  控制直連既有 API，保留位置回報啟停、GPS／手動、手動角度；資訊只留兩端電量、定位、校正、速度及 IP。
  不照搬網頁，移除 App 地圖／雷達、重複標頭／重整、所有兩端 SD 與工程診斷；原網頁 debug 保留。
  提醒疊在影像，手機電量／充電狀態與剩餘 GB 移至右下畫質右側，以 13sp 上下兩行共用同一個半透明背景、沒有白框；
  上行接電時顯示綠色閃電 icon＋N%，拔電為「電量 N%」，接電且滿電也保留閃電與百分比，下行顯示剩餘 GB。最新閃電 icon／百分比改動尚未部署到手機。
  海邊狀態改實心背景、粗體與模式勾選，就緒綠 `#7acd9c`、沙色等待、粉紅 `#e99a94` 未確認／斷線／錯誤、灰色停用；
  尚未取得第一筆定位為沙色等待。紅綠提高飽和度，五種文字對比為 11.24／6.71／8.40／5.24／5.42:1。
  橫向留 8dp、面板按鈕垂直留 12dp 間距，移除整條頂列背景，上方底板同高 32dp／觸控區 48dp。
  頂列只留定位／Station，固定寬 112／128dp，移除儲存 chip；文字變動不推動位置。錄影／暫停／倍率／畫質／資源區均高 48dp、距底部 26dp 對齊。
  畫質寬 128dp／距右 172dp，資源區寬 144dp／距右 20dp；倍率／畫質保留白色細框，開面板時與資源區一起隱藏、收合後恢復。
  一般按鈕底板維持 6dp 小圓角長方形；錄影鍵改半透明圓形底板，中心紅圓／停止方塊 alpha `0xb3`、底板 `0x99`，移除白色外框。
  移除 1×／2×／5×，倍率滑桿列縮至前次寬度 80%、再以 `max(200dp, 螢幕寬 - 464dp)` 夾限，左側預留錄影／暫停區；8dp 軌道／28dp 拖曳點不變。
  點按 Station 面板外的相機預覽可收合，不重新綁定相機或改變錄影。
  App 主畫面／對話框攔住手機內建音量鍵，不調音量、不當錄影鍵，不改全域設定；外接輸入另辨識，尚未加入遙控錄影。
  手機定位前景固定更新，移除停止／開始更新；存在但停用的 provider 仍訂閱，以便重新啟用後恢復。
  主畫面連線／控制／資訊合併為「Station 控制／Station 斷線」兩狀態單一入口，內容整合一頁，未連線只可設 IP、不能進入控制／資訊，失聯收起並停用。
  IP 欄不要求協定，改數字鍵盤支援多個點、完成即連線；新增 `shore-spotter-android` skill 與唯讀實機檢查工具供 agent 接續。
  新 Android 位置最多 1 Hz／10 秒 freshness，核對 Station 回覆才確認；動作保留 boot／epoch／seq／2 秒期限、
  202 受理與操作 confirmed 分開，不自動重送舊意圖。CameraX 1.6.2 修復舊 JNI 16 KB 對齊警告。
  Pixel 10／Android 17 已安裝；build、三組 JUnit、lint、16 KB 靜態檢查通過；
  1080p／4K 60 目標短片無音軌、暫停繼續、原生控制展收／縮放、Station 手機位置接收與 Surfer 啟停已確認。
  實機影片與驗收界線見 [App 紀錄](docs/records-index.md#android-app-2026-11-13)／[使用方式](android/README.md)。
  本 App 變更未修改或燒錄兩端韌體，未進行 Servo 動作或戶外驗收。

- **2026-11-13 Station Web 逾時恢復（584C 已部署）**：HTTP 2 秒逾時不再依賴 abort 完成才釋放佇列，
  遲到 fetch／本文不更新資料或控制世代；逾時後冷卻 1 秒。背景化取消舊讀取與未送出操作，
  回到前景／快取恢復／online 時重新讀取狀態，清除舊資料新鮮度；POST 不重試、舊控制意圖失效，
  保留後端新鮮度與期限，HTTPS 手機定位仍可送新位置給背景 opener，不重放舊待送位置。
  新增 16 筆有界頁面錯誤摘要，不改版面、RF／GPS／BLE 或後端追蹤／Servo 規則。
  修復前卡住 fetch 案例 2／2 失敗；新增 13／13、9 組既有網頁回歸、LilyGO Station build
  與 binary 內 HTML 核對通過，詳見 [修復紀錄](docs/records-index.md#web-recovery-2026-11-13)。
  使用者授權後 USB application-only 寫入 584C app1，Flash hash／新 boot `352284237`／實機 HTML 核對通過，
  F035 同 boot／BLE claimed／Ready／B 與速限保存確認；Client 未更新，未送 HTTP Servo／Client 指令。
  BLE 約 uptime 141 秒才觀察到恢復，SD 文字持續寫入但快照停止增加／跳過計數上升，兩項仍待查；戶外恢復尚待驗收。

- **2026-10-30 Station 584C 改綁 F035**：核對 USB serial／MAC `48:CA:43:57:58:4C`、
  hostname、既有 1.0 韌體與 boot 後，透過 `/api/whitelist?action=set&id=F035` 將 FB27 替換為 F035。
  HTTP 200 與 GET 讀回確認保存；後續 BLE connected／claimed、owner 584C、Ready、`can_control=true`，
  兩次新鮮狀態的 Client boot 同為 `D52C5A29`（age 888／846 ms）。Servo 維持 manual／179.5°；
  未重新 build／燒錄或啟動 Tracking／Test。詳見 [維護紀錄](docs/records-index.md#t096-maintenance-2026-10-28)。

- **2026-10-30 T096 F035 更新完成**：將目前接上的 F035（USB serial `C3BFA5F76F915E4A`）
  由 0.6.1-dev 更新為目前工作目錄的 1.0／UI=8；native 140／140 與 13 組相關主機回歸、
  獨立目錄 Client build、ZIP CRC／application-only manifest／SoftDevice 0xB6 核對通過。
  application-only DFU exit 0／`Device programmed.`，同 serial 返回 application USB；
  新 boot `CF8DCA00`、1.0／UI=8、USB DIAG 同 boot 回覆已確認。Station 未重燒或修改綁定，
  BLE 認領、實體按鍵／TFT 及戶外 GPS／RF 尚未驗收；見 [維護紀錄](docs/records-index.md#t096-maintenance-2026-10-28)。

- **2026-10-28 T096 USER ACT／SLP（UI=8，FB27 已部署）**：長按滿 5 秒放開改為 Ready → Tracking、Tracking／Test → Ready；
  未認領亦可本機 ACT，保留 owner／BLE／boot／命令序號與 runtime 頻道（開機 A，本次改 B 後沿用 B）。
  RF 忙碌等現有交易結束；低電禁止啟動，初始化失敗保留 RF 原始錯誤；舊 LoRa 重播不能誤確認已被本機切換的模式。
  加入 CLIENT BOOTING 開機提示、ACT STARTED／ENTERED SLP 兩秒文字與低電／RF 失敗提示，不增加阻塞等待。
  USER 短按與 60 秒查看不變，重啟與換 Station 改用 RST。13 組主機回歸、native 140／140 通過；
  顯示提示追加後 13 組回歸與 T096 build 通過（RAM 19,724／Flash 200,008 bytes），ZIP CRC 通過。
  FB27 已 application-only DFU，新 boot E4017F95／UI=8／BLE owner 584C／Ready 確認；實體按鍵與文字仍待驗收。
  詳見 [維護紀錄](docs/records-index.md#t096-maintenance-2026-10-28)。

- **2026-10-28 私有 Wi-Fi 設定**：依使用者提供的連線資訊更新本機 `include/wifi_config.h`，
  已確認受 Git ignore 保護且未追蹤；不在版本文件記錄 SSID／密碼。Station 重新 build 通過，尚未燒錄或驗證新設定連線。

- **2026-10-28 Station 部署完成**：使用者授權先更新 Station；584C OTA 因漏核對 Wi-Fi 設定而未回原網路，
  已以先前成功部署設定隔離重建，核對 USB serial／MAC 後 application-only 寫入 app1，hash 通過。
  RTS reset 未恢復，watchdog reset 後新 boot `2025037645`／HTTP／核准 HTML／FB27 新鮮 STATE／SD 錄製已確認。
  BLE 仍 searching，重連改善尚未驗證；Client UI=7、後續 RF 顯示與另行更新的私人 Wi-Fi 設定未部署。
  過程與後續驗證見 [BLE 重連紀錄](docs/records-index.md#ble-reconnect-2026-10-28)。

- **2026-10-28 RF 指標易讀化（2026-11-13 已部署 584C）**：Surfer 診斷的 RSSI／SNR 分為訊號強度、訊號品質兩列，
  各顯示 Good／OK／Bad／Off，原始數值收在折疊內。Off 僅表示沒有近期 RF 樣本；
  網頁失聯／資料過期顯示 `--`，不誤稱 Client 關機。不改後端綜合分級、RF、GPS 或 Servo 行為。
  motion UI（含分級邊界／原始值／過期／恢復）、radar layout、Client control UI、BLE UI 四組回歸通過；
  `pio run -e tbeam-station` build 通過（Flash 1,927,546 bytes），`git diff --check` 通過。
  此階段未燒錄；11/13 隨使用者授權的 HTTP 恢復產物部署至 584C，實際 HTML 一致性已核對，見上述修復紀錄。

- **2026-10-28 BLE 重連查證與介面修正（Station 已部署，Client 未部署）**：核對實際 NimBLE 分支，舊 `connect(...,5000)`
  未採用 5000，內部期限為 30000 ms，不能用五秒逾時解釋舊 61 秒觀察。
  Station 改 passive scan／目標廣播到達即由 worker 結束掃描，增加有界掃描等待、切換目標檢查與
  SDK 固定佇列階段計時／失敗原因；Client 10 秒廣播、owner 與 GPS／LoRa 行為不變。
  T096 UI=7 首行改動態 `CLIENT ID xxxx`，真實 BLE MAC 保留 USB 診斷；頻道彈窗移除取消、
  點窗外關閉且不送出草稿；按鈕改「開啟位置回報／關閉位置回報」。
  10 組針對性主機回歸、雙端 build、隔離彈窗與 TFT 原字型 mock 通過；
  見 [本輪紀錄](docs/records-index.md#ble-reconnect-2026-10-28)。實機延遲改善仍未驗證。

- **2026-10-28 室內驗收交接**：新增 [驗收與接續任務](docs/records-index.md#indoor-acceptance-handoff-2026-10-28)，
  記錄使用者按鍵／Test／A/B 保存／重啟／網頁驗收、BLE 重連約 22～61 秒待查、三項尚未實作介面需求。
  同步助手入口；未修改韌體、build 或部署，不將重連速度、多 Station 或戶外項目列為通過。

- **2026-10-28 文件一致性整理**：README、交接／功能入口與通用規格同步現行 1.0／T096 UI=6；
  舊 Ready 30 秒 LoRa、USER 2 秒回 Ready、四區 UI／網頁置中及舊版本資料明確分為歷史。
  綁定／控制 OpenAPI 統一 api_version=2、BLE／LoRa 新鮮度、ready_poll_ms=0、409／503 與同 ID no-op；
  TEL 主表補 humidity=254，區分 manifest A/SF10 與 runtime A/B 皆 SF9。
  補齊實體置中校正、頻道保存與 SD runtime 快照；區分速度 UI 整數／API 小數及最近 RF／DATA 平均 RSSI，
  保留各版實際部署／待驗證界線。
  兩份 OpenAPI JSON／local refs／共有契約、196 個本機文件連結通過；binding API、Station BLE backend、
  BLE codec、T096 維護電源與 motion backend 五組相關主機回歸通過。本項未修改韌體或重新 build／部署。

- T096 診斷頁版本文字改為 `VERSION 1.0`（UI=6），韌體版本仍為 1.0；顯示回歸與 build 通過，FB27 已燒錄，新 boot／BLE 認領確認。

## 1.0 — 2026-10-28

- 建立 a.b 雙端相容規則；取代 0.x 開發系列，不承諾與舊 0.x 相容。正常韌體版本為 1.0，診斷專用建置為 1.0-debug。
- T096 UI=5：首頁維持五行，電壓與 UP 同列；診斷頁六行加回完整 SAT、AGE、HDOP，保留所有系統／傳輸診斷。七組相關主機回歸、雙端 build 通過；584C／FB27 已 application-only 燒錄，新 boot／1.0／BLE 認領／SD recording 確認。實體畫面仍待目視；詳見 [維護紀錄](docs/records-index.md#t096-maintenance-2026-10-28)。

- **2026-10-28 T096 維護首頁 UI=4**：完整 MAC 六組以冒號分隔；sleep／active 中間改畫連續實線，
  不使用有缺口的字型直線。GPS／RF 狀態同行、RF SLEEP 文字改 OFF；GPS 衛星／HDOP／年齡與電壓同列，
  保留首頁診斷資訊與既有按鍵／控制行為。兩組顯示／按鍵回歸與 build 通過，原字型 mock 預覽確認實線；
  FB27 已 application-only DFU，新 boot 4E9805E0／UI=4／BLE 自動認領確認。驗證與部署見 [維護紀錄](docs/records-index.md#t096-maintenance-2026-10-28)。

- **2026-10-28 T096 維護首頁 UI=3**：第一行改實際完整 BLE MAC（`AA:BB:CC:DDEEFF`），
  第二行固定 sleep／active 並框出目前模式（Ready／Tracking 或 Test）；owner 改十六進位並與 BLE 同行。
  移除畫面 USB，GPS 狀態／衛星／HDOP／年齡集中首頁；診斷頁只留版本、uptime、TX／錯誤與 LAST。
  三組相關回歸與 Client build 通過，原字型 mock 預覽確認邊框；FB27 已 application-only DFU，
  新 boot B881BE78／UI=3／實際 BLE MAC 與 Station 自動認領已確認，實體畫面仍待目視驗收。
  按鍵、控制狀態與 BLE／LoRa 協定不變；驗證與部署見 [維護紀錄](docs/records-index.md#t096-maintenance-2026-10-28)。

- **2026-10-28 T096 開蓋維護介面**：USER 短按亮屏 60 秒／切換連線與診斷兩頁；
  長按滿 5 秒後放開重啟，1–5 秒放開取消，移除長按 2 秒回 Ready。
  顯示十進位 Client／owner ID、實際 BLE／GPS／RF、電壓、epoch 年齡／衛星／HDOP、TX／錯誤計數與最後 RF／BLE 失敗原碼。
  開機／低電自動畫面仍約 5 秒，開機已按住必須先放開；owner 仍僅於重新開機解除。
  140 native、11 組 T096／BLE 與 RF power 回歸、Client build 通過；FB27 已 application-only DFU，
  新 boot／維護標記／BLE 自動重新認領與 Ready 已確認；實體按鍵、螢幕與跨 Station 換機仍待操作驗收。
  驗證／部署與換 Station 操作見 [維護介面紀錄](docs/records-index.md#t096-maintenance-2026-10-28)。

- **2026-10-27 BLE 待機／開機認領與手動換頻（0.7.0-dev）**：T096 待機改為 BLE 每 10 秒可連線廣播，
  不再定期使用 LoRa 收發；Station 自動呼叫已指定的 Client，第一個有效認領來源持有本次開機控制權，
  斷線與回待機不釋放，只能重開 Client 才換 Station。GPS／LoRa 啟動與無 GPS 的 RF 測試仍分開。
  Station 增加非阻塞候選頻道 CAD 掃描、待機選頻與實際 LoRa 回報驗證後保存；A／B 候選皆用 SF9。
  Surfer ID 改為藍色可點文字，編輯框需確認才修改；網頁與未來 iOS／Android App 共用 Wi-Fi HTTP API，
  不由 App 直接控制 Client。實作、API 與驗證界線見 [BLE 版本紀錄](docs/records-index.md#ble-control-2026-10-27)。
  140 native＋46 Python／Node 主機腳本通過（全套與修正後相關重跑），T096／LilyGO Station build 通過，隔離網頁預覽完成。
  Ready 不重試 RF 初始化、Station 故障後恢復原工作頻道及 SD runtime 頻率已加回歸。
  **2026-10-28 已 application-only USB 部署 584C／FB27**：新 boot／BLE 自動認領／實機 HTML 一致、
  B 頻道 5 個測試封包無缺號與 Stop 回 Ready 通過；配對、60°/s 及 SD recording 保留。
  掃頻與跨頻保存、待機電流、戶外 RF、GPS 與 Servo 仍未做本版驗收。

- **2026-10-27 資訊頁精簡與配對狀態修正**：移除控制板規格、版本、正常溫濕度／SD 與速度保存成功文字，
  詳細定位／RF／MCU 數值收進診斷；SD error 接進提醒。保留已確認 Client ID，HTTP 失敗與 STATE 過期分開顯示，
  僅明確 `client_id=0` 顯示未配對。移除網頁置中，新增操作者確認實體置中的校正參數／中位同步，保留舊 API。
  RF 測試改名無線連線測試並增加結束操作。584C 已以 USB application-only 更新、hash 驗證通過；新開機、HTML 逐 byte 一致、FB27 持續 STATE、60°/s 與 SD recording 保留。驗證與部署狀態見 [本次紀錄](docs/records-index.md#info-page-2026-10-27)。

- **2026-10-27 App Client 綁定 API 契約**：整理既有 `/api/whitelist` 的讀取、替換、解除、NVS 保存與錯誤語意，
  加入 iOS／Android 共用串接流程及 OpenAPI 子集，區分設定成功與新鮮 STATE／控制命令確認。
  實際 handler／NVS 主機回歸與 Station downlink 回歸通過；沿用現有端點與韌體行為，不需為本次文件／測試重新燒錄。
  串接入口見 [App 綁定契約](docs/client-binding-api.md)；手機 App 尚未實作／驗收。

- **2026-10-22 T096 健康診斷（0.6.1-dev）**：MCU 溫度每 30 秒機會式取樣，
  不增加待機週期喚醒；SoftDevice 依其 API 讀取，裸 peripheral 有 10 ms 非阻塞逾時及 STOP。
  既有 11-byte TEL 的 humidity=254 明示 MCU 溫度／無濕度，保留 wire v5 與原本發送頻率；
  舊 Station 會拒絕此保留碼，因此須先更新 Station。新 API 將 MCU 與環境溫度分開，
  顯示接收最高溫／接收年齡、逾期 unknown，不套用環境溫度警告或自動停機。
  GNSS parser 增加拒收原因，不改接受／定位規則；T096 固定 4 筆 RAM 原始 bytes 證據，
  含時間／截斷／覆寫計數，只經唯讀 USB `DIAG STATUS`、`DIAG NMEA` 匯出，不寫 Flash。
  140 native＋39 Python/Node 通過；584C／F035 已更新 0.6.1-dev，fresh boot、MCU USB／LoRa／網頁、原始 NMEA 匯出及 Ready 逾期 unknown 通過。
  實作、驗證與部署紀錄見 [T096 健康診斷](docs/records-index.md#t096-health-diagnostics-2026-10-22)。

- **2026-10-16 USB 匯出隔離 SDK log**：ESP-IDF 訊息改由固定容量、跨 task 保護的佇列，
  在主迴圈經既有 LogTee 寫入診斷紀錄；SDK 不再直接寫入 SD／DIAG 的 USB 封裝串流。
  過長訊息加截斷標記，佇列溢位回報遺失筆數，不改正式匯出工具的長度／CRC 驗證。
  新增實際 callback／LogTee 的 SD、DIAG 隔離、正常輸出、並行 producer、溢位與截斷測試。
  140 native＋37 Python/Node 與三種 ESP32 角色 build 通過；E91C application-only USB 更新及晶片 hash 驗證成功。
  Wi-Fi 離線重試期間，同一舊檔連續兩次與新 session 標準匯出均通過原始長度／CRC，
  SDK 錯誤仍在紀錄內；新 boot／F035 Ready／SD 恢復錄製已確認。
  使用者確認 Youmi＋控制板 Servo 實體驗收完成；熱點與 GNSS 留待會面處理。
  本輪回歸、build 與部署結果見 [深度健檢](docs/records-index.md#health-check-2026-10-16)。

- **2026-10-16 深度健檢／建置重現性**：空白 PlatformIO core 實測
  `espressif32@55.3.39` 無法由 registry 取得，改為本機原已使用的 pioarduino
  55.03.39 release URL；空白 core 已成功安裝該 platform（未宣稱空白 OS 全工具鏈驗證）。
  固定本次實際解析的函式庫版本與 native platform，移除 OTA 的私人 LAN 預設目標。
  加入 `requirements-dev.txt` 與 `tools/verify_project.py --prepare`，先準備測試所需第三方
  原始碼再執行 native／Python／Node 回歸；不操作實機、不上傳。
  保留韌體 `0.6-dev`、wire v5、SF9、GPS／Servo／電源行為；以來源 manifest／binary SHA-256
  識別本輪驗證來源。RadioLib 此次兩端固定 7.8.1，先前 Station 本機快取為 7.7.1，
  已重新 build／部署 E91C、F035；140 native、36 Python/Node 與五種 firmware build 通過，
  桌上 RF 61.1 秒新增 109 probes 無新增缺號。最終新 boot／Ready STATE／SD 恢復錄製已確認；
  專用熱點尚未連線，發現 Wi-Fi SDK log 混入 USB 匯出導致 CRC 失敗，列為未修正缺陷。
  GNSS／Servo 物理驗收仍未完成。結果與部署界線見
  [深度健檢](docs/records-index.md#health-check-2026-10-16)。

- **2026-10-11 正式資訊頁與 M0 串口輸出**：四區定位資訊／Station Info／控制板／Surfer Info；座標 Maps 連結、衛星分級、HDOP 說明與真實誤差界線、手填經度／緯度／誤差直排；移除 uptime、USB 及多餘 SD／來源欄位。固定 Youmi 359°＋控制板 M0、TX43／9600 8N1，最多50Hz合併去重非阻塞输出，中位179.5°；移除直接LEDC、外部UART模式及 GPS α UI／運算／API。完整圓形操作、點數字速度視窗、鏡頭置中；新增唯讀 SD status、UART counters 與無回授欄位。140 native、實際HTTP／UART／NVS／GPS與UI回歸通過；584C application OTA 完成，新boot／M0中位／保存速限／持續STATE／SD記錄與HTML比對通過；[部署驗證](docs/records-index.md#servo-ui-deployment-2026-10-11)，物理轉向／端點仍待驗收。

- **2026-10-10 資訊頁獨立 demo／Youmi 控制板需求**：新增 `demo/info-page/`，三區整理、
  HDOP／GPS狀態與診斷年齡、Client電量／STATE／DATA、Station溫濕度／SD／Servo、
  點數字改速限、整數指南針輸入／鏡頭置中、來源toggle、七種mock情境與pending→STATE確認。
  使用者進一步確認完整圓形0～359°軌道與只留手動／GPS；demo追加放開才套用、端點不環繞的雷達預覽。
  demo不含GPS α、複製摘要或手機權限流程變更；CSP禁止API連線，無實機請求／持久化。
  使用者指定Youmi 35kg 5–8.4V 359°＋9600baud雙路控制板；圖片協定、8.4V／共地／手動燈／
  旋鈕接近全圈目視確認已記錄，通道／GPIO／TX電平／ACK與角度映射待驗。
  正式後續取消115200外部UART輸入，改9600控制板輸出；本輪正式180°LEDC／GPS α功能／雷達未修改。
  JS語法及瀏覽器七情境／速度超限／儲存／取消／指令確認／指南針範圍／來源toggle／展開通過；
  320／360px無橫向溢出、速度視窗可操作，JS error為0。
  圓形把手44px、拖曳90°、Home／End與跨端點夾住0／359、GPS不可拖曳通過。
  本輪無韌體修改、build／燒錄或實機試轉；見 [控制板契約](docs/records-index.md#servo-controller-youmi-2026-10-10)
  與 [UI新預覽規格](docs/web-ui-design.md)。

- **2026-10-10 交接文件與帶殼外測分析**：補交接入口、最新 WIP／舊 worktree 差異與分享快照說明；
  README／功能／介面／硬體／入口同步 B 組 SF9、T096 診斷節奏及 RTC 30 秒計時／指令確認界線。
  帶殼內建 GNSS 外測約 61 分鐘才見有效位置，157 有效 DATA、速度旗標全無效；seq 0–160 全收，
  保留 GNSS checksum／快照遺失與攜帶關係未知，不單憑手機對照判天線故障。
  外接 GNSS 需核對官方選路電阻／天線規格，尚未改硬體。私人原始路線不納入 Git／分享包。
  本次僅更新文件與準備分享包，驗證連結／快照完整性；沒有程式修改、重新 build 或部署。
  現況見 [交接入口](docs/assistant-handoff.md)、[10/10 外測摘要](docs/records-index.md#t096-field-validation-2026-10-10)。

- **2026-10-10 T096 RTC 待機實機部署／radio 喚醒修正**：實機發現 RadioLib 7.8.1 的 TX 路徑
  在喚醒前讀取 packet type，導致 warm-start sleep 後無法繼續發送；送出前明確 `standby()`，
  喚醒失敗不開 FEM TX、安排重試，失敗發送也遵守500 ms guard。
  實際 sendBuffered 主機測試新增睡眠時禁止 TX 讀取、喚醒失敗與失敗 guard 回歸；
  radio／idle／power／position 通過，Client build 與 F035 application-only DFU 完成。
  新 boot 2126673892 持續 Ready，每30秒增量 STATE；啟動GPS／回待機分別 confirmed，最後錯誤0。
  Station 配對、B 組 SF9、手動90°及SD錄製保持；首次候選失敗與修正原始紀錄保留。
  純電池RTC待機120.944秒／5筆增量STATE、同boot與Ready通過，該段HTTP無錯誤。
  電池下啟動GPS30.776秒／回待機5.399秒均confirmed；額外指令觀測前有2次HTTP讀取逾時，恢復後完成，原失敗log保留。
  按鍵／充電座切換、低電恢復與整機電流／續航仍待驗；詳見[RTC待機驗證](docs/records-index.md#t096-rtc-standby-2026-10-09)。

- **2026-10-09 T096 保留狀態的 RTC 待機**：日常改為啟動GPS／待機；移除12小時及USER長按SystemOFF，
  Ready每30秒交換指令，以FreeRTOS tickless RTC阻塞loop，DIO1／USER中斷可提前喚醒。
  SX1262空閒warm-start sleep／FEM關閉；健康待機ADC每30秒、警告／Tracking每秒。
  三次<3.2V停止GPS／新RF，每60秒檢查，三次≥3.5V回Ready、不自動恢復GPS；保護板切斷後恢复待實測。
  Web啟動GPS／回待機移除深度休眠，舊store API轉Stop，wire v5數值與長度保留，Servo／GNSS定位流程不變。
  native139/139、9組主機回歸與雙端build通過；E91C應用區已更新並核對配對／API／SD恢復。
  當日F035未接USB；10月10日接上後部署及修正見上方紀錄。狀態見 [RTC待機驗證](docs/records-index.md#t096-rtc-standby-2026-10-09)；整機電流與續航仍待量測。

- **2026-10-08 T096 收納後 USER 喚醒修正**：進入 SystemOFF 前依 nRF52 bootloader 契約設定
  一次性的 skip-DFU 標記，避免 USER 喚醒仍按住時被判為進入 BLE OTA。
  SoftDevice 啟用／停用路徑分開；SDK 準備失敗時保留供電、取消關機並回報 Ready，不帶錯誤標記進入 SystemOFF。
  USB identity 補上 bootloader 原始版本與 reset reason。保留 USER 短按／長按、12 小時收納、
  螢幕 5 秒、電量顯示、SF9／wire v5 與應用程式之外的開機器／SoftDevice。
  power／radio／GNSS／display／position 回歸與 build 通過，F035 application-only DFU 已完成，
  實機 bootloader 0.9.0、新 boot／持續 Ready 已確認；純電池按鍵／網頁收納→USER 喚醒兩輪均通過
  使用者目視與新 boot／持續 STATE；另通過 WOUT 充電座上短按 USER 喚醒，40 秒內持續 Ready，
  移離充電座後 60 秒同 boot／持續 Ready、HTTP 無錯誤，電池接手驗收通過。
  收納後單純放上充電座未觀察到自動喚醒；收納電流、充電電流與續航另待驗。狀態見
  [T096 電源與喚醒紀錄](docs/records-index.md#t096-power-validation-2026-10-08)。

- **2026-10-07 B 組改 SF9，保留 18-byte DATA**：T096／對應 Station 由 RF group 設定取得 SF9，
  A 組維持 SF10；923.8 MHz、BW125／CR4/5、wire v5、全部欄位／長度與 T096 500 ms 排程不變。
  RF loader 明確傳入並檢查 SF，拒絕重複 macro 與未驗證值；T096 USB identity 顯示 SF。
  SD session 原先硬寫 SF10／923.2 MHz，改記實際 radio profile，舊檔不修改。
  SF9 DATA airtime 185.344 ms（SF10 為329.728 ms），GNSS DIAG 267.264 ms；
  RX window／控制時限與 Servo 行為不變。SF9／SF10 無法互通，其他未更新 B 組 Station 仍需另行部署。
  native139/139、10組主機回歸與雙端build通過；E91C application-only USB與F035 DFU已完成。
  桌上110個18B probes counter無缺號，Test／Stop confirmed、SD CRC匯出與恢復錄製已確認；
  戶外／海面仍待驗。完整驗證及部署界線見 [SF9 更新紀錄](docs/records-index.md#sf9-field-pair-2026-10-07)。

- **2026-10-06 LilyGO E91C 改為 Station／SD 記錄**：核對 USB 身分後，以本機
  `station-lilygo-e91c` profile build／application-only 燒錄，保留 0.6-dev、wire v5 與 B 組923.8 MHz。
  NVS 綁定 T096 F035，實際 Ready 接收及 Stop 命令 confirmed；重啟後綁定保留、SD 自動恢復錄製。
  依使用者更正不填 log server key，Axiom 停用。SD 匯出541,224 bytes、277筆JSON、CRC正確，
  包含5筆F035 STATE原始封包；驗證與備份界線見[部署紀錄](docs/records-index.md#lilygo-station-2026-10-06)。

- **2026-10-06 T096 TFT 面板偏移修正**：通用 Adafruit 1.11.0 的
  `MINI160x80_PLUGIN` 預設 offset 為 (26,1)，Heltec T096 原廠 fork 為 (24,0)。
  新增板專用 display wrapper，在初始化後、rotation 前套用原廠 offset；保留160×80、
  rotation 1、色彩反轉、單行buffer與5秒亮屏。修正及部署驗證見
  [T096 控制筆記](docs/records-index.md#t096-controls-2026-10-05)。display／power／GNSS／radio回歸與build通過，
  F035 application-only DFU成功，新boot與Station Ready已核對；使用者目視確認文字正常、不再斜著錯位。

- **2026-10-05 T096 按鍵／螢幕／電量與 Station LoRa 分級**：兩種 Station OLED 移除 `TEL` 替代字，
  依最近接受的綁定 Client DATA／TEL／DIAG／GNSS／STATE／RF probe RSSI/SNR 顯示 Good／OK／Bad；
  90 秒無有效 RF 才 Miss，位置與 GPS freshness gate 不變。資訊 API 的即時 RSSI/SNR 同步改為任一有效封包，
  DATA 的平均／FPS 保持原定義；網頁有效的0 dBm不再誤顯示為未知。T096 新增 160×80 TFT、5秒亮屏、USER短按重計時／長按2秒放開才收納；
  USB供電時長按改回Ready，RST仍為硬體重啟。採原廠3.0V／12bit／4.9倍ADC換算，每秒在非收發時取樣；
  <3.5V提醒、三次有效讀值<3.2V顯示5秒後SystemOFF，USB抑制且未知值不誤關機。STATE／TEL帶電壓；
  Station畫面／提醒／cloud採90秒內最近STATE或TEL電量，不把STATE冒充溫濕度遙測。
  TFT以5KiB單行buffer送出，背光5秒GPIO期限不等RF frame；Ready保留Vext但TFT睡眠、GNSS load switch關閉，
  SystemOFF才切Vext。主機驗證與三裝置部署狀態見[本輪筆記](docs/records-index.md#t096-controls-2026-10-05)；ADC先信任原廠值，電錶與實體按鍵／畫面驗收仍分開。
  LilyGO USB、V4 OTA與T096 application-only DFU均已部署；兩台Station前一輪短RF測試各收100 probes且counter無缺號。
  T096實機驗證補上自建SPIM2的`begin()`：Adafruit不會自動啟用未宣告為variant SPI介面的物件，
  首次新版因此未走到持續radio／ADC回報；回歸新增TFT初始化前SPI必須ready的外部函式庫契約檢查。
  修正後T096 raw ADC換算約4.14V、STATE／TEL電量正常；V4收70個新Client probes無counter缺號，Web開始／停止均確認並回Ready。
  本輪GNSS UART設定可回讀，但室內無有效epoch／位置DATA，定位與實際2Hz仍待室外驗收；面板與純電池電流／關機另待實測。

- **2026-10-04 LilyGO Station 共用 T096（桌上部署）**：device manifest 允許多個 Station profile
  指向同一 Client，保留同 RF group 與 USB／名稱唯一性檢查。T096 每次開機只接受第一個 Station
  控制的規則不變；切換控制 Station 需重啟 Client。原板有效 NVS 中的 Axiom ingest 設定已取回至本機私人檔案。
  LilyGO 原約117 KB閒置 heap，無法同時負擔32 KiB batch與原100 KiB TLS門檻。
  單純降低門檻的實測最低 heap 只有5,496 bytes，因此改啟用 Supreme 的 QSPI PSRAM，
  batch 優先配置於 PSRAM；未有可用 PSRAM 時保留內部 RAM fallback。
  TLS前保留原100 KiB總量／48 KiB連續區塊與48 KiB採樣門檻，buffer／queue容量不變。
  Axiom狀態新增free／largest heap、門檻、PSRAM容量及batch位置。
  device config 7 tests、Axiom／debug主機回歸與LilyGO／V4 build通過；僅LilyGO實體部署、ROM hash驗證。
  最終120.270秒觀測219個probe、缺號0、最大間隔1,014 ms；最低heap70,220 bytes、兩端boot不變。
  雲端回讀164 records且isPartial=false；並行仍有2次4秒HTTP逾時。Client stop確認後回Ready，Servo維持manual 90°。
  這是USB近距離桌上結果，非水上距離／手機熱點或續航驗證；細節見docs/lilygo-t096-2026-10-04.md。
  當晚另以既有API將同一ingest key設到V4，確認NVS儲存／連續上傳與雙Station cloud分組回讀；
  V4未重新燒錄／重啟，表單仍維持9/22移除後的設計。兩站當下皆無Client STATE，不把cloud成功當成RF證據。

- **2026-10-02 Heltec 夜間修正／無 SD 記錄（桌上部署）**：修正 T096 Vext/GNSS 共用上游供電，
  加入 UC6580 專用暫存設定及回讀；R6 Build3700 實測 RMC/GGA/epoch 2.00 Hz，室內無 fix。
  STATE 接收窗改為 85 ms listen、有 preamble/header 才延長至最多800 ms；Station 20 ms 後下行，
  超過40 ms略過。RF 每500 ms最早時槽，獨立DATA seq、正確同epoch配對／無效轉換與低頻wire-v5診斷，
  USB connected但未讀取不再阻塞Client。新Client boot清除舊定位／診斷／RF測試統計與命令基準。
  V4 使用既有3.375 MiB SPIFFS分區做有CRC、bounded queue的RF/phone/Station Flash記錄；10秒flush、
  滿即停、未知內容保留、不自動erase，新增唯讀`/api/flash`與USB匯出解碼。Axiom入口保留，缺ingest token時維持disabled。
  native139/139與33支Python/JS回歸通過；V4/T096已USB部署。第一次22-frame實機匯出CRC全數通過，
  app更新重啟後原11,264 bytes逐byte保留。
  實機重現空TCP preconnect占用單一HTTP入口數秒；未送任何bytes連線加250 ms期限，
  若有其他連線排隊則100 ms寬限後讓位，保留請求內容／SSE流程。
  `timing.http_idle_closed`呈現清理次數；同一空連線情境由6.9秒改善至0.183–0.217秒。
  V4另關閉Wi-Fi modem sleep；20次Ping平均62.7→7.1 ms，後續相同RF負載驗證記在[Heltec外測準備](docs/heltec-field-ready.md)。
  最終版本600.15秒收到1,089個probe、counter缺號0、Client無重啟、Flash dropped/errors為0；
  並行壓測仍有2次4秒API timeout，端到端網頁延遲尚未完全排除，未宣稱水上鏈路或室外GPS已通過。
  交付前648 frames完整匯出、CRC／decode／drop均0，桌上記錄備份後清除；重新開機驗證recording，
  T096 Ready、B組923.8 MHz、手動90°及MAC固定網址可用。
  校正深眠文件：nRF52840原生支援VBUS上升wake，實際無線接收器路徑／電流仍待接線量測；不再將未驗證說成晶片不支援。


- **2026-10-02 Station MAC 固定網址（V4 USB 已部署）**：device config 的 `hostname: "auto"`
  選擇既有完整 Wi-Fi MAC 命名路徑；範例與本機 V4 改採 auto，保留自訂 hostname。
  多台 auto 不視為重名，只有實際自訂名需唯一；`plan` 不以 USB serial 冒充 Wi-Fi MAC。
  config 6 tests、V4 build 通過；app-only 燒錄 hash 驗證成功，API 回報 MAC 名稱，
  電腦瀏覽器以新 `.local` 名稱開啟控制頁成功，RF 收包恢復。手機熱點端尚待驗收。
  T096 2 Hz 缺口已補程式對照紀錄，本次未變更其 GNSS／RF 排程。

- **2026-10-02 每台 Station 固定名稱（V4 USB 已部署）**：device manifest 新增必填 `hostname`，
  檢查 DNS 格式與唯一性；`SHORE_DEVICE` 將名稱帶入 Wi-Fi／mDNS／OTA，並宣告 HTTP service。
  通用 Station build 改採完整 MAC 後綴，避免多板使用同一名稱。`/api/track.hostname` 與 `plan`
  可查名稱；本機 V4 設為 `shore-b`，RF group／頻率與 Client 排程不變。
  config 6 項與既有 debug／packet 回歸通過，V4／T-Beam Station build 通過。V4 app-only 燒錄 hash 校驗通過，
  API 回報 `shore-b` 且保持 F035／923.8 MHz，電腦瀏覽器實際以 `http://shore-b.local/` 開啟控制頁成功；
  手機熱點端與桌面捷徑尚待實機驗收，T096 2 Hz 移植缺口仍待修正。

- **2026-10-02 多板 Station、T096 Client 與固定 A/B（Heltec USB 桌上部署）**：
  共用 Station 邏輯新增 Heltec LoRa32 V4 R2 profile／FEM 偵測與 SD/GNSS 能力區分，
  保留 LilyGO；T096 採 nRF52840 + SX1262/FEM 與真 GNSS source，RF 測試另用 probe 不偽造座標。
  `config/rf-groups.json` 固定 A 923.2／B 923.8 MHz；本機 device manifest 以 USB serial、角色、
  配對與組別管理，`SHORE_DEVICE` 編譯時選組，首次配對可填 Client radio_id，既有 NVS 優先。
  wire v5 保留原封包，新增 CONTROL/STATE/PROBE；Web 命令需 Client STATE 回覆才確認。
  Client 待命每 30 秒回報並開 800 ms 接收窗，運行／測試每 5 秒；待命 12 小時進 SystemOFF，
  USB 供電時抑制，拔除 USB 重新計時；追蹤不因 RF 失聯自動休眠。深眠不能由 Web 喚醒。
  手機定位由 HTTPS 定位頁回傳 Station HTTP 視窗，含 origin/source 檢查、時間與精度；
  Station RAM 保留最後成功位置直到重開機；HTTPS helper 已部署並核對 HTTP 200／內容一致。
  原追蹤／Servo 模式不因開始或停止 Client 改變。
  Heltec 的 0 dBm 為 SX1262 drive 設定，不能當天線端功率；充電板未接、待機電流與水上效果未驗證。
  V4 原廠 16 MiB 已備份，device digest 比對成功。V4 app-only 燒錄 hash 驗證、T096 application DFU 完成；
  修正 nRF UARTE 未 begin 就 end 的開機卡住，以及 FEM 延遲使舊時間 underflow、RX 窗立即關閉。
  四種 profile build、native 138/138、實際函式 UART／RF lifecycle 與既有主機回歸通過。
  實機 RF 測試 60.9 秒新增 99 probe，counter 缺號 0；test/start/stop 均由 STATE 確認，
  Station 重啟後保存配對並同步命令序號；USB 中 store 回 409。LilyGO 未接，未燒錄。
  GNSS fix、水上 RF、深眠電流／充電喚醒與手機定位授權仍待實測。詳見 [桌上驗證](docs/records-index.md#link-test-2026-10-02)。

- **2026-10-02 多板移植前基線保存**：整理 9/13 之後既有 v5／SF10、Station 命名、Web、
  GNSS／SD／Flash 外測診斷與電源修正，保留各條目的歷史部署界線。
  本次重新執行 native 及 `tools/test_*` 共 25 支 Python／JavaScript 測試程式，全部通過；
  此保存點尚未加入 Heltec 支援，也沒有重新燒錄或新增外測證據。

- **2026-09-24 Client 開機資訊頁的新按壓（已 USB 部署 E91C）**：使用者確認 USB 接著時，在開機資訊頁
  重新長按會直接黑屏、沒有 `SHUTDOWN`；先前 guard 在整段開機顯示持續重置，確實抑制了這類軟體關機。
  改為第一次完整讀取／清除只建立一次基準，之後新的下降沿在動畫／資訊頁內也能接受短／長按；
  不以無 IRQ 猜測已放開，初始殘留或持續按住仍不能觸發軟體關機。關機請求返回時不再重畫開機頁，
  資訊頁末段的短按保留既有十秒顯示期限。Station、硬體長按時間、充電／供電設定不變。
  28 組實際 power handler／boot loop 主機測試通過，含頁面內新長按與末段短按期限；
  合併後 Client／Station／Trip 三個 build 通過。實體按鍵顯示待驗收；這不代表電池供電問題已修復。

- **2026-09-24 Client PMU 電池原始診斷（已 USB 部署 E91C）**：針對正常電池接入後仍 `battery_mv=0`，
  Client diagnostic／trip 新增 `pmu_battery`：保留原本四次偵測／ADC enable 呼叫的結果，
  與 signed raw `00/01/30/34/35/68/22/27`、逐寄存器錯誤、PMU lock／online及取樣耗時。
  初始化後、首個主迴圈各記一次，之後每五分鐘記錄；VBAT 高低位相鄰讀取，所有設定與關機判斷維持原樣。
  唯讀 I2C helper 檢查 TX／短讀，不使用可能等待一秒的 `readBytes`；共享 PMU lock 只嘗試一次。
  可分辨未讀、匯流排失敗、電池未偵測與 ADC／偵測設定；也保留硬體長按關機設定，
  不把沒有 SHUTDOWN 畫面直接當作電池失電。一般 Client／Station 不加入這些讀取。
  主機驗證：19 個 handler JSON 情境、15 個 transport 情境、16 組初始化回傳組合與真 Flash codec 通過；
  合併按鍵修正後三個 build 通過。Trip 六個實際 SD worker 情境通過，
  更新後 24 小時容量模型為 2888/3072 frames；額外事件與既有占用會縮短可錄時間。
  E91C Trip App 746,912 bytes，SHA256 `e5ea4ae30919643e435a276d9898bf9ea37c2b1a42bdae52798d1d692a3dbc1c`，
  app-only 寫入前後完整 Flash 校驗通過，舊記錄保留；Station 未燒錄。
  新 boot `4153263253` init／runtime 讀取均成功（4 ms），偵測與 ADC 已啟用但 PMU 報電池 absent；
  VBAT ADC 366／368 mV 不能當作電芯量測。SD／Flash errors、drops 均 0；因疑似板面受損，
  最後 SD 完整同步後停止，實體按鍵驗收延後，電池供電原因仍待硬體量測。

- **2026-09-24 Client 電池開機 PKEY 殘留與 3 秒動畫**：純電池重試的兩次 boot 都在 uptime 12,782 ms
  記下真實 long IRQ 與 `shutdown/pwr_long_press`，電池 4.159／4.160 V、I2C read/clear error 0；
  SD／Flash 均保存主動關機意圖，確認開機畫面後首次讀取把啟動期間累積的按鍵事件當成運行關機。
  這與 signed I2C 誤判修正不同，也不能當作較早外測重啟已全部定因。
  Client 將開機手勢與之後的新按壓分開，保存 startup 原始 IRQ 與抑制原因；Station 按鍵行為不變。
  新增 3 秒單色雷達／浪形開機動畫，之後保留 10 秒資訊頁；GPS／診斷／USB／PMU 服務持續執行。
  主機驗證：23 組真實 PMU／main handler／boot loop 案例通過，含實機兩個啟動 latch、400 ms OLED 初始化、
  完整 3 秒動畫與 10 秒資訊期間服務、OLED 永久失敗有界退出、後續短／長按及 I2C 錯誤恢復；
  實際 U8g2 renderer 檢查每毫秒 0–3000 及最大時間值，產生 30 個預覽 frame；Trip／GNSS／USB 回歸通過。
  一般 Client／Station／Client trip 三個 build 通過。僅 Client E91C 燒入 trip app 745,968 bytes，
  SHA256 `71a689b9b150a9ac0f67c9ae331ecf0b0d2e7c16c24886c384dbc252a72bd2be`；更新前後整顆 8 MiB device digest 與預期相符，
  只寫 app0，NVS／SD／既有 Flash 診斷資料保留。新 USB 開機驗收：raw seq 0–139連續，136句正常NMEA checksum全過，
  SD回讀95,636 bytes／CRC `7e8fbafb`；10組SD/DIAG連續status、2次舊SD回讀及Flash完整匯出通過，
  舊Flash前31,744 bytes逐byte保留；SD/Flash errors、drops、USB write errors均0，SD已恢復錄製。
  `pmu_startup/wait_new_press`有保存，未出現shutdown；純電池按鍵重試及實體OLED觀感仍待使用者確認。

- **2026-09-24 Client 電源／USB 修正與長時間外測版**：PMU IRQ 保留 signed I2C 結果，讀取失敗不再變成
  0xFF 誤判短／長按；clear 失敗不重播同一按鍵，VBUS 讀錯不當作純電池低電量。長按優先，關機意圖先於 OLED／SD drain，
  保存原始 IRQ、讀取／清除結果、boot／uptime 與累計錯誤；持續相同錯誤每 10 秒彙報。
  SD／DIAG USB router 保留一筆完整 pending 命令，完成回覆後的下一命令不因 cleanup 尚未完成而消失；背壓／queue error 可見。
  實機初版驗收另見 USB 忙碌期間 text queue 丟失（含 8 筆 raw），新增錄製中 USB write 絕對 50 ms 上限與
  `usb_write_errors`；非法命令附有界接收 hex 證據。原始初版資料保留，未將首次非法行歸因為已證實的 bootloader 殘留。
  新 `tbeam-client-trip` 保留 1 Hz、固定 RF on；SD 無定位也錄原始 UART hex／epoch／TX，Flash 只即時保留電源事件，
  snapshot 60 秒、SD status 300 秒，不再跑 P0–P6；Flash 滿不影響 RF／SD。一般版 2 Hz、LoRa wire、Station 追蹤／Servo 不變。
  原韌體 USB 對照已依使用者要求停止：09:58–13:07 未觀測重啟，45 次 SD 回讀通過；1 次已知漏指令逾時，
  host 交接有 81.165 秒 console 缺口，不能排除純電池／接觸故障，也未將 PMU 候選定為外測根因。
  驗證：native 129／129；PMU 14 組、USB completion 11 組、錄製中 USB 背壓 4 組、trip 真實 producer／worker 6 情境，
  以及既有 GNSS／packet／SD／diagnostic／motion 主機回歸通過；一般 Client／Station／Client trip 三個 build 通過。
  部署：Client E91C `tbeam-client-trip` app 740,960 bytes（SHA256 `2da2f0df2f3a40be0f4f4f75981a8ecdb798cd3a4c5b73a51a509526b4bdc392`）；
  更新前後整顆 8 MiB 與預期內容 device digest 相符，NVS／其他分區保留，SD 未格式化；Station 未燒錄。
  最終室內實機：同 boot 下回讀 SD 162,467 bytes／CRC `791405eb`、222 段 raw seq 0–221 無缺口，220 句正常 NMEA checksum 全過；
  另保留開機 backlog guard 的 100 bytes 原樣。無有效定位仍錄製，6 次診斷／遙測 TX 本地成功，不代表 Station 收到或戶外精度。
  30 對 SD／DIAG 連續 status、4 次 LIST→READ→DIAG 舊檔 CRC／SHA 校驗通過；Flash 18 frames 含兩次受控部署 boot，完整匯出。
  最終 SD／Flash errors、text／client drops、USB write errors 皆 0，SD 已恢復錄製。
  初次格式錯誤的新增 RX hex 證據顯示裝置收到自己 BOOT／PMU／LoRa 開機文字回送；host／USB bridge 的具體回送層仍未定位。
  純電池、外殼／接頭／按鍵移動與戶外穩定性尚待使用者實測。詳見 [field-diagnostic.md](docs/field-diagnostic.md)。

- **2026-09-23 Station 改名與 L76K 獨立診斷版**：專案角色／編譯環境／OLED／Web／mDNS 改為 Station；
  `/api/track.station` 表示岸端，30 秒平均移至 `station_average`；debug／本地 snapshot schema 升 3。
  LoRa wire／NVS key 不變，舊 SD 段仍可辨識；Axiom 上傳明確保留 schema 2 歷史 key 相容，避免同 dataset 新舊欄位聯集超限。
  LoRa 顯示分開已接受 RF（90 秒）與 DATA 新鮮度（5 秒）；只有 TEL／DIAG 時不再說成 RF MISS，GPS／追蹤 gate 不放寬。
  新增兩端 `*-diagnostic`：1 Hz RMC+GGA、內建 1.5 MiB Flash 原始 UART／UTC／parsed snapshot／重啟與 PMU 原因，
  Client 自動比較 RF／SD 階段，外出不用帶筆電；正常版本維持 2 Hz。
  記錄有界 queue、512-byte CRC/readback frame、滿停止及未知內容保護；回家 USB 匯出及逐 frame 重試，
  原始 bytes 與完整性報告留存，不以 SD／LoRa 作診斷資料唯一來源。
  詳見 [field-diagnostic.md](docs/field-diagnostic.md)。驗證：native 129／129、診斷 Flash worker 17 情境、USB reader 14 項、實際 UART／USB router／UTC／phase、
  SD／packet／motion／debug／Axiom backend 與 UI 回歸通過；Client／Station／Station OTA／兩端診斷版共 5 個 build 通過。
  診斷產物與 SHA256 已另存。2026-09-24 Client E91C USB 部署完成：先保存 8 MiB 原始 Flash、
  以裝置 digest 核對備份；限定初始化診斷區並更新 app0，更新後整顆 Flash 與預期內容 digest 相符，NVS／OTA／其餘區域保留。
  室內實測 RMC／GGA／epoch 各 1.00 Hz，Flash 記錄／USB CRC 匯出／受控重啟後逐 byte 保存通過；
  初次 35 frames（17,920 bytes）、重啟後 81 frames（41,472 bytes），dropped／errors／corrupt／sequence gap 均 0。
  Station 新版尚未部署；完整 RF／SD 階段、實際關機尾端保存、外測精度與穩定性仍待驗收。
  後續首次外測：P1–P5 完成，讀回 1,702 個 Flash frames 與 290,040 bytes SD 記錄，CRC／記錄 dropped／error 通過；
  正常長按關機事件保存。1,124 個解析座標及 772 個 fix DATA 座標與原始 NMEA 吻合。
  對手機停留中位距離 1.20 m、移動 8.46 m，但移動期間 3 次非預期重啟，穩定性未通過；另有兩次 backlog guard 短失效。
  這輪只讀回／分析，不重燒或改控制行為；詳見診斷文件之外測章節。
  後續 11 個主機故障注入案例確認 PMU INTSTS2 讀錯會誤判短／長按，且 shutdown intent 前有 OLED 初始化延遲；
  尚未證實為外測根因。原韌體 USB 長測已啟動，僅保存／測試，未修正或重新部署。
  長測另確認 SD USB 完成回覆早於 worker 釋放串流，緊接的下一命令可能被靜默丟棄；主機測試已重現，
  實機無 header 逾時未伴隨重啟。主機加 500 ms 間隔後首次回讀 CRC／SHA 通過；裝置韌體此項尚未修正。

- **2026-09-23 兩端 SD 循環記錄與 Client GPS 省電**：Station／Client 開新段前回收最舊自家 log，保留
  至少 5%（最低 64 MiB）空間與下一個 32 MiB 分段；嚴格檔名／session 辨識，不刪其他檔案、不自動格式化。
  新檔跨開機遞增序號，舊 boot-ID 檔以修改時間／檔名排序回收；累計 bytes 升為 64-bit。
  Client 加入 GPS epoch、品質／診斷、TX 原始封包與完成／錯誤事件的背景有界 queue、批次寫入及 5 秒同步。
  僅有效新鮮 GPS、至少 6 衛星、HDOP ≤ 3 才錄製；失效後記原因、排空、同步、關 SD 電，恢復後自動開新段。
  不改 GPS／LoRa 發送節奏、功率或 wire 格式；USB 沿用原有工具，Linux 讀取不切換 DTR／RTS，
  關閉 HUPCL 避免意外進入下載模式；尚無 log 的新卡列檔回空清單，其他目錄錯誤仍明示。
  驗證：SD／Client／回收 26 個主機情境、USB reader、native 129／129、packet／motion／debug／Axiom／
  radio power backend 與既有 UI 回歸通過；Client／Station／OTA build 通過，最後列檔修正重編 Client／OTA 通過。
  部署：Client E91C USB 燒錄／hash 校驗成功，32 GB 卡可掛載，空清單正常，未格式化；
  無合格定位時 auto_enabled=true、gps_allowed=false、card_powered=false，I/O errors=0。
  重複開啟 Linux USB 工具保持同一 boot ID；尚無合格定位，實際 Client 記錄／戶外 RF／耗電尚待驗收。
  Station 584C OTA 的 TCP 3233 回連逾時後，依使用者接線改 USB 燒錄，build／hash 校驗成功；
  燒錄前 173,237 bytes 全部同步。新序號 log 111,304 bytes 經 USB 大小／CRC32 `457bb3be` 及全部 JSON 行
  驗證，含 27 筆快照與 3 筆 LoRa 事件；I/O errors=0、packet/text dropped=0、sample skipped=4。
  已開下一段並恢復記錄，overwrite=true；實機 HTML 與來源一致、API 正常。未把實卡寫滿或做戶外追蹤驗收。

- **2026-09-23 大按鈕與 10 秒 LoRa FPS**：修正模式群組中間按鈕繼承獨立圓角的缺口，只有整組兩端保留圓角。
  頂部整排改 18 px 字級／46 px 高度（約 1.5 倍），窄螢幕收緊水平留白維持單排。
  再縮小雷達下方方位字與提醒間距，調整可用圓圈邊界與燈號避讓。
  LoRa 燈旁顯示最近 10 秒 DATA 收包 FPS；新增 `/api/track.lora_fps_10s`，由 Station 固定容量時間窗
  計算接受的 DATA 包數 ÷ 10，排除拒收與其他包型，停收後自然歸零，不依賴瀏覽器輪詢。
  5 秒連線燈判斷不變，資料過期不顯示舊 FPS。設計規格同步更新。
  驗證：motion／slider／map／ring／radar layout／debug／Axiom UI 回歸及 packet backend 通過；
  FPS 覆蓋 10 秒邊界、停收歸零、恢復、持續 2 Hz、millis 回繞與拒收／其他包型排除。
  320×640／844×390 瀏覽器預覽確認單排大按鈕、分段圓角與雷達間距。Station／OTA build 通過。
  部署：OTA 握手未完成，改以 USB 燒錄 Station 584C 成功並通過 hash 校驗；燒錄前 SD 已停止且
  2,618,154 bytes 全部同步。重啟後實機 HTML 與來源一致，API／燈號顯示 0.0 FPS，
  SD 恢復記錄、I/O errors=0。目前無 LoRa DATA，非零 FPS／RF 收包表現尚未做實機外測。

- **2026-09-23 自適應燈號與浮動提醒**：提醒列／展開內容整區半透明覆蓋圖面，不再佔用雷達高度；
  展開上緣按下方 NESW 方位圈安排，長清單在面板內捲動。雷達使用完整畫面並加大可用圓圈。
  GPS／UART 只隱藏軌道、刻度及拖曳提示，保留不可操作的目前命令角度圓形標記。
  新增 Station GPS／Client GPS／LoRa 垂直燈號群組，依有效定位／既有 5 秒 DATA 連線狀態分別亮綠或紅；
  track 超過 3 秒未更新則灰燈，避免沿用過期綠燈。直橫向與接近方形畫面自動選燈號位置及圓心，
  避開雷達外圈／把手；地圖固定左側。
  新增 [網頁設計規格](docs/web-ui-design.md)，AGENTS.md 要求修改 UI 前先讀，按最新使用者指示更新規格，
  不得自行恢復移除項目或擴大成控制行為變更。
  驗證：motion／slider／map／ring／radar layout／debug／Axiom UI 回歸通過；新增六種尺寸的圓圈避讓、
  燈號獨立判斷／過期恢復，以及資訊頁提醒高度不被背景輪詢覆寫的檢查。
  390×844／844×390 瀏覽器預覽確認直橫向、浮動提醒、地圖靠左與 GPS 角度標記。
  Station／OTA build 通過，Station 584C USB 燒錄／hash 校驗成功；實機三燈狀態與 API 一致，
  最終重啟後 SD 已恢復錄製、I/O errors=0。未做戶外定位／LoRa 或實體 Servo 轉角驗收。

- **2026-09-23 白底雷達與固定上半圓控制**：「資訊」與「雷達」間隔 10 px，模式順序改為「手動／GPS／UART」。
  手動軌道固定左 0°、上 90°、右 180°，不隨校正偏移／磁偏角旋轉；GPS／UART 隱藏整組滑桿。
  控制刻度仍沿用 `raw = 180 - ui`，地理波束另依校正計算。雷達改白底、深綠格線／方位／波束，
  GPS 標籤改白底深色文字。motion／slider／map／ring／debug／Axiom UI 回歸及 Station／OTA build 通過；
  主機與 320 px 本機預覽確認 GPS／UART 隱藏、手動恢復，以及固定上半圓和拖動角度。
  Station 584C USB 燒錄／hash 校驗成功，實機頁面確認白底、10 px 間距、模式順序與正上方 90°。
  燒錄前 SD 2,354,460 bytes 完整同步，重啟後恢復錄製、I/O errors=0；未驗證實體轉角。

- **2026-09-23 雷達操作位置與無定位顯示**：提醒展開／收合改為綠底白字醒目按鈕；
  「資訊」移到最左上、在「雷達」左側。兩端皆無 GPS 定位也能自由選雷達，失去定位不再強制切地圖；
  無定位時保留格線與手動外圈，不假造位置，GPS 自動追蹤門檻不變。
  角度數字從右下移入外圈把手，拖動同步更新，保留放開才送出及鍵盤操作。
  驗證／部署：motion／slider／map／ring／debug／Axiom UI 回歸、Station／OTA build 通過；
  320 px 瀏覽器預覽無橫向溢出，真實手勢驗證把手數字更新、提醒展開覆蓋圖面。
  Station 584C USB 燒錄及 hash 校驗成功；實機雙端無定位時可切回雷達，資訊入口與提醒展開正常。
  燒錄前 SD 3,441,645 bytes 完整同步，重啟後恢復錄製；未做實體轉角或戶外追蹤驗證。

- **2026-09-22 雷達滿版與外圈角度控制**：移除頂部分頁列，資訊入口改為圖面左下半透明按鈕；
  雷達／地圖置左、GPS／UART／手動置右，單排半透明疊放。移除路徑圖例與右上視圖／模式文字。
  移除頁底水平滑桿，使用雷達最外圈的 180° 可轉範圍，粗軌道／刻度／44 px 把手；只保留角度數字，移除操作提示文字。
  把手依既有 `raw = 180 - ui` 方向對齊瞄準線，拖動預覽、放開才送出，地圖也能調整。
  取消、第二觸點、切換模式／畫面不送出舊拖曳角度，保留鍵盤方向鍵與原本 HTTP 新鮮度／排程。
  任一端有效 GPS 即可選雷達；只有 Surfer 時以其為中心，不畫虛構岸站瞄準線；兩端無定位才切地圖。
  GPS 自動追蹤仍要求雙端有效定位。手機高度跟隨 visual viewport，移除提醒列額外底部 padding。
  UI 回歸（含外圈真實手勢與 HTTP queue、單端雷達、手機 viewport）與 Station／OTA build 通過；
  Station 584C USB 燒錄／hash 校驗成功，實機網頁核對滿版、圖面資訊入口及移除項目。
  另在實機反覆發生 SD write failure 後，改為每次最多 512-byte write，保留 4 KiB batch、
  5 秒同步及原本錯誤停止政策；13 個 SD 主機情境通過。改後 2 分鐘錄製 118 筆快照、460,157 bytes，
  USB CRC32 `aa0e2c96` 與全部 JSON 行通過，I/O errors=0、快照略過 2 次，已恢復錄製。
  長時間穩定性與卡片／驅動根因仍需驗證。

- **2026-09-22 UI 精簡與預設操作**：只保留雷達／資訊兩頁，移除除錯頁及其瀏覽器錄製／JSON 匯出／Axiom 表單、GPX／Strava／清除軌跡、Uptime、指定小標與雷達圖例。
  全頁改白底高對比文字／控制，雷達使用深綠底、亮綠格線及波束，地圖保留單獨配色。
  當時雷達／地圖與控制模式按鈕、狀態疊在圖面上方；後續改為上列滿版外圈控制。
  提醒列移到最底部、預設收合，展開以半透明面板覆蓋圖面，不減少雷達高度。
  瀏覽器只保留畫面需要的 5 分鐘路徑。HTTP HTML 明示 UTF-8。
  開機預設手動模式、Servo 置中 90°；採用 **90°有效校正偏移**，有效 GPS／磁偏角條件滿足後可直接切 GPS 追蹤。
  資訊頁保留偏移數值與手動重新校正；重開機恢復 90°，不沿用 RAM 校正。
  `/api/debug`、`/api/log`、`/api/axiom`、綁定及 start/resume/pause API 仍可直接呼叫；SD 改由 USB 讀回。
  雷達以上方校正偏移旋轉（90°時 E 上、N 左）；後續已放寬為任一端有定位即可使用雷達。
  地圖單端以該端為中心，雙端使用即時中點，已用實際繪圖函式驗證。
  入口盤點見 [功能入口](docs/function-entrypoints.md)。

- **2026-09-22 Station SD 完整場次記錄**：插卡開機自動錄製，獨立 HSPI／BLDO1 3.3 V；
  逐包保存最多 255-byte raw、CRC 錯誤與拒收原因、文字 log、每秒詳細快照與快照間狀態變化。
  不依賴網路、Axiom、Station GPS 或控制模式；LoRa v5／SF10、Client 排程與 Servo 行為不變。
- 主迴圈只做固定容量 zero-wait queue，core 0 背景批次寫入，每 5 秒同步與尾端 bytes 讀回，
  約 32 MiB 輪替、同名不覆寫；卡滿／故障停止記錄並保留計數，不自動格式化、不自動刪檔。
  新增 USB 狀態／停止／開始／列檔／讀檔與 CRC32 核對工具；SD 功能不新增網頁／OLED 控制。
  另提供明確 `SD FORMAT FAT32 ERASE` 指令重建單一 FAT32 分割區，只在授權清空時使用。
  無有效 UTC 的離線快照 `_time=null`，保留 boot ID／單調時間；Axiom 正常 UTC 輸出不變。
- OLED 啟動增加電源 readback、位址 probe 與一次 Wire 重建，無回應停止反覆顯示寫入。
  USB 燒錄前觀察到 BME280 未找到及大量 OLED I²C `ESP_ERR_INVALID_STATE`；尚不能只憑錯誤
  判定螢幕、排線或供電的實體故障，初始化成功也不等於目視亮屏。
- 主機 SD／USB、packet／debug／Axiom／motion／radio power 回歸與 native **129／129** 通過；
  新增開機 90°有效校正、地圖／旋轉方位與 UI 移除回歸，SD 12 情境另通過 ASAN。
  Client／Station／Station OTA build 成功；Station 584C 已 USB 燒錄與 hash 校驗，Client 未燒錄。
  卡片已依授權重建 FAT32，122,988-byte log 經 USB 大小／CRC32 與全部 JSON 行驗證，續錄正常。
  實機網頁核對白底、手動模式與 90°有效校正、圖內按鈕與底部提醒展開；此次沒有實際 LoRa 收包或戶外追蹤證據。詳見 [SD 記錄](docs/sd-log.md)。

- **2026-09-20 校正固定在 Servo 90°**：命令位置與目標均為 90° 且停止移動才接受，
  狀態新增 `servo.calibration_ready`；非 90°／移動中點擊顯示通知，後端重查回 409 並保留原參考。
- 資訊頁新增「回到 90°」與 `POST /api/servo/center`：退出 GPS／UART、切為手動，
  依共用速限回中；保留校正及設定，不自動恢復追蹤。沿用命令新鮮度、PWM／fault 拒絕及 HTTP 排程。
- 校正提示精簡為北 0°／東 90°／南 180°／西 270°，其他方向填普通磁針實際讀數；
  同步文件，修正硬體文件舊滑桿方向說明（目前 UI 90→120 對應內部 90→60）。
- 驗證／部署：native **129／129**、motion backend／UI、servo slider、debug backend／UI、Axiom UI 通過；
  Client／Station／Station OTA build 全部成功，僅既有 RadioLib USB CDC 提示，無本次 source 警告。
  已覆蓋非 90°／移動中／四捨五入邊界拒絕、回中取消追蹤且按速限移動、PWM fault／舊世代拒絕，
  以及在途滑桿先完成、未送舊目標取消。尚未燒錄，未驗證實體回中或現場追蹤。

- **2026-09-19 v5 定位／追蹤定案已實作**：18-byte DATA 改完整 32-bit E7 經緯度，移除 DATA 年齡；
  GNSS 診斷改 36-byte 單包／約 60 秒，無組裝狀態或重啟舊序號等待。所有封包版本為 v5，v4 拒收，雙端需一起更新。
- GPS 來源維持 **2 Hz**，LoRa 空閒且 guard 已滿即送最新一筆；中間資料略過、不補傳。
  收集器直接提供最新 epoch，RMC／GGA 缺欄保持未知，Client 配對短等最多 150 ms。
  本地位置以新 epoch 到達後時間、Station 遠端以新 DATA 接收後時間作 2 秒更新逾時；
  推算來源 age 不再 gate 追蹤。UART 積壓、重複／倒退、格式、單次失效通知及 3 秒序號重建保留。
- 品質差仍追有效新位置並警告；衛星 ≥6、HDOP ≤3 的雙端條件改為速度外推資格，品質未知／
  向量無效也禁止外推。原速度與方向保留，不偽造為 0。新 NVS `gpspred5` 預設 **α=0**，
  舊 gpspredict 不沿用；選用 α=1 僅按接收後時間外推。保留 Servo 速限、轉完最後目標與手動／故障撤銷。
- API／log 區分原始向量與 `prediction_active`；v5 DATA source_age_ms／sample_age_ms／
  inferred_source_* 為 null，本地 arrival_age_ms 另列。raw 封包容量 36 bytes、Client API／Axiom E7 座標。
- 驗證：native **129／129**；135 秒模擬收到 267 包定位、9 包診斷，所有 guard ≥80 ms、
  最大 DATA 間隔 984 ms。packet／motion／debug／Axiom、GNSS 命令／radio power 與四組 UI 驗證通過；
  來源估算偏移不擋新定位、重複不刷新、品質差仍追但禁外推、預測設定遷移與失效轉完目標均有檢查。
  Debug fixture 4878 bytes；Axiom 主機快照 1200 bytes、222／256 欄位；10 段 JSON 範例可解析。
  Client／Station／Station OTA build 全部成功，無專案 source 警告；Client **E91C 已 USB 燒錄、Flash hash 校驗成功**，
  Client 45 秒開機／運行紀錄確認 v5／SF10、20 dBm、ALDO3 readback，連續 5 個完整 5 秒窗
  epoch／RMC／GGA 皆 2.00 Hz；TX errors／backlog drops 為 0，尚無有效定位 DATA，戶外及雙端收包待驗。
  同日追加授權後 **Station 584C 已 USB 燒錄、Flash hash 校驗成功**；API／網頁核對 v5／SF10，
  ALDO3 readback／boosted RX 初始化成功，綁定 E91C／30°/s／Axiom 設定保留，預測 α=0。
  Axiom 恢復 HTTP 200；岸端仍缺有效 UTC epoch（燒錄前亦有），2 Hz 與雙端收包尚未實機驗收。
  部署與實測結果見
  [追蹤更新紀錄](docs/records-index.md#tracking-update-2026-09-19)。以下 v4 未部署階段為歷史，不代表 v5 現況。

- **2026-09-19 最新定位／追蹤更新**：本項取代下方尚未部署的 1 Hz＋1.5 秒狀態回報排程。
  Client 只送可編碼的新鮮新定位，radio 空閒且 guard 已滿就送；忙碌只留最新一筆、不排隊、不重傳。
  RMC／GGA 配對最多短等 150 ms；有效轉無效只嘗試一包通知，持續無效不送 DATA，恢復直接送新位置。
  TEL／DIAG／GNSS 三頁維持約 60 秒，無 GPS 也獨立發送；每秒最多一包診斷，有 DATA 時交錯，
  必要時可延後一筆定位。所有 TX 完成後至少留 80 ms；SF10、固定 20 dBm、無 ACK、ALDO3 初始化保留。
- 雙端 GPS 改 **115200／2 Hz RMC＋GGA 驗證版**：冷啟動與 GNSS 保留 115200 的 MCU 重啟
  使用同一初始化路徑，不另存 GNSS flash。原廠 >1 Hz 要求單語句，依確認先驗證雙語句，不靜默降級。
  每 5 秒量測 epoch／RMC／GGA Hz；Station 除錯與 Axiom 保留三值、Client 序列保留三值，
  Client DIAG bits6／7 新增「量測完成／三者約 2 Hz」。**舊 Station 會拒收新 DIAG 旗標**，
  wire 版本仍為 v4、長度不變；SF9／SF10 不互通，雙端均需更新。設定寫入不等於實測成功。
- Station 開啟 SX1262 boosted RX gain，初始化／復原錯誤明示；距最後接受 DATA **滿 3 秒**，
  第一包有效位置重建序號基準並記錄原因；連續通聯仍拒絕重複／倒退，不新增 boot ID／握手。
  GPS 失效停止更新／外推，但按速限轉完最後有效目標才停，API／UI 明示
  `finishing_last_gps_target`；手動暫停、模式切換、PWM 故障仍撤銷移動。2 秒新鮮度 gate 保留。
- 岸端增加 **30 秒座標平均**供追蹤幾何使用，保留 raw／mean／樣本數／RMS；RMS >3 m
  只警告、不丟離群點，平均不延長定位有效期。資訊／除錯／Axiom 同步提供；Axiom 原始座標保留。
  指南針仍為北 0°、東 90°、西 270°，未改公式。`AGENTS.md` 新增保持批判態度的協作原則。
- 驗證：native **130／130**；135 秒實際排程／封包模擬收到 264 包有效 DATA、15 包診斷，
  最大發送間隔 859 ms、guard ≥80 ms、GNSS 三頁在期限內收齊；驗證無 GPS 診斷、單次失效、
  最新一筆、編碼範圍外不反覆發 DATA、DIAG 實測旗標、序號中斷／回繞、raw／mean 分離、
  超時轉完目標／恢復／手動暫停／模式切換／PWM fault。實際 GNSS 命令 checksum 與
  冷／暖啟動主機模擬、ALDO3／RX boost 失敗路徑、packet／motion／debug／Axiom 後端及
  motion／debug／Axiom／slider UI 通過，10 段 API JSON 可解析。debug fixture 最大 4755 bytes；
  Axiom 主機快照 1032 bytes、220 個欄位路徑。Client／Station／Station OTA build 全部成功，
  無專案 source 警告，保留依賴庫／framework 警告。**尚未燒錄、未操作雲台**；GPS 實際 2 Hz、
  電源與 RX 性能、RF 距離及機械追蹤待實機驗收。範圍／外測散布／未決問題見
  [追蹤更新紀錄](docs/records-index.md#tracking-update-2026-09-19)。

- Client 發送改為 **新 GNSS epoch 觸發、最高 1 Hz**，維持 SF10、固定 20 dBm、無 ACK。
  RMC／GGA 最多短等 150 ms，無新資料每 1500 ms 以 DATA 回報狀態；保留真實來源 age，
  不放寬 2 秒追蹤 gate。GNSS 較快或執行延遲時只送最新快照，不排隊補歷史定位。
  TEL、DIAG 與 GNSS 三頁診斷改約 **60 秒**，利用 DATA 後空檔、前後各保留 80 ms，
  不取代定位；`skipped_slots` 舊欄名改計 radio 忙碌／未就緒造成的 DATA 延期，同一次
  等待只計一次。debug 新增 send_mode／status_heartbeat_ms／diagnostic_interval_ms，
  網頁顯示上限、狀態回報與診斷間隔。此項取代下方尚未部署的 500 ms 診斷占格版本。
- 雙端 **ALDO3 明確初始化**：修正開機先 radio 後 PMU 的順序，改先 PMU，再於每次
  radio 初始化／復原設定 3300 mV、啟用並讀回確認，短等 10 ms 後才存取 SPI。
  PMU 不可用／寫入／讀回失敗會回報錯誤，不繼續 radio 初始化；不依賴舊韌體殘留設定。
  ALDO4 GPS 與 ALDO1 OLED／BME280 行為保留。寄存器確認不等於實測電壓或 RF 功率。
  驗證（2026-09-19）：native **127／127**；135 秒實際排程模擬維持新定位 1 Hz、
  60 秒診斷不占定位、三頁在期限內收齊；另測無 GNSS 狀態回報、radio 延期僅計一次、
  只送最新快照、UART 年齡不刷新。ALDO3 寫入／啟用／讀回／PMU 失敗阻止 radio 初始化，
  開機 PMU 順序與兩角色 recovery 主機驗證通過。packet／固定功率／debug／Axiom／motion
  後端及 debug／Axiom／motion／slider UI 通過，10 段 API JSON 可解析；18 份其他檔案
  與修改前快照逐 byte 相同。Client／Station／Station OTA build 全部成功，無專案 source
  警告（保留依賴庫／framework 警告）。尚未燒錄，電源、耗電、RF 與實際定位待現場驗證；
  本次不新增 App／設定模式／SF11 或深度睡眠。

- LoRa 改為 **SF10／BW125／CR4/5、無 ACK 單向傳送**：Client 固定 20 dBm，
  移除 ACK codec／等待／接收狀態，非阻塞 TX 完成回 standby；Station 只收上行，
  不再產生下行封包。RSSI／SNR 仍由 Station 記錄。v4 DATA／TEL／DIAG／GNSS DIAG
  格式保留，舊 type 2 拒收；**SF9 舊板無法互通，Client／Station 必須一起更新**。
  500 ms 無線時槽保留；DATA／DIAG 約 330 ms、TEL 約 289 ms，加 80 ms guard。
  低頻 TEL、DIAG 各約 30 秒一次，GNSS 診斷約 30 秒一組三頁，改占完整時槽並與
  DATA 交錯；正常定位間隔通常 500 ms、遇診斷 1000 ms，平均約 1.83–1.84 Hz。
  診斷不消耗 DATA seq、不算 skipped_slots，Client 序列摘要另列 diagnostic_slots。
  診斷不會連續發送；過期時槽略過，無 ACK 重傳。GPS 原始更新率與 2 秒過期 gate 不變。
  status／debug／Axiom 保留舊 ACK 計數為 null，並加 ack_enabled=false；debug 設定的
  ACK 長度／週期／airtime 為 0，Axiom 不再依 ACK 判斷故障。網頁及介面文件同步說明。
  驗證（2026-09-18）：native 122／122；實際 SF10 排程 95 秒模擬無重疊、診斷與 DATA
  交錯、正常 DATA 最大間隔 1 秒、三頁期限與序號一致；packet／固定功率／debug／Axiom／
  motion 後端及 debug／Axiom／motion／slider UI 通過。Client／Station／Station OTA build
  全部成功，僅依賴庫／framework 既有警告，沒有專案 source 警告；10 段 API JSON 範例
  可解析，12 份其他既有 WIP 逐 byte 未變。尚未燒錄，實際 RF 距離、收包率、電耗與
  GPS／追蹤表現待實機驗證。
  以下 9 月 15 日以前的 SF9／ACK 排程與部署紀錄為歷史狀態，不代表此版已部署。

- Axiom log schema 2：每秒完整快照新增英文 `message`／`level` 摘要與錯誤增量 `delta`；
  Client 收包、Station GPS、模式／來源／GPS 條件與 PWM／控制故障改變時額外送一筆
  `state_change`，合併同次觀察的變化，正常待命為 info。未收到的 Client 診斷改為 null，
  保留原始計數、來源年齡與 raw 封包。第一次觀察／重啟／設定世代／換綁定重建增量基準。
  格式化與 64-byte 主機比較狀態都在背景 worker，不增加 loop 快照；維持 8 筆 queue／32 KiB
  batch，每批最多 5 快照及其變化事件。收件計數按實際文件數驗證，API 名稱保留並補充語意。
  英文摘要與顯示方式見 [docs/axiom-log.md](docs/axiom-log.md)。
  主機驗證（2026-09-15）：native 127／127、實際 Axiom worker／encoder／API、motion／debug
  後端與 Axiom／motion／debug UI 通過；Client／Station／Station OTA build 成功，僅既有
  RadioLib USB CDC 警告。涵蓋斷線／恢復、七項同時變化、GPS 模式分級、故障、計數／時鐘
  回繞、基準重建、unknown、buffer 不足不消耗基準、9 文件批次完整／部分收件。
  快照 fixture 4,806 bytes，含變化事件 fixture 5,484 bytes，合計 206 個欄位路徑。
  Station OTA 已完成：板上 `Sep 15 2026 19:44:40`、新 boot ID 與 82,035-byte 網頁核對通過，
  artifact 上傳前後 SHA-256 一致；Token／啟用／綁定／控制設定保留。約 30 秒觀察中 Axiom
  HTTP 200、確認收件 6→36 筆、失敗／丟棄均 0，worker 最低剩餘 stack 6,100 bytes，
  control gap max 123 ms；UART waiting／hold、90°、PWM 正常。未發出動作命令，
  未直接讀取雲端事件內容，戶外 GPS／LoRa 負載與長時間性能仍待現場驗證。

- 資訊頁移除鏡頭指南針校正的長段操作說明，將 Servo 最高速度與 GPS 位置預測（α）
  移到校正區塊下方；保留校正結果與設定自動儲存行為。
  驗證／部署（2026-09-15）：motion／slider／debug／Axiom UI 與 Station OTA build 通過，
  OTA 上傳成功；板上 `Sep 15 2026 19:31:38`、82,035-byte 網頁與本地一致，
  說明移除及控制順序均已核對。綁定／控制設定保留，UART waiting／hold、90°、PWM 正常。

- Axiom 網頁移除 Dataset 與區域選項，儲存固定使用 `shore-spotter`／US East 1；
  保留 Token、開關與清除操作，清除時沿用已存目的地。同步設定說明與完整網頁測試。
  驗證／部署（2026-09-15）：Axiom／motion／slider／debug UI 通過，Station OTA build 與上傳成功；
  讀回 `0.6-dev`／`Sep 15 2026 19:29:05` 和新 boot ID，82,474-byte 網頁與本地一致，
  兩欄位已移除，下載頁面重跑 Axiom UI 通過。綁定／控制設定保留，UART waiting／hold、
  90°、PWM 正常，Axiom 仍關閉且未設定 Token。

- Station 新增可選 **Axiom 背景除錯上傳**，預設關閉；「除錯」頁可設定 dataset、
  US／EU 區域及 ingest API Token，開關／清除 Token 與設定保存使用 `GET/POST /api/axiom`。
  Token 僅從 JSON POST body 接收、NVS 保存，不經 GET／雲端 JSON／瀏覽器除錯匯出回傳。
  關閉網頁後仍由 Station 上傳；Client、LoRa v4 與 Servo 控制契約不變。
  主迴圈每秒最多複製一份固定快照與 8 個封包事件；另一核心低優先權 task 負責 NDJSON、
  CA 驗證 HTTPS、SNTP 與設定 NVS 寫入。約每 5 秒一批，固定 8 筆 RAM queue／32 KiB batch；
  busy／低記憶體／斷網時可丟棄，15 秒過期，失敗退避、尊重 Retry-After、拒收需重新儲存。
  記錄 GNSS、LoRa raw bytes、控制／UART、heap、HTTP／loop 耗時與遺失計數；不寫 flash log。
  新增實際 worker／encoder／API 主機模擬與完整網頁測試；設定儲存失敗、部分收件、429、
  Token 拒收、OTA／低 heap／斷網、傳送中關閉、重啟與 Token 清除均納入。
  實作與限制見 [docs/axiom-log.md](docs/axiom-log.md)。上述主機驗證階段尚未燒錄或用真實 Axiom 帳號驗證，
  主機隔離測試不代表實際 Wi-Fi／GPS／LoRa／Servo 性能零影響。
  驗證（2026-09-15）：native **127／127**、Axiom 後端與完整網頁模擬、既有 motion／packet／
  debug／固定功率後端和三套 UI 測試全部通過；Client／Station／Station OTA build 成功。
  雲端 schema fixture 193 欄、快照主機 RAM 1000 bytes；編譯沒有新增專案 source 警告，
  保留既有 framework／RadioLib 警告。無關的 16 份既有 tracked 修改逐份比對一致。

- Axiom Station **USB 更新（2026-09-15）完成**：USB 序號與開機角色確認 Station 尾碼 584C，
  使用 `tbeam-station` 上傳，四個寫入區段雜湊驗證通過，NVS 區段未清除。
  更新前後韌體 artifact SHA-256 一致；板上讀回 `0.6-dev`／`Sep 15 2026 17:50:22` 與新 boot ID，
  **83,165 bytes** 網頁與本地來源逐 byte 相同。`GET /api/axiom` 回報 disabled、Token 未設定、
  upload requests 0、queue 0、worker stack 0，確認預設停用時尚未建立上傳 task。
  E91C 綁定、30°/s、關閉 GPS 預測保留；連續 5 次狀態讀回維持同一 boot、UART waiting／hold、
  90°、PWM 正常且無 motion fault。Client 未重刷，未發出轉角／模式／校正命令。
  真實 Axiom ingest、啟用後長時間 heap／RF／控制時序及機械性能仍待驗證。

- Client 固定發射功率 **20 dBm**：移除 ATPC 調整與 Client 功率 NVS 讀寫；舊
  `shorespt_client/txpwr`、`atpc` 保留但不使用。開機與 radio re-init 由同一角色常數
  套用，Station ACK 仍為 17 dBm。LoRa v4 封包格式、airtime、500 ms DATA 排程與
  ACK 窗口不變；Client 開機明示固定 20 dBm／ATPC 關閉。
- GPS 追蹤取消額外連續有效 2 秒的等待：初次符合條件或恢復時即可選用 GPS 來源，
  下個控制週期更新目標；失效仍立即保持。保留兩端定位品質、資料新鮮度、指南針參考、
  磁偏角、模式隔離與 Servo 限速。GNSS UTC 時間基準重建、LoRa 序號重同步和 HTTP
  命令期限是不同檢查，本項不更動。網頁移除「GPS 穩定中」的舊等待說明。
- GPX／整趟除錯紀錄繼續由瀏覽器保存，Station 不新增軌跡或整趟 log 儲存；文件補充
  使用另一支手機保持網頁前景與螢幕亮起，並啟動除錯錄製。此批 Client／Station 更新
  分別改善固定功率與 GPS 恢復；與既有 v4 對端通訊格式相容。
  驗證（2026-09-15）：native **127／127** 通過；實際控制後端驗證 GPS 首次／恢復後
  50 ms 內更新目標、2000 ms 新鮮度邊界保持、反覆失效／恢復、限速與 PWM 故障保持。
  新增 `tools/test_radio_power.py`，以實際 radio init／recovery 驗證兩角色固定 20／17 dBm、
  舊 NVS／ATPC 覆蓋路徑移除，以及初始化／RX 重啟失敗不回報成功。封包／診斷後端與
  三套網頁測試通過。Client／Station／Station OTA build 全部成功，僅有既有 RadioLib
  USB CDC 警告。上述主機驗證階段尚未燒錄；主機測試不代表實際 RF 功率、戶外收包或機械追蹤已改善。

- 雙端燒錄（2026-09-15）完成：USB 序號與晶片 MAC 確認 Client E91C，四個 Flash
  區段雜湊驗證通過，開機明示 `TX power=20 dBm (fixed, ATPC=off)`；未清除 NVS。
  Station OTA 使用已驗證 artifact，讀回新 boot ID、`0.6-dev`／`Sep 15 2026 01:28:54`，
  77,203-byte 網頁與本地完全一致。E91C 綁定、30°/s 與關閉 GPS 預測設定保留；
  更新後 UART waiting／hold、90°，PWM 正常且無 motion fault。沒有另發模式、校正或
  轉角命令；指南針 RAM 參考隨重啟重置，使用 GPS 追蹤前需重新校正。
  完成開機後的 21.903 秒區間收到 44 個 DATA（約 2 Hz），無新增 RF／序號／ACK
  錯誤；Client 40 秒內 TX 140→220、ACK 17→27，TX error／skipped／ACK rejected
  均 0。較早 USB 監看曾觸發 Client `USB_UART_CHIP_RESET`，開機空白與後續穩定
  採樣分開判讀。當時兩端無 GPS fix，未啟動 GPS 追蹤；實際輸出功率、戶外距離與
  機械跟隨效果仍待現場驗證。

- 鏡頭指南針可隨時重新校正：取消兩端 GPS 品質、模式、命令移動、PWM／motion fault
  及其他控制 busy 狀態對校正按鈕與 API 的限制。校正只更新 RAM 方位參考，錯了可重新
  提交；保留十進位角度、HTTP 排程與命令新鮮度檢查，校正不切模式、不直接寫 PWM 或
  清除硬體故障。GPS 後續目標重算使用新參考，UART／手動目標不變。
- 校正按鈕保持可用，不依賴 GPS 狀態或額外可用性欄位；修正控制設定保存後狀態刷新
  失敗、按鈕殘留灰色／「控制更新中」的路徑。直接顯示送出中、成功或失敗原因，失敗
  可再按且不自動重送；連續提交只讓最新操作更新提示。成功回覆立即更新校正偏移。
  操作說明改為可隨時重校，鏡頭與磁針停穩時讀值較準。
  驗證（2026-09-14）：實際校正 handler／狀態 JSON 主機測試通過，涵蓋四模式、移動中、
  12 組 GPS 品質／缺漏／過期資料、PWM 與控制器故障、重校覆蓋、非法角度及過期／重送
  拒絕。完整控制 UI 涵蓋保存中提交、刷新失敗不卡灰、離線／首次上下文恢復、錯誤回覆
  與連續重校；原 GPS 保持／未校正／350° 截圖情境、滑桿 10／10、除錯後端／UI 及
  9 段文件 JSON 均通過。Client／Station／Station OTA build 全部成功，只有既有 RadioLib
  USB CDC 警告。上述主機驗證階段未燒錄，Client 無須重刷；本項取代未部署的 Station Good 門檻。

- Station 校正更新 OTA（2026-09-14）完成：USB 序號確認為 Station，更新前 UART waiting／
  hold、90°；燒入與已驗證 build 相同的 binary，讀回 `0.6-dev`／`Sep 14 2026 23:12:55`
  及新 boot ID。77,198-byte 網頁與本地完全一致，下載頁面重跑控制／滑桿／除錯 UI 測試
  全部通過；再用實機 UART／無 GPS 狀態執行下載頁面模擬，校正按鈕保持可用。
  E91C 綁定、30°/s、關閉 GPS 預測設定保留，PWM 正常、無 motion fault，重啟後維持
  UART waiting／hold、90°。本輪只燒 Station，Client 未重刷；當時 Client 未連線，未重測
  戶外定位／RF。沒有另發轉角、模式或校正 POST；實際指南針讀值與機械追蹤仍待現場驗證。

- GPS時間基準恢復：修正UTC停滯後正常輸出仍長期被判過期；UTC倒退或估計age達2秒時，
  至少3個有效新epoch、至少2秒且UTC／本機到達節奏一致才重建基準。保留重複epoch年齡、
  UART 200 ms積壓丟棄、無效定位撤銷及模式隔離；不將舊座標／速度刷新成新資料。
- 新增可選LoRa v4 type 6 GPS診斷，每組3個17-byte頁，約30秒一組；既有DATA／ACK／
  TEL／DIAG格式不變。Station收齊同Client／seq快照才發布，重複／缺頁不刷新原時間。
  新增GPS資料流／完整來源年齡／原始fix／時間恢復與拒收計數；網頁區分未更新、未定位、
  過期與恢復確認中。epoch間隔改標「最後記錄」，避免誤認持續有1 Hz定位。
- 主機重現與現場根因分開判讀。Client／Station需更新才有完整診斷；
  新Station可接舊Client，舊Station拒收新增type 6但仍接受既有DATA。
- GPS修正主機驗證（2026-09-14）：完整native 124／124通過，另補2個確認時間長度／
  checksum連續性及慢句子案例通過（共126個）；實際NMEA→DATA→Station恢復路徑、
  凍結三頁／缺頁／重複／回繞／錯Client與95秒排程模擬通過，190個DATA保持2 Hz、
  無略過且GPS診斷避開ACK窗口。實際HTTP除錯JSON、控制後端及三套UI測試通過；
  27個除錯fixture回覆每次≤8事件，最大4,349 bytes。Client／Station／Station OTA最終
  build全部成功，專案source無警告，相依套件警告仍在；上述主機驗證階段未重刷或發出動作命令。
  現場Client的實際根因、戶外GPS恢復與機械行為仍待實機觀察確認。

- Station OTA（2026-09-14）完成，讀回`0.6-dev`／`Sep 14 2026 00:50:16`及新的boot ID；
  76,022-byte網頁與本地一致，下載頁面重跑控制／滑桿／除錯UI測試均通過。
  新增`gps.stream`、type 6三頁設定與`client_gnss`正常讀回；本機GPS有NMEA但尚無epoch，
  初次快照拒收24句皆歸於時間空白／非法，checksum錯誤0。原E91C綁定、30°/s與現場已關閉
  的GPS預測設定保留，UART waiting／hold、90°命令值、PWM正常。更新後LoRa DATA持續接收，
  無新增RF／格式／綁定／序號缺口／ACK錯誤；當時Client尚未重刷，完整GPS診斷尚未收到。
  只更新Station，沒有另發轉角／追蹤命令；Client戶外定位根因與機械行為仍未驗證。

- Client USB（2026-09-14）完成，以USB序號與晶片MAC確認E91C，四個Flash區段均通過
  雜湊驗證；應用程式SHA256與已驗證build相同。開機回報0.6-dev／Client E91C，
  PMU、BME280、LoRa正常；10秒內成功TX增加20，TX error／skipped／backlog均0，
  GNSS最後觀察到的epoch間隔1000 ms。Station HTTP於此階段暫時無法連線、Client ACK=0，
  尚未讀回三頁GPS診斷或確認有效定位；不把epoch間隔當成已恢復fix，也不推定Station離線原因。
  初次nobuild上傳因工具未帶完整位址參數而在寫入前失敗，改標準上傳後成功；沒有修改韌體。

- 雙端更新後戶外觀察（2026-09-14）：使用者將Client拿到戶外後，Station收到完整GPS三頁
  診斷，Client由未定位轉為fresh_fix；接續30秒、11組HTTP快照皆有定位，新增61個DATA
  無invalid-fix、序號缺口或RF／ACK錯誤。低頻GPS快照衛星9→12顆，DATA快照HDOP
  1.0–1.2、來源age 450–950 ms，新增一組完整三頁診斷。
  NMEA拒收／checksum／倒退／重複均0；時間基準恢復次數0，**不能以此次取得定位證明
  原故障必由時間基準造成，或已在實機觸發恢復邏輯**。Client積壓1、略過1是觀察前
  已有計數，本段未增加。Station仍在室內無定位，維持UART hold／90°，未啟動GPS追蹤。
  原始定位與採樣保留本機暫存，不加入Git；未驗證定位精度或機械追蹤。

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
  操作路徑在外層等待時的期限／boot／epoch 測試。Client、Station、Station OTA build
  全部成功，專案 source 無警告；依賴套件警告仍在。Station OTA 完成，74,656-byte
  網頁與本地一致，下載頁面重跑三套 UI 測試通過；schema 2 與分段計時讀回正常，
  E91C 綁定、30°/s、α 開啟與 UART hold 保留，Client 無須重刷。
- 更新後以實際網頁 JavaScript 接真實 Station 唯讀重測 5 分鐘：916 個完整 HTTP 回覆，
  單次最多 8 事件、最多 1 個 fetch＋body；debug 平均 2,344.6 bytes，較更新前實讀
  縮小約 83.8%。新增 DATA 600、TEL／DIAG 各 10、Station ACK 75，無新增 RF／序號
  缺口；648 個含起始歷史的事件 ID 連續且不重複。GPS backlog 0；HTTP 最大 112.84 ms
  來自初次完整首頁，持續輪詢沒有新增 ≥50 ms 板上請求。仍觀察到 3 次主機請求逾時，
  連帶 2 次除錯排隊逾時及 3 次取樣間隔缺口，後續自動恢復；不能宣稱端到端等待已消除。
  schema 2 JSON 匯出成功，完整結果與證據限制見 [實機重測](docs/records-index.md#link-test-2026-09-13)。
- 資訊頁新增「GPS 位置預測（α）」二態開關：預設開啟 α=1，保留等速外推；關閉 α=0
  使用最後收到座標。只影響 GPS 目標估計，保留共用速限、模式、校正與來源資格門檻。
- 新增 GET／POST `/api/track/prediction` 與 servo.prediction_enabled／prediction_alpha；
  POST 檢查既有 epoch／seq／stamp，NVS `gpspredict` 寫入回讀後套用、相同值不重寫。
  UI 自動保存，失敗復原並防止延遲回覆覆蓋較新設定；共用控制上下文拒絕已退役 boot 的回覆。
- 韌體原始碼標示 `0.6-dev`；LoRa 協定升至 **v4**，Client／Station 需一起更新，v3 拒收。
  DATA 由 32 縮為 **17 bytes**，ACK 11、TEL 11、DIAG 17；改用明確 little-endian codec
  與嚴格長度／欄位驗證。移除空 MAC、加速度、group、目的 ID 與 payloadLen，保留 PHY CRC。
- DATA 使用固定原點 24°N／121°E 的 signed24 E6 座標（約 0.1 m 量化），速度
  0.1 m/s／最高 25.4 m/s，保留方向、衛星分級、HDOP、來源 age、fix 與向量有效旗標。
  未知／超界速度停用外推，不偽造數值；精確衛星數移至低頻 TEL。
- RF DATA 每 500 ms 排程、每 8 個 DATA 序號回 ACK；DATA／TEL／DIAG 序號獨立。
  Client TX、Station ACK 非阻塞；過期工作跳過、不積欠，低頻包只在非 ACK 空檔發送。
  嚴格核對 ACK ID、序號與回覆期限，ATPC 避開收 ACK 窗口，RX 失敗可恢復。
- 新增同 epoch RMC／GGA 快照；重複 epoch 不刷新 age，非法定位撤銷快照，UART
  超過 200 ms 未服務則丟棄積壓。追蹤 age 合計來源＋RF airtime＋接收後時間，另保留
  200 ms 不確定度，未滿 2 秒才可用；向量與位置分開判定，α 使用有效向量與合計 age。
  RF 2 Hz 不等於新定位 2 Hz：維持 GPS UART 9600、不強制未知模組命令，實際 GNSS
  更新間隔由診斷回報；GNSS 模組內部延遲未量測。
- 新 Station 預設未綁定，既有 NVS 綁定與控制設定保留；ID 避開保留值。TEL／DIAG／
  拒收包不覆蓋接受 DATA 的位置與訊號統計，事件保留當包實際訊號；DATA 缺口不再計入 TEL。
- 網頁新增唯讀「除錯」頁：封包／來源年齡、Station GNSS、Client 低頻 DIAG、RX／ACK／
  排程錯誤與 raw hex。可加備註、錄製、匯出 JSON 給 AI；有限容量，標示漏採、覆寫、
  重啟與過期資料。新增 `/api/debug`、`X-Log-Boot`、no-store，修正文字游標 uint32 回繞。
  Boot ID 在 PWM 初始化失敗時仍可識別重啟；匯出設定不含 Wi-Fi 帳密。
- `/api/track` 移除加速度，新增 satellite_class、velocity_valid、來源／接收／合計 age；
  未知值回 null。新增 [現場除錯操作](docs/debug-export.md)，協定與 API 以
  [interface.md](docs/interface.md) 為準；原 [提案審核](docs/records-index.md#packet-proposal-review-2026-09-13)
  保留為實作前歷史，後續以 1-byte 速度達成 17 bytes。
- 驗證（2026-09-13）：92／92 個 native 測試通過；實際控制／NVS／GPS 預測、NMEA→
  v4 封包、實際 RX 分流／錯誤 ID／重複包不污染追蹤狀態，以及診斷 JSON／環形紀錄／
  游標回繞與 PWM 失敗開機識別主機測試通過。
  10／10 個滑桿案例、完整控制 UI 與除錯錄製／匯出模擬通過，9 段文件 JSON 範例可解析。
  Client、Station、Station OTA 最終 build 全部成功，專案 source 無警告；相依套件仍有
  RadioLib USB CDC 等警告；戶外收包率、實際 GNSS 2 Hz 與機械追蹤效果尚未驗收。
- Station OTA（2026-09-13）成功，重開機 HTTP 讀回 `0.6-dev`／LoRa v4、17／11／11／17
  bytes、500 ms／ACK 每 8 DATA。既有 E91C 綁定與 30°/s 速限保留，α 預設開啟，
  UART waiting／hold、90° 命令位置、PWM 正常且無 motion fault。`/api/debug` 與
  log boot／cursor headers 正常，下載的 68,669-byte 網頁與本地完全一致；以板上頁面
  重跑除錯錄製／匯出主機模擬通過。此階段只更新 Station，Client 隨後另行 USB 更新；
  未另發轉角或啟動追蹤命令，戶外定位／機械追蹤仍待驗。
- Client USB 燒錄（2026-09-13）成功，以 USB／晶片 MAC 確認目標為 E91C，四個 Flash
  區段均通過雜湊驗證。開機紀錄回報 `0.6-dev`、v4 DATA17／ACK11／TEL11、500 ms
  發送與每 8 DATA 回 ACK；PMU／BME280／LoRa 初始化正常。兩段 10 秒觀察皆增加 20
  個成功 TX（約 2 Hz），TX error／skipped／backlog_drops 均 0；GNSS epoch 間隔
  1000 ms，沒有把 RF 2 Hz 當成新定位 2 Hz。當時 Station HTTP 無法連線，Client ACK=0，
  因此尚未完成雙端 v4 收包／ACK 驗證；不推定 Station 離線原因。
- 實機通訊測試（2026-09-13）：Station 上線後唯讀觀察 60 秒，新增 DATA 120、TEL 2、
  DIAG 2、Station ACK 發送 15；DATA 間隔 499–501 ms，無新增序號缺口或無線／解析錯誤。
  188 個去重原始事件由實際 codec 重解成功，1,281 次 API 欄位核對一致。兩端仍無 fix，
  Client GNSS epoch 為 1 Hz；Client ACK 接收尚無直接證據。另記錄一次 HTTP 211.34 ms
  停頓、控制間隔 212 ms 與 Station GPS backlog 丟棄 1 次，未修改韌體。詳見
  [實機通訊測試](docs/records-index.md#link-test-2026-09-13)；全程保持 UART hold，未執行機械追蹤。

## 0.5 — 2026-09-09

- 所有模式改為單一實際時間限速器，預設 30°/s；移除加速度、減速度、jerk、deadband
  與 Ruckig／等加速度規劃器。只處理最新目標，保留固定角度範圍與長停頓步幅保護。
- 唯一速限移至資訊頁，1–90°/s、修改自動保存，移動中與任何模式都可改；NVS 單一
  `servospd` 值、寫入回讀後生效，重複相同值不重寫。舊 motioncfg 忽略，首次採 30。
- 開機仍歸中 90°，完成初始化後預設 UART。GPS 只有 Station、Client 都為新鮮 Good／OK
  才可選；mode／start／resume 都在後端檢查。Bad／Miss／過期拒絕並保留原模式。
- GPS 追蹤門檻同步接受 Good／OK；指南針校正、有效磁偏角與 2 秒穩定期仍必要。
- 移除所有 Servo 進階／頻率／OLED 調試介面與獨立 log 網頁；OLED 固定分段刷新。
  settings API 改為單一 speed 自動保存，移除 diagnostics API、control_elapsed 與舊運動欄位。
- 49 個 native、實際 HTTP／PWM／GPS gate／NVS 主機整合、10 個滑桿與完整 UI 模擬通過。
  三環境 build 與 Station OTA 成功，板上版本 `0.5`、預設 UART、速限 30、PWM 正常。
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
  模擬通過；三環境 build 成功，Station OTA 成功、板上回報 `0.4`，下載 UI 與本地一致。
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
  及完整設定頁／診斷開關模擬通過。三環境 build 與 **Station OTA 上傳成功**，板上
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
- **Station OTA 上傳成功**，API 已讀回版本 `0.2`、speed=90、acceleration=30、jerk=120、
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
- 新增 Client／Station 開機 OLED、序列紀錄與 Station API／網頁版本顯示。

### 文件與相容性

- 建立版本唯一來源、版本紀錄與 API／OLED／網頁版本顯示說明。
- 修正硬體接線文件：Servo 實際直接使用 2S、2000 mAh、35C 電池，不經 UBEC。
- HTTP 動作需要世代、序號與板上時間；手動角度 API 不再自動切換模式。
  從先前韌體升級後，瀏覽器需重新整理；舊 UART `SET` 保持相容。

本次標版不調整 Servo 控制參數、LoRa wire 格式或 NVS 格式。

### 驗證與部署（2026-09-09）

- Client、Station、Station OTA 三個環境編譯通過，產物已確認包含 `0.1` 版本資訊。
- 11 個滑桿案例、完整設定頁模擬、版本顯示與舊韌體回應相容檢查通過；
  8 個 API JSON 範例與本地文件連結檢查通過。
- 共用控制後端先前已通過 52 個 native 測試；本次只加版本識別，未重跑控制測試。
- **0.1 未單獨上傳 Client／Station**：當時 Station 無法連線、USB 未接；Station 後續
  直接更新為 0.2。更早共用後端的 OTA 成功紀錄屬於未標版韌體。

### 已知限制

- Servo 移動時抖動、固定時偶爾抖一下，原因尚未確認；目前沒有動態供電或機構振動量測。
  此版本不宣稱已修復抖動。
- 開機第一次歸中 90° 沒有起始位置回授，無法保證物理速度／平滑度。
- 舊 UART `SET` 與 LoRa 封包缺少完整來源時間資訊，無法保證排除所有傳送前積壓資料。

實機部署與尚未驗收項目見 [健檢紀錄](docs/records-index.md#health-check-2026-09-08)。
