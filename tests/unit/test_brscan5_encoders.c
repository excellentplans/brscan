/*
 * test_brscan5_encoders.c — byte-exact encoder/reader tests for the
 * brscan5 command layer (Brother DS-640).
 *
 * Golden vectors are byte-exact captures of the brscan5 command-layer
 * protocol (reverse-engineered from a Brother DS-640; see
 * docs/brscan5-protocol.md). Q/QDI/CKD are full
 * byte-exact vectors (text captures). SSP/XSC are full byte-exact
 * vectors since T8b (binary usbmon capture of the REFERENCE driver,
 * hw-window/t8b-capture-ref.pcapmon, empty feeder).
 *
 * NOTE (T8b correction): the T8a "SSP byte-exact for all 9 combos"
 * claim was wrong — those captures were OUR OWN port's output (the
 * encoder echoed itself). The real reference SSP body differs (CLR=
 * C24BIT, AREA=NORMAL, BRIT=50/CONT=50, no ATCL/THRS/RATE lines,
 * PAGE=0, TONE=ON, flags OFF); the device rejected the old body with
 * 83 53 53 50 00 00 00 00.
 *
 * Plain C, no test framework. Returns 0 on success, 1 on failure.
 */

#include <stdio.h>
#include <string.h>

#include "brother_brscan5.h"

static int failures = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        printf("FAIL: %s (line %d)\n", msg, __LINE__);                   \
        failures++;                                                      \
    }                                                                    \
} while (0)

/* ------------------------------------------------------------------ */

/* Golden vectors (hex strings from commands.golden.json). */
static const char *g_q_hex = "1b510a80";                         /* captured */
static const char *g_qdi_hex = "1b5144490a80";                   /* captured */
static const char *g_ckd_hex = "1b434b440a505352433d4144460a80"; /* captured */
/* SSP: 32-B prefix from the early text captures (valid for every
 * variant); the full T8b reference body is in test_t8b_reference_vectors. */
static const char *g_ssp_prefix_hex =
    "1b5353500a4f533d4c4e580a505352433d4144460a5245534f3d3330302c3330";
/* XSC: T8b reference vector (50 B, explicit full-area coords). */
static const char *g_xsc_hex =
    "1b5853430a5245534f3d3330302c3330300a415245413d302c302c32353530"
    "2c343230300a4d4f44453d4e4f524d414c0a80";

static int hex2bytes(const char *hex, unsigned char *out, int maxlen)
{
    int n = 0;
    unsigned int b;
    const char *p = hex;

    while (*p) {
        if (sscanf(p, "%2x", &b) != 1 || n >= maxlen)
            return -1;
        out[n++] = (unsigned char)b;
        p += 2;
    }
    return n;
}

/* Compare out[0..prefix) to golden, then check total length. */
static void check_encoder(const char *name,
                          int (*enc)(char *, int),
                          const char *golden_hex,
                          int golden_prefix_len,
                          int golden_total_len)
{
    unsigned char golden[512];
    char buf[512];
    int glen, len;

    glen = hex2bytes(golden_hex, golden, sizeof(golden));
    CHECK(glen > 0, "golden hex parse");

    len = enc(buf, (int)sizeof(buf));
    CHECK(len == golden_total_len, name);
    if (len != golden_total_len)
        return;

    /* golden_prefix_len == 0 means compare the whole vector. */
    if (golden_prefix_len == 0)
        golden_prefix_len = golden_total_len;
    CHECK(golden_prefix_len <= glen, "golden prefix within vector");
    CHECK(memcmp(buf, golden, (size_t)golden_prefix_len) == 0, name);

    /* Every command must be self-terminating (trailing 0x80). */
    CHECK((unsigned char)buf[len - 1] == 0x80, "trailing 0x80");

    /* Buffer-too-small must return 0. */
    CHECK(enc(buf, len - 1) == 0, "small buffer rejected");
}

/* ------------------------------------------------------------------ */

