# 專案健檢：2026-09-08（更新至 2026-09-09）

## 2026-09-09 版本 0.5：共用單一速限、UART 開機與 GPS 選取門檻

- 實際時間限速器，預設 30°/s，資訊頁 1–90°/s 自動保存；移除其他參數與調試 UI。
- `servospd` NVS 單一速度值，寫入／回讀後套用；任何模式及移動中可更新，失敗保留原值。
  忽略舊 motioncfg，不帶入先前 90°/s 或曲線設定；後續開機沿用最後保存速度。
- 完成初始化後預設 UART；GPS 按鈕／mode／start／resume 均要求兩端新鮮 Good／OK。
  Bad／Miss／品質缺漏／過期拒絕；進入後仍需校正、磁偏角與穩定期，失效保持。
- 49 個 native 測試通過；10 個滑桿案例、完整 UI 自動保存／失敗／GPS 按鈕狀態競態，
  實際 HTTP／PWM／GPS gate／NVS 主機整合通過。舊曲線測試隨功能移除。
- 三環境最終 build 成功；Server flash 1,272,344 B（38.1%）、RAM 60,580 B（18.5%）。
  Server OTA 成功、版本 `0.5`，開機歸中後預設 UART，等待新指令，PWM 正常、無故障。
- 板上 HTML 47,155 bytes 與本地完全相同；以下載頁面重跑 10 個滑桿與完整 UI 模擬通過。
  8 個更新後的 API JSON 範例與文件連結通過；diagnostics API 回 404，調試控制與 log 頁移除。
- 實機 GPS 不可用，mode=gps／track/start 均回 409 並保留 UART；Good／OK 與資料過期、
  resume 入口由主機實際 handler 測試涵蓋，室內未驗收戶外定位或實體追蹤。
- 實機將速度 29.5 自動寫入，第二次相同韌體 OTA 重啟後仍讀到 29.5，證明不是僅存 RAM；
  最後已恢復並保存 30°/s。最終 UART、gps_available=false、PWM 正常、motion_fault=false，
  E91C 綁定保留。Client 未重刷；驗證腳本未另發角度命令，機械振動效果未量測。
- 快照（含 0.4 產物）、完整 build／OTA log、NVS 跨重啟取樣與部署腳本：
  `/tmp/shore-control-05-ggf_c8i2/`。

## 2026-09-09 版本 0.4：不限頻實際時間軌跡

- 取消 Servo 50 ms 節流；微秒實際時間推進、GPS 目標仍 20 Hz、長停頓最多推進 50 ms。
- 修正反向煞車交接的剩餘時間丟失；PWM 保留小數微秒、略過相同 duty。
- `timing.control_elapsed` 取代 `control_20hz`，提供實際間隔及 PWM 寫入／略過統計。
- 新測試涵蓋不規則／同時刻／小於 1 ms 更新、微秒回繞、不同取樣率的相同軌跡、
  速度／加減速度／jerk 限制、長停頓及輸出量化精度；擷取實際 PWM／HTTP／後端驗證。
- 60 個 native 測試、10 個滑桿案例、完整 UI 模擬、實際 handler／PWM 主機整合通過，
  8 個 API JSON 範例及文件相對連結通過。三環境 build 成功；Server flash 1,457,536 B
  （43.6%）、RAM 64,772 B（19.8%）。新增入口 `python3 tools/test_motion_backend.py`。
- Server OTA 成功，板上回報 `0.4`；來源／產物雜湊一致，下載頁面 51,678 bytes 與本地
  相同，以下載頁面重跑滑桿／參數診斷模擬通過。Client 綁定 E91C 保留，Client 未重刷。
- 開機依既有流程回 90°。升級後還原 RAM 設定：速度 30、加速度 30、減速度 30，三項
  開啟；jerk 120、deadband 1 保留數值但關閉，saved=false；OLED 暫停。未寫 NVS。
  還原後另收到調參，最後讀取速度為 90、其餘設定相同；未覆蓋新調整。
- 約一秒間隔讀取 20 次 status，同一 boot ID、PWM 正常、無規劃故障。取樣包含新的
  手動目標，最後命令位置／目標為內部 58°（UI 122°）、moving=false；驗證腳本未另發
  角度命令。不能將軟體命令完成當作實體抵達或減振量測。

| 0.4 不限頻統計 | 取樣結束累計值 |
|---|---:|
| 控制服務間隔樣本 | 185,084 |
| 平均／最小／最大間隔 | 0.118／0.006／73.100 ms |
| 間隔 >5 ms／>50 ms | 41／2 |
| 不同 duty 寫入／略過重複 | 1,765／15,209 |
| OLED 刷新／幀數 | 暫停／0 |
| HTTP 最大耗時 | 73.04 ms |

