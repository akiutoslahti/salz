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
#include <stdlib.h>
#include <string.h>

#include "salz.h"
#include "vlc.h"
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
#define roundup(a, b) (divup(a, b) * b)

#define unused(var) ((void)var)

#define array_len(arr) (sizeof(arr) / sizeof(arr[0]))

#define field_sizeof(t, f) (sizeof(((t*)0)->f))

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

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

static uint8_t read_u8(struct io_stream *stream)
{
    assert(stream->buf_pos < stream->buf_len);
    return stream->buf[stream->buf_pos++];
}

static void write_u8(struct io_stream *stream, uint8_t val)
{
    assert(stream->buf_pos < stream->buf_len);
    stream->buf[stream->buf_pos++] = val;
}

static uint64_t read_u64(uint8_t *src, size_t pos)
{
    uint64_t val;

    memcpy(&val, &src[pos], sizeof(val));

    return val;
}

static void write_u64(uint8_t *dst, size_t pos, uint64_t val)
{
    memcpy(&dst[pos], &val, sizeof(val));
}

static void cpy_u8_froms(struct io_stream *stream, uint8_t *dst,
        size_t dst_pos)
{
    assert(stream->buf_pos < stream->buf_len);
    dst[dst_pos] = stream->buf[stream->buf_pos++];
}

static void cpy_u8_tos(struct io_stream *stream, uint8_t *src,
        size_t src_pos)
{
    assert(stream->buf_pos < stream->buf_len);
    stream->buf[stream->buf_pos++] = src[src_pos];
}

static const int inc1[8] = { 0, 1, 2, 1, 4, 4, 4, 4 };
static const int inc2[8] = { 0, 1, 2, 2, 4, 3, 2, 1 };

static void cpy_factor(uint8_t *buf, size_t pos, size_t offs, size_t len)
{
    uint8_t *src = &buf[pos - offs];
    uint8_t *dst = &buf[pos];
    uint8_t *end = dst + len;

    if (offs < 8) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        memcpy(&dst[4], &src[inc1[offs]], 4);
        src += inc2[offs];
        dst += 8;
    }

    while (dst < end) {
        memcpy(dst, src, 8);
        dst += 8;
        src += 8;
    }
}

static size_t read_vbyte(uint8_t *buf, size_t buf_len, size_t pos,
        uint32_t *res)
{
#ifdef NDEBUG
    unused(buf_len);
#endif

    assert(pos < buf_len);
    uint8_t byte = buf[pos++];
    *res = byte & 0x7fu;

    if (byte >= 0x80u)
        return 1;

    assert(pos < buf_len);
    byte = buf[pos++];
    *res = ((*res + 1) << 7) | (byte & 0x7fu);
    if (byte >= 0x80u)
        return 2;

    assert(pos < buf_len);
    byte = buf[pos++];
    *res = ((*res + 1) << 7) | (byte & 0x7fu);
    if (byte >= 0x80u)
        return 3;

    assert(pos < buf_len);
    byte = buf[pos++];
    *res = ((*res + 1) << 7) | (byte & 0x7fu);
    if (byte >= 0x80u)
        return 4;

    assert(pos < buf_len);
    byte = buf[pos++];
    *res = ((*res + 1) << 7) | (byte & 0x7fu);
    return 5;
}

static size_t write_vbyte(uint8_t *buf, size_t buf_len, size_t pos,
        uint32_t val)
{
#ifdef NDEBUG
    unused(buf_len);
#endif

    uint64_t vbyte;
    size_t vbyte_len = encode_vbyte_be(val, &vbyte);

    assert(pos + vbyte_len < buf_len + 1);
    memcpy(&buf[pos], &vbyte, vbyte_len);

    return vbyte_len;
}

static void queue_bits(struct io_stream *stream)
{
    assert(stream->buf_pos + field_sizeof(struct io_stream, bits) <
           stream->buf_len + 1);
    stream->bits = read_u64(stream->buf, stream->buf_pos);
    stream->buf_pos += field_sizeof(struct io_stream, bits);
    stream->bits_avail = field_sizeof(struct io_stream, bits) * 8;
}

static void flush_bits(struct io_stream *stream)
{
    assert(stream->buf_pos + field_sizeof(struct io_stream, bits) <
           stream->buf_len + 1);
    write_u64(stream->buf, stream->bits_pos, stream->bits);
    stream->bits = 0;
    stream->bits_avail = field_sizeof(struct io_stream, bits) * 8;
    stream->bits_pos = stream->buf_pos;
    stream->buf_pos += field_sizeof(struct io_stream, bits);
}

