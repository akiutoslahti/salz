/*
 * salz.h - Definitions for SA based LZ compressor
 *
 * Copyright (c) 2021-2023 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#ifndef SALZ_H
#define SALZ_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

/*
 * Get worst case length for encoded segment
 *
 * @param[in]  plain_len  Length of segment (in bytes)
 *
 * @return                Worst case length for encoded segment
 */
static inline int salz_encoded_len_max(size_t plain_len)
{
    return 4 + plain_len + roundup(plain_len, 64) / 8;
}

/*
 * Encode plain segment with SALZ
 *
 * @param[in]     src      Plain segment to encode with SALZ
 * @param[in]     src_len  Length of @p src (in bytes)
 * @param[in]     dst      Preallocated space for encoded segment
 * @param[in/out] dst_len  Space available in @p dst (in bytes) [in]
 *                         Length of encoded segment (in bytes) [out]
 *
 * @return                 0, if successful
 *                         -1, otherwise
 */
extern int salz_encode_safe(const uint8_t *src, size_t src_len,
    uint8_t *dst, size_t *dst_len);

/*
 * Decode SALZ encoded segment
 *
 * @param[in]     src      SALZ encoded segment to decode
 * @param[in]     src_len  Length of @p src (in bytes)
 * @param[in]     dst      Preallocated space for decoded segment
 * @param[in/out] dst_len  Space available in @p dst (in bytes) [in]
 *                         Length of decoded segment (in bytes) [out]
 *
 * @return                 0, if successful
 *                         -1, otherwise
 */
extern int salz_decode_safe(const uint8_t *src, size_t src_len,
    uint8_t *dst, size_t *dst_len);

#endif /* !SALZ_H */
