# 共用 Servo 控制器

更新：2026-09-13，0.6-dev／LoRa v4，含 GPS 預測開關；Server 已 OTA、Client 已 USB
燒錄。v4 上行收包與 Server ACK 發送已實測；Client ACK 接收、戶外定位與機械效果
仍待驗，並發現一次 HTTP 停頓，詳見 [通訊測試](link-test-2026-09-13.md)。

手動 HTTP、GPS 與 UART 只提供最新目標；`servo_motion::Controller` 是唯一運行中的
限速器，`serviceControl()` 是唯一運行中的 PWM 輸出路徑。開機仍先輸出 90°，完成
初始化後預設 UART，等待新的完整指令。首次歸中沒有起始機械位置回饋，不保證物理限速。

## 共用運動設定：最高速度

- 預設 **30°/秒**，可調 **1–90°/秒**，所有模式共用且永遠啟用。
- 首頁「資訊」頁修改數值，離開欄位／按 Enter 後自動儲存；沒有套用、另存或啟用開關。
- 手動、GPS、UART 及移動中都能改速限；保留既有模式、位置與最新目標，下一次服務使用新值。
- 加速度、減速度、jerk、deadband 與相關規劃器全部移除；小目標不再因 1° 死區而忽略。
- 每次以微秒實際經過時間決定最大位移，不設固定 Servo 更新頻率、不排隊補送歷史目標。
  長停頓單次最多採計 50 ms，超出時間不積存；反向指令直接以共用速限朝新目標移動。
- PWM 保留小數微秒到最後轉成 14-bit duty，只有 duty 改變才寫入。固定命令範圍 0–180°，
  PWM 333 Hz。這些電氣／時間常數不是使用者調參項目。

來源失效、模式撤銷、OTA、關機或 PWM 故障會保持位置；這是命令位置而非實體回饋。
主迴圈中的同步 HTTP／I2C 仍可能拖延服務；未承諾實體抖動已消除。

## 設定套用與保存

`GET /api/servo/settings` 回傳 `speed`、`min_speed=1`、`max_speed=90`、`default_speed=30`
及控制上下文。`POST /api/servo/settings?speed=<值>` 自動儲存並生效，同樣要求 epoch／seq／stamp。

保存使用 `shorespotter/servospd` 單一 uint32 NVS 值，單位毫度／秒。寫入後回讀確認，成功
才變更運行中的速限；重複相同已保存值不重寫。400 表示非法數字／超界／舊 action 或多餘參數，
409 表示控制上下文過期，503 表示保存失敗、RAM 速限保留原值。

首次使用、NVS 無有效值時預設 30；之後開機讀回上次保存值。舊 `motioncfg` v1/v2
設定完全忽略，不帶入先前的 90°/秒、加速度、jerk 或死區。指南針參考仍只存 RAM，
`gpsclient` 與 Client ATPC 保存互不影響。降回 0.4 會讀舊設定鍵，不讀本版速度鍵。

## GPS 選擇條件與模式

開機預設 **UART**，三種來源互斥、不自動交接。

| 來源 | 有效性 |
|---|---|
| 手動 | 有效 HTTP 目標持續保持，直到抵達／被新目標取代／模式撤銷 |
| UART SET | 收到完整指令起算 250 ms；逾期保持，新的完整 SET 可恢復 |
| UART SET2 | 依指定板上時間起算 250 ms，並檢查 session 與遞增序號 |
| GPS | 兩端均為 Good 或 OK、資料未滿 2 秒；已校正並有有效磁偏角，條件連續穩定 2 秒後追蹤 |

GPS 按鈕只在 `servo.gps_available=true` 時可點。Server／Client 任一 Bad、Miss、
位置或品質資料過期／缺漏即為 false；Good 為 fix、衛星 ≥8、HDOP ≤1.5，OK 為 fix、
衛星 ≥6、HDOP ≤3。`/api/servo/mode?mode=gps`、`/api/track/start`、恢復 GPS 的
`/api/track/resume` 都在執行當下再次檢查；不合格回 409，保留原模式、來源與目標。

