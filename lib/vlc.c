/*
 * vlc.c - Variable-Length Codes used in SALZ
 *
 * Copyright (c) 2021-2023 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#include <stddef.h>
#include <stdint.h>

#include "vlc.h"

size_t vbyte_size(uint32_t val)
{
    if (val < 128)
        return 1;

    if (val < 16512)
        return 2;

    if (val < 2113664)
        return 3;

    if (val < 270549120)
        return 4;

    return 5;
}

size_t encode_vbyte_be(uint32_t val, uint64_t *res)
{
    uint8_t *p = (uint8_t *)res;

    if  (val < 128) {
        p[0] = val | 0x80u;
        return 1;
    }

    if (val < 16512) {
        p[0] = (val - 128) >> 7;
        p[1] = (val & 0x7fu) | 0x80u;
        return 2;
    }

    if (val < 2113664) {
        p[0] = (val - 16512) >> 14;
        p[1] = ((val - 128) >> 7) & 0x7fu;
        p[2] = (val & 0x7fu) | 0x80u;
        return 3;
    }

    if (val < 270549120) {
        p[0] = (val - 2113664) >> 21;
        p[1] = ((val - 16512) >> 14) & 0x7fu;
        p[2] = ((val - 128) >> 7) & 0x7fu;
        p[3] = (val & 0x7fu) | 0x80u;
        return 4;
    }

    p[0] = (val - 270549120) >> 28;
    p[1] = ((val - 2113664) >> 21) & 0x7fu;
    p[2] = ((val - 16512) >> 14) & 0x7fu;
    p[3] = ((val - 128) >> 7) & 0x7fu;
    p[4] = (val & 0x7fu) | 0x80u;
    return 5;
}
