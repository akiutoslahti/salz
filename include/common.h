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

#include <stdint.h>
#include <time.h>

#define NS_IN_SEC 1000000000

static uint64_t get_time_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        /* @TODO externalize printing error to caller */
        //perror("clock_gettime");
        return 0;
    }

    return ts.tv_sec * NS_IN_SEC + ts.tv_nsec;
}

#endif /* !SALZ_COMMON_H */
