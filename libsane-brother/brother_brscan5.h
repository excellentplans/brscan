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
 * it via a single dispatch gate at sane_open() when the model has a
 * brscan5 device profile (see below).
 */

#ifndef _H_BROTHER_BRSCAN5
#define _H_BROTHER_BRSCAN5

#include <stdint.h>

#include "brother.h"

/* ------------------------------------------------------------------
 * Device profiles — data-driven model facts for the brscan5 layer.
 *
 * brscan5 identity is keyed by USB vendor+product ID via the profile
 * table below, NOT by the ini's seriesNo: the ini's seriesNo only
 * feeds the legacy default feature tables (get_model_config()).
 * A profile exists for exactly the devices this backend knows how to
 * drive with the brscan5 command layer; the dispatch gate
 * (brscan5_is_brscan5) is "a profile exists for this model".
 *
 * Note: the ini's seriesNo for a brscan5 model is irrelevant to the
 * dispatch — see docs/dispatch-points.md.
 * ------------------------------------------------------------------ */

struct brscan5_model_profile {
    uint16_t      product_id;        /* USB product ID (vendor 0x04f9)  */
    const char   *name;              /* informational                   */
    RESOLIST      reso;              /* supported resolutions           */
    SCANMODELIST  scanmode;          /* supported scan modes            */
    SCANSRCLIST   scansrc;           /* supported scan sources          */
    double        scan_area_width;   /* scan area width  (mm)           */
    double        scan_area_height;  /* scan area height (mm)           */
    unsigned char ep_in;             /* bulk IN endpoint                */
    unsigned char ep_out;            /* bulk OUT endpoint               */
};

/* The profile table (one entry per supported brscan5 model). */
extern const struct brscan5_model_profile brscan5_profiles[];
extern const int brscan5_profile_count;

/* Profile lookup by USB vendor+product ID. NULL when unknown; the
 * vendor must be SCANNER_VENDOR (0x04f9). */
const struct brscan5_model_profile *brscan5_find_profile(uint16_t vendor,
                                                         uint16_t product);

/* Dispatch gate: true when a brscan5 profile exists for this model. */
int brscan5_is_brscan5(const MODELINF *m);

/* Apply the profile's feature facts to a MODELCONFIG (after
 * get_model_config()): identical field writes, values from the
 * profile. NULL profile is a no-op. */
void brscan5_apply_model_profile(const struct brscan5_model_profile *p,
                                 MODELCONFIG *mc);

/* Pure protocol module: command encoders + response readers (split out of
 * brother_brscan5.c so they can be unit-tested standalone). */
#include "brscan5_proto.h"

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
 * Ops dispatch — the command-layer entry points (context for T5-T7).
 *
 * The struct is defined here and instantiated in brother.c as the two
 * global dispatch handles (brscan5_ops_dispatch for models with a
 * brscan5 device profile and a static legacy table for every other
 * model). Return values follow the
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

/* brscan5 command layer (models with a brscan5 device profile). Defined
 * in brother.c, points to the brscan5_* functions below. */
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
 *   USB     — libusb bulk on the profile's endpoints (ep_in/ep_out of
 *             the device profile; real hardware).
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
     * clear_halt on both bulk endpoints (the profile's ep_in/ep_out).
     * Replay: no-op.
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