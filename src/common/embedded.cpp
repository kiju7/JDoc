// License: MIT
#include "common/embedded.h"
#include "convert_internal.h"
#include "legacy/ole_reader.h"

#include <cstring>
#include <vector>

namespace jdoc {

namespace {

const uint8_t kCfbMagic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

bool is_cfb(const uint8_t* d, size_t n) {
    return n >= 512 && std::memcmp(d, kCfbMagic, 8) == 0;
}

uint32_t le32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

// The file an OLE "Package" object wraps (\x01Ole10Native): a size, a type,
// the label and source path as C strings, a reserved word, the temp path
// length-prefixed, then the file itself length-prefixed.
bool ole10_native_payload(const std::vector<char>& s, std::string& name,
                          size_t& off, size_t& len) {
    const uint8_t* d = reinterpret_cast<const uint8_t*>(s.data());
    size_t n = s.size(), p = 6;
    auto cstr = [&](std::string* out) {
        size_t e = p;
        while (e < n && d[e]) ++e;
        if (e >= n) return false;
        if (out) out->assign(reinterpret_cast<const char*>(d + p), e - p);
        p = e + 1;
        return true;
    };
    if (n < 6 || !cstr(&name) || !cstr(nullptr) || p + 8 > n) return false;
    p += 4;
    uint32_t tmp = le32(d + p);
    p += 4;
    if (tmp > n - p || n - p - tmp < 4) return false;
    p += tmp;
    len = le32(d + p);
    off = p + 4;
    return len <= n - off;
}

thread_local int g_depth = 0;

} // namespace

std::string embedded_document_text(const uint8_t* data, size_t size,
                                   const std::string& name_hint,
                                   const ConvertOptions& opts) {
    int max_depth = opts.archive.max_depth;
    if (!data || (max_depth >= 0 && g_depth >= max_depth)) return "";
    std::string name = name_hint;
    std::vector<char> inner;   // owns the unwrapped payload
    try {
        // HWP stores an OLE object behind a 4-byte length.
        if (size > 4 && !is_cfb(data, size) && is_cfb(data + 4, size - 4)) {
            data += 4;
            size -= 4;
        }
        if (is_cfb(data, size)) {
            OleReader ole(data, size);
            if (!ole.is_open()) return "";
            // A linked or packaged file: the document is the stream itself.
            if (ole.has_stream("Package")) {
                inner = ole.read_stream("Package");
            } else if (ole.has_stream("EmbeddedOdf")) {
                inner = ole.read_stream("EmbeddedOdf");
            } else if (ole.has_stream("\x01Ole10Native")) {
                auto s = ole.read_stream("\x01Ole10Native");
                size_t off = 0, len = 0;
                if (!ole10_native_payload(s, name, off, len)) return "";
                inner.assign(s.begin() + off, s.begin() + off + len);
            }
            if (!inner.empty()) {
                data = reinterpret_cast<const uint8_t*>(inner.data());
                size = inner.size();
            }
        }
        FileFormat fmt = detect_format_mem(data, size, name);
        if (fmt != FileFormat::PDF && fmt != FileFormat::OFFICE &&
            fmt != FileFormat::HWP && fmt != FileFormat::HWPX &&
            fmt != FileFormat::TXT)
            return "";
        ConvertOptions o = opts;
        o.images = false;
        o.image_dir.clear();
        o.pages.clear();
        o.page_chunks = false;
        ++g_depth;
        std::string md;
        try {
            md = convert_from_memory_as(fmt, data, size, name, o);
        } catch (...) {
        }
        --g_depth;
        size_t b = md.find_first_not_of(" \t\r\n"), e = md.find_last_not_of(" \t\r\n");
        return b == std::string::npos ? std::string() : md.substr(b, e - b + 1);
    } catch (...) {
        return "";
    }
}

} // namespace jdoc
