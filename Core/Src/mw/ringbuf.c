/**
 * @file    ringbuf.c
 * @brief   SPSC 環形緩衝區實作。
 */
#include "mw/ringbuf.h"
#include <string.h>

static bool is_pow2(uint16_t v)
{
    return (v >= 2u) && ((v & (v - 1u)) == 0u);
}

int rb_init(ringbuf_t *rb, uint8_t *mem, uint16_t size)
{
    if ((rb == NULL) || (mem == NULL) || !is_pow2(size)) {
        return APP_EINVAL;
    }
    rb->buf = mem;
    rb->mask = (uint16_t)(size - 1u);
    rb->head = 0u;
    rb->tail = 0u;
    rb->dropped = 0u;
    return APP_OK;
}

uint16_t rb_put(ringbuf_t *rb, const uint8_t *data, uint16_t n)
{
    uint16_t space = rb_free(rb);
    uint16_t to_copy = APP_MIN(n, space);

    if (to_copy < n) {
        rb->dropped += (uint32_t)(n - to_copy);
    }

    uint16_t head = rb->head;
    for (uint16_t i = 0; i < to_copy; i++) {
        rb->buf[(uint16_t)(head + i) & rb->mask] = data[i];
    }
    /* 先寫資料再發布 head：確保消費者看到 head 前資料已就緒 */
    __asm volatile ("" ::: "memory");
    rb->head = (uint16_t)(head + to_copy);
    return to_copy;
}

uint16_t rb_get(ringbuf_t *rb, uint8_t *data, uint16_t n)
{
    uint16_t avail = rb_used(rb);
    uint16_t to_copy = APP_MIN(n, avail);

    uint16_t tail = rb->tail;
    for (uint16_t i = 0; i < to_copy; i++) {
        data[i] = rb->buf[(uint16_t)(tail + i) & rb->mask];
    }
    __asm volatile ("" ::: "memory");
    rb->tail = (uint16_t)(tail + to_copy);
    return to_copy;
}

int rb_getc(ringbuf_t *rb)
{
    if (rb_is_empty(rb)) {
        return -1;
    }
    uint8_t c = rb->buf[rb->tail & rb->mask];
    __asm volatile ("" ::: "memory");
    rb->tail = (uint16_t)(rb->tail + 1u);
    return (int)c;
}

bool rb_putc(ringbuf_t *rb, uint8_t c)
{
    if (rb_free(rb) == 0u) {
        rb->dropped++;
        return false;
    }
    rb->buf[rb->head & rb->mask] = c;
    __asm volatile ("" ::: "memory");
    rb->head = (uint16_t)(rb->head + 1u);
    return true;
}

uint16_t rb_peek_linear(const ringbuf_t *rb, const uint8_t **ptr)
{
    uint16_t used = rb_used(rb);
    uint16_t tail_idx = (uint16_t)(rb->tail & rb->mask);
    uint16_t to_end = (uint16_t)((rb->mask + 1u) - tail_idx);
    *ptr = &rb->buf[tail_idx];
    return APP_MIN(used, to_end);
}

void rb_skip(ringbuf_t *rb, uint16_t n)
{
    rb->tail = (uint16_t)(rb->tail + n);
}

void rb_clear(ringbuf_t *rb)
{
    rb->tail = rb->head;
}