static uint8_t read_bit(struct io_stream *stream)
{
    if (!stream->bits_avail)
        queue_bits(stream);

    uint8_t ret = !!(stream->bits & 0x8000000000000000u);
    stream->bits <<= 1;
    stream->bits_avail -= 1;

    return ret;
}

static void write_bit(struct io_stream *stream, uint8_t bit)
{
    if (!stream->bits_avail)
        flush_bits(stream);

    stream->bits = (stream->bits << 1) | bit;
    stream->bits_avail -= 1;
}

static uint64_t read_bits(struct io_stream *stream, size_t count)
{
    uint64_t ret = 0;

    if (!stream->bits_avail)
        queue_bits(stream);

    if (count > stream->bits_avail) {
        ret = stream->bits >> (64 - stream->bits_avail);
        count -= stream->bits_avail;

        queue_bits(stream);
    }

    ret = (ret << count) | (stream->bits >> (64 - count));
    stream->bits <<= count;
    stream->bits_avail -= count;

    return ret;
}

static void write_bits(struct io_stream *stream, uint64_t bits, size_t count)
{
    if (!stream->bits_avail)
        flush_bits(stream);

    if (count > stream->bits_avail) {
        stream->bits = (stream->bits << stream->bits_avail) |
                       ((bits >> (count - stream->bits_avail)) &
                        ((1u << stream->bits_avail) - 1));
        count -= stream->bits_avail;

        flush_bits(stream);
    }

    stream->bits = (stream->bits << count) | (bits & ((1u << count) - 1));
    stream->bits_avail -= count;
}

static void write_zeros(struct io_stream *stream, size_t count)
{
    while (count) {
        if (!stream->bits_avail)
            flush_bits(stream);

        size_t write_count = min(stream->bits_avail, count);
        stream->bits <<= write_count;
        stream->bits_avail -= write_count;
        count -= write_count;
    }
}

static uint32_t read_unary(struct io_stream *stream)
{
    if (!stream->bits_avail)
        queue_bits(stream);

    uint32_t ret = 0;

    while (!stream->bits) {
        ret += stream->bits_avail;

        queue_bits(stream);
    }

    uint32_t last_zeros = __builtin_clzll(stream->bits);
    stream->bits <<= last_zeros + 1;
    stream->bits_avail -= last_zeros + 1;

    return ret + last_zeros;
}

static void write_unary(struct io_stream *stream, uint32_t val)
{
    write_zeros(stream, val);
    write_bit(stream, 1);
}

static size_t gr_bitsize(uint32_t val, size_t k)
{
    if (likely(k))
        return (val >> k) + 1 + k;
    return val + 1;
}

static uint32_t read_gr(struct io_stream *stream, size_t k)
{
    if (likely(k))
        return (read_unary(stream) << k) | read_bits(stream, k);
    return read_unary(stream);
}

static void write_gr(struct io_stream *stream, uint32_t val, size_t k)
{
    if (likely(k)) {
        write_unary(stream, val >> k);
        write_bits(stream, val, k);
        return;
    }
    write_unary(stream, val);
}

static uint8_t read_nibble(struct io_stream *stream)
{
    uint8_t ret = 0;

    if (!stream->bits_avail)
        queue_bits(stream);

    if (stream->bits_avail < 4)
        return (uint8_t)read_bits(stream, 4);

    ret = stream->bits >> 60;
    stream->bits <<= 4;
    stream->bits_avail -= 4;

    return ret;
}

static uint32_t read_vnibble(struct io_stream *stream)
{
    uint8_t nibble = read_nibble(stream);
    uint32_t ret = nibble & 0x7u;

    if (nibble >= 0x8u)
        return ret;

    nibble = read_nibble(stream);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_nibble(stream);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_nibble(stream);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_nibble(stream);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_nibble(stream);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_nibble(stream);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_nibble(stream);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_nibble(stream);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_nibble(stream);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_nibble(stream);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    return ret;
}

static void write_vnibble(struct io_stream *stream, uint32_t val)
{
    uint64_t nibbles;

    size_t nibbles_len = encode_vnibble_le(val, &nibbles);

    write_bits(stream, nibbles, nibbles_len * 4);
}

static bool stream_empty(struct io_stream *stream)
{
    return stream->buf_pos == stream->buf_len;
}

