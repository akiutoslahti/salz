/*
 * salz.c - SA based LZ compressor
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "salz.h"
#include "divsufsort.h"

#ifdef NDEBUG
#   ifndef assert
#       define assert(condition) do {} while(0)
#   endif
#   define debug(...) do {} while(0)
#else
#   include <assert.h>
#   include <stdio.h>
#   define debug(...)                       \
        do {                                \
            fprintf(stderr, "(%s:%d) - ",   \
                    __func__, __LINE__);    \
            fprintf(stderr, __VA_ARGS__);   \
            fprintf(stderr, "\n");          \
        } while (0)
#endif

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) < (b)) ? (b) : (a))

#define divup(a, b) (((a) + (b) - 1) / (b))

#define unused(var) ((void)var)

#ifdef ENABLE_STATS
    struct stats st;

    struct stats *get_stats(void)
    {
        return &st;
    }

    static uint64_t time_ns;

#   define start_clock() do { time_ns = get_time_ns(); } while(0)
#   define increment_clock(dst)             \
        do {                                \
            dst += get_time_ns() - time_ns; \
            time_ns = get_time_ns();        \
        } while (0)
#endif

static inline uint16_t read_u16(uint8_t *src, size_t pos)
{
    uint16_t val;
    memcpy(&val, &src[pos], sizeof(val));
    return val;
}

static inline uint64_t read_u64(uint8_t *src, size_t pos)
{
    uint64_t val;
    memcpy(&val, &src[pos], sizeof(val));
    return val;
}

static inline void write_u16(uint8_t *dst, size_t pos, uint16_t val)
{
    memcpy(&dst[pos], &val, sizeof(val));
}

static inline void write_u64(uint8_t *dst, size_t pos, uint64_t val)
{
    memcpy(&dst[pos], &val, sizeof(val));
}

static inline void copy(uint8_t *src, size_t src_pos, uint8_t *dst,
        size_t dst_pos, size_t copy_len)
{
    memcpy(&dst[dst_pos], &src[src_pos], copy_len);
}

static inline void copy_oaat(uint8_t *src, size_t src_pos, uint8_t *dst,
        size_t dst_pos, size_t copy_len)
{
    while (copy_len > 0) {
        dst[dst_pos] = src[src_pos];
        dst_pos += 1;
        src_pos += 1;
        copy_len -= 1;
    }
}

struct vnibble_decode_ctx {
    uint8_t *buf;
    size_t len;
    size_t pos;
    uint64_t nibbles;
    uint16_t bits;
    size_t nibbles_left;
    size_t bits_left;
};

static inline void vnibble_decode_ctx_init(struct vnibble_decode_ctx *ctx,
        uint8_t *buf, size_t buf_len)
{
    ctx->buf = buf;
    ctx->len = buf_len;
    ctx->pos = 10;
    ctx->nibbles = read_u64(buf, 0);
    ctx->bits = read_u16(buf, 8);
    ctx->nibbles_left = 16;
    ctx->bits_left = 16;
}

struct vnibble_encode_ctx {
    uint8_t *buf;
    size_t len;
    size_t pos;
    size_t nibbles_pos;
    size_t bits_pos;
    uint64_t nibbles;
    uint16_t bits;
    size_t nibbles_left;
    size_t bits_left;
};

static inline void vnibble_encode_ctx_init(struct vnibble_encode_ctx *ctx,
        uint8_t *buf, size_t buf_len)
{
    ctx->buf = buf;
    ctx->len = buf_len;
    ctx->pos = 10;
    ctx->nibbles_pos = 0;
    ctx->bits_pos = 8;
    ctx->nibbles = 0;
    ctx->bits = 0;
    ctx->nibbles_left = 16;
    ctx->bits_left = 16;
}

static inline void vnibble_encode_ctx_fini(struct vnibble_encode_ctx *ctx)
{
    ctx->nibbles <<= (ctx->nibbles_left * 4);
    write_u64(ctx->buf, ctx->nibbles_pos, ctx->nibbles);
    ctx->bits <<= (ctx->bits_left);
    write_u16(ctx->buf, ctx->bits_pos, ctx->bits);
}

static inline uint8_t read_bit(struct vnibble_decode_ctx *ctx)
{
    if (ctx->bits_left == 0) {
        assert(ctx->pos + 2 - 1 < ctx->len);
        ctx->bits = read_u64(ctx->buf, ctx->pos);
        ctx->pos += 2;
        ctx->bits_left = 16;
    }

    uint8_t ret = (ctx->bits & 0x8000) ? 1 : 0;
    ctx->bits <<= 1;
    ctx->bits_left -= 1;

    return ret;
}

static inline uint8_t read_nibble(struct vnibble_decode_ctx *ctx)
{
    if (ctx->nibbles_left == 0) {
        assert(ctx->pos + 8 - 1 < ctx->len);
        ctx->nibbles = read_u64(ctx->buf, ctx->pos);
        ctx->pos += 8;
        ctx->nibbles_left = 16;
    }

    uint8_t ret = (ctx->nibbles & 0xf000000000000000llu) >> 60;
    ctx->nibbles <<= 4;
    ctx->nibbles_left -= 1;

    return ret;
}

static inline void write_bit(struct vnibble_encode_ctx *ctx, uint8_t flag)
{
    if (ctx->bits_left == 0) {
        write_u16(ctx->buf, ctx->bits_pos, ctx->bits);
        ctx->bits = 0;
        ctx->bits_left = 16;
        ctx->bits_pos = ctx->pos;
        ctx->pos += 2;
    }

    ctx->bits = (ctx->bits << 1) | flag;
    ctx->bits_left -= 1;
}

static inline void write_nibble(struct vnibble_encode_ctx *ctx, uint8_t nibble)
{
    if (ctx->nibbles_left == 0) {
        write_u64(ctx->buf, ctx->nibbles_pos, ctx->nibbles);
        ctx->nibbles = 0;
        ctx->nibbles_left = 16;
        ctx->nibbles_pos = ctx->pos;
        ctx->pos += 8;
    }

    ctx->nibbles = (ctx->nibbles << 4) | nibble;
    ctx->nibbles_left -= 1;
}

static inline size_t gr_bitsize(uint32_t val, size_t k)
{
    return (val >> k) + 1 + k;
}

static inline void write_gr(struct vnibble_encode_ctx *ctx, uint32_t val, size_t k)
{
    uint32_t q = val >> k;

    while (q != 0) {
        write_bit(ctx, 0);
        q -= 1;
    }
    write_bit(ctx, 1);

    for (int32_t i = k - 1; i >= 0; i--)
        write_bit(ctx, val & (1 << i) ? 1 : 0);
}

static inline void read_gr(struct vnibble_decode_ctx *ctx, uint32_t *res, size_t k)
{
    uint32_t q = 0;

    while (read_bit(ctx) == 0)
        q += 1;

    *res = q;

    for (int32_t i = k - 1; i >= 0; i--)
        *res = (*res << 1) | read_bit(ctx);
}

static inline size_t vnibble_size(uint32_t val)
{
    if (val < (1u << 3))
        return 1;

    if (val < (1u << 6))
        return 2;

    if (val < (1u << 9))
        return 3;

    if (val < (1u << 12))
        return 4;

    if (val < (1u << 15))
        return 5;

    if (val < (1u << 18))
        return 6;

    if (val < (1u << 21))
        return 7;

    if (val < (1u << 24))
        return 8;

    if (val < (1u << 27))
        return 9;

    if (val < (1u << 30))
        return 10;

    return 11;
}

static inline size_t vnibble_bitsize(uint32_t val)
{
    return 4 * vnibble_size(val);
}

static inline size_t read_vnibble(struct vnibble_decode_ctx *ctx, uint32_t *res)
{
    uint8_t nibble;

    nibble = read_nibble(ctx);
    *res = nibble & 0x7u;
    if (nibble >= 0x8u)
        return 1;

    nibble = read_nibble(ctx);
    *res = ((nibble & 0x7u) << 3) | *res;
    if (nibble >= 0x8u)
        return 2;

    nibble = read_nibble(ctx);
    *res = ((nibble & 0x7u) << 6) | *res;
    if (nibble >= 0x8u)
        return 3;

    nibble = read_nibble(ctx);
    *res = ((nibble & 0x7u) << 9) | *res;
    if (nibble >= 0x8u)
        return 4;

    nibble = read_nibble(ctx);
    *res = ((nibble & 0x7u) << 12) | *res;
    if (nibble >= 0x8u)
        return 5;

    nibble = read_nibble(ctx);
    *res = ((nibble & 0x7u) << 15) | *res;
    if (nibble >= 0x8u)
        return 6;

    nibble = read_nibble(ctx);
    *res = ((nibble & 0x7u) << 18) | *res;
    if (nibble >= 0x8u)
        return 7;

    nibble = read_nibble(ctx);
    *res = ((nibble & 0x7u) << 21) | *res;
    if (nibble >= 0x8u)
        return 8;

    nibble = read_nibble(ctx);
    *res = ((nibble & 0x7u) << 24) | *res;
    if (nibble >= 0x8u)
        return 9;

    nibble = read_nibble(ctx);
    *res = ((nibble & 0x7u) << 27) | *res;
    if (nibble >= 0x8u)
        return 10;

    nibble = read_nibble(ctx);
    *res = ((nibble & 0x7u) << 30) | *res;
    return 11;
}

static inline size_t encode_vnibble(uint32_t val, uint64_t *res)
{
    uint8_t *p = (uint8_t *)res;

    if (val < (1u << 3)) {
        p[0] = val | 0x8u;
        return 1;
    }

    if (val < (1u << 6)) {
        p[0] = (val & 0x7u) | (((val >> 3) | 0x8u) << 4);
        return 2;
    }

    if (val < (1u << 9)) {
        p[0] = (val & 0x7u) | (((val >> 3) & 0x7u) << 4);
        p[1] = (val >> 6) | 0x8u;
        return 3;
    }

    if (val < (1u << 12)) {
        p[0] = (val & 0x7u) | (((val >> 3) & 0x7u) << 4);
        p[1] = ((val >> 6) & 0x7u) | (((val >> 9) | 0x8u) << 4);
        return 4;
    }

    if (val < (1u << 15)) {
        p[0] = (val & 0x7u) | (((val >> 3) & 0x7u) << 4);
        p[1] = ((val >> 6) & 0x7u) | (((val >> 9) & 0x7u) << 4);
        p[2] = (val >> 12) | 0x8u;
        return 5;
    }

    if (val < (1u << 18)) {
        p[0] = (val & 0x7u) | (((val >> 3) & 0x7u) << 4);
        p[1] = ((val >> 6) & 0x7u) | (((val >> 9) & 0x7u) << 4);
        p[2] = ((val >> 12) & 0x7u) | (((val >> 15) | 0x8u) << 4);
        return 6;
    }

    if (val < (1u << 21)) {
        p[0] = (val & 0x7u) | (((val >> 3) & 0x7u) << 4);
        p[1] = ((val >> 6) & 0x7u) | (((val >> 9) & 0x7u) << 4);
        p[2] = ((val >> 12) & 0x7u) | (((val >> 15) & 0x7u) << 4);
        p[3] = (val >> 18) | 0x8u;
        return 7;
    }

    if (val < (1u << 24)) {
        p[0] = (val & 0x7u) | (((val >> 3) & 0x7u) << 4);
        p[1] = ((val >> 6) & 0x7u) | (((val >> 9) & 0x7u) << 4);
        p[2] = ((val >> 12) & 0x7u) | (((val >> 15) & 0x7u) << 4);
        p[3] = ((val >> 18) & 0x7u) | (((val >> 21) | 0x8u) << 4);
        return 8;
    }

    if (val < (1u << 27)) {
        p[0] = (val & 0x7u) | (((val >> 3) & 0x7u) << 4);
        p[1] = ((val >> 6) & 0x7u) | (((val >> 9) & 0x7u) << 4);
        p[2] = ((val >> 12) & 0x7u) | (((val >> 15) & 0x7u) << 4);
        p[3] = ((val >> 18) & 0x7u) | (((val >> 21) & 0x7u) << 4);
        p[4] = (val >> 24) | 0x8u;
        return 9;
    }

    if (val < (1u << 30)) {
        p[0] = (val & 0x7u) | (((val >> 3) & 0x7u) << 4);
        p[1] = ((val >> 6) & 0x7u) | (((val >> 9) & 0x7u) << 4);
        p[2] = ((val >> 12) & 0x7u) | (((val >> 15) & 0x7u) << 4);
        p[3] = ((val >> 18) & 0x7u) | (((val >> 21) & 0x7u) << 4);
        p[4] = ((val >> 24) & 0x7u) | (((val >> 27) | 0x8u) << 4);
        return 10;
    }

    p[0] = (val & 0x7u) | (((val >> 3) & 0x7u) << 4);
    p[1] = ((val >> 6) & 0x7u) | (((val >> 9) & 0x7u) << 4);
    p[2] = ((val >> 12) & 0x7u) | (((val >> 15) & 0x7u) << 4);
    p[3] = ((val >> 18) & 0x7u) | (((val >> 21) & 0x7u) << 4);
    p[4] = ((val >> 24) & 0x7u) | (((val >> 27) & 0x7u) << 4);
    p[5] = (val >> 30) | 0x8u;
    return 11;
}

static inline void write_vnibble(struct vnibble_encode_ctx *ctx, uint32_t val)
{
    uint64_t vnibble = 0;
    size_t vnibble_len = encode_vnibble(val, &vnibble);

    while (vnibble_len > 0) {
        uint8_t nibble = vnibble & 0xfu;
        write_nibble(ctx, nibble);
        vnibble >>= 4;
        vnibble_len -= 1;
    }
}

static inline size_t vbyte_size(uint32_t val)
{
    if (val < (1u << 7))
        return 1;

    if (val < (1u << 14))
        return 2;

    if (val < (1u << 21))
        return 3;

    if (val < (1u << 28))
        return 4;

    return 5;
}

static inline size_t vbyte_bitsize(uint32_t val)
{
    return 8 * vbyte_size(val);
}

static inline size_t read_vbyte(uint8_t *src, size_t src_len, size_t pos,
        uint32_t *res)
{
#ifdef NDEBUG
    unused(src_len);
#endif

    uint8_t *p = &src[pos];

    assert(pos < src_len);
    *res = p[0] & 0x7fu;
    if (p[0] >= 0x80u)
        return 1;

    assert(pos + 1 < src_len);
    *res = ((p[1] & 0x7fu) << 7) | *res;
    if (p[1] >= 0x80u)
        return 2;

    assert(pos + 2 < src_len);
    *res = ((p[2] & 0x7fu) << 14) | *res;
    if (p[2] >= 0x80u)
        return 3;

    assert(pos + 3 < src_len);
    *res = ((p[3] & 0x7fu) << 21) | *res;
    if (p[3] >= 0x80u)
        return 4;

    assert(pos + 4 < src_len);
    *res = ((p[4] & 0x7fu) << 28) | *res;
    return 5;
}

static inline size_t encode_vbyte(uint32_t val, uint64_t *res)
{
    uint8_t *p = (uint8_t *)res;

    if (val < (1u << 7)) {
        p[0] = val | 0x80u;
        return 1;
    }

    if (val < (1u << 14)) {
        p[0] = val & 0x7fu;
        p[1] = (val >> 7) | 0x80u;
        return 2;
    }

    if (val < (1u << 21)) {
        p[0] = val & 0x7fu;
        p[1] = (val >> 7) & 0x7fu;
        p[2] = (val >> 14) | 0x80u;
        return 3;
    }

    if (val < (1u << 28)) {
        p[0] = val & 0x7fu;
        p[1] = (val >> 7) & 0x7fu;
        p[2] = (val >> 14) & 0x7fu;
        p[3] = (val >> 21) | 0x80u;
        return 4;
    }

    p[0] = val & 0x7fu;
    p[1] = (val >> 7) & 0x7fu;
    p[2] = (val >> 14) & 0x7fu;
    p[3] = (val >> 21) & 0x7fu;
    p[4] = (val >> 28) | 0x80u;
    return 5;
}

static inline size_t write_vbyte(uint8_t *dst, size_t dst_len, size_t pos,
        uint32_t val)
{
#ifdef NDEBUG
    unused(dst_len);
#endif

    uint64_t vbyte;
    size_t vbyte_len = encode_vbyte(val, &vbyte);

    assert(pos + vbyte_len - 1 < dst_len);
    memcpy(&dst[pos], &vbyte, vbyte_len);

    return vbyte_len;
}

static inline size_t write_lsic(uint8_t *dst, size_t dst_len, size_t pos,
        uint32_t val)
{
#ifdef NDEBUG
    unused(dst_len);
#endif

    size_t orig_pos = pos;

    while (val >= 255) {
        assert(pos < dst_len);
        dst[pos] = 255;
        pos += 1;
        val -= 255;
    }
    assert(pos < dst_len);
    dst[pos] = val;

    return pos - orig_pos + 1;
}

static inline size_t read_lsic(uint8_t *src, size_t src_len, size_t pos,
        uint32_t *res)
{
#ifdef NDEBUG
    unused(src_len);
#endif

    uint32_t val = 0;
    uint8_t read_more;
    size_t orig_pos = pos;

    do {
        assert(pos < src_len);
        read_more = src[pos];
        pos += 1;
        val += read_more;
    } while (read_more == 0xff);

    *res = val;

    return pos - orig_pos;
}

static inline size_t lcp_cmp(uint8_t *text, size_t text_len, size_t common_len,
        size_t pos1, size_t pos2)
{
    size_t len = common_len;

    while (pos2 + len <= text_len - 8) {
        uint64_t val1 = read_u64(text, pos1 + len);
        uint64_t val2 = read_u64(text, pos2 + len);
        uint64_t diff = val1 ^ val2;

        if (diff != 0)
            return len + (__builtin_ctzll(diff) >> 3);

        len += 8;
    }

    while (pos2 + len < text_len && text[pos1 + len] == text[pos2 + len])
        len += 1;

    return len;
}

struct lz_factor_ctx {
    int32_t psv;
    size_t psv_len;
    int32_t nsv;
    size_t nsv_len;
};

static inline void lz_factor(struct lz_factor_ctx *ctx, uint8_t *text,
        size_t text_len, size_t pos, int32_t psv, int32_t nsv)
{
    size_t psv_len = 0;
    size_t nsv_len = 0;

    if (psv != -1) {
        size_t common_len = ctx->psv_len != 0 ? ctx->psv_len - 1 : 0;
        psv_len = lcp_cmp(text, text_len, common_len, psv, pos);
    }

    if (nsv != -1) {
        size_t common_len = ctx->nsv_len != 0 ? ctx->nsv_len - 1 : 0;
        nsv_len = lcp_cmp(text, text_len, common_len, nsv, pos);
    }

    ctx->psv = psv;
    ctx->psv_len = psv_len;
    ctx->nsv = nsv;
    ctx->nsv_len = nsv_len;
}

uint32_t salz_encode_default(uint8_t *src, size_t src_len, uint8_t *dst,
        size_t dst_len)
{
#ifdef NDEBUG
    unused(dst_len);
#endif

    int32_t *sa;
    int32_t *aux;
    size_t sa_len = src_len + 2;
    size_t aux_len = 4 * (src_len + 1);
    uint32_t ret = 0;

    sa = malloc(sa_len * sizeof(*sa));
    aux = malloc(aux_len * sizeof(*aux));

    if (sa == NULL || aux == NULL) {
        debug("Memory allocation failed");
        goto clean;
    }

#ifdef ENABLE_STATS
    start_clock();
#endif

    if (divsufsort(src, sa + 1, src_len) != 0) {
        debug("divsufsort failed");
        goto clean;
    }

#ifdef ENABLE_STATS
    increment_clock(st.sa_time);
#endif

    sa[0] = -1;
    sa[src_len + 1] = -1;
    for (size_t top = 0, i = 1; i < sa_len; i++) {
        while (sa[top] > sa[i]) {
            aux[0 + 4 * sa[top]] = sa[top - 1];
            aux[1 + 4 * sa[top]] = sa[i];
            top -= 1;
        }
        top += 1;
        sa[top] = sa[i];
    }

#ifdef ENABLE_STATS
    increment_clock(st.psv_nsv_time);
#endif

    struct lz_factor_ctx lz_ctx = { -1, 0, -1, 0 };

#define nr_offs 8
    int32_t *offs;
    size_t offs_len = nr_offs * (src_len + 1);
    offs = malloc(offs_len * sizeof(*offs));

    memset(offs, 0, offs_len * sizeof(*offs));

    for (size_t i = 0; i < src_len + 1; i++)
        aux[2 + 4 * i] = INT_MAX;

    aux[2 + 4 * 0] = 0; /* First position cost */
    aux[2 + 4 * 1] = 9; /* Second position cost */
    aux[3 + 4 * 1] = 0; /* Second position parent */

    for (size_t src_pos = 1; src_pos < src_len; src_pos++) {
        int32_t psv = aux[0 + 4 * src_pos];
        int32_t nsv = aux[1 + 4 * src_pos];

        lz_factor(&lz_ctx, src, src_len, src_pos, psv, nsv);

        int32_t base_cost = aux[2 + 4 * src_pos];

        int32_t literal_cost = 9 + base_cost;
        if (literal_cost < aux[2 + 4 * (src_pos + 1)]) {
            aux[2 + 4 * (src_pos + 1)] = literal_cost;
            aux[3 + 4 * (src_pos + 1)] = src_pos;

            for (size_t i = 0; i < nr_offs; i++)
                offs[i + nr_offs * (src_pos + 1)] = offs[i + nr_offs * src_pos];
        }

        int32_t psv_len = (int32_t)lz_ctx.psv_len;
        if (psv_len >= 4) {
            int32_t psv_offs = (int32_t)(src_pos - lz_ctx.psv);
            int32_t psv_cost = base_cost + gr_bitsize(psv_len - 4, 3);

            size_t i;
            for (i = 0; i < nr_offs; i++)
                if (psv_offs == offs[i + nr_offs * src_pos])
                    break;

            if (i == nr_offs)
                psv_cost += 2 + 8 + vnibble_bitsize(psv_offs >> 8);
            else
                psv_cost += 3 + i;

            if (psv_cost < aux[2 + 4 * (src_pos + psv_len)]) {
                aux[2 + 4 * (src_pos + psv_len)] = psv_cost;
                aux[3 + 4 * (src_pos + psv_len)] = src_pos;

                for (size_t j = 0; j < nr_offs; j++)
                    offs[j + nr_offs * (src_pos + psv_len)] = offs[j + nr_offs * src_pos];

                for (i = min(i, nr_offs - 1); i > 0; i--)
                    offs[i + nr_offs * (src_pos + psv_len)] = offs[-1 + i + nr_offs * (src_pos + psv_len)];
                offs[0 + nr_offs * (src_pos + psv_len)] = psv_offs;
            }
        }

        int32_t nsv_len = (int32_t)lz_ctx.nsv_len;
        if (nsv_len >= 4) {
            int32_t nsv_offs = (int32_t)(src_pos - lz_ctx.nsv);
            int32_t nsv_cost = base_cost + gr_bitsize(nsv_len - 4, 3);

            size_t i;
            for (i = 0; i < nr_offs; i++)
                if (nsv_offs == offs[i + nr_offs * src_pos])
                    break;

            if (i == nr_offs)
                nsv_cost += 2 + 8 + vnibble_bitsize(nsv_offs >> 8);
            else
                nsv_cost += 3 + i;

            if (nsv_cost < aux[2 + 4 * (src_pos + nsv_len)]) {
                aux[2 + 4 * (src_pos + nsv_len)] = nsv_cost;
                aux[3 + 4 * (src_pos + nsv_len)] = src_pos;

                for (size_t j = 0; j < nr_offs; j++)
                    offs[j + nr_offs * (src_pos + nsv_len)] = offs[j + nr_offs * src_pos];

                for (i = min(i, nr_offs - 1); i > 0; i--)
                    offs[i + nr_offs * (src_pos + nsv_len)] = offs[-1 + i + nr_offs * (src_pos + nsv_len)];
                offs[0 + nr_offs * (src_pos + nsv_len)] = nsv_offs;
            }
        }
    }

