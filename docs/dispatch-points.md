<!-- Copyright 2025 excellentplans. SPDX-License-Identifier: GPL-2.0-or-later -->
# brscan — Dispatch points for the DS-640 (brscan5) port

Status: build baseline of 2026-09-20, commit `f30e923` (endpoint patch
0x85→0x83) on aarch64. All line references refer to that state.

The backend is built via `BRSANESUFFIX` (CMake: `BRSANESUFFIX=2` → brscan4 path,
`BRSANESUFFIX=1` → brscan3 path). CMake sets `BRSANESUFFIX=2`.

---

## 1. Model/INI tables (USB IDs → models)

Two parallel tables:

| File | Function | Line | Description |
|---|---|---|---|
| `data/Brsane.ini` | — | — | Main table. Per-line format in `[Support Model]`: `pid,seriesNo,modelType,"Name"` (e.g. `0x02d0,14,2,"DCP-1510"`). Plus `[ModelTypeName]` (`1=MFC Scanner`, `2=DCP Scanner`) and `[Driver]`. **The DS-640 most likely uses the same `0x02d0` entry (DCP-1510).** |
| `libsane-brother/brother_modelinf.c` | `init_model_info()` | 126 | Reads `BROTHER_SANE_DIR + INIFILE_NAME` (`brother.h:295` = `/usr/share/sane/brother/`). For SUFFIX 2, `INIFILE_NAME="Brsane2.ini"` (`brother_modelinf.h:52`), mapped to `Brsane.ini` via symlink at install time. Parsing: `ReadModelInfoSize2`/`ReadModelInfo2`, fields via `GetHexInfo` (productID) :198, `GetDecInfo` (seriesNo) :202, `GetModelNo` (modelType) :204, `[ModelTypeName]` lookup :208. |
| `libsane-brother/brother_modelinf.c` | `get_model_info()` | 409 | Builds a linked `MODELINF` list (index, vendorID, productID, seriesNo, modelName, modelTypeName). |
| `libsane-brother/brother_advini.c` | `scan_model_directory()` / `get_model_structure()` / `get_model_info_from_ini_by_product_id()` | 199 / 152 / 228 | Newer "advini" table with **extended format** incl. `r_endpoint, w_endpoint, colmatchDL, colmatchTBL, graylevelTBL` (`parse_and_add_model_info` :89). Sources: `MAININIFILE`/`MODELINIDIR` = `CONFDIR "Brsane2.ini"` / `CONFDIR "models2"` (`brother_advini.h:112-114`). |
| `libsane-brother/brother.c` | `sane_init()` | 373 | USB scan: `usb_find_busses/devices` :406-407, match `idVendor/idProduct` against the model list :442-443, `RegisterSaneDev` :447 (netdev path :455-470). |

**USB endpoints** come from the advini table:
`get_p_model_info_by_index(index)->w_endpoint / r_endpoint`
(`brother.c:573-576`, `brother_scanner.c:257-260`).

## 2. seriesNo / model-type evaluation, brscan3-vs-brscan4 switches

| Constant | File:line | Meaning |
|---|---|---|
| `MUST_CONVERT_MODEL 10` | `brother_modelinf.h:92` | **The central switch.** |
| `ChangeEndpoint[]` | `brother_modelinf.c:869` | List of series with an alternative endpoint (`{AL_FB_DCP, AL_DUPLEX, L4CFB, GENERIC_YCBCR_MODEL_2, _NOADF_2, _NOFB_2, _NOADF}`). Comment: `DCP-1510 = series 14`. |
| `ANOTHERENDPOINT 7` | `brother_modelinf.h` | Length of the list above. |
| `GENERIC_YCBCR_NOADF 14` | `brother_modelinf.h:81` | SeriesNo of the DCP-1510 family. The DS-640's ini entry uses seriesNo 5, which only feeds the legacy default feature tables — brscan5 identity is keyed by USB product ID (brscan5 profile table), not seriesNo. |
| `BHMINI_FB_ONLY 10` | `brother_modelinf.h:128` (SUFFIX 1 only) | brscan3 series used in the DCP-1510 workaround. |

**brscan4-vs-brscan3 switch in the data path** (`brother_scanner.c:1007-1012`,
inside the brscan4 `PageScan` :909):

```c
#if BRSANESUFFIX == 2
    if (this->modelInf.seriesNo >= MUST_CONVERT_MODEL)
        rc = brscan4_read_next_record_from_device( this, lpReadBuf, nReadSize );
    else
#endif
        rc = ReadNonFixedData( this->hScanner, lpReadBuf, nReadSize, READ_TIMEOUT, this->modelInf.seriesNo );
```

→ For seriesNo 14 (DCP-1510 family) the brscan4 record-read path runs.
The DS-640's ini entry is seriesNo 5 (< 10) and it is dispatched to the
brscan5 layer before this path anyway (brscan5 profile by USB product
ID), so this switch never applies to it.

