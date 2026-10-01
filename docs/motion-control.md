# 共用 Servo 控制器

更新：2026-09-19，0.6-dev／LoRa v4；最新原始碼新增 GPS 失效後轉完最後目標與 3 秒序號重建，尚未燒錄。
本文件描述原始碼行為，建置／部署狀態見 [CHANGELOG](../CHANGELOG.md)；
先前 v4 收包、ACK 與 HTTP 停頓證據見 [2026-09-13 通訊測試](link-test-2026-09-13.md)。

手動 HTTP、GPS 與 UART 只提供最新目標；`servo_motion::Controller` 是唯一運行中的
限速器，`serviceControl()` 是唯一運行中的 PWM 輸出路徑。開機仍先輸出 90°，完成
初始化後預設手動，GPS／UART 由使用者切換啟用。首次歸中沒有起始機械位置回饋，不保證物理限速。

## 共用運動設定：最高速度

- 預設 **30°/秒**，可調 **1–90°/秒**，所有模式共用且永遠啟用。
- 首頁「資訊」頁修改數值，離開欄位／按 Enter 後自動儲存；沒有套用、另存或啟用開關。
- 手動、GPS、UART 及移動中都能改速限；保留既有模式、位置與最新目標，下一次服務使用新值。
- 加速度、減速度、jerk、deadband 與相關規劃器全部移除；小目標不再因 1° 死區而忽略。
- 每次以微秒實際經過時間決定最大位移，不設固定 Servo 更新頻率、不排隊補送歷史目標。
  長停頓單次最多採計 50 ms，超出時間不積存；反向指令直接以共用速限朝新目標移動。
- PWM 保留小數微秒到最後轉成 14-bit duty，只有 duty 改變才寫入。固定命令範圍 0–180°，
  PWM 333 Hz。這些電氣／時間常數不是使用者調參項目。

GPS 來源失效後凍結最後有效目標，按速限轉完後保持，不繼續外推；UART 過期、模式撤銷、
手動暫停、OTA、關機或 PWM 故障仍立即撤銷移動。這是命令位置而非實體回饋。
主迴圈中的同步 HTTP／I2C 仍可能拖延服務；未承諾實體抖動已消除。

## 設定套用與保存

`GET /api/servo/settings` 回傳 `speed`、`min_speed=1`、`max_speed=90`、`default_speed=30`
及控制上下文。`POST /api/servo/settings?speed=<值>` 自動儲存並生效，同樣要求 epoch／seq／stamp。

保存使用 `shorespotter/servospd` 單一 uint32 NVS 值，單位毫度／秒。寫入後回讀確認，成功
才變更運行中的速限；重複相同已保存值不重寫。400 表示非法數字／超界／舊 action 或多餘參數，
409 表示控制上下文過期，503 表示保存失敗、RAM 速限保留原值。

首次使用、NVS 無有效值時預設 30；之後開機讀回上次保存值。舊 `motioncfg` v1/v2
設定完全忽略，不帶入先前的 90°/秒、加速度、jerk 或死區。指南針參考仍只存 RAM，
`gpsclient` 保存不受影響。Client 舊 `txpwr`／`atpc` 鍵已停用，保留但不讀寫。
降回 0.4 會讀舊設定鍵，不讀本版速度鍵。

## GPS 選擇條件與模式

開機預設 **手動**，三種來源互斥、不自動交接。

| 來源 | 有效性 |
|---|---|
| 手動 | 有效 HTTP 目標持續保持，直到抵達／被新目標取代／模式撤銷 |
| UART SET | 收到完整指令起算 250 ms；逾期保持，新的完整 SET 可恢復 |
| UART SET2 | 依指定板上時間起算 250 ms，並檢查 session 與遞增序號 |
| GPS | 兩端定位有效、本地新 epoch／遠端新 DATA 未滿 2 秒；已校正並有有效磁偏角，條件恢復即於下個控制週期追蹤 |

GPS 按鈕只在 `servo.gps_available=true` 時可點。兩端都需有效且持續更新的位置，
品質差／未知仍可直接追蹤並警告。`/api/servo/mode`、`/api/track/start`、`/api/track/resume`
執行時重查定位；明確失效／逾時回 409，保留原模式、來源與目標。

指南針校正僅接受命令位置與目標均為 90°、且已停止命令移動的狀態。`servo.calibration_ready`
回報此資格；非 90°／移動中點擊會通知，後端送達時再次檢查，失敗回 409 並保留原參考。
「回到 90°」使用 `POST /api/servo/center`，一次切為手動、撤銷 GPS／UART／待完成目標，再按
共用速限回中；不清除校正、不自動恢復追蹤。前端取消未送滑桿命令並等待在途請求結束。
校正不需 GPS；角度為軟體命令，仍需確認鏡頭及指南針實際停穩。`gps_usable` 另要求校正
與磁偏角；`gps_ready` 表示目前在 GPS 模式、PWM 正常且 GPS 使用條件成立。
沒有額外恢復等待；GPS 定位無效／更新逾時時不再重算目標，轉完最後有效目標後停住，不自動回 UART。
`servo.finishing_last_gps_target` 可區分轉完舊目標與正在使用新定位；恢復後正常新目標直接取代它。
GPS 位置外推目標每 50 ms 重算，與 Servo 限速器服務獨立；重算不刷新封包年齡。

## GPS 位置預測開關（α，未發行）

資訊頁提供二態開關，預設關閉並自動保存：

- 開啟 α=1：沿有效 GPS 速度與方向，僅以接收後經過時間外推；
  兩端衛星 ≥6、HDOP ≤3 且同 epoch 速度有效、至少 0.3 m/s 才外推，時間最多 2 秒。
