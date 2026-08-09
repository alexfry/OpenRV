# OpenRV HDR on Wayland (Hyprland) — field notes

Working notes for **absolute HDR presentation of the image plane** on
Wayland + Vulkan. Branch / experimental; not all of it is upstream-ready.

**Platform brought up against:** Arch Linux, Hyprland, dual Dell S3225QC
(`cm=hdr`, XBGR2101010), NVIDIA, Qt **6.11.1**, OpenRV CY2025 + OCIO **2.5.2**.

---

## Goals

1. **Image plane** shows super-white (scene-linear \> 1.0 / ACES HDR OT peaks)
   as visibly brighter than reference white on an HDR OLED.
2. Prefer a **single window** (embedded present), not a separate tiled client.
3. Support **stock RV** (`RV_HDR` + DisplayIPNode PQ) and **OCIO** display
   transforms (ACES 2.0 built-ins via OCIO 2.5.2).
4. A/B **PQ HDR10** vs **linear extended** present (P3 / scRGB / Display P3 Extended).
5. **No full-frame CPU readback** on the hot path — GL→Vulkan stays on the GPU.

Non-goals (for now): whole-UI PQ tagging, float EGL configs on NVIDIA Wayland,
perfect HUD color on the HDR surface, chroma-safe PQ.

---

## Pipeline overview

```
  EXR / media (float)
       │
       ▼
  IP graph  ──►  OCIODisplay  (optional: scene_linear → PQ or Display P3, …)
       │         or RVDisplayColor / DisplayIPNode (RV_HDR → SMPTE-2084)
       ▼
  GL present FBO
       │   pq / p3linear / srgblinear : 8-bit widget FBO (PQ codes typical)
       │   p3extended                 : RGBA16F FBO (EDR headroom >1)
       │
       ├─► GPU interop (DEFAULT)
       │     glBlit → exportable VkImage (GL_EXT_memory_object_fd)
       │     → GPU copy → QRhi sample image → present pass
       │
       └─► CPU fallback (RV_HDR_GL_VK_INTEROP=0 or interop create failure)
             pq…: grabFramebuffer (RGBA8)
             p3extended: glReadPixels HALF_FLOAT → upload RGBA16F
       ▼
  Vulkan present (QRhi swapchain)  ──►  Hyprland color management
       │
       │   RV_HDR_PRESENT selects surface + fragment convert:
       │     pq         → HDR10 ST.2084  (+ optional SDR-white scale)
       │     p3linear   → P3 extended linear  (PQ→linear, 1.0 = SDR white)
       │     srgblinear → scRGB extended linear (same convert from PQ)
       │     p3extended → P3 extended linear  (sRGB EOTF → linear P3; EDR)
       ▼
  HDR panel (e.g. Dell S3225QC)
```

**Post-OCIO:** present encodes/scales live in the **Vulkan fragment shader** only.
`RV_HDR_PRESENT` does not rewrite the IP graph.

---

## GL → Vulkan handoff (GPU interop) — default path

### Why it exists

Full-frame **CPU readback** (`glReadPixels` / `grabFramebuffer` + QRhi upload)
was the playback bottleneck: large windows with HDR present sat ~11 fps while
the non-HDR GL path locked 24. Conversion in the present shader was already GPU;
the bus copy was not.

### Architecture (v1 that works on this NVIDIA stack)

```
  Vulkan side                          GL side
  ───────────                          ───────
  VkImage (exportable, dedicated,
  OPTIMAL, COLOR+TRANSFER+SAMPLED)
       │
       │ vkGetMemoryFdKHR (OPAQUE_FD)
       ▼
  GL memory object  ◄── DEDICATED_MEMORY_OBJECT_EXT = TRUE  (required!)
       │
       │ glTextureStorageMem2DEXT + TEXTURE_TILING_EXT = OPTIMAL
       ▼
  GL texture + FBO
       │
       │ each frame: glBlitFramebuffer(srcPresentFbo → sharedFbo)
       │             glFinish
       ▼
  VkImage (shared) ──GPU copy──► VkImage (sample-only, not exported)
                                       │
                                       │ QRhiTexture::createFrom
                                       ▼
                                 present shader → HDR swapchain
```

