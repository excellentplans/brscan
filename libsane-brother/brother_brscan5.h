/*
 * brother_brscan5.h — brscan5 command layer for the Brother DS-640.
 *
 * The DS-640 (04f9:0468) speaks the brscan5 text-command protocol on
 * interface 1 (EP 0x04 OUT, EP 0x83 IN). The protocol is reverse
 * engineered from usbmon captures of the working amd64 driver plus
 * disassembly of libLxBsScanCoreApi.so.3.2.6 — see docs/brscan5-protocol.md
 * (byte-exact test reference; SSP body is a documented reconstruction).
 *
 * This module is PARALLEL to the existing 3/4 path. brother.c selects
 * it via a single dispatch gate at sane_open() when the model's
 * seriesNo is BRSCAN5_SERIES_NO.
 */

#ifndef _H_BROTHER_BRSCAN5
#define _H_BROTHER_BRSCAN5

#include "brother.h"

/*
 * seriesNo that selects the brscan5 command layer (DS-640).
 *
 * seriesNo semantics in the SUFFIX==2 (brscan4) codebase were checked
 * (brother_modelinf.h:92, brother_modelinf.c GetSupport* functions):
 *  - MUST_CONVERT_MODEL == 10, and the only seriesNo-based data-path
 *    switch is `seriesNo >= MUST_CONVERT_MODEL` (brother_scanner.c:1008),
 *    which routes to brscan4_read_next_record_from_device(). 5 < 10, so
 *    series 5 does NOT trigger the brscan4 record-framing path — exactly
 *    what we want: the DS-640 never enters the 3/4 pipeline, it is
 *    dispatched to the brscan5 ops below.
 *  - GetSeriesNo() accepts 1..MAX_SERIES_NO (19) → 5 is valid.
 *  - Feature lookup for series 5 (ALL_SF_TYPE) yields ADF-only, gray/BW
 *    modes, 100..1200 dpi — a reasonable match for a document scanner;
 *    T5-T7 may refine modelConfig for the DS-640.
 *  - seriesNo 5 is NOT in ChangeEndpoint[] ({8,9,16,17,18,19,14}), so the
 *    shared devaccs primitives would select bulk EP 0x84/0x03 for it —
 *    wrong for the DS-640 (0x83/0x04). That is why this module uses its
 *    own fixed-endpoint USB wrappers (see brother_brscan5.c).
 *
 * Note: in a SUFFIX==1 (brscan3) build seriesNo 5 means ZL2_SF_TYPE; the
 * gate is therefore compiled to the legacy table for BRSANESUFFIX != 2.
 */
#define BRSCAN5_SERIES_NO   5

/* ------------------------------------------------------------------
 * DS-640 model identification (dispatch gate + replay guard).
 *
 * The dispatch gate routes a device to the brscan5 command layer. The DS-640
 * (04f9:0468) is identified here by its unique product ID (plus the Brother
 * vendor), because its model-table seriesNo is actually 14 (shared with the
 * DCP-1510 family), not 5 as the original T3 series-based dispatch assumed.
 * `brscan5_is_ds640()` is the authoritative selector; the seriesNo==5 check
 * is kept as a back-compat accept so a series-5 build/config still routes
 * here. See docs/brscan5-status.md.
 * ------------------------------------------------------------------ */
#define BRSCAN5_PRODUCT_ID  0x0468

static inline int
brscan5_is_ds640(const MODELINF *m)
{
    return (m != NULL &&
            m->vendorID == SCANNER_VENDOR &&
            m->productID == BRSCAN5_PRODUCT_ID);
}

static inline int
brscan5_is_brscan5(const MODELINF *m)
{
    return (m != NULL &&
            (m->seriesNo == BRSCAN5_SERIES_NO || brscan5_is_ds640(m)));
}

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
 * Return command length, or 0 if the output buffer is too small. */
int brscan5_enc_ssp_dyn(char *buf, int bufsz,
                        int reso_x, int reso_y, int color_type,
                        int brit, int cont, const char *area);
int brscan5_enc_xsc_dyn(char *buf, int bufsz, int reso_x, int reso_y,
                        const char *area);

