# brscan: Open-source Brother scanner driver

A fully open-source SANE backend for Brother MFC/DCP scanners, tested on Raspberry Pi and NanoPi NEO (ARM). Produces pixel-perfect scans on the Brother DCP-1510 with no proprietary binary blobs.

## About this fork

I forked because I wanted to get my DS-640 with the newer brscan5 protocol to run on ARM (Raspberry Pi 4). The driver is vibe-coded with GLM-5.3-Flash, so everybody should use and redistribute with that in mind. I cleaned up and tested the driver, but could physically only test on the DS-640/Raspberry Pi combo.

The source code originates from the Brother [download page](http://www.brother.com/cgi-bin/agreement/agreement.cgi?dlfile=http://www.brother.com/pub/bsc/linux/dlf/brscan3-src-0.2.11-5.tar.gz&lang=English_source) and has been cleaned and extended.

## Provenance

The code has passed through several hands:

- Brother Industries released the original brscan3 source under the Brother license preserved in `copying.brother` (though it shipped two proprietary binary blobs with no source).
- [neicker/brscan](https://github.com/neicker/brscan) is Norbert Eicker's initial open-source packaging and cleanup.
- [dmikushin/brscan](https://github.com/dmikushin/brscan) is Dmitry Mikushin's reverse engineering of the binary blobs (`libbrscandec`, `libbrcolm`) and the brscan4 protocol. See "What was done" below.
- [excellentplans/brscan](https://github.com/excellentplans/brscan) is this fork: the brscan5/DS-640 support described above.

## What was done

The original Brother distribution shipped two proprietary x86-64 binary blobs (`libbrscandec.so` and `libbrcolm.so`) with no source code. Both were reverse-engineered and replaced with open-source C implementations:

- libbrcolm (color matching), written from scratch by analyzing the original blob's disassembly. It implements 3D trilinear LUT interpolation with optional gamma pre-correction. Verified byte-for-byte against the original blob (21/21 tests).

- libbrscandec (scan decoder and resolution changer), fixed from Ghidra decompilation by verifying every exported function against the original disassembly. It fixes six classes of Ghidra decompilation errors. Verified byte-for-byte (6/6 tests across same-reso, upscale, downscale, B&W, and color modes).

Dmitry Mikushin also reverse-engineered the DCP-1510 scan protocol (brscan4) from `libsane-brother4.so.1.0.7`:

- Mono packbits wire format: `[10-byte wrapper][2-byte LE length][packbits data]`, with the per-block 1-byte status code (`0x80` Page End, `0x81` NextPage, `0x83`/`0xE3` Cancel) inline between frames.
- 24-bit color is a single baseline JPEG stream framed into per-block wrappers. The driver stages the payload to a temp file and decodes it via libjpeg with `out_color_space = JCS_RGB`.
- EOF detection propagates the in-stream `0x80` byte to `ProcessMain`'s `GetStatusCode` path so `SCAN_EOF` is raised properly (replacing the prior heuristic that lost trailing scanlines).
- USB session teardown mirrors the reference `CloseDevice` exactly: `BREQ_GET_CLOSE` then `usb_release_interface(1)`, with no stray `usb_set_altinterface(0)` between them. This eliminates the firmware-side `BCOMMAND_RETURN=0x80` that previously required a power cycle between scans.
- USB endpoint mapping (EP 0x85 IN / EP 0x04 OUT) and I-command response parsing with variable-length headers.

## Supported models

Tested on Brother DCP-1510 and Brother DS-640. Should work on other Brother MFC/DCP models listed in `data/Brsane.ini`. Models with `seriesNo >= 10` use the brscan4 protocol with the new line framing format; the DS-640 (`0x0468`) goes to the brscan5 protocol layer below.

## brscan5 protocol (DS-640)

The DS-640 portable scanner (`04f9:0468`) speaks the newer brscan5 protocol, reverse-engineered from usbmon captures and the vendor's proprietary `libLxBsScanCoreApi.so`. It is implemented natively in `libsane-brother/brother_brscan5.c` (session handshake, SSP/XSC encoders, URB-boundary framing, JPEG and RLENGTH data paths), tested on aarch64 (Raspberry Pi 4).

- USB endpoints: the DS-640 uses EP 0x83 IN / EP 0x04 OUT. brscan4 models use EP 0x85 IN instead.
- Replay-based testing: `BROTHER5_REPLAY=<tlv>` drives the full SANE lifecycle against recorded protocol fixtures in `tests/data/brscan5/`. The ctest battery (unit, end-to-end, fault injection) needs no hardware.
- Protocol details: [docs/brscan5-protocol.md](docs/brscan5-protocol.md) (wire-level reference) and [docs/dispatch-points.md](docs/dispatch-points.md) (code map of the dispatch and data path).

## Prerequisites

```
sudo apt install git libsane-dev sane-utils libusb-dev libjpeg-dev pkg-config cmake gcc
```

Get the source:

```
git clone https://github.com/excellentplans/brscan.git
cd brscan
```

## Building and installing

```
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make -j4
sudo make install
sudo sh -c "echo brother >> /etc/sane.d/dll.conf"
```

Pre-built binaries for amd64, arm64, and armv7 are published as GitHub releases. Each release tarball contains an `INSTALL.txt` with copy-paste install commands, so you can skip building from source entirely. Releases are cut automatically by pushing a `v*` tag. See [Releases](https://github.com/excellentplans/brscan/releases).

## USB permissions

Add a udev rule for your scanner (use the appropriate product ID):

```
lsusb | grep Brother
# Bus 004 Device 009: ID 04f9:02d0 Brother Industries, Ltd DCP-1510
```

```
sudo tee /etc/udev/rules.d/60-brother-scanner.rules << 'EOF'
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTRS{idVendor}=="04f9", ATTRS{idProduct}=="02d0", GROUP="lp", ENV{libsane_matched}="yes", MODE="0666"
EOF
sudo udevadm control --reload-rules
```

## Troubleshooting

- `scanimage -L` finds nothing after adding the udev rule? Unplug and replug the scanner, or run `sudo udevadm trigger`. Existing devices only pick up new rules when re-attached.
- Scanner is listed but scanning fails with "permission denied"? The udev rule didn't match. Recheck `idVendor`/`idProduct` against your `lsusb` output.
- A previously installed official Brother driver package (brscan4/brscan5 `.deb`) can shadow this backend in `/usr/lib/sane`. Uninstall it before installing this one.

## Scanning

Verify the scanner is detected:

```
scanimage -L
# device `brother:bus4;dev1' is a Brother DCP-1510 USB scanner
```

Scan a full A4 page in grayscale:

```
scanimage --mode "True Gray" --resolution 200 -x 210 -y 297 --format=pnm > scan.pnm
```

Scan a full A4 page in 24-bit color:

```
scanimage --mode "24bit Color" --resolution 200 -x 210 -y 297 --format=pnm > color.pnm
```

Available scan modes: `Black & White`, `Gray[Error Diffusion]`, `True Gray`, `24bit Color`

## Tests

Tests cover brscan4 frame structure, packbits decompression (including ARM `signed char` edge cases), ScanDecOpen parameter computation, the full decode pipeline, color matching, and end-to-end JPEG color decode. The brscan5 layer adds protocol-encoder unit tests, RLENGTH/record stream tests, and full replay-driven SANE lifecycle tests with fault injection (`tests/test_brscan5_*.c`), all hardware-free via `BROTHER5_REPLAY` fixtures in `tests/data/brscan5/`. Run everything with `ctest` from the build directory.

## Debug logging

Set `SANE_DEBUG_BROTHER=<level>` to enable the SANE debug channel
(stderr, `[brother]` prefix; output when `level <= debug level`):

```
SANE_DEBUG_BROTHER=30 scanimage --mode "True Gray" --resolution 200 -x 210 -y 297 > scan.pnm
```

Levels: `DBG(1)` errors/faults, `DBG(3)` state transitions, `DBG(5)`
per-action traces (replay transport write/read/control/drain, USB
drain/cancel). The legacy brscan3/4 code paths log via `WriteLog()`
(legacy stderr logging, see brother_log.c).

## Architecture

```
libsane-brother.so.1    SANE backend (talks USB, parses scan protocol)
  ├── libbrscandec.so.1  Scan decoder: packbits decompression, resolution scaling
  ├── libbrcolm.so.1     Color matching: 3D LUT interpolation from .dat/.cm files
  └── libjpeg            24-bit color decode (JPEG-over-USB for brscan4)
```

## License

GPL v2 or later, see [Copying](Copying). The original Brother license terms are preserved in [copying.brother](copying.brother), and [copying.lib](copying.lib) (LGPL 2.1) covers the library sources inherited from the original Brother distribution.
