# Cross-platform present-surface color management — design

One fork, one vocabulary of **buffer spaces**, two native backends:
Wayland/Vulkan on Linux, EDR/Metal on macOS. Supersedes the ad-hoc
`RV_HDR_PRESENT` mode enum on `alexfry/arch-qt611-wayland-build`.

Companion docs: `HDR-WAYLAND.md` (Linux bring-up), `HDR-MACOS-EDR.md`
(macOS feasibility).

---

## 1. The insight the design rests on

There are **three** layers between the IP graph and the panel, and they have
very different vocabularies. The current branch is written against the
narrowest one.

| Layer | Linux | macOS | Rec.709 D65 γ2.4? |
|---|---|---|---|
| **A. Qt QRhi swapchain enum** | `SDR`, `HDR10`, `HDRExtendedSrgbLinear`, `HDRExtendedDisplayP3Linear` | same enum, Metal backend supports `SDR` + the two extended-linear ones (no HDR10) | **No** |
| **B. Graphics-API color space** | `VkColorSpaceKHR` (16 values: `SRGB_NONLINEAR`, `DISPLAY_P3_NONLINEAR`, `BT709_NONLINEAR`, `HDR10_ST2084`, `HDR10_HLG`, `EXTENDED_SRGB_LINEAR`, `PASS_THROUGH`, …) | `CAMetalLayer.colorspace` = **any** `CGColorSpace` | Linux: no (`BT709_NONLINEAR` is the 709 *OETF*, not a 2.4 display EOTF). macOS: yes |
| **C. Platform color-management layer** | `wp_color_manager_v1` image descriptions: named primaries + named TF, **parametric** (`set_tf_power`, `set_primaries`), or **ICC v2/v4** | ColorSync: named `CGColorSpace`, calibrated RGB, or **ICC data** | **Yes, on both** |

Layer A is a lowest-common-denominator abstraction and is where the current
branch lives — hence the "PQ is impossible on macOS" conclusion in the first
draft of `HDR-MACOS-EDR.md`, and hence no Rec.709-γ2.4 anywhere.

**The abstraction belongs at layer C.** Both platforms converge on the same
contract there:

> Hand the compositor a **float buffer** plus a **description of what the
> numbers in it mean**. It color-manages to the panel.

Layers A/B are then demoted to "get me a float buffer on screen", which is all
QRhi is actually good at, and the shaders stay portable.

### What each platform accepts at layer C

**Wayland** `wp_color_manager_v1` (verified against protocol XML, staging v1):

- Named primaries: `srgb` (= BT.709/D65), `pal_m`, `pal`, `ntsc`,
  `generic_film`, `bt2020`, `cie1931_xyz`, `dci_p3` (DCI white),
  `display_p3` (P3/D65), `adobe_rgb`.
- Named transfer functions: `bt1886`, `gamma22`, `gamma28`, `st240`,
  `ext_linear`, `log_100`, `log_316`, `xvycc`, `st2084_pq`, `st428`, `hlg`,
  `compound_power_2_4` (since v2 — the IEC 61966-2-1 / sRGB piecewise curve).
  `srgb` (9) and `ext_srgb` (10) are **deprecated since v2** — do not emit.
- `wp_image_description_creator_params_v1` requests: `set_tf_named`,
  **`set_tf_power`**, `set_primaries_named`, **`set_primaries`** (custom
  chromaticities), `set_luminances`, `set_mastering_display_primaries`,
  `set_mastering_luminance`, `set_max_cll`, `set_max_fall`.
- Compositor advertises what it will accept via the `feature` enum:
  `icc_v2_v4`, `parametric`, `set_primaries`, `set_tf_power`,
  `set_luminances`, `set_mastering_display_primaries`,
  `extended_target_volume`, `windows_scrgb`. **Query before use** — a
  compositor may support named-only.
- Attach with `wp_color_management_surface_v1::set_image_description` on the
  `wl_surface`; `wp_color_management_surface_feedback_v1::preferred_changed`
  reports what the display currently prefers.

**macOS** ColorSync / `CAMetalLayer.colorspace`:

- ~25 named `CGColorSpace`s including `itur_2100_PQ`, `itur_2100_HLG`,
  `itur_2020`, `displayP3`, `extendedLinearDisplayP3`, `dcip3`.
- `CGColorSpace(calibratedRGBWhitePoint:blackPoint:gamma:matrix:)` for
  arbitrary primaries + power gamma (ImageScope's `createBT1886ColorSpace`,
  `createP3D65ColorSpace`).
- `CGColorSpace(iccData:)` for anything else.
- `wantsExtendedDynamicRangeContent = true` + `rgba16Float` for EDR.

### The unifier for arbitrary spaces: ICC

Anything not expressible with named/parametric primitives on **both**
platforms is expressible as an **ICC v4 profile**, which Wayland accepts
(`icc_v2_v4` feature, `wp_image_description_creator_icc_v1`) and macOS accepts
(`CGColorSpace(iccData:)`). Windows accepts it too, when that day comes. So
the fallback ladder for any canonical space is:

```
named  →  parametric  →  synthesized ICC  →  approximate + residual shader transform  →  SDR
```

---

## 1.5 Who decides what

Three roles, and keeping them separate is the whole design:

| Role | Owns | Does **not** own |
|---|---|---|
| **OCIO display colorspace** (session color pipeline) | *What the numbers in the buffer mean.* This **is** the surface descriptor — 1:1, derived, not independently chosen. | Whether the platform can express it |
| **Device / platform** | *Capability* (is this descriptor expressible exactly, approximately, not at all? what is the EDR headroom? what does the display prefer?) and *policy* (color-managed vs pass-through; which fallback is acceptable) | Which color space the image is in |
| **ColorSync / compositor** | Surface tag → panel | Everything upstream |

The surface tag therefore **follows the color pipeline**, and changes whenever
the user changes OCIO display or view. It is dynamic session state, not a
static device preference.

The device's legitimate influence runs the other way: capability should
**filter or recommend** the OCIO display list (offering `Rec.2100-PQ` on an
SDR panel is legal but pointless), and the chosen display then determines the
surface. Recommendation is advisory; it must never silently override the
user's display choice.

One genuinely device-level decision survives, and it is not "which color
space": **management policy** — color-managed (tag truthfully, let ColorSync
or the compositor convert) versus pass-through (send code values untouched to
a calibrated display, the traditional DI model). That is a per-device,
per-facility preference and belongs in device state.

Performance corollary: **re-tagging is cheap, reformatting is not.** Changing
`CAMetalLayer.colorspace` or attaching a new Wayland image description costs
nothing; changing buffer format (RGBA8 ↔ RGBA16F) rebuilds the swapchain.
Group the canonical spaces by required buffer format and only rebuild when a
display change crosses a format boundary.

---

## 2. Canonical buffer-space vocabulary

Platform-neutral IDs. This is not an independent user-facing menu — it is the
platform-expressible image of the config's display colorspaces (§2.1).

| ID | Primaries / white | Transfer | Range | Notes |
|---|---|---|---|---|
| `srgb` | BT.709 / D65 | IEC 61966-2-1 | SDR relative | Safe default |
| `rec709-bt1886` | BT.709 / D65 | pure 2.4 (BT.1886, Lb=0) | SDR relative | **Broadcast/reference SDR** |
| `rec709-g22` | BT.709 / D65 | pure 2.2 | SDR relative | |
| `display-p3` | P3 / D65 | IEC 61966-2-1 | SDR relative | Apple Display P3 |
| `p3d65-g26` | P3 / D65 | pure 2.6 | SDR relative | Cinema on a D65 display |
| `dcip3-g26` | P3 / DCI white | pure 2.6 | SDR relative | True DCI |
| `rec2100-pq` | BT.2020 / D65 | ST.2084 | **absolute** (cd/m²) | HDR10 |
| `p3d65-pq` | P3 / D65 | ST.2084 | **absolute** | P3-limited HDR master |
| `rec2100-hlg` | BT.2020 / D65 | HLG | scene relative | |
| `display-p3-ext` | P3 / D65 | extended IEC 61966-2-1 | **extended** (>1 = headroom) | macOS EDR classic; branch's `p3extended` |
| `linear-p3-ext` | P3 / D65 | linear | **extended** | branch's `p3linear` |
| `linear-srgb-ext` | BT.709 / D65 | linear | **extended** | scRGB; branch's `srgblinear` |

