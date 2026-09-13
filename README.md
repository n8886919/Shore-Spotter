# Shore Spotter - Surf Tracking System🏄🏄🏄 

目前原始碼版本：**0.6-dev（未發行）**。版本唯一來源為 [firmware_version.h](include/firmware_version.h)，
發版紀錄見 [CHANGELOG.md](CHANGELOG.md)。Client／Server 共用版本號，開機 OLED 與
序列紀錄顯示版本；Server 網頁「資訊 → 站專屬」及 `/api/status` 的
`health.firmware_version` 顯示板上實際版本。舊 Client 不會因 Server 更新而自動更新。

本版使用 LoRa v4：位置封包 **17 bytes**、每 **500 ms** 排程發送，保留約 0.1 m
座標量化、速度／方向及來源資料年齡。**與 v3 不相容，Client、Server 必須一起更新。**
RF 發送 2 Hz 不代表 GPS 每秒有兩筆新定位；網頁「除錯」可分開觀察接收與 GNSS 更新間隔，
開始錄製後匯出 JSON 給 AI 檢查，操作見 [現場除錯與匯出](docs/debug-export.md)。
除錯已改用 schema 2 增量事件與全頁共用請求排程；Server 更新後請重新整理網頁。

![alt text](image.png)
 > **衝浪攝影不求人**

 > **解放浪人的女友與衝浪教練**

 > **不需帶手機下水**

## 系統架構
### Surfer端
Surfer帶著搭載GPS與傳輸模組的追蹤器持續向岸上攝影站發送座標資料

### 攝影站端
將攝影設備裝在伺服雲台上，攝影站會接收並計算Surfer方位，控制雲台實現自動追蹤

LCD顯示: 連線狀態 / 電量 / 濕度 / GPS狀態 / 監控頁面IP

手機開啟個人熱點，攝影站開機自動連線（熱點 SSID/密碼設定於 `include/wifi_config.h`），再用手機瀏覽器輸入攝影站 IP（開機時 OLED 顯示）即可進入監控頁面

## 怎麼分 client / server
這個專案不是用序列埠來分角色，而是用 `platformio.ini` 裡的 PlatformIO environment 來分：

+ `tbeam-client` = Surfer 端追蹤器，對應 `ROLE_CLIENT`
+ `tbeam-server` = 岸邊攝影站，對應 `ROLE_SERVER`

燒錄哪塊板、走哪個 `upload_port`，看的是你執行 `pio run -e tbeam-client` 還是 `-e tbeam-server`（或 VS Code 下方切換 environment），跟埠號本身無關。

`upload_port` / `monitor_port` 依作業系統指定，設定檔保留 Windows 與 Linux 範例。
先用 `pio device list` 核對 USB 序號／晶片 MAC，再依開機紀錄確認角色；埠號會隨插拔改變，
不能只用 ACM0／ACM1 判定。上傳時可用 `--upload-port <實際埠號>` 覆蓋設定。

## 跑測試

指向計算（角度環繞、方位角、保留的圓擬合數學）與現場提醒的門檻都抽在
`include/geo_math.h` / `include/alerts.h`；GPS/UART 模式隔離與控制核心放在
`include/tracking_policy.h` / `include/servo_motion.h`，都不依賴 Arduino，可在筆電驗證：

```bash
pio test -e native
node tools/test_servo_slider.js
node tools/test_motion_ui.js
node tools/test_debug_ui.js
python3 tools/test_motion_backend.py
python3 tools/test_packet_backend.py
python3 tools/test_debug_backend.py
```

native 測試涵蓋 GPS／UART 模式隔離、SET 解析、watchdog、ACK、client 綁定、磁偏角與
既有數學／提醒，以及 v4 codec、同 epoch GNSS 與無線排程；Node 測試執行實際頁面，
模擬時鐘與 HTTP 回覆；Python 工具擷取實際 C++ 後端驗證控制、封包與除錯 JSON。
實機驗證結果與待處理問題見 [最新健檢紀錄](docs/health-check-2026-09-08.md)。

## Server 透過 Wi-Fi 更新（PlatformIO espota）

第一次必須用 `tbeam-server` 經 USB 燒錄，讓板子取得 OTA 功能。之後電腦與 Server 連在同一個手機熱點時，可使用 `tbeam-server-ota`：

```bash
pio run -e tbeam-server-ota -t upload
```

