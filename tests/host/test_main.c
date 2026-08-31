/**
 * @file    test_main.c
 * @brief   主機端單元測試：驗證與硬體無關的中介層模組。
 *
 * 這是「可移植性」的直接紅利：ringbuf / crc16 / proto / button
 * 不含任何 HAL 相依，直接在 PC 上以原生 gcc 編譯執行，
 * 幾秒鐘跑完回歸 —— 不必燒錄板子就能攔下大部分邏輯錯誤。
 *
 * 建置：cd tests/host && make && ./run_tests
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "mw/ringbuf.h"
#include "mw/crc16.h"
#include "mw/proto.h"
#include "mw/button.h"
#include "mw/xutil.h"

static int s_checks;
#define CHECK(cond) do { \
        s_checks++; \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1; \
        } \
    } while (0)

/* ------------------------------------------------------------------ */

static int test_crc16(void)
{
    /* CCITT-FALSE 標準驗證向量 */
    CHECK(crc16_ccitt((const uint8_t *)"123456789", 9, 0xFFFF) == 0x29B1);
    CHECK(crc16_ccitt(NULL, 0, 0xFFFF) == 0xFFFF);
    /* 增量計算 = 一次計算 */
    uint16_t inc = 0xFFFF;
    const char *msg = "hello";
    for (int i = 0; i < 5; i++) {
        inc = crc16_ccitt_update(inc, (uint8_t)msg[i]);
    }
    CHECK(inc == crc16_ccitt((const uint8_t *)msg, 5, 0xFFFF));
    return 0;
}

static int test_ringbuf(void)
{
    uint8_t mem[16];
    ringbuf_t rb;
    CHECK(rb_init(&rb, mem, 16) == APP_OK);
    CHECK(rb_init(&rb, mem, 15) == APP_EINVAL);   /* 非 2 的冪 */
    CHECK(rb_init(&rb, mem, 16) == APP_OK);

    CHECK(rb_is_empty(&rb));
    CHECK(rb_put(&rb, (const uint8_t *)"abcdef", 6) == 6);
    CHECK(rb_used(&rb) == 6);

    uint8_t out[16];
    CHECK(rb_get(&rb, out, 4) == 4);
    CHECK(memcmp(out, "abcd", 4) == 0);

    /* 環繞測試：寫滿 → 索引跨界 → 資料完整 */
    CHECK(rb_put(&rb, (const uint8_t *)"0123456789ABCD", 14) == 14);
    CHECK(rb_free(&rb) == 0);
    CHECK(rb_put(&rb, (const uint8_t *)"x", 1) == 0);   /* 滿了丟棄 */
    CHECK(rb.dropped == 1);
    CHECK(rb_get(&rb, out, 16) == 16);
    CHECK(memcmp(out, "ef0123456789ABCD", 16) == 0);

    /* peek_linear + skip（DMA 零複製路徑）。
     * rb_clear 不歸零索引（tail=head），此時 tail 索引為 20（mod16=4）；
     * 再推進 10 讓 tail 停在實體索引 14，使 4B 資料被緩衝區尾端切成 2+2 */
    rb_clear(&rb);
    CHECK(rb_put(&rb, (const uint8_t *)"0123456789", 10) == 10);
    rb_skip(&rb, 10);
    CHECK(rb_put(&rb, (const uint8_t *)"WXYZ", 4) == 4);
    const uint8_t *p;
    uint16_t n = rb_peek_linear(&rb, &p);
    CHECK(n == 2);                          /* 到緩衝區尾端只剩 2B */
    CHECK(memcmp(p, "WX", 2) == 0);
    rb_skip(&rb, n);
    n = rb_peek_linear(&rb, &p);
    CHECK(n == 2);                          /* 環繞後的頭段 */
    CHECK(memcmp(p, "YZ", 2) == 0);
    return 0;
}

/* ------------------------------------------------------------------ */

static proto_frame_t s_last_frame;
static int s_frame_count;

static void on_frame(const proto_frame_t *f, void *user)
{
    (void)user;
    s_last_frame = *f;
    s_frame_count++;
}

