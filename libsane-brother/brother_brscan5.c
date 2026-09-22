/*
 * brother_brscan5.c — brscan5 command layer for the Brother DS-640.
 *
 * Protocol reference: docs/brscan5-protocol.md.
 *
 * This module is PARALLEL to the existing brscan3/4 pipeline and is only
 * reached through the single ops-dispatch gate in brother.c (sane_open,
 * brscan5_is_brscan5(): DS-640 product ID 0x0468 or seriesNo==5). It must
 * not contain any scattered series checks.
 *
 * The pure protocol functions (command encoders, response readers,
 * parameter mapping) live in brscan5_proto.c / brscan5_proto.h.
 *
 * USB transport: the shared ReadNonFixedData()/WriteDeviceData()
 * primitives do NOT imply brscan4 record framing (that lives in
 * brother_brscan4.c), but their bulk-endpoint selection is seriesNo
 * driven (ChangeEndpoint[], default 0x84 IN / 0x03 OUT) and would pick
 * the wrong endpoints for series 5. The DS-640 needs fixed EP 0x04 OUT /
 * EP 0x83 IN on interface 1, so this module uses its own thin libusb
 * wrappers with those endpoints instead.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include <usb.h>
#include <jpeglib.h>

#include "brother.h"
#include "brother_brscan5.h"
#include "brother_scanner.h"   /* CnvResoNoToUserResoValue()            */
#include "brscan5_stream.h"
#include "brother_devaccs.h"   /* CloseDevice() — control session close */
#include "brother_log.h"       /* WriteLog()                            */

/* ======================================================================
 * Transport interface (T5)
 *
 * All device I/O runs through brscan5_transport_t. Two backends:
 *   USB     — libusb bulk on EP 0x83 IN / 0x04 OUT (real hardware)
 *   Replay  — reads OUT/IN from a fixture file, asserts each write byte-
 *             exactly against the expected command (the OUT-assertion test)
 *
 * The replay backend has NO USB handle (guarded: this->hScanner->usb stays
 * NULL, no libusb call is ever made).
 * ====================================================================== */

#define BRSCAN5_EP_OUT          0x04
#define BRSCAN5_EP_IN           0x83
/* Timeouts (T7). The USB transport reads URB-by-URB with a short
 * per-URB timeout and enforces an idle budget: if no bytes at all arrive
 * within idle_timeout_ms, the read fails with IO_ERROR (no silent
 * endless reads). Command responses during the handshake get a tighter
 * budget than the data phase. */
#define BRSCAN5_TIMEOUT_URB     1000    /* per-URB bulk read (ms)        */
#define BRSCAN5_TIMEOUT_CMD     5000    /* command response budget (ms)  */
#define BRSCAN5_TIMEOUT_XSC     15000   /* XSC response budget (ms) — the
                                         * device grabs the paper first and
                                         * only then answers (8.8–9.3 s
                                         * observed in the chroot reference
                                         * at 300 dpi; empty feeder: 0.3 s) */
#define BRSCAN5_TIMEOUT_DATA    30000   /* data-phase idle budget (ms)   */
#define BRSCAN5_TIMEOUT_WRITE   2000    /* bulk write (ms)               */
#define BRSCAN5_DRAIN_QUIESCE   1000    /* cancel-drain silence window   */
#define BRSCAN5_WRITE_RETRY     5

/* Vendor control transfers (T8b dance, PROTOCOL.md §1a):
 *   GET_OPEN  = c0 01 0002 0000 00ff  (before the first Q, and after the
 *                                      dance's GET_CLOSE)
 *   GET_CLOSE = c0 02 0002 0000 00ff  (after the CKD response, before the
 *                                      ~300 ms settle + GET_OPEN)
 * Both answer 5 B `05 10 <bRequest> 02 00`. The reference captures show
 * wLength 0x00ff (the device answers 5 B regardless); we mirror it. */
#define BRSCAN5_CTRL_OPEN       "\xc0\x01\x02\x00\x00\x00\xff\x00"
#define BRSCAN5_CTRL_CLOSE      "\xc0\x02\x02\x00\x00\x00\xff\x00"
#define BRSCAN5_CTRL_SETUP_LEN  8
#define BRSCAN5_CTRL_BUF_LEN    260     /* request wLength 255 + slack   */
#define BRSCAN5_CTRL_RSP_LEN    5
#define BRSCAN5_CTRL_RETRY      5
/* Settle time between the dance's GET_CLOSE and GET_OPEN (reference:
 * 302/308 ms in usbmon4.log/usbmon-scan.log). Skipped in replay. */
#define BRSCAN5_CTRL_REOPEN_MS  300

/* Host URB buffer for the data phase (device max data URB is 262144 B;
 * a few extra bytes of headroom for safety). */
#define BRSCAN5_DATA_URB_MAX    262160

static void brscan5_on_event(const br5_event_t *ev, void *userdata);

/* brother_brscan5_replay.c — close/free a replay transport. */
void brscan5_replay_close(brscan5_transport_t *t);

/* ---- session state (T5) --------------------------------------------- */

/* libjpeg error handling: the default error_exit() would terminate the
 * process — route errors through setjmp back to the decoder owner. */
struct br5_jpeg_err {
    struct jpeg_error_mgr pub;
    jmp_buf               jb;
};

struct brscan5_session {
    brscan5_transport_t tport;   /* active transport (usb or replay)   */
    br5_parser_t       *parser;  /* data-phase stream parser           */

    /* page state: the compressed page JPEG is accumulated in page_buf
     * (SOI..EOI) and handed to a libjpeg decoder; sane_read then serves
     * decoded scanlines out of the decoder (streaming, T6).            */
    uint8_t       *page_buf;     /* accumulated JPEG (SOI..EOI)        */
    size_t         page_len;     /* valid bytes in page_buf            */
    size_t         page_cap;     /* allocated capacity (doubling)      */
    int            page_ready;   /* EOI reached, JPEG available        */
    int            eof;          /* SESSION_END seen / transport done  */

    /* libjpeg decoder (created when the page is complete)              */
    struct jpeg_decompress_struct cinfo;
    struct br5_jpeg_err    jerr;
    int            dec_active;   /* decoder created, header read       */
    JSAMPROW       row_buf;      /* scratch scanline (decoded)         */
    size_t         row_bytes;    /* decoded bytes per scanline         */
    long           lines_out;    /* scanlines delivered to frontend    */
    SANE_Parameters params;      /* SANE parameters of the active page */

    /* image dimensions: estimated from the options before sane_start
     * (est_*), corrected to the real JPEG header values once the page
     * header has been read (real_*, have_real). sane_get_parameters
     * reports the estimate before, the real values after sane_start.   */
    long           est_w, est_h;
    long           real_w, real_h;
    int            have_real;

    /* T8c RLENGTH (B/W) page: the page buffer holds the concatenated
     * packbits payload (one compressed scanline per 00 01 block);
     * rle_offs marks the end offset of each line in page_buf. After the
     * page is complete the payload is decoded once into raw_buf
     * (SANE 1-bit rows, row_bytes = (width+7)/8) and served directly. */
    int            rle_mode;     /* page is RLENGTH, not JPEG          */
    size_t        *rle_offs;     /* end offset of line i in page_buf   */
    size_t         rle_nlines;   /* number of line offsets             */
    size_t         rle_ocap;     /* allocated rle_offs capacity        */
    uint8_t       *raw_buf;      /* decoded 1-bit bitmap               */
    size_t         raw_len;      /* decoded bitmap bytes               */
    int            rle_active;   /* raw bitmap ready for sane_read     */

