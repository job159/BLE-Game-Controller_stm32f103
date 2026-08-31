/**
 * @file    proto.c
 * @brief   協定框架層實作：串流解析狀態機 + 封包組裝。
 */
#include "mw/proto.h"
#include "mw/crc16.h"
#include <string.h>

enum {
    ST_SOF0 = 0,   /* 等 0xAA */
    ST_SOF1,       /* 等 0x55 */
    ST_TYPE,
    ST_SEQ,
    ST_LEN,
    ST_PAYLOAD,
    ST_CRC_LO,
    ST_CRC_HI,
};

void proto_parser_init(proto_parser_t *p, proto_frame_cb_t cb, void *user)
{
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->user = user;
    p->state = ST_SOF0;
}

/* 掉出同步：計數並回到找 SOF 狀態。
 * 註：不回退重掃緩衝區內容 —— 串流下一個 0xAA 自然會再同步，
 * 實作簡單且最壞情況只多丟一個封包。 */
static void resync(proto_parser_t *p)
{
    p->stats.resyncs++;
    p->state = ST_SOF0;
}

static void feed_byte(proto_parser_t *p, uint8_t b)
{
    switch (p->state) {
    case ST_SOF0:
        if (b == PROTO_SOF0) {
            p->state = ST_SOF1;
        }
        break;

    case ST_SOF1:
        if (b == PROTO_SOF1) {
            p->state = ST_TYPE;
        } else if (b != PROTO_SOF0) {
            /* 若又收到 0xAA 則停留在 ST_SOF1（可能是 0xAA 0xAA 0x55） */
            p->state = ST_SOF0;
        }
        break;

    case ST_TYPE:
        p->frame.type = b;
        p->crc_calc = crc16_ccitt_update(0xFFFFu, b);
        p->state = ST_SEQ;
        break;

    case ST_SEQ:
        p->frame.seq = b;
        p->crc_calc = crc16_ccitt_update(p->crc_calc, b);
        p->state = ST_LEN;
        break;

    case ST_LEN:
        if (b > PROTO_MAX_PAYLOAD) {
            resync(p);
            break;
        }
        p->frame.len = b;
        p->crc_calc = crc16_ccitt_update(p->crc_calc, b);
        p->idx = 0u;
        p->state = (b == 0u) ? ST_CRC_LO : ST_PAYLOAD;
        break;

    case ST_PAYLOAD:
        p->frame.payload[p->idx++] = b;
        p->crc_calc = crc16_ccitt_update(p->crc_calc, b);
        if (p->idx >= p->frame.len) {
            p->state = ST_CRC_LO;
        }
        break;

    case ST_CRC_LO:
        p->crc_rx = b;
        p->state = ST_CRC_HI;
        break;

    case ST_CRC_HI:
        p->crc_rx |= (uint16_t)b << 8;
        if (p->crc_rx == p->crc_calc) {
            p->stats.rx_frames++;
            if (p->cb != NULL) {
                p->cb(&p->frame, p->user);
            }
            p->state = ST_SOF0;
        } else {
            p->stats.crc_errors++;
            resync(p);
        }
        break;

    default:
        resync(p);
        break;
    }
}

void proto_parser_feed(proto_parser_t *p, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        feed_byte(p, data[i]);
    }
}

uint16_t proto_build(uint8_t *out, uint16_t cap, uint8_t type, uint8_t seq,
                     const void *payload, uint8_t len)
{
    uint16_t total = (uint16_t)(PROTO_OVERHEAD + len);
    if ((out == NULL) || (cap < total) || (len > PROTO_MAX_PAYLOAD) ||
        ((payload == NULL) && (len > 0u))) {
        return 0u;
    }

    out[0] = PROTO_SOF0;
    out[1] = PROTO_SOF1;
    out[2] = type;
    out[3] = seq;
    out[4] = len;
    if (len > 0u) {
        memcpy(&out[5], payload, len);
    }
    uint16_t crc = crc16_ccitt(&out[2], (uint16_t)(3u + len), 0xFFFFu);
    out[5 + len] = (uint8_t)(crc & 0xFFu);
    out[6 + len] = (uint8_t)(crc >> 8);
    return total;
}
