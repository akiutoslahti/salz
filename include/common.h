/*
 * common.h - Common definitions and functions
 *
 * Copyright (c) 2021-2023 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#ifndef SALZ_COMMON_H
#define SALZ_COMMON_H

#include <errno.h>
#include <stdint.h>
#include <time.h>

#include "common.h"

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define divup(a, b) (((a) + (b) - 1) / (b))
#define roundup(a, b) (divup(a, b) * b)

#define unlikely(x) __builtin_expect((x),0)

#define unused(x) ((void)(x))

#define NS_IN_SEC (1 * 1000 * 1000 * 1000)
static inline int get_time_ns(uint64_t *res)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return errno;

    *res = ts.tv_sec * NS_IN_SEC + ts.tv_nsec;

    return 0;
}

#endif /* !SALZ_COMMON_H */