    /* monitoring (progress records, warnings) */
    int            n_progress;
    int            n_warns;
    int            n_errors;

    /* T7 cancel/timeout state:
     *   armed    — a scan is running (XSC-OK received, parser armed);
     *              the cancel drain only runs in this state.
     *   canceled — sticky cancel latch: brscan5_read returns
     *              SANE_STATUS_CANCELLED while set, cleared by the next
     *              brscan5_start (fresh Q→QDI→CKD→SSP→XSC handshake). */
    int            armed;
    int            canceled;
};

/* ---- USB backend ----------------------------------------------------- */

/* The USB transport wraps the DS-640 fixed bulk endpoints (0x04 OUT /
 * 0x83 IN on interface 1). Replay mode never reaches these wrappers: the
 * replay transport (brother_brscan5_replay.c) has no USB handle and makes
 * no libusb call. */

typedef struct {
    Brother_Scanner *this;
} brscan5_usb_t;

static int
brscan5_usb_write(void *ctx, const uint8_t *buf, size_t len)
{
    brscan5_usb_t *u = (brscan5_usb_t *)ctx;
    Brother_Scanner *this = u->this;
    int i, rc = -1;

    if (!this || !this->hScanner || !this->hScanner->usb)
        return -1;
    for (i = 0; i < BRSCAN5_WRITE_RETRY; i++) {
        rc = usb_bulk_write(this->hScanner->usb, BRSCAN5_EP_OUT,
                            (char *)buf, (int)len, 2000);
        if (rc >= 0)
            break;
    }
    return (rc >= 0) ? 0 : -1;
}

/* Monotonic clock in ms (idle-budget accounting). */
static long
brscan5_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int
brscan5_usb_read(void *ctx, uint8_t *buf, size_t len, size_t *got)
{
    brscan5_usb_t *u = (brscan5_usb_t *)ctx;
    Brother_Scanner *this = u->this;
    brscan5_transport_t *t;
    long deadline;
    int idle_ms;

    if (!this || !this->hScanner || !this->hScanner->usb)
        return -1;
    t = this->br5 ? &this->br5->tport : NULL;
    idle_ms = t ? (int)t->idle_timeout_ms : BRSCAN5_TIMEOUT_CMD;

    /* Poll loop: 1 short URB read per iteration (BRSCAN5_TIMEOUT_URB);
     * idle ticks (timeout without bytes) are tolerated until the idle
     * budget runs out, then the read fails with IO_ERROR. */
    deadline = brscan5_now_ms() + idle_ms;
    for (;;) {
        int rc = usb_bulk_read(this->hScanner->usb, BRSCAN5_EP_IN,
                               (char *)buf, (int)len, BRSCAN5_TIMEOUT_URB);
        if (rc > 0) {
            *got = (size_t)rc;
            return 0;
        }
        if (rc == 0 || rc == -ETIMEDOUT) {
            /* idle tick: no bytes within this URB window */
            if (brscan5_now_ms() >= deadline) {
                WriteLog("brscan5 usb: idle timeout after %d ms without "
                         "data (EP 0x83)", idle_ms);
                return -1;
            }
            continue;
        }
        WriteLog("brscan5 usb: bulk read error rc=%d (EP 0x83)", rc);
        return -1;
    }
}

/* Cancel drain (USB): read and discard bulk data until the endpoint has
 * been silent for ~1 s (BRSCAN5_DRAIN_QUIESCE). Best effort — always
 * returns 0; errors are logged. */
static int
brscan5_usb_drain(void *ctx)
{
    brscan5_usb_t *u = (brscan5_usb_t *)ctx;
    Brother_Scanner *this = u->this;
    uint8_t *scratch;
    long last_data;
    size_t discarded = 0;

    if (!this || !this->hScanner || !this->hScanner->usb)
        return 0;
    scratch = (uint8_t *)malloc(BRSCAN5_DATA_URB_MAX);
    if (!scratch)
        return 0;
    last_data = brscan5_now_ms();
    for (;;) {
        int rc = usb_bulk_read(this->hScanner->usb, BRSCAN5_EP_IN,
                               (char *)scratch, BRSCAN5_DATA_URB_MAX, 250);
        if (rc > 0) {
            discarded += (size_t)rc;
            last_data = brscan5_now_ms();
            continue;
        }
        if (rc == 0 || rc == -ETIMEDOUT) {
            if (brscan5_now_ms() - last_data >= BRSCAN5_DRAIN_QUIESCE)
                break;
            continue;
        }
        WriteLog("brscan5 usb: drain read error rc=%d", rc);
        break;
    }
    free(scratch);
    WriteLog("brscan5 usb: cancel drain done (%zu B discarded)",
             discarded);
    return 0;
}

/* Post-cancel/post-error endpoint recovery (USB): clear_halt on both
 * bulk endpoints so stale stalls don't poison the next scan. */
static int
brscan5_usb_reset(void *ctx)
{
    brscan5_usb_t *u = (brscan5_usb_t *)ctx;
    Brother_Scanner *this = u->this;

    if (!this || !this->hScanner || !this->hScanner->usb)
        return 0;
    WriteLog("brscan5 usb: clear_halt 0x83/0x04 after cancel/error");
    usb_clear_halt(this->hScanner->usb, BRSCAN5_EP_IN);
    usb_clear_halt(this->hScanner->usb, BRSCAN5_EP_OUT);
    return 0;
}

static void
brscan5_usb_close(brscan5_transport_t *t)
{
    if (!t || !t->ctx)
        return;
    free(t->ctx);
    t->ctx = NULL;
    t->write = NULL;
    t->read  = NULL;
    t->drain = NULL;
    t->reset = NULL;
    t->control = NULL;
}

/* Vendor control transfer (USB): usb_control_msg with a small retry,
 * mirroring OpenDevice()/CloseDevice(). */
static int
brscan5_usb_control(void *ctx, unsigned char bmRequestType,
                    unsigned char bRequest, unsigned wValue, unsigned wIndex,
                    unsigned wLength, unsigned char *buf, int buflen,
                    int *got)
{
    brscan5_usb_t *u = (brscan5_usb_t *)ctx;
    Brother_Scanner *this = u->this;
    int i, rc = -1;

    if (!this || !this->hScanner || !this->hScanner->usb)
        return -1;
    for (i = 0; i < BRSCAN5_CTRL_RETRY; i++) {
        rc = usb_control_msg(this->hScanner->usb, bmRequestType, bRequest,
                             (int)wValue, (int)wIndex, (char *)buf,
                             (int)wLength, 2000);
        if (rc >= 0)
            break;
    }
    if (rc < 0) {
        WriteLog("brscan5 usb: control %02x/%02x failed rc=%d",
                 bmRequestType, bRequest, rc);
        return -1;
    }
    if (got)
        *got = rc;
    return 0;
}

/* ---- Replay backend --------------------------------------------------
 *
 * Implemented in brother_brscan5_replay.c (kept separate so the replay
 * code and its TLV fixture format are self-contained). It opens a fixture
 * file when BROTHER5_REPLAY is set and has no USB handle.
 */

/* ---- transport factory ------------------------------------------------ */

int
brscan5_replay_active(void)
{
    const char *p = getenv("BROTHER5_REPLAY");
    return (p && p[0]) ? 1 : 0;
}