static size_t stream_len(struct io_stream *stream)
{
    return stream->buf_pos;
}

static size_t in_stream_init(struct io_stream *stream, uint8_t *src,
        size_t src_len, size_t src_pos)
{
    size_t orig_pos = src_pos;

    uint32_t stream_size;
    src_pos += read_vbyte(src, src_len, src_pos, &stream_size);

    assert(field_sizeof(struct io_stream, bits) < stream_size + 1);
    stream->buf = &src[src_pos];
    stream->buf_len = stream_size;
    stream->buf_pos = field_sizeof(struct io_stream, bits);
    stream->bits = read_u64(stream->buf, 0);
    stream->bits_avail = field_sizeof(struct io_stream, bits) * 8;

    return (src_pos + stream_size) - orig_pos;
}

static bool out_stream_alloc(struct io_stream *stream, size_t size)
{
    stream->buf = malloc(size);

    if (!stream->buf)
        return false;

    stream->buf_len = size;

    return true;
}

static void out_stream_free(struct io_stream *stream)
{
    free(stream->buf);
}

static void out_stream_init(struct io_stream *stream)
{
    assert(field_sizeof(struct io_stream, bits) < stream->buf_len + 1);
    stream->buf_pos = field_sizeof(struct io_stream, bits);
    stream->bits = 0;
    stream->bits_pos = 0;
    stream->bits_avail = field_sizeof(struct io_stream, bits) * 8;
}

static size_t out_stream_fini(struct io_stream *stream, uint8_t *dst,
        size_t dst_len, size_t dst_pos)
{
    size_t orig_pos = dst_pos;

    stream->bits <<= (stream->bits_avail);
    write_u64(stream->buf, stream->bits_pos, stream->bits);

    if (dst) {
        dst_pos += write_vbyte(dst, dst_len, dst_pos, stream->buf_pos);
        assert(dst_pos + stream->buf_pos < dst_len + 1);
        memcpy(&dst[dst_pos], stream->buf, stream->buf_pos);
        dst_pos += stream->buf_pos;
    }

    return dst_pos - orig_pos;
}

void encode_ctx_init(struct encode_ctx **ctx_out, size_t src_len)
{
    *ctx_out = NULL;

    struct encode_ctx *ctx = malloc(sizeof(*ctx));

    if (!ctx)
        return;

    ctx->aux1_len = src_len + 2;
    ctx->aux2_len = 2 * (src_len + 1);

    ctx->aux1 = malloc(ctx->aux1_len * sizeof(*(ctx->aux1)));
    ctx->aux2 = malloc(ctx->aux2_len * sizeof(*(ctx->aux2)));

    if (!ctx->aux1 || !ctx->aux2)
        goto fail_b;

    size_t main_size_max = src_len + roundup(src_len, 64) / 8;
    main_size_max += vbyte_size(main_size_max);
    size_t ordinals_size_max = divup(src_len, 2);

    if (!out_stream_alloc(&(ctx->main), main_size_max))
        goto fail_b;

    if (!out_stream_alloc(&(ctx->ordinals), ordinals_size_max))
        goto fail_a;

    *ctx_out = ctx;

    return;

fail_a:
    out_stream_free(&(ctx->main));
fail_b:
    debug("Resource allocation failed");
    free(ctx->aux1);
    free(ctx->aux2);
}

void encode_ctx_fini(struct encode_ctx **ctx)
{
    free((*ctx)->aux1);
    free((*ctx)->aux2);
    out_stream_free(&((*ctx)->main));
    out_stream_free(&((*ctx)->ordinals));
    free(*ctx);
    *ctx = NULL;
}

#define min_factor_offs 1
#define min_factor_len 3

static size_t factor_offs_bytesize(uint32_t val)
{
    return divup(8 + 4 * vnibble_size((val - min_factor_offs) >> 8), 8);
}

static uint32_t read_factor_offs(struct io_stream *stream)
{
    return ((read_vnibble(stream) << 8) | read_u8(stream)) + min_factor_offs;
}

static void write_factor_offs(struct io_stream *stream, uint32_t val)
{
    write_vnibble(stream, (val - min_factor_offs) >> 8);
    write_u8(stream, (val - min_factor_offs) & 0xffu);
}

static uint32_t read_factor_len(struct io_stream *stream, size_t gr_k)
{
    return read_gr(stream, gr_k) + min_factor_len;
}