#ifdef ENABLE_STATS
    increment_clock(st.factor_time);
#endif

    for (size_t src_pos = src_len; src_pos > 0;) {
        int32_t prev_pos = aux[3 + 4 * src_pos];
        int32_t factor_len = src_pos - prev_pos;

        if (factor_len == 1) {
            aux[0 + 4 * prev_pos] = 0;
            aux[1 + 4 * prev_pos] = 1;
        } else {
            aux[0 + 4 * prev_pos] = offs[0 + nr_offs * src_pos];
            aux[1 + 4 * prev_pos] = factor_len;
        }

        src_pos = prev_pos;
    }

    free(offs);

#ifdef ENABLE_STATS
    increment_clock(st.mincost_time);
#endif

    struct vnibble_encode_ctx ctx;
    vnibble_encode_ctx_init(&ctx, dst, dst_len);

    uint32_t prev_offs[nr_offs];

    size_t src_pos = 0;
    while (src_pos < src_len) {
        uint32_t factor_len = (uint32_t)aux[1 + 4 * src_pos];
        if (factor_len == 1) {
            write_gr(&ctx, 0, 0);

            copy(src, src_pos, ctx.buf, ctx.pos, 1);
            src_pos += 1;
            ctx.pos += 1;
        } else {
            uint32_t factor_offs = (uint32_t)aux[0 + 4 * src_pos];
            assert(factor_offs <= src_pos);

            size_t i;
            for (i = 0; i < nr_offs; i++)
                if (prev_offs[i] == factor_offs)
                    break;

            if (i == nr_offs) {
                write_gr(&ctx, 1, 0);
                write_vnibble(&ctx, factor_offs >> 8);
                ctx.buf[ctx.pos] = factor_offs & 0xffu;
                ctx.pos += 1;
            } else {
                write_gr(&ctx, 2 + i, 0);
            }

            for (i = min(i, nr_offs - 1); i > 0; i--)
                prev_offs[i] = prev_offs[i - 1];
            prev_offs[0] = factor_offs;

            write_gr(&ctx, factor_len - 4, 3);
            src_pos += factor_len;
        }
    }

    write_gr(&ctx, 0, 0);

    vnibble_encode_ctx_fini(&ctx);

