/*
 * brscan5_proto.c — pure brscan5 protocol module (Brother DS-640).
 *
 * Command encoders and response readers, split out of
 * brother_brscan5.c: no USB transport, no libjpeg, no backend session
 * state, no logging — unit-testable standalone
 * (tests/unit/test_brscan5_encoders.c).
 *
 * Protocol reference: docs/brscan5-protocol.md.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "brother.h"
#include "brscan5_proto.h"

/* ======================================================================
 * Command vectors
 *
 * Byte-exact vs. commands.golden.json. Q/QDI/CKD are fully captured;
 * XSC is prefix-captured (first 32 B) with the MODE=NORMAL\n\x80 tail
 * reconstructed; SSP is prefix-captured (first 32 B) with the body
 * reconstructed from the MakeSSPcmdString builder order in
 * libLxBsScanCoreApi.so.3.2.6. The SSP total length (278 B) is observed
 * in the usbmon traces (S Bo:1:007:4 -115 278).
 * ====================================================================== */

/* ESC Q\n\x80 — 4 B (captured). */
#define BRSCAN5_Q_CMD       "\x1bQ\n\x80"
#define BRSCAN5_Q_LEN       4

/* ESC QDI\n\x80 — 6 B (captured). */
#define BRSCAN5_QDI_CMD     "\x1bQDI\n\x80"
#define BRSCAN5_QDI_LEN     6

/* ESC CKD\nPSRC=ADF\n\x80 — 15 B (captured).
 * Note: "\x1bCKD" must be split — \x1b followed by 'C' (a hex digit)
 * would parse as a single out-of-range hex escape. */
#define BRSCAN5_CKD_CMD     "\x1b" "CKD\nPSRC=ADF\n\x80"
#define BRSCAN5_CKD_LEN     15

/* SSP/XSC are built dynamically since T6 (brscan5_enc_ssp_dyn() /
 * brscan5_enc_xsc_dyn() below): RESO and CLR follow the configured
 * options; the 300 dpi colour default is byte-identical to the golden
 * vectors (278/43 B, verified by tests/unit/test_brscan5_encoders.c).
 * Provenance: 32-B prefix captured, body reconstructed from the
 * MakeSSPcmdString builder order in libLxBsScanCoreApi.so.3.2.6; the SSP
 * total length 278 B is observed in the usbmon traces
 * (S Bo:1:007:4 -115 278). */

/* ======================================================================
 * Command encoders
 *
 * SSP/XSC are built dynamically (T6): RESO and the CLR= line follow the
 * configured scan options. The default scenario (300 dpi, colour) must
 * stay byte-identical to the captured/reconstructed golden vectors —
 * brscan5_enc_ssp()/brscan5_enc_xsc() are the fixed-default wrappers used
 * by the golden tests; the session side (brscan5_start) uses the *_dyn()
 * builders.
 * ====================================================================== */

int
brscan5_enc_q(char *buf, int bufsz)
{
    if (bufsz < BRSCAN5_Q_LEN)
        return 0;
    memcpy(buf, BRSCAN5_Q_CMD, BRSCAN5_Q_LEN);
    return BRSCAN5_Q_LEN;
}

int
brscan5_enc_qdi(char *buf, int bufsz)
{
    if (bufsz < BRSCAN5_QDI_LEN)
        return 0;
    memcpy(buf, BRSCAN5_QDI_CMD, BRSCAN5_QDI_LEN);
    return BRSCAN5_QDI_LEN;
}

int
brscan5_enc_ckd(char *buf, int bufsz)
{
    if (bufsz < BRSCAN5_CKD_LEN)
        return 0;
    memcpy(buf, BRSCAN5_CKD_CMD, BRSCAN5_CKD_LEN);
    return BRSCAN5_CKD_LEN;
}

/* CLR= value for a scan mode (COLOR_* from brother.h). T8b: C24BIT is
 * reference-proven (t8b-capture-ref.pcapmon, --mode '24bit Color');
 * TEXT/GRAY256 follow the mode names, ERRDIF/C256 remain reconstructed. */
static const char *
brscan5_clr_string(int color_type)
{
    switch (color_type) {
    case COLOR_BW:  return "TEXT";     /* 1-bit black & white */
    case COLOR_ED:  return "ERRDIF";   /* error-diffusion gray */
    case COLOR_TG:  return "GRAY256";  /* 8-bit gray */
    case COLOR_256: return "C256";     /* 256 colour (unsupported by us) */
    default:        return "C24BIT";   /* COLOR_FUL / _NOCM: 24-bit colour */
    }
}

/* SSP/XSC field layout per the T8b reference-driver capture
 * (hw-window/t8b-capture-ref.pcapmon, 278 B at 300 dpi / C24BIT /
 * AREA=NORMAL / BRIT=50 / CONT=50): NO ATCL/THRS/RATE lines, PAGE=0,
 * TONE=ON, flag fields are "OFF" (not 0), USER= carries a trailing \n
 * before the 0x80 terminator. */
