# 2026-10-07 T096／LilyGO Station SF9 更新

使用者授權 SF9＋18B 實作、驗證後燒錄。目前 USB 為 T096 F035 與 LilyGO E91C Station。

## 範圍

- B 組 923.8 MHz 由 SF10 改 SF9，BW125／CR4/5／preamble 8／explicit header／CRC 不變；A 組保留 SF10。
- wire v5／DATA 18 bytes／E7 座標、GNSS 2 Hz、T096 500 ms 最早時槽及控制插入保持不變。
- 既有 85 ms preamble listen／800 ms RX 上限與 Station 40 ms command start deadline 保持不變。
- SD session 使用實際 profile 的 SF／頻率，避免外測誤標；既有原始檔不修改。
- SF9／SF10 不互通。僅部署本次 USB 兩台；其他 B 組 Station 需另行更新才能接收。
- 最新裝置來源位於主 checkout，含已驗收的 TFT 修正及電量／按鍵功能；保留全部既有 WIP，
  本次 RF patch 同步套用工作分支與最新來源後，以最新來源 build／部署。

## 外測 DATA 到達率的解釋

2026-10-06 22:56:49–22:57:19 收到48包DATA，seq26–73連續、CRC0，仍只有1.6包／秒。
T096 Tracking 的500ms時槽由DATA、約5秒STATE與約5秒輪替診斷共用，約每5秒剩8個DATA時槽；
各類TEL／DIAG／GNSS約15秒一包，與T-Beam Client的60秒診斷排程不同。
本次只降低SF，保留上述排程；不能把1.6／2當作80%收包率，也不會自動提升到2包DATA／秒。

## 驗證與部署

- 主機 native **139/139**、10組Python回歸通過（device config共9個case；packet／Station downlink／T096 radio、position、GNSS、display、power／SD worker／Client SD）。
  profile測試以loader產生的定義編譯實際radio adapter，確認A仍SF10、B為SF9、wire v5／DATA18B不變；SD測試覆蓋A/B與停止後重啟。
  T-Beam Client排程模擬同時跑SF10與SF9；不把該模擬的2Hz當作T096現行DATA發送率。
- 最新來源的T096與LilyGO Station build成功，沒有專案source警告。Station application 1,651,920 bytes；
  T096 application 121,428 bytes、DFU zip 122,230 bytes，package僅application，含DFU版本metadata。
- LilyGO E91C先停止／排空SD，備份0x0000–0xFFFF設定區，確認原partition table與build一致、app0為作用槽，
  ROM驗證舊application與保存的SF10映像一致。只寫0x10000 application，esptool hash verified。
  首次RTS reset後無API／SD回覆，補一次watchdog reset後正常開機；沒有再次改寫韌體。
- T096 F035核對USB serial後application-only DFU成功；USB identity明示SF9，新boot為4156024021，與Station收到的STATE一致。
  Station新boot 2525938483；API回讀923.8MHz／SF9／BW125／CR5、DATA18B，向上取整airtime為DATA186／TEL145／DIAG165／GNSS268ms。
- 桌上RF測試收到**110個18-byte probes**；SD raw counter正好1–110，缺號／重複／倒序0，最大間隔1000ms。
  STATE插入可造成1秒間隔，不能當作漏包。Test／Stop均confirmed；最終T096 Ready、last_error0，Station保持Manual90°。
  此段HTTP輪詢有1次讀取錯誤，RF counter未缺號，沒有把HTTP錯誤算為RF遺失。
- SD `/logs/0000000000000026-0000.ndjson` 匯出943,429 bytes，CRC32 `c27fc7b1`，577行JSON可解析。
  session正確記SF9／923.8MHz／wire v5；19個STATE＋110個probes、packet event無缺號，無radio_error事件。
  卡寫入errors／packet_dropped0；sample_dropped1屬快照丟棄，非RF丟包。首次USB讀取遇殘留錯誤回覆，清空後匯出完整且CRC通過；
  USB write error counter為1，保留原始紀錄。匯出後已另行確認SD恢復recording。
- NVS配對F035、Axiom停用／無token及Servo Manual90°均在新boot確認。146個本次未修改的既有來源檔保持原hash；
  既有T096面板修正、電量、按鍵與其他WIP保留。

本輪為USB供電桌上RF／命令／SD驗證，沒有進入GPS Tracking，沒有宣稱有效GNSS定位、海面距離、
外測收包率或Servo實體追蹤通過。其他B組Station尚未燒錄，仍須同步SF9才能與本次T096通訊。

原始來源快照、舊韌體／設定備份、最終產物、build／燒錄／API／USB／SD證據保存在
`/home/nolanasd123/shore-spotter-debug/2026-10-07-sf9/`（私人，不提交原始位置／網路設定）。

最終產物SHA-256：

- `tbeam-station/firmware.bin`：`c16551743f1092f43354769195955a397fae5dbe670c81db25a7f3ab0bc2ac01`
- `t096-client/firmware.zip`：`729dcb60ae85641053412abebb6055ad913b129e24c915274464dc61b1316711`
