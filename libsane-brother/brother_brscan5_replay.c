/*
 * brother_brscan5_replay.c — replay transport for the brscan5 command layer.
 *
 * Implements the brscan5_transport interface against a fixture file instead
 * of real hardware. Used when BROTHER5_REPLAY=<path> is set at sane_open.
 * It performs NO libusb/USB access at all — this transport has no USB handle.
 *
 * Fixture file format (TLV, little-endian):
 *
 *   A sequence of entries, each entry is:
 *     uint8_t  direction  0x01 = OUT-Expect, 0x02 = IN-Deliver
 *                         0x03 = IN-Timeout-Simulation (T7)
 *                         0x04 = IN-Error-Simulation (T7)
 *                         0x05 = OUT-Control-Expect (T8b)
 *                         0x06 = IN-Control-Deliver (T8b)
 *     uint32_t len        (little-endian)
 *     uint8_t  data[len]
 *
 *   0x03 entries carry NO payload: the len field itself is the simulated
 *   timeout in milliseconds. The next read() sleeps min(ms, 50) real ms
 *   (so tests stay fast) and then fails with a timeout error (-1), i.e.
 *   the backend maps it to SANE_STATUS_IO_ERROR. The entry is consumed.
 *   0x04 entries carry no payload either (len reserved, 0): the next
 *   read() fails immediately with a transport error (-1). Consumed.
 *
 * T8b vendor control transfers (brscan5_ctrl_dance(), brscan5_open()):
 *
 *   0x05  OUT-Control-Expect — the 8-B USB setup packet the backend must
 *         send (asserted byte-exact: bmRequestType, bRequest, wValue LE,
 *         wIndex LE, wLength LE). Always followed by a 0x06 entry.
 *   0x06  IN-Control-Deliver — the response payload of the preceding
 *         control transfer (copied verbatim into the caller's buffer;
 *         *got = len). A control() call consumes exactly one 0x05 +
 *         one 0x06 entry.
 *
 * Behavior:
 *   write(): asserts the sent bytes are byte-identical to the next
 *            OUT-Expect entry. On mismatch -> returns -1 and reports
 *            (DBG(1), SANE debug channel) the offending offset and the
 *            expected/actual bytes. On match the entry is consumed.
 *   read() : returns the next IN-Deliver entry verbatim. A 0x01 direction
 *            byte (OUT-Expect) is NOT consumed — it ends the IN stream
 *            with *got = 0 (EOF semantics), so a cancel drain can stop
 *            right at the next command. 0x03/0x04 inject the simulated
 *            fault (see above). EOF (fixture exhausted) => *got = 0.
 *   drain(): consumes IN entries (and fault markers) discarding their
 *            bytes until the next OUT-Expect entry or EOF — used by the
 *            cancel path to reposition the fixture at the next command.
 *   reset(): no-op (replay has no endpoints to clear).
 *   debug  : every write/read/control/drain action is traced at DBG(5)
 *            (SANE_DEBUG_BROTHER), direction, length and stream offset.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "brother_brscan5.h"
#include "brscan5_dbg.h"

#define BR5_REPLAY_DIR_OUT      0x01
#define BR5_REPLAY_DIR_IN       0x02
#define BR5_REPLAY_DIR_TIMEOUT  0x03
#define BR5_REPLAY_DIR_ERROR    0x04
#define BR5_REPLAY_DIR_CTRL_EXP 0x05
#define BR5_REPLAY_DIR_CTRL_DEL 0x06

#define BR5_REPLAY_CTRL_SETUP_LEN 8

/* Real-sleep cap for simulated timeouts (keeps tests fast). */
#define BR5_REPLAY_TIMEOUT_SLEEP_CAP_MS 50

typedef struct {
    FILE   *fp;
    long    out_off;      /* fixture offset of the next OUT-Expect entry */
    long    in_off;       /* fixture offset of the next IN-Deliver entry */
} brscan5_replay_t;

/* Read a 4-byte little-endian length. Returns 0 on success. */
static int
brscan5_replay_getlen(FILE *fp, uint32_t *out)
{
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4)
        return -1;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

/* Read + validate the direction byte of the next entry. Returns 0 on
 * success (direction matches), -1 on EOF/mismatch. */
static int
brscan5_replay_getdir(FILE *fp, int want)
{
    int d = fgetc(fp);
    if (d == EOF)
        return -1;
    if (d != want) {
        DBG(1,             "brscan5 replay: unexpected direction byte %02x "
                           "(wanted %02x)\n", d, want);
        return -1;
    }
    return 0;
}

