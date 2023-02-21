/*
 * salz.c - SA based LZ compressor
 *
 * Copyright (c) 2021-2023 Aki Utoslahti. All rights reserved.
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
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "salz.h"
#include "libsais.h"

#ifdef NDEBUG
#   define debug(fmt, ...) do {} while(0)
#else
#   include <stdio.h>
#   define debug(fmt, ...) \
        do { \
            fprintf(stderr, "(%s:%d) - " fmt "\n", \
                    __func__, __LINE__, ## __VA_ARGS__); \
        } while (0)
#endif

#ifdef __GNUC__
#   define salz_memcpy(dst, src, n) __builtin_memcpy(dst, src, n)
#else
#   define salz_memcpy(dst, src, n) memcpy(dst, src, n)
#endif

enum salz_stream_type {
    SALZ_STREAM_TYPE_PLAIN = 0,
    SALZ_STREAM_TYPE_SALZ,
    SALZ_STREAM_TYPE_MAX,
};

#define SALZ_TOKEN_TYPE_LITERAL (0)
#define SALZ_TOKEN_TYPE_FACTOR  (1)

/* SALZ I/O context */
struct salz_io_ctx {
    /* Common members */

    /* Input buffer */
    const uint8_t *src;
    /* Length of input buffer (in bytes) */
    size_t src_len;
    /* Current position in input buffer */
    size_t src_pos;

    /* Output buffer */
    uint8_t *dst;
    /* Length of output buffer (in bytes) */
    size_t dst_len;
    /* Current position in output buffer */
    size_t dst_pos;

    /* Buffered bitfield (interleaved synchronously with compressed stream) */
    uint64_t bits;
    /* Bits currently available for writing in buffered bitfield */
    size_t bits_avail;

    /* Context-bound members */
    union {
        /* Encoding-only members */
        struct {
            /* Output buffer position reserved for current buffered bitfield */
            size_t bits_pos;

            /* Suffix array */
            int32_t *sa;
            /* Length of suffix array */
            size_t sa_len;
            /* Auxiliary array */
            int32_t *aux;
            /* Length of auxiliary array */
            size_t aux_len;

            /* Previous PSV value */
            int32_t prev_psv;
            /* Matching length associated with previous PSV value */
            size_t prev_psv_len;
            /* Previous NSV value */
            int32_t prev_nsv;
            /* Matching length associated with previous NSV value */
            size_t prev_nsv_len;
        };

        /* Decoding-only members */
        struct {
            /* Type of SALZ encoded stream */
            uint8_t stream_type;
        };
    };
};

typedef struct salz_io_ctx salz_io_ctx;

/*******************
 * Raw I/O functions
 *******************/

static void write_u32_raw(uint8_t *buf, size_t pos, uint32_t val)
{
    salz_memcpy(&buf[pos], &val, sizeof(val));
}

static void write_u64_raw(uint8_t *buf, size_t pos, uint64_t val)
{
    salz_memcpy(&buf[pos], &val, sizeof(val));
}

static uint64_t read_u32_raw(const uint8_t *buf, size_t pos)
{
    uint32_t val;

    salz_memcpy(&val, &buf[pos], sizeof(val));

    return val;
}

static uint64_t read_u64_raw(const uint8_t *buf, size_t pos)
{
    uint64_t val;

    salz_memcpy(&val, &buf[pos], sizeof(val));

    return val;
}

/******************************
 * Common I/O context functions
 ******************************/

static bool input_processed(salz_io_ctx *ctx)
{
    assert(ctx->src_pos <= ctx->src_len);

    return ctx->src_pos == ctx->src_len;
}

static bool cpy_literal(salz_io_ctx *ctx)
{
    if (unlikely(ctx->src_pos >= ctx->src_len || ctx->dst_pos >= ctx->dst_len))
        return false;

    ctx->dst[ctx->dst_pos++] = ctx->src[ctx->src_pos++];

    return true;
}

/*************************************
 * Encoding-only I/O context functions
 *************************************/

