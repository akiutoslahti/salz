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
#include "libsais.h"

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

static uint8_t read_u8(uint8_t *src, size_t pos)
{
    return src[pos];
}

static uint64_t read_u64(uint8_t *src, size_t pos)
{
    uint64_t val;

    memcpy(&val, &src[pos], sizeof(val));

    return val;
}

static void write_u8(uint8_t *dst, size_t pos, uint8_t val)
{
    dst[pos] = val;
}

static void write_u64(uint8_t *dst, size_t pos, uint64_t val)
{
    memcpy(&dst[pos], &val, sizeof(val));
}

static void copy_u8(uint8_t *src, size_t src_pos, uint8_t *dst,
        size_t dst_pos)
{
    dst[dst_pos] = src[src_pos];
}

static void copy_len_oaat(uint8_t *src, size_t src_pos, uint8_t *dst,
        size_t dst_pos, size_t copy_len)
{
    while (copy_len--)
        dst[dst_pos++] = src[src_pos++];
}

struct decode_in_ctx {
    uint8_t *src;
    size_t src_len;
    size_t src_pos;
    uint64_t bits;
    size_t bits_left;
};

static void decode_in_ctx_init(struct decode_in_ctx *ctx,
        uint8_t *src, size_t src_len)
{
    assert(sizeof(ctx->bits) < src_len + 1);
    ctx->src = src;
    ctx->src_len = src_len;
    ctx->src_pos = sizeof(ctx->bits);
    ctx->bits = read_u64(ctx->src, 0);
    ctx->bits_left = sizeof(ctx->bits) * 8;
}

struct encode_out_ctx {
    uint8_t *dst;
    size_t dst_len;
    size_t dst_pos;
    size_t bits_pos;
    uint64_t bits;
    size_t bits_avail;
};

static void encode_out_ctx_init(struct encode_out_ctx *ctx,
        uint8_t *dst, size_t dst_len)
{
    assert(sizeof(ctx->bits) < dst_len + 1);
    ctx->dst = dst;
    ctx->dst_len = dst_len;
    ctx->dst_pos = sizeof(ctx->bits);
    ctx->bits_pos = 0;
    ctx->bits = 0;
    ctx->bits_avail = sizeof(ctx->bits) * 8;
}

static void encode_out_ctx_fini(struct encode_out_ctx *ctx)
{
    ctx->bits <<= (ctx->bits_avail);
    write_u64(ctx->dst, ctx->bits_pos, ctx->bits);
}

static void queue_bits(struct decode_in_ctx *ctx)
{
    assert(ctx->src_pos + sizeof(ctx->bits) < ctx->src_len + 1);
    ctx->bits = read_u64(ctx->src, ctx->src_pos);
    ctx->src_pos += sizeof(ctx->bits);
    ctx->bits_left = sizeof(ctx->bits) * 8;
}

static uint8_t read_bit(struct decode_in_ctx *ctx)
{
    if (!ctx->bits_left)
        queue_bits(ctx);

    uint8_t ret = (ctx->bits & 0x8000000000000000u) ? 1 : 0;
    ctx->bits <<= 1;
    ctx->bits_left -= 1;

    return ret;
}

static uint64_t read_bits(struct decode_in_ctx *ctx, size_t count)
{
    uint64_t ret = 0;

    if (!ctx->bits_left)
        queue_bits(ctx);

    if (count > ctx->bits_left) {
        ret = ctx->bits >> (64 - ctx->bits_left);
        count -= ctx->bits_left;

        queue_bits(ctx);
    }

    ret = (ret << count) | (ctx->bits >> (64 - count));
    ctx->bits <<= count;
    ctx->bits_left -= count;

    return ret;
}

static uint32_t read_unary(struct decode_in_ctx *ctx)
{
    if (!ctx->bits_left)
        queue_bits(ctx);

    uint32_t ret = 0;

    while (!ctx->bits) {
        ret += ctx->bits_left;

        queue_bits(ctx);
    }

    uint32_t last_zeros = __builtin_clzll(ctx->bits);
    ctx->bits <<= last_zeros + 1;
    ctx->bits_left -= last_zeros + 1;

    return ret + last_zeros;
}

static void flush_bits(struct encode_out_ctx *ctx)
{
    assert(ctx->dst_pos + sizeof(ctx->bits) < ctx->dst_len + 1);
    write_u64(ctx->dst, ctx->bits_pos, ctx->bits);
    ctx->bits = 0;
    ctx->bits_avail = sizeof(ctx->bits) * 8;
    ctx->bits_pos = ctx->dst_pos;
    ctx->dst_pos += sizeof(ctx->bits);
}

