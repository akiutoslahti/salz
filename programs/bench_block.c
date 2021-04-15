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

static double
compute_lcp_mean(uint8_t *text, int32_t *sa, size_t n, int32_t *aux, size_t aux_len)
{
    int32_t *phi;
    int32_t *plcp;
    uint64_t lcp_sum;
    size_t i, l;

    if (aux_len < 2 * n) {
        fprintf(stderr, "auxiliary array is too short\n");
        return 0.0;
    }

    phi = aux;
    for (i = 1; i < n; i++)
        phi[sa[i]] = sa[i - 1];

    plcp = &aux[n];
    l = 0;
    for (i = 0; i < n; i++) {
        while (i + l < n &&
               phi[i] + l < n &&
               text[i + l] == text[phi[i] + l]) {
            l += 1;
        }
        plcp[i] = l;
        if (l > 0)
            l -= 1;
    }

    lcp_sum = 0;
    for (i = 1; i < n; i++)
        lcp_sum += plcp[sa[i]];

    return 1.0 * lcp_sum / (n - 1);
}

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
           "lcp mean,phrases (nr)\n");

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
        double lcp_mean = 0.0;
        double lcp_mean_block;
        size_t prev_blocks = 0;
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

            /* mean lcp */
            lcp_mean_block = compute_lcp_mean(block, sa + 1, bytes_read, aux, aux_len);
            /* weigh mean if last block is short */
            if (bytes_read != block_len)
                lcp_mean = (lcp_mean * prev_blocks * block_len + lcp_mean_block * bytes_read) /
                           (prev_blocks * block_len + bytes_read);
            else
                lcp_mean = (lcp_mean * prev_blocks + lcp_mean_block) / (prev_blocks + 1);
            prev_blocks += 1;

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

        printf("%s,%zu,%ld,%.5f,%.5f,%.5f,%.5f,%.5f,%.1f,%zu\n",
               fname,
               log2_bs,
               block_len,
               1.0 * io_ns / NS_IN_SEC,
               1.0 * divsufsort_ns / NS_IN_SEC,
               1.0 * sais_ns / NS_IN_SEC,
               1.0 * kkp2_ns / NS_IN_SEC,
               1.0 * kkp3_ns / NS_IN_SEC,
               lcp_mean,
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
