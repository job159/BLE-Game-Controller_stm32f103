/**
 * @file    app_cli.c
 * @brief   除錯命令表：把系統內部狀態攤在陽光下。
 *
 * 可觀測性（observability）是穩定韌體的支柱 —— 出問題時
 * 不用接除錯器，接上序列埠敲 `stat` 就能看到全部健康指標。
 */
#include "app/app_tasks.h"
#include "app/app_config.h"
#include "bsp/bsp.h"
#include "bsp/bsp_uart.h"
#include "bsp/bsp_i2c.h"
#include "mw/cli.h"
#include "mw/sched.h"
#include "mw/storage.h"
#include "mw/xutil.h"
#include "drivers/imu.h"
#include "drivers/drv_at24cxx.h"
#include "drivers/drv_ssd1306.h"
#include "drivers/drv_gfx.h"
#include "stm32f1xx_hal.h"      /* HAL_GetTick */
#include <stdlib.h>
#include <string.h>

#if APP_USE_CLI

static int cmd_ver(int argc, char **argv)
{
    APP_UNUSED(argc); APP_UNUSED(argv);
    dbg_printf("%s v%s  proto v%d  build %s %s\r\n",
               APP_FW_NAME, APP_FW_VERSION, APP_PROTO_VERSION,
               __DATE__, __TIME__);
    dbg_printf("imu backend: %s\r\n", (APP_USE_MPU_DMP != 0) ? "DMP" : "Mahony");
    return APP_OK;
}

static int cmd_stat(int argc, char **argv)
{
    APP_UNUSED(argc); APP_UNUSED(argv);
    char up[10];
    (void)uptime_to_str(up, sizeof(up), HAL_GetTick() / 1000u);
    dbg_printf("uptime %s  cpu %u%%  reset %s  boots %lu\r\n",
               up, (unsigned)sched_cpu_percent(), bsp_reset_cause_str(),
               (unsigned long)stor_get()->boot_count);

    dbg_printf("-- tasks --\r\n");
    dbg_printf("%-8s %6s %8s %8s %8s %5s\r\n",
               "name", "per", "runs", "last_us", "worst_us", "ovr");
    for (int i = 0; i < sched_task_count(); i++) {
        const sched_task_info_t *t = sched_task_info(i);
        dbg_printf("%-8s %6lu %8lu %8lu %8lu %5lu\r\n",
                   t->name, (unsigned long)t->period_ms,
                   (unsigned long)t->runs, (unsigned long)t->last_us,
                   (unsigned long)t->worst_us, (unsigned long)t->overruns);
    }

    const bsp_uart_stats_t *bu = ble_stats();
    const bsp_uart_stats_t *du = dbg_stats();
    dbg_printf("-- uart --\r\n");
    dbg_printf("ble: tx %lu rx %lu drop %lu/%lu err %lu restart %lu\r\n",
               (unsigned long)bu->tx_bytes, (unsigned long)bu->rx_bytes,
               (unsigned long)bu->tx_dropped, (unsigned long)bu->rx_dropped,
               (unsigned long)bu->hw_errors, (unsigned long)bu->rx_restarts);
    dbg_printf("dbg: tx %lu rx %lu drop %lu/%lu err %lu\r\n",
               (unsigned long)du->tx_bytes, (unsigned long)du->rx_bytes,
               (unsigned long)du->tx_dropped, (unsigned long)du->rx_dropped,
               (unsigned long)du->hw_errors);

    const proto_stats_t *ps = comm_proto_stats();
    dbg_printf("-- proto --\r\n");
    dbg_printf("rx_frames %lu  tx_frames %lu  crc_err %lu  resync %lu\r\n",
               (unsigned long)ps->rx_frames,
               (unsigned long)app_state()->comm_tx_frames,
               (unsigned long)ps->crc_errors, (unsigned long)ps->resyncs);

    const bsp_i2c_stats_t *i1 = bsp_i2c_stats(BSP_I2C_SENSOR);
    const bsp_i2c_stats_t *i2 = bsp_i2c_stats(BSP_I2C_DISP);
    dbg_printf("-- i2c --\r\n");
    dbg_printf("sensor: ok %lu err %lu recover %lu | disp: ok %lu err %lu recover %lu\r\n",
               (unsigned long)i1->xfer_ok, (unsigned long)i1->xfer_err,
               (unsigned long)i1->recover_cnt,
               (unsigned long)i2->xfer_ok, (unsigned long)i2->xfer_err,
               (unsigned long)i2->recover_cnt);

    dbg_printf("-- storage --\r\n");
    dbg_printf("online %d  addr 0x%02x  dirty %d  seq %u  commits %lu  fails %lu  src %lu\r\n",
               stor_healthy() ? 1 : 0, at24_dev_addr(),
               stor_is_dirty() ? 1 : 0, (unsigned)stor_seq(),
               (unsigned long)stor_stats()->commits,
               (unsigned long)stor_stats()->commit_fails,
               (unsigned long)stor_stats()->load_source);

    dbg_printf("-- health --  imu:%d oled:%d eeprom:%d\r\n",
               imu_healthy() ? 1 : 0, oled_ok() ? 1 : 0,
               stor_healthy() ? 1 : 0);
    return APP_OK;
}