int
brscan5_transport_open(Brother_Scanner *this)
{
    const char *replay_path = getenv("BROTHER5_REPLAY");
    brscan5_transport_t *t;

    if (!this || !this->br5)
        return -1;
    t = &this->br5->tport;

    if (replay_path && replay_path[0])
        return brscan5_replay_open(t, replay_path);

    /* Real USB: hScanner->usb must already be opened+claimed by sane_open. */
    {
        brscan5_usb_t *u = (brscan5_usb_t *)calloc(1, sizeof(*u));
        if (!u)
            return -1;
        u->this = this;
        t->write = brscan5_usb_write;
        t->read  = brscan5_usb_read;
        t->drain = brscan5_usb_drain;
        t->reset = brscan5_usb_reset;
        t->control = brscan5_usb_control;
        t->urb_timeout_ms  = BRSCAN5_TIMEOUT_URB;
        t->idle_timeout_ms = BRSCAN5_TIMEOUT_CMD;
        t->is_replay = 0;
        t->ctx   = u;
        WriteLog("brscan5 transport: USB (EP 0x04 OUT / 0x83 IN)");
        return 0;
    }
}

void
brscan5_transport_close(brscan5_transport_t *t)
{
    if (!t)
        return;
    if (t->write == brscan5_usb_write)
        brscan5_usb_close(t);
    else if (t->ctx)
        brscan5_replay_close(t);
}

/* ======================================================================
 * Ops — brscan5 command layer (open/start/read/cancel/close)
 *
 * T5: open allocates the session + transport, start runs the
 * Q→QDI→CKD→SSP→XSC handshake and arms the parser.
 * T6: start then receives the whole page JPEG (the DS-640 data phase
 * delivers one page as a single JPEG stream) and reads its header with
 * libjpeg so sane_get_parameters can report the real dimensions; read
 * streams decoded scanlines out of the decoder (libjpeg streaming, no
 * more whole-page raw delivery). Cancel protocol is still undetermined
 * (no capture) — T7.
 * ====================================================================== */

/* Estimate the pixel dimensions of the next page from the configured
 * options (scan area in 0.1 mm + resolution). Used by
 * brscan5_get_parameters before the JPEG header is available. */
static void
brscan5_estimate_dims(Brother_Scanner *this, long *w, long *h)
{
    RESOLUTION reso;

    memset(&reso, 0, sizeof(reso));
    CnvResoNoToUserResoValue(&reso, this->uiSetting.wResoType);
    *w = (long)(this->uiSetting.ScanAreaMm.right - this->uiSetting.ScanAreaMm.left)
	 * reso.wResoX / 254L;
    *h = (long)(this->uiSetting.ScanAreaMm.bottom - this->uiSetting.ScanAreaMm.top)
	 * reso.wResoY / 254L;
    if (*w < 1)
        *w = 1;
    if (*h < 1)
        *h = 1;
}

int
brscan5_get_parameters(Brother_Scanner *this, SANE_Parameters *p)
{
    brscan5_session_t *s = this ? this->br5 : NULL;

    if (!p)
        return SANE_STATUS_INVAL;

    /* After sane_start the page JPEG header has been read: report the
     * real decoded dimensions. Before that: estimate from the options. */
    if (s && s->have_real) {
        brscan5_fill_params(p, s->real_w, s->real_h,
                            this->uiSetting.wColorType);
    } else {
        long w, h;
        brscan5_estimate_dims(this, &w, &h);
        brscan5_fill_params(p, w, h, this->uiSetting.wColorType);
    }
    WriteLog("brscan5_get_parameters: %dx%d bpl=%d depth=%d fmt=%d%s",
             p->pixels_per_line, p->lines, p->bytes_per_line, p->depth,
             (int)p->format, (s && s->have_real) ? " (real)" : " (est)");
    return SANE_STATUS_GOOD;
}

void
brscan5_override_model_config(MODELCONFIG *mc)
{
    if (!mc)
        return;
    mc->SupportReso.val = 0;
    mc->SupportReso.bit.bDpi100x100   = TRUE;
    mc->SupportReso.bit.bDpi150x150   = TRUE;
    mc->SupportReso.bit.bDpi200x200   = TRUE;
    mc->SupportReso.bit.bDpi300x300   = TRUE;
    mc->SupportReso.bit.bDpi400x400   = TRUE;
    mc->SupportReso.bit.bDpi600x600   = TRUE;
    mc->SupportReso.bit.bDpi1200x1200 = TRUE;
    mc->SupportReso.bit.bDpi2400x2400 = FALSE;
    mc->SupportReso.bit.bDpi4800x4800 = FALSE;
    mc->SupportReso.bit.bDpi9600x9600 = FALSE;

    mc->SupportScanMode.val = 0;
    mc->SupportScanMode.bit.bBlackWhite     = TRUE;  /* "Black & White" */
    mc->SupportScanMode.bit.bErrorDiffusion = FALSE;
    mc->SupportScanMode.bit.bTrueGray       = TRUE;  /* "True Gray"     */
    mc->SupportScanMode.bit.b24BitColor     = TRUE;  /* "24bit Color"   */
    mc->SupportScanMode.bit.b24BitNoCMatch  = FALSE;

    mc->SupportScanSrc.val = 0;
    mc->SupportScanSrc.bit.FB      = FALSE;
    mc->SupportScanSrc.bit.ADF     = TRUE;
    mc->SupportScanSrc.bit.ADF_DUP = FALSE;

    mc->SupportScanAreaWidth  = 215.9;   /* DS-640: letter/legal width  */
    mc->SupportScanAreaHeight = 355.6;   /* DS-640: letter/legal length */
}

/* ---- libjpeg decoder ------------------------------------------------- */

static void
brscan5_jpeg_error_exit(j_common_ptr cinfo)
{
    struct br5_jpeg_err *err = (struct br5_jpeg_err *)cinfo->err;

    /* The default error_exit() would terminate the process — longjmp back
     * to the decoder owner instead (message handling via output_message). */
    longjmp(err->jb, 1);
}

static void
brscan5_jpeg_output_message(j_common_ptr cinfo)
{
    char buf[JMSG_LENGTH_MAX];

    (*cinfo->err->format_message)(cinfo, buf);
    WriteLog("brscan5 libjpeg: %s", buf);
}

/* Destroy any active decoder and release its scratch/page buffers. */
static void
brscan5_decoder_teardown(brscan5_session_t *s)
{
    if (s->dec_active) {
        jpeg_destroy_decompress(&s->cinfo);
        s->dec_active = 0;
    }
    if (s->row_buf) {
        free(s->row_buf);
        s->row_buf = NULL;
    }
    if (s->page_buf) {
        free(s->page_buf);
        s->page_buf = NULL;
        s->page_cap = 0;
    }
    if (s->raw_buf) {
        free(s->raw_buf);
        s->raw_buf = NULL;
    }
    if (s->rle_offs) {
        free(s->rle_offs);
        s->rle_offs = NULL;
    }
    s->rle_ocap = 0;
    s->rle_nlines = 0;
    s->rle_mode = 0;
    s->rle_active = 0;
    s->page_len = 0;
    s->page_ready = 0;
    s->lines_out = 0;
}

/* T8c: packbits-decode one RLENGTH line (vendor semantics, libbrscandec
 * FUN_001063f3): control byte < 0x80 = copy c+1 literal bytes,
 * c > 0x80 = repeat the next byte (257-c) times, c == 0x80 = no-op.
 * Returns the number of output bytes written (up to cap). */
