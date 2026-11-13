# BLE 待機、開機認領與手動選頻（2026-10-27）

目前版本 **1.0**，同主版本的 CL／ST 子版本必須互相相容；不相容變更升主版本。下方 0.7.0-dev 為歷史實作／部署紀錄，1.0 不改 BLE／LoRa wire 契約。

版本：**0.7.0-dev；2026-10-28 已部署 LilyGO 584C 與 T096 FB27**。本文件優先於舊版 Ready 每 30 秒 LoRa 回報／接收流程。
本次針對 T096 Client 與 LilyGO Station；LilyGO 不再作為本專案使用中的 Client。

## 使用流程

2026-10-28 原始碼 UI=8（FB27 已部署）：短按亮屏 60 秒與切頁，長按 5 秒放開切換 ACT／SLP，RST 重啟。
未認領也可本機 ACT；沿用 runtime 頻道（開機 A），不清 owner／boot，低電保護禁止 ACT。
Tracking 中可被首次 BLE Claim 認領；若與 Station 頻道不同，先 BLE Stop，再選頻與 Start。
操作、換 Station 競爭限制及本輪驗證／部署見 [維護介面紀錄](t096-maintenance-2026-10-28.md)。

1. Client 開機進入 Ready，GPS 關閉、LoRa 休眠；BLE 未連線時持續每約 10 秒發送可連線廣播。
   這是廣播間隔，不是只開放十秒。廣播時序與掃描可能使首次連線更久，不能保證十秒內完成。
2. Station 啟動 BLE worker 後，自動搜尋 NVS 已儲存的目標 Client ID；使用者不需另按呼叫。
   修改目標 ID 後也自動搜尋新目標。沒有目標時不認領其他 Client。
3. Client 接受本次開機第一個有效 Claim，記住 Station ID；這是應用層認領，不是 Bluetooth bonding。
   普通 BLE 連線、掃描或無效請求不取得控制權。回待機、BLE 斷線、Station 重開均不解除認領。
   同一 Station 可以重連；要換另一 Station，必須重新啟動 Client。認領不是身分密碼學驗證。
4. BLE 連線維持時，命令不必等下一次十秒廣播。Station 約每秒讀狀態；Web/App 經 Wi-Fi HTTP 控制 Station。
   Client 斷線不自行停止 Tracking，也不換 owner。
5. 「啟動」開 GPS 與 LoRa；「無線連線測試」只開 LoRa；「停止」回 Ready。
   Ready 不再有定期 LoRa STATE 或接收窗口。活躍狀態仍可使用已確認 owner 的 LoRa 控制路徑。

## ID 編輯與網頁狀態

Surfer ID 顯示為藍色底線文字。點擊開啟編輯視窗，取消不送請求，確認後才保存。
僅 Station 成功回覆後更新已確認 ID；拒絕、HTTP 失敗或舊輪詢回覆不能把畫面改成新 ID。
正在執行命令、掃頻或傳輸時拒絕更換目標。修改 Station 目標不會解除 Client 的既有 owner。
若目標已被別台 Station 認領，提示重啟 Client，不做強制接管。
HTTP 失聯、Client 尚未回報及明確未配對分開顯示，保留已確認 ID。

## 候選頻道與驗證

| 候選 | 頻率 | SF | BW | CR | Sync word |
| --- | --- | --- | --- | --- | --- |
| A | 923.2 MHz | 9 | 125 kHz | 4/5 | 0x12 |
| B | 923.8 MHz | 9 | 125 kHz | 4/5 | 0x12 |

沿用既有兩個候選，不增加其他頻點。這不是完整頻譜分析或法規驗證。

流程：**BLE 已認領且 Ready → 手動掃描 → 選擇 → 啟動 → 收到匹配 LoRa STATE → 保存**。
Station 每個候選執行五次 CAD；顯示偵測數／有效樣本與錯誤數，掃描結束還原接收頻率。
CAD 只能表示此次設定下的 LoRa 偵測，不保證沒有其他調變干擾或該頻道可用。
沒有有效樣本或還原失敗明確顯示錯誤。掃描期間不執行控制操作。

