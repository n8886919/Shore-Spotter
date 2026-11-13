# Session 交接入口（2026-11-14）

先讀 `AGENTS.md` → [下次更新與外測](next-session-plan.md)。過往證據由 [紀錄索引](records-index.md) 查回。
使用者已授權完成健檢六項修正與交接；**下次起床後會再要求更新 App，再出門測試**。
本輪只修程式、測試、build 與保存產物；沒有操作手機、網路、燒錄、重啟或舵機。

## 這次已完成

| 項目 | 已有證據 | 部署／驗收界線 |
| --- | --- | --- |
| Android 0.2／versionCode 2 | 保留 GPS-only；失聯撤銷排隊控制；背景保存後回前景恢復錄影鍵；新整合回歸先紅後綠、5 JUnit、assembleDebug、16 KB／zipalign 通過，lint 0 errors／26 warnings | **未安裝手機、未真機／外測** |
| Station 快照 | 板載 GNSS freshness 不再混用手機狀態；SD／Axiom 追加實際選用岸端 `station_position` | **Station 未燒錄；只更新 App 不會得到新 SD 欄位** |
| T096 DATA | 超出速度編碼範圍仍送有效位置；編碼錯誤可見、不空增序號、不重試同 epoch | **Client 未燒錄**；wire v5、BLE v1、正常 2 Hz 不變 |
| 部署工具／文件 | board／role／environment 交叉檢查；換 Station 改用 RST，USER 長按保留 owner | 11 組設定回歸與現有四台 manifest 通過；不等同 USB 目標驗證 |
| 整體主機驗證 | **51／51 suites，native 140／140**；LilyGO Station／T096 build 通過；T096 ZIP CRC／application-only 通過 | 不是 RF、CameraX、機械或戶外驗收 |

程式 commits：`4e1c00c`（manifest／文件）、`60d5d7b`（韌體／記錄）、`b395973`（Android）。
修正負例、驗證與產物保存見 [本次階段紀錄](records-index.md#health-fixes-2026-11-14)。
韌體 release 仍為 1.0，修正記在 CHANGELOG「未發行」；不能只看 1.0 判定是否已有本次修正。

## 下次使用的產物

本機目錄：`~/.local/state/shore-spotter/health-fixes-20261114T011939+0800/`。
`artifacts.json` 保存各產物 SHA-256、來源 commit、build environment、私人設定 fingerprint 與驗證摘要。
產物與 ELF／設定 fingerprint 僅在本機，不加入公開 repo；部署前仍須核對目前目標與 Wi-Fi 設定。

- **優先更新 APK：`shore-spotter-0.2.apk`**，套件 `tw.shorespotter.app`、versionCode 2／versionName 0.2。
  SHA-256：`9f5cb1f551452739ce3e21d02869f9cdfff64885a47f1ddae6e822919b2d8681`。
- Station：`tbeam-station/firmware.bin` 與對應 `firmware.elf`。
- T096：`t096-client/firmware.zip`（application-only），另留 bin／elf。
- 同目錄有 Android red／green、Gradle、lint 摘要、16 KB／zipalign、完整主機與雙端 build logs。

先前 0.1 GPS-only 基準 APK 仍保留於 `~/.local/state/shore-spotter/android-gps-only-20261114T002648+0800/`，
hash `7e2fbf19faf1edd3c91c2297cfd241917ba91b2c05d8ae91c753de56496bfb0c`；它不是下次預設安裝產物。

## 仍不能宣稱已解決

- 使用者最新觀察：第一次除 NETWORK GPS 外其他正常。GPS-only 尚未真機外測，不能將每次跳角都定案為 NETWORK。
- 11/13 21:56–22:19 多次約 80° 跳動，Surfer／板載 GPS 無相應跳點；舊紀錄缺實際手機座標／provider。
  新快照只能改善部署後證據，無法回填過去。
- 11/13 22:18:26 有 panic，但沒有 crash backtrace；本輪沒有找到或修正已證明的 panic 根因。
- 當時手動命令 119°／266°／109° 已抵達軟體；UART、控制板與 Servo 無回授，不能全部歸因 Wi-Fi。
- Station 上次部署仍為 main build `Nov 13 2026 01:13:33` 的 Web 恢復版；當日後續仍有連線異常。
- Axiom 某次回家後 boot 有 enabled／low_memory／requests=0，不能套用外測或當成崩潰根因；本輪未移除或改設定。
- CAD 仍由 `cad/reference.json` 指向獨立 repo。FCStd／STL committed hash 已核對，但 STL 與目前幾何一致性未驗證。

## 系統邊界

T096 Ready 關 GPS／LoRa，BLE Claim 固定本次 owner；Start 的 HTTP 202 只是受理，匹配 STATE 才確認。
現役為 T096 Client＋LilyGO Station；runtime A／B 均 SF9。Youmi M0、TX43、9600、0～359°，無 ACK／物理角度回授。
手機位置保留到 Station 重開，錄影／定位生命週期不自動切換 Servo 模式。舊 profiles／Axiom 本輪均未刪除。
操作規格見 [App](../android/README.md)、[API](interface.md)、[SD](sd-log.md)、[硬體](hardware.md)。
