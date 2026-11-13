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
bool ole10_native_payload(std::vector<char>& s, std::string& name) {
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
    size_t len = le32(d + p);
    p += 4;
    if (len > n - p) return false;
    s = std::vector<char>(s.begin() + p, s.begin() + p + len);
    return true;
}

// The document an OLE object holds as one stream: an OOXML or other file
// (Package), an ODF package (EmbeddedOdf; package_stream as LibreOffice
// writes it), or a packaged file (Ole10Native).
bool ole_payload(const OleReader& ole, std::vector<char>& out, std::string& name) {
    for (const char* s : {"Package", "EmbeddedOdf", "package_stream"}) {
        if (ole.has_stream(s)) {
            out = ole.read_stream(s);
            return !out.empty();
        }
    }
    if (ole.has_stream("\x01Ole10Native")) {
        out = ole.read_stream("\x01Ole10Native");
        if (!ole10_native_payload(out, name)) out.clear();
    }
    return !out.empty();
}

thread_local int g_depth = 0;
thread_local int g_unnamed = 0;

// The folder an embedded document's pictures go to, under the host's
// image_dir: named after the object (BinData/BIN0001.OLE, ObjectPool/_123,
// word/embeddings/oleObject1.bin), which is unique within its host, so the
// page1_img0 names every document starts from cannot collide. An object with
// no name gets a running number.
std::string embedded_image_folder(const std::string& name) {
    std::string out = "embedded_";
    if (name.empty()) return out + "object" + std::to_string(++g_unnamed);
    for (unsigned char c : name) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c >= 0x80;
        out += ok ? static_cast<char>(c) : '_';
    }
    return out;
}

// Run one nested conversion: the whole document, depth-bounded, failures
// swallowed, surrounding blank lines trimmed. Its pictures are extracted
// when the host writes pictures to a folder — a picture pasted inside an
// embedded report is as much a part of what the reader sees as its text —
// into a subfolder of their own (see embedded_image_folder).
template <class Fn>
std::string nested(const ConvertOptions& opts, const std::string& name, Fn&& fn) {
    int max_depth = opts.archive.max_depth;
    if (max_depth >= 0 && g_depth >= max_depth) return "";
    ConvertOptions o = opts;
    if (o.images && !o.image_dir.empty()) {
        const std::string folder = embedded_image_folder(name);
        o.image_dir += "/" + folder;
        o.image_ref_prefix += folder + "/";
    } else {
        o.images = false;
        o.image_dir.clear();
    }
    o.pages.clear();
    o.page_chunks = false;
    o.metadata = false;
    ++g_depth;
    std::string md;
    try {
        md = fn(o);
    } catch (...) {
    }
    --g_depth;
    size_t b = md.find_first_not_of(" \t\r\n"), e = md.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : md.substr(b, e - b + 1);
}

} // namespace

std::string embedded_document_text(const uint8_t* data, size_t size,
                                   const std::string& name_hint,
                                   const ConvertOptions& opts) {
    if (!data) return "";
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
            if (ole_payload(ole, inner, name)) {
                data = reinterpret_cast<const uint8_t*>(inner.data());
                size = inner.size();
            }
        }
        FileFormat fmt = detect_format_mem(data, size, name);
        if (fmt != FileFormat::PDF && fmt != FileFormat::OFFICE &&
            fmt != FileFormat::HWP && fmt != FileFormat::HWPX &&
            fmt != FileFormat::TXT)
            return "";
        return nested(opts, name_hint, [&](const ConvertOptions& o) {
            return convert_from_memory_as(fmt, data, size, name, o);
        });
    } catch (...) {
        return "";
    }
}

std::string embedded_storage_text(const OleReader& host, const std::string& storage,
                                  const ConvertOptions& opts) {
    try {
        auto sub = host.open_storage(storage);
        if (!sub) return "";
        std::vector<char> bytes;
        std::string name = storage;
        if (ole_payload(*sub, bytes, name))
            return embedded_document_text(reinterpret_cast<const uint8_t*>(bytes.data()),
                                          bytes.size(), name, opts);
        return nested(opts, storage, [&](const ConvertOptions& o) {
            return office_storage_to_markdown(host, storage, o);
        });
    } catch (...) {
        return "";
    }
}

std::string ole_storage_attachments(const OleReader& host, const ConvertOptions& opts) {
    std::vector<std::string> paths;
    for (auto& s : host.storages("ObjectPool")) paths.push_back("ObjectPool/" + s);
    for (auto& s : host.storages(""))
        if (s.compare(0, 3, "MBD") == 0) paths.push_back(s);
    std::string out;
    int n = 0;
    for (auto& p : paths) {
        std::string text = embedded_storage_text(host, p, opts);
        if (!text.empty())
            out += "- Embedded object " + std::to_string(++n) + "\n\n" + text + "\n\n";
    }
    return out.empty() ? out : "\n\n## Attachments\n\n" + out;
}

} // namespace jdoc
