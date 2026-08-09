# OpenRV HDR on Wayland (Hyprland) — notes

Working notes for absolute HDR presentation of the **image plane** on
Wayland/Vulkan. This is branch/experimental work; not all of it is
upstream-ready.

## Goals

1. **Image plane** shows super-white (scene-linear \> 1.0 / ACES HDR OT peaks)
   as visibly brighter than reference white on an HDR OLED.
2. Prefer a single window (embedded present), not a separate tiled client.
3. Support both **stock RV** (`RV_HDR` + DisplayIPNode PQ) and **OCIO**
   display transforms (including ACES 2.0 built-ins).
4. A/B **PQ HDR10** vs **linear extended** present (P3 / scRGB).

Non-goals (for now): whole-UI PQ tagging, float EGL configs on NVIDIA Wayland,
perfect HUD color on the HDR surface.

---

## Pipeline overview

```
  EXR / media (float)
       │
       ▼
  IP graph  ──►  OCIODisplay  (optional: scene_linear → PQ or other view)
       │         or RVDisplayColor / DisplayIPNode (RV_HDR → SMPTE-2084)
       ▼
  GL FBO  (RGBA8 PQ codes, or RGBA16F for p3extended)
       │
       │   GPU interop (default): glBlit → GL↔Vk shared image (opaque FD)
       │   CPU fallback: glReadPixels / grabFramebuffer → upload
       ▼
  Vulkan present (QRhi swapchain)  ──►  Hyprland color management
       │
       │   RV_HDR_PRESENT selects surface + fragment convert:
       │     pq         → HDR10 ST.2084  (+ optional SDR-white scale)
       │     p3linear   → P3 extended linear  (PQ→linear, 1.0 = SDR white)
       │     srgblinear → scRGB extended linear (same convert from PQ)
       │     p3extended → P3 extended linear  (piecewise sRGB TF→linear P3;
       │                    macOS Display P3 Extended / EDR-style buffer)
       ▼
  HDR panel (e.g. Dell S3225QC, cm=hdr, 10-bit)
```

**Post-OCIO:** all present encodes/scales happen in the **Vulkan present
fragment shader**, after the FBO already holds OCIO/Display output. The IP
graph is not modified by `RV_HDR_PRESENT` / `RV_HDR_PQ_SDR_SCALE`.

### GL → Vulkan handoff (GPU interop)

The old bottleneck was full-frame **CPU readback** (`glReadPixels` /
`grabFramebuffer`) + upload into a QRhi texture — that capped playback around
~11 fps at large window sizes while the non-HDR GL path stayed locked at 24.

**Default path now keeps the frame on the GPU:**

1. Vulkan allocates an exportable `VkImage` (`VK_KHR_external_memory_fd`).
2. GL imports it via `GL_EXT_memory_object_fd` as a texture.
3. Each frame: `glBlitFramebuffer` from the present FBO → shared texture,
   `glFinish`, then the present pass samples the same image through
   `QRhiTexture::createFrom` (layout `GENERAL`). Serial sync is
   `glFinish` + `QRhi::finish()` (v1; can move to semaphores later).

Log line when active:

```
INFO: GL↔Vulkan shared image WxH RGBA8|RGBA16F (GPU interop, no CPU readback)
INFO: present GPU interop WxH swap=... shMode=...
```

Force the old CPU path for A/B: `RV_HDR_GL_VK_INTEROP=0`.

---

## Compositor setup (Hyprland)

Example (`~/.config/hypr/monitors.conf` style):

```conf
# Desktop in HDR so windowed clients can present HDR10 / linear HDR.
monitorv2 {
    ...
    bitdepth = 10
    cm = hdr
    sdrbrightness = 1.0
    sdr_min_luminance = 0.005
    sdr_max_luminance = 100   # local SDR “1.0”; media white for HDR match is 203 nits
}
```

Check live state:

```bash
hyprctl monitors -j | jq '.[] | {name, colorManagementPreset, currentFormat, sdrMaxLuminance, sdrBrightness}'
```

Expect something like: `colorManagementPreset: hdr`, `currentFormat: XBGR2101010`.

### The “203 nits” problem

