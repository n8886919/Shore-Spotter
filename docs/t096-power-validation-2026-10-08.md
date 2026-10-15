# T096 無線充電與收納喚醒驗證（2026-10-08）

> **歷史電源／SystemOFF 驗證。** 10/9 起日常操作已改為保留 RAM 的 RTC 待機；本文保留 10/8 當時故障與驗收，不能視為現行收納流程。現況見 [RTC 文件](t096-rtc-standby-2026-10-09.md) 與 [交接入口](assistant-handoff.md)。

## 身分與接法

- Client F035，USB serial `C3BFA5F76F915E4A`；LilyGO E91C Station 保持既有配對與 SD 記錄。
- 接收 PCB 的 WOUT 接 T096 太陽能接口，BAT 留空；3.7 V 額定鋰電池接 T096 電池接口。
- 正確接線的 30 分鐘充電／10 分鐘離座測試均為 Client USB 拔除、Ready、不按鍵。
- `charging` 是 USB VBUS 判斷，並非太陽能接口充電電流或充電完成指示。

## 已觀察的電源結果

2026-10-07 正確 WOUT 接線後，使用者確認 T096 紅色充電燈亮。

| 階段 | 不同 boot/tx 去重後的電壓中位數 | 電壓範圍 |
|---|---:|---:|
| 2 分鐘離座純電池基準 | 3.880 V | 3.876–3.887 V |
| 充電第一個 5 分鐘 | 4.014 V | 3.987–4.023 V |
| 充電最後 5 分鐘 | 4.088 V | 4.077–4.102 V |
| 10 分鐘離座最後 5 分鐘 | 3.978 V | 3.969–3.987 V |

30 分鐘內有 60 筆不同 STATE，10 分鐘離座有 21 筆；均為同 boot `2356000195`、Ready、
STATE 年齡約 30 秒以內。HTTP 分別有 11／3 次逾時，後續恢復，未以 HTTP 錯誤推定 RF 丟包。
離座最後 5 分鐘仍比充電前高 98 mV，支持充電後電池電壓提升；沒有量得電池淨充電電流、容量或續航。
前一天 BAT 錯接的觀察不作本次基準。

## 收納過夜與故障重現

- 使用者確認過夜期間曾按鍵或使用網頁收納；不能把這段當作整晚 Ready 續航或 12 小時自動收納驗證。
- 隔日上午 Station 保留 Storage、舊 boot `2356000195`，STATE 已約 11 小時未更新；舊 3.980 V 不是即時讀值。
- 純電池離座短按 USER，使用者再度看到 BLE OTA，Station 未收到正常程式的新 boot／STATE。
- 單次 RST 恢復 F035／Ready，新 boot `230792480`；3 筆持續 STATE，約 3.944–3.951 V，40 秒讀取無錯誤。
- 接回 USB 後身分再次核對為 F035，正常程式 identity 為 SF9／923.8 MHz／0.6-dev。

## 修正與相容性

現行 `systemOff(USER, LOW)` 將 USER 用作 SystemOFF 喚醒來源，但原先沒有指定下次跳過 DFU 按鍵判斷。
上游 Adafruit nRF52 bootloader 0.9.0 定義 GPREGRET `0x6D`，在 DFU 按鍵檢查之前消耗並清除此標記，
並明載可用於 SystemOFF 喚醒。這提供應用程式修正方向；T096 實機支援仍須由版本診斷與實際 USER 喚醒驗收。

修正只在真正進入 SystemOFF 前設定標記，包含 USER／網頁／Ready 期限收納與低電量關機路徑。
SoftDevice 啟用時以 SDK clear/set，停用時直接設定 GPREGRET；準備失敗時取消待關機並保留供電與 Ready，
USB 記錄原始 SDK 錯誤。USB identity 補上 core 在開機時保存的 bootloader 版本與 reset reason。
正常程式運作時的雙擊 RST／1200 baud DFU 機制保留；收納後下一次開機會先消耗一次性 skip 標記。
LoRa 排程、SF9、GNSS、Station、Servo、電量百分比與螢幕文字保持不變。

上游來源：
https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.0/src/main.c

## 驗證與部署狀態

- `test_t096_power.py` 通過：實際 SystemOFF 入口必須先設標記；覆蓋 SoftDevice 兩種模式、
  舊 OTA 標記替換、SDK query/clear/set 失敗不關機，及既有電量／TFT／按鍵／USB 政策。
- `test_t096_radio_lifecycle.py`、`test_t096_gnss_uart.py`、`test_t096_display.py` 通過。
- `test_t096_position.py` 通過；`git diff --check` 通過。
- T096 build 成功（73.05 秒），專案 source 無警告；SDK header 有既有 unused-parameter 警告。
  application 121,764 bytes，ZIP 122,566 bytes，manifest 僅含 application 與 DFU 格式版本 metadata。
  HEX 僅涵蓋 0x26000–0x43BA3，沒有開機器／SoftDevice／UICR 寫入。
- 核對 F035 serial 後以 1200 baud 進入 DFU；USB 重枚舉早於 DTR clear 回傳，保留該主機錯誤紀錄，
  重新核對同 serial 的 0x239A:0x0071 才燒錄。application-only DFU 成功。
