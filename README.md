# rockbox-hiby-r1

Patch-only repository for HiBy R1 Rockbox builds.

This repo does not vendor Rockbox source. CI pulls upstream Rockbox at a pinned base commit, applies the patch series in `patches/`, then builds:

- `rockbox.r1` (firmware)
- `bootloader.r1` (bootloader)
- `r1_rb.upt` (flashable update package, when a base `r1.upt` is configured)

Standalone source files that do not exist upstream live in `overlay/`. The build
copies `overlay/` into the upstream checkout before applying the patch series, so
`patches/` stays limited to edits against existing upstream files.

## Current Build

- Pinned upstream base commit: `9a6e3799e18ba1e365cb23f6a2a5044e49cecffc`
- Reviewable patch series (applied in filename order):
  - `0001-hibyr1-add-bluetooth-UI-menu-integration.patch`
  - `0002-hibyr1-add-bluealsa-pcm-backend-and-routing-hooks.patch`
  - `0003-hibyr1-use-usb-mode-setting-for-adb.patch`
  - `0004-hibyr1-keep-bluetooth-active-across-bootloader-handoff.patch`

## Baseline (currently running device build)

- Running binary SHA-256: `ada98d8ac6f6ddef7845619e1e5189e829ba92fb1003031ffa0c609d1567ca17`
- Embedded signature: `dd21a1d1d9M-260307`
- Upstream base commit: `dd21a1d1d9ae4f314eb16d4813414ee72e2aa0de`
- Reconstructed baseline source commit (single-commit reconstruction): `6af68b9dc7373a02d1c55d169780f35f01d4c011`

See `baseline/BASELINE.md` for provenance.

## Local build

```bash
./scripts/build.sh
```

Artifacts are written to `out/`.

To also produce a flashable `.upt`, provide an official base `r1.upt`:

```bash
R1_UPDATE_PATH=/path/to/r1.upt ./scripts/build.sh
```

Or use the direct public download URL plus checksum:

```bash
R1_UPDATE_URL='https://drive.usercontent.google.com/download?id=1cvZdQeJsb2qYu2qv6sZaYJaP8vqW1LqG&export=download' \
R1_UPDATE_SHA256='9aada81995d8d2b2ed80d6cf292c62bc5f0f705e51e4f69c7e766ee67536ba60' \
./scripts/build.sh
```

## GitHub release build

Push a version tag to create a GitHub Release with `rockbox.r1`, `bootloader.r1`,
`r1_rb.upt`, checksums, and build metadata attached.

```bash
git tag v0.1.0
git push origin v0.1.0
```

The release workflow rebuilds from the pinned upstream base plus `patches/`,
so the release assets are tied to the tagged commit in this repo.