Descriptor carried in code (platform-neutral, no Qt, no GL):

```cpp
struct SurfaceSpace {
    std::string id;                  // "rec709-bt1886"
    std::string label;               // "Rec.709 D65 — Gamma 2.4 (BT.1886)"
    Chromaticities primaries;        // xy R,G,B,W  (always explicit)
    Transfer  transfer;              // {Power(g) | Srgb | Pq | Hlg | Linear | St428}
    Range     range;                 // SdrRelative | Extended | Absolute
    Luminance luminance;             // {min, max, refWhite} nits — optional
    Mastering mastering;             // optional: primaries, maxCLL, maxFALL
};
```

Chromaticities are **always** explicit even when a named platform primary
exists — that is what lets the resolver decide "named `srgb` is bit-exact for
this" versus "must go parametric/ICC", and it is what the residual matrix is
computed from.

### 2.1 Validation against ACES 2.0 (`studio-config-v4.0.0_aces-v2.0_ocio-v2.5`)

The nine active displays in the ACES 2.0 studio config, and how they resolve.
Note the **second alias** of each display colorspace follows a rigorous
`<transfer>_<primaries>_display` convention, and `encoding:` gives the range
class — so the descriptor is derivable from the config *today*, without
waiting for OCIO #1996.

| OCIO display | Structured alias | `encoding` | Canonical id | Wayland | macOS |
|---|---|---|---|---|---|
| `sRGB - Display` | `srgb_rec709_display` | sdr-video | `srgb` | named `srgb` + `compound_power_2_4` | named `sRGB` |
| `Gamma 2.2 Rec.709 - Display` | `g22_rec709_display` | sdr-video | `rec709-g22` | named `srgb` + `gamma22` | calibrated γ2.2 |
| `Display P3 - Display` | `srgb_p3d65_display` | sdr-video | `display-p3` | named `display_p3` + `compound_power_2_4` | named `displayP3` |
| `Display P3 HDR - Display` | `srgbe_p3d65_display` | **edr-video** | `display-p3-ext` | `display_p3` + ext transfer | named `extendedDisplayP3` |
| `P3-D65 - Display` | `g26_p3d65_display` | sdr-video | `p3d65-g26` | `display_p3` + `set_tf_power(2.6)` | `createP3D65ColorSpace()` |
| `Rec.1886 Rec.709 - Display` | `g24_rec709_display` | sdr-video | `rec709-bt1886` | named `srgb` + **`bt1886`** | `createBT1886ColorSpace()` |
| `Rec.2100-HLG - Display` | `hlg_rec2020_display` | hdr-video | `rec2100-hlg` | named `bt2020` + `hlg` | named `itur_2100_HLG` |
| `Rec.2100-PQ - Display` | `pq_rec2020_display` | hdr-video | `rec2100-pq` | named `bt2020` + `st2084_pq` | named `itur_2100_PQ` |
| `ST2084-P3-D65 - Display` | `pq_p3d65_display` | hdr-video | `p3d65-pq` | named `display_p3` + `st2084_pq` | ICC or calibrated + PQ |

Observations:

- The mapping is **1:1 and near-total**. The canonical vocabulary was not
  invented independently — it is what the config already describes.
- Every SDR/HDR case resolves to **named** primitives on Wayland and named or
  calibrated on macOS. `set_tf_power` (feature-gated) is needed only for γ2.6.
- `Display P3 HDR` / `srgbe_p3d65_display` / `edr-video` is *literally* the
  macOS EDR surface — the ACES config already ships a display designed for it.
- `encoding:` (`sdr-video`, `hdr-video`, `edr-video`, `display-linear`) maps
  directly onto the descriptor's `range` field.
- Views do **not** affect the surface tag — `ACES 2.0 - HDR 1000 nits (P3 D65)`
  and `Un-tone-mapped` on the same display produce different pixels in the
  same encoding. Views change tone mapping; displays change encoding. Only the
  display drives re-tagging. (Views *do* inform luminance metadata — see
  `set_mastering_luminance` / `CAEDRMetadata`.)

