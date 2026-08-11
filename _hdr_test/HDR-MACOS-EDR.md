# OpenRV HDR on macOS (EDR) — feasibility assessment

Assessment of porting the Wayland/Vulkan desktop-HDR work
(`alexfry/arch-qt611-wayland-build`, see `HDR-WAYLAND.md`) to macOS and its
EDR display chain. Written against OpenRV `main` (Qt 6.8.3 pin) with the
Wayland branch as the reference implementation.

> **See also `HDR-SURFACE-DESIGN.md`** — the cross-platform design that
> supersedes this document's macOS-specific framing. Its central point: both
> platforms' color-management layers (ColorSync / `wp_color_manager_v1`) are
> far richer than the graphics-API swapchain enums, and the shared
> abstraction belongs there. This document remains accurate as the macOS
> feasibility and effort analysis.

---

## Implementation status — BUILT AND WORKING

The assessment below was written before any code existed. It has since been
implemented and **HDR presentation on macOS works**, confirmed by eye on a
built-in Liquid Retina XDR (M4 Pro, Qt 6.11.1, Xcode 26.6, OCIO 2.5.2).

### What exists now

| Piece | Where |
|---|---|
| `PresentSurface` — backend-neutral present interface | `RvCommon/PresentSurface.h` |
| Metal/QRhi EDR present surface | `RvCommon/MetalPresentWidget.{h,mm}` |
| Per-platform backend selection | `RvDocument.cpp` |
| Display P3 Extended encoding (default on macOS) | `DisplayIPNode.cpp`, `GLView.cpp` |
| Embedding spike that de-risked it | `_hdr_test/spike_b/` |

Run it:

```bash
OCIO="ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5" RV_HDR=1 \
  _build/stage/app/RV.app/Contents/MacOS/RV your.exr
```

### Predictions that held

- **Present shaders run on Metal unchanged.** The `.qsb` blobs already carry an
  MSL 12 variant next to SPIR-V/GLSL/HLSL. Zero shader work was needed.
- **`HDRExtendedDisplayP3Linear` is supported** — `isFormatSupported()` returns
  true on Qt 6.11, no SDR fallback.
- **The 203-nit SDR-white mess disappears.** The Wayland backend's
  `sdrWhite/refWhite` boost is simply not applied: on an extended-linear macOS
  surface linear 1.0 *is* SDR white.
- **Mode 3 (`p3extended`) was the right target.** It was written on the Linux
  side as a "macOS EDR-style" analogue and turned out to be exactly correct.

### What the assessment got wrong or missed

