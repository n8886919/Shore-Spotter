# GPS／LoRa／追蹤更新：2026-09-19

狀態：0.6-dev／LoRa v5；下列已確認決策現已套用原始碼並完成主機驗證。
本次已 build Client／Station，先 USB 燒錄 Client，再依追加授權 USB 燒錄 Station；未執行雲台動作測試。
雙端必須使用 v5／SF10 才能互通；GNSS 115200／2 Hz RMC＋GGA 仍須實機與外測驗收。

## 本批實作與驗證（2026-09-19）

- 18-byte 全域 E7 DATA、36-byte 單包 GNSS 診斷；無 ACK，SF10，60 秒低頻診斷，版本升為 v5。
- 本地定位以新 epoch 首次到達後時間判斷更新逾時；遠端用新 DATA 接收後時間。
  推算 source age 只供診斷；重複、倒退、格式與 UART 積壓檢查保留，無舊定位重傳。
- 最新 epoch 直接可見，缺句保持未知；Client 最多等 150 ms 配對。品質差仍追有效位置，品質只限制外推。
- `gpspred5` 新設定鍵預設 α=0，舊 gpspredict 不沿用；不影響 Servo 速限。品質差／未知或速度無效禁止外推；
  選用 α=1 僅用接收後時間，API／Axiom 另列 prediction_active。原速度／方向保留，不改成 0。
- API v5 DATA 來源／合計年齡與推算來源頻率為 null；本地 arrival_age_ms 與接收 rx_age_ms 語意分開。
  raw 封包容量增為 36 bytes，Client API／Axiom 保留 E7 座標；版本／包長由共用定義提供。
- 主機驗證：native 129／129；135 秒 2 Hz 來源模擬 267 包有效 DATA、9 包診斷，最大 DATA 間隔 984 ms，
  所有發射 guard ≥80 ms。驗證最新一筆、單次失效、無 GPS 診斷、來源估算偏移不擋新位置、重複不刷新期限、
  v4 拒收、E7 範圍／精度、36-byte raw、品質差仍追／禁止外推、α=0 舊設定遷移、失效轉完目標與暫停撤銷。
- packet／motion／debug／Axiom 主機整合、GNSS 初始化／供電路徑，以及 motion／debug／Axiom／slider UI 通過。
  Debug 普通 fixture 最大 4878 bytes；Axiom 快照主機 RAM 1200 bytes、222／256 欄位，最大配對紀錄 5810 bytes。
- 最終 Client／Station／Station OTA build 全部成功，無專案 source 警告；保留 RadioLib 的 USB CDC 既有提示。
- USB 以序號／esptool MAC 確認 Client E91C，經 /dev/ttyACM0 燒錄成功，Flash hash verified；
  upload 後映像 SHA-256 與 build 留存完全一致。Station 部署證據見下方追加紀錄。
- 燒錄後 45 秒 USB 開機／運行紀錄：v0.6-dev、protocol=5、SF10、20 dBm fixed、ALDO3 3300 mV readback 通過；
  RadioLib 回報 airtime DATA/TEL/DIAG/GNSS=330/289/330/494 ms，診斷間隔 60000 ms。
  初始量測窗 epoch／RMC／GGA 各 1.80 Hz，後續連續 5 個完整量測窗三者皆 2.00 Hz，epoch 間隔 500 ms。
  最新摘要 tx=0、errors=0、diagnostic_tx=3、backlog_drops=0；尚未觀察到有效定位 DATA，不能據此聲稱戶外定位、
  RF 收包／距離或機械追蹤通過。這是短時間實機輸出證據，不取代 10 分鐘及 fix／失效／恢復驗收。
- 本次修改前快照 `/tmp/shore-v5-baseline-yax9lxmy`；27 份檔案修改，49 份既有檔案未變，包含既有 VS Code WIP。
  build 映像／SHA-256 manifest 留於 `/tmp/shore-v5-build-20260919`；build、upload、serial 證據為
  `/tmp/shore-v5-build-final.log`、`/tmp/shore-v5-client-upload.log`、`/tmp/shore-v5-client-serial.log`。
  Client image SHA-256：`0edc4a3fb7c464e695e998a0a4142c7877bb349483ea151001653011d3ef9576`。

### Station USB 部署（同日追加授權）

- 以 USB 序號與 esptool MAC 確認 Station **584C**；燒錄前 API 為 v4／SF9、9 月 15 日 build。
  上傳前逐一核對 src／include 與已驗證映像；USB 燒錄成功、Flash hash verified，映像未變更。
  Station image SHA-256：`749cc731032a673ab19c63827e4467d10c273acf2a6e5665f47e9d58cd3d9117`。