/* ---- transport interface ---------------------------------------------- */

static int
brscan5_replay_write(void *ctx, const uint8_t *buf, size_t len)
{
    brscan5_replay_t *r = (brscan5_replay_t *)ctx;
    uint32_t want;
    int      rc = 0;

    if (brscan5_replay_getdir(r->fp, BR5_REPLAY_DIR_OUT) != 0) {
        DBG(1,             "brscan5 replay: OUT record %ld: expected OUT "
                           "entry (got %zu B sent)\n", r->out_off, len);
        return -1;
    }
    if (brscan5_replay_getlen(r->fp, &want) != 0) {
        DBG(1,             "brscan5 replay: OUT record %ld: EOF before length "
                           "(%zu B sent)\n", r->out_off, len);
        return -1;
    }
    if (want != len) {
        DBG(1,             "brscan5 replay: OUT record %ld: length mismatch: "
                           "sent %zu B, fixture %u B\n", r->out_off, len,
                           (unsigned)want);
        return -1;
    }
    {
        uint8_t *exp = (uint8_t *)malloc(want ? want : 1);
        if (!exp)
            return -1;
        if (fread(exp, 1, want, r->fp) != want) {
            free(exp);
            return -1;
        }
        if (memcmp(exp, buf, want) != 0) {
            size_t i = 0;
            while (i < want && exp[i] == buf[i])
                i++;
            DBG(1,             "brscan5 replay: OUT byte mismatch at record "
                               "%ld, offset %zu: expected %02x got %02x "
                               "(OUT-assertion FAILED)\n", r->out_off, i,
                               exp[i], buf[i]);
            rc = -1;
        }
        free(exp);
    }
    DBG(5,             "brscan5 replay: write %zu B @ %ld%s\n", len,
                       r->out_off, rc ? " (FAIL)" : "");
    r->out_off += (long)(5 + want);
    return rc;
}

static int
brscan5_replay_read(void *ctx, uint8_t *buf, size_t len, size_t *got)
{
    brscan5_replay_t *r = (brscan5_replay_t *)ctx;
    uint32_t want;
    int d;

    *got = 0;

    d = fgetc(r->fp);
    if (d == EOF)
        return 0;                       /* EOF: fixture exhausted */
    if (d == BR5_REPLAY_DIR_OUT) {
        /* Stop marker: the next command is pending (peeked, NOT
         * consumed) — models "no more IN data right now". */
        ungetc(d, r->fp);
        return 0;
    }
    if (d == BR5_REPLAY_DIR_TIMEOUT || d == BR5_REPLAY_DIR_ERROR) {
        /* Fault injection (T7): the len field of a 0x03 entry is the
         * simulated timeout in ms (no payload); 0x04 has no payload. */
        if (brscan5_replay_getlen(r->fp, &want) != 0)
            return 0;                   /* truncated: model EOF */
        r->in_off += 5;
        DBG(5,             "brscan5 replay: fault entry %s (%u ms) @ %ld\n",
                           (d == BR5_REPLAY_DIR_TIMEOUT) ? "timeout"
                                                         : "error",
                           (d == BR5_REPLAY_DIR_TIMEOUT) ? (unsigned)want : 0u,
                           r->in_off - 5);
        if (d == BR5_REPLAY_DIR_TIMEOUT) {
            unsigned ms = (want > BR5_REPLAY_TIMEOUT_SLEEP_CAP_MS)
                          ? BR5_REPLAY_TIMEOUT_SLEEP_CAP_MS : (unsigned)want;
            if (ms)
                usleep(ms * 1000u);
        }
        return -1;                      /* transport/timeout error */
    }
    if (d != BR5_REPLAY_DIR_IN) {
        DBG(1,             "brscan5 replay: unexpected direction byte %02x "
                           "(IN entry %ld)\n", d, r->in_off);
        return 0;
    }
    if (brscan5_replay_getlen(r->fp, &want) != 0)
        return 0;                       /* truncated: model EOF */
    if (want > len) {
        DBG(1,             "brscan5 replay: IN entry %ld is %u B, caller "
                           "buffer %zu B\n", r->in_off, (unsigned)want, len);
        return -1;
    }
    if (fread(buf, 1, want, r->fp) != want)
        return -1;
    *got = (size_t)want;
    DBG(5,             "brscan5 replay: read  %zu B @ %ld\n", (size_t)want,
                       r->in_off);
    r->in_off += (long)(5 + want);
    return 0;
}