---

## 3. Resolution: descriptor → plan

One platform-neutral resolver interface, two backends. Output:

```cpp
struct SurfacePlan {
    BufferFormat   bufferFormat;   // RGBA16F | RGB10A2 | RGBA8
    NativeTag      tag;            // opaque: CGColorSpaceRef | wp_image_description_v1*
    Exactness      exactness;      // Exact | Approximate(reason) | Unavailable
    ResidualXform  residual;       // normally identity
};
```

### The residual transform

The branch's present shader has a 4-value `mode` enum (`0` pass, `1` PQ×scale,
`2` PQ→linear, `3` sRGB-EOTF→linear-P3). That enum grows quadratically with
the space list. **Replace it with a data-driven transform** — one shader, all
cases:

```
  decode(srcTransfer)  →  3×3 primaries matrix  →  scale  →  encode(dstTransfer)
```

driven by uniforms (`srcTransfer` id, `dstTransfer` id, `mat3`, `scale`). All
four existing modes fall out as special cases, and every new pairing is free.

**The residual is identity in the normal case** — OCIO (or `DisplayIPNode`)
already produced exactly the encoding the surface is tagged with. It is
non-identity only when:

1. the platform can't express the requested space and we fell back to a
   neighbour (e.g. no PQ surface → tag extended-linear, decode PQ in shader);
2. absolute↔relative rescaling is needed (the "203 nits" case, Linux only);
3. the user deliberately A/Bs a buffer against a different surface tag.

Non-identity residual is a **diagnostic-worthy state** — surface it in the HUD
and logs, because it means the display chain is doing math the color pipeline
didn't ask for.

### Fallback ladder

Try in order, stop at first success, record exactness:

1. **Named** platform primitive, if primaries + transfer both match exactly.
2. **Parametric** — Wayland `set_primaries` / `set_tf_power` (gated on the
   advertised `feature`); macOS `calibratedRGBWhitePoint:…:gamma:matrix:`.
3. **Synthesized ICC v4** from the descriptor, fed to both platforms.
4. **Approximate** — nearest expressible space + residual shader transform.
   Mark `Approximate` with a human-readable reason.
5. **SDR** with residual, and a loud log line.

---

## 4. Worked example: `rec709-bt1886`

The space that motivated this design. Neither platform has it at layer A or B;
both have it at layer C.

| | Linux / Wayland | macOS |
|---|---|---|
| Route | **Named**: `set_primaries_named(srgb)` + `set_tf_named(bt1886)` | **Parametric**: `CGColorSpace(calibratedRGBWhitePoint: D65, blackPoint: 0, gamma: [2.4,2.4,2.4], matrix: BT.709→XYZ)` |
| Exactness | Exact | Exact (BT.1886 with Lb=0 **is** a pure 2.4 power law) |
| Buffer | RGBA8 or RGB10A2 sufficient (SDR relative); RGBA16F fine | RGBA16F drawable, `wantsEDR` **off** |
| Residual | identity | identity |
| Fallback | parametric `set_tf_power(2.4)` + `set_primaries(709 xy)`; then ICC | ICC v4 (709 primaries + `para` curve g=2.4) |

ImageScope's `createBT1886ColorSpace()` is exactly route 2 and is proven in
production — reuse its constants. Note its `gamma:` argument is the
**decoding** exponent and its white point is D65 in XYZ; both correct.

Same shape for `p3d65-g26`: Wayland `display_p3` + `set_tf_power(2.6)`
(feature-gated), macOS `createP3D65ColorSpace()`.

---

## 5. Module layout

