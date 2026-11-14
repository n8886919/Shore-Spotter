# 階段性紀錄索引

當前待辦見 [下一 session 規劃](next-session-plan.md)，現行規格見 [交接入口](assistant-handoff.md)。
本頁只保留可查回的索引；已結束的實驗／部署／階段交接文件不留在工作目錄。
原路徑、完整 commit 與 SHA-256／附件清單保存在 [records.json](records.json)。

最初保存 checkpoint：`bcce441291c4c9ee6f2b9fe2b7817b1976702bf6`，標籤 `records/2026-11-14`。
當時的 Android 原始碼在這個 commit；後續修正以各紀錄釘選的 commit 為準。CAD 目錄之後由使用者更正為 `~/freecad/Shore-Spotter-Case/`，
正式外殼已在獨立 repository 提交 `f3c69b92dbabbcf2fabaf60c2b81fcd3ed723c81`，見 [CAD 索引](../cad/README.md)。
原實驗記錄保留更正前的 Pocket 診斷脈絡；請以當前 CAD 索引為準。本地建置輸出、憑證與原始位置 log 未納入。

## 查閱與取回

在專案根目錄執行：

```sh
# 列出全部紀錄；可再用 rg 搜尋標題／日期／ID
python3 tools/read_record.py
# 直接閱讀本次實驗，不改工作目錄
python3 tools/read_record.py field-experiment-2026-11-14
# 匯出紀錄與圖片，沿用 docs/ 相對路徑
python3 tools/read_record.py field-experiment-2026-11-14 --export /tmp/shore-records
# 逐份核對 Git 內文件／圖片的 SHA-256
python3 tools/read_record.py --verify
```

不依賴工具也可讀：

```sh
git show records/2026-11-14:docs/field-experiment-2026-11-14.md
git log --all -- docs/field-experiment-2026-11-14.md
```

刪除 commit 的 `HEAD:原路徑` 會找不到檔案，請使用表內索引所釘選的保存 commit。
需整套歷史文件互相導覽時，可在臨時目錄建立該 commit 的 detached worktree；不要切換或 reset 現有開發目錄。

## 紀錄清單

