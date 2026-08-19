#include "legacy/ole_reader.h"
#include "common/png_encode.h"
#include "common/string_utils.h"
#include "pdf/pdf_content.h"
#include "pdf/pdf_extract.h"
#include "jdoc/detect.h"
#include "jdoc/jdoc.h"
#include "jdoc/pdf.h"

#include <zlib.h>

#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) \
            throw std::runtime_error("Check failed: " #condition); \
    } while (false)

constexpr uint32_t kEndOfChain = 0xFFFFFFFE;
constexpr uint32_t kFreeSector = 0xFFFFFFFF;
constexpr uint32_t kFatSector = 0xFFFFFFFD;
constexpr uint32_t kNoStream = 0xFFFFFFFF;

void put_u16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

std::vector<uint8_t> minimal_ole() {
    std::vector<uint8_t> bytes(3 * 512, 0);
    const uint8_t magic[8] =
        {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    std::memcpy(bytes.data(), magic, sizeof(magic));
    put_u16(bytes, 0x1A, 3);
    put_u16(bytes, 0x1C, 0xFFFE);
    put_u16(bytes, 0x1E, 9);
    put_u16(bytes, 0x20, 6);
    put_u32(bytes, 0x2C, 1);
    put_u32(bytes, 0x30, 1);
    put_u32(bytes, 0x38, 4096);
    put_u32(bytes, 0x3C, kEndOfChain);
    put_u32(bytes, 0x44, kEndOfChain);
    for (size_t offset = 0x4C; offset < 512; offset += 4)
        put_u32(bytes, offset, kFreeSector);
    put_u32(bytes, 0x4C, 0);

    put_u32(bytes, 512, kFatSector);
    put_u32(bytes, 516, kEndOfChain);
    for (size_t offset = 520; offset < 1024; offset += 4)
        put_u32(bytes, offset, kFreeSector);
    return bytes;
}

std::string image_pdf() {
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> offsets(6);
    auto object = [&](int id, const std::string& body) {
        offsets[static_cast<size_t>(id)] = pdf.size();
        pdf += std::to_string(id) + " 0 obj\n" + body + "\nendobj\n";
    };
    object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    object(3,
           "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
           "/Resources << /XObject << /Im0 4 0 R >> >> "
           "/Contents 5 0 R >>");
    std::string image =
        "<< /Type /XObject /Subtype /Image /Width 1 /Height 1 "
        "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 3 >>\nstream\n";
    image.append("\xFF\0\0", 3);
    image += "\nendstream";
    object(4, image);
    const std::string content = "q 10 0 0 10 0 0 cm /Im0 Do Q";
    object(5, "<< /Length " + std::to_string(content.size()) +
              " >>\nstream\n" + content + "\nendstream");

    const size_t xref = pdf.size();
    pdf += "xref\n0 6\n0000000000 65535 f \n";
    for (int id = 1; id <= 5; ++id) {
        std::ostringstream row;
        row << std::setw(10) << std::setfill('0')
            << offsets[static_cast<size_t>(id)]
            << " 00000 n \n";
        pdf += row.str();
    }
    pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

std::string unicode_pdf() {
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> offsets(7);
    auto object = [&](int id, const std::string& body) {
        offsets[static_cast<size_t>(id)] = pdf.size();
        pdf += std::to_string(id) + " 0 obj\n" + body + "\nendobj\n";
    };
    object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    object(3,
           "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
           "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>");
    object(4,
           "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
           "/ToUnicode 6 0 R >>");
    const std::string content = "BT /F1 12 Tf 10 50 Td <41> Tj ET";
    object(5, "<< /Length " + std::to_string(content.size()) +
              " >>\nstream\n" + content + "\nendstream");
    const std::string cmap =
        "1 beginbfchar\n<41> <D835DC00>\nendbfchar";
    object(6, "<< /Length " + std::to_string(cmap.size()) +
              " >>\nstream\n" + cmap + "\nendstream");

    const size_t xref = pdf.size();
    pdf += "xref\n0 7\n0000000000 65535 f \n";
    for (int id = 1; id <= 6; ++id) {
        std::ostringstream row;
        row << std::setw(10) << std::setfill('0')
            << offsets[static_cast<size_t>(id)]
            << " 00000 n \n";
        pdf += row.str();
    }
    pdf += "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

// Assemble a PDF from object bodies numbered 1..n, with a matching xref.
std::string assemble_pdf(const std::vector<std::string>& bodies) {
    const size_t count = bodies.size();
    std::string pdf = "%PDF-1.7\n";
    std::vector<size_t> offsets(count + 1, 0);
    for (size_t id = 1; id <= count; ++id) {
        offsets[id] = pdf.size();
        pdf += std::to_string(id) + " 0 obj\n" + bodies[id - 1] + "\nendobj\n";
    }
    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(count + 1) + "\n0000000000 65535 f \n";
    for (size_t id = 1; id <= count; ++id) {
        std::ostringstream row;
        row << std::setw(10) << std::setfill('0') << offsets[id] << " 00000 n \n";
        pdf += row.str();
    }
    pdf += "trailer\n<< /Size " + std::to_string(count + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

std::string stream_object(const std::string& dict_extra,
                          const std::string& payload) {
    return "<< " + dict_extra + " /Length " + std::to_string(payload.size()) +
           " >>\nstream\n" + payload + "\nendstream";
}

// One label per quarter turn, the way a CAD export writes dimension text
// around a drawing: upright, and rotated 90 / 180 / 270 degrees.
std::string rotated_text_pdf() {
    const std::string content =
        "BT /F1 12 Tf\n"
        "1 0 0 1 40 300 Tm (UPRIGHT) Tj\n"
        "0 1 -1 0 40 100 Tm (QUARTER) Tj\n"
        "-1 0 0 -1 170 300 Tm (HALFTURN) Tj\n"
        "0 -1 1 0 200 100 Tm (THREEQTR) Tj\n"
        "ET";
    return assemble_pdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        stream_object("", content),
    });
}

// A ruled 3x4 table whose middle cell holds a 180°-rotated label — the shape a
// CAD title block makes when it repeats the drawing number upside down along
// the sheet edge. Cell assembly is a separate path from line layout, so the
// quarter-turn coverage above does not reach it.
std::string rotated_table_pdf() {
    const std::string content =
        "q 0.6 w 0 G\n"
        "30 210 m 280 210 l S\n30 170 m 280 170 l S\n30 130 m 280 130 l S\n"
        "30 90 m 280 90 l S\n30 50 m 280 50 l S\n"
        "30 50 m 30 210 l S\n120 50 m 120 210 l S\n"
        "210 50 m 210 210 l S\n280 50 m 280 210 l S\nQ\n"
        "BT /F1 10 Tf\n"
        "1 0 0 1 36 184 Tm (ALPHA) Tj\n"
        "1 0 0 1 126 184 Tm (BRAVO) Tj\n"
        "1 0 0 1 216 184 Tm (CHARLIE) Tj\n"
        "1 0 0 1 36 144 Tm (DELTA) Tj\n"
        "1 0 0 1 216 144 Tm (FOXTROT) Tj\n"
        "1 0 0 1 36 104 Tm (GOLF) Tj\n"
        "1 0 0 1 126 104 Tm (HOTEL) Tj\n"
        "1 0 0 1 216 104 Tm (INDIA) Tj\n"
        "1 0 0 1 36 64 Tm (JULIET) Tj\n"
        "1 0 0 1 126 64 Tm (KILO) Tj\n"
        "1 0 0 1 216 64 Tm (LIMA) Tj\n"
        // Advances toward -x, so the origin is the run's top-right corner.
        // Placed on INDIA's baseline on purpose: the borderless table builder
        // groups rows by page-space y, so this is what puts an upright run and
        // a half-turn run in the SAME cell and exercises the separator between
        // two writing frames. Give it its own y and the two never meet.
        "-1 0 0 -1 275 104 Tm (ECHO) Tj\n"
        "ET";
    return assemble_pdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 250] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        stream_object("", content),
    });
}

