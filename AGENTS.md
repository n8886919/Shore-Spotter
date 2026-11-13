## DOCS
- `docs/assistant-handoff.md` — 新 session 入口、目前已驗證／未驗證界線。
- `docs/next-session-plan.md` — GPS-only 優先驗證與程式精簡待辦；使用者最新觀察是第一次除 NETWORK GPS 外其他正常。
- `docs/records-index.md` / `docs/records.json` / `tools/read_record.py` — 已結束的實驗／部署／階段交接從 Git 取回。
  不因舊文件已移出工作目錄就判定證據遺失；例如 `python3 tools/read_record.py field-experiment-2026-11-14`。
- `README.md` / `docs/features.md` / `docs/function-entrypoints.md` — 現行流程與功能入口。
- `android/README.md` / `.agents/skills/shore-spotter-android/SKILL.md` — 原生相機／控制／GPS-only App；主機建置不等於已安裝。
- `cad/README.md` / `cad/reference.json` — 正式外殼在 `~/freecad/Shore-Spotter-Case/` 的獨立 Git commit／主檔 hash，不重複拷貝模型。
- `docs/interface.md` / `docs/client-binding-api.md` / `docs/openapi-client-control.json` / `docs/openapi-client-binding.json` — 現行 LoRa／BLE／HTTP 契約。
- `docs/hardware.md` / `docs/device-config.md` — 硬體、設定、身分與部署邊界；私人 Wi-Fi／裝置設定不提交。
- `docs/web-ui-design.md` / `include/web_ui.h` — 網頁修改前必讀；圖示只改 `tools/make_icon.py`，`include/web_icon.h` 為產生檔。
- `docs/motion-control.md` / `docs/uart-servo.md` / `include/servo_motion.h` / `include/servo_controller.h` — 手動／GPS 共用限速與 Youmi M0／TX43／9600，無回授。
- `include/gnss_snapshot.h` / `include/gnss_rate.h` / `include/station_position.h` / `include/geo_math.h` / `include/tracking_policy.h` — GNSS、平均位置、方位與追蹤有效性。
- `include/protocol.h` / `include/client_cadence.h` / `include/lora_schedule.h` / `include/packet_diagnostics.h` / `include/packet_rate.h` — RF 封包與時序。
- `include/station_extensions.h` / `include/station_ble.h` / `include/station_client_link.h` / `include/t096_control.h` / `include/t096_power.h` — Client 控制／BLE／狀態與電源。
- `docs/sd-log.md` / `docs/debug-export.md` / `src/sd_log.cpp` / `src/sdk_log.cpp` / `include/http_timing.h` / `include/loop_metrics.h` — 有界記錄、USB 匯出與時序；不要把快照略過當成 RF 漏包。
- `include/firmware_version.h` / `CHANGELOG.md` — 版本唯一來源與變更紀錄。
- `tools/verify_project.py` / `test/` — 主機回歸，不會燒錄或操作硬體；相關 `tools/test_*` 見功能入口。

## RULES:
+ 使用者希望精簡程式，方便記憶與 debug。先看真正執行／占資源的路徑，優先刪除確認不用的階段性功能，不先增加框架、快取或重試狀態機。
+ 階段性實驗／部署紀錄先 commit 保存，再將 commit／原路徑／SHA-256／附件加入紀錄索引；驗證可取回後刪除工作目錄副本並 commit 清理。現行規格與 `next-session-plan.md` 持續保留，舊連結改指索引。
+ 刪除前執行 `python3 tools/read_record.py --verify`；收尾實際匯出一份含圖片的紀錄。不要重寫歷史、只保留易移動的 HEAD 指標，或為了減少文件把尚未驗收改成通過。
+ 網頁修改先讀 `docs/web-ui-design.md`；使用者最新明確指示優先，只改已授權範圍，並同步規格與驗證紀錄。不得因美化／重構自行重排控制、恢復已移除功能或改變追蹤／Servo 行為。
+ 功能、修正、介面或設定行為異動須同步記錄 `CHANGELOG.md`；未定版先放「未發行」，標版時歸入對應版本並記錄驗證／部署狀態。
+ build 與驗證由 agent 執行，輸出以結果摘要為主；使用者明確要求不 build 時不得編譯。
+ 使用中文回答,也可以中文夾雜,不用將專有名詞刻意翻譯

