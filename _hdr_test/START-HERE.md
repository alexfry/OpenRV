# START HERE — HDR / EDR present-surface work

Handoff note for a session picking this branch up cold. The docs added here
are **design only** — no new code yet — but they now sit *on top of* the
working Wayland implementation rather than beside it. Read this file first,
then work in the order below.

---

## 1. What this branch is

Design work for unifying **desktop HDR presentation** across Linux (Wayland /
Vulkan) and macOS (EDR / Metal) in one fork, driven from OCIO display
colorspaces.

It is branched from `alexfry/arch-qt611-wayland-build` — Wayland + Vulkan HDR,
brought up on Arch / Hyprland / NVIDIA / dual Dell S3225QC / Qt 6.11 / OCIO
2.5.2. That implementation works today and is present in this tree; the design
docs plan how to generalize it to a second platform.

### Reading order

| # | Doc | What it is |
|---|---|---|
| 1 | `HDR-SURFACE-DESIGN.md` | **The main document.** Cross-platform design: the three-layer insight, canonical space vocabulary, resolution and fallback, platform backends, OCIO integration, mapping file, UI strategy, phasing |
| 2 | `HDR-MACOS-EDR.md` | macOS feasibility and effort estimate; IOSurface interop design |
| 3 | `HDR-WAYLAND.md` | Pre-existing Linux field notes from the Wayland base (env vars, A/B recipes, pitfalls). Not written as part of this design work |

### The design in three sentences

Both platforms have a **color-management layer** (Wayland
`wp_color_manager_v1`, macOS ColorSync) far richer than the graphics-API
swapchain enums Qt exposes; the abstraction belongs there, and Qt/QRhi gets
demoted to "put a float buffer on screen". The **OCIO display colorspace
determines the surface tag** — it is derived, not separately chosen. Spaces
neither platform names (Rec.709 D65 γ2.4) resolve via named/parametric
primitives where possible and synthesized **ICC** where not.

---

## 2. Verified vs assumed — read before changing any conclusion

Several conclusions here were checked against primary sources, and two earlier
*wrong* conclusions were corrected after pushback. **Do not re-derive these
from memory, and do not "correct" them back.** If you disagree, check the
cited source first.

### Verified against source

| Claim | Source |
|---|---|
| Qt Metal supports EDR swapchains: `HDRExtendedSrgbLinear` / `HDRExtendedDisplayP3Linear` → `MTLPixelFormatRGBA16Float` + `kCGColorSpaceExtendedLinear{SRGB,DisplayP3}` + `wantsExtendedDynamicRangeContent` | qtbase `6.8`, `src/gui/rhi/qrhimetal.mm`: `isFormatSupported` ~6194, `chooseFormats` ~6231, `createOrResize` ~6286 |
| Qt Metal exposes **no** HDR10/PQ swapchain format | same, `isFormatSupported` ~6194 |
| Qt Metal `hdrInfo()` is `ColorComponentValue` + `DisplayReferred`; **`sdrWhiteLevel = 200` is a hardcoded dummy** — never consume it on Apple | same, `hdrInfo` ~6403 |
| Headroom comes from `NSScreen.maximumExtendedDynamicRangeColorComponentValue` / `maximumPotential…` | same |
| QRhi Metal can adopt a native texture (`QRhiTexture::createFrom`) — needed for IOSurface interop | same, `QMetalTexture::createFrom` ~3872 |
| Qt Vulkan bakes colorspace at swapchain creation; format chosen from enumerated surface formats; no `PASS_THROUGH` path | qtbase `6.8`, `qrhivulkan.cpp`: `imageColorSpace` assignment line 1924, `hdrFormatMatchesVkSurfaceFormat` ~7956, `isFormatSupported` ~7974, `ensureSurface` ~8074 |
| `VkColorSpaceKHR` full value list, incl. `PASS_THROUGH_EXT` and `BT709_NONLINEAR_EXT` (the 709 **OETF**, not a 2.4 EOTF) | Vulkan-Headers `main`, `vulkan_core.h` ~8945–8966 |
| Wayland CM protocol vocabulary — TF enum (incl. `bt1886`, `compound_power_2_4`, deprecated `srgb`/`ext_srgb`), primaries enum, `feature` enum, creator-params requests incl. `set_tf_power` / `set_primaries` | `color-management-v1.xml` (staging v1) — fetched via the `godotengine/godot` third-party mirror; freedesktop GitLab is egress-blocked from the cloud container |
| Real-world usage of the above | mpv `video/out/wayland_common.c`: `map_tf` ~2073, `map_primaries` ~2103, image-description creation ~3684 |
| ACES 2.0 studio config display list, structured aliases (`g24_rec709_display` etc.) and `encoding:` values | `studio-config-v4.0.0_aces-v2.0_ocio-v2.5.ocio`, shipped in the `alexfry/imagescope` repo under `OCIO/` |
| macOS can tag any `CGColorSpace` incl. PQ and custom calibrated (BT.1886, P3-D65 γ2.6) | `alexfry/imagescope`: `MetalEDRImageView.swift` (`colorSpaceForIdentifier`, `createBT1886ColorSpace`, `createP3D65ColorSpace`) |
| OCIO #1996 is open, Feature Request / Needs Discussion, milestone 2.6.0, **no API sketch yet** | the issue itself |
| RV already publishes device color info into session properties | `DisplayGroupIPNode::setPhysicalVideoDevice` ~line 109 (`device.systemProfileURL` / `device.systemProfileType`) |

