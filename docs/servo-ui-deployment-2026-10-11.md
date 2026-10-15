# 2026-10-11 M0 串口／正式資訊頁部署驗證

版本 0.6-dev、wire v5；使用者批准正式整合與燒錄，指定固定 M0、目標 LilyGO 584C。
本輪只更新該 Station，沒有燒 Client 或 E91C。原始碼位於主 checkout，其他 WIP 保留。

## 主機與瀏覽器

- native 140/140；實際 HTTP／UART frame／速限／保存／GPS gate、packet/downlink、SD worker、debug backend 通過。
- UI 速度整數驗證／取消、Maps 連結、STATE 新鮮度、完整圓手勢／端點／模式與地圖／雷達回歸通過。
- 正式 HTML 預覽：320 px 無橫向溢出，經度／緯度／誤差直排；99°/s 留在視窗並顯示非法整數範圍。
- `tbeam-station`、SHORE_DEVICE=station-lilygo-584c build 成功，Flash 49.2%、RAM 25.2%；依賴庫 USB CDC 警告保留。

## 實機唯讀證據

- application OTA 傳輸 exit 0；重啟後 control_boot_id 從 3541114487 → 3444833199。
- profile `youmi-359-uart-v1`；TX43／9600 8N1／RX disabled／M0；中位 179.5°，`#00P1500!`、sent=1、deferred=0、errors=0。
- 速限原 NVS 60°/s 保留，開機手動；沒有額外 Servo 移動／GPS 啟動命令。
- F035／61493 配對保留，Client boot 2467651202、Ready；兩次 STATE tx_packets 7778→7779，最後年齡 4.929 s。
- B 組 923.8 MHz、SF9、wire v5，DATA18；與當前 Client 同組收包。這是近距離收包，不代表外測。
- SD recording；packets 1→2、I/O errors=0、packet_dropped=0。啟動階段 sample_dropped=2，後次仍為2，沒有宣稱所有快照均保留。
- 部署 HTML bytes 與本機產物完全相同；SHA256 `038a7d524edef878132007d98f3d0378b9e60470d20480dc76b7b570b1bb615d`。
- `/api/track/prediction` 實機 404；資訊頁四區及 M0 固定顯示、JS error/warn 0。

## 未驗收

控制板沒有 ACK 或角度回授；上述 UART 指標不能證明 Servo 動作、方向、實際中位／端點或帶載。
GPIO43 是程式與板卡對照，使用者接線未另用儀表核對；M0 實體接口須目視確認。
GPS 目前關閉、Station 無 fix，沒有本輪實體 GPS 追蹤或真實位置誤差證據。
HDOP 不等於公尺精度，30 秒 RMS 是樣本散布；需已知參考位置才能驗證實際誤差。

執行期完整 JSON、build／OTA log、韌體 hash 與畫面保存在主機私人 state 目錄，不納入 Git。
