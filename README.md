# IMU-BLE Node — STM32F103 嵌入式工程教學專案

以「真實產品等級」的做法打造的教學韌體：STM32F103C8T6 讀取 MPU6050
姿態，經 nRF52832 BLE 模組回傳 PC；OLED 即時儀表板、EEPROM 參數持久化、
多按鍵（8 鍵）人機介面、序列埠 CLI、看門狗與全面的健康監控。

```
                    ┌───────────────┐
   MPU6050 ──I2C1──▶│               │──USART2──▶ nRF52832 ─)))BLE(((─ PC (Python)
   AT24Cxx ──I2C1──▶│  STM32F103    │──USART1──▶ USB-TTL → CLI 終端機
   KEY0~7   ─GPIO─▶│  (72MHz)      │──I2C2+DMA▶ SSD1306 OLED 128x64
   MPU INT ──EXTI──▶│               │──GPIO────▶ 狀態 LED (PC13)
                    └───────────────┘
```

## 功能總覽

| 功能 | 說明 |
|---|---|
| 姿態解算 | 100Hz Mahony 互補濾波（預設）或 MPU6050 DMP（可切換） |
| BLE 遙測 | 二進位協定（SOF+CRC16），姿態 10Hz（0~50Hz 可調）+ 系統狀態 1Hz + 事件 |
| PC 端工具 | Python 即時儀表板 / 命令下發 / CSV 記錄；Serial 與 BLE(NUS) 雙傳輸 |
| OLED | 儀表板 / EEPROM 記錄 / BLE 鏈路 / 原始值 / 系統資訊五頁 + 開機畫面 + 提示條；DMA 非同步刷新 |
| 按鍵 | 去彈跳 / 單擊 / 雙擊 / 長按狀態機；KEY0 點擊計數寫入 EEPROM |
| 手柄模式 | KEY2~KEY5 事件即時上報，PC 端（pynput）映射為鍵盤/滑鼠動作 —— 裝置可當 BLE 遙控器/簡報器 |
| 參數持久化 | 雙槽輪替 + CRC + 讀回驗證（斷電安全）、延遲落盤（磨耗控制） |
| 穩定性 | IWDG 任務簽到制、I2C bus recovery、降級運行、重置原因追蹤 |
| 可觀測性 | CLI `stat` 完整統計（任務耗時/CPU%/錯誤計數）、BLE STAT 封包 |
| 品質保障 | 中介層純 C 無硬體相依，附 PC 端單元測試（48 項檢查） |

## 1. 硬體與接線

| 訊號 | STM32 腳位 | 對接 | 備註 |
|---|---|---|---|
| I2C1 SCL / SDA | PB6 / PB7 | MPU6050 + AT24Cxx | 感測/儲存匯流排，400kHz |
| MPU6050 INT | PB5 | MPU INT 腳 | 下降緣 EXTI（教學觀測用） |
| I2C2 SCL / SDA | PB10 / PB11 | OLED (SSD1306/Y00429) | 顯示匯流排，DMA 刷新 |
| USART1 TX / RX | PA9 / PA10 | USB-TTL RX / TX | CLI，115200 8N1 |
| USART2 TX / RX | PA2 / PA3 | nRF52832 RXD / TXD | BLE 透傳，9600 8N1（遷就 HC-42 出廠值；交叉接） |
| KEY0 | PB0 | 按鍵另一端接 GND | 內部上拉，低電位按下（本地功能：計數/空中滑鼠/歸零） |
| KEY1 | PA1 | 按鍵另一端接 GND | 內部上拉，低電位按下（UI 操作） |
| KEY2~KEY5 | PA4~PA7 | 按鍵另一端接 GND | 手柄鍵：事件上報 PC，由 GUI 映射鍵盤/滑鼠 |
| KEY6 / KEY7 | PA0 / PB1 | 按鍵另一端接 GND | 手柄鍵（PB0/PB1 為新腳位，可自由選其他空腳） |
| LED | PC13 | 板載 LED | 低電位點亮 |
| 電源 | 3.3V / GND | 所有模組共地 | 全系統 3.3V；模組板載 I2C 上拉即可 |

> 想把三顆 I2C 裝置合併到同一條匯流排？可以 —— CubeMX 移除 I2C2、
> OLED 改接 PB6/PB7，並把 `bsp_board.h` 的 `BSP_I2C_DISP_HANDLE`
> 指向 `&hi2c1` 即可（代價見 `Docs/architecture.md` §4 的匯流排分析）。

## 2. 快速上手

### 2.1 產生 HAL 程式碼（首次必做的一步）

