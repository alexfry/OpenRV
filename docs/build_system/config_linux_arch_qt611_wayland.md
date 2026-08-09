# Building Open RV on Arch Linux (Qt 6.11, Hyprland / Wayland)

**Author:** Alex Fry ([@alexfry](https://github.com/alexfry))  
**Date:** 2026-08-09  
**Host verified:** Arch Linux (Omarchy/Hyprland), NVIDIA TITAN RTX, XWayland for the viewer  

This note records what it took to get **Open RV** configuring, linking, launching, and **playing EXR** on a modern Arch + Wayland desktop—outside the usual Rocky/VFX-pin path. It is a field report, not an official ASWF platform.

---

## Goals

1. **Build and run Open RV as-is** on this machine (XWayland OK).
2. **Later:** native windowed Wayland HDR (Qt 6.10+ color management / `wp_color_management_v1`). Goal 2 is **not** finished here; goal 1 is.

---

## What worked (summary)

| Item | Choice |
|------|--------|
| VFX platform pin | `CY2025` (Python 3.11, Boost 1.85, OCIO 2.4.2, …) |
| Qt | **aqt Qt 6.11.1** under `~/Qt/6.11.1/gcc_64` (not the CY2025 pin of 6.5.3 alone) |
| PySide | **6.11.1**, aligned with that Qt |
| Generator | Ninja, Release |
| Viewer platform | **Force `QT_QPA_PLATFORM=xcb`** (XWayland) + NVIDIA GLX |
| Staged app | `_build/stage/app` → run `_build/stage/app/bin/rv` |

```bash
# Typical successful launch
/path/to/openrv/_build/stage/app/bin/rv /path/to/sequence.#.exr
```

---

## Layout on disk

Paths used on the reference host (adjust as needed):

| What | Path |
|------|------|
| Source | `~/github/openrv` |
| CMake / Ninja tree | `~/github/openrv/_build` |
| Staged install-like tree | `~/github/openrv/_build/stage/app` |
| **`rv` launcher** | `~/github/openrv/_build/stage/app/bin/rv` |
| **`rv` binary** | `~/github/openrv/_build/stage/app/bin/rv.bin` |
| Third-party deps | `~/openrv-deps` |
| aqt Qt SDK | `~/Qt/6.11.1/gcc_64` |
| OSMesa (+ staged glapi + LLVM 18 for Fedora OSMesa) | `~/openrv-deps/osmesa` |
| Build status helper (local, not in git) | `~/github/openrv/_build_status/` |

---

## Host prerequisites

### Packages (Arch)

See also [`install_arch_deps.sh`](../../install_arch_deps.sh) at the repo root (pacman list). Roughly:

- Build: `base-devel`, CMake ≥ 3.31 (or portable cmake), Ninja, git, flex/bison, meson, nasm, patchelf, …
- Desktop/GL: Mesa, GLU, X11/XCB stack, `libxkbcommon`, Wayland libs as needed
- Runtime helpers: `zlib-ng` (for OCIO minizip-ng `zng_*` symbols on this build)
- Optional tools: `mesa-utils`, ImageMagick, `grim` (screenshots under Hyprland)

### Qt 6.11 via aqt

Official CY2025 still pins Qt 6.5.3. For newer Qt APIs (and future Wayland HDR work) we installed:

```bash
# Example — use your preferred aqt invocation
aqt install-qt linux desktop 6.11.1 gcc_64 \
  -O "$HOME/Qt" \
  -m all   # or at least Gui, Widgets, OpenGL, OpenGLWidgets, Multimedia,
           # Svg, Network, Xml, UiTools, WebEngine, WebChannel, Qml, …
```

Critical modules that were easy to miss: **UiTools**, **WebEngine**, private Gui headers if required by the tree.

### Submodules

```bash
git submodule update --init --recursive
```

Empty `src/pub` (or similar) will fail configure/build until submodules are present.

### OSMesa (for TwkGLFMesa / rvio_sw)

Arch does not ship a drop-in OSMesa matching every upstream assumption. This build staged **Fedora 40** `mesa-libOSMesa` under `~/openrv-deps/osmesa`, then also staged:

- `libglapi.so` from the same Mesa generation  
- `libLLVM.so.18.1` from Fedora `llvm-libs` 18.x  

and taught `FindOSMesa.cmake` to link those as interface libs. Without them, **rvio_sw** fails to link; **`rv` itself** does not require OSMesa for normal interactive viewing.

### libaio

Fedora `libaio` was staged under `~/openrv-deps/libaio` when the distro package layout differed; TwkUtil CMake was adjusted to find/stage it.

---

## Configure & build

Example (matches the working machine):

```bash
export PATH="$HOME/.local/bin:$HOME/.local/cmake-portable/bin:$PATH"
export RV_DEPS_BASE_DIR="$HOME/openrv-deps"
export OSMESA_ROOT="$HOME/openrv-deps/osmesa"
export QT_HOME="$HOME/Qt/6.11.1/gcc_64"

cd ~/github/openrv

cmake -B _build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DRV_DEPS_BASE_DIR="$RV_DEPS_BASE_DIR" \
  -DRV_VFX_PLATFORM=CY2025 \
  -DRV_DEPS_QT_LOCATION="$QT_HOME" \
  -DOSMESA_ROOT="$OSMESA_ROOT"

# Full tree can fail on optional plugins (e.g. NDI without SDK).
# Goal 1 only needs the main viewer:
cmake --build _build --config Release --target main_executable --parallel "$(nproc)"
```

Optional helpers: `source rvcmds.sh` after exporting `RV_*` / `QT_HOME` (see patches that add system/aqt Qt alignment flags).

**Success criterion for goal 1:**

```bash
_build/stage/app/bin/rv -version
_build/stage/app/bin/rv /path/to/media.#.exr
```

---

## Code / build changes in this effort

These are the categories of patches applied on the working branch (see git history / diff):

### 1. Qt 6.11 + PySide alignment

- `cmake/defaults/system_qt.cmake` — when using non-CY Qt (system or aqt), align PySide version/URL/MD5 to the real Qt version.
- `cmake/defaults/rv_options.cmake` — options such as `RV_USE_SYSTEM_QT` / `RV_ALIGN_PYSIDE`.
- `cmake/dependencies/qt6.cmake` — stage/find aqt or system Qt; handle GuiPrivate, etc.
- `src/build/make_pyside6.py` — `qtpaths` / ApiExtractor path fixes for 6.11.
- `rvcmds.sh` — pass-through for system/aqt Qt flags.

### 2. Toolchain / GCC 16 / dependency hard corners

- Breakpad: only certain tools on very new GCC.
- ImGui / nedmalloc / minizip / yaml-cpp: casts, `_GNU_SOURCE`, `cstdint`, etc. as needed.
- `TwkUtil` + libaio find/stage.
- **OCIO:** link consumers against **zlib-ng** (`zng_*` from bundled minizip-ng). Optionally `patchelf --add-needed libz-ng.so.2` on `libOpenColorIO.so` so Python/PyOpenColorIO loads cleanly.

### 3. MuQt6 / Qt6 API

- `SignalSpy` rewritten as a `QObject` (not `QSignalSpy` subclass issues).
- Bulk `operator*` / comparison fixes (`arg0.operator==` → `arg0 ==`, etc.) for Qt 6.11 bindings.

### 4. Launchers: tcsh → bash

All `src/bin/**/**.wrapper` scripts converted to **bash** (Arch/minimal hosts often have no `tcsh`). The **`rv.wrapper`** additionally:

- Forces **`QT_QPA_PLATFORM=xcb`** when `DISPLAY` is set, unless `RV_ALLOW_WAYLAND=1` or `RV_QT_PLATFORM=…` is set.  
  Many desktops export `QT_QPA_PLATFORM=wayland;xcb`, which prefers Wayland: **UI/thumbnails work, main GL image plane stays black**.
- Sets **`__GLX_VENDOR_LIBRARY_NAME=nvidia`** when NVIDIA GLX is present.
- Clears invalid **`QT_STYLE_OVERRIDE=kvantum`** for aqt Qt (no Kvantum style plugin).
- Optionally raises Qt WebEngine Chromium log level (still may print one GPUInfo line).

### 5. Runtime robustness under Wayland / XWayland

- **`RvDocument.cpp`:** never call `XQueryExtension(QX11Info::display(), …)` unless platform is `xcb` and `Display*` is non-null (was a hard **SIGSEGV** on pure Wayland QPA).
- Soften the old **NV-GLX “ERROR”** banner: under XWayland, NV-GLX is often missing even when proprietary NVIDIA GLX works.
- Menu rebuild: replace wildcard `QObject::disconnect()` with specific signal disconnects (Qt 6 spammed `QMenu::unnamed` warnings otherwise).
- **`ocio_source_setup.py`:** `$OCIO` unset is an **INFO** with a hint, not a scary WARNING.

### 6. OSMesa discovery

- `cmake/macros/FindOSMesa.cmake` — also link `glapi` + matching LLVM when found next to `OSMESA_ROOT`.

---

## Runtime issues we hit (and fixes)

### Segfault on launch (Wayland QPA)

**Symptom:** `rv -version` OK; opening a window **SIGSEGV** in `_XFlush` / `XQueryExtension` from `RvDocument`.  
**Cause:** Default Qt platform `wayland` (or `wayland;xcb`) + X11-only startup code.  
**Fix:** Guard X11 calls; force **xcb** in `rv.wrapper`.

### Black main view, thumbnails OK

**Symptom:** Sidebar thumbnails load; center view empty/black.  
**Cause:** Same Wayland-first QPA — GL path unhappy; CPU thumbs still work.  
**Fix:** Forced **xcb** + NVIDIA GLX vendor. Verify with:

```bash
# While rv is running
tr '\0' '\n' < /proc/$(pgrep -n rv.bin)/environ | grep QT_QPA
# Expect: QT_QPA_PLATFORM=xcb
# hyprctl clients: OpenRV window should show xwayland=true
```

### OCIO / zlib-ng link and load

**Symptom:** Undefined `zng_inflate` / `zng_crc32` at link, or PyOpenColorIO fails to import.  
**Fix:** Link `libz-ng` into OpenColorIO consumers; ensure `zlib-ng` is installed; optional `patchelf` NEEDED entry on `libOpenColorIO.so`.

### NV-GLX message

**Not fatal** on XWayland + NVIDIA. Proprietary GLX can work without the historical `NV-GLX` X extension string.

### Qt WebEngine

`WARNING: GPUInfo not initialized on GpuInfoUpdate` comes from **Qt WebEngine/Chromium**, not the image pipeline. Harmless for EXR playback.

### Optional targets

Full `ninja` / default build may still fail on **NDI** (no SDK headers) or other optional plugins. Prefer `--target main_executable` for the viewer.

---

## OCIO / ACES configs

Open RV **does not ship** a facility ACES 2.0 (or other) `config.ocio` tree on disk. It ships:

- The **OpenColorIO** library (this build: **2.4.2**)
- The **`ocio_source_setup`** package (uses `$OCIO` or a saved path)
- A few **ACES-related shader nodes** (not a full OCIO config)

OCIO 2.2+ embeds configs **inside the library**. With 2.4.2 you can use ACES **1.3** builtins:

```bash
export OCIO=ocio://cg-config-latest
# or
export OCIO=ocio://studio-config-latest
# explicit:
# export OCIO=ocio://cg-config-v2.2.0_aces-v1.3_ocio-v2.4
```

**ACES 2.0** OCIO configs need **OCIO ≥ 2.5** (or a downloaded config file from [OpenColorIO-Config-ACES](https://github.com/AcademySoftwareFoundation/OpenColorIO-Config-ACES/releases)). This CY2025 pin is 2.4.2 → builtins are ACES 1.3-class.

Without `$OCIO`, playback still works; display transforms are just not OCIO-managed.

---

## Environment cheatsheet

| Variable | Purpose |
|----------|---------|
| `QT_QPA_PLATFORM=xcb` | Set by wrapper unless overridden — XWayland |
| `RV_ALLOW_WAYLAND=1` | Leave desktop Qt platform alone (native Wayland experiments) |
| `RV_QT_PLATFORM=…` | Explicit QPA override |
| `__GLX_VENDOR_LIBRARY_NAME=nvidia` | Prefer NVIDIA GLX under X11 |
| `OCIO=…` | Path or `ocio://…` builtin URI |
| `OSMESA_ROOT` | Offscreen Mesa root for rvio_sw / TwkGLFMesa |
| `RV_DEPS_BASE_DIR` | Where ExternalProject deps land |
| `RV_DEPS_QT_LOCATION` / `QT_HOME` | aqt or system Qt prefix |

---

## Rebuild after pulling this branch

```bash
export PATH="$HOME/.local/bin:$HOME/.local/cmake-portable/bin:$PATH"
export RV_DEPS_BASE_DIR="$HOME/openrv-deps"
export OSMESA_ROOT="$HOME/openrv-deps/osmesa"
export QT_HOME="$HOME/Qt/6.11.1/gcc_64"

cd ~/github/openrv
cmake -B _build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DRV_DEPS_BASE_DIR="$RV_DEPS_BASE_DIR" \
  -DRV_VFX_PLATFORM=CY2025 \
  -DRV_DEPS_QT_LOCATION="$QT_HOME" \
  -DOSMESA_ROOT="$OSMESA_ROOT"

cmake --build _build --config Release --target main_executable --parallel "$(nproc)"

# Wrapper is installed as stage/app/bin/rv — ensure bash wrapper is current:
cp src/bin/apps/rv/rv.wrapper _build/stage/app/bin/rv && chmod +x _build/stage/app/bin/rv

_build/stage/app/bin/rv -version
```

---

## What’s next (goal 2)

- Native **Wayland** QPA without black GL / X11 assumptions.
- Qt **HDR / color management** (6.10+) and compositor HDR (Hyprland).
- Possibly bump OCIO for **ACES 2.0** builtins or bundle a studio config.

---

## License / contribution

Patches and this document are intended for a personal fork / PR discussion with upstream ASWF Open RV. Follow the project’s Apache-2.0 license and contribution guide when proposing upstream changes—several items (bash wrappers, Wayland guards, MuQt6 6.11 fixes) are good candidates for cleaned-up PRs rather than a single mega-patch.
