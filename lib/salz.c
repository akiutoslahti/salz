/*
 * salz.c - SA based LZ compressor
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "salz.h"
#include "divsufsort.h"

void test_libdivsufsort(void)
{
    char *T = "abracadabra";
    int n = strlen(T);
    int i, j;

    int *SA = (int *)malloc(n * sizeof(int));

    divsufsort((sauchar_t *)T, SA, n);

    for(i = 0; i < n; ++i) {
        printf("SA[%2d] = %2d: ", i, SA[i]);
        for(j = SA[i]; j < n; ++j) {
            printf("%c", T[j]);
        }
        printf("$\n");
    }

    free(SA);
}