static size_t
br5_packbits(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n && o < cap) {
        uint8_t c = src[i++];
        if (c == 0x80)
            continue;
        if (c < 0x80) {
            size_t k = (size_t)c + 1;
            if (k > n - i)
                k = n - i;
            if (k > cap - o)
                k = cap - o;
            memcpy(dst + o, src + i, k);
            o += k;
            i += k;
        } else {
            size_t k = 257 - (size_t)c;
            if (i >= n)
                break;
            {
                uint8_t v = src[i++];
                if (k > cap - o)
                    k = cap - o;
                memset(dst + o, v, k);
                o += k;
            }
        }
    }
    return o;
}

/* Decode the RLENGTH page payload (page_buf = concatenated block
 * payloads, line ends in rle_offs) into a SANE 1-bit bitmap. The device
 * already delivers the "1 = black" bit convention the BW JPEG path uses
 * (verified against the chroot reference BW-300 scan: 3491 lines,
 * pixel correlation 1.0 with bit 1 = black; white runs are 0x00-byte
 * repeats). No bit flip needed. */
static SANE_Status
brscan5_rle_decode(brscan5_session_t *s, Brother_Scanner *this)
{
    size_t row_bytes = ((size_t)s->est_w + 7) / 8;
    uint8_t *row = (uint8_t *)malloc(row_bytes ? row_bytes : 1);

    s->rle_active = 0;
    if (!row) {
        WriteLog("brscan5 rle: OOM for row buffer");
        return SANE_STATUS_NO_MEM;
    }
    s->raw_len = (size_t)s->rle_nlines * row_bytes;
    s->raw_buf = (uint8_t *)malloc(s->raw_len ? s->raw_len : 1);
    if (!s->raw_buf) {
        free(row);
        WriteLog("brscan5 rle: OOM for bitmap (%zu bytes)", s->raw_len);
        return SANE_STATUS_NO_MEM;
    }
    for (size_t li = 0; li < s->rle_nlines; li++) {
        size_t start = li ? s->rle_offs[li - 1] : 0;
        size_t end = s->rle_offs[li];
        size_t got = br5_packbits(s->page_buf + start, end - start,
                                  row, row_bytes);
        uint8_t *out = s->raw_buf + li * row_bytes;
        size_t k;
        for (k = 0; k < got; k++)
            out[k] = row[k];
        for (; k < row_bytes; k++)
            out[k] = 0x00;          /* white padding (1 = black) */
    }
    free(row);
    s->real_w = s->est_w;
    s->real_h = (long)s->rle_nlines;
    s->have_real = 1;
    brscan5_fill_params(&s->params, s->real_w, s->real_h, COLOR_BW);
    s->row_bytes = row_bytes;
    s->rle_active = 1;
    s->lines_out = 0;
    WriteLog("brscan5 rle: page %ldx%ld decoded (%zu blocks, %zu payload "
             "bytes)", s->real_w, s->real_h, s->rle_nlines, s->page_len);
    return SANE_STATUS_GOOD;
}

/* Create the decoder for the completed page JPEG in page_buf: read the
 * header (fixes the real image dimensions), select the output format for
 * the configured scan mode and start the decompressor. Returns
 * SANE_STATUS_GOOD or an error status. */
static SANE_Status
brscan5_decoder_start(brscan5_session_t *s, Brother_Scanner *this)
{
    s->dec_active = 0;
    if (s->rle_mode)
        return brscan5_rle_decode(s, this);
    memset(&s->cinfo, 0, sizeof(s->cinfo));
    jpeg_create_decompress(&s->cinfo);
    s->cinfo.err = jpeg_std_error(&s->jerr.pub);
    s->jerr.pub.error_exit = brscan5_jpeg_error_exit;
    s->jerr.pub.output_message = brscan5_jpeg_output_message;
    if (setjmp(s->jerr.jb)) {
        WriteLog("brscan5 decoder: libjpeg error (page dropped)");
        jpeg_destroy_decompress(&s->cinfo);
        s->n_errors++;
        s->eof = 1;
        return SANE_STATUS_IO_ERROR;
    }

    jpeg_mem_src(&s->cinfo, s->page_buf, (unsigned long)s->page_len);
    jpeg_read_header(&s->cinfo, TRUE);

    /* Output format per configured scan mode. The captured scenario is
     * colour (RGB JPEG); for Gray we let libjpeg convert to grayscale.
     * B/W (1-bit) is decoded to grayscale and threshold-packed per
     * scanline in brscan5_read() — untested against hardware. */
    switch (this->uiSetting.wColorType) {
    case COLOR_TG:
    case COLOR_ED:
    case COLOR_BW:
        s->cinfo.out_color_space = JCS_GRAYSCALE;
        break;
    default:
        s->cinfo.out_color_space = JCS_RGB;
        break;
    }
    jpeg_start_decompress(&s->cinfo);

    s->real_w = s->cinfo.output_width;
    s->real_h = s->cinfo.output_height;
    s->have_real = 1;

    brscan5_fill_params(&s->params, s->real_w, s->real_h,
                        this->uiSetting.wColorType);
    s->row_bytes = (size_t)s->cinfo.output_width *
                   (size_t)s->cinfo.output_components;
    s->row_buf = (JSAMPROW)malloc(s->row_bytes);
    if (!s->row_buf) {
        WriteLog("brscan5 decoder: OOM for scanline buffer");
        jpeg_destroy_decompress(&s->cinfo);
        s->n_errors++;
        s->eof = 1;
        return SANE_STATUS_NO_MEM;
    }
    s->dec_active = 1;
    s->lines_out = 0;

    WriteLog("brscan5 decoder: page %ldx%ld, comps=%d, mode=%d",
             s->real_w, s->real_h, s->cinfo.output_components,
             this->uiSetting.wColorType);
    return SANE_STATUS_GOOD;
}

/* Drain the remaining transport data after a page (PAGE_END/SESSION_END
 * records) so the stream is fully consumed and the replay fixture's
 * records are all processed. */
static void
brscan5_drain(brscan5_session_t *s)
{
    while (!s->eof) {
        uint8_t drain[BRSCAN5_DATA_URB_MAX];
        size_t  dgot = 0;
        if (s->tport.read(s->tport.ctx, drain, sizeof(drain), &dgot) != 0)
            break;
        if (dgot == 0)
            break;
        if (br5_parser_feed(s->parser, drain, dgot,
                            BR5_FLAG_CHUNK_START) != 0)
            break;
        if (s->n_errors)
            break;
    }
}

/* Forward declarations (defined below, used by brscan5_start). */
static SANE_Status brscan5_pump(brscan5_session_t *s);

/* ---- vendor control dance (T8b) ---------------------------------------
 *
 * The DS-640 accepts SSP only in a freshly opened control session
 * (PROTOCOL.md §1a, usbmon4.log lines 23-27 / usbmon-scan.log lines
 * 23-26): after the CKD response the reference driver sends GET_CLOSE
 * (c0 02), waits ~300 ms, sends GET_OPEN (c0 01) and only then SSP —
 * which is then acknowledged with the 38-B ack even with an empty
 * feeder. Without the dance the device answers SSP with the 8-B
 * rejection 83 53 53 50 00 00 00 00 (T8a captures pass1/pass2 are OUR
 * port's runs: no c0 01 anywhere, SSP -> 0x83, the trailing c0 02 is
 * our session-close).
 *
 * Replay: the dance is modelled with 0x05/0x06 TLV entries; the 300 ms
 * settle is hardware timing and skipped (t->is_replay). */