## 協作原則：保持批判態度
- 主動檢查使用者與 agent 自己的假設；不因為某個方向被提出、先前建議過或已經實作，就把它當成正確答案。發現反例或更好的方向時，直接說明證據與理由。
- 先看整套 GPS／LoRa／追蹤流程與相依關係，再提出有優先順序的一致方案；不要只隨每個提問零碎增加建議。
- 清楚區分已知事實、推測、主機測試、實機與外測結果。資料不足就指出缺口，不把設定寫入、成功 build 或接收封包當成精度與穩定性的證明。
- 現階段優先簡單、穩定、方便 debug，讓問題可見並學習原因。保留原始資料與失敗原因，避免靜默降級、掩蓋異常，或為低機率情況堆疊複雜狀態機；耗電優化後置。
- 已確認的工作自主完成並驗證；新發現若需要改變尚未確認的產品行為，先提出取捨，不以假設安全或可靠為理由自行擴大範圍。

## 協作原則：適度節省 token
- 在正確性、可維護性與必要驗證不打折的前提下，減少不必要的上下文、重複探索與冗長輸出；不為省 token 省略關鍵證據、風險或未完成事項。
- 由 agent 依任務複雜度、風險、所需上下文、可驗證性與預期總成本，自主選擇可用模型、推理強度及是否使用 subagent，不固定指定某個模型。範圍明確、可獨立驗證的工作可交給合適的 subagent；主 agent 負責介面與行為決策、審查、整合及必要驗證。涉及跨模組狀態、時序、硬體或燒錄風險時，優先確保推理與驗證能力；若較小模型反覆失敗，應調整分工或升級模型，避免無效重試。
- 派工只提供必要背景、檔案與驗收條件，避免複製整段對話或整個專案；各 agent 分工與檔案範圍要清楚，避免重複探索或互相覆寫。短小、相依性高的工作由主 agent 直接完成，不為派工而派工。
- 以完成任務的整體 token、時間與返工成本判斷是否值得分工，不只比較單次模型成本；subagent 也消耗 token，不承諾總 token、費用或帳號額度必然下降。模型能力與可用性以當前工具為準，遇到限制時如實說明，不宣稱已使用實際未啟用的模型。

## 外測診斷
- `docs/field-diagnostic.md` — 無筆電外測流程、內建 Flash 邊界、UTC／原始資料與 USB 匯出。
- `include/field_diagnostic.h` — LilyGO 診斷版流程，不改正常版 2 Hz。
- `include/diagnostic_store_codec.h` / `src/diagnostic_store.cpp` — LilyGO 診斷版與 V4 Station 共用的 bounded Flash worker／CRC 匯出。
- `include/station_flash.h` / `src/station_flash.cpp` / `docs/heltec-field-ready.md` — V4 無 SD 的 RF／位置脈絡記錄與外測準備。
- `include/t096_gnss_config.h` — UC6580 暫存設定／回讀；真實輸出率與有效 fix 分開驗證。
- `include/station_http.h` — Station 空 TCP 連線250 ms期限；其他連線排隊時100 ms寬限後讓位。
- `include/trip_log.h` / `tools/test_trip_logging.py` — Client trip 1 Hz 長時間記錄；SD 全程 raw UART、Flash 稀疏電源／狀態，正常版 GPS gate 不變。
- `include/power_irq.h` / `tools/test_power_irq_backend.py` — signed PMU IRQ／VBUS、clear failure 去重與動作前關機證據。
- `tools/test_pmu_battery_diagnostic.py` — Client 診斷版唯讀 PMU 原始取樣、短讀／鎖忙／初始化回傳值與資料完整性。
- `include/client_boot_animation.h` / `tools/preview_client_boot_animation.py` — Client 3 秒 OLED 開機動畫與實際 U8g2 像素預覽；不代表 GPS 狀態
- `tools/test_usb_completion.py` — 實際 SD／DIAG worker 完成回覆後立即下一指令的回歸。
- `tools/test_sd_usb_budget.py` — 錄製中 USB 背壓的有界等待、寫卡不中斷與非法輸入原始證據。
