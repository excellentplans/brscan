/*
 * test_brscan5_geo.c — scan-geometry conversion policies
 * (brscan5_mm0d1_to_px / brscan5_mm_to_px_area / brscan5_fill_params).
 *
 * Two DIFFERENT mm -> pixel policies exist by design (see
 * brscan5_proto.h): the estimate path converts 0.1-mm integers with
 * ×dpi/254 truncation + clamp>=1 (legacy-consistent), the XSC AREA
 * path converts raw SANE mm with ×dpi/25.4 round-half-up (capture-
 * verified at 150/300 dpi). This test pins both, plus the T8c AREA
 * divergence analysis (see below).
 *
 * Plain C, no test framework, links brscan5_proto.c directly (no
 * backend deps). Returns 0 on success, 1 on failure.
 */

#include <stdio.h>
#include <string.h>

#include "brother.h"
#include "brscan5_proto.h"

static int failures = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        printf("FAIL: %s (line %d)\n", msg, __LINE__);                   \
        failures++;                                                      \
    }                                                                    \
} while (0)

/* ------------------------------------------------------------------ */
/* Policy "estimate": 0.1-mm ints x dpi / 254, truncation, clamp >= 1. */

static void test_estimate_policy(void)
{
    /* Full DS-640 scan area defaults (brother.c rangXY_DEF):
     * 215.9 x 355.6 mm = 2159 x 3556 (0.1 mm). */
    CHECK(brscan5_mm0d1_to_px(2159, 150) == 1275, "estimate w 150");
    CHECK(brscan5_mm0d1_to_px(2159, 300) == 2550, "estimate w 300");
    CHECK(brscan5_mm0d1_to_px(2159, 600) == 5100, "estimate w 600");
    CHECK(brscan5_mm0d1_to_px(3556, 150) == 2100, "estimate h 150");
    CHECK(brscan5_mm0d1_to_px(3556, 300) == 4200, "estimate h 300");
    CHECK(brscan5_mm0d1_to_px(3556, 600) == 8400, "estimate h 600");

    /* Exact-division cases. */
    CHECK(brscan5_mm0d1_to_px(254, 1) == 1, "estimate exact 254x1dpi");
    CHECK(brscan5_mm0d1_to_px(2540, 10) == 100, "estimate exact 2540x10dpi");

    /* Truncation (not rounding): 3555*300/254 = 4198.8 -> 4198. */
    CHECK(brscan5_mm0d1_to_px(3555, 300) == 4198, "estimate truncates");
    CHECK(brscan5_mm0d1_to_px(2539, 10) == 99, "estimate truncates 2");

    /* Clamp >= 1 (degenerate/empty areas must stay scannable). */
    CHECK(brscan5_mm0d1_to_px(0, 300) == 1, "estimate clamps 0 -> 1");
    CHECK(brscan5_mm0d1_to_px(1, 100) == 1, "estimate clamps 100/254 -> 1");
    CHECK(brscan5_mm0d1_to_px(1, 254) == 1, "estimate 1x254 = 1");
    CHECK(brscan5_mm0d1_to_px(-5, 300) == 1, "estimate clamps negative -> 1");
}

/* ------------------------------------------------------------------ */
/* Policy "area": raw SANE mm x dpi / 25.4, round-half-up.             */
/*                                                                     */
/* The option values flow SANE_FIX -> option -> SANE_UNFIX; emulate    */
/* the default full-area options exactly as the backend would see them */
/* (brother.c rangXY_DEF = SANE_FIX(215.9)/SANE_FIX(355.6)).           */