/* One vendor control transfer with response validation. Returns 0 on
 * success (5-B response `05 10 <breq> 02 00`), -1 on error. */
static int
brscan5_ctrl_xfer(brscan5_transport_t *t, int is_close)
{
    unsigned char setup[BRSCAN5_CTRL_SETUP_LEN];
    unsigned char rsp[BRSCAN5_CTRL_BUF_LEN];
    int breq = is_close ? 0x02 : 0x01;
    int got = 0;

    if (!t->control) {
        WriteLog("brscan5 ctrl: transport has no control op");
        return -1;
    }
    if (is_close)
        brscan5_enc_ctrl_close(setup);
    else
        brscan5_enc_ctrl_open(setup);
    if (t->control(t->ctx, setup[0], setup[1],
                   (unsigned)setup[2] | ((unsigned)setup[3] << 8),
                   (unsigned)setup[4] | ((unsigned)setup[5] << 8),
                   (unsigned)setup[6] | ((unsigned)setup[7] << 8),
                   rsp, (int)sizeof(rsp), &got) != 0) {
        WriteLog("brscan5 ctrl: GET_%s transfer failed",
                 is_close ? "CLOSE" : "OPEN");
        return -1;
    }
    if (brscan5_rsp_ctrl(rsp, got, breq) != 0) {
        WriteLog("brscan5 ctrl: GET_%s bad response (%d B: "
                 "%02x %02x %02x %02x %02x)", is_close ? "CLOSE" : "OPEN",
                 got, got > 0 ? rsp[0] : 0, got > 1 ? rsp[1] : 0,
                 got > 2 ? rsp[2] : 0, got > 3 ? rsp[3] : 0,
                 got > 4 ? rsp[4] : 0);
        return -1;
    }
    WriteLog("brscan5 ctrl: GET_%s ok (05 10 %02x 02 00)",
             is_close ? "CLOSE" : "OPEN", breq);
    return 0;
}

/* The dance between the CKD response and SSP: GET_CLOSE, ~300 ms settle
 * (skipped in replay), GET_OPEN. */
static int
brscan5_ctrl_dance(brscan5_transport_t *t)
{
    if (brscan5_ctrl_xfer(t, 1) != 0)
        return -1;
    if (!t->is_replay)
        usleep(BRSCAN5_CTRL_REOPEN_MS * 1000);
    if (brscan5_ctrl_xfer(t, 0) != 0)
        return -1;
    return 0;
}

int
brscan5_open(Brother_Scanner *this)
{
    /* Allocate the session + transport. The transport is selected by
     * BROTHER5_REPLAY: replay mode has no USB handle and performs no
     * libusb access at all — control transfers are replayed as TLV
     * entries (0x05/0x06). For real USB the vendor session-open
     * (GET_OPEN, c0 01 — the same control the legacy OpenDevice() sends)
     * runs over the transport right after it is created, mirroring the
     * reference driver's sane_open (usbmon4.log line 9: c0 01 before the
     * first Q). */
    this->br5 = (brscan5_session_t *)calloc(1, sizeof(*this->br5));
    if (!this->br5)
        return 0;                       /* FALSE */
    this->br5->parser = br5_parser_new(brscan5_on_event, this);
    if (!this->br5->parser) {
        free(this->br5);
        this->br5 = NULL;
        return 0;
    }
    if (brscan5_transport_open(this) != 0) {
        br5_parser_free(this->br5->parser);
        free(this->br5);
        this->br5 = NULL;
        return 0;
    }
    /* Session-open control (T8b): GET_OPEN before the first Q. */
    if (brscan5_ctrl_xfer(&this->br5->tport, 0) != 0) {
        WriteLog("brscan5_open: GET_OPEN control failed");
        brscan5_transport_close(&this->br5->tport);
        br5_parser_free(this->br5->parser);
        free(this->br5);
        this->br5 = NULL;
        return 0;
    }
    return 1;                           /* TRUE */
}

