# Post-install helper for RV_DEPS_OCIO: ensure libOpenColorIO DT_NEEDEDs libz-ng.
# OCIO 2.5 minizip-ng references zng_* but the shared lib may not record NEEDED.

IF(NOT DEFINED ENV{PATCHELF} OR "$ENV{PATCHELF}" STREQUAL "")
  MESSAGE(STATUS "ocio_patchelf_zng: patchelf not set, skipping")
  RETURN()
ENDIF()

SET(_so "$ENV{OCIO_SO}")
IF(NOT EXISTS "${_so}")
  SET(_so "$ENV{OCIO_SO64}")
ENDIF()

IF(NOT EXISTS "${_so}")
  MESSAGE(STATUS "ocio_patchelf_zng: libOpenColorIO not found, skipping")
  RETURN()
ENDIF()

EXECUTE_PROCESS(
  COMMAND "$ENV{PATCHELF}" --print-needed "${_so}"
  OUTPUT_VARIABLE _needed
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _rc
)
IF(NOT _rc EQUAL 0)
  MESSAGE(WARNING "ocio_patchelf_zng: patchelf --print-needed failed on ${_so}")
  RETURN()
ENDIF()

IF("${_needed}" MATCHES "libz-ng\\.so")
  MESSAGE(STATUS "ocio_patchelf_zng: ${_so} already NEEDEDs libz-ng")
  RETURN()
ENDIF()

EXECUTE_PROCESS(
  COMMAND "$ENV{PATCHELF}" --add-needed libz-ng.so.2 "${_so}"
  RESULT_VARIABLE _rc2
)
IF(_rc2 EQUAL 0)
  MESSAGE(STATUS "ocio_patchelf_zng: added NEEDED libz-ng.so.2 to ${_so}")
ELSE()
  MESSAGE(WARNING "ocio_patchelf_zng: failed to add NEEDED on ${_so}")
ENDIF()
