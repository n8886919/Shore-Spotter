# LilyGO E91C 改為 Station，配對 T096 F035

2026-10-06，依使用者指示將目前 USB 接著的 LilyGO 改為 Station，與接著的 T096 Client 配對。
使用者後續更正為 log 存記憶卡，不填 log server key；本輪採 SD 記錄、Axiom 停用。

## 裝置與配置

- USB 核對 LilyGO `48:CA:43:57:E9:1C`（E91C），ESP32-S3、8 MB Flash；不是另一台 LilyGO 584C。
- 接著的 T096 USB serial 與開機訊息確認 Client `F035`／61493，boot `1859143360`。
- 新增被 Git 忽略的本機 `station-lilygo-e91c` profile：`tbeam-station`、B 組 923.8 MHz、配對 `t096-client`。
  原 V4、LilyGO 584C 與 T096 profiles 保留。版本仍為 `0.6-dev`／wire v5，SF10／BW125／CR4/5。
- Station hostname 為 `shore-48ca4357e91c`，可用 `http://shore-48ca4357e91c.local/`；實際 LAN IP 留在私人驗證紀錄。
- 實際 NVS 綁定透過既有 `/api/whitelist?action=set&id=F035` 保存，不能只以 build profile 當作配對證據。

## 備份與燒錄

先以 `SD STOP` 排空並同步原 Client 記錄，停止時 errors 0、unconfirmed bytes 0。
ROM 讀回完整 8,388,608-byte Flash，原始 SHA-256：
`9e9c7aa6a0b7c64610bc8d334bcfcb9322d8b33a8442739314b992cdad59afc5`。

原分區表與 Station build 一致、原 OTA 選擇 app0；只在 `0x10000` 寫入 Station application，
未改 bootloader、partition table、otadata、NVS 或 SPIFFS 分區。esptool 確認寫入 hash。
application 1,651,872 bytes，SHA-256：
`36f4426109043ee9b33f41c3b3812f0cf16dea923ce92afc9b4bc8959bbf8f9b`。
後續明確配對操作會更新 NVS 的 Client 綁定。

## 實機驗證

- 第一個 Station boot `1091334672`；API hostname 與 E91C 一致，綁定 F035，收到該 Client 新鮮 Ready STATE。
- 透過這台 LilyGO 下達 Stop，Client 回覆 command 1 confirmed、last_error 0、維持 Ready；
  SD 原始 STATE bytes 也包含控制 Station E91C。這確認實際雙向命令交換，不只接收綁定設定。
- Axiom `enabled=false`、`token_set=false`，本輪沒有填入 key。PSRAM 實讀 8,388,608 bytes。
- SD 開機自動錄製，第一個新段 `/logs/0000000000000020-0000.ndjson`。
  USB 逐 byte 匯出 541,224 bytes、CRC32 `7e581c8d`，277 行 JSON 均可解析；
  包含 5 筆來源 F035 的 `lora_packet`／`client_state`，seq 37–41、raw 未截斷，另有 Station 快照與 SD 狀態。
  原檔 SHA-256：`628995c794b1fca19f484fcb5584fbf28e0435bba3247f76b04de252320789fb`。
- 匯出後重啟 Station 驗證 NVS：新 boot `2868549597` 仍綁定 F035、Axiom 停用／無 key，收到同一 Client boot 的 Ready。
  無需手動 START 即恢復 SD recording，新段 `/logs/0000000000000021-0000.ndjson`。
  第一份讀回狀態有 1 筆封包、24 筆快照、96,895 bytes，errors／packet_dropped／sample_dropped 均 0。
  本輪兩個 Station boot 的 deleted_files 均 0；沒有格式化或手動清除原卡資料。

## 主機檢查與界線

device config 7 tests、實際 packet backend、Station downlink、Axiom backend 與 SD worker 回歸通過；Station build 成功。
此次是 USB 供電桌上 Ready／下行／SD 驗證，不代表有效戶外定位、Tracking 負載、遠距或水上收包已通過。
SD 採既有循環記錄政策；容量不足時回收已辨識的舊 log，未收到的 RF 封包無法還原。

原始 Flash、來源快照、build／燒錄／API／SD 匯出與驗證摘要保存在本機私人 validation 目錄，不提交網路設定或原始定位資料。