int
brscan5_enc_ssp_dyn(char *buf, int bufsz,
                    int reso_x, int reso_y, int color_type,
                    int brit, int cont, const char *area,
                    int comp_override)
{
    int len;
    /* T8c live-verified (t8c-phase2-chroot-ref.bin, SSP OUT per mode):
     * Color -> COMP=JPEG TONE=ON, Gray -> COMP=JPEG TONE=ON,
     * B/W   -> COMP=RLENGTH TONE=OFF. COMP=NONE for B/W is a
     * caller-provided comp_override (e.g. tests/hardware experiments)
     * — device support unknown. */
    const char *comp = "JPEG";
    const char *tone = "ON";
    if (color_type == COLOR_BW) {
        comp = comp_override ? "NONE" : "RLENGTH";
        tone = "OFF";
    }

    if (!buf || bufsz <= 0 || reso_x <= 0 || reso_y <= 0 ||
        !area || !area[0] || brit < 0 || brit > 255 ||
        cont < 0 || cont > 255)
        return 0;
    len = snprintf(buf, (size_t)bufsz,
        "\x1bSSP\n"
        "OS=LNX\nPSRC=ADF\nRESO=%d,%d\nCLR=%s\nAREA=%s\nMRGN=0,0,0,0\n"
        "DPLX=OFF\nBRIT=%d\nCONT=%d\nCOMP=%s\nJSF=420\nIPRC=NORMAL\n"
        "PTYPE=NORMAL\nPAGE=0\nLONG=OFF\nCARR=OFF\nRMGC=OFF\nDTDF=OFF\n"
        "DT4V=OFF\nDSKW=OFF\nLSMD=OFF\nRMBP=OFF\nRMMR=OFF\nGMMA=OFF\n"
        "TONE=%s\nQTFD=OFF\nATCN=OFF\nATCRP=OFF\nUSER=\n\x80",
        reso_x, reso_y, brscan5_clr_string(color_type), area, brit, cont,
        comp, tone);
    if (len < 0 || len >= bufsz)
        return 0;
    return len;
}

int
brscan5_enc_xsc_dyn(char *buf, int bufsz, int reso_x, int reso_y,
                    const char *area)
{
    int len;

    if (!buf || bufsz <= 0 || reso_x <= 0 || reso_y <= 0 ||
        !area || !area[0])
        return 0;
    len = snprintf(buf, (size_t)bufsz,
        "\x1bXSC\nRESO=%d,%d\nAREA=%s\nMODE=NORMAL\n\x80",
        reso_x, reso_y, area);
    if (len < 0 || len >= bufsz)
        return 0;
    return len;
}

/* Fixed-default wrappers: byte-identical to the T8b reference vectors at
 * the captured scenario (300 dpi, colour, full area — verified by the
 * unit test). */
int
brscan5_enc_ssp(char *buf, int bufsz)
{
    return brscan5_enc_ssp_dyn(buf, bufsz, 300, 300, COLOR_FUL,
                               50, 50, "NORMAL", 0);
}

int
brscan5_enc_xsc(char *buf, int bufsz)
{
    return brscan5_enc_xsc_dyn(buf, bufsz, 300, 300, "0,0,2550,4200");
}

/* ---- vendor control setup packets (T8b) ------------------------------- */

int
brscan5_enc_ctrl_open(unsigned char setup[8])
{
    if (!setup)
        return 0;
    memcpy(setup, BRSCAN5_CTRL_OPEN, BRSCAN5_CTRL_SETUP_LEN);
    return BRSCAN5_CTRL_SETUP_LEN;
}

int
brscan5_enc_ctrl_close(unsigned char setup[8])
{
    if (!setup)
        return 0;
    memcpy(setup, BRSCAN5_CTRL_CLOSE, BRSCAN5_CTRL_SETUP_LEN);
    return BRSCAN5_CTRL_SETUP_LEN;
}

/* ======================================================================
 * Response readers
 * ====================================================================== */

int
brscan5_rsp_q(const unsigned char *buf, int len)
{
    static const unsigned char magic[] = { 0xc1, 0x00, 0x49, 0x10 };

    if (len < BRSCAN5_RSP_Q_LEN)
        return -1;
    if (memcmp(buf, magic, sizeof(magic)) != 0)
        return -1;
    return 0;
}

int
brscan5_rsp_qdi(const unsigned char *buf, int len,
                const unsigned char **payload)
{
    int plen;

    if (len < 8)
        return -1;
    if (buf[0] != 0x00 || buf[1] != 'Q' || buf[2] != 'D' || buf[3] != 'I')
        return -1;
    plen = buf[4] | (buf[5] << 8);      /* little-endian payload length */
    if (8 + plen > len)
        return -1;
    if (payload)
        *payload = buf + 8;
    return 0;
}

int
brscan5_rsp_ckd(const unsigned char *buf, int len)
{
    if (len < 2)
        return -1;
    if (buf[0] != 0x00)
        return -1;
    if (buf[1] == 0x01)
        return 1;                       /* paper present */
    if (buf[1] == 0x02)
        return 0;                       /* feeder empty */
    return -1;
}

