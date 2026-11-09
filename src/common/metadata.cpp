// metadata.cpp - Document properties from OLE property sets and zip packages,
// and the front-matter block they are printed as.
// License: MIT
#include "common/metadata.h"
#include "common/binary_utils.h"
#include "common/string_utils.h"
#include "legacy/hwp3_parser.h"
#include "legacy/ole_reader.h"
#include "xml_utils.h"
#include "zip_reader.h"

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace jdoc {

namespace {

const char* const kPropKeys[PROP_COUNT] = {
    "title", "subject", "author", "last_modified_by", "keywords",
    "description", "category", "company", "manager", "template",
    "application", "created", "modified", "date",
};

// ── OLE property sets (MS-OLEPS) ────────────────────────

struct IdProp { uint32_t id; DocProp prop; };

// SummaryInformation and HwpSummaryInformation share the property ids.
const IdProp kSummaryIds[] = {
    {2, PROP_TITLE}, {3, PROP_SUBJECT}, {4, PROP_AUTHOR}, {5, PROP_KEYWORDS},
    {6, PROP_DESCRIPTION}, {7, PROP_TEMPLATE}, {8, PROP_LAST_AUTHOR},
    {12, PROP_CREATED}, {13, PROP_MODIFIED}, {18, PROP_APPLICATION},
};
const IdProp kDocSummaryIds[] = {
    {2, PROP_CATEGORY}, {14, PROP_MANAGER}, {15, PROP_COMPANY},
};

constexpr uint16_t VT_LPSTR = 0x1E, VT_LPWSTR = 0x1F, VT_FILETIME = 0x40;
constexpr uint32_t PID_CODEPAGE = 1;

// "YYYY-MM-DDTHH:MM:SSZ" for a FILETIME (100 ns ticks since 1601, UTC).
std::string filetime_iso(uint64_t ft) {
    if (ft == 0) return "";
    int64_t secs = int64_t(ft / 10000000) - 11644473600LL;
    int64_t z = (secs >= 0 ? secs : secs - 86399) / 86400, t = secs - z * 86400;
    z += 719468;                                   // civil date from day count
    int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = yoe + era * 400 + (m <= 2);
    char buf[32];
    snprintf(buf, sizeof buf, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
             (long long)y, (long long)m, (long long)d, (long long)(t / 3600),
             (long long)(t / 60 % 60), (long long)(t % 60));
    return buf;
}

std::string until_nul(std::string s) {
    size_t nul = s.find('\0');
    if (nul != std::string::npos) s.resize(nul);
    return s;
}

std::string codepage_string(const uint8_t* p, size_t n, uint16_t codepage) {
    if (codepage == 1200) return until_nul(util::utf16le_to_utf8(p, n & ~size_t(1)));
    std::string raw = until_nul(std::string(reinterpret_cast<const char*>(p), n));
    const char* charset = codepage == 65001 ? "utf-8"
                        : codepage == 1252 || codepage == 28591 ? "latin1" : "";
    return util::charset_to_utf8(raw, charset);
}

// String and date values of the first section of a property set stream.
template <size_t N>
void read_property_set(const std::vector<char>& stream, const IdProp (&ids)[N],
                       DocProps& props) {
    const uint8_t* d = reinterpret_cast<const uint8_t*>(stream.data());
    if (stream.size() < 48 || util::read_u16_le(d) != 0xFFFE) return;
    size_t off = util::read_u32_le(d + 44);
    if (off > stream.size() - 8) return;
    const uint8_t* set = d + off;
    size_t len = stream.size() - off;
    size_t count = std::min<size_t>(util::read_u32_le(set + 4), (len - 8) / 8);

    auto value_at = [&](size_t i, uint16_t& type, size_t& avail) -> const uint8_t* {
        size_t at = util::read_u32_le(set + 12 + i * 8);
        if (at > len - 8) return nullptr;
        type = util::read_u16_le(set + at);
        avail = len - at - 4;
        return set + at + 4;
    };
    uint16_t codepage = 0, type = 0;
    size_t avail = 0;
    for (size_t i = 0; i < count; i++) {
        const uint8_t* v;
        if (util::read_u32_le(set + 8 + i * 8) == PID_CODEPAGE &&
            (v = value_at(i, type, avail)) && type == 2)
            codepage = util::read_u16_le(v);
    }
    for (size_t i = 0; i < count; i++) {
        uint32_t id = util::read_u32_le(set + 8 + i * 8);
        const IdProp* hit = nullptr;
        for (const auto& e : ids) if (e.id == id) hit = &e;
        const uint8_t* v = hit ? value_at(i, type, avail) : nullptr;
        if (!v || avail < 4) continue;
        size_t n = util::read_u32_le(v);
        std::string s;
        if (type == VT_LPSTR)
            s = codepage_string(v + 4, std::min(n, avail - 4), codepage);
        else if (type == VT_LPWSTR)
            s = until_nul(util::utf16le_to_utf8(v + 4, std::min(n, (avail - 4) / 2) * 2));
        else if (type == VT_FILETIME && avail >= 8)
            s = filetime_iso(util::read_u64_le(v));
        add_property(props, hit->prop, s);
    }
}

void read_ole_properties(const uint8_t* data, size_t size, DocProps& props) {
    OleReader ole(data, size);
    if (!ole.is_open()) return;
    for (const char* name : {"\x05SummaryInformation", "\x05HwpSummaryInformation"})
        if (ole.has_stream(name)) read_property_set(ole.read_stream(name), kSummaryIds, props);
    if (ole.has_stream("\x05DocumentSummaryInformation"))
        read_property_set(ole.read_stream("\x05DocumentSummaryInformation"),
                          kDocSummaryIds, props);
}

// ── Zip packages: OOXML, ODF, HWPX ──────────────────────

struct NameProp { const char* name; DocProp prop; };

const NameProp kCoreNames[] = {   // OOXML docProps/core.xml
    {"title", PROP_TITLE}, {"subject", PROP_SUBJECT}, {"creator", PROP_AUTHOR},
    {"lastModifiedBy", PROP_LAST_AUTHOR}, {"keywords", PROP_KEYWORDS},
    {"description", PROP_DESCRIPTION}, {"category", PROP_CATEGORY},
    {"created", PROP_CREATED}, {"modified", PROP_MODIFIED},
};
const NameProp kAppNames[] = {    // OOXML docProps/app.xml
    {"Company", PROP_COMPANY}, {"Manager", PROP_MANAGER},
    {"Template", PROP_TEMPLATE}, {"Application", PROP_APPLICATION},
};
const NameProp kOdfNames[] = {    // ODF meta.xml
    {"title", PROP_TITLE}, {"subject", PROP_SUBJECT},
    {"initial-creator", PROP_AUTHOR}, {"creator", PROP_LAST_AUTHOR},
    {"keyword", PROP_KEYWORDS}, {"description", PROP_DESCRIPTION},
    {"creation-date", PROP_CREATED}, {"date", PROP_MODIFIED},
    {"generator", PROP_APPLICATION},
};
const NameProp kHpfNames[] = {    // HWPX content.hpf (<opf:meta name=...>)
    {"title", PROP_TITLE}, {"subject", PROP_SUBJECT}, {"creator", PROP_AUTHOR},
    {"lastsaveby", PROP_LAST_AUTHOR}, {"keyword", PROP_KEYWORDS},
    {"description", PROP_DESCRIPTION}, {"CreatedDate", PROP_CREATED},
    {"ModifiedDate", PROP_MODIFIED},
};

// The children of `parent` (the root when null) in a package part, keyed by
// their local name or, for <meta name="...">, by that name.
template <size_t N>
void read_xml_properties(ZipReader& zip, const std::string& path, const char* parent,
                         const NameProp (&names)[N], DocProps& props) {
    pugi::xml_document doc;
    std::vector<char> buf;
    if (!zip.has_entry(path) || !xml_load_part(zip, path, doc, buf)) return;
    pugi::xml_node node = doc.document_element();
    if (parent) node = xml_child(node, parent);
    for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
        const char* key = xml_local_name(c);
        if (strcmp(key, "meta") == 0 && *xml_attr(c, "name")) key = xml_attr(c, "name");
        for (const auto& e : names)
            if (strcmp(e.name, key) == 0) add_property(props, e.prop, xml_text_content(c));
    }
}

bool ends_with(const char* s, const char* suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && strcmp(s + n - k, suffix) == 0;
}

void read_zip_properties(const uint8_t* data, size_t size, DocProps& props) {
    ZipReader zip(data, size);
    if (!zip.is_open()) return;
    // The package relationships name the OOXML property parts.
    std::string core = "docProps/core.xml", app = "docProps/app.xml";
    pugi::xml_document rels;
    std::vector<char> buf;
    if (zip.has_entry("_rels/.rels") && xml_load_part(zip, "_rels/.rels", rels, buf)) {
        for (pugi::xml_node r : rels.document_element().children()) {
            const char* type = xml_attr(r, "Type");
            const char* target = xml_attr(r, "Target");
            while (*target == '/') target++;
            if (ends_with(type, "/core-properties")) core = target;
            else if (ends_with(type, "/extended-properties")) app = target;
        }
    }
    read_xml_properties(zip, core, nullptr, kCoreNames, props);
    read_xml_properties(zip, app, nullptr, kAppNames, props);
    read_xml_properties(zip, "meta.xml", "meta", kOdfNames, props);
    read_xml_properties(zip, "Contents/content.hpf", "metadata", kHpfNames, props);
}

// One line per value: controls become spaces, runs of spaces collapse.
std::string one_line(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out.push_back(char(c));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace

DocProps document_properties(const uint8_t* data, size_t size) {
    DocProps props;
    if (!data || size < 8) return props;
    try {
        if (memcmp(data, "PK\x03\x04", 4) == 0) read_zip_properties(data, size, props);
        else if (memcmp(data, "\xD0\xCF\x11\xE0", 4) == 0) read_ole_properties(data, size, props);
        else if (memcmp(data, "%PDF-", 5) == 0) read_pdf_properties(data, size, props);
        else if (is_hwp3_signature(data, size)) read_hwp3_properties(data, size, props);
    } catch (...) {
    }
    return props;
}

std::string properties_block(const DocProps& props) {
    std::string out;
    for (int k = 0; k < PROP_COUNT; k++) {
        std::string v = one_line(props[k]);
        if (v.empty()) continue;
        out += kPropKeys[k];
        out += ": \"";
        for (char c : v) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        out += "\"\n";
    }
    return out.empty() ? out : "---\n" + out + "---\n\n";
}

} // namespace jdoc
