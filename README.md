# Shore Spotter - Surf Tracking System 🏄

目前原始碼為 **1.0（診斷專用建置 1.0-debug），LoRa wire v5、BLE wire v1**；版本唯一來源為
[firmware_version.h](include/firmware_version.h)，變更見 [CHANGELOG](CHANGELOG.md)。
其他工程師先讀 [交接入口](docs/assistant-handoff.md) 與 [功能入口](docs/function-entrypoints.md)。
截至 2026-10-28，最新改動仍含未提交與未追蹤檔；只取 HEAD 會漏掉 BLE、維護介面與相關文件。

## 現行流程（2026-10-28）

- 使用中的 Client 為 Heltec T096；LilyGO T-Beam Supreme 作 Station。舊 T-Beam Client 與診斷 profile
  仍保留於原始碼，不能把它們的 PMU／SD／按鍵說明套在 T096。
- T096 開機進 Ready：GPS 關閉、LoRa 休眠，未連線時 BLE 約每 10 秒可連線廣播。
  Station 自動搜尋已指定 Client，第一個有效 BLE Claim 取得本次開機 owner。
  Stop、斷線、Station 重開不解除 owner；換 Station 前先停用舊 Station 搜尋，再重啟 Client。
- Start 開 GPS／LoRa；Test 只開 LoRa；Stop 回 Ready。Ready **沒有定期 LoRa STATE 或接收窗口**。
  HTTP 202 只是受理；Start 要收到匹配 LoRa STATE 才確認。Web／未來 App 都經 Wi-Fi HTTP 控制 Station。
- 候選 A 923.2 MHz、B 923.8 MHz **皆 SF9／BW125／CR4/5**；Ready 且 BLE 已認領時可手動 CAD 掃描及選擇。
  選擇先存 RAM，Start 的匹配 LoRa STATE 驗證後才保存 Station 頻道偏好。
  舊 manifest 的 A/SF10 與現行 runtime 候選是不同層，見 [裝置設定](docs/device-config.md)。
- USER 短按亮屏 60 秒／切換連線與診斷兩頁；按住 1 秒出現 ACT／SLP 倒數，未滿 5 秒放開取消，
  滿 5 秒放開切換模式，未認領也可 ACT；保留 owner／BLE／boot，重啟改按 RST。
  原始碼 UI=8 FB27 已部署，首頁 CLIENT ID、sleep／active、owner、BLE／GPS／RF 與電壓＋UP；診斷六行保留 SAT／AGE／HDOP。
  操作與限制見 [維護介面](docs/t096-maintenance-2026-10-28.md)。

完整控制、混版限制及 API 見 [BLE 流程](docs/ble-control-2026-10-27.md)、
[綁定契約](docs/client-binding-api.md)、[OpenAPI](docs/openapi-client-control.json)。

## 網頁與 Servo

資訊頁為 **定位資訊／Station Info／Surfer Info** 三區；詳細定位、RF、命令及 MCU 數據收進診斷。
已移除控制板卡、網頁置中、正常溫濕度／SD／版本常駐顯示；SD error 仍會提醒。
Surfer ID 與頻道可點擊開啟編輯視窗；失聯保留已確認 ID，不假裝未配對。
版面與操作以 [網頁設計規格](docs/web-ui-design.md) 為準。

Youmi 35 kg＋串口控制板固定 M0，LilyGO TX GPIO43、9600 8N1、RX 不配置。
命令 `#00P0500!`～`#00P2500!`，0～359°、中位 179.5°／1500 μs；控制板產生 PWM。
開機手動，操作只留手動／GPS；舊外部 UART 輸入與 GPS α 已移除。
共用速限預設 30°/s、1–90 可調並保存；GPS 目標每 50 ms 重算，定位失效轉完最後有效目標。
先切手動、按控制板實體置中鍵並等停穩，再輸入指南針讀數校正；網頁以 `physical_center=1`
確認並同步軟體中位、更新控制世代，校正只存 RAM。UART 成功與命令角度都不是實體位置回授。
完整控制與接線見 [motion-control](docs/motion-control.md)、[uart-servo](docs/uart-servo.md)。