版本庫策略（.gitignore 已落實）：入庫的是「事實來源」——`.ioc`、
`.project`/`.cproject`、應用程式碼（`main.c/main.h` 因含 USER CODE
掛載點也入庫）；CubeMX 可再生的產物（HAL 程式庫、CMSIS、startup、
msp/it/hal_conf 等）與授權受限的 eMPL 一律不入庫。
因此 clone 或首次取得後：

1. 以 STM32CubeIDE 開啟本專案，雙擊 `STM32F103_E.ioc`。
2. 直接 **Project → Generate Code**（或存檔觸發）。
   所有周邊（時脈 72MHz、I2C×2、USART×2、DMA、EXTI、IWDG）已配置完成；
   `Core/Src/main.c` 的 USER CODE 區塊已掛好應用層進入點，重新產生不會遺失。
3. **設定最佳化**：Project → Properties → C/C++ Build → Settings →
   MCU GCC Compiler → Optimization → **`-Os`**。
   （`-O0` 會讓 HAL 體積逼近 64KB flash 上限。）
4. Build（榔頭圖示）→ 以 ST-Link 燒錄。

產生程式碼後可核對（CubeMX 手動確認清單）：
- Clock：HSE 8MHz × PLL9 = SYSCLK 72MHz、APB1 36MHz。
- DMA：USART2_RX（Ch6，**Circular**）、USART2_TX（Ch7）、I2C2_TX（Ch4）。
- NVIC：USART1/2、DMA Ch4/6/7、I2C2 EV/ER、EXTI9_5 均啟用。
- IWDG：prescaler 64、reload 2500。

### 2.2 看到第一筆輸出

1. USB-TTL 接 USART1，終端機 115200 → 上電應見開機 banner 與 `>` 提示符。
2. 輸入 `help`、`stat`、`imu` 試玩 CLI。
3. OLED 顯示開機畫面後進入姿態儀表板；轉動板子數字即時變化。
4. 按 KEY0 數次 → 斷電重上電 → CLI `clicks` 確認次數已持久化。

### 2.3 PC 端接收（Python）

```bash
cd tools/pyhost
pip install -r requirements.txt
```

**圖形介面（建議）**：

```bash
python host_gui.py
```

人工地平線 + 航向儀 + 20 秒滾動曲線、系統狀態面板（健康燈號/CPU/
點擊數）、命令按鈕與 CSV 記錄；Serial 與 BLE 皆可在介面內選擇連線
（BLE 含掃描與 UUID 手動覆寫）。

**CLI 版（開發期免 BLE）**：USB-TTL 直接接 USART2（PA2/PA3），

```bash
python host.py --port COM5
```

**BLE 模式**：nRF52832 接 USART2，PC 需支援 BLE：

```bash
pip install bleak
python host.py --ble --name <模組廣播名>
```

不同透傳韌體的 GATT UUID 各異（Nordic NUS `6E40…`、`FFE0/FFE1`、
`FFF0…`）——工具連線後會**自動偵測**並列出採用的服務。若偵測失敗：

```bash
python host.py --scan
```

先檢視模組廣播的服務，再以 `--uuid-notify FFE1 --uuid-write FFE1`
之類手動指定（支援 16-bit 縮寫）。

畫面即時更新 roll/pitch/yaw/溫度/點擊數；輸入 `rate 20`、`cal`、
`reset`、`info` 下發命令；`--csv log.csv` 同步記錄資料。
手機端亦可用 nRF Toolbox（UART/NUS）連線觀察原始封包。

**賽車遊戲**：`python kart_game.py --ble --name HC-42` —— 裝置傾斜當方向盤、
甩尾鍵漂移集氣、氮氣鍵爆發（F1 遊戲內綁定實體鍵，鍵盤也可玩）。

**平台跳躍遊戲**：`python platformer.py` —— 原創角色 VOLT 的經典橫向平台遊戲
（蘑菇變大/火力花射火球/踩殼踢殼/頂磚/檢查點/雙關卡），四鍵操控
（←→/Space/A）；手柄映射 KEY2=left、KEY3=right、KEY4=space、KEY5=a
即可用裝置遊玩。

### 2.4 BLE 模組準備

任何「透傳（transparent UART）」韌體的 nRF52832 模組皆可，
韌體預設假定 **115200 8N1**。

> ⚠ **鮑率不合 = BLE 連得上但收不到任何資料**（模組把 framing error
> 位元組全數丟棄）。GUI 的「位元組」計數是判斷指標：
> 位元組 0 → 資料根本沒進模組（多半是鮑率或 TX/RX 未交叉）；
> 位元組 >0 但封包 0 → 資料有到但內容亂（鮑率/雜訊）。

**鮑率自動佈建（預設開啟，免 USB-TTL）**：HC-42 等模組出廠常為
9600。韌體開機時會自動探測模組鮑率（AT 掃描 115200/9600/57600/
38400/19200），不符時自動下 `AT+BAUD` 改寫模組並驗證：

