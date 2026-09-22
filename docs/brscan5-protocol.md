# Brother DS-640 — brscan5 USB Protocol Specification

Status: 2026-09-21 · Source: usbmon captures of the working amd64 driver
(captured USB traffic, text format) **plus binary usbmon captures**
(`*.pcapmon`, untruncated) plus disassembly of the proprietary driver
(the vendor's proprietary `libLxBsScanCoreApi.so.3.2.6` from the official
brscan5 deb package).

Device: Brother DS-640, USB `04f9:0468`.
Interface 1 (scanner): **EP 0x04 OUT** (commands), **EP 0x83 IN** (data/responses).
Interface 0: EP 0x02 OUT / EP 0x81 IN (not part of this protocol).
EP 0 (control): Session-Open/Close.

> ⚠️ **Capturing limitation:** the usbmon **text format** truncates every URB payload to
> **32 bytes**. Commands > 32 B (SSP 278 B, XSC 43 B) and the complete JPEG stream
> cannot be reconstructed **byte-exactly** from it. Byte-exact golden vectors for these
> fields require a **binary** usbmon capture. Where possible, the missing part was
> reconstructed from the driver disassembly (marked as `reconstructed`).

> ✅ **T8a update (2026-09-21):** Binary usbmon captures of both test-matrix passes
> (pass1/pass2, mon_bin API0 format, 48-B header + payload, untruncated).
> **SSP is thereby proven byte-exactly for all 9 RESO×CLR combinations** —
> the MakeSSPcmdString reconstruction was correct. XSC remains reconstructed (prefix
> byte-exact; without paper our port does not send XSC — see §1a). Details: raw
> binary captures plus per-pass extracts, produced with the dedicated
> binary-usbmon extractor tooling.
>
> ⚠️ **T8b correction (2026-09-21):** The T8a statement "SSP proven byte-exactly for all
> 9 combinations" was **circular** — pass1/pass2 are OUR port (9-combination matrix,
> HWTEST runs), not the reference driver. The real reference SSP body differs
> substantially (CLR=C24BIT, AREA=NORMAL, BRIT/CONT=50, no ATCL/THRS/RATE lines,
> PAGE=0, TONE=ON, flags OFF) and has been proven byte-exactly since T8b
> (a binary usbmon capture of the reference-chroot driver). XSC is
> option-dependent (AREA parameter, 43-B ATDSKW or 50-B pixel-coordinate variant).
> Details: §1a; the reference vector is pinned by the encoder unit test
> (`test_t8b_reference_vectors`).

---

## 1. Command table (OUT, EP 0x04)

All commands are ASCII text, begin with `ESC` (0x1b) and end with `\n\x80`.
`\x80` (0x80) is the terminator marker of the text block.

| Command | Bytes (hex) | ASCII | Length | Purpose |
|---|---|---|---|---|
| Q | `1b 51 0a 80` | `ESC Q\n\x80` | 4 | Dev-Status-Query |
| QDI | `1b 51 44 49 0a 80` | `ESC QDI\n\x80` | 6 | Device info |
| CKD | `1b 43 4b 44 0a 50 53 52 43 3d 41 44 46 0a 80` | `ESC CKD\nPSRC=ADF\n\x80` | 15 | Document check (feeder) |
| SSP | `1b 53 53 50 0a 4f 53 3d 4c 4e 58 0a 50 53 52 43 3d 41 44 46 0a 52 45 53 4f 3d 33 30 30 2c 33 30 …` (278 B) | `ESC SSP\nOS=LNX\nPSRC=ADF\nRESO=300,30…` | 278 | Scan settings |
| XSC | `1b 58 53 43 0a …` (43 B with `AREA=ATDSKW`, 50 B with pixel coordinates `AREA=0,0,2550,4200`) | `ESC XSC\nRESO=…\nAREA=…\nMODE=NORMAL\n\x80` | 43/50 | Scan start |

**Proven (captured, byte-exact):** Q, QDI, CKD complete; **SSP complete
(T8b reference binary capture, 278 B)**; XSC complete in two variants (43 B
`ATDSKW` from the text-format capture prefix + reconstruction, 50 B
pixel coordinates from the T8b reference binary capture). XSC is **option-dependent**
(AREA parameter), not a fixed vector. Details: §1a; the corrected vectors are
pinned by the encoder unit test (`test_t8b_reference_vectors`).

