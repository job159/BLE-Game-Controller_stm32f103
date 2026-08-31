/**
 * @file    app_config.h
 * @brief   專案「單一設定點」：版本、功能開關、參數。
 *
 * 真實產品的做法：所有可調參數集中一處，並以編譯期檢查防呆。
 * 換板子 / 換零件時優先改這裡與 bsp_board.h，不動業務邏輯。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ---- 韌體識別 ---- */
#define APP_FW_NAME             "imu-ble-node"
#define APP_FW_VERSION          "1.0.0"
#define APP_PROTO_VERSION       1        /* BLE 二進位協定版本 */

/* ---- 功能開關 ---- */
/* 姿態解算後端：
 *   0 = 內建 Mahony 互補濾波（純軟體，開箱即用）
 *   1 = InvenSense eMPL DMP（需自行放入 inv_mpu.c 等檔案，見 Docs/dmp_porting.md）*/
#define APP_USE_MPU_DMP         0

#define APP_USE_CLI             1        /* USART1 除錯命令列 */
#define APP_USE_OLED            1        /* OLED 顯示 */

/* ---- OLED 面板變體 ----
 * 市售模組實際晶片混雜（SSD1306/SSD1315/SH1106）。畫面異常時
 * 用 CLI `oled` 命令現場診斷（test/ctrl/com/flip），找到正確組合
 * 後回填這裡固化。SH1106 特徵：需 2 欄偏移 + 不同升壓命令。 */
#define APP_OLED_SH1106         0        /* 1 = SH1106 系控制器 */
#define APP_OLED_COM_PINS       0x12     /* 0xDA 參數：0x12 交錯 / 0x02 順序 */

/* 安全區內縮（px）：模組外框遮到邊緣像素時把 UI 往內收。
 * CLI `oled inset <l> <t> <r> <b>` 可現場微調後回填此處。 */
#define APP_OLED_INSET_LEFT     6
#define APP_OLED_INSET_TOP      2
#define APP_OLED_INSET_RIGHT    2
#define APP_OLED_INSET_BOTTOM   6

/* ---- AT24Cxx EEPROM 型號選擇（決定容量 / 頁大小 / 定址方式） ---- */
#define AT24C02   1
#define AT24C04   2
#define AT24C08   3
#define AT24C16   4
#define AT24C32   5
#define AT24C64   6
#define APP_AT24_TYPE           AT24C32

/* ---- 取樣與更新率 ---- */
#define APP_IMU_PERIOD_MS       10       /* 100Hz 姿態解算 */
#define APP_BTN_PERIOD_MS       10       /* 按鍵掃描 */
#define APP_UI_PERIOD_MS        100      /* OLED 10FPS */
#define APP_COMM_RX_PERIOD_MS   10       /* BLE 收包解析 */
#define APP_TELEMETRY_HZ_DEF    10       /* 姿態回報預設頻率（0=關閉，上限 50） */
#define APP_TELEMETRY_HZ_MAX    50

/* ---- 儲存策略 ---- */
#define APP_STOR_COMMIT_DELAY_MS   2000  /* 最後一次變更後延遲寫入（減少 EEPROM 磨耗） */
#define APP_STOR_COMMIT_FORCE_MS   10000 /* 持續變更時，最久多長時間必須落盤一次 */

/* ---- 按鍵時序 ---- */
#define APP_BTN_DEBOUNCE_MS     20
#define APP_BTN_LONG_MS         1000
#define APP_BTN_DOUBLE_GAP_MS   250

/* ---- 編譯期防呆 ---- */
#if (APP_TELEMETRY_HZ_DEF > APP_TELEMETRY_HZ_MAX)
#error "APP_TELEMETRY_HZ_DEF 不可大於 APP_TELEMETRY_HZ_MAX"
#endif

#endif /* APP_CONFIG_H */
