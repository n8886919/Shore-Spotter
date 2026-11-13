# 健檢六項修正與待部署產物（2026-11-14）

使用者授權：完成健檢指出的六項必做修正及交接，下次起床後再要求更新 App，接著外測。
本輪沒有操作手機、Station／Client、網路、serial、燒錄或舵機。下次以當前交接為入口，不能套用本階段狀態到未核對的新部署。

## 修正與證據

| 問題 | 修正／負例 | 提交 |
| --- | --- | --- |
| manifest 接受錯誤板型／role／environment | check／plan／loader 共用明確對照；舊碼 15 個新增 subcases 失敗，修正後 11 組設定測試通過 | 4e1c00c |
| 換 Station 說明誤稱 USER 重啟 | 更正 RST；USER ACT／SLP 保留 owner | 4e1c00c |
| 板載 GNSS 快照錯用手機 freshness，缺實際岸端輸入 | 板載 freshness 獨立，新增 station_position；實際來源／capture／encoder 的 phone、GNSS、invalid 兩種 schema 負例先失敗後通過 | 60d5d7b |
| T096 25.41 m/s 有效位置整包編碼失敗，序號空增 | 保留 fix、速度 unknown；錯誤可見、不重試同 epoch／空增序號，實際函式負例先失敗後通過 | 60d5d7b |
| App 背景完成錄影後回前景不能錄 | 恢復錄影鍵，保存中／相機不可用仍停用，實際 lifecycle methods 先失敗後通過 | b395973 |
| App poll 失聯後仍送 queued control | worker 先封鎖 failedGeneration，UI 撤銷舊意圖；執行前／GET 後重查 freshness，舊失敗不影響新連線，motion／client 排隊案例先失敗後通過 | b395973 |

沒有改 GPS 追蹤演算法、手機位置保留規則、限速、Axiom 設定、BLE／LoRa wire 或正常 2 Hz。
沒有加入新 worker／重試狀態機。Sample 主機大小由1200增加到1240 bytes，Axiom queue增加320 bytes、SD queue增加80 bytes；
另有各局部 Sample 的40 bytes，實機記憶體峰值尚未量測。schema2／3新增選用位置，既有keys保持；欄位路徑230／256。

## 主機結果

- `python3 tools/verify_project.py`：51／51 suites；native140／140。
- `env -u SHORE_DEVICE pio run -e tbeam-station -e t096-client`：兩端成功；僅建置，沒有upload target。
- 原有Android三組JUnit共5／5、assembleDebug、lint0 errors／26 warnings（未全部消除既有警告）。
- 新Android實際方法整合測試：UI回呼前後失聯、舊世代、期限、重連新手勢、背景／前景Finalize及相機不可用。
- APK四個64位JNI的16 KB ELF／APK對齊、zipalign通過，package/versionCode/versionName為tw.shorespotter.app／2／0.2。
- T096 ZIP CRC與application-only manifest通過，未改bootloader／SoftDevice。

## 保存產物

本機目錄：`~/.local/state/shore-spotter/health-fixes-20261114T011939+0800/`。
`artifacts.json`保存各檔案hash、各程式來源commit、build環境與本機私人設定fingerprint；不把私人設定、bin／elf或完整原始位置log提交。

- App：`shore-spotter-0.2.apk`，SHA-256 `9f5cb1f551452739ce3e21d02869f9cdfff64885a47f1ddae6e822919b2d8681`。
- Station：`tbeam-station/firmware.bin`、對應elf。
- Client：`t096-client/firmware.zip`、bin／elf；兩端firmware source為60d5d7b。
- Android source為b395973；另留紅／綠回歸、Gradle、16 KB／zipalign、主機與雙端build logs。
- 先前0.1 GPS-only APK仍保留在先前目錄，不覆寫；新0.2才是下次預設安裝產物。

## 尚未驗證

App未安裝、CameraX／實際Wi-Fi／GPS尚未驗收；兩端未燒錄，不能把新SD欄位或DATA修正算在現役裝置上。
11/13跳角／失聯與panic沒有完整根因證據。只有panic reset reason，尚無配對ELF的crash backtrace，本輪未新增持久化coredump。
App／Station／T096三個更新範圍須分開確認，外測分段不混入清理Axiom或改追蹤門檻。
當前交接及下一session規劃持續留在工作目錄；本階段記錄依Git固定commit／path／SHA-256索引歸檔。
