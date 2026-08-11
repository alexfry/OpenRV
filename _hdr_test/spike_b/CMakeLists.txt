#
# SPDX-License-Identifier: Apache-2.0
#
# Spike B — macOS EDR present-surface embedding.
#
# Standalone on purpose: it does not need OpenRV or its 38 dependencies, so it
# builds in seconds and can be iterated on quickly. Build with:
#
#   cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.1/macos"
#   cmake --build build
#   ./build/spike_b
#

CMAKE_MINIMUM_REQUIRED(VERSION 3.20)
PROJECT(spike_b LANGUAGES CXX OBJCXX)

IF(NOT APPLE)
  MESSAGE(FATAL_ERROR "Spike B is macOS-only: it tests Metal/EDR embedding.")
ENDIF()

SET(CMAKE_CXX_STANDARD 17)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
SET(CMAKE_AUTOMOC ON)

# GuiPrivate provides rhi/qrhi.h. It is versioned private API, which is exactly
# what the real RvCommon target already depends on for the Vulkan path.
FIND_PACKAGE(
  Qt6
  COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets GuiPrivate
  REQUIRED
)

ADD_EXECUTABLE(
  spike_b
  main.cpp
  edrinfo.mm
  edrinfo.h
)

TARGET_LINK_LIBRARIES(
  spike_b
  PRIVATE Qt6::Core
          Qt6::Gui
          Qt6::Widgets
          Qt6::OpenGL
          Qt6::OpenGLWidgets
          Qt6::GuiPrivate
          "-framework AppKit"
          "-framework Metal"
          "-framework QuartzCore"
)

# Bake the GLSL to .qsb next to the binary. Metal needs the MSL variant, so ask
# for it explicitly rather than relying on qsb defaults.
FIND_PROGRAM(
  QSB_EXECUTABLE qsb
  HINTS ${Qt6_DIR}/../../../bin
  REQUIRED
)

SET(_shader_out ${CMAKE_CURRENT_BINARY_DIR}/shaders)
FILE(MAKE_DIRECTORY ${_shader_out})

SET(_qsb_outputs)
FOREACH(_s wedge.vert wedge.frag)
  ADD_CUSTOM_COMMAND(
    OUTPUT ${_shader_out}/${_s}.qsb
    COMMAND ${QSB_EXECUTABLE} --glsl "410,300es" --msl 12 -o ${_shader_out}/${_s}.qsb
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_s}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_s}
    COMMENT "qsb ${_s}"
  )
  LIST(APPEND _qsb_outputs ${_shader_out}/${_s}.qsb)
ENDFOREACH()

ADD_CUSTOM_TARGET(spike_b_shaders DEPENDS ${_qsb_outputs})
ADD_DEPENDENCIES(spike_b spike_b_shaders)