// Two embedded files reached through a branching name tree. The first is
// registered under two keys and carries a UTF-16BE name; the second declares
// no /Params, so its size has to come from the stream length.
std::string attachment_pdf() {
    const std::string payload = "AC1027 not really a drawing";
    return assemble_pdf({
        "<< /Type /Catalog /Pages 2 0 R "
        "/Names << /EmbeddedFiles 6 0 R >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
        "/Resources << /Font << /F1 13 0 R >> >> /Contents 14 0 R "
        "/Annots [10 0 R] >>",
        "<< /Kids [7 0 R] >>",                       // unused filler slot 4
        "<< /Type /Filespec /F (notes.txt) /EF << /F 9 0 R >> >>",
        "<< /Kids [7 0 R 8 0 R] >>",                 // name tree root
        // 도면.dwg in UTF-16BE, listed twice to exercise de-duplication.
        "<< /Names [ (a) 11 0 R (b) 11 0 R ] >>",
        "<< /Names [ (n) 5 0 R ] >>",
        stream_object("/Type /EmbeddedFile", payload),
        "<< /Type /Annot /Subtype /FileAttachment /Rect [10 10 30 30] "
        "/FS 11 0 R /Contents (drawing attached) >>",
        "<< /Type /Filespec "
        "/UF <FEFFB3C4BA74002E006400770067> /F (drawing.dwg) "
        "/EF << /F 12 0 R >> /Desc (site plan) >>",
        stream_object("/Type /EmbeddedFile /Params << /Size 2097152 >>", payload),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        stream_object("", "BT /F1 12 Tf 20 150 Td (body text) Tj ET"),
    });
}

// A name tree whose only node lists itself twice. Bounding the walk by depth
// alone would fan this out 2^depth times.
std::string cyclic_name_tree_pdf() {
    return assemble_pdf({
        "<< /Type /Catalog /Pages 2 0 R "
        "/Names << /EmbeddedFiles 4 0 R >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] >>",
        "<< /Kids [4 0 R 4 0 R] >>",
    });
}

void put_directory_entry(std::vector<uint8_t>& bytes, size_t index,
                         const char* name, uint8_t type) {
    const size_t offset = 1024 + index * 128;
    const size_t length = std::strlen(name);
    for (size_t i = 0; i < length; ++i)
        put_u16(bytes, offset + i * 2, static_cast<uint8_t>(name[i]));
    put_u16(bytes, offset + 0x40,
            static_cast<uint16_t>((length + 1) * 2));
    bytes[offset + 0x42] = type;
    put_u32(bytes, offset + 0x44, kNoStream);
    put_u32(bytes, offset + 0x48, kNoStream);
    put_u32(bytes, offset + 0x4C, kNoStream);
    put_u32(bytes, offset + 0x74, kEndOfChain);
}

void test_ole_rejects_invalid_sector_shift() {
    auto bytes = minimal_ole();
    put_u16(bytes, 0x1E, 31);
    jdoc::OleReader reader(bytes.data(), bytes.size());
    CHECK(!reader.is_open());
}

void test_ole_rejects_oversized_directory_name() {
    auto bytes = minimal_ole();
    put_u16(bytes, 1024 + 0x40, 66);
    jdoc::OleReader reader(bytes.data(), bytes.size());
    CHECK(!reader.is_open());
}

void test_ole_directory_cycle_terminates() {
    auto bytes = minimal_ole();
    put_directory_entry(bytes, 0, "Root Entry", 5);
    put_directory_entry(bytes, 1, "Data", 2);
    put_u32(bytes, 1024 + 0x4C, 1);
    put_u32(bytes, 1024 + 128 + 0x44, 1);

    jdoc::OleReader reader(bytes.data(), bytes.size());
    CHECK(reader.is_open());
    const auto names = reader.list_streams();
    CHECK(names.size() == 1);
    CHECK(names[0] == "Data");
}

void test_ole_rejects_stream_larger_than_source() {
    auto bytes = minimal_ole();
    put_directory_entry(bytes, 0, "Root Entry", 5);
    put_directory_entry(bytes, 1, "Data", 2);
    put_u32(bytes, 1024 + 0x4C, 1);
    put_u32(bytes, 1024 + 128 + 0x74, 1);
    put_u32(bytes, 1024 + 128 + 0x78, UINT32_MAX);

    jdoc::OleReader reader(bytes.data(), bytes.size());
    CHECK(reader.is_open());
    bool rejected = false;
    try {
        reader.read_stream("Data");
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected);
}

void test_png_rejects_short_pixels() {
    const uint8_t pixels[3] = {0, 0, 0};
    CHECK(jdoc::util::pixels_to_png(
        pixels, sizeof(pixels), 2, 2, 3).empty());
}

void test_png_converts_cmyk() {
    const uint8_t cmyk[4] = {0, 255, 255, 0};
    const auto png =
        jdoc::util::pixels_to_png(cmyk, sizeof(cmyk), 1, 1, 4);
    CHECK(png.size() > 45);

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(png.data());
    const uint32_t compressed_size =
        (uint32_t{bytes[33]} << 24) | (uint32_t{bytes[34]} << 16) |
        (uint32_t{bytes[35]} << 8) | bytes[36];
    CHECK(std::memcmp(bytes + 37, "IDAT", 4) == 0);

    uint8_t raw[4] = {};
    uLongf raw_size = sizeof(raw);
    CHECK(uncompress(raw, &raw_size, bytes + 41, compressed_size) == Z_OK);
    CHECK(raw_size == sizeof(raw));
    CHECK(raw[0] == 0);
    // Full magenta+yellow ink through the Acrobat-matching conversion is a
    // printed red, not the additive primary the old linear formula produced.
    CHECK(raw[1] == 255 && raw[2] == 46 && raw[3] == 23);
}