- 50 秒 USB 紀錄確認 ALDO3 3300 mV readback、boosted RX gain 初始化成功、SF10／無 ACK、
  airtime 330／289／330／494 ms、115200／2 Hz 請求及 60000 ms 診斷設定。
  HTTP 確認 0.6-dev／v5、18-byte DATA／36-byte GNSS；部署網頁 83687 bytes 與原始碼完全一致。
- 綁定 E91C、Servo 30°/s、校正及 Axiom 啟用／Token 設定保留。預測 α=0，UART／hold，
  angle／target 均為 90°、未移動、無 PWM fault；未發送模式切換或移動命令。
  Axiom 重啟後恢復 HTTP 200，約 57 秒時 sent_samples=46、dropped／failed=0；
  這是 Station 上傳成功證據，不代表已查驗 Axiom dataset 內容。
- 岸端 UART 持續收到語句，但缺少有效 UTC epoch，約 57 秒時 missing_or_invalid_time=198、
  epochs=0、checksum_errors=0、backlog_drops=0，尚無有效定位。燒錄前亦為 no_epoch、
  missing_or_invalid_time=417；不能將目前 0 Hz 判為燒錄造成，也尚不能驗收岸端 2 Hz。
  延長到開機約 135 秒仍未收到 Client DATA／診斷，radio_errors=0、未再次重啟；
  未確認 Client 當下供電，不能據此推算 RF 失敗率。
- 證據留於 `/tmp/shore-v5-station-upload.log`、`/tmp/shore-v5-station-serial.log`、
  `/tmp/shore-v5-station-before-*.json`、`/tmp/shore-v5-station-after-*.json`、
  `/tmp/shore-v5-station-final-*.json`；build manifest 已追加 Station 部署紀錄。

## 已實作決策：直接追最新位置

使用者已確認，以衝浪實拍能否留住人、cutback 後能否追上為優先；精確延遲量測後置。
此決策已取代先前以推算來源年齡作追蹤 gate／預測的實作：

- Client 檢查 GPS 時間確實前進、定位有效，盡快發送最新一筆；保留重複／倒退、讀取積壓、格式與失效檢查，不排隊補舊座標。
- Station 收到通過格式、綁定、序號與定位有效性檢查的新位置即更新目標；不以尚未驗證的 GPS 年齡估算淘汰新位置。
- 先採 α=0，不做直線外推；保留 Servo 共用速限、失聯後轉完最後有效目標即停，以及手動暫停／故障撤銷移動。
- 失聯使用最後一次收到新定位後的本機經過時間；不增加 Client／Station 對時或 PPS，不把未知 GNSS 內部延遲當精確值。
- 保留必要診斷，實拍比較人在畫面內的比例與 cutback 後追上時間，再決定是否重新啟用預測。
- 新版 Station 首次使用新設定鍵採 α=0；本次已 USB 部署並以 API 確認關閉預測。

## 已實作決策：品質警告與速度外推分開

- 有效新定位即使衛星顆數少或 HDOP 差，也繼續更新追蹤位置並顯示／記錄品質警告；不再以兩端 Good／OK 作直接位置追蹤的必要條件。
- 定位明確無效或新位置逾時時，仍轉完最後有效目標後停止；損壞、越界、重複／倒退封包拒收且不刷新失聯計時。校正、模式、手動暫停與 PWM 故障條件保留。
- 使用者追加確認：品質差時禁止使用速度／方向外推位置，仍可直接追最新有效座標。此處指 GPS 位置外推，不改 Servo 共用速限，也不新增相鄰座標差分測速。
- 先沿用既有 Good／OK 門檻作外推資格：兩端衛星 ≥6、HDOP ≤3 且品質欄位有效；品質未知也禁止外推。這是控制策略門檻，不是速度準確度的實測保證。
- GPS 回報的速度／方向保留於封包與診斷；分開表達數值有效性與控制上是否允許使用，不因禁用外推而把原始速度改成 0。
- 本批既定基準仍為 α=0，品質好也不自動開啟預測。若後續使用者開啟預測，品質不合格即退回直接位置；恢復須有新的合格定位與同 epoch 有效速度／方向，不沿用降級前的速度。
- 原始碼／主機流程已驗證；板上部署狀態見本文件開頭。

## 已實作決策：封包定版

- 定位包改為 18 bytes：6-byte 標頭／ID／序號、各 4-byte 的完整經緯度、1-byte 速度、2-byte 方向／狀態旗標、1-byte HDOP；移除原 DATA 年齡欄位。
- 完整座標採 1e-7 度整數刻度，取消固定原點／signed24 範圍；這是編碼解析度，不能當成現有 GNSS 已有公分級實際精度。
- 速度／方向先保留供記錄與後續外推對照；目前追蹤採 α=0。
- GNSS 診斷三頁合為單包，約 36 bytes／60 秒；移除三頁組裝狀態，不把低頻診斷塞入每筆定位。
- 協定升版，Client／Station 一起更新；舊版格式拒收，API／log 同步更新欄位語意。
- SF10／BW125／CR4/5、preamble 8、explicit header／CRC 下，18-byte DATA 計算 airtime 329.728 ms，36-byte GNSS 診斷 493.568 ms，均未含額外 80 ms guard；仍待實機收包驗證。
- codec／整合測試已完成；既定 115200／2 Hz RMC＋GGA 驗證版仍需實機驗收，不自動降頻。

