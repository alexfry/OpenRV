#
# Copyright (C) 2022  Autodesk, Inc. All Rights Reserved.
#
# SPDX-License-Identifier: Apache-2.0
#

# -----------------------------------------------------------------------------
# Qt6 dependency: official SDK layout (…/gcc_64) or system/distro install (/usr).
#
# System mode (RV_USE_SYSTEM_QT=ON or auto-detected FHS layout):
#   - Does NOT copy all of ${prefix}/lib into the stage (would pull in /usr/lib).
#   - Links against system libQt6*.so and stages only plugins + WebEngine assets.
# -----------------------------------------------------------------------------

SET(RV_DEPS_QT_LOCATION
    ""
    CACHE STRING "Path to pre-compiled Qt6 (SDK gcc_64 prefix, or /usr for system Qt)"
)

OPTION(RV_USE_SYSTEM_QT "Use distro/system Qt (FHS layout under /usr) instead of an official SDK tree" OFF)

SET(_target
    RV_DEPS_QT
)

IF(NOT RV_DEPS_QT_LOCATION
   OR RV_DEPS_QT_LOCATION STREQUAL ""
)
  MESSAGE(
    FATAL_ERROR
      "Unable to build without a RV_DEPS_QT_LOCATION. Provide a Qt6 root path. "
      "SDK example: -DRV_DEPS_QT_LOCATION=$HOME/Qt/6.5.3/gcc_64 "
      "System example: -DRV_DEPS_QT_LOCATION=/usr -DRV_USE_SYSTEM_QT=ON"
  )
ENDIF()

# Normalize trailing slash
GET_FILENAME_COMPONENT(RV_DEPS_QT_LOCATION "${RV_DEPS_QT_LOCATION}" REALPATH)

# Auto-detect system/FHS layout when not forced:
#   SDK:     ${prefix}/lib/cmake/Qt6
#   System:  ${prefix}/lib/cmake/Qt6  AND plugins under ${prefix}/lib/qt6/plugins
IF(NOT RV_USE_SYSTEM_QT)
  IF(EXISTS "${RV_DEPS_QT_LOCATION}/lib/qt6/plugins"
     OR EXISTS "${RV_DEPS_QT_LOCATION}/lib/qt6/QtWebEngineProcess"
     OR (EXISTS "${RV_DEPS_QT_LOCATION}/lib/cmake/Qt6"
         AND NOT EXISTS "${RV_DEPS_QT_LOCATION}/plugins")
  )
    SET(RV_USE_SYSTEM_QT
        ON
    )
    MESSAGE(STATUS "Auto-detected system/FHS Qt layout at ${RV_DEPS_QT_LOCATION}")
  ENDIF()
ENDIF()

