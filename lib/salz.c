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
#   define debug(...)                         \
        do {                                \
            fprintf(stderr, "(%s:%d) - ",   \
                    __func__, __LINE__);    \
            fprintf(stderr, __VA_ARGS__);   \
            fprintf(stderr, "\n");          \
        } while (0);
#endif

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) < (b)) ? (b) : (a))

static size_t kkp_sa_len(size_t text_len)
{
    return text_len + 2;
}

__attribute__((unused)) static size_t kkp2_phi_len(size_t text_len)
{
    return text_len + 1;
}

static size_t kkp3_psv_nsv_len(size_t text_len)
{
    return 2 * text_len;
}

__attribute__((unused)) static size_t write_vbyte(uint8_t *dst, size_t pos,
        uint32_t value)
{
    size_t orig_pos = pos;

    while (value > 0x7f) {
        dst[pos] = value & 0x7f;
        pos += 1;
        value >>= 7;
    }

    dst[pos] = value | 0x80;

    return pos - orig_pos + 1;
}

__attribute__((unused)) static size_t read_vbyte(uint8_t *src, size_t pos, uint32_t *res)
{
    uint32_t value = 0;
    size_t vbyte_len = 0;

    while (src[pos] < 0x80) {
        value += src[pos] << (7 * vbyte_len);
        pos += 1;
        vbyte_len += 1;
    }

    value += (src[pos] & 0x7f) << (7 * vbyte_len);
    *res = value;

    return vbyte_len + 1;
}

static size_t lcp_compare(uint8_t *text, size_t text_len, size_t pos1,
        size_t pos2)
{
    size_t len = 0;

    /* @TODO this is inefficient, do something to it */
    while (pos2 + len < text_len && text[pos1 + len] == text[pos2 + len])
        len += 1;

    return len;
}

static void lz_factor(uint8_t *text, size_t text_len, size_t pos, int32_t psv,
        int32_t nsv, uint32_t *out_pos, uint32_t *out_len)
{
    size_t len = 0;

    /* @TODO this is ugly, do something to it */
    if (nsv == -1 && psv == -1) {
    } else if (nsv == -1) {
        len += lcp_compare(text, text_len, psv, pos);
        *out_pos = psv;
    } else if (psv == -1) {
        len += lcp_compare(text, text_len, nsv, pos);
        *out_pos = nsv;
    } else {
        if (psv < nsv)
            len += lcp_compare(text, text_len, psv, nsv);
        else
            len += lcp_compare(text, text_len, nsv, psv);
        if (text[psv + len] == text[pos + len]) {
            len += lcp_compare(text, text_len, psv + len, pos + len);
            *out_pos = psv;
        } else {
            len += lcp_compare(text, text_len, nsv + len, pos + len);
            *out_pos = nsv;
        }
    }

    if (len == 0)
        *out_pos = text[pos];

    *out_len = len;
}

uint32_t salz_encode_default(uint8_t *src, size_t src_len, uint8_t *dst,
        size_t dst_len)
{
    /* @TODO add buffer boundary checks for memsafety */
    (void)dst_len;

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
                uint32_t tmp = literals - 15;
                while (tmp >= 255) {
                    assert(dst_pos < dst_len);
                    dst[dst_pos] = 255;
                    dst_pos += 1;
                    tmp -= 255;
                }
                assert(dst_pos < dst_len);
                dst[dst_pos] = tmp;
                dst_pos += 1;
            }

            size_t copy_pos = src_pos - literals;
            assert(dst_pos + literals - 1 < dst_len);
            memcpy(&dst[dst_pos], &src[copy_pos], literals);
            dst_pos += literals;

            uint16_t factor_offs = src_pos - factor_pos;
            assert(factor_offs <= src_pos);
            assert(dst_pos + sizeof(factor_offs) - 1 < dst_len);
            memcpy(&dst[dst_pos], &factor_offs, sizeof(factor_offs));
            dst_pos += sizeof(factor_offs);

            if (factor_len - 4 >= 15) {
                uint32_t tmp = factor_len - 15 - 4;
                while (tmp >= 255) {
                    assert(dst_pos < dst_len);
                    dst[dst_pos] = 255;
                    dst_pos += 1;
                    tmp -= 255;
                }
                assert(dst_pos < dst_len);
                dst[dst_pos] = tmp;
                dst_pos += 1;
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
            uint32_t tmp = literals - 15;
            while (tmp >= 255) {
                assert(dst_pos < dst_len);
                dst[dst_pos] = 255;
                dst_pos += 1;
                tmp -= 255;
            }
            assert(dst_pos < dst_len);
            dst[dst_pos] = tmp;
            dst_pos += 1;
        }

        size_t copy_pos = src_pos - literals;
        assert(dst_pos + literals - 1 < dst_len);
        memcpy(&dst[dst_pos], &src[copy_pos], literals);
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
    /* @TODO add buffer boundary checks for memsafety */
    (void)dst_len;

    size_t src_pos = 0;
    size_t dst_pos = 0;

    while (src_pos < src_len) {
        assert(src_pos < src_len);
        uint8_t token = src[src_pos];
        src_pos += 1;

        uint32_t literals = token >> 4;
        if (literals == 0xf) {
            uint8_t read_more;
            do {
                assert(src_pos < src_len);
                read_more = src[src_pos];
                src_pos += 1;
                literals += read_more;
            } while (read_more == 0xff);
        }

        assert(dst_pos + literals - 1 < dst_len);
        assert(src_pos + literals - 1 < src_len);
        memcpy(&dst[dst_pos], &src[src_pos], literals);
        dst_pos += literals;
        src_pos += literals;

        if (src_pos == src_len)
            break;

        uint16_t factor_offs;
        assert(src_pos + sizeof(factor_offs) - 1 < src_len);
        memcpy(&factor_offs, &src[src_pos], sizeof(factor_offs));
        src_pos += sizeof(factor_offs);

        uint32_t factor_len = token & 0xf;
        if (factor_len == 0xf) {
            uint8_t read_more;
            do {
                assert(src_pos < src_len);
                read_more = src[src_pos];
                src_pos += 1;
                factor_len += read_more;
            } while (read_more == 0xff);
        }
        factor_len += 4;

        size_t copy_pos = dst_pos - factor_offs;
        assert(dst_pos + factor_len - 1 < dst_len);
        while (factor_len > 0) {
            dst[dst_pos] = dst[copy_pos];
            dst_pos += 1;
            copy_pos += 1;
            factor_len -= 1;
        }
    }

    return (uint32_t)dst_pos;
}