| Step | Detail |
|------|--------|
| Allocate | Vulkan creates exportable image with **dedicated** allocation + external-memory handle types |
| Import | GL sets `DEDICATED_MEMORY_OBJECT_EXT=TRUE` **before** `glImportMemoryFdEXT` |
| Tiling | `glTextureParameteri(…, TEXTURE_TILING_EXT, OPTIMAL_TILING_EXT)` before `TextureStorageMem` |
| Write | GL blits present FBO → shared texture (layout stays **GENERAL** for the shared image) |
| Sample | Separate non-export sample image; `vkCmdCopyImage` shared → sample; QRhi samples sample image |
| Sync (v1) | `glFinish` after blit; queue wait after copy (semaphores later) |

**Do not sample the exportable image directly in QRhi** across frames without
careful layout restore — QRhi tends to leave `SHADER_READ_ONLY`; next GL write
then goes black. Copy-to-sample avoids that thrash.

### Log markers (success)

```
INFO: GL↔Vulkan shared image WxH RGBA8|RGBA16F (GPU interop, no CPU readback)
INFO: present GPU interop WxH swap=… shMode=… copy=ok
```

### Force CPU path

```bash
export RV_HDR_GL_VK_INTEROP=0
```

### Pitfalls we hit (do not reintroduce)

| Symptom | Cause | Fix |
|---------|--------|-----|
| Shared blit “ok” but pure black present | Missing `DEDICATED_MEMORY_OBJECT_EXT` on GL import of a Vulkan dedicated alloc | Set dedicated **before** `ImportMemoryFd` |
| Interop always “fails” and falls back | `glReadPixels` of external FBO returns zeros → false “black blit” detector | Never use ReadPixels as validity check on EXT_memory_object stores |
| Green static noise | `glReadPixels(GL_FLOAT)` uploaded into `RGBA16F` texture | Match formats: half-float end-to-end (or float32→RGBA32F) |
| QRhi create fails at startup | `initRhi` before present window is exposed / has a handle | Only create QRhi when `handle()` && `isExposed()` |
| Black after a few frames | Sampling shared image; layout left SHADER_READ_ONLY for GL | GPU-copy into a sample-only image each frame |
| Linear P3 looks “double gamma” | Surface tagged `QColorSpace::DisplayP3` (sRGB TF) after linear handoff | Tag **Linear** + DciP3D65 primaries |

---

## p3extended float / half path

Intended buffer (OCIO **Display P3** or Mac-style EDR OT):

| Property | Value |
|----------|--------|
| Primaries | Display P3 |
| White | D65 |
| Transfer | Piecewise sRGB EOTF (encoded → linear), extended for \|c\| \> 1 |
| Dynamic range | 1.0 ≈ paper white; \>1 = EDR headroom |

| Stage | Format |
|-------|--------|
| GL present FBO | **RGBA16F** |
| GPU interop shared + sample | **RGBA16F** |
| CPU fallback readback | `glReadPixels(…, GL_HALF_FLOAT)` → `uint16_t` |
| CPU fallback QRhi texture | **RGBA16F** |

Shader mode 3: per-channel IEC sRGB **EOTF** (not OETF). Verified vs OCIO
Display P3 Un-tone-mapped (code 1.825 → linear 4.0). Then optional
× `sdrWhite/refWhite` for UI white match (`RV_HDR_LINEAR_SDR_MATCH`, default on).

**Do not** pair `p3extended` with a Rec.2100-PQ OCIO view — use Display P3
(gamma-encoded), not PQ. PQ views belong with `pq` / `p3linear`.

```
INFO: GL float present FBO RGBA16F WxH (p3extended EDR transfer)
INFO: present upload texture RGBA16F (half-float transfer for p3extended)   # CPU path only
```

---

## Compositor setup (Hyprland)

```conf
# Desktop in HDR so windowed clients can present HDR10 / linear HDR.
monitorv2 {
    ...
    bitdepth = 10
    cm = hdr
    sdrbrightness = 1.0
    sdr_min_luminance = 0.005
    sdr_max_luminance = 100   # compositor local SDR; media white for match is 203 nits
}
```

```bash
hyprctl monitors -j | jq '.[] | {name, colorManagementPreset, currentFormat, sdrMaxLuminance, sdrBrightness}'
```

Expect: `colorManagementPreset: hdr`, `currentFormat: XBGR2101010` (or similar 10-bit HDR).

### The “203 nits” problem

**SDR media / reference white is 203 nits** (not 200). Absolute **PQ** codes for a
**100-nit** peak (typical ACES 100-nit container / `refWhite=100`) are ~0.51 PQ and
look **dimmer than desktop UI white** if chrome sits near 203 nits.