int
brscan5_rsp_ssp(const unsigned char *buf, int len)
{
    static const unsigned char magic[] = { 0x00, 0x53, 0x53, 0x50, 0x1e };

    if (len < BRSCAN5_RSP_SSP_LEN)
        return -1;
    if (memcmp(buf, magic, sizeof(magic)) != 0)
        return -1;
    return 0;
}

/* SSP rejection (T8b): 8 B `83 53 53 50 00 00 00 00` — status 0x83 =
 * rejected (device state: vendor control dance missing). */
int
brscan5_rsp_ssp_rejected(const unsigned char *buf, int len)
{
    return (len >= 8 && buf[0] == 0x83 &&
            buf[1] == 'S' && buf[2] == 'S' && buf[3] == 'P');
}

/* Vendor control response: 5 B `05 10 <breq> 02 00` (breq echo of the
 * request number; 0x10 = BDESC_TYPE, 02 = BCOMMAND_SCANNER echo). */
int
brscan5_rsp_ctrl(const unsigned char *buf, int len, int breq)
{
    if (len < BRSCAN5_CTRL_RSP_LEN)
        return -1;
    if (buf[0] != BRSCAN5_CTRL_RSP_LEN ||   /* 5: descriptor length   */
        buf[1] != 0x10 ||                   /* BDESC_TYPE             */
        buf[2] != (unsigned char)breq ||    /* request echo           */
        buf[3] != 0x02 ||                   /* BCOMMAND_SCANNER echo  */
        buf[4] != 0x00)                     /* status OK              */
        return -1;
    return 0;
}

int
brscan5_rsp_xsc(const unsigned char *buf, int len)
{
    if (len < 2)
        return -1;
    if (buf[0] == 0x90 && buf[1] == 0x00)
        return 0;                       /* empty feeder */
    if (len >= 14 && buf[0] == 0x00 && buf[1] == 0x02)
        return 1;                       /* scan started, first progress record */
    /* T8c: B/W (COMP=RLENGTH) answers with a 00 01 data-block record
     * instead of a 00 02 progress record (chroot reference, BW 300). */
    if (len >= 14 && buf[0] == 0x00 && buf[1] == 0x01 &&
        buf[2] == 0x01 && buf[3] == 0x00)
        return 1;                       /* scan started, first block record */
    if (buf[0] == 0x91 && buf[1] == 0x00)
        return 2;                       /* device reports a document jam */
    return -1;
}

/* ======================================================================
 * Scan-geometry conversion policies (see brscan5_proto.h)
 * ====================================================================== */

/* Policy "estimate": 0.1-mm integers × dpi / 254, truncation, clamp ≥ 1.
 * C integer division truncates toward zero, matching the legacy
 * brother.c ScanAreaDot arithmetic this replaces. */
long
brscan5_mm0d1_to_px(long mm0d1, int dpi)
{
    long px = mm0d1 * (long)dpi / 254L;

    if (px < 1)
        px = 1;
    return px;
}

/* Policy "area": raw SANE mm × dpi / 25.4, round-half-up. Byte-exact
 * against every captured XSC AREA value at 150/300 dpi (T8b/T8c); the
 * 600-dpi T8c height (8399) is NOT reproduced from the DS-640 default
 * br-y 355.6 mm — documented divergence, see test_brscan5_geo.c. */
long
brscan5_mm_to_px_area(double mm, int dpi)
{
    return (long)(mm * dpi / 25.4 + 0.5);
}

/* Fill the SANE parameters for a given width/height and scan mode
 * (COLOR_* ids from brother.h). Mapping per T6 spec:
 *   Color   -> SANE_FRAME_RGB,  depth 8, bytes_per_line = width*3
 *   Gray    -> SANE_FRAME_GRAY, depth 8, bytes_per_line = width
 *   B/W/ED  -> SANE_FRAME_GRAY, depth 1, bytes_per_line = (width+7)/8
 * (Gray/B&W mapping is implemented but untested against hardware — the
 * DS-640 capture only covers 300 dpi colour.) */
void
brscan5_fill_params(SANE_Parameters *p, long w, long h, int color_type)
{
    p->pixels_per_line = (SANE_Int)w;
    p->lines = (SANE_Int)h;
    p->last_frame = SANE_TRUE;
    switch (color_type) {
    case COLOR_TG:
    case COLOR_ED:
        p->format = SANE_FRAME_GRAY;
        p->depth = 8;
        p->bytes_per_line = (SANE_Int)w;
        break;
    case COLOR_BW:
        p->format = SANE_FRAME_GRAY;
        p->depth = 1;
        p->bytes_per_line = (SANE_Int)((w + 7) / 8);
        break;
    default:                        /* COLOR_FUL, COLOR_FUL_NOCM */
        p->format = SANE_FRAME_RGB;
        p->depth = 8;
        p->bytes_per_line = (SANE_Int)(w * 3);
        break;
    }
}