IF(RV_USE_SYSTEM_QT)
  MESSAGE(STATUS "Using system Qt from ${RV_DEPS_QT_LOCATION}")

  # Arch/Debian/Fedora-style paths relative to prefix
  SET(_qt_sys_cmake_root
      "${RV_DEPS_QT_LOCATION}/lib/cmake"
  )
  SET(_qt_sys_plugins
      "${RV_DEPS_QT_LOCATION}/lib/qt6/plugins"
  )
  SET(_qt_sys_libexec
      "${RV_DEPS_QT_LOCATION}/lib/qt6"
  )
  SET(_qt_sys_bin
      "${RV_DEPS_QT_LOCATION}/lib/qt6/bin"
  )
  # Also check /usr/bin for tools when prefix is /usr
  IF(EXISTS "${RV_DEPS_QT_LOCATION}/bin")
    LIST(APPEND _qt_sys_bin "${RV_DEPS_QT_LOCATION}/bin")
  ENDIF()

  IF(EXISTS "${RV_DEPS_QT_LOCATION}/share/qt6/resources")
    SET(RV_DEPS_QT_RESOURCES_FOLDER
        "${RV_DEPS_QT_LOCATION}/share/qt6/resources"
        CACHE STRING "Path to the Qt resources files folder" FORCE
    )
  ELSEIF(EXISTS "${RV_DEPS_QT_LOCATION}/share/qt/resources")
    SET(RV_DEPS_QT_RESOURCES_FOLDER
        "${RV_DEPS_QT_LOCATION}/share/qt/resources"
        CACHE STRING "Path to the Qt resources files folder" FORCE
    )
  ELSE()
    SET(RV_DEPS_QT_RESOURCES_FOLDER
        "${RV_DEPS_QT_LOCATION}/share/qt6/resources"
        CACHE STRING "Path to the Qt resources files folder" FORCE
    )
  ENDIF()

  IF(EXISTS "${RV_DEPS_QT_LOCATION}/share/qt6/translations")
    SET(RV_DEPS_QT_TRANSLATIONS_FOLDER
        "${RV_DEPS_QT_LOCATION}/share/qt6/translations"
        CACHE STRING "Path to the Qt translations files folder" FORCE
    )
  ELSE()
    SET(RV_DEPS_QT_TRANSLATIONS_FOLDER
        "${RV_DEPS_QT_LOCATION}/share/qt6/translations"
        CACHE STRING "Path to the Qt translations files folder" FORCE
    )
  ENDIF()

  # Expose CMake packages
  IF(EXISTS "${_qt_sys_cmake_root}")
    LIST(PREPEND CMAKE_PREFIX_PATH "${RV_DEPS_QT_LOCATION}")
    FILE(GLOB QT6_CMAKE_DIRS ${_qt_sys_cmake_root}/Qt6*)
    FOREACH(
      QT6_CMAKE_DIR
      ${QT6_CMAKE_DIRS}
    )
      IF(IS_DIRECTORY ${QT6_CMAKE_DIR})
        GET_FILENAME_COMPONENT(QT_COMPONENT_NAME ${QT6_CMAKE_DIR} NAME)
        SET(${QT_COMPONENT_NAME}_DIR
            ${QT6_CMAKE_DIR}
            CACHE INTERNAL "Path to ${QT_COMPONENT_NAME} CMake"
        )
      ENDIF()
    ENDFOREACH()
  ELSE()
    MESSAGE(FATAL_ERROR "System Qt CMake packages not found under ${_qt_sys_cmake_root}")
  ENDIF()

