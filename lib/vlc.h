/*
 * vlc.h - Definitions for Variable-Length Codes used in SALZ
 *
 * Copyright (c) 2021-2023 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#ifndef VLC_H
#define VLC_H

#include <stddef.h>
#include <stdint.h>

extern size_t vbyte_size(uint32_t val);
extern size_t encode_vbyte_be(uint32_t val, uint64_t *res);

#endif /* !VLC_H */
