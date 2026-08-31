/**
 * @file    app_main.h
 * @brief   應用程式進入點（由 CubeMX main.c 的 USER CODE 區呼叫）。
 */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 應用初始化：於所有 MX_*_Init 完成後呼叫一次 */
void app_main_init(void);

/** @brief 主迴圈：反覆呼叫（內部為協作式排程器） */
void app_main_loop(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MAIN_H */