int
brscan5_start(Brother_Scanner *this)
{
    char            cmd[512];
    unsigned char   rsp[BRSCAN5_RSP_QDI_LEN];
    size_t          got = 0;
    int             rc;
    int             reso_x, reso_y;
    int             comp_none;
    SANE_Status     st;
    const unsigned char *qdi_payload = NULL;
    brscan5_transport_t *t = &this->br5->tport;
    brscan5_session_t *s = this->br5;

    WriteLog("brscan5_start: Q -> QDI -> CKD -> SSP -> XSC");

    /* T7: a fresh start after cancel (or after a previous page) must work
     * WITHOUT sane_close/sane_open: clear the cancel latch, drop any
     * leftover page/decoder state and reset the parser to IDLE so it can
     * be re-armed after XSC-OK. */
    this->scanState.bCanceled = FALSE;
    s->canceled = 0;
    s->armed = 0;
    s->have_real = 0;
    s->eof = 0;
    br5_parser_reset(s->parser);
    /* Command responses during the handshake: 5 s idle budget. */
    t->idle_timeout_ms = BRSCAN5_TIMEOUT_CMD;

    /* Drop any leftover page/decoder state from a previous page. */
    brscan5_decoder_teardown(s);

    /* Q — device status query (75 B, c1 00 49 10 ...). */
    rc = brscan5_enc_q(cmd, sizeof(cmd));
    if (rc <= 0)
        return SANE_STATUS_INVAL;
    if (t->write(t->ctx, (const uint8_t *)cmd, (size_t)rc) != 0)
        return SANE_STATUS_IO_ERROR;
    if (t->read(t->ctx, rsp, sizeof(rsp), &got) != 0 || got == 0 ||
        brscan5_rsp_q(rsp, (int)got) != 0)
        return SANE_STATUS_IO_ERROR;

    /* QDI — device info (662 B; payload consumed, not parsed yet). */
    rc = brscan5_enc_qdi(cmd, sizeof(cmd));
    if (rc <= 0)
        return SANE_STATUS_INVAL;
    if (t->write(t->ctx, (const uint8_t *)cmd, (size_t)rc) != 0)
        return SANE_STATUS_IO_ERROR;
    got = 0;
    if (t->read(t->ctx, rsp, sizeof(rsp), &got) != 0 || got == 0 ||
        brscan5_rsp_qdi(rsp, (int)got, &qdi_payload) != 0)
        return SANE_STATUS_IO_ERROR;

    /* CKD — document check (00 01 = paper, 00 02 = feeder empty). */
    rc = brscan5_enc_ckd(cmd, sizeof(cmd));
    if (rc <= 0)
        return SANE_STATUS_INVAL;
    if (t->write(t->ctx, (const uint8_t *)cmd, (size_t)rc) != 0)
        return SANE_STATUS_IO_ERROR;
    got = 0;
    if (t->read(t->ctx, rsp, sizeof(rsp), &got) != 0 || got == 0)
        return SANE_STATUS_IO_ERROR;
    rc = brscan5_rsp_ckd(rsp, (int)got);
    if (rc < 0)
        return SANE_STATUS_IO_ERROR;
    if (rc == 0) {
        /* The reference driver does NOT abort on CKD 00 02 (feeder
         * empty) — it continues through the control dance to SSP and
         * XSC, and XSC then answers 90 00 (usbmon4.log, empty feeder).
         * The HWTEST hook from T8a is therefore a no-op since T8b; it
         * is kept (default-off) for capture-script compatibility. */
        if (getenv("BROTHER5_HWTEST_SSP_ALWAYS"))
            WriteLog("brscan5_start: BROTHER5_HWTEST_SSP_ALWAYS set — "
                     "no-op since T8b (normal flow continues to SSP/XSC)");
        WriteLog("brscan5_start: feeder empty (CKD 00 02) — continuing "
                 "through dance/SSP/XSC per reference flow");
    }

    /* T8b vendor control dance: GET_CLOSE — ~300 ms — GET_OPEN. SSP is
     * only accepted in this freshly opened control session (without it
     * the device answers 83 53 53 50 …, T8a). */
    if (brscan5_ctrl_dance(t) != 0)
        return SANE_STATUS_IO_ERROR;

    /* SSP — scan settings (38 B ack). RESO/CLR/BRIT/CONT come from the
     * configured options (T6); uiSetting is filled by
     * SetupInternalParameters() before ops->start() runs.
     * T8b: BRIT=/CONT= = option value + 50 (defaults 0 -> 50, reference
     * capture); AREA=NORMAL with the explicit area in the XSC command
     * (reference behaviour for scanimage-driven scans). */
    CnvResoNoToUserResoValue(&this->uiSetting.UserSelect,
                             this->uiSetting.wResoType);
    reso_x = this->uiSetting.UserSelect.wResoX;
    reso_y = this->uiSetting.UserSelect.wResoY;
    /* HWTEST hook (moved out of brscan5_enc_ssp_dyn): COMP=NONE for B/W
     * when BROTHER5_HWTEST_COMP_NONE is set — device support unknown. */
    comp_none = getenv("BROTHER5_HWTEST_COMP_NONE") ? 1 : 0;
    rc = brscan5_enc_ssp_dyn(cmd, sizeof(cmd), reso_x, reso_y,
                             this->uiSetting.wColorType,
                             this->uiSetting.nBrightness + 50,
                             this->uiSetting.nContrast + 50,
                             "NORMAL", comp_none);
    if (rc <= 0)
        return SANE_STATUS_INVAL;
    if (t->write(t->ctx, (const uint8_t *)cmd, (size_t)rc) != 0)
        return SANE_STATUS_IO_ERROR;
    got = 0;
    if (t->read(t->ctx, rsp, sizeof(rsp), &got) != 0 || got == 0 ||
        brscan5_rsp_ssp(rsp, (int)got) != 0) {
        if (got > 0 && brscan5_rsp_ssp_rejected(rsp, (int)got)) {
            WriteLog("brscan5_start: SSP rejected (83 53 53 50 … status "
                     "0x83) — device refused the settings");
        }
        return SANE_STATUS_IO_ERROR;
    }

    /* XSC — scan start (90 00 = empty feeder, 14-B 00 02 record = OK).
     * RESO must match the SSP settings; AREA= carries the configured
     * scan area in PIXELS at the scan resolution. The reference driver
     * computes px = round(mm * dpi / 25.4) from the SANE br-/tl- options
     * (reference full-area at 300 dpi: 0,0,2550,4200 from br-x 215.88 /
     * br-y 355.567 mm). */
    {
        char area[40];
        SANE_Fixed tlx = this->aoptVal[optTLX].w;
        SANE_Fixed brx = this->aoptVal[optBRX].w;
        SANE_Fixed tly = this->aoptVal[optTLY].w;
        SANE_Fixed bry = this->aoptVal[optBRY].w;
        double x0 = SANE_UNFIX(tlx < brx ? tlx : brx);
        double x1 = SANE_UNFIX(tlx < brx ? brx : tlx);
        double y0 = SANE_UNFIX(tly < bry ? tly : bry);
        double y1 = SANE_UNFIX(tly < bry ? bry : tly);
        snprintf(area, sizeof(area), "%ld,%ld,%ld,%ld",
                 (long)(x0 * reso_x / 25.4 + 0.5),
                 (long)(y0 * reso_y / 25.4 + 0.5),
                 (long)(x1 * reso_x / 25.4 + 0.5),
                 (long)(y1 * reso_y / 25.4 + 0.5));
        rc = brscan5_enc_xsc_dyn(cmd, sizeof(cmd), reso_x, reso_y, area);
    }
    if (rc <= 0)
        return SANE_STATUS_INVAL;
    if (t->write(t->ctx, (const uint8_t *)cmd, (size_t)rc) != 0)
        return SANE_STATUS_IO_ERROR;
    /* T8c: with paper in the feeder the device only answers XSC after it
     * grabbed the sheet (~9 s observed) — raise the idle budget for this
     * read, restore the command budget afterwards. The response buffer
     * must take a full URB: for B/W the first 00 01 block record rides
     * in the XSC response URB together with ~52 kB of page data. */
    t->idle_timeout_ms = BRSCAN5_TIMEOUT_XSC;
    got = 0;
    {
        uint8_t xrsp[BRSCAN5_DATA_URB_MAX];
        if (t->read(t->ctx, xrsp, sizeof(xrsp), &got) != 0 || got == 0) {
            t->idle_timeout_ms = BRSCAN5_TIMEOUT_CMD;
            return SANE_STATUS_IO_ERROR;
        }
        t->idle_timeout_ms = BRSCAN5_TIMEOUT_CMD;
        rc = brscan5_rsp_xsc(xrsp, (int)got);
        if (rc < 0)
            return SANE_STATUS_IO_ERROR;
        if (rc == 0) {
            WriteLog("brscan5_start: XSC 90 00 (empty feeder)");
            return SANE_STATUS_NO_DOCS;
        }
        if (rc == 2) {
            /* T8c live (t8c-p6-jam.bin): XSC 91 00 = document jam at the
             * device, no data phase, USB stays up. Map to the same status
             * the legacy path reports for SCAN_DOCJAM (0xC3) — its status
             * string "Document feeder jammed" is what paperless-scan.sh
             * greps for ('feeder jammed'). Recovery works without
             * replug. */
            WriteLog("brscan5_start: XSC 91 00 (feeder jammed)");
            return SANE_STATUS_JAMMED;
        }

        /* Scan started. Arm the parser and feed the XSC response (the
         * first 00 02 progress record / B/W: first 00 01 block record +
         * data) as the first chunk — matches the fixture record offsets
         * (parser README §Integration). */
        br5_parser_arm(this->br5->parser);
        s->armed = 1;
        if (br5_parser_feed(this->br5->parser, xrsp, got,
                            BR5_FLAG_CHUNK_START) != 0)
            return SANE_STATUS_IO_ERROR;
    }

    this->scanState.nPageCnt++;
    this->scanState.bEOF = FALSE;
    this->scanState.bScanning = TRUE;

    /* T6: receive the page JPEG and read its header now, so
     * sane_get_parameters reports the real image dimensions directly
     * after sane_start (two-phase parameter behaviour). The scanlines
     * themselves are streamed out by brscan5_read(). */
    brscan5_estimate_dims(this, &s->est_w, &s->est_h);
    st = brscan5_pump(s);
    if (st != SANE_STATUS_GOOD)
        return st;
    st = brscan5_decoder_start(s, this);
    if (st != SANE_STATUS_GOOD)
        return st;

    WriteLog("brscan5_start: scan started (rc=GOOD, page %ldx%ld)",
             s->real_w, s->real_h);
    return SANE_STATUS_GOOD;
}