**XSC (option-dependent, T8b: two variants proven byte-exact):**
```
Variant A (43 B, text-format usbmon capture of an auto-deskew run):
  ESC XSC\nRESO=300,300\nAREA=ATDSKW\nMODE=NORMAL\n\x80
Variant B (50 B, T8b reference binary capture, scanimage defaults/full area):
  ESC XSC\nRESO=300,300\nAREA=0,0,2550,4200\nMODE=NORMAL\n\x80
```
- `RESO=<x>,<y>` — resolution in dpi.
- `AREA=…` — for `ATDSKW`/literal values (`FULL, OVER, AUTO, ATDSKW, NORMAL`) the
  area is determined in the device; otherwise explicit pixel coordinates
  `left,top,right,bottom` = round(mm·dpi/25.4) (2550×4200 = full 215.88×355.567 mm
  at 300 dpi). The SSP `AREA=` value mirrors the configuration
  (`NORMAL` ⇔ coordinates in XSC, `ATDSKW` ⇔ `ATDSKW`).
- `MODE=NORMAL` — special mode. Values in the driver: `EDGEPRE, ORIGINAL, ATC1PASS, NORMAL`.

**SSP (278 B, T8b proven byte-exact — binary capture of the reference-chroot
driver):** complete structure; AREA/BRIT/CONT are
option-dependent (BRIT/CONT = option value + 50, default 50/50):
```
ESC SSP\n
OS=LNX\nPSRC=ADF\nRESO=<x>,<y>\nCLR=<clr>\nAREA=<area>\nMRGN=0,0,0,0\n
DPLX=OFF\nBRIT=<brit>\nCONT=<cont>\nCOMP=JPEG\nJSF=420\nIPRC=NORMAL\n
PTYPE=NORMAL\nPAGE=0\nLONG=OFF\nCARR=OFF\nRMGC=OFF\nDTDF=OFF\n
DT4V=OFF\nDSKW=OFF\nLSMD=OFF\nRMBP=OFF\nRMMR=OFF\nGMMA=OFF\nTONE=ON\n
QTFD=OFF\nATCN=OFF\nATCRP=OFF\nUSER=\n\x80
```
Length: 278 B with 6-character CLR (`C24BIT`) + `AREA=NORMAL`. **⚠️ T8b correction:**
the earlier reconstruction (ATCL/THRS/RATE lines, 0 values, PAGE=1, TONE=0,
OPDF=CONT, no \n before 0x80) was WRONG — the T8a "captured matrix" was a
self-echo of our port (pass1/pass2 = our HWTEST runs) and the device
rejected the old body with `83 53 53 50 00 00 00 00`. The complete hex bytes
of the reference vector are pinned in the unit test
`tests/unit/test_brscan5_encoders.c` (`test_t8b_reference_vectors`). CLR mapping
(proven: C24BIT for 24bit Color):
`CLR={TEXT,ERRDIF,GRAY256,C256,C24BIT,AUTO}`; `AREA={FULL,OVER,AUTO,ATDSKW,NORMAL}`
(observed: `NORMAL` + pixel coordinates in XSC, `ATDSKW`); `COMP={RLENGTH,JPEG,NONE}`;
`JSF={422,411,420,400,444}`; `IPRC={COPY,OCR,PHOTO,TXTGR,NORMAL}`;
`PTYPE={THIN,THOCK,PCARD,NORMAL}`; `DPLX={BACK,ON,OFF}`; `MRGN=<4 numbers, comma-separated>`.

---

## 1a. Sequence/order (T8b, live-verified) — **proven**

Reference driver (text-format usbmon captures + the **T8b binary capture**,
chroot reference driver, empty feeder) per scan start:

```
c0 01 (GET_OPEN,  wValue=0x0002, wIndex=0, wLength=255 → 05 10 01 02 00)  — BEFORE the first Q
Q        → 75-B status
QDI      → 662-B device info
CKD      → 00 02 (feeder EMPTY — does NOT abort!)
c0 02 (GET_CLOSE, wValue=0x0002, wIndex=0, wLength=255 → 05 10 02 02 00)  — after CKD
  (~300 ms settle: 302/308 ms in both reference captures)
c0 01 (GET_OPEN)  → 05 10 01 02 00                                        — after c0 02
SSP      → 38-B ack 00 53 53 50 1e …    (even with an empty feeder!)
XSC      → 90 00 (error/feeder empty)
c0 02 (GET_CLOSE) — after scan end/XSC error (= session close)
```