void test_pdf_honors_images_option() {
    const std::string pdf = image_pdf();
    jdoc::ConvertOptions opts;
    opts.images = false;
    opts.min_image_size = 0;
    const auto without_images = jdoc::pdf_to_markdown_chunks_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size(), opts);
    CHECK(without_images.size() == 1);
    CHECK(without_images[0].images.empty());

    opts.images = true;
    const auto with_images = jdoc::pdf_to_markdown_chunks_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size(), opts);
    CHECK(with_images.size() == 1);
    CHECK(with_images[0].images.size() == 1);
    CHECK(!with_images[0].images[0].data.empty() ||
          !with_images[0].images[0].pixels.empty());

    const auto markdown = jdoc::pdf_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size(), opts);
    CHECK(markdown.find("![") != std::string::npos);
}

void test_pdf_composite_clips_extreme_coordinates_before_narrowing() {
    using namespace jdoc::pdf_detail;

    const uint8_t placeholder = 0;
    PdfDoc doc(&placeholder, 1);

    auto image = PdfObj::make_dict();
    image.type = ObjType::STREAM;
    image.dict.push_back({"Subtype", PdfObj::make_name("Image")});
    image.dict.push_back({"Width", PdfObj::make_int(1)});
    image.dict.push_back({"Height", PdfObj::make_int(1)});
    image.dict.push_back({"BitsPerComponent", PdfObj::make_int(8)});
    image.dict.push_back({"ColorSpace", PdfObj::make_name("DeviceRGB")});
    image.stream_data = {255, 0, 0};

    auto mask = PdfObj::make_dict();
    mask.type = ObjType::STREAM;
    mask.dict.push_back({"Subtype", PdfObj::make_name("Image")});
    mask.dict.push_back({"Width", PdfObj::make_int(1)});
    mask.dict.push_back({"Height", PdfObj::make_int(1)});
    mask.dict.push_back({"BitsPerComponent", PdfObj::make_int(1)});
    mask.dict.push_back({"ImageMask", PdfObj::make_bool(true)});
    mask.stream_data = {0};

    auto xobjects = PdfObj::make_dict();
    xobjects.dict.push_back({"Im0", image});
    xobjects.dict.push_back({"Mask0", mask});
    auto resources = PdfObj::make_dict();
    resources.dict.push_back({"XObject", xobjects});

    ContentParseResult parsed;
    RenderPath path{};
    constexpr double kExtreme = 1e11;
    path.points = {
        {-kExtreme, -kExtreme, PathPoint::MOVE},
        { kExtreme, -kExtreme, PathPoint::LINE},
        { kExtreme,  kExtreme, PathPoint::LINE},
        {-kExtreme,  kExtreme, PathPoint::LINE},
        {0, 0, PathPoint::CLOSE},
    };
    path.fill_r = 0.2; path.fill_g = 0.3; path.fill_b = 0.4;
    path.stroke_r = 0; path.stroke_g = 0; path.stroke_b = 0;
    path.line_width = 1;
    path.do_fill = true;
    path.do_stroke = true;
    parsed.paths.push_back(path);

    auto placement = [](const char* name, const double (&ctm)[6], int seq) {
        ImagePlacement ip{};
        ip.xobj_name = name;
        std::memcpy(ip.ctm, ctm, sizeof(ip.ctm));
        ip.alpha = 1;
        ip.seq = seq;
        return ip;
    };
    // Both placements cross the page but have coordinates far outside int.
    // The axis-aligned case also verifies that work is limited to visible
    // pixels instead of iterating over the full 2e11-unit destination.
    const double axis[6] =
        {2 * kExtreme, 0, 0, 2 * kExtreme, -kExtreme, -kExtreme};
    const double rotated[6] =
        {0, 2 * kExtreme, -2 * kExtreme, 0, kExtreme, -kExtreme};
    parsed.images.push_back(placement("Im0", axis, 1));
    parsed.images.push_back(placement("Im0", rotated, 2));
    parsed.images.push_back(placement("Mask0", axis, 3));
    parsed.images.push_back(placement("Mask0", rotated, 4));

    auto rendered = render_page_composite(doc, resources, parsed, 0, 100, 100, "");
    CHECK(rendered.width > 0);
    CHECK(rendered.height > 0);
    CHECK(!rendered.data.empty());

    auto invalid = render_page_composite(
        doc, resources, {}, 0, std::numeric_limits<double>::infinity(), 100, "");
    CHECK(invalid.data.empty());
}

void test_pdf_region_composite_matches_translated_geometry() {
    using namespace jdoc::pdf_detail;

    const uint8_t placeholder = 0;
    PdfDoc doc(&placeholder, 1);
    auto image = PdfObj::make_dict();
    image.type = ObjType::STREAM;
    image.dict.push_back({"Subtype", PdfObj::make_name("Image")});
    image.dict.push_back({"Width", PdfObj::make_int(1)});
    image.dict.push_back({"Height", PdfObj::make_int(1)});
    image.dict.push_back({"BitsPerComponent", PdfObj::make_int(8)});
    image.dict.push_back({"ColorSpace", PdfObj::make_name("DeviceRGB")});
    image.stream_data = {220, 30, 20};
    auto xobjects = PdfObj::make_dict();
    xobjects.dict.push_back({"Im0", std::move(image)});
    auto resources = PdfObj::make_dict();
    resources.dict.push_back({"XObject", std::move(xobjects)});

    ContentParseResult source;
    RenderPath path{};
    path.points = {{30, 30, PathPoint::MOVE},
                   {70, 30, PathPoint::LINE},
                   {70, 70, PathPoint::LINE},
                   {30, 70, PathPoint::LINE},
                   {0, 0, PathPoint::CLOSE}};
    path.fill_r = 0.1; path.fill_g = 0.2; path.fill_b = 0.8;
    path.stroke_r = path.stroke_g = path.stroke_b = 0;
    path.line_width = 0;
    path.do_fill = true;
    path.do_stroke = false;
    path.clip[0] = 25; path.clip[1] = 25;
    path.clip[2] = 75; path.clip[3] = 75;
    path.seq = 0;
    source.paths.push_back(path);

    ImagePlacement placement{};
    placement.xobj_name = "Im0";
    const double ctm[6] = {15, 0, 0, 15, 50, 50};
    std::memcpy(placement.ctm, ctm, sizeof(ctm));
    placement.clip[0] = 20; placement.clip[1] = 20;
    placement.clip[2] = 80; placement.clip[3] = 80;
    placement.alpha = 1;
    placement.seq = 1;
    source.images.push_back(placement);

    const double region[4] = {20, 20, 80, 80};
    const std::vector<size_t> members = {0};
    auto actual = render_region_composite(doc, resources, source, members, 0,
                                          region, "", 0, nullptr);

    // Reproduce the former copy-and-translate implementation as the oracle.
    ContentParseResult translated = source;
    for (auto& pt : translated.paths[0].points) {
        pt.x -= region[0]; pt.y -= region[1];
        pt.cx1 -= region[0]; pt.cy1 -= region[1];
        pt.cx2 -= region[0]; pt.cy2 -= region[1];
    }
    translated.paths[0].clip[0] -= static_cast<float>(region[0]);
    translated.paths[0].clip[1] -= static_cast<float>(region[1]);
    translated.paths[0].clip[2] -= static_cast<float>(region[0]);
    translated.paths[0].clip[3] -= static_cast<float>(region[1]);
    translated.images[0].ctm[4] -= region[0];
    translated.images[0].ctm[5] -= region[1];
    translated.images[0].clip[0] -= static_cast<float>(region[0]);
    translated.images[0].clip[1] -= static_cast<float>(region[1]);
    translated.images[0].clip[2] -= static_cast<float>(region[0]);
    translated.images[0].clip[3] -= static_cast<float>(region[1]);
    auto expected = render_page_composite(doc, resources, translated, 0, 60,
                                          60, "", 0, nullptr);

    CHECK(!actual.data.empty());
    CHECK(actual.width == expected.width);
    CHECK(actual.height == expected.height);
    CHECK(actual.data == expected.data);
}

