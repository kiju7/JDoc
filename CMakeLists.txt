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

# ── Common utilities (compiled once) ──────────────────────────

add_library(jdoc_common STATIC src/common/string_utils.cpp)
target_include_directories(jdoc_common PUBLIC include PRIVATE src)

# ── ZIP reader (compiled once) ────────────────────────────────

add_library(jdoc_zip STATIC src/zip_reader.cpp)
target_include_directories(jdoc_zip PUBLIC include PRIVATE src)
target_link_libraries(jdoc_zip PUBLIC ZLIB::ZLIB jdoc_common)

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

# ── Shared library with C API ─────────────────────────────────

add_library(jdoc_shared SHARED src/jdoc_c_api.cpp)
target_link_libraries(jdoc_shared PRIVATE jdoc_pdf jdoc_office jdoc_hwpx jdoc_hwp)
target_include_directories(jdoc_shared PUBLIC include PRIVATE src)
set_target_properties(jdoc_shared PROPERTIES
    OUTPUT_NAME "jdoc"
    VERSION ${PROJECT_VERSION}
    SOVERSION 2
)


# Windows: export all symbols from shared library
if(WIN32)
    set_target_properties(jdoc_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# ── Unified CLI ───────────────────────────────────────────────

add_executable(jdoc src/main.cpp)
target_link_libraries(jdoc PRIVATE jdoc_pdf jdoc_office jdoc_hwpx jdoc_hwp)
target_include_directories(jdoc PRIVATE include src)


# ── Tests ─────────────────────────────────────────────────────

add_executable(test_pdf test/test_pdf.cpp)
target_link_libraries(test_pdf PRIVATE jdoc_pdf)
target_include_directories(test_pdf PRIVATE include)

add_executable(test_office test/test_office.cpp)
target_link_libraries(test_office PRIVATE jdoc_office)
target_include_directories(test_office PRIVATE include src)

# ── Python bindings (pybind11) ────────────────────────────────

option(BUILD_PYTHON "Build Python bindings" OFF)

if(BUILD_PYTHON)
    find_package(pybind11 REQUIRED)
    pybind11_add_module(_jdoc src/python_bindings.cpp)
    target_link_libraries(_jdoc PRIVATE jdoc_pdf jdoc_office jdoc_hwpx jdoc_hwp)
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
install(TARGETS jdoc_shared jdoc_common jdoc_zip jdoc_pdf jdoc_hwpx jdoc_hwp jdoc_office DESTINATION lib)
install(DIRECTORY include/jdoc DESTINATION include)
