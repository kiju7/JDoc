# PDFium Package Configuration for CMake
# Supports Linux x64, macOS (arm64/x64), and Windows x64.
#
# Features:
#   - Auto-downloads PDFium from GitHub releases if not found locally
#   - Falls back to bundled binaries if download fails
#
# Options:
#   PDFIUM_VERSION    - Version to download (default: 7749)
#   PDFIUM_NO_DOWNLOAD - Set to ON to disable auto-download

include(FindPackageHandleStandardArgs)

set(PDFIUM_VERSION "7749" CACHE STRING "PDFium chromium version to download")
set(PDFIUM_FULL_VERSION "148.0.${PDFIUM_VERSION}.0")
option(PDFIUM_NO_DOWNLOAD "Disable automatic PDFium download" OFF)

# ── Detect platform ──────────────────────────────────────────────
if(WIN32)
  set(_pdfium_platform "win-x64")
  set(_pdfium_libname "pdfium.dll.lib")
  set(_pdfium_dllname "pdfium.dll")
  set(_pdfium_archive "pdfium-win-x64.tgz")
elseif(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(_pdfium_platform "mac-arm64")
    set(_pdfium_archive "pdfium-mac-arm64.tgz")
  else()
    set(_pdfium_platform "mac-x64")
    set(_pdfium_archive "pdfium-mac-x64.tgz")
  endif()
  set(_pdfium_libname "libpdfium.dylib")
else()
  set(_pdfium_platform "linux-x64")
  set(_pdfium_libname "libpdfium.so")
  set(_pdfium_archive "pdfium-linux-x64.tgz")
endif()

set(_pdfium_libdir "${CMAKE_CURRENT_LIST_DIR}/lib/${_pdfium_platform}")
set(_pdfium_bindir "${CMAKE_CURRENT_LIST_DIR}/bin/${_pdfium_platform}")
set(_pdfium_download_url "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F${PDFIUM_VERSION}/${_pdfium_archive}")

# ── Find include directory ───────────────────────────────────────
find_path(PDFium_INCLUDE_DIR
    NAMES "fpdfview.h"
    PATHS "${CMAKE_CURRENT_LIST_DIR}"
    PATH_SUFFIXES "include"
)

# ── Helper function to download PDFium ───────────────────────────
function(_pdfium_download)
  message(STATUS "PDFium: Downloading version ${PDFIUM_VERSION} for ${_pdfium_platform}...")

  set(_download_dir "${CMAKE_CURRENT_LIST_DIR}/_download")
  set(_archive_path "${_download_dir}/${_pdfium_archive}")

  # Create download directory
  file(MAKE_DIRECTORY "${_download_dir}")

  # Download archive
  file(DOWNLOAD
    "${_pdfium_download_url}"
    "${_archive_path}"
    STATUS _download_status
    SHOW_PROGRESS
  )

  list(GET _download_status 0 _download_error)
  if(_download_error)
    list(GET _download_status 1 _download_message)
    message(WARNING "PDFium: Download failed: ${_download_message}")
    file(REMOVE_RECURSE "${_download_dir}")
    return()
  endif()

  # Extract archive
  message(STATUS "PDFium: Extracting ${_pdfium_archive}...")
  file(ARCHIVE_EXTRACT
    INPUT "${_archive_path}"
    DESTINATION "${_download_dir}"
  )

  # Create target directories
  file(MAKE_DIRECTORY "${_pdfium_libdir}")
  if(WIN32)
    file(MAKE_DIRECTORY "${_pdfium_bindir}")
  endif()

  # Copy library files
  if(WIN32)
    file(COPY "${_download_dir}/lib/${_pdfium_libname}" DESTINATION "${_pdfium_libdir}")
    file(COPY "${_download_dir}/bin/${_pdfium_dllname}" DESTINATION "${_pdfium_bindir}")
  else()
    file(COPY "${_download_dir}/lib/${_pdfium_libname}" DESTINATION "${_pdfium_libdir}")
  endif()

  # Copy headers if not present
  if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/include/fpdfview.h")
    file(COPY "${_download_dir}/include/" DESTINATION "${CMAKE_CURRENT_LIST_DIR}/include")
  endif()

  # Cleanup
  file(REMOVE_RECURSE "${_download_dir}")

  message(STATUS "PDFium: Successfully installed version ${PDFIUM_VERSION}")
endfunction()

# ── Find or download library ─────────────────────────────────────
if(WIN32)
  find_file(PDFium_LIBRARY NAMES "${_pdfium_dllname}"
    PATHS "${_pdfium_bindir}" NO_DEFAULT_PATH)
  find_file(PDFium_IMPLIB NAMES "${_pdfium_libname}"
    PATHS "${_pdfium_libdir}" NO_DEFAULT_PATH)

  # Try download if not found
  if((NOT PDFium_LIBRARY OR NOT PDFium_IMPLIB) AND NOT PDFIUM_NO_DOWNLOAD)
    _pdfium_download()
    unset(PDFium_LIBRARY CACHE)
    unset(PDFium_IMPLIB CACHE)
    find_file(PDFium_LIBRARY NAMES "${_pdfium_dllname}"
      PATHS "${_pdfium_bindir}" NO_DEFAULT_PATH)
    find_file(PDFium_IMPLIB NAMES "${_pdfium_libname}"
      PATHS "${_pdfium_libdir}" NO_DEFAULT_PATH)
  endif()

  if(PDFium_LIBRARY AND PDFium_IMPLIB)
    add_library(pdfium SHARED IMPORTED)
    set_target_properties(pdfium PROPERTIES
      IMPORTED_LOCATION "${PDFium_LIBRARY}"
      IMPORTED_IMPLIB "${PDFium_IMPLIB}"
      INTERFACE_INCLUDE_DIRECTORIES "${PDFium_INCLUDE_DIR};${PDFium_INCLUDE_DIR}/cpp"
    )
  endif()

  find_package_handle_standard_args(PDFium
    REQUIRED_VARS PDFium_LIBRARY PDFium_IMPLIB PDFium_INCLUDE_DIR
    VERSION_VAR PDFIUM_FULL_VERSION
  )
else()
  find_library(PDFium_LIBRARY NAMES "pdfium"
    PATHS "${_pdfium_libdir}" NO_DEFAULT_PATH)

  # Try download if not found
  if(NOT PDFium_LIBRARY AND NOT PDFIUM_NO_DOWNLOAD)
    _pdfium_download()
    unset(PDFium_LIBRARY CACHE)
    find_library(PDFium_LIBRARY NAMES "pdfium"
      PATHS "${_pdfium_libdir}" NO_DEFAULT_PATH)
  endif()

  if(PDFium_LIBRARY)
    add_library(pdfium SHARED IMPORTED)
    set_target_properties(pdfium PROPERTIES
      IMPORTED_LOCATION "${PDFium_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${PDFium_INCLUDE_DIR};${PDFium_INCLUDE_DIR}/cpp"
    )
  endif()

  find_package_handle_standard_args(PDFium
    REQUIRED_VARS PDFium_LIBRARY PDFium_INCLUDE_DIR
    VERSION_VAR PDFIUM_FULL_VERSION
  )
endif()