**1. PQ end-to-end was the real blocker, not GPU interop.**
`RV_HDR` encoded the display as SMPTE-2084 on every platform, so the macOS
path encoded to PQ and then immediately undid it in the present shader — a
round trip through a transfer function no surface ever carried. Visible as a
plausible-looking but wrong image (spotted by eye as "PQ-encoded buffer tagged
Display P3 extended"). Fixed by adding `RV_HDR_ENCODING` and defaulting macOS
to `p3extended`.

**Three stages must agree**, and nothing enforces it:

| Stage | Sets | macOS default |
|---|---|---|
| `DisplayIPNode` | buffer encoding | sRGB piecewise TF |
| `GLView` | GL surface tag | `QColorSpace::DisplayP3` |
| `MetalPresentWidget` | decode mode | 3 |

A mismatch decodes with the wrong transfer and still *looks* like an image.
This is the single easiest thing to get wrong in the whole path, and it argues
for the canonical-space registry in `HDR-SURFACE-DESIGN.md` carrying the
encoding rather than three files agreeing by convention.

**2. EDR headroom is dynamic, and swings hard.**
Measured on one panel across three launches: **1.2, 2.95, 8.56**. macOS
throttles for brightness, thermals and other on-screen EDR content. This
reframes open question #3 (pin the surface tag or follow it) — it is not a
multi-monitor edge case, it happens sitting still on a single display. Any
"which steps are visible" reasoning must treat headroom as a live value.

**3. `QRhiSwapChain::hdrInfo()` really is useless on Metal.**
Confirmed in practice, not just by reading qtbase: the hardcoded
`sdrWhiteLevel = 200` is what you get. All headroom here comes from
`NSScreen.maximumExtendedDynamicRangeColorComponentValue`.

**4. OCIO ships the ACES 2.0 studio config built in.**
No file needed — OCIO 2.5.2 resolves
`ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5` through `CreateFromEnv()`,
and it is the same config as the `alexfry/imagescope` copy. Its
`Display P3 - Display` display with the **`Un-tone-mapped`** view is the pairing
the mode-3 shader was validated against (`code 1.825 → linear 4.0`).

**5. Frame orientation must persist with the texture.**
A per-frame `flipV` local is a trap: repaints that carry no new frame
(`UpdateRequest`, resize) re-upload the uniform while the texture still holds
the previous frame, so the image flips intermittently. The half-float GL
readback is bottom-up (`flipV=1`); `grabFramebuffer()` is already top-left
(`flipV=0`). Store it as state alongside the texture.

**6. `exposeEvent` arrives late.** The embedded QWindow is not exposed until
well after `RvDocument` construction — after plugin loading. Diagnostics that
sample too early will wrongly conclude the swapchain never came up.

**7. `Qt::WindowTransparentForInput` is required on macOS too, and it is the
only layer that works.** Without it the native overlay swallows clicks in the
viewer and timeline and modifier drags like E+drag for exposure — the same
symptom Linux hit. Two things make this easy to get wrong:

- `WA_TransparentForMouseEvents` on the widget and container is *not* enough.
  Those govern QWidget delivery; the overlay is a native window on top.
- `GLView::eventFilter` cannot cover for it. It matches on
  `qobject_cast<QWidget*>(object)`, and events delivered to a native `QWindow`
  are not QWidgets, so they never match. On Linux it is a genuine backup only
  because some events still arrive via the container widget.

Set the flag *after* `createWindowContainer`, which can reset window flags.

**Spike B did not catch this**, and it is worth understanding why: the spike
validated stacking, exposure, sizing and fullscreen, but had no interactive
content beneath the overlay, so "do clicks reach the widget below" was never
exercised. A spike passing is not the same as the pattern being complete. Any
future present-surface spike should put something clickable underneath.

### Still not done

- **GPU interop.** `ensureGpuInterop()` returns false, so GLView uses its CPU
  readback path. The IOSurface design below is unimplemented. This is the
  remaining performance work, and the assessment's effort estimate for it is
  still untested.
- **Surface tag driven by the OCIO display.** Currently the encoding is chosen
  by platform default, not derived from the OCIO display colorspace as
  `HDR-SURFACE-DESIGN.md` requires.
- **Headroom is measured but unused.** Nothing adapts to it and nothing warns
  when content exceeds it.

---

## Verdict

**Feasible, and easier than the Wayland bring-up.** The architecture on the
Wayland branch was deliberately shaped around an EDR-style mode
(`RV_HDR_PRESENT=p3extended`, alias `edr-p3`, "macOS EDR-style"), and that is
precisely the *native* model on macOS:

- Qt 6.8's QRhi **Metal backend supports EDR swapchains today**:
  `HDRExtendedDisplayP3Linear` and `HDRExtendedSrgbLinear` map to
  `MTLPixelFormatRGBA16Float` + `kCGColorSpaceExtendedLinearDisplayP3` /
  `kCGColorSpaceExtendedLinearSRGB` + `CAMetalLayer.wantsExtendedDynamicRangeContent = YES`
  (verified in `qtbase/6.8` `src/gui/rhi/qrhimetal.mm`, `chooseFormats()` /
  `createOrResize()` ~lines 6238–6298).
- The present widget is already QRhi-based, and the `.qsb` shaders were
  compiled with `--msl 12` — the **present shaders run on Metal unchanged**.
- The whole "203 nits" SDR-white mess **disappears**: macOS EDR is
  display-referred by construction (1.0 = SDR white, headroom above it).
- The one real porting job is the GPU interop class: `GlVkSharedImage`
  (external-memory FD) has no macOS equivalent and must be replaced by an
  **IOSurface**-based GL↔Metal share — which is a *simpler* and better-trodden
  mechanism than the Vulkan external-memory path.

One caveat, narrower than it first looks: Qt's QRhi Metal backend exposes no
HDR10/PQ swapchain *format* (`isFormatSupported()` returns true only for SDR +
the two extended-linear formats). But on macOS the drawable colorspace is just
a `CALayer` property — WindowServer color-matches whatever the layer is tagged
with, including `kCGColorSpaceITUR_2100_PQ`, HLG, and fully **custom**
calibrated spaces. A small native shim that overrides
`CAMetalLayer.colorspace` after QRhi creates the swapchain gives a true PQ
surface while keeping the whole QRhi present path. ImageScope
(`alexfry/imagescope`) ships exactly this model in production — see
"Prior art: ImageScope" below.

---

## macOS EDR model vs Wayland HDR model

| Aspect | Wayland (Hyprland) branch | macOS EDR |
|---|---|---|
| Reference white | Compositor `sdrWhiteLevel` in nits (203 default, env overrides) | **Always 1.0 in the swapchain** (display-referred); OS maps to panel via brightness |
| Headroom | Implied by monitor mode (`cm=hdr`, PQ 10k ceiling) | `NSScreen.maximumExtendedDynamicRangeColorComponentValue` (dynamic, brightness-dependent) + `maximumPotential…` |
| Absolute nits | Achievable (PQ surface = absolute ST.2084) | **Not in the default model.** Only pinned in Reference Modes (e.g. "HDR Video (P3-ST 2084)": SDR white = 100 nits, peak 1000) |
| Surface tagging | Vulkan swapchain colorspace (HDR10 / extended linear) | `CAMetalLayer` colorspace + `wantsExtendedDynamicRangeContent` |
| Tone mapping above headroom | Compositor-dependent | OS clamps/rolls off EDR values above current headroom |
| Who composites | Hyprland CM | WindowServer (EDR-aware since 10.11; battle-tested) |

Consequences:

1. `p3extended` is the **primary and default** macOS mode. Content 1.0 = UI
   white automatically — `RV_HDR_LINEAR_SDR_MATCH` becomes a no-op (scale 1.0).
2. `RV_HDR_SDR_WHITE`, `RV_HDR_PQ_SDR_SCALE` and the 203-nit policy are
   Linux-only concerns. On macOS, `QRhiSwapChainHdrInfo` reports
   `limitsType = ColorComponentValue`, `luminanceBehavior = DisplayReferred`,
   and a **dummy** `sdrWhiteLevel = 200` — do not consume `sdrWhiteLevel` on
   Apple platforms (verified `qrhimetal.mm` `hdrInfo()` ~line 6403).
3. For *absolute* review (100-nit PQ container checks), the macOS answer is a
   Reference Mode on an XDR panel, not shader math: in "HDR Video" reference
   mode, EDR value = nits / 100 exactly. Worth a docs note, not code.

---

## Component-by-component port map

| Wayland branch piece | macOS EDR equivalent | Port effort |
|---|---|---|
| `VulkanPresentWidget/Window` (QRhi + QVulkanInstance) | Same class, QRhi **Metal** backend: `QWindow::setSurfaceType(MetalSurface)`, `QRhi::create(QRhi::Metal, &params)`. No instance object needed. | Small — factor backend selection out; ~50 lines of `#ifdef`/runtime switch |
| `selectSwapChainFormat()` | Same logic; on Metal prefer `HDRExtendedDisplayP3Linear`, fall back `HDRExtendedSrgbLinear`, then SDR. Gate with `isFormatSupported()` per screen. | Trivial |
| `present.frag/.vert` `.qsb` | **Unchanged** — already compiled with `--msl 12`. Mode 3 (sRGB-EOTF→linear, extended for \|c\|>1) is exactly the EDR decode. | None |
| `GlVkSharedImage` (VkImage + `vkGetMemoryFdKHR` + `GL_EXT_memory_object_fd`) | **`GlMtlSharedImage`**: one `IOSurface` shared by both APIs. GL: `CGLTexImageIOSurface2D` → `GL_TEXTURE_RECTANGLE` texture → FBO, blit into it. Metal: `newTextureWithDescriptor:iosurface:plane:` → `QRhiTexture::createFrom(NativeTexture)` (supported by QRhi Metal, `QMetalTexture::createFrom`, `qrhimetal.mm` ~line 3872). | The main job — but simpler than Vulkan: no memory import, no dedicated-allocation trap, no image layouts. Est. 300–400 lines Obj-C++ |
| CPU fallback (`glReadPixels` HALF_FLOAT → `setFrameHalf`) | **Unchanged** — pure GL + QRhi upload, works on Metal as-is. Day-one bring-up path before interop lands. | None |
| `RvDocument` embed (`createWindowContainer` over `GLView` in `QStackedLayout::StackAll`) | Same mechanism; on macOS the container wraps an `NSView`, both are sibling layers in one `NSWindow`. Gate: `PLATFORM_DARWIN` + `RV_HDR` instead of Wayland platform check. | Trivial + testing |
| `GLView` handoff (`presentExternalFrame`, float FBO, viewport-derived size) | Unchanged. `devicePixelRatio` is a clean 2.0 on retina — the fractional-DPR robustness comes along for free. | None |
| `QTGLVideoDevice` FBO redirection | Unchanged (platform-agnostic). | None |
| `DisplayIPNode` `RV_HDR` PQ force | Keep for `pq`-style modes; **not** the macOS default. Stock macOS path wants Display-P3/sRGB encode with values >1 preserved (see "Stock path" below). | Small |
| OCIO 2.5.2 bump (`CY2025.cmake`, `ocio.cmake`) | Platform-independent, carries over. `ocio_patchelf_zng` is Linux-only; macOS staging uses install_name paths instead. | Small build work |
| Wrapper env plumbing (`rv.wrapper`) | macOS launches via bundle; put env defaults in `main.cpp`/`RvApplication` instead of shell wrappers. | Small |

---

## PQ (and arbitrary surface tags) on macOS

Two facts, which pull in opposite directions:

- `QMetalSwapChain::isFormatSupported()` (qtbase 6.8, ~line 6194) accepts only
  `SDR`, `HDRExtendedSrgbLinear`, `HDRExtendedDisplayP3Linear` — QRhi's
  *abstraction* has no HDR10/PQ swapchain format on Metal.
- Metal/Core Animation itself has no such limitation. The drawable's
  colorspace is an assignable `CALayer` property, and WindowServer
  color-matches layer contents from **any** CGColorSpace to the display:
  `itur_2100_PQ`, `itur_2100_HLG`, `itur_2020`, and custom calibrated spaces
  built from primaries + gamma. ImageScope demonstrates this in production
  with an `rgba16Float` MTKView drawable, `wantsExtendedDynamicRangeContent =
  true`, and a user-selectable surface tag including PQ and a hand-built
  BT.1886 (Rec.709 γ2.4) space (`MetalEDRImageView.swift`:
  `colorSpaceForIdentifier()`, `createBT1886ColorSpace()`).

So a native PQ surface through the QRhi path costs one small shim, not a
hand-rolled present loop:

1. **QRhi + native colorspace override — recommended.** Create the swapchain
   as `HDRExtendedSrgbLinear` (that buys `MTLPixelFormatRGBA16Float` +
   `wantsExtendedDynamicRangeContent` from Qt), then in a ~30-line Obj-C++
   shim grab the `CAMetalLayer` from the present window's `NSView` and assign
   `layer.colorspace` per the selected present mode
   (`kCGColorSpaceITUR_2100_PQ` for `pq`, etc.). Qt assigns the colorspace in
   `createOrResize()`, so re-apply the override after every swapchain
   create/resize (the branch already funnels those through one place).
   Present shader mode 0 (passthrough) — the FBO already holds PQ codes.
   Optionally set `CAMetalLayer.edrMetadata` (`CAEDRMetadata`, macOS 10.15+)
   for HDR10-style mastering metadata.
2. **Decode PQ in the present shader onto the extended-linear surface** —
   already exists as mode 2 (`p3linear`/`srgblinear`), divisor =
   `RV_HDR_PQ_REF_WHITE` (default 100): EDR = nits/refWhite. Zero native code;
   display-referred rather than absolute. Keep as the A/B partner and
   fallback.
3. Upstream the gap to Qt — HDR10-on-Metal is a contained `qrhimetal.mm`
   patch (`chooseFormats` + colorspace + `CAEDRMetadata`). Worth filing, not
   worth blocking on.

Semantics note: a PQ-tagged surface is **absolute** (code 1.0 = 10 000 nits);
macOS tone-maps into current panel/EDR headroom. That restores the
absolute-nits review model on macOS *without* requiring a Reference Mode —
with the usual caveat that above-headroom content is OS-tone-mapped rather
than clipped, so Reference Modes remain the strict-grading answer.

### Prior art: ImageScope (`alexfry/imagescope`)

A working macOS-native OCIO EDR viewer whose display end mirrors what OpenRV's
present window needs to become:

| ImageScope piece | Relevance to the OpenRV port |
|---|---|
| `MetalEDRImageView.swift` — `rgba16Float` drawable, `wantsExtendedDynamicRangeContent`, assignable `colorspace` | Proof of every surface tag OpenRV would use: extended-linear P3 (what QRhi provides), PQ, HLG, BT.2020, plus custom calibrated spaces (BT.1886 γ2.4, P3-D65 γ2.6) for display-managed **SDR** review surfaces |
| `DisplaySurfaceMapping.swift` — user-editable JSON regex rules mapping OCIO display name → surface tag, first match wins | Better selection model than a static `RV_HDR_PRESENT` env var: derive the surface from the active OCIO display/view, with user-overridable rules. Worth adopting once the macOS path lands |
| OCIO GPU pipeline → tagged drawable handoff | Same encode-in-view / tag-the-surface contract the Wayland branch's present modes formalize |

The custom-calibrated-space trick also generalizes the story beyond HDR: a
BT.1886 or γ2.6 P3 surface tag lets ColorSync manage SDR review spaces
end-to-end, replacing the ICC-profile guesswork in
`CGDesktopVideoDevice::colorProfile()` for the image plane.

---

## GL → Metal interop: IOSurface design

Replaces the `GlVkSharedImage` FD-import scheme. This is the standard macOS
cross-API mechanism (Chromium/Firefox/CEF all present GL content this way),
and it works on both Intel and Apple Silicon (Apple's GL is itself layered on
Metal on AS — same IOSurface machinery underneath).

```
  IOSurfaceCreate {width, height,
    kIOSurfacePixelFormat = '64RGAh' (RGBA16F), bytesPerElement = 8}
       │
       ├─ GL side (RV's existing QOpenGLWidget context, via
       │   QNativeInterface::QCocoaGLContext → NSOpenGLContext → CGLContextObj)
       │     glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex)
       │     CGLTexImageIOSurface2D(ctx, GL_TEXTURE_RECTANGLE_ARB,
       │         GL_RGBA16F, w, h, GL_RGBA, GL_HALF_FLOAT, surf, 0)
       │     attach to FBO → glBlitFramebuffer(presentFbo → iosurfFbo)
       │     glFlush()          // required for IOSurface coherency
       │
       └─ Metal side (QRhi Metal device)
             MTLTextureDescriptor {RGBA16Float, w, h, usage: ShaderRead}
             [device newTextureWithDescriptor:desc iosurface:surf plane:0]
             → QRhiTexture::createFrom({(quint64)mtlTex, 0})
             → existing present pipeline samples it
```

Notes vs the Vulkan version:

- **No dedicated-memory trap, no tiling parameter, no layout thrash.** Metal
  has no image layouts, so the copy-to-sample-image workaround (needed because
  QRhi left the Vulkan image `SHADER_READ_ONLY`) is unnecessary — sample the
  IOSurface-backed texture directly. If flicker ever shows up, the same
  copy-to-private-texture trick is available, but don't start there.
- **Sync**: `glFlush()` after the blit is the documented IOSurface publish;
  keep the branch's `glFinish()` initially for determinism, relax later
  (same "semaphores later" TODO as Wayland — here it would be
  `MTLSharedEvent`, or simply double-buffered IOSurfaces).
- **Rectangle texture**: `CGLTexImageIOSurface2D` binds to
  `GL_TEXTURE_RECTANGLE` only. Attaching a rectangle texture to an FBO and
  `glBlitFramebuffer` into it is fine on Apple GL; the Metal side sees a
  normal 2D texture, so the present shader's normalized UVs are unaffected.
  (UV flip stays: GL FBO is bottom-up, `params.w` already handles it.)
- **Formats**: `'64RGAh'` (RGBA16F) for `p3extended`; RGBA8 variant
  (`'RGBA'`/BGRA) for the 8-bit modes, matching the branch's format matrix.
- Resize = drop and recreate surface + both textures (same lifecycle as the
  branch's `ensureGpuInterop`).

---

## What gets simpler on macOS

| Wayland pain | macOS status |
|---|---|
| QOpenGLWidget composite black on NVIDIA/Wayland (forced `QT_WIDGETS_RHI_BACKEND=vulkan`, CPU overlay fallback) | Not a thing — QOpenGLWidget composition works on macOS. The overlay exists purely to gain the EDR surface, not to fix broken composition |
| SDR-white nits guessing (203 vs 200, env overrides) | Display-referred: 1.0 = SDR white, done |
| Compositor variability (Hyprland vs KDE vs GNOME CM maturity, HDR10 fallback logs) | One compositor (WindowServer), EDR stable since 2016-era macOS |
| `hdrInfo` sometimes absent | Always available: `NSScreen.maximumExtendedDynamicRangeColorComponentValue` (+ `maximumPotential…`) per screen |
| Fractional DPR framing | Retina DPR = 2.0 integer |
| Wrapper-script env for QPA/RHI selection | None needed |

New macOS-only considerations:

- **Dynamic headroom.** Max EDR component value changes with the brightness
  slider / ambient (XDR at low brightness reports up to ~16×; at max
  brightness ~2×; SDR-only panels report 1.0). Poll `hdrInfo()` per frame or
  on `NSApplicationDidChangeScreenParametersNotification`-equivalent
  (`QWindow::screenChanged` + a timer) and surface it in the HUD — clipping
  above headroom is silent otherwise. Long-term this is the input to a
  display-adaptive OCIO view choice.
- **Multi-display**: headroom and EDR capability are per-`NSScreen`;
  re-evaluate on window screen change (the branch already rebuilds the
  swapchain on expose/resize — add screen-change to that trigger set).

---

## Qt / widget-stack considerations

- **Backing-store API mixing**: with a `QOpenGLWidget` in the window, Qt's
  widget composition uses the OpenGL RHI backend for that top-level, while the
  embedded present `QWindow` owns its own Metal layer. These coexist — the
  container window is a separate native view/layer, composited by
  WindowServer, same separation the Wayland subsurface relies on. Risk is
  low but this is the first thing to smoke-test (Phase 0 below).
- **`createWindowContainer` on macOS** historically has stacking/clipping
  quirks (it becomes a child `NSView`). RV's usage — full-rect overlay inside
  a `QStackedLayout`, no scrolling, no reparenting — is the benign case.
  Fullscreen and `RvDocument`'s menu-bar fullscreen path need explicit testing.
- **`CVDisplayLink`** (`DisplayLink.cpp`) is deprecated as of macOS 15 in
  favor of per-view `CADisplayLink`. Unrelated to EDR, but if present pacing
  work happens here anyway, prefer `QWindow::requestUpdate` (Qt drives it off
  the display link internally) rather than adding new CVDisplayLink usage.
- **Qt pin**: main pins Qt 6.8.3 — sufficient (EDR formats verified against
  the 6.8 branch source). No need for the 6.11 system-Qt arrangement the Arch
  branch uses, though nothing breaks with newer Qt either.

---

## GL-on-macOS constraints (upstream renderer side)

- RV requests a legacy/compatibility `QSurfaceFormat` (no core profile set —
  see `GLView::rvGLFormat`), which on macOS yields **GL 2.1 + Apple
  extensions**. Everything the HDR path needs exists there:
  `EXT_framebuffer_object`, `EXT_framebuffer_blit`, `ARB_texture_float`
  (RGBA16F FBO), `ARB_texture_rectangle`, `CGLTexImageIOSurface2D`.
  `QOpenGLFramebufferObject` with an RGBA16F internal format works on this
  stack — same class the branch already uses for the float present FBO.
- OpenGL is deprecated on macOS but fully functional through Sequoia/Tahoe,
  including Apple Silicon. Strategically, this work *reduces* GL exposure:
  presentation moves to Metal, and the interop boundary (IOSurface blit) is
  exactly where a future Metal renderer would slot in.

---

## Stock (non-OCIO) path — one thing to verify early

The Wayland stock path forces DisplayIPNode to PQ-encode (`RV_HDR=1` →
scene-linear ×(nits/100) → ST.2084). On macOS with a `p3extended`-style
default, the stock path instead wants: display transform (sRGB/P3 encode)
that **does not clamp** encoded values at 1.0, so scene-linear 4.0 →
encoded ≈1.825 survives into the RGBA16F FBO and the present shader's
extended EOTF (mode 3) restores it. Two things to check on day one:

1. `RVDisplayColor` / display shader clamp behavior above 1.0 (a
   `clamp(c, 0.0, 1.0)` anywhere in the display chain silently kills EDR).
   The Wayland branch verified this for the OCIO path (Display P3
   Un-tone-mapped: 1.825 → 4.0); the stock sRGB path needs the same check.
2. 30-bit / "billions of colors" display setting is *not* required — the
   swapchain is RGBA16F regardless of panel bit depth claims.

OCIO guidance is unchanged from the Wayland notes: `p3extended` pairs with
**Display P3** (gamma-encoded) views; Rec.2100-PQ views pair with the
PQ-decode present mode. ACES 2.0 configs via the OCIO 2.5.2 bump carry over.

---

## Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| `createWindowContainer` overlay misbehaves over QOpenGLWidget on macOS (stacking, fullscreen, spaces) | Medium | Phase 0 spike; fallback = present window as the *only* video surface (GL renders offscreen FBO only), which the FBO-redirect plumbing already supports |
| IOSurface GL binding fails for RGBA16F on some GPU | Low (mechanism used by every browser) | CPU half-float fallback already exists and is the bring-up path anyway |
| GL↔Metal sync artifacts (torn/stale frames) | Medium | Start `glFinish`, double-buffer IOSurfaces if needed |
| Display chain clamps >1.0 before FBO (stock path) | Medium | Early probe with wedge EXR (`RV_GL_PROBE` equivalent reading float FBO) |
| EDR clipped because user brightness maxed (headroom →1.0) | Certain to be *reported* as a bug eventually | HUD note showing live headroom from `hdrInfo()`; docs |
| Qt widget RHI backend conflicts (GL widgets + Metal child window) | Low | Phase 0 spike; worst case `QT_WIDGETS_RHI_BACKEND=metal` + GL-widget-as-texture path still exists in Qt |
| ICC/ColorSync double-management (RV's `CGDesktopVideoDevice::colorProfile()` display-profile logic vs EDR layer tagging) | Low | EDR layer colorspace bypasses per-profile guessing; ensure RV's "use display ICC" option is documented as SDR-path-only |

---

## Suggested phases

Ordering assumes the Wayland branch's platform-neutral refactors (GLView
handoff, float FBO, present widget, env plumbing) land first or are
cherry-picked; the macOS work is then additive.

| Phase | Scope | Est. |
|---|---|---|
| **0 — Spike** | Hello-EDR: bare `QWindow(MetalSurface)` + QRhi Metal + `HDRExtendedDisplayP3Linear`, render a >1.0 gradient; embed via `createWindowContainer` over a live QOpenGLWidget in a toy app. Confirms the two structural risks in a day. | 1–2 days |
| **1 — CPU present** | Gate present widget on `PLATFORM_DARWIN`: QRhi Metal backend selection, `p3extended` default, skip Vulkan instance. Existing half-float readback path → EDR on screen end-to-end. Wedge EXR + OCIO Display P3 view validation. | 2–4 days |
| **2 — IOSurface interop** | `GlMtlSharedImage` (Obj-C++), wire into `ensureGpuInterop`/`blitFromGlFramebuffer`/present. Perf target: same as Wayland (locked 24 large-window). | 3–5 days |
| **3 — PQ mode + polish** | Native PQ surface via the `CAMetalLayer.colorspace` override shim (+ optional `edrMetadata`); PQ-decode mode as A/B partner; per-screen headroom in HUD; screen-change swapchain rebuild; stock-path clamp audit; OCIO-display→surface mapping rules (ImageScope model); docs (`HDR-MACOS.md` field notes as bring-up proceeds). | 3–5 days |

Total: roughly **2–3 weeks** of focused work given the Wayland branch exists —
the majority of the design risk was already retired there.

---

## Validation plan

- **Hardware**: any MacBook Pro 14/16 (M1 Pro onward, XDR panel, 1600-nit
  peak / ~8× headroom at default brightness) is sufficient; Pro Display XDR
  for reference modes. Studio Display is *not* an EDR validation target
  (headroom ≈ 1.0 at max brightness).
- **Reuse** `_hdr_test/hdr_wedge_1080.exr` (1.0 / 10.0 / 4.0 thirds).
  Expected on `p3extended` + OCIO Display P3 Un-tone-mapped: encoded
  1.0 / ~2.34 / ~1.825 in the FBO → EDR 1.0 / 10 / 4 (10 clipped to current
  headroom by the OS).
- **Cross-check** against Preview.app / QuickTime displaying an HDR still or
  video on the same screen, and Apple's EDR debug overlay
  (Xcode → Debug → "EDR usage") to confirm the layer is actually in EDR mode.
- Screenshots tone-map EDR (same caveat as `grim` on Wayland) — trust the
  panel.
- A/B CPU vs interop identical to the Wayland recipes
  (`RV_HDR_GL_VK_INTEROP=0` → rename to a neutral `RV_HDR_GPU_INTEROP`).

---

## Fact base / references

- Qt 6.8 Metal swapchain EDR support, formats, colorspaces, `hdrInfo()`
  semantics: `qtbase` branch `6.8`, `src/gui/rhi/qrhimetal.mm`
  (`isFormatSupported` ~6194, `chooseFormats` ~6231, colorspace/EDR flag
  ~6286, `hdrInfo` ~6403, `QMetalTexture::createFrom` ~3872).
- [QRhiSwapChain docs](https://doc.qt.io/qt-6/qrhiswapchain.html),
  [QRhiSwapChainHdrInfo docs](https://doc.qt.io/qt-6/qrhiswapchainhdrinfo.html)
- Apple: [Explore HDR rendering with EDR (WWDC21)](https://developer.apple.com/videos/play/wwdc2021/10161/),
  CAMetalLayer `wantsExtendedDynamicRangeContent`, `CGLTexImageIOSurface2D`,
  `NSScreen.maximumExtendedDynamicRangeColorComponentValue`.
- Prior art for the IOSurface GL↔Metal share: Chromium's macOS compositor
  path ([HDR on macOS tracking issue](https://issues.chromium.org/issues/41466723)).
- Reference implementation: this repo's Wayland branch
  `alexfry/arch-qt611-wayland-build` + `_hdr_test/HDR-WAYLAND.md`.
- macOS-native prior art: `alexfry/imagescope` — `MetalEDRImageView.swift`
  (EDR drawable + assignable surface colorspace incl. PQ/HLG/custom BT.1886),
  `DisplaySurfaceMapping.swift` (OCIO display → surface tag rules).
