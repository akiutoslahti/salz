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

int main(int argc, const char *argv[])
{
    FILE *fp;
    char *p;
    long arg_bs;
    size_t log2_min_bs;
    size_t log2_max_bs;
    enum bench_target { sa, kkp3 };
    enum bench_target target;
    int rc = 0;

    if (argc != 5) {
        fprintf(stderr, "Invalid arguments\n\n"
                "Usage: %s [file] [log2_min_bs] [log2_max_bs] [target]\n"
                "    file           Filepath\n"
                "    log2_min_bs    Log2 of minimum block size\n"
                "    log2_max_bs    Log2 of maximum block size\n"
                "    target         Benchmark target: 'sa' or 'kkp3'\n",
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

    if (strcmp(argv[4], "sa") == 0) {
        target = sa;
    } else if (strcmp(argv[4], "kkp3") == 0) {
        target = kkp3;
    } else {
        fprintf(stderr, "Unknown target: %s\n", argv[4]);
        return 1;
    }

    if ((fp = fopen(argv[1], "r")) == NULL) {
        perror("fopen");
        return 1;
    }

    printf("block size (log2),block size (b),suffix sorting time (s),");
    if (target == sa)
        printf("I/O time (s),total time (s)\n");
    else
        printf("factorization time (s),I/O time (s),total time (s),factors (#)\n");

    for (size_t log2_bs = log2_min_bs; log2_bs <= log2_max_bs; log2_bs++) {
        uint8_t *T;
        size_t T_len;
        saidx_t *SA;
        size_t SA_len;
        int32_t *CPSS = NULL;
        size_t CPSS_len;
        size_t bytes_read;
        uint64_t t_total;
        uint64_t t_sa = 0;
        uint64_t t_target = 0;
        uint64_t t_start;
        int res;
        size_t nfactors = 0;

        T_len = 1L << log2_bs;
        SA_len = T_len;

        if (target == kkp3) {
            SA_len = T_len + 2;
            CPSS_len = 2 * T_len;
        }

        T = malloc(T_len * sizeof(*T));
        SA = malloc(SA_len * sizeof(*SA));

        if (T == NULL || SA == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            rc = 1;
            break;
        }

        if (target == kkp3) {
            if ((CPSS = malloc(CPSS_len * sizeof(*CPSS))) == NULL) {
                fprintf(stderr, "Memory allocation failed\n");
                rc = 1;
                break;
            }
        }

        t_total = get_time_ns();
        if (target == sa) {
            while ((bytes_read = fread(T, sizeof(*T), T_len, fp)) != 0) {
                t_start = get_time_ns();
                if (divsufsort((sauchar_t *)T, SA, bytes_read) != 0) {
                    fprintf(stderr, "divsufsort failed\n");
                    rc = 1;
                    break;
                }
                t_sa += get_time_ns() - t_start;
            }
        } else {
            while ((bytes_read = fread(T, sizeof(*T), T_len, fp)) != 0) {
                t_start = get_time_ns();
                if (divsufsort((sauchar_t *)T, SA + 1, bytes_read) != 0) {
                    fprintf(stderr, "divsufsort failed\n");
                    rc = 1;
                    break;
                }
                t_sa += get_time_ns() - t_start;

                t_start = get_time_ns();
                if ((res = kkp3_factor(T, bytes_read, SA, SA_len, CPSS, CPSS_len)) < 0) {
                    fprintf(stderr, "kkp3 factorization failed\n");
                    rc = 1;
                    break;
                }
                t_target += get_time_ns() - t_start;
                nfactors += res;
            }
        }
        t_total = get_time_ns() - t_total;

        if (ferror(fp) || !feof(fp)) {
            fprintf(stderr, "Did not reach EOF\n");
            rc = 1;
        }

        rewind(fp);
        if (errno != 0) {
            perror("rewind");
            rc = 1;
        }

        printf("%zu,%ld,%.5Lf,", log2_bs, T_len, 1.0L * t_sa / 1000000000);
        if (target == sa)
            printf("%.5Lf,%.5Lf\n",
                   1.0L * (t_total - t_sa) / 1000000000,
                   1.0L * t_total / 1000000000);
        else
            printf("%.5Lf,%.5Lf,%.5Lf,%zu\n",
                   1.0L * t_target / 1000000000,
                   1.0L * (t_total - t_sa - t_target) / 1000000000,
                   1.0L * t_total / 1000000000,
                   nfactors);

        free(T);
        free(SA);
        free(CPSS);

        if (rc != 0)
            break;
    }

    fclose(fp);
    return rc;
}