**SDR reference / media white is 203 nits.** Absolute **PQ** codes for a **100-nit**
peak (typical ACES 100-nit container / `refWhite=100`) are ~0.51 (PQ), which looks
**dimmer than desktop UI white** if the compositor lights SDR chrome near 203 nits.

That is **not** an ACES OT bug; it is reference-white mismatch between:

| Signal | Meaning of “1.0” / peak |
|--------|-------------------------|
| ACES 100-nit PQ view | Peak ~100 nits absolute |
| Wayland SDR UI / scRGB-like ref | Often ~203 nits |
| PQ surface (HDR10) | Code 1.0 = 10 000 nits absolute |

**Mitigation A (PQ hack):** `RV_HDR_PQ_SDR_SCALE=1`  
`PQ → linear → ×(sdrWhite/refWhite) → PQ` in the present shader  
(default `sdrWhite` from swapchain or 203, `refWhite` default 100 → scale ≈ 2.03).

**Mitigation B (cleaner model, from PQ buffers):** `RV_HDR_PRESENT=p3linear`  
Present as **extended linear** with **1.0 = SDR/reference white**, converting
PQ codes → `nits / sdrWhite` in the present shader. Super-whites become \>1.0 on
the float HDR swapchain.

**Mitigation C (macOS EDR-style):** `RV_HDR_PRESENT=p3extended`  
Buffer is **Display P3 primaries + D65 + piecewise sRGB transfer** (possibly
extended \>1 in float). Present linearizes with the IEC sRGB EOTF (extended)
and hands off **linear P3** to Wayland. Matches transforms authored for
macOS Display P3 Extended / EDR more closely than PQ.

---

## Environment variables

| Variable | Default | Purpose |
|----------|---------|---------|
| `RV_HDR=1` | off | Enable HDR present path + (stock) DisplayIPNode PQ force |
| `RV_ALLOW_WAYLAND=1` | — | Allow native Wayland QPA |
| `QT_QPA_PLATFORM=wayland` | — | Force Wayland |
| `QT_WIDGETS_RHI_BACKEND=vulkan` | set by wrapper when HDR/Wayland | Vulkan RHI (needed: pure QOpenGLWidget is black on this NVIDIA stack) |
| **`RV_HDR_PRESENT`** | `pq` | Present surface mode: `pq` \| `p3linear` \| `srgblinear` \| `p3extended` |
| **`RV_HDR_PQ_SDR_SCALE=1`** | off | PQ path: × sdrWhite/refWhite in PQ domain |
| **`RV_HDR_LINEAR_SDR_MATCH`** | **on** for `p3extended` | After sRGB EOTF, × sdrWhite/refWhite so paper white ≈ UI white |
| **`RV_HDR_SDR_WHITE`** | swapchain `sdrWhiteLevel` as reported, else **203** | Compositor SDR white in nits (default/fallback **203**) |
| **`RV_HDR_PQ_REF_WHITE`** | **100** | Content reference white nits (ACES 100-nit containers / linear-1.0 calibration) |
| `RV_GL_PROBE=1` | off | Log FBO L/C/R 8-bit samples (PQ100≈130, PQ400≈164, PQ1000≈192) |
| `RV_HDR_TEST_PATTERN=1` | off | Synthetic PQ wedges in present (bypass GL) |
| **`RV_HDR_GL_VK_INTEROP=1`** | **off** | Opt-in GPU interop (GL blit → shared VkImage → GPU copy → present). Default is CPU readback (stable). |
| `OCIO` | unset | OCIO config URI or path (see OCIO section) |

### `RV_HDR_PRESENT` values

| Value | Aliases | QRhi swapchain | Expected FBO content | Fragment shader |
|-------|---------|----------------|----------------------|-----------------|
| `pq` | `hdr10`, `st2084` | `HDR10` (ST.2084) | PQ codes | optional mode 1: PQ→lin→×s→PQ |
| `p3linear` | `p3`, `linear-p3` | `HDRExtendedDisplayP3Linear` | PQ codes | mode 2: PQ→nits/`sdrWhite` → linear |
| `srgblinear` | `scrgb`, `srgb-linear` | `HDRExtendedSrgbLinear` | PQ codes | mode 2: same |
| **`p3extended`** | `displayp3-extended`, `edr-p3`, `p3-srgb` | `HDRExtendedDisplayP3Linear` | **P3 + piecewise sRGB TF** (macOS EDR) | mode 3: sRGB EOTF → linear P3 |

