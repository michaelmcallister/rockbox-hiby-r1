# rockbox-hiby-r1

Patch-maintenance repo for **HiBy R1** Rockbox builds.

This repository does **not** vendor the full Rockbox source tree. Instead, it builds a HiBy R1 Rockbox release by:

1. fetching upstream Rockbox at a pinned commit  
2. copying in extra source files from `overlay/`  
3. applying the patch series from `patches/`  
4. building release artifacts for the HiBy R1

## What it adds

This patch set enables **Bluetooth audio support** for Rockbox on the **HiBy R1**, including:

- a **Bluetooth menu** in Rockbox
- device **scan, pair, connect, disconnect, and status**
- a **Codec** menu (Auto / SBC / AAC / aptX / aptX-HD / LDAC), persisted
- playback routing to **BlueALSA A2DP**, with working A2DP audio
- fallback to **local audio output** on disconnect/failure
- **absolute volume** updates from Rockbox to the Bluetooth stack
- Bluetooth preserved across the **bootloader → Rockbox** handoff

It also includes a small **USB/ADB** patch so ADB is handled through the existing USB mode path.

### Bluetooth codec notes

On connect, Rockbox starts BlueALSA offering only **SBC + AAC**. This is
deliberate: on the R1, BlueALSA's **LDAC** encoder negotiates a 96 kHz A2DP
transport that the device cannot open for playback, which presents as
"connected, but no audio." Restricting the offered codecs means the sink
negotiates **AAC** (48 kHz), which plays cleanly. LDAC remains visible in the
Codec menu but is not offered by the daemon on this hardware.

The audio data path uses a dedicated poll thread on a fixed interval,
because BlueALSA does not implement ALSA async (SIGIO) handlers. (A
poll-descriptor-driven variant was tried but regressed playback on this
hardware, so the simple fixed-interval pump is used.)

### Known issues

- **Plugging in USB stops Bluetooth playback.** USB insertion triggers the
  HiBy USB-gadget mode switch (per the `usb mode` setting, e.g. `adb`), which
  reconfigures the gadget and interrupts audio. Charging while playing is not
  currently supported; unplug and resume playback.

## Milkdrop visualiser

`overlay/apps/plugins/milkdrop.c` is a from-scratch, GPU-less Milkdrop-*style*
music visualiser plugin that **parses real `.milk` presets**. It ships a small
NS-EEL expression VM (Milkdrop's `per_frame`/`per_pixel` equation language) plus
self-contained math (no libm), evaluates `per_pixel` over a warp mesh, and
drives a fixed-point software feedback renderer: affine + time-animated
sinusoidal warp, video echo, `nWaveMode` waveforms, and preset crossfades. It
reads presets from `/.rockbox/milkdrop/` — see [`presets/`](presets/). Registered
via `patches/0005`.

Building on an Apple-Silicon host: the MIPS cross-toolchain can't build natively
on macOS (case-insensitive FS + 2012-era autotools that don't know aarch64), so
use `scripts/docker-build.sh` (ubuntu-22.04 container; `patches/0006` refreshes
the affected `config.guess`/`config.sub` for arm64 build hosts).

## Repository layout

- `patches/` — patch series applied on top of upstream Rockbox
- `overlay/` — new files not present upstream (incl. the milkdrop plugin)
- `presets/` — demo `.milk` presets for the milkdrop plugin
- `scripts/build.sh` — local reproducible build
- `scripts/docker-build.sh` — containerised build (for non-Linux hosts)
- `docker/` — build image matching the CI runner
- `.github/workflows/` — CI and release automation
- `baseline/` — provenance for the captured on-device baseline

## Build outputs

A successful build produces:

- `rockbox.r1` — Rockbox firmware
- `bootloader.r1` — bootloader
- `r1_rb.upt` — flashable update package, when a base `r1.upt` is provided
- `rockbox-info.txt` — Rockbox build info
- `build-metadata.txt` — build provenance
- `SHA256SUMS` — checksums

## Build locally

```bash
./scripts/build.sh
```

Artifacts are written to `out/`.

To also build a flashable `.upt` package:

```bash
R1_UPDATE_PATH=/path/to/r1.upt ./scripts/build.sh
```

Or:

```bash
R1_UPDATE_URL='https://example.invalid/r1.upt' \
R1_UPDATE_SHA256='<sha256>' \
./scripts/build.sh
```

## Install

### Update an existing Rockbox install
If Rockbox is already installed, replace:

```text
/usr/data/mnt/sd_0/.rockbox/rockbox.r1
```

with the built `rockbox.r1`, then reboot into Rockbox.

### Install a packaged release
If a release includes `r1_rb.upt`, use that through the normal HiBy R1 local update flow.

For most users, `r1_rb.upt` is the simplest way to install a full packaged build.

## Releases

GitHub Actions builds release artifacts automatically for version tags:

```bash
git tag v0.1.0
git push origin v0.1.0
```

## Known Issues

- **Plugging in USB stops Bluetooth playback** (USB-gadget mode switch); see
  *Bluetooth codec notes → Known issues* above.
- **LDAC** is not usable on this hardware (BlueALSA's LDAC encoder yields a
  96 kHz transport that won't open); AAC is used instead.
- Bluetooth audio (A2DP, AAC) works; report any pairing/connect edge cases.
