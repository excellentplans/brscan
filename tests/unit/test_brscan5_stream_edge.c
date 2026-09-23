/*
 * Copyright 2025 excellentplans. SPDX-License-Identifier: GPL-2.0-or-later
 * test_brscan5_stream_edge.c — synthetic edge-case tests for the brscan5
 * stream parser, ported from the standalone parser suite (frozen in
 * attic/brscan5-parser; the canonical parser source lives in
 * libsane-brother/). All inputs are assembled synthetically — the tests
 * do not depend on capture fixtures; full-page behaviour against real
 * captures is covered by the replay e2e tests at a higher level.
 *
 * Covered cases:
 *   - chunking: stream fed at arbitrary split points (1/7/4096/262144 B
 *     data chunks, both CHUNK_START flag modes) plus a clean stream fed
 *     byte-by-byte and in 7-byte chunks — JPEG stays byte-identical
 *   - EOI (ff d9) split across two chunk boundaries
 *   - JPEG buffer overflow (> BR5_JPEG_CAP_MAX without EOI) -> error
 *   - feed without arm(): error event, parser refuses input
 *   - multipage: second page through the same parser instance
 *   - trailing data after EOI: small => silent, large => warn
 *   - unknown record types: warn + skip, state stays defined
 *   - fuzz: randomized mutations and chunk splits, no crash/hang
 *     (FUZZ_ITERATIONS compile definition, default 5000; overridable
 *     on the command line for CI)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brscan5_stream.h"

#ifndef FUZZ_ITERATIONS
#define FUZZ_ITERATIONS 5000
#endif

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", (msg), __LINE__); \
        failures++; \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* event collector                                                     */
/* ------------------------------------------------------------------ */

#define MAX_RECORDS 64

typedef struct {
    size_t n_records;
    uint16_t rec_ids[MAX_RECORDS];
    size_t rec_lens[MAX_RECORDS];
    uint64_t rec_pos[MAX_RECORDS];
    uint8_t rec_payload[MAX_RECORDS][64];
    size_t n_errors, n_warns, n_eoi, n_page_end, n_session_end;
    uint64_t jpeg_data_bytes;
} collector_t;

static void
collect_cb(const br5_event_t *ev, void *ud)
{
    collector_t *c = (collector_t *)ud;
    switch (ev->type) {
    case BR5_EV_RECORD:
        if (c->n_records < MAX_RECORDS) {
            c->rec_ids[c->n_records] = ev->record_id;
            c->rec_lens[c->n_records] = ev->record_len;
            c->rec_pos[c->n_records] = ev->stream_pos;
            size_t n = ev->record_len < 64 ? ev->record_len : 64;
            if (ev->record_payload)
                memcpy(c->rec_payload[c->n_records], ev->record_payload, n);
            c->n_records++;
        }
        break;
    case BR5_EV_JPEG_DATA:
        c->jpeg_data_bytes += ev->jpeg_len;
        break;
    case BR5_EV_JPEG_EOI:
        c->n_eoi++;
        break;
    case BR5_EV_PAGE_END:
        c->n_page_end++;
        break;
    case BR5_EV_SESSION_END:
        c->n_session_end++;
        break;
    case BR5_EV_WARN:
        c->n_warns++;
        break;
    case BR5_EV_ERROR:
        c->n_errors++;
        break;
    default:
        break;
    }
}

static void
reset_collector(collector_t *c)
{
    memset(c, 0, sizeof(*c));
}

/* ------------------------------------------------------------------ */
/* synthetic JPEG + framing constants                                  */
/* ------------------------------------------------------------------ */

/* A synthetic "JPEG": SOI + APP0-ish marker + payload bytes + EOI. The
 * parser only collects bytes and looks for ff d9 — the payload content
 * is irrelevant as long as it contains no marker sequence. */
static size_t jpeg_len;
static uint8_t *jpeg;

static void
build_jpeg(size_t len)
{
    size_t i;
    jpeg_len = len;
    jpeg = (uint8_t *)malloc(len);
    CHECK(jpeg != NULL, "malloc jpeg");
    jpeg[0] = 0xff; jpeg[1] = 0xd8; jpeg[2] = 0xff; jpeg[3] = 0xe0;
    jpeg[4] = 0x00; jpeg[5] = 0x10;
    for (i = 6; i < len - 2; i++) {
        uint8_t b = (uint8_t)((i * 37 + 11) & 0xff);
        jpeg[i] = (b == 0xff) ? 0xfe : b;   /* keep the payload EOI-free */
    }
    jpeg[len - 2] = 0xff;
    jpeg[len - 1] = 0xd9;
}

