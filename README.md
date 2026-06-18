# Shore Spotter - Surf Tracking System🏄🏄🏄 
![alt text](image.png)
 > **衝浪攝影不求人**

 > **解放浪人的女友與衝浪教練**

 > **不需帶手機下水**

## 系統架構
### Surfer端
Surfer帶著搭載 **MAX-M10S-00B GNSS模組**的 [LILYGO T-Beam Supreme開發板](https://lilygo.cc/en-us/products/t-beam-supreme?srsltid=AfmBOoq3s9O-LDWzy9d2w7GJoa8A5vIrM3SJnSHUNvkenHu-Tx0hViCG)，透過低功耗的**LoRa**技術持續發送座標資料

### 攝影站端
將攝影設備裝在伺服雲台上，攝影站會接收並計算Surfer方位，控制雲台實現自動追蹤
- 攝影設備(單眼或手機)
- LilyGO T-Beam Supreme
- 2S鋰電池
- HobbyWing 5A 穩壓模組
- LCD顯示: 連線狀態 / 電量 / 濕度 / GPS狀態 / 監控頁面IP

手機連線至攝影站Wifi可進入攝影站監控頁面

## 為什麼選 LoRa？
衝浪環境對無線通訊有幾個特殊條件：距離遠、無遮蔽物、不適合攜帶手機。LoRa 在這個場景下的優勢在於低功耗與長距離，更重要的是它讓「下水端」的職責單純：只負責定位與傳輸，不需要維護網路連線。

相比藍牙或 WiFi，LoRa 在開放水域場域的穿透性與覆蓋距離更有優勢；相比帶手機或智慧手錶，這套設備的任務邊界更清晰，也更好做防水設計。

[LoRa封包設計](docs/lora-packet.md)
[攝影站端API](docs/api.md)

## 未來目標
+ 多Surfer追蹤
+ Auto Record\
  結合GPS路徑或影像分析，偵測「追浪」與「起程」事件，讓錄影自動觸發，不再依賴手動操作。

+ 純影像追蹤，不需要攜帶追蹤器\
  岸上相機透過影像追蹤自動鎖定目標。從「GPS 輔助追蹤」演進為「完全被動」的使用體驗——這也是整個系統最終想到達的地方。