// Composite PNGs carry filter byte 0 on every row (Canvas keeps rows in PNG
// layout and prefiltered_to_png preserves that), so tests can inflate the
// IDAT with zlib and probe raw samples directly.
struct DecodedPng {
    unsigned w = 0, h = 0;
    int comps = 0;
    std::vector<uint8_t> rows; // per row: 1 filter byte + samples
};

DecodedPng decode_test_png(const std::vector<char>& png) {
    DecodedPng out;
    CHECK(png.size() > 33);
    const uint8_t* d = reinterpret_cast<const uint8_t*>(png.data());
    auto be32 = [&](size_t off) {
        return (static_cast<uint32_t>(d[off]) << 24) |
               (static_cast<uint32_t>(d[off + 1]) << 16) |
               (static_cast<uint32_t>(d[off + 2]) << 8) |
               static_cast<uint32_t>(d[off + 3]);
    };
    out.w = be32(16);
    out.h = be32(20);
    int ctype = d[25];
    out.comps = ctype == 2 ? 3 : ctype == 6 ? 4 : 1;
    std::vector<uint8_t> idat;
    size_t pos = 8;
    while (pos + 12 <= png.size()) {
        uint32_t len = be32(pos);
        if (len > png.size() - pos - 12) break;
        if (std::memcmp(d + pos + 4, "IDAT", 4) == 0)
            idat.insert(idat.end(), d + pos + 8, d + pos + 8 + len);
        pos += 12 + len;
    }
    uLongf dst_len = static_cast<uLongf>(out.h) *
                     (1 + static_cast<size_t>(out.w) * out.comps);
    out.rows.resize(dst_len);
    CHECK(uncompress(out.rows.data(), &dst_len, idat.data(),
                     static_cast<uLong>(idat.size())) == Z_OK);
    return out;
}

uint8_t png_sample0(const DecodedPng& p, unsigned x, unsigned y) {
    size_t stride = 1 + static_cast<size_t>(p.w) * p.comps;
    CHECK(p.rows[y * stride] == 0); // filter byte
    return p.rows[y * stride + 1 + static_cast<size_t>(x) * p.comps];
}

// A valid Flate payload may itself contain whitespace+EI. The inline scanner
// must use the zlib end marker, not partial decompression of each EI candidate.
void test_pdf_inline_flate_ignores_embedded_ei() {
    using namespace jdoc::pdf_detail;

    std::vector<uint8_t> samples(256, 37);
    samples[100] = '\n';
    samples[101] = 'E';
    samples[102] = 'I';
    samples[103] = ' ';
    uLongf compressed_len = compressBound(samples.size());
    std::vector<uint8_t> compressed(compressed_len);
    CHECK(compress2(compressed.data(), &compressed_len, samples.data(),
                    samples.size(), Z_NO_COMPRESSION) == Z_OK);
    compressed.resize(compressed_len);
    CHECK(std::search(compressed.begin(), compressed.end(),
                      samples.begin() + 100, samples.begin() + 104) !=
          compressed.end());

    const std::string prefix =
        "BI /W 256 /H 1 /BPC 8 /CS /G /F /Fl ID\n";
    const std::string suffix = "\nEI\n10 10 m 20 20 l S\n";
    std::vector<uint8_t> content(prefix.begin(), prefix.end());
    content.insert(content.end(), compressed.begin(), compressed.end());
    content.insert(content.end(), suffix.begin(), suffix.end());

    const uint8_t placeholder = 0;
    PdfDoc doc(&placeholder, 1);
    ContentParseOptions options;
    options.graphics = GraphicsCollection::RenderPaths;
    auto parsed = parse_content_stream(doc, content, PdfObj::make_dict(), 100,
                                       nullptr, options);
    CHECK(parsed.inline_images == 1);
    CHECK(parsed.inline_scan_bailouts == 0);
    CHECK(parsed.images.size() == 1);
    CHECK(parsed.paths.size() == 1); // content after the real EI survived
    CHECK(parsed.images[0].inline_img != nullptr);
    auto decoded = doc.decode_stream(*parsed.images[0].inline_img);
    CHECK(decoded == samples);
}

static jdoc::pdf_detail::PdfObj number_array(
    std::initializer_list<double> values) {
    using jdoc::pdf_detail::PdfObj;
    auto out = PdfObj::make_arr();
    for (double value : values) out.arr.push_back(PdfObj::make_real(value));
    return out;
}

static jdoc::pdf_detail::PdfObj gradient_function() {
    using jdoc::pdf_detail::PdfObj;
    auto fn = PdfObj::make_dict();
    fn.dict.push_back({"FunctionType", PdfObj::make_int(2)});
    fn.dict.push_back({"Domain", number_array({0, 1})});
    fn.dict.push_back({"C0", number_array({1, 0, 0})});
    fn.dict.push_back({"C1", number_array({0, 0, 1})});
    fn.dict.push_back({"N", PdfObj::make_int(1)});
    return fn;
}

