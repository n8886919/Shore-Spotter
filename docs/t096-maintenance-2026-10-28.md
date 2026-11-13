# T096 開蓋維護按鍵與顯示（2026-10-28）

目前原始碼為 1.0 BLE Client／UI=8（FB27 10/28、F035 10/30 已部署）；前版部署與 USER 重啟驗收證據保留原樣。
USER 長按改為 ACT／SLP，本機啟動不要求 Station owner；RST 保留硬體重啟與換 Station 用途。
不改 BLE 認領規則、封包或 Station Servo 行為。

## USER

| 操作 | 結果 |
| --- | --- |
| 熄屏短按後放開 | 開啟連線首頁，約 60 秒後熄屏 |
| 亮屏短按後放開 | 連線／診斷兩頁切換，重計約 60 秒 |
| 按住滿 1 秒 | 顯示目標 ACT／SLP 與倒數，尚不切換模式 |
| 1 至未滿 5 秒放開 | 取消，不切頁、不改 GPS／owner，畫面再保留 60 秒 |
| 滿 5 秒後放開 | Ready → Tracking（ACT）；Tracking／Test → Ready（SLP）；保留 owner、boot 與 BLE |
| RST 按一次 | 硬體重啟，回 Ready 並重新開放認領 |
| 開機時已按住 | 先放開才接受下一手勢；持續壓住不反覆切換 |

短按指去彈跳後未滿 1 秒；按下與放開各有 30 ms 去彈跳。5 秒從確認按下起算，
以首次放開邊緣計算持續時間，不把放開去彈跳多出的 30 ms 算成長按。
按住時不切換；達門檻顯示 `Release to switch`，放開才提出切換。USB／電池行為相同。
若 RF 正在 TX／RX，先等現有交易結束再切換，不中途切斷 FEM；完成後回首頁並更新 BLE 狀態。
開機及低電狀態變更仍自動顯示約 5 秒；手動查看才延長為 60 秒。
低電保護期間可查看，長按 ACT 顯示禁止提示並保留 `USER BAT` 診斷；不能繞過低電保護。
ACT 開 GNSS 與 LoRa；SLP 關 GNSS、讓 LoRa 休眠，BLE 繼續運作，不是關機或 SystemOFF。
未認領時亦可 ACT，OWNER 保持 NONE；之後第一個有效 BLE Claim 可在 Tracking 中取得 owner，不重啟 GPS。
本機切換沿用 Client 目前 runtime 頻道：開機預設 A（923.2 MHz／SF9），本次開機由 Station 改成 B 後則沿用 B。
Client 不讀取 Station 的保存偏好、不自動掃頻或換頻。Station 若在不同頻道，可先用 BLE Stop，再選頻與 Start；
直接認領不代表 LoRa 已互通，也不保存新頻道偏好。
RF 初始化失敗則保持原模式、保留原始 RF INIT 錯誤，可再次操作；切換不清除計數或最後失敗。
重啟／換 owner 改用 RST；開機會重新取樣電池，重啟不代表解除低電保護。

## 開機與模式文字提示（UI=8）

- 通電／RST 開機：TFT 初始化後立即顯示 `CLIENT BOOTING`，讓 radio／BLE 初始化期間可見；不增加阻塞延遲。
- USER 切換完成：首頁首行短暫顯示 `ACT STARTED`／`ENTERED SLP`，約 2 秒後恢復 `CLIENT ID xxxx`。
- 低電／radio 初始化失敗：`ACT BLOCKED: LOW BAT`／`ACT FAILED: RF ERROR`，不誤顯示成功，診斷頁仍保留原因。
- 提示沿用 6×8 英文字型，不增加字庫；短按可立即切頁，長按倒數優先顯示。手動查看仍為 60 秒，開機自動約 5 秒。
- SLP 仍保留 BLE；真正直接斷電後無法顯示關機文字。本版不新增 SystemOFF 或斷電控制。

## 兩頁顯示