static void test_encoders(void)
{
    check_encoder("Q",   brscan5_enc_q,   g_q_hex,         0, 4);
    check_encoder("QDI", brscan5_enc_qdi, g_qdi_hex,       0, 6);
    check_encoder("CKD", brscan5_enc_ckd, g_ckd_hex,       0, 15);
    check_encoder("SSP", brscan5_enc_ssp, g_ssp_prefix_hex, 32, 278);
    check_encoder("XSC", brscan5_enc_xsc, g_xsc_hex,       0, 50);
}

/* ------------------------------------------------------------------ */
/* Dynamic SSP/XSC builders (T6).                                      */
/*                                                                     */
/* Default case (300 dpi, colour) must be byte-identical to the fixed  */
/* golden default — verified here against the *_dyn output at the      */
/* captured scenario, including the reconstructed body (the only       */
/* byte-exact reference we have; a binary capture (T8) will confirm    */
/* it end-to-end).                                                     */
/* ------------------------------------------------------------------ */

static void test_dyn_encoders_default(void)
{
    char a[512], b[512];
    int la, lb;

    la = brscan5_enc_ssp_dyn(a, (int)sizeof(a), 300, 300, COLOR_FUL,
                             50, 50, "NORMAL", 0);
    lb = brscan5_enc_ssp(b, (int)sizeof(b));
    CHECK(la == 278, "SSP dyn default length 278");
    CHECK(la == lb, "SSP dyn default == fixed wrapper");
    CHECK(la > 0 && memcmp(a, b, (size_t)la) == 0,
          "SSP dyn default byte-identical (300 dpi, colour)");

    la = brscan5_enc_xsc_dyn(a, (int)sizeof(a), 300, 300, "0,0,2550,4200");
    lb = brscan5_enc_xsc(b, (int)sizeof(b));
    CHECK(la == 50, "XSC dyn default length 50");
    CHECK(la == lb, "XSC dyn default == fixed wrapper");
    CHECK(la > 0 && memcmp(a, b, (size_t)la) == 0,
          "XSC dyn default byte-identical (300 dpi, full area)");

    /* Buffer-too-small must return 0. */
    CHECK(brscan5_enc_ssp_dyn(a, la - 1, 300, 300, COLOR_FUL,
                              50, 50, "NORMAL", 0) == 0,
          "SSP dyn small buffer rejected");
    CHECK(brscan5_enc_ssp_dyn(a, sizeof(a), 0, 300, COLOR_FUL,
                              50, 50, "NORMAL", 0) == 0,
          "SSP dyn invalid reso rejected");
    CHECK(brscan5_enc_ssp_dyn(a, sizeof(a), 300, 300, COLOR_FUL,
                              50, 50, NULL, 0) == 0,
          "SSP dyn NULL area rejected");
    CHECK(brscan5_enc_xsc_dyn(a, la - 1, 300, 300, "0,0,2550,4200") == 0,
          "XSC dyn small buffer rejected");
}

/* CLR=/COMP=/TONE= substitution per mode. Lengths computed from the
 * 278-B C24BIT reference (the CLR value replaces "C24BIT"; T8c live
 * verified via the chroot reference capture: B/W -> COMP=RLENGTH +
 * TONE=OFF, Gray -> COMP=JPEG + TONE=ON). */