/* ------------------------------------------------------------------
 * DS-640 feature profile (T6).
 *
 * The DS-640's model-table seriesNo (14) maps onto legacy feature tables
 * that do not describe this device. sane_open applies this override after
 * get_model_config() so the SANE options reflect the real DS-640 feature
 * profile (from the T3 QDI/capture analysis):
 *   - resolution 100/150/200/300/400/600/1200 dpi
 *   - modes "Black & White" (binary), "True Gray", "24bit Color"
 *   - source: ADF only (no flatbed, no duplex)
 *   - scan area 215.9 x 355.6 mm (letter/legal length)
 * ------------------------------------------------------------------ */
void brscan5_override_model_config(MODELCONFIG *mc);

/* SANE parameters for the brscan5 path (T6).
 *
 * Two-phase behaviour:
 *   - before sane_start (or with no session): width/height are ESTIMATED
 *     from the configured options (scan area mm + resolution); the frame
 *     format/depth/bytes_per_line follow the selected mode.
 *   - after sane_start: brscan5_start() has received the page JPEG and
 *     read its header (jpeg_read_header); the REAL decoded dimensions are
 *     reported (the device auto-crops/derotates, so they can differ from
 *     the estimate — measured: 2446x3485 for a full A4 300 dpi scan).
 * Returns SANE_STATUS_GOOD, or SANE_STATUS_INVAL for an unknown mode. */
int brscan5_get_parameters(Brother_Scanner *this, SANE_Parameters *p);

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
int brscan5_enc_ctrl_open(unsigned char setup[8]);
int brscan5_enc_ctrl_close(unsigned char setup[8]);

/* Vendor control response: 5 B `05 10 <breq> 02 00` (breq = expected
 * bRequest echo: 1 = open, 2 = close). Returns 0 on success, -1 on
 * parse/mismatch error. */
int brscan5_rsp_ctrl(const unsigned char *buf, int len, int breq);

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

/* ------------------------------------------------------------------
 * Ops dispatch — the command-layer entry points (context for T5-T7).
 *
 * The struct is defined here and instantiated in brother.c as the two
 * global dispatch handles (brscan5_ops_dispatch for seriesNo==5 and a
 * static legacy table for every other model). Return values follow the
 * existing backend conventions (SANE_Status for start/read, BOOL for
 * open, 0 for cancel/close).
 * ------------------------------------------------------------------ */

struct brscan5_ops {
    int (*open)(Brother_Scanner *this);
    int (*start)(Brother_Scanner *this);
    int (*read)(Brother_Scanner *this, char *buf, int maxlen, int *len);
    int (*cancel)(Brother_Scanner *this);
    int (*close)(Brother_Scanner *this);
};

/* brscan5 command layer (seriesNo == 5). Defined in brother.c, points to
 * the brscan5_* functions below. */
extern const struct brscan5_ops brscan5_ops_dispatch;

/* ------------------------------------------------------------------
 * Transport interface (T5).
 *
 * All device I/O in the brscan5 module runs through this small flat
 * interface so a scan can be replayed from a fixture file instead of
 * touching hardware.
 *
 *   write(): send a command. Returns 0 on success, < 0 on error.
 *   read() : receive the next IN URB into buf (capacity len). On success
 *            *got holds the number of bytes read (*got == 0 models EOF /
 *            no data). Returns 0 on success, < 0 on error.
 *   ctx    : implementation-private state.
 *
 * Two implementations:
 *   USB     — libusb bulk on EP 0x83 IN / 0x04 OUT (real hardware; wraps
 *             the fixed-endpoint primitives from T3).
 *   Replay  — brother_brscan5_replay.c: reads OUT/IN from a TLV fixture
 *             file, asserts each write matches the expected command
 *             byte-exactly (the OUT-assertion test). Has NO USB handle.
 *
 * Selection is via BROTHER5_REPLAY=<path>: when set, the replay transport
 * is used at sane_open, otherwise USB.
 * ------------------------------------------------------------------ */