160×80 TFT 首頁與長按預覽為五行、每行 16 px；診斷頁六行、每行 13 px，底部 2 px 清黑。
共用 160×16 row buffer，字體維持 6×8 px；每列最多 26 字，無自動翻頁。
每輪畫面更新間隔 500 ms，RF 傳輸／接收進行中讓位；背光到期處理不等待 RF。
SLPOUT 的 120 ms 以非阻塞期限處理，亮屏時間約從操作起算。

連線首頁：

1. 顯示 `CLIENT ID xxxx`（沿用 UI=7），四位十六進位供 Station 配對。
   真實 BLE MAC 保留 USB 診斷；從 SoftDevice 讀取，失敗保留原始錯誤碼。
2. 固定顯示 sleep 與 active，中間用繪圖函式畫 14 px 連續垂直實線，不使用中間有缺口的字型 `|`；用矩形邊框框住目前模式。Ready 對應 sleep，Tracking／RF Test 對應 active。
   sleep 是應用待機分類，不代表 BLE 關閉／SystemOFF；RF Test 不開 GPS，內部狀態與協定保持分開。
3. 合併 `OWNER 584C, BLE LINK`；owner 使用四位十六進位，未認領為 `OWNER NONE`。
   BLE `ADV/LINK/OFF/ERR` 仍依實際廣播／連線／初始化結果；LINK 不代表有效認領。
4. GPS 與 RF 狀態同一行，例如 `GPS OFF RF OFF`、`GPS FIX RF ON`。
   GPS 保留 OFF／NO EPOCH／STALE／FIX／NOFIX；RF 顯示 ERR／OFF／ON，原 SLEEP 文字改 OFF，不改硬體休眠行為。
5. 電池 ADC 電壓／未知，與 uptime，例如 `BAT 4.17V UP 125s`；GPS 指標移回診斷頁。
   低電保護改顯示低電電壓與 CHARGE 提示。
   移除畫面 USB 文字；電壓不是電流、剩餘容量或充電完成證據。

診斷頁：

1. 韌體版本，例如 `VERSION 1.0`。
2. `TX <完成數>`；不代表 Station 已收到。
3. `ERR TX<發送錯誤> RX<接收錯誤>`。畫面上限 TX 完成 999999、發送錯誤 99999，超過加 `+`；底層計數不改。
4. `SAT 12`；無新鮮 GGA、GPS 關閉或衛星數未知時顯示 `SAT --`。
5. `AGE 0.3s  HDOP 1.1`；AGE 是最新 epoch 到達年齡，無 epoch 為 `--`；過期仍保留年齡、HDOP 改 `--`。
   HDOP 未知／非法為 `--`，不代表已知位置誤差。
6. `LAST <來源> <原始碼>` 或 `LAST NONE`。最後一次 RF／BLE 失敗保留到下一個失敗或重啟，成功操作不清除。
   只回 boolean 的 API／封包驗證失敗標記 `FAIL`，不虛構 SDK 錯誤碼；RF IRQ 記錄原 flags。
   這是本次開機最後失敗，不代表該故障仍持續；詳細時序仍查 USB log。

長按預覽期間暫時顯示目標 ACT／SLP、倒數、GPS／LoRa 開關及保留 BLE／owner 提示；提前放開取消後回原頁。
首頁保留 GPS／RF 狀態，完整 SAT／AGE／HDOP 放診斷頁；正常頁面沒有 USB 文字。以上為 UI=8 排版，正常頁沿用 UI=7。
不常駐舊 `USER hold 2s: Ready` 提示。RAM 診斷與計數在重啟後清除，應先查看／匯出再重啟。

## 換 Station

1. 先讓舊 Station 停止搜尋此 Client（關閉舊 Station，或改掉其目標 ID）。
2. 新 Station 設定此 Client ID。
3. 按一次 RST，Client 重啟；USER 長按已不重啟、不解除 owner。
4. 從首頁確認 OWNER 為新 Station ID，並從新 Station 確認新 boot 的認領狀態。

