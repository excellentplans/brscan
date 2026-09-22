#ifndef BRSCAN5_PROTO_H
#define BRSCAN5_PROTO_H

/*
 * brscan5_proto.h — pure brscan5 protocol module (Brother DS-640).
 *
 * Command encoders and response readers for the brscan5 text-command
 * protocol, split out of brother_brscan5.c so they can be unit-tested
 * standalone (no USB transport, no libjpeg, no backend session state).
 *
 * Protocol reverse-engineered from USB captures of a Brother DS-640;
 * see docs/brscan5-protocol.md.
 */

#include "brother.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Response sizes (from usbmon captures, per response URB)
 * ------------------------------------------------------------------ */

#define BRSCAN5_RSP_Q_LEN   75      /* c1 00 49 10 ... device status OK */
#define BRSCAN5_RSP_QDI_LEN 662     /* 00 51 44 49 <u16 LE len> 00 00 + payload */
#define BRSCAN5_RSP_SSP_LEN 38      /* 00 53 53 50 1e ... settings ack */

/* ------------------------------------------------------------------
 * Vendor control transfers (T8b dance).
 *
 * The device requires a vendor control dance around SSP (PROTOCOL.md
 * §1a, usbmon4.log/usbmon-scan.log): GET_OPEN (bmRequestType 0xC0,
 * bRequest 0x01, wValue 0x0002, wIndex 0, wLength 255) before the first
 * Q, and between the CKD response and SSP a GET_CLOSE (bRequest 0x02) —
 * ~300 ms — GET_OPEN sequence. Without it the device rejects SSP with
 * 8 B `83 53 53 50 00 00 00 00`. Both requests answer 5 B
 * `05 10 <bRequest> 02 00`.
 * ------------------------------------------------------------------ */

/* Vendor control setup packets (8 B, usbmon captures):
 *   open  = c0 01 02 00 00 00 ff 00  (GET_OPEN,  wValue 0x0002, wLength 255)
 *   close = c0 02 02 00 00 00 ff 00  (GET_CLOSE, wValue 0x0002, wLength 255)
 * (usbmon text captures show wLength 0x00ff; the device answers 5 B
 * regardless — pass1-ckd captured our wLength-5 close answering the same
 * 5 bytes. We mirror the reference 0x00ff.) Return 8, or 0 on error. */
#define BRSCAN5_CTRL_OPEN       "\xc0\x01\x02\x00\x00\x00\xff\x00"
#define BRSCAN5_CTRL_CLOSE      "\xc0\x02\x02\x00\x00\x00\xff\x00"
#define BRSCAN5_CTRL_SETUP_LEN  8
#define BRSCAN5_CTRL_RSP_LEN    5

int brscan5_enc_ctrl_open(unsigned char setup[8]);
int brscan5_enc_ctrl_close(unsigned char setup[8]);

/* Vendor control response: 5 B `05 10 <breq> 02 00` (breq = expected
 * bRequest echo: 1 = open, 2 = close). Returns 0 on success, -1 on
 * parse/mismatch error. */
int brscan5_rsp_ctrl(const unsigned char *buf, int len, int breq);

/* ------------------------------------------------------------------
 * Command encoders (byte-exact vs. commands.golden.json).
 * Return command length, or 0 if the output buffer is too small.
 * ------------------------------------------------------------------ */

/* ESC Q\n\x80 — device status query, 4 B (captured). */
int brscan5_enc_q(char *buf, int bufsz);
/* ESC QDI\n\x80 — device info query, 6 B (captured). */
int brscan5_enc_qdi(char *buf, int bufsz);
/* ESC CKD\nPSRC=ADF\n\x80 — document (feeder) check, 15 B (captured). */
int brscan5_enc_ckd(char *buf, int bufsz);
/* ESC SSP\n<settings>\n...\x80 — scan settings, 278 B
 * (32-B prefix captured; body reconstructed from MakeSSPcmdString). */
int brscan5_enc_ssp(char *buf, int bufsz);
/* ESC XSC\nRESO=<x>,<y>\nAREA=ATDSKW\nMODE=NORMAL\n\x80 — scan start,
 * 43 B at 300 dpi (prefix captured, MODE tail reconstructed). The dynamic
 * variant substitutes the configured resolution; at 300 dpi both are
 * byte-identical. */
int brscan5_enc_xsc(char *buf, int bufsz);

