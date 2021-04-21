/*
 * salz.c - SA based LZ compressor
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#include <stdio.h>
#include <string.h>

#include "salz.h"
#include "divsufsort.h"

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

static size_t write_vbyte(uint8_t *dst, size_t pos, uint32_t value)
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

size_t salz_encode_default(uint8_t *src, size_t src_len, uint8_t *dst,
        size_t dst_len)
{
    /* @TODO destination buffer boundary checks */
    (void)dst_len;

    int32_t *sa;
    int32_t *psv_nsv;
    size_t sa_len = kkp_sa_len(src_len);
    size_t psv_nsv_len = kkp3_psv_nsv_len(src_len);
    size_t ret = 0;

    sa = malloc(sa_len * sizeof(*sa));
    psv_nsv = malloc(psv_nsv_len * sizeof(*psv_nsv));

    if (sa == NULL || psv_nsv == NULL) {
        /* @TODO add error ? */
        goto clean;
    }

    if (divsufsort(src, sa + 1, src_len) != 0) {
        /* @TODO add error ? */
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

        lz_factor(src, src_len, src_pos, psv, nsv, &factor_pos, &factor_len);

        if (factor_len < 4) {
            literals += max(1, factor_len);
            src_pos += max(1, factor_len);
        } else {
            dst_pos += write_vbyte(dst, dst_pos, literals);
            memcpy(&dst[dst_pos], &src[src_pos - literals], literals);
            dst_pos += literals;
            literals = 0;

            dst_pos += write_vbyte(dst, dst_pos, src_pos - factor_pos);
            dst_pos += write_vbyte(dst, dst_pos, factor_len);
            src_pos += factor_len;
        }
    }

    if (literals != 0) {
        dst_pos += write_vbyte(dst, dst_pos, literals);
        memcpy(&dst[dst_pos], &src[src_pos - literals], literals);
        dst_pos += literals;
    }

    ret = dst_pos;

clean:
    free(sa);
    free(psv_nsv);

    return ret;
}