#ifdef ENABLE_STATS
    increment_clock(st.encode_time);
#endif

    ret = (uint32_t)ctx.pos;

clean:
    free(sa);
    free(aux);

    return ret;
}

uint32_t salz_decode_default(uint8_t *src, size_t src_len, uint8_t *dst,
        size_t dst_len)
{
#ifdef NDEBUG
    unused(dst_len);
#endif

    struct vnibble_decode_ctx ctx;
    vnibble_decode_ctx_init(&ctx, src, src_len);
    size_t dst_pos = 0;
    uint32_t prev_offs[nr_offs];

    while (true) {
        uint32_t code;
        read_gr(&ctx, &code, 0);

        if (code == 0) {
            if (ctx.pos == ctx.len)
                break;

            assert(ctx.pos < ctx.len);
            assert(dst_pos < dst_len);
            copy(ctx.buf, ctx.pos, dst, dst_pos, 1);
            ctx.pos += 1;
            dst_pos += 1;
            continue;
        } else if (code == 1) {
            uint32_t factor_offs;

            read_vnibble(&ctx, &factor_offs);
            factor_offs = (factor_offs << 8) | ctx.buf[ctx.pos];
            ctx.pos += 1;

            for (size_t i = nr_offs - 1; i > 0; i--)
                prev_offs[i] = prev_offs[i - 1];
            prev_offs[0] = factor_offs;
        } else {
            uint32_t factor_offs = prev_offs[code - 2];

            for (size_t i = code - 2; i > 0; i--)
                prev_offs[i] = prev_offs[i - 1];
            prev_offs[0] = factor_offs;
        }

        uint32_t factor_offs = prev_offs[0];

        uint32_t factor_len;
        read_gr(&ctx, &factor_len, 3);
        factor_len += 4;

        assert(dst_pos + factor_len - 1 < dst_len);
        assert(factor_offs <= dst_pos);
        copy_oaat(dst, dst_pos - factor_offs, dst, dst_pos, factor_len);
        dst_pos += factor_len;
    }

    return (uint32_t)dst_pos;
}