Restart RV after changing `RV_HDR_PRESENT` (surface colorspace + swapchain).

### Display P3 Extended (`p3extended`) — macOS EDR compatibility

Intended buffer (from OCIO **Display P3** display/view or a Mac-style OT):

| Property | Value |
|----------|--------|
| Primaries | Display P3 |
| White | D65 |
| Transfer | Piecewise sRGB (IEC 61966-2-1), **extended** for \|c\| \> 1 |
| Dynamic range | 1.0 ≈ reference white; \>1 = EDR headroom (needs float transfer) |

Present path (**float transfer required**):

1. GL renders into an **RGBA16F** FBO (not the 8-bit QOpenGLWidget FBO) so
   encoded values \>1.0 are kept.
2. `glReadPixels(..., GL_FLOAT)` → flip to top-left → upload **RGBA32F** present texture.
3. Fragment shader: per-channel **sRGB EOTF** (encoded → linear), not OETF:  
   `linear = c/12.92` if `|c|≤0.04045`, else `((|c|+0.055)/1.055)^2.4` (sign preserved).  
   Matches OCIO Display P3 Un-tone-mapped (e.g. code 1.825 → linear 4.0).
4. Output **linear** Display P3 to `HDRExtendedDisplayP3Linear`.  
   Surface must be tagged **linear** transfer (not `QColorSpace::DisplayP3`, which is
   gamma/sRGB-TF — that would double-decode).

**Do not** pair this with a Rec.2100-PQ OCIO view — that is mode `p3linear` / `pq`.  
Use OCIO **Display P3 - Display** (or equivalent gamma-encoded P3), not PQ.

Log markers when float path is live:

```
INFO: GL float present FBO RGBA16F WxH (p3extended EDR transfer)
INFO: present upload texture RGBA32F (float transfer for p3extended)
INFO: present upload FLOAT ... LCR_f=...
```

```bash
export RV_HDR=1 RV_ALLOW_WAYLAND=1 QT_QPA_PLATFORM=wayland
export RV_HDR_PRESENT=p3extended
export OCIO='ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5'
# OCIO menu: Display P3 - Display / appropriate view (not Rec.2100-PQ)
```

---

## A/B comparison recipes

Assume stage binary and an HDR monitor with `cm=hdr`.

### A — PQ + SDR-white scale (known-good look)

```bash
export OCIO='ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5'   # or ACES 1.3 URI
export RV_HDR=1 RV_ALLOW_WAYLAND=1 QT_QPA_PLATFORM=wayland
export RV_HDR_PRESENT=pq
export RV_HDR_PQ_SDR_SCALE=1
# optional: export RV_HDR_SDR_WHITE=203 RV_HDR_PQ_REF_WHITE=100

/home/alex/github/openrv/_build/stage/app/bin/rv /path/to/media
```

Enable OCIO **display** manually if needed (OCIO does not always auto-attach for
untagged EXRs). Use a **PQ** display/view (e.g. Rec.2100-PQ / 1000-nit or
100-nit limited).

Expect log:

```
INFO: VulkanPresentWindow HDR present ON mode=pq/HDR10 ...
INFO: VulkanPresentWindow swapchain format=HDR10 (PQ/ST.2084) ...
INFO: swapchain HDR info: ... sdrWhiteLevel=203 ...
INFO: post-OCIO PQ SDR-white scale ON (GPU present): PQ→linear→×2.03→PQ ...
```

### B — Linear Display-P3 (cleaner reference-white model)

```bash
export OCIO='ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5'
export RV_HDR=1 RV_ALLOW_WAYLAND=1 QT_QPA_PLATFORM=wayland
export RV_HDR_PRESENT=p3linear
# do NOT need RV_HDR_PQ_SDR_SCALE — 1.0 is sdrWhite by construction

/home/alex/github/openrv/_build/stage/app/bin/rv /path/to/media
```

Expect log:

```
INFO: VulkanPresentWindow HDR present ON mode=p3linear ...
INFO: ... swapchain format=HDRExtendedDisplayP3Linear ...
INFO: present shader mode=PQ→linear (1.0=203 nits SDR white) for linear HDR surface.
```

Same OCIO PQ view in both A and B; only the **present handoff** changes.

