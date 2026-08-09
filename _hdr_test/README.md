# OpenRV HDR test kit

## Docs

**Full write-up** (pipeline, GPU interop, present modes, OCIO, 203-nit SDR white,
env vars, pitfalls, A/B recipes):

→ **[HDR-WAYLAND.md](./HDR-WAYLAND.md)**

## Quick start

```bash
export RV_HDR=1 RV_ALLOW_WAYLAND=1 QT_QPA_PLATFORM=wayland
export LD_LIBRARY_PATH="/home/alex/github/openrv/_build/stage/app/lib:${LD_LIBRARY_PATH:-}"
export OCIO='ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5'   # optional

# A — PQ + Wayland SDR-white scale
export RV_HDR_PRESENT=pq RV_HDR_PQ_SDR_SCALE=1

# B — linear Display-P3 from PQ buffer
# export RV_HDR_PRESENT=p3linear

# C — Display P3 Extended (macOS EDR): float FBO + sRGB EOTF → linear P3
# export RV_HDR_PRESENT=p3extended
# (OCIO: Display P3 view, not Rec.2100-PQ)

# GPU interop is ON by default (no full-frame readback).
# Force CPU path: export RV_HDR_GL_VK_INTEROP=0

/home/alex/github/openrv/_build/stage/app/bin/rv \
  /home/alex/github/openrv/_hdr_test/hdr_wedge_1080.exr
```

Expect: `INFO: GL↔Vulkan shared image … (GPU interop, no CPU readback)`.

With OCIO: enable **display Active** in the menu. PQ views for A/B; **Display P3** for C.

## Wedge

`hdr_wedge_1080.exr` — 1920×1080, left 1.0 / center 10.0 / right 4.0 (stock
`RV_HDR` maps 1.0 → 100 nits). Rebuild: `./write_hdr_exr` (see `write_hdr_exr.cpp`).

## Probes

`RV_GL_PROBE=1` logs FBO L/C/R. For unscaled stock PQ: `LCR8≈130,192,166`
(100 / 1000 / 400 nits).

## Screenshots

`grim` tone-maps HDR; panel is ground truth for absolute brightness.
`rv_interop_fix.png` — GPU interop smoke (ACES sample) after dedicated-import fix.
