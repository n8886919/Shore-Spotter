# 下次：App 0.3／兩端韌體已部署，出門前檢查與分段外測

更新：2026-11-14。本輪已依使用者授權完成 Pixel 10 App 0.3、Station 584C、T096 F035 更新。
版本／build／boot／APK hash／真機結果以 [交接入口](assistant-handoff.md) 為準；不要再安裝舊 0.2。

## 1. 出門前

**優先處理記錄缺口**：新版同一 boot 的 SD samples 停在20，heap_free約21 KB低於32 KB快照門檻，Axiom low_memory／requests=0。
先查持續佔用與原有 Axiom／BLE／HTTP 記憶體分布；不要僅降門檻或把已匯出20筆當成全程記錄通過。
若決定減少功能，先說明取捨並保留 SD／原始 RF；修正後需在 App 連線及預計外測模式下驗證快照持續增加。


- Station 最後是手動、60°/s，T096 是 Ready、BLE 已認領。燒錄時使用者已斷開舵機電源；重新接電前確認機構位置，不能把本輪 UART 中位輸出當作已實體回中。
- 重新辨識當下裝置／boot／網路，不沿用舊 ADB port 或 IP。App IP 框下方顯示最多兩個最近成功地址；家中地址已驗證，換熱點後依 Station 螢幕填入新 IP，成功後自動成為第二筆。
- App 0.3 已真機驗證背景結束保存後回前景再錄、IP 快選；仍需在戶外確認日照操作、長錄影與實際衛星 fix。
- 室外有衛星 fix 時確認新 GPS 樣本持續更新、Station 回覆相同快照；無 fix 時等待，不採 network／fused。
- 再開始使用時才由使用者開啟位置回報、選 GPS 追蹤。不要在讀取交接或單純安裝時自動啟動 Client／切模式。
- SD 目前 recording、errors=0，已讀回新 `station_position`；仍存在快照略過與 USB 寫入錯誤計數，先觀察增量，不把它們直接當 RF 漏包，也不宣稱記錄已完全可靠。

驗收記錄安裝版本、時間與結果。CameraX、GPS／Wi-Fi、API、RF、實際機械動作分開記。
不要自行更動手機全域 GPS、網路或其他 App。

## 2. 外測分段與再現時留證據

基本路徑：`手機衛星 GPS → Station 岸端位置` ＋ `Surfer GNSS → LoRa DATA → 方位 → 共用限速 → UART → 控制板 → 舵機`。
先重跑第一次正常流程，再分段測停止錄影、切 App、重連；本輪不混入 Axiom 刪除、頻道／追蹤門檻或大型重構。

- 若跳角：記時間與當時操作，保留同段原始 RF、實際岸端來源／座標／年齡、Surfer DATA、目標與命令。未部署新 Station 時要明說缺少岸端快照。
- 若失聯：區分 App／HTTP、Station 是否重開，以及 Client RF；回家後另一個 boot 的計數不能代替當時證據。
- 若手動不動：先分清 API 抵達、UART sent／deferred／errors 與實體接收；沒有回授不宣稱 Servo 已到位。
- 若重開／panic：記錄發生時間、操作與新 boot，先保留／匯出 SD／DIAG，不先重刷清資料。需配對當時 ELF 與 crash backtrace 才能定位；本輪未加入持久化 coredump，無筆電現場可能仍取不到 backtrace，必要時回主機控制條件重現並捕捉 USB 輸出。

使用者不需在海邊操作診斷工具；先記時間／操作並保留裝置資料，回來由 agent 匯出分析。

## 3. 後續與交接

接續外測，不把「沒有再現」寫成已找到 panic 根因。
每完成一段更新交接的版本／hash／boot／實測結果；有紀錄就依固定 commit＋hash 索引保存並驗證可取回。
尚待決定：Axiom 是否需要、舊 profile／入口用途。SD 與 Axiom 共用快照／encoder，移除上傳時保留本地記錄。
CAD 幾何／STL 一致性、RF 長時間與機構驗收仍是獨立待辦；本輪沒有擴張這些範圍。