## 驗證、部署與記錄

2026-10-28 維護紀錄已記載 584C Station／FB27 Client application-only USB 更新至 **1.0／UI=5**，
雙端新 boot、Ready／BLE 認領與 SD recording 已確認；FB27 隨後已更新 UI=6，診斷版本行為 `VERSION 1.0`。
詳見該文件的雙端部署與版本標籤節。
此前 0.7.0-dev 的 B 頻道 5 個 test probes 無缺號及 Stop 回 Ready 是前版證據，1.0 部署未重做 RF Test／GPS。
產物 hash、boot、主機回歸／build 與實機結果分列於 [BLE 部署紀錄](docs/ble-control-2026-10-27.md)
及 [維護紀錄](docs/t096-maintenance-2026-10-28.md)。這些是既有部署證據，文件同步不等於重新驗收。
多 Station 競爭、跨頻保存、長時間重連、待機電流、維護按鍵／TFT 目視及新版戶外追蹤仍有待驗項目。

DATA 18 bytes、無 ACK；最新有效 epoch 才送，失效通知一次，E7 解析度不是實際精度。
T096 UC6580 的 2 Hz 配置／epoch 證據不代表每秒兩筆有效位置或 RF 到達；歷史外測見
[10/10 GNSS 紀錄](docs/t096-field-validation-2026-10-10.md)。
T096 TEL 的 `humidityPct=254` 表示 MCU 溫度／無濕度，不能套用環境告警；
唯讀 USB NMEA 證據與取樣界線見 [健康診斷](docs/t096-health-diagnostics-2026-10-22.md)。

LilyGO Station 自動 SD 記錄；header 是開檔時的 runtime RF 快照，同檔切頻看 `[RF]` 事件。
T096 正常版無 SD；V4 使用有界 Flash。USB 匯出與 CRC 驗證見 [SD 記錄](docs/sd-log.md)。
Axiom API 保留、預設關閉，需要自行設定 ingest Token；目前部署採 SD，忙碌或離線可能遺失上傳資料。
網頁 GPX／Strava／除錯錄製已移除，診斷 HTTP API 仍可使用，見 [現場除錯](docs/debug-export.md)。

全套主機驗證入口為 `python3 tools/verify_project.py --prepare`，不會燒錄或操作硬體。
[10/16 健檢](docs/health-check-2026-10-16.md)、[10/11 Servo 部署](docs/servo-ui-deployment-2026-10-11.md)
與 [10/9 RTC 待機](docs/t096-rtc-standby-2026-10-09.md) 保留當時證據；舊 Ready 每 30 秒 LoRa 流程已由 BLE 取代。

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

## 怎麼分 client / station
角色由 PlatformIO environment／board profile 決定，設定在 `platformio.ini` 與 `platformio-t096.ini`：

+ `tbeam-client` = 保留的舊 LilyGO Client profile，對應 `ROLE_CLIENT`；目前使用中的追蹤器為 T096
+ `tbeam-station` = LilyGO 岸邊攝影站，對應 `ROLE_STATION`
+ `heltec-v4-station` = Heltec V4 攝影站；明確 FEM override 見硬體文件
+ `pio run -c platformio-t096.ini -e t096-client` = Heltec T096 追蹤器，獨立 nRF52840 程式

RF group／板型／配對由 [裝置 manifest](docs/device-config.md) 交叉核對；build profile 不會轉移 Client 的控制權。