```
src/lib/base/TwkSurfaceSpace/          ← new; no Qt, no GL, no platform deps
  SurfaceSpace.h/.cpp        descriptor, canonical registry, id parsing
  SurfacePlan.h              resolved plan + exactness + residual
  ColorMath.h/.cpp           chromaticities → 3×3, transfer encode/decode refs
  IccBuilder.h/.cpp          synthesize minimal ICC v4 from a descriptor

src/lib/app/RvCommon/
  PresentSurface.h           ISurfaceTagger + ISurfaceCapabilities (abstract)
  PresentSurfaceCocoa.mm     CAMetalLayer.colorspace, NSScreen EDR headroom
  PresentSurfaceWayland.cpp  wp_color_manager_v1, image descriptions, feedback
  PresentWidget.{h,cpp}      ← renamed from VulkanPresentWidget; QRhi-backend-agnostic
  GlSharedImage.h            abstract GL→GPU-API handoff
  GlVkSharedImage.cpp        existing (Linux, external-memory fd)
  GlMtlSharedImage.mm        new (macOS, IOSurface)
  shaders/present.frag       data-driven residual (replaces mode enum)
```

`PresentWidget` keeps QRhi for pipeline/shaders/swapchain and delegates *all*
color decisions to `ISurfaceTagger`. Backend selection (`QRhi::Vulkan` vs
`QRhi::Metal`) becomes one factory call; everything else is shared.

---

## 6. Platform backends

### macOS — clean

Qt creates the swapchain (`HDRExtendedSrgbLinear` for a float+EDR layer, or
SDR); the Cocoa tagger then overrides `CAMetalLayer.colorspace` **after every
create/resize** (Qt assigns it in `createOrResize()`), plus
`wantsExtendedDynamicRangeContent` per the descriptor's range, and optionally
`edrMetadata` (`CAEDRMetadata`) for PQ/HLG mastering metadata.

Capabilities: `NSScreen.maximumExtendedDynamicRangeColorComponentValue` (live)
and `maximumPotential…`; re-query on screen change. Ignore Qt's
`hdrInfo().sdrWhiteLevel` on Apple — it is a hardcoded dummy (200).

### Linux — one open question

Tagging works the same way (bind `wp_color_manager_v1` via
`QNativeInterface::QWaylandApplication`, get the `wl_surface` from
`QNativeInterface::QWaylandWindow`, attach an image description). **The open
question is whether Mesa's WSI will fight us**: with a non-`PASS_THROUGH`
`VkColorSpaceKHR`, the Vulkan WSI sets its own image description on the same
surface. Vulkan-Docs issue #2307 indicates `VK_COLOR_SPACE_PASS_THROUGH_EXT`
is the value that guarantees no `wp_color_management_surface_v1` is created,
leaving the app in control — but QRhi cannot request `PASS_THROUGH`.

Three ways out, in order of preference:

1. **Spike first** (½ day): with QRhi's `HDRExtendedSrgbLinear` swapchain,
   attach our own image description and see whether it sticks (last-writer-
   wins) or is overwritten. If it sticks reliably on the target compositors,
   ship it and revisit.
2. **Own the swapchain** — drop QRhi's swapchain (keep QRhi for
   pipeline/shaders) and create the `VkSwapchainKHR` with `PASS_THROUGH`
   directly. Most control; the branch already carries substantial Vulkan code
   and the shaders are already SPIR-V via `qsb`.
3. **Patch Qt** — small, contained change to `qrhivulkan.cpp` to allow a
   caller-specified `VkColorSpaceKHR`. Worth upstreaming regardless; painful
   as a distribution dependency.

Capabilities: compositor `feature`/`supported_tf_named`/
`supported_primaries_named` events at bind, plus
`wp_color_management_surface_feedback_v1::preferred_changed` for the live
"what does this display want" signal — the Wayland analogue of NSScreen EDR
headroom.

### Windows (not now, but don't preclude it)

`IDXGISwapChain3::SetColorSpace1` + `DXGI_COLOR_SPACE_*` is layer B;
Windows Advanced Color / ICC is layer C. The same descriptor + resolver shape
applies. Keep `TwkSurfaceSpace` free of any two-platform assumptions.

---

## 7. Capability negotiation and OCIO (issue #1996)