Supplemented by a text-format usbmon capture with paper in the feeder: identical sequence, CKD → `00 01`,
XSC → 14-B progress record `00 02 …`, then the JPEG data phase.

**Exact libusb translation of the control messages (T8b):**

| bmRequestType | bRequest | wValue | wIndex | wLength | Response | Meaning |
|---|---|---|---|---|---|---|
| 0xC0 (D2H, Vendor) | 0x01 | 0x0002 | 0x0000 | 0x00FF | `05 10 01 02 00` | GET_OPEN (session open; identical to the legacy `OpenDevice()` BREQ_GET_OPEN/BCOMMAND_SCANNER) |
| 0xC0 (D2H, Vendor) | 0x02 | 0x0002 | 0x0000 | 0x00FF | `05 10 02 02 00` | GET_CLOSE (session close; identical to the legacy `CloseDevice()` BREQ_GET_CLOSE) |

Response format: `05 10 <bRequest echo> 02 00` (0x10 = BDESC_TYPE, 0x02 =
BCOMMAND_SCANNER echo, 0x00 = OK). The text capture shows wLength 0x00FF; our
legacy CloseDevice sends wLength 5 — the device responds with the same
5 bytes in both cases (pass1-ckd proves the wLength-5 variant for GET_CLOSE). The
reference driver does NOT release/claim the interface during the dance (control
transfers only); the ~300 ms between GET_CLOSE and GET_OPEN are a reference
observation, not a measured device requirement.

**T8b key findings (replace the T8a interpretation):**

1. **The T8a pass1/pass2 binary captures are OUR port, not the reference driver**
   (SET_CONFIGURATION + clear_halt per round = our sane_open; 9-combination matrix). The
   `c0 02` visible there is our session close (CloseDevice) after the abort — NOT
   a dance of the reference driver. The dance is only visible in the text-format
   captures and, since T8b, additionally in the
   reference binary capture.
2. **Our port put the dance on the wire live and byte-exactly** (T8b binary
   capture of our port: GET_OPEN before Q, GET_CLOSE + 300 ms + GET_OPEN after CKD,
   identical responses) — and SSP was STILL rejected with
   `83 53 53 50 00 00 00 00`. **The 0x83 cause is the SSP body, not the dance.**
3. **The SSP body was wrong (the T8a "byte-exact" claim was circular):** the T8a
   9-combination matrix was a self-echo of our encoder (pass2 = our HWTEST run). The real
   reference body (278 B, T8b binary capture): `CLR=C24BIT`, `AREA=NORMAL`,
   `BRIT=50`/`CONT=50` (option value + 50), **no** ATCL/THRS/RATE lines,
   `PAGE=0`, `TONE=ON`, flag fields `OFF` instead of `0`, no `OPDF=`,
   `USER=` with `\n` before the `0x80` terminator. With the old body → 0x83.
   Corrected encoder vectors: pinned by the encoder unit test
   (`test_t8b_reference_vectors`).
4. **XSC is option-dependent, not fixed:** reference chroot run (scanimage defaults,
   full area): 50 B `ESC XSC\nRESO=300,300\nAREA=0,0,2550,4200\nMODE=NORMAL\n\x80`
   (AREA in pixels = round(mm·dpi/25.4)). The text-format capture showed the
   43-B `ATDSKW`
   variant. Both are valid variants; the encoder takes the area as a parameter.
5. **CKD empty does NOT abort** — both drivers continue to SSP/XSC; XSC is the
   point at which the reference driver receives `90 00` (→ SANE_STATUS_NO_DOCS).
   Since T8b the brscan5 port follows the same flow (the obsolete HWTEST
   SSP_ALWAYS hook has been removed).
6. The QDI response (662 B) and Q response (75 B) are byte-identical across all
   combinations; CKD responds `00 02` (empty) / `00 01` (paper).

---

## 2. Response table (IN, EP 0x83)

Each response arrives as its **own URB** on EP 0x83 (not concatenated).

