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
#include "libsais.h"

#define NS_IN_SEC 1000000000

int
main(int argc, const char *argv[])
{
    const char *fname;
    FILE *fp;
    char *p;
    long arg_bs;
    size_t log2_min_bs;
    size_t log2_max_bs;
    int rc = 0;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Invalid arguments\n\n"
                "Usage: %s [file] [log2_min_bs] [log2_max_bs]\n"
                "    file           Path to test file\n"
                "    log2_min_bs    Log2 of minimum block size\n"
                "    log2_max_bs    Log2 of maximum block size (opt)\n",
                argv[0]);
        return 1;
    }

    fname = argv[1];

    errno = 0;
    arg_bs = strtol(argv[2], &p, 10);
    if (errno != 0 || *p != '\0' || arg_bs < 0) {
        fprintf(stderr, "Could not parse minimum block size\n");
        return 1;
    }
    log2_min_bs = arg_bs;

    if (argc == 4) {
        arg_bs = strtol(argv[3], &p, 10);
        if (errno != 0 || *p != '\0' || arg_bs < 0) {
            fprintf(stderr, "Could not parse maximum block size\n");
            return 1;
        }
        log2_max_bs = arg_bs;
    } else {
        log2_max_bs = log2_min_bs;
    }

    if (log2_min_bs < 10 || log2_min_bs > log2_max_bs || log2_max_bs > 31) {
        fprintf(stderr, "Invalid range for block size - specify log2 sizes in "
                "range [10, 31]\n");
        return 1;
    }

    if ((fp = fopen(fname, "r")) == NULL) {
        perror("fopen");
        return 1;
    }

    printf("filename,block size (log2),block size (b),io time (s),"
           "divsufsort time (s),sais time (s),kkp2 time (s),kkp3 time (s),"
           "phrases (nr)\n");

    for (size_t log2_bs = log2_min_bs; log2_bs <= log2_max_bs; log2_bs++) {
        uint8_t *block;
        size_t block_len;
        saidx_t *sa;
        size_t sa_len;
        int32_t *aux;
        size_t aux_len;
        size_t bytes_read;
        uint64_t start_ns;
        uint64_t io_ns = 0;
        uint64_t divsufsort_ns = 0;
        uint64_t sais_ns = 0;
        uint64_t kkp2_ns = 0;
        uint64_t kkp3_ns = 0;
        size_t nr_kkp2_factors = 0;
        size_t nr_kkp3_factors = 0;
        int res;

        block_len = 1L << log2_bs;
        sa_len = block_len + 2;
        /*
         * Auxiliary space demand for text of length N:
         *   - KKP2 - N + 1
         *   - KKP3 - 2N
         */
        aux_len = 2 * block_len;

        block = malloc(block_len * sizeof(*block));
        sa = malloc(sa_len * sizeof(*sa));
        aux = malloc(aux_len * sizeof(*aux));

        if (block == NULL || sa == NULL || aux == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            rc = 1;
            break;
        }

        while (1) {
            /* File I/O */
            start_ns = get_time_ns();
            bytes_read = fread(block, sizeof(*block), block_len, fp);
            io_ns += get_time_ns() - start_ns;

            if (bytes_read == 0)
                break;

            /* divsufsort */
            start_ns = get_time_ns();
            res = divsufsort((sauchar_t *)block, sa + 1, bytes_read);
            divsufsort_ns += get_time_ns() - start_ns;

            if (res != 0) {
                fprintf(stderr, "divsufsort failed\n");
                rc = 1;
                break;
            }

            /* kkp2 factorization */
            start_ns = get_time_ns();
            res = kkp2_factor(block, bytes_read, sa, sa_len, aux, aux_len);
            kkp2_ns += get_time_ns() - start_ns;

            if (res < 0) {
                fprintf(stderr, "kkp2 factorization failed\n");
                rc = 1;
                break;
            }

            nr_kkp2_factors += res;

            /* sais */
            start_ns = get_time_ns();
            res = libsais(block, sa + 1, bytes_read, 0);
            sais_ns += get_time_ns() - start_ns;

            if (res != 0) {
                fprintf(stderr, "sais failed\n");
                rc = 1;
                break;
            }

            /* kkp3 factorization */
            start_ns = get_time_ns();
            res = kkp3_factor(block, bytes_read, sa, sa_len, aux, aux_len);
            kkp3_ns += get_time_ns() - start_ns;

            if (res < 0) {
                fprintf(stderr, "kkp3 factorization failed\n");
                rc = 1;
                break;
            }

            nr_kkp3_factors += res;
        }

        if (ferror(fp) || !feof(fp)) {
            fprintf(stderr, "Error or EOF not reached\n");
            rc = 1;
        }

        rewind(fp);
        if (errno != 0) {
            perror("rewind");
            rc = 1;
        }

        if (nr_kkp2_factors != nr_kkp3_factors) {
            fprintf(stderr, "Differing factor counts for kkp2 and kkp3 - "
                    "kkp2: %ld, kkp3: %ld\n",
                    nr_kkp2_factors,
                    nr_kkp3_factors);
            rc = 1;
        }

        printf("%s,%zu,%ld,%.5Lf,%.5Lf,%.5Lf,%.5Lf,%.5Lf,%zu\n",
               fname,
               log2_bs,
               block_len,
               1.0L * io_ns / NS_IN_SEC,
               1.0L * divsufsort_ns / NS_IN_SEC,
               1.0L * sais_ns / NS_IN_SEC,
               1.0L * kkp2_ns / NS_IN_SEC,
               1.0L * kkp3_ns / NS_IN_SEC,
               nr_kkp2_factors);

        free(block);
        free(sa);
        free(aux);

        if (rc != 0)
            break;
    }

    fclose(fp);

    return rc;
}