- 關閉 α=0：GPS 目標使用最後收到的座標。Servo 仍按共用速限朝目標移動，並非立即停止。

α 調整的是位置估計，不是 PID 的 P/D 比例或 Servo 速度上限。只有 GPS 來源會使用；
切換保留模式、校正、速限與來源有效性門檻，下次正常 GPS 目標重算時生效。
可在其他模式預先保存；不會啟動 GPS，也不會讓失效的位置恢復追蹤資格。

`GET /api/track/prediction` 讀取，`POST /api/track/prediction?enabled=0|1` 保存，
POST 同樣要求 epoch／seq／stamp。NVS 使用 `shorespotter/gpspred5` uint32（0／1）；
寫入並回讀成功才套用，重複相同已保存值不重寫，缺值或非法值預設關閉；舊 gpspredict 鍵保留但不沿用，讓 v5 首次更新採 α=0。
400 為非法參數，409 為上下文失效，503 為保存失敗並保留運行值；網頁錯誤時回復已確認狀態。

v5 DATA 不傳來源 age；Client 本地及 Station 本地以新 epoch 首次到達起算，遠端以 DATA 接收後起算，
均採 2 秒更新逾時；重複不刷新期限。GNSS 推算年齡只診斷，不影響追蹤。
品質差／未知或向量無效時仍直接追有效位置，`prediction_active=false`，不把原速度改成 0。
品質恢復時只能用新的同 epoch 速度，α=0 不自動開啟。`prediction_enabled` 仍是使用者設定；
`prediction_active` 另要求 GPS 模式、控制資格、雙端品質與速度有效。
UART 積壓 >200 ms 仍丟棄；此上限不再加進位置年齡。封包見 [interface.md](interface.md)。

## UI 角度與放開送出

雷達最外圈的 180° 軌道控制 UI 0–180°，對應內部 180–0°：`raw = 180 - ui`。
手動模式顯示軌道、刻度與把手；GPS／UART 隱藏軌道及刻度，保留不可拖曳的圓形角度標記，
依 `servo.angle` 顯示目前命令角度（不是實體回授）。地圖上也能手動操作。
控制刻度固定在畫面上半圓：左 0°、上 90°、右 180°，不隨校正或磁偏角旋轉。
這是 Servo 控制角度而非地理方位；雷達波束仍按 `mount_offset + declination - raw` 計算。
HTTP／UART／GPS 內部方向、指南針北 0°／東 90° 保持不變。
拖動只預覽，放開（pointerup；鍵盤保留原生 range change）才送出。最多一筆 HTTP 在途，待送只保留最新已放開
目標；pointercancel／失去 capture／第二觸點／切視圖取消未完成拖曳。模式切換取消未送目標並等待在途請求完成。

加速度／jerk／死區／頻率與 OLED 調參介面均移除。OLED 正常分段刷新，不提供暫停開關。
網頁除錯頁已移除；SD／USB 及 HTTP API 提供封包、GPS 與控制統計，
見 [除錯操作](debug-export.md)。舊 `/api/servo/diagnostics` 與 `timing.control_elapsed` 仍已移除。

## HTTP 世代、序號與期限

所有 Servo 設定／模式／角度及 track start/resume/pause/calibrate/prediction 的 POST 都要求：

| 參數 | 來源 |
|---|---|
| epoch | 最新 `servo.control_epoch` |
| seq | 大於該世代已接受的 `servo.command_seq`，uint32 wrap 比較 |
| stamp | 最新 status／track 的 `servo.clock_ms`，ESP32 millis |

距 stamp 滿 2000 ms、時間在未來、舊世代或重複／倒退序號回 409。驗證通過即消耗序號，
即使後續參數或模式檢查失敗。模式切換更新世代，修改速限或 α 不撤銷來源／目標或重設世代。
網頁自動處理上下文；升級後必須重整。這些欄位不是網路認證。

## UART 與 LoRa 相容

接線、baud、`SET <angle_mdeg>\n`、SYNC／SET2 契約不變：

```text
SYNC
# 回覆 SESSION <epoch> <last_seq> <station_ms>
SET2 <epoch> <seq> <stamp> <angle_mdeg>
```

SYNC 不刷新目標有效期；STAMP 使用板上時間且未滿 250 ms。若以 SYNC 作時間基準，
需在 SYNC 後取得新目標，不能把舊影像結果標成新資料。session 接受 SET2 後拒絕裸 SET，
直到重新進入 UART。Legacy SET 無發送端時間資訊，不能保證來源端資料年齡。

LoRa 改為 v4，Client／Station 必須一起更新，舊 v3 拒收。DATA／TEL／DIAG 各用獨立序號，
連續通聯拒絕重複／倒退 DATA seq；uint16 回繞可接受。距最後接受 DATA 滿 3000 ms 後，第一包 fix=1 位置重建序號基準並明確記錄原因；
fix=0 不重建。此簡化規則不區分重啟、失聯或延遲舊包；沒有 boot ID／握手。來源 age 並非跨裝置同步時鐘；
封包不含 Client 開機 session，GNSS 模組內部延遲仍未量測。

## 驗證入口

```sh
pio test -e native
node tools/test_servo_slider.js
node tools/test_servo_ring.js
node tools/test_motion_ui.js
python3 tools/test_motion_backend.py
```

驗證狀態見 [健檢紀錄](health-check-2026-09-08.md)。

開機校正偏移預設 90°並視為有效；資訊頁可重新校正，重開機恢復 90°。