平均間隔包含同一 loop 內的多次服務，不能換算成有效 PWM 或 Servo 反應頻率。
最長控制間隔與 HTTP 最大耗時接近；同步 HTTP 仍是可能的停頓來源，取消 20 Hz
不會消除主迴圈阻塞。保留長停頓最多推進 50 ms，超出時間不累積。軸心鬆動仍存在，
實體平滑度待使用者比較。快照、測試與部署紀錄：`/tmp/shore-control-04-hz084air/`。

## 2026-09-09 版本 0.3：共用 20 Hz、參數開關與 OLED 診斷

- 五項獨立參數／開關、UI 反轉與放開送出、固定 0–180°、v2 NVS 遷移。
- 20 Hz 共用排程，來源失效即刻撤銷；OLED 八段傳輸與可清零診斷。
- 57 個 native 測試（全部開關組合、途中反向、獨立加減速、舊格式與計時 wrap），
  10 個滑桿案例、完整設定頁模擬通過。擷取實際 HTTP handler／serviceControl 的主機
  整合模擬確認模式隔離、UART 過期、GPS 恢復／失效、NVS 失敗保持、PWM 故障與 50 ms 更新。
- 三環境 build 通過；Server 韌體 1,456,504 B（43.6%），RAM 64,684 B（19.7%）。
  **Server OTA 成功**，產物雜湊未變；板上回報 `0.3`、manual 90°、PWM 正常、無故障，
  五項預設 90／30／30／120／1 與開關全開，指南針清空，E91C 綁定保留。
- 板上頁面 51,594 bytes 與本地完全相同，以下載頁面重跑 10 個滑桿案例及完整設定頁
  模擬通過。Client 未重刷，無實體轉角命令；尚未驗收機械減振或戶外追蹤。
- OLED 三段對照：每段 20 次、間隔約一秒讀取 status，含 HTTP 耗時，每段約 22–23 秒；
  全程保持 manual、angle／target 90°、moving=false，boot ID 一致。最後恢復 OLED 刷新。

| OLED 刷新 | 控制樣本 | 平均間隔 ms | 最大間隔 ms | 延遲 >5 ms | 漏過週期 | OLED 單段最大 ms |
|---|---:|---:|---:|---:|---:|---:|
| 開啟 | 449 | 50.10 | 55 | 0 | 0 | 4.66 |
| 暫停 | 453 | 50.02 | 55 | 0 | 0 | 0 |
| 再開啟 | 467 | 50.08 | 56 | 1 | 0 | 5.24 |

分段傳輸縮短單次阻塞：更新前 0.2 的整幀 OLED 累計最大耗時為 38.62 ms，新版首次
讀取累計單段最大 5.45 ms；兩者測量單位不同，不能拿控制服務間隔直接當作同一更新
頻率比較。三段對照僅驗證固定角度下的時序，不能證明移動中的規劃耗時或機械抖動已消除。

- 快照與完整 log：`/tmp/shore-control-03-8ei_8av3/`。

## 2026-09-09 版本 0.2：90°/s 速限

- GPS／UART／手動共用速度預設／可設定上限提高至 90°/s，網頁、HTTP 驗證訊息
  與文件同步。加減速度 30°/s²、jerk 120°/s³、死區 1° 維持原值。
- NVS 格式相容，原有較低速度不會被韌體強制覆寫。上傳前板上為手動保持 98°，
  速度 30°/s、saved=false，Client E91C 綁定保留；預期更新後採用 90°/s 預設。
- 53 個 native 測試、11 個滑桿案例與完整設定頁模擬通過；新增雙向接近 90°/s
  仍有界、及舊速度設定可讀回的測試。8 個 API JSON 範例與文件連結檢查通過。
- Client／Server／Server OTA 三環境 build 通過。Server OTA 韌體 1,448,152 B，
  靜態 RAM 64,524 B；**OTA 已成功上傳 Server**，上傳產物雜湊未變。
- 板上 API 已回報版本 `0.2`、speed=90、acceleration=30、jerk=120、deadband=1、
  minimum=0、maximum=180、saved=false（使用韌體預設，不另寫 NVS）。開機 manual
  90°、PWM 正常、無 motion fault，指南針校正清空，單一 E91C 綁定保留。
- 板上頁面 49,538 bytes 與本地完全一致，以下載頁面重跑 11 個滑桿案例及設定頁
  模擬通過。Client 未重刷，本次未發送實體轉角指令或量測機械速度／抖動。
- 來源快照、完整 log、來源／產物雜湊與板上回應：`/tmp/shore-speed-90-i4y8ws31/`。

## 2026-09-09 版本 0.1

- 新增版本唯一來源 `include/firmware_version.h` 與 `CHANGELOG.md`，以現有功能作為
  0.1 基準。Client／Server 開機 OLED、序列紀錄共用版本；Server status API 的
  `health.firmware_version` 與資訊頁顯示實際運行版本。
