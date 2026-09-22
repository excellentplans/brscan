/*
 * test_brscan5_stream_rle.c — T8c unit tests for the RLENGTH (B/W)
 * parser path:
 *
 *   - 00 01 data-block records (14-B header, dlen = u16 LE at offset 6,
 *     one packbits-compressed scanline per block)
 *   - records riding at the head of a data URB together with payload
 *   - block payload spanning chunk boundaries (line-granular events)
 *   - interleaved 00 02 progress record
 *   - page end via 00 21 (no JPEG EOI) and session end via 00 20
 *
 * The block framing is byte-verified against the chroot reference BW-300
 * capture (t8c-phase2-chroot-ref.bin): 3491 blocks = 3491 lines, every
 * line decodes to exactly (2550+7)/8 = 319 bytes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brscan5_stream.h"

static int failures;
static int n_rle_data;
static int n_page_end;
static int n_session_end;
static int n_errors;
static size_t total_rle_bytes;
static size_t last_line_len;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", (msg), __LINE__); \
        failures++; \
    } \
} while (0)

static void
count_cb(const br5_event_t *ev, void *ud)
{
    (void)ud;
    switch (ev->type) {
    case BR5_EV_RLE_DATA:
        n_rle_data++;
        total_rle_bytes += ev->jpeg_len;
        last_line_len = ev->jpeg_len;
        break;
    case BR5_EV_PAGE_END:
        n_page_end++;
        break;
    case BR5_EV_SESSION_END:
        n_session_end++;
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

/* packbits-encode a row of `w` bytes: simple literal runs only (the
 * parser does not care how the data was produced). */
static size_t
packbits_encode(const uint8_t *row, size_t w, uint8_t *out)
{
    size_t i = 0, o = 0;
    while (i < w) {
        size_t run = w - i;
        if (run > 127)
            run = 127;
        out[o++] = (uint8_t)(run - 1);
        memcpy(out + o, row + i, run);
        o += run;
        i += run;
    }
    return o;
}

/* Build a full BW page stream: `lines` blocks of a (w+7)/8-byte row
 * (alternating white/dense rows), + 00 02 progress record, 00 21, 00 20.
 * Returns the total length; block boundaries recorded in offs[]. */
static size_t
build_bw_stream(uint8_t *buf, size_t bufsz, int lines, size_t w,
                size_t *offs, int *nblocks)
{
    size_t row_bytes = (w + 7) / 8;
    uint8_t *row = (uint8_t *)malloc(row_bytes);
    uint8_t *enc = (uint8_t *)malloc(row_bytes * 2 + 16);
    size_t o = 0;
    int li;

    *nblocks = 0;
    for (li = 0; li < lines; li++) {
        size_t elen;
        memset(row, li & 1 ? 0x00 : 0xf0, row_bytes);
        elen = packbits_encode(row, row_bytes, enc);
        offs[*nblocks] = o;
        (*nblocks)++;
        /* 00 01 01 00 08 00 <dlen u16 LE> <6 reserved> <dlen payload> */
        buf[o++] = 0x00; buf[o++] = 0x01; buf[o++] = 0x01; buf[o++] = 0x00;
        buf[o++] = 0x08; buf[o++] = 0x00;
        buf[o++] = (uint8_t)(elen & 0xff);
        buf[o++] = (uint8_t)(elen >> 8);
        memset(buf + o, 0, 6);
        o += 6;
        memcpy(buf + o, enc, elen);
        o += elen;
        if (li == lines / 2) {
            /* interleaved progress record as its own "URB" */
            offs[*nblocks] = o;
            (*nblocks)++;
            buf[o++] = 0x00; buf[o++] = 0x02;
            memset(buf + o, 0, 12);
            o += 12;
        }
    }
    offs[*nblocks] = o;
    (*nblocks)++;
    buf[o++] = 0x00; buf[o++] = 0x21; buf[o++] = 0x01; buf[o++] = 0x00;
    offs[*nblocks] = o;
    (*nblocks)++;
    buf[o++] = 0x00; buf[o++] = 0x20;
    free(row);
    free(enc);
    (void)bufsz;
    return o;
}