按鈕可用不表示已校正。指南針校正仍需先切手動並等命令停止；`gps_usable` 另要求校正
與磁偏角，`gps_ready` 表示已通過穩定期。GPS 中品質失效時保持，不自動回 UART。
GPS 位置外推目標每 50 ms 重算，與 Servo 限速器服務獨立；重算不刷新封包年齡。

## GPS 位置預測開關（α，未發行）

資訊頁提供二態開關，預設開啟並自動保存：

- 開啟 α=1：沿有效 GPS 速度與方向，以來源 age＋RF airtime＋接收後時間外推；
  同 epoch 的速度向量有效且至少 0.3 m/s 才外推，時間最多 2 秒。
- 關閉 α=0：GPS 目標使用最後收到的座標。Servo 仍按共用速限朝目標移動，並非立即停止。

α 調整的是位置估計，不是 PID 的 P/D 比例或 Servo 速度上限。只有 GPS 來源會使用；
切換保留模式、校正、速限與來源有效性門檻，下次正常 GPS 目標重算時生效。
可在其他模式預先保存；不會啟動 GPS，也不會讓失效的位置恢復追蹤資格。

`GET /api/track/prediction` 讀取，`POST /api/track/prediction?enabled=0|1` 保存，
POST 同樣要求 epoch／seq／stamp。NVS 使用 `shorespotter/gpspredict` uint32（0／1）；
寫入並回讀成功才套用，重複相同已保存值不重寫，缺值或非法值預設開啟。
400 為非法參數，409 為上下文失效，503 為保存失敗並保留運行值；網頁錯誤時回復已確認狀態。

v4 傳送來源 age 與獨立 `velocity_valid`。Client／Server 用同 epoch 的 RMC／GGA
快照，重複 epoch 不刷新資料年齡；超過 200 ms 未服務 GPS 時丟棄 UART 積壓並撤銷快照。
來源 age 依 NMEA epoch 對齊到達時間估計，沒有量測 GNSS 模組內部延遲。
追蹤另保留 200 ms 年齡不確定度：Client 的來源 age＋airtime＋接收後時間＋200 ms
必須未滿 2 秒；Server 也檢查本地快照 age＋200 ms。200 ms 不加入外推位移。
速度未知、超出 25.4 m/s 或方向無效時，仍可使用有效位置，但不外推。
封包與年齡定義見 [interface.md](interface.md)。

## UI 角度與放開送出

滑桿由左到右 0–180°，對應內部 180–0°：`raw = 180 - ui`。目前角度、目標文字同步；
HTTP／UART／GPS 內部方向、指南針北 0°／東 90° 保持不變。
拖動只預覽，放開（range change）才送出。最多一筆 HTTP 在途，待送只保留最新已放開
目標；pointercancel 不送出。模式切換取消未送目標並等待在途請求完成。

加速度／jerk／死區／頻率與 OLED 調參介面均移除。OLED 正常分段刷新，不提供暫停開關。
0.6-dev 的「除錯」頁唯讀顯示封包、GPS 與控制統計，並可錄製、匯出 JSON；
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
# 回覆 SESSION <epoch> <last_seq> <server_ms>
SET2 <epoch> <seq> <stamp> <angle_mdeg>
```

SYNC 不刷新目標有效期；STAMP 使用板上時間且未滿 250 ms。若以 SYNC 作時間基準，
需在 SYNC 後取得新目標，不能把舊影像結果標成新資料。session 接受 SET2 後拒絕裸 SET，
直到重新進入 UART。Legacy SET 無發送端時間資訊，不能保證來源端資料年齡。

LoRa 改為 v4，Client／Server 必須一起更新，舊 v3 拒收。DATA／TEL／DIAG 各用獨立序號，
連續通聯拒絕重複／倒退 DATA seq；uint16 回繞可接受。失聯滿 2500 ms 後需兩筆前進、
相隔 250–1999 ms 的候選資料重建序號基準。來源 age 並非跨裝置同步時鐘；
封包不含 Client 開機 session，GNSS 模組內部延遲仍未量測。

## 驗證入口

```sh
pio test -e native
node tools/test_servo_slider.js
node tools/test_motion_ui.js
python3 tools/test_motion_backend.py
```

驗證狀態見 [健檢紀錄](health-check-2026-09-08.md)。