static void write_bit(struct encode_out_ctx *ctx, uint8_t flag)
{
    if (!ctx->bits_avail)
        flush_bits(ctx);

    ctx->bits = (ctx->bits << 1) | flag;
    ctx->bits_avail -= 1;
}

static void write_bits(struct encode_out_ctx *ctx, uint64_t bits, size_t count)
{
    if (!ctx->bits_avail)
        flush_bits(ctx);

    if (count > ctx->bits_avail) {
        ctx->bits = (ctx->bits << ctx->bits_avail) |
                    ((bits >> (count - ctx->bits_avail)) &
                     ((1u << ctx->bits_avail) - 1));
        count -= ctx->bits_avail;

        flush_bits(ctx);
    }

    ctx->bits = (ctx->bits << count) | (bits & ((1u << count) - 1));
    ctx->bits_avail -= count;
}

static void write_zeros(struct encode_out_ctx *ctx, size_t count)
{
    while (count) {
        if (!ctx->bits_avail)
            flush_bits(ctx);

        size_t write_count = min(ctx->bits_avail, count);
        ctx->bits <<= write_count;
        ctx->bits_avail -= write_count;
        count -= write_count;
    }
}

static void write_unary(struct encode_out_ctx *ctx, uint32_t val)
{
    write_zeros(ctx, val);
    write_bit(ctx, 1);
}

static size_t gr3_bitsize(uint32_t val)
{
    return (val >> 3) + 1 + 3;
}

static void write_gr3(struct encode_out_ctx *ctx, uint32_t val)
{
    write_unary(ctx, val >> 3);
    write_bits(ctx, val & ((1u << 3) - 1), 3);
}

static uint32_t read_gr3(struct decode_in_ctx *ctx)
{
    return (read_unary(ctx) << 3) | read_bits(ctx, 3);
}

static size_t vnibble_size(uint32_t val)
{
    if (val < 8)
        return 1;

    if (val < 72)
        return 2;

    if (val < 584)
        return 3;

    if (val < 4680)
        return 4;

    if (val < 37448)
        return 5;

    if (val < 299592)
        return 6;

    if (val < 2396744)
        return 7;

    if (val < 19173960)
        return 8;

    if (val < 153391688)
        return 9;

    if (val < 1227133512)
        return 10;

    return 11;
}

static size_t vnibble_bitsize(uint32_t val)
{
    return 4 * vnibble_size(val);
}