// Pattern shading is only lowered to bbox-clipped strips for a real
// axis-aligned rectangle. A triangle must retain its own path boundary.
void test_pdf_pattern_shading_does_not_fill_triangle_bbox() {
    using namespace jdoc::pdf_detail;

    auto shading = PdfObj::make_dict();
    shading.dict.push_back({"ShadingType", PdfObj::make_int(2)});
    shading.dict.push_back({"ColorSpace", PdfObj::make_name("DeviceRGB")});
    shading.dict.push_back({"Coords", number_array({20, 20, 80, 20})});
    shading.dict.push_back({"Function", gradient_function()});
    auto extend = PdfObj::make_arr();
    extend.arr.push_back(PdfObj::make_bool(true));
    extend.arr.push_back(PdfObj::make_bool(true));
    shading.dict.push_back({"Extend", std::move(extend)});

    auto pattern = PdfObj::make_dict();
    pattern.dict.push_back({"PatternType", PdfObj::make_int(2)});
    pattern.dict.push_back({"Shading", std::move(shading)});
    auto patterns = PdfObj::make_dict();
    patterns.dict.push_back({"P1", std::move(pattern)});
    auto resources = PdfObj::make_dict();
    resources.dict.push_back({"Pattern", std::move(patterns)});

    const std::string stream =
        "/Pattern cs /P1 scn 20 20 m 80 20 l 50 80 l h f";
    const uint8_t placeholder = 0;
    PdfDoc doc(&placeholder, 1);
    ContentParseOptions options;
    options.graphics = GraphicsCollection::RenderPaths;
    options.page_width = 100;
    auto parsed = parse_content_stream(
        doc, std::vector<uint8_t>(stream.begin(), stream.end()), resources,
        100, nullptr, options);
    CHECK(parsed.shading_paths == 0);
    CHECK(parsed.paths.size() == 1);

    auto rendered = render_page_composite(doc, resources, parsed, 0, 100, 100,
                                          "");
    CHECK(!rendered.data.empty());
    auto png = decode_test_png(rendered.data);
    auto sample = [&](double x, double y) {
        unsigned px = std::min(png.w - 1,
                               static_cast<unsigned>(x * png.w / 100));
        unsigned py = std::min(png.h - 1, static_cast<unsigned>(
            (100 - y) * png.h / 100));
        return png_sample0(png, px, py);
    };
    CHECK(sample(22, 75) == 255); // outside triangle, inside its bbox
    CHECK(sample(50, 40) < 32);   // inside triangle: flat-color fallback
}

// With neither radial endpoint extended, a nonzero start circle leaves its
// interior untouched. The gradient occupies only the annulus r0..r1.
void test_pdf_radial_shading_preserves_unextended_center() {
    using namespace jdoc::pdf_detail;

    auto shading = PdfObj::make_dict();
    shading.dict.push_back({"ShadingType", PdfObj::make_int(3)});
    shading.dict.push_back({"ColorSpace", PdfObj::make_name("DeviceRGB")});
    shading.dict.push_back({"Coords", number_array({50, 50, 20, 50, 50, 40})});
    shading.dict.push_back({"Function", gradient_function()});
    auto extend = PdfObj::make_arr();
    extend.arr.push_back(PdfObj::make_bool(false));
    extend.arr.push_back(PdfObj::make_bool(false));
    shading.dict.push_back({"Extend", std::move(extend)});
    auto shadings = PdfObj::make_dict();
    shadings.dict.push_back({"Sh1", std::move(shading)});
    auto resources = PdfObj::make_dict();
    resources.dict.push_back({"Shading", std::move(shadings)});

    const std::string stream = "/Sh1 sh";
    const uint8_t placeholder = 0;
    PdfDoc doc(&placeholder, 1);
    ContentParseOptions options;
    options.graphics = GraphicsCollection::RenderPaths;
    options.page_width = 100;
    auto parsed = parse_content_stream(
        doc, std::vector<uint8_t>(stream.begin(), stream.end()), resources,
        100, nullptr, options);
    auto rendered = render_page_composite(doc, resources, parsed, 0, 100, 100,
                                          "");
    CHECK(!rendered.data.empty());
    auto png = decode_test_png(rendered.data);
    auto sample = [&](double x, double y) {
        unsigned px = std::min(png.w - 1,
                               static_cast<unsigned>(x * png.w / 100));
        unsigned py = std::min(png.h - 1, static_cast<unsigned>(
            (100 - y) * png.h / 100));
        return png_sample0(png, px, py);
    };
    CHECK(sample(50, 50) == 255); // r < 20 remains white
    CHECK(sample(80, 50) < 240);  // r = 30 lies in the gradient

    // Extending the outer endpoint must color only outside r1; it must not
    // leak through the unextended start-circle hole.
    auto& shading_obj = resources.dict[0].second.dict[0].second;
    for (auto& entry : shading_obj.dict)
        if (entry.first == "Extend") entry.second.arr[1].bool_val = true;
    auto extended = parse_content_stream(
        doc, std::vector<uint8_t>(stream.begin(), stream.end()), resources,
        100, nullptr, options);
    auto extended_render = render_page_composite(
        doc, resources, extended, 0, 100, 100, "");
    CHECK(!extended_render.data.empty());
    auto extended_png = decode_test_png(extended_render.data);
    auto extended_sample = [&](double x, double y) {
        unsigned px = std::min(extended_png.w - 1,
            static_cast<unsigned>(x * extended_png.w / 100));
        unsigned py = std::min(extended_png.h - 1, static_cast<unsigned>(
            (100 - y) * extended_png.h / 100));
        return png_sample0(extended_png, px, py);
    };
    CHECK(extended_sample(50, 50) == 255);
    CHECK(extended_sample(95, 50) < 32);
}

// Successfully decoded ImageMasks may still have zero coverage. They must not
// turn a fragment-classified page into a blank white composite image.
void test_pdf_composite_skips_fully_transparent_masks() {
    using namespace jdoc::pdf_detail;

    const uint8_t placeholder = 0;
    PdfDoc doc(&placeholder, 1);
    auto mask = PdfObj::make_dict();
    mask.type = ObjType::STREAM;
    mask.dict.push_back({"Subtype", PdfObj::make_name("Image")});
    mask.dict.push_back({"Width", PdfObj::make_int(8)});
    mask.dict.push_back({"Height", PdfObj::make_int(1)});
    mask.dict.push_back({"BitsPerComponent", PdfObj::make_int(1)});
    mask.dict.push_back({"ImageMask", PdfObj::make_bool(true)});
    mask.stream_data = {0xFF}; // default Decode [0 1]: no painted samples
    auto xobjects = PdfObj::make_dict();
    xobjects.dict.push_back({"M1", mask});
    xobjects.dict.push_back({"M2", mask});
    auto resources = PdfObj::make_dict();
    resources.dict.push_back({"XObject", std::move(xobjects)});

    ContentParseResult parsed;
    auto placement = [](const char* name, double x, double y) {
        ImagePlacement ip{};
        ip.xobj_name = name;
        const double ctm[6] = {40, 0, 0, 2, x, y};
        std::memcpy(ip.ctm, ctm, sizeof(ctm));
        ip.alpha = 1;
        return ip;
    };
    parsed.images.push_back(placement("M1", 5, 20));
    parsed.images.push_back(placement("M2", 55, 70));
    auto rendered = render_page_composite(doc, resources, parsed, 0, 100, 100,
                                          "");
    CHECK(rendered.data.empty());
}