static void write_factor_len(struct io_stream *stream, uint32_t val, size_t gr_k)
{
    write_gr(stream, val - min_factor_len, gr_k);
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

    if (psv != -1)
        psv_len = lcp_cmp(text, text_len, 0, psv, pos);

    if (nsv != -1)
        nsv_len = lcp_cmp(text, text_len, 0, nsv, pos);

    ctx->psv = psv;
    ctx->psv_len = psv_len;
    ctx->nsv = nsv;
    ctx->nsv_len = nsv_len;
}

#define last_literals 8

uint32_t salz_encode_default(struct encode_ctx *ctx, uint8_t *src,
        size_t src_len, uint8_t *dst, size_t dst_len)
{
#ifdef NDEBUG
    unused(dst_len);
#endif

    src_len -= last_literals;

    if (ctx->aux1 == NULL ||
        ctx->aux2 == NULL ||
        ctx->aux1_len < src_len + 2 ||
        ctx->aux2_len < 2 * (src_len + 1) ||
        ctx->main.buf == NULL ||
        ctx->ordinals.buf == NULL) {
        debug("Allocated resources are not enough");
        return 0;
    }

    int32_t *sa = ctx->aux1;
    int32_t *aux = ctx->aux2;
    int32_t *dist = ctx->aux1;
    size_t dist_len = ctx->aux1_len;

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
            aux[0 + 2 * sa[top]] = sa[top - 1]; /* PSV */
            aux[1 + 2 * sa[top]] = sa[i];       /* NSV */
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

    memset(dist, 0, dist_len * sizeof(*dist));
    size_t factor_len_max = 0;

    size_t factor_pos = 0;
    aux[1 + 2 * factor_pos] = 0;
    factor_pos += 1;
    for (size_t src_pos = 1; src_pos < src_len; ) {
        int32_t psv = aux[0 + 2 * src_pos];
        int32_t nsv = aux[1 + 2 * src_pos];

        lz_factor(&fctx, src, src_len, src_pos, psv, nsv);

        uint32_t psv_offs = src_pos - fctx.psv;
        uint32_t psv_len = fctx.psv_len;
        uint32_t nsv_offs = src_pos - fctx.nsv;
        uint32_t nsv_len = fctx.nsv_len;

        uint32_t factor_offs = (psv_len == nsv_len) * min(psv_offs, nsv_offs) +
                               (psv_len > nsv_len) * psv_offs +
                               (nsv_len > psv_len) * nsv_offs;

        uint32_t factor_len = max(psv_len, nsv_len);

        factor_len *= ((factor_len >= min_factor_len) &&
                       (factor_offs_bytesize(factor_offs) < factor_len));

        if (factor_len) {
            uint32_t psv_np = aux[0 + 2 * (src_pos + 1)];
            uint32_t nsv_np = aux[1 + 2 * (src_pos + 1)];

            lz_factor(&fctx, src, src_len, src_pos + 1, psv_np, nsv_np);

            uint32_t psv_offs_np = (src_pos + 1) - fctx.psv;
            uint32_t psv_len_np = fctx.psv_len;
            uint32_t nsv_offs_np = (src_pos + 1) - fctx.nsv;
            uint32_t nsv_len_np = fctx.nsv_len;

            uint32_t factor_offs_np = (psv_len_np == nsv_len_np) * min(psv_offs_np, nsv_offs_np) +
                                      (psv_len_np > nsv_len_np) * psv_offs_np +
                                      (nsv_len_np > psv_len_np) * nsv_offs_np;

            uint32_t factor_len_np = max(psv_len_np, nsv_len_np);
            factor_len_np *= (factor_offs_bytesize(factor_offs_np) < factor_len_np);

            factor_len *= (factor_len_np < factor_len + 2);
        }

        aux[0 + 2 * factor_pos] = (int32_t)factor_offs;
        aux[1 + 2 * factor_pos] = (int32_t)factor_len;
        factor_pos += 1;

        dist[factor_len] += 1;
        factor_len_max = max(factor_len_max, factor_len);

        src_pos += max(1, factor_len);
    }

#ifdef ENABLE_STATS
    increment_clock(st.factor_time);
#endif

    size_t gr_k = 0;

#define gr_k_lim 27
#define gr_opt_depth 8192

    if (factor_len_max) {
        size_t cost = UINT_MAX;

        for (size_t k = 0; k < gr_k_lim; k++) {
            size_t alt_cost = 0;

            for (size_t i = min_factor_len;
                 i < min(factor_len_max, gr_opt_depth) + 1;
                 i++) {
                alt_cost += dist[i] * gr_bitsize(i - min_factor_len, k);
            }

            if (alt_cost > cost)
                break;

            cost = alt_cost;
            gr_k = k;
        }
    }

#ifdef ENABLE_STATS
    increment_clock(st.mincost_time);
#endif

    struct io_stream *main = &ctx->main;
    out_stream_init(main);

    struct io_stream *ordinals = &ctx->ordinals;
    out_stream_init(ordinals);

    write_vnibble(main, gr_k);

    size_t src_pos = 0;
    size_t prev_offs = 0;
    size_t ord = 0;
    size_t prev_ord = 0;
    factor_pos = 0;
    while (src_pos < src_len) {
        uint32_t factor_offs = (uint32_t)aux[0 + 2 * factor_pos];
        uint32_t factor_len = (uint32_t)aux[1 + 2 * factor_pos];
        factor_pos += 1;

        if (!factor_len) {
            write_bit(main, 0);
            assert(src_pos < src_len);
            cpy_u8_tos(main, src, src_pos++);
        } else {
            write_bit(main, 1);
            assert(factor_offs <= src_pos);

            if (unlikely(factor_offs == prev_offs)) {
                write_vnibble(ordinals, (uint32_t)(ord - prev_ord));
                prev_ord = ord;
            } else {
                write_factor_offs(main, factor_offs);
            }

            write_factor_len(main, factor_len, gr_k);
            src_pos += factor_len;

            prev_offs = factor_offs;
            ord += 1;
        }
    }

    src_len += last_literals;
    for (size_t i = 0; i < last_literals; i++) {
        write_bit(main, 0);
        assert(src_pos < src_len);
        cpy_u8_tos(main, src, src_pos++);
    }

    if (stream_len(main) + stream_len(ordinals) >= src_len + 18) {
        out_stream_init(main);
        out_stream_init(ordinals);
        src_pos = 0;
    }

    write_vnibble(ordinals, (uint32_t)(ord - prev_ord));

    size_t dst_pos = 0;
    dst_pos += out_stream_fini(main, dst, dst_len, dst_pos);
    dst_pos += out_stream_fini(ordinals, dst, dst_len, dst_pos);

    if (src_pos < src_len) {
        size_t copy_len = src_len - src_pos;
        assert(dst_pos + copy_len < dst_len + 1);
        memcpy(&dst[dst_pos], &src[src_pos], copy_len);
        src_pos += copy_len;
        dst_pos += copy_len;
    }

#ifdef ENABLE_STATS
    increment_clock(st.encode_time);
#endif

    return (uint32_t)dst_pos;
}

