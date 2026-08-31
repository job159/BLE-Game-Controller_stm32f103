/**
 * @file    xutil.c
 * @brief   小型格式化工具實作。
 */
#include "mw/xutil.h"
#include <stdio.h>

int fix100_to_str(char *buf, size_t cap, int32_t v)
{
    if ((buf == NULL) || (cap == 0u)) {
        return 0;
    }
    const char *sign = "";
    uint32_t mag;
    if (v < 0) {
        sign = "-";
        /* 以無號運算取絕對值，避免 INT32_MIN 取負的未定義行為 */
        mag = (uint32_t)(-(int64_t)v);
    } else {
        mag = (uint32_t)v;
    }
    int n = snprintf(buf, cap, "%s%lu.%02lu", sign,
                     (unsigned long)(mag / 100u), (unsigned long)(mag % 100u));
    return (n < 0) ? 0 : APP_MIN(n, (int)cap - 1);
}

int uptime_to_str(char *buf, size_t cap, uint32_t seconds)
{
    if ((buf == NULL) || (cap == 0u)) {
        return 0;
    }
    uint32_t h = seconds / 3600u;
    uint32_t m = (seconds % 3600u) / 60u;
    uint32_t s = seconds % 60u;
    int n = snprintf(buf, cap, "%02lu:%02lu:%02lu",
                     (unsigned long)h, (unsigned long)m, (unsigned long)s);
    return (n < 0) ? 0 : APP_MIN(n, (int)cap - 1);
}
