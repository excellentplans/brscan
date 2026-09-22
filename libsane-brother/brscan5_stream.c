/*
 * brscan5_stream.c — incremental byte-oriented parser for the Brother DS-640
 * scan-data stream (USB EP 0x83 IN).
 *
 * Protocol reverse-engineered from USB captures of a Brother DS-640;
 * see docs/brscan5-protocol.md.
 *
 * Framing rule (belegt, siehe fixtures/framing.analysis.json):
 *   - Status records (00 02 / 00 11 / 00 21 / 00 20) arrive as their OWN
 *     short URBs with exactly their record length (14/12/4/2 B).
 *   - JPEG data arrives in large (262144-byte, last 221459-byte) URBs.
 *   - Hence record detection is URB-boundary + length driven, NOT pattern
 *     scanning: a short chunk matching a known record length at a chunk
 *     start is a record; every byte inside a data chunk (including any
 *     00 02 pattern) is JPEG payload.
 *   - Progress records can be interleaved INSIDE the JPEG byte range (the
 *     2nd progress record of the reference capture sits at stream offset
 *     524302, i.e. after JPEG byte 524288). They must be recognized and
 *     skipped even in ST_DATA so the collected JPEG stays byte-identical
 *     to the reference.
 *   - The device sends ~1.53 MB but the decodable JPEG is 627,980 B: the
 *     parser MUST terminate at EOI (ff d9) and discard trailing data.
 *
 * No assumption is made about chunk sizes (256 KB is host-URB reality, not
 * a protocol invariant): feeding is byte-oriented and chunk boundaries only
 * matter for record detection via BR5_FLAG_CHUNK_START.
 */

#include "brscan5_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BR5_JPEG_CAP_INIT  (64u * 1024u)   /* start cap, doubles on growth */
#define BR5_MAX_RECORD_LEN 64u             /* status records are <= 14 B   */
#define BR5_TRAILING_WARN  4096u           /* trailing-after-EOI warn cap  */

struct br5_parser {
    br5_event_cb cb;
    void *ud;

    br5_state_t state;
    uint64_t stream_pos;          /* bytes consumed so far */

    /* JPEG accumulation (doubling, cap BR5_JPEG_CAP_MAX) */
    uint8_t *jpeg_buf;
    size_t   jpeg_len;
    size_t   jpeg_cap;
    int      jpeg_eoi;

    /* 1-byte overlap for EOI (ff d9) across chunk boundaries */
    int      pending_ff;

    /* record accumulation across chunks (non-DATA phases only) */
    uint8_t  rec_buf[BR5_MAX_RECORD_LEN];
    size_t   rec_accum_len;       /* bytes buffered so far                */
    size_t   rec_expected_len;    /* 0 = 2nd byte not seen yet            */
    uint64_t rec_start_pos;       /* stream offset where the record began */

    /* trailing bytes after EOI */
    uint64_t trailing;
    int      trailing_warned;

    /* T8c RLENGTH mode (B/W): the page payload is a sequence of
     * 00 01 data-block records, each carrying one packbits-compressed
     * scanline. rle_pend_len = payload bytes still owed by the current
     * block, rle_lines = completed blocks (= scanlines). The payload
     * accumulates in rle_line_buf so each BR5_EV_RLE_DATA event carries
     * exactly one complete scanline, even when a block payload spans
     * chunk boundaries. */
    int      rle_mode;
    size_t   rle_pend_len;
    size_t   rle_lines;
    uint8_t *rle_line_buf;
    size_t   rle_line_len;
    size_t   rle_line_cap;

    char     msg[96];             /* event message scratch                */
};

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static void emit_ev(br5_parser_t *p, uint64_t pos, br5_event_type_t type,
                    uint16_t rec_id, const uint8_t *rec_payload, size_t rec_len,
                    const uint8_t *jpeg_data, size_t jpeg_len,
                    const char *message)
{
    br5_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.state = p->state;
    ev.stream_pos = pos;
    ev.record_id = rec_id;
    ev.record_payload = rec_payload;
    ev.record_len = rec_len;
    ev.jpeg_data = jpeg_data;
    ev.jpeg_len = jpeg_len;
    ev.message = message;
    if (p->cb)
        p->cb(&ev, p->ud);
}