| Signal | Meaning of “1.0” / peak |
|--------|-------------------------|
| ACES 100-nit PQ view | Peak ~100 nits absolute |
| Wayland SDR UI / scRGB-like ref | Often ~203 nits |
| PQ surface (HDR10) | Code 1.0 = 10 000 nits absolute |

| Mitigation | How |
|------------|-----|
| A — PQ scale | `RV_HDR_PQ_SDR_SCALE=1` → PQ→lin→×(sdrWhite/refWhite)→PQ in present shader |
| B — linear surface | `RV_HDR_PRESENT=p3linear` → 1.0 = compositor SDR white |
| C — EDR-style | `RV_HDR_PRESENT=p3extended` + linear white-match |

**Policy:** use compositor-reported `sdrWhiteLevel` as-is when available; fallback
default is **203**. Never invent 200 when you mean 203; do not snap compositor
values randomly.

---

## Environment variables

| Variable | Default | Purpose |
|----------|---------|---------|
| `RV_HDR=1` | off | Enable HDR present path + (stock) DisplayIPNode PQ force |
| `RV_ALLOW_WAYLAND=1` | — | Allow native Wayland QPA |
| `QT_QPA_PLATFORM=wayland` | — | Force Wayland |
| `QT_WIDGETS_RHI_BACKEND=vulkan` | set by wrapper when HDR/Wayland | Vulkan RHI (pure QOpenGLWidget is black on this NVIDIA stack) |
| **`RV_HDR_PRESENT`** | `pq` | `pq` \| `p3linear` \| `srgblinear` \| `p3extended` |
| **`RV_HDR_PQ_SDR_SCALE=1`** | off | PQ path: × sdrWhite/refWhite in PQ domain |
| **`RV_HDR_LINEAR_SDR_MATCH`** | **on** for `p3extended` | After sRGB EOTF, × sdrWhite/refWhite |
| **`RV_HDR_SDR_WHITE`** | swapchain report, else **203** | Compositor SDR white nits |
| **`RV_HDR_PQ_REF_WHITE`** | **100** | Content reference white nits |
| **`RV_HDR_GL_VK_INTEROP=0`** | **on** (interop enabled) | Force CPU readback present |
| `RV_GL_PROBE=1` | off | Log FBO L/C/R 8-bit samples (PQ100≈130, PQ400≈164, PQ1000≈192) |
| `RV_HDR_TEST_PATTERN=1` | off | Synthetic PQ wedges in present (bypass GL) |
| `OCIO` | unset | OCIO config URI or path |

### `RV_HDR_PRESENT` values

| Value | Aliases | QRhi swapchain | Expected FBO | Fragment mode |
|-------|---------|----------------|--------------|---------------|
| `pq` | `hdr10`, `st2084` | `HDR10` | PQ codes (8-bit typical) | 0 pass / 1 PQ×scale |
| `p3linear` | `p3`, `linear-p3` | `HDRExtendedDisplayP3Linear` | PQ codes | 2 PQ→nits/sdrWhite |
| `srgblinear` | `scrgb`, `srgb-linear` | `HDRExtendedSrgbLinear` | PQ codes | 2 same |
| **`p3extended`** | `displayp3-extended`, `edr-p3` | `HDRExtendedDisplayP3Linear` | P3 + sRGB TF (float) | 3 sRGB EOTF → linear P3 |

Restart RV after changing `RV_HDR_PRESENT` (surface colorspace + swapchain).

---

## A/B recipes

Stage binary: `_build/stage/app/bin/rv`. HDR monitor with `cm=hdr`.

### A — PQ + SDR-white scale

```bash
export OCIO='ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5'
export RV_HDR=1 RV_ALLOW_WAYLAND=1 QT_QPA_PLATFORM=wayland
export RV_HDR_PRESENT=pq
export RV_HDR_PQ_SDR_SCALE=1
# optional: RV_HDR_SDR_WHITE / RV_HDR_PQ_REF_WHITE

_build/stage/app/bin/rv /path/to/media
```

OCIO: enable **display Active**, pick **Rec.2100-PQ** (or similar). Expect:

```
INFO: ... mode=pq/HDR10 ...
INFO: ... swapchain format=HDR10 (PQ/ST.2084) ...
INFO: GL↔Vulkan shared image … (GPU interop, no CPU readback)
INFO: post-OCIO PQ SDR-white scale ON …   # if RV_HDR_PQ_SDR_SCALE=1
```

