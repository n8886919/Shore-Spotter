# Session 交接入口（2026-11-14）

先讀 `AGENTS.md` → [下次更新與外測](next-session-plan.md)。過往證據由 [紀錄索引](records-index.md) 查回。
目前已完成 **LilyGO Station 584C、T096 F035 燒錄，以及 Pixel 10 App 0.3 安裝**。使用者確認舵機斷電後才重啟 Station。
App 0.3 增加最近兩個成功 Station IP 快選；目前一個真實地址已在手機驗證可點選連線。
下次以出門前檢查與外測為主，**不需再安裝先前 0.2**。戶外 GPS-only／跳角／panic 尚未驗收。

## 最新部署（2026-11-14）

完整步驟、真機負例與驗證見 [部署紀錄](records-index.md#deployment-app-0-3-2026-11-14)。

- Station：build `Nov 14 2026 01:21:02`，boot `3950833316`；active app1 application-only 燒錄，hash 驗證通過。
  保留 NVS／分區／SD，實際匯出新 SD 並核對 CRC，20 個快照含 `station_position`。SD 已恢復 recording；快照仍有略過，不能等同 RF 漏包或宣稱所有快照持續保存。
- T096 F035：build `Nov 14 2026 01:21:52`，boot `82F0D506`；application-only DFU 成功，保留 bootloader／SoftDevice。
  Station 讀回相同 boot、Ready、BLE connected／claimed、owner 584C，未自動啟動位置回報。
- Station 保持手動、60°/s、command_seq=0；重啟中位輸出只有 UART 證據，舵機斷電，沒有機械驗收。
- Pixel 10／Android 17：App **0.3（versionCode 3）** 安裝成功，拉回 APK 與產物逐位元 hash 相符；保留 App 資料。
  成功歷史首次從真實 poll 累積，第二個地址需下次成功連上不同 IP 才會出現；兩筆排序／去重／失敗不入列已有主機回歸。
- App 0.2 真機已驗證背景完成保存→回前景→再次錄影，暫停／繼續；兩段 1080p 約30 FPS、無音軌。0.3 亦重跑背景保存→回前景→再錄，兩段 1080p 約30 FPS、無音軌；測完已結束 App。
- 本輪沒有取得衛星 fix，不能宣稱 GPS 精度或戶外穩定；沒有用 network／fused 補位。沒有更改手機全域網路、GPS 或音訊設定。

本機部署證據：`~/.local/state/shore-spotter/deployment-20261114T013211/`，包含舊 APK、現役 Station app／NVS metadata 備份、DFU／USB／API／SD／影片檢查與新 APK。
App 0.3 SHA-256：`51df3ec533560626ba406a15b9ed2d66cc58780a72996cbc4a98334381d5b8ba`；程式 commit `f14db44`。

## 這次已完成

| 項目 | 已有證據 | 部署／驗收界線 |
| --- | --- | --- |
| Android 0.2／versionCode 2 | 保留 GPS-only；失聯撤銷排隊控制；背景保存後回前景恢復錄影鍵；新整合回歸先紅後綠、5 JUnit、assembleDebug、16 KB／zipalign 通過，lint 0 errors／26 warnings | 已先安裝 0.2 驗證，再更新 0.3；戶外未驗收 |
| Station 快照 | 板載 GNSS freshness 不再混用手機狀態；SD／Axiom 追加實際選用岸端 `station_position` | 已燒錄，實際 SD 匯出確認新增欄位 |
| T096 DATA | 超出速度編碼範圍仍送有效位置；編碼錯誤可見、不空增序號、不重試同 epoch | 已燒錄、Ready／認領確認；wire v5、BLE v1、正常 2 Hz 不變 |
| 部署工具／文件 | board／role／environment 交叉檢查；換 Station 改用 RST，USER 長按保留 owner | 11 組設定回歸與現有四台 manifest 通過；不等同 USB 目標驗證 |
| 整體主機驗證 | **51／51 suites，native 140／140**；LilyGO Station／T096 build 通過；T096 ZIP CRC／application-only 通過 | 不是 RF、CameraX、機械或戶外驗收 |

程式 commits：`4e1c00c`（manifest／文件）、`60d5d7b`（韌體／記錄）、`b395973`（Android）。
修正負例、驗證與產物保存見 [本次階段紀錄](records-index.md#health-fixes-2026-11-14)。
韌體 release 仍為 1.0，修正記在 CHANGELOG「未發行」；不能只看 1.0 判定是否已有本次修正。

## 保留的健檢產物（0.2 已由 0.3 取代）

本機目錄：`~/.local/state/shore-spotter/health-fixes-20261114T011939+0800/`。
`artifacts.json` 保存各產物 SHA-256、來源 commit、build environment、私人設定 fingerprint 與驗證摘要。
產物與 ELF／設定 fingerprint 僅在本機，不加入公開 repo；部署前仍須核對目前目標與 Wi-Fi 設定。

- 先前驗證 APK：`shore-spotter-0.2.apk`，套件 `tw.shorespotter.app`、versionCode 2／versionName 0.2。
  SHA-256：`9f5cb1f551452739ce3e21d02869f9cdfff64885a47f1ddae6e822919b2d8681`。
- Station：`tbeam-station/firmware.bin` 與對應 `firmware.elf`。
- T096：`t096-client/firmware.zip`（application-only），另留 bin／elf。
- 同目錄有 Android red／green、Gradle、lint 摘要、16 KB／zipalign、完整主機與雙端 build logs。

先前 0.1 GPS-only 基準 APK 仍保留於 `~/.local/state/shore-spotter/android-gps-only-20261114T002648+0800/`，
hash `7e2fbf19faf1edd3c91c2297cfd241917ba91b2c05d8ae91c753de56496bfb0c`；它不是下次預設安裝產物。

## 出門前優先缺口：位置快照停滯

同一新 boot 的 SD `samples` 停在20，`sample_dropped` 持續增加；文字／bytes／同步持續、SD errors=0。
本輪讀回 `heap_free=21048`、largest block=7668，低於 `sd_log::captureDue()` 的32768 bytes門檻；
Axiom enabled、state=`low_memory`、requests=0。這足以解釋當時快照略過，**沒有證明記憶體不足的配置來源或 panic 根因**。
新欄位存在已驗證，但目前不能保證外測全程都有位置快照。下次先定位記憶體佔用與記錄持續性；
不要直接降低門檻，或未釐清取捨就刪除 Axiom。USB write errors=3，後段檢查未增加，匯出 CRC 通過。

## 仍不能宣稱已解決

- 使用者最新觀察：第一次除 NETWORK GPS 外其他正常。GPS-only 尚未真機外測，不能將每次跳角都定案為 NETWORK。
- 11/13 21:56–22:19 多次約 80° 跳動，Surfer／板載 GPS 無相應跳點；舊紀錄缺實際手機座標／provider。
  新快照只能改善部署後證據，無法回填過去。
- 11/13 22:18:26 有 panic，但沒有 crash backtrace；本輪沒有找到或修正已證明的 panic 根因。
- 當時手動命令 119°／266°／109° 已抵達軟體；UART、控制板與 Servo 無回授，不能全部歸因 Wi-Fi。
- Station 先前 `Nov 13 2026 01:13:33` Web 恢復版仍曾有連線異常；現役已更新，但不因此認定歷史根因排除。
- Axiom 某次回家後 boot 有 enabled／low_memory／requests=0，不能套用外測或當成崩潰根因；本輪未移除或改設定。
- CAD 仍由 `cad/reference.json` 指向獨立 repo。FCStd／STL committed hash 已核對，但 STL 與目前幾何一致性未驗證。

## 系統邊界

T096 Ready 關 GPS／LoRa，BLE Claim 固定本次 owner；Start 的 HTTP 202 只是受理，匹配 STATE 才確認。
現役為 T096 Client＋LilyGO Station；runtime A／B 均 SF9。Youmi M0、TX43、9600、0～359°，無 ACK／物理角度回授。
手機位置保留到 Station 重開，錄影／定位生命週期不自動切換 Servo 模式。舊 profiles／Axiom 本輪均未刪除。
操作規格見 [App](../android/README.md)、[API](interface.md)、[SD](sd-log.md)、[硬體](hardware.md)。
