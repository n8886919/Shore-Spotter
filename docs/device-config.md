# RF 頻道、建置 group 與裝置 manifest

## 1.0 執行期頻道（2026-10-28）

目前使用中的組合為 **LilyGO Station＋T096 Client**。現行 1.0 的 Station 與 T096（沿用 0.7 BLE 流程）
執行期候選 A/B 均固定 **SF9、BW125 kHz、CR4/5、sync word `0x12`**；
不能用舊 manifest/build profile 的 A/SF10 推定這兩端的實際 SF。

| 名稱／ID | 頻率 | `rf-groups.json` 建置 profile | 現行 Station／T096 執行期候選 |
| --- | --- | --- | --- |
| A／0 | 923.2 MHz | SF10 | SF9 |
| B／1 | 923.8 MHz | SF9 | SF9 |

執行期候選與固定 SF 由 `include/ble_control_protocol.h` 定義；T096 直接使用這份設定，
Station 的 `src/main.cpp` 在 `ROLE_STATION` 下也固定 `RF_SF=9`。
`include/runtime_radio_profile.h` 保存 Station 當下頻率／SF 的快照供記錄使用。
舊 LilyGO Client 仍沿用建置 profile，但已不是本專案使用中的 Client；不可假設與新流程混版可用。

Station 開機優先讀取 NVS `rfchannel`；沒有有效保存值時，以建置頻率選 A/B 作為預設。
手動選頻流程為 **BLE 已認領且 Ready → CAD 掃描／選擇 → 啟動 → 匹配 LoRa STATE → 保存**：

- 掃描由使用者觸發，每個候選執行五次 CAD，結束後還原接收頻率；不自動選台或跳頻。
  CAD 只表示該設定下的 LoRa 偵測，不能證明沒有其他干擾或頻道可用。
- 選擇先留在 RAM；啟動／測試時兩端才套用該頻率。只有「啟動」收到同 Client boot、owner、
  command 的實際 LoRa STATE，且 Station 在選定頻道，才將偏好保存到這台 Station 的 NVS。
- BLE accepted、HTTP 202、測試模式回報都不直接保存。NVS 寫入失敗明確回報錯誤；
  未保存的選擇在 Station 重開或修改目標 ID 後捨棄，失敗也不自動跳頻。

上述驗證只證明啟動狀態與選定頻道的上行 STATE，不代表 GPS fix、LoRa 下行、戶外距離或 Servo 驗收。
頻率設定也不構成 RF 認證或法規符合性聲明。
操作與已記錄的部署／驗證界線見 [BLE 控制與手動選頻](ble-control-2026-10-27.md)。

## 建置 profile 與本機 manifest

`config/rf-groups.json` 是 **manifest／建置 loader** 的 RF profile 來源，仍保留 A/SF10、B/SF9。
PlatformIO 環境以 `custom_rf_group=A` 或 `B` 設定預設組別，並透過 `tools/load_rf_profile.py`
將選定 group 的 `SHORE_RF_GROUP`、`SHORE_RF_FREQUENCY_MHZ` 與 `SHORE_RF_SPREADING_FACTOR`
加到該次 build。不要在環境的 `build_flags` 再硬編這三個 macro，loader 會拒絕重複定義。
這些建置值不會覆蓋上述 現行 Station／T096 的固定 SF9，也不會覆蓋已保存的 Station 頻道偏好。

實際裝置清單放在被忽略的 `config/devices.local.json`；可由 `config/devices.example.json` 開始。欄位為：

- `name`：manifest 的預期裝置 ID，不是推測出的 LoRa 或硬體 ID。
- `board`：工具接受 `heltec-v4`（Station）、`heltec-t096`（Client）與 `lilygo-tbeam`（Station／舊 Client profile）；目前使用中的 LilyGO 角色為 Station。
- `role`：`station` 或 `client`，必須與 board 相容。
- `environment`：對應的 PlatformIO environment。
- `usb_serial`：僅作 USB 裝置辨識；不可重複，工具不會開啟 serial port。
- `rf_group`：`rf-groups.json` 內的組別名稱，目前為 `A` 或 `B`。
- Station 的 `hostname`：建議填 `auto`，由完整 Wi-Fi MAC 產生固定名稱。
  也可自訂如 `shore-b`，不填 `.local`；1–31 字元（ESP32 網路函式庫上限），只接受小寫英數與中間的連字號，
  以英文字母開頭。`auto` 為保留值；Client 不使用此欄位。
- Station 的 `client`：接收 Client 的 **device name**。manifest 要求兩端填相同 `rf_group`；這是建置設定檢查，不是執行期頻道或連線證據。多個 Station profile 可指向同一 Client。

