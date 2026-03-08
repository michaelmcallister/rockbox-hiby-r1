# rockbox-hiby-r1

Patch-only repository for HiBy R1 Rockbox builds.

This repo does not vendor Rockbox source. CI pulls upstream Rockbox at a pinned base commit, applies the patch series in `patches/`, then builds:

- `rockbox.r1` (firmware)
- `bootloader.r1` (bootloader)

## Baseline (currently running device build)

- Running binary SHA-256: `ada98d8ac6f6ddef7845619e1e5189e829ba92fb1003031ffa0c609d1567ca17`
- Embedded signature: `dd21a1d1d9M-260307`
- Upstream base commit: `dd21a1d1d9ae4f314eb16d4813414ee72e2aa0de`
- Reconstructed baseline source commit (single-commit reconstruction): `6af68b9dc7373a02d1c55d169780f35f01d4c011`
- Maintained reviewable patch series (applied in filename order):
  - `0001-hibyr1-add-bluetooth-UI-menu-integration.patch`
  - `0002-hibyr1-add-bluealsa-pcm-backend-and-routing-hooks.patch`
  - `0003-hibyr1-keep-usb-adb-active-and-add-hosted-runtime-gl.patch`
  - `0004-hosted-add-devinput-trace-hooks-for-on-device-debugg.patch`
  - `0005-hibyr1-preserve-EC2-baseline-auxiliary-source-snapsh.patch`

See `baseline/BASELINE.md` for provenance.

## Local build

```bash
./scripts/build.sh
```

Artifacts are written to `out/`.
