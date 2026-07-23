#include "legacy/ole_reader.h"
#include "common/png_encode.h"
#include "common/string_utils.h"
#include "jdoc/jdoc.h"
#include "jdoc/pdf.h"

#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
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
    CHECK(raw[1] == 255 && raw[2] == 0 && raw[3] == 0);
}

void test_pdf_honors_images_option() {
    const std::string pdf = image_pdf();
    jdoc::ConvertOptions opts;
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

} // namespace

int main() {
    test_ole_rejects_invalid_sector_shift();
    test_ole_rejects_oversized_directory_name();
    test_ole_directory_cycle_terminates();
    test_ole_rejects_stream_larger_than_source();
    test_png_rejects_short_pixels();
    test_png_converts_cmyk();
    test_pdf_honors_images_option();
    test_pdf_decodes_surrogate_pair();
    test_memory_streaming_supports_eml();
    std::cout << "Safety regression tests passed\n";
}