- 本次不修改控制參數、LoRa wire 或 NVS 格式。舊 Client 不會自動更新或回傳韌體版本。
- 使用者確認 Servo 直接 2S 2000 mAh 35C 電池，固定時偶爾抖動；硬體文件已修正
  原 UBEC 接法。尚未量測動態供電電壓／電流或機構振動，不能斷定抖動原因。
- 11 個滑桿案例、完整設定頁模擬及實際 status 渲染函式的版本／舊韌體顯示檢查通過，
  8 個 API JSON 範例與本地文件連結有效。本批未更動控制邏輯，因此未重跑 native 控制測試。
- `tbeam-client`／`tbeam-server`／`tbeam-server-ota` 全部編譯通過，三個產物均確認
  包含 `v0.1`；Server 產物包含 API 版本欄位。Server／OTA 靜態 RAM 64,524 B，
  韌體分別 1,448,132／1,448,148 B。快照、log 與來源／產物雜湊保存在
  `/tmp/shore-version-01-y6qso363/`。
- Server 兩次連線檢查回 `No route to host`，USB 未接，**Client 與 Server 均尚未
  上傳本次 0.1 版本**；下方共用後端的 OTA 成功紀錄是前一個未標版韌體。

## 最新狀態：2026-09-09 共用後端、S 曲線與 1° 死區

- GPS／UART／手動共用 `servo_motion::Controller`，唯一運行中的 PWM 路徑在 serviceControl。
  移除原 UART 專屬運動控制器／簡單限步器，UART 現在只解析並保存最新輸入。
- 預設最高 30°/s、加減速度 30°/s²、jerk 120°/s³、deadband 1°，參數在資訊頁共用設定。
  保持時相差不超過 ±1° 不啟動；移動時以已接受目標為基準抑制小幅變化。
  使用固定 MIT Ruckig Community 0.15.3、本地計算；先檢查軌跡位置極值再輸出。
- 「套用試調」只改 RAM，「儲存已套用設定」才寫入有版本／校驗值的單一 NVS blob；
  指南針仍只存 RAM，Client 綁定與 Client ATPC 不改。
- HTTP 動作使用世代／序號／板上時間，2 秒期限；模式切換使舊世代失效。
  手動角度不再自動切回 manual。新版 API 需要上下文，更新後舊網頁必須重整。
- UART 舊 SET 相容；新增 SYNC／SET2，支援 session／seq／250 ms 來源期限。
  同一 UART session 接受 SET2 後拒絕裸 SET。舊 SET 仍只知道板上解析時間。
- LoRa wire 不改；新增 DATA seq 去重／順序檢查，斷線後兩筆前進候選才重同步，
  拒絕的資料不延長定位有效期。Client 不需要因本批更新重刷；舊 wire 仍缺來源量測時間。
- 52 個 native 測試、11 個網頁案例與共用設定完整頁面模擬通過；包含恰好 1° 的
  millidegree／浮點邊界。擷取實際 HTTP handler、
  serviceControl、模式切換做主機整合模擬，deadband／S 曲線／舊世代拒絕／模式隔離／
  UART 過期／GPS 恢復與失效／設定保存失敗／PWM 故障保持皆通過。
- `tbeam-client`／`tbeam-server`／`tbeam-server-ota` 最終 build 全部通過，來源與產物
  SHA-256 已記錄。Server RAM 64,524 B（19.7%）、韌體 1,447,888 B（43.3%）。
- **Server OTA 已成功**（`upload-ota.log` 回 `Success`，上傳產物雜湊未變）。板上
  首頁 49,361 bytes 與本地新版逐 byte 相同，以下載頁面重跑 11 個滑桿案例與完整
  設定頁模擬皆通過。status／track／settings API 新欄位已核對。
- 板上回報 manual 90°、PWM 正常、無 motion fault、指南針校正清空；共用設定為
  30／30／120／1／0／180，尚未寫入 NVS，Client E91C 單一綁定保留。
  uptime 14–30 秒間 boot ID 一致，控制最大間隔 65 ms，≥250 ms 次數 0。
  `timing.motion.max_ms=0.09` 僅是靜止觀察值，不代表移動中重規劃的最壞耗時。
  本輪未收到 Client DATA／TELEMETRY，也未發送實體轉角或 UART 命令；不宣稱
  LoRa 通聯、機械 S 曲線或減振效果已驗收。Client 本次不需重刷。
- 開機首次 90°、逾時／故障立即保持不保證 jerk 連續。機械速度與振動改善仍待實測。
  本批不處理既有 HTTP 網路偶發延遲、分鐘 log 百分比、GPX 匯出立即清除與讀取請求期限。