仍是第一個有效 Claim 取得本次開機 owner；舊 Station 若持續搜尋，可能再次搶先認領。
按鍵不提供強制指定另一 Station 或原地清 owner 的第二套流程。

## UI=8 ACT／SLP 主機驗證

- `tools/test_t096_*.py` 十組，加 BLE codec、radio power、Station BLE backend，共 13 組通過。
- 實際 USER／renderer 主機測試涵蓋 USB／電池、4999 ms 提前放開、滿 5 秒放開、按住不執行、開機已按住、wrap／bounce、
  開機提示／切換成功與失敗文字／提示期限 wrap／短按略過、
  保留 owner／Station boot／command／channel、未認領 ACT、Tracking 中 Claim、TX／RX 忙碌延後、Test → SLP、低電拒絕及 RF 初始化失敗／恢復。
- DATA 主機測試改用真實 Controller，在無 owner 的本機 ACT 下驗證有效 epoch DATA、過期失效通知與 SLP 停發。
- BLE／LoRa 的舊 Start／Stop 重播在本機切換後不得錯誤確認；新序號仍可正常控制。
- `pio test -e native` 140／140 通過；顯示提示追加後 13 組主機回歸重跑通過，`git diff --check` 通過。
- `pio run -c platformio-t096.ini -e t096-client` 通過；RAM 19,724／248,832 bytes，Flash 200,008／815,104 bytes。
  SDK 仍有 unused-parameter warnings，非零警告建置。產物包含目前 Client WIP（含先前 UI=7 的 Client ID）。
- `.pio/build/t096-client/firmware.zip` ZIP CRC 通過，SHA-256 `6230b51f8b3e84a8c60f0eb7660d63daa4b2235eb7e7033e17fea0c024083360`。
- FB27 已燒錄並確認新 boot／BLE 認領（見下節）；實體按鍵、畫面、不同頻道接手與戶外 GNSS／RF 仍未驗收。


## UI=8 FB27 部署（2026-10-28）

- 燒錄前 USB 核對 serial `F314D986FA637CF1`／Client FB27，舊 UI=6、boot `A19F6835`。
- 產物為上節 SHA-256 `6230b51f8b3e84a8c60f0eb7660d63daa4b2235eb7e7033e17fea0c024083360`；
  ZIP CRC、application-only manifest、SoftDevice requirement `0xB6` 與 UI=8／模式切換／文字字串均已確認。
- 1200-baud touch 後同 serial 由 `239A:8071` → `239A:0071`，115200 application-only DFU
  `Device programmed.`／exit 0；返回同 serial `239A:8071`。
- USB 新 boot `E4017F95`（3825303445）、1.0、UI=8；build `Oct 28 2026 09:47:27`，
  `mode_hold_ms=5000 release_required=1 unclaimed_act=1`；bootloader 0.9.0。
- Station 即時 API 確認同一 Client／新 boot，Ready、BLE connected／claimed、owner 584C、
  `can_control=true`、狀態 age 939 ms、tx_packets=0。Station boot 2025037645；本輪未重燒 Station。
- 尚未實體操作 USER／RST 或驗收 TFT 文字；未為部署驗證啟動 Tracking／RF Test 或操作 Servo。
  長按未認領啟動、模式切換與文字顯示目前為主機測試證據，實機手勢與外測仍待驗收。
- 私人來源 hash manifest、產物、DFU／USB／HTTP 證據：`~/.local/state/shore-spotter/t096-user-mode-20261028T043048Z/`，不納入 Git。

## UI=8 F035 部署（2026-10-30）

- 使用者要求更新目前接上的 T096。USB 核對 serial `C3BFA5F76F915E4A`／Client F035，
  舊韌體 0.6.1-dev、boot `56D17C8C`、build `Oct 22 2026 02:11:40`、bootloader 0.9.0。
