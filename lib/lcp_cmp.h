/*
 * lcp_cmp.h - Definitions for lcp-comparison functions
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#include <stdlib.h>
#include <stdint.h>

/**
 * Find longest common prefix (lcp) of two positions in a single text
 *
 * @param text        Text (byte alphabet) utilized in comparison
 * @param text_len    Length of @p text in bytes
 * @param pos1        First position in @p text to compare
 * @param pos2        Second position in @p text to compare
 * @param common_len  Known lower boundary for lcp
 *
 * Use this function for performing comparison on a single text as it is
 * significantly faster than lcp_cmp_dual() due to reduced number of
 * buffer boundary checks.
 */
extern size_t lcp_cmp_single(uint8_t *text, size_t text_len, size_t pos1,
        size_t pos2, size_t common_len);

/**
 * Find longest common prefix (lcp) of two positions of different texts
 *
 * @param text1        First text (byte alphabet) utilized in comparison
 * @param text1_len    Length of @p text in bytes
 * @param text2        Second text (byte alphabet) utilized in comparison
 * @param text2_len    Length of @p text in bytes
 * @param pos1         Position in @p text1 to compare
 * @param pos2         Position in @p text2 to compare
 * @param common_len   Known lower boundary for lcp
 */
extern size_t lcp_cmp_dual(uint8_t *text1, size_t text1_len, uint8_t *text2,
        size_t text2_len, size_t pos1, size_t pos2, size_t common_len);