static salz_io_ctx *encode_ctx_create(const uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len)
{
    salz_io_ctx *ctx = NULL;
    int32_t *sa = NULL;
    size_t sa_len;
    int32_t *aux = NULL;
    size_t aux_len;

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        debug("Couldn't allocate memory (%zu bytes)", sizeof(*ctx));
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));

    /*
     * @note: We prevent encoding functions from touching last 8 bytes in
     * order to guarantee safe copying of factors 8 bytes at a time.
     * This must be taken into account when finalizing the encoding, where
     * these bytes can be simply encoded as literals.
     */
    src_len -= 8;

    sa_len = src_len + 2;
    sa = calloc(sa_len, sizeof(*sa));
    if (sa == NULL) {
        debug("Couldn't allocate memory (%zu bytes)", sa_len * sizeof(*sa));
        goto fail;
    }

    aux_len = 4 * (src_len + 1);
    aux = calloc(aux_len, sizeof(*aux));
    if (sa == NULL) {
        debug("Couldn't allocate memory (%zu bytes)", aux_len * sizeof(*aux));
        goto fail;
    }

    ctx->src = src;
    ctx->src_len = src_len;
    ctx->src_pos = 0;

    ctx->dst = dst;
    ctx->dst_len = dst_len;
    if (dst_len < 4) {
        debug("Couldn't reserve space for stream header");
        goto fail;
    }
    ctx->dst_pos = 4;

    ctx->bits = 0;
    ctx->bits_avail = 0;
    ctx->bits_pos = 0;

    ctx->sa = sa;
    ctx->sa_len = sa_len;
    ctx->aux = aux;
    ctx->aux_len = aux_len;

    ctx->prev_psv = -1;
    ctx->prev_psv_len = 0;
    ctx->prev_nsv = -1;
    ctx->prev_nsv_len = 0;

    return ctx;

fail:
    free(sa);
    free(aux);
    free(ctx);

    return NULL;
}

static void encode_ctx_destroy(salz_io_ctx *ctx)
{
    if (ctx != NULL) {
        free(ctx->sa);
        free(ctx->aux);
        free(ctx);
    }
}

static bool write_u8(salz_io_ctx *ctx, uint8_t val)
{
    if (unlikely(ctx->dst_pos >= ctx->dst_len))
        return false;

    ctx->dst[ctx->dst_pos++] = val;

    return true;
}

static bool flush_bits(salz_io_ctx *ctx)
{
    static_assert(sizeof(ctx->bits) == 8);

    write_u64_raw(ctx->dst, ctx->bits_pos, ctx->bits);

    if (unlikely(ctx->dst_pos + 8 > ctx->dst_len))
        return false;

    ctx->bits = 0;
    ctx->bits_avail = 64;
    ctx->bits_pos = ctx->dst_pos;
    ctx->dst_pos += 8;

    return true;
}

static bool write_bit(salz_io_ctx *ctx, uint8_t val)
{
    if (ctx->bits_avail == 0 && unlikely(!flush_bits(ctx)))
        return false;

    ctx->bits = (ctx->bits << 1) | (val & 1);
    ctx->bits_avail -= 1;

    return true;
}

static bool write_bits(salz_io_ctx *ctx, uint64_t bits, size_t count)
{
    if (ctx->bits_avail == 0 && unlikely(!flush_bits(ctx)))
        return false;

    if (count > ctx->bits_avail) {
        ctx->bits = (ctx->bits << ctx->bits_avail) |
                    ((bits >> (count - ctx->bits_avail)) &
                     ((1u << ctx->bits_avail) - 1));
        count -= ctx->bits_avail;

        if (unlikely(!flush_bits(ctx)))
            return false;
    }

    ctx->bits = (ctx->bits << count) | (bits & ((1u << count) - 1));
    ctx->bits_avail -= count;

    return true;
}

static bool write_zeros(salz_io_ctx *ctx, size_t count)
{
    while (count) {
        if (ctx->bits_avail == 0 && unlikely(!flush_bits(ctx)))
            return false;

        size_t write_count = min(ctx->bits_avail, count);
        ctx->bits <<= write_count;
        ctx->bits_avail -= write_count;
        count -= write_count;
    }

    return true;
}