## 排程澄清：GPS 來源 2 Hz，LoRa 盡快送最新一筆

- 使用者比較 5 Hz 後確認維持 2 Hz；本批不改 5 Hz。
- GPS 請求 2 Hz 是來源更新頻率，不要求 LoRa 固定每 500 ms 發送，也不要求 Station 每秒必收到兩包。
- 持續服務 GNSS；LoRa 非阻塞傳輸期間仍接收、解析新資料。radio 空閒且必要 guard 已滿，就選最新有效且未送過的定位。
- 待送位置只保留一筆，新位置覆寫舊待送位置；來不及的中間定位直接略過，不排隊補送。已起送的封包正常完成，不因新定位中止 TX。
- 診斷、radio 忙碌或 RF 漏收導致 Station 接收率低於 2 Hz，均不能單憑此判定 GPS 2 Hz 不穩，也不因此降低 GPS 來源頻率。
- 必須另行實測的限制是 L76K 原廠 >1 Hz 要求 115200／單語句，而目前選擇 RMC＋GGA 驗證版。若模組實際缺句或達不到來源頻率，明確記錄後查原因，不與 LoRa 排程混為一談。

## 上一批 v4 實作基準（歷史，下列年齡門檻／三頁診斷已由 v5 取代）

| 項目 | 行為與理由 |
|---|---|
| GPS | 雙端 L76K 請求 115200 baud、500 ms 更新、RMC＋GGA；每 5 秒量測 epoch／RMC／GGA 頻率，缺語句／不符明示，無靜默降級 |
| DATA | 新鮮且可編碼的新定位、radio 空閒且滿足 guard 就送；無 1 Hz 人為上限、無佇列、無舊定位重傳；RMC／GGA 最多短等 150 ms |
| 無定位 | 有效轉無效只嘗試一次 fix=0 通知；持續無效及開機未定位不送 DATA。恢復直接送有效新定位；Station 仍以年齡逾時判斷 |
| RF | SF10／BW125／CR4/5、Client 固定 20 dBm、移除 ACK；兩端初始化／復原先確認 ALDO3 3.3 V，Station 開 boosted RX gain |
| 診斷 | TEL／DIAG 各約 60 秒，GNSS 約 60 秒一組三頁；透過 LoRa 送到 Station，沒有定位也送。診斷最多一包／秒、有 DATA 時交錯，可延後一筆定位 |
| 新鮮度 | 保留來源 age、airtime、接收後年齡與 200 ms UART 不確定量；有效門檻仍為合計 <2 秒，不把舊位置重新標成現在位置 |
| 序號 | 連續收到 DATA 時拒絕重複／倒退；距最後接受 DATA 滿 3 秒，第一包有效位置直接重建基準並記錄原因。無 boot ID／握手 |
| GPS 超時 | 停止外推／重算，依共用速限轉完最後有效目標，再停住；手動暫停、模式切換與 PWM 故障仍撤銷移動 |
| 岸端定位 | 最近 30 秒新鮮新 epoch 座標平均供追蹤幾何使用；保留原始值與 RMS 散布，RMS >3 m 只警告、不丟離群點。追蹤仍受原始定位品質與年齡限制 |
| 校正 | 維持北 0°、東 90°、西 270°，不改原公式 |
| 協作 | `AGENTS.md` 記錄批判態度：檢查雙方假設、提出整體優先順序、區分證據層級，讓異常可見，避免擴大未確認行為 |

低頻診斷不是直接送 Log Station：Client 先走 LoRa 到岸端，岸端再依是否啟用 Axiom 上傳。
兩者獨立；Axiom 關閉不會關閉 LoRa 診斷，Wi-Fi 斷線也不代表 LoRa 失聯。

## 外測紀錄能支持的結論

9 月 18 日 21:42:07–22:19:23 的 Axiom 紀錄仍來自 9 月 15 日 build 的舊 SF9 韌體。
以下是在各分析區間相對座標中位中心的散布，**不是相對真值的定位誤差**：

| 區間 | 有效岸端座標樣本 | 徑向中位數 | 徑向 P95 | 最大半徑 |
|---|---:|---:|---:|---:|
| 全段 | 1684 | 1.67 m | 9.86 m | 11.94 m |
| 22:00 後 | 960 | 0.90 m | 1.44 m | 1.83 m |

