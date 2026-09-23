#ifndef BRSCAN5_STREAM_H
#define BRSCAN5_STREAM_H

/*
 * brscan5_stream.h — incremental, byte-oriented stream parser for the
 * Brother DS-640 scan-data phase (USB EP 0x83 IN).
 *
 * Standalone C library (no external deps). See README.md for the protocol
 * notes, the state diagram and the framing rule this parser is built on.
 *
 * Protocol reverse-engineered from USB captures of a Brother DS-640;
 * see docs/brscan5-protocol.md.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Feed flags                                                          */
/* ------------------------------------------------------------------ */

/* The chunk handed to br5_parser_feed() begins at a USB URB boundary.
 * DS-640 framing is URB-driven: status records arrive as their own short
 * URBs, JPEG data in large (262144-byte) URBs. Record detection only
 * happens at chunk starts carrying this flag. */
#define BR5_FLAG_CHUNK_START 0x0001u

/* ------------------------------------------------------------------ */
/* Known record ids (record_id field of BR5_EV_RECORD)                 */
/* ------------------------------------------------------------------ */

#define BR5_RECORD_BLOCK     0x0001u   /* RLENGTH data block: 14-B header
                                       * (dlen = u16 LE at offset 6) + dlen B
                                       * payload; one block = one scanline
                                       * (T8c, chroot BW-300 capture)      */
#define BR5_RECORD_PROGRESS  0x0002u   /* 14 B, during transfer          */
#define BR5_RECORD_ENDSTAT   0x0011u   /* 12 B, completion statistics    */
#define BR5_RECORD_PAGE_END  0x0021u   /*  4 B, page end                 */
#define BR5_RECORD_SCAN_END  0x0020u   /*  2 B, scan/session end         */

/* Max accumulated JPEG size (bytes). Doubling buffer; overflow => error. */
#define BR5_JPEG_CAP_MAX     (64u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* Parser states                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    BR5_ST_IDLE = 0,   /* no active scan; br5_parser_arm() starts one  */
    BR5_ST_ARMED,      /* scan started (XSC-OK): expect 00 02 or data  */
    BR5_ST_DATA,       /* accumulating JPEG payload until EOI (ff d9)  */
    BR5_ST_PAGE_DONE,  /* page JPEG complete; expect 00 11/00 21/00 20 */
    BR5_ST_SESSION_DONE, /* after page-end 00 21; expect 00 20 / 00 02 */
    BR5_ST_ERROR       /* fatal error; parser refuses further input    */
} br5_state_t;

/* ------------------------------------------------------------------ */
/* Events                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    BR5_EV_RECORD = 1,      /* status record (record_id, record_payload, record_len) */
    BR5_EV_JPEG_DATA,       /* JPEG bytes appended (jpeg_data, jpeg_len)              */
    BR5_EV_JPEG_EOI,        /* JPEG complete: EOI ff d9 reached                        */
    BR5_EV_PAGE_END,        /* page-end record 00 21 processed                         */
    BR5_EV_SESSION_END,     /* session-end record 00 20 processed                      */
    BR5_EV_WARN,            /* non-fatal: unknown record, large trailing               */
    BR5_EV_ERROR,           /* fatal: JPEG overflow, protocol violation                */
    BR5_EV_RLE_DATA         /* RLENGTH payload bytes appended (jpeg_data,
                               jpeg_len); one event per complete block */
} br5_event_type_t;

typedef struct {
    br5_event_type_t type;
    br5_state_t state;          /* parser state when the event fired        */
    uint64_t stream_pos;        /* record: offset where it began; others:
                                   bytes consumed when the event fired      */
    uint16_t record_id;         /* BR5_EV_RECORD / PAGE_END / SESSION_END   */
    const uint8_t *record_payload; /* record bytes (valid during callback)  */
    size_t record_len;          /* record length                            */
    const uint8_t *jpeg_data;   /* BR5_EV_JPEG_DATA: appended bytes         */
    size_t jpeg_len;            /* number of appended bytes                 */
    const char *message;        /* BR5_EV_WARN / BR5_EV_ERROR               */
} br5_event_t;

typedef void (*br5_event_cb)(const br5_event_t *ev, void *userdata);

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

typedef struct br5_parser br5_parser_t;

/* Heap convenience wrappers. */
br5_parser_t *br5_parser_new(br5_event_cb cb, void *userdata);
void br5_parser_free(br5_parser_t *ctx);

/* (Re)start a scan session: ST_IDLE -> ST_ARMED. Call after XSC-OK. */
void br5_parser_arm(br5_parser_t *ctx);

/* Reset to ST_IDLE (buffers cleared, error state cleared). */
void br5_parser_reset(br5_parser_t *ctx);

/* Number of completed RLENGTH blocks (scanlines) in the current page. */
size_t br5_parser_rle_lines(const br5_parser_t *ctx);

/* Feed one chunk. flags: BR5_FLAG_CHUNK_START. Returns 0 on success,
 * -1 after a fatal error (state == BR5_ST_ERROR). */
int br5_parser_feed(br5_parser_t *ctx, const uint8_t *chunk, size_t len,
                    unsigned flags);

br5_state_t br5_parser_state(const br5_parser_t *ctx);

/* Completed JPEG (SOI..EOI inclusive) once BR5_EV_JPEG_EOI fired. */
const uint8_t *br5_parser_jpeg_data(const br5_parser_t *ctx);
size_t br5_parser_jpeg_size(const br5_parser_t *ctx);

/* Bytes fed so far (stream position). */
uint64_t br5_parser_stream_pos(const br5_parser_t *ctx);

/* Bytes discarded after EOI (trailing data). */
uint64_t br5_parser_trailing(const br5_parser_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* BRSCAN5_STREAM_H */