選擇先存在 RAM，不立即改 NVS。啟動／測試時兩端使用選定頻率；
只有「啟動」收到同 Client boot、owner、command 的實際 LoRa STATE，且 Station 在選定頻道，才保存。
BLE accepted、HTTP 202、測試模式回報都不直接保存。NVS 寫入失敗會回報錯誤。
尚未驗證的選擇於 Station 重開或修改目標後捨棄；保存的是這台 Station 的頻道偏好。
驗證失敗不自動跳頻；使用者停止回 Ready 後再選擇。

這個成功條件證明 Client 已進入啟動狀態，且 Station 收到該頻道的上行 STATE；
不代表 GPS 已定位、LoRa 下行已驗證、長距離可靠或 Servo 已正確追蹤。
Client 接收啟動失敗保留原始 RadioLib 錯誤供診斷，不撤銷先前已成功的上行證據。

## API 與雙平台 App

入口：[OpenAPI 子集](openapi-client-control.json)、[綁定 API](client-binding-api.md)。
App 只經 Wi-Fi HTTP 與 Station 溝通，不直接連 Client BLE；Web 與 App 共用相同端點及狀態判斷。

- `/api/whitelist`：讀取／保存 Station 目標 Client ID。
- `/api/client/control`：`api_version=2`；GET 狀態，POST query `action=start|stop|test`。
  舊 `store` 暫作 Stop 別名，不進 SystemOFF。命令回傳 202 表示受理，不能立即顯示完成。
- `/api/client/radio`：GET 候選／選擇／保存／掃描狀態；POST query `action=scan` 或 `action=select&channel=0|1`。
- App 追蹤 `station_boot_id`、`operation_id` 與命令狀態。只保留最新操作快照，沒有操作歷史查詢或冪等重試服務。
  HTTP 結果不明時先重讀；不能自動重送寫入來假定前次失敗。
- `state_source` 區分 BLE／LoRa／none；`can_control` 是當下可送出的條件。失聯與 ID 保存各自獨立。
  命令 pending 禁止重複提交；完成、錯誤、逾時分開顯示。現行命令期限為 65 秒。

BLE worker 使用固定容量佇列，避免同步掃描／GATT 阻塞主迴圈 HTTP 與 Servo 服務。
BLE wire v1 的唯一 codec 為 `include/ble_control_protocol.h`，命令 20 bytes、狀態 40 bytes；
MTU 設定／重連協商仍需實機驗證，不能僅依設定值宣稱成功。
Station 以 Client uptime 是否前進判斷 BLE 狀態新鮮度，不能靠重讀相同 bytes 延長存活。
命令綁定 Client boot、Station boot 及序號；拒絕舊開機／重播，兩來源舊狀態不能倒退新狀態。

## 記錄與相容性

SD session header 記錄開檔當時頻率及 SF9，`frequency_scope=session_open_snapshot`。
同一檔案內切頻另記錄 `[RF]` 事件；不能把 header 當成整份檔案的固定頻率。
LoRa wire v5 保留，但舊韌體沒有此 BLE Ready 流程；兩端需搭配升級，不能假設混版可正常喚醒。
T096 使用既有 Bluefruit/SoftDevice；板型沒有已確認專用 BLE LED，因此關閉連線 LED 並以越界 sentinel 保持 GPIO 不動。

## 驗證與未完成項目

主機驗證：140 個 native test cases 通過；46 個 Python／Node 測試腳本通過（一次全套及修正後相關重跑）。
全套第一次的三個失敗為舊 fixture 尚未提供新的 runtime channel／BLE 依賴；同步 actual-source fixture 後通過。
另外審查找到並修正 Ready 故障重試喚醒，以及 Station 初始化失敗後無法恢復頻道；
新增真實函式的故障→重試回歸，不能靠測試手動重設 active channel 冒充復原。
測試涵蓋 codec、owner／boot／重播、BLE／LoRa 狀態新舊、CAD 逾時、LoRa 證據後保存、
NVS 失敗保留、ID 編輯防誤觸、SD runtime metadata、既有定位／Servo／SD／USB 邏輯。

使用實際內嵌 HTML＋隔離 mock fetch（CSP 禁止連外）確認 ID 與頻道視窗、取消操作及手機寬度版面；
沒有向實際 Station 發命令。App 只完成 API 契約，沒有實作 iOS／Android App。