全段有緩慢位置偏移，22:00 後較集中；不能說整晚都飄十多公尺，也不能把後段集中當成位置準確。
30 秒平均可減少短期晃動，但共同偏差／慢漂移仍可能存在；移動腳架後也會需要時間跟上。
RF 接收錯誤、DATA 序號缺口、拒收、Axiom 丟樣本及手機錄製缺口是不同問題，不可合成一個失敗率。

## 上一批 v4 驗證與界線（歷史）

- Native 純邏輯：130／130，包含新定位排程、3 秒序號基準、時間回繞、30 秒平均／RMS 及頻率判讀。
- 實際 Client 發送與 Station 解析主機整合：135 秒、2 Hz NMEA、264 包有效 DATA、15 包診斷，
  相鄰發射保留至少 80 ms guard，最大 DATA 起送間隔 859 ms。無歷史定位補送；無 GPS 時仍有診斷。
- 實際控制流程：失效邊界不再更新目標、繼續限速到達最後目標；恢復、手動暫停／模式切換、PWM 故障有檢查。
- 實際 GNSS 初始化：命令 checksum、冷啟動 9600 與 GNSS 留在 115200 的模擬皆通過；這不是接收器回覆或實際 2 Hz 證據。
- ALDO3／boosted RX 設定與失敗路徑、debug JSON、Axiom encoder／worker／API、既有 UI 檢查通過。
  Debug 普通最大 fixture 4755 bytes；Axiom 快照主機 RAM 1032 bytes、欄位路徑 220／256，板上大小仍以 API 為準。
- Client／Station／Station OTA build 通過；僅保留依賴庫／framework 警告。尚未驗證實際供電、GPS Hz、RF 距離或雲台機械效果。

## 下一步按優先順序驗收

1. **先證明 GPS 輸出完整**：雙端冷啟動及單獨 MCU 重啟，確認 epoch／RMC／GGA 各約 2 Hz、同 epoch 配對、缺句及 checksum／backlog 計數；
   連續觀察至少 10 分鐘，包含有 fix／無 fix／恢復。不得把偶爾一個 `observed_2hz` 當整段合格。
   依 [L76K 原廠協定](https://forums.quectel.com/uploads/short-url/kmb3zNuV2SldThkOOJoNg9th40S.pdf)，>1 Hz 的前提是 115200 且只開單一語句；雙語句失敗時需再討論保留完整欄位或替代模組／協定。
2. **建立 RF 對照**：記錄同路線距離、天線高度／姿態、遮蔽、RSSI／SNR、DATA seq 缺口和 TX 錯誤；
   固定天線位置，先測開闊陸地，再增加人體、近水面／水的條件。優先核對 923 MHz 天線與接頭、保持天線在水面上、提高岸端天線並維持一致極化。
   幾公里的宣傳條件不能代替實際安裝條件；SF10／boosted RX／ALDO3 是否改善需新舊條件對照。
3. **再驗證追蹤**：岸端靜止 10 分鐘看 raw／mean／RMS；Client 靜止、直行、折返，先關位置預測建立基準，再比較預測。
   同時記錄實際畫面與角度方向，軟體 PWM 角度不是鏡頭實際角度回授。做一次斷訊，確認轉完有限目標而非持續外推，且暫停能取消。
4. **最後才決定調參或升級**：在同路線資料支持下決定 SF11／低 GPS 率／不同封包組合；目前不增加 App、遠端 SF 切換、睡眠或 RTK。
   是否需要原生 App 應依背景錄製／離線保存需求評估；App 本身不改善 RF。SF 遠端改動還需雙端同步／恢復規則，暫時不引入。

## 保留到下次討論的問題

- **年齡基準偏移仍只作診斷**：v5 已移除推算年齡對追蹤的 gate；不增加對時／PPS。
  來源推算值保留供後續研究，不把它當已量測的實際延遲。
- **3 秒重建是簡化取捨**：它不能識別真正重啟，也不能防止間斷後延遲舊包重建基準。當前 Client 不排隊、不重傳；若未來新增其他來源／轉送／重播，須重新評估，不能直接沿用。
- **診斷舊序號等待已移除**：v5 單包直接替換，仍需雙端重啟外測確認。
- **HDOP 換算「±公尺」只是估計**：UI 仍用固定 UERE 2.5 m，不是實測準確度，程式旁還有舊模組註解。
  後續可討論改成 HDOP／散布而非似乎精確的誤差圈，這次未變更呈現語意。
- **紀錄完整性仍有限**：Axiom 保留有限 RAM queue，手機／網路離線可能丟資料；完整軌跡與錄影背景執行是另一項需求。
  本次未新增 flash log、離線補傳或 App，也不把雲端取樣完整度當成 RF 完整度。

以上新問題先列出，待確認再改；目前優先穩定、可解釋、容易 debug，耗電優化後置。
