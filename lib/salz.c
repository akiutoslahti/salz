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
#       define assert(condition) ((void)0)
#   endif
#   define debug(...) do {} while(0);
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

static inline void copy_baat(uint8_t *src, size_t src_pos, uint8_t *dst,
        size_t dst_pos, size_t copy_len)
{
    while (copy_len > 0) {
        dst[dst_pos] = src[src_pos];
        dst_pos += 1;
        src_pos += 1;
        copy_len -= 1;
    }
}

static inline size_t write_vbyte(uint8_t *dst, size_t dst_len, size_t pos,
        uint32_t val)
{
#ifdef NDEBUG
    (void)dst_len;
#endif

    size_t orig_pos = pos;

    while (val > 0x7f) {
        assert(pos < dst_len);
        dst[pos] = val & 0x7f;
        pos += 1;
        val >>= 7;
    }

    assert(pos < dst_len);
    dst[pos] = val | 0x80;

    return pos - orig_pos + 1;
}

static inline size_t read_vbyte(uint8_t *src, size_t src_len, size_t pos,
        uint32_t *res)
{
#ifdef NDEBUG
    (void)src_len;
#endif

    uint32_t val = 0;
    size_t vbyte_len = 0;

    while (src[pos] < 0x80) {
        assert(pos < src_len);
        val += src[pos] << (7 * vbyte_len);
        pos += 1;
        vbyte_len += 1;
    }

    assert(pos < src_len);
    val += (src[pos] & 0x7f) << (7 * vbyte_len);
    *res = val;

    return vbyte_len + 1;
}

static inline size_t write_lsic(uint8_t *dst, size_t dst_len, size_t pos,
        uint32_t val)
{
#ifdef NDEBUG
    (void)dst_len;
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
    (void)src_len;
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
    (void)dst_len;
#endif

    int32_t *sa;
    int32_t *psv_nsv;
    size_t sa_len = kkp_sa_len(src_len);
    size_t psv_nsv_len = kkp3_psv_nsv_len(src_len);
    uint32_t ret = 0;

    sa = malloc(sa_len * sizeof(*sa));
    psv_nsv = malloc(psv_nsv_len * sizeof(*psv_nsv));

    if (sa == NULL || psv_nsv == NULL) {
        debug("Memory allocation failed for SA or NSV/PSV");
        goto clean;
    }

    if (divsufsort(src, sa + 1, src_len) != 0) {
        debug("divsufsort failed");
        goto clean;
    }

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

    uint32_t literals = 1;
    size_t src_pos = 1;
    size_t dst_pos = 0;
    while (src_pos < src_len) {
        size_t addr = src_pos << 1;
        int32_t psv = psv_nsv[addr];
        int32_t nsv = psv_nsv[addr + 1];
        uint32_t factor_pos;
        uint32_t factor_len;

        /* Limit factor offset */
        psv = src_pos - psv < 65536 ? psv : -1;
        nsv = src_pos - nsv < 65536 ? nsv : -1;
        lz_factor(src, src_len, src_pos, psv, nsv, &factor_pos, &factor_len);

        if (factor_len < 4) {
            literals += max(1, factor_len);
            src_pos += max(1, factor_len);
        } else {
            uint8_t token = (min(literals, 15) << 4) | min(factor_len - 4, 15);
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

            uint16_t factor_offs = src_pos - factor_pos;
            assert(factor_offs <= src_pos);
            assert(dst_pos + sizeof(factor_offs) - 1 < dst_len);
            write_u16(dst, dst_pos, factor_offs);
            dst_pos += sizeof(factor_offs);

            if (factor_len - 4 >= 15) {
                dst_pos += write_lsic(dst, dst_len, dst_pos, factor_len - 15 - 4);
            }

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

    ret = (uint32_t)dst_pos;

clean:
    free(sa);
    free(psv_nsv);

    return ret;
}


uint32_t salz_decode_default(uint8_t *src, size_t src_len, uint8_t *dst,
        size_t dst_len)
{
#ifdef NDEBUG
    (void)dst_len;
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

        uint16_t factor_offs;
        assert(src_pos + sizeof(factor_offs) - 1 < src_len);
        factor_offs = read_u16(src, src_pos);
        src_pos += sizeof(factor_offs);

        uint32_t factor_len = token & 0xf;
        if (factor_len == 0xf) {
            uint32_t extra;
            src_pos += read_lsic(src, src_len, src_pos, &extra);
            factor_len += extra;
        }
        factor_len += 4;

        assert(dst_pos + factor_len - 1 < dst_len);
        assert(factor_offs <= dst_pos);
        copy_baat(dst, dst_pos - factor_offs, dst, dst_pos, factor_len);
        dst_pos += factor_len;
    }

    return (uint32_t)dst_pos;
}
