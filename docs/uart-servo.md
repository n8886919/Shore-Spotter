# GPS / UART 獨立控制

正式映像為 `tbeam-server`。開機 Servo 置中 90°，初始化完成後預設 UART，等待新的完整 SET。
網頁選「手動／GPS／UART」，只有所選模式能控制 Servo，訊號變差不切換到另一模式。
UART 模式可由 Jetson 或其他 3.3 V TTL 控制端使用。Wi-Fi 提供監控頁、手動 HTTP
控制與 OTA；外部 UDP 控制已移除。

| 模式 | 行為 | 訊號失效 |
|---|---|---|
| 手動 | slider 控制；GPS／UART 均不自動移動 | 保持最後輸出角度 |
| GPS | 使用單一綁定 client 的位置與鏡頭指南針校正 | 保持，不切 UART |
| UART | 僅使用 UART SET，不要求 GPS 或指南針校正 | SET 過期後保持，不切 GPS |

切換模式會清除舊目標、關閉舊 UART session。只在 UART 模式開啟 UART；重新進入後
需收到新的完整 SET 才移動，重複選取相同模式不重置。保持是維持最後 PWM 角度，
不會回 90°或關閉 Servo 電源。Manual、pause、OTA、關機與 PWM 故障會撤銷自動控制。

## UART 接線與指令

UART2，115200 8N1；GPS 使用另一組 UART1（RX9/TX8）。

- USB-to-TTL TX → LilyGo GPIO44 RX
- USB-to-TTL RX → LilyGo GPIO43 TX
- GND → GND；3.3 V TTL；各自供電時不接 adapter VCC/5V

只接受每行一個 ASCII 指令，以 LF 或 CRLF 結束：

```text
SET 90000
```

Server 開機預設 UART，初始化完成後等待新 SET；資訊頁速限 1–90°/s，修改自動保存。
單位毫度，範圍 0..180000。建議持續 20 Hz；不再需要 ARM，也不再接受 STOP。
停送 SET 後，最後有效指令滿 250 ms 便撤銷 UART 目標。新 SET 可直接恢復，第一步
從最後輸出角度開始；預設限速 30°/s；所有模式按微秒實際時間推進，只共用最高速度限制。
預設速限下，持續有效指令的 90° → 150° 命令約需 2 秒；沒有額外加減速規劃。角度限制、watchdog 與 PWM 故障保護仍在 ESP32。
開機首次歸中 90° 沒有實際位置回饋，無法保證該次動作的物理速度。

無效、過長、含 NUL／符號／雜字的命令不刷新期限；未完成的一行也有 250 ms 期限。
主 loop 停頓 ≥250 ms 後丟棄已排隊的 bytes，並等到下一個換行才接受新完整命令，
避免把緩衝截斷的後半句當新資料。恢復時因此可能略過第一行 SET。
250 ms 由主 loop 檢查；它不是獨立硬體中斷保證。

## GPS 與鏡頭校正

GPS 按鈕與 API 要求兩端 Good／OK：衛星 ≥6、HDOP ≤3、位置／品質未滿 2 秒；
Bad／Miss 不可選。追蹤另需鏡頭校正、有效磁偏角與條件穩定 2 秒；失效保持，不啟用 UART。

資訊頁新增 GPS 位置預測 α 開關（未發行）：開啟沿速度方向外推，關閉使用最後收到位置；
預設開啟、自動保存。兩者仍共用 Servo 速限，切換不更改模式或校正；UART 不使用此設定。
詳見 [GPS 預測說明](motion-control.md#gps-位置預測開關α未發行)。

切到手動，固定腳架，輸入鏡頭上普通磁針指南針角度（北 0°、東 90°）。校正本身不需
GPS 或地標；RAM 參考在 start/resume／模式切換間保留，重開機必須重校。
只使用這個參考，沒有板上 QMC 航向補償；腳架轉動、重新架設或移動指南針後請重校。

```text
mount_offset = compass_bearing + servo_angle
true_camera_bearing = mount_offset + declination - servo_angle
```

磁偏角東正西負，依攝影站 GPS 位置與 UTC 日期使用 WMM2025 離線表，適用北緯 18–28°、
東經 116–124°、2025–2029 年、海平面。位置／日期無效或超出模型範圍時，GPS 保持。
UART 模式完全不依賴此校正。來源與更新方式見 `tools/make_declination.py`。

## ACK 與阻塞診斷

ACK 非阻塞 startTransmit → TxDone／timeout → RX；處理太晚（RxDone 後 >50 ms）的 ACK
略過、不排隊。主迴圈在 HTTP／感測器／OLED／LoRa 工作之間服務控制，所有模式運動輸出不限頻，GPS 目標重算保持 20 Hz，OLED 分段刷新。

Wire 與 PMUWire 每筆交易 timeout 10 ms（原預設 50 ms）。單次裝置操作可能含多筆交易，
因此 10 ms 不是整段感測器操作的上限。移除板上磁力計後，不再有其週期 I2C 取樣。
WebServer 仍是同步實作；本批沒有把它改成非同步，也不保證服務間隔一定小於 250 ms。

`GET /api/status` 的 `timing` 保存最大控制服務間隔、HTTP／BME280／PMU／OLED／LoRa／OTA
與主 loop 的耗時、超時服務與 UART 丟棄／拒絕計數；不逐次寫 log。數值自開機累計，
HTTP 與 loop 的本次耗時在返回後才更新，讀到的可能是上一筆。先在實機確認主要阻塞來源，
若仍超過控制期限，再評估有界 HTTP 接收／傳送或獨立控制 task。

## 單一 GPS client 與相容性

GPS 綁定在 NVS 保存一個 `gpsclient`（0 = 未綁定）。`/api/whitelist` 容量 1；set 明確替換，
add 不覆寫不同 ID。成功變更後清空舊 client 狀態並回手動。
舊多筆 `wl` 只遷移第一個有效 ID；刻意清空保持清空。
鏡頭與板上校正都不讀寫 NVS；舊 mount*／mag* 鍵留存但忽略，不做整區清除。

`POST /api/servo/mode?mode=manual|gps|uart` 是模式入口；舊 mode=auto 回 400。
舊 `mode=jetson` 保留為 UART 的輸入別名，所有模式回應與 status／track 統一回 `uart`。
HTTP `/api/track/stop` 與 `/api/mag/calibrate` 已移除，回 404。
`/api/track/start` 選 GPS；`/api/track/resume` 恢復上次選取的 GPS／UART（開機預設 UART）；
`/api/track/pause` 暫停；要手動調整請先明確選 manual。舊 UART 程式若依賴 ARM／STOP，應同步改用 SET
與停送；本專案不會修改或部署外部控制端程式。

## 共用後端與設定（2026-09-13，0.6-dev）

所有來源只共用預設 30°/s 速限；資訊頁可調 1–90°/s，修改自動保存，重開機沿用。
加速度、jerk、死區與頻率調參已移除，詳見 [共用控制器](motion-control.md)。
資訊頁提供 GPS 預測 α 開關；唯讀「除錯」頁可錄製並匯出資料，見 [操作指南](debug-export.md)。
HTTP 控制需要 epoch／seq／stamp，模式 API 保留 jetson 別名。GPS 的 mode／start／resume
入口都檢查兩端 Good／OK；拒絕時保留來源。舊 SET 保留，SYNC／SET2 可檢查 session、
序號與期限；接受 SET2 後該 session 不再接受裸 SET。指南針校正仍只存 RAM。
UART 契約保留；LoRa 已改為 v4／17-byte DATA，Client、Server 必須一起更新才能互通。