- 應用重新枚舉為 0x239A:0x8071；Station 接到新 boot `564744272`、持續 Ready，
  40 秒觀察有不同 STATE，HTTP 無錯誤，後續 tx 至 5。USB identity 明示 SF9、F035。
- USB 補抓到實機 bootloader raw `00000900`／版本 **0.9.0**、reset reason `00000004`。
  初次開啟 serial 沒收到一次性的 identity，不將此當成版本缺失；115200 baud 重建 CDC 連線後取得，
  同一應用 boot 保持不變。版本一致仍不取代 Heltec fork 的純電池 USER 喚醒實測。
- 部署 ZIP SHA-256：`fbe494724f4e957af9023c0f1e9fc238b03766030b105d065eab7060ea2f5d8d`。
- 第一輪純電池 USER 實機驗收通過：使用者拔 USB、離座，長按 USER 2 秒放開收納；
  Station 收到 boot `564744272` 的 Storage，USB 序列埠清單亦已無 F035。
  短按 USER 後使用者目視 F035／Ready，Station 收到新 boot `2255604815`。
  隨後 35 秒不同 STATE tx 2→3 持續更新、Ready／USB false、HTTP 無錯誤，3.987–3.991 V。
  這證明此輪實機 Storage→USER→正常程式恢復，尚未量得 SystemOFF 電流或驗證長期續航。
- 網頁收納入口已確認：同一純電池 boot `2255604815`，Station POST store 回 202 pending，
  隨後 command 1 confirmed／Storage／tx 7／USB false／last_error 0；5 秒後同 boot 與 tx、
  沒有立即返回 Ready，HTTP 無錯誤。
- 第二輪網頁收納→USER 喚醒實機驗收通過：使用者再次目視 F035／Ready，Station 收到新 boot
  `2403619104`；USB 清單無 F035。35 秒觀察 tx 2→3 持續更新、Ready／USB false、HTTP 無錯誤，
  3.984–3.987 V。兩次喚醒均未使用 RST；證明一次性標記可在下一次收納重新設定。
- 本次已驗收按鍵／網頁收納後的兩輪純電池 USER 喚醒，以及下述充電座上短按 USER 喚醒。
  12 小時自動收納、低電量真實電池門檻與收納時淨充電仍不能用 USER 喚醒推定通過；充電電流、
  SystemOFF／Ready 電流、接收 PCB 反向漏電、溫度、容量及續航仍未量測。沒有宣稱已具備下水條件。
- 12:08（台灣時間）使用者確認將網頁收納後的 Client 放上充電座，隨後目視 T096 紅色充電燈亮、
  螢幕未亮。放上後約 3 分鐘的 API 抽查仍為舊 boot `2403619104`／Storage／tx 8，未收到新的
  應用程式 STATE；這輪沒有觀察到充電座自動喚醒。紅燈是充電指示觀察，未量得收納時充電電流；
  API 的舊 3.987 V 也不是收納後的即時電壓。12:11 使用者確認已移離充電座並出門，座上短按
  USER 測試留待返回後進行，未執行長時間收納充電測試；既有計時 automation 保持暫停。
- 23:46 使用者返回後，API 仍保留 boot `2403619104`／Storage／tx 8 的歷史資料；
  這不是即時電壓，也不能單憑 Station 沉默證明離開期間沒有任何重啟或操作。
  使用者依座上短按 USER 流程回報 F035／Ready，Station 收到新 boot `870428536`。
  隨後 40 秒共 9 次 API 讀取，HTTP 無錯誤、同 boot／Ready／USB VBUS false／last_error 0；
  去重後 2 筆新鮮 STATE（tx 2→3），電壓均為 4.120 V，最大 STATE 年齡 26,147 ms。
  本輪座上手動 USER 喚醒驗收通過，不將座上 ADC 電壓當作休息後電池電壓或淨充電電流。
- 23:49:59 至 23:50:59，使用者確認移離充電座後，完成 60 秒電池接手觀察；USB 拔除、
  電池與 WOUT 維持接著、不按鍵。13 次 API 讀取均為同 boot `870428536`／Ready／
  USB VBUS false／last_error 0，HTTP 無錯誤，最大 STATE 年齡 27,268 ms。
  以查詢開始時間減去 STATE 年齡，排除觀察起點以前的 tx 5，再依 boot/tx 去重：
  觀察開始後的 2 筆 STATE 為 tx 6→7，電壓 3.994–4.005 V、中位數 3.9995 V。
  本輪座上 USER 喚醒後的離座電池接手通過；沒有觀察到重啟或 STATE 停止更新。
  一分鐘觀察不能代替續航、容量、收納電流、休息後電壓或充電電流量測；未安排額外計時。

私人原始 API／USB／使用者事件位於 `~/.local/state/shore-spotter/power-validation-20261007` 與
`power-validation-20261008`。修正前來源、可回復的 SF9 application DFU、build／部署證據保存在
`~/.local/state/shore-spotter/validation/2026-10-08-t096-user-wake`，不提交執行期原始資料。