/* Parser event callback: monitor progress/records, accumulate JPEG into the
 * session's own page buffer (doubling, cap 64 MB), mark page completion on
 * EOI and session end on SESSION_END. See step 4 of the T5 spec. */
static void
brscan5_on_event(const br5_event_t *ev, void *userdata)
{
    Brother_Scanner *this = (Brother_Scanner *)userdata;
    brscan5_session_t *s = this ? this->br5 : NULL;

    if (!s)
        return;
    switch (ev->type) {
    case BR5_EV_RECORD:
        if (ev->record_id == BR5_RECORD_PROGRESS)
            s->n_progress++;
        if (ev->record_id == BR5_RECORD_BLOCK)
            s->rle_mode = 1;
        break;
    case BR5_EV_RLE_DATA:
        /* RLENGTH payload: append to the page buffer and record the
         * line boundary (each 00 01 block = one scanline). */
        s->rle_mode = 1;
        if (ev->jpeg_len) {
            size_t need = s->page_len + ev->jpeg_len;
            if (need > BR5_JPEG_CAP_MAX) {
                s->n_errors++;
                WriteLog("brscan5 read: page buffer overflow (cap 64 MB)");
                s->eof = 1;
                break;
            }
            if (need > s->page_cap) {
                size_t nc = s->page_cap ? s->page_cap : 65536;
                while (nc < need)
                    nc *= 2;
                if (nc > BR5_JPEG_CAP_MAX)
                    nc = BR5_JPEG_CAP_MAX;
                uint8_t *nb = (uint8_t *)realloc(s->page_buf, nc);
                if (!nb) {
                    s->n_errors++;
                    WriteLog("brscan5 read: OOM growing page buffer");
                    s->eof = 1;
                    break;
                }
                s->page_buf = nb;
                s->page_cap = nc;
            }
            memcpy(s->page_buf + s->page_len, ev->jpeg_data, ev->jpeg_len);
            s->page_len += ev->jpeg_len;
        }
        if (s->rle_nlines == s->rle_ocap) {
            size_t nc = s->rle_ocap ? s->rle_ocap * 2 : 256;
            size_t *nb2 = (size_t *)realloc(s->rle_offs, nc * sizeof(*nb2));
            if (!nb2) {
                s->n_errors++;
                WriteLog("brscan5 read: OOM growing line table");
                s->eof = 1;
                break;
            }
            s->rle_offs = nb2;
            s->rle_ocap = nc;
        }
        s->rle_offs[s->rle_nlines++] = s->page_len;
        break;
    case BR5_EV_JPEG_DATA:
        /* Append the chunk to the session page buffer (doubling). */
        if (ev->jpeg_len) {
            size_t need = s->page_len + ev->jpeg_len;
            if (need > BR5_JPEG_CAP_MAX) {
                s->n_errors++;
                WriteLog("brscan5 read: page buffer overflow (cap 64 MB)");
                s->eof = 1;
                break;
            }
            if (need > s->page_cap) {
                size_t nc = s->page_cap ? s->page_cap : 65536;
                while (nc < need)
                    nc *= 2;
                if (nc > BR5_JPEG_CAP_MAX)
                    nc = BR5_JPEG_CAP_MAX;
                uint8_t *nb = (uint8_t *)realloc(s->page_buf, nc);
                if (!nb) {
                    s->n_errors++;
                    WriteLog("brscan5 read: OOM growing page buffer");
                    s->eof = 1;
                    break;
                }
                s->page_buf = nb;
                s->page_cap = nc;
            }
            memcpy(s->page_buf + s->page_len, ev->jpeg_data, ev->jpeg_len);
            s->page_len += ev->jpeg_len;
        }
        break;
    case BR5_EV_JPEG_EOI:
        /* Whole page JPEG now complete in the session page buffer; the
         * libjpeg decoder is created by brscan5_start()/brscan5_read(). */
        s->page_ready = 1;
        break;
    case BR5_EV_PAGE_END:
        /* Page-end marker: nothing to do for whole-page delivery —
         * except in RLENGTH mode, where there is no JPEG EOI and this
         * record marks the end of the page payload. */
        if (s->rle_mode)
            s->page_ready = 1;
        break;
    case BR5_EV_SESSION_END:
        s->eof = 1;
        break;
    case BR5_EV_WARN:
        s->n_warns++;
        if (ev->message)
            WriteLog("brscan5 parser warn: %s", ev->message);
        break;
    case BR5_EV_ERROR:
        s->n_errors++;
        if (ev->message)
            WriteLog("brscan5 parser error: %s", ev->message);
        s->eof = 1;
        break;
    default:
        break;
    }
}

/* Drive the parser with IN URBs (262144 B buffers) until a page is complete
 * or the session ends. Returns SANE_STATUS_GOOD if the page is ready,
 * SANE_STATUS_EOF if the session ended with no page, or a SANE error status. */
static SANE_Status
brscan5_pump(brscan5_session_t *s)
{
    uint8_t urb[BRSCAN5_DATA_URB_MAX];
    size_t  got = 0;

    /* Data phase: 30 s idle budget (vs 5 s for command responses). */
    s->tport.idle_timeout_ms = BRSCAN5_TIMEOUT_DATA;
    for (;;) {
        if (s->tport.read(s->tport.ctx, urb, sizeof(urb), &got) != 0) {
            WriteLog("brscan5_pump: transport read error");
            return SANE_STATUS_IO_ERROR;
        }
        if (got == 0) {
            /* EOF: fixture exhausted / device silent. If no page was
             * produced this is a clean end of stream. */
            WriteLog("brscan5_pump: transport EOF (got=0)");
            s->eof = 1;
            return (s->page_ready) ? SANE_STATUS_GOOD : SANE_STATUS_EOF;
        }
        if (br5_parser_feed(s->parser, urb, got,
                            BR5_FLAG_CHUNK_START) != 0) {
            WriteLog("brscan5_pump: parser fatal error");
            return SANE_STATUS_IO_ERROR;
        }
        if (s->n_errors)
            return SANE_STATUS_IO_ERROR;
        if (s->eof)
            return (s->page_ready) ? SANE_STATUS_GOOD : SANE_STATUS_EOF;
        if (s->page_ready)
            return SANE_STATUS_GOOD;
    }
}

/* Deliver one decoded scanline to the caller buffer, converting the
 * libjpeg output to the SANE frame format if needed:
 *   RGB/gray 8-bit: libjpeg output is used as-is (row_bytes ==
 *                   bytes_per_line for those modes).
 *   B/W depth 1:    threshold-pack the grayscale row, MSB first
 *                   (pixel >= 128 -> black bit 0? -> white=1? We map
 *                   luminance < 128 to 1 (black), >= 128 to 0 (white),
 *                   SANE 1-bit convention: 1 = black). Untested.
 * Returns the number of bytes written (bytes_per_line) or 0 on error. */
static size_t
brscan5_emit_row(brscan5_session_t *s, unsigned char *dst)
{
    const SANE_Parameters *p = &s->params;

    if (p->depth == 1) {
        /* Pack 8 grayscale pixels per byte, MSB first: bit set = black. */
        int i, bit;
        long w = p->pixels_per_line;
        unsigned char byte = 0;
        for (i = 0; i < w; i++) {
            bit = 7 - (i & 7);
            if (s->row_buf[i] < 128)
                byte |= (unsigned char)(1u << bit);
            if (bit == 0 || i == w - 1) {
                *dst++ = byte;
                byte = 0;
            }
        }
        return (size_t)p->bytes_per_line;
    }
    memcpy(dst, s->row_buf, s->row_bytes);
    return s->row_bytes;
}