| Command | Response length | Initial bytes (hex) | Meaning |
|---|---|---|---|
| Q | 75 B | `c1 00 49 10 00 17 …` | Dev status (OK) |
| QDI | 662 B | `00 51 44 49 …` (="QDI" header), contains "DS-640" | Device info |
| CKD (paper) | 2 B | `00 01` | Paper in feeder |
| CKD (empty) | 2 B | `00 02` | Feeder empty |
| SSP | 38 B | `00 53 53 50 1e …` (="SSP" header + 0x1e=30) | Settings ack (reference driver, both paper states) |
| SSP (rejecting) | 8 B | `83 53 53 50 00 00 00 00` | Status 0x83 = rejected (clarified in T8b: wrong SSP body; the control dance alone is NOT sufficient — our port sent the dance correctly and still received 0x83 until the body was fixed) |
| SSP (busy) | 8 B | `b0 53 53 50 …` | Status 0xb0 = **busy** (T8c, proven after cancel + immediate re-scan; binary capture, ts 1790027706.561) |
| XSC (empty) | 2 B | `90 00` | Feeder empty → `SANE_STATUS_NO_DOCS` (T8c phase 1, byte-exact live) |
| XSC (jam) | 2 B | `91 00` | **Document jam at the device** → `SANE_STATUS_JAMMED` (T8c binary capture, offset …438.706; T8d mapping, legacy 0xC3/DOCJAM convention). ~10 s of silence before the response, no data phase, USB stays up, recovery without replug ✅ |
| XSC (with paper, Color/Gray) | 14 B | `00 02 …` | Scan start, first progress record; JPEG data phase follows |
| XSC (with paper, B/W) | 14 B | `00 01 01 00 08 00 <dlen> …` | Scan start, **first RLENGTH block record** — rides in the same URB as ~52 kB of page data (BW 300: 52076-B URB, chroot reference) |

The QDI response is 662 B (in the text capture only the first 32 B are visible). The driver
parses categories/options from it; for the port it suffices to skip the length.

---

## 3. Data phase after XSC (with paper)

### 3a. Color/Gray: JPEG on the wire (T8c live-confirmed)

After XSC the device delivers a **raw JPEG stream** on EP 0x83, beginning directly
with `ff d8 ff e0 0010 JFIF` (JFIF density 300x300 dpi). No framing, no packbits
inside the JPEG itself. **T8c: Gray (GRAY256) is also JPEG on the wire** — the device
JPEG is a 1-component JFIF that decodes directly through libjpeg (live-verified,
`decoder page …, comps=1`).

In between there are **status records**, each delivered as its **own URB**:

| Record ID | Length | Observed content (hex) | Meaning |
|---|---|---|---|
| `00 02` | 14 B | `00 02 01 00 15 00 00000000 08 000000 0000` resp. `… 15 00 1361 0700 9d0d0000` | Progress (scanned bytes/lines; `0x0d9d` = 3485 ≈ A4 lines @300dpi) |
| `00 11` | 12 B | `00 11 01 00 8e090000 9d0d0000` | Final statistics |
| `00 21` | 4 B | `00 21 01 00` | Page end |
| `00 20` | 2 B | `00 20` | Scan/session end |

Sequence (scan, proven from the text-format usbmon capture with paper):
```
XSC
  → 14-B 00-02 record
  → JPEG data URBs (262144 B, starting ff d8) — further 00-02 records in between
  → last data chunk (221459 B)
  → 00 11 (12 B)
  → 00 21 (4 B)
  → 00 20 (2 B)
  → (control close)
```

### 3b. B/W (COMP=RLENGTH): block records, no JPEG (T8c, proven byte-exact)

With `CLR=TEXT / COMP=RLENGTH / TONE=OFF` (SSP 280 B, T8c native captures)
the device delivers **NO JPEG**. The page data is a sequence of
`00 01` block records, **one record per scan line**:

```
00 01 01 00 08 00 <dlen:u16 LE> <6 B reserved> <dlen B packbits payload>
```

- Header 14 B; `dlen` = payload length (u16 LE, offset 6..7). The 6 reserved bytes
  are usually 0; in end blocks `0000 a30d 0000` was observed (`0x0da3` = 3491 =
  line count — the records carry the real geometry).
- **Each line decodes to exactly `(width+7)/8` bytes** (300 dpi/2550 px → 319 B;
  verified 3491/3491 lines of exactly 319 B in the T8c chroot oracle capture;
  600 dpi correspondingly (5100+7)/8 = 639 B/line).
