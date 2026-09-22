/*
 * test_brscan5_stream_records.c — T8e unit tests for status records that
 * ride INSIDE a data chunk instead of arriving as their own read().
 *
 * Hardware background (T8e bug): the USB short-packet rule makes a status
 * record terminate the pending bulk-in URB, so on the DS-640 a record can
 * arrive as the TAIL of a data read (progress record emitted while a
 * 262144-B JPEG data URB is pending) instead of as its own read(). The
 * pre-T8e parser only recognized records at chunk starts with an exact
 * record length, so the record bytes were appended to the collected JPEG
 * — the Huffman decoder desynced at that point and the page showed
 * localized shear/ghost bands ("gestaucht/überlappt") in Color/Gray scans.
 *
 * Covered cases:
 *   - progress record merged at the tail of a data chunk (mid-page)
 *   - XSC response record coalesced with the first JPEG bytes (ARMED)
 *   - end-statistics + page-end + scan-end burst in one chunk
 *   - EOI + record burst inside the same data chunk
 * In every case the collected JPEG must stay byte-identical.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brscan5_stream.h"

static int failures;
static int n_records;
static int n_eoi;
static int n_page_end;
static int n_session_end;
static int n_errors;
static int n_warns;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", (msg), __LINE__); \
        failures++; \
    } \
} while (0)

static void
event_cb(const br5_event_t *ev, void *ud)
{
    (void)ud;
    switch (ev->type) {
    case BR5_EV_RECORD:
        n_records++;
        break;
    case BR5_EV_JPEG_EOI:
        n_eoi++;
        break;
    case BR5_EV_PAGE_END:
        n_page_end++;
        break;
    case BR5_EV_SESSION_END:
        n_session_end++;
        break;
    case BR5_EV_WARN:
        n_warns++;
        break;
    case BR5_EV_ERROR:
        n_errors++;
        if (ev->message)
            printf("  parser error: %s\n", ev->message);
        break;
    default:
        break;
    }
}

/* A tiny synthetic "JPEG": SOI + entropy-ish bytes + EOI. The parser only
 * collects bytes and looks for ff d9 — the payload content is irrelevant
 * as long as it does not contain a marker sequence. */
static size_t jpeg_len;
static uint8_t *jpeg;

static void
build_jpeg(void)
{
    jpeg_len = 4096;
    jpeg = (uint8_t *)malloc(jpeg_len);
    CHECK(jpeg != NULL, "malloc jpeg");
    jpeg[0] = 0xff; jpeg[1] = 0xd8; jpeg[2] = 0xff; jpeg[3] = 0xe0;
    jpeg[4] = 0x00; jpeg[5] = 0x10;
    for (size_t i = 6; i < jpeg_len - 2; i++)
        jpeg[i] = (uint8_t)((i * 37 + 11) & 0xff);   /* no 0xff bytes */
    jpeg[jpeg_len - 2] = 0xff;
    jpeg[jpeg_len - 1] = 0xd9;
}