### C — Display P3 Extended (macOS EDR-style OT)

```bash
export OCIO='ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5'
export RV_HDR=1 RV_ALLOW_WAYLAND=1 QT_QPA_PLATFORM=wayland
export RV_HDR_PRESENT=p3extended

/home/alex/github/openrv/_build/stage/app/bin/rv /path/to/media
```

OCIO **Display P3** (sRGB TF / gamma-encoded), not Rec.2100-PQ. Expect:

```
INFO: ... mode=p3extended (DisplayP3 + sRGB TF → linear P3) ...
INFO: ... swapchain format=HDRExtendedDisplayP3Linear ...
INFO: present shader mode=DisplayP3 Extended (piecewise sRGB TF → linear P3) ...
```

### C — Stock wedge (no OCIO)

```bash
unset OCIO
export RV_HDR=1 RV_ALLOW_WAYLAND=1 QT_QPA_PLATFORM=wayland
export RV_GL_PROBE=1
# Compare:
#   RV_HDR_PRESENT=pq RV_HDR_PQ_SDR_SCALE=1
#   RV_HDR_PRESENT=p3linear

/home/alex/github/openrv/_build/stage/app/bin/rv \
  /home/alex/github/openrv/_hdr_test/hdr_wedge_1080.exr
```

DisplayIPNode maps scene-linear **1.0 → 100 nits → PQ** when `RV_HDR=1`.
Probes without present convert: `LCR8≈130,192,166` for 100/1000/400 nits.

---

## OCIO

### Version (this tree)

| | |
|--|--|
| VFX pin | **CY2025** (other deps unchanged) |
| OpenColorIO | **2.5.2** (bumped for ACES 2.0 built-ins; stock CY2025 was 2.4.2) |
| Install | `~/openrv-deps/RV_DEPS_OCIO` → staged under `_build/stage/app/lib` |

### Built-in configs (2.5.2)

ACES **1.3** (still available):

```bash
export OCIO='ocio://studio-config-v2.2.0_aces-v1.3_ocio-v2.4'
export OCIO='ocio://cg-config-v2.2.0_aces-v1.3_ocio-v2.4'
```

ACES **2.0**:

```bash
export OCIO='ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5'
export OCIO='ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5'
```

**Do not** append `.ocio` to `ocio://` URIs.

### Activation behaviour

- `$OCIO` only **loads** the config.
- **Display** stays on `RVDisplayColor` until you enable OCIO for the display
  (menu: OCIO Source Setup → device → **Active**), or until `OCIOFile` is
  installed on a source (e.g. colorspace in the filename) and the package
  auto-enables display OCIO.
- Default studio display is often **sRGB** — for HDR you must pick
  **Rec.2100-PQ** (or similar). On HDR10 present, SDR OT + PQ surface looks
  crushed/wrong.
- When `OCIODisplay` replaces `RVDisplayColor`, stock `RV_HDR` DisplayIPNode PQ
  force **no longer runs**; OCIO owns the encode. Present still tags HDR.

### Rebuild OCIO only (CY2025)

Pin: `cmake/defaults/CY2025.cmake` → `RV_DEPS_OCIO_VERSION=2.5.2`.

```bash
ninja -C _build clean-RV_DEPS_OCIO
# wipe install if needed: rm -rf ~/openrv-deps/RV_DEPS_OCIO/{build,install,src}
cmake -S . -B _build
ninja -C _build RV_DEPS_OCIO RV_DEPS_OCIO-stage-target OCIONodes rv -j$(nproc)
```

Notes from the 2.5.2 bring-up:

- Vendored yaml-cpp 0.8 needed `-include cstdint` on newer GCC (`ocio.cmake`).
- `libOpenColorIO` may need `libz-ng` DT_NEEDED (`ocio_patchelf_zng.cmake` +
  `patchelf` post-install) so PyOpenColorIO resolves `zng_*`.

---

## Implementation map