// A W n clip rect must confine an image to its window: the uncovered part of
// the canvas stays white. Before the rect clip tier, the whole 100×100-pt
// placement painted regardless of the 40×40-pt window.
void test_pdf_composite_applies_clip_rect() {
    using namespace jdoc::pdf_detail;

    const uint8_t placeholder = 0;
    PdfDoc doc(&placeholder, 1);

    auto image = PdfObj::make_dict();
    image.type = ObjType::STREAM;
    image.dict.push_back({"Subtype", PdfObj::make_name("Image")});
    image.dict.push_back({"Width", PdfObj::make_int(2)});
    image.dict.push_back({"Height", PdfObj::make_int(2)});
    image.dict.push_back({"BitsPerComponent", PdfObj::make_int(8)});
    image.dict.push_back({"ColorSpace", PdfObj::make_name("DeviceGray")});
    image.stream_data = {100, 100, 100, 100};

    auto xobjects = PdfObj::make_dict();
    xobjects.dict.push_back({"Im0", image});
    auto resources = PdfObj::make_dict();
    resources.dict.push_back({"XObject", xobjects});

    ContentParseResult parsed;
    ImagePlacement ip{};
    ip.xobj_name = "Im0";
    const double ctm[6] = {100, 0, 0, 100, 0, 0}; // covers the whole page
    std::memcpy(ip.ctm, ctm, sizeof(ctm));
    ip.alpha = 1;
    ip.clip[0] = 30; ip.clip[1] = 30; ip.clip[2] = 70; ip.clip[3] = 70;
    ip.seq = 1;
    parsed.images.push_back(ip);

    auto rendered = render_page_composite(doc, resources, parsed, 0, 100, 100, "");
    CHECK(!rendered.data.empty());
    auto png = decode_test_png(
        std::vector<char>(rendered.data.begin(), rendered.data.end()));
    CHECK(png.w > 0 && png.h > 0);
    double sx = png.w / 100.0, sy = png.h / 100.0;
    auto px = [&](double page_x, double page_y) {
        unsigned cx = static_cast<unsigned>(page_x * sx);
        unsigned cy = static_cast<unsigned>((100.0 - page_y) * sy);
        return png_sample0(png, cx, cy);
    };
    CHECK(px(50, 50) == 100);  // inside the window
    CHECK(px(10, 50) == 255);  // left of it: white
    CHECK(px(50, 90) == 255);  // above it: white
    CHECK(px(90, 10) == 255);  // opposite corner: white
}

// Axial (type 2) shadings decompose into gradient strips at parse time; the
// composite must run red→blue across the page instead of rendering nothing
// (the `sh` operator used to be ignored entirely).
void test_pdf_composite_renders_axial_shading() {
    const char* fixture = "test/fixtures/pdf/axial_shading.pdf";
    std::ifstream probe(fixture);
    if (!probe.good()) {
        std::cout << "  (skip: fixture not found)\n";
        return;
    }
    probe.close();
    auto chunks = jdoc::pdf_to_markdown_chunks(fixture);
    CHECK(chunks.size() == 1);
    CHECK(chunks[0].images.size() == 1);
    CHECK(chunks[0].images[0].format == "png");
    auto png = decode_test_png(chunks[0].images[0].data);
    CHECK(png.comps == 3);
    auto rgb = [&](unsigned x, unsigned y) {
        size_t stride = 1 + static_cast<size_t>(png.w) * 3;
        const uint8_t* p = png.rows.data() + y * stride + 1 +
                           static_cast<size_t>(x) * 3;
        return std::array<uint8_t, 3>{p[0], p[1], p[2]};
    };
    auto left = rgb(png.w / 50, png.h / 2);
    auto mid = rgb(png.w / 2, png.h / 2);
    auto right = rgb(png.w - 1 - png.w / 50, png.h / 2);
    CHECK(left[0] > 220 && left[2] < 40);   // red end
    CHECK(right[2] > 220 && right[0] < 40); // blue end
    CHECK(mid[0] > 90 && mid[0] < 165 && mid[2] > 90 && mid[2] < 165);
}

void test_pdf_reads_rotated_text() {
    const std::string pdf = rotated_text_pdf();
    const std::string text = jdoc::pdf_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size());
    // Every quarter turn survives; the vertical ones used to be dropped.
    CHECK(text.find("UPRIGHT") != std::string::npos);
    CHECK(text.find("QUARTER") != std::string::npos);
    CHECK(text.find("HALFTURN") != std::string::npos);
    CHECK(text.find("THREEQTR") != std::string::npos);
    // 180° text advances along -x. Sorting its row left to right without
    // accounting for that spelled it backwards, one character per line.
    CHECK(text.find("NRUTFLAH") == std::string::npos);
}

// Line width is set in user space, so the CTM has to scale the pen the same
// way it scales the geometry. A CAD sheet plotted at 1:2.8 sets `24 w` under a
// 0.03 matrix — 0.72pt on the page. Recording the 24 raw drew it as a bar wide
// enough to swallow the drawing when the page was composited to a raster.
// Ruled tables assemble a cell through PageCharCache::get_text_in_rect, which
// is a separate implementation from the borderless builder that the end-to-end
// fixture happens to exercise. Drive it directly so a regression confined to
// this one cannot hide.
void test_pdf_cell_assembly_reading_order() {
    using namespace jdoc::pdf_detail;

    // "ECHO" drawn 180°-rotated: glyphs advance toward -x, one em apart, all
    // on the same baseline — the shape a Tm of [-1 0 0 -1] produces.
    auto glyph = [](uint32_t cp, double right, int16_t rot) {
        TextChar t{};
        t.x = right; t.y = 150;
        t.left = right - 6; t.right = right;
        t.top = 152; t.bot = 142;
        t.font_size = 10;
        t.unicode = cp;
        t.rot = rot;
        return t;
    };
    std::vector<TextChar> chars = {
        glyph('E', 250, 12), glyph('C', 244, 12),
        glyph('H', 238, 12), glyph('O', 232, 12),
    };
    PageCharCache cache;
    cache.build(chars);
    CHECK(cache.get_text_in_rect(200, 170, 300, 130) == "ECHO");

    // Upright text is the identity case and must be untouched.
    std::vector<TextChar> upright = {
        glyph('A', 106, 0), glyph('B', 112, 0), glyph('C', 118, 0),
    };
    for (auto& t : upright) { t.left = t.x; t.right = t.x + 6; }
    PageCharCache up;
    up.build(upright);
    CHECK(up.get_text_in_rect(100, 170, 200, 130) == "ABC");
}