static const uint8_t REC_PROGRESS[14] = {
    0x00, 0x02, 0x01, 0x00, 0x15, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t REC_ENDSTAT[12] = {
    0x00, 0x11, 0x01, 0x00, 0x8e, 0x09, 0x00, 0x00,
    0x9d, 0x0d, 0x00, 0x00
};
static const uint8_t REC_PAGEEND[4]  = { 0x00, 0x21, 0x01, 0x00 };
static const uint8_t REC_SCANEND[2]  = { 0x00, 0x20 };

static void
reset_counts(void)
{
    n_records = n_eoi = n_page_end = n_session_end = 0;
    n_errors = n_warns = 0;
}

/* Check that the parser's collected JPEG equals the synthetic input. */
static int
jpeg_intact(br5_parser_t *p)
{
    const uint8_t *got = br5_parser_jpeg_data(p);
    size_t got_len = br5_parser_jpeg_size(p);
    if (got_len != jpeg_len)
        return 0;
    return got && memcmp(got, jpeg, jpeg_len) == 0;
}

int
main(void)
{
    build_jpeg();
    size_t hl = 1000;                    /* data bytes per chunk */

    /* 1: progress record merged at the TAIL of a data chunk */
    {
        reset_counts();
        br5_parser_t *p = br5_parser_new(event_cb, NULL);
        CHECK(p != NULL, "parser alloc");
        br5_parser_arm(p);
        /* [REC_PROGRESS own][hl JPEG][hl JPEG + REC_PROGRESS merged] */
        CHECK(br5_parser_feed(p, REC_PROGRESS, 14,
                              BR5_FLAG_CHUNK_START) == 0, "feed rec");
        CHECK(br5_parser_feed(p, jpeg, hl,
                              BR5_FLAG_CHUNK_START) == 0, "feed data 1");
        uint8_t *buf = (uint8_t *)malloc(hl + 14);
        CHECK(buf != NULL, "malloc");
        memcpy(buf, jpeg + hl, hl);
        memcpy(buf + hl, REC_PROGRESS, 14);
        CHECK(br5_parser_feed(p, buf, hl + 14,
                              BR5_FLAG_CHUNK_START) == 0, "feed merged");
        CHECK(br5_parser_feed(p, jpeg + 2 * hl, jpeg_len - 2 * hl,
                              BR5_FLAG_CHUNK_START) == 0, "feed data 2");
        CHECK(n_errors == 0, "no parser errors");
        CHECK(n_records == 2, "two progress records recognized");
        CHECK(n_eoi == 1, "EOI seen");
        CHECK(jpeg_intact(p), "JPEG byte-identical despite merged record");
        free(buf);
        br5_parser_free(p);
    }

    /* 2: XSC response record coalesced with the first JPEG bytes
     * (ARMED state — chunk starts with the record, data follows) */
    {
        reset_counts();
        br5_parser_t *p = br5_parser_new(event_cb, NULL);
        CHECK(p != NULL, "parser alloc");
        br5_parser_arm(p);
        uint8_t *buf = (uint8_t *)malloc(14 + hl);
        CHECK(buf != NULL, "malloc");
        memcpy(buf, REC_PROGRESS, 14);
        memcpy(buf + 14, jpeg, hl);
        CHECK(br5_parser_feed(p, buf, 14 + hl,
                              BR5_FLAG_CHUNK_START) == 0, "feed coalesced");
        CHECK(br5_parser_feed(p, jpeg + hl, jpeg_len - hl,
                              BR5_FLAG_CHUNK_START) == 0, "feed rest");
        CHECK(n_errors == 0, "no parser errors");
        CHECK(n_records == 1, "record recognized once");
        CHECK(n_eoi == 1, "EOI seen");
        CHECK(jpeg_intact(p), "JPEG byte-identical after ARMED coalesce");
        free(buf);
        br5_parser_free(p);
    }

    /* 3: end-statistics + page-end + scan-end burst in one chunk */
    {
        reset_counts();
        br5_parser_t *p = br5_parser_new(event_cb, NULL);
        CHECK(p != NULL, "parser alloc");
        br5_parser_arm(p);
        uint8_t tail[12 + 4 + 2];
        memcpy(tail, REC_ENDSTAT, 12);
        memcpy(tail + 12, REC_PAGEEND, 4);
        memcpy(tail + 16, REC_SCANEND, 2);
        CHECK(br5_parser_feed(p, REC_PROGRESS, 14,
                              BR5_FLAG_CHUNK_START) == 0, "feed rec");
        CHECK(br5_parser_feed(p, jpeg, jpeg_len,
                              BR5_FLAG_CHUNK_START) == 0, "feed jpeg");
        CHECK(br5_parser_feed(p, tail, sizeof(tail),
                              BR5_FLAG_CHUNK_START) == 0, "feed burst");
        CHECK(n_errors == 0, "no parser errors");
        CHECK(n_records == 4, "all four records recognized");
        CHECK(n_page_end == 1, "page end emitted");
        CHECK(n_session_end == 1, "session end emitted");
        CHECK(br5_parser_state(p) == BR5_ST_IDLE, "state back to IDLE");
        CHECK(jpeg_intact(p), "JPEG byte-identical");
        br5_parser_free(p);
    }

    /* 4: EOI + record burst inside the SAME data chunk */
    {
        reset_counts();
        br5_parser_t *p = br5_parser_new(event_cb, NULL);
        CHECK(p != NULL, "parser alloc");
        br5_parser_arm(p);
        uint8_t *buf = (uint8_t *)malloc(jpeg_len + 12 + 4 + 2);
        CHECK(buf != NULL, "malloc");
        memcpy(buf, jpeg, jpeg_len);
        memcpy(buf + jpeg_len, REC_ENDSTAT, 12);
        memcpy(buf + jpeg_len + 12, REC_PAGEEND, 4);
        memcpy(buf + jpeg_len + 16, REC_SCANEND, 2);
        CHECK(br5_parser_feed(p, REC_PROGRESS, 14,
                              BR5_FLAG_CHUNK_START) == 0, "feed rec");
        CHECK(br5_parser_feed(p, buf, jpeg_len + 18,
                              BR5_FLAG_CHUNK_START) == 0, "feed eoi+burst");
        CHECK(n_errors == 0, "no parser errors");
        CHECK(n_records == 4, "all four records recognized");
        CHECK(n_eoi == 1, "EOI seen");
        CHECK(n_page_end == 1 && n_session_end == 1, "page+session end");
        CHECK(br5_parser_state(p) == BR5_ST_IDLE, "state back to IDLE");
        CHECK(jpeg_intact(p), "JPEG byte-identical");
        free(buf);
        br5_parser_free(p);
    }

    /* 5: EOI + merged progress record in the final data chunk */
    {
        reset_counts();
        br5_parser_t *p = br5_parser_new(event_cb, NULL);
        CHECK(p != NULL, "parser alloc");
        br5_parser_arm(p);
        uint8_t *buf = (uint8_t *)malloc(jpeg_len + 14);
        CHECK(buf != NULL, "malloc");
        memcpy(buf, jpeg, jpeg_len);
        memcpy(buf + jpeg_len, REC_PROGRESS, 14);
        CHECK(br5_parser_feed(p, REC_PROGRESS, 14,
                              BR5_FLAG_CHUNK_START) == 0, "feed rec");
        CHECK(br5_parser_feed(p, buf, jpeg_len + 14,
                              BR5_FLAG_CHUNK_START) == 0, "feed eoi+rec");
        CHECK(n_errors == 0, "no parser errors");
        CHECK(n_records == 2, "both progress records recognized");
        CHECK(n_eoi == 1, "EOI seen");
        CHECK(jpeg_intact(p), "JPEG byte-identical");
        free(buf);
        br5_parser_free(p);
    }

    free(jpeg);
    if (failures) {
        printf("test_brscan5_stream_records: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_brscan5_stream_records: all in-process checks passed\n");
    return 0;
}