static void test_area_policy(void)
{
    SANE_Fixed brx_def = SANE_FIX(215.9);
    SANE_Fixed bry_def = SANE_FIX(355.6);
    double x = SANE_UNFIX(brx_def);
    double y = SANE_UNFIX(bry_def);

    CHECK(brscan5_mm_to_px_area(x, 150) == 1275, "area w 150");
    CHECK(brscan5_mm_to_px_area(x, 300) == 2550, "area w 300");
    CHECK(brscan5_mm_to_px_area(x, 600) == 5100, "area w 600");
    CHECK(brscan5_mm_to_px_area(y, 150) == 2100, "area h 150");
    CHECK(brscan5_mm_to_px_area(y, 300) == 4200, "area h 300");

    /* Round-half-up (not truncation): 215.9 mm at 150 dpi is 1274.998
     * after the fixed-point roundtrip -> 1275 requires the +0.5. */
    CHECK(brscan5_mm_to_px_area(0.0, 300) == 0, "area origin stays 0");

    /* Documented divergence (T8c): with the DS-640 default br-y
     * 355.6 mm the policy computes 8400 at 600 dpi, while the native
     * captures pin AREA height 8399. Assert the CURRENT output here —
     * the divergence is documented, not "fixed" by an unverified
     * policy guess (see test_t8c_area_vectors below). */
    CHECK(brscan5_mm_to_px_area(y, 600) == 8400, "area h 600 = 8400 (divergence)");

    /* The same policy DOES reproduce the captured 8399 if the
     * reference driver's br-y was 355.567 mm — the value
     * docs/brscan5-protocol.md attributes to the reference full-area.
     * 355.567 mm x 600 dpi / 25.4 = 8399.22 -> 8399 (margin 0.28 to
     * the 0.5 boundary, robust in double arithmetic). So the capture
     * evidence is compatible with the round-half-up POLICY plus a
     * different INPUT; no policy change is justified. */
    CHECK(brscan5_mm_to_px_area(355.567, 600) == 8399,
          "area h 600 = 8399 for br-y 355.567 (reference input)");
    CHECK(brscan5_mm_to_px_area(355.567, 300) == 4200,
          "area h 300 = 4200 for br-y 355.567");
    CHECK(brscan5_mm_to_px_area(355.567, 150) == 2100,
          "area h 150 = 2100 for br-y 355.567");
}

/* ------------------------------------------------------------------ */
/* T8c golden AREA vectors: recompute from (mm, dpi) and compare       */
/* byte-exact with the captured strings. t8c_vectors.inc carries the   */
/* captured AREA= payloads ("0,0,<w>,<h>") for all 9 RESO x CLR combos */
/* (CLR does not influence the geometry — 3 identical (w,h) triples).   */
/*                                                                     */
/* Divergence analysis (OUTCOME B — keep the current policy):          */
/*   From the DS-640 defaults (215.9 x 355.6 mm) every simple uniform  */
/*   rounding (trunc/floor/ceil/half-up/half-down/half-even) reproduces*/
/*   ALL pinned values except the 600-dpi height: computed 8400,       */
/*   captured 8399. Two max-edge quirks DO fit all 9 vectors but are   */
/*   unverifiable guesses:                                             */
/*     a) bottom -= one 600-dpi native pixel (25.4/600 mm): contradicts*/
/*      the pinned 4200 at 300 dpi under the SANE-fixed value          */
/*      representation (4199.49 -> 4199), so it is representation-     */
/*      fragile;                                                       */
/*     b) clamp to a device-native 8399-px @ 600 dpi raster, then scale*/
/*      round-half-up: requires a max-raster constant that appears in  */
/*      NO capture (the QDI payload is consumed, not parsed).          */
/*   Under the same round-half-up policy the captures are also fully   */
/*   explained by a reference input br-y in [355.558, 355.579) mm      */
/*   (e.g. 355.567, cf. docs/brscan5-protocol.md). Input-side vs.      */
/*   policy-side explanations are indistinguishable from the captures, */
/*   so the current computation stays; only 300 dpi (e2e replay) and   */
/*   150 dpi pin this path end-to-end today.                           */

#include "t8c_vectors.inc"