- 條件：該次開機時模組**不可處於 BLE 連線中**（HC 系列連線中不吃
  AT）。若被連線探測會失敗，下次開機自動再試。
- 成功改寫時 OLED 會顯示提示條（如 `BLE 9600>115200`），並在
  EEPROM 記「已佈建」旗標 —— 之後每次開機零成本跳過。
- 換了新模組：CLI 輸入 `ble auto` 隨時重跑（`ble` 可查旗標狀態）。
- 開關：`app_config.h` 的 `APP_BLE_AUTOBAUD`。

手動備援（自動佈建失敗、或非 CRLF AT 方言的模組）：CLI
`ble 9600` → `bridge`（終端機送 CR+LF）→ `AT` / `AT+BAUD=115200`
→ `Ctrl+]` → `ble 115200`。或反向遷就：CubeMX 把 USART2 改 9600
（頻寬上限 ≈960B/s，遙測 ≤20Hz）。

## 3. 目錄結構

```
Core/
  Inc/ Src/
    app/       應用層：app_main(初始化/排程)、task_*(7 個任務)、app_cli
    mw/        中介層：sched、proto、storage、button、ringbuf、crc16、cli、xutil
    drivers/   驅動：mpu6050、imu(介面)+mahony/dmp 後端、at24cxx、ssd1306、gfx、字型
    bsp/       板級：bsp_board.h(腳位總表)、bsp_uart、bsp_i2c、bsp_wdg、bsp
  Src/main.c   CubeMX 進入點（USER CODE 掛 app_main_init/loop）
Drivers/eMPL/  （自行放入 InvenSense eMPL，見 Docs/dmp_porting.md）
Docs/          protocol.md(協定)、architecture.md(設計)、oled_guide.md(OLED畫面看懂指南)、engineering_review.md(工程復盤)、dmp_porting.md
tools/pyhost/  Python 主機工具（GUI/CLI/診斷）與 kart_game.py 賽車遊戲
tests/host/    PC 端單元測試（gcc 即可執行）
```

閱讀順序建議：`Docs/engineering_review.md`(為什麼) → `Docs/architecture.md`(是什麼) → `bsp_board.h` → `app_main.c`
→ 感興趣的任務檔。

## 4. 人機介面

**按鍵**

| 操作 | 功能 |
|---|---|
| KEY0 單擊 | 點擊計數 +1（EEPROM 持久化，靜止 2 秒自動落盤） |
| KEY0 雙擊 | 空中滑鼠模式開關（MPU 姿態 → PC 游標；GUI 有靈敏度滑桿與反轉選項，受「啟用控制電腦」總開關管制） |
| KEY0 長按 | 計數歸零（立即落盤） |
| KEY1 單擊 | OLED 換頁（儀表板 → EEPROM 記錄 → BLE 鏈路 → 原始值 → 系統資訊） |
| KEY1 雙擊 | 遙測開/關 |
| KEY1 長按 | 陀螺儀校正（保持靜置 1 秒） |
| KEY2~KEY5 / KEY6 / KEY7 | 手柄鍵（裝置端無本地功能）：press/release 即時經 BLE 上報，GUI「手柄映射」面板以分類選單配置動作（分「方向鍵/其他」兩區）—— 滑鼠鍵（含 x1/x2 側鍵）/四向滾輪（按住連發）/單鍵與組合鍵（按住=按住）/`text:` 輸入整段文字/`run:` 啟動程式。KEY2~5 **配置寫入裝置 EEPROM**（換電腦連上即恢復）；KEY6/KEY7 映射存本機 keymap.json |

**LED**：每秒短亮 = 正常；5Hz 快閃 = 降級（IMU 或 EEPROM 離線）。

**CLI**（USART1，115200）：`help / ver / stat / imu / clicks [reset] /
rate <hz> / cal / save / dump [addr] [len] / oled(面板診斷) / ble(鮑率/佈建) / bridge / reboot`

## 5. 教學單元（實驗建議）