| ID／原檔名 | 日期 | 內容 |
| --- | --- | --- |
| <a id="memory-and-field-evidence-2026-11-14"></a>`memory-and-field-evidence-2026-11-14` | 2026-11-14 | Axiom停用對照、SD UTC／UART讀回、既有coredump、App／Surfer組合與驗收界線 |
| <a id="deployment-app-0-3-2026-11-14"></a>`deployment-app-0-3-2026-11-14` | 2026-11-14 | 584C／F035 燒錄、App 0.3 安裝／IP 快選及位置快照記憶體缺口 |
| <a id="health-fixes-2026-11-14"></a>`health-fixes-2026-11-14` | 2026-11-14 | 健檢六項修正、App 0.2／兩端韌體待部署產物與驗證 |
| <a id="field-experiment-2026-11-14"></a>`field-experiment-2026-11-14` | 2026-11-14 | 11/13 外測與 GPS-only 修正實驗紀錄 |
| <a id="web-recovery-2026-11-13"></a>`web-recovery-2026-11-13` | 2026-11-13 | Station Web 逾時與前景恢復修復（2026-11-13，已部署 584C） |
| <a id="android-app-2026-11-13"></a>`android-app-2026-11-13` | 2026-11-13 | Android 相機／控制／定位整合（2026-11-13） |
| <a id="t096-maintenance-2026-10-28"></a>`t096-maintenance-2026-10-28` | 2026-10-28 | T096 開蓋維護按鍵與顯示（2026-10-28） |
| <a id="indoor-acceptance-handoff-2026-10-28"></a>`indoor-acceptance-handoff-2026-10-28` | 2026-10-28 | 2026-10-28 室內驗收與接續任務 |
| <a id="ble-reconnect-2026-10-28"></a>`ble-reconnect-2026-10-28` | 2026-10-28 | 2026-10-28 BLE 重連查證與修補（Station 已部署，Client 未部署） |
| <a id="info-page-2026-10-27"></a>`info-page-2026-10-27` | 2026-10-27 | 2026-10-27 資訊頁精簡與狀態顯示驗證 |
| <a id="ble-control-2026-10-27"></a>`ble-control-2026-10-27` | 2026-10-27 | BLE 待機、開機認領與手動選頻（2026-10-27） |
| <a id="t096-health-diagnostics-2026-10-22"></a>`t096-health-diagnostics-2026-10-22` | 2026-10-22 | T096 MCU 溫度與異常 NMEA 證據（2026-10-22） |
| <a id="health-check-2026-10-16"></a>`health-check-2026-10-16` | 2026-10-16 | 專案深度健檢（2026-10-16） |
| <a id="servo-ui-deployment-2026-10-11"></a>`servo-ui-deployment-2026-10-11` | 2026-10-11 | 2026-10-11 M0 串口／正式資訊頁部署驗證 |
| <a id="t096-field-validation-2026-10-10"></a>`t096-field-validation-2026-10-10` | 2026-10-10 | T096 帶殼／內建 GNSS 天線外測（2026-10-10） |
| <a id="servo-controller-youmi-2026-10-10"></a>`servo-controller-youmi-2026-10-10` | 2026-10-10 | Youmi 35 kg＋雙路串口 Servo 控制板（2026-10-10） |
| <a id="t096-rtc-standby-2026-10-09"></a>`t096-rtc-standby-2026-10-09` | 2026-10-09 | T096 保留狀態的 RTC 待機 |
| <a id="t096-power-validation-2026-10-08"></a>`t096-power-validation-2026-10-08` | 2026-10-08 | T096 無線充電與收納喚醒驗證（2026-10-08） |
| <a id="sf9-field-pair-2026-10-07"></a>`sf9-field-pair-2026-10-07` | 2026-10-07 | 2026-10-07 T096／LilyGO Station SF9 更新 |
| <a id="lilygo-station-2026-10-06"></a>`lilygo-station-2026-10-06` | 2026-10-06 | LilyGO E91C 改為 Station，配對 T096 F035 |
| <a id="t096-controls-2026-10-05"></a>`t096-controls-2026-10-05` | 2026-10-05 | T096 按鍵、螢幕、電量與 Station RF 顯示（2026-10-05） |
| <a id="lilygo-t096-2026-10-04"></a>`lilygo-t096-2026-10-04` | 2026-10-04 | LilyGO Station 接收 T096（2026-10-04） |
| <a id="link-test-2026-10-02"></a>`link-test-2026-10-02` | 2026-10-02 | 2026-10-02 Heltec 桌上驗證 |
| <a id="tracking-update-2026-09-19"></a>`tracking-update-2026-09-19` | 2026-09-19 | GPS／LoRa／追蹤更新：2026-09-19 |
| <a id="packet-proposal-review-2026-09-13"></a>`packet-proposal-review-2026-09-13` | 2026-09-13 | 封包精簡與 2 Hz 提案審核 |
| <a id="link-test-2026-09-13"></a>`link-test-2026-09-13` | 2026-09-13 | LoRa v4 實機通訊測試 — 2026-09-13 |
| <a id="health-check-2026-09-08"></a>`health-check-2026-09-08` | 2026-09-08 | 專案健檢：2026-09-08（更新至 2026-09-09） |

## 之後每個階段的固定操作

1. 寫短實驗紀錄，更新當前規格、CHANGELOG 與 `next-session-plan.md`，正常 commit 保存。
2. 在 `records.json` 追加該紀錄的原路徑、保存 commit、SHA-256 及必要圖片；commit 必須已存在，不能用可能移動的 `HEAD` 代替。
3. 先執行 `read_record.py --verify`，確定文件／圖片真的在 Git；再移除工作目錄中的階段性文件與不再使用的專用圖片，更新舊連結到本索引，提交清理。
4. 實際匯出至少一份，確認圖片也能取回。當前操作規格、API、硬體接線與下一步計畫持續保留，不能只留在歷史。

未結束的工作寫在固定的待辦入口；已結束的待辦細節可隨階段紀錄歸檔。未驗收事項保持未驗收，不因歸檔改成通過。
本流程保存正常可達的 Git commit，不使用 amend、reset 或歷史重寫來清理紀錄。