static int test_proto(void)
{
    uint8_t frame[64];
    const uint8_t payload[] = { 0x11, 0x22, 0x33 };
    uint16_t n = proto_build(frame, sizeof(frame), 0x01, 7, payload, 3);
    CHECK(n == 3 + PROTO_OVERHEAD);

    proto_parser_t parser;
    proto_parser_init(&parser, on_frame, NULL);

    /* 前後夾雜垃圾位元組 */
    s_frame_count = 0;
    uint8_t stream[80];
    stream[0] = 0x00;
    stream[1] = 0xAA;      /* 假 SOF 開頭 */
    memcpy(&stream[2], frame, n);
    stream[2 + n] = 0xFF;
    proto_parser_feed(&parser, stream, (uint16_t)(n + 3));
    CHECK(s_frame_count == 1);
    CHECK(s_last_frame.type == 0x01);
    CHECK(s_last_frame.seq == 7);
    CHECK(s_last_frame.len == 3);
    CHECK(memcmp(s_last_frame.payload, payload, 3) == 0);

    /* CRC 破壞 → 丟棄 + 統計；之後的好封包仍可解 */
    uint8_t bad[64];
    memcpy(bad, frame, n);
    bad[n - 1] ^= 0xFF;
    proto_parser_feed(&parser, bad, n);
    proto_parser_feed(&parser, frame, n);
    CHECK(s_frame_count == 2);
    CHECK(parser.stats.crc_errors == 1);

    /* 逐位元組餵入（模擬 UART 碎片化） */
    for (uint16_t i = 0; i < n; i++) {
        proto_parser_feed(&parser, &frame[i], 1);
    }
    CHECK(s_frame_count == 3);

    /* 空 payload 封包 */
    n = proto_build(frame, sizeof(frame), 0x80, 1, NULL, 0);
    CHECK(n == PROTO_OVERHEAD);
    proto_parser_feed(&parser, frame, n);
    CHECK(s_frame_count == 4);
    CHECK(s_last_frame.len == 0);
    return 0;
}

/* ------------------------------------------------------------------ */

static bool s_btn_raw;
static int  s_evt_count[8];

static bool btn_read(void *user)
{
    (void)user;
    return s_btn_raw;
}

static void btn_cb(uint8_t id, btn_event_t evt, void *user)
{
    (void)id; (void)user;
    s_evt_count[evt]++;
}

/* 以 1ms 步進模擬 now 前進 */
static void btn_advance(btn_t *b, uint32_t *now, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        (*now)++;
        btn_poll(b, *now);
    }
}

static int test_button(void)
{
    static const btn_config_t cfg = {
        .debounce_ms = 20, .long_ms = 1000, .double_gap_ms = 250,
    };
    btn_t b;
    uint32_t now = 0;
    memset(s_evt_count, 0, sizeof(s_evt_count));
    btn_init(&b, 0, &cfg, btn_read, NULL, btn_cb, NULL);
    btn_enable_double(&b, false);   /* KEY0 模式：零延遲單擊 */

    /* 單擊：按 50ms 放開 */
    s_btn_raw = true;  btn_advance(&b, &now, 50);
    CHECK(s_evt_count[BTN_EVT_PRESS] == 1);
    s_btn_raw = false; btn_advance(&b, &now, 50);
    CHECK(s_evt_count[BTN_EVT_CLICK] == 1);
    CHECK(s_evt_count[BTN_EVT_RELEASE] == 1);

    /* 彈跳：5ms 毛刺不得產生任何事件 */
    s_btn_raw = true;  btn_advance(&b, &now, 5);
    s_btn_raw = false; btn_advance(&b, &now, 50);
    CHECK(s_evt_count[BTN_EVT_PRESS] == 1);

    /* 長按：1.2 秒 → LONG_PRESS 一次、無 CLICK */
    s_btn_raw = true;  btn_advance(&b, &now, 1200);
    CHECK(s_evt_count[BTN_EVT_LONG_PRESS] == 1);
    s_btn_raw = false; btn_advance(&b, &now, 50);
    CHECK(s_evt_count[BTN_EVT_CLICK] == 1);   /* 沒有增加 */

    /* 雙擊模式（KEY1）：兩次快速點擊 → DOUBLE，且不發 CLICK */
    memset(s_evt_count, 0, sizeof(s_evt_count));
    btn_enable_double(&b, true);
    s_btn_raw = true;  btn_advance(&b, &now, 40);
    s_btn_raw = false; btn_advance(&b, &now, 60);
    s_btn_raw = true;  btn_advance(&b, &now, 40);
    s_btn_raw = false; btn_advance(&b, &now, 60);
    CHECK(s_evt_count[BTN_EVT_DOUBLE] == 1);
    btn_advance(&b, &now, 400);
    CHECK(s_evt_count[BTN_EVT_CLICK] == 0);

    /* 雙擊模式的單擊：窗口逾時後補發 CLICK */
    s_btn_raw = true;  btn_advance(&b, &now, 40);
    s_btn_raw = false; btn_advance(&b, &now, 400);
    CHECK(s_evt_count[BTN_EVT_CLICK] == 1);
    return 0;
}

static int test_xutil(void)
{
    char buf[16];
    (void)fix100_to_str(buf, sizeof(buf), 1234);
    CHECK(strcmp(buf, "12.34") == 0);
    (void)fix100_to_str(buf, sizeof(buf), -507);
    CHECK(strcmp(buf, "-5.07") == 0);
    (void)fix100_to_str(buf, sizeof(buf), 0);
    CHECK(strcmp(buf, "0.00") == 0);
    (void)uptime_to_str(buf, sizeof(buf), 3661);
    CHECK(strcmp(buf, "01:01:01") == 0);
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= test_crc16();
    rc |= test_ringbuf();
    rc |= test_proto();
    rc |= test_button();
    rc |= test_xutil();
    if (rc == 0) {
        printf("ALL TESTS PASSED (%d checks)\n", s_checks);
    }
    return rc;
}
