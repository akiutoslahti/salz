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
#include "vlc.h"
#include "salz.h"

#define DEFAULT_LOG2_BLOCK_SIZE 16

static inline size_t fread_vbyte(FILE *stream, uint32_t *res)
{
    int byte;

    if ((byte = fgetc(stream)) == EOF)
        return 0;
    *res = (unsigned int)byte & 0x7fu;
    if ((unsigned int)byte >= 0x80u)
        return 1;

    if ((byte = fgetc(stream)) == EOF)
        return 0;
    *res = ((*res + 1) << 7) | ((unsigned int)byte & 0x7fu);
    if ((unsigned int)byte >= 0x80u)
        return 2;

    if ((byte = fgetc(stream)) == EOF)
        return 0;
    *res = ((*res + 1) << 7) | ((unsigned int)byte & 0x7fu);
    if ((unsigned int)byte >= 0x80u)
        return 3;

    if ((byte = fgetc(stream)) == EOF)
        return 0;
    *res = ((*res + 1) << 7) | ((unsigned int)byte & 0x7fu);
    if ((unsigned int)byte >= 0x80u)
        return 4;

    if ((byte = fgetc(stream)) == EOF)
        return 0;
    *res = ((*res + 1) << 7) | ((unsigned int)byte & 0x7fu);
    return 5;
}

static inline size_t fwrite_vbyte(FILE *stream, uint32_t val)
{
    uint64_t vbyte;
    size_t vbyte_len = encode_vbyte_be(val, &vbyte);

    if (!fwrite(&vbyte, vbyte_len, 1, stream))
        return 0;

    return vbyte_len;
}

static int compress_fname(char *in_fname, char *out_fname,
        uint32_t log2_block_size)
{
    FILE *in_stream = NULL;
    FILE *out_stream = NULL;
    uint8_t *src_buf = NULL;
    uint8_t *dst_buf = NULL;
    uint32_t block_size;
    size_t src_len;
    size_t dst_len;
    size_t read_len;
    size_t in_fsize = 0;
    size_t out_fsize = 0;
    size_t write_len;
    int ret = 0;
    uint64_t clock;

    block_size = 1 << log2_block_size;
    src_len = block_size;
    /* @TODO formulate maximum compressed size better */
    dst_len = 2 * block_size;

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

    write_len = fwrite_vbyte(out_stream, block_size);
    if (write_len == 0) {
        /* @TODO add error ? */
        goto fail;
    }
    out_fsize += write_len;

    while ((read_len = fread(src_buf, 1, src_len, in_stream)) != 0) {
        in_fsize += read_len;
        int32_t rc = salz_encode_default(src_buf, read_len, dst_buf, dst_len);

        if (rc <= 0) {
            /* @TODO add error ? */
            goto fail;
        }

        write_len = fwrite_vbyte(out_stream, rc);
        if (write_len == 0) {
            /* @TODO add error ? */
            goto fail;
        }
        out_fsize += write_len;

        if (fwrite(dst_buf, 1, rc, out_stream) != (size_t)rc) {
            /* @TODO add error ? */
            goto fail;
        }
        out_fsize += rc;
    }
    clock = get_time_ns() - clock;

    fprintf(stdout, "Compressed %zu bytes into %zu bytes (ratio: %.3f) in %.3f seconds\n",
            in_fsize, out_fsize, 1.0 * in_fsize / out_fsize, 1.0 * clock / NS_IN_SEC);

#ifdef ENABLE_STATS
    struct stats *st = get_stats();

    fprintf(stderr, "    SACA time: %f, PSV/NSV time: %f, LZ factor time: %f, "
            "DP mincost time: %f, encode time: %f\n",
            1.0 * st->sa_time / NS_IN_SEC, 1.0 * st->psv_nsv_time / NS_IN_SEC,
            1.0 * st->factor_time / NS_IN_SEC, 1.0 * st->mincost_time / NS_IN_SEC,
            1.0 * st->encode_time / NS_IN_SEC);
#endif

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

static int decompress_fname(char *in_fname, char *out_fname)
{
    FILE *in_stream = NULL;
    FILE *out_stream = NULL;
    uint8_t *src_buf = NULL;
    uint8_t *dst_buf = NULL;
    size_t src_len;
    size_t dst_len;
    uint32_t block_size;
    uint32_t read_size;
    size_t out_fsize = 0;
    uint64_t clock;
    int ret = 0;
    size_t read_len;

    if ((in_stream = fopen(in_fname, "r")) == NULL) {
        perror("Input file could not be opened");
        goto fail;
    }

    if ((out_stream = fopen(out_fname, "w")) == NULL) {
        perror("Output file could not be opened");
        goto fail;
    }

    read_len = fread_vbyte(in_stream, &block_size);
    if (read_len == 0) {
        /* @TODO Add error ? */
        goto fail;
    }

    /* @TODO formulate maximum compressed size better */
    src_len = 2 * block_size;
    dst_len = block_size;

    src_buf = malloc(src_len);
    dst_buf = malloc(dst_len);

    if (src_buf == NULL || dst_buf == NULL)
        goto fail;

    clock = get_time_ns();
    while (fread_vbyte(in_stream, &read_size) != 0) {
        if (read_size < src_len &&
            fread(src_buf, 1, read_size, in_stream) != read_size) {
            /* @TODO Add error ? */
            goto fail;
        }

        int32_t rc = salz_decode_default(src_buf, read_size, dst_buf, dst_len);

        if (rc <= 0) {
            /* @TODO Add error ? */
            goto fail;
        }

        if (fwrite(dst_buf, 1, rc, out_stream) != (size_t)rc) {
            /* @TODO Add error ? */
            goto fail;
        }

        out_fsize += rc;
    }
    clock = get_time_ns() - clock;

    fprintf(stdout, "Decompressed %zu bytes in %.3f seconds\n",
            out_fsize, 1.0 * clock / NS_IN_SEC);

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
    uint32_t log2_block_size = DEFAULT_LOG2_BLOCK_SIZE;
    bool decompress_mode = false;

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
                log2_block_size = u32val;
                break;
            }

            case 'd':
                decompress_mode = true;
                break;

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

    if (decompress_mode)
        return decompress_fname(in_fname, out_fname);
    return compress_fname(in_fname, out_fname, log2_block_size);
}