- 暫存快照、完整 log、主機整合模擬與產物雜湊：`/tmp/shore-unified-motion-lchb3m10/`。
  完整契約見 [motion-control.md](motion-control.md)。

## 2026-09-09 統一 Servo 限速（前一版）

- GPS／UART／手動滑桿與 HTTP 運行中命令角度上限統一為 **30°/s**，共用
  `servo_slew.h`，速率設定只在 `servo_profile.h` 定義。停頓後最多採計 50 ms，
  模式切換／保持會清除累積時間，避免恢復時跳大角度。
- 手動 HTTP 改為更新目標後立即回覆 `angle`／`target`，由主迴圈持續漸進輸出；
  新指令取代目標，不排隊。網頁保留滑桿目標並顯示移動進度，動作未完成禁止指南針校正。
- 保留開機首次輸出 90°。普通 PWM Servo 沒有位置回饋，因此無法保證未知位置到 90°
  的第一次歸中動作也符合物理速度上限；API 命令角度不是實測角度。
- 50 個 native 測試、9 個網頁案例與完整頁面模擬通過；涵蓋一秒 30°、反向／目標替換、
  停頓後限步、模式重入、millis wrap、UART watchdog 與手動移動期間的校正限制。
- 另擷取實際 `serviceControl`／手動切換／UART 輸出與 HTTP handler 做主機模擬，6 組整合情境通過：
  手動目標替換與保持、GPS 恢復／失效、UART watchdog／模式切換、停頓限步、非法目標／
  PWM 故障，以及 HTTP 非同步目標／校正阻擋／400／503 回應。時鐘與硬體寫入為替身，
  不代表實體 Servo 已量測驗收。
- `tbeam-client`／`tbeam-server`／`tbeam-server-ota` build 全部通過，最終來源再次核對完成。
  Server 靜態 RAM 60,420 B（18.4%），韌體空間 1,266,556 B（37.9%）；Client 邏輯未改。
- **OTA 已成功上傳。** USB 未接，透過 Wi-Fi 更新；首輪已接受邀請，但等待回連 TCP 3233
  逾時，未傳送韌體，並恢復手動保持 68°。使用者開啟 3233 後重試成功，espota 回 `OK`／
  `Success`，韌體產物 SHA-256 與更新前記錄一致。成功 log：`upload-ota-retry.log`。
- 板上首頁與本地新版逐 byte 相同；以下載頁面重跑 9 個網頁案例全部通過。重開後 API
  回報 manual、angle／target 90°、PWM 正常、校正清空，E91C 綁定保留。
  uptime 21–28 秒間持續增加；控制服務最大間隔 69 ms，≥250 ms 次數 0。
  本次沒有收到 Client 封包，也未發送實體轉角測試指令，不能據此宣稱 LoRa／機械速度已驗收。
  下方 9/8 上傳紀錄屬於舊的 120°/s 追蹤／手動直接輸出版本。
- 本次 log 與變更前快照：`/tmp/shore-servo-30-k6e9f37g/`。
  實機速度、戶外 GPS／UART 控制仍待驗收；先前列出的 HTTP 延遲、log 百分比等問題未修改。

## 2026-09-08 UART 更名與文件同步結果

- 操作名稱統一為 **UART**，模式為 `manual / gps / uart / paused`；舊 HTTP
  `mode=jetson` 保留為輸入別名，回應統一使用 `uart`。操作說明移至 [uart-servo.md](uart-servo.md)。
- 保留單一 GPS client、開機手動 90°、指南針 RAM 校正／磁偏角、GPS／UART 互斥、
  非阻塞 ACK，以及第一筆立即送出的手動滑桿排程。
- 本次名稱調整的 45 個 native 測試、7 個滑桿案例與完整網頁模擬通過；所有本地文件
  連結／章節與 7 個 JSON 範例已檢查，status／track 範例欄位與板上 API 核對一致。
- 本次 `tbeam-client`、`tbeam-server`、`tbeam-server-ota` build 全部通過；
  專案 src／include 無編譯警告，SDK／第三方套件仍有既有警告。
  依 USB 晶片尾碼 584C 確認 Server 後上傳成功，各區塊雜湊驗證通過。
- 板上首頁與本地 UART 版逐 byte 相同，下載頁面重跑 7 個滑桿案例全部通過。
  開機 API 回報 manual、angle／target 90°、PWM 正常；LoRa 初始化成功、Client E91C
  綁定保留。本次短測尚未收到 Client 封包，因此未重新確認 LoRa 通聯，不能以先前
  成功紀錄代替本次結果。
  載入完整首頁後，控制服務最大間隔 81 ms、HTTP 最大 80.84 ms，≥250 ms 間隔 0 次。