static uint16_t rec_id_of(const uint8_t *b)
{
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

/* Expected total length for a known record (b[0]==0x00), 0 if unknown. */
static size_t record_expected_len(const uint8_t *b)
{
    if (b[0] != 0x00)
        return 0;
    switch (b[1]) {
    case 0x02: return 14;
    case 0x11: return 12;
    case 0x21: return  4;
    case 0x20: return  2;
    default:   return  0;
    }
}

/* Copy record payload into the stable internal buffer and emit. */
static void emit_record(br5_parser_t *p, uint64_t pos, const uint8_t *payload,
                        size_t len)
{
    size_t n = len < BR5_MAX_RECORD_LEN ? len : BR5_MAX_RECORD_LEN;
    if (payload != p->rec_buf)
        memcpy(p->rec_buf, payload, n);
    emit_ev(p, pos, BR5_EV_RECORD, rec_id_of(p->rec_buf), p->rec_buf, n,
            NULL, 0, NULL);
}

/* Phase transition + page/session-end events for a received record.
 * pos is the stream offset where the record began. */
static void handle_record(br5_parser_t *p, uint64_t pos, const uint8_t *rec,
                          size_t len)
{
    uint8_t id = rec[1];
    uint16_t rid = rec_id_of(rec);
    emit_record(p, pos, rec, len);
    switch (p->state) {
    case BR5_ST_ARMED:
        if (id == 0x02) {
            p->state = BR5_ST_DATA;                 /* JPEG follows */
        } else if (id == 0x01 && len >= 8) {
            /* T8c: B/W RLENGTH first block record — page payload is a
             * sequence of packbits-compressed lines, no JPEG/EOI. The
             * block payload (dlen bytes at offset 14) is owed. */
            p->rle_mode = 1;
            p->state = BR5_ST_DATA;
            p->rle_pend_len = (size_t)rec[6] | ((size_t)rec[7] << 8);
        } else if (id == 0x21) {
            p->state = BR5_ST_SESSION_DONE;
            emit_ev(p, pos, BR5_EV_PAGE_END, rid, p->rec_buf, len, NULL, 0, NULL);
        } else if (id == 0x20) {
            p->state = BR5_ST_IDLE;
            emit_ev(p, pos, BR5_EV_SESSION_END, rid, p->rec_buf, len, NULL, 0, NULL);
        }
        break;
    case BR5_ST_DATA:
        /* interleaved progress record at a URB boundary: stay in DATA.
         * In RLENGTH mode the page also ends without an EOI marker. */
        if (p->rle_mode && id == 0x21) {
            p->state = BR5_ST_PAGE_DONE;
            p->jpeg_eoi = 1;      /* page payload complete (no ff d9) */
            emit_ev(p, pos, BR5_EV_PAGE_END, rid, p->rec_buf, len, NULL, 0, NULL);
        } else if (p->rle_mode && id == 0x20) {
            p->state = BR5_ST_IDLE;
            p->jpeg_eoi = 1;
            emit_ev(p, pos, BR5_EV_SESSION_END, rid, p->rec_buf, len, NULL, 0, NULL);
        }
        break;
    case BR5_ST_PAGE_DONE:
        if (id == 0x21) {
            p->state = BR5_ST_SESSION_DONE;
            emit_ev(p, pos, BR5_EV_PAGE_END, rid, p->rec_buf, len, NULL, 0, NULL);
        } else if (id == 0x20) {
            p->state = BR5_ST_IDLE;
            emit_ev(p, pos, BR5_EV_SESSION_END, rid, p->rec_buf, len, NULL, 0, NULL);
        }
        break;
    case BR5_ST_SESSION_DONE:
        if (id == 0x20) {
            p->state = BR5_ST_IDLE;
            emit_ev(p, pos, BR5_EV_SESSION_END, rid, p->rec_buf, len, NULL, 0, NULL);
        } else if (id == 0x02) {
            p->state = BR5_ST_ARMED;                /* next page */
        }
        break;
    default:
        break;
    }
}

static int ensure_capacity(br5_parser_t *p, size_t need)
{
    if (need <= p->jpeg_cap)
        return 0;
    if (need > BR5_JPEG_CAP_MAX) {
        snprintf(p->msg, sizeof(p->msg),
                 "JPEG buffer overflow: need %zu B > cap %u B",
                 need, (unsigned)BR5_JPEG_CAP_MAX);
        emit_ev(p, p->stream_pos, BR5_EV_ERROR, 0, NULL, 0, NULL, 0, p->msg);
        p->state = BR5_ST_ERROR;
        return -1;
    }
    size_t nc = p->jpeg_cap ? p->jpeg_cap : BR5_JPEG_CAP_INIT;
    while (nc < need)
        nc *= 2;
    if (nc > BR5_JPEG_CAP_MAX)
        nc = BR5_JPEG_CAP_MAX;
    uint8_t *nb = (uint8_t *)realloc(p->jpeg_buf, nc);
    if (!nb) {
        snprintf(p->msg, sizeof(p->msg), "out of memory (JPEG buffer %zu B)", nc);
        emit_ev(p, p->stream_pos, BR5_EV_ERROR, 0, NULL, 0, NULL, 0, p->msg);
        p->state = BR5_ST_ERROR;
        return -1;
    }
    p->jpeg_buf = nb;
    p->jpeg_cap = nc;
    return 0;
}

static void count_trailing(br5_parser_t *p, size_t n)
{
    if (n == 0)
        return;
    p->trailing += n;
    if (!p->trailing_warned && p->trailing > BR5_TRAILING_WARN) {
        p->trailing_warned = 1;
        snprintf(p->msg, sizeof(p->msg),
                 "trailing data after EOI: %llu bytes (warn threshold %u)",
                 (unsigned long long)p->trailing, (unsigned)BR5_TRAILING_WARN);
        emit_ev(p, p->stream_pos, BR5_EV_WARN, 0, NULL, 0, NULL, 0, p->msg);
    }
}

/* ------------------------------------------------------------------ */
/* byte processors                                                     */
/* ------------------------------------------------------------------ */

/* T8e: strict record signature, used where a record rides INSIDE a data
 * chunk. The USB short-packet rule makes a status record terminate the
 * pending bulk-in URB, so on hardware a record can arrive as the TAIL of
 * a data read (or a burst of records coalesce after the EOI) instead of
 * as its own read() — the reference capture's "own URB" framing is only
 * one scheduling outcome. Signature: 00 <id> 01 00 (+ 00 at offset 5 for
 * the 14-B progress record, whose offset 4 carries the scan mode); the
 * strict prefix keeps false positives inside JPEG entropy data
 * negligible. Returns the record length or 0. */
static size_t strict_record_len(const uint8_t *b, size_t avail)
{
    if (avail >= 14 && b[0] == 0x00 && b[1] == 0x02 &&
        b[2] == 0x01 && b[3] == 0x00 && b[5] == 0x00)
        return 14;
    if (avail >= 12 && b[0] == 0x00 && b[1] == 0x11 &&
        b[2] == 0x01 && b[3] == 0x00)
        return 12;
    if (avail >= 4 && b[0] == 0x00 && b[1] == 0x21 &&
        b[2] == 0x01 && b[3] == 0x00)
        return 4;
    return 0;
}

/* Consume trailing bytes after the JPEG EOI: coalesced record bursts are
 * processed as records, everything else counts as trailing padding. */
static void consume_trailing(br5_parser_t *p, const uint8_t *chunk,
                             size_t i, size_t len)
{
    while (i < len) {
        size_t avail = len - i;
        size_t elen = strict_record_len(chunk + i, avail);
        if (elen == 0 && avail >= 2 &&
            chunk[i] == 0x00 && chunk[i + 1] == 0x20)
            elen = 2;                       /* bare 00 20 session end */
        if (elen == 0) {
            count_trailing(p, avail);
            p->stream_pos += avail;
            return;
        }
        p->stream_pos += elen;
        handle_record(p, p->stream_pos - elen, chunk + i, elen);
        i += elen;
    }
}

/* ST_DATA: append JPEG bytes, scan for EOI (ff d9) with 1-byte overlap.
 * T8e: a status record that terminated this chunk's URB rides at the
 * chunk tail and must NOT be appended to the JPEG payload. */
static size_t data_bytes(br5_parser_t *p, const uint8_t *chunk, size_t i,
                         size_t len)
{
    size_t appended_start = p->jpeg_len;
    size_t datalen = len;
    size_t rec_len = 0;

    if (!p->rle_mode && p->state == BR5_ST_DATA && len >= 4) {
        size_t rp = 0;
        if (len >= 14 && (rp = strict_record_len(chunk + len - 14, 14)) != 0)
            rec_len = rp;
        else if (len >= 12 &&
                 (rp = strict_record_len(chunk + len - 12, 12)) != 0)
            rec_len = rp;
        else if ((rp = strict_record_len(chunk + len - 4, 4)) != 0)
            rec_len = rp;
        if (rec_len)
            datalen = len - rec_len;
    }

    for (; i < datalen; i++) {
        uint8_t b = chunk[i];
        if (p->pending_ff && b == 0xd9) {
            /* EOI: append the d9 (the ff was already appended), finish */
            if (ensure_capacity(p, p->jpeg_len + 1) != 0)
                return len;
            p->jpeg_buf[p->jpeg_len++] = 0xd9;
            p->stream_pos++;
            p->pending_ff = 0;
            if (p->jpeg_len > appended_start)
                emit_ev(p, p->stream_pos, BR5_EV_JPEG_DATA, 0, NULL, 0,
                        p->jpeg_buf + appended_start,
                        p->jpeg_len - appended_start, NULL);
            p->jpeg_eoi = 1;
            emit_ev(p, p->stream_pos, BR5_EV_JPEG_EOI, 0, NULL, 0, NULL, 0, NULL);
            p->state = BR5_ST_PAGE_DONE;
            p->trailing = 0;
            if (i + 1 < len)
                consume_trailing(p, chunk, i + 1, len);
            return len;
        }
        if (ensure_capacity(p, p->jpeg_len + 1) != 0)
            return len;
        p->jpeg_buf[p->jpeg_len++] = b;
        p->stream_pos++;
        p->pending_ff = (b == 0xff);
    }
    if (p->jpeg_len > appended_start)
        emit_ev(p, p->stream_pos, BR5_EV_JPEG_DATA, 0, NULL, 0,
                p->jpeg_buf + appended_start,
                p->jpeg_len - appended_start, NULL);
    if (rec_len) {
        p->stream_pos += rec_len;
        handle_record(p, p->stream_pos - rec_len, chunk + datalen, rec_len);
    }
    return len;
}

/* Non-DATA phases: finish an accumulated record or count trailing bytes. */
static size_t non_data_bytes(br5_parser_t *p, const uint8_t *chunk, size_t i,
                             size_t len)
{
    if (p->rec_accum_len > 0) {
        size_t target = p->rec_expected_len;
        if (target == 0) {
            /* need the 2nd byte to identify the record */
            size_t need = 2 - p->rec_accum_len;
            size_t take = (len - i) < need ? (len - i) : need;
            memcpy(p->rec_buf + p->rec_accum_len, chunk + i, take);
            p->rec_accum_len += take;
            p->stream_pos += take;
            i += take;
            if (p->rec_accum_len >= 2) {
                size_t elen;
                if (p->rec_buf[0] == 0x00 && p->rec_buf[1] == 0x01) {
                    /* T8c RLENGTH block: dlen lives at offset 6..7. The
                     * accumulator only buffers the 14-B header (rec_buf
                     * is small); the payload is tracked as rle_pend_len
                     * and streamed by rle_bytes(). */
                    if (p->rec_accum_len < 8) {
                        size_t need8 = 8 - p->rec_accum_len;
                        take = (len - i) < need8 ? (len - i) : need8;
                        memcpy(p->rec_buf + p->rec_accum_len, chunk + i,
                               take);
                        p->rec_accum_len += take;
                        p->stream_pos += take;
                        i += take;
                        if (p->rec_accum_len < 8)
                            return i;
                    }
                    elen = 14;
                } else {
                    elen = record_expected_len(p->rec_buf);
                }
                if (elen > 0) {
                    p->rec_expected_len = elen;
                    /* a 2-byte record (00 20) identified exactly completes
                     * immediately */
                    if (p->rec_accum_len >= elen) {
                        size_t rlen = p->rec_accum_len;
                        uint64_t rpos = p->rec_start_pos;
                        p->rec_accum_len = 0;
                        p->rec_expected_len = 0;
                        handle_record(p, rpos, p->rec_buf, rlen);
                    }
                } else {
                    /* unknown record id: log + skip, state stays defined */
                    uint64_t pos = p->rec_start_pos;
                    snprintf(p->msg, sizeof(p->msg),
                             "unknown record 0x%04x len %zu, skipped",
                             (unsigned)rec_id_of(p->rec_buf),
                             p->rec_accum_len);
                    emit_ev(p, p->stream_pos, BR5_EV_WARN, 0, NULL, 0, NULL, 0,
                            p->msg);
                    emit_record(p, pos, p->rec_buf, p->rec_accum_len);
                    p->rec_accum_len = 0;
                }
            }
            return i;
        }
        size_t need = target - p->rec_accum_len;
        size_t take = (len - i) < need ? (len - i) : need;
        memcpy(p->rec_buf + p->rec_accum_len, chunk + i, take);
        p->rec_accum_len += take;
        p->stream_pos += take;
        i += take;
        if (p->rec_accum_len >= target) {
            size_t rlen = p->rec_accum_len;
            uint64_t pos = p->rec_start_pos;
            p->rec_accum_len = 0;
            p->rec_expected_len = 0;
            handle_record(p, pos, p->rec_buf, rlen);
        }
        return i;
    }

    /* no pending candidate */
    if (p->state == BR5_ST_ARMED) {
        p->state = BR5_ST_DATA;   /* hand back to data_bytes */
        return i;
    }
    {
        size_t run = len - i;
        count_trailing(p, run);
        p->stream_pos += run;
        return len;
    }
}

/* ------------------------------------------------------------------ */
/* RLENGTH data phase (T8c, B/W)                                        */
/* ------------------------------------------------------------------ */

/* Page payload of a B/W (COMP=RLENGTH) scan: a sequence of 00 01 data
 * blocks, each = 14-B header (dlen = u16 LE at offset 6, 6 reserved
 * bytes) + dlen bytes of packbits data for ONE scanline. Records ride
 * at URB/block boundaries; progress records (00 02) may interleave as
 * their own URBs. Page ends with 00 21 (no JPEG EOI in this mode). */
static size_t rle_bytes(br5_parser_t *p, const uint8_t *chunk, size_t i,
                        size_t len)
{
    while (i < len) {
        if (p->rle_pend_len > 0) {
            size_t take = (len - i) < p->rle_pend_len
                              ? (len - i) : p->rle_pend_len;
            if (p->rle_line_len + take > p->rle_line_cap) {
                size_t nc = p->rle_line_cap ? p->rle_line_cap : 1024;
                while (nc < p->rle_line_len + take)
                    nc *= 2;
                uint8_t *nb = (uint8_t *)realloc(p->rle_line_buf, nc);
                if (!nb) {
                    snprintf(p->msg, sizeof(p->msg),
                             "RLENGTH OOM growing line buffer");
                    emit_ev(p, p->stream_pos, BR5_EV_ERROR, 0, NULL, 0,
                            NULL, 0, p->msg);
                    p->state = BR5_ST_ERROR;
                    return i;
                }
                p->rle_line_buf = nb;
                p->rle_line_cap = nc;
            }
            memcpy(p->rle_line_buf + p->rle_line_len, chunk + i, take);
            p->rle_line_len += take;
            p->rle_pend_len -= take;
            p->stream_pos += take;
            i += take;
            if (p->rle_pend_len == 0) {
                /* block complete: one event per scanline */
                emit_ev(p, p->stream_pos, BR5_EV_RLE_DATA, 0, NULL, 0,
                        p->rle_line_buf, p->rle_line_len, NULL);
                p->rle_line_len = 0;
                p->rle_lines++;
            }
            continue;
        }
        {
            size_t rem = len - i;
            if (rem >= 8 && chunk[i] == 0x00 && chunk[i + 1] == 0x01 &&
                chunk[i + 2] == 0x01 && chunk[i + 3] == 0x00) {
                size_t dlen = (size_t)chunk[i + 6] |
                              ((size_t)chunk[i + 7] << 8);
                if (rem < 14 + dlen) {
                    snprintf(p->msg, sizeof(p->msg),
                             "RLENGTH block record spans chunk boundary "
                             "(need %zu, have %zu)", 14 + dlen, rem);
                    emit_ev(p, p->stream_pos, BR5_EV_ERROR, 0, NULL, 0,
                            NULL, 0, p->msg);
                    p->state = BR5_ST_ERROR;
                    return i;
                }
                emit_record(p, p->stream_pos, chunk + i, 14);
                p->stream_pos += 14;
                p->rle_pend_len = dlen;
                i += 14;
                continue;
            }
            if (rem >= 4 && chunk[i] == 0x00 && chunk[i + 1] == 0x21) {
                uint64_t pos = p->stream_pos;
                p->stream_pos += 4;
                handle_record(p, pos, chunk + i, 4);
                i += 4;
                continue;   /* 00 20 may follow in the same chunk */
            }
            if (rem >= 2 && chunk[i] == 0x00 && chunk[i + 1] == 0x20) {
                uint64_t pos = p->stream_pos;
                p->stream_pos += 2;
                handle_record(p, pos, chunk + i, 2);
                return i + 2;
            }
            if (rem >= 14 && chunk[i] == 0x00 && chunk[i + 1] == 0x02) {
                uint64_t pos = p->stream_pos;
                p->stream_pos += 14;
                handle_record(p, pos, chunk + i, 14);
                i += 14;
                continue;
            }
            snprintf(p->msg, sizeof(p->msg),
                     "RLENGTH desync at stream offset %llu "
                     "(bytes %02x %02x %02x %02x)",
                     (unsigned long long)p->stream_pos,
                     chunk[i], rem > 1 ? chunk[i + 1] : 0,
                     rem > 2 ? chunk[i + 2] : 0,
                     rem > 3 ? chunk[i + 3] : 0);
            emit_ev(p, p->stream_pos, BR5_EV_ERROR, 0, NULL, 0, NULL, 0,
                    p->msg);
            p->state = BR5_ST_ERROR;
            return i;
        }
    }
    return i;
}

/* Chunk-boundary handling (only when BR5_FLAG_CHUNK_START is set). */
static size_t boundary(br5_parser_t *p, const uint8_t *chunk, size_t len)
{
    if (len == 0)
        return 0;
    if (p->rec_accum_len > 0)
        return 0;   /* mid-record: continuation handled by non_data_bytes */

    if (p->state == BR5_ST_DATA) {
        if (p->rle_mode)
            return 0;   /* RLENGTH: rle_bytes() does its own framing */
        /* A COMPLETE known record (exact URB length) is a record even in
         * ST_DATA (progress records are interleaved mid-JPEG). Everything
         * else is JPEG payload — including any 00 02 pattern in the chunk. */
        if (len >= 2 && chunk[0] == 0x00 &&
            record_expected_len(chunk) == len) {
            uint64_t pos = p->stream_pos;
            p->stream_pos += len;
            handle_record(p, pos, chunk, len);
            return len;
        }
        return 0;
    }

    /* non-DATA phase (ARMED / PAGE_DONE / SESSION_DONE) */
    if (chunk[0] == 0x00 && len >= 8 && chunk[1] == 0x01 &&
        chunk[2] == 0x01 && chunk[3] == 0x00) {
        /* T8c RLENGTH: the first data block opens the page. The whole
         * URB carries the block header + payload + further blocks, so
         * switch to RLE data processing; rle_bytes() re-parses the
         * record itself and emits the events. */
        p->rle_mode = 1;
        p->state = BR5_ST_DATA;
        return 0;
    }
    /* T8e: in ARMED the XSC response record can be coalesced with the
     * first JPEG data bytes of the same read (no short packet between
     * record and data). Consume the record prefix and hand the JPEG
     * remainder to data_bytes. */
    if (p->state == BR5_ST_ARMED && len >= 14 && chunk[0] == 0x00 &&
        chunk[1] == 0x02 && chunk[2] == 0x01 && chunk[3] == 0x00) {
        p->stream_pos += 14;
        handle_record(p, p->stream_pos - 14, chunk, 14);
        return 14;
    }
    if (chunk[0] == 0x00 && len <= BR5_MAX_RECORD_LEN) {
        uint64_t pos = p->stream_pos;
        if (len < 2) {
            /* single 0x00: buffer until the 2nd byte identifies the record */
            memcpy(p->rec_buf, chunk, len);
            p->rec_accum_len = len;
            p->rec_expected_len = 0;
            p->rec_start_pos = pos;
            p->stream_pos += len;
            return len;
        }
        /* T8e: records can burst within one read (e.g. 00 21 + 00 20
         * after the EOI) — consume a sequence of known records; a final
         * partial record is buffered for continuation as before. */
        {
            size_t j = 0;
            while (j < len) {
                size_t elen = record_expected_len(chunk + j);
                if (elen == 0) {
                    if (j == 0)
                        break;      /* unknown first id: legacy path */
                    count_trailing(p, len - j);
                    p->stream_pos += len - j;
                    return len;
                }
                if (j + elen > len) {
                    /* partial known record: buffer until complete */
                    memcpy(p->rec_buf, chunk + j, len - j);
                    p->rec_accum_len = len - j;
                    p->rec_expected_len = elen;
                    p->rec_start_pos = pos + j;
                    p->stream_pos += len - j;
                    return len;
                }
                p->stream_pos += elen;
                handle_record(p, p->stream_pos - elen, chunk + j, elen);
                j += elen;
                if (p->state == BR5_ST_DATA || p->state == BR5_ST_ARMED) {
                    /* the record opened a (next) data phase — the rest of
                     * the chunk is JPEG data, not more records */
                    return j;
                }
                if (p->state == BR5_ST_IDLE)
                    break;      /* session complete: rest is trailing */
            }
            if (j >= len)
                return len;
            if (j > 0) {
                count_trailing(p, len - j);
                p->stream_pos += len - j;
                return len;
            }
            /* unknown first id: legacy single-record path below */
        }
        {
            size_t elen = record_expected_len(chunk);
            if (elen > 0) {
                if (len >= elen) {
                    p->stream_pos += elen;
                    handle_record(p, pos, chunk, elen);
                    return elen;    /* leftover [elen..len) handled by main loop */
                }
                /* partial record: buffer until the full record has arrived */
                memcpy(p->rec_buf, chunk, len);
                p->rec_accum_len = len;
                p->rec_expected_len = elen;
                p->rec_start_pos = pos;
                p->stream_pos += len;
                return len;
            }
        }
        /* unknown record id (0x00 xx, xx not in {02,11,21,20}) */
        snprintf(p->msg, sizeof(p->msg),
                 "unknown record 0x%04x len %zu, skipped",
                 (unsigned)rec_id_of(chunk), len);
        emit_ev(p, p->stream_pos, BR5_EV_WARN, 0, NULL, 0, NULL, 0, p->msg);
        emit_record(p, pos, chunk, len);
        p->stream_pos += len;
        return len;
    }

    /* not a record: in ARMED this is the start of JPEG data, in
     * PAGE_DONE/SESSION_DONE it is trailing padding */
    if (p->state == BR5_ST_ARMED)
        p->state = BR5_ST_DATA;
    return 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                           */
/* ------------------------------------------------------------------ */

size_t br5_parser_size(void)
{
    return sizeof(struct br5_parser);
}

int br5_parser_init(br5_parser_t *p, br5_event_cb cb, void *userdata)
{
    if (!p)
        return -1;
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->ud = userdata;
    p->state = BR5_ST_IDLE;
    return 0;
}

br5_parser_t *br5_parser_new(br5_event_cb cb, void *userdata)
{
    br5_parser_t *p = (br5_parser_t *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->cb = cb;
    p->ud = userdata;
    p->state = BR5_ST_IDLE;
    return p;
}

void br5_parser_free(br5_parser_t *p)
{
    if (!p)
        return;
    free(p->jpeg_buf);
    free(p->rle_line_buf);
    free(p);
}

void br5_parser_reset(br5_parser_t *p)
{
    if (!p)
        return;
    free(p->jpeg_buf);
    p->jpeg_buf = NULL;
    p->jpeg_len = 0;
    p->jpeg_cap = 0;
    p->jpeg_eoi = 0;
    p->pending_ff = 0;
    p->rec_accum_len = 0;
    p->rec_expected_len = 0;
    p->rec_start_pos = 0;
    p->trailing = 0;
    p->trailing_warned = 0;
    p->rle_mode = 0;
    p->rle_pend_len = 0;
    p->rle_lines = 0;
    p->rle_line_len = 0;
    p->stream_pos = 0;
    p->state = BR5_ST_IDLE;
}

void br5_parser_arm(br5_parser_t *p)
{
    if (!p)
        return;
    free(p->jpeg_buf);
    p->jpeg_buf = NULL;
    p->jpeg_len = 0;
    p->jpeg_cap = 0;
    p->jpeg_eoi = 0;
    p->pending_ff = 0;
    p->rec_accum_len = 0;
    p->rec_expected_len = 0;
    p->rec_start_pos = 0;
    p->trailing = 0;
    p->trailing_warned = 0;
    p->rle_mode = 0;
    p->rle_pend_len = 0;
    p->rle_lines = 0;
    p->rle_line_len = 0;
    p->stream_pos = 0;
    p->state = BR5_ST_ARMED;
}

int br5_parser_feed(br5_parser_t *p, const uint8_t *chunk, size_t len,
                    unsigned flags)
{
    if (p == NULL || (chunk == NULL && len > 0))
        return -1;
    if (p->state == BR5_ST_ERROR)
        return -1;
    if (p->state == BR5_ST_IDLE) {
        snprintf(p->msg, sizeof(p->msg),
                 "feed in IDLE: call br5_parser_arm() after XSC-OK");
        emit_ev(p, p->stream_pos, BR5_EV_ERROR, 0, NULL, 0, NULL, 0, p->msg);
        p->state = BR5_ST_ERROR;
        return -1;
    }
    if (len == 0)
        return 0;

    size_t i = 0;
    if (flags & BR5_FLAG_CHUNK_START) {
        i = boundary(p, chunk, len);
        if (p->state == BR5_ST_ERROR)
            return -1;
        if (i >= len)
            return 0;
    }

    while (i < len) {
        size_t old = i;
        br5_state_t st = p->state;
        switch (p->state) {
        case BR5_ST_DATA:
            if (p->rle_mode)
                i = rle_bytes(p, chunk, i, len);
            else
                i = data_bytes(p, chunk, i, len);
            break;
        case BR5_ST_ARMED:
        case BR5_ST_PAGE_DONE:
        case BR5_ST_SESSION_DONE:
            i = non_data_bytes(p, chunk, i, len);
            break;
        case BR5_ST_IDLE:
        case BR5_ST_ERROR:
        default:
            return 0;   /* session finished / fatal: ignore rest of chunk */
        }
        if (p->state == BR5_ST_ERROR)
            return -1;
        if (i == len)
            break;
        if (i == old && p->state == st)
            break;      /* safety: no progress */
    }
    return 0;
}

br5_state_t br5_parser_state(const br5_parser_t *p)
{
    return p ? p->state : BR5_ST_ERROR;
}

const uint8_t *br5_parser_jpeg_data(const br5_parser_t *p)
{
    return p ? p->jpeg_buf : NULL;
}

size_t br5_parser_jpeg_size(const br5_parser_t *p)
{
    return p ? p->jpeg_len : 0;
}

uint64_t br5_parser_stream_pos(const br5_parser_t *p)
{
    return p ? p->stream_pos : 0;
}

uint64_t br5_parser_trailing(const br5_parser_t *p)
{
    return p ? p->trailing : 0;
}
size_t br5_parser_rle_lines(const br5_parser_t *p)
{
    return p ? p->rle_lines : 0;
}
