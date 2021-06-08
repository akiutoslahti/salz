/*
 * salz.c - SA based LZ compressor
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

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

struct decode_ctx {
    uint8_t *buf;
    size_t len;
    size_t pos;
    uint64_t nibbles;
    uint16_t bits;
    size_t nibbles_left;
    size_t bits_left;
};

static inline void decode_ctx_init(struct decode_ctx *ctx,
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

struct encode_ctx {
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

static inline void encode_ctx_init(struct encode_ctx *ctx,
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

static inline void encode_ctx_fini(struct encode_ctx *ctx)
{
    ctx->nibbles <<= (ctx->nibbles_left * 4);
    write_u64(ctx->buf, ctx->nibbles_pos, ctx->nibbles);
    ctx->bits <<= (ctx->bits_left);
    write_u16(ctx->buf, ctx->bits_pos, ctx->bits);
}

static inline uint8_t read_bit(struct decode_ctx *ctx)
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

static inline uint8_t read_nibble(struct decode_ctx *ctx)
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

static inline void write_bit(struct encode_ctx *ctx, uint8_t flag)
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

static inline void write_nibble(struct encode_ctx *ctx, uint8_t nibble)
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

static inline void write_gr(struct encode_ctx *ctx, uint32_t val, size_t k)
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

static inline uint32_t read_gr(struct decode_ctx *ctx, size_t k)
{
    uint32_t ret = 0;

    while (read_bit(ctx) == 0)
        ret += 1;

    for (int32_t i = k - 1; i >= 0; i--)
        ret = (ret << 1) | read_bit(ctx);

    return ret;
}

static inline size_t vnibble_size(uint32_t val)
{
    size_t vnibble_len = 1;

    while (val >>= 3) {
        val -= 1;
        vnibble_len += 1;
    }

    return vnibble_len;
}

static inline size_t vnibble_bitsize(uint32_t val)
{
    return 4 * vnibble_size(val);
}

static inline uint32_t read_vnibble(struct decode_ctx *ctx)
{
    uint8_t nibble = read_nibble(ctx);
    uint32_t ret = nibble & 0x7u;

    while (nibble < 0x8u) {
        nibble = read_nibble(ctx);
        ret += 1;
        ret = (ret << 3) | (nibble & 0x7u);
    }

    return ret;
}

static inline size_t encode_vnibble(uint32_t val, uint64_t *res)
{
    size_t vnibble_len = 1;
    *res = (val & 0x7u) | 0x8u;

    while (val >>= 3) {
        val -= 1;
        *res = (*res << 4) | (val & 0x7u);
        vnibble_len += 1;
    }

    return vnibble_len;
}

static inline void write_vnibble(struct encode_ctx *ctx, uint32_t val)
{
    uint64_t nibbles;
    size_t nibbles_len = encode_vnibble(val, &nibbles);

    while (nibbles_len > 0) {
        uint8_t nibble = nibbles & 0xfu;
        write_nibble(ctx, nibble);
        nibbles >>= 4;
        nibbles_len -= 1;
    }
}

static inline size_t vlq_size(uint32_t val, size_t k)
{
    size_t vlq_len = 1;

    while (val >>= k) {
        val -= 1;
        vlq_len += 1;
    }

    return vlq_len;
}

static inline size_t vlq_bitsize(uint32_t val, size_t k)
{
    return (k + 1) * vlq_size(val, k);
}

static inline uint32_t read_vlq(struct decode_ctx *ctx, size_t k)
{
    uint32_t part = 0;
    for (size_t i = 0; i < k + 1; i++)
        part = (part << 1) | read_bit(ctx);

    uint32_t ret = part & ((1u << k) - 1);

    while (part < (1u << k)) {
        part = 0;
        for (size_t i = 0; i < k + 1; i++)
            part = (part << 1) | read_bit(ctx);
        ret += 1;
        ret = (ret << k) | (part & ((1u << k) - 1));
    }

    return ret;
}

static inline size_t encode_vlq(uint32_t val, uint64_t *res, size_t k)
{
    size_t vlq_len = 1;
    *res = (val & ((1u << k) - 1)) | (1u << k);

    while (val >>= k) {
        val -= 1;
        *res = (*res << (k + 1)) | (val & ((1u << k) - 1));
        vlq_len += 1;
    }

    return vlq_len;
}

static inline void write_vlq(struct encode_ctx *ctx, uint32_t val, size_t k)
{
    uint64_t parts;
    size_t parts_len = encode_vlq(val, &parts, k);

    while (parts_len > 0) {
        uint32_t mask = 1u << (k + 1);
        while (mask >>= 1)
            write_bit(ctx, parts & mask ? 1 : 0);
        parts >>= (k + 1);
        parts_len -= 1;
    }
}

static inline size_t vbyte_size(uint32_t val)
{
    size_t vbyte_len = 1;

    while (val >>= 7) {
        val -= 1;
        vbyte_len += 1;
    }

    return vbyte_len;
}

static inline size_t vbyte_bitsize(uint32_t val)
{
    return 8 * vbyte_size(val);
}

static inline uint32_t read_vbyte(struct decode_ctx *ctx)
{
    uint8_t byte = ctx->buf[ctx->pos];
    ctx->pos += 1;
    uint32_t ret = byte & 0x7fu;

    while (byte < 0x80u) {
        byte = ctx->buf[ctx->pos];
        ctx->pos += 1;
        ret += 1;
        ret = (ret << 7) | (byte & 0x7fu);
    }

    return ret;
}

static inline size_t encode_vbyte(uint32_t val, uint64_t *res)
{
    size_t vbyte_len = 1;
    *res = (val & 0x7fu) | 0x80u;

    while (val >>= 7) {
        val -= 1;
        *res = (*res << 8) | (val & 0x7fu);
        vbyte_len += 1;
    }

    return vbyte_len;
}

static inline void write_vbyte(struct encode_ctx *ctx, uint32_t val)
{
    uint64_t bytes;
    size_t bytes_len = encode_vbyte(val, &bytes);

    while (bytes_len > 0) {
        ctx->buf[ctx->pos] = bytes & 0xffu;
        ctx->pos += 1;
        bytes >>= 8;
        bytes_len -= 1;
    }
}

static size_t factor_offs_bitsize(uint32_t val)
{
    val -= 1;

    size_t ret = 8 + vnibble_bitsize(val >> 8);

    return ret;
}

static inline void write_factor_offs(struct encode_ctx *ctx, uint32_t val)
{
    val -= 1;

    write_vnibble(ctx, val >> 8);
    ctx->buf[ctx->pos] = val & 0xffu;
    ctx->pos += 1;
}

static inline uint32_t read_factor_offs(struct decode_ctx *ctx)
{
    uint32_t ret = (read_vnibble(ctx) << 8) | ctx->buf[ctx->pos];
    ctx->pos += 1;
    ret += 1;

    return ret;
}

static size_t factor_len_bitsize(uint32_t val)
{
    val -= 3;

    size_t ret = gr_bitsize(val, 3);

    return ret;
}

static inline void write_factor_len(struct encode_ctx *ctx, uint32_t val)
{
    val -= 3;

    write_gr(ctx, val, 3);
}

static inline uint32_t read_factor_len(struct decode_ctx *ctx)
{
    uint32_t ret = read_gr(ctx, 3);
    ret += 3;

    return ret;
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

struct factor_ctx {
    int32_t psv;
    size_t psv_len;
    int32_t nsv;
    size_t nsv_len;
};

static inline void lz_factor(struct factor_ctx *ctx, uint8_t *text,
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
    size_t aux_len = 4 * src_len;
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

    struct factor_ctx fctx = { -1, 0, -1, 0 };

    aux[1] = 1;
    for (size_t src_pos = 1; src_pos < src_len; src_pos++) {
        int32_t psv = aux[0 + 4 * src_pos];
        int32_t nsv = aux[1 + 4 * src_pos];

        lz_factor(&fctx, src, src_len, src_pos, psv, nsv);

        aux[0 + 4 * src_pos] = (int32_t)(src_pos - fctx.psv);
        aux[1 + 4 * src_pos] = (int32_t)fctx.psv_len;
        aux[2 + 4 * src_pos] = (int32_t)(src_pos - fctx.nsv);
        aux[3 + 4 * src_pos] = (int32_t)fctx.nsv_len;
    }

#ifdef ENABLE_STATS
    increment_clock(st.factor_time);
#endif

    int32_t *mc = sa;
    mc[src_len] = 0;
    for (size_t src_pos = src_len - 1; src_pos > 0; src_pos--) {
        int32_t lcost = 9 + mc[src_pos + 1];

        int32_t offs1 = aux[0 + 4 * src_pos];
        int32_t len1 = aux[1 + 4 * src_pos];
        int32_t cost1;

        if (len1 < 3)
            cost1 = lcost + 1;
        else
            cost1 = 1 + factor_offs_bitsize(offs1) +
                    factor_len_bitsize(len1) +
                    mc[src_pos + len1];

        int32_t offs2 = aux[2 + 4 * src_pos];
        int32_t len2 = aux[3 + 4 * src_pos];
        int32_t cost2;

        if (len2 < 3)
            cost2 = lcost + 1;
        else
            cost2 = 1 + factor_offs_bitsize(offs2) +
                    factor_len_bitsize(len2) +
                    mc[src_pos + len2];

        int32_t mincost = min(lcost, min(cost1, cost2));

        if (lcost == mincost) {
            aux[1 + 4 * src_pos] = 1;
            mc[src_pos] = lcost;
        } else if (cost1 == mincost) {
            aux[0 + 4 * src_pos] = offs1;
            aux[1 + 4 * src_pos] = len1;
            mc[src_pos] = cost1;
        } else {
            aux[0 + 4 * src_pos] = offs2;
            aux[1 + 4 * src_pos] = len2;
            mc[src_pos] = cost2;
        }
    }

#ifdef ENABLE_STATS
    increment_clock(st.mincost_time);
#endif

    struct encode_ctx ctx;
    encode_ctx_init(&ctx, dst, dst_len);

    size_t src_pos = 0;
    while (src_pos < src_len) {
        uint32_t factor_len = (uint32_t)aux[1 + 4 * src_pos];
        if (factor_len == 1) {
            write_bit(&ctx, 0);

            copy(src, src_pos, ctx.buf, ctx.pos, 1);
            src_pos += 1;
            ctx.pos += 1;
        } else {
            write_bit(&ctx, 1);

            uint32_t factor_offs = (uint32_t)aux[0 + 4 * src_pos];
            assert(factor_offs <= src_pos);
            write_factor_offs(&ctx, factor_offs);

            write_factor_len(&ctx, factor_len);
            src_pos += factor_len;
        }
    }

    encode_ctx_fini(&ctx);

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

    struct decode_ctx ctx;
    decode_ctx_init(&ctx, src, src_len);
    size_t dst_pos = 0;

    while (ctx.pos < ctx.len) {
        uint8_t flag = read_bit(&ctx);

        if (flag) {
            uint32_t factor_offs = read_factor_offs(&ctx);
            uint32_t factor_len = read_factor_len(&ctx);

            assert(dst_pos + factor_len - 1 < dst_len);
            assert(factor_offs <= dst_pos);
            copy_oaat(dst, dst_pos - factor_offs, dst, dst_pos, factor_len);
            dst_pos += factor_len;
        } else {

            assert(ctx.pos < ctx.len);
            assert(dst_pos < dst_len);
            copy(ctx.buf, ctx.pos, dst, dst_pos, 1);
            ctx.pos += 1;
            dst_pos += 1;
        }

    }

    return (uint32_t)dst_pos;
}