/* Cancel drain: consume and discard IN entries (and fault markers) until
 * the next OUT-Expect entry or EOF. The OUT entry is peeked and left in
 * place so the next sane_start finds the expected command. */
static int
brscan5_replay_drain(void *ctx)
{
    brscan5_replay_t *r = (brscan5_replay_t *)ctx;
    size_t discarded = 0;

    for (;;) {
        int d = fgetc(r->fp);
        if (d == EOF)
            break;
        if (d == BR5_REPLAY_DIR_OUT) {
            ungetc(d, r->fp);
            break;
        }
        if (d == BR5_REPLAY_DIR_TIMEOUT || d == BR5_REPLAY_DIR_ERROR) {
            uint32_t v;
            if (brscan5_replay_getlen(r->fp, &v) != 0)
                break;
            r->in_off += 5;
            continue;
        }
        if (d == BR5_REPLAY_DIR_CTRL_EXP) {
            /* Control entry (T8b): consume the 8-B setup + the 0x06
             * deliver entry, keep draining. */
            uint32_t clen;
            if (brscan5_replay_getlen(r->fp, &clen) != 0 ||
                fseek(r->fp, (long)clen, SEEK_CUR) != 0)
                break;
            r->in_off += (long)(5 + clen);
            if (fgetc(r->fp) != BR5_REPLAY_DIR_CTRL_DEL ||
                brscan5_replay_getlen(r->fp, &clen) != 0 ||
                fseek(r->fp, (long)clen, SEEK_CUR) != 0)
                break;
            r->in_off += (long)(5 + clen);
            continue;
        }
        if (d != BR5_REPLAY_DIR_IN) {
            DBG(1,             "brscan5 replay: drain: unexpected direction "
                               "byte %02x\n", d);
            break;
        }
        {
            uint32_t want;
            if (brscan5_replay_getlen(r->fp, &want) != 0)
                break;                  /* truncated */
            if (fseek(r->fp, (long)want, SEEK_CUR) != 0) {
                /* not seekable: fall back to byte-wise discard */
                uint32_t left = want;
                while (left-- > 0 && fgetc(r->fp) != EOF)
                    ;
            }
            r->in_off += (long)(5 + want);
            discarded += want;
        }
    }
    DBG(5,             "brscan5 replay: drain done (%zu B discarded)\n",
                       discarded);
    return 0;
}

/* Post-cancel endpoint recovery: replay has no USB endpoints. */
static int
brscan5_replay_reset(void *ctx)
{
    (void)ctx;
    return 0;
}

/* Vendor control transfer (T8b): assert the next fixture entries are a
 * 0x05 OUT-Control-Expect (8-B setup, byte-exact vs. the setup the
 * backend builds from bmRequestType/bRequest/wValue/wIndex/wLength)
 * followed by a 0x06 IN-Control-Deliver (response payload). */