- **Packbits (vendor semantics, libbrscandec FUN_001063f3):** control byte `< 0x80`
  → c+1 literals; `> 0x80` → repeat the next byte (257-c)×; `0x80` → skip.
- **Bit convention: bit 1 = BLACK** (white line = 0x00 repetitions) — identical
  to the backend's SANE/"1=black" convention, **no bit flip needed**.
- **End:** `00 21` (4 B) → `00 20` (2 B), **no JPEG EOI** and (observed) no
  `00 11` end-stat record in the B/W path.
- Blocks can start in the middle of a URB; the first block rides **in the XSC
  response URB** (BW 300 chroot: 52076-B URB = XSC start + data; URB distribution
  52076/48746/6474 + 00 21 + 00 20 = 107296 B page payload).

Replay verification: `tests/data/brscan5/fixture-bw300.tlv` (from the chroot
capture, regenerated byte-identically by the fixture tooling) → scanimage BW 300 → PBM,
pixel correlation 1.0000 vs. the chroot reference (T8c); pinned by md5 as the ctest
`brscan5_replay_bw` (T8d).

### 3c. Dark right edge in native B/W (T8d analysis) — **device content, not a backend bug**

Finding (evidence: native B/W scan output vs. the chroot reference JPEG scans):

- The dark right edge in native B/W scans starts at **x≈2486–2491 (300 dpi)**,
  BW 600 at **x≈5032** (= 2× 2516, same mm position ≈ 211 mm) and runs to the
  right image border. 210 mm A4 width ≈ 2480 px @300 dpi — the edge is the
  **area beyond the paper edge** (scan width 215.9 mm = 2550 px > paper).
- **The zone also exists in the chroot reference** — there as dark gray
  (column average ≈ 42–92, identical transition position 2484–2486), because the
  vendor decoder delivers grayscale/JPEG output. "The chroot doesn't show it" was
  only true visually (soft gray vs. hard black).
- Natively the zone becomes **hard black**, because B/W 1-bit with `TONE=OFF` is
  thresholded (dark gray < threshold → 1 = black). Raw PBM evidence: all 3508 lines
  have px 2544–2551 set (last byte 0xff), i.e. the padding bits px 2548/2549
  (est_w 2548 vs. 2550 data width) are part of the band and are discarded by the
  PBM writer — but the start of the band (~2491) clearly lies within the declared
  width.
- **Not explainable backend-side** (RLENGTH block boundaries/decoder correct,
  correlation 1.0). T9 option (not implemented): tie the `AREA=` width to the
  paper/option width (or device-side auto-trim as with the native 150-dpi
  behavior, which delivered 1771 instead of 2100 lines), so that the
  background area is not scanned at all.

---

## 4. Framing rule (core decision) — **proven**

**Proven from the byte offsets of the captures** (not guessed):

1. **Every status record arrives as its own URB** (length = record length: 14/12/4/2 B),
   **not** interleaved in the middle of a JPEG data URB. Data URBs are exactly 262144 B
   (or the last one 221459 B).
   → Framing is **URB-/chunk-boundary-driven**, not pattern-based: a short
   IN URB whose length matches a record length is a status record; a
   262144-B URB is JPEG data.

2. **JPEG start:** the JPEG (`ff d8`) begins **directly at the end of the first
   00-02 record**. Concretely: first 00-02 record at stream offset **0** (14 B,
   offsets 0–13), JPEG SOI at offset **14**. → SOI follows immediately (fixed
   offset, no further gap).

3. **Record offsets (proven):**

   | Record | Stream offset | Length |
   |---|---|---|
   | 00 02 (1st) | 0 | 14 |
   | JPEG SOI | 14 | — |
   | 00 02 (2nd) | 524302 | 14 |
   | 00 02 (3rd) | 1048604 | 14 |
   | 00 11 | 1532221 | 12 |
   | 00 21 | 1532233 | 4 |
   | 00 20 | 1532237 | 2 |
   | **Total (data phase)** | — | **1532239** |

4. **EOI thesis verified:** the device transmits **1,532,239 B** (≈ 1.53 MB) in the
   data phase, but the decodable JPEG is only **627,980 B** (1× SOI, 1× EOI).
   After the JPEG EOI (`ff d9`) there are **~0.9 MB of trailing data** (padding +
   records).
   → **The parser MUST terminate at EOI (`ff d9`) and discard the rest.**
   "Read until the URB is empty" or "read a fixed size" does NOT work.