- 仍待處理：HTTP 端到端偶發延遲、分鐘 log 百分比，以及下方列出的 payload／順序驗證、
  網頁請求期限、GPX 匯出清除與封包統計。尚未驗收戶外 GPS 實際追蹤、UART 實體 SET、
  Client 端 ACK 接收紀錄、斷線恢復、慢 HTTP／I2C 故障注入與機械指向。

文件已同步修正軌跡保存位置／期限、ACK 的 seq 規則、完整狀態回應、RSSI/SNR 來源、
校正版本與命令角度含義。名稱調整不改 LoRa 封包或 Client 運作邏輯，Client 保留本日
先前已成功上傳的版本。

本次 Server 韌體空間 1,265,784 B（37.9%）、靜態 RAM 60,420 B（18.4%）。
驗證 log、上傳產物雜湊、板上頁面與只讀 API 樣本位於 `/tmp/shore-uart-rename-4hsw_uh9/`。
瀏覽器需重新整理才會顯示 UART 名稱。本次未送實體 UART SET 或手動角度測試指令。
後續開啟 USB 序列觀察時讀到新的開機紀錄，並遇到一次 HTTP 拒絕連線；再讀 API
已恢復正常（uptime 24 秒、reset_reason 11）。這次包含上傳／序列埠操作，未作長時間
不干預的穩定性驗收；不將該次重啟歸因為韌體崩潰。

## 歷次修改與驗證紀錄

以下保留各輪測試當時的版本與證據；歷史紀錄中的「Jetson 模式」即目前的 UART 模式。
先前某輪的「尚未驗證」只描述該輪，最新狀態以上方及後續測試結果為準。

本批依最新需求改為 GPS／Jetson 獨立模式，移除 ARM、STOP、HTTP track/stop，
只用鏡頭指南針校正，並處理 I2C 逾時、服務間隔診斷、關機撤銷控制與週期計時。
修改階段依要求未 build；後續收到「build and upload」授權，已完成 native 測試、
三個韌體環境 build、USB Server 燒錄與開機／HTTP 檢查；後續亦依授權完成 Client
USB 燒錄與開機／LoRa 發送檢查。未 commit／push。
工作目錄仍包含前批未提交修改；原有 .vscode 與 Wi-Fi 帳密未更動。

已執行且通過的檢查：

- 兩個網頁 JavaScript 語法、DOM 引用與重複 ID 檢查。
- Node 模擬手動／GPS／Jetson／暫停與訊號等待狀態，確認 Jetson 不依賴 GPS／校正。
- 手動請求與模式切換順序、三個模式按鈕的 URL、指南針合法／非法輸入。
- 不含 mag 欄位的 track 回應可供監控頁更新與雷達繪製程式使用（模擬 Canvas）。
- 舊控制／磁力計 API 與校正 NVS 讀寫已移除的靜態檢查。
- PlatformIO INI、本地 include、7 個文件 JSON 範例、Python 工具語法檢查。
- C++ 大括號／括號與條件編譯結構的字面檢查；這不是 C++ 編譯器檢查。
- `git diff --check`。

45 個 native C++ 案例已編譯並全部通過，包含既有數學／提醒 18 個、ACK／單一 client／
磁偏角 11 個、SET 控制／分行器 10 個、模式隔離／逾時／計時／服務間隔統計 6 個。
這些測試與以下開機檢查仍不能證明所有實機功能全面沒有回歸。

### Build 與上傳結果

| 環境 | Build | 靜態 RAM | 韌體空間 |
|---|---|---|---|
| tbeam-client | 通過 | 36,700 B（11.2%） | 630,407 B（18.9%） |
| tbeam-server | 通過 | 60,420 B（18.4%） | 1,265,364 B（37.9%） |
| tbeam-server-ota | 通過 | 60,420 B（18.4%） | 1,265,364 B（37.9%） |

使用 espressif32 55.3.39／Arduino-ESP32 3.3.9。專案 src／include 無編譯警告；
SDK／第三方套件仍有初始化、fallthrough 與 USB CDC 提醒，未改動套件來源。

Server 先以序列紀錄確認角色，再透過 `/dev/ttyACM0` 上傳；
esptool 各區塊雜湊驗證成功並重開機。
後續接入 Client，依晶片 MAC 尾碼 E91C 確認為原綁定裝置；當時也分配到
`/dev/ttyACM0`，使用 `tbeam-client` 並明確覆寫 upload port 完成燒錄。
Client 各區塊雜湊驗證成功，韌體 SHA-256 與先前通過 build 的產物一致。
OTA 環境本輪僅驗證 build，沒有執行無線傳送。

Server 開機與 HTTP 驗證：