int
brscan5_read(Brother_Scanner *this, char *buf, int maxlen, int *len)
{
    brscan5_session_t *s = this->br5;
    const SANE_Parameters *p;
    SANE_Int line_bytes;
    size_t filled = 0;
    SANE_Status st;

    *len = 0;

    /* T7: after sane_cancel the sticky cancel latch wins over EOF — reads
     * keep returning SANE_STATUS_CANCELLED until the frontend starts a
     * fresh scan (sane_start clears the latch and re-runs the handshake).
     * This mirrors the legacy paths (AbortPageScan sets bCanceled, the
     * read loops return SANE_STATUS_CANCELLED, brother_scanner.c:644). */
    if (s && s->canceled)
        return SANE_STATUS_CANCELLED;
    if (!s || s->eof)
        return SANE_STATUS_EOF;

    p = &s->params;
    line_bytes = p->bytes_per_line;

    /* sane_read must return whole scanlines; the frontend guarantees a
     * buffer of at least one line (SANE standard). */
    if (!s->dec_active && !s->page_ready) {
        st = brscan5_pump(s);
        if (st != SANE_STATUS_GOOD)
            return st;
        st = brscan5_decoder_start(s, this);
        if (st != SANE_STATUS_GOOD)
            return st;
    }
    if (maxlen < line_bytes)
        return SANE_STATUS_INVAL;

    /* T8c RLENGTH: the raw 1-bit bitmap is pre-decoded; serve rows
     * directly (no libjpeg). */
    if (s->rle_active) {
        while ((long)s->lines_out < p->lines &&
               filled + (size_t)line_bytes <= (size_t)maxlen) {
            memcpy(buf + filled,
                   s->raw_buf + (size_t)s->lines_out * s->row_bytes,
                   (size_t)line_bytes);
            filled += (size_t)line_bytes;
            s->lines_out++;
        }
        if (filled) {
            *len = (SANE_Int)filled;
            return SANE_STATUS_GOOD;
        }
        WriteLog("brscan5 read: RLENGTH page decoded (%ld lines)",
                 s->lines_out);
        brscan5_decoder_teardown(s);
        brscan5_drain(s);
        return SANE_STATUS_EOF;
    }

    /* Decode scanlines into the caller buffer as long as whole lines
     * fit (libjpeg streaming — never holds more than one scanline). */
    if (setjmp(s->jerr.jb)) {
        WriteLog("brscan5 read: libjpeg error while decoding");
        brscan5_decoder_teardown(s);
        s->n_errors++;
        s->eof = 1;
        return SANE_STATUS_IO_ERROR;
    }
    while ((long)s->lines_out < p->lines &&
           filled + (size_t)line_bytes <= (size_t)maxlen) {
        JSAMPLE *rowptr[1];

        rowptr[0] = (JSAMPROW)s->row_buf;
        if (jpeg_read_scanlines(&s->cinfo, rowptr, 1) < 1)
            break;                       /* truncated stream */
        filled += brscan5_emit_row(s, (unsigned char *)buf + filled);
        s->lines_out++;
    }
    if (filled) {
        *len = (SANE_Int)filled;
        return SANE_STATUS_GOOD;
    }

    /* All scanlines delivered: finish the decoder, free the page JPEG
     * buffer and drain the trailing records (PAGE_END/SESSION_END). */
    if (jpeg_finish_decompress(&s->cinfo))
        WriteLog("brscan5 read: page decoded (%ld lines)", s->lines_out);
    brscan5_decoder_teardown(s);
    brscan5_drain(s);
    return SANE_STATUS_EOF;
}

/* ------------------------------------------------------------------
 * brscan5 status mapping (T7, consolidated):
 *
 *   transport write/read error, idle/URB timeout       -> SANE_STATUS_IO_ERROR
 *   CKD response 00 02 (feeder empty)                  -> SANE_STATUS_NO_DOCS
 *   XSC response 90 00 (empty feeder)                  -> SANE_STATUS_NO_DOCS
 *   parser BR5_EV_ERROR (protocol violation, overflow) -> SANE_STATUS_IO_ERROR
 *   libjpeg decode error / truncated stream            -> SANE_STATUS_IO_ERROR
 *   unknown record id in the data phase                -> BR5_EV_WARN, skipped
 *   page fully delivered (SESSION_END drained)         -> SANE_STATUS_EOF
 *   read during/after sane_cancel                      -> SANE_STATUS_CANCELLED
 *
 * sane_cancel() itself is void in the SANE API and the ops->cancel hook
 * returns 0; by SANE convention cancelling never surfaces an error status
 * — error reporting happens in the surrounding sane_read/sane_start.
 * ------------------------------------------------------------------ */

int
brscan5_cancel(Brother_Scanner *this)
{
    brscan5_session_t *s = this->br5;
    int was_armed = s ? s->armed : 0;

    WriteLog("brscan5_cancel: armed=%d", was_armed);

    /* SANE semantics: cancel only flags the abort — the caller (sane_)
     * performs the synchronous cleanup here, so this function IS the
     * cleanup (brother.c sane_cancel runs it inline). */
    this->scanState.bCanceled = TRUE;      /* AbortPageScan convention */
    if (s) {
        /* 1. sticky latch: reads return SANE_STATUS_CANCELLED until the
         *    next sane_start. */
        s->canceled = 1;
        /* 2. decoder teardown: jpeg_destroy_decompress only — NEVER
         *    jpeg_finish_decompress (the page is incomplete from the
         *    frontend's point of view; finishing would emit warnings and
         *    waste time). */
        brscan5_decoder_teardown(s);
        s->have_real = 0;
        /* 3. transport drain (only while a scan is running): discard
         *    pending IN data so the device/fixture is quiescent. Replay:
         *    consume IN entries until the next OUT-Expect; USB: ~1 s
         *    quiesce window. */
        if (was_armed && s->tport.drain)
            s->tport.drain(s->tport.ctx);
        /* 4. endpoint recovery hook (USB: clear_halt 0x83/0x04; replay:
         *    no-op). */
        if (was_armed && s->tport.reset)
            s->tport.reset(s->tport.ctx);
        /* 5. parser back to IDLE, side buffers freed — the next start
         *    re-arms it after a fresh Q→QDI→CKD→SSP→XSC handshake. */
        br5_parser_reset(s->parser);
        s->armed = 0;
        s->eof = 1;
    }
    this->scanState.bScanning = FALSE;
    return 0;
}

int
brscan5_close(Brother_Scanner *this)
{
    if (this->br5) {
        brscan5_session_t *s = this->br5;
        if (s->tport.write || s->tport.read)
            brscan5_transport_close(&s->tport);
        if (s->parser)
            br5_parser_free(s->parser);
        brscan5_decoder_teardown(s);
        free(s);
        this->br5 = NULL;
    }
    /* Real hardware: session-close control message (BREQ_GET_CLOSE) +
     * release_interface + usb_close. Replay mode has no USB handle
     * (hScanner->usb stays NULL) and skips this — the fixture scan must
     * never touch libusb. */
    if (this->hScanner && this->hScanner->usb) {
        CloseDevice(this->hScanner);
        usb_close(this->hScanner->usb);
        this->hScanner->usb = NULL;
    }
    return 0;
}