- 使用目前工作目錄（含既有 WIP）在獨立輸出目錄完整建置，建置期間 210 個來源／專案檔案 hash 未變；
  未修改韌體原始碼。native 140／140 與 13 組 T096／BLE codec／radio power／Station BLE backend 回歸通過，
  `git diff --check` 通過。Client build 成功，RAM 19,724／248,832、Flash 200,008／815,104 bytes；
  SDK 仍有 unused-parameter warnings。
- ZIP CRC、application-only manifest、SoftDevice requirement `0xB6`、UI=8／模式提示字串均已核對。
  DFU ZIP SHA-256：`67c9d95e42926de31fca55519d55b4c0d259b0f4089dbf61eb5b2dbf70dfdbac`；
  HEX SHA-256：`0d43b75a3cd0de75f79648082cd054b4c8517b4714813a2d7e59e1b69b49ad08`。
- 1200-baud touch 回報 EPIPE 後，已核對同 serial `239A:0071` 與可用 tty 才繼續；
  115200 application-only DFU 回覆 `Device programmed.`／exit 0，保留 bootloader／SoftDevice。
  同 serial 返回 `239A:8071`，USB 確認 1.0／UI=8、新 boot `CF8DCA00`、
  build `Oct 30 2026 23:34:18`、bootloader 0.9.0，`mode_hold_ms=5000 release_required=1 unclaimed_act=1`。
- 新開機 USB 顯示 runtime A／923.2 MHz／SF9；這是現行 1.0 的開機預設，不能沿用舊韌體 B 頻道推定。
  唯讀 `DIAG STATUS` 回覆同 boot／fw=1.0、`mcu_valid=1`、`failures=0`，證明新程式仍在回應。
  Station 未重燒或修改綁定；未驗證 BLE 認領、實體 USER／RST／TFT 或戶外 GNSS／RF，未操作 Servo。
- 私人來源 manifest、原 WIP patch、測試／建置／DFU／USB 與產物保存在
  `~/.local/state/shore-spotter/t096-flash-20261030T153351Z/`，不納入 Git。

## Station 584C 與 F035 配對（2026-10-30）

- 使用者接上 Station 並要求配對剛更新的 F035。USB 核對 VID:PID `303a:1001`、
  serial／MAC `48:CA:43:57:58:4C`；HTTP 核對 hostname `shore-48ca4357584c`、
  韌體 1.0、build `Oct 28 2026 02:28:31`、Station boot `602164504`，原綁定為 FB27。
- POST `/api/whitelist?action=set&id=F035` 回 HTTP 200、單一綁定 F035；後續 GET 讀回一致。
  等待實際 BLE 認領後，Client ID 61493／F035、boot `D52C5A29`、`state_source=ble`、
  Ready、BLE connected／claimed、owner 22604／584C、`can_control=true`、無錯誤。
  兩次獨立查詢為同一 Client／Station boot，age 888／846 ms；Client 電壓 4059 mV、`charging=false`。
- 這次確認的是保存綁定及新鮮 BLE 認領，不把 HTTP 200 當作在線證據。
  未重啟兩端來重測 NVS 保留，未重新 build／燒錄或啟動 Tracking／Test；
  Servo 維持 manual／179.5°，未送移動指令。LoRa 上下行、GPS 定位及戶外追蹤仍待驗收。
- 私人 HTTP 前後快照、配對時間線與結果保存在
  `~/.local/state/shore-spotter/station-pair-f035-20261030T155400Z/`，不納入 Git。

以下為舊 UI=2～6 的當時驗證與部署，包含已移除的 USER 重啟手勢。

## 初版 UI=2 驗證／部署

本輪主機驗證：

- `pio test -e native`：140／140 通過。
- `tools/test_t096_*.py` 十組與 `tools/test_ble_control_protocol.py`：11／11 通過。
  包含實際按鍵／renderer／formatter、4999 ms 放開不誤重啟、開機已按住、wrap、
  USB／電池、兩頁 26 字上限、GPS 新鮮度、原碼 0／完整 int32 與原 ADC／低電回歸。