### B — Linear Display-P3 from PQ buffer

```bash
export RV_HDR_PRESENT=p3linear
# no RV_HDR_PQ_SDR_SCALE — 1.0 is sdrWhite by construction
```

### C — Display P3 Extended (macOS EDR-style)

```bash
export RV_HDR_PRESENT=p3extended
# OCIO: Display P3 (sRGB TF), not Rec.2100-PQ
```

### Stock wedge (no OCIO)

```bash
unset OCIO
export RV_HDR=1 RV_ALLOW_WAYLAND=1 QT_QPA_PLATFORM=wayland
export RV_GL_PROBE=1
_build/stage/app/bin/rv _hdr_test/hdr_wedge_1080.exr
```

DisplayIPNode: scene-linear **1.0 → 100 nits → PQ** when `RV_HDR=1`.  
Unscaled probes: `LCR8≈130,192,166` for 100/1000/400 nits.

### A/B CPU vs GPU present

```bash
# GPU (default)
unset RV_HDR_GL_VK_INTEROP

# CPU readback
export RV_HDR_GL_VK_INTEROP=0
```

---

## OCIO

| | |
|--|--|
| VFX pin | **CY2025** (other deps unchanged) |
| OpenColorIO | **2.5.2** (ACES 2.0 built-ins; stock CY2025 was 2.4.2) |
| Install | `~/openrv-deps/RV_DEPS_OCIO` → staged under `_build/stage/app/lib` |

```bash
# ACES 2.0
export OCIO='ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5'
export OCIO='ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5'
# ACES 1.3 still available via ocio://studio-config-v2.2.0_… etc.
```

**Do not** append `.ocio` to `ocio://` URIs.

### Activation

- `$OCIO` only **loads** the config.
- Display stays on `RVDisplayColor` until OCIO display is **Active** (or
  `OCIOFile` auto-enables it).
- Default studio display is often **sRGB** — for HDR10 pick **Rec.2100-PQ**
  (or Display P3 for `p3extended`).
- When `OCIODisplay` replaces `RVDisplayColor`, stock `RV_HDR` DisplayIPNode PQ
  force **no longer runs**; OCIO owns the encode. Present still tags HDR.

### Rebuild OCIO only

Pin: `cmake/defaults/CY2025.cmake` → `RV_DEPS_OCIO_VERSION=2.5.2`.

```bash
ninja -C _build clean-RV_DEPS_OCIO
cmake -S . -B _build
ninja -C _build RV_DEPS_OCIO RV_DEPS_OCIO-stage-target OCIONodes rv -j$(nproc)
```

Notes: yaml-cpp 0.8 may need `-include cstdint`; `libOpenColorIO` may need
`libz-ng` DT_NEEDED (`ocio_patchelf_zng.cmake`).

---

## Implementation map

| Piece | Location |
|-------|----------|
| Vulkan present window / modes / interop API | `src/lib/app/RvCommon/VulkanPresentWidget.{h,cpp}` |
| GL↔Vk shared image + sample copy | `src/lib/app/RvCommon/GlVkSharedImage.{h,cpp}` |
| Present shaders (GPU convert) | `src/lib/app/RvCommon/shaders/present.{vert,frag}` → `*.qsb` + `*_qsb.h` |
| Embed present into main UI | `RvDocument.cpp` (`createWindowContainer` subsurface) |
| Present target FBO + handoff | `GLView.cpp` (`presentExternalFrame`, float FBO, interop blit) |
| Bind float/default FBO for graph | `QTGLVideoDevice.cpp` |
| Stock PQ encode | `DisplayIPNode.cpp` (`RV_HDR=1` → SMPTE-2084, linear nits/100) |
| OCIO dep pin | `cmake/defaults/CY2025.cmake`, `cmake/dependencies/ocio.cmake` |
| Test wedge / docs | `_hdr_test/` |

### Rebuild present shaders after editing GLSL

```bash
QSB=/usr/lib/qt6/bin/qsb   # or Qt install path
cd src/lib/app/RvCommon/shaders
$QSB --glsl "440,300 es" --hlsl 50 --msl 12 -o present.frag.qsb present.frag
$QSB --glsl "440,300 es" --hlsl 50 --msl 12 -o present.vert.qsb present.vert
# regenerate present_*_qsb.h byte arrays, then:
ninja -C _build RvCommon rv -j$(nproc)
```

