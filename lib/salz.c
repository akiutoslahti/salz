/*
 * salz.c - SA based LZ compressor
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

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
        } while (0);
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
#endif

static inline size_t kkp_sa_len(size_t text_len)
{
    return text_len + 2;
}

static inline size_t kkp2_phi_len(size_t text_len)
{
    return text_len + 1;
}

static inline size_t kkp3_psv_nsv_len(size_t text_len)
{
    return 2 * text_len;
}

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
    *res = ((p[4] & 0x7fu) << 21) | *res;
    return 5;
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

static inline size_t lcp_cmp(uint8_t *text, size_t text_len, size_t pos1,
        size_t pos2)
{
    size_t len = 0;

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

static inline void lz_factor(uint8_t *text, size_t text_len, size_t text_pos,
        int32_t psv, int32_t nsv, uint32_t *out_pos, uint32_t *out_len)
{
    size_t factor_pos = text[text_pos];
    size_t factor_len = 0;

    if (nsv != psv) {
        if (nsv == -1) {
            factor_pos = psv;
            factor_len += lcp_cmp(text, text_len, psv, text_pos);
        } else if (psv == -1) {
            factor_pos = nsv;
            factor_len += lcp_cmp(text, text_len, nsv, text_pos);
        } else {
            factor_len += lcp_cmp(text, text_len, min(psv, nsv), max(psv, nsv));

            if (text_pos + factor_len < text_len &&
                text[psv + factor_len] == text[text_pos + factor_len]) {
                factor_pos = psv;
                factor_len += 1;
                factor_len += lcp_cmp(text, text_len, psv + factor_len,
                                      text_pos + factor_len);
            } else {
                factor_pos = nsv;
                factor_len += lcp_cmp(text, text_len, nsv + factor_len,
                                      text_pos + factor_len);
            }
        }
    }

    *out_pos = factor_pos;
    *out_len = factor_len;
}

uint32_t salz_encode_default(uint8_t *src, size_t src_len, uint8_t *dst,
        size_t dst_len)
{
#ifdef NDEBUG
    unused(dst_len);
#endif

    int32_t *sa;
    int32_t *psv_nsv;
    size_t sa_len = kkp_sa_len(src_len);
    size_t psv_nsv_len = kkp3_psv_nsv_len(src_len);
    uint32_t ret = 0;
    uint32_t *moffs;
    uint32_t *mlens;
    size_t *costs;

    sa = malloc(sa_len * sizeof(*sa));
    psv_nsv = malloc(psv_nsv_len * sizeof(*psv_nsv));
    moffs = malloc(src_len * sizeof(*moffs));
    mlens = malloc(src_len * sizeof(*mlens));
    costs = malloc((src_len + 1) * sizeof(*costs));

    if (sa == NULL || psv_nsv == NULL) {
        debug("Memory allocation failed for SA or NSV/PSV");
        goto clean;
    }

    if (moffs == NULL || mlens == NULL || costs == NULL) {
        debug("Memory allocation failed for match descriptors");
        goto clean;
    }

#ifdef ENABLE_STATS
    uint64_t clock = get_time_ns();
#endif

    if (divsufsort(src, sa + 1, src_len) != 0) {
        debug("divsufsort failed");
        goto clean;
    }

#ifdef ENABLE_STATS
    st.sa_time += get_time_ns() - clock;
    clock = get_time_ns();
#endif

    sa[0] = -1;
    sa[src_len + 1] = -1;
    for (size_t top = 0, i = 1; i < sa_len; i++) {
        while (sa[top] > sa[i]) {
            size_t addr = sa[top] << 1;
            psv_nsv[addr] = sa[top - 1];
            psv_nsv[addr + 1] = sa[i];
            top -= 1;
        }
        top += 1;
        sa[top] = sa[i];
    }

#ifdef ENABLE_STATS
    st.psv_nsv_time += get_time_ns() - clock;
    clock = get_time_ns();
#endif

    size_t n = src_len;

    costs[n] = 0;
    costs[n - 1] = 1;
    costs[n - 2] = 2;
    costs[n - 3] = 3;

    mlens[n - 1] = 1;
    mlens[n - 2] = 1;
    mlens[n - 3] = 1;
    mlens[0] = 1;

    size_t literal_cost_inc = 15;
    for (size_t i = n - 4; i != 0; i--) {
        size_t literal_cost = 1 + costs[i + 1];
        literal_cost_inc -= 1;

        if (literal_cost_inc == 0) {
            literal_cost += 1;
            literal_cost_inc = 255;
        }

        int32_t psv = psv_nsv[2 * i];
        int32_t nsv = psv_nsv[1 + 2 * i];
        uint32_t factor_pos;
        uint32_t factor_len;

        lz_factor(src, src_len, i, psv, nsv, &factor_pos, &factor_len);

        if (factor_len < 4) {
            costs[i] = literal_cost;
            mlens[i] = 1;
        } else {
            uint32_t factor_offs = i - factor_pos;
            size_t factor_cost = 1 + vbyte_size(factor_offs) +
                                 divup(factor_len - 18, 255) +
                                 costs[i + factor_len];

            if (literal_cost <= factor_cost) {
                costs[i] = literal_cost;
                mlens[i] = 1;
            } else {
                costs[i] = factor_cost;
                moffs[i] = i - factor_pos;
                mlens[i] = factor_len;
                literal_cost_inc = 15;
            }
        }
    }

#ifdef ENABLE_STATS
    st.mincost_time += get_time_ns() - clock;
    clock = get_time_ns();
#endif

    uint32_t literals = 0;
    size_t src_pos = 0;
    size_t dst_pos = 0;
    while (src_pos < src_len) {
        if (mlens[src_pos] == 1) {
            literals += 1;
            src_pos += 1;
        } else {
            uint32_t factor_len = mlens[src_pos];
            uint8_t token = (min(literals, 15) << 4) | min(factor_len - 4, 15);
            assert(dst_pos < dst_len);
            dst[dst_pos] = token;
            dst_pos += 1;

            if (literals >= 15)
                dst_pos += write_lsic(dst, dst_len, dst_pos, literals - 15);

            size_t copy_pos = src_pos - literals;
            assert(dst_pos + literals - 1 < dst_len);
            copy(src, copy_pos, dst, dst_pos, literals);
            dst_pos += literals;

            uint32_t factor_offs = moffs[src_pos];
            assert(factor_offs <= src_pos);
            dst_pos += write_vbyte(dst, dst_len, dst_pos, factor_offs);

            if (factor_len - 4 >= 15)
                dst_pos += write_lsic(dst, dst_len, dst_pos, factor_len - 15 - 4);

            literals = 0;
            src_pos += factor_len;
        }
    }

    if (literals != 0) {
        uint8_t token = min(literals, 0xf) << 4;
        assert(dst_pos < dst_len);
        dst[dst_pos] = token;
        dst_pos += 1;

        if (literals >= 15) {
            dst_pos += write_lsic(dst, dst_len, dst_pos, literals - 15);
        }

        size_t copy_pos = src_pos - literals;
        assert(dst_pos + literals - 1 < dst_len);
        copy(src, copy_pos, dst, dst_pos, literals);
        dst_pos += literals;
    }

#ifdef ENABLE_STATS
    st.encode_time += get_time_ns() - clock;
#endif

    ret = (uint32_t)dst_pos;

clean:
    free(sa);
    free(psv_nsv);
    free(moffs);
    free(mlens);
    free(costs);

    return ret;
}


uint32_t salz_decode_default(uint8_t *src, size_t src_len, uint8_t *dst,
        size_t dst_len)
{
#ifdef NDEBUG
    unused(dst_len);
#endif

    size_t src_pos = 0;
    size_t dst_pos = 0;

    while (src_pos < src_len) {
        uint8_t token = src[src_pos];
        src_pos += 1;

        uint32_t literals = token >> 4;
        if (literals == 0xf) {
            uint32_t extra;
            src_pos += read_lsic(src, src_len, src_pos, &extra);
            literals += extra;
        }

        assert(dst_pos + literals - 1 < dst_len);
        assert(src_pos + literals - 1 < src_len);
        copy(src, src_pos, dst, dst_pos, literals);
        dst_pos += literals;
        src_pos += literals;

        if (src_pos == src_len)
            break;

        uint32_t factor_offs;
        src_pos += read_vbyte(src, src_len, src_pos, &factor_offs);

        uint32_t factor_len = token & 0xf;
        if (factor_len == 0xf) {
            uint32_t extra;
            src_pos += read_lsic(src, src_len, src_pos, &extra);
            factor_len += extra;
        }
        factor_len += 4;

        assert(dst_pos + factor_len - 1 < dst_len);
        assert(factor_offs <= dst_pos);
        copy_oaat(dst, dst_pos - factor_offs, dst, dst_pos, factor_len);
        dst_pos += factor_len;
    }

    return (uint32_t)dst_pos;
}