struct brscan5_transport {
    int (*write)(void *ctx, const uint8_t *buf, size_t len);  /* 0=ok,<0=err */
    int (*read)(void *ctx, uint8_t *buf, size_t len, size_t *got);
    /* Drain pending IN data after a cancel (T7). Replay: consume IN
     * entries until the next OUT-Expect entry (or EOF) and discard them —
     * this repositions the fixture at the next command so a fresh
     * sane_start works without sane_close/sane_open. USB: read+discard
     * bulk data for up to ~1 s of silence. Returns 0 on success, < 0 on
     * hard error (drain is best-effort; the caller proceeds anyway). */
    int (*drain)(void *ctx);
    /* Post-error/post-cancel endpoint recovery (T7). USB: libusb
     * clear_halt on both endpoints (0x83 IN / 0x04 OUT). Replay: no-op.
     * Returns 0 on success, < 0 on error. */
    int (*reset)(void *ctx);
    /* Vendor control transfer (T8b dance). Sends the 8-B setup
     * (bmRequestType/bRequest/wValue/wIndex/wLength) and — for IN
     * requests (bmRequestType 0xC0) — receives up to buflen bytes into
     * buf (buflen must be >= wLength); *got receives the actual
     * transfer length. wLength is the USB setup value, NOT the buffer
     * size. Returns 0 on success, < 0 on error. Replay: asserts the
     * next fixture entries are a 0x05 OUT-Control-Expect (8-B setup,
     * byte-exact) followed by a 0x06 IN-Control-Deliver (response
     * payload). */
    int (*control)(void *ctx, unsigned char bmRequestType,
                   unsigned char bRequest, unsigned wValue, unsigned wIndex,
                   unsigned wLength, unsigned char *buf, int buflen,
                   int *got);
    /* Timeouts (T7), honoured by the USB transport; the replay transport
     * simulates timeouts via 0x03 fixture entries instead.
     *   urb_timeout_ms  : per-URB bulk read timeout (USB: 1000 ms)
     *   idle_timeout_ms : max time without ANY bytes before the read
     *                     fails with IO_ERROR (USB: 5000 ms for command
     *                     responses during the handshake, 30000 ms for
     *                     the data phase). brscan5_start()/brscan5_pump()
     *                     set this before their read loops. */
    unsigned urb_timeout_ms;
    unsigned idle_timeout_ms;
    /* 1 when this is the replay transport: hardware timing quirks (the
     * ~300 ms settle between the dance's GET_CLOSE and GET_OPEN) are
     * skipped, fixture timing is not modelled. */
    int is_replay;
    void *ctx;
};
typedef struct brscan5_transport brscan5_transport_t;

/* True when BROTHER5_REPLAY is set (replay transport active). */
int brscan5_replay_active(void);

/* Open the replay transport (brother_brscan5_replay.c) for the TLV fixture
 * at `path`. On success fills *t (replay has no USB handle). Returns 0 on
 * success, -1 on failure. */
int brscan5_replay_open(brscan5_transport_t *t, const char *path);

/* Create the transport for `this` based on BROTHER5_REPLAY. Returns 0 on
 * success, -1 on failure. The USB variant requires hScanner->usb to be a
 * valid opened+claimed handle (set up by sane_open for real hardware). */
int brscan5_transport_open(Brother_Scanner *this);

/* Close + free a transport (does NOT send the session-close control
 * message — that is brscan5_close()'s job). */
void brscan5_transport_close(brscan5_transport_t *t);

/* Session state shared between brscan5_start/read/close (opaque). */
struct brscan5_session;
typedef struct brscan5_session brscan5_session_t;

/* Op implementations. T5: open creates the transport, start runs the
 * Q→QDI→CKD→SSP→XSC sequence and arms the parser. T6: start receives the
 * page JPEG and reads its header (real parameters), read streams decoded
 * scanlines out of the libjpeg decoder, close tears down. */
int brscan5_open(Brother_Scanner *this);
int brscan5_start(Brother_Scanner *this);
int brscan5_read(Brother_Scanner *this, char *buf, int maxlen, int *len);
int brscan5_cancel(Brother_Scanner *this);
int brscan5_close(Brother_Scanner *this);

#endif /* _H_BROTHER_BRSCAN5 */