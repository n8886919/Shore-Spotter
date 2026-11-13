# T096 MCU 溫度與異常 NMEA 證據（2026-10-22）

## 範圍與硬體界線

使用者授權加入 MCU 溫度及異常 NMEA 證據，Station 對應接收／顯示並更新兩端。
版本 0.6.1-dev；不改定位接受門檻、GNSS 2 Hz、RF 參數／發送排程、Servo 或電源模式。
MCU 溫度不是電池／環境溫度；沒有新增電流感測器或自動過熱停機。
本次使用者於實作期間拔除 Client 電池；USB 供電的 ADC 電壓不能解讀成電池存在、電量、
淨充電電流或續航。低電壓保護、斷電重啟及實際溫度精度不是本輪驗收項目。

## MCU 溫度

- 每 30 秒機會式取樣：只在既有主迴圈醒來且 radio idle 時開始，不新增待機週期喚醒。
- SoftDevice 開啟時檢查 `sd_temp_get` 結果；未開啟時使用 TEMP START/DATARDY/STOP。
  直接 peripheral 採非阻塞 10 ms 逾時，量測期間最多 1 ms task wait，成功／失敗均 STOP。
- 每次開始先使前次讀值無效。成功值 -40..85°C、取樣未滿 60 秒才可傳；失敗為 INT8_MIN。
- USB 保留 0.25°C 量化值、開機後最高值、樣本年齡與失敗計數；解析度不是精度。
- 原 11-byte TEL 使用 humidity=254 標示 MCU 溫度、無濕度；不新增封包／發送頻率。
  舊解碼器拒絕 254，因此先更新 Station，再更新 Client；舊環境 Client 仍支援。
- Station 舊環境 `temp_c`／`humidity_pct` 不填 MCU 數字；新增明確 MCU 欄位與 Surfer Info 三列。
  溫度接收逾 60 秒後顯示 --；Ready 不增加 TEL。最高值只統計 Station 收到的有效樣本，
  換綁定、Client boot 改變、Station 重啟時重置，並非 MCU 的完整歷史最高。
- 接收年齡不是取樣年齡：未新增來源時間 bytes，因此不宣稱精確端到端溫度年齡。

## 異常 NMEA 證據

`include/gnss_snapshot.h` 只補拒收原因，不改既有接受／拒收、fix、epoch、速度與座標政策。
T096 `nmea_evidence::Store` 在同一真實 UART feed 前保留原始 bytes：包含 `$`、CR/LF、非法 byte；
正常句子與正常 no-fix 不保存。固定 4 筆環形 RAM，每筆最多 160 bytes，記錄自開機毫秒、
遞增 id、reason、length、truncated。超長／非法字元的紀錄是到拒收點的前綴，明示截斷；
重新遇 `$` 或 UART backlog 的半句也保留。無 Flash 寫入，斷電／燒錄即清除。

reason：framing、checksum、fields、time、coordinates、epoch、non_ascii、overflow、resync、backlog。
計數 total／overwritten 不等於 parser rejectedSentences：半句重同步也可留下證據。
記錄採最近四筆，不去重，避免掩蓋反覆錯誤；覆寫數可知證據缺口。

115200 USB CDC 唯讀指令：

- `DIAG STATUS`：`@DIAG boot=... fw=... mcu_valid=... mcu_qc=... max_qc=... age_ms=... failures=... nmea_total=... overwritten=...`。
  mcu_qc/max_qc 除 4 為°C；mcu_valid=0 時不可使用 mcu_qc；無最高值時 max_qc=INT32_MIN。
- `DIAG NMEA`：固定快照，`@NMEA BEGIN` → 每筆 META/DATA hex chunks → END。
  邊匯出邊新增錯誤不改已匯出的快照；USB 無空間時保留待送行並回主迴圈，短寫接續。
  原有正常 log 可以插行，接收端只辨認 @NMEA；無清除／寫入／切模式指令。

使用 `tools/read_t096_diagnostics.py --serial <實際USB序號> --out <新JSON路徑>`：
用已安裝 pyserial 的 PlatformIO Python，核對身分而非 ttyACM 編號，不觸發重啟。
匯出驗證 BEGIN/END、boot、筆數、ID、offset、原始 byte 長度；缺資料失敗，不能靜默拼接。
輸出為 owner-only 新檔，保留 raw_hex 與原始 USB chunks；原始句子可能含私人座標，不能提交 Git。

## 驗證／部署

### 主機與 build

- `python3 tools/verify_project.py`：39 組 Python／Node 全部通過；首次 native 尚保留
  humidity=254 一律非法的舊斷言而失敗。依新契約補上 -40..85 全範圍、無效值與
  101..253 仍拒收後，單獨重跑 `pio test -e native`：140/140 通過。
- 實際 TEMP／USB 函式擷取測試含成功、SDK 失敗、非阻塞逾時 STOP、millis wrap、
  radio busy 不取樣、短寫／背壓；ASan／UBSan 通過。真實 GNSS parser 測原始 bytes、
  no-fix、不合法時間、checksum、非法 byte、overflow、環形覆寫與不可變匯出快照。