- `tools/test_radio_power.py` 通過；`git diff --check` 通過。
- `PLATFORMIO_BUILD_DIR=/tmp/shore-t096-maintenance-20261028 pio run -c platformio-t096.ini -e t096-client` 通過。
  RAM 19,692／248,832 bytes，Flash 198,516／815,104 bytes；版本保留 0.7.0-dev。
- 獨立審查修正原碼 0 與 boolean FAIL 混淆，以及 RF 忙碌時不繪圖導致 TX／RX 標示不可達；
  改為 ON／SLEEP／ERR，保留 RF 空檔繪圖與背光獨立期限。

已燒錄產物：application-only DFU ZIP SHA-256
`1d6e9af0c52bf7f79ce9233170034937228c3b7c600eb6d5c802dd633511bbe9`；
HEX SHA-256 `2c736cd865d6bd560e0e530d7f32d6a9c6936e938958030e506da512198bb434`。
ZIP CRC／application-only manifest／SoftDevice requirement 0xB6 已核對。

部署前實機 USB 確認 FB27／64295、serial `F314D986FA637CF1`、舊 boot `240E79A3`，
Station HTTP 確認 Ready／BLE claimed／owner 22604，Station boot 866644376、manual 179.5°／60°/s。
1200-baud touch 後 sysfs 已確認同 serial 的 `239A:0071` DFU；但 Linux 未建立 tty 裝置節點，
主機拒絕 mknod，當時尚未寫入；使用者重新連接／分享 USB 後，同 serial DFU 串口恢復。
115200 application-only DFU 回覆 `Device programmed.`、exit 0；未覆寫 SoftDevice／bootloader。

燒錄後同 serial 回到 `239A:8071`；USB 回報 FB27、0.7.0-dev、build `Oct 28 2026 00:30:26`，
新 boot `5BB4BC9F`（1538571423）、bootloader 0.9.0、`RADIO init ok`，以及
`MAINT UI=2 short=page screen_ms=60000 restart_hold_ms=5000 release_required=1`。
實機 Station API 回報同一新 boot、Ready、BLE connected／claimed、owner 22604、`can_control=true`，
狀態年齡 239 ms、tx_packets=0；沒有為本輪驗證開啟 GPS 或送 RF Test。
Station 未重燒，boot 866644376 不變；manual 179.5°／60°/s，SD recording／errors=0。
SD `sample_dropped=971` 是既有持續錄製的快照丟棄計數，不能宣稱記錄零遺失。

私人原始證據、來源 hash manifest 與產物位於本機 state `t096-maintenance-20261028`，不納入 Git。

螢幕實體可讀性、真人按鍵手感及跨 Station 換機仍需實機操作驗收，不能以 fake GPIO／build 代替。

## 後續 UI=3 驗證／部署

依使用者後續指示改為本文上方的新首頁排版，GPS 集中首頁，診斷頁只留系統／傳輸診斷。
這次未改按鍵、控制狀態機、BLE／LoRa 協定或 Station 韌體。

- `test_t096_power.py`、`test_t096_display.py`、`test_t096_ble_control.py` 通過。
  實際 formatter／renderer 覆蓋六-byte MAC 反轉與讀取失敗、十六進位 owner、兩個常駐模式文字與 Ready／Tracking／Test 的選取框、
  GPS 關閉／無 epoch／無 fix／過期／未知指標、整行寬度、GPS 不重複／USB 移除及原按鍵／低電回歸。
- 用實際 formatter／繪圖座標與 Adafruit 原字型產生 mock 像素預覽；sleep／active 邊框沒有蓋字或越界。
  此為主機渲染，不代表實體 TFT 已目視驗收。
- Client build 通過：RAM 19,708／248,832 bytes、Flash 198,680／815,104 bytes；版本仍為 0.7.0-dev。
  `git diff --check` 通過。
- FB27（serial `F314D986FA637CF1`）同 serial application → DFU → application 身分完整核對；
  application-only DFU exit 0／`Device programmed.`。新 boot `B881BE78`（3095510648），
  build `Oct 28 2026 00:48:54`，`MAINT UI=3`，實際 BLE MAC `EE:1F:71:6739CD`，bootloader 0.9.0。
