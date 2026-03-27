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
- playback routing to **BlueALSA A2DP**
- fallback to **local audio output** on disconnect/failure
- **absolute volume** updates from Rockbox to the Bluetooth stack
- Bluetooth preserved across the **bootloader → Rockbox** handoff

It also includes a small **USB/ADB** patch so ADB is handled through the existing USB mode path.

## Repository layout

- `patches/` — patch series applied on top of upstream Rockbox
- `overlay/` — new files not present upstream
- `scripts/build.sh` — local reproducible build
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

- Bluetooth connection setup currently forces the **SBC** codec.
- Bluetooth support is still **flaky** and needs further testing. Expect issues with pairing, connecting, or routing audio.
- **Music playback does work**, but the overall Bluetooth experience is not yet fully reliable.
