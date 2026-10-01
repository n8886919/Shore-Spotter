# 固定 RF group 與裝置 manifest

目前配置兩個固定 group，不掃描、不自動換頻：A 是 id 0、923.2 MHz；B 是 id 1、923.8 MHz。兩組都固定 SF10、BW125、CR5、sync word `0x12`。頻率設定本身不構成任何 RF 認證或法規符合性聲明。

`config/rf-groups.json` 是這些 RF 值的唯一設定來源。PlatformIO 環境以 `custom_rf_group=A` 或 `B` 設定預設組別，並透過 `tools/load_rf_profile.py` 將選定 group 的 `SHORE_RF_GROUP` 與 `SHORE_RF_FREQUENCY_MHZ` 加到該次 build。不要在環境的 `build_flags` 再硬編這兩個 macro，loader 會拒絕重複定義。

實際裝置清單放在被忽略的 `config/devices.local.json`；可由 `config/devices.example.json` 開始。欄位為：

- `name`：manifest 的預期裝置 ID，不是推測出的 LoRa 或硬體 ID。
- `board`：目前接受 `heltec-v4`（Station）、`heltec-t096`（Client）與後續可註冊的 `lilygo-tbeam`。
- `role`：`station` 或 `client`，必須與 board 相容。
- `environment`：對應的 PlatformIO environment。
- `usb_serial`：僅作 USB 裝置辨識；不可重複，工具不會開啟 serial port。
- `rf_group`：`rf-groups.json` 內的組別名稱，目前為 `A` 或 `B`。
- Station 的 `hostname`：每台唯一的小寫 DNS 名稱，例如 `shore-b`；不填 `.local`。
  1–31 字元（配合目前 ESP32 網路函式庫的長度限制），只接受英數與中間的連字號，名稱必須以英文字母開頭；Client 不使用此欄位。
- Station 的 `client`：配對 Client 的 **device name**。兩端必須在同一 RF group；一個 Client 不可被兩個 Station 配對。

範例以 Heltec V4 Station 與 T096 Client 同在 B 示範，USB serial 是待替換的 placeholder。
真實序號只保存在被忽略的本機 manifest；未連線的 LILYGO 與第二個 Client 待確認硬體後加入。

```sh
python3 tools/device_config.py list
python3 tools/device_config.py check
python3 tools/device_config.py plan station-v4
python3 tools/device_config.py --json plan station-v4
python3 tools/test_device_config.py
```

`list`、`check`、`plan` 都是純本機設定檢查：不 build、不 flash、不 reset USB。`plan` 列出 board、RF group、manifest 預期裝置 ID、配對 Client 與 `custom_rf_group`，供下一步由已確認的 build 流程使用。

## 依設備選擇 build

`SHORE_DEVICE=station-v4 pio run -e heltec-v4-station` 會從本機 manifest 取得組別，
與所選 environment 不一致時拒絕編譯；不依賴該 environment 的預設 A/B。
Client 可增加 `radio_id` 整數欄位（取自實際 USB 開機訊息，不能把 USB serial 當 radio ID）；
其配對 Station 首次開機會採用這個 ID。既有 NVS 配對優先，改檔不會覆蓋現場已保存的配對，
須透過原有 `/api/whitelist` 明確修改。未填 radio_id 的新 Station 保持未配對。
`plan` 只列命令；build 與 USB upload 各自明確執行，燒錄前核對實際 USB serial。

目前檔案只配置 A/B；往後可在同一 JSON 加入有獨立 name/id/frequency 的固定組別，
不必複製韌體。工具拒絕重複頻率與 ID；硬體頻率範圍檢查不代表法規許可。
現有 SF10/BW125/CR4/5 profile 維持共用，這次不新增掃頻或跳頻。

## Station 固定名稱

`SHORE_DEVICE=station-v4` 會把該設備的 `hostname` 編入韌體，Wi-Fi DHCP hostname、
mDNS／HTTP service 與 OTA 共用同一名稱。範例 `shore-b` 的入口為 `http://shore-b.local/`；
它是設備身份，不隨 RF group、家中 Wi-Fi 或熱點分配的 IP 改變。改名後需重新 build／燒錄。
工具拒絕同一 manifest 中的重名；不同 manifest 的名稱也應由管理者避免重複。

沒有指定 `SHORE_DEVICE` 的通用 Station build 以完整 Wi-Fi MAC 產生 `shore-<12位hex>`，
不再讓所有 Station 共用 `shore-spotter-station`。`GET /api/track` 的 `hostname` 回報當前名稱，
`plan` 也列出 `.local` 網址。現有 IP 網址仍可直接使用。

`.local` 依賴區網 mDNS；韌體保存名稱不代表每一種手機的熱點主機端都能解析。
2026-10-02 已在目前家中網路，由電腦瀏覽器以 `http://shore-b.local/` 開啟 V4 控制頁成功。
接著需由開熱點的同一支手機實測，成功後再將名稱網址加入主畫面。
目前沒有新增雲端 IP 登記服務，也沒有改 Wi-Fi 帳密／追蹤更新率。
