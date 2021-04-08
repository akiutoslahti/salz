/*
 * bench_block.c - Benchmark suffix sorting and LZ77 factorization
 * with variable block sizes.
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "common.h"
#include "salz.h"
#include "divsufsort.h"

static size_t kkp2_aux_len(size_t block_len)
{
    return block_len + 1;
}

static size_t kkp3_aux_len(size_t block_len)
{
    return 2 * block_len;
}

int
main(int argc, const char *argv[])
{
    FILE *fp;
    char *p;
    long arg_bs;
    size_t log2_min_bs;
    size_t log2_max_bs;
    char *target_name;
    int (*target_func)(uint8_t *, size_t, saidx_t *, size_t, int32_t *, size_t);
    size_t (*target_aux_len_func)(size_t);
    int rc = 0;

    if (argc != 5) {
        fprintf(stderr, "Invalid arguments\n\n"
                "Usage: %s [file] [log2_min_bs] [log2_max_bs] [target]\n"
                "    file           Path to test file\n"
                "    log2_min_bs    Log2 of minimum block size\n"
                "    log2_max_bs    Log2 of maximum block size\n"
                "    target         Benchmark target: 'kkp2/KKP2' or 'kkp3/KKP3'\n",
                argv[0]);
        return 1;
    }

    errno = 0;
    arg_bs = strtol(argv[2], &p, 10);
    if (errno != 0 || *p != '\0' || arg_bs < 0) {
        fprintf(stderr, "Could not parse minimum block size\n");
        return 1;
    }
    log2_min_bs = arg_bs;

    arg_bs = strtol(argv[3], &p, 10);
    if (errno != 0 || *p != '\0' || arg_bs < 0) {
        fprintf(stderr, "Could not parse maximum block size\n");
        return 1;
    }
    log2_max_bs = arg_bs;

    if (log2_min_bs < 10 || log2_min_bs > log2_max_bs || log2_max_bs > 31) {
        fprintf(stderr, "Invalid range for block size - specify log2 sizes in "
                "range [10, 31]\n");
        return 1;
    }

    if (strcmp(argv[4], "kkp2") == 0 || strcmp(argv[4], "KKP2") == 0) {
        target_name = "KKP2";
        target_func = &kkp2_factor;
        target_aux_len_func = &kkp2_aux_len;
    } else if (strcmp(argv[4], "kkp3") == 0 || strcmp(argv[4], "KKP3") == 0) {
        target_name = "KKP3";
        target_func = &kkp3_factor;
        target_aux_len_func = &kkp3_aux_len;
    } else {
        fprintf(stderr, "Unknown target: %s\n", argv[4]);
        return 1;
    }

    if ((fp = fopen(argv[1], "r")) == NULL) {
        perror("fopen");
        return 1;
    }

    printf("block size (log2),block size (b),divsufsort time (s),"
           "%s time (s),I/O time (s),total time (s),phrases (#)\n",
           target_name);

    for (size_t log2_bs = log2_min_bs; log2_bs <= log2_max_bs; log2_bs++) {
        uint8_t *block;
        size_t block_len;
        saidx_t *sa;
        size_t sa_len;
        int32_t *aux;
        size_t aux_len;
        size_t bytes_read;
        uint64_t total_ns;
        uint64_t start_ns;
        uint64_t sa_ns = 0;
        uint64_t target_ns = 0;
        size_t nr_target_factors = 0;
        int res;

        block_len = 1L << log2_bs;
        sa_len = block_len + 2;
        aux_len = (*target_aux_len_func)(block_len);

        block = malloc(block_len * sizeof(*block));
        sa = malloc(sa_len * sizeof(*sa));
        aux = malloc(aux_len * sizeof(*aux));

        if (block == NULL || sa == NULL || aux == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            rc = 1;
            break;
        }

        total_ns = get_time_ns();
        while ((bytes_read = fread(block, sizeof(*block), block_len, fp)) != 0) {
            start_ns = get_time_ns();
            res = divsufsort((sauchar_t *)block, sa + 1, bytes_read);
            sa_ns += get_time_ns() - start_ns;

            if (res != 0) {
                fprintf(stderr, "divsufsort failed\n");
                rc = 1;
                break;
            }

            start_ns = get_time_ns();
            res = (*target_func)(block, bytes_read, sa, sa_len, aux, aux_len);
            target_ns += get_time_ns() - start_ns;

            if (res < 0) {
                fprintf(stderr, "%s factorization failed\n", target_name);
                rc = 1;
                break;
            }

            nr_target_factors += res;
        }
        total_ns = get_time_ns() - total_ns;

        if (ferror(fp) || !feof(fp)) {
            fprintf(stderr, "Error or EOF not reached\n");
            rc = 1;
        }

        rewind(fp);
        if (errno != 0) {
            perror("rewind");
            rc = 1;
        }

        printf("%zu,%ld,%.5Lf,%.5Lf,%.5Lf,%.5Lf,%zu\n",
               log2_bs,
               block_len,
               1.0L * sa_ns / 1000000000,
               1.0L * target_ns / 1000000000,
               1.0L * (total_ns - sa_ns - target_ns) / 1000000000,
               1.0L * total_ns / 1000000000,
               nr_target_factors);

        free(block);
        free(sa);
        free(aux);

        if (rc != 0)
            break;
    }

    fclose(fp);

    return rc;
}