uint32_t salz_decode_default(uint8_t *src, size_t src_len, uint8_t *dst,
        size_t dst_len)
{
#ifdef NDEBUG
    unused(dst_len);
#endif

    size_t src_pos = 0;
    size_t dst_pos = 0;

    struct io_stream main;
    struct io_stream ordinals;

    src_pos += in_stream_init(&main, src, src_len, src_pos);
    src_pos += in_stream_init(&ordinals, src, src_len, src_pos);

    size_t gr_k = read_vnibble(&main);

    size_t prev_offs = 0;
    size_t ord = 0;
    size_t next_ord = read_vnibble(&ordinals);
    while (!stream_empty(&main)) {
        if (!read_bit(&main)) {
            assert(dst_pos < dst_len);
            cpy_u8_froms(&main, dst, dst_pos++);
        } else {
            uint32_t factor_offs;

            if (unlikely(ord == next_ord)) {
                factor_offs = prev_offs;
                next_ord += read_vnibble(&ordinals);
            } else {
                factor_offs = read_factor_offs(&main);
            }

            uint32_t factor_len = read_factor_len(&main, gr_k);
            assert(factor_offs <= dst_pos);
            assert(dst_pos + factor_len < dst_len + 1);
            cpy_factor(dst, dst_pos, factor_offs, factor_len);
            dst_pos += factor_len;

            prev_offs = factor_offs;
            ord += 1;
        }
    }

    if (src_pos < src_len) {
        size_t copy_len = src_len - src_pos;
        assert(dst_pos + copy_len < dst_len + 1);
        memcpy(&dst[dst_pos], &src[src_pos], copy_len);
        src_pos += copy_len;
        dst_pos += copy_len;
    }

    return (uint32_t)dst_pos;
}
