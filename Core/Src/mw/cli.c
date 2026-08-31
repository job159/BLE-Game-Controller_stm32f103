/**
 * @file    cli.c
 * @brief   CLI 引擎實作。
 */
#include "mw/cli.h"
#include "bsp/bsp_uart.h"
#include <string.h>

static const cli_cmd_t *s_table;
static uint16_t s_count;
static char     s_line[CLI_MAX_LINE];
static uint16_t s_len;

static void prompt(void)
{
    dbg_printf("> ");
}

void cli_init(const cli_cmd_t *table, uint16_t count)
{
    s_table = table;
    s_count = count;
    s_len = 0u;
    /* 執行期輸出一律 ASCII：序列終端機編碼設定五花八門，
     * 英文輸出在任何環境都不會變亂碼（教學註解才用中文） */
    dbg_printf("type 'help' for command list\r\n");
    prompt();
}

void cli_print_help(void)
{
    dbg_printf("commands:\r\n");
    for (uint16_t i = 0; i < s_count; i++) {
        dbg_printf("  %-10s %s\r\n", s_table[i].name, s_table[i].help);
    }
    dbg_printf("  %-10s %s\r\n", "help", "show this list");
}

static void execute(char *line)
{
    char *argv[CLI_MAX_ARGS];
    int argc = 0;

    char *p = line;
    while ((*p != '\0') && (argc < CLI_MAX_ARGS)) {
        while (*p == ' ') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while ((*p != ' ') && (*p != '\0')) {
            p++;
        }
    }
    if (argc == 0) {
        return;
    }

    if (strcmp(argv[0], "help") == 0) {
        cli_print_help();
        return;
    }
    for (uint16_t i = 0; i < s_count; i++) {
        if (strcmp(argv[0], s_table[i].name) == 0) {
            int rc = s_table[i].fn(argc, argv);
            if (rc != APP_OK) {
                dbg_printf("(command returned %d)\r\n", rc);
            }
            return;
        }
    }
    dbg_printf("unknown command '%s', try 'help'\r\n", argv[0]);
}

void cli_poll(void)
{
    int c;
    /* 每輪最多消化 64 字元，避免貼上長文時占用過久 */
    for (int budget = 0; budget < 64; budget++) {
        c = dbg_getc();
        if (c < 0) {
            return;
        }
        if ((c == '\r') || (c == '\n')) {
            dbg_printf("\r\n");
            s_line[s_len] = '\0';
            if (s_len > 0u) {
                execute(s_line);
            }
            s_len = 0u;
            prompt();
        } else if ((c == 0x7F) || (c == '\b')) {   /* backspace */
            if (s_len > 0u) {
                s_len--;
                dbg_printf("\b \b");
            }
        } else if ((c >= 0x20) && (c < 0x7F)) {
            if (s_len < (CLI_MAX_LINE - 1u)) {
                s_line[s_len++] = (char)c;
                char echo = (char)c;
                dbg_write(&echo, 1u);
            }
        }
        /* 其他控制字元靜默忽略 */
    }
}
