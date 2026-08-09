#
# Copyright (C) 2023  Autodesk, Inc. All Rights Reserved.
#
# SPDX-License-Identifier: Apache-2.0
#

# Find the off-screen rendering Mesa (OSMesa) headers and libraries.
#
# Once done this will define: OSMesa_FOUND        - true if OSMesa has been found OSMesa_VERSION      - Version of OSMesa found OSMesa::OSMesa      - Imported
# target
#
# Note: Set OSMESA_ROOT to specify an additional directory to search.

FIND_PATH(
  OSMESA_INCLUDE_DIR
  NAMES GL/osmesa.h
  PATHS "${OSMESA_ROOT}/include" "$ENV{OSMESA_ROOT}/include"
  DOC "OSMesa include directory"
)
MARK_AS_ADVANCED(OSMESA_INCLUDE_DIR)

FIND_LIBRARY(
  OSMESA_LIBRARY
  NAMES OSMesa OSMesa16 OSMesa32
  PATHS "${OSMESA_ROOT}/lib" "$ENV{OSMESA_ROOT}/lib"
  DOC "OSMesa library"
)
MARK_AS_ADVANCED(OSMESA_LIBRARY)

IF(OSMESA_INCLUDE_DIR
   AND EXISTS "${OSMESA_INCLUDE_DIR}/GL/osmesa.h"
)
  FILE(
    STRINGS "${OSMESA_INCLUDE_DIR}/GL/osmesa.h" _OSMesa_version_lines
    REGEX "OSMESA_[A-Z]+_VERSION"
  )
  STRING(
    REGEX
    REPLACE ".*# *define +OSMESA_MAJOR_VERSION +([0-9]+).*" "\\1" _OSMesa_version_major "${_OSMesa_version_lines}"
  )
  STRING(
    REGEX
    REPLACE ".*# *define +OSMESA_MINOR_VERSION +([0-9]+).*" "\\1" _OSMesa_version_minor "${_OSMesa_version_lines}"
  )
  STRING(
    REGEX
    REPLACE ".*# *define +OSMESA_PATCH_VERSION +([0-9]+).*" "\\1" _OSMesa_version_patch "${_OSMesa_version_lines}"
  )
  SET(OSMesa_VERSION
      "${_OSMesa_version_major}.${_OSMesa_version_minor}.${_OSMesa_version_patch}"
  )
  UNSET(_OSMesa_version_major)
  UNSET(_OSMesa_version_minor)
  UNSET(_OSMesa_version_patch)
  UNSET(_OSMesa_version_lines)
ENDIF()

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(
  OSMesa
  REQUIRED_VARS OSMESA_INCLUDE_DIR OSMESA_LIBRARY
  VERSION_VAR OSMesa_VERSION
)

IF(OSMesa_FOUND)
  IF(NOT TARGET OSMesa::OSMesa)
    ADD_LIBRARY(OSMesa::OSMesa UNKNOWN IMPORTED)
    SET_TARGET_PROPERTIES(
      OSMesa::OSMesa
      PROPERTIES IMPORTED_LOCATION "${OSMESA_LIBRARY}"
                 INTERFACE_INCLUDE_DIRECTORIES "${OSMESA_INCLUDE_DIR}"
    )
    # Fedora-packaged libOSMesa needs libglapi + a matching libLLVM (e.g. 18.1).
    # Search beside OSMESA_ROOT and on the system so Arch hosts can stage them.
    SET(_osmesa_search_lib_dirs "")
    IF(OSMESA_ROOT)
      LIST(APPEND _osmesa_search_lib_dirs "${OSMESA_ROOT}/lib" "${OSMESA_ROOT}/lib64")
    ENDIF()
    IF(DEFINED ENV{OSMESA_ROOT})
      LIST(APPEND _osmesa_search_lib_dirs "$ENV{OSMESA_ROOT}/lib" "$ENV{OSMESA_ROOT}/lib64")
    ENDIF()
    LIST(APPEND _osmesa_search_lib_dirs /usr/lib /usr/lib64 /usr/lib/llvm18/lib)

    FIND_LIBRARY(
      OSMESA_GLAPI_LIBRARY
      NAMES glapi
      PATHS ${_osmesa_search_lib_dirs}
      NO_DEFAULT_PATH
    )
    IF(NOT OSMESA_GLAPI_LIBRARY)
      FIND_LIBRARY(OSMESA_GLAPI_LIBRARY NAMES glapi)
    ENDIF()

    FIND_LIBRARY(
      OSMESA_LLVM_LIBRARY
      NAMES LLVM-18.1 LLVM.so.18.1 LLVM
      PATHS ${_osmesa_search_lib_dirs}
      NO_DEFAULT_PATH
    )
    IF(NOT OSMESA_LLVM_LIBRARY)
      FIND_LIBRARY(OSMESA_LLVM_LIBRARY NAMES LLVM-18.1 LLVM)
    ENDIF()

    SET(_osmesa_iface_libs "")
    IF(OSMESA_GLAPI_LIBRARY)
      LIST(APPEND _osmesa_iface_libs "${OSMESA_GLAPI_LIBRARY}")
    ENDIF()
    IF(OSMESA_LLVM_LIBRARY)
      LIST(APPEND _osmesa_iface_libs "${OSMESA_LLVM_LIBRARY}")
    ENDIF()
    IF(_osmesa_iface_libs)
      SET_PROPERTY(
        TARGET OSMesa::OSMesa
        APPEND
        PROPERTY INTERFACE_LINK_LIBRARIES ${_osmesa_iface_libs}
      )
      MESSAGE(STATUS "OSMesa: also linking ${_osmesa_iface_libs}")
    ENDIF()
  ENDIF()
ENDIF()
