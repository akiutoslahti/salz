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

#include "divsufsort.h"

extern int kkp2_factor(uint8_t *T, size_t T_len, saidx_t *SA, size_t SA_len,
                       int32_t *phi, size_t phi_len);

extern int kkp3_factor(uint8_t *T, size_t T_len, saidx_t *SA, size_t SA_len,
                       int32_t *CPSS, size_t CPSS_len);

#endif /* !SALZ_H */