ELSE()
  # -------------------- Official SDK layout --------------------
  SET(RV_DEPS_QT_RESOURCES_FOLDER
      "${RV_DEPS_QT_LOCATION}/resources"
      CACHE STRING "Path to the Qt resources files folder"
  )
  SET(RV_DEPS_QT_TRANSLATIONS_FOLDER
      "${RV_DEPS_QT_LOCATION}/translations"
      CACHE STRING "Path to the Qt translations files folder"
  )

  FILE(GLOB QT6_CMAKE_DIRS ${RV_DEPS_QT_LOCATION}/lib/cmake/*)
  FOREACH(
    QT6_CMAKE_DIR
    ${QT6_CMAKE_DIRS}
  )
    IF(IS_DIRECTORY ${QT6_CMAKE_DIR})
      GET_FILENAME_COMPONENT(QT_COMPONENT_NAME ${QT6_CMAKE_DIR} NAME)
      SET(CMAKE_PREFIX_PATH
          ${CMAKE_PREFIX_PATH} ${QT6_CMAKE_DIR}
      )
      SET(${QT_COMPONENT_NAME}_DIR
          ${QT6_CMAKE_DIR}
          CACHE INTERNAL "Path to ${QT_COMPONENT_NAME} CMake"
      )
    ENDIF()
  ENDFOREACH()

  # For newer versions of Qt on macOS, QtWebEngine may not be part of the base installation.
  STRING(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+)" _qt_version_from_path "${RV_DEPS_QT_LOCATION}")

  IF(_qt_version_from_path VERSION_EQUAL "6.8.3"
     AND NOT EXISTS "${RV_DEPS_QT_LOCATION}/lib/cmake/Qt6WebEngineCore/Qt6WebEngineCoreConfig.cmake"
  )
    MESSAGE(STATUS "QtWebEngine component for 6.8.3 not found. Attempting to download and install it.")

    FIND_PROGRAM(
      SEVEN_ZIP_EXECUTABLE
      NAMES 7z p7zip
    )
    IF(NOT SEVEN_ZIP_EXECUTABLE)
      MESSAGE(FATAL_ERROR "p7zip (or 7z) is required to extract the QtWebEngine module but was not found. Please install it (e.g., 'brew install p7zip').")
    ENDIF()

    IF(RV_TARGET_DARWIN)
      SET(QT_WEBENGINE_URL
          "https://download.qt.io/online/qtsdkrepository/mac_x64/extensions/qtwebengine/683/clang_64/extensions.qtwebengine.683.clang_64/6.8.3-0-202503201424qtwebengine-MacOS-MacOS_14-Clang-MacOS-MacOS_14-X86_64-ARM64.7z"
      )
      SET(QT_WEBENGINE_ARCHIVE
          "${CMAKE_BINARY_DIR}/qtwebengine-6.8.3-macos.7z"
      )
    ELSEIF(RV_TARGET_LINUX)
      SET(QT_WEBENGINE_URL
          "https://download.qt.io/online/qtsdkrepository/linux_x64/extensions/qtwebengine/683/x86_64/extensions.qtwebengine.683.linux_gcc_64/6.8.3-0-202503201424qtwebengine-Linux-RHEL_8_10-GCC-Linux-RHEL_8_10-X86_64.7z"
      )
      SET(QT_WEBENGINE_ARCHIVE
          "${CMAKE_BINARY_DIR}/qtwebengine-6.8.3-linux.7z"
      )
    ELSEIF(RV_TARGET_WINDOWS)
      SET(QT_WEBENGINE_URL
          "https://download.qt.io/online/qtsdkrepository/windows_x86/extensions/qtwebengine/683/msvc2022_64/extensions.qtwebengine.683.win64_msvc2022_64/6.8.3-0-202503201424qtwebengine-Windows-Windows_11_23H2-MSVC2022-Windows-Windows_11_23H2-X86_64.7z"
      )
      SET(QT_WEBENGINE_ARCHIVE
          "${CMAKE_BINARY_DIR}/qtwebengine-6.8.3-windows.7z"
      )
    ELSE()
      MESSAGE(
        FATAL_ERROR
          "Failed to determine platform for downloading QtWebEngine. Modify qt6.cmake to add support for this platform.  Installers can be found at https://download.qt.io/online/qtsdkrepository/ under platform/extensions/qtwebengine/683/"
      )
    ENDIF()

    MESSAGE(STATUS "Downloading QtWebEngine from ${QT_WEBENGINE_URL}")
    FILE(
      DOWNLOAD ${QT_WEBENGINE_URL} ${QT_WEBENGINE_ARCHIVE}
      SHOW_PROGRESS
    )

    SET(QT_WEBENGINE_TEMP_DIR
        "${CMAKE_BINARY_DIR}/qtwebengine_temp_extract"
    )
    IF(EXISTS "${QT_WEBENGINE_TEMP_DIR}")
      FILE(REMOVE_RECURSE "${QT_WEBENGINE_TEMP_DIR}")
    ENDIF()
    FILE(MAKE_DIRECTORY "${QT_WEBENGINE_TEMP_DIR}")

    MESSAGE(STATUS "Extracting QtWebEngine to temporary directory: ${QT_WEBENGINE_TEMP_DIR}")
    EXECUTE_PROCESS(
      COMMAND ${SEVEN_ZIP_EXECUTABLE} x ${QT_WEBENGINE_ARCHIVE} -o${QT_WEBENGINE_TEMP_DIR}
      RESULT_VARIABLE extract_result
      OUTPUT_QUIET ERROR_QUIET
    )

    IF(NOT extract_result EQUAL 0)
      MESSAGE(FATAL_ERROR "Failed to extract QtWebEngine archive to temp directory. Result: ${extract_result}.")
    ENDIF()

    MESSAGE(STATUS "Copying extracted files from ${QT_WEBENGINE_TEMP_DIR} to ${RV_DEPS_QT_LOCATION}")
    FILE(
      COPY "${QT_WEBENGINE_TEMP_DIR}/"
      DESTINATION "${RV_DEPS_QT_LOCATION}"
    )

    FILE(REMOVE_RECURSE "${QT_WEBENGINE_TEMP_DIR}")
    FILE(REMOVE "${QT_WEBENGINE_ARCHIVE}")

    MESSAGE(STATUS "QtWebEngine for 6.8.3 installed successfully.")
  ENDIF()
ENDIF()

# Testing if everything is alright. In Qt6, QtWebEngine has been split into Qt6WebEngineCore and Qt6WebEngineWidgets.
FIND_PACKAGE(
  Qt6
  COMPONENTS Core WebEngineCore WebEngineWidgets
  REQUIRED
)

GET_TARGET_PROPERTY(MOC_EXECUTABLE Qt6::moc IMPORTED_LOCATION)
GET_TARGET_PROPERTY(UIC_EXECUTABLE Qt6::uic IMPORTED_LOCATION)

SET(QT_MOC_EXECUTABLE
    "${MOC_EXECUTABLE}"
    CACHE STRING "Qt MOC executable"
)
SET(QT_UIC_EXECUTABLE
    "${UIC_EXECUTABLE}"
    CACHE STRING "Qt UIC executable"
)

SET(_qt_copy_message
    "Copying Qt into ${RV_STAGE_ROOT_DIR}"
)
MESSAGE(STATUS "${_qt_copy_message}")

SET(RV_DEPS_QT_VERSION
    ${Qt6Core_VERSION}
    CACHE STRING "Qt Version String" FORCE
)

IF(RV_USE_SYSTEM_QT)
  # --------------------------------------------------------------------------
  # System Qt staging: plugins + WebEngine support files only (no /usr/lib copy)
  # --------------------------------------------------------------------------
  IF(RV_TARGET_LINUX
     OR RV_TARGET_DARWIN
  )
    IF(EXISTS "${_qt_sys_plugins}")
      MESSAGE(STATUS "Staging system Qt plugins from ${_qt_sys_plugins}")
      FILE(
        GLOB _qt_plugins_dirs
        RELATIVE ${_qt_sys_plugins}
        ${_qt_sys_plugins}/*
      )
      FOREACH(
        _qt_plugin_dir
        ${_qt_plugins_dirs}
      )
        # Skip desktop-environment plugin trees that are not part of Qt itself
        IF(_qt_plugin_dir MATCHES "^(kf6|kiconthemes|kaccounts|plasma|designer)$")
          CONTINUE()
        ENDIF()
        IF(IS_DIRECTORY "${_qt_sys_plugins}/${_qt_plugin_dir}")
          FILE(
            COPY ${_qt_sys_plugins}/${_qt_plugin_dir}
            DESTINATION ${RV_STAGE_PLUGINS_QT_DIR}
          )
        ENDIF()
      ENDFOREACH()
    ELSE()
      MESSAGE(WARNING "System Qt plugins directory not found: ${_qt_sys_plugins}")
    ENDIF()
  ENDIF()

  IF(RV_TARGET_LINUX)
    # WebEngine process (Arch: /usr/lib/qt6/QtWebEngineProcess)
    SET(_webengine_process
        "${_qt_sys_libexec}/QtWebEngineProcess"
    )
    IF(EXISTS "${_webengine_process}")
      MESSAGE(STATUS "Staging QtWebEngineProcess from ${_webengine_process}")
      FILE(MAKE_DIRECTORY "${RV_STAGE_ROOT_DIR}/libexec")
      FILE(
        COPY "${_webengine_process}"
        DESTINATION "${RV_STAGE_ROOT_DIR}/libexec"
      )
      # Also keep a copy under stage root for scripts that expect the Arch path shape
      FILE(
        COPY "${_webengine_process}"
        DESTINATION "${RV_STAGE_ROOT_DIR}"
      )
    ELSE()
      MESSAGE(WARNING "QtWebEngineProcess not found at ${_webengine_process}")
    ENDIF()

    IF(EXISTS "${RV_DEPS_QT_RESOURCES_FOLDER}")
      MESSAGE(STATUS "Copying Qt resources from ${RV_DEPS_QT_RESOURCES_FOLDER}")
      FILE(
        COPY "${RV_DEPS_QT_RESOURCES_FOLDER}"
        DESTINATION "${RV_STAGE_ROOT_DIR}"
      )
    ELSE()
      MESSAGE(WARNING "Qt resources folder missing: ${RV_DEPS_QT_RESOURCES_FOLDER}")
    ENDIF()

    IF(EXISTS "${RV_DEPS_QT_TRANSLATIONS_FOLDER}")
      MESSAGE(STATUS "Copying Qt translations from ${RV_DEPS_QT_TRANSLATIONS_FOLDER}")
      FILE(
        COPY "${RV_DEPS_QT_TRANSLATIONS_FOLDER}"
        DESTINATION "${RV_STAGE_ROOT_DIR}"
      )
    ENDIF()

    # Record paths for runtime env helpers / PySide --qtpaths
    SET(RV_DEPS_QT_LIB_DIR
        "${RV_DEPS_QT_LOCATION}/lib"
        CACHE STRING "System Qt lib dir" FORCE
    )
    SET(RV_DEPS_QT_BIN_DIR
        "${_qt_sys_bin}"
        CACHE STRING "System Qt bin dir(s)" FORCE
    )
  ENDIF()

  MESSAGE(STATUS "System Qt: linking against distro libraries (not vendoring libQt6*.so into stage)")

ELSE()
  # -------------------- Official SDK staging (original behavior) --------------------

  # Common to both Mac and Linux platforms
  IF(RV_TARGET_DARWIN
     OR RV_TARGET_LINUX
  )
    FILE(
      GLOB _qt_plugins_dirs
      RELATIVE ${RV_DEPS_QT_LOCATION}/plugins
      ${RV_DEPS_QT_LOCATION}/plugins/*
    )
    FOREACH(
      _qt_plugin_dir
      ${_qt_plugins_dirs}
    )
      FILE(
        COPY ${RV_DEPS_QT_LOCATION}/plugins/${_qt_plugin_dir}
        DESTINATION ${RV_STAGE_PLUGINS_QT_DIR}
      )
    ENDFOREACH()
  ENDIF()

  # Mac
  IF(RV_TARGET_DARWIN)
    SET(_qt5_lib_dir
        ${RV_DEPS_QT_LOCATION}/lib
    )
    FILE(
      GLOB libs_to_copy
      RELATIVE ${_qt5_lib_dir}
      ${_qt5_lib_dir}/*
    )
    FOREACH(
      lib_to_copy
      ${libs_to_copy}
    )
      IF(lib_to_copy MATCHES "framework"
         AND NOT
             (CMAKE_BUILD_TYPE STREQUAL "Release"
              AND lib_to_copy MATCHES "\\.dSYM$")
      )
        FILE(
          COPY ${_qt5_lib_dir}/${lib_to_copy}
          DESTINATION ${RV_STAGE_FRAMEWORKS_DIR}
        )
      ENDIF()
    ENDFOREACH()
  ENDIF()

  # Linux
  IF(RV_TARGET_LINUX)
    SET(RV_DEPS_QT_LIB_DIR
        ${RV_DEPS_QT_LOCATION}/lib
    )
    FILE(
      GLOB libs_to_copy
      RELATIVE ${RV_DEPS_QT_LIB_DIR}
      CONFIGURE_DEPENDS ${RV_DEPS_QT_LIB_DIR}/*
    )
    FOREACH(
      lib_to_copy
      ${libs_to_copy}
    )
      FILE(
        COPY ${RV_DEPS_QT_LIB_DIR}/${lib_to_copy}
        DESTINATION ${RV_STAGE_LIB_DIR}
      )
    ENDFOREACH()

    MESSAGE(STATUS "Copying Qt libexec files ...")
    FILE(
      COPY "${RV_DEPS_QT_LOCATION}/libexec"
      DESTINATION "${RV_STAGE_ROOT_DIR}"
    )

    MESSAGE(STATUS "Copying Qt resources files ...")
    FILE(
      COPY "${RV_DEPS_QT_RESOURCES_FOLDER}"
      DESTINATION "${RV_STAGE_ROOT_DIR}"
    )

    MESSAGE(STATUS "Copying Qt translations files ...")
    FILE(
      COPY "${RV_DEPS_QT_TRANSLATIONS_FOLDER}"
      DESTINATION "${RV_STAGE_ROOT_DIR}"
    )
  ENDIF()

  # Windows
  IF(RV_TARGET_WINDOWS)
    # Note: On windows, the Qt distribution has both the debug and release versions of its libs, dlls, and plugins into the same directories. This will prevent RV
    # from working correctly. We need to only copy the ones matching the CMAKE_BUILD_TYPE
    FUNCTION(COPY_ONLY_LIBS_MATCHING_BUILD_TYPE SRC_DIR DST_DIR)

      IF(NOT EXISTS (${DST_DIR}))
        FILE(MAKE_DIRECTORY ${DST_DIR})
      ENDIF()

      # First make a list of problematic libs: the ones ending up with d (which means they end with dd in debug)
      FILE(
        GLOB _qt_dd_libs
        RELATIVE ${SRC_DIR}
        ${SRC_DIR}/*dd.*
      )

      FILE(
        GLOB _qt_debug_libs
        RELATIVE ${SRC_DIR}
        ${SRC_DIR}/*d.*
      )

      # If we have dd libs then we need to remove the single d ones from the _qt_debug_libs
      FOREACH(
        _qt_dd_lib
        ${_qt_dd_libs}
      )
        # Determine the name of the associated single d lib so we can remove it from the _qt_debug_libs
        STRING(REPLACE "dd." "d." _d_lib ${_qt_dd_lib})
        LIST(REMOVE_ITEM _qt_debug_libs ${_d_lib})
      ENDFOREACH()

      # Remove Qt6 executables that are not needed.
      FILE(
        GLOB _qt_executables
        RELATIVE ${SRC_DIR}
        ${SRC_DIR}/*.exe
      )

      # Filtering. Some executables are needed for RV to work: QtWebEngineProcess.exe
      FOREACH(
        _qt_executable
        ${_qt_executables}
      )
        IF("${_qt_executable}" STREQUAL "QtWebEngineProcess.exe")
          LIST(REMOVE_ITEM _qt_executables "${_qt_executable}")
        ENDIF()
      ENDFOREACH()

      IF(CMAKE_BUILD_TYPE MATCHES "^Debug$")
        SET(_qt_libs_to_copy
            ${_qt_debug_libs}
        )
      ELSE()
        FILE(
          GLOB _qt_libs_to_copy
          RELATIVE ${SRC_DIR}
          ${SRC_DIR}/*
        )
        LIST(REMOVE_ITEM _qt_libs_to_copy ${_qt_debug_libs})
        LIST(REMOVE_ITEM _qt_libs_to_copy ${_qt_executables})
      ENDIF()

      FOREACH(
        _qt_lib
        ${_qt_libs_to_copy}
      )
        IF(NOT IS_DIRECTORY "${SRC_DIR}/${_qt_lib}")
          FILE(COPY_FILE ${SRC_DIR}/${_qt_lib} ${DST_DIR}/${_qt_lib} ONLY_IF_DIFFERENT)
        ENDIF()
      ENDFOREACH()
    ENDFUNCTION()

    # Copy the Qt plugins
    FILE(
      GLOB _qt_plugins_dirs
      RELATIVE ${RV_DEPS_QT_LOCATION}/plugins
      ${RV_DEPS_QT_LOCATION}/plugins/*
    )
    FOREACH(
      _qt_plugin_dir
      ${_qt_plugins_dirs}
    )
      COPY_ONLY_LIBS_MATCHING_BUILD_TYPE(${RV_DEPS_QT_LOCATION}/plugins/${_qt_plugin_dir} ${RV_STAGE_PLUGINS_QT_DIR}/${_qt_plugin_dir})
    ENDFOREACH()

    # Copy the Qt import libs
    SET(RV_DEPS_QT_LIB_DIR
        ${RV_DEPS_QT_LOCATION}/lib
    )
    COPY_ONLY_LIBS_MATCHING_BUILD_TYPE(${RV_DEPS_QT_LIB_DIR} ${RV_STAGE_LIB_DIR})

    # Copy the Qt dlls
    SET(RV_DEPS_QT_BIN_DIR
        ${RV_DEPS_QT_LOCATION}/bin
    )
    COPY_ONLY_LIBS_MATCHING_BUILD_TYPE(${RV_DEPS_QT_BIN_DIR} ${RV_STAGE_BIN_DIR})

    MESSAGE(STATUS "Copying Qt translations files ...")
    FILE(
      COPY "${RV_DEPS_QT_LOCATION}/translations"
      DESTINATION "${RV_STAGE_ROOT_DIR}"
    )

    MESSAGE(STATUS "Copying Qt resources files ...")
    FILE(
      COPY "${RV_DEPS_QT_LOCATION}/resources"
      DESTINATION "${RV_STAGE_ROOT_DIR}"
    )
  ENDIF()
ENDIF()

# Qt5: OpenGLWidgets is in component Widgets Qt6: OpenGLWidgets is in component OpenGLWidgets
SET(QT6_QOPENGLWIDGET_COMPONENT
    "OpenGLWidgets"
    CACHE STRING "Qt QOpenGLWidget component name"
)
SET(QT6_QOPENGLWIDGET_TARGET
    "Qt6::OpenGLWidgets"
    CACHE STRING "Qt QOpenGLWidget target name"
)

MESSAGE(STATUS "${_qt_copy_message} -- DONE")
MESSAGE(STATUS "Qt version: ${RV_DEPS_QT_VERSION} (system=${RV_USE_SYSTEM_QT})")