- LoRa、PMU、BME280 初始化成功；PWM 初始化成功，狀態為 manual、angle／target 90°。
- 校正為 false、mount offset 為 0，UART inactive；GPS client E91C 遷移保留、capacity 1。
- status／track 與首頁可讀，首頁提供手動／GPS／Jetson；已移除的兩個 API 路徑 GET 回 JSON 404。
- 短時間正常 HTTP 讀取取樣：控制服務最大間隔 54 ms、≥250 ms 次數 0；
  HTTP 最大 53.66 ms、OLED 36.42 ms、PMU 3.64 ms、BME280 1.88 ms。
- 取樣在 manual、無 client 封包／有效 GPS／Jetson SET 的情境，沒有慢 HTTP 或 I2C 故障注入。
  不代表追蹤負載下的延遲保證，也未目視確認 OLED 畫面或實際機械角度。

Client 開機與發送驗證（25 秒序列觀察，包含開機等待）：

- 回報 CLIENT 模式、node ID E91C，LoRa／PMU／BME280 初始化成功。
- 記錄 13 次 position TX 成功、1 次 telemetry TX 成功，position TX 失敗 0 次。
- GPS fix 為 0，未收到 ACK；本輪 Server HTTP 亦無法連線。
  已確認 Client 開機與本機無線電完成發送，尚未確認 Server 收到或 GPS 實際追蹤。

### 後續室內兩端通聯測試

使用者同時開啟 Client／Server，電腦 USB 接 Server（晶片尾碼 584C）。
先確認 GPS client 綁定仍為 E91C；接著以 GET track 約每秒、status 約每 3 秒進行
120.06 秒觀察，共 164 個 HTTP 請求，全部成功。

| 項目 | 本次觀察 |
|---|---|
| 新增位置／遙測封包 | 120／4 |
| 新增 ACK 發送完成 | 31 |
| 接收錯誤／拒收／ACK 錯誤／逾期 ACK 跳過 | 全部 0 |
| 連線狀態 | 所有樣本 linked=true，位置 last_rx_sec 均為 0 |
| 訊號樣本 | RSSI -30 至 -26 dBm；SNR 10.5 至 11.8 dB |
| 控制服務最大間隔 | 37 ms；≥250 ms 次數 0 |
| ESP32 HTTP／LoRa 操作最大耗時 | 9.76／7.78 ms |
| OLED／PMU／BME280 最大耗時 | 36.42／4.52／1.87 ms |
| 重開機 | 未觀察到，uptime 持續增加 |
| GPS 與角度 | 兩端 fix=0；角度／目標回報均維持 90° |

GPS 未校正狀態下，18 個 GPS 模式樣本皆保持原角度且 UART inactive，並出現未校正
提醒。原定 pause／resume 自動流程因外部模式切換中止；使用者確認當時正在操作
網頁按鈕，因此不列為韌體故障，也不算 pause／resume 已通過。本次後段改為只讀，
保留使用者選擇的 Jetson 模式，持續回報 source=hold、uart_state=waiting、等待
Jetson 指令；LoRa 仍持續接收。沒有注入 UART SET 或設定指南針假角度。

已移除的 stop／磁力計校正 API POST 回 404；舊 auto 模式與 bearing=360 校正
請求回 400，未寫入校正。GPS 實際定位追蹤、Client 端收到 ACK 的紀錄、斷線恢復
與實際機械指向仍未驗證；不能把 Server 的 ack_tx 當成 Client 已接收的直接證據。

本次發現兩項問題，尚未修改韌體：

1. **HTTP 端到端延遲。** 兩分鐘測試回覆中位數 20.82 ms、最大 1,068.42 ms。
   另 30 次分段短測全部 HTTP 200，重現 2 次 408–418 ms 回覆：TCP 連線建立約
   203–212 ms、送出請求後等待回應標頭約 205 ms。期間控制最大間隔仍 37 ms，
   ESP32 HTTP 處理最大仍 9.76 ms。延遲出現在連線／等待回應階段，尚未定位到
   Wi-Fi、TCP 或 HTTP 排隊的哪一層；目前無證據顯示主迴圈同步卡住一秒。
2. **分鐘 log 的接收百分比偏低。** 反覆出現 `pkt=60/62 (96%) tlm=2`。
   DATA 與 TELEMETRY 共用 txSeq，但 log 分子只算 DATA，分母卻含遙測序號，
   因此即使完整收到 60 個位置與 2 個遙測封包也會顯示 96%。這不能當作實際
   遺失 4% 的證據；需統一同一序號視窗內的計數範圍，再處理重複與 client 重啟。

本次暫存結果、樣本與分段延遲資料位於 `/tmp/shore-indoor-test-0qkmzvoj/`。

### 手動滑桿卡頓修正與再次上傳

已重現前端每次 input 重設 75 ms 倒數，連續快速拖動一秒可完全不送角度，直到
放開才送。改為第一筆立即送，拖動時依請求開始時間限頻至最多每 75 ms 一筆；
新輸入只更新待送角度。放開儘快補最後一筆，慢回覆時仍只允許一筆在途，模式切換
會取消未送角度並等待已送請求完成。

