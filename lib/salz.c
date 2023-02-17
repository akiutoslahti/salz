/*
 * salz.c - SA based LZ compressor
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

/*
 * References:
 *   [1] Utoslahti, A. (2022). Practical Aspects of Implementing a Suffix
 *       Array-based Lempel-Ziv Data Compressor [Master’s thesis, University
 *       of Helsinki]. http://urn.fi/URN:NBN:fi:hulib-202206132325
 *   [2] Kärkkäinen, J., Kempa, D., Puglisi, S.J. (2013). Linear Time
 *       Lempel-Ziv Factorization: Simple, Fast, Small. In: Fischer, J.,
 *       Sanders, P. (eds) Combinatorial Pattern Matching. CPM 2013.
 *       Lecture Notes in Computer Science, vol 7922. Springer, Berlin,
 *       Heidelberg. https://doi.org/10.1007/978-3-642-38905-4_19
 */

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "salz.h"
#include "vlc.h"
#include "libsais.h"

#ifdef NDEBUG
#   define debug(fmt, ...) do {} while(0)
#else
#   include <stdio.h>
#   define debug(fmt, ...)                              \
        do {                                            \
            fprintf(stderr, "(%s:%d) - " fmt "\n",      \
                    __func__, __LINE__, ##__VA_ARGS__); \
        } while (0)
#endif

#define min(a, b) (((a) < (b)) ? (a) : (b))

#define divup(a, b) (((a) + (b) - 1) / (b))
#define roundup(a, b) (divup(a, b) * b)

#define field_sizeof(t, f) (sizeof(((t*)0)->f))

#define unlikely(x) __builtin_expect((x),0)

#ifdef __GNUC__
#   define salz_memcpy(dst, src, n) __builtin_memcpy(dst, src, n)
#else
#   define salz_memcpy(dst, src, n) memcpy(dst, src, n)
#endif

#ifdef ENABLE_STATS
    struct stats st;

    struct stats *get_stats(void) {
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

enum salz_stream_type {
    STREAM_TYPE_PLAIN = 0,
    STREAM_TYPE_SALZ,
    STREAM_TYPE_MAX,
};

#define TOKEN_TYPE_LITERAL (0)
#define TOKEN_TYPE_FACTOR  (1)

struct io_stream {
    uint8_t *buf;
    size_t buf_len;
    size_t buf_pos;
    uint64_t bits;
    size_t bits_avail;
    size_t bits_pos;
};

static void write_u8(struct io_stream *stream, uint8_t val)
{
    assert(stream->buf_pos < stream->buf_len);
    stream->buf[stream->buf_pos++] = val;
}

static void write_u32(uint8_t *dst, size_t pos, uint32_t val)
{
    salz_memcpy(&dst[pos], &val, sizeof(val));
}

static uint64_t read_u64_unsafe(const uint8_t *src, size_t pos)
{
    uint64_t val;

    salz_memcpy(&val, &src[pos], sizeof(val));

    return val;
}

static void write_u64(uint8_t *dst, size_t pos, uint64_t val)
{
    salz_memcpy(&dst[pos], &val, sizeof(val));
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

static void write_bit(struct io_stream *stream, uint8_t bit)
{
    if (stream->bits_avail == 0)
        flush_bits(stream);

    stream->bits = (stream->bits << 1) | bit;
    stream->bits_avail -= 1;
}

static void write_bits(struct io_stream *stream, uint64_t bits, size_t count)
{
    if (stream->bits_avail == 0)
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
        if (stream->bits_avail == 0)
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

static size_t vnibble_bitsize(uint32_t val)
{
    return 4 * vnibble_size(val);
}

static void write_vnibble(struct io_stream *stream, uint32_t val)
{
    uint64_t nibbles;

    size_t nibbles_len = encode_vnibble_le(val, &nibbles);

    write_bits(stream, nibbles, nibbles_len * 4);
}

static size_t stream_len_get(struct io_stream *stream)
{
    return stream->buf_pos;
}

static void enc_stream_init(struct io_stream *stream)
{
    assert(field_sizeof(struct io_stream, bits) < stream->buf_len + 1);
    stream->buf_pos = field_sizeof(struct io_stream, bits);
    stream->bits = 0;
    stream->bits_pos = 0;
    stream->bits_avail = field_sizeof(struct io_stream, bits) * 8;
}

static struct io_stream *enc_stream_create(size_t size)
{
    struct io_stream *stream;

    if ((stream = malloc(sizeof(*stream))) == NULL)
        return NULL;
    memset(stream, 0, sizeof(*stream));

    if ((stream->buf = malloc(size)) == NULL) {
        free(stream);
        return NULL;
    }

    stream->buf_len = size;
    enc_stream_init(stream);

    return stream;
}

static void enc_stream_destroy(struct io_stream *stream)
{
    if (stream == NULL)
        return;
    free(stream->buf);
    free(stream);
}

static size_t enc_stream_fini(struct io_stream *stream, uint8_t *dst,
    size_t dst_len, size_t dst_pos)
{
    size_t orig_pos = dst_pos;

    stream->bits <<= (stream->bits_avail);
    write_u64(stream->buf, stream->bits_pos, stream->bits);

    if (dst) {
        uint32_t stream_hdr = 0;
        stream_hdr |= STREAM_TYPE_SALZ << 24;
        stream_hdr |= stream->buf_pos & 0xffffff;
        write_u32(dst, dst_pos, stream_hdr);
        dst_pos += 4;

        if (dst_pos + stream->buf_pos > dst_len)
            return 0;

        salz_memcpy(&dst[dst_pos], stream->buf, stream->buf_pos);
        dst_pos += stream->buf_pos;
    }

    return dst_pos - orig_pos;
}

struct factor_ctx {
    int32_t psv;
    size_t psv_len;
    int32_t nsv;
    size_t nsv_len;
};

static struct factor_ctx *factor_ctx_create(void)
{
    struct factor_ctx *ctx;

    if ((ctx = malloc(sizeof(*ctx))) == NULL)
        return NULL;

    ctx->psv = -1;
    ctx->psv_len = 0;
    ctx->nsv = -1;
    ctx->nsv_len = 0;

    return ctx;
}

static void factor_ctx_destroy(struct factor_ctx *ctx)
{
    if (ctx == NULL)
        return;
    free(ctx);
}

#define min_factor_offs 1
#define min_factor_len 3

static size_t factor_offs_bitsize(uint32_t val)
{
    return 8 + vnibble_bitsize((val - min_factor_offs) >> 8);
}

static void write_factor_offs(struct io_stream *stream, uint32_t val)
{
    write_vnibble(stream, (val - min_factor_offs) >> 8);
    write_u8(stream, (val - min_factor_offs) & 0xffu);
}

static size_t factor_len_bitsize(uint32_t val)
{
    return gr3_bitsize(val - min_factor_len);
}

static void write_factor_len(struct io_stream *stream, uint32_t val)
{
    write_gr3(stream, val - min_factor_len);
}

static size_t lcp_cmp(const uint8_t *text, size_t text_len, size_t common_len,
    size_t pos1, size_t pos2)
{
    size_t len = common_len;

    while (pos2 + len < text_len - 8 + 1) {
        uint64_t val1 = read_u64_unsafe(text, pos1 + len);
        uint64_t val2 = read_u64_unsafe(text, pos2 + len);
        uint64_t diff = val1 ^ val2;

        if (diff)
            return len + (__builtin_ctzll(diff) >> 3);

        len += 8;
    }

    while (pos2 + len < text_len && text[pos1 + len] == text[pos2 + len])
        len += 1;

    return len;
}

static void lz_factor(struct factor_ctx *ctx, const uint8_t *text,
    size_t text_len, size_t pos, int32_t psv, int32_t nsv)
{
    size_t psv_len = 0;
    size_t nsv_len = 0;

    if (psv != -1) {
        size_t common_len = ctx->psv_len + !ctx->psv_len - 1;
        psv_len = lcp_cmp(text, text_len, common_len, psv, pos);
    }

    if (nsv != -1) {
        size_t common_len = ctx->nsv_len + !ctx->nsv_len - 1;
        nsv_len = lcp_cmp(text, text_len, common_len, nsv, pos);
    }

    ctx->psv = psv;
    ctx->psv_len = psv_len;
    ctx->nsv = nsv;
    ctx->nsv_len = nsv_len;
}

size_t salz_encode_default(const uint8_t *src, size_t src_len, uint8_t *dst,
    size_t dst_len)
{
    int32_t *sa = NULL;
    size_t sa_len;
    int32_t *aux = NULL;
    size_t aux_len;
    struct io_stream *enc_stream = NULL;
    size_t enc_stream_capacity;
    struct factor_ctx *fctx = NULL;
    size_t src_pos;
    size_t dst_pos;
    size_t written = 0;

    /*
     * @TODO Determine the size of smallest data chunk that can be encoded.
     * Atleast if src_len <= 8 MUST fail
     */

    /*
     * Allocate resources for encoding
     */

    sa_len = src_len + 2;
    aux_len = 4 * (src_len + 1);
    /*
     * Determine the upper boundary for the final compressed size to eliminate
     * buffer boundary checks when writing to the encoding stream.
     */
    enc_stream_capacity = src_len + roundup(src_len, 64) / 8;

    if ((sa = malloc(sa_len * sizeof(*sa))) == NULL ||
        (aux = malloc(aux_len * sizeof(*aux))) == NULL ||
        (enc_stream = enc_stream_create(enc_stream_capacity)) == NULL ||
        (fctx = factor_ctx_create()) == NULL) {
        debug("Couldn't allocate enough memory for encoding");
        goto exit;
    }

    /*
     * Force last 8 bytes to be encoded as literals to make
     * copying factors 8 bytes at a time safe.
     */
    src_len -= 8;

#ifdef ENABLE_STATS
    start_clock();
#endif

    /*
     * Construction of Suffix Array
     */

    if (libsais(src, sa + 1, src_len, 0, NULL) != 0) {
        debug("Couldn't construct Suffix Array with 'libsais'");
        goto exit;
    }

#ifdef ENABLE_STATS
    increment_clock(st.sa_time);
#endif

    /*
     * PSV/NSV array construction using only Suffix Array as described in [2].
     */

    sa[0] = -1;
    sa[src_len + 1] = -1;
    for (size_t top = 0, i = 1; i < src_len + 2; i++) {
        while (sa[top] > sa[i]) {
            aux[0 + 4 * sa[top]] = sa[top - 1]; /* PSV */
            aux[1 + 4 * sa[top]] = sa[i]; /* NSV */
            top -= 1;
        }
        top += 1;
        sa[top] = sa[i];
    }

#ifdef ENABLE_STATS
    increment_clock(st.psv_nsv_time);
#endif

    /*
     * Determining factor candidates for all text positions as described in
     * Section 3.4 of [1].
     */

    /* Skip factorization of first position and force it to be a literal */
    aux[1 + 4 * 0] = 1;
    aux[3 + 4 * 0] = 1;
    for (size_t src_pos = 1; src_pos < src_len; src_pos++) {
        int32_t psv = aux[0 + 4 * src_pos];
        int32_t nsv = aux[1 + 4 * src_pos];

        lz_factor(fctx, src, src_len, src_pos, psv, nsv);

        aux[0 + 4 * src_pos] = (int32_t)(src_pos - fctx->psv);
        aux[1 + 4 * src_pos] = (int32_t)fctx->psv_len;
        aux[2 + 4 * src_pos] = (int32_t)(src_pos - fctx->nsv);
        aux[3 + 4 * src_pos] = (int32_t)fctx->nsv_len;
    }

#ifdef ENABLE_STATS
    increment_clock(st.factor_time);
#endif

    /*
     * Dynamic programming SSSP (Single Source Shortest Path) approach to
     * optimizing the final encoded size of the Lempel-Ziv factorization
     * of a text as described in Section 3.5.4 of [1].
     */

    /*
     * Nothing left to do after reaching last position, initialize cost as zero
     */
    aux[2 + 4 * src_len] = 0;
    for (size_t src_pos = src_len - 1; src_pos; src_pos--) {
        /* Cost of using a literal */
        int32_t factor_offs = 0;
        int32_t factor_len = 1;
        int32_t cost = 9 + aux[2 + 4 * (src_pos + 1)];

        /* Cost of using PSV candidate */
        int32_t alt_len = aux[1 + 4 * src_pos];
        if (alt_len >= min_factor_len) {
            int32_t alt_offs = aux[0 + 4 * src_pos];
            int32_t alt_cost = 1 + factor_offs_bitsize(alt_offs) +
                               factor_len_bitsize(alt_len) +
                               aux[2 + 4 * (src_pos + alt_len)];

            if (alt_cost < cost) {
                cost = alt_cost;
                factor_offs = alt_offs;
                factor_len = alt_len;
            }
        }

        /* Cost of using NSV candidate */
        alt_len = aux[3 + 4 * src_pos];
        if (alt_len >= min_factor_len) {
            int32_t alt_offs = aux[2 + 4 * src_pos];
            int32_t alt_cost = 1 + factor_offs_bitsize(alt_offs) +
                               factor_len_bitsize(alt_len) +
                               aux[2 + 4 * (src_pos + alt_len)];

            if (alt_cost < cost) {
                cost = alt_cost;
                factor_offs = alt_offs;
                factor_len = alt_len;
            }
        }

        aux[0 + 4 * src_pos] = factor_offs;
        aux[1 + 4 * src_pos] = factor_len;
        aux[2 + 4 * src_pos] = cost;
    }

#ifdef ENABLE_STATS
    increment_clock(st.mincost_time);
#endif

    /*
     * Encoding the produced Lempel-Ziv factorization using an encoding format
     * described in Sections 3.6.1 and 3.6.3 of [1].
     */

    src_pos = 0;
    while (src_pos < src_len) {
        uint32_t factor_len = (uint32_t)aux[1 + 4 * src_pos];
        if (factor_len == 1) {
            /* Literal */
            write_bit(enc_stream, 0);
            assert(src_pos < src_len);
            write_u8(enc_stream, src[src_pos]);
        } else {
            /* Factor */
            write_bit(enc_stream, 1);
            uint32_t factor_offs = (uint32_t)aux[0 + 4 * src_pos];
            write_factor_offs(enc_stream, factor_offs);
            write_factor_len(enc_stream, factor_len);
        }
        src_pos += factor_len;
    }

    /*
     * Encode the last 8 bytes that were "forced" to be literals in the start.
     */
    src_len += 8;
    for (size_t i = 0; i < 8; i++) {
        write_bit(enc_stream, 0);
        assert(src_pos < src_len);
        write_u8(enc_stream, src[src_pos]);
        src_pos += 1;
    }

    dst_pos = 0;
    if (stream_len_get(enc_stream) >= src_len + 9) {
        uint32_t stream_hdr = 0;
        stream_hdr |= STREAM_TYPE_PLAIN << 24;
        stream_hdr |= src_len & 0xffffff;
        write_u32(dst, dst_pos, stream_hdr);
        dst_pos += 4;
        if (dst_pos + src_len > dst_len) {
            debug("Couldn't write uncompressed data into 'dst' buf - "
                  "not enough space");
            goto exit;
        }

        salz_memcpy(&dst[dst_pos], &src[src_pos], src_len);
        src_pos += src_len;
        dst_pos += dst_len;
    }
    else {
        dst_pos += enc_stream_fini(enc_stream, dst, dst_len, dst_pos);
        if (dst_pos == 0) {
            debug("Couldn't write encoded data into 'dst' buf - not enough space");
            goto exit;
        }
    }

#ifdef ENABLE_STATS
    increment_clock(st.encode_time);
#endif

    written = dst_pos;

exit:
    factor_ctx_destroy(fctx);
    enc_stream_destroy(enc_stream);
    free(aux);
    free(sa);

    return written;
}

struct salz_decode_ctx {
    uint8_t stream_type;

    uint8_t *src;
    size_t src_len;
    size_t src_pos;

    uint8_t *dst;
    size_t dst_len;
    size_t dst_pos;

    uint64_t bits;
    size_t bits_avail;
};

typedef struct salz_decode_ctx salz_decode_ctx;

static salz_decode_ctx *decode_ctx_init(uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len)
{
    salz_decode_ctx *ctx;
    uint32_t stream_hdr;
    uint8_t stream_type;
    size_t stream_len;

    ctx = malloc(sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));

    if (ctx == NULL) {
        debug("Couldn't allocate memory (%zu bytes)", sizeof(*ctx));
        return NULL;
    }

    if (src_len < 4) {
        debug("Couldn't read stream header");
        goto fail;
    }

    salz_memcpy(&stream_hdr, src, 4);
    stream_type = stream_hdr >> 24;
    stream_len = stream_hdr & 0xffffff;

    if (stream_type >= STREAM_TYPE_MAX) {
        debug("Unknown stream type (%u)", stream_type);
        goto fail;
    }

    if (stream_len > src_len - 4) {
        debug("Stream is truncated (expected: %zu, have: %zu)",
               stream_len, src_len - 4);
        goto fail;
    }

    ctx->stream_type = stream_type;
    ctx->src = src + 4;
    ctx->src_len = stream_len;
    ctx->src_pos = 0;
    ctx->dst = dst;
    ctx->dst_len = dst_len;
    ctx->dst_pos = 0;
    ctx->bits = 0;
    ctx->bits_avail = 0;

    return ctx;

fail:
    free(ctx);
    return NULL;
}

static void decode_ctx_fini(salz_decode_ctx *ctx)
{
    if (ctx != NULL)
        free(ctx);
}

static bool stream_is_empty(salz_decode_ctx *ctx)
{
    return ctx->src_pos == ctx->src_len;
}

static bool cpy_plain_stream(salz_decode_ctx *ctx)
{
    if (ctx->dst_len - ctx->dst_pos < ctx->src_len - ctx->src_pos)
        return false;

    salz_memcpy(&ctx->dst[ctx->dst_pos],
                &ctx->src[ctx->src_pos],
                ctx->src_len - ctx->src_pos);

    return true;
}

static bool read_u8(salz_decode_ctx *ctx, uint8_t *res)
{
    static_assert(sizeof(*res) == 1);

    if (unlikely(ctx->src_pos + 1 > ctx->src_len))
        return false;

    *res = ctx->src[ctx->src_pos++];

    return true;
}

static bool read_u64(salz_decode_ctx *ctx, uint64_t *res)
{
    static_assert(sizeof(*res) == 8);

    if (unlikely(ctx->src_pos + 8 > ctx->src_len))
        return false;

    salz_memcpy(res, &ctx->src[ctx->src_pos], 8);
    ctx->src_pos += 8;

    return true;
}

static bool queue_bits(salz_decode_ctx *ctx)
{
    if (unlikely(!read_u64(ctx, &ctx->bits)))
        return false;

    ctx->bits_avail = 64;

    return true;
}

static bool read_bit(salz_decode_ctx *ctx, uint8_t *res)
{
    if (ctx->bits_avail == 0 && unlikely(!queue_bits(ctx)))
        return false;

    *res = !!(ctx->bits & 0x8000000000000000u);
    ctx->bits <<= 1;
    ctx->bits_avail -= 1;

    return true;
}

static bool read_bits(salz_decode_ctx *ctx, size_t count, uint64_t *res)
{
    assert(count <= 64);

    if (ctx->bits_avail == 0 && unlikely(!queue_bits(ctx)))
        return false;

    if (count <= ctx->bits_avail) {
        *res = ctx->bits >> (64 - count);
        ctx->bits <<= count;
        ctx->bits_avail -= count;
        return true;
    }

    *res = ctx->bits >> (64 - ctx->bits_avail);
    count -= ctx->bits_avail;

    if (unlikely(!queue_bits(ctx)))
        return false;

    *res = (*res << count) | (ctx->bits >> (64 - count));
    ctx->bits <<= count;
    ctx->bits_avail -= count;

    return true;
}

static bool read_unary(salz_decode_ctx *ctx, uint32_t *res)
{
    uint32_t last_zeros;

    if (ctx->bits_avail == 0 && unlikely(!queue_bits(ctx)))
        return false;

    *res = 0;
    while (ctx->bits == 0) {
        *res += ctx->bits_avail;
        if (!unlikely(queue_bits(ctx)))
            return false;
    }

    last_zeros = __builtin_clzll(ctx->bits);
    ctx->bits <<= last_zeros + 1;
    ctx->bits_avail -= last_zeros + 1;

    *res += last_zeros;

    return true;
}

static bool read_gr3(salz_decode_ctx *ctx, uint32_t *res)
{
    uint32_t var;
    uint64_t fixed;

    if (unlikely(!read_unary(ctx, &var)))
        return false;
    if (unlikely(!read_bits(ctx, 3, &fixed)))
        return false;

    *res = (var << 3) | fixed;

    return true;
}

static bool read_nibble(salz_decode_ctx *ctx, uint8_t *res)
{
    uint64_t var;

    if (unlikely(!read_bits(ctx, 4, &var)))
        return false;

    *res = (uint8_t)var;

    return true;
}

static bool read_vnibble(salz_decode_ctx *ctx, uint32_t *res)
{
    uint8_t nibble;

    if (unlikely(!read_nibble(ctx, &nibble)))
        return false;
    *res = nibble & 0x7u;
    if (nibble >= 0x8u)
        return true;

    if (unlikely(!read_nibble(ctx, &nibble)))
        return false;
    *res = ((*res + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return true;

    if (unlikely(!read_nibble(ctx, &nibble)))
        return false;
    *res = ((*res + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return true;

    if (unlikely(!read_nibble(ctx, &nibble)))
        return false;
    *res = ((*res + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return true;

    if (unlikely(!read_nibble(ctx, &nibble)))
        return false;
    *res = ((*res + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return true;

    if (unlikely(!read_nibble(ctx, &nibble)))
        return false;
    *res = ((*res + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return true;

    if (unlikely(!read_nibble(ctx, &nibble)))
        return false;
    *res = ((*res + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return true;

    if (unlikely(!read_nibble(ctx, &nibble)))
        return false;
    *res = ((*res + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return true;

    if (unlikely(!read_nibble(ctx, &nibble)))
        return false;
    *res = ((*res + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return true;

    if (unlikely(!read_nibble(ctx, &nibble)))
        return false;
    *res = ((*res + 1) << 3) | (nibble & 0x7u);
    if (nibble >= 0x8u)
        return true;

    if (unlikely(!read_nibble(ctx, &nibble)))
        return false;
    *res = ((*res + 1) << 3) | (nibble & 0x7u);
    return true;
}

static bool cpy_literal(salz_decode_ctx *ctx)
{
    if (unlikely(ctx->src_pos == ctx->src_len || ctx->dst_pos == ctx->dst_len))
        return false;

    ctx->dst[ctx->dst_pos++] = ctx->src[ctx->src_pos++];

    return true;
}

static const int inc1[8] = { 0, 1, 2, 1, 4, 4, 4, 4 };
static const int inc2[8] = { 0, 1, 2, 2, 4, 3, 2, 1 };

static bool read_factor_offs(salz_decode_ctx *ctx, uint32_t *res)
{
    uint32_t var;
    uint8_t fixed;

    if (unlikely(!read_vnibble(ctx, &var)))
        return false;
    if (unlikely(!read_u8(ctx, &fixed)))
        return false;

    *res = ((var << 8) | fixed) + min_factor_offs;

    return true;
}

static bool read_factor_len(salz_decode_ctx *ctx, uint32_t *res)
{
    if (unlikely(!read_gr3(ctx, res)))
        return false;

    *res += min_factor_len;

    return true;
}

static bool cpy_factor(salz_decode_ctx *ctx)
{
    uint32_t factor_offs;
    uint32_t factor_len;
    uint8_t *src;
    uint8_t *dst;
    uint8_t *end;

    if (unlikely(!read_factor_offs(ctx, &factor_offs)))
        return false;
    if (unlikely(!read_factor_len(ctx, &factor_len)))
        return false;

    if (unlikely(ctx->dst_pos + factor_len > ctx->dst_len))
        return false;

    dst = &ctx->dst[ctx->dst_pos];
    src = dst - factor_offs;
    end = dst + factor_len;

    if (factor_offs < 8) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        salz_memcpy(&dst[4], &src[inc1[factor_offs]], 4);
        src += inc2[factor_offs];
        dst += 8;
    }

    while (dst < end) {
        salz_memcpy(dst, src, 8);
        dst += 8;
        src += 8;
    }

    ctx->dst_pos += factor_len;

    return true;
}

size_t salz_decode_default(uint8_t *src, size_t src_len, uint8_t *dst,
        size_t dst_len)
{
    salz_decode_ctx *ctx = NULL;
    size_t decoded_len = 0;

    if (src == NULL || dst == NULL) {
        debug("NULL I/O buffer(s)");
        goto out;
    }

    ctx = decode_ctx_init(src, src_len, dst, dst_len);
    if (ctx == NULL) {
        debug("Couldn't initialize decoding context");
        goto out;
    }

    if (ctx->stream_type == STREAM_TYPE_PLAIN) {
        if (!cpy_plain_stream(ctx)) {
            debug("Couldn't copy plain stream");
            goto out;
        }
    } else if (ctx->stream_type == STREAM_TYPE_SALZ) {
        while (unlikely(!stream_is_empty(ctx))) {
            uint8_t token;

            if (unlikely(!read_bit(ctx, &token))) {
                debug("Couldn't read token");
                goto out;
            }

            if (token == TOKEN_TYPE_LITERAL) {
                if (unlikely(!cpy_literal(ctx))) {
                    debug("Couldn't copy a literal");
                    goto out;
                }
            } else if (token == TOKEN_TYPE_FACTOR) {
                if (unlikely(!cpy_factor(ctx))) {
                    debug("Couldn't copy a factor");
                    goto out;
                }
            } else {
                debug("Unexpected error");
                goto out;
            }
        }
    } else {
        debug("Unexpected error");
        goto out;
    }

    decoded_len = ctx->dst_pos;

out:
    decode_ctx_fini(ctx);
    return decoded_len;
}
