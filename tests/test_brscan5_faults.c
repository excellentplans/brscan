/*
 * Copyright 2025 excellentplans. SPDX-License-Identifier: GPL-2.0-or-later
 * test_brscan5_faults.c — T7 fault-injection e2e tests for the brscan5
 * command layer (cancel mid-page, transport error, simulated timeout,
 * empty feeder).
 *
 * Loads the backend .so directly (dlopen) and drives the SANE lifecycle
 * against the replay transport (BROTHER5_REPLAY=<fixture>, no hardware).
 * Mode is the first argument:
 *
 *   cancel  — sane_start, sane_read until ~10 % of the page, sane_cancel
 *             (probe: sane_read must return SANE_STATUS_CANCELLED), then
 *             sane_start AGAIN (no sane_open in between) and read to EOF.
 *             Only the second cycle's decoded page is written to the
 *             output file; the caller (cmake/run_replay_test.cmake)
 *             verifies md5/size against the reference page.
 *   error   — sane_start #1 must fail with SANE_STATUS_IO_ERROR (0x04
 *             transport-error injection ~10 % into the data phase),
 *             sane_cancel must be clean, sane_start #2 must deliver the
 *             full page (written to the output file, md5-checked).
 *   timeout — sane_start must fail with SANE_STATUS_IO_ERROR within a
 *             bounded time (0x03 timeout injection; real sleep capped at
 *             50 ms). Output file stays empty.
 *   nodocs  — sane_start must fail with SANE_STATUS_NO_DOCS (CKD 00 02).
 *   jam     — sane_start must fail with SANE_STATUS_JAMMED (XSC 91 00).
 *             Output file stays empty.
 *
 * Usage:
 *   test_brscan5_faults <backend.so> <outfile> <cancel|error|timeout|nodocs>
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
#include <time.h>
#include <dlfcn.h>

#include <sane/sane.h>

/* Reference dimensions/size of the 300 dpi colour A4 replay page. */
#define REF_WIDTH   2446
#define REF_HEIGHT  3485
#define REF_BPL     (REF_WIDTH * 3)
#define REF_SIZE    ((size_t)REF_BPL * REF_HEIGHT)
/* ~10 % of the reference page in bytes (cancel/error cut, T7 spec). */
#define CUT_SIZE    (REF_SIZE / 10)

static int failures = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);          \
        failures++;                                                      \
    }                                                                    \
} while (0)

static double
now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Read loop until EOF (or a non-GOOD status, which is reported). If
 * limit > 0, stop early once *total has reached limit (cancel cut).
 * Returns the last SANE status; writes everything read to fp (fp == NULL
 * discards, counting only). */
static SANE_Status
read_all(SANE_Handle h,
         SANE_Status (*read_fn)(SANE_Handle, SANE_Byte *, SANE_Int, SANE_Int *),
         FILE *fp, size_t *total, size_t limit)
{
    unsigned char *buf = (unsigned char *)malloc(1u << 20);
    SANE_Status st = SANE_STATUS_GOOD;

    if (!buf)
        return SANE_STATUS_NO_MEM;
    for (;;) {
        SANE_Int len = 0;
        st = read_fn(h, buf, (SANE_Int)(1u << 20), &len);
        if (st == SANE_STATUS_EOF)
            break;
        if (st != SANE_STATUS_GOOD)
            break;
        if (len > 0) {
            if (fp && fwrite(buf, 1, (size_t)len, fp) != (size_t)len) {
                perror("fwrite");
                failures++;
                break;
            }
            *total += (size_t)len;
            if (limit && *total >= limit)
                break;
        }
    }
    free(buf);
    return st;
}