預設使用 `platformio.ini` 的 `upload_port`。現場 IP 有變時，以 OLED 顯示的 IP 覆蓋：

```bash
pio run -e tbeam-server-ota -t upload --upload-port <SERVER_IP>
```

更新期間請維持供電與 Wi-Fi 穩定。OTA 開始時會暫停自動追蹤，成功後 Server 自動重開；USB 燒錄仍保留作為救援方式。OTA 本身未設密碼，任何連上同一熱點且能連到 Server 的裝置都能送出韌體，因此不要讓不受信任的裝置加入熱點。監控頁 API 與 LoRa 封包也是同樣的取捨（都刻意沒做存取控制），完整說明見 [interface.md 的存取控制一節](docs/interface.md#監控頁與-ota-的存取控制)。

## GPS / UART 獨立模式與指南針校正

`tbeam-server` 開機 Servo 90°、預設 UART 等待新指令。網頁選「手動／GPS／UART」；
GPS 與 UART 控制互斥，來源失效就保持，不會自動切到另一模式。
GPS 只在兩端 Good／OK 且資料未滿 2 秒時允許選取；追蹤另需校正與連續穩定 2 秒；UART 只需持續 UART `SET`，
不需要 ARM，也不再接受 STOP。250 ms 無有效 SET 則保持，新指令可直接恢復。

GPS／UART／手動共用微秒實際時間軌跡，Servo 更新不限頻；GPS 目標重算維持 20 Hz；滑桿拖動只預覽，放開才送出。
UI 左 0°、右 180° 對應內部 180°、0°，HTTP／UART 角度協定不變。
所有模式只共用一個最高速度限制，預設 **30°/秒**。「資訊」頁可調 **1–90°/秒**，
修改後自動儲存，重開機讀回上次值；沒有加速度、jerk、死區或頻率調參。
資訊頁另有「GPS 位置預測（α）」開關；「除錯」頁提供唯讀診斷與錄製匯出。
HTTP 控制仍需世代／序號／期限；升級後請重整網頁。
完整操作見 [共用控制器](docs/motion-control.md)。開機首次回 90° 無位置回饋，無法保證平滑速度。

GPS 鏡頭校正只用普通磁針讀數（北 0°、東 90°），只存 RAM；校正時不需 GPS fix。
板上磁力計與 hard-iron 已停用，不讀寫校正 NVS。腳架轉動或重開機後必須重校。
台灣磁偏角依攝影站 GPS 位置與日期自動補償，WMM2025 適用 2025–2029 年。
UART 不受 GPS 品質或校正狀態影響。

只綁定一個 GPS client，保留共用封包與 ClientState 供未來擴充。
ACK 非阻塞，傳送期間繼續服務控制；HTTP／感測器耗時與最大服務間隔可由
`/api/status` 的 `timing` 查看，不增加逐次 log。I2C 每筆交易 timeout 為 10 ms。

HTTP track/stop 與板上校正 API 已移除；切到手動可停止全部自動追蹤。
UART 控制端（例如 Jetson）走 USB-to-TTL GPIO UART；UDP 控制已移除，Wi-Fi 網頁與 OTA 保留。
模式 API 使用 `mode=uart`；舊 `mode=jetson` 仍可輸入，但回應統一為 `uart`。
接線、操作與相容性見 [docs/uart-servo.md](docs/uart-servo.md)。

## 為什麼選 LoRa？
衝浪環境對無線通訊有幾個特殊條件：距離遠、無遮蔽物、不適合攜帶手機。LoRa 在這個場景下的優勢在於低功耗與長距離，更重要的是它讓「下水端」的職責單純：只負責定位與傳輸，不需要維護網路連線。

相比藍牙或 WiFi，LoRa 在開放水域場域的穿透性與覆蓋距離更有優勢；相比帶手機或智慧手錶，這套設備的任務邊界更清晰，也更好做防水設計。

[LoRa 封包與攝影站 API](docs/interface.md)\
[韌體功能說明](docs/features.md)

## 未來目標
+ 多Surfer追蹤
+ Auto Record\
  結合GPS路徑或影像分析，偵測「追浪」與「起程」事件，讓錄影自動觸發，不再依賴手動操作。

+ 純影像追蹤，不需要攜帶追蹤器\
  岸上相機透過影像追蹤自動鎖定目標。從「GPS 輔助追蹤」演進為「完全被動」的使用體驗——這也是整個系統最終想到達的地方。
