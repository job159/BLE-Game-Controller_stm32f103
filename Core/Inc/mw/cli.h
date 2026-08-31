/**
 * @file    cli.h
 * @brief   除錯命令列引擎（USART1）。
 *
 * 引擎只負責：行編輯（echo / backspace）、斷詞、查表分派。
 * 命令內容由應用層提供（app/app_cli.c）—— 引擎可攜、命令跟產品走。
 */
#ifndef MW_CLI_H
#define MW_CLI_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLI_MAX_LINE   96
#define CLI_MAX_ARGS   8

typedef int (*cli_cmd_fn_t)(int argc, char **argv);

typedef struct {
    const char  *name;
    const char  *help;
    cli_cmd_fn_t fn;
} cli_cmd_t;

/** @brief 初始化並註冊命令表（表必須為靜態生命週期） */
void cli_init(const cli_cmd_t *table, uint16_t count);

/** @brief 週期輪詢：消化 UART RX、執行完整命令列 */
void cli_poll(void);

/** @brief 印出所有命令（help 指令由引擎內建） */
void cli_print_help(void);

#ifdef __cplusplus
}
#endif

#endif /* MW_CLI_H */