static bool write_unary(salz_io_ctx *ctx, uint32_t val)
{
    if (unlikely(!write_zeros(ctx, val)))
        return false;
    if (unlikely(!write_bit(ctx, 1)))
        return false;

    return true;
}

static bool write_gr3(salz_io_ctx *ctx, uint32_t val)
{
    if (unlikely(!write_unary(ctx, val >> 3)))
        return false;
    if (unlikely(!write_bits(ctx, val & 0x7u, 3)))
        return false;

    return true;
}

size_t encode_vnibble_le(uint32_t val, uint64_t *res)
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

static bool write_vnibble(salz_io_ctx *ctx, uint32_t val)
{
    uint64_t nibbles;

    size_t nibbles_len = encode_vnibble_le(val, &nibbles);

    if (unlikely(!write_bits(ctx, nibbles, nibbles_len * 4)))
        return false;

    return true;
}

/********************
 * Encoding functions
 ********************/

static bool build_suffix_array(salz_io_ctx *ctx)
{
    if (libsais(ctx->src, ctx->sa + 1, ctx->src_len, 0, NULL) < 0)
        return false;

    return true;
}

static void build_psvnsv_array(salz_io_ctx *ctx)
{
    /* PSV/NSV array construction from Suffix Array as described in [2] */

    int32_t *sa = ctx->sa;
    int32_t *aux = ctx->aux;
    size_t len = ctx->src_len;

    sa[0] = -1;
    sa[len + 1] = -1;
    for (size_t top = 0, i = 1; i < len + 2; i++) {
        while (sa[top] > sa[i]) {
            aux[0 + 4 * sa[top]] = sa[top - 1]; /* PSV */
            aux[1 + 4 * sa[top]] = sa[i]; /* NSV */
            top -= 1;
        }
        top += 1;
        sa[top] = sa[i];
    }
}

static size_t lcp_cmp(salz_io_ctx *ctx, size_t common_len, size_t pos1,
    size_t pos2)
{
    assert(pos2 > pos1);

    size_t len = common_len;

    while (pos2 + len + 8 <= ctx->src_len) {
        uint64_t val1 = read_u64_raw(ctx->src, pos1 + len);
        uint64_t val2 = read_u64_raw(ctx->src, pos2 + len);
        uint64_t diff = val1 ^ val2;

        if (diff)
            return len + (__builtin_ctzll(diff) >> 3);

        len += 8;
    }

    while (pos2 + len < ctx->src_len && ctx->src[pos1 + len] == ctx->src[pos2 + len])
        len += 1;

    return len;
}

static void factorize_pos(salz_io_ctx *ctx, size_t pos, int32_t psv,
    int32_t nsv)
{
    size_t psv_len = 0;
    size_t nsv_len = 0;

    if (psv != -1) {
        /* Prevent wrap-around */
        size_t common_len = ctx->prev_psv_len + !ctx->prev_psv_len - 1;
        psv_len = lcp_cmp(ctx, common_len, psv, pos);
    }

    if (nsv != -1) {
        /* Prevent wrap-around */
        size_t common_len = ctx->prev_nsv_len + !ctx->prev_nsv_len - 1;
        nsv_len = lcp_cmp(ctx, common_len, nsv, pos);
    }

    ctx->prev_psv = psv;
    ctx->prev_psv_len = psv_len;
    ctx->prev_nsv = nsv;
    ctx->prev_nsv_len = nsv_len;
}

static void factorize(salz_io_ctx *ctx)
{
    /* Factorization of all text positions as described in Section 3.4 of [1] */

    int32_t *aux = ctx->aux;

    /* Skip factorization of first position and force it to be a literal */
    aux[1 + 4 * 0] = 1;
    aux[3 + 4 * 0] = 1;
    for (size_t pos = 1; pos < ctx->src_len; pos++) {
        int32_t psv = aux[0 + 4 * pos];
        int32_t nsv = aux[1 + 4 * pos];

        factorize_pos(ctx, pos, psv, nsv);

        aux[0 + 4 * pos] = (int32_t)(pos - ctx->prev_psv);
        aux[1 + 4 * pos] = (int32_t)ctx->prev_psv_len;
        aux[2 + 4 * pos] = (int32_t)(pos - ctx->prev_nsv);
        aux[3 + 4 * pos] = (int32_t)ctx->prev_nsv_len;
    }
}