> **Note on the EOI trailing figure:** it assumes that the reference JPEG
> (628 KB) is byte-identical to the SOI→EOI section of the stream. The SOI region
> demonstrably matches (`ff d8 ff e0 0010 JFIF` … 300 dpi). A byte-exact trailing
> figure requires a binary usbmon capture.

### 4a. Framing exceptions at the device (T8e, hardware finding) — **proven by output forensics**

The rule "record = own URB" (above) is only **one** scheduling outcome. At the
real device the **USB short-packet rule** additionally applies:

1. **Record at the tail of a data URB.** If a status record is sent while
   the host is just reading a 262144-B data URB, **the short packet (14 B < 512 B)
   terminates that URB** — the `read()` result then ends with
   `[JPEG part …][14-B record]`. This is a race (timing), not a fixed state:
   the same scan delivers the record sometimes as its own URB, sometimes at the
   chunk end.
   **Proof:** the native color/gray scans of the T8c session show localized
   shear/ghost bands ("compressed/overlapped"); B/W (its own RLENGTH framing path,
   which already scans mid-chunk) is clean (T8e output forensics).
2. **Record bursts:** after the JPEG EOI, `00 11`/`00 21`/`00 20` can arrive
   together in one read (burst) instead of individually.
3. **Consequence for the parser:** record detection must NOT rely solely on
   chunk boundaries with exact record length. The brscan5 parser (T8e)
   therefore also detects records via **strict signatures**
   (`00 <id> 01 00`, for the 00-02 progress record additionally `00` at offset 5):
   a) at the **chunk end** (tail merge) and b) as a **record sequence** in
   non-data phases and behind the EOI. The signature strictness keeps false
   positives in the JPEG entropy stream negligible (~2^-32 per position).
   Byte-exactness is proven in replay: `tests/data/brscan5/fixture-color-300-recmerge.tlv`
   (record at the data-URB tail) decodes **pixel-identically** (0 differing bytes,
   row-profile correlation 1.000000 at dy=0) to the reference.

**Geometry policy (T8e, binding for frontends):**

- `sane_get_parameters` reports **before** `sane_start` the option estimate
  (AREA × dpi) and **after** `sane_start` **always the real JPEG header dimensions**
  (`jpeg_read_header` in sane_start; T6 design, for all modes). Canvas and
  `bytes_per_line` of the delivery are thus always consistent
  (Color: `w*3`/RGB, Gray: `w`/8-bit, BW: `(w+7)/8`/1-bit).
- With `XSC AREA=0,0,2550,4200` the device scans the **full area**: the
  wire JPEG is 2550×4200, even if the paper is shorter. The area below the paper
  end is **device-side 128-luminance fill** (expected, honest).
  **Not** the fill is the bug — a mismatch between reported parameters and
  delivered bytes would be. Both are mapped exactly onto the wire-JPEG dimensions;
  the policy "canvas = scan area including the gray tail" is retained.
- BW/RLENGTH: `real_w = est_w` (XSC AREA), `real_h` = block count of the capture;
  internal consistency `row_bytes == bytes_per_line` holds (the known
  cosmetic deviation est_w 2548 vs. 2550 remains — T9 open item).

---

## 5. Timing (from timestamps, usbmon µs)

| Event | Δt |
|---|---|
| Q → response | 0.004 s |
| QDI → response | 0.006 s |
| CKD → response | 0.001 s |
| SSP → response | 0.011 s |
| XSC → first data | **9.1 s** (physical scan/pre-roll) |
| XSC → response **with paper** (T8c) | **8.8–9.3 s** (the device pulls the sheet first; empty feeder: 0.3 s) → hence `BRSCAN5_TIMEOUT_XSC=15000` ms in the backend |
| JPEG transfer (SOI→SCANEND) | 0.79 s |
| Progress record interval (during transfer) | ~0.17–0.20 s |
| XSC → response on **jam** (T8c phase 6) | ~10 s of silence, then `91 00` |
| SSP → response after cancel+re-scan (T8c phase 5) | immediate `b0 53 53 50 …` (busy) |

→ Commands respond in milliseconds; the actual scan/transfer time dominates
the wait. A timeout for the data phase should be > 10–15 s.

### 5a. T8c hardware findings (2026-09-21/22)