static void test_dyn_encoders_variants(void)
{
    char buf[512];
    int len;
    static const struct {
        int color_type;
        const char *clr;
        const char *comp;
        const char *tone;
    } clr_cases[] = {
        { COLOR_BW,  "TEXT"    , "RLENGTH", "OFF" },
        { COLOR_ED,  "ERRDIF"  , "JPEG",    "ON"  },
        { COLOR_TG,  "GRAY256" , "JPEG",    "ON"  },
        { COLOR_256, "C256"    , "JPEG",    "ON"  },
        { COLOR_FUL, "C24BIT"  , "JPEG",    "ON"  },
    };
    size_t i;

    for (i = 0; i < sizeof(clr_cases)/sizeof(clr_cases[0]); i++) {
        char want[64];
        int want_len = 278 + (int)strlen(clr_cases[i].clr) - 6 /* C24BIT */
                           + (int)strlen(clr_cases[i].comp) - 4 /* JPEG */
                           + (int)strlen(clr_cases[i].tone) - 2 /* ON */;
        len = brscan5_enc_ssp_dyn(buf, (int)sizeof(buf), 300, 300,
                                  clr_cases[i].color_type, 50, 50, "NORMAL",
                                  0);
        CHECK(len == want_len, "SSP dyn variant length (CLR line size)");
        snprintf(want, sizeof(want), "\nCLR=%s\n", clr_cases[i].clr);
        CHECK(len > 0 && strstr(buf, want) != NULL, "SSP dyn CLR= value");
        snprintf(want, sizeof(want), "\nCOMP=%s\n", clr_cases[i].comp);
        CHECK(len > 0 && strstr(buf, want) != NULL, "SSP dyn COMP= value");
        snprintf(want, sizeof(want), "\nTONE=%s\n", clr_cases[i].tone);
        CHECK(len > 0 && strstr(buf, want) != NULL, "SSP dyn TONE= value");
        CHECK(len > 0 && buf[len - 1] == '\x80', "SSP dyn trailing 0x80");
    }

    /* RESO substitution (length grows by the extra digits at 1200 dpi). */
    len = brscan5_enc_ssp_dyn(buf, (int)sizeof(buf), 1200, 1200, COLOR_FUL,
                              50, 50, "NORMAL", 0);
    CHECK(len == 280, "SSP dyn 1200 dpi length 280");
    CHECK(len > 0 && strstr(buf, "RESO=1200,1200\n") != NULL,
          "SSP dyn RESO=1200,1200");

    len = brscan5_enc_xsc_dyn(buf, (int)sizeof(buf), 1200, 1200,
                              "0,0,10200,16800");
    CHECK(len == 54, "XSC dyn 1200 dpi length 54");
    CHECK(len > 0 && strstr(buf, "RESO=1200,1200\n") != NULL,
          "XSC dyn RESO=1200,1200");

    /* SSP and XSC must carry the same RESO value. */
    {
        char ssp[512], xsc[512];
        brscan5_enc_ssp_dyn(ssp, (int)sizeof(ssp), 600, 600, COLOR_TG,
                            50, 50, "NORMAL", 0);
        brscan5_enc_xsc_dyn(xsc, (int)sizeof(xsc), 600, 600, "ATDSKW");
        CHECK(strstr(ssp, "RESO=600,600\n") != NULL &&
              strstr(xsc, "RESO=600,600\n") != NULL,
              "SSP/XSC RESO consistent");
    }

    /* ATDSKW variant (usbmon4.log: XSC 43 B with AREA=ATDSKW). */
    len = brscan5_enc_xsc_dyn(buf, (int)sizeof(buf), 300, 300, "ATDSKW");
    CHECK(len == 43, "XSC dyn ATDSKW length 43");
    CHECK(len > 0 && strstr(buf, "AREA=ATDSKW\n") != NULL,
          "XSC dyn AREA=ATDSKW");

    /* COMP=NONE override (HWTEST; the env read moved to the session
     * side — the test passes the override directly, env-free). Only
     * COLOR_BW is affected: RLENGTH -> NONE, TONE stays OFF. */
    len = brscan5_enc_ssp_dyn(buf, (int)sizeof(buf), 300, 300, COLOR_BW,
                              50, 50, "NORMAL", 1);
    CHECK(len > 0 && strstr(buf, "\nCOMP=NONE\n") != NULL,
          "SSP dyn COMP=NONE override (B/W)");
    CHECK(len > 0 && strstr(buf, "\nTONE=OFF\n") != NULL,
          "SSP dyn TONE=OFF with COMP=NONE");
    len = brscan5_enc_ssp_dyn(buf, (int)sizeof(buf), 300, 300, COLOR_FUL,
                              50, 50, "NORMAL", 1);
    CHECK(len > 0 && strstr(buf, "\nCOMP=JPEG\n") != NULL,
          "SSP dyn COMP=NONE override ignored for colour");
}

