/*
 * vlc.c - Variable-Length Codes used in SALZ
 *
 * Copyright (c) 2021-2023 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#include "vlc.h"

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