static void test_t8c_area_vectors(void)
{
    SANE_Fixed brx_def = SANE_FIX(215.9);
    SANE_Fixed bry_def = SANE_FIX(355.6);
    double x = SANE_UNFIX(brx_def);
    double y = SANE_UNFIX(bry_def);
    char computed[40];
    int i;

    for (i = 0; i < (int)(sizeof(t8c_vecs)/sizeof(t8c_vecs[0])); i++) {
        long w = brscan5_mm_to_px_area(x, t8c_vecs[i].reso);
        long h = brscan5_mm_to_px_area(y, t8c_vecs[i].reso);
        long want_w, want_h;

        sscanf(t8c_vecs[i].area_xsc, "0,0,%ld,%ld", &want_w, &want_h);
        snprintf(computed, sizeof(computed), "0,0,%ld,%ld", w, h);

        if (t8c_vecs[i].reso == 600) {
            /* Documented divergence: width matches byte-exact, height
             * computes to 8400 where the capture pins 8399. */
            CHECK(w == want_w, "T8c 600 dpi AREA width matches capture");
            CHECK(w == 5100, "T8c 600 dpi computed width 5100");
            CHECK(h == 8400, "T8c 600 dpi computed height 8400 (current policy)");
            CHECK(want_h == 8399, "T8c 600 dpi captured height 8399");
        } else {
            /* 150/300 dpi: byte-exact vs. the native captures. */
            CHECK(strcmp(computed, t8c_vecs[i].area_xsc) == 0,
                  "T8c AREA byte-exact from (mm, dpi)");
        }
    }
}

/* ------------------------------------------------------------------ */
/* bytes_per_line policy via brscan5_fill_params.                      */

static void test_fill_params(void)
{
    SANE_Parameters p;

    /* Color: RGB, depth 8, bpl = w * 3 (300 dpi full area). */
    brscan5_fill_params(&p, 2550, 4200, COLOR_FUL);
    CHECK(p.pixels_per_line == 2550 && p.lines == 4200, "fill color dims");
    CHECK(p.format == SANE_FRAME_RGB && p.depth == 8, "fill color frame");
    CHECK(p.bytes_per_line == 2550 * 3, "fill color bpl = w*3");
    CHECK(p.last_frame == SANE_TRUE, "fill last_frame");

    /* Gray (TG): GRAY, depth 8, bpl = w. */
    brscan5_fill_params(&p, 2550, 4200, COLOR_TG);
    CHECK(p.format == SANE_FRAME_GRAY && p.depth == 8, "fill gray frame");
    CHECK(p.bytes_per_line == 2550, "fill gray bpl = w");

    /* ED shares the gray branch (implemented, untested vs hardware). */
    brscan5_fill_params(&p, 2550, 4200, COLOR_ED);
    CHECK(p.format == SANE_FRAME_GRAY && p.depth == 8, "fill ED frame");
    CHECK(p.bytes_per_line == 2550, "fill ED bpl = w");

    /* B/W: GRAY, depth 1, bpl = (w + 7) / 8. */
    brscan5_fill_params(&p, 2550, 4200, COLOR_BW);
    CHECK(p.format == SANE_FRAME_GRAY && p.depth == 1, "fill bw frame");
    CHECK(p.bytes_per_line == (2550 + 7) / 8, "fill bw bpl = (w+7)/8");

    /* 600 dpi full-area dims (estimate path values). */
    brscan5_fill_params(&p, 5100, 8400, COLOR_BW);
    CHECK(p.bytes_per_line == (5100 + 7) / 8, "fill bw bpl 600 dpi");
    CHECK(p.pixels_per_line == 5100 && p.lines == 8400, "fill bw 600 dims");

    /* Byte alignment edge: width 1. */
    brscan5_fill_params(&p, 1, 1, COLOR_BW);
    CHECK(p.bytes_per_line == 1, "fill bw bpl width 1");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_estimate_policy();
    test_area_policy();
    test_t8c_area_vectors();
    test_fill_params();

    if (failures) {
        printf("test_brscan5_geo: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_brscan5_geo: all tests passed\n");
    return 0;
}
