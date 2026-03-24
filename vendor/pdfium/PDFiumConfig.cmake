# PDFium Package Configuration for CMake
# Supports Linux x64, macOS (arm64/x64), and Windows x64.
#
# Directory layout:
#   vendor/pdfium/
#     include/          — headers (fpdfview.h, ...)
#     lib/linux-x64/    — libpdfium.so
#     lib/mac-arm64/    — libpdfium.dylib
#     lib/mac-x64/      — libpdfium.dylib
#     lib/win-x64/      — pdfium.dll.lib
#     bin/win-x64/      — pdfium.dll

include(FindPackageHandleStandardArgs)

find_path(PDFium_INCLUDE_DIR
    NAMES "fpdfview.h"
    PATHS "${CMAKE_CURRENT_LIST_DIR}"
    PATH_SUFFIXES "include"
)

set(PDFium_VERSION "147.0.7699.0")

# ── Detect platform sub-directory ──────────────────────────────
if(WIN32)
  set(_pdfium_libdir "lib/win-x64")
  set(_pdfium_bindir "bin/win-x64")
elseif(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(_pdfium_libdir "lib/mac-arm64")
  else()
    set(_pdfium_libdir "lib/mac-x64")
  endif()
else()
  set(_pdfium_libdir "lib/linux-x64")
endif()

# ── Find and import ────────────────────────────────────────────
if(WIN32)
  find_file(PDFium_LIBRARY
        NAMES "pdfium.dll"
        PATHS "${CMAKE_CURRENT_LIST_DIR}/${_pdfium_bindir}"
        NO_DEFAULT_PATH)

  find_file(PDFium_IMPLIB
        NAMES "pdfium.dll.lib"
        PATHS "${CMAKE_CURRENT_LIST_DIR}/${_pdfium_libdir}"
        NO_DEFAULT_PATH)

  add_library(pdfium SHARED IMPORTED)
  set_target_properties(pdfium
    PROPERTIES
    IMPORTED_LOCATION             "${PDFium_LIBRARY}"
    IMPORTED_IMPLIB               "${PDFium_IMPLIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${PDFium_INCLUDE_DIR};${PDFium_INCLUDE_DIR}/cpp"
  )

  find_package_handle_standard_args(PDFium
    REQUIRED_VARS PDFium_LIBRARY PDFium_IMPLIB PDFium_INCLUDE_DIR
    VERSION_VAR PDFium_VERSION
  )
elseif(APPLE)
  find_library(PDFium_LIBRARY
        NAMES "pdfium"
        PATHS "${CMAKE_CURRENT_LIST_DIR}/${_pdfium_libdir}"
        NO_DEFAULT_PATH)

  add_library(pdfium SHARED IMPORTED)
  set_target_properties(pdfium
    PROPERTIES
    IMPORTED_LOCATION             "${PDFium_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PDFium_INCLUDE_DIR};${PDFium_INCLUDE_DIR}/cpp"
  )

  find_package_handle_standard_args(PDFium
    REQUIRED_VARS PDFium_LIBRARY PDFium_INCLUDE_DIR
    VERSION_VAR PDFium_VERSION
  )
else()
  find_library(PDFium_LIBRARY
        NAMES "pdfium"
        PATHS "${CMAKE_CURRENT_LIST_DIR}/${_pdfium_libdir}"
        NO_DEFAULT_PATH)

  add_library(pdfium SHARED IMPORTED)
  set_target_properties(pdfium
    PROPERTIES
    IMPORTED_LOCATION             "${PDFium_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PDFium_INCLUDE_DIR};${PDFium_INCLUDE_DIR}/cpp"
  )

  find_package_handle_standard_args(PDFium
    REQUIRED_VARS PDFium_LIBRARY PDFium_INCLUDE_DIR
    VERSION_VAR PDFium_VERSION
  )
endif()
