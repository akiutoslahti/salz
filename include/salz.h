/*
 * salz.h - Defines for SA based LZ compressor
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#ifndef SALZ_H
#define SALZ_H

#include <stddef.h>
#include <stdint.h>

#ifdef ENABLE_STATS
#include "common.h"

struct stats {
    uint64_t sa_time;
    uint64_t psv_nsv_time;
    uint64_t factor_time;
    uint64_t mincost_time;
    uint64_t encode_time;
};

extern struct stats *get_stats(void);
#endif

struct io_stream {
    uint8_t *buf;
    size_t buf_len;
    size_t buf_pos;
    uint64_t bits;
    size_t bits_avail;
    size_t bits_pos;
};

struct encode_ctx {
    int32_t *aux1;
    size_t aux1_len;
    int32_t *aux2;
    size_t aux2_len;

    struct io_stream main;
    struct io_stream ordinals;
};

extern void encode_ctx_init(struct encode_ctx **ctx, size_t src_len);

extern void encode_ctx_fini(struct encode_ctx **ctx);

extern uint32_t salz_encode_default(struct encode_ctx *ctx, uint8_t *src,
        size_t src_len, uint8_t *dst, size_t dst_len);

extern uint32_t salz_decode_default(uint8_t *src, size_t src_len,
        uint8_t *dst, size_t dst_len);

#endif /* !SALZ_H */