- 新增 `node tools/test_servo_slider.js`，7 個案例全部通過，涵蓋連續拖動、慢回覆、
  最後角度、重新開始拖動，以及模式切換時取消計時器／等待在途請求。
- 使用實際頁面處理函式模擬每 16 ms 一個 input：修正後在一秒拖動期間送出 14 筆，
  第一筆在 0 ms、之後間隔 75 ms，放開於 1,000 ms 補第 15 筆。這是模擬傳輸結果，
  實際網路較慢時頻率會降低，保留最新角度而不累積舊指令。
- 既有網頁語法／DOM／模式／指南針模擬檢查與 `git diff --check` 通過。
- Server build 通過：RAM 60,420 B，韌體空間 1,265,748 B；依晶片尾碼 584C 確認
  `/dev/ttyACM0` 後 USB 上傳成功，各區塊雜湊驗證通過。
- 板上下載的首頁與本地修正版逐 byte 相同；以該頁程式重跑 7 個滑桿案例也全部通過。
  開機紀錄與 API 回報 manual／90°／PWM 正常、E91C 綁定保留，6 秒內新增 6 個
  Client 位置封包；本次沒有用模擬角度驅動實體 Servo。

既有瀏覽器分頁需重新整理才會載入新排程。前述 HTTP 端到端延遲與 log 百分比問題
仍待處理；本次未把編譯／模擬通過當成現場拖動手感已驗收。
本次暫存 log、上傳產物雜湊與板上頁面位於 `/tmp/shore-slider-upload-ffj52kh6/`。

本輪亦重跑網頁模擬與 `git diff --check`，均通過。完整暫存 log 與驗證摘要位於
`/tmp/shore-build-upload-m6dqj6oo/`（暫存目錄可能清除）。

前批已驗證 WMM2025 產生器的 12 組 NOAA 參考值，5,412 個網格取樣最大插值誤差
0.002660°；本批保留該模型與換算表。這不含指南針／模型本身或現場磁干擾誤差。

## 本批結果

| 項目 | 現在行為 |
|---|---|
| 控制模式 | 手動／GPS／UART，來源互斥；訊號失效保持，不自動切另一模式 |
| 開機 | 90° PWM、manual；沒有新命令不自行追蹤 |
| GPS | 校正／磁偏角有效且兩端 Good 持續 2 秒後追蹤；失效保持 |
| UART | 只接受 UART SET，不需 ARM，不要求 GPS／指南針校正 |
| UART 停止／恢復 | 停送 SET 後滿 250 ms 保持；新的 SET 可直接恢復，保留限速／角度限制／故障處理 |
| 過期串流 | 主 loop 停頓 ≥250 ms 丟掉已緩衝 bytes，等換行恢復；半行也有期限 |
| 模式切換 | 先撤銷舊目標與 UART session；只有 UART 模式打開 UART，重新進入需新的完整 SET |
| HTTP stop | `/api/track/stop` 移除，回 404；網頁手動與既有 pause 仍可停止追蹤 |
| 指南針校正 | 普通磁針讀數、只存 RAM；保留 GPS 位置／日期自動磁偏角換算 |
| 板上磁力計 | QMC 初始化、取樣、旋轉補償、校正頁、API、SensorLib 依賴與校正 NVS 讀寫移除 |
| OLED | 位址偵測獨立於 QMC，保留 0x3C／0x3D 板型支援 |
| I2C | Wire／PMUWire 單筆交易 timeout 改 10 ms |
| 診斷 | status.timing 保存各類同步操作耗時與最大控制服務間隔，不逐次 log |
| 關機 | 一般與低電量路徑先回手動、撤銷控制，再顯示畫面、延遲與要求 PMU 斷電 |
| 計時 | 週期 deadline 改差值比較，修正長期未用的重繪／Wi-Fi／idle log deadline |

開機 90°、單一 GPS client、非阻塞 ACK、磁偏角、手動 slider、Wi-Fi 網頁／OTA、
LoRa wire layout、GPS 品質門檻與 120°/s 限速仍保留。

## 操作與相容性變化

- GPS 不好時不再切 UART；UART 沒指令也不會切 GPS。
- GPS 模式才顯示定位／連線／鏡頭未校正的操作警告；UART 顯示等待 SET。
- 腳架轉動不再自動補償，需重新讀取鏡頭磁針。重開機／重新架設也需重校。
- 所有校正 NVS 都不再需要；但單一 client 綁定、Client ATPC／功率仍需 NVS。
  舊 mount*／mag* 鍵保留但忽略，不清除整個儲存區。
- 模式入口 `mode=manual|gps|uart`；舊 `mode=auto` 回 400。
  `mode=jetson` 是相容輸入別名，輸出統一為 `uart`。
  `/api/track/stop`、`/api/mag/calibrate` 回 404；不會轉址到 HTML 冒充成功。
