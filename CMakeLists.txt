cmake_minimum_required(VERSION 3.16)
project(jdoc VERSION 1.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# MSVC assumes the system code page for sources and literals; force UTF-8 so
# the non-ASCII (Korean) string literals in sources and tests survive.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# A bare `cmake -S . -B build` leaves CMAKE_BUILD_TYPE empty on single-config
# generators, which means no optimization flags at all — a -O0 build that looks
# normal. Release is the only sensible default for a conversion library.
# (Release already implies -O3 -DNDEBUG on GCC/Clang; spelling -O3 out again
# would be a no-op.)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

# ── Dependencies (zlib, libjpeg) ────────────────────────────
# Prefer a system copy; on machines without one (typically a clean Windows box)
# fall back to fetching and building it, so `cmake -B build` works out of the
# box on Linux, macOS and Windows without a package manager.
include(FetchContent)

# zlib: still needed for the streaming gzip/tar.gz path (GzInflateStream), which
# libdeflate cannot serve (it has no streaming API). Prefer the system copy —
# on macOS that is Apple's specially tuned build, the fastest inflate available
# on Apple Silicon — and fall back to fetching madler-zlib on a clean box.
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    message(STATUS "System zlib not found — fetching and building it (FetchContent)")
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()
    target_include_directories(zlibstatic INTERFACE
        $<BUILD_INTERFACE:${zlib_SOURCE_DIR}>
        $<BUILD_INTERFACE:${zlib_BINARY_DIR}>)
endif()

# libdeflate: whole-buffer DEFLATE/zlib, ~1.3-1.6x faster than zlib/zlib-ng for
# inflate and 2-3x for deflate. Every "read whole member then (de)compress" path
# (zip members, PDF FlateDecode, HWP sections, office metafiles, PNG encode) uses
# it; only the streaming gz path stays on zlib. Portable C, no NASM, MSVC-clean.
FetchContent_Declare(libdeflate
    GIT_REPOSITORY https://github.com/ebiggers/libdeflate.git
    GIT_TAG v1.20
    GIT_SHALLOW TRUE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libdeflate)

find_package(JPEG QUIET)
if(NOT JPEG_FOUND)
    message(STATUS "System libjpeg not found — building libjpeg-turbo (ExternalProject)")
    # libjpeg-turbo refuses add_subdirectory()/FetchContent, so build+install it
    # as a standalone project and consume the result as an imported static lib.
    include(ExternalProject)
    set(_jpeg_prefix ${CMAKE_BINARY_DIR}/_deps/libjpeg-turbo)
    # libjpeg-turbo installs its static lib as "libjpeg.a" everywhere except
    # MSVC, where it keeps the target name and installs "jpeg-static.lib".
    if(MSVC)
        set(_jpeg_libname jpeg-static)
    else()
        set(_jpeg_libname jpeg)
    endif()
    set(_jpeg_lib ${_jpeg_prefix}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${_jpeg_libname}${CMAKE_STATIC_LIBRARY_SUFFIX})
    ExternalProject_Add(libjpeg_turbo_ext
        GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
        GIT_TAG 3.0.4
        GIT_SHALLOW TRUE
        PREFIX ${_jpeg_prefix}
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${_jpeg_prefix}
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DENABLE_SHARED=OFF
            -DENABLE_STATIC=ON
            -DWITH_TURBOJPEG=OFF
            -DWITH_SIMD=ON             # SIMD decode; x86 needs NASM (CI installs it), arm64 uses NEON
            -DWITH_CRT_DLL=ON         # use /MD to match this project (no LIBCMT clash)
            # Relay any compiler-cache launcher (sccache in CI); empty = no-op.
            -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
            -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
        BUILD_BYPRODUCTS ${_jpeg_lib})
    # The include dir must exist at configure time for the imported target check.
    file(MAKE_DIRECTORY ${_jpeg_prefix}/include)
    add_library(JPEG::JPEG STATIC IMPORTED GLOBAL)
    set_target_properties(JPEG::JPEG PROPERTIES
        IMPORTED_LOCATION ${_jpeg_lib}
        INTERFACE_INCLUDE_DIRECTORIES ${_jpeg_prefix}/include)
    add_dependencies(JPEG::JPEG libjpeg_turbo_ext)
endif()

find_package(Threads REQUIRED)

# ── Vendor libraries (compiled as objects, folded into libjdoc) ──────

# pugixml (XML parser for HWPX/OOXML)
add_library(pugixml OBJECT vendor/pugixml/pugixml.cpp)
target_include_directories(pugixml PUBLIC vendor/pugixml)

# LZMA SDK (7z container + LZMA/LZMA2 decoders, public domain)
# Decoder-only subset: encoders and all multithreading code (Threads.c,
# MtCoder.c, MtDec.c) are excluded, so decoding is single-threaded.
add_library(lzma_sdk OBJECT
    vendor/lzma/7zAlloc.c
    vendor/lzma/7zArcIn.c
    vendor/lzma/7zBuf.c
    vendor/lzma/7zCrc.c
    vendor/lzma/7zCrcOpt.c
    vendor/lzma/7zDec.c
    vendor/lzma/7zStream.c
    vendor/lzma/LzmaDec.c
    vendor/lzma/Lzma2Dec.c
    vendor/lzma/Bcj2.c
    vendor/lzma/Ppmd7.c
    vendor/lzma/Ppmd7Dec.c
    vendor/lzma/Bra.c
    vendor/lzma/Bra86.c
    vendor/lzma/Delta.c
    vendor/lzma/CpuArch.c
)
target_include_directories(lzma_sdk PUBLIC vendor/lzma)
target_compile_definitions(lzma_sdk PRIVATE Z7_PPMD_SUPPORT)

# ── Core library (single static archive: libjdoc.a) ──────────────────
# Every format parser, the archive walker and the auto-detecting API are
# compiled into one library. bzip2 stays optional; when off, those inputs
# fail with a clear error.
option(JDOC_WITH_BZIP2 "Decode bzip2 (.bz2, alz/egg members) via system libbz2" OFF)

add_library(jdoc_lib STATIC
    # common + zip
    src/common/string_utils.cpp
    src/common/inflate.cpp
    src/zip_reader.cpp
    # archive containers + walker
    src/archive/input_stream.cpp
    src/archive/tar_reader.cpp
    src/archive/seven_zip_reader.cpp
    src/archive/codec_streams.cpp
    src/archive/alz_reader.cpp
    src/archive/rar_reader.cpp
    src/archive/egg_reader.cpp
    src/archive/archive_walker.cpp
    # pdf
    src/pdf/pdf_core.cpp
    src/pdf/pdf_content.cpp
    src/pdf/pdf_tables.cpp
    src/pdf/pdf_visual.cpp
    src/pdf/pdf_markdown.cpp
    src/pdf/pdf.cpp
    src/pdf/pdf_crypt.cpp
    # hwpx
    src/hwpx.cpp
    # hwp
    src/hwp.cpp
    src/legacy/hwp3_parser.cpp
    # office
    src/office.cpp
    src/ooxml/docx_parser.cpp
    src/ooxml/pptx_parser.cpp
    src/ooxml/xlsx_parser.cpp
    src/ooxml/xlsb_parser.cpp
    src/odf/odf_parser.cpp
    src/legacy/doc_parser.cpp
    src/legacy/xls_parser.cpp
    src/legacy/ppt_parser.cpp
    src/legacy/rtf_parser.cpp
    src/html/html_parser.cpp
    src/html/html_charset.cpp
    # mail
    src/mail/eml_parser.cpp
    src/mail/msg_parser.cpp
    # OLE reader — shared by hwp and office, compiled once
    src/legacy/ole_reader.cpp
    # unified auto-detect API
    src/jdoc.cpp
    # vendored code, folded in as objects
    $<TARGET_OBJECTS:pugixml>
    $<TARGET_OBJECTS:lzma_sdk>
)
set_target_properties(jdoc_lib PROPERTIES OUTPUT_NAME jdoc)
# On Windows the shared library's import lib is also jdoc.lib, which would
# collide with the static jdoc.lib (LNK1149); give the static one its own name.
if(WIN32)
    set_target_properties(jdoc_lib PROPERTIES OUTPUT_NAME jdoc_static)
endif()
target_include_directories(jdoc_lib
    PUBLIC include
    PRIVATE src vendor vendor/pugixml vendor/lzma)
target_link_libraries(jdoc_lib PUBLIC ZLIB::ZLIB JPEG::JPEG Threads::Threads
                                       libdeflate::libdeflate_static)
if(JDOC_WITH_BZIP2)
    find_package(BZip2 REQUIRED)
    target_compile_definitions(jdoc_lib PRIVATE JDOC_WITH_BZIP2)
    target_link_libraries(jdoc_lib PUBLIC BZip2::BZip2)
endif()

# ── Shared library with C API ─────────────────────────────────

add_library(jdoc_shared SHARED src/jdoc_c_api.cpp)
target_link_libraries(jdoc_shared PRIVATE jdoc_lib)
target_include_directories(jdoc_shared PUBLIC include PRIVATE src)
set_target_properties(jdoc_shared PROPERTIES
    OUTPUT_NAME "jdoc"
    VERSION ${PROJECT_VERSION}
    SOVERSION 4
)


# Windows: export all symbols from shared library
if(WIN32)
    set_target_properties(jdoc_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# ── Unified CLI ───────────────────────────────────────────────

add_executable(jdoc src/main.cpp)
target_link_libraries(jdoc PRIVATE jdoc_lib)
target_include_directories(jdoc PRIVATE include src)

# GCC 8 requires explicit linking for std::filesystem
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(jdoc PRIVATE stdc++fs)
endif()


# ── Tests ─────────────────────────────────────────────────────
# Opt-in: `cmake -B build` yields just the library + CLI. Configure with
# -DJDOC_BUILD_TESTS=ON to build them, then run with `ctest`.
option(JDOC_BUILD_TESTS "Build unit tests" OFF)

if(JDOC_BUILD_TESTS)
    enable_testing()
    foreach(t IN ITEMS test_pdf test_pdf_crypt test_office test_archive
                       test_hwp3 test_hwp5 test_hwpx test_eml test_msg test_odf)
        add_executable(${t} test/${t}.cpp)
        target_link_libraries(${t} PRIVATE jdoc_lib)
        target_include_directories(${t} PRIVATE include src)
        # Tests resolve fixtures relative to the repo root (test/fixtures/…).
        add_test(NAME ${t} COMMAND ${t}
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endforeach()
endif()

# ── Python bindings (pybind11) ────────────────────────────────

option(BUILD_PYTHON "Build Python bindings" OFF)

if(BUILD_PYTHON)
    find_package(pybind11 REQUIRED)
    pybind11_add_module(_jdoc src/python_bindings.cpp)
    target_link_libraries(_jdoc PRIVATE jdoc_lib)
    target_include_directories(_jdoc PRIVATE include src)
    if(APPLE)
        set_target_properties(_jdoc PROPERTIES
            INSTALL_RPATH "@loader_path"
            BUILD_RPATH "@loader_path"
        )
    elseif(UNIX)
        set_target_properties(_jdoc PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_RPATH "$ORIGIN"
        )
    endif()
endif()

# ── Install ───────────────────────────────────────────────────

install(TARGETS jdoc DESTINATION bin)
install(TARGETS jdoc_lib jdoc_shared DESTINATION lib)
install(DIRECTORY include/jdoc DESTINATION include)
