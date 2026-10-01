# 版本紀錄

專案從 **0.1** 開始標版。韌體版本的唯一來源是
[`include/firmware_version.h`](include/firmware_version.h)；Client／Station 共用。
一般後續發版依序 0.2、0.3；若只修補既有版本，可用 0.1.1。
版本號不取代 LoRa `PROTO_VERSION`、NVS 格式版本或實機部署驗證。
每次功能、修正、介面或設定行為異動都更新本檔；尚未定版先記在「未發行」，
正式標版時移入對應版本，保留日期、相容性與驗證紀錄。

## 未發行

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
  [追蹤更新紀錄](docs/tracking-update-2026-09-19.md)。以下 v4 未部署階段為歷史，不代表 v5 現況。

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
  [追蹤更新紀錄](docs/tracking-update-2026-09-19.md)。

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
  schema 2 JSON 匯出成功，完整結果與證據限制見 [實機重測](docs/link-test-2026-09-13.md)。
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
  [interface.md](docs/interface.md) 為準；原 [提案審核](docs/packet-proposal-review-2026-09-13.md)
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
  [實機通訊測試](docs/link-test-2026-09-13.md)；全程保持 UART hold，未執行機械追蹤。

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

實機部署與尚未驗收項目見 [健檢紀錄](docs/health-check-2026-09-08.md)。