/* Dynamic SSP/XSC builders (T6/T8b): RESO=<reso_x>,<reso_y>, the CLR=
 * line, AREA=, BRIT= and CONT= follow the configured scan options.
 * (color_type: COLOR_BW/COLOR_ED/COLOR_TG/COLOR_FUL/_NOCM from
 * brother.h; brit/cont are the BRIT=/CONT= values as sent on the wire
 * = uiSetting value + 50; area is the AREA= value: "NORMAL" for a
 * normal rectangular scan area or "ATDSKW" for auto-deskew.)
 *
 * T8b: the SSP body was WRONG before (ATCL/THRS/RATE lines, 0-valued
 * fields, PAGE=1, TONE=0 — a MakeSSPcmdString misread that was only
 * ever checked against our own port's echoes). The REAL body comes
 * from the T8b reference-driver binary capture
 * (hw-window/t8b-capture-ref.pcapmon): CLR=C24BIT, AREA=NORMAL,
 * BRIT=50, CONT=50, PAGE=0, TONE=ON, all flags OFF, USER= with a
 * trailing \n before the 0x80 terminator. The DS-640 REJECTS the old
 * body with 8 B 83 53 53 50 00 00 00 00 (status 0x83).
 *
 * CLR mapping (T8b: C24BIT proven by the reference capture):
 *   COLOR_BW       -> CLR=TEXT      (1-bit black & white)
 *   COLOR_ED       -> CLR=ERRDIF    (error-diffusion gray)
 *   COLOR_TG       -> CLR=GRAY256   (8-bit gray)
 *   COLOR_FUL/NOCM -> CLR=C24BIT    (24-bit colour, reference-proven)
 *
 * comp_override: 0 = emit the mode's default COMP= value; nonzero =
 * emit COMP=NONE for COLOR_BW instead of COMP=RLENGTH (caller-provided
 * parameter, e.g. for tests/hardware experiments — no env var; ignored
 * for all other modes).
 * Return command length, or 0 if the output buffer is too small. */
int brscan5_enc_ssp_dyn(char *buf, int bufsz,
                        int reso_x, int reso_y, int color_type,
                        int brit, int cont, const char *area,
                        int comp_override);
int brscan5_enc_xsc_dyn(char *buf, int bufsz, int reso_x, int reso_y,
                        const char *area);

/* ------------------------------------------------------------------
 * Response readers. Responses arrive as a single IN-URB on EP 0x83.
 * Return 0 on success, -1 on parse/length error.
 * ------------------------------------------------------------------ */

/* Q response: 75 B, begins c1 00 49 10 00 17. */
int brscan5_rsp_q(const unsigned char *buf, int len);
/* QDI response: 662 B (8-B header 00 51 44 49 <u16 LE len> 00 00 +
 * 654-B payload). Consumes the length, hands back the payload pointer. */
int brscan5_rsp_qdi(const unsigned char *buf, int len,
                    const unsigned char **payload);
/* CKD response: 2 B. Returns 1 = paper present, 0 = feeder empty,
 * -1 = error. */
int brscan5_rsp_ckd(const unsigned char *buf, int len);
/* SSP response: 38 B, begins 00 53 53 50 1e ("SSP" + 0x1e=30). */
int brscan5_rsp_ssp(const unsigned char *buf, int len);
/* SSP rejection (T8b): 8 B `83 53 53 50 …` (status 0x83 = rejected —
 * device answers this when the vendor control dance was not performed).
 * Returns 1 when buf is the 0x83 rejection, 0 otherwise. */
int brscan5_rsp_ssp_rejected(const unsigned char *buf, int len);
/* XSC response: 2 B 90 00 = empty feeder, 14-B record beginning 00 02
 * = scan started. Returns 1 = started, 0 = empty, -1 = error. */
int brscan5_rsp_xsc(const unsigned char *buf, int len);

/* Fill the SANE parameters for a given width/height and scan mode
 * (COLOR_* ids from brother.h). Mapping per T6 spec:
 *   Color   -> SANE_FRAME_RGB,  depth 8, bytes_per_line = width*3
 *   Gray    -> SANE_FRAME_GRAY, depth 8, bytes_per_line = width
 *   B/W/ED  -> SANE_FRAME_GRAY, depth 1, bytes_per_line = (width+7)/8
 * (Gray/B&W mapping is implemented but untested against hardware — the
 * DS-640 capture only covers 300 dpi colour.) */
void brscan5_fill_params(SANE_Parameters *p, long w, long h, int color_type);

#ifdef __cplusplus
}
#endif

#endif /* BRSCAN5_PROTO_H */