- **600 dpi natively WITHOUT hang** (Color 1.98 MB / Gray 1.34 MB / BW 7008 blocks,
  each < 1 min clean, no disconnect). The 600-dpi hang observed 2× is a
  **chroot/vendor-driver problem** (qemu + 32-kB scanimage buffer); our
  backend reads with 262144-B persistent reads and scales cleanly.
- **Cancel (phase 5):** sane_cancel itself cleans up properly (drain + clear_halt +
  GET_CLOSE). **An immediate re-scan fails at the device firmware:**
  SSP ack `b0 …` (busy), then XSC silent (15-s budget expired, 3 attempts).
   Recovery only via replug/re-enumeration (dev→16). No backend fix possible/
   needed — for scan loops: avoid cancel, let the sheet run completely.
   XSC ABT (ESC ABT) as a potentially cleaner abort path: unobserved, T9.
- **Idle disconnect:** after ~10 min of idle the device drops itself off the bus
  (proven ≥4× in the T8c kernel logs, presumably device-side power save).
  Scan loops must handle re-enumeration (T9, analysis only).
- **Multi-page:** DS-640 = single slot, no ADF stack; 2 consecutive scanimage
  cycles without replug work (phase 4b ✅).
- **Jam recovery:** directly after `91 00` a new scan works **without
  replug** (verified live) — unlike after cancel.

---

## 6. What the captures do NOT cover (limitations)

- **Cancel:** there is NO capture of an abort (`ESC ABT` / cancel). The
  XSC status styles for "stop by cancel command" exist in the driver but were not
  observed. → Error handling for cancel unresolved.
- **Error/jam:** empty feeder only proven via `90 00` on XSC resp. `00 02` on CKD.
  A real jam ("Document feeder jammed", rc=6) is NOT captured.
- **Multi-page:** the captures contain **exactly one page**. The `00 21`→`00 20` cycle
  for a second page (data phase repeated per page) is not proven.
- **Grayscale / B&W data phase:** SSP settings for all modes are proven byte-exactly
  since T8a, but the **data phase** (JPEG/packbits?) exists in the captures only for
  300 dpi color. The grayscale/B&W data format is open.
- **Other resolutions (data phase):** SSP/XSC for 150/600 dpi settings proven (T8a);
  the actual scan data only for 300 dpi.
- **XSC full text:** prefix (32 B) byte-exact; tail (`MODE=NORMAL\n\x80`) reconstructed.
  A byte-exact XSC proof requires paper in the feeder.
- **c0 02/c0 01 vendor control:** requests/responses proven (`05 10 02 02 00` /
  `05 10 01 02 00`), semantics unresolved; definitely part of the sequence before SSP (T8a).
- **JPEG bytes:** only the SOI region + reference JPEG proven; the complete stream (with
  the exact trailing distribution) requires a binary usbmon capture.
- **QDI content (662 B):** only the prefix captured; the complete option parsing
  is based on driver symbols, not on the capture.

---

## 7. Artifacts

The evidence base consists of the following artifact types; the raw captures,
extracts and tooling themselves live outside this repository. What is checked
in here are the derived fixtures under `tests/data/brscan5/` (`*.tlv`) that
drive the replay ctests, plus the unit tests under `tests/unit/`.

- **Text usbmon captures** — per-direction streams (32-B payload truncation),
  with derived URB metadata/offset tables and framing-offset analyses.
- **Binary usbmon captures** (T8a; mon_bin API0, 48-B header + len_cap payload,
  untruncated) — the test-matrix passes, the reference-chroot run and our port,
  plus per-direction extracts (out.bin/in.bin/events.json).
- **Command golden vectors** — machine-readable; SSP since T8a byte-exact in
  9 variants, the T8b reference vector pinned by the encoder unit test
  (`test_t8b_reference_vectors`).
- **Expected record sequences and framing offsets** per capture (proven).
- **Reference scan output** — validated 300-dpi color JPEG of the reference
  driver plus its JSON metadata.
- **Capture/fixture tooling** — extractors for the text captures and for the
  binary captures (mon_bin API0, 48-B header + len_cap payload), plus the
  replay-fixture generator.
- **Extracted vendor blobs** from the official brscan5 deb package — notably
  the proprietary driver binary `libLxBsScanCoreApi.so.3.2.6`
  (disassembly source).
- **Test-matrix run logs and kernel-log baselines** documenting the hardware
  sessions.