#define FACTOR_OFFSET_MIN 1
#define FACTOR_LENGTH_MIN 3

size_t vnibble_size(uint32_t val)
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

static size_t factor_offs_bitsize(uint32_t val)
{
    return 8 + vnibble_bitsize((val - FACTOR_OFFSET_MIN) >> 8);
}

static size_t gr3_bitsize(uint32_t val)
{
    return (val >> 3) + 1 + 3;
}

static size_t factor_len_bitsize(uint32_t val)
{
    return gr3_bitsize(val - FACTOR_LENGTH_MIN);
}

static void optimize_factorization(salz_io_ctx *ctx)
{
    /*
     * Dynamic programming SSSP (Single Source Shortest Path) approach to
     * optimizing the final encoded size of the Lempel-Ziv factorization
     * of a text as described in Section 3.5.4 of [1].
     */

    int32_t *aux = ctx->aux;

    /* Nothing to do after reaching last position - initialize cost as zero */
    aux[2 + 4 * ctx->src_len] = 0;
    for (size_t src_pos = ctx->src_len - 1; src_pos; src_pos--) {
        /* Cost of using a literal */
        int32_t factor_offs = 0;
        int32_t factor_len = 1;
        int32_t cost = 9 + aux[2 + 4 * (src_pos + 1)];

        /* Cost of using PSV candidate */
        int32_t alt_len = aux[1 + 4 * src_pos];
        if (alt_len >= FACTOR_LENGTH_MIN) {
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
        if (alt_len >= FACTOR_LENGTH_MIN) {
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
}

static bool write_token(salz_io_ctx *ctx, uint8_t val)
{
    if (unlikely(!write_bit(ctx, val)))
        return false;

    return true;
}

static bool write_factor_offs(salz_io_ctx *ctx, uint32_t val)
{
    if (unlikely(!write_vnibble(ctx, (val - FACTOR_OFFSET_MIN) >> 8)))
        return false;
    if (unlikely(!write_u8(ctx, (val - FACTOR_OFFSET_MIN) & 0xffu)))
        return false;

    return true;
}

static bool write_factor_len(salz_io_ctx *ctx, uint32_t val)
{
    if (unlikely(!write_gr3(ctx, val - FACTOR_LENGTH_MIN)))
        return false;

    return true;
}

static bool write_factor(salz_io_ctx *ctx, uint32_t factor_offs,
    uint32_t factor_len)
{
    if (unlikely(!write_factor_offs(ctx, factor_offs)))
        return false;
    if (unlikely(!write_factor_len(ctx, factor_len)))
        return false;

    return true;
}

static bool emit_encoding(salz_io_ctx *ctx)
{
    /*
     * Optimal Lempel-Ziv factorization is encoded using a format described in
     * Sections 3.6.1 and 3.6.3 of [1].
     */

    int32_t *aux = ctx->aux;

    while (!input_processed(ctx)) {
        uint32_t factor_len = (uint32_t)aux[1 + 4 * ctx->src_pos];

        if (factor_len == 1) {
            if (unlikely(!write_token(ctx, SALZ_TOKEN_TYPE_LITERAL)))
                return false;
            if (unlikely(!cpy_literal(ctx)))
                return false;
        } else {
            uint32_t factor_offs = (uint32_t)aux[0 + 4 * ctx->src_pos];

            if (unlikely(!write_token(ctx, SALZ_TOKEN_TYPE_FACTOR)))
                return false;
            if (unlikely(!write_factor(ctx, factor_offs, factor_len)))
                return false;

            /* Manual adjustment of input position necessary */
            ctx->src_pos += factor_len;
        }
    }

    return true;
}

static bool finalize_encoding(salz_io_ctx *ctx)
{
    /*
     * @todo: create more substantial stream header, which contains
     * version, type, flags, size and checksum (optional?)
     */
    uint32_t stream_hdr = 0;

    /* Encode the last 8 bytes */
    ctx->src_len += 8;
    for (size_t i = 0; i < 8; i++) {
        if (unlikely(!write_token(ctx, SALZ_TOKEN_TYPE_LITERAL)))
            return false;
        if (unlikely(!cpy_literal(ctx)))
            return false;
    }

    /* Flush last bit buffer */
    ctx->bits <<= ctx->bits_avail;
    write_u64_raw(ctx->dst, ctx->bits_pos, ctx->bits);

    if (ctx->dst_pos > ctx->src_len + 4) {
        /*
         * Encoded size exceed original size. Discard encoded segment
         * and use plain input instead
         */
        stream_hdr |= SALZ_STREAM_TYPE_PLAIN << 24;
        stream_hdr |= ctx->src_len & 0xffffff;

        if (unlikely(ctx->src_len + 4 > ctx->dst_len))
            return false;

        salz_memcpy(ctx->dst + 4, ctx->src, ctx->src_len);
        ctx->dst_pos = ctx->src_len + 4;
    } else {
        stream_hdr |= SALZ_STREAM_TYPE_SALZ << 24;
        stream_hdr |= (ctx->dst_pos - 4) & 0xffffff;
    }
    write_u32_raw(ctx->dst, 0, stream_hdr);

    return true;
}

int salz_encode_safe(const uint8_t *src, size_t src_len, uint8_t *dst,
    size_t *dst_len)
{
    salz_io_ctx *ctx = NULL;
    int ret = 0;

    if (src == NULL || dst == NULL) {
        debug("NULL I/O buffer(s)");
        return -1;
    }

    ctx = encode_ctx_create(src, src_len, dst, *dst_len);
    if (ctx == NULL) {
        debug("Couldn't initialize encoding context");
        return -1;
    }

    if (!build_suffix_array(ctx)) {
        debug("Couldn't build SA");
        ret = -1;
        goto out;
    }

    build_psvnsv_array(ctx);

    factorize(ctx);

    optimize_factorization(ctx);

    if (!emit_encoding(ctx)) {
        debug("Encoding failed");
        ret = -1;
        goto out;
    }

    if (!finalize_encoding(ctx)) {
        debug("Couldn't finalize encoding");
        ret = -1;
        goto out;
    }

    *dst_len = ctx->dst_pos;

out:
    encode_ctx_destroy(ctx);
    return ret;
}

/*************************************
 * Decoding-only I/O context functions
 *************************************/

static salz_io_ctx *decode_ctx_create(const uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len)
{
    salz_io_ctx *ctx = NULL;
    uint32_t stream_hdr;
    uint8_t stream_type;
    size_t stream_len;

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        debug("Couldn't allocate memory (%zu bytes)", sizeof(*ctx));
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));

    if (src_len < 4) {
        debug("Couldn't read stream header");
        goto fail;
    }

    stream_hdr = read_u32_raw(src, 0);
    stream_type = stream_hdr >> 24;
    stream_len = stream_hdr & 0xffffff;

    if (stream_type >= SALZ_STREAM_TYPE_MAX) {
        debug("Unknown stream type (%u)", stream_type);
        goto fail;
    }

    if (stream_len > src_len - 4) {
        debug("Stream is truncated (expected: %zu, have: %zu)",
               stream_len, src_len - 4);
        goto fail;
    }

    ctx->stream_type = stream_type;
    /* Stream is repositioned right after the header, as it is no longer needed */
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

static void decode_ctx_destroy(salz_io_ctx *ctx)
{
    if (ctx != NULL)
        free(ctx);
}

static bool read_u8(salz_io_ctx *ctx, uint8_t *res)
{
    if (unlikely(ctx->src_pos >= ctx->src_len))
        return false;

    *res = ctx->src[ctx->src_pos++];

    return true;
}

static bool read_u64(salz_io_ctx *ctx, uint64_t *res)
{
    if (unlikely(ctx->src_pos + 8 > ctx->src_len))
        return false;

    salz_memcpy(res, &ctx->src[ctx->src_pos], 8);
    ctx->src_pos += 8;

    return true;
}

static bool queue_bits(salz_io_ctx *ctx)
{
    if (unlikely(!read_u64(ctx, &ctx->bits)))
        return false;

    ctx->bits_avail = 64;

    return true;
}

static bool read_bit(salz_io_ctx *ctx, uint8_t *res)
{
    if (ctx->bits_avail == 0 && unlikely(!queue_bits(ctx)))
        return false;

    *res = !!(ctx->bits & 0x8000000000000000u);
    ctx->bits <<= 1;
    ctx->bits_avail -= 1;

    return true;
}

static bool read_bits(salz_io_ctx *ctx, size_t count, uint64_t *res)
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

static bool read_unary(salz_io_ctx *ctx, uint32_t *res)
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

static bool read_gr3(salz_io_ctx *ctx, uint32_t *res)
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

static bool read_nibble(salz_io_ctx *ctx, uint8_t *res)
{
    uint64_t var;

    if (unlikely(!read_bits(ctx, 4, &var)))
        return false;

    *res = (uint8_t)var;

    return true;
}

static bool read_vnibble(salz_io_ctx *ctx, uint32_t *res)
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

/********************
 * Decoding functions
 ********************/

static bool cpy_plain_stream(salz_io_ctx *ctx)
{
    if (unlikely(ctx->src_len > ctx->dst_len))
        return false;

    salz_memcpy(ctx->dst, ctx->src, ctx->src_len);
    ctx->dst_pos = ctx->src_len;

    return true;
}

static bool read_token(salz_io_ctx *ctx, uint8_t *res)
{
    if (unlikely(!read_bit(ctx, res)))
        return false;

    return true;
}

static bool read_factor_offs(salz_io_ctx *ctx, uint32_t *res)
{
    uint32_t var;
    uint8_t fixed;

    if (unlikely(!read_vnibble(ctx, &var)))
        return false;
    if (unlikely(!read_u8(ctx, &fixed)))
        return false;

    *res = ((var << 8) | fixed) + FACTOR_OFFSET_MIN;

    return true;
}

static bool read_factor_len(salz_io_ctx *ctx, uint32_t *res)
{
    if (unlikely(!read_gr3(ctx, res)))
        return false;

    *res += FACTOR_LENGTH_MIN;

    return true;
}

static bool cpy_factor(salz_io_ctx *ctx)
{
    uint32_t factor_offs;
    uint32_t factor_len;
    uint8_t *src;
    uint8_t *dst;
    uint8_t *end;

    static const int inc1[8] = { 0, 1, 2, 1, 4, 4, 4, 4 };
    static const int inc2[8] = { 0, 1, 2, 2, 4, 3, 2, 1 };

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

static bool decode(salz_io_ctx *ctx)
{
    while (!input_processed(ctx)) {
        uint8_t token;

        if (unlikely(!read_token(ctx, &token))) {
            debug("Couldn't read token");
            return false;
        }

        if (token == SALZ_TOKEN_TYPE_LITERAL && unlikely(!cpy_literal(ctx))) {
            debug("Couldn't copy a literal");
            return false;
        }

        if (token == SALZ_TOKEN_TYPE_FACTOR && unlikely(!cpy_factor(ctx))) {
            debug("Couldn't copy a factor");
            return false;
        }
    }

    return true;
}

int salz_decode_safe(const uint8_t *src, size_t src_len, uint8_t *dst,
    size_t *dst_len)
{
    salz_io_ctx *ctx = NULL;
    int ret = 0;

    if (src == NULL || dst == NULL) {
        debug("NULL I/O buffer(s)");
        return -1;
    }

    ctx = decode_ctx_create(src, src_len, dst, *dst_len);
    if (ctx == NULL) {
        debug("Couldn't initialize decoding context");
        return -1;
    }

    if (ctx->stream_type == SALZ_STREAM_TYPE_PLAIN && !cpy_plain_stream(ctx)) {
        debug("Couldn't copy plain stream");
        ret = -1;
        goto out;
    }

    if (ctx->stream_type == SALZ_STREAM_TYPE_SALZ && !decode(ctx)) {
        debug("Decoding failed");
        ret = -1;
        goto out;
    }

    *dst_len = ctx->dst_pos;

out:
    decode_ctx_destroy(ctx);
    return ret;
}