[OCIO #1996](https://github.com/AcademySoftwareFoundation/OpenColorIO/issues/1996)
(Feature Request / Needs Discussion, milestone 2.6.0) asks for exactly the two
things this design needs, and has no concrete API yet:

1. display color spaces carrying enough metadata to apply an OS surface tag;
2. an API to negotiate between config tags and what the OS/display supports.

What #1996 does **not** change: the display colorspace is defined by a
*transform* (CIE XYZ-D65 → display encoding), not by declarative
primaries/transfer metadata. You cannot mechanically read "BT.709 primaries,
γ2.4" out of the config. What you *can* read — and what §2.1 shows is
sufficient in practice — is the **standardized alias** and the `encoding:`
field.

So the derivation runs as a **precedence chain**, first hit wins:

| Precedence | Source | Status |
|---|---|---|
| 1 | **Explicit user override** for this display (pinned in prefs) | Always wins; the escape hatch |
| 2 | **Declared metadata** — primaries/transfer published by OCIO | Requires #1996; read-if-present, ignore-if-absent |
| 3 | **Standard alias + `encoding`** — `g24_rec709_display` → descriptor, via a table shipped with RV | **Works today** with every ACES 2.0 built-in config |
| 4 | **Name-pattern rules file** — user-editable JSON, regex on display name, first match wins | Fallback for custom/legacy configs; ImageScope's `DisplaySurfaceMapping.swift` model |
| 5 | **Default** (`srgb`) + loud log | Last resort |

Tier 3 is the important correction to the first draft of this document: the
alias vocabulary is structured and stable, so the interim mechanism is a
*deterministic lookup table shipped with RV*, not studio-authored policy.
Tier 4 remains valuable but is demoted to handling configs that don't follow
the convention.

The `SurfaceCapabilities` struct is worth designing carefully now — it is
precisely what a tier-2 negotiation API would consume, and a real
implementation across two platforms is the strongest possible contribution to
that discussion. Worth taking to the OCIO issue once tier 3 is working.

The stock (non-OCIO) path maps through `DisplayIPNode` the same way: its
output encoding determines the surface id, replacing the current
`RV_HDR=1 → force PQ` special case.

---

## 8. UI strategy — recommendation

**There is no "Output Color Space" picker.** That is the main UI consequence
of §1.5, and it makes the UI much smaller than a first pass suggests.

The user already has the control that selects the surface: the **OCIO display**
(`View → Display` / the display+view menus RV ships today). Adding a second
picker would create two controls that can silently disagree — and the failure
mode of disagreement is a double-transformed image that looks plausible but
is wrong, which is the single worst outcome in a review tool. The surface tag
must be **derived and shown**, not independently chosen.

So the UI splits into three things, none of which is a color-space menu:

1. **Status (always visible enough to trust).** What the image plane is
   currently tagged as, and how faithfully: `Rec.1886 Rec.709 → bt1886 ·
   Exact` or `Rec.2100-PQ → extended-linear P3 · Approximate (no PQ surface;
   decoding in present shader)`. A non-identity residual transform is a
   warning state and must be visible — it means the display chain is doing
   math the color pipeline didn't ask for.
2. **Override (rare, per display, sticky).** For when derivation is wrong,
   the config is non-standard, or a fallback needs pinning. This is the only
   place a canonical id is user-selectable, and it should read as an
   override — showing the derived value it replaces.
3. **Device policy (per device, genuinely device-level).** Color-managed vs
   pass-through, and the fallback preference. This *is* device state and
   belongs in Preferences → Video alongside the existing data-format and
   color-profile settings.

### Where each piece lives

| Piece | Home | Why |
|---|---|---|
| Derivation, capability query, tagging | **Core C++** | Must run on every display change, before first frame |
| Session properties | **Core C++** | `DisplayGroupIPNode` already publishes `device.systemProfileURL`/`device.systemProfileType`; add `device.outputColorSpace`, `.exactness`, `.residualTransform`, `.edrHeadroom`, `.preferredSurfaceSpace`. Scriptable and serializable for free |
| Device policy (managed/pass-through, fallback pref) | **Native — Preferences → Video** | Genuinely per-device; sits with `dataFormatAtIndex()`/`ColorProfile` |
| Commands (`outputColorSpaceInfo`, `setOutputColorSpaceOverride`, …) | **Core C++** → Mu/Python | Needed by any UI, native or package |
| Status readout, override menu, diagnostics HUD, rules-file editor, A/B hotkeys | **Package** `display_surface_tools` | Iterates weekly during bring-up; studio-replaceable; `commands.writeSettings` for persistence, like `custom_lut_menu_mode` |

The package earns its place on the diagnostics alone: live EDR headroom,
compositor-preferred description, active tag, exactness, residual warning,
buffer format, interop on/off. That panel is what makes bring-up and later
support tractable, and it is exactly the kind of thing that should not require
a C++ rebuild to change.

**Env override** — `RV_OUTPUT_COLORSPACE=rec709-bt1886` for CI, headless and
bring-up; `RV_HDR_PRESENT` kept as a deprecated alias for a release.

Full precedence (matching §7): env override → per-display user override →
OCIO declared metadata → standard alias → rules file → default.

---

## 9. Migration from the current branch

| Today | Becomes |
|---|---|
| `RV_HDR_PRESENT=pq` | `rec2100-pq` (native PQ tag on both platforms now) |
| `RV_HDR_PRESENT=p3linear` | `linear-p3-ext` |
| `RV_HDR_PRESENT=srgblinear` | `linear-srgb-ext` |
| `RV_HDR_PRESENT=p3extended` | `display-p3-ext` |
| `RV_HDR=1` | `RV_OUTPUT_COLORSPACE` set to any HDR-range space |
| `RV_HDR_SDR_WHITE`, `RV_HDR_PQ_SDR_SCALE`, `RV_HDR_LINEAR_SDR_MATCH` | Descriptor `luminance` fields + residual scale; Linux-only |
| `RV_HDR_GL_VK_INTEROP=0` | `RV_PRESENT_GPU_INTEROP=0` (platform-neutral name) |
| present.frag `mode` 0–3 | data-driven residual (§3) |

---

## 10. Phasing

| Phase | Scope | Est. |
|---|---|---|
| **0** | Spikes: (a) Linux — can we tag a QRhi-created Vulkan surface ourselves, or do we need `PASS_THROUGH`? (b) macOS — `createWindowContainer` overlay over a live `QOpenGLWidget`. Both are go/no-go on architecture. | 2–3 days |
| **1** | `TwkSurfaceSpace` lib + canonical registry + data-driven present shader; port the four existing Wayland modes onto it with no behavior change. Pure refactor, testable on Linux alone. | 4–6 days |
| **2** | `ISurfaceTagger` + Wayland backend (named + parametric + feature query + feedback). `rec709-bt1886` working on Linux. | 4–6 days |
| **3** | macOS backend: QRhi Metal path, Cocoa tagger, `GlMtlSharedImage` (IOSurface). `rec709-bt1886` and `display-p3-ext` working on macOS. | 6–10 days |
| **4** | ICC synthesis tier; UI (derivation + session properties + device policy in Preferences + commands + package with status/override/HUD). | 5–8 days |
| **5** | Docs, A/B recipes for both platforms, take `SurfaceCapabilities` to OCIO #1996. | 2–3 days |

Phases 1–2 are useful on their own on Linux; phase 3 is the macOS payoff;
phase 4 is what makes it usable by anyone who didn't write it.

---

## 11. Open questions

1. **Linux tagging authority** — does an app-set image description survive
   alongside a Mesa-managed Vulkan swapchain, or is `PASS_THROUGH` (and
   therefore bypassing QRhi's swapchain) mandatory? Phase-0 spike; drives
   whether we keep QRhi's swapchain on Linux.
2. **Named vs ICC preference** — when both work, named is cheaper and more
   likely to hit a compositor fast path; ICC is more precise for odd spaces.
   Default to named, allow a per-space override in the rules file.
3. **Where does the residual live** — present shader (proposed) or appended
   to the OCIO/display GPU pass? Present shader keeps color decisions out of
   the IP graph and matches the current branch; revisit if it costs a pass.
4. **UI HDR** — the whole-window UI stays SDR-composited; only the image
   plane is tagged. Unchanged from the Wayland branch, worth stating as
   policy.
5. **Per-display state with multiple monitors** — surface space is per-device,
   but a window dragged between displays keeps its tag while the *preferred*
   description changes. Follow the compositor's preference, or pin? Suggest
   pin + HUD warning.