- Station API 確認新 boot／Ready／BLE connected 與 claimed、owner 0x584C、`can_control=true`，狀態年齡 35 ms。
  Station boot 866644376、manual 179.5°／60°/s 不變；本輪未開 GPS、未送 RF Test／Servo 指令。

DFU ZIP SHA-256：`e2504bc66e447eeedda675bf15f11c893036fc8ddf4a71e3ed444962f0870d30`。
HEX SHA-256：`8ffe1885318fadab9f17c757eab793c62e479689a0d91d0d58514210d7ac804f`。
來源 hash manifest 與產物吻合；私人原始證據／mock 預覽位於本機 state `t096-maintenance-ui3-20261028`。

## 後續 UI=4 驗證／部署

- 完整 MAC 改為五個冒號；sleep／active 分隔線用 `drawFastVLine(45,1,14)` 畫連續 14 px，避開字型 `|` 的缺口。
- GPS／RF 狀態同列，RF 休眠文字由 SLEEP 改 OFF；衛星／HDOP／年齡移至電壓列，沒有移回診斷頁。
- `test_t096_power.py`、`test_t096_display.py` 通過；實際 formatter／renderer 檢查 MAC、實線呼叫、
  框選模式、GPS／RF 各狀態與最長電壓／指標文字不超過 26 字，以及既有按鍵／低電回歸。
  原字型 mock 像素預覽確認冒號齊全、實線連續且沒有重疊；實體 TFT 仍待目視確認。
- Client build 通過，RAM 19,708／248,832 bytes、Flash 198,728／815,104 bytes；`git diff --check` 通過。
- FB27／serial `F314D986FA637CF1` 已 application-only DFU，exit 0／`Device programmed.`；
  新 boot `4E9805E0`、build `Oct 28 2026 00:56:49`、`MAINT UI=4`、BLE MAC `EE:1F:71:67:39:CD`。
  Station 確認新 boot／Ready／BLE claimed、owner 0x584C、狀態年齡 481 ms，manual 179.5°／60°/s 不變。
  本輪未開 GPS、未送 RF Test／Servo 指令。

DFU ZIP SHA-256：`167cbe33d1a470bbfd63ebc2daf7149388ca8b1b297ba333c440a6683a99954d`。
HEX SHA-256：`f133afff58901447921f8f9ba2e278515d19124abfd90dd738a9c871c950e447`。
來源 hash manifest 與產物吻合；私人原始證據／mock 預覽位於本機 state `t096-maintenance-ui4-20261028`。

## 1.0／UI=5 驗證與雙端部署

- 版本來源改為 `1.0`，採 a.b：同主版本所有子版本的 CL／ST 必須相容；不相容變更升主版本。
  正常版本不帶 dev；`FIELD_DIAGNOSTIC` 建置為 `1.0-debug`。正常版附診斷頁不等於診斷專用建置。
  LoRa wire v5／BLE 契約不變，不宣稱與舊 0.x 任意混用相容；歷史證據保留當時版本字串。
- 首頁仍五行，BAT／UP 同行；診斷頁六行，完整 SAT 與 AGE／HDOP 加回，保留版本／TX／ERR／LAST。
  字型維持 6×8，診斷列高 13 px、底部 2 px 清黑；完整 MAC／模式實線與邊框／owner／GPS+RF 不變。
- 實際來源回歸 `test_t096_power.py`、`test_t096_display.py`、`test_t096_ble_control.py`、
  `test_t096_idle.py`、`test_debug_backend.py`、`test_station_ble_backend.py`、`test_ble_control_protocol.py` 通過。
  覆蓋六行 renderer、GPS 無資料／過期、未知 HDOP、最長 uptime／低電提示不截字、原 USER 行為。
  正常／FIELD_DIAGNOSTIC 預處理版本分別為 1.0／1.0-debug；原字型 mock 預覽無重疊。