- HTTP start 選 GPS；resume 恢復上次選的 GPS／UART，開機預設 GPS；pause 保持並允許手動。
- 不再輸出 mag、站體 heading／pose 或 uart_armed 欄位；使用 uart_state／uart_ready。
- 舊 UART 程式若送 ARM，該行被忽略；有效 SET 仍可用。若依賴 STOP，應改成停止
  傳送 SET，或由操作端切 manual。此批沒有修改／部署外部控制端專案。

## 阻塞改善做到哪裡

主 loop 現在於 HTTP／環境／電量／PMU／LoRa／OLED 工作之間服務控制，GPS tick 仍是
20 Hz。兩個 I2C bus 的單筆交易等待由預設 50 ms 改 10 ms；板上磁力計取樣完全移除。
OLED 位址不再靠 QMC 初始化回推，改先探測 0x3D、否則用 0x3C。

`/api/status.timing` 記錄：

- 控制最大服務間隔與 ≥250 ms 次數。
- loop、HTTP、BME280、PMU、OLED、LoRa、OTA 的 last/max 毫秒與 ≥50 ms 次數。
- UART 晚服務、丟棄 bytes、非法／過期指令行數。

數值自開機累計；HTTP 與 loop 當次耗時在返回後更新。HTTP 內的 PMU 讀取亦會算入
HTTP 耗時，數字有重疊，不能直接加總。

**同步 HTTP 尚未根除。** 本機 WebServer 的 handleClient 會將 socket timeout 設為
5,000 ms，再同步讀 request／header；僅在主程式先 setTimeout 會被覆寫。因此本批
沒有假裝一行設定就能保證 250 ms。I2C 一段操作也可含多筆交易，10 ms 不是整段上限。
要確認改善幅度，需要實機讀取新指標；若 HTTP 峰值仍超過控制期限，再做有界的收送
或獨立控制 task。task 方案須同時處理模式命令、PWM 與狀態資料的執行緒邊界。

## 仍建議處理的項目（本批未修改）

### 1. LoRa DATA 值域與順序驗證

`parseDataPacket()` 驗過 header 後就使用經緯度、fix、course，尚未拒絕非法範圍、
重複／倒退 seq。異常或重複資料能刷新接收時間，被當成新位置。
建議加入 payload 驗證與序號去重，處理 uint16 wrap 及 client 重啟。舊資料若在發送端
才排隊，接收端沒有量測時間可判斷，未來若需要端到端期限才擴充時間欄位。
GPS／UART 已互斥，因此前批「自動交接到不同 surfer」不再列為本架構問題。

### 2. 網頁請求逾時與過期狀態

`fetchTrack()` 與通用 `post()` 無 timeout；卡住可能讓 refreshing／controlChanging
無法清掉，畫面停在舊狀態或不能切模式。手動角度 POST 雖已有 deadline，其餘還沒有。
建議統一請求期限、顯示最後成功更新時間，失聯時撤掉有效狀態提示。
這是瀏覽器請求管理，與本批 ESP32 的 HTTP 耗時計數是不同的修改。

### 3. GPX 匯出不要立即清空

`exportTrackGpx()` 觸發下載後立即 revoke URL、clearHist；頁面不知道手機是否保存成功。
取消／失敗時可能失去整段軌跡。建議匯出與清除分開，沿用現有清除按鈕。
更換 GPS client 時也應清空或分段瀏覽器 hist，避免不同人的位置串成同一段。

### 4. 掉包率與封包率的顯示含義

status 的 drop_rate 是「收到但拒收」比例，不包含完全沒收到的封包；cachedPktRate
只在收到新資料時刷新，斷線可能維持舊值。建議分為拒收／接收錯誤／seq 推算遺失率，
並讓斷線封包率歸零，避免把數字當成無線訊號品質的完整證明。

### 5. 指南針位置與實機驗收

WMM 磁偏角無法補 Servo、鏡頭金屬與線路電流造成的局部磁場。建議比較 Servo 上電／
斷電、不同指向的讀數；若變化明顯，拉開距離或改非磁性支架。現在沒有板上旋轉補償，
腳架固定與重校的操作尤其重要。

native 45 案例、client/server/server-ota build 與 USB Server 開機驗證已完成。
仍需台架測模式隔離、斷線／恢復、晚命令丟棄、I2C 故障與慢 HTTP 的最大服務間隔。
編譯通過不能取代實機 OLED 位址、PWM 指向、GPS 日期、手機 UI 與 OTA 驗收。

模型來源：[NOAA 磁偏角](https://www.ncei.noaa.gov/products/magnetic-declination)、
[WMM2025](https://www.ncei.noaa.gov/products/world-magnetic-model)。