int
main(int argc, char **argv)
{
    const char *mode, *so_path, *out_path, *dev;
    void *h = NULL;
    FILE *fp = NULL;
    size_t total = 0;
    int rc = 1;
    SANE_Handle handle = NULL;
    SANE_Parameters par;
    SANE_Status st;
    SANE_Int info = 0;
    SANE_Int reso300 = 300;
    const char *mode_color = "24bit Color";

    SANE_Status (*sane_init_fn)(SANE_Int *, SANE_Auth_Callback);
    SANE_Status (*sane_open_fn)(SANE_String_Const, SANE_Handle *);
    SANE_Status (*sane_start_fn)(SANE_Handle);
    SANE_Status (*sane_read_fn)(SANE_Handle, SANE_Byte *, SANE_Int, SANE_Int *);
    SANE_Status (*sane_get_parameters_fn)(SANE_Handle, SANE_Parameters *);
    SANE_Status (*sane_control_option_fn)(SANE_Handle, SANE_Int, SANE_Action,
                                          void *, SANE_Int *);
    void (*sane_cancel_fn)(SANE_Handle);
    void (*sane_close_fn)(SANE_Handle);
    void (*sane_exit_fn)(void);

    if (argc < 4) {
        fprintf(stderr, "usage: %s <backend.so> <outfile> "
                        "<cancel|error|timeout|nodocs>\n", argv[0]);
        return 2;
    }
    so_path = argv[1];
    out_path = argv[2];
    mode = argv[3];
    dev = (argc > 4) ? argv[4] : "brscan5;replay0";

    if (!getenv("BROTHER5_REPLAY")) {
        fprintf(stderr, "BROTHER5_REPLAY must be set (replay fixture)\n");
        return 2;
    }

    h = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen(%s): %s\n", so_path, dlerror());
        return 2;
    }
    sane_init_fn = (SANE_Status (*)(SANE_Int *, SANE_Auth_Callback))
        dlsym(h, "sane_init");
    sane_open_fn = (SANE_Status (*)(SANE_String_Const, SANE_Handle *))
        dlsym(h, "sane_open");
    sane_start_fn = (SANE_Status (*)(SANE_Handle))dlsym(h, "sane_start");
    sane_read_fn = (SANE_Status (*)(SANE_Handle, SANE_Byte *, SANE_Int,
                                    SANE_Int *))dlsym(h, "sane_read");
    sane_get_parameters_fn = (SANE_Status (*)(SANE_Handle, SANE_Parameters *))
        dlsym(h, "sane_get_parameters");
    sane_control_option_fn = (SANE_Status (*)(SANE_Handle, SANE_Int,
        SANE_Action, void *, SANE_Int *))dlsym(h, "sane_control_option");
    sane_cancel_fn = (void (*)(SANE_Handle))dlsym(h, "sane_cancel");
    sane_close_fn = (void (*)(SANE_Handle))dlsym(h, "sane_close");
    sane_exit_fn = (void (*)(void))dlsym(h, "sane_exit");
    if (!sane_init_fn || !sane_open_fn || !sane_start_fn || !sane_read_fn ||
        !sane_get_parameters_fn || !sane_control_option_fn ||
        !sane_cancel_fn || !sane_close_fn || !sane_exit_fn) {
        fprintf(stderr, "dlsym: missing SANE symbol: %s\n", dlerror());
        goto out;
    }

    /* The output file always exists (empty for timeout/nodocs). */
    fp = fopen(out_path, "wb");
    if (!fp) {
        perror("fopen outfile");
        goto out;
    }

    if (sane_init_fn(NULL, NULL) != SANE_STATUS_GOOD) {
        fprintf(stderr, "sane_init failed\n");
        goto out;
    }
    if (sane_open_fn(dev, &handle) != SANE_STATUS_GOOD) {
        fprintf(stderr, "sane_open(%s) failed\n", dev);
        goto init_done;
    }
    st = sane_control_option_fn(handle, 3 /*optResolution*/,
                                SANE_ACTION_SET_VALUE, &reso300, &info);
    CHECK(st == SANE_STATUS_GOOD, "set resolution 300");
    st = sane_control_option_fn(handle, 2 /*optMode*/,
                                SANE_ACTION_SET_VALUE,
                                (void *)mode_color, &info);
    CHECK(st == SANE_STATUS_GOOD, "set mode 24bit Color");

    if (!strcmp(mode, "cancel")) {
        /* ---- cycle 1: start, read ~10 %, cancel -------------------- */
        CHECK(sane_start_fn(handle) == SANE_STATUS_GOOD, "cycle1 sane_start");
        st = sane_get_parameters_fn(handle, &par);
        CHECK(st == SANE_STATUS_GOOD && par.pixels_per_line == REF_WIDTH &&
              par.lines == REF_HEIGHT, "cycle1 real parameters");
        st = read_all(handle, sane_read_fn, NULL, &total, CUT_SIZE);
        CHECK(st == SANE_STATUS_GOOD, "cycle1 read loop status");
        CHECK(total >= CUT_SIZE, "cycle1 read ~10 % of the page");
        printf("test_brscan5_faults: cycle1 read %zu B (~10 %% cut at %zu)\n",
               total, CUT_SIZE);

        sane_cancel_fn(handle);
        /* Reads after cancel must return SANE_STATUS_CANCELLED (T7). */
        {
            SANE_Byte probe[8];
            SANE_Int plen = 0;
            st = sane_read_fn(handle, probe, (SANE_Int)sizeof(probe), &plen);
            CHECK(st == SANE_STATUS_CANCELLED,
                  "read after cancel -> SANE_STATUS_CANCELLED");
        }

        /* ---- cycle 2: fresh sane_start WITHOUT sane_open ------------- */
        total = 0;
        CHECK(sane_start_fn(handle) == SANE_STATUS_GOOD,
              "cycle2 sane_start after cancel (no reopen)");
        st = sane_get_parameters_fn(handle, &par);
        CHECK(st == SANE_STATUS_GOOD && par.pixels_per_line == REF_WIDTH &&
              par.lines == REF_HEIGHT, "cycle2 real parameters");
        st = read_all(handle, sane_read_fn, fp, &total, 0);
        CHECK(st == SANE_STATUS_EOF, "cycle2 read loop ends with EOF");
        printf("test_brscan5_faults: cycle2 wrote %zu B\n", total);
    } else if (!strcmp(mode, "error")) {
        /* ---- start #1: transport error mid-data ---------------------- */
        st = sane_start_fn(handle);
        CHECK(st == SANE_STATUS_IO_ERROR,
              "start #1 fails with SANE_STATUS_IO_ERROR (0x04 injection)");
        printf("test_brscan5_faults: start #1 -> %d (IO_ERROR expected)\n",
               (int)st);

        sane_cancel_fn(handle);

        /* ---- start #2: the fixture's second cycle is clean ----------- */
        CHECK(sane_start_fn(handle) == SANE_STATUS_GOOD,
              "start #2 after error+cancel (no reopen)");
        st = read_all(handle, sane_read_fn, fp, &total, 0);
        CHECK(st == SANE_STATUS_EOF, "start #2 read loop ends with EOF");
        printf("test_brscan5_faults: cycle2 wrote %zu B\n", total);
    } else if (!strcmp(mode, "timeout")) {
        double t0 = now_s();
        st = sane_start_fn(handle);
        double dt = now_s() - t0;
        CHECK(st == SANE_STATUS_IO_ERROR,
              "sane_start fails with SANE_STATUS_IO_ERROR (timeout sim)");
        CHECK(dt < 5.0, "simulated timeout must not hang (<5 s, sleep cap)");
        printf("test_brscan5_faults: timeout path took %.3f s\n", dt);
        sane_cancel_fn(handle);
        {
            SANE_Byte probe[8];
            SANE_Int plen = 0;
            st = sane_read_fn(handle, probe, (SANE_Int)sizeof(probe), &plen);
            CHECK(st == SANE_STATUS_CANCELLED,
                  "read after cancel -> SANE_STATUS_CANCELLED");
        }
    } else if (!strcmp(mode, "nodocs")) {
        st = sane_start_fn(handle);
        CHECK(st == SANE_STATUS_NO_DOCS,
              "sane_start returns SANE_STATUS_NO_DOCS (CKD 00 02)");
        printf("test_brscan5_faults: sane_start -> %d (NO_DOCS expected)\n",
               (int)st);
        sane_cancel_fn(handle);
    } else if (!strcmp(mode, "jam")) {
        /* T8d: XSC 91 00 (device document jam, t8c-p6-jam.bin) must map
         * to the legacy DOCJAM convention: SANE_STATUS_JAMMED, whose
         * status string "Document feeder jammed" is what
         * paperless-scan.sh greps for. */
        st = sane_start_fn(handle);
        CHECK(st == SANE_STATUS_JAMMED,
              "sane_start returns SANE_STATUS_JAMMED (XSC 91 00)");
        printf("test_brscan5_faults: sane_start -> %d (JAMMED expected)\n",
               (int)st);
        sane_cancel_fn(handle);
    } else {
        fprintf(stderr, "unknown mode '%s'\n", mode);
        rc = 2;
        goto close;
    }

    rc = failures ? 1 : 0;
    if (!failures)
        printf("test_brscan5_faults: all in-process checks passed (%s)\n",
               mode);

close:
    if (handle)
        sane_close_fn(handle);
init_done:
    sane_exit_fn();
out:
    if (fp)
        fclose(fp);
    if (h)
        dlclose(h);
    return rc;
}