static const uint8_t REC1[14] = {
    0x00, 0x02, 0x01, 0x00, 0x15, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t REC3[14] = {
    0x00, 0x02, 0x01, 0x00, 0x15, 0x00, 0x13, 0x61,
    0x07, 0x00, 0x9d, 0x0d, 0x00, 0x00
};
static const uint8_t ENDSTAT[12] = {
    0x00, 0x11, 0x01, 0x00, 0x8e, 0x09, 0x00, 0x00,
    0x9d, 0x0d, 0x00, 0x00
};
static const uint8_t PAGEEND[4] = { 0x00, 0x21, 0x01, 0x00 };
static const uint8_t SCANEND[2] = { 0x00, 0x20 };

static int
jpeg_intact(br5_parser_t *p)
{
    const uint8_t *got = br5_parser_jpeg_data(p);
    size_t got_len = br5_parser_jpeg_size(p);
    if (got_len != jpeg_len)
        return 0;
    return got && memcmp(got, jpeg, jpeg_len) == 0;
}

/* ------------------------------------------------------------------ */
/* stream builders + feeding helpers                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t off;
    size_t len;
    int is_record;
} urb_t;

/* Clean stream: REC1 + JPEG + ENDSTAT + PAGEEND + SCANEND. */
static uint8_t *
build_clean(size_t *out_len)
{
    size_t len = 14 + jpeg_len + 12 + 4 + 2;
    uint8_t *s = (uint8_t *)malloc(len);
    CHECK(s != NULL, "malloc clean stream");
    memcpy(s, REC1, 14);
    memcpy(s + 14, jpeg, jpeg_len);
    memcpy(s + 14 + jpeg_len, ENDSTAT, 12);
    memcpy(s + 14 + jpeg_len + 12, PAGEEND, 4);
    memcpy(s + 14 + jpeg_len + 16, SCANEND, 2);
    *out_len = len;
    return s;
}

/* Build the clean-stream URB table (records as own URBs, the JPEG split
 * into two data URBs with the interleaved progress record between). */
static size_t
build_interleaved(uint8_t **out_stream, urb_t *urbs, size_t data_split)
{
    size_t len = 14 + jpeg_len + 14 + 12 + 4 + 2;
    uint8_t *s = (uint8_t *)malloc(len);
    size_t off = 0;
    size_t n_urbs = 0;
    CHECK(s != NULL, "malloc interleaved stream");
    memcpy(s, REC1, 14);
    urbs[n_urbs].off = off; urbs[n_urbs].len = 14;
    urbs[n_urbs].is_record = 1; n_urbs++; off += 14;
    memcpy(s + off, jpeg, data_split);
    urbs[n_urbs].off = off; urbs[n_urbs].len = data_split;
    urbs[n_urbs].is_record = 0; n_urbs++; off += data_split;
    memcpy(s + off, REC1, 14);
    urbs[n_urbs].off = off; urbs[n_urbs].len = 14;
    urbs[n_urbs].is_record = 1; n_urbs++; off += 14;
    memcpy(s + off, jpeg + data_split, jpeg_len - data_split);
    urbs[n_urbs].off = off; urbs[n_urbs].len = jpeg_len - data_split;
    urbs[n_urbs].is_record = 0; n_urbs++; off += jpeg_len - data_split;
    memcpy(s + off, ENDSTAT, 12);
    urbs[n_urbs].off = off; urbs[n_urbs].len = 12;
    urbs[n_urbs].is_record = 1; n_urbs++; off += 12;
    memcpy(s + off, PAGEEND, 4);
    urbs[n_urbs].off = off; urbs[n_urbs].len = 4;
    urbs[n_urbs].is_record = 1; n_urbs++; off += 4;
    memcpy(s + off, SCANEND, 2);
    urbs[n_urbs].off = off; urbs[n_urbs].len = 2;
    urbs[n_urbs].is_record = 1; n_urbs++; off += 2;
    *out_stream = s;
    return n_urbs;
}

/* Feed the URB table; data URBs are split into `split`-byte pieces
 * (split==0: whole URBs). flag_all: mark every piece as chunk start.
 * Records are kept whole: a trailing byte delivered as its own 1-byte
 * URB would be a plausible record prefix, which is not what the device
 * does — data arrives in large URBs, records as short-packet URBs. */
static void
feed_urbs(br5_parser_t *p, const uint8_t *stream, const urb_t *urbs,
          size_t n_urbs, size_t split, int flag_all)
{
    size_t u;
    for (u = 0; u < n_urbs; u++) {
        size_t off = urbs[u].off, len = urbs[u].len;
        if (!urbs[u].is_record && split > 0 && len > split) {
            size_t pos = off, end = off + len;
            int first = 1;
            while (pos < end) {
                size_t n = (end - pos) < split ? (end - pos) : split;
                unsigned flags = 0;
                if (first || flag_all)
                    flags |= BR5_FLAG_CHUNK_START;
                CHECK(br5_parser_feed(p, stream + pos, n, flags) == 0,
                      "feed data piece");
                pos += n;
                first = 0;
            }
        } else {
            CHECK(br5_parser_feed(p, stream + off, len,
                                  BR5_FLAG_CHUNK_START) == 0,
                  "feed urb");
        }
    }
}

/* Assert the standard end-of-session invariants on a parsed clean-ish
 * page: no errors, one EOI / page end / session end, JPEG intact. */
static void
check_page_done(const collector_t *c, br5_parser_t *p)
{
    CHECK(c->n_errors == 0, "no parser errors");
    CHECK(c->n_eoi == 1, "one EOI");
    CHECK(c->n_page_end == 1, "one page end");
    CHECK(c->n_session_end == 1, "one session end");
    CHECK(br5_parser_state(p) == BR5_ST_IDLE, "state back to IDLE");
    CHECK(jpeg_intact(p), "JPEG byte-identical");
}

/* ------------------------------------------------------------------ */
/* chunking: no URB-size assumption                                    */
/* ------------------------------------------------------------------ */

static void
test_chunking(void)
{
    static const size_t splits[4] = { 1, 7, 4096, 262144 };
    size_t clen = 0;
    uint8_t *clean = build_clean(&clen);
    uint8_t *istream = NULL;
    urb_t urbs[7];
    size_t n_urbs = build_interleaved(&istream, urbs, 8192);
    int fm;
    size_t si;

    for (fm = 0; fm < 2; fm++) {
        for (si = 0; si < 4; si++) {
            collector_t c;
            br5_parser_t *p;
            reset_collector(&c);
            p = br5_parser_new(collect_cb, &c);
            CHECK(p != NULL, "parser alloc");
            br5_parser_arm(p);
            feed_urbs(p, istream, urbs, n_urbs, splits[si], fm);
            check_page_done(&c, p);
            CHECK(c.n_records == 5, "five records (2x progress + 3 end)");
            br5_parser_free(p);
        }
    }

    /* clean stream fed byte-by-byte: every byte is a chunk boundary, so
     * records split across chunks are reassembled -> full assertions */
    {
        collector_t c;
        br5_parser_t *p;
        size_t pos;
        reset_collector(&c);
        p = br5_parser_new(collect_cb, &c);
        CHECK(p != NULL, "parser alloc");
        br5_parser_arm(p);
        for (pos = 0; pos < clen; pos++)
            CHECK(br5_parser_feed(p, clean + pos, 1,
                                  BR5_FLAG_CHUNK_START) == 0,
                  "feed byte");
        check_page_done(&c, p);
        CHECK(c.n_records == 4, "four records reassembled");
        CHECK(c.rec_ids[0] == BR5_RECORD_PROGRESS && c.rec_lens[0] == 14,
              "first record is the 14-B progress record");
        CHECK(c.rec_pos[0] == 0, "progress record at stream offset 0");
        CHECK(c.rec_pos[1] == 14 + jpeg_len, "endstat position");
        CHECK(c.rec_pos[2] == 14 + jpeg_len + 12, "pageend position");
        CHECK(c.rec_pos[3] == 14 + jpeg_len + 16, "scanend position");
        br5_parser_free(p);
    }

    /* clean stream fed in 7-byte chunks: JPEG must stay byte-identical
     * and the EOI detected. End records may or may not sit on a chunk
     * boundary, so the record sequence is not asserted here. */
    {
        collector_t c;
        br5_parser_t *p;
        size_t pos;
        reset_collector(&c);
        p = br5_parser_new(collect_cb, &c);
        CHECK(p != NULL, "parser alloc");
        br5_parser_arm(p);
        for (pos = 0; pos < clen; pos += 7) {
            size_t n = (clen - pos) < 7 ? (clen - pos) : 7;
            CHECK(br5_parser_feed(p, clean + pos, n,
                                  BR5_FLAG_CHUNK_START) == 0,
                  "feed 7-byte chunk");
        }
        CHECK(c.n_errors == 0, "no parser errors (7-byte chunks)");
        CHECK(c.n_eoi == 1, "EOI seen (7-byte chunks)");
        CHECK(jpeg_intact(p), "JPEG byte-identical (7-byte chunks)");
        br5_parser_free(p);
    }

    free(clean);
    free(istream);
}

/* ------------------------------------------------------------------ */
/* EOI split across two chunks                                         */
/* ------------------------------------------------------------------ */

static void
test_eoi_split(void)
{
    size_t clen = 0;
    uint8_t *clean = build_clean(&clen);
    const size_t ff_at = 14 + jpeg_len - 2;   /* the ff of EOI */
    collector_t c;
    br5_parser_t *p;

    CHECK(clean[ff_at] == 0xff, "EOI ff byte where expected");
    CHECK(clean[ff_at + 1] == 0xd9, "EOI d9 byte where expected");

    reset_collector(&c);
    p = br5_parser_new(collect_cb, &c);
    CHECK(p != NULL, "parser alloc");
    br5_parser_arm(p);
    CHECK(br5_parser_feed(p, clean, 14, BR5_FLAG_CHUNK_START) == 0,
          "feed progress record");
    /* chunk ending with the ff of EOI */
    CHECK(br5_parser_feed(p, clean + 14, ff_at - 14 + 1,
                          BR5_FLAG_CHUNK_START) == 0,
          "feed data up to EOI ff");
    /* next chunk begins with d9 */
    CHECK(br5_parser_feed(p, clean + ff_at + 1, 1,
                          BR5_FLAG_CHUNK_START) == 0,
          "feed EOI d9");
    /* remaining records as their own URBs */
    CHECK(br5_parser_feed(p, clean + 14 + jpeg_len, 12,
                          BR5_FLAG_CHUNK_START) == 0, "feed endstat");
    CHECK(br5_parser_feed(p, clean + 14 + jpeg_len + 12, 4,
                          BR5_FLAG_CHUNK_START) == 0, "feed pageend");
    CHECK(br5_parser_feed(p, clean + 14 + jpeg_len + 16, 2,
                          BR5_FLAG_CHUNK_START) == 0, "feed scanend");
    check_page_done(&c, p);
    CHECK(c.n_records == 4, "all four records recognized");
    br5_parser_free(p);
    free(clean);
}

/* ------------------------------------------------------------------ */
/* unknown record types: warn + skip, state stays defined              */
/* ------------------------------------------------------------------ */

static void
test_unknown_records(void)
{
    size_t clen = 0;
    uint8_t *clean = build_clean(&clen);
    static const uint8_t unk[8] = {
        0x00, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99
    };

    /* unknown before the scan data (ARMED) */
    {
        collector_t c;
        br5_parser_t *p;
        int i;
        static const uint16_t ids[4] = {
            BR5_RECORD_PROGRESS, BR5_RECORD_ENDSTAT,
            BR5_RECORD_PAGE_END, BR5_RECORD_SCAN_END
        };
        reset_collector(&c);
        p = br5_parser_new(collect_cb, &c);
        CHECK(p != NULL, "parser alloc");
        br5_parser_arm(p);
        CHECK(br5_parser_feed(p, unk, sizeof(unk),
                              BR5_FLAG_CHUNK_START) == 0,
              "feed unknown record (ARMED)");
        CHECK(c.n_warns == 1, "one warn for the unknown record");
        CHECK(c.n_records == 1, "unknown record emitted as record event");
        CHECK(c.rec_ids[0] == 0x0033 && c.rec_lens[0] == 8,
              "unknown record id/len");
        CHECK(br5_parser_state(p) == BR5_ST_ARMED, "state stays ARMED");
        /* a normal scan must still parse fine afterwards */
        CHECK(br5_parser_feed(p, clean, 14, BR5_FLAG_CHUNK_START) == 0,
              "feed progress record");
        CHECK(br5_parser_feed(p, clean + 14, jpeg_len,
                              BR5_FLAG_CHUNK_START) == 0, "feed jpeg");
        CHECK(br5_parser_feed(p, clean + 14 + jpeg_len, 12,
                              BR5_FLAG_CHUNK_START) == 0, "feed endstat");
        CHECK(br5_parser_feed(p, clean + 14 + jpeg_len + 12, 4,
                              BR5_FLAG_CHUNK_START) == 0, "feed pageend");
        CHECK(br5_parser_feed(p, clean + 14 + jpeg_len + 16, 2,
                              BR5_FLAG_CHUNK_START) == 0, "feed scanend");
        CHECK(c.n_errors == 0, "no parser errors after unknown record");
        CHECK(c.n_session_end == 1, "session end seen");
        CHECK(jpeg_intact(p), "JPEG byte-identical after unknown record");
        /* unknown record first, then the clean sequence shifted by 8 B */
        CHECK(c.n_records == 5, "unknown + four clean-stream records");
        CHECK(c.rec_pos[0] == 0, "unknown record at offset 0");
        for (i = 0; i < 4; i++)
            CHECK(c.rec_ids[i + 1] == ids[i], "clean record id order");
        CHECK(c.rec_pos[1] == 8, "progress record shifted by 8");
        CHECK(c.rec_pos[2] == 8 + 14 + jpeg_len, "endstat shifted by 8");
        CHECK(c.rec_pos[3] == 8 + 14 + jpeg_len + 12, "pageend shifted by 8");
        CHECK(c.rec_pos[4] == 8 + 14 + jpeg_len + 16, "scanend shifted by 8");
        br5_parser_free(p);
    }
    /* unknown record after EOI (PAGE_DONE) */
    {
        collector_t c;
        br5_parser_t *p;
        size_t endstat_off = 14 + jpeg_len;
        reset_collector(&c);
        p = br5_parser_new(collect_cb, &c);
        CHECK(p != NULL, "parser alloc");
        br5_parser_arm(p);
        CHECK(br5_parser_feed(p, clean, 14, BR5_FLAG_CHUNK_START) == 0,
              "feed progress record");
        CHECK(br5_parser_feed(p, clean + 14, jpeg_len,
                              BR5_FLAG_CHUNK_START) == 0, "feed jpeg");
        CHECK(br5_parser_feed(p, unk, sizeof(unk),
                              BR5_FLAG_CHUNK_START) == 0,
              "feed unknown record (PAGE_DONE)");
        CHECK(c.n_warns == 1, "one warn for the unknown record");
        CHECK(br5_parser_state(p) == BR5_ST_PAGE_DONE,
              "state stays PAGE_DONE");
        CHECK(br5_parser_feed(p, clean + endstat_off, 12,
                              BR5_FLAG_CHUNK_START) == 0, "feed endstat");
        CHECK(br5_parser_feed(p, clean + endstat_off + 12, 4,
                              BR5_FLAG_CHUNK_START) == 0, "feed pageend");
        CHECK(br5_parser_feed(p, clean + endstat_off + 16, 2,
                              BR5_FLAG_CHUNK_START) == 0, "feed scanend");
        CHECK(c.n_errors == 0, "no parser errors (PAGE_DONE unknown)");
        CHECK(c.n_session_end == 1, "session end seen");
        CHECK(jpeg_intact(p), "JPEG byte-identical");
        br5_parser_free(p);
    }
    free(clean);
}

/* ------------------------------------------------------------------ */
/* trailing data after EOI                                             */
/* ------------------------------------------------------------------ */

static void
test_trailing(void)
{
    const size_t trail = 5000;
    uint8_t *s = (uint8_t *)malloc(jpeg_len + trail);
    CHECK(s != NULL, "malloc trailing buffer");
    memcpy(s, jpeg, jpeg_len);

    /* small trailing: 100 B -> no warn */
    {
        collector_t c;
        br5_parser_t *p;
        reset_collector(&c);
        p = br5_parser_new(collect_cb, &c);
        CHECK(p != NULL, "parser alloc");
        br5_parser_arm(p);
        CHECK(br5_parser_feed(p, REC1, 14, BR5_FLAG_CHUNK_START) == 0,
              "feed progress record");
        CHECK(br5_parser_feed(p, s, jpeg_len, BR5_FLAG_CHUNK_START) == 0,
              "feed jpeg");
        memset(s + jpeg_len, 0xAA, 100);
        CHECK(br5_parser_feed(p, s + jpeg_len, 100,
                              BR5_FLAG_CHUNK_START) == 0,
              "feed 100 B trailing");
        CHECK(br5_parser_feed(p, ENDSTAT, 12, BR5_FLAG_CHUNK_START) == 0,
              "feed endstat");
        CHECK(br5_parser_feed(p, PAGEEND, 4, BR5_FLAG_CHUNK_START) == 0,
              "feed pageend");
        CHECK(br5_parser_feed(p, SCANEND, 2, BR5_FLAG_CHUNK_START) == 0,
              "feed scanend");
        CHECK(c.n_errors == 0, "no parser errors (small trailing)");
        CHECK(c.n_warns == 0, "no warn for small trailing");
        CHECK(br5_parser_trailing(p) == 100, "trailing counter = 100");
        CHECK(c.n_session_end == 1, "session end seen");
        br5_parser_free(p);
    }
    /* large trailing: 5000 B -> warn event, no crash */
    {
        collector_t c;
        br5_parser_t *p;
        reset_collector(&c);
        p = br5_parser_new(collect_cb, &c);
        CHECK(p != NULL, "parser alloc");
        br5_parser_arm(p);
        CHECK(br5_parser_feed(p, REC1, 14, BR5_FLAG_CHUNK_START) == 0,
              "feed progress record");
        CHECK(br5_parser_feed(p, s, jpeg_len, BR5_FLAG_CHUNK_START) == 0,
              "feed jpeg");
        memset(s + jpeg_len, 0x55, trail);
        CHECK(br5_parser_feed(p, s + jpeg_len, trail,
                              BR5_FLAG_CHUNK_START) == 0,
              "feed 5000 B trailing");
        CHECK(br5_parser_feed(p, ENDSTAT, 12, BR5_FLAG_CHUNK_START) == 0,
              "feed endstat");
        CHECK(br5_parser_feed(p, PAGEEND, 4, BR5_FLAG_CHUNK_START) == 0,
              "feed pageend");
        CHECK(br5_parser_feed(p, SCANEND, 2, BR5_FLAG_CHUNK_START) == 0,
              "feed scanend");
        CHECK(c.n_errors == 0, "no parser errors (large trailing)");
        CHECK(c.n_warns >= 1, "warn for trailing beyond threshold");
        CHECK(br5_parser_trailing(p) == trail, "trailing counter = 5000");
        CHECK(c.n_session_end == 1, "session end seen");
        CHECK(br5_parser_state(p) == BR5_ST_IDLE, "state back to IDLE");
        br5_parser_free(p);
    }
    free(s);
}

/* ------------------------------------------------------------------ */
/* JPEG buffer overflow                                                */
/* ------------------------------------------------------------------ */

static void
test_overflow(void)
{
    const size_t chunk = 1024u * 1024u;
    uint8_t *buf = (uint8_t *)malloc(chunk);
    collector_t c;
    br5_parser_t *p;
    int rc = 0;
    size_t fed;

    CHECK(buf != NULL, "malloc 1 MiB chunk");
    memset(buf, 0x55, chunk);

    reset_collector(&c);
    p = br5_parser_new(collect_cb, &c);
    CHECK(p != NULL, "parser alloc");
    br5_parser_arm(p);
    CHECK(br5_parser_feed(p, REC1, 14, BR5_FLAG_CHUNK_START) == 0,
          "feed progress record");
    for (fed = 0; fed <= BR5_JPEG_CAP_MAX; fed += chunk) {
        size_t n = (BR5_JPEG_CAP_MAX + 1 - fed) < chunk
                       ? (BR5_JPEG_CAP_MAX + 1 - fed) : chunk;
        rc = br5_parser_feed(p, buf, n, BR5_FLAG_CHUNK_START);
        if (rc != 0)
            break;
    }
    CHECK(rc != 0, "feed fails once the JPEG cap is exceeded");
    CHECK(c.n_errors == 1, "one EVENT_ERROR on overflow");
    CHECK(br5_parser_state(p) == BR5_ST_ERROR, "state ERROR after overflow");
    CHECK(br5_parser_jpeg_size(p) == BR5_JPEG_CAP_MAX,
          "collected JPEG capped at BR5_JPEG_CAP_MAX");
    /* parser refuses further input */
    CHECK(br5_parser_feed(p, buf, 1, BR5_FLAG_CHUNK_START) != 0,
          "feed refused after error");
    br5_parser_free(p);
    free(buf);
}

/* ------------------------------------------------------------------ */
/* feed without arm()                                                  */
/* ------------------------------------------------------------------ */

static void
test_idle_feed(void)
{
    collector_t c;
    br5_parser_t *p;
    reset_collector(&c);
    p = br5_parser_new(collect_cb, &c);
    CHECK(p != NULL, "parser alloc");
    CHECK(br5_parser_feed(p, REC1, 14, BR5_FLAG_CHUNK_START) != 0,
          "feed in IDLE state fails");
    CHECK(c.n_errors == 1, "one EVENT_ERROR for idle feed");
    CHECK(br5_parser_state(p) == BR5_ST_ERROR, "state ERROR after idle feed");
    br5_parser_free(p);
}

/* ------------------------------------------------------------------ */
/* multipage: one parser instance, two pages                           */
/* ------------------------------------------------------------------ */

static void
test_multipage(void)
{
    collector_t c;
    br5_parser_t *p;
    reset_collector(&c);
    p = br5_parser_new(collect_cb, &c);
    CHECK(p != NULL, "parser alloc");
    br5_parser_arm(p);
    /* page 1: REC1 + JPEG + ENDSTAT + PAGEEND (no SCANEND) */
    CHECK(br5_parser_feed(p, REC1, 14, BR5_FLAG_CHUNK_START) == 0,
          "feed progress record");
    CHECK(br5_parser_feed(p, jpeg, jpeg_len, BR5_FLAG_CHUNK_START) == 0,
          "feed jpeg page 1");
    CHECK(br5_parser_feed(p, ENDSTAT, 12, BR5_FLAG_CHUNK_START) == 0,
          "feed endstat");
    CHECK(br5_parser_feed(p, PAGEEND, 4, BR5_FLAG_CHUNK_START) == 0,
          "feed pageend");
    CHECK(c.n_page_end == 1, "page 1 end seen");
    CHECK(br5_parser_state(p) == BR5_ST_SESSION_DONE,
          "state SESSION_DONE after page end");
    /* page 2: 00 02 progress record -> back to ARMED, then data */
    CHECK(br5_parser_feed(p, REC1, 14, BR5_FLAG_CHUNK_START) == 0,
          "feed progress record (page 2)");
    CHECK(br5_parser_state(p) == BR5_ST_ARMED, "state ARMED for page 2");
    CHECK(br5_parser_feed(p, jpeg, jpeg_len, BR5_FLAG_CHUNK_START) == 0,
          "feed jpeg page 2");
    CHECK(br5_parser_state(p) == BR5_ST_PAGE_DONE,
          "state PAGE_DONE after page 2 EOI");
    CHECK(c.n_eoi == 2, "two EOIs seen");
    CHECK(br5_parser_feed(p, ENDSTAT, 12, BR5_FLAG_CHUNK_START) == 0,
          "feed endstat (page 2)");
    CHECK(br5_parser_feed(p, PAGEEND, 4, BR5_FLAG_CHUNK_START) == 0,
          "feed pageend (page 2)");
    CHECK(br5_parser_feed(p, SCANEND, 2, BR5_FLAG_CHUNK_START) == 0,
          "feed scanend");
    CHECK(c.n_errors == 0, "no parser errors (multipage)");
    CHECK(c.n_session_end == 1, "one session end for both pages");
    CHECK(c.n_page_end == 2, "two page ends");
    CHECK(br5_parser_state(p) == BR5_ST_IDLE, "state back to IDLE");
    br5_parser_free(p);
}

/* ------------------------------------------------------------------ */
/* fuzz: randomized mutations + chunk splits, no crash/hang            */
/* ------------------------------------------------------------------ */

static uint32_t g_rng = 0x13579bdu;

static uint32_t
rng32(void)
{
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

static size_t
build_fuzz_base(uint8_t **out)
{
    /* REC1 + small JPEG (with EOI) + trailing + progress record + end
     * records */
    const size_t small = 4096;
    size_t len = 14 + small + 2 + 5000 + 14 + 12 + 4 + 2;
    uint8_t *b = (uint8_t *)malloc(len);
    size_t p = 0;
    int i;
    if (!b)
        return 0;
    memcpy(b + p, REC1, 14); p += 14;
    memcpy(b + p, jpeg, small); p += small;
    b[p++] = 0xff; b[p++] = 0xd9;            /* EOI */
    for (i = 0; i < 5000; i++)
        b[p++] = (uint8_t)rng32();
    memcpy(b + p, REC3, 14); p += 14;        /* trailing progress record */
    memcpy(b + p, ENDSTAT, 12); p += 12;
    memcpy(b + p, PAGEEND, 4); p += 4;
    memcpy(b + p, SCANEND, 2); p += 2;
    *out = b;
    return p;
}

static void
test_fuzz(long iterations, uint32_t seed)
{
    uint8_t *base = NULL;
    size_t base_len = build_fuzz_base(&base);
    uint8_t *buf;
    br5_parser_t *p;
    long it;

    CHECK(base != NULL, "malloc fuzz base");
    CHECK(base_len > 0, "fuzz base built");

    g_rng = seed;
    buf = (uint8_t *)malloc(base_len + 512);
    CHECK(buf != NULL, "malloc fuzz buffer");

    p = br5_parser_new(NULL, NULL);
    CHECK(p != NULL, "parser alloc");

    for (it = 0; it < iterations; it++) {
        int ops;
        size_t pos, len;

        memcpy(buf, base, base_len);
        len = base_len;

        ops = (int)(rng32() % 9);
        for (int o = 0; o < ops; o++) {
            size_t mpos = rng32() % (len + 1);
            switch (rng32() % 5) {
            case 0: /* bit flip */
                if (len)
                    buf[mpos % len] ^= (uint8_t)(1u << (rng32() & 7));
                break;
            case 1: /* byte set */
                if (len)
                    buf[mpos % len] = (uint8_t)rng32();
                break;
            case 2: /* fill range */
                if (len) {
                    size_t n = rng32() % 64;
                    for (size_t k = mpos % len; k < len && n; k++, n--)
                        buf[k] = (uint8_t)rng32();
                }
                break;
            case 3: /* delete range */
                if (len) {
                    size_t n = rng32() % 64;
                    if (n > len - (mpos % len))
                        n = len - (mpos % len);
                    memmove(buf + mpos % len, buf + mpos % len + n,
                            len - (mpos % len) - n);
                    len -= n;
                }
                break;
            case 4: /* truncate */
                len = mpos;
                break;
            }
        }

        /* optional: feed without arm() first (IDLE error path) */
        if ((rng32() & 0xFF) == 0x00) {
            br5_parser_feed(p, buf, 1, BR5_FLAG_CHUNK_START);
            br5_parser_reset(p);
        }
        br5_parser_arm(p);

        pos = 0;
        while (pos < len) {
            size_t n = 1 + (rng32() % 8192);
            if (n > len - pos)
                n = len - pos;
            unsigned flags = (rng32() & 1) ? BR5_FLAG_CHUNK_START : 0;
            if (br5_parser_feed(p, buf + pos, n, flags) != 0)
                break;
            pos += n;
            /* random mid-stream restart */
            if ((rng32() & 0x7FFF) == 0x1337) {
                br5_parser_reset(p);
                br5_parser_arm(p);
            }
        }
        /* invariants */
        CHECK(br5_parser_state(p) >= BR5_ST_IDLE &&
              br5_parser_state(p) <= BR5_ST_ERROR,
              "fuzz: state stays within enum range");
        CHECK(br5_parser_jpeg_size(p) <= BR5_JPEG_CAP_MAX,
              "fuzz: JPEG size within cap");
    }

    br5_parser_free(p);
    free(buf);
    free(base);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    long fuzz_iters = FUZZ_ITERATIONS;
    if (argc > 1)
        fuzz_iters = atol(argv[1]);
    if (fuzz_iters < 1000)
        fuzz_iters = 1000;

    build_jpeg(100000);

    test_chunking();
    test_eoi_split();
    test_unknown_records();
    test_trailing();
    test_overflow();
    test_idle_feed();
    test_multipage();
    test_fuzz(fuzz_iters, 0x13579bdu);

    free(jpeg);

    if (failures) {
        printf("test_brscan5_stream_edge: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_brscan5_stream_edge: all in-process checks passed\n");
    return 0;
}