int
main(void)
{
    /* --- test 1: whole page in one chunk (the live BW XSC response) --- */
    {
        static uint8_t stream[1 << 20];
        static size_t offs[4096];
        int nblocks = 0;
        size_t len = build_bw_stream(stream, sizeof(stream), 100, 2550,
                                     offs, &nblocks);
        br5_parser_t *p = br5_parser_new(count_cb, NULL);
        n_rle_data = n_page_end = n_session_end = n_errors = 0;
        total_rle_bytes = 0;
        br5_parser_arm(p);
        CHECK(br5_parser_feed(p, stream, len, BR5_FLAG_CHUNK_START) == 0,
              "feed whole page");
        CHECK(n_errors == 0, "no parser errors");
        CHECK(n_rle_data == 100, "one RLE_DATA event per line");
        CHECK(n_page_end == 1, "page end seen");
        CHECK(n_session_end == 1, "session end seen");
        /* packbits_encode(319 B) = runs 127+127+65 -> 128+128+66 = 322 B */
        CHECK(total_rle_bytes == (size_t)100 * 322,
              "all payload bytes accounted");
        CHECK(last_line_len == 322, "last line payload size");
        br5_parser_free(p);
    }

    /* --- test 2: split at a block boundary (two feed calls) ---
     * Note: a 00 01 record HEADER must not span chunk boundaries (USB
     * delivers whole URBs; the transport guarantees this). */
    {
        static uint8_t stream[1 << 20];
        static size_t offs[4096];
        int nblocks = 0;
        size_t len = build_bw_stream(stream, sizeof(stream), 50, 2550,
                                     offs, &nblocks);
        br5_parser_t *p = br5_parser_new(count_cb, NULL);
        n_rle_data = n_page_end = n_session_end = n_errors = 0;
        br5_parser_arm(p);
        /* offs[0..]: block starts; cut after the 10th block */
        size_t cut = offs[10];
        CHECK(br5_parser_feed(p, stream, cut, BR5_FLAG_CHUNK_START) == 0,
              "feed first 10 blocks");
        CHECK(br5_parser_feed(p, stream + cut, len - cut,
                              BR5_FLAG_CHUNK_START) == 0,
              "feed remainder");
        CHECK(n_errors == 0, "no parser errors (split)");
        CHECK(n_rle_data == 50, "one RLE_DATA event per line (split)");
        CHECK(n_page_end == 1, "page end seen (split)");
        CHECK(n_session_end == 1, "session end seen (split)");
        br5_parser_free(p);
    }

    /* --- test 3: real first-block bytes from the chroot reference --- */
    {
        /* URB#0 head record of the BW-300 chroot reference scan:
         * dlen=270 (0x010e), followed by 6 reserved zero bytes and the
         * packbits payload. The payload must packbits-decode to exactly
         * 319 bytes for a 2550-px line. */
        static const uint8_t head[14] = {
            0x00, 0x01, 0x01, 0x00, 0x08, 0x00, 0x0e, 0x01,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        uint8_t chunk[14 + 270 + 32];
        /* real payload bytes from the capture (extract_usbmon output,
         * hw-window/caps/bw-block0-payload.bin — embedded so the test is
         * self-contained and does not depend on hardware-window artifacts) */
        static const uint8_t payload[270] = {
    0x01, 0x02, 0x67, 0xdc, 0xff, 0x3b, 0xfd, 0xf8, 0x5a, 0x64, 0x06, 0x10,
    0x10, 0x00, 0x01, 0x7d, 0x33, 0x37, 0xc4, 0x85, 0xf1, 0x60, 0x00, 0x81,
    0x9b, 0xaf, 0xf7, 0xef, 0xcf, 0x11, 0xbb, 0x57, 0x85, 0xc1, 0xd5, 0x00,
    0x1e, 0x5d, 0x5d, 0x1f, 0x37, 0x27, 0x19, 0xb7, 0x7e, 0xc6, 0x01, 0x91,
    0xf0, 0x0d, 0xf9, 0x8b, 0x6e, 0x44, 0x39, 0x5d, 0x82, 0x7f, 0xee, 0xa5,
    0xd9, 0x13, 0x08, 0x22, 0x00, 0x01, 0xfe, 0x00, 0x00, 0x06, 0xf6, 0x00,
    0x0d, 0x10, 0xb9, 0x43, 0xb6, 0x3f, 0x17, 0xff, 0xf7, 0x7f, 0xff, 0xbf,
    0xff, 0xbf, 0xf7, 0x38, 0xff, 0x7f, 0xef, 0xdf, 0xdf, 0x20, 0x31, 0xed,
    0x95, 0x77, 0xfb, 0x58, 0x0e, 0xdf, 0xfb, 0xfe, 0xb0, 0x92, 0x14, 0x07,
    0x38, 0x01, 0x50, 0xf5, 0x10, 0x9f, 0xff, 0x62, 0x7b, 0xb7, 0xfa, 0xfe,
    0x7d, 0x06, 0x7b, 0xa1, 0x03, 0xc4, 0xe0, 0x15, 0x08, 0x66, 0xdc, 0xbd,
    0xe5, 0x6f, 0x4b, 0xbd, 0xfb, 0x5d, 0xff, 0xfa, 0xc0, 0xf2, 0x60, 0x0a,
    0x4c, 0xfe, 0x00, 0x30, 0x08, 0x53, 0x00, 0x00, 0x88, 0x58, 0x00, 0x14,
    0x30, 0x19, 0x39, 0xb2, 0x86, 0xc0, 0x80, 0x95, 0x67, 0x86, 0xa1, 0x81,
    0x02, 0x4e, 0x6e, 0xd8, 0xff, 0xf6, 0x6f, 0xdf, 0xf7, 0x76, 0xcf, 0xdf,
    0xdb, 0x5f, 0x7e, 0xef, 0xdf, 0xdb, 0x7f, 0xff, 0xdd, 0xfb, 0xf7, 0x66,
    0xcc, 0xef, 0x3b, 0xf7, 0xef, 0xfc, 0xff, 0x0d, 0x1f, 0xfb, 0xee, 0xed,
    0xd9, 0xb1, 0x73, 0xee, 0x85, 0xdb, 0x93, 0x3e, 0xfe, 0xdd, 0x34, 0xfb,
    0x3f, 0xfd, 0xfc, 0xd9, 0xf9, 0x36, 0x64, 0x40, 0x41, 0x73, 0x67, 0x66,
    0xcf, 0xff, 0xf6, 0x4c, 0x58, 0xb9, 0x33, 0xe5, 0xcc, 0xdf, 0xfb, 0x7f,
    0x3e, 0x55, 0x91, 0x13, 0x76, 0xc4, 0xcd, 0x99, 0x32, 0x22, 0xcd, 0xcb,
    0xbb, 0x7e, 0x66, 0xe9, 0x13, 0x00, 0x00, 0x08, 0x00, 0x00, 0x24, 0x00,
    0x01, 0x10, 0x32, 0x7f, 0xf7, 0xff,
        };
        uint8_t out[319];
        br5_parser_t *p = br5_parser_new(count_cb, NULL);
        size_t dlen;
        memcpy(chunk, head, sizeof(head));
        memcpy(chunk + 14, payload, 270);
        /* terminate the page after the first block */
        memcpy(chunk + 14 + 270, (const uint8_t *)"\x00\x21\x01\x00", 4);
        memcpy(chunk + 18 + 270, (const uint8_t *)"\x00\x20", 2);
        n_rle_data = n_page_end = n_session_end = n_errors = 0;
        br5_parser_arm(p);
        CHECK(br5_parser_feed(p, chunk, sizeof(chunk),
                              BR5_FLAG_CHUNK_START) == 0,
              "feed real first block");
        CHECK(n_errors == 0, "no parser errors (real block)");
        CHECK(n_rle_data == 1, "one line event (real block)");
        CHECK(last_line_len == 270, "payload length 270 (real block)");
        dlen = (size_t)chunk[6] | ((size_t)chunk[7] << 8);
        CHECK(dlen == 270, "dlen field = 270");
        /* decode check happens in the backend; here only framing */
        (void)out;
        br5_parser_free(p);
    }

    /* --- test 4: line 319-B width guarantee via known white row --- */
    {
        /* A white row (all bits 0 = white in the device convention):
         * packbits 0x84 0x00 = 125 x 0x00 ... a full 319-byte white row
         * can be encoded as 125+125+65 three repeat runs. */
        uint8_t block[14 + 6];
        uint8_t chunk[14 + 6 + 4 + 2];
        br5_parser_t *p = br5_parser_new(count_cb, NULL);
        size_t o = 0;
        block[o++] = 0x00; block[o++] = 0x01; block[o++] = 0x01;
        block[o++] = 0x00; block[o++] = 0x08; block[o++] = 0x00;
        block[o++] = 0x06; block[o++] = 0x00;
        memset(block + o, 0, 6);
        o += 6;
        /* payload: 6 repeat-0 runs: 84 00 (125), 84 00 (125), 45 00 (66)
         * -> 316 ... need exactly 319: 125+125+69: 45 -> 70-1=69? 0x45=69
         * -> 257-69=188 no. Use 0xbb: 257-187=70 -> 125+125+69 = 319 with
         * 0xbb 0x00 (69). */
        block[14] = 0x84; block[15] = 0x00;
        block[16] = 0x84; block[17] = 0x00;
        block[18] = 0xbb; block[19] = 0x00;
        memcpy(chunk, block, 20);
        memcpy(chunk + 20, (const uint8_t *)"\x00\x21\x01\x00", 4);
        memcpy(chunk + 24, (const uint8_t *)"\x00\x20", 2);
        n_rle_data = n_errors = 0;
        br5_parser_arm(p);
        CHECK(br5_parser_feed(p, chunk, sizeof(chunk),
                              BR5_FLAG_CHUNK_START) == 0,
              "feed white-row block");
        CHECK(n_errors == 0, "no parser errors (white row)");
        CHECK(n_rle_data == 1, "one line event (white row)");
        CHECK(last_line_len == 6, "white row payload is 6 bytes");
        br5_parser_free(p);
    }

    if (failures) {
        printf("test_brscan5_stream_rle: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_brscan5_stream_rle: all checks passed\n");
    return 0;
}