Further seriesNo locations:
- `sane_open` sets `this->modelInf.seriesNo = pdev->modelInf.seriesNo` (`brother.c:616`);
  SUFFIX-1 workaround `seriesNo==14 → BHMINI_FB_ONLY` :617-621.
- `GetSeriesNo()` `brother_modelinf.c:530` (validation, < MAX_SERIES_NO).
- Per-series feature lookup (scan source/reso/CM files) in `brother_modelinf.c`
  (`GetSupportScanSrc` :676/:1004, `GetGrayLebelName` :800/:1120, `GetColorMatchName` :839/:1196).

## 3. USB read/write primitives (`brother_devaccs.c`)

| Function | Line | Signature |
|---|---|---|
| `OpenDevice` | 301-302 | `int OpenDevice(usb_dev_handle *hScanner, int seriesNo)` |
| `CloseDevice` | 501-502 | `void CloseDevice(usb_dev_handle *hScanner)` |
| `ReadDeviceData` | 575 | `int ReadDeviceData(usb_dev_handle *hScanner, LPSTR lpRxBuffer, int nReadSize, int seriesNo)` |
| `ReadNonFixedData` | 708 | `int ReadNonFixedData(usb_dev_handle *hScanner, LPSTR lpBuffer, WORD wReadSize, DWORD dwTimeOutMsec, int seriesNo)` |
| `WriteDeviceData` | 913-914 | `int WriteDeviceData(usb_dev_handle *hScanner, LPSTR lpTxBuffer, int nWriteSize, int seriesNo)` |
| `WriteDeviceCommand` | 992-993 | `int WriteDeviceCommand(usb_dev_handle*, LPSTR, int, int)` |
| `usb_set_configuration_or_reset_toggle` | 1091-1092 | `int usb_set_configuration_or_reset_toggle(Brother_Scanner *this, int configuration)` — this is where the `in_ep` patch was (line 1116). |

Low level: `brscan_io_replay_or_usb_bulk_read()` (`brother_devaccs.c:160`, capture/replay shim
around `usb_bulk_read`), `usb_control_msg` in OpenDevice/CloseDevice.

**brscan4 framing layer** (`brother_brscan4.c` / `brother_brscan4.h`):
`brscan4_read_next_record()` :124, `brscan4_cache_read()` :81, `brscan4_cache_reset()` :76,
`brscan4_is_boundary_status()` :5, `brscan4_record_length()` :16.
Binding to USB: `brscan4_device_read()` (`brother_scanner.c:800`, calls `ReadNonFixedData`)
and `brscan4_read_next_record_from_device()` :810.

## 4. sane_start / sane_read / sane_cancel + ops dispatch

| SANE API | Function (in `brother.c`, renamed to `sane_brother_*` via the `ENTRY()` macro from `include/sane/sanei_backend.h`) | Line | Key call |
|---|---|---|---|
| init | `sane_init` | 373 | `init_model_info()` :409, USB scan |
| get_devices | `sane_get_devices` | 512 | — |
| open | `sane_open` | 538 | `OpenDevice` :600, `QueryDeviceInfo` :644, `LoadColorMatchDll` :668, `LoadScanDecDll` :673, `InitOptions` :680 |
| close | `sane_close` | 686 | ScanEnd/CloseDevice |
| control_option | `sane_control_option` | 757 | — |
| get_parameters | `sane_get_parameters` | 904 | — |
| **start** | `sane_start` | 963 | `SetupInternalParameters` :970 → `ScanStart` :974 |
| **read** | `sane_read` | 982 | `PageScan` :995 (brscan4 variant `brother_scanner.c:909`) |
| **cancel** | `sane_cancel` | 1009 | `AbortPageScan` |
| set_io_mode / get_select_fd | — | — | present as stubs, not implemented in brother.c |

**Ops dispatch / frontend contract:** the backend exports **no**
`SANE_SaneBrother` ops struct, but flat `sane_*` symbols:
- `brother.c` defines the functions under the names `sane_*`; the bundled
  `include/sane/sanei_backend.h:49-61` (`ENTRY(name)` = `sane_BACKEND_NAME_name`,
  see `include/sane/sanei_debug.h:26`) renames them to `sane_brother_*` in the object.
- `libsane-brother/stubs.c` (compiled without `BACKEND_NAME`) provides the external
  `sane_init/sane_open/...` symbols that the SANE frontend loads.

**For a brscan5 dispatch**, the clean insertion point would be:
`stubs.c` (frontend symbols) or a new `sane_brscan5_*` name family via
`ENTRY()` — or the start of `sane_open` in `brother.c:538`, where a per-
`pdev->modelInf` switch to brscan3/4/5 handlers can be built in.

## 5. JPEG passthrough route (brscan4)