/* ------------------------------------------------------------------ */
/* T8b: byte-exact SSP/XSC vectors from the REFERENCE driver's binary
 * usbmon capture (hw-window/t8b-capture-ref.pcapmon, empty feeder,
 * --mode '24bit Color' --resolution 300). The device answered the
 * 38-B SSP ack and XSC 90 00 to EXACTLY these bytes. The T8a 9-combo
 * "captured matrix" was our own port's output and is gone. */

static const char *g_ref_ssp_hex =
    "1b5353500a4f533d4c4e580a505352433d4144460a5245534f3d3330302c3330300a434c523d4332344249540a415245413d"
    "4e4f524d414c0a4d52474e3d302c302c302c300a44504c583d4f46460a425249543d35300a434f4e543d35300a434f4d503d"
    "4a5045470a4a53463d3432300a495052433d4e4f524d414c0a50545950453d4e4f524d414c0a504147453d300a4c4f4e473d"
    "4f46460a434152523d4f46460a524d47433d4f46460a445444463d4f46460a445434563d4f46460a44534b573d4f46460a4c"
    "534d443d4f46460a524d42503d4f46460a524d4d523d4f46460a474d4d413d4f46460a544f4e453d4f4e0a515446443d4f46460a4154434e3d4f46460a41544352503d4f46460a555345523d0a80"
    ;
static const char *g_ref_xsc_hex =
    "1b5853430a5245534f3d3330302c3330300a415245413d302c302c32353530"
    "2c343230300a4d4f44453d4e4f524d414c0a80";

static void test_t8b_reference_vectors(void)
{
    unsigned char want[600];
    char buf[600];
    int want_len, len;

    want_len = hex2bytes(g_ref_ssp_hex, want, sizeof(want));
    CHECK(want_len == 278, "T8b ref SSP vector length 278");
    len = brscan5_enc_ssp_dyn(buf, (int)sizeof(buf), 300, 300, COLOR_FUL,
                              50, 50, "NORMAL", 0);
    CHECK(len == want_len, "T8b SSP length");
    CHECK(len > 0 && memcmp(buf, want, (size_t)len) == 0,
          "T8b SSP byte-exact vs reference capture");

    want_len = hex2bytes(g_ref_xsc_hex, want, sizeof(want));
    CHECK(want_len == 50, "T8b ref XSC vector length 50");
    len = brscan5_enc_xsc_dyn(buf, (int)sizeof(buf), 300, 300,
                              "0,0,2550,4200");
    CHECK(len == want_len, "T8b XSC length");
    CHECK(len > 0 && memcmp(buf, want, (size_t)len) == 0,
          "T8b XSC byte-exact vs reference capture");
}

/* ------------------------------------------------------------------ */
/* T8c: byte-exact SSP/XSC vectors from the NATIVE T8c captures with
 * paper (content document). First complete SSP/XSC pair of each
 * t8c-p3-native-*-doc.bin capture (raw usbmon search; the captures
 * contain interleaved event copies of parallel reader sessions, see
 * T8C-HANDOFF.md section 8). All 9 RESO x CLR combos are covered; the
 * device accepted EXACTLY these bytes (SSP ack + XSC 90 00/data
 * start). XSC AREA is in pixels: 0,0,<w>,<h> with h=8399 at 600 dpi
 * (not 8400) - captured verbatim. */
#include "t8c_vectors.inc"

static void test_t8c_byte_exact_variants(void)
{
    unsigned char want[1024];
    char buf[1024];
    int want_len, len, i;

    for (i = 0; i < (int)(sizeof(t8c_vecs)/sizeof(t8c_vecs[0])); i++) {
        want_len = hex2bytes(t8c_vecs[i].ssp, want, sizeof(want));
        CHECK(want_len > 0, "T8c SSP vector parses");
        len = brscan5_enc_ssp_dyn(buf, (int)sizeof(buf), t8c_vecs[i].reso,
                                  t8c_vecs[i].reso, t8c_vecs[i].ct,
                                  50, 50, "NORMAL", 0);
        CHECK(len == want_len, "T8c SSP length");
        CHECK(len > 0 && memcmp(buf, want, (size_t)len) == 0,
              "T8c SSP byte-exact vs native capture");

        want_len = hex2bytes(t8c_vecs[i].xsc, want, sizeof(want));
        CHECK(want_len > 0, "T8c XSC vector parses");
        len = brscan5_enc_xsc_dyn(buf, (int)sizeof(buf), t8c_vecs[i].reso,
                                  t8c_vecs[i].reso, t8c_vecs[i].area_xsc);
        CHECK(len == want_len, "T8c XSC length");
        CHECK(len > 0 && memcmp(buf, want, (size_t)len) == 0,
              "T8c XSC byte-exact vs native capture");
    }
}