建置使用獨立目錄，避免多個 PlatformIO 環境互相清除輸出：
- T096：`PLATFORMIO_BUILD_DIR=/tmp/shore-ble-client-build-20261027 pio run -c platformio-t096.ini -e t096-client` 通過。
  RAM 19,676 / 248,832 bytes；Flash 196,260 / 815,104 bytes。
- LilyGO Station：`SHORE_DEVICE=station-lilygo-584c PLATFORMIO_BUILD_DIR=/tmp/shore-station-ble-build pio run -e tbeam-station` 通過。
  RAM 87,912 / 327,680 bytes；Flash 1,924,814 / 3,342,336 bytes。

以上為燒錄前的主機驗證；實機部署結果見下節。後續部署仍須重新核對目標身分與私人 Wi-Fi 設定。

必須另做實機驗收：首次認領與多 Station 競爭、連線／重連延遲、重啟保有/解除邊界、
兩個候選切換與保存、GPS 實際定位、LoRa 上下行、Wi-Fi/BLE 共存、Web 操作及待機電流。
本輪不宣稱電池續航改善，也不以 build 或主機 fake transport 取代硬體驗收。

## 2026-10-28 雙端 USB 部署

使用者明確授權「燒錄兩者」。兩端均完成 application-only 更新，保留設定與 bootloader。

- Station：USB／ROM MAC `48:CA:43:57:58:4C`，LilyGO T-Beam Supreme。
  以先前成功部署檔核對私人 Wi-Fi 設定後重建；工作目錄 header 在 finally 還原並比對一致。
  SD STOP 確認完成；備份 0x8000–0xffff，分區表與 build 一致，OTA seq 3／2 的 CRC 均有效，選定 app0。
  僅寫入 `0x10000` 的 1,925,216 bytes application；esptool hash 驗證成功，watchdog reset 後開機。
  未覆寫 NVS、otadata、分區表或 bootloader。
- Client：USB serial `F314D986FA637CF1`，USB 身分回報 `FB27`。
  ZIP CRC、application-only manifest 與 SoftDevice requirement 0xB6 已核對；
  1200-baud touch 後確認同 serial 的 `239A:0071`，115200 DFU 回 `Device programmed.`。
  回到 `239A:8071`，USB 回報 `v0.7.0-dev`、boot `240E79A3`（604928419）、bootloader 0.9.0。
  SoftDevice／bootloader 未重寫。

Station 新 boot `866644376`，build `Oct 28 2026 00:13:56`。
實機 HTTP `api_version=2`、BLE connected／claimed／Ready、`can_control=true`；
owner `22604`（584C），保存的目標仍為 FB27。
實機 HTML 與目前內嵌 HTML 逐 byte 一致；本次未另做實機瀏覽器視覺驗收。
Station 保持 manual／179.5°／60°/s，SD 恢復 recording；未送 Servo 移動或校正指令。

執行一次短暫 Test → Stop：HTTP 202 先顯示 pending，後續讀到 confirmed；
B 頻道 923.8 MHz 收到 5 個 test probes，missing=0，最後回到 BLE Ready／confirmed。
這確認實際 BLE 控制與 LoRa 上行；Test 不開 GPS、不將候選頻道寫入 NVS。
沒有執行 GPS 定位驗收、A/B 掃頻與跨頻保存、多 Station 競爭、長時間重連或耗電量測。

私人 artifact／USB／HTTP 原始證據位於本機 state 的 `ble-deploy-20261028`，不納入 Git。

| Artifact | SHA-256 |
| --- | --- |
| Station application | `aa50ba85a324819ea103b304c2901213df7d4713b9f0e9eee4737a65dfdc57e5` |
| T096 application DFU ZIP | `105180dff085dc387288a6f2eec0769d3f9ca4a83e5949d1b9eecaca4e32ddc0` |
| T096 HEX | `89970540a7053de138c069bc260cd6d13fe7b0708f960e7b8772ef9f0a287ca1` |
| Station embedded HTML | `c1bb71b7dd02c09ea8087147c5dfd41410dda4922837a0be99e6728cf4e13911` |

停止後連續觀察 36.0 秒，Client tx_packets 維持 12 → 12，BLE 全程 claimed，沒有觀察到舊版 Ready 定期 RF 發送。
最後 SD recording／errors=0；sample_dropped=91（快照丟棄），因此不宣稱記錄零遺失。
