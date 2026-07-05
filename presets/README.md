# milkdrop presets

Demo `.milk` presets for the `milkdrop` visualiser plugin
(`overlay/apps/plugins/milkdrop.c`).

Deploy to the device with:

```sh
adb push presets/*.milk /data/mnt/sd_0/.rockbox/milkdrop/
```

The plugin reads `/.rockbox/milkdrop/*.milk` and cycles them (tap = next preset,
press-and-hold = quit, plus a 20 s auto-advance with crossfade). If the folder
is empty or missing it falls back to a built-in default preset.

## Demo presets (hand-authored)

- `aurora`, `driftcalm`, `pulse`, `vortex` — per-frame motion + colour cycling
- `swirl`, `tunnel2`, `fold`, `dali` — per-pixel mesh-warp showcases (swirl,
  pulsing tunnel, sinusoidal fold, liquid melt)

## Real preset packs

The plugin ships a from-scratch NS-EEL expression engine and parses standard
`.milk` files, so you can drop in real presets. The classic Winamp/Geiss
MilkDrop 1.x packs (GPL) are mirrored at e.g.
<https://github.com/clangen/projectM-musikcube> (`presets_milkdrop_104/`).

Caveats (no GPU on this device):
- MilkDrop **2** presets' HLSL `warp_`/`comp_` shaders are skipped — you get the
  motion/warp/waveform, not the shader-based colouring.
- Custom waves/shapes aren't rendered; presets that colour only via those are
  partially supported through a `q1/q2/q3` waveform-colour fallback.