static int cmd_imu(int argc, char **argv)
{
    APP_UNUSED(argc); APP_UNUSED(argv);
    const imu_state_t *s = imu_get();
    char r[12], p[12], y[12], t[12];
    (void)fix100_to_str(r, sizeof(r), (int32_t)(s->roll_deg * 100.0f));
    (void)fix100_to_str(p, sizeof(p), (int32_t)(s->pitch_deg * 100.0f));
    (void)fix100_to_str(y, sizeof(y), (int32_t)(s->yaw_deg * 100.0f));
    (void)fix100_to_str(t, sizeof(t), s->temp_cdegc);
    dbg_printf("roll %s  pitch %s  yaw %s  temp %sC\r\n", r, p, y, t);
    dbg_printf("samples %lu  errors %lu  int %lu  healthy %d\r\n",
               (unsigned long)s->sample_count, (unsigned long)s->error_count,
               (unsigned long)imu_int_count(), imu_healthy() ? 1 : 0);
    return APP_OK;
}

static int cmd_clicks(int argc, char **argv)
{
    if ((argc >= 2) && (strcmp(argv[1], "reset") == 0)) {
        stor_get()->click_count = 0u;
        stor_mark_dirty();
        (void)stor_commit_now();
        comm_send_event(PROTO_EV_CLICKS_RESET, 0u);
        dbg_printf("clicks reset\r\n");
        return APP_OK;
    }
    dbg_printf("clicks = %lu\r\n", (unsigned long)stor_get()->click_count);
    return APP_OK;
}

static int cmd_rate(int argc, char **argv)
{
    if (argc < 2) {
        dbg_printf("telemetry = %u Hz (usage: rate <0..%d>)\r\n",
                   (unsigned)comm_telemetry_hz(), APP_TELEMETRY_HZ_MAX);
        return APP_OK;
    }
    int hz = atoi(argv[1]);
    if ((hz < 0) || (hz > APP_TELEMETRY_HZ_MAX)) {
        dbg_printf("range 0..%d\r\n", APP_TELEMETRY_HZ_MAX);
        return APP_EINVAL;
    }
    comm_set_telemetry_hz((uint8_t)hz);
    return APP_OK;
}

static int cmd_cal(int argc, char **argv)
{
    APP_UNUSED(argc); APP_UNUSED(argv);
    if (!imu_healthy()) {
        dbg_printf("imu offline\r\n");
        return APP_ENODEV;
    }
    app_state()->cal_request = true;
    dbg_printf("calibration scheduled - keep device still ~1s\r\n");
    return APP_OK;
}

static int cmd_save(int argc, char **argv)
{
    APP_UNUSED(argc); APP_UNUSED(argv);
    int rc = stor_commit_now();
    dbg_printf("commit %s\r\n", (rc == APP_OK) ? "ok" : "FAILED");
    return rc;
}

static int cmd_dump(int argc, char **argv)
{
    uint16_t addr = 0u;
    uint16_t len = 64u;
    if (argc >= 2) {
        addr = (uint16_t)strtoul(argv[1], NULL, 0);
    }
    if (argc >= 3) {
        len = (uint16_t)strtoul(argv[2], NULL, 0);
    }
    if ((addr >= at24_size()) || (len == 0u)) {
        dbg_printf("usage: dump [addr] [len]  (eeprom %u bytes)\r\n",
                   (unsigned)at24_size());
        return APP_EINVAL;
    }
    len = APP_MIN(len, (uint16_t)(at24_size() - addr));

    uint8_t row[16];
    for (uint16_t off = 0u; off < len; off += 16u) {
        uint16_t n = APP_MIN((uint16_t)16u, (uint16_t)(len - off));
        if (at24_read((uint16_t)(addr + off), row, n) != APP_OK) {
            dbg_printf("read error @0x%04x\r\n", addr + off);
            return APP_ERR;
        }
        dbg_printf("%04x:", addr + off);
        for (uint16_t i = 0u; i < n; i++) {
            dbg_printf(" %02x", row[i]);
        }
        dbg_printf("\r\n");
        dbg_flush();   /* 逐行送出，避免一次擠爆 TX ring */
    }
    return APP_OK;
}

/* USART2（BLE 模組側）鮑率現場切換。典型流程 —— HC-42 出廠 9600：
 *   ble 9600 → bridge → AT / AT+BAUD=115200（需 CRLF 結尾）
 *   → Ctrl+] → ble 115200 → 之後模組永久 115200 */
static int cmd_ble(int argc, char **argv)
{
    if (argc < 2) {
        dbg_printf("usart2 baud = %lu (usage: ble <1200..921600>)\r\n",
                   (unsigned long)ble_get_baud());
        return APP_OK;
    }
    uint32_t baud = strtoul(argv[1], NULL, 0);
    int rc = ble_set_baud(baud);
    if (rc == APP_OK) {
        dbg_printf("usart2 baud -> %lu (runtime only; reboot restores %u)\r\n",
                   (unsigned long)baud, 115200u);
    } else {
        dbg_printf("invalid baud (1200..921600)\r\n");
    }
    return rc;
}

