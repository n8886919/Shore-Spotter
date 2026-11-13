# 下次起床後：更新 App 0.2，分段驗證再外測

更新：2026-11-14。使用者已授權健檢六項修正與產物準備，會在下次起床後再要求更新 App。
最新產物／hash／主機結果以 [交接入口](assistant-handoff.md) 為準。本輪未操作手機或燒錄。

## 1. 更新 App

1. 重新列出 ADB 裝置，核對 Pixel 型號／系統／當次 device ID。所有命令明確指定 `-s`；不沿用歷史 IP／port。
2. 核對本輪 `shore-spotter-0.2.apk` hash／package／versionCode 2，以更新安裝保留 App 資料；不清除資料、不動其他 App 或全域網路設定。
3. 讀回安裝版本；只開 `tw.shorespotter.app`，初次檢查保持 Station 手動，不因更新 App 自動啟動 Client 或 GPS／Servo。
4. 使用者若只要求更新 App，就先完成 App 範圍，不自動加做兩端韌體燒錄。

## 2. 出門前的短驗收

- 開始一段測試錄影 → 離開 App 等保存完成 → 回前景 → 能開始第二段並保存。再測保存尚未完成就回前景，確認不會重複啟動。
- 確認無聲、影片可保存、暫停／繼續及控制面板展收不破壞錄影；只查本次 App 新建影片。
- 室外有衛星 fix 時，確認新 GPS 樣本持續更新、Station 回覆相同快照；無 fix 時等待，不採 network／fused。
- GPS 停用／啟用恢復需要使用者操作或明確授權，不為了測試自行改手機全域設定。
- 失聯／重連不應重放舊意圖；涉及真實控制或斷網的驗收另按當次授權和安全狀態進行。主機已涵蓋排隊撤銷，不為測它而臨時移動舵機。

驗收記錄安裝版本、時間與結果。CameraX、GPS／Wi-Fi、API、實際機械動作分開記，不能把 build 當成通過。

## 3. Station／Client 更新是另外兩件事

| 更新範圍 | 會生效的修正 | 尚未更新時的限制 |
| --- | --- | --- |
| App 0.2 | GPS-only、失聯撤銷舊操作、錄影回前景恢復 | 不會修正韌體 DATA 或增加 SD 記錄 |
| LilyGO Station | 正確板載 freshness、實際選用岸端 `station_position` | 舊 SD 無法證明當時真正使用的手機座標 |
| T096 Client | 超出速度範圍保留有效位置、編碼故障有界可見 | 舊韌體仍有該邊界缺陷 |

若使用者要求兩端更新：重新核對板型／USB serial／boot／現有設定與回復產物，再使用本輪 manifest 對應檔案。
Station 產物使用本機私人 Wi-Fi 設定，不能因有 build 成功就假定與現場網路一致。
Station 重啟會輸出中位命令，先處理舵機電源／機構安全；T096 僅更新 application，保留 bootloader／SoftDevice。
部署後讀回新 boot／build、配對／owner／頻道與速限；不自動發送動作。新 SD 快照需實際讀回 `station_position` 才算生效。

## 4. 外測分段與再現時留證據

基本路徑：`手機衛星 GPS → Station 岸端位置` ＋ `Surfer GNSS → LoRa DATA → 方位 → 共用限速 → UART → 控制板 → 舵機`。
先重跑第一次正常流程，再分段測停止錄影、切 App、重連；本輪不混入 Axiom 刪除、頻道／追蹤門檻或大型重構。

- 若跳角：記時間與當時操作，保留同段原始 RF、實際岸端來源／座標／年齡、Surfer DATA、目標與命令。未部署新 Station 時要明說缺少岸端快照。
- 若失聯：區分 App／HTTP、Station 是否重開，以及 Client RF；回家後另一個 boot 的計數不能代替當時證據。
- 若手動不動：先分清 API 抵達、UART sent／deferred／errors 與實體接收；沒有回授不宣稱 Servo 已到位。
- 若重開／panic：記錄發生時間、操作與新 boot，先保留／匯出 SD／DIAG，不先重刷清資料。需配對當時 ELF 與 crash backtrace 才能定位；本輪未加入持久化 coredump，無筆電現場可能仍取不到 backtrace，必要時回主機控制條件重現並捕捉 USB 輸出。

使用者不需在海邊操作診斷工具；先記時間／操作並保留裝置資料，回來由 agent 匯出分析。

## 5. 後續與交接

先完成本輪安裝／部署與外測，不把「沒有再現」寫成已找到 panic 根因。
每完成一段更新交接的版本／hash／boot／實測結果；有紀錄就依固定 commit＋hash 索引保存並驗證可取回。
尚待決定：Axiom 是否需要、舊 profile／入口用途。SD 與 Axiom 共用快照／encoder，移除上傳時保留本地記錄。
CAD 幾何／STL 一致性、RF 長時間與機構驗收仍是獨立待辦；本輪沒有擴張這些範圍。
