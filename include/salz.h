/*
 * salz.h - Defines for SA based LZ compressor
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#ifndef SALZ_H
#define SALZ_H

#include <stdint.h>
#include <stdlib.h>

extern size_t salz_encode_default(uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_len);

#endif /* !SALZ_H */
