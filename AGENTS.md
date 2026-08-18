## DOCS
- `docs/interface.md` — LoRa 封包格式 + 攝影站 HTTP API + 欄位對應表
- `docs/features.md` — 韌體功能/流程/狀態/OLED 顯示
- `docs/hardware.md` — 腳位與接線
- `include/protocol.h` — client/server 共用的 LoRa 封包定義
- `include/geo_math.h` — 純數學（角度/方位/圓擬合/電量/GPS 分級），不依賴 Arduino
- `include/alerts.h` — 現場提醒的門檻與分級，不依賴 Arduino
- `test/` — `pio test -e native` 在筆電上跑的單元測試（測上面兩個 header）
- `include/wifi_config.h` — 手機熱點 SSID/密碼設定（SERVER 開機連線用）
- `include/web_ui.h` — 內嵌監控頁
- `include/web_icon.h` — 分頁圖示 PNG（**產生檔，勿手改**）
- `tools/make_icon.py` — 圖示的唯一來源；改設計後重跑會更新 web_icon.h

## RULES:
+ 將可能產生大量log的指令交給我執行,節省token
+ 使用中文回答,也可以中文夾雜,不用將專有名詞刻意翻譯