void test_pdf_line_width_follows_ctm() {
    using namespace jdoc::pdf_detail;
    const std::string ops =
        "q 0.03 0 0 0.03 0 0 cm 24 w 0 0 m 1000 0 l S Q\n"   // scaled pen
        "q 2 0 0 2 0 0 cm 3 w 0 0 m 100 0 l S Q\n"           // magnified pen
        "1.5 w 0 0 m 100 0 l S";                             // identity CTM
    std::vector<uint8_t> stream(ops.begin(), ops.end());

    uint8_t placeholder = 0;
    PdfDoc doc(&placeholder, 1);
    PdfObj resources;
    ContentParseOptions options;
    options.graphics = GraphicsCollection::RenderPaths;
    auto parsed = parse_content_stream(doc, stream, resources, 800.0, nullptr,
                                       options, nullptr);

    CHECK(parsed.paths.size() == 3);
    CHECK(std::abs(parsed.paths[0].line_width - 24.0 * 0.03) < 1e-9);
    CHECK(std::abs(parsed.paths[1].line_width - 3.0 * 2.0) < 1e-9);
    // No CTM in force: the width travels through unchanged.
    CHECK(std::abs(parsed.paths[2].line_width - 1.5) < 1e-9);
}

void test_pdf_table_cell_rotated_text() {
    const std::string pdf = rotated_table_pdf();
    const std::string text = jdoc::pdf_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size());
    // Cell assembly ordered glyphs left to right in page space, which reversed
    // the half-turn run the same way line layout used to.
    CHECK(text.find("ECHO") != std::string::npos);
    CHECK(text.find("OHCE") == std::string::npos);
    // Sharing a cell with the upright run: the two frames have no common axis,
    // so the word gap between them cannot be measured and the separator is
    // unconditional. Without it they run together as "INDIAECHO".
    CHECK(text.find("INDIA ECHO") != std::string::npos);
    CHECK(text.find("INDIAECHO") == std::string::npos);
    // The upright cells are untouched by grouping runs per direction.
    CHECK(text.find("CHARLIE") != std::string::npos);
    CHECK(text.find("JULIET") != std::string::npos);
}

void test_pdf_lists_attachments() {
    const std::string pdf = attachment_pdf();
    const std::string text = jdoc::pdf_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size());
    CHECK(jdoc::util::is_valid_utf8(text));
    // UTF-16BE name, /Params /Size in preference to the stream length, and the
    // description the producer attached.
    const std::string listed = "- \xEB\x8F\x84\xEB\xA9\xB4.dwg (2.0 MB)";
    const size_t first = text.find(listed);
    CHECK(first != std::string::npos);
    CHECK(text.find("site plan") != std::string::npos);
    // Registered under two tree keys, listed once.
    CHECK(text.find(listed, first + 1) == std::string::npos);
    // The second file declares no /Params, so its size falls back to /Length.
    CHECK(text.find("- notes.txt (27 B)") != std::string::npos);
    // A paperclip annotation names the file it stands for, which is a separate
    // mention from the document-level list.
    CHECK(text.find("drawing attached [\xEB\x8F\x84\xEB\xA9\xB4.dwg]") !=
          std::string::npos);

    // Chunk consumers must see the same document-level attachment listing.
    const auto chunks = jdoc::pdf_to_markdown_chunks_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size());
    CHECK(!chunks.empty());
    CHECK(chunks[0].text.find(listed) != std::string::npos);
}

// Object identity, rather than the display name, determines whether two name-
// tree entries refer to the same attachment. Also exercise a supplementary
// Unicode character encoded as a UTF-16 surrogate pair in /UF.
void test_pdf_preserves_same_named_attachments() {
    const std::string pdf = assemble_pdf({
        "<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles 5 0 R >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
        "/Resources << >> /Contents 4 0 R >>",
        stream_object("", "BT ET"),
        "<< /Names [ (a) 6 0 R (b) 7 0 R ] >>",
        "<< /Type /Filespec /UF <FEFFD835DC00002E00620069006E> "
        "/Desc (first payload) >>",
        "<< /Type /Filespec /UF <FEFFD835DC00002E00620069006E> "
        "/Desc (second payload) >>",
    });
    const std::string text = jdoc::pdf_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size());
    const std::string name = "\xF0\x9D\x90\x80.bin";  // U+1D400
    const size_t first = text.find(name);
    CHECK(first != std::string::npos);
    CHECK(text.find(name, first + name.size()) != std::string::npos);
    CHECK(text.find("first payload") != std::string::npos);
    CHECK(text.find("second payload") != std::string::npos);
    CHECK(jdoc::util::is_valid_utf8(text));
}

// /UF, /F and /Desc are producer-supplied. A filespec carrying newlines would
// otherwise forge headings and list items in the extracted text — the same
// threat the OOXML embedded-part listing already defends against.
void test_pdf_attachment_name_cannot_forge_structure() {
    const std::string pdf = assemble_pdf({
        "<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles 5 0 R >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
        "/Resources << >> /Contents 4 0 R >>",
        stream_object("", "BT ET"),
        "<< /Names [ (a) 6 0 R ] >>",
        "<< /Type /Filespec "
        "/F (benign.txt\\n\\n## Table of Contents\\n\\n- INJECTED\\n\\nmore) "
        "/Desc (note\\n\\n## Forged) /EF << /F 7 0 R >> >>",
        stream_object("/Type /EmbeddedFile /Params << /Size 13 >>", "payload bytes"),
    });
    const std::string text = jdoc::pdf_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size());

    // Reported in full — only prevented from starting a line.
    CHECK(text.find("INJECTED") != std::string::npos);
    CHECK(text.find("\n## Table of Contents") == std::string::npos);
    CHECK(text.find("\n## Forged") == std::string::npos);
    // One list item, one heading.
    size_t items = 0;
    for (size_t p = text.find("\n- "); p != std::string::npos;
         p = text.find("\n- ", p + 3))
        items++;
    CHECK(items == 1);
}