static int
brscan5_replay_control(void *ctx, unsigned char bmRequestType,
                       unsigned char bRequest, unsigned wValue,
                       unsigned wIndex, unsigned wLength,
                       unsigned char *buf, int buflen, int *got)
{
    brscan5_replay_t *r = (brscan5_replay_t *)ctx;
    unsigned char want_setup[BR5_REPLAY_CTRL_SETUP_LEN];
    unsigned char exp_setup[BR5_REPLAY_CTRL_SETUP_LEN];
    uint32_t want;

    if (wLength > (unsigned)buflen)
        return -1;                      /* caller buffer too small */
    if (brscan5_replay_getdir(r->fp, BR5_REPLAY_DIR_CTRL_EXP) != 0) {
        DBG(1,             "brscan5 replay: control: expected 0x05 "
                           "OUT-Control-Expect entry\n");
        return -1;
    }
    if (brscan5_replay_getlen(r->fp, &want) != 0 ||
        want != BR5_REPLAY_CTRL_SETUP_LEN) {
        DBG(1,             "brscan5 replay: control: 0x05 entry must carry "
                           "an 8-B setup packet (got %u)\n", (unsigned)want);
        return -1;
    }
    if (fread(exp_setup, 1, sizeof(exp_setup), r->fp) !=
        sizeof(exp_setup))
        return -1;
    want_setup[0] = bmRequestType;
    want_setup[1] = bRequest;
    want_setup[2] = (unsigned char)(wValue & 0xff);
    want_setup[3] = (unsigned char)(wValue >> 8);
    want_setup[4] = (unsigned char)(wIndex & 0xff);
    want_setup[5] = (unsigned char)(wIndex >> 8);
    want_setup[6] = (unsigned char)(wLength & 0xff);
    want_setup[7] = (unsigned char)((wLength >> 8) & 0xff);
    if (memcmp(exp_setup, want_setup, sizeof(exp_setup)) != 0) {
        DBG(1,             "brscan5 replay: control: setup mismatch: "
                           "expected %02x%02x%02x%02x%02x%02x%02x%02x, "
                           "backend built %02x%02x%02x%02x%02x%02x%02x%02x\n",
                           exp_setup[0], exp_setup[1], exp_setup[2],
                           exp_setup[3], exp_setup[4], exp_setup[5],
                           exp_setup[6], exp_setup[7],
                           want_setup[0], want_setup[1], want_setup[2],
                           want_setup[3], want_setup[4], want_setup[5],
                           want_setup[6], want_setup[7]);
        return -1;
    }
    r->out_off += (long)(5 + want);

    /* The response: 0x06 IN-Control-Deliver. */
    if (brscan5_replay_getdir(r->fp, BR5_REPLAY_DIR_CTRL_DEL) != 0) {
        DBG(1,             "brscan5 replay: control: expected 0x06 "
                           "IN-Control-Deliver after the 0x05 entry\n");
        return -1;
    }
    if (brscan5_replay_getlen(r->fp, &want) != 0)
        return -1;
    if (want > (uint32_t)buflen) {
        DBG(1,             "brscan5 replay: control: response %u B exceeds "
                           "caller buffer %d B\n", (unsigned)want, buflen);
        return -1;
    }
    if (fread(buf, 1, want, r->fp) != want)
        return -1;
    if (got)
        *got = (int)want;
    DBG(5,             "brscan5 replay: control %02x/%02x -> %u B\n",
                       bmRequestType, bRequest, (unsigned)want);
    r->in_off += (long)(5 + want);
    return 0;
}

void
brscan5_replay_close(brscan5_transport_t *t)
{
    if (!t || !t->ctx)
        return;
    brscan5_replay_t *r = (brscan5_replay_t *)t->ctx;
    if (r->fp)
        fclose(r->fp);
    free(r);
    t->ctx = NULL;
    t->write = NULL;
    t->read  = NULL;
    t->drain = NULL;
    t->reset = NULL;
    t->control = NULL;
}

/* ---- factory ---------------------------------------------------------- */

int
brscan5_replay_open(brscan5_transport_t *t, const char *path)
{
    brscan5_replay_t *r;
    int first_dir;

    if (!t || !path)
        return -1;
    r = (brscan5_replay_t *)calloc(1, sizeof(*r));
    if (!r)
        return -1;
    r->fp = fopen(path, "rb");
    if (!r->fp) {
        DBG(1,             "brscan5 replay: cannot open '%s'\n", path);
        free(r);
        return -1;
    }

    /* Validate the first entry header so a bad fixture fails at open, not
     * halfway through the handshake. 0x05 (OUT-Control-Expect) is valid
     * since T8b — the fixture starts with the session-open GET_OPEN
     * control pair. */
    first_dir = fgetc(r->fp);
    if (first_dir == EOF ||
        (first_dir != BR5_REPLAY_DIR_OUT && first_dir != BR5_REPLAY_DIR_IN &&
         first_dir != BR5_REPLAY_DIR_CTRL_EXP)) {
        DBG(1,             "brscan5 replay: '%s' is not a valid TLV fixture "
                           "(bad leading direction byte %d)\n", path, first_dir);
        fclose(r->fp);
        free(r);
        return -1;
    }
    rewind(r->fp);

    t->write = brscan5_replay_write;
    t->read  = brscan5_replay_read;
    t->drain = brscan5_replay_drain;
    t->reset = brscan5_replay_reset;
    t->control = brscan5_replay_control;
    /* Replay does not use the USB timeout fields; timeouts are simulated
     * with 0x03 fixture entries. Set the defaults anyway for symmetry. */
    t->urb_timeout_ms  = 1000;
    t->idle_timeout_ms = 5000;
    t->is_replay = 1;
    t->ctx   = r;
    DBG(1,             "brscan5 transport: REPLAY from '%s' (no USB)\n",
                       path);
    return 0;
}