| Piece | Location |
|-------|----------|
| Vulkan present window / modes | `src/lib/app/RvCommon/VulkanPresentWidget.cpp` |
| Present shaders (GPU convert) | `src/lib/app/RvCommon/shaders/present.{vert,frag}` → `*.qsb` + `*_qsb.h` |
| Embed present into main UI | `RvDocument.cpp` (createWindowContainer subsurface) |
| GL grab → present | `GLView.cpp` (`grabFramebuffer`, no Bt2100Pq on GL when Vulkan present) |
| Stock PQ encode | `DisplayIPNode.cpp` (`RV_HDR=1` → SMPTE-2084, linear nits/100) |
| OCIO display node | `OCIOIPNode.cpp` / package `ocio_source_setup` |
| OCIO dep pin | `cmake/defaults/CY2025.cmake`, `cmake/dependencies/ocio.cmake` |
| Test wedge | `_hdr_test/write_hdr_exr.cpp`, `hdr_wedge_1080.exr` |

### Rebuild present shaders after editing GLSL

```bash
QSB=/usr/lib/qt6/bin/qsb
cd src/lib/app/RvCommon/shaders
$QSB --glsl "440,300 es" --hlsl 50 --msl 12 -o present.frag.qsb present.frag
$QSB --glsl "440,300 es" --hlsl 50 --msl 12 -o present.vert.qsb present.vert
# regenerate present_*_qsb.h byte arrays (see prior session script or xxd embed)
ninja -C _build RvCommon rv -j$(nproc)
```

Fragment `params` (std140 `vec4` after `mat4`):

| Component | Meaning |
|-----------|---------|
| `x` | PQ linear scale (mode 1), else ignored |
| `y` | mode: `0` pass, `1` PQ×scale, `2` PQ→linear, `3` sRGB-TF→linear P3 |
| `z` | SDR white nits (mode 2) |

---

## Test media

### Full-frame wedge (`hdr_wedge_1080.exr`)

1920×1080 scene-linear (stock `RV_HDR` mapping 1.0 = 100 nits):

| Region | Linear | Nits (stock PQ) |
|--------|--------|-----------------|
| Left third | 1.0 | 100 |
| Center third | 10.0 | 1000 |
| Right third | 4.0 | 400 |
| Bottom ramp | 0.01 … 10 | stepped |

```bash
cd _hdr_test
g++ -O2 -std=c++17 write_hdr_exr.cpp -o write_hdr_exr \
  -I/usr/include/OpenEXR -I/usr/include/Imath \
  -lOpenEXR -lOpenEXRCore -lImath -lIex -lIlmThread
./write_hdr_exr
```

### Eyeball

- Exposure/brightness **0** for path checks (grade scales scene-linear before PQ).
- 100-nit PQ peak **without** scale sits below 203-nit UI white — expected.
- With scale or linear-P3 present, 100-nit peak should sit near desktop white.
- Screenshots (grim) tone-map HDR; trust eyes / mpv ladder.

---

## Known gaps / next precision work

1. **8-bit GL FBO grab** — still used for `pq` / `p3linear` / `srgblinear`
   (quantization / banding). **`p3extended` uses RGBA16F render + RGBA32F
   upload** so EDR \>1 can survive; PQ path should get the same treatment later.
2. **HUD / UI** drawn into the same FBO is not image-referred on HDR10 (odd
   greens, etc.). Separate image-plane present already helps; keep chrome off
   the PQ codes when possible.
3. **Per-channel PQ convert** in present (not luminance-first) — fine for grey
   wedges; not chroma-safe for saturated HDR.
4. **Hyprland** linear client surface support may lag KWin; if `p3linear` falls
   back to HDR10, logs will say so.
5. **OCIO display default** is still sRGB unless the user selects PQ (or a
   future HDR-aware default).

---

## Quick checklist

- [ ] Monitors: `cm=hdr`, 10-bit  
- [ ] `RV_HDR=1`, Wayland + Vulkan RHI  
- [ ] OCIO URI without spurious `.ocio`  
- [ ] OCIO display **Active** + Rec.2100-PQ (or intended HDR view)  
- [ ] A: `RV_HDR_PRESENT=pq` + `RV_HDR_PQ_SDR_SCALE=1`  
- [ ] B: `RV_HDR_PRESENT=p3linear` (no scale env)  
- [ ] Log shows intended swapchain format + shader mode  
- [ ] Exposure at 0 for comparison  

---

*Last updated from OpenRV HDR bring-up on Arch + Hyprland + dual Dell S3225QC,
Qt 6.11, OCIO 2.5.2 (CY2025 + OCIO-only bump).*