/* ------------------------------------------------------------------ */

static void test_response_readers(void)
{
    unsigned char rsp[1024];

    /* Q: 75 B beginning c1 00 49 10 00 17 (usbmon capture). */
    memset(rsp, 0x00, sizeof(rsp));
    rsp[0] = 0xc1; rsp[1] = 0x00; rsp[2] = 0x49; rsp[3] = 0x10; rsp[4] = 0x00;
    rsp[5] = 0x17;
    CHECK(brscan5_rsp_q(rsp, 75) == 0, "Q: 75-B response OK");
    CHECK(brscan5_rsp_q(rsp, 74) == -1, "Q: short response rejected");
    rsp[0] = 0xc2;
    CHECK(brscan5_rsp_q(rsp, 75) == -1, "Q: wrong magic rejected");
    rsp[0] = 0xc1;

    /* QDI: 00 51 44 49 <len 0x028e LE> 00 00 + 654-B payload. */
    memset(rsp, 0x00, sizeof(rsp));
    rsp[0] = 0x00; rsp[1] = 'Q'; rsp[2] = 'D'; rsp[3] = 'I';
    rsp[4] = 0x8e; rsp[5] = 0x02;
    {
        const unsigned char *payload = NULL;
        CHECK(brscan5_rsp_qdi(rsp, 662, &payload) == 0, "QDI: 662-B response OK");
        CHECK(payload == rsp + 8, "QDI: payload pointer after header");
        CHECK(brscan5_rsp_qdi(rsp, 661, NULL) == -1, "QDI: short response rejected");
        rsp[3] = 'X';
        CHECK(brscan5_rsp_qdi(rsp, 662, NULL) == -1, "QDI: wrong magic rejected");
        rsp[3] = 'I';
    }

    /* CKD: 00 01 = paper present, 00 02 = feeder empty. */
    {
        unsigned char ckd[] = { 0x00, 0x01 };
        CHECK(brscan5_rsp_ckd(ckd, 2) == 1, "CKD: 00 01 = paper");
        ckd[1] = 0x02;
        CHECK(brscan5_rsp_ckd(ckd, 2) == 0, "CKD: 00 02 = empty");
        CHECK(brscan5_rsp_ckd(ckd, 1) == -1, "CKD: short rejected");
        ckd[1] = 0x03;
        CHECK(brscan5_rsp_ckd(ckd, 2) == -1, "CKD: unknown status rejected");
    }

    /* SSP: 38 B beginning 00 53 53 50 1e (usbmon capture). */
    memset(rsp, 0x00, sizeof(rsp));
    rsp[0] = 0x00; rsp[1] = 'S'; rsp[2] = 'S'; rsp[3] = 'P'; rsp[4] = 0x1e;
    CHECK(brscan5_rsp_ssp(rsp, 38) == 0, "SSP: 38-B response OK");
    CHECK(brscan5_rsp_ssp(rsp, 37) == -1, "SSP: short rejected");

    /* XSC: 90 00 = empty feeder, 14-B 00 02 record = scan started. */
    {
        unsigned char empty[] = { 0x90, 0x00 };
        unsigned char started[14] = { 0x00, 0x02 };
        CHECK(brscan5_rsp_xsc(empty, 2) == 0, "XSC: 90 00 = empty");
        CHECK(brscan5_rsp_xsc(started, 14) == 1, "XSC: 00 02 14-B = started");
        CHECK(brscan5_rsp_xsc(started, 13) == -1, "XSC: short 00 02 rejected");
        CHECK(brscan5_rsp_xsc(empty, 1) == -1, "XSC: 1-B response rejected");
    }
}