### Assumed — still needs proving

| Assumption | Risk | How to settle |
|---|---|---|
| **An app-set Wayland image description survives alongside a Mesa-WSI-managed Vulkan swapchain** | **High — drives the Linux architecture** | Spike A below. Sourced only from Vulkan-Docs issue #2307, which says "presumably `PASS_THROUGH` is the value that guarantees no `wp_color_management_surface_v1` is created". Not tested |
| ~~`createWindowContainer` overlay over a live `QOpenGLWidget` behaves on macOS~~ | — | **Settled — Spike B passed.** See below |
| `QNativeInterface::QWaylandWindow::surface()` availability in the pinned Qt | Low | Check the Qt version in use |
| IOSurface RGBA16F ↔ GL binding works on all target GPUs | Low | Every browser does this; CPU fallback exists regardless |
| Compositors advertise `set_tf_power` (needed for γ2.6) | Medium | Query the `feature` enum at bind; named `bt1886` covers γ2.4 without it |
| All effort estimates in §10 of the design doc | — | They are estimates, not measurements |

### Corrections already made — do not revert

1. **"Metal can't do PQ" was wrong** (fixed in `9c0cec6`). Qt's *QRhi enum*
   has no HDR10 on Metal; Metal/CoreAnimation happily takes
   `kCGColorSpaceITUR_2100_PQ` or a custom space via `CAMetalLayer.colorspace`.
   ImageScope ships this in production. The fix is a ~30-line shim overriding
   the layer colorspace after QRhi creates the swapchain.
2. **"Surface space is a property of the output device" was the wrong
   framing** (fixed in `b7c50a2`). The OCIO display colorspace determines the
   tag; the device contributes *capability* and *managed-vs-pass-through
   policy*, not identity. This is why there is deliberately **no "Output
   Color Space" picker** in the UI design — two controls that can disagree
   produce a plausible-looking, wrongly-transformed image.
3. **The mapping file is the mechanism, not a fallback** (fixed in
   `9ea52e4`). ACES 2.0 alias derivation is a good default that keeps the file
   empty for well-named configs; arbitrary configs are the common case.

---

## 3. The phase-0 spikes

Both are go/no-go on architecture. **Spike B is done and passed**; Spike A is
still outstanding and needs the Arch box. Neither can run in a cloud container
— see §4.

### Spike A — Linux tagging authority (½–1 day, Arch/Hyprland box)

**Question:** can we tag the surface ourselves while QRhi owns the Vulkan
swapchain, or must we take over swapchain creation to get `PASS_THROUGH`?

1. On the working Wayland branch, keep the QRhi swapchain
   (`HDRExtendedSrgbLinear`).
2. Bind `wp_color_manager_v1` directly (`QNativeInterface::QWaylandApplication`
   for the `wl_display`; `QNativeInterface::QWaylandWindow` for the
   `wl_surface`).
3. Build an image description — start with named `srgb` primaries +
   `bt1886` TF, the Rec.709 γ2.4 target — and
   `set_image_description` on the surface.
4. Observe: does it stick, or does Mesa's WSI overwrite it on the next
   present? Check with `wp_color_management_surface_feedback_v1` and by eye.

**Outcome decides:** sticks → keep QRhi's swapchain (simplest). Doesn't stick
→ pick between owning the `VkSwapchainKHR` (preferred) or patching
`qrhivulkan.cpp` (§6 of the design doc).

### Spike B — macOS embedding — **DONE, PASSED**

Implemented in `spike_b/`. Standalone (no OpenRV deps), builds in seconds:

```bash
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.1/macos"
cmake --build build && ./build/spike_b
```

Measured on M4 Pro / built-in Liquid Retina XDR / Qt 6.11.1 / Xcode 26.6:

| Check | Result |
|---|---|
| `QRhi::create(QRhi::Metal)` | OK |
| `HDRExtendedDisplayP3Linear` `isFormatSupported()` | **true** — no SDR fallback |
| `createWindowContainer` over live `QOpenGLWidget` in `QStackedLayout(StackAll)` | **works** — container sized, Metal window exposed, correctly stacked |
| EDR headroom (`NSScreen.maximumExtendedDynamicRangeColorComponentValue`) | **2.95** |
| EDR potential (`maximumPotential…`) | **16** |
| Steps above 1.0 visibly brighter on the panel | **confirmed by eye** |
| Fullscreen (`F`) | **works** — stacking survives the transition |

**Conclusion: phase 3 proceeds as designed.** The fallback (present window as
the *only* video surface) is not needed.

Note the spike queries `NSScreen` directly and deliberately ignores
`QRhiSwapChain::hdrInfo()` — on Metal Qt returns a hardcoded dummy
`sdrWhiteLevel = 200`, so consuming it would mislead. See `edrinfo.h`.

Still open from the original spike list: dragging between displays with
different headroom (open question #3 below) — **untested, only one display
available on the test machine**. Note the spike also measured headroom of
**2.95 current vs 16 potential** on the same panel, so headroom varies over
time regardless of which display the window is on. Question #3 is therefore
not purely a multi-monitor problem.

Then follow `HDR-SURFACE-DESIGN.md` §10 phasing.

---

## 4. Where work can happen

**macOS builds now work.** The tree configures and builds on macOS (Qt 6.11.1
via `aqt`, CMake 3.31.7, `RV_ALIGN_PYSIDE=ON`); `RV -version` runs and the
present overlay is correctly inert on Cocoa. Getting there needed three fixes,
all on this branch: the Vulkan present path confined to Linux, a libclang
fallback for modern Xcode, and OIIO's Nuke plugins disabled.

| Work | Machine | Why |
|---|---|---|
| Phase 1 lib: canonical registry, colour math, **mapping-file parser**, **ICC builder**, alias table + unit tests | **Anywhere**, incl. cloud | Deliberately platform-neutral: no Qt, no GL, no platform deps. Builds standalone with `g++`/`cmake`. Colour math and parsing are exactly where a display doesn't help and unit tests do |
| Wayland tagger, Linux visual validation | **Arch/Hyprland box** | Needs a real HDR panel and a colour-managing compositor |
| Everything macOS (phase 3) | **Mac + Xcode** | Metal toolchain, Obj-C++, EDR panel |
| Full OpenRV build | **Local** | See below |

**The cloud container cannot build OpenRV.** Measured, not assumed:
`download.qt.io` unreachable and no system Qt; `codeload.github.com` and
GitHub release assets return **403**; `download.osgeo.org` / `libraw.org`
unreachable; `sourceforge.net` 403. With 38 source dependencies, `RV_DEPS_*`
cannot populate. 30 GB free / 4 cores is also thin for a full build.

**More fundamentally, none of this work can be *validated* in the cloud.** HDR
presentation needs a physical HDR panel and a colour-managed compositor, and
per `HDR-WAYLAND.md` screenshots tone-map — you have to look at the panel.

---

## 5. Still genuinely open

Beyond the spikes:

1. **Named vs ICC preference** when both work — named is cheaper and more
   likely to hit a compositor fast path; ICC is more precise. Default named,
   allow per-space override.
2. **Where the residual transform lives** — present shader (proposed) vs
   appended to the OCIO GPU pass.
3. **Multi-display**: surface tag is per-device, but a window dragged between
   displays keeps its tag while the *preferred* description changes. Pin and
   warn, or follow? Design suggests pin + HUD warning.
4. **`SurfaceCapabilities` shape** — worth designing carefully, since it is
   what an eventual OCIO negotiation API would consume. Taking a real
   two-platform implementation to OCIO #1996 is the strongest contribution
   available.

---

## 6. Branch state

Branch: `claude/macos-edr-display-assessment-hxid10`, branched from
`alexfry/arch-qt611-wayland-build` (originally cut from `main` and rebased
onto the Wayland branch, so the working Vulkan/Wayland HDR present path is in
this tree). The six commits this branch adds on top are **documentation
only — no code changes**; everything below `48858444 docs: complete HDR
Wayland / GPU interop field notes` is the Wayland implementation.

`_hdr_test/` therefore holds both the design docs and the pre-existing Wayland
notes, README, and test wedge material (`hdr_wedge_1080.exr`,
`hdr_white_step1.exr`, `write_hdr_exr.cpp`).

Related: `alexfry/imagescope` (macOS-native EDR viewer used as prior art —
`MetalEDRImageView.swift`, `DisplaySurfaceMapping.swift`, and a copy of the
ACES 2.0 config).