T096 每次開機由第一個有效 **BLE Claim** 的 Station 取得 owner；一般連線、掃描或無效命令不算認領。
Station 會自動搜尋 NVS 已保存的目標 ID。回 Ready、BLE 斷線或 Station 重開都不解除 Client owner；
同一 Station 可重連，其他 Station 即使收到 LoRa 上行也不能取得控制權。

換 Station 時依序：先關閉舊 Station 或改掉其目標 ID，讓它停止搜尋此 Client；
在新 Station 設定此 Client ID；T096 USER 長按滿 5 秒後放開重啟；確認 Client 新 boot 與新 owner。
舊 Station 若仍搜尋，可能再次搶先認領。只改 manifest 或 Station 接收綁定不會轉移現有 owner。
按鍵流程見 [T096 維護與換 Station](t096-maintenance-2026-10-28.md)。

範例以 Heltec V4 Station 與 T096 Client 同在 B 示範，USB serial 是待替換的 placeholder。
真實序號只保存在被忽略的本機 manifest；新增裝置前須先核對硬體與USB身份。

```sh
python3 tools/device_config.py list
python3 tools/device_config.py check
python3 tools/device_config.py plan station-v4
python3 tools/device_config.py --json plan station-v4
python3 tools/test_device_config.py
```

`list`、`check`、`plan` 都是純本機設定檢查：不 build、不 flash、不 reset USB。`plan` 列出 board、RF group、manifest 預期裝置 ID、配對 Client 與 `custom_rf_group`，供下一步由已確認的 build 流程使用。
目前工具檢查 board／role 組合，但 `environment` 只檢查非空；仍須核對實際 PlatformIO environment 的板型與角色，不能把 `check` 成功當成完整相容性驗證。

## 依設備選擇 build

`SHORE_DEVICE=station-v4 pio run -e heltec-v4-station` 會從本機 manifest 取得組別，
manifest 的 `environment` 字串與所選 environment 不一致時拒絕編譯；不依賴該 environment 的預設 A/B。
Client 可增加 `radio_id` 整數欄位（取自實際 USB 開機訊息，不能把 USB serial 當 radio ID）；
其配對 Station 首次開機會採用這個 ID。既有 NVS 配對優先，改檔不會覆蓋現場已保存的配對，
須透過原有 `/api/whitelist` 明確修改。未填 radio_id 的新 Station 保持未配對。
`plan` 只列命令；build 與 USB upload 各自明確執行，燒錄前核對實際 USB serial。

建置 loader 接受具有獨立 name/id/frequency 的 group，拒絕重複頻率與 ID；SF 僅接受整數 9 或 10，
BW125／CR4/5 共用。這是建置格式的能力，**單改 JSON 不會擴充 現行 BLE 執行期候選或改成 SF10**。
執行期目前只接受 channel 0/1，兩端 SF 必須一致；設定不會遠端更新其他 Station。
硬體頻率範圍檢查不代表法規許可。

## Station 固定名稱

`hostname: "auto"` 讓每台 Station 在開機時讀取自己的完整 Wi-Fi MAC，
產生 `shore-<12位小寫hex>`，例如示意 MAC `00:11:22:33:44:55` 對應
`http://shore-001122334455.local/`。不使用冒號，也不從 USB serial 猜 MAC。
同一份通用韌體燒到不同板子會得到不同名稱；換 Wi-Fi／IP／RF group 不改名。

自訂 `hostname` 則由 `SHORE_DEVICE` 編入名稱。Wi-Fi DHCP hostname、mDNS／HTTP service 與 OTA
共用結果；`GET /api/track.hostname` 回報實際名稱。`plan` 對 auto 只列格式，沒有假稱查到硬體 MAC。
工具拒絕重複的自訂名稱；多台 `auto` 合法。沒有指定 `SHORE_DEVICE` 的通用 Station build 也使用 MAC。
變更命名方式或自訂名需重新 build／燒錄；更換實體板子後 MAC 網址會變，桌面捷徑需更新。

`.local` 依賴區網 mDNS；韌體保存名稱不代表每一種手機的熱點主機端都能解析。
2026-10-02 已在目前家中網路，由電腦瀏覽器分別以 V4 原自訂名稱、後續燒錄的 MAC 名稱成功開啟控制頁。
手機熱點端仍需由開熱點的同一支手機實測，再將名稱網址加入主畫面。
目前沒有新增雲端 IP 登記服務，也沒有改 Wi-Fi 帳密／追蹤更新率。