燒錄哪塊板、走哪個 `upload_port`，看的是你執行 `pio run -e tbeam-client` 還是 `-e tbeam-station`（或 VS Code 下方切換 environment），跟埠號本身無關。

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
python3 tools/test_radio_power.py
python3 tools/test_debug_backend.py
```

native 測試涵蓋 GPS／UART 模式隔離、SET 解析、watchdog、非阻塞 TX、client 綁定、磁偏角與
既有數學／提醒，以及 v5 codec、同 epoch GNSS 與無線排程；Node 測試執行實際頁面，
模擬時鐘與 HTTP 回覆；Python 工具擷取實際 C++ 後端驗證控制、封包與除錯 JSON。
現況與待處理項目見 [交接入口](docs/assistant-handoff.md) 和 [版本紀錄](CHANGELOG.md)。
[9/8 健檢](docs/health-check-2026-09-08.md) 是歷史驗證，不代表現行部署。

## Station 透過 Wi-Fi 更新（PlatformIO espota）

第一次必須用 `tbeam-station` 經 USB 燒錄，讓板子取得 OTA 功能。之後電腦與 Station 連在同一個手機熱點時，可使用 `tbeam-station-ota`：

```bash
pio run -e tbeam-station-ota -t upload --upload-port <STATION_IP>
```

設定檔使用不可用的 placeholder，必須以核對身分後的 OLED IP／hostname 覆蓋：

```bash
pio run -e tbeam-station-ota -t upload --upload-port <STATION_IP>
```

更新期間請維持供電與 Wi-Fi 穩定。OTA 開始時會暫停自動追蹤，成功後 Station 自動重開；USB 燒錄仍保留作為救援方式。OTA 本身未設密碼，任何連上同一熱點且能連到 Station 的裝置都能送出韌體，因此不要讓不受信任的裝置加入熱點。監控頁 API 與 LoRa 封包也是同樣的取捨（都刻意沒做存取控制），完整說明見 [interface.md 的存取控制一節](docs/interface.md#http-與-ota-存取)。

## GPS 與指南針校正

GPS 模式需要兩端有效且新鮮的位置與有效磁偏角；手動／GPS 不自動切換。
實體置中校正、179.5°中位與速限見上方「網頁與 Servo」及 [控制契約](docs/motion-control.md)。
開機指南針參考為東 90°，搭配中位形成 `mount_offset=269.5°`；手動校正只保留 RAM。
台灣磁偏角依位置與日期補償；板上磁力計／hard-iron 與外部 SET／SET2 輸入已移除。
舊 180°／UART 模式資料只作歷史參考，不作現行操作指引。

## 為什麼選 LoRa？
衝浪環境對無線通訊有幾個特殊條件：距離遠、無遮蔽物、不適合攜帶手機。LoRa 在這個場景下的優勢在於低功耗與長距離，更重要的是它讓「下水端」的職責單純：只負責定位與傳輸，不需要維護網路連線。

本系統用 LoRa 傳送戶外定位，BLE 用於近距離認領與待機控制；實際海面覆蓋、定位品質及防水效果仍需外測。

[LoRa 封包與攝影站 API](docs/interface.md)\
[韌體功能說明](docs/features.md)\
[網頁設計規格（修改 UI 前必讀）](docs/web-ui-design.md)

## 未來目標
+ 多Surfer追蹤
+ Auto Record\
  結合GPS路徑或影像分析，偵測「追浪」與「起程」事件，讓錄影自動觸發，不再依賴手動操作。

+ 純影像追蹤，不需要攜帶追蹤器\
  岸上相機透過影像追蹤自動鎖定目標。從「GPS 輔助追蹤」演進為「完全被動」的使用體驗——這也是整個系統最終想到達的地方。

## L76K 外測診斷版

使用 `tbeam-client-diagnostic`／`tbeam-station-diagnostic`，以內建 Flash 保存原始 GNSS、時間與重啟原因，外測不需要筆電。流程、容量限制與 USB 匯出見 [field-diagnostic.md](docs/field-diagnostic.md)。普通韌體仍使用原 2 Hz 設定。