/* OLED 面板現場診斷：畫面異常時依序嘗試
 *   oled test → oled ctrl sh1106 → oled com 02 → oled flip
 * 找到正確組合後回填 app_config.h 的 APP_OLED_* 固化。 */
static int cmd_oled(int argc, char **argv)
{
    oled_cfg_t *cfg = oled_cfg();

    if (argc < 2) {
        dbg_printf("addr 0x%02x  ok %d  ctrl %s  com 0x%02x  rot180 %d  test %d\r\n",
                   oled_addr(), oled_ok() ? 1 : 0,
                   cfg->sh1106 ? "sh1106" : "ssd1306",
                   cfg->com_pins, cfg->rotate180 ? 1 : 0,
                   app_state()->ui_test_mode ? 1 : 0);
        dbg_printf("view %ux%u (inset from %ux%u)\r\n",
                   (unsigned)gfx_width(), (unsigned)gfx_height(),
                   (unsigned)OLED_WIDTH, (unsigned)OLED_HEIGHT);
        dbg_printf("usage: oled test | ctrl <ssd1306|sh1106> | com <12|02> | "
                   "flip | inset <l> <t> <r> <b>\r\n");
        return APP_OK;
    }

    if (strcmp(argv[1], "test") == 0) {
        app_state()->ui_test_mode = !app_state()->ui_test_mode;
        dbg_printf("test pattern %s\r\n",
                   app_state()->ui_test_mode ? "on" : "off");
        return APP_OK;
    }

    if (strcmp(argv[1], "inset") == 0) {
        if (argc < 6) {
            dbg_printf("usage: oled inset <left> <top> <right> <bottom>  (px)\r\n");
            return APP_EINVAL;
        }
        gfx_set_inset((uint8_t)atoi(argv[2]), (uint8_t)atoi(argv[3]),
                      (uint8_t)atoi(argv[4]), (uint8_t)atoi(argv[5]));
        dbg_printf("view %ux%u - bake into app_config.h APP_OLED_INSET_*\r\n",
                   (unsigned)gfx_width(), (unsigned)gfx_height());
        return APP_OK;
    }

    int rc;
    if ((strcmp(argv[1], "ctrl") == 0) && (argc >= 3)) {
        if (strcmp(argv[2], "sh1106") == 0) {
            cfg->sh1106 = true;
        } else if (strcmp(argv[2], "ssd1306") == 0) {
            cfg->sh1106 = false;
        } else {
            return APP_EINVAL;
        }
        rc = oled_reinit();
    } else if ((strcmp(argv[1], "com") == 0) && (argc >= 3)) {
        cfg->com_pins = (strcmp(argv[2], "02") == 0) ? 0x02u : 0x12u;
        rc = oled_reinit();
    } else if (strcmp(argv[1], "flip") == 0) {
        cfg->rotate180 = !cfg->rotate180;
        rc = oled_reinit();
    } else {
        return APP_EINVAL;
    }
    dbg_printf("reinit %s\r\n", (rc == APP_OK) ? "ok" : "FAILED");
    return rc;
}

static int cmd_bridge(int argc, char **argv)
{
    APP_UNUSED(argc); APP_UNUSED(argv);
    dbg_printf("bridge mode: console <-> BLE module, exit with Ctrl+]\r\n");
    app_state()->bridge_mode = true;
    return APP_OK;
}

static int cmd_reboot(int argc, char **argv)
{
    APP_UNUSED(argc); APP_UNUSED(argv);
    dbg_printf("rebooting...\r\n");
    (void)stor_commit_now();
    bsp_system_reset();
    return APP_OK;   /* not reached */
}

static const cli_cmd_t s_cmds[] = {
    { "ver",    "firmware version",                    cmd_ver    },
    { "stat",   "system / task / io statistics",       cmd_stat   },
    { "imu",    "current attitude",                    cmd_imu    },
    { "clicks", "show or reset click counter [reset]", cmd_clicks },
    { "rate",   "get/set telemetry rate [hz]",         cmd_rate   },
    { "cal",    "gyro calibration (device still)",     cmd_cal    },
    { "save",   "commit record to eeprom now",         cmd_save   },
    { "dump",   "hex-dump eeprom [addr] [len]",        cmd_dump   },
    { "oled",   "panel diag: test|ctrl|com|flip",      cmd_oled   },
    { "ble",    "get/set usart2 baud [rate]",          cmd_ble    },
    { "bridge", "uart bridge to BLE module",           cmd_bridge },
    { "reboot", "software reset",                      cmd_reboot },
};

void app_cli_init(void)
{
    cli_init(s_cmds, (uint16_t)APP_ARRAY_COUNT(s_cmds));
}

#else  /* !APP_USE_CLI */

void app_cli_init(void)
{
}

#endif /* APP_USE_CLI */
