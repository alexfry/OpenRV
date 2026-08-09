#
# SPDX-License-Identifier: Apache-2.0
#
# Overrides applied when building against a distro/system Qt (RV_USE_SYSTEM_QT=ON).
# Keeps the rest of the VFX platform pin (Python, Boost, OCIO, …) while aligning
# PySide with the installed Qt so shiboken binds the right headers/libs.
#

# Align PySide to a non-VFX-pin Qt when:
#   - RV_USE_SYSTEM_QT=ON  (distro/FHS Qt), or
#   - RV_ALIGN_PYSIDE=ON   (e.g. aqt-installed Qt 6.11 while keeping SDK staging)
IF(NOT RV_USE_SYSTEM_QT AND NOT RV_ALIGN_PYSIDE)
  RETURN()
ENDIF()

# cxx_defaults (and others) may include this file more than once in one configure;
# only apply once per configure. Use a normal var (not CACHE) so reconfigures re-run.
IF(RV_SYSTEM_QT_APPLIED_THIS_RUN)
  RETURN()
ENDIF()
SET(RV_SYSTEM_QT_APPLIED_THIS_RUN
    ON
)

# Resolve the *installed* system Qt version. Do NOT use the CY platform pin
# (e.g. CY2025 sets 6.5.3) — that is exactly what we are overriding.
SET(_system_qt_version
    ""
)

# Explicit override wins.
IF(DEFINED RV_SYSTEM_QT_VERSION AND NOT RV_SYSTEM_QT_VERSION STREQUAL "")
  SET(_system_qt_version
      "${RV_SYSTEM_QT_VERSION}"
  )
ENDIF()

# Already found by FIND_PACKAGE earlier in this configure (unusual at this point).
IF(NOT _system_qt_version AND DEFINED Qt6Core_VERSION AND Qt6Core_VERSION)
  SET(_system_qt_version
      "${Qt6Core_VERSION}"
  )
ENDIF()