**Yes.** `PageScanColor()` in `brother_scanner.c:639` (comment :617) is the
JPEG passthrough for brscan4 models: it collects the continuous JPEG stream
(block wrapper: `[hdr < 0x80][len_lo][len_hi][payload]`, status bytes ≥ 0x80
end the stream) into a temp file and decodes it scanline-wise via libjpeg
into the SANE buffer.

Helper module `brother_color.c`/`brother_color.h`:
`brother_color_begin_page()` :71, `brother_color_append_payload()` :97,
`brother_color_begin_decode()` :110, `brother_color_read_scanlines()` :169,
`brother_color_phase()` :54, `brother_color_cleanup()`.

> **Caution:** `PageScanColor` is currently **not called anywhere** — the active
> brscan4 color path is the block-record logic directly in `PageScan` at
> `brother_scanner.c:1087-1118` (wrapper/status byte parsing inline) plus
> `brscan4_process_color_direct()` :840. The JPEG path via `PageScanColor` is
> the intended but not yet wired insertion point for the DS-640 port
> (decision: YCbCr-direct vs. JPEG passthrough).

## brscan5 dispatch (T3 state)

| Module | `libsane-brother/brother_brscan5.c` + `.h` (parallel, NEW) |
|---|---|
| Dispatch gate | **exactly one**, in `brother.c` `sane_open` (before `OpenDevice`): a model with a brscan5 device profile (`brscan5_is_brscan5`, keyed by USB vendor+product ID; originally T3's `seriesNo == 5`) → `&brscan5_ops_dispatch`, else → `&brscan5_ops_legacy` (static table, byte-/sequence-identical behavior to the previous 3/4 code) |
| Ops struct | `struct brscan5_ops` in `brother_brscan5.h`: `open/start/read/cancel/close`; global dispatch handle `brscan5_ops_dispatch` in `brother.c` |
| Start stub | `brscan5_start` performs Q→QDI→CKD→SSP→XSC (EP 0x04 OUT / 0x83 IN); CKD `00 02` resp. XSC `90 00` → `SANE_STATUS_NO_DOCS` |
| read/cancel | placeholders (T5-T7) |
| own USB wrappers | `brscan5_usb_read/write` with fixed endpoints 0x83/0x04, because `ReadNonFixedData`/`WriteDeviceData` choose the bulk endpoints based on seriesNo (series 5 ∉ `ChangeEndpoint[]` → 0x84/0x03, wrong). No brscan4 framing in the primitives, but a wrong EP policy. |

**seriesNo=5 semantics (checked and decided):**

- `MUST_CONVERT_MODEL == 10` (`brother_modelinf.h:92`); the only seriesNo switch in the data path is
  `seriesNo >= MUST_CONVERT_MODEL` (`brother_scanner.c:1008`) → brscan4 record path. **5 < 10, i.e.
  series 5 does NOT trigger the brscan4 path** — exactly as desired, the DS-640 never runs through the 3/4 pipeline
  (it is dispatched to the brscan5 ops before that).
- `GetSeriesNo()` accepts 1..`MAX_SERIES_NO` (19) → 5 is valid.
- Feature lookup for series 5 (= `ALL_SF_TYPE`, among others the MFC-8220 family): ADF-only, B/W/ErrDif/Gray,
  100–1200 dpi — fits a document scanner; T5-T7 can refine `modelConfig` for the DS-640.
- series 5 ∉ `ChangeEndpoint[]` → see the USB wrappers above.
- **Caution SUFFIX==1:** there, seriesNo 5 means `ZL2_SF_TYPE` (existing brscan3 models); the gate is
  therefore compiled to the legacy table for `BRSANESUFFIX != 2`.

## Summary for the port

| Most important switch | `MUST_CONVERT_MODEL` (`brother_modelinf.h:92`), queried at `brother_scanner.c:1008` |
|---|---|
| DS-640 model entry | `0x0468,5,2,"DS-640"` in `data/Brsane.ini` (the seriesNo feeds only the legacy default feature tables; brscan5 identity is keyed by USB product ID via the brscan5 profile table) |
| Endpoint switch | `ChangeEndpoint[]` (`brother_modelinf.c:869`), used in `OpenDevice` :348-351, `ReadDeviceData` :587-592, `in_ep` fallback `usb_set_configuration_or_reset_toggle` :1116 |
| USB layer | `brother_devaccs.c` (`ReadNonFixedData` :708 / `WriteDeviceCommand` :993) — replaced for brscan5 by fixed EPs in `brother_brscan5.c` |
| Framing | `brother_brscan4.c` (`brscan4_read_next_record` :124) — for brscan5 URB-boundary-driven (`brscan5-protocol.md` §4) |
| Scan control | `brother.c` `sane_start` / `sane_read` / `sane_cancel` → via `this->ops`; scan core `brother_scanner.c` `ScanStart` :221 / `PageScan` :909 (SUFFIX 2) |
| Color/JPEG hook | `PageScanColor` `brother_scanner.c:639` (unwired) |
