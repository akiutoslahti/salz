/*
 * salzcli.c - SALZ Command Line Interface
 *
 * Copyright (c) 2021 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "salz.h"

#define DEFAULT_LOG2_BLOCK_SIZE 16
#define DEFAULT_BLOCK_SIZE (1 << DEFAULT_LOG2_BLOCK_SIZE)

static int compress_filename(char *in_fname, char *out_fname, size_t block_size)
{
    FILE *in_stream = NULL;
    FILE *out_stream = NULL;
    uint8_t *src_buf = NULL;
    uint8_t *dst_buf = NULL;
    size_t src_len = block_size;
    /* @TODO formulate maximum compressed size better */
    size_t dst_len = 2 * block_size;
    size_t read_len;
    size_t in_fsize = 0;
    size_t out_fsize = 0;
    int ret = 0;
    uint64_t clock;

    if ((in_stream = fopen(in_fname, "r")) == NULL) {
        perror("Input file could not be opened");
        goto fail;
    }

    if ((out_stream = fopen(out_fname, "w")) == NULL) {
        perror("Output file could not be opened");
        goto fail;
    }

    src_buf = malloc(src_len);
    dst_buf = malloc(dst_len);

    if (src_buf == NULL || dst_buf == NULL)
        goto fail;

    clock = get_time_ns();
    while ((read_len = fread(src_buf, 1, src_len, in_stream)) != 0) {
        in_fsize += read_len;
        size_t encoded_len = salz_encode_default(src_buf, read_len, dst_buf,
                                                 dst_len);

        if (encoded_len == 0) {
            /* @TODO add error ? */
        }

        if (fwrite(dst_buf, 1, encoded_len, out_stream) != encoded_len) {
            /* @TODO add error ? */
        }

        out_fsize += encoded_len;
    }
    clock = get_time_ns() - clock;

    fprintf(stdout, "Original size: %zuB, compressed size: %zuB, "
            "compression ratio: %.2lf, compression time: %.2lfs\n", in_fsize,
            out_fsize, 1.0 * in_fsize / out_fsize, 1.0 * clock / NS_IN_SEC);


exit:
    if (in_stream != NULL)
        fclose(in_stream);
    if (out_stream != NULL)
        fclose(out_stream);
    free(src_buf);
    free(dst_buf);

    return ret;

fail:
    ret = -1;
    goto exit;
}

static void print_usage(char *binary_name)
{
    printf("Usage: %s [OPTION] [in_fname] [out_fname]\n", binary_name);
    printf("Compress or uncompress in_fname to out_fname (by default, compress).\n");
    printf("\n");
    printf("-h, --help       display this help\n");
    printf("-d, --decompress decompress\n");
}

static void suggest_help(char *binary_name)
{
    printf("See `%s --help` for more information.\n", binary_name);
}

static char *get_name(char *fpath)
{
    char *name;

    if ((name = strrchr(fpath, '/')) != NULL)
        return name + 1;

    return fpath;
}

static bool parse_u32(char *optarg, uint32_t *res)
{
    char *p;
    unsigned long val;

    if (optarg == NULL || res == NULL)
        return false;

    errno = 0;
    val = strtoul(optarg, &p, 10);
    if (errno != 0 || *p != '\0' || val > UINT32_MAX)
        return false;

    *res = (uint32_t)val;

    return true;
}

int main(int argc, char *argv[])
{
    char *binary_name = get_name(argv[0]);
    size_t block_size = DEFAULT_BLOCK_SIZE;

    while (1) {
        char *short_opt = "b:dh";
        struct option long_opt[] = {
            { "decompress", no_argument, NULL, 'd' },
            { "help", no_argument, NULL, 'h' },
            { NULL, 0, NULL, 0 }
        };

        int optc = getopt_long(argc, argv, short_opt, long_opt, NULL);

        if (optc == -1)
            break;

        switch (optc) {
            case 'b': {
                uint32_t u32val;
                if (!parse_u32(optarg, &u32val) || u32val < 10 || 30 < u32val) {
                    fprintf(stderr, "Invalid block size\n");
                    return 1;
                }
                block_size = 1 << u32val;
                break;
            }

            case 'd':
                printf("decompress mode not yet implemented\n");
                return 1;

            case 'h':
                print_usage(binary_name);
                return 0;

            case '?':
                suggest_help(binary_name);
                break;

            default:
                return 1;
        }
    }

    char *in_fname = NULL;
    char *out_fname = NULL;
    while (optind < argc) {
        if (in_fname == NULL) {
            in_fname = argv[optind++];
            continue;
        }

        if (out_fname == NULL) {
            out_fname = argv[optind++];
            continue;
        }

        fprintf(stderr, "%s: too many arguments\n", binary_name);
        suggest_help(binary_name);
        return 1;
    }

    if (in_fname == NULL || out_fname == NULL) {
        fprintf(stderr, "%s: too few arguments\n", binary_name);
        suggest_help(binary_name);
        return 1;
    }

    return compress_filename(in_fname, out_fname, block_size);
}