# Query qmake6 / qtpaths for the real installed version.
IF(NOT _system_qt_version)
  SET(_qt_query_candidates
      "${RV_DEPS_QT_LOCATION}/lib/qt6/bin/qmake6"
      "${RV_DEPS_QT_LOCATION}/lib/qt6/bin/qmake"
      "${RV_DEPS_QT_LOCATION}/bin/qmake6"
      "${RV_DEPS_QT_LOCATION}/bin/qmake"
      "qmake6"
      "qmake"
  )
  FOREACH(
    _qmake
    ${_qt_query_candidates}
  )
    IF(NOT _system_qt_version)
      EXECUTE_PROCESS(
        COMMAND ${_qmake} -query QT_VERSION
        RESULT_VARIABLE _qmake_rc
        OUTPUT_VARIABLE _qmake_ver
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      IF(_qmake_rc EQUAL 0
         AND _qmake_ver MATCHES "^[0-9]+\\.[0-9]+"
      )
        SET(_system_qt_version
            "${_qmake_ver}"
        )
        MESSAGE(STATUS "Detected system Qt version via ${_qmake}: ${_system_qt_version}")
      ENDIF()
    ENDIF()
  ENDFOREACH()
ENDIF()

# Parse Qt6ConfigVersion.cmake if present (Arch: /usr/lib/cmake/Qt6/…)
IF(NOT _system_qt_version)
  SET(_qt_ver_file
      "${RV_DEPS_QT_LOCATION}/lib/cmake/Qt6/Qt6ConfigVersion.cmake"
  )
  IF(NOT EXISTS "${_qt_ver_file}")
    SET(_qt_ver_file
        "${RV_DEPS_QT_LOCATION}/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"
    )
  ENDIF()
  IF(EXISTS "${_qt_ver_file}")
    FILE(
      STRINGS "${_qt_ver_file}" _qt_ver_lines
      REGEX "set\\(PACKAGE_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+\"\\)"
      LIMIT_COUNT 1
    )
    IF(_qt_ver_lines)
      STRING(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _system_qt_version "${_qt_ver_lines}")
      MESSAGE(STATUS "Detected system Qt version from ${_qt_ver_file}: ${_system_qt_version}")
    ENDIF()
  ENDIF()
ENDIF()

# Last resort default (current Arch qt6-base as of 2026-08).
IF(NOT _system_qt_version OR _system_qt_version STREQUAL "")
  SET(_system_qt_version
      "6.11.1"
  )
  MESSAGE(STATUS "Could not probe system Qt version; defaulting to ${_system_qt_version}")
ENDIF()

# Strip any trailing pre-release junk; keep major.minor.patch
STRING(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" _system_qt_version "${_system_qt_version}")

MESSAGE(STATUS "RV_USE_SYSTEM_QT: aligning PySide with system Qt ${_system_qt_version}")

# CY*.cmake sets these as normal (non-cache) variables, which would shadow CACHE entries.
# Set both normal and CACHE so subsequent cmake code sees the system-Qt values.
SET(RV_DEPS_QT_VERSION
    "${_system_qt_version}"
)
SET(RV_DEPS_QT_VERSION
    "${_system_qt_version}"
    CACHE STRING "Qt Version String" FORCE
)
SET(RV_DEPS_QT_MAJOR
    "6"
)
SET(RV_DEPS_QT_MAJOR
    "6"
    CACHE STRING "Qt major" FORCE
)

SET(RV_DEPS_PYSIDE_VERSION
    "${_system_qt_version}"
)
SET(RV_DEPS_PYSIDE_VERSION
    "${_system_qt_version}"
    CACHE STRING "PySide version (matched to system Qt)" FORCE
)
SET(RV_DEPS_PYSIDE_TARGET
    "RV_DEPS_PYSIDE6"
)
SET(RV_DEPS_PYSIDE_TARGET
    "RV_DEPS_PYSIDE6"
    CACHE STRING "PySide fetch content target" FORCE
)

# Official sources: prefer .zip (same as CY pins) from the Berkeley mirror used elsewhere.
SET(RV_DEPS_PYSIDE_ARCHIVE_URL
    "https://mirrors.ocf.berkeley.edu/qt/official_releases/QtForPython/pyside6/PySide6-${RV_DEPS_PYSIDE_VERSION}-src/pyside-setup-everywhere-src-${RV_DEPS_PYSIDE_VERSION}.zip"
)
SET(RV_DEPS_PYSIDE_ARCHIVE_URL
    "https://mirrors.ocf.berkeley.edu/qt/official_releases/QtForPython/pyside6/PySide6-${RV_DEPS_PYSIDE_VERSION}-src/pyside-setup-everywhere-src-${RV_DEPS_PYSIDE_VERSION}.zip"
    CACHE STRING "PySide source archive URL" FORCE
)

# Known MD5 sums for PySide source zips we have verified. Override with -DRV_DEPS_PYSIDE_DOWNLOAD_HASH=...
# if you pin a version not listed here.
SET(_pyside_known_hash
    ""
)
IF(RV_DEPS_PYSIDE_VERSION VERSION_EQUAL "6.11.1")
  SET(_pyside_known_hash
      "267c5eef10714c613b7a680b00077a5a"
  )
ELSEIF(RV_DEPS_PYSIDE_VERSION VERSION_EQUAL "6.11.0")
  # Placeholder: set when needed
  SET(_pyside_known_hash
      ""
  )
ENDIF()

IF(NOT _pyside_known_hash STREQUAL "")
  SET(RV_DEPS_PYSIDE_DOWNLOAD_HASH
      "${_pyside_known_hash}"
  )
  SET(RV_DEPS_PYSIDE_DOWNLOAD_HASH
      "${_pyside_known_hash}"
      CACHE STRING "PySide source MD5" FORCE
  )
ELSEIF(NOT DEFINED RV_DEPS_PYSIDE_DOWNLOAD_HASH OR RV_DEPS_PYSIDE_DOWNLOAD_HASH STREQUAL "515d3249c6e743219ff0d7dd25b8c8d8")
  # Still the CY2025 hash — refuse to use the wrong one silently.
  MESSAGE(
    FATAL_ERROR
      "RV_USE_SYSTEM_QT: no known MD5 for PySide ${RV_DEPS_PYSIDE_VERSION}. "
      "Download the zip, md5sum it, and pass -DRV_DEPS_PYSIDE_DOWNLOAD_HASH=<md5>."
  )
ENDIF()

MESSAGE(STATUS "  PySide URL:  ${RV_DEPS_PYSIDE_ARCHIVE_URL}")
MESSAGE(STATUS "  PySide MD5:  ${RV_DEPS_PYSIDE_DOWNLOAD_HASH}")
