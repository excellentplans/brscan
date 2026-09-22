/*
 * test_brscan5_replay_bw.c — T8d end-to-end replay test for the B/W
 * (COMP=RLENGTH) data path: loads the built backend .so, selects mode
 * "Black & White" and runs the full SANE lifecycle against the BW-300
 * replay fixture (fixture-bw300.tlv, generated from the T8c chroot
 * reference capture by make_replay_fixture.py --bw300).
 *
 * The page arrives as one packbits-compressed 00 01 block record per
 * scanline (no JPEG/EOI in this mode); sane_start must report the real
 * geometry (1-bit GRAY frame, bytes_per_line == (width+7)/8) and sane_read
 * must stream the decoded bitmap until EOF.
 *
 * Usage: test_brscan5_replay_bw <backend.so> <outfile> [device]
 * Env:    BROTHER5_REPLAY=<fixture.tlv>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sane/sane.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", (msg), __LINE__); \
        failures++; \
    } \
} while (0)

int
main(int argc, char **argv)
{
    const char *so_path, *out_path, *dev;
    void *h = NULL;
    FILE *fp = NULL;
    unsigned char *buf;
    const size_t BUF_SZ = 1u << 20;
    size_t total = 0;
    int rc = 1;
    SANE_Handle handle = NULL;
    SANE_Parameters par;
    SANE_Status st;
    SANE_Int info = 0;
    SANE_Int reso300 = 300;
    const char *mode_bw = "Black & White";

    if (argc < 3) {
        fprintf(stderr, "usage: %s <backend.so> <outfile> [device]\n",
                argv[0]);
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

    st = sane_control_option_fn(handle, 3 /*optResolution*/,
                                SANE_ACTION_SET_VALUE, &reso300, &info);
    CHECK(st == SANE_STATUS_GOOD, "set resolution 300");
    st = sane_control_option_fn(handle, 2 /*optMode*/,
                                SANE_ACTION_SET_VALUE,
                                (void *)mode_bw, &info);
    CHECK(st == SANE_STATUS_GOOD, "set mode Black & White");

    if (sane_start_fn(handle) != SANE_STATUS_GOOD) {
        fprintf(stderr, "sane_start failed\n");
        goto close;
    }

    /* The RLENGTH page is complete after sane_start (eager pump): the
     * real geometry is known — 1-bit GRAY frame, packed rows. */
    st = sane_get_parameters_fn(handle, &par);
    CHECK(st == SANE_STATUS_GOOD, "get_parameters after start");
    CHECK(par.format == SANE_FRAME_GRAY, "post-start: GRAY frame");
    CHECK(par.depth == 1, "post-start: depth 1");
    CHECK(par.pixels_per_line > 0 && par.lines > 0,
          "post-start: real dimensions positive");
    CHECK(par.bytes_per_line == (par.pixels_per_line + 7) / 8,
          "post-start: bytes_per_line == (width+7)/8 (packed rows)");
    printf("test_brscan5_replay_bw: post-start params %dx%d bpl=%d "
           "(RLENGTH real)\n", par.pixels_per_line, par.lines,
           par.bytes_per_line);

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
    printf("test_brscan5_replay_bw: wrote %zu B to %s\n", total, out_path);
    CHECK(total == (size_t)par.lines * (size_t)par.bytes_per_line,
          "output size == lines * bytes_per_line");

    rc = failures ? 1 : 0;
    if (!failures)
        printf("test_brscan5_replay_bw: all in-process checks passed\n");

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
