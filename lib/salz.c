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


static size_t read_vbyte(uint8_t *buf, size_t buf_len, size_t pos,
        uint32_t *res);
static size_t write_vbyte(uint8_t *buf, size_t buf_len, size_t pos,
        uint32_t val);

struct io_stream {
    uint8_t *buf;
    size_t buf_len;
    size_t buf_pos;
    uint64_t bits;
    size_t bits_avail;
    size_t bits_pos;
};

static uint8_t read_u8(struct io_stream *stream)
{
    assert(stream->buf_pos < stream->buf_len);
    return stream->buf[stream->buf_pos++];
}

static uint64_t read_u64(uint8_t *src, size_t pos)
{
    uint64_t val;

    memcpy(&val, &src[pos], sizeof(val));

    return val;
}

static void write_u8(struct io_stream *stream, uint8_t val)
{
    assert(stream->buf_pos < stream->buf_len);
    stream->buf[stream->buf_pos++] = val;
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

static void cpy_factor(uint8_t *buf, size_t cpy_pos, size_t pos, size_t len)
{
    while (len--)
        buf[pos++] = buf[cpy_pos++];
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

static bool out_stream_init(struct io_stream *stream, size_t size)
{
    stream->buf = malloc(size);

    if (!stream->buf)
        return false;

    assert(field_sizeof(struct io_stream, bits) < size + 1);
    stream->buf_len = size;
    stream->buf_pos = field_sizeof(struct io_stream, bits);
    stream->bits = 0;
    stream->bits_pos = 0;
    stream->bits_avail = field_sizeof(struct io_stream, bits) * 8;

    return true;
}

static void out_stream_reset(struct io_stream *stream)
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

    free(stream->buf);

    return dst_pos - orig_pos;
}

static void queue_bits(struct io_stream *stream)
{
    assert(stream->buf_pos + field_sizeof(struct io_stream, bits) <
           stream->buf_len + 1);
    stream->bits = read_u64(stream->buf, stream->buf_pos);
    stream->buf_pos += field_sizeof(struct io_stream, bits);
    stream->bits_avail = field_sizeof(struct io_stream, bits) * 8;
}

static uint8_t read_bit(struct io_stream *stream)
{
    if (!stream->bits_avail)
        queue_bits(stream);

    uint8_t ret = (stream->bits & 0x8000000000000000u) ? 1 : 0;
    stream->bits <<= 1;
    stream->bits_avail -= 1;

    return ret;
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

static void write_bit(struct io_stream *stream, uint8_t flag)
{
    if (!stream->bits_avail)
        flush_bits(stream);

    stream->bits = (stream->bits << 1) | flag;
    stream->bits_avail -= 1;
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

static void write_unary(struct io_stream *stream, uint32_t val)
{
    write_zeros(stream, val);
    write_bit(stream, 1);
}

static size_t gr3_bitsize(uint32_t val)
{
    return (val >> 3) + 1 + 3;
}

static void write_gr3(struct io_stream *stream, uint32_t val)
{
    write_unary(stream, val >> 3);
    write_bits(stream, val & ((1u << 3) - 1), 3);
}

static uint32_t read_gr3(struct io_stream *stream)
{
    return (read_unary(stream) << 3) | read_bits(stream, 3);
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

static uint32_t read_vnibble(struct io_stream *stream)
{
    uint8_t nibble = read_bits(stream, 4);
    uint32_t ret = nibble & 0x7u;

    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(stream, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(stream, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(stream, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(stream, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(stream, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(stream, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(stream, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(stream, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(stream, 4);
    ret = ((ret + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return ret;

    nibble = read_bits(stream, 4);
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

static void write_vnibble(struct io_stream *stream, uint32_t val)
{
    uint64_t nibbles;

    size_t nibbles_len = encode_vnibble(val, &nibbles);

    write_bits(stream, nibbles, nibbles_len * 4);
}

static size_t read_vbyte(uint8_t *buf, size_t buf_len, size_t pos,
        uint32_t *res)
{
#ifdef NDEBUG
    unused(buf_len);
#endif

    size_t orig_pos = pos;

    assert(pos < buf_len);
    uint8_t byte = buf[pos++];
    *res = byte & 0x7fu;

    while (byte < 0x80u) {
        assert(pos < buf_len);
        byte = buf[pos++];
        *res = ((*res + 1) << 7) | (byte & 0x7fu);
    }

    return pos - orig_pos;
}

static size_t vbyte_size(uint32_t val)
{
    size_t vbyte_len = 1;

    while (val >>= 7) {
        val -= 1;
        vbyte_len += 1;
    }

    return vbyte_len;
}

static size_t encode_vbyte(uint32_t val, uint64_t *res)
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

static size_t write_vbyte(uint8_t *buf, size_t buf_len, size_t pos,
        uint32_t val)
{
#ifdef NDEBUG
    unused(buf_len);
#endif

    size_t orig_pos = pos;

    uint64_t vbyte;
    size_t vbyte_len = encode_vbyte(val, &vbyte);

    assert(pos + vbyte_len < buf_len + 1);

    while (vbyte_len--) {
        buf[pos++] = vbyte & 0xffu;
        vbyte >>= 8;
    }

    return pos - orig_pos;
}

static size_t factor_offs_bitsize(uint32_t val)
{
    return 8 + vnibble_bitsize((val - 1) >> 8);
}

static void write_factor_offs(struct io_stream *stream, uint32_t val)
{
    write_vnibble(stream, (val - 1) >> 8);
    write_u8(stream, (val - 1) & 0xffu);
}

static uint32_t read_factor_offs(struct io_stream *stream)
{
    return ((read_vnibble(stream) << 8) |
            read_u8(stream)) + 1;
}

static size_t factor_len_bitsize(uint32_t val)
{
    return gr3_bitsize(val - 3);
}

static void write_factor_len(struct io_stream *stream, uint32_t val)
{
    write_gr3(stream, val - 3);
}

static uint32_t read_factor_len(struct io_stream *stream)
{
    return read_gr3(stream) + 3;
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

uint32_t salz_encode_default(struct encode_ctx *ctx, uint8_t *src,
        size_t src_len, uint8_t *dst, size_t dst_len)
{
#ifdef NDEBUG
    unused(dst_len);
#endif

    if (ctx->sa == NULL ||
        ctx->aux == NULL ||
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

    for (size_t i = 0; i < src_len + 1; i++) {
        aux[2 + 5 * i] = INT_MAX; /* Costs */
        aux[4 + 5 * i] = 0; /* Offsets */
    }

    aux[2 + 5 * 0] = 0; /* First position cost */
    aux[2 + 5 * 1] = 9; /* Second position cost */
    aux[3 + 5 * 1] = 0; /* Second position parent */

    for (size_t src_pos = 1; src_pos < src_len; src_pos++) {
        int32_t psv = aux[0 + 5 * src_pos];
        int32_t nsv = aux[1 + 5 * src_pos];

        lz_factor(&fctx, src, src_len, src_pos, psv, nsv);

        int32_t base_cost = aux[2 + 5 * src_pos];
        int32_t prev_offs = aux[4 + 5 * src_pos];

        int32_t literal_cost = 9 + base_cost;
        if (literal_cost < aux[2 + 5 * (src_pos + 1)]) {
            aux[2 + 5 * (src_pos + 1)] = literal_cost;
            aux[3 + 5 * (src_pos + 1)] = src_pos;
            aux[4 + 5 * (src_pos + 1)] = prev_offs;
        }

        int32_t psv_len = (int32_t)fctx.psv_len;
        if (psv_len >= 3) {
            int32_t psv_offs = (int32_t)(src_pos - fctx.psv);
            int32_t psv_cost = 1 + factor_offs_bitsize(psv_offs) +
                               factor_len_bitsize(psv_len) +
                               base_cost;

            if (psv_offs == prev_offs)
                psv_cost -= factor_offs_bitsize(psv_offs);

            if (psv_cost < aux[2 + 5 * (src_pos + psv_len)]) {
                aux[2 + 5 * (src_pos + psv_len)] = psv_cost;
                aux[3 + 5 * (src_pos + psv_len)] = src_pos;
                aux[4 + 5 * (src_pos + psv_len)] = psv_offs;
            }
        }

        int32_t nsv_len = (int32_t)fctx.nsv_len;
        if (nsv_len >= 3) {
            int32_t nsv_offs = (int32_t)(src_pos - fctx.nsv);
            int32_t nsv_cost = 1 + factor_offs_bitsize(nsv_offs) +
                               factor_len_bitsize(nsv_len) +
                               base_cost;

            if (nsv_offs == prev_offs)
                nsv_cost -= factor_offs_bitsize(nsv_offs);

            if (nsv_cost < aux[2 + 5 * (src_pos + nsv_len)]) {
                aux[2 + 5 * (src_pos + nsv_len)] = nsv_cost;
                aux[3 + 5 * (src_pos + nsv_len)] = src_pos;
                aux[4 + 5 * (src_pos + nsv_len)] = nsv_offs;
            }
        }
    }

#ifdef ENABLE_STATS
    increment_clock(st.factor_time);
#endif

    for (size_t src_pos = src_len; src_pos > 0; ) {
        int32_t prev_pos = aux[3 + 5 * src_pos];
        int32_t factor_len = src_pos - prev_pos;

        if (factor_len == 1) {
            aux[0 + 5 * prev_pos] = 0;
            aux[1 + 5 * prev_pos] = 0;
        } else {
            aux[0 + 5 * prev_pos] = aux[4 + 5 * src_pos];
            aux[1 + 5 * prev_pos] = factor_len;
        }

        src_pos = prev_pos;
    }

#ifdef ENABLE_STATS
    increment_clock(st.mincost_time);
#endif

    struct io_stream main;
    struct io_stream ordinals;

    size_t main_size_max = src_len + roundup(src_len, 64) / 8;
    main_size_max += vbyte_size(main_size_max);
    size_t ordinals_size_max = divup(src_len, 2);

    if (!out_stream_init(&main, main_size_max))
        return 0;

    if (!out_stream_init(&ordinals, ordinals_size_max)) {
        out_stream_fini(&ordinals, NULL, 0, 0);
        return 0;
    }

    size_t src_pos = 0;
    uint32_t prev_offs = 0;
    size_t ord = 0;
    size_t prev_ord = 0;
    while (src_pos < src_len) {
        uint32_t factor_len = (uint32_t)aux[1 + 5 * src_pos];

        if (!factor_len) {
            write_bit(&main, 0);
            assert(src_pos < src_len);
            cpy_u8_tos(&main, src, src_pos++);
        } else {
            write_bit(&main, 1);
            uint32_t factor_offs = (uint32_t)aux[0 + 5 * src_pos];

            assert(factor_offs <= src_pos);

            if (factor_offs == prev_offs) {
                write_vnibble(&ordinals, (uint32_t)(ord - prev_ord));
                prev_ord = ord;
            } else {
                write_factor_offs(&main, factor_offs);
            }

            write_factor_len(&main, factor_len);
            src_pos += factor_len;

            prev_offs = factor_offs;
            ord += 1;
        }
    }

    if (stream_len(&main) + stream_len(&ordinals) >= src_len + 18) {
        out_stream_reset(&main);
        out_stream_reset(&ordinals);
        src_pos = 0;
    }

    write_bit(&main, 0);
    write_vnibble(&ordinals, (uint32_t)(ord - prev_ord));

    size_t dst_pos = 0;
    dst_pos += out_stream_fini(&main, dst, dst_len, dst_pos);
    dst_pos += out_stream_fini(&ordinals, dst, dst_len, dst_pos);

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

    size_t prev_offs = 0;
    size_t ord = 0;
    size_t next_ord = read_vnibble(&ordinals);
    while (true) {
        if (!read_bit(&main)) {
            if (stream_empty(&main))
                break;

            assert(dst_pos < dst_len);
            cpy_u8_froms(&main, dst, dst_pos++);
        } else {
            uint32_t factor_offs;

            if (ord == next_ord) {
                factor_offs = prev_offs;
                next_ord += read_vnibble(&ordinals);
            } else {
                factor_offs = read_factor_offs(&main);
            }

            uint32_t factor_len = read_factor_len(&main);

            assert(factor_offs <= dst_pos);
            assert(dst_pos + factor_len < dst_len + 1);
            cpy_factor(dst, dst_pos - factor_offs, dst_pos, factor_len);
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

void encode_ctx_init(struct encode_ctx **ctx_out, size_t src_len)
{
    size_t sa_len = src_len + 2;
    size_t aux_len = 5 * (src_len + 1);
    int32_t *sa = malloc(sa_len * sizeof(*sa));
    int32_t *aux = malloc(aux_len * sizeof(*aux));

    if (!sa || !aux) {
        debug("Resource allocation failed");
        free(sa);
        free(aux);
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