Fragment `params` (std140 `vec4` after `mat4`):

| Component | Meaning |
|-----------|---------|
| `x` | Scale (mode 1 PQ linear scale; mode 3 linear white-match) |
| `y` | Mode: `0` pass, `1` PQ×scale, `2` PQ→linear, `3` sRGB-TF→linear P3 |
| `z` | SDR white nits (mode 2) |
| `w` | UV flip V (1 = GL bottom-up FBO) |

---

## Test media

### Full-frame wedge (`hdr_wedge_1080.exr`)

1920×1080 scene-linear (stock `RV_HDR`: 1.0 = 100 nits):

| Region | Linear | Nits (stock PQ) |
|--------|--------|-----------------|
| Left third | 1.0 | 100 |
| Center third | 10.0 | 1000 |
| Right third | 4.0 | 400 |

```bash
cd _hdr_test
g++ -O2 -std=c++17 write_hdr_exr.cpp -o write_hdr_exr \
  -I/usr/include/OpenEXR -I/usr/include/Imath \
  -lOpenEXR -lOpenEXRCore -lImath -lIex -lIlmThread
./write_hdr_exr
```

### Eyeball

- Exposure/brightness **0** for path checks.
- 100-nit PQ peak without scale sits below 203-nit UI white — expected.
- Screenshots (`grim`) tone-map HDR; trust eyes on the panel for absolute HDR.
- Green static ⇒ half/float format mismatch. Pure black image plane with UI OK
  ⇒ present path empty (interop layout / dedicated / fallback).

---

## Performance

| Path | Per frame | Notes |
|------|-----------|--------|
| **GPU interop (default)** | GL blit + Vk copy + present | Target for locked 24 at large windows |
| CPU + `pq` | RGBA8 grab + upload | Lighter CPU path |
| CPU + `p3extended` | HALF_FLOAT readback + RGBA16F upload | ~½ of float32; still bus-bound |
| Pre-interop HDR | float32/8-bit full readback | ~11 fps large window observed |

If playback crawls, confirm logs still say **GPU interop, no CPU readback**.  
`RV_HDR_GL_VK_INTEROP=0` reverts to the slow path on purpose.

---

## Known gaps / future work

1. **8-bit FBO** for `pq` / `p3linear` — still quantizes; float/half optional later.
2. **HUD / UI** in the same FBO is not image-referred on HDR10.
3. **Per-channel PQ** present convert is not luminance-first / chroma-safe.
4. **Semaphores** instead of `glFinish` + queue wait for less CPU stall.
5. **OCIO display default** still sRGB unless the user selects HDR/P3.
6. Hyprland linear client support may lag; watch for HDR10 fallback logs.

---

## Quick checklist

- [ ] Monitors: `cm=hdr`, 10-bit  
- [ ] `RV_HDR=1`, Wayland + Vulkan RHI  
- [ ] OCIO URI without spurious `.ocio`  
- [ ] OCIO display **Active** + intended view (PQ vs Display P3)  
- [ ] Log: **GPU interop, no CPU readback** (unless intentionally CPU)  
- [ ] A: `RV_HDR_PRESENT=pq` + optional `RV_HDR_PQ_SDR_SCALE=1`  
- [ ] B: `RV_HDR_PRESENT=p3linear`  
- [ ] C: `RV_HDR_PRESENT=p3extended` + Display P3 OCIO  
- [ ] Exposure at 0 for comparison  
- [ ] Fullscreen play locks ~24 with interop  

---

## Commit trail (this bring-up)

High-signal commits on the Wayland/HDR branch (newest first as of docs update):

| Commit | Summary |
|--------|---------|
| `b5a8c387` | Interop: dedicated GL import + drop false black ReadPixels probe |
| `c090c855` | p3extended CPU: half-float end-to-end |
| `b1ab2570` | Fix green static (float32 vs RGBA16F mismatch) |
| `d919b4a5` | Temporary interop off + auto-fallback (superseded by b5a8c387) |
| `b13a9f99` | First GPU interop plumbing |
| `50a1d53f` | Present modes, OCIO 2.5.2, SDR-white handling |
| `359fbfd3` / `bc2ea8e9` | HDR10 swapchain + Wayland Vulkan present path |

---

*Last updated: GPU interop working (dedicated memory + sample copy), half-float
p3extended CPU fallback, Arch + Hyprland + dual Dell S3225QC, Qt 6.11, OCIO 2.5.2.*
