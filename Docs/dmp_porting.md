# eMPL DMP 整合指南

預設韌體使用內建 Mahony 濾波（`APP_USE_MPU_DMP = 0`），開箱即可建置。
本指南說明如何切換到 InvenSense eMPL DMP 硬體解算後端。

## 為什麼需要這份指南？

eMPL（Embedded Motion Driver）是 InvenSense 官方程式庫，內含
約 3KB 的 DMP 韌體映像，**授權上不適合隨第三方專案散布**，
且其原始碼要求使用者自行提供平台層（I2C 讀寫、延遲、時基）。
本專案已備妥平台層（`empl_port.h/.c`），你只需放檔案 + 兩處小修改。

## 步驟

### 1. 取得檔案，放入 `Drivers/eMPL/`

```
inv_mpu.c
inv_mpu.h
inv_mpu_dmp_motion_driver.c
inv_mpu_dmp_motion_driver.h
dmpKey.h
dmpmap.h
```

來源：InvenSense Motion Driver 5.1 / 6.12，或正點原子、野火等
開發板範例中的相同檔案。

### 2. 修改 `inv_mpu.c`

(a) 在檔案**最上方、所有 `#include` 之前**加入：

```c
#include "drivers/empl_port.h"
```

> 必須在 `#include "inv_mpu.h"` 之前 —— `empl_port.h` 定義的
> `EMPL_TARGET_STM32F4` 決定 `struct int_param_s` 的形狀。

(b) 找到平台相依區塊（在檔案前段，形如）：

```c
#if defined MOTION_DRIVER_TARGET_MSP430
#include "msp430.h"
...
#elif defined EMPL_TARGET_STM32F4
#include "i2c.h"
...
#endif
```

**整段刪除（或註解掉）**。所需的 `i2c_write / i2c_read / delay_ms /
get_ms / log_i / log_e / min()` 已由 `empl_port.h` 提供。

### 3. 修改 `inv_mpu_dmp_motion_driver.c`

同樣處理：最上方加入 `#include "drivers/empl_port.h"`，
刪除其平台相依 `#if ... #endif` 區塊。

### 4. 切換編譯開關

`Core/Inc/app/app_config.h`：

```c
#define APP_USE_MPU_DMP  1
```

重新建置。`imu_mahony.c` 會整檔停編、`imu_dmp.c` 與 `empl_port.c` 生效
—— 上層程式碼（任務、UI、協定）一行都不用改，這就是 `imu.h`
介面抽象的意義。

### 5. 驗證

- 開機 log 應出現 `[imu ] init ok (backend: DMP)`。
- 若出現 `DMP firmware load fail`：多半是 I2C 寫入逾時，
  檢查接線與上拉；DMP 韌體下載共約 3KB，需連續成功寫入。
- CLI `imu` 指令與 OLED DASH 頁應顯示穩定角度。

## 版本差異備註

| 情況 | 處理 |
|---|---|
| 使用正點原子改版 `inv_mpu.c`（`mpu_init()` 無參數） | `imu_dmp.c` 中 `mpu_init(&int_param)` 改為 `mpu_init()` |
| 檔案內已含 `EMPL_TARGET_STM32` 相關修改 | 保留其修改亦可，只要 `i2c_write` 等符號最終指到可用實作 |
| Flash 超限（F103C8 64KB） | 確認最佳化為 `-Os`；仍不足可設 `APP_USE_CLI 0` 省下 CLI |

## DMP 模式的行為差異

| 項目 | Mahony（預設） | DMP |
|---|---|---|
| 解算位置 | MCU（軟體浮點） | 感測器內部 |
| 陀螺零偏 | 手動校正（`cal`），存 EEPROM | `DMP_FEATURE_GYRO_CAL` 靜置 8 秒自動歸零 |
| `cal` 命令 | 實際執行 1 秒校正 | 直接回報成功（交由 DMP 自理） |
| Yaw | 相對、緩慢漂移 | 相對、緩慢漂移（同樣無磁力計） |
| CPU 佔用 | 每樣本 ~數百 µs | 讀 FIFO 的 I2C 時間 |

兩個後端並存的教學價值：學生可以 A/B 對比同一顆感測器上
「軟體融合 vs 硬體融合」的收斂速度、雜訊與漂移特性。
