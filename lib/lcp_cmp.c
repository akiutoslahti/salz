/*
 * lcp_cmp.c - lcp-comparison functions
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#include <string.h>

//#define LCP_CMP_NATIVE
//#define LCP_CMP_16B
//#define LCP_CMP_32B

#if (defined(LCP_CMP_NATIVE) && (defined(LCP_CMP_16B) || defined(LCP_CMP_32B))) || \
    (defined(LCP_CMP_16B) && (defined(LCP_CMP_NATIVE) || defined(LCP_CMP_32B))) || \
    (defined(LCP_CMP_32B) && (defined(LCP_CMP_NATIVE) || defined(LCP_CMP_16B)))
#   error Defining multiple word sizes is not allowed!
#endif

#if defined(LCP_CMP_16B)
#   include <emmintrin.h>
#endif

#if defined(LCP_CMP_32B)
#   include <immintrin.h>
#endif

#include "lcp_cmp.h"

#define swap(a_, b_) do { \
        unsigned char swap_[                                            \
            sizeof((a_)) == sizeof((b_)) ? (signed)sizeof((a_)) : -1];  \
        memcpy(swap_, &(b_), sizeof((a_)));                             \
        memcpy(&(b_), &(a_), sizeof((a_)));                             \
        memcpy(&(a_), swap_, sizeof((a_)));                             \
    } while(0)

/**
 * Read platform native word from byte stream
 *
 * @param src             Byte stream to read from
 * @param pos             Position to read from
 *
 * @return unsigned long  Value read from the byte stream
 *
 * The function does NOT peform check for violating buffer boundary. Instead,
 * the caller of this function is responsible for that.
 */
static inline unsigned long
read_ulong(uint8_t *src, size_t pos)
{
    unsigned long val;
    memcpy(&val, &src[pos], sizeof(val));
    return val;
}

size_t
lcp_cmp_single(uint8_t *text, size_t text_len, size_t pos1,
        size_t pos2, size_t common_len)
{
    size_t len = common_len;

    if (pos1 > pos2)
        swap(pos1, pos2);

#if defined(LCP_CMP_NATIVE)
    while (pos2 + len <= text_len - sizeof(unsigned long)) {
        unsigned long val1 = read_ulong(text, pos1 + len);
        unsigned long val2 = read_ulong(text, pos2 + len);
        unsigned long diff = val1 ^ val2;

        if (diff != 0)
            return len + (__builtin_ctzl(diff) >> 3);

        len += sizeof(unsigned long);
    }
#elif defined(LCP_CMP_16B)
    while (pos2 + len <= text_len - 16) {
        __m128i val1 = _mm_loadu_si128((void *)&text[pos1 + len]);
        __m128i val2 = _mm_loadu_si128((void *)&text[pos2 + len]);
        __m128i cmp = _mm_cmpeq_epi8(val1, val2);
        int cmpmask = _mm_movemask_epi8(cmp);
        int diff = ~cmpmask & 0x0000ffff;

        if (diff != 0)
            return len + __builtin_ctz(diff);

        len += 16;
    }
#elif defined(LCP_CMP_32B)
    while (pos2 + len <= text_len - 32) {
        __m256i val1 = _mm256_loadu_si256((void *)&text[pos1 + len]);
        __m256i val2 = _mm256_loadu_si256((void *)&text[pos2 + len]);
        __m256i cmp = _mm256_cmpeq_epi8(val1, val2);
        int cmpmask = _mm256_movemask_epi8(cmp);
        int diff = ~cmpmask;

        if (diff != 0)
            return len + __builtin_ctz(diff);

        len += 32;
    }
#endif

    while (pos2 + len < text_len && text[pos1 + len] == text[pos2 + len])
        len += 1;

    return len;
}

size_t
lcp_cmp_dual(uint8_t *text1, size_t text1_len, uint8_t *text2,
        size_t text2_len, size_t pos1, size_t pos2, size_t common_len)
{
    size_t len = common_len;

#if defined(LCP_CMP_NATIVE)
    while (pos1 + len <= text1_len - sizeof(unsigned long) &&
           pos2 + len <= text2_len - sizeof(unsigned long)) {
        unsigned long val1 = read_ulong(text1, pos1 + len);
        unsigned long val2 = read_ulong(text2, pos2 + len);
        unsigned long diff = val1 ^ val2;

        if (diff != 0)
            return len + (__builtin_ctzl(diff) >> 3);

        len += sizeof(unsigned long);
    }
#elif defined(LCP_CMP_16B)
    while (pos1 + len <= text1_len - 16 && pos2 + len <= text2_len - 16) {
        __m128i val1 = _mm_loadu_si128((void *)&text1[pos1 + len]);
        __m128i val2 = _mm_loadu_si128((void *)&text2[pos2 + len]);
        __m128i cmp = _mm_cmpeq_epi8(val1, val2);
        int cmpmask = _mm_movemask_epi8(cmp);
        int diff = ~cmpmask & 0x0000ffff;

        if (diff != 0)
            return len + __builtin_ctz(diff);

        len += 16;
    }
#elif defined(LCP_CMP_32B)
    while (pos1 + len <= text1_len - 32 && pos2 + len <= text2_len - 32) {
        __m256i val1 = _mm256_loadu_si256((void *)&text1[pos1 + len]);
        __m256i val2 = _mm256_loadu_si256((void *)&text2[pos2 + len]);
        __m256i cmp = _mm256_cmpeq_epi8(val1, val2);
        int cmpmask = _mm256_movemask_epi8(cmp);
        int diff = ~cmpmask;

        if (diff != 0)
            return len + __builtin_ctz(diff);

        len += 32;
    }
#endif

    while (pos1 + len < text1_len &&
           pos2 + len < text2_len &&
           text1[pos1 + len] == text2[pos2 + len]) {
        len += 1;
    }

    return len;
}
