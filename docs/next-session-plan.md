# 下次：App 0.3／兩端韌體已部署，出門前檢查與分段外測

更新：2026-11-14。本輪已依使用者授權完成 Pixel 10 App 0.3、Station 584C、T096 F035 更新。
版本／build／boot／APK hash／真機結果以 [交接入口](assistant-handoff.md) 為準；不要再安裝舊 0.2。

## 1. 出門前

**記錄停滯已改善，先核對現役再外測**：11/14下午停用Axiom、重啟前後heap約29→52～55 KB，SD快照恢復。
現役Station build `Nov 14 2026 16:52:02`；新增控制epoch／序號與UART計數，既有SNTP已獨立於Axiom，
沒有新增記錄worker／queue／重試。版本／hash／SD讀回以交接入口及[本次紀錄](records-index.md#memory-and-field-evidence-2026-11-14)為準。
保留Axiom disabled、原SD／原始RF與採樣門檻；出門前在App連線／Surfer啟動狀態確認samples持續增加、
errors增量與UTC，不把bytes增加或少量快照當整場通過。仍有採樣略過，長時間／戶外續驗。

- Station 最後是手動、60°/s，T096 是 Ready、BLE 已認領。燒錄時使用者已斷開舵機電源；重新接電前確認機構位置，不能把本輪 UART 中位輸出當作已實體回中。
- 重新辨識當下裝置／boot／網路，不沿用舊 ADB port 或 IP。App IP 框下方顯示最多兩個最近成功地址；家中地址已驗證，換熱點後依 Station 螢幕填入新 IP，成功後自動成為第二筆。
- App 0.3 已真機驗證背景結束保存後回前景再錄、IP 快選；仍需在戶外確認日照操作、長錄影與實際衛星 fix。
- 室外有衛星 fix 時確認新 GPS 樣本持續更新、Station 回覆相同快照；無 fix 時等待，不採 network／fused。
- 再開始使用時才由使用者開啟位置回報、選 GPS 追蹤。不要在讀取交接或單純安裝時自動啟動 Client／切模式。
- SD 應保持 recording、errors=0，快照包含 `station_position` 與 `control.controller`；未校時UTC仍可能null，先核對現況與增量，不把採樣略過直接當RF漏包，也不宣稱整場可靠。

驗收記錄安裝版本、時間與結果。CameraX、GPS／Wi-Fi、API、RF、實際機械動作分開記。
不要自行更動手機全域 GPS、網路或其他 App。

## 2. 外測分段與再現時留證據

基本路徑：`手機衛星 GPS → Station 岸端位置` ＋ `Surfer GNSS → LoRa DATA → 方位 → 共用限速 → UART → 控制板 → 舵機`。
先重跑第一次正常流程，再分段測停止錄影、切 App、重連；本輪不混入 Axiom 刪除、頻道／追蹤門檻或大型重構。

- 若跳角：記時間與當時操作，保留同段原始 RF、實際岸端來源／座標／年齡、Surfer DATA、目標與命令。未部署新 Station 時要明說缺少岸端快照。
- 若失聯：區分 App／HTTP、Station 是否重開，以及 Client RF；回家後另一個 boot 的計數不能代替當時證據。
- 若手動不動：先分清 API 抵達、UART sent／deferred／errors 與實體接收；沒有回授不宣稱 Servo 已到位。
- 若重開／panic：記錄發生時間、操作與新 boot，先保留／匯出 SD／DIAG，不先重刷清資料。需配對當時 ELF 與 crash backtrace 才能定位；SDK既有Flash coredump已配置／連結，且曾讀回有效歷史dump；本次部署bin／ELF已保存，回主機依[取回流程](debug-export.md)保留原分區再解碼。64 KiB容量、斷電或未進panic handler仍不保證保存，不故意製造panic。

使用者不需在海邊操作診斷工具；先記時間／操作並保留裝置資料，回來由 agent 匯出分析。

## 3. 後續與交接

接續外測，不把「沒有再現」寫成已找到 panic 根因。
每完成一段更新交接的版本／hash／boot／實測結果；有紀錄就依固定 commit＋hash 索引保存並驗證可取回。
現役Axiom先停用，之後再決定是否刪除uploader原始碼；舊profile／入口用途尚待評估。SD與Axiom共用快照／encoder，不能刪掉本地記錄或把必要BLE／HTTP一併移除。
CAD 幾何／STL 一致性、RF 長時間與機構驗收仍是獨立待辦；本輪沒有擴張這些範圍。
