# Session 交接入口（2026-11-14）

先讀 `AGENTS.md` → [下一 session 規劃](next-session-plan.md)。需要過往證據才到 [紀錄索引](records-index.md) 查回。
使用者希望精簡程式與機制，方便理解及 debug；**最新觀察：第一次除了 NETWORK GPS 問題，其餘正常**。
先驗證單一修正，再決定刪除哪些階段性功能；本輪未開始功能刪減。

## 已完成與尚未驗證

| 項目 | 已有證據 | 仍未證明 |
| --- | --- | --- |
| Android GPS-only | 只訂閱 GPS，收樣／送出前拒絕非 GPS；5 個 JUnit、build／16 KB 對齊通過，lint 0 errors／26 warnings | **未安裝手機、未外測** |
| Station Web 恢復 | 11/13 曾 USB 部署逾時釋放／前景重讀版本，main build `Nov 13 2026 01:13:33` | 11/13 晚仍有連線異常，不能視為戶外已解決 |
| 11/13 21:56–22:19 外測 | 多次約 80° 跳動時 Surfer／板載 GPS 沒相應跳點；22:18:26 開機原因為 panic | 未記實際手機座標／provider，不能將每個尖峰定案為 NETWORK；無 crash backtrace |
| 手動 Servo | 後段 Station 收到 119°／266°／109° 等目標並改變軟體命令 | UART／控制板接收與實體動作無回授，不能全部歸因 Wi-Fi |
| Axiom | 調查時另一 boot 回報 enabled／low_memory／requests=0；queue＋stack 常駐至少約 22 KB | 回家後計數不能套用外測，未證實上傳堵塞主迴圈 |
| CAD | 使用者更正為 `~/freecad/Shore-Spotter-Case/`，獨立 commit `f3c69b9` 保存 FCStd／STL；對照 `cad/reference.json` | 未重算幾何，STL 早於 FCStd 存檔，兩者是否一致未確認 |

GPS-only APK SHA-256：`7e2fbf19faf1edd3c91c2297cfd241917ba91b2c05d8ae91c753de56496bfb0c`。
本機產物：`~/.local/state/shore-spotter/android-gps-only-20261114T002648+0800/shore-spotter-gps-only.apk`。
手機現有 APK 尚未與目前 source 位元組比對，不把歷史真機驗收套用新版。

## Git 與紀錄

- `records/2026-11-14`／`bcce441291c4c9ee6f2b9fe2b7817b1976702bf6` 保存實驗、既有程式 WIP 與 Android；當時誤納入 Pocket 診斷，後續已移除副本。
- 正式外殼在獨立 `~/freecad/Shore-Spotter-Case/` repository 提交，完整 commit／hash 見 `cad/reference.json`。
- 階段性文件由後續清理 commit 移出工作目錄，`python3 tools/read_record.py` 可列出／讀取／匯出；索引固定保存 commit／原路徑／hash。
- 原始 USB／HTTP／GPS 證據留在本機 `~/.local/state/shore-spotter/field-investigation-20261113T235548+0800/`，不納入公開內容。
- checkpoint 前完整主機回歸 50／50 組通過；文件歸檔不等於重新燒錄、手機或戶外驗收。

## 現行系統入口

- 原始碼 1.0、LoRa wire v5／BLE wire v1；版本唯一來源 `include/firmware_version.h`。
  使用 T096 Client、LilyGO T-Beam Supreme Station；舊 LilyGO Client／Heltec V4／診斷 profiles 仍在，刪除前查清用途。
- T096 Ready 關 GPS／LoRa，BLE claim 固定本次 owner；Start 開 GPS／LoRa，Stop 回 Ready。
  HTTP 202 是 pending，匹配 Client boot／owner／command 的 STATE 才確認；Ready 沒有舊 30 秒 LoRa 輪詢。
- A／B runtime 候選 923.2／923.8 MHz，皆 SF9。API 見 [interface](interface.md)、[binding](client-binding-api.md)、[OpenAPI](openapi-client-control.json)。
- Youmi 固定 M0，UART2 TX43、9600 8N1，RX 不配置；0～359°／中位 179.5°，無 ACK／實體回授。
  接線／校正見 [hardware](hardware.md)、[motion-control](motion-control.md)、[uart-servo](uart-servo.md)。
- Station SD 自動記錄；低 heap／忙碌會略過快照，`sample_dropped` 不是 RF 漏包。
  Axiom 預設關閉不代表 NVS 實際停用；見 [SD](sd-log.md)、[Axiom](axiom-log.md)。
- App 規格與建置見 [android/README](../android/README.md)。僅操作 `tw.shorespotter.app`，不改手機網路或全域設定；無背景／鎖屏服務。
  Station 保留最後手機位置至重開機。

## 下一步與操作界線

依 [下一 session 規劃](next-session-plan.md) 先做 GPS-only 真機／戶外基準，再評估 Axiom、舊實驗入口與重複流程。
不先重寫整條追蹤，不為掩蓋失敗降低記憶體／freshness 門檻。
定位來源、網路到達、UART 寫入與實體動作分別驗證。

本輪沒有操作手機、改網路、燒錄、重啟或發送舵機／Client 命令。
下一 session 操作前重新核對裝置、IP、boot、模式與授權；歷史位址不能直接沿用。
