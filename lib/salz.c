/*
 * salz.c - SA based LZ compressor
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "salz.h"

static size_t
lcp_compare(uint8_t *T, size_t T_len, size_t pos1, size_t pos2)
{
    size_t len = 0;

    while (pos2 + len < T_len && T[pos1 + len] == T[pos2 + len])
        len += 1;

    return len;
}

static void
lz_factor(uint8_t *T, size_t T_len, size_t pos, int32_t psv, int32_t nsv,
          size_t *out_pos, size_t *out_len)
{
    size_t len = 0;

    if (nsv == -1) {
        len += lcp_compare(T, T_len, psv, pos);
        *out_pos = psv;
    } else if (psv == -1) {
        len += lcp_compare(T, T_len, nsv, pos);
        *out_pos = nsv;
    } else {
        if (psv < nsv)
            len += lcp_compare(T, T_len, psv, nsv);
        else
            len += lcp_compare(T, T_len, nsv, psv);
        if (T[psv + len] == T[pos + len]) {
            len += lcp_compare(T, T_len, psv + len, pos + len);
            *out_pos = psv;
        } else {
            len += lcp_compare(T, T_len, nsv + len, pos + len);
            *out_pos = nsv;
        }
    }

    if (len == 0)
        *out_pos = T[pos];

    *out_len = len;
}

int
kkp3_factor(uint8_t *T, size_t T_len, saidx_t *SA, size_t SA_len,
            int32_t *CPSS, size_t CPSS_len)
{
    size_t top;
    size_t addr;
    size_t nfactors;
    size_t i;

    if (SA_len < T_len + 2 || CPSS_len < 2 * T_len)
        return -1;

    SA[0] = -1;
    SA[T_len + 1] = -1;

    top = 0;
    for (i = 1; i < T_len + 2; i++) {
        while (SA[top] > SA[i]) {
            addr = SA[top] << 1;
            CPSS[addr] = SA[top - 1];
            CPSS[addr + 1] = SA[i];
            top -= 1;
        }
        top += 1;
        SA[top] = SA[i];
    }

    //printf("%d %d\n", T[0], 0);
    i = 1;
    nfactors = 1;
    while (i < T_len) {
        int32_t psv;
        int32_t nsv;
        size_t len;
        size_t pos;

        addr = i << 1;
        psv = CPSS[addr];
        nsv = CPSS[addr + 1];
        lz_factor(T, T_len, i, psv, nsv, &pos, &len);
        i += len > 0 ? len : 1;
        nfactors += 1;
        //printf("%zu %zu\n", pos, len);
    }

    return nfactors;
}
