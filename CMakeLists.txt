cmake_minimum_required(VERSION 3.16)
project(jdoc VERSION 2.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)

# ── Vendor libraries ────────────────────────────────────────

# pugixml (XML parser for HWPX/OOXML)
add_library(pugixml STATIC vendor/pugixml/pugixml.cpp)
target_include_directories(pugixml PUBLIC vendor/pugixml)

# LZMA SDK (7z container + LZMA/LZMA2 decoders, public domain)
# Decoder-only subset: encoders and all multithreading code (Threads.c,
# MtCoder.c, MtDec.c) are excluded, so decoding is single-threaded.
add_library(lzma_sdk STATIC
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
    vendor/lzma/Bra.c
    vendor/lzma/Bra86.c
    vendor/lzma/Delta.c
    vendor/lzma/CpuArch.c
)
target_include_directories(lzma_sdk PUBLIC vendor/lzma)

# ── Common utilities (compiled once) ──────────────────────────

add_library(jdoc_common STATIC src/common/string_utils.cpp)
target_include_directories(jdoc_common PUBLIC include PRIVATE src)

# ── ZIP reader (compiled once) ────────────────────────────────

add_library(jdoc_zip STATIC src/zip_reader.cpp)
target_include_directories(jdoc_zip PUBLIC include PRIVATE src)
target_link_libraries(jdoc_zip PUBLIC ZLIB::ZLIB jdoc_common)

# ── Archive streams (gz/tar; container readers for convert_archive) ──

# bzip2 (standalone .bz2/.tar.bz2 and alz/egg members) is a minority method:
# optional system libbz2 instead of vendoring. OFF by default; those inputs
# then fail with a clear error.
option(JDOC_WITH_BZIP2 "Decode bzip2 (.bz2, alz/egg members) via system libbz2" OFF)

add_library(jdoc_archive STATIC
    src/archive/input_stream.cpp
    src/archive/tar_reader.cpp
    src/archive/seven_zip_reader.cpp
    src/archive/codec_streams.cpp
    src/archive/alz_reader.cpp
    src/archive/egg_reader.cpp
)
target_include_directories(jdoc_archive PUBLIC include PRIVATE src)
target_link_libraries(jdoc_archive PUBLIC ZLIB::ZLIB jdoc_common lzma_sdk)
if(JDOC_WITH_BZIP2)
    find_package(BZip2 REQUIRED)
    target_compile_definitions(jdoc_archive PRIVATE JDOC_WITH_BZIP2)
    target_link_libraries(jdoc_archive PUBLIC BZip2::BZip2)
endif()

# ── PDF ───────────────────────────────────────────────────────

add_library(jdoc_pdf STATIC src/pdf.cpp)
target_include_directories(jdoc_pdf PUBLIC include PRIVATE src)
target_link_libraries(jdoc_pdf PUBLIC jdoc_common ZLIB::ZLIB JPEG::JPEG)

# ── HWPX (ZIP+XML format) ────────────────────────────────────

add_library(jdoc_hwpx STATIC src/hwpx.cpp)
target_include_directories(jdoc_hwpx PUBLIC include PRIVATE src)
target_link_libraries(jdoc_hwpx PUBLIC pugixml jdoc_zip jdoc_common)

# ── HWP (binary format) ──────────────────────────────────────

add_library(jdoc_hwp STATIC src/hwp.cpp src/legacy/ole_reader.cpp)
target_include_directories(jdoc_hwp PUBLIC include PRIVATE src)
target_link_libraries(jdoc_hwp PUBLIC ZLIB::ZLIB jdoc_common)

# ── Office (DOCX/XLSX/XLSB/PPTX/DOC/XLS/PPT/RTF/HTML) ────────

add_library(jdoc_office STATIC
    src/office.cpp
    src/ooxml/docx_parser.cpp
    src/ooxml/pptx_parser.cpp
    src/ooxml/xlsx_parser.cpp
    src/ooxml/xlsb_parser.cpp
    src/legacy/ole_reader.cpp
    src/legacy/doc_parser.cpp
    src/legacy/xls_parser.cpp
    src/legacy/ppt_parser.cpp
    src/legacy/rtf_parser.cpp
    src/html/html_parser.cpp
)
target_include_directories(jdoc_office PUBLIC include PRIVATE src vendor)
target_link_libraries(jdoc_office PUBLIC pugixml jdoc_zip jdoc_common)

# ── Unified API (auto-detect format) ─────────────────────────

add_library(jdoc_all STATIC src/jdoc.cpp src/archive/archive_walker.cpp)
target_include_directories(jdoc_all PUBLIC include PRIVATE src)
target_link_libraries(jdoc_all PUBLIC jdoc_pdf jdoc_office jdoc_hwpx jdoc_hwp
                                      jdoc_zip jdoc_archive)

# ── Shared library with C API ─────────────────────────────────

add_library(jdoc_shared SHARED src/jdoc_c_api.cpp)
target_link_libraries(jdoc_shared PRIVATE jdoc_all)
target_include_directories(jdoc_shared PUBLIC include PRIVATE src)
set_target_properties(jdoc_shared PROPERTIES
    OUTPUT_NAME "jdoc"
    VERSION ${PROJECT_VERSION}
    SOVERSION 3
)


# Windows: export all symbols from shared library
if(WIN32)
    set_target_properties(jdoc_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# ── Unified CLI ───────────────────────────────────────────────

add_executable(jdoc src/main.cpp)
target_link_libraries(jdoc PRIVATE jdoc_all)
target_include_directories(jdoc PRIVATE include src)

# GCC 8 requires explicit linking for std::filesystem
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(jdoc PRIVATE stdc++fs)
endif()


# ── Tests ─────────────────────────────────────────────────────

add_executable(test_pdf test/test_pdf.cpp)
target_link_libraries(test_pdf PRIVATE jdoc_pdf)
target_include_directories(test_pdf PRIVATE include)

add_executable(test_office test/test_office.cpp)
target_link_libraries(test_office PRIVATE jdoc_office)
target_include_directories(test_office PRIVATE include src)

add_executable(test_archive test/test_archive.cpp)
target_link_libraries(test_archive PRIVATE jdoc_all ZLIB::ZLIB)
target_include_directories(test_archive PRIVATE include src)

add_executable(bench_convert test/bench_convert.cpp)
target_link_libraries(bench_convert PRIVATE jdoc_all)
target_include_directories(bench_convert PRIVATE include)

# ── Python bindings (pybind11) ────────────────────────────────

option(BUILD_PYTHON "Build Python bindings" OFF)

if(BUILD_PYTHON)
    find_package(pybind11 REQUIRED)
    pybind11_add_module(_jdoc src/python_bindings.cpp)
    target_link_libraries(_jdoc PRIVATE jdoc_all)
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
install(TARGETS jdoc_shared jdoc_all jdoc_common jdoc_zip jdoc_pdf jdoc_hwpx jdoc_hwp jdoc_office DESTINATION lib)
install(DIRECTORY include/jdoc DESTINATION include)