- Client 與 LilyGO Station build 通過；Client RAM 19,708／248,832、Flash 198,840／815,104 bytes。
  Station application 1,925,184 bytes。來源 hash manifest 核對通過，暫用部署網路設定後已還原 workspace header。
- Station serial `48:CA:43:57:58:4C` 已停止 SD worker；USB 讀回 partition／otadata 並核對 CRC，
  確認 app0 後只寫 `0x10000` application，esptool hash 驗證通過；保留 bootloader／partition／NVS。
  USB reset 後 HTTP 初次逾時，開啟 USB 確認新開機與 Wi-Fi 連線後，HTTP 恢復。
- Client FB27／serial `F314D986FA637CF1` application→DFU→application 同身分核對；
  ZIP CRC／application-only manifest／SoftDevice 0xB6 核對、DFU exit 0／Device programmed。
  USB 回報 1.0／UI=5、新 boot `14BA0BDD`，build `Oct 28 2026 01:10:41`、bootloader 0.9.0。
- Station API 回報 1.0，build `Oct 28 2026 01:10:08`、新 boot 1901137112；
  Client 同一新 boot／Ready／BLE claimed／owner 0x584C／can_control=true，狀態年齡 615 ms。
  SD recording／errors=0、sample_dropped=3；不宣稱記錄零遺失。
  Servo manual／179.5°／60°/s 與校正保留；本輪未開 GPS、未送 RF Test 或額外 Servo 操作指令。
- 本輪證據是主機測試、像素 mock、燒錄與新開機控制狀態；實體 TFT 可讀性、按鍵手感及戶外定位未重新驗收。

Client DFU ZIP SHA-256：`b8ac1b6e490e3612aa94915d82e56502c2c25d6d01efa6e3fa7d12f2196aa8fc`。
Station application SHA-256：`857820fbc784e1d8aba0d864d5d2694f97b31332fc28a50cb42c29282a69a1f6`。
私人原始證據、來源 manifest、產物與預覽位於本機 state `release-1.0-20261028`，不納入 Git。

## UI=6 版本標籤

診斷第一行改 `VERSION 1.0`；其餘排版與版本號不變。實際顯示／按鍵回歸、Client build 與原字型 mock 通過。
FB27 已 application-only DFU（Device programmed／exit 0），USB 確認 1.0、UI=6、新 boot `CF0225A5`，
Station 確認同一新 boot／Ready／BLE claimed／owner 0x584C。Station 本輪未重燒；實體文字仍待目視。
產物／來源 manifest 與私人證據位於本機 state `t096-version-label-20261028`；DFU ZIP SHA-256
`1f690a119bff90dcb2c39fbbdd113e3b5f4124817b1d2fada2cecd2cf017eb31`。

## 後續使用者室內驗收

USER 短按切頁／熄屏、長按取消、滿 5 秒放開重啟、Test／Stop、A/B 掃描與啟動保存、
兩端重啟後 B 頻道操作、網頁重開及選頻限制已完成互動驗收。
重連曾等到 Client UP 61s，速度尚未通過；三項顯示需求僅記錄、未實作。
逐項證據、限制與下一 session 任務見 [室內驗收交接](indoor-acceptance-handoff-2026-10-28.md)。
此節補充後續觀察，不更改上方各次部署當時的驗證界線。

## UI=7 Client 識別文字（2026-10-28，未部署）

使用者確認首頁第一行改為 `CLIENT ID FB27`，由 `thisClientId` 動態產生四位十六進位，
其他 Client 顯示自己的 ID（例如 F035），不硬編 FB27。不再以 BLE MAC 充當配對 ID。
真實 BLE 位址仍由 `sd_ble_gap_addr_get()` 讀取，保留於 USB `BLE MAC` 診斷；不偽造 MAC 尾碼。
其餘五行首頁／六行診斷排版及按鍵行為不變，boot 標記更新為 `MAINT UI=7`。
既有 UI=6 部署與室內驗收仍是舊版證據；本版尚未燒錄或實體目視。
