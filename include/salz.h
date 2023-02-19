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

#ifdef ENABLE_STATS
#include "common.h"

struct stats {
    uint64_t sa_time;
    uint64_t psv_nsv_time;
    uint64_t factor_time;
    uint64_t mincost_time;
    uint64_t encode_time;
};

extern struct stats *get_stats(void);
#endif

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