| # | 主題 | 動手做 | 對應程式碼 |
|---|---|---|---|
| 1 | 分層與可移植性 | 把 KEY1 改到 PB8：只允許改 CubeMX + `bsp_board.h`，驗證上層零修改 | `bsp/` |
| 2 | UART 三種收法 | 對比輪詢/逐位元組中斷/DMA+IDLE 的 CPU 佔用（`stat` 觀測） | `bsp_uart.c` |
| 3 | 環形緩衝區 | 在 PC 端跑 `tests/host`，把 `rb_put` 改壞一行，觀察哪個測試抓到 | `mw/ringbuf.c` |
| 4 | 通訊協定 | 加一個 `PROTO_T_SET_CONTRAST` 命令調 OLED 亮度（韌體+Python 兩端） | `mw/proto.*`、`task_comm.c` |
| 5 | EEPROM 斷電安全 | 落盤瞬間拔電源 ×20 次，驗證記錄永不損毀；對照單槽寫法的損壞率 | `mw/storage.c` |
| 6 | 姿態融合 | 只用陀螺積分（把 acc_valid 強制 false）觀察漂移；調 Kp/Ki 看收斂 | `imu_mahony.c` |
| 7 | 看門狗 | 在任一任務加 `while(1);`，觀察 4 秒後重啟且 banner 顯示 IWDG | `bsp_wdg.c` |
| 8 | DMP 整合 | 依 `Docs/dmp_porting.md` 切換 DMP，A/B 比較兩後端的雜訊與收斂 | `imu_dmp.c` |
| 9 | 匯流排復原 | 執行中短接 SDA 到 GND 再放開，觀察 `stat` 的 recover 計數與自癒 | `bsp_i2c.c` |
| 10 | 磨耗計算 | 由 `stat` 的 commits 推估 EEPROM 壽命；把延遲落盤改成即時寫並重算 | `mw/storage.c` |

## 6. 疑難排解

| 症狀 | 檢查 |
|---|---|
| 建置錯誤：找不到 HAL | 未執行 CubeMX Generate Code（步驟 2.1） |
| Flash 溢位 | 最佳化未設 `-Os`；或關閉 `APP_USE_CLI` |
| OLED 全黑 | `stat` 看 disp 匯流排錯誤數；量 PB10/PB11 上拉；部分模組位址 0x3D（驅動會自動探測） |
| OLED 畫面錯亂/滾動/交錯 | 面板晶片變體，用 CLI 現場診斷（見下方「OLED 顯示異常」流程） |
| IMU FAIL | PB6/PB7 接線與 3.3V；`stat` 看 sensor 匯流排；WHO_AM_I 非 0x68 的混料晶片需放寬檢查 |
| Python 收不到資料 | 先用 `--port` 直連 USART2 排除 BLE；確認 TX/RX 交叉；`rate` 是否為 0 |
| 亂碼 | 終端機鮑率 115200；橋接模式忘了退出（Ctrl+]） |
| 上電即重啟循環 | banner 重置原因若為 IWDG，查哪個任務未簽到（`stat` overruns） |
| 按鍵單擊延遲 250ms | 該鍵啟用了雙擊偵測，屬預期行為（見 `mw/button.h` 說明） |
| CLK 重開機歸零 | ① 點擊後須等 2 秒延遲落盤完成（STOR 頁 `*` 消失、W +1）再斷電；長按 KEY0 歸零與 CLI `save` 為立即落盤。② STOR 頁首顯示 OFFLINE = EEPROM 未上線：查接線與 3.3V；驅動會自動掃位址 0x50~0x57 並每 5 秒重試上線（`stat` 的 storage 行可看 addr/dirty/commits）。③ `dump 0 64` 直接檢視兩個槽的原始內容（開頭應為 C3 A5 magic） |

**OLED 顯示異常診斷流程**（畫面有東西但錯亂/滾動/交錯時）：

1. `stat` 看 `disp` 匯流排 err —— 若持續增加是訊號品質問題
   （上拉電阻/接線/降到 100kHz），不是驅動設定。err 為 0 → 續下一步。
2. `oled test` 顯示測試圖（外框 + 十字 + 左右兩排頁序號 0–7）。
3. 依症狀對症下藥（每步後看測試圖是否恢復正常）：
   - 畫面**橫向錯位/右緣缺兩欄/滾動** → `oled ctrl sh1106`（SH1106 系晶片）
   - **隔行交錯、頁序跳動**（0..7 不按序） → `oled com 02`
   - **上下顛倒/左右相反** → `oled flip`
4. **邊緣被模組外框遮住**（測試圖外框某邊看不到）→
   `oled inset <左> <上> <右> <下>` 微調安全區內縮（px），
   直到外框四邊完整可見。UI 全部繪圖會自動平移進安全區。
5. 恢復正常後把組合回填 `app_config.h`（`APP_OLED_SH1106` /
   `APP_OLED_COM_PINS` / `APP_OLED_INSET_*`）固化，
   再 `oled test` 關閉測試圖。

## 7. 延伸路線圖

九軸融合（加磁力計解 yaw 漂移）、BLE 私有服務直跑 nRF52832 SDK、
FreeRTOS 移植（任務函式已相容）、bootloader + BLE OTA、低功耗
（STOP mode + RTC 喚醒）、CI 自動跑 `tests/host`。

---
授權：MIT。教學使用請隨意；eMPL 依 InvenSense 原始授權自行取得。
