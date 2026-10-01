# 硬體說明（Hardware）

本文件彙整 Shore Spotter 的硬體規格與接線：

1. **腳位圖（Pins Map）** — T-Beam Supreme GPIO 分配
2. **I2C 裝置位址** — OLED / 感測器 / PMU
3. **電氣參數與電源通道** — AXP2101 各路供電
4. **按鈕說明** — PWR / BOOT / RST
5. **Servo 規格** — GXServo Brushless 42KG (QY3242BLS / GX3242)
6. **鏡頭指南針校正** — 磁針讀數、腳架固定與磁場干擾
7. **接線圖** — 電池直供 Servo / 攝影機（2026-09-09 使用者確認）

韌體行為見 [features.md](features.md)；封包/HTTP 介面見 [interface.md](interface.md)。

韌體啟動順序：先連接 AXP2101，再設定 ALDO3 為 3300 mV、啟用並讀回確認，
等待 10 ms 後才執行 LoRa SPI／radio.begin；無線電復原也重新套用與驗證。
PMU 連線、設定寫入或讀回失敗時不進行 radio 初始化，序列紀錄明示錯誤。
ALDO4 仍供 GPS；ALDO1 供 OLED／BME280，不因 OLED 睡眠而斷電。
這是寄存器設定驗證，不代表已量測實際電壓／RF 功率。
電源通道來源：[LILYGO 官方 T-Beam Supreme 文件](https://wiki.lilygo.cc/products/t-beam-series/t-beam-supreme/)。

---

# LILYGO T-Beam Supreme - V3.0 / L76K / 915mhz / SX1262
### 📍 Pins Map

| Name                                         | GPIO NUM                   | Free |
| -------------------------------------------- | -------------------------- | ---- |
| Uart1 TX                                     | 43(External QWIIC Socket)  | ✅️    |
| Uart1 RX                                     | 44(External QWIIC Socket)  | ✅️    |
| SDA                                          | 17                         | ❌    |
| SCL                                          | 18                         | ❌    |
| OLED(**SH1106**) SDA                         | Share with I2C bus         | ❌    |
| OLED(**SH1106**) SCL                         | Share with I2C bus         | ❌    |
| RTC(**PCF8563**) SDA                         | Share with **PMU** I2C bus | ❌    |
| RTC(**PCF8563**) SCL                         | Share with **PMU** I2C bus | ❌    |
| MAG Sensor(**QMC6310U/QMC6310N/QC6309**) SDA | Share with I2C bus         | ❌    |
| MAG Sensor(**QMC6310U/QMC6310N/QC6309**) SCL | Share with I2C bus         | ❌    |
| RTC(**PCF8563**) Interrupt                   | 14                         | ❌    |
| IMU Sensor(**QMI8658**) Interrupt            | 33                         | ❌    |
| IMU Sensor(**QMI8658**) MISO                 | Share with SPI bus         | ❌    |
| IMU Sensor(**QMI8658**) MOSI                 | Share with SPI bus         | ❌    |
| IMU Sensor(**QMI8658**) SCK                  | Share with SPI bus         | ❌    |
| IMU Sensor(**QMI8658**) CS                   | 34                         | ❌    |
| SPI MOSI                                     | 35                         | ❌    |
| SPI MISO                                     | 37                         | ❌    |
| SPI SCK                                      | 36                         | ❌    |
| SD CS                                        | 47                         | ❌    |
| SD MOSI                                      | Share with SPI bus         | ❌    |
| SD MISO                                      | Share with SPI bus         | ❌    |
| SD SCK                                       | Share with SPI bus         | ❌    |
| GNSS(**L76K or Ublox M10**) TX               | 8                          | ❌    |
| GNSS(**L76K or Ublox M10**) RX               | 9                          | ❌    |
| GNSS(**L76K or Ublox M10**) PPS              | 6                          | ❌    |
| GNSS(**L76K**) Wake-up                       | 7                          | ❌    |
| LoRa(**SX1262 or LR1121**) SCK               | 12                         | ❌    |
| LoRa(**SX1262 or LR1121**) MISO              | 13                         | ❌    |
| LoRa(**SX1262 or LR1121**) MOSI              | 11                         | ❌    |
| LoRa(**SX1262 or LR1121**) RESET             | 5                          | ❌    |
| LoRa(**SX1262 or LR1121**) DIO1/DIO9         | 1                          | ❌    |
| LoRa(**SX1262 or LR1121**) BUSY              | 4                          | ❌    |
| LoRa(**SX1262 or LR1121**) CS                | 10                         | ❌    |
| Button1 (BOOT)                               | 0                          | ❌    |
| PMU (**AXP2101**) IRQ                        | 40                         | ❌    |
| PMU (**AXP2101**) SDA                        | 42                         | ❌    |
| PMU (**AXP2101**) SCL                        | 41                         | ❌    |

> \[!IMPORTANT]
> 
> 1. GNSS Wake-up is only available in L76K version
> 
> 2. Radio has its own SPI bus, and other peripheral SPI devices share the SPI bus.
>
> 3. T-BeamSupreme has three magnetometer versions: QMC6310N, QMC6310U, and QMC6309, each with a different device address.

### 🧑🏼‍🔧 I2C Devices Address

| Devices                                 | 7-Bit Address | Share Bus      |
| --------------------------------------- | ------------- | -------------- |
| OLED Display (**SH1106**)               | 0x3C/0x3D     | ✅️  (I2C Bus 0) |
| MAG Sensor(**QMC6310U OR QMC6310N**)    | 0x1C/0x3C     | ✅️  (I2C Bus 0) |
| MAG Sensor(**QMC6309**)                 | 0x7C          | ✅️  (I2C Bus 0) |
| Temperature/humidity Sensor(**BME280**) | 0x77          | ✅️  (I2C Bus 0) |
| RTC (**PCF8563**)                       | 0x51          | ❌ (I2C Bus 1)  |
| Power Manager (**AXP2101**)             | 0x34          | ❌ (I2C Bus 1)  |

> \[!IMPORTANT]
> If the I2C device is connected to pins 17 (SDA) or 18 (SCL)
> the sensor power supply must be connected to DC1. If connected to other LDOs
> the power must be turned on before accessing the sensor I2C bus
> otherwise, the I2C access will fail or freeze.
>
> The QMC6310U and QMC6310N use different device addresses: QMC6310U (0x1C) and QMC6310N (0x3C).
> The SH1106 uses either device address 0x3C or 0x3D. If using the QMC6310U version, the device address is 0x3C; if using the QMC6310N version, the device address is 0x3D.
> The screen device address using the QMC6309 magnetic sensor is 0x3C, the same as the QMC6310U.
>

> [!NOTE]
> **OLED 位址獨立偵測。** `detectOledAddress()` 先檢查 0x3D，存在就使用，否則選 0x3C；
> 不再靠磁力計初始化判定。這保留不同板子版本的螢幕支援，無需使用板上磁力計。

### BME280 Address

* If you need to change the BME280 device address, you can remove the resistor and then connect it to the fixed pad via a wire. This will change the device address to 0x76.


### ⚡ Electrical parameters

| Features             | Details                     |
| -------------------- | --------------------------- |
| 🔗USB-C Input Voltage | 3.9V-6V                     |
| ⚡Charge Current      | 0-1024mA (\(Programmable\)) |
| 🔋Battery Voltage     | 3.7V                        |

### ⚡ PowerManage Channel

| Channel    | Peripherals                              | Max Current                              |
| ---------- | ---------------------------------------- | ---------------------------------------- |
| DC1        | **ESP32-S3**                             | 2A(Includes ESP operating current 800mA) |
| DC2        | Unused                                   | X                                        |
| DC3        | External M.2 Socket                      | 2A                                       |
| DC4        | External M.2 Socket                      | 1.5A                                     |
| DC5        | External M.2 Socket                      | 1A                                       |
| LDO1(VRTC) | Unused                                   | X                                        |
| ALDO1      | **BME280 Sensor & Display & MAG Sensor** | 300mA                                    |
| ALDO2      | **Sensor**                               | 300mA                                    |
| ALDO3      | **Radio**                                | 300mA                                    |
| ALDO4      | **GPS**                                  | 300mA                                    |
| BLDO1      | **SD Card**                              | 300mA                                    |
| BLDO2      | External pin header                      | 300mA                                    |
| DLDO1      | Unused                                   | X                                        |
| CPUSLDO    | Unused                                   | X                                        |
| VBACKUP    | Unused                                   | X                                        |

* T-Beam Supreme GPS backup power comes from 18650 battery. If you remove the 18650 battery, you will not be able to get GPS hot start. If you need to use GPS hot start, please connect the 18650 battery.

### Button Description

| Channel | Peripherals                       |
| ------- | --------------------------------- |
| PWR     | PMU button, customizable function |
| BOOT    | Boot mode button, customizable    |
| RST     | Reset button                      |

* The PWR button is connected to the PMU
  1. In shutdown mode, press the PWR button to turn on the power supply
  2. In power-on mode, press the PWR button for 6 seconds (default time) to turn off the power supply

> 韌體用法（本專案）：Client 端**短按 PWR** 喚醒 OLED 顯示狀態 10 秒；**長按 PWR** 顯示關機畫面後由 PMU 斷電。Station 端長按 PWR 同樣為關機。Client 第一次完整讀取／清除啟動 IRQ 只建立基準；其後新的按下沿，在動畫／資訊頁內也能接受一般短／長按。初始殘留或持續按住開機鍵不算新按壓；硬體長按關斷仍由 PMU 自身設定決定，韌體未更改該設定。

# GXServo Brushless 42KG (QY3242BLS / GX3242)

> 本專案目前使用：**500–2500us, 180°**（與韌體 `SERVO_MIN_US=500`, `SERVO_MAX_US=2500` 對應）。

> [!IMPORTANT]
> **旋轉方向：實機已確認為「角度增加 = 從上方看逆時針」**，與韌體追蹤公式的假設一致
> （`true_bearing = mount_offset + declination − servo_angle`，見 `main.cpp` 幾何註解）。
>
> 鏡頭校正只存 RAM；舊 NVS 安裝偏移不再沿用。
> 若日後把 servo 上下顛倒重裝，方向會反轉，追蹤會往錯的方向跑（surfer 往左、鏡頭往右），
> 而且**不會有任何錯誤訊息**。重裝機構後請務必重驗：
>
> 目前網頁滑桿採 `raw = 180 - ui`；手動從顯示 90 拉到 120，內部為 90→60，
> 從上方看相機應該**順時針**轉。若與此相反，
> 就要先修正安裝方向或控制幾何，再重新用鏡頭指南針校正；目前校正僅存 RAM，
> 已無 `SERVO_CAL_VERSION` 或校正 NVS 版本需要更新。

| Parameter | Value |
| --- | --- |
| 型號 | GXServo QY3242BLS / GX3242（42KG 級） |
| 馬達型式 | Brushless |
| 操作電壓 | 5.0V–8.4V |
| 控制訊號 | PWM, 1520us / 333Hz（常見標示） |
| PWM 輸入電平 | 3.3V–5.0V |
| 脈寬/角度 | 500–2500us 對應 180° |
| 堵轉扭力 | 30 kg.cm @ 6.0V / 38 kg.cm @ 7.4V / 42 kg.cm @ 8.4V |
| 空載速度 | 0.118s/60° @ 6.0V / 0.092s/60° @ 7.4V / 0.085s/60° @ 8.4V |
| Dead band | 2us |
| 防護 | IP65（依賣場標示） |
| 齒輪/軸承 | 金屬齒輪（常見為銅鋁組合）/ 雙滾珠 |
| 尺寸/重量 | 約 40 x 20 x 37mm / 約 69g |
| 線材 | Brown(-) / Red(+) / Orange(Signal) |

> [!IMPORTANT]
> 1. GXServo 42KG 在不同通路會有標示差異（例如殼材、齒輪材質、是否可程式化、角度選項）。
> 2. 本專案以 **180° 版本 + 500–2500us** 為準，若你買到 270/360° 版本，需重新校正行程。
> 3. 目前使用者確認 Servo 直接 **2S、2000 mAh、35C 電池**，不經 UBEC。一般 2S LiPo 充滿為
>    8.4 V，直供必須以實際 Servo 銘牌／規格支援該電壓為前提；本表通路規格不能代替實物核對。
> 4. Servo Brown(-)、電池負極與 T-Beam GND 必須共地；只接 IO21 訊號線無法形成有效的 PWM 電位基準。

資料來源（2026-07-23 查詢）：
- https://www.ariesrc.gr/en/servo-4/22326-gxservo-qy3242bls-42kg-brushless-motor-180-degree-metal-gear-digital-servo-for-rc.html
- https://hobbyant.com/p/sale-268576
- https://offthegridsun.com/Servo-Robot-Motor/GXservo-GX3242-42KG-Brushless-Steering-Gear-High-Speed-Servo

# 鏡頭指南針校正

目前只使用黏在鏡頭上的普通磁針指南針；韌體不初始化／取樣板上 QMC，不做 hard-iron，
也不保存板上校正到 NVS。上方表格仍記錄板子實際裝有的元件與位址。

先按「回到 90°」切為手動，等鏡頭停穩再輸入北 0°、東 90°、南 180°、西 270°等讀數。
校正僅接受 Servo 命令位置與目標均為 90°、不再移動；RAM 保存 `mount_offset = compass_bearing + servo_angle`。
GPS 追蹤以 `servo_angle = mount_offset + declination - true_bearing` 計算，維持原本
Servo 角度增加為逆時針的方向；台灣磁偏角依 GPS 位置／日期自動換算。

指南針需與鏡頭光軸朝向對齊。腳架轉動、重新架設、移動指南針或重開機後必須重校。
若磁針讀數隨 Servo 上電／轉動明顯變化，先拉開指南針與馬達／金屬的距離。
UART 只傳 Servo 絕對角度，不要求 GPS 或指南針校正。

# 接線圖
```mermaid
flowchart TD
    B[2S 2000mAh 35C 鋰電池]

    B -->|XT60| C[Type-C Converter]
    B -->|直供電源| S[GXServo 42KG Servo]
    C --> T[LILYGO T-Beam Supreme]
    T -->|IO21 PWM| S
    T ---|GND 共地| S
    S -->|3D列印連接器| A[攝影機]

```

Servo 直供為 2026-09-09 使用者確認；2000 mAh × 35C 對應標稱 70 A，並非實測
持續輸出保證。線徑／長度、接頭、電池健康度與移動時電壓尚未量測。
使用者回報移動時抖動，固定時也偶爾抖一下；控制模式與抖動當下命令角度尚未核對。
抖動排查先量 Servo 電源接頭的動態電壓，再比較減輕負載與降低共用速限的結果；
保持角度時仍抖，也要檢查共地、訊號與 Servo 自身的定位修正。軟體限速無法
消除 Servo 內部的保持修正或機構共振。
電源需同時滿足 Servo 的電壓與電流要求，參考
[Pololu 供電說明](https://www.pololu.com/docs/0J40/7.a)；一般 2S LiPo 與 LiHV 充滿
電壓不同，參考 [Gens ace 電池說明](https://genstattu.com/blog/how-to-choose-the-best-2s-shorty-lipo-pack)。

## 電源與電量量測

- **Station**：2S 經 Type-C 變壓供 USB-C（PMU 視為 VBUS），同時板載 **18650** 作備援 → 像手機插著電使用。
- **Client**：板載 **18650** 單獨供電。
- 板上 **AXP2101 只量得到 18650（單 cell）**；2S 無法直接讀（對 Station 只是 VBUS）。
- 電量 %：**3.2V=0%、4.15V=100%**（線性）。低於 3.2V 自動關機；**接 USB（VBUS 在）時不關機**，Type-C 失效改吃 18650 過低才關（保險）。
- 接 USB/Type-C 時 OLED 與網頁顯示 **⚡**；「插著 Type-C 仍回報 18650 電壓」為正常。
- 18650 為 LilyGO 板載電池，亦是 GPS 熱啟動備援電源。

## UART 控制

使用 3.3 V USB-to-TTL：TX 接 GPIO44、RX 接 GPIO43，共地，獨立供電時不接 VCC。
UART2 為 115200 8N1，與 GPS 的 UART1（GPIO9/8）分開。控制規則見
[uart-servo.md](uart-servo.md)。鏡頭上的磁針指南針跟著鏡頭轉；板上 QMC 不參與控制。


## 目前 GNSS／RX 設定（2026-09-19，待實機驗證）

本版依 L76K 設定 115200 baud、500 ms 更新間隔，只開 RMC＋GGA；沒有使用 PMTK，
也未宣稱支援表中另一款 u-blox M10 的設定命令。先送 9600→115200 切換命令，再於
115200 重送，涵蓋冷啟動與 MCU 重啟但 GNSS 仍供電。未將設定另存 GNSS flash。
L76K 原廠要求 >1 Hz 時僅開一種 NMEA 語句；本版雙語句是使用者確認的驗證選擇，
必須在序列／診斷核對實際 epoch、RMC、GGA Hz，而非看到命令送出就認定已成功。
來源：[Quectel L76K 協定，第 22–23 頁](https://forums.quectel.com/uploads/short-url/kmb3zNuV2SldThkOOJoNg9th40S.pdf)。

Station 每次初始化／復原開啟 SX1262 boosted RX gain，較高接收耗電用於接收性能，
不是提高 Client 發射功率，也不能保證穿透水、人體、地形遮蔽。這次只驗證設定路徑與錯誤處理；
實際改善幅度須同位置、天線、姿態、頻率與 SF 的現場對照。ALDO3 讀回亦非供電電壓實測。

## Heltec V4 Station 與 T096 Client（2026-10-02，USB 桌上部署）

配置兩個獨立 RF group：A 保留原本的 923.2 MHz（group 0），B 是目前接線的
Heltec V4 Station／T096 Client（group 1、923.8 MHz）。兩塊 LilyGO 預定都當 Station；
未連線，尚未改寫實體角色，A 的 Client 待實際硬體登記配對。兩組均用 BW 125 kHz、SF10、CR 4/5、Sync
Word `0x12`，但不會掃描、協商或互通。兩塊 Heltec 已燒錄並完成桌上雙向命令；LilyGO 未連接，僅 build，沒有外測。

Heltec V4 Station 使用 ESP32-S3 N16R2（16 MB flash、內嵌 2 MB QSPI PSRAM，不是 R8/OPI）。

| 功能 | GPIO／設定 |
|---|---|
| SX1262 | SCK 9、MISO 11、MOSI 10、NSS 8、DIO1 14、NRST 12、BUSY 13、TCXO 1.8 V |
| OLED | SSD1306：SDA 17、SCL 18、RESET 21；Vext GPIO36 active-low |
| Servo | GPIO4（Heltec 公開 header 的外接 GPIO）；只是 PWM 命令，無實體角度回授 |
| 外接 GNSS | RX 39／TX 38 僅屬擴充模組；裸板未安裝時不初始化 GNSS |
| 無板載項目 | 無 AXP2101、SD、BME280；電量／充電必須呈現 unknown，不能把 0 當健康 |

FEM power 為 GPIO7、CSD 為 GPIO2。auto profile 依 Meshtastic 的 runtime probe：上電後釋放 GPIO2
為 input，連續 8 次全低是 GC1109、全高是 KCT8103L，任何不一致 fail closed，不猜 RF path。GC1109
使用 CPS GPIO46、DIO2 自動 CTX；KCT8103L 使用 CTX GPIO5（TX high／RX LNA low）、DIO2 自動 CPS。
`heltec-v4-2-station`／`heltec-v4-3-station` 是明確 override，僅適用已確認 FEM 的板。
本次 USB 確認 R2／16 MB flash，runtime probe 為 KCT8103L，屬 V4.3 FEM 路徑；這不能確認板面細分版號。
雙向 RF 已通；OLED 僅有初始化成功訊息，實際顯示與 GPIO4 Servo 尚未驗收。
0 dBm 是 SX1262 chip drive；FEM enabled 不是天線輸出功率量測。

T096 是獨立 nRF52840 Client，沒有 ESP Web/Wi-Fi，也不以 fake GNSS 冒充定位：SX1262 NSS 5、DIO1 21、
RESET 16、BUSY 19，SPI SCK 40/MISO 14/MOSI 11，FEM power 30/CSD 12/CTX 41，chip drive 0 dBm；
GNSS enable 6（low active）、reset 46、PPS 43、RX 23、TX 25，115200 baud；battery ADC 3/control 47、
button 42、LED 28、Vext 26。RF 收發與 USB VBUS 回報已在桌上確認；GNSS fix、ADC 電量、
SystemOFF 電流與喚醒尚未實測。USB VBUS 抑制 Ready 12 小時後的 SystemOFF，拔除後重新計時；
現有充電板未接，不能稱為已驗證無線充電或 VBUS wake。

收納 UX 預定採「充電座喚醒 → Web 開始／停止 → 待命 12 小時自動收納」。
避免充電短暫斷續造成反覆關機，不以拔離充電座作為睡眠開關。封殼前需確認接收板 5 V 路徑
確實能觸發本板喚醒，以及接收板／充電器反向漏電；若無法喚醒，再考慮磁簧外部喚醒。
本版深眠只啟用使用者按鍵 wake，未宣稱完成充電座喚醒。1000／2500 mAh 的月數須以整機休眠電流量測估算。