// Rotated runs reach the line list in page space, so a vertical caption whose
// midpoint lands on a body line's baseline used to be merged into it.
void test_pdf_rotated_run_not_merged_into_body_line() {
    const std::string content =
        "BT /F1 12 Tf\n"
        "1 0 0 1 200 300 Tm (MIDDLE LINE OF BODY) Tj\n"
        "0 1 -1 0 160 272 Tm (SIDEBAR) Tj\n"
        "ET";
    const std::string pdf = assemble_pdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        stream_object("", content),
    });
    const std::string text = jdoc::pdf_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size());

    CHECK(text.find("SIDEBAR") != std::string::npos);
    CHECK(text.find("MIDDLE LINE OF BODY") != std::string::npos);
    // Never welded together, with or without a separator.
    CHECK(text.find("SIDEBAR MIDDLE") == std::string::npos);
    CHECK(text.find("SIDEBARMIDDLE") == std::string::npos);
}

void test_pdf_name_tree_cycle_terminates() {
    const std::string pdf = cyclic_name_tree_pdf();
    const std::string text = jdoc::pdf_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size());
    CHECK(text.find("## Attachments") == std::string::npos);
}

void test_pdf_decodes_surrogate_pair() {
    const std::string pdf = unicode_pdf();
    const std::string text = jdoc::pdf_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(pdf.data()), pdf.size());
    CHECK(jdoc::util::is_valid_utf8(text));
    CHECK(text.find("\xF0\x9D\x90\x80") != std::string::npos);

    std::string invalid_scalar;
    jdoc::util::append_utf8(invalid_scalar, 0xD835);
    CHECK(invalid_scalar == "\xEF\xBF\xBD");
}

void test_memory_streaming_supports_eml() {
    const std::string eml =
        "From: sender@example.com\r\n"
        "To: receiver@example.com\r\n"
        "Subject: Memory stream\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n\r\n"
        "streamed body\r\n";
    std::vector<jdoc::PageChunk> chunks;
    jdoc::for_each_chunk(eml.data(), eml.size(), "message.eml", {},
                         [&](jdoc::PageChunk&& chunk) {
                             chunks.push_back(std::move(chunk));
                             return true;
                         });
    CHECK(chunks.size() == 1);
    CHECK(chunks[0].text.find("streamed body") != std::string::npos);
}

void test_empty_memory_and_invalid_pages_are_consistent() {
    CHECK(jdoc::detect(nullptr, 0, "empty.txt").format == "TXT");
    CHECK(jdoc::convert(nullptr, 0, "empty.txt") == "");
    const auto pages = jdoc::convert_chunks(nullptr, 0, "empty.txt");
    CHECK(pages.size() == 1);
    CHECK(pages[0].text.empty());

    jdoc::ConvertOptions opts;
    opts.pages = {-1};
    bool rejected = false;
    try {
        (void)jdoc::convert(nullptr, 0, "empty.txt", opts);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

void test_concurrent_image_saves_do_not_overwrite() {
    std::string base = "/tmp";
    for (const char* var : {"TMPDIR", "TEMP", "TMP"}) {
        const char* value = std::getenv(var);
        if (value && *value) { base = value; break; }
    }
    const auto nonce = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    const std::string dir = base + "/jdoc-save-collision-" +
                            std::to_string(nonce);

    constexpr size_t kCount = 8;
    std::vector<std::string> paths(kCount);
    std::vector<std::string> payloads(kCount);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < kCount; ++i) {
        payloads[i] = "payload-" + std::to_string(i);
        threads.emplace_back([&, i] {
            paths[i] = jdoc::util::save_named_file(
                dir, "page1_img0.png", payloads[i].data(), payloads[i].size());
        });
    }
    for (auto& thread : threads) thread.join();

    std::set<std::string> unique_paths(paths.begin(), paths.end());
    CHECK(unique_paths.size() == kCount);
    std::set<std::string> actual_payloads;
    for (const auto& path : paths) {
        CHECK(!path.empty());
        std::ifstream in(jdoc::util::io_path(path), std::ios::binary);
        CHECK(in.good());
        actual_payloads.emplace(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
        std::filesystem::remove(jdoc::util::io_path(path));
    }
    CHECK(actual_payloads == std::set<std::string>(payloads.begin(), payloads.end()));
    std::filesystem::remove(jdoc::util::io_path(dir));
}

void test_utf8_file_and_output_paths() {
    std::string base = "/tmp";
    for (const char* var : {"TMPDIR", "TEMP", "TMP"}) {
        const char* value = std::getenv(var);
        if (value && *value) { base = value; break; }
    }
    const auto nonce = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    const std::string dir = base + "/jdoc-ê²½ë¡-" +
                            std::to_string(nonce);
    const std::string input = dir + "/ë¬¸ì.pdf";
    const std::string image_dir = dir + "/ì´ë¯¸ì§";

    jdoc::util::ensure_dirs(dir);
    {
        std::ofstream out(jdoc::util::io_path(input), std::ios::binary);
        CHECK(out.good());
        const std::string pdf = image_pdf();
        out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
    }

    CHECK(jdoc::detect(input).format == "PDF");
    jdoc::ConvertOptions opts;
    opts.image_dir = image_dir;
    opts.min_image_size = 0;
    const auto pages = jdoc::convert_chunks(input, opts);
    CHECK(pages.size() == 1);
    CHECK(pages[0].images.size() == 1);
    CHECK(!pages[0].images[0].saved_path.empty());
    CHECK(std::filesystem::exists(
        jdoc::util::io_path(pages[0].images[0].saved_path)));

    std::filesystem::remove_all(jdoc::util::io_path(dir));
}

} // namespace

int main() {
    test_ole_rejects_invalid_sector_shift();
    test_ole_rejects_oversized_directory_name();
    test_ole_directory_cycle_terminates();
    test_ole_rejects_stream_larger_than_source();
    test_png_rejects_short_pixels();
    test_png_converts_cmyk();
    test_pdf_honors_images_option();
    test_pdf_composite_clips_extreme_coordinates_before_narrowing();
    test_pdf_region_composite_matches_translated_geometry();
    test_pdf_inline_flate_ignores_embedded_ei();
    test_pdf_pattern_shading_does_not_fill_triangle_bbox();
    test_pdf_radial_shading_preserves_unextended_center();
    test_pdf_composite_skips_fully_transparent_masks();
    test_pdf_composite_applies_clip_rect();
    test_pdf_composite_renders_axial_shading();
    test_pdf_reads_rotated_text();
    test_pdf_table_cell_rotated_text();
    test_pdf_line_width_follows_ctm();
    test_pdf_cell_assembly_reading_order();
    test_pdf_lists_attachments();
    test_pdf_preserves_same_named_attachments();
    test_pdf_attachment_name_cannot_forge_structure();
    test_pdf_rotated_run_not_merged_into_body_line();
    test_pdf_name_tree_cycle_terminates();
    test_pdf_decodes_surrogate_pair();
    test_memory_streaming_supports_eml();
    test_empty_memory_and_invalid_pages_are_consistent();
    test_concurrent_image_saves_do_not_overwrite();
    test_utf8_file_and_output_paths();
    std::cout << "Safety regression tests passed\n";
}
