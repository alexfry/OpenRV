#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Host packages for building Open RV on Arch Linux (field notes: Alex Fry, 2026-08).
# See docs/build_system/config_linux_arch_qt611_wayland.md for the full story
# (aqt Qt 6.11, OSMesa staging, XWayland launcher, OCIO/zlib-ng, etc.).
#
set -euo pipefail

sudo pacman -S --needed --noconfirm \
  base-devel cmake ninja ccache git curl \
  python python-pip python-virtualenv \
  flex bison autoconf automake libtool patch \
  meson nasm yasm patchelf zip unzip p7zip wget \
  alsa-lib libpulse \
  libx11 libxext libxrender libxrandr libxcursor libxi libxxf86vm \
  libxcomposite libxdamage libxtst libxkbcommon libxkbfile \
  mesa glu libffi openssl zlib zlib-ng bzip2 xz ncurses readline sqlite \
  tcl tk systemd ocl-icd opencl-headers avahi nss pcsclite rust \
  clang llvm \
  freetype2 fontconfig \
  imagemagick

cat <<'EOF'

Host packages installed.

Next steps (typical aqt Qt 6.11 + CY2025 path):

  1. Install Qt 6.11.1 via aqt into ~/Qt/6.11.1/gcc_64 (include WebEngine, UiTools, …).
  2. git submodule update --init --recursive
  3. Stage OSMesa if you need rvio_sw (see docs/build_system/config_linux_arch_qt611_wayland.md).
  4. Configure and build:

       export PATH="$HOME/.local/bin:$PATH"
       export RV_DEPS_BASE_DIR="$HOME/openrv-deps"
       export OSMESA_ROOT="$HOME/openrv-deps/osmesa"   # if staged
       export QT_HOME="$HOME/Qt/6.11.1/gcc_64"

       cmake -B _build -G Ninja \
         -DCMAKE_BUILD_TYPE=Release \
         -DRV_DEPS_BASE_DIR="$RV_DEPS_BASE_DIR" \
         -DRV_VFX_PLATFORM=CY2025 \
         -DRV_DEPS_QT_LOCATION="$QT_HOME" \
         -DOSMESA_ROOT="${OSMESA_ROOT:-}"

       cmake --build _build --config Release --target main_executable --parallel "$(nproc)"

  5. Run:

       _build/stage/app/bin/rv -version

Full notes: docs/build_system/config_linux_arch_qt611_wayland.md
EOF