static uint32_t read_vnibble(struct decode_in_ctx *ctx)
{
    uint8_t nibble = read_bits(ctx, 4);
    uint32_t ret = nibble & 0x7u;

    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(ctx, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(ctx, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(ctx, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(ctx, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(ctx, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(ctx, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(ctx, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(ctx, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(ctx, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(ctx, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    return ret;
}

static size_t encode_vnibble(uint32_t val, uint64_t *res)
{
    uint8_t *p = (uint8_t *)res;

    uint32_t v0 = val;

    if (val < 8) {
        p[0] = v0 | 0x8u;
        return 1;
    }

    if (val < 72) {
        p[0] = (((v0 >> 3) - 1) << 4) | ((v0 & 0x7u) | 0x8u);
        return 2;
    }

    uint32_t v1 = val - 72;

    if (val < 584) {
        p[0] = ((((v0 >> 3) - 1) & 0x7u) << 4) | ((v0 & 0x7u) | 0x8u);
        p[1] = v1 >> 6;
        return 3;
    }

    if (val < 4680) {
        p[0] = ((((v0 >> 3) - 1) & 0x7u) << 4) | ((v0 & 0x7u) | 0x8u);
        p[1] = (((v1 >> 9) - 1) << 4) | ((v1 >> 6) & 0x7u);
        return 4;
    }

    uint32_t v2 = val - 4680;

    if (val < 37448) {
        p[0] = ((((v0 >> 3) - 1) & 0x7u) << 4) | ((v0 & 0x7u) | 0x8u);
        p[1] = ((((v1 >> 9) - 1) & 0x7u) << 4) | ((v1 >> 6) & 0x7u);
        p[2] = v2 >> 12;
        return 5;
    }

    if (val < 299592) {
        p[0] = ((((v0 >> 3) - 1) & 0x7u) << 4) | ((v0 & 0x7u) | 0x8u);
        p[1] = ((((v1 >> 9) - 1) & 0x7u) << 4) | ((v1 >> 6) & 0x7u);
        p[2] = (((v2 >> 15) - 1) << 4) | ((v2 >> 12) & 0x7u);
        return 6;
    }

    uint32_t v3 = val - 299592;

    if (val < 2396744) {
        p[0] = ((((v0 >> 3) - 1) & 0x7u) << 4) | ((v0 & 0x7u) | 0x8u);
        p[1] = ((((v1 >> 9) - 1) & 0x7u) << 4) | ((v1 >> 6) & 0x7u);
        p[2] = ((((v2 >> 15) - 1) & 0x7u) << 4) | ((v2 >> 12) & 0x7u);
        p[3] = v3 >> 18;
        return 7;
    }

    if (val < 19173960) {
        p[0] = ((((v0 >> 3) - 1) & 0x7u) << 4) | ((v0 & 0x7u) | 0x8u);
        p[1] = ((((v1 >> 9) - 1) & 0x7u) << 4) | ((v1 >> 6) & 0x7u);
        p[2] = ((((v2 >> 15) - 1) & 0x7u) << 4) | ((v2 >> 12) & 0x7u);
        p[3] = (((v3 >> 21) - 1) << 4) | ((v3 >> 18) & 0x7u);
        return 8;
    }

    uint32_t v4 = val - 19173960;

    if (val < 153391688) {
        p[0] = ((((v0 >> 3) - 1) & 0x7u) << 4) | ((v0 & 0x7u) | 0x8u);
        p[1] = ((((v1 >> 9) - 1) & 0x7u) << 4) | ((v1 >> 6) & 0x7u);
        p[2] = ((((v2 >> 15) - 1) & 0x7u) << 4) | ((v2 >> 12) & 0x7u);
        p[3] = ((((v3 >> 21) - 1) & 0x7u) << 4) | ((v3 >> 18) & 0x7u);
        p[4] = v4 >> 24;
        return 9;
    }

    if (val < 1227133512) {
        p[0] = ((((v0 >> 3) - 1) & 0x7u) << 4) | ((v0 & 0x7u) | 0x8u);
        p[1] = ((((v1 >> 9) - 1) & 0x7u) << 4) | ((v1 >> 6) & 0x7u);
        p[2] = ((((v2 >> 15) - 1) & 0x7u) << 4) | ((v2 >> 12) & 0x7u);
        p[3] = ((((v3 >> 21) - 1) & 0x7u) << 4) | ((v3 >> 18) & 0x7u);
        p[4] = (((v4 >> 27) - 1) << 4) | ((v4 >> 24) & 0x7u);
        return 10;
    }

    uint32_t v5 = val - 1227133512;

    p[0] = ((((v0 >> 3) - 1) & 0x7u) << 4) | ((v0 & 0x7u) | 0x8u);
    p[1] = ((((v1 >> 9) - 1) & 0x7u) << 4) | ((v1 >> 6) & 0x7u);
    p[2] = ((((v2 >> 15) - 1) & 0x7u) << 4) | ((v2 >> 12) & 0x7u);
    p[3] = ((((v3 >> 21) - 1) & 0x7u) << 4) | ((v3 >> 18) & 0x7u);
    p[4] = ((((v4 >> 27) - 1) & 0x7u) << 4) | ((v4 >> 24) & 0x7u);
    p[5] = v5 >> 30;
    return 11;
}

static void write_vnibble(struct encode_out_ctx *ctx, uint32_t val)
{
    uint64_t nibbles;

    size_t nibbles_len = encode_vnibble(val, &nibbles);

    write_bits(ctx, nibbles, nibbles_len * 4);
}

static size_t factor_offs_bitsize(uint32_t val)
{
    return 8 + vnibble_bitsize((val - 1) >> 8);
}

static void write_factor_offs(struct encode_out_ctx *ctx, uint32_t val)
{
    assert(ctx->dst_pos < ctx->dst_len);
    write_vnibble(ctx, (val - 1) >> 8);
    write_u8(ctx->dst, ctx->dst_pos++, (val - 1) & 0xffu);
}

static uint32_t read_factor_offs(struct decode_in_ctx *ctx)
{
    assert(ctx->src_pos < ctx->src_len);
    return ((read_vnibble(ctx) << 8) | read_u8(ctx->src, ctx->src_pos++)) + 1;
}

static size_t factor_len_bitsize(uint32_t val)
{
    return gr3_bitsize(val - 3);
}

static void write_factor_len(struct encode_out_ctx *ctx, uint32_t val)
{
    write_gr3(ctx, val - 3);
}

static uint32_t read_factor_len(struct decode_in_ctx *ctx)
{
    return read_gr3(ctx) + 3;
}

static size_t lcp_cmp(uint8_t *text, size_t text_len, size_t common_len,
        size_t pos1, size_t pos2)
{
    size_t len = common_len;

    while (pos2 + len < text_len - 8 + 1) {
        uint64_t val1 = read_u64(text, pos1 + len);
        uint64_t val2 = read_u64(text, pos2 + len);
        uint64_t diff = val1 ^ val2;

        if (diff)
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

static void init_factor_ctx(struct factor_ctx *ctx)
{
    ctx->psv = -1;
    ctx->psv_len = 0;
    ctx->nsv = -1;
    ctx->nsv_len = 0;
}

static void lz_factor(struct factor_ctx *ctx, uint8_t *text,
        size_t text_len, size_t pos, int32_t psv, int32_t nsv)
{
    size_t psv_len = 0;
    size_t nsv_len = 0;

    if (psv != -1) {
        size_t common_len = ctx->psv_len ? ctx->psv_len - 1 : 0;
        psv_len = lcp_cmp(text, text_len, common_len, psv, pos);
    }

    if (nsv != -1) {
        size_t common_len = ctx->nsv_len ? ctx->nsv_len - 1 : 0;
        nsv_len = lcp_cmp(text, text_len, common_len, nsv, pos);
    }

    ctx->psv = psv;
    ctx->psv_len = psv_len;
    ctx->nsv = nsv;
    ctx->nsv_len = nsv_len;
}

uint32_t salz_encode_default(struct encode_ctx *ctx, uint8_t *src, size_t src_len,
        uint8_t *dst, size_t dst_len)
{
#ifdef NDEBUG
    unused(dst_len);
#endif

    if (ctx->sa == NULL || ctx->aux == NULL ||
        ctx->sa_len < src_len + 2 ||
        ctx->aux_len < 5 * (src_len + 1)) {
        debug("Allocated resources are not enough");
        return 0;
    }

    int32_t *sa = ctx->sa;
    int32_t *aux = ctx->aux;

#ifdef ENABLE_STATS
    start_clock();
#endif

    if (libsais(src, sa + 1, src_len, 0)) {
        debug("libsais failed");
        return 0;
    }

#ifdef ENABLE_STATS
    increment_clock(st.sa_time);
#endif

    sa[0] = -1;
    sa[src_len + 1] = -1;
    for (size_t top = 0, i = 1; i < src_len + 2; i++) {
        while (sa[top] > sa[i]) {
            aux[0 + 5 * sa[top]] = sa[top - 1];
            aux[1 + 5 * sa[top]] = sa[i];
            top -= 1;
        }
        top += 1;
        sa[top] = sa[i];
    }

#ifdef ENABLE_STATS
    increment_clock(st.psv_nsv_time);
#endif

    struct factor_ctx fctx;
    init_factor_ctx(&fctx);

    aux[1 + 5 * 0] = 0;
    aux[3 + 5 * 0] = 0;
    for (size_t src_pos = 1; src_pos < src_len; src_pos++) {
        int32_t psv = aux[0 + 5 * src_pos];
        int32_t nsv = aux[1 + 5 * src_pos];

        lz_factor(&fctx, src, src_len, src_pos, psv, nsv);

        aux[0 + 5 * src_pos] = (int32_t)(src_pos - fctx.psv);
        aux[1 + 5 * src_pos] = (int32_t)fctx.psv_len;
        aux[2 + 5 * src_pos] = (int32_t)(src_pos - fctx.nsv);
        aux[3 + 5 * src_pos] = (int32_t)fctx.nsv_len;
    }

#ifdef ENABLE_STATS
    increment_clock(st.factor_time);
#endif

    aux[4 + 5 * src_len] = 0;
    for (size_t src_pos = src_len - 1; src_pos; src_pos--) {
        int32_t lcost = 9 + aux[4 + 5 * (src_pos + 1)];

        int32_t offs1 = aux[0 + 5 * src_pos];
        int32_t len1 = aux[1 + 5 * src_pos];
        int32_t cost1 = lcost + 1;

        if (len1 >= 3)
            cost1 = 1 + factor_offs_bitsize(offs1) +
                    factor_len_bitsize(len1) +
                    aux[4 + 5 * (src_pos + len1)];

        int32_t offs2 = aux[2 + 5 * src_pos];
        int32_t len2 = aux[3 + 5 * src_pos];
        int32_t cost2 = lcost + 1;

        if (len2 >= 3)
            cost2 = 1 + factor_offs_bitsize(offs2) +
                    factor_len_bitsize(len2) +
                    aux[4 + 5 * (src_pos + len2)];

        int32_t mincost = min(lcost, min(cost1, cost2));

        if (lcost == mincost) {
            aux[1 + 5 * src_pos] = 0;
            aux[4 + 5 * src_pos] = lcost;
        } else if (cost1 == mincost) {
            aux[0 + 5 * src_pos] = offs1;
            aux[1 + 5 * src_pos] = len1;
            aux[4 + 5 * src_pos] = cost1;
        } else {
            aux[0 + 5 * src_pos] = offs2;
            aux[1 + 5 * src_pos] = len2;
            aux[4 + 5 * src_pos] = cost2;
        }
    }

#ifdef ENABLE_STATS
    increment_clock(st.mincost_time);
#endif

    struct encode_out_ctx out_ctx;
    encode_out_ctx_init(&out_ctx, dst, dst_len);

    size_t src_pos = 0;
    while (src_pos < src_len) {
        uint32_t factor_len = (uint32_t)aux[1 + 5 * src_pos];

        if (!factor_len) {
            write_bit(&out_ctx, 0);
            assert(src_pos < src_len);
            assert(out_ctx.dst_pos < out_ctx.dst_len);
            copy_u8(src, src_pos++, out_ctx.dst, out_ctx.dst_pos++);
        } else {
            write_bit(&out_ctx, 1);
            uint32_t factor_offs = (uint32_t)aux[0 + 5 * src_pos];

            assert(factor_offs <= src_pos);

            write_factor_offs(&out_ctx, factor_offs);
            write_factor_len(&out_ctx, factor_len);
            src_pos += factor_len;
        }
    }

    encode_out_ctx_fini(&out_ctx);

#ifdef ENABLE_STATS
    increment_clock(st.encode_time);
#endif

    return (uint32_t)out_ctx.dst_pos;
}

uint32_t salz_decode_default(uint8_t *src, size_t src_len, uint8_t *dst,
        size_t dst_len)
{
#ifdef NDEBUG
    unused(dst_len);
#endif

    struct decode_in_ctx in_ctx;
    decode_in_ctx_init(&in_ctx, src, src_len);

    size_t dst_pos = 0;

    while (in_ctx.src_pos < in_ctx.src_len) {
        if (!read_bit(&in_ctx)) {
            assert(in_ctx.src_pos < in_ctx.src_len);
            assert(dst_pos < dst_len);
            copy_u8(in_ctx.src, in_ctx.src_pos++, dst, dst_pos++);
        } else {
            uint32_t factor_offs = read_factor_offs(&in_ctx);
            uint32_t factor_len = read_factor_len(&in_ctx);

            assert(dst_pos + factor_len < dst_len + 1);
            assert(factor_offs <= dst_pos);
            copy_len_oaat(dst, dst_pos - factor_offs, dst, dst_pos, factor_len);
            dst_pos += factor_len;
        }
    }

    return (uint32_t)dst_pos;
}

void encode_ctx_init(struct encode_ctx **ctx_out, size_t src_len)
{
    size_t sa_len = src_len + 2;
    size_t aux_len = 5 * (src_len + 1);
    int32_t *sa = malloc(sa_len * sizeof(*sa));
    int32_t *aux = malloc(aux_len * sizeof(*aux));

    if (!sa || !aux) {
        debug("Resource allocation failed");
        *ctx_out = NULL;
        return;
    }

    struct encode_ctx *ctx = malloc(sizeof(*ctx));
    ctx->sa = sa;
    ctx->sa_len = sa_len;
    ctx->aux = aux;
    ctx->aux_len = aux_len;
    *ctx_out = ctx;
}

void encode_ctx_fini(struct encode_ctx **ctx)
{
    free((*ctx)->sa);
    free((*ctx)->aux);
    free(*ctx);
    *ctx = NULL;
}
