/**
 * @file    ringbuf.h
 * @brief   SPSC（單生產者/單消費者）位元組環形緩衝區。
 *
 * 無鎖設計契約（教學重點）：
 *  - head 只由「生產者」寫入、tail 只由「消費者」寫入；
 *    Cortex-M 上對齊的 16-bit 存取是原子的，因此一端在 ISR、
 *    一端在主迴圈時「不需要關中斷」。
 *  - 同一端若有多個 context 存取（例如兩個 ISR 都 put），
 *    呼叫端必須自行以臨界區保護 —— 本模組不隱藏這個代價。
 *  - 容量必須為 2 的冪，索引以 mask 取餘，避免昂貴的除法。
 *
 * 純 C、無硬體相依 → 可直接在 x86 上做單元測試（tests/host）。
 */
#ifndef MW_RINGBUF_H
#define MW_RINGBUF_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t           *buf;
    uint16_t           mask;      /* size - 1（size 為 2 的冪） */
    volatile uint16_t  head;      /* 生產者索引（只增不減，自然環繞） */
    volatile uint16_t  tail;      /* 消費者索引 */
    volatile uint32_t  dropped;   /* 因滿而丟棄的位元組數（診斷用） */
} ringbuf_t;

/**
 * @brief 初始化。size 必須為 2 的冪且 >= 2，否則回傳 APP_EINVAL。
 */
int rb_init(ringbuf_t *rb, uint8_t *mem, uint16_t size);

static inline uint16_t rb_used(const ringbuf_t *rb)
{
    return (uint16_t)(rb->head - rb->tail);
}

static inline uint16_t rb_free(const ringbuf_t *rb)
{
    return (uint16_t)((rb->mask + 1u) - rb_used(rb));
}

static inline bool rb_is_empty(const ringbuf_t *rb)
{
    return rb->head == rb->tail;
}

/** @brief 寫入最多 n bytes，回傳實際寫入數；空間不足的部分計入 dropped */
uint16_t rb_put(ringbuf_t *rb, const uint8_t *data, uint16_t n);

/** @brief 讀出最多 n bytes，回傳實際讀出數 */
uint16_t rb_get(ringbuf_t *rb, uint8_t *data, uint16_t n);

/** @brief 單位元組讀取；-1 表示緩衝區為空 */
int rb_getc(ringbuf_t *rb);

/** @brief 單位元組寫入；false 表示已滿（計入 dropped） */
bool rb_putc(ringbuf_t *rb, uint8_t c);

/**
 * @brief 取得 tail 起連續可讀區段的指標與長度（零複製，供 DMA/批次傳送）。
 *        讀走後以 rb_skip() 前進。
 */
uint16_t rb_peek_linear(const ringbuf_t *rb, const uint8_t **ptr);

/** @brief 消費 n bytes（必須 <= rb_used()） */
void rb_skip(ringbuf_t *rb, uint16_t n);

/** @brief 清空（僅可由消費者端呼叫） */
void rb_clear(ringbuf_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* MW_RINGBUF_H */
