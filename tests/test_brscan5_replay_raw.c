/*
 * Copyright 2025 excellentplans. SPDX-License-Identifier: GPL-2.0-or-later
 * test_brscan5_replay_raw.c — end-to-end brscan5 replay test (T6).
 *
 * Loads the backend .so directly (dlopen), runs the full SANE lifecycle
 * (sane_init -> sane_open -> options -> sane_start -> sane_read* ->
 * sane_close -> sane_exit) against the replay transport and writes the
 * DECODED raw scanlines to an output file. Since T6 the read path streams
 * decoded scanlines out of a libjpeg decoder (no more raw-JPEG
 * passthrough — the T5 JPEG-passthrough test was removed with it).
 *
 * The test also verifies the two-phase sane_get_parameters behaviour:
 *   - before sane_start: estimate from the options (consistent with mode)
 *   - after  sane_start: real JPEG header dimensions (2446x3485 for the
 *     300 dpi colour A4 reference scan, bytes_per_line 7338, RGB, depth 8)
 *
 * Usage:
 *   test_brscan5_replay_raw <backend.so> <outfile> [device]
 *
 * Environment:
 *   SANE_CONFIG_DIR   backend config dir (tests/sane)
 *   BROTHER5_REPLAY   replay fixture path (must be set)
 *   LD_LIBRARY_PATH   where libsane-brother's deps (usb) resolve
 *
 * Returns 0 on success, non-zero on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include <sane/sane.h>

/* Reference dimensions of the 300 dpi colour A4 replay fixture page
 * (decoded by libjpeg; see fixtures/jpeg.reference.json). */
#define REF_WIDTH   2446
#define REF_HEIGHT  3485
#define REF_BPL     (REF_WIDTH * 3)
#define REF_SIZE    ((size_t)REF_BPL * REF_HEIGHT)

static int failures = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);          \
        failures++;                                                      \
    }                                                                    \
} while (0)

