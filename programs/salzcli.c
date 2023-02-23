/*
 * salzcli.c - SALZ Command Line Interface
 *
 * Copyright (c) 2021-2023 Aki Utoslahti. All rights reserved.
 *
 * This work is distributed under terms of the MIT license.
 * See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include <linux/limits.h>

#include "common.h"
#include "salz.h"

static const uint32_t salz_magic = 0x53414C5A;

#define OK     (0)
#define ERROR (-1)

static const char *suffix = ".salz";
static const char *unsalz = "unsalz";
static const char *salzcat = "salzcat";

enum operation_mode {
    COMPRESS,
    DECOMPRESS,
    PRINT_INFO,
};
static int operation_mode = COMPRESS;

enum log_lvl {
    LOG_LVL_NONE,
    LOG_LVL_CRITICAL,
    LOG_LVL_ERROR,
    LOG_LVL_INFO,
};
static int log_lvl = LOG_LVL_INFO;

static bool overwrite_output = false;
static bool keep_input = false;
static int compression_level = 5;

#define log(lvl, fmt, ...) \
    do { \
        if (lvl <= log_lvl) \
            fprintf(stderr, fmt "\n", ## __VA_ARGS__);\
    } while (0)

#define log_crit(fmt, ...) log(LOG_LVL_CRITICAL, fmt, ## __VA_ARGS__)
#define log_err(fmt, ...) log(LOG_LVL_ERROR, fmt, ## __VA_ARGS__)
#define log_info(fmt, ...) log(LOG_LVL_INFO, fmt, ## __VA_ARGS__)

static const char *get_filename(const char *path)
{
    const char *name;
    if ((name = strrchr(path, '/')) != NULL)
        return name + 1;

    return path;
}

static void fill_outpath(const char *path, char buf[PATH_MAX])
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);

    if (operation_mode == DECOMPRESS) {
        size_t copy_len = path_len - suffix_len;
        memcpy(buf, path, copy_len);
        buf[copy_len] = '\0';
    } else {
        /* @todo: check this with fresh eyes */
        const char *filename = get_filename(path);
        size_t filename_len = strlen(filename);
        size_t copy_len = path_len;

        if (filename_len + suffix_len + 1 > NAME_MAX)
            copy_len = path_len - filename_len + (NAME_MAX - (suffix_len + 1));

        if (path_len + suffix_len + 1 > PATH_MAX)
            copy_len = PATH_MAX - (suffix_len + 1);

        memcpy(buf, path, copy_len);
        memcpy(buf + copy_len, suffix, suffix_len);
        buf[copy_len + suffix_len] = '\0';
    }
}

static int compress(FILE *in, FILE *out)
{
    uint8_t *inbuf;
    uint8_t *outbuf;
    size_t inbuf_cap;
    size_t outbuf_cap;

    uint32_t plain_len = 1 << (15 + compression_level);
    /*
     * @todo: create more substantial file header which contais
     * magic number, version, flags, original size, original
     * timestamp(s), original filename and checksum (optional)
     */
    uint8_t salz_hdr[8];

    int ret = OK;

    static_assert(sizeof(salz_magic) + sizeof(plain_len) == sizeof(salz_hdr));
    memcpy(salz_hdr, &salz_magic, sizeof(salz_magic));
    memcpy(salz_hdr + sizeof(salz_magic), &plain_len, sizeof(plain_len));

    inbuf_cap = plain_len;
    if ((inbuf = malloc(inbuf_cap)) == NULL) {
        log_err("Couldn't allocate memory: (%zu bytes)", inbuf_cap);
        return ERROR;
    }

    outbuf_cap = salz_encoded_len_max(inbuf_cap);
    if ((outbuf = malloc(outbuf_cap)) == NULL) {
        log_err("Couldn't allocate memory: (%zu bytes)", outbuf_cap);
        free(inbuf);
        return ERROR;
    }

    if (fwrite(salz_hdr, 1, sizeof(salz_hdr), out) != sizeof(salz_hdr)) {
        log_err("Couldn't write SALZ header to output");
        free(outbuf);
        free(inbuf);
        return ERROR;
    }

    for ( ;; ) {
        size_t inbuf_len;
        size_t outbuf_len = outbuf_cap;
        uint32_t encoded_len;

        if ((inbuf_len = fread(inbuf, 1, inbuf_cap, in)) != inbuf_cap) {
            if (ferror(in)) {
                log_err("Couldn't read from input stream");
                ret = ERROR;
                break;
            }
        }

        if (salz_encode_safe(inbuf, inbuf_len, outbuf, &outbuf_len) != 0) {
            log_err("Couldn't encode segment");
            ret = ERROR;
            break;
        }

        encoded_len = outbuf_len;
        if (fwrite(&encoded_len, 1, sizeof(encoded_len), out) != sizeof(encoded_len)) {
            log_err("Couldn't write encoded segments length to output stream");
            ret = ERROR;
            break;
        }

        if (fwrite(outbuf, 1, outbuf_len, out) != outbuf_len) {
            log_err("Couldn't write encoded segment to output stream");
            ret = ERROR;
            break;
        }

        if (inbuf_len != inbuf_cap && feof(in)) {
            ret = OK;
            break;
        }
    }

    free(outbuf);
    free(inbuf);

    return ret;
}

static int decompress(FILE *in, FILE *out)
{
    uint8_t *inbuf;
    uint8_t *outbuf;
    size_t inbuf_cap;
    size_t outbuf_cap;

    uint8_t salz_hdr[8];
    uint32_t plain_len;

    int ret = OK;

    if (fread(salz_hdr, 1, sizeof(salz_hdr), in) != sizeof(salz_hdr)) {
        log_err("Couldn't read SALZ header from input");
        return ERROR;
    }

    if (memcmp(salz_hdr, &salz_magic, sizeof(salz_magic)) != 0) {
        log_err("Not a SALZ header, unexpected magic number");
        return ERROR;
    }
    memcpy(&plain_len, salz_hdr + sizeof(salz_magic), sizeof(plain_len));

    inbuf_cap = salz_encoded_len_max(plain_len);
    if ((inbuf = malloc(inbuf_cap)) == NULL) {
        log_err("Couldn't allocate memory (%zu bytes)", inbuf_cap);
        return ERROR;
    }

    outbuf_cap = plain_len;
    if ((outbuf = malloc(outbuf_cap)) == NULL) {
        log_err("Couldn't allocate memory (%zu bytes)", outbuf_cap);
        free(inbuf);
        return ERROR;
    }

    for ( ;; ) {
        size_t inbuf_len;
        size_t outbuf_len = outbuf_cap;
        uint32_t encoded_len;

        if (fread(&encoded_len, 1, sizeof(encoded_len), in) != sizeof(encoded_len)) {
            if (ferror(in)) {
                log_err("Couldn't read encoded segments length from input stream");
                ret = ERROR;
                break;
            }

            if (feof(in)) {
                ret = OK;
                break;
            }
        }

        if (encoded_len > inbuf_cap) {
            log_err("Encoded segment too large to fit into input buffer");
            ret = ERROR;
            break;
        }

        if ((inbuf_len = fread(inbuf, 1, encoded_len, in)) != encoded_len) {
            log_err("Couldn't read encoded segment from input stream");
            ret = ERROR;
            break;
        }

        if (salz_decode_safe(inbuf, inbuf_len, outbuf, &outbuf_len) != 0) {
            log_err("Couldn't decode segment");
            ret = ERROR;
            break;
        }

        if (fwrite(outbuf, 1, outbuf_len, out) != outbuf_len) {
            log_err("Couldn't write decoded segment to output stream");
            ret = ERROR;
            break;
        }
    }

    free(outbuf);
    free(inbuf);

    return ret;
}

static int process_path(const char *path)
{
    FILE *instream;
    FILE *outstream;
    off_t insize;
    off_t outsize;
    char outpath[PATH_MAX];
    bool has_suffix;
    uint64_t ns_begin = 0;
    uint64_t ns_end = 0;
    int rc;

    struct stat st;

    has_suffix = strstr(path, suffix) != NULL;
    if (has_suffix && operation_mode == COMPRESS) {
        log_err("\"%s\" path already has \".salz\" suffix", path);
        return ERROR;
    }

    if (!has_suffix && (operation_mode == DECOMPRESS || operation_mode == PRINT_INFO)) {
        log_err("\"%s\" path has unknown suffix", path);
        return ERROR;
    }

    if (stat(path, &st) != 0) {
        log_err("Couldn't stat \"%s\" path (err: %d)", path, errno);
        return ERROR;
    }

    if (!S_ISREG(st.st_mode)) {
        log_err("\"%s\" path is not a regular file", path);
        return ERROR;
    }
    insize = st.st_size;

    instream = fopen(path, "r");
    if (instream == NULL) {
        log_err("Couldn't open \"%s\" path (err: %d)", path, errno);
        return ERROR;
    }

    if (operation_mode == PRINT_INFO) {
        outstream = NULL;
    } else {
        fill_outpath(path, outpath);
        if (!overwrite_output && stat(outpath, &st) == 0) {
            log_err("\"%s\" path already exists", outpath);
            fclose(instream);
            return ERROR;
        }
        outstream = fopen(outpath, "w");
        if (outstream == NULL) {
            log_err("Couldn't open \"%s\" path (err: %d)", outpath, errno);
            fclose(instream);
            return ERROR;
        }
    }

    get_time_ns(&ns_begin);
    if (operation_mode == COMPRESS) {
        rc = compress(instream, outstream);
    } else if (operation_mode == DECOMPRESS) {
        rc = decompress(instream, outstream);
    } else if (operation_mode == PRINT_INFO) {
        /* @todo: support this */
        log_err("Operation not supported");
        rc = ERROR;
    } else {
        log_crit("Unknown operation mode");
        abort();
    }
    get_time_ns(&ns_end);

    if (outstream != NULL)
        fclose(outstream);
    fclose(instream);

    if (rc != 0) {
        log_err("Operation failed");
        unlink(outpath);
        return ERROR;
    } else if (!keep_input)
        unlink(path);

    if (stat(outpath, &st) != 0) {
        log_err("Couldn't stat \"%s\" path (err: %d)", outpath, errno);
        return ERROR;
    }
    outsize = st.st_size;

    if (operation_mode == COMPRESS)
        log_info("%s: compressed %ld bytes to %ld bytes (ratio: %.3f) in %.3f seconds",
                 path, insize, outsize, 1.0 * insize / outsize,
                 (ns_end - ns_begin) * 1.0 / NS_IN_SEC);
    else if (operation_mode == DECOMPRESS)
        log_info("%s: decompressed %ld bytes in %.3f seconds",
                 path, insize, (ns_end - ns_begin) * 1.0 / NS_IN_SEC);

    return OK;
}