- 真實 Station 接收測試驗證 MCU 標記／最高值及舊環境欄位隔離；UI 測試驗證有效、
  逾期、無效與舊環境資料。最後 STATUS 加入版本號後已重跑健康診斷測試。
- `t096-client`、`tbeam-station` build 成功；`git diff --check` 通過。
  既有 SDK／依賴編譯警告仍存在，不宣稱零警告或空白 OS 重現性。

### 部署映像與現場證據

| 裝置／最終映像 | bytes | SHA-256 |
|---|---:|---|
| F035 application DFU ZIP | 125639 | `c3989110420b9e8d9ba062cb72549ecd6d59feac51bc601befff8776f6a3d6c8` |
| 584C Station application BIN | 1648480 | `15f8657d094ec09982bd08a23b68e870bed05953d2920bf534b2e9934f6f066b` |

- 依 USB serial／MAC 確認 F035／584C，不依 tty 編號。
  Station 先更新；初次 OTA 傳輸成功，但 workspace 私人 Wi-Fi 設定與 584C 原網路不同，
  導致 HTTP 暫時離線。使用本機保留的原網路設定重建上述最終 BIN，USB 只寫 active
  app0 `0x10000`，晶片 hash 核對成功；bootloader、NVS、otadata 未改寫。
  workspace 私人 Wi-Fi header 已還原成作業前內容，不將密碼納入 Git。
  **重建部署 584C 時需套用該裝置的私人網路設定，不能直接使用 workspace 預設設定。**
- F035 使用 application-only ZIP，DFU 回覆 `Device programmed.`；保留 bootloader／SoftDevice。
  1200-baud touch 斷線曾回報 EPIPE，隨後以同 serial 的 DFU PID 確認後完成寫入。
- Station HTTP／USB 實報 `0.6.1-dev`，boot `412603713`；F035 USB 同版本，boot
  `2690381520`（`A05BF6D0`）。Station 綁定 F035、B 組 SF9，手動 179.5°／60°/s 保留。
- USB 首次 MCU 29°C，後續 28.75／30.75°C，感測 failures=0；Station 經原本 TEL
  收到 29→31→32°C，最高值同步更新。64.82 秒 HTTP 觀測所有成功樣本均維持同一
  Client boot／Tracking confirmed，Station rx_error=0；診斷 tx_errors=0、backlog_drops=0。
  舊環境 temp_c／humidity_pct 維持 null。
- 實際瀏覽器 Surfer Info 顯示 MCU／最高值 32°C 與接收年齡，Station 另顯示原本環境
  溫濕度；伺服器送出的 HTML 81978 bytes 與來源完全一致。
- USB 原始 NMEA 匯出通過筆數／id／offset／長度／boot 核對；初始 Ready 無紀錄，
  Tracking 匯出 total=50／overwritten=46，後續 total=236／overwritten=232，各保存最新四筆。
  首批四筆為完整 CR/LF 的 GNRMC／GNGGA、reason=time，原始 UTC 欄位空白。
  RF 診斷另見 checksum_errors=1；四筆環形證據未保留該 checksum 原句，不能推定原因。
  目前室內沒有有效 epoch／fix，不能將 UART 有輸出當成 2 Hz 有效定位或戶外驗收。
- SD 恢復 recording，errors=0、packet_dropped=0；中途 sample_dropped=5、最終為 6，未宣稱完全無掉樣。
  未改 Servo 追蹤、校正或實際角度驗收。

### 最終狀態與驗收界線

啟動／停止均由匹配的 Client STATE 確認（command #1／#2）。最終同一 Client boot
維持 Ready／confirmed／last_error=0；Station 同 boot、manual 179.5°／60°/s、SD recording。
最後一筆 MCU TEL 接收年齡 60471 ms 時，API current=null／fresh=false、最高值保留 32°C；
實際瀏覽器同步顯示 MCU `--`、最高 32°C、接收年齡 82 s。Ready 不新增溫度 TEL。
最後 USB 同 boot 讀到 30.5°C、Client 開機最高 32.75°C、failures=0；
NMEA total=368／overwritten=364，最後四筆仍可完整匯出。
電池欄位仍是既有 ADC 換算，USB 無電池時網頁仍可能顯示約 4.15 V／100%；硬體沒有
經驗證的電池存在判斷，此數字不是本輪電池驗收結果。
本輪沒有接回電池、量測整機電流／耗電增量、校準絕對溫度或進行戶外 GNSS／續航測試。
MCU 取樣使用既有喚醒與 TEL，NMEA 僅固定容量 RAM；實際省電效果仍須電表／功率分析儀驗證。
私有部署 log、原始 USB／NMEA、HTTP 快照、映像及 SHA manifest 留在本機
`~/.local/state/shore-spotter/health-20261022/`，不得提交 Git。