int
main(int argc, char **argv)
{
    const char *so_path, *out_path, *dev;
    void *h;
    FILE *fp;
    unsigned char *buf;
    const size_t BUF_SZ = 1u << 20;      /* 1 MiB reads */
    size_t total = 0;
    int rc = 1;
    SANE_Handle handle = NULL;
    SANE_Parameters par;
    SANE_Status st;
    SANE_Int info = 0;
    SANE_Int reso300 = 300;
    const char *mode_color = "24bit Color";

    if (argc < 3) {
        fprintf(stderr, "usage: %s <backend.so> <outfile> [device]\n", argv[0]);
        return 2;
    }
    so_path = argv[1];
    out_path = argv[2];
    dev = (argc > 3) ? argv[3] : "brscan5;replay0";

    if (!getenv("BROTHER5_REPLAY")) {
        fprintf(stderr, "BROTHER5_REPLAY must be set (replay fixture)\n");
        return 2;
    }

    h = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen(%s): %s\n", so_path, dlerror());
        return 2;
    }

    /* Resolve the SANE entry points. */
    SANE_Status (*sane_init_fn)(SANE_Int *, SANE_Auth_Callback) =
        (SANE_Status (*)(SANE_Int *, SANE_Auth_Callback))dlsym(h, "sane_init");
    SANE_Status (*sane_open_fn)(SANE_String_Const, SANE_Handle *) =
        (SANE_Status (*)(SANE_String_Const, SANE_Handle *))dlsym(h, "sane_open");
    SANE_Status (*sane_start_fn)(SANE_Handle) =
        (SANE_Status (*)(SANE_Handle))dlsym(h, "sane_start");
    SANE_Status (*sane_read_fn)(SANE_Handle, SANE_Byte *, SANE_Int,
                                SANE_Int *) =
        (SANE_Status (*)(SANE_Handle, SANE_Byte *, SANE_Int, SANE_Int *))dlsym(h, "sane_read");
    SANE_Status (*sane_get_parameters_fn)(SANE_Handle, SANE_Parameters *) =
        (SANE_Status (*)(SANE_Handle, SANE_Parameters *))dlsym(h, "sane_get_parameters");
    SANE_Status (*sane_control_option_fn)(SANE_Handle, SANE_Int, SANE_Action,
                                          void *, SANE_Int *) =
        (SANE_Status (*)(SANE_Handle, SANE_Int, SANE_Action,
                         void *, SANE_Int *))dlsym(h, "sane_control_option");
    void (*sane_cancel_fn)(SANE_Handle) =
        (void (*)(SANE_Handle))dlsym(h, "sane_cancel");
    void (*sane_close_fn)(SANE_Handle) =
        (void (*)(SANE_Handle))dlsym(h, "sane_close");
    void (*sane_exit_fn)(void) = (void (*)(void))dlsym(h, "sane_exit");

    if (!sane_init_fn || !sane_open_fn || !sane_start_fn || !sane_read_fn ||
        !sane_get_parameters_fn || !sane_control_option_fn ||
        !sane_cancel_fn || !sane_close_fn || !sane_exit_fn) {
        fprintf(stderr, "dlsym: missing SANE symbol: %s\n", dlerror());
        dlclose(h);
        return 2;
    }

    if (sane_init_fn(NULL, NULL) != SANE_STATUS_GOOD) {
        fprintf(stderr, "sane_init failed\n");
        goto out;
    }

    if (sane_open_fn(dev, &handle) != SANE_STATUS_GOOD) {
        fprintf(stderr, "sane_open(%s) failed\n", dev);
        goto init_done;
    }

    /* Configure the captured scenario: 300 dpi colour ADF scan. */
    st = sane_control_option_fn(handle, 3 /*optResolution*/,
                                SANE_ACTION_SET_VALUE, &reso300, &info);
    CHECK(st == SANE_STATUS_GOOD, "set resolution 300");
    st = sane_control_option_fn(handle, 2 /*optMode*/,
                                SANE_ACTION_SET_VALUE,
                                (void *)mode_color, &info);
    CHECK(st == SANE_STATUS_GOOD, "set mode 24bit Color");

    /* --- Phase 1: parameters before sane_start = option estimate ------ */
    st = sane_get_parameters_fn(handle, &par);
    CHECK(st == SANE_STATUS_GOOD, "get_parameters before start");
    CHECK(par.format == SANE_FRAME_RGB, "pre-start: RGB frame");
    CHECK(par.depth == 8, "pre-start: depth 8");
    CHECK(par.pixels_per_line > 0 && par.lines > 0,
          "pre-start: estimated dimensions positive");
    CHECK(par.bytes_per_line == par.pixels_per_line * 3,
          "pre-start: bytes_per_line == width*3 (colour)");
    printf("test_brscan5_replay_raw: pre-start params %dx%d bpl=%d "
           "(estimate)\n", par.pixels_per_line, par.lines,
           par.bytes_per_line);

    if (failures)
        goto cancel;

    if (sane_start_fn(handle) != SANE_STATUS_GOOD) {
        fprintf(stderr, "sane_start failed\n");
        goto close;
    }

    /* --- Phase 2: parameters after sane_start = real JPEG header ------ */
    st = sane_get_parameters_fn(handle, &par);
    CHECK(st == SANE_STATUS_GOOD, "get_parameters after start");
    CHECK(par.format == SANE_FRAME_RGB, "post-start: RGB frame");
    CHECK(par.depth == 8, "post-start: depth 8");
    CHECK(par.pixels_per_line == REF_WIDTH,
          "post-start: width 2446 (real JPEG header)");
    CHECK(par.lines == REF_HEIGHT,
          "post-start: height 3485 (real JPEG header)");
    CHECK(par.bytes_per_line == REF_BPL, "post-start: bytes_per_line 7338");
    printf("test_brscan5_replay_raw: post-start params %dx%d bpl=%d "
           "(real)\n", par.pixels_per_line, par.lines, par.bytes_per_line);

    fp = fopen(out_path, "wb");
    if (!fp) {
        perror("fopen outfile");
        goto cancel;
    }
    buf = (unsigned char *)malloc(BUF_SZ);
    if (!buf) {
        fclose(fp);
        goto cancel;
    }

    for (;;) {
        SANE_Int len = 0;
        st = sane_read_fn(handle, buf, (SANE_Int)BUF_SZ, &len);
        if (st == SANE_STATUS_EOF)
            break;
        if (st != SANE_STATUS_GOOD) {
            fprintf(stderr, "sane_read: status %d\n", (int)st);
            free(buf);
            fclose(fp);
            goto cancel;
        }
        if (len > 0) {
            if (fwrite(buf, 1, (size_t)len, fp) != (size_t)len) {
                perror("fwrite");
                free(buf);
                fclose(fp);
                goto cancel;
            }
            total += (size_t)len;
        }
    }
    free(buf);
    if (fclose(fp) != 0) {
        perror("fclose outfile");
        goto cancel;
    }
    printf("test_brscan5_replay_raw: wrote %zu B to %s\n", total, out_path);

    CHECK(total == REF_SIZE, "raw output size 2446*3485*3 = 25572930 B");
    rc = failures ? 1 : 0;
    if (!failures)
        printf("test_brscan5_replay_raw: all in-process checks passed\n");

cancel:
    sane_cancel_fn(handle);
close:
    sane_close_fn(handle);
init_done:
    sane_exit_fn();
out:
    if (h)
        dlclose(h);
    return rc;
}