/* ------------------------------------------------------------------ */
/* T8b: vendor control dance (CKD→SSP). Golden vectors from the usbmon
 * captures: setup packets from usbmon4.log/usbmon-scan.log (reference
 * driver, wValue 0x0002 = BCOMMAND_SCANNER, wLength 0x00ff), responses
 * `05 10 <bReq> 02 00` (open: usbmon4.log L10/L26; close: binary
 * capture pass1-ckd — our CloseDevice, same 5-B shape). The SSP
 * rejection vector 83 53 53 50 … is the T8a pass2 capture (our port
 * without the dance). */

static void test_control_dance(void)
{
    unsigned char setup[8];
    unsigned char open_rsp[]  = { 0x05, 0x10, 0x01, 0x02, 0x00 };
    unsigned char close_rsp[] = { 0x05, 0x10, 0x02, 0x02, 0x00 };
    static const unsigned char open_want[]  =
        { 0xc0, 0x01, 0x02, 0x00, 0x00, 0x00, 0xff, 0x00 };
    static const unsigned char close_want[] =
        { 0xc0, 0x02, 0x02, 0x00, 0x00, 0x00, 0xff, 0x00 };

    CHECK(brscan5_enc_ctrl_open(setup) == 8, "ctrl open setup length");
    CHECK(memcmp(setup, open_want, 8) == 0, "ctrl open setup bytes");
    CHECK(brscan5_enc_ctrl_close(setup) == 8, "ctrl close setup length");
    CHECK(memcmp(setup, close_want, 8) == 0, "ctrl close setup bytes");
    CHECK(brscan5_enc_ctrl_open(NULL) == 0, "ctrl open NULL rejected");

    CHECK(brscan5_rsp_ctrl(open_rsp, 5, 0x01) == 0, "ctrl open rsp OK");
    CHECK(brscan5_rsp_ctrl(close_rsp, 5, 0x02) == 0, "ctrl close rsp OK");
    CHECK(brscan5_rsp_ctrl(open_rsp, 4, 0x01) == -1, "ctrl rsp short");
    CHECK(brscan5_rsp_ctrl(open_rsp, 5, 0x02) == -1, "ctrl rsp wrong echo");
    close_rsp[1] = 0x11;
    CHECK(brscan5_rsp_ctrl(close_rsp, 5, 0x02) == -1, "ctrl rsp bad type");
    close_rsp[1] = 0x10;
    close_rsp[4] = 0x01;
    CHECK(brscan5_rsp_ctrl(close_rsp, 5, 0x02) == -1, "ctrl rsp bad status");
    close_rsp[4] = 0x00;

    /* SSP rejection (0x83) vs. the 38-B ack. */
    {
        unsigned char reject[8] =
            { 0x83, 'S', 'S', 'P', 0x00, 0x00, 0x00, 0x00 };
        unsigned char ack[38] = { 0x00, 'S', 'S', 'P', 0x1e };
        CHECK(brscan5_rsp_ssp_rejected(reject, 8) == 1,
              "SSP: 83 53 53 50 … = rejected");
        CHECK(brscan5_rsp_ssp_rejected(ack, 38) == 0,
              "SSP: 38-B ack not flagged rejected");
        CHECK(brscan5_rsp_ssp_rejected(reject, 3) == 0,
              "SSP: short buffer not flagged rejected");
        CHECK(brscan5_rsp_ssp(reject, 8) == -1,
              "SSP: rejection is not a valid ack");
    }
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_encoders();
    test_dyn_encoders_default();
    test_dyn_encoders_variants();
    test_t8b_reference_vectors();
    test_t8c_byte_exact_variants();
    test_response_readers();
    test_control_dance();

    if (failures) {
        printf("test_brscan5_encoders: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_brscan5_encoders: all tests passed\n");
    return 0;
}