int main(int argc, char *argv[])
{
    const char *execname = get_filename(argv[0]);
    const char *short_opt = "cdfhklq0123456789";
    const struct option long_opt[] = {
        { "stdout", no_argument, NULL, 'c' },
        { "decompress", no_argument, NULL, 'd' },
        { "force", no_argument, NULL, 'f' },
        { "help", no_argument, NULL, 'h' },
        { "keep", no_argument, NULL, 'k' },
        { "list", no_argument, NULL, 'l' },
        { "quiet", no_argument, NULL, 'q' },
        { "fast", no_argument, NULL, '1' },
        { "best", no_argument, NULL, '9' },
        { NULL, 0, NULL, 0 },
    };
    int ret = 0;

    for ( ;; ) {
        int opt = getopt_long(argc, argv, short_opt, long_opt, NULL);
        if (opt == -1)
            break;

        switch (opt) {
            case 'c':
                /* @todo: support this */
                fprintf(stderr, "writing to stdout not supported\n");
                return ERROR;

            case 'd':
                operation_mode = DECOMPRESS;
                break;

            case 'f':
                overwrite_output = true;
                break;

            case 'h':
                printf("salz, a Suffix Array-based Lempel-Ziv data compressor\n");
                printf("\n");
                printf("  usage: %s [options] input_file ...\n", execname);
                printf("\n");
                printf("  -c --stdout        write to standard output, keep input file\n");
                printf("  -d --decompress    force decompression mode\n");
                printf("  -f --force         force overwrite of output file\n");
                printf("  -h --help          print this message\n");
                printf("  -k --keep          keep input file\n");
                printf("  -l --list          print information about salz-compressed file\n");
                printf("  -q --quiet         suppress output\n");
                printf("                     (specify twice to all but non-critical errors)\n");
                printf("  -0 ... -9          compression level [default: 5]\n");
                printf("                     (note that memory usage grows exponentially)\n");
                printf("  --fast             alias of \"-1\"\n");
                printf("  --best             alias of \"-9\"\n");
                printf("\n");
                printf("  Default action is to compress.\n");
                printf("  If invoked as \"unsalz\", default action is to decompress.\n");
                printf("                \"salzcat\", default action is to decompress to stdout.\n");
                printf("\n");
                printf("  If no input file is given, or - is provided instead, salz compresses\n");
                printf("  or decompresses from standard input to standard output.\n");
                return 0;

            case 'k':
                keep_input = true;
                break;

            case 'l':
                /* @todo: support this */
                fprintf(stderr, "listing info not supported\n");
                return ERROR;

            case 'q':
                if (log_lvl > 0)
                    log_lvl--;
                break;

            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                compression_level = opt - '0';
                break;

            case '?':
            default:
                fprintf(stderr, "See \"%s --help\" for more information.\n", execname);
                return ERROR;
        }
    }

    if (strncmp(execname, unsalz, strlen(unsalz)) == 0) {
        operation_mode = DECOMPRESS;
    }

    if (strncmp(execname, salzcat, strlen(salzcat)) == 0) {
        /* @todo: support this */
        fprintf(stderr, "writing to stdout not supported\n");
        return ERROR;
    }

    argv += optind;
    argc -= optind;

    if (argc == 0 || *argv[0] == '-') {
        /* @todo: support this */
        fprintf(stderr, "compressing from stdin not supported\n");
        return ERROR;
    } else {
        for ( ; *argv != NULL; argv++) {
            int rc = process_path(*argv);
            ret = min(ret, rc);
        }
    }

    return ret;
}

