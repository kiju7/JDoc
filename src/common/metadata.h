#pragma once
// metadata.h - Document properties (title, author, last saved by, dates...)
// read from each format's own store: OLE property sets (doc/xls/ppt, HWP 5),
// docProps (OOXML), meta.xml (ODF), content.hpf (HWPX), the PDF /Info
// dictionary and the HWP 3.x summary block. ConvertOptions::metadata prints
// them ahead of the body.
// License: MIT

#include <array>
#include <cstdint>
#include <string>

namespace jdoc {

enum DocProp {
    PROP_TITLE, PROP_SUBJECT, PROP_AUTHOR, PROP_LAST_AUTHOR, PROP_KEYWORDS,
    PROP_DESCRIPTION, PROP_CATEGORY, PROP_COMPANY, PROP_MANAGER, PROP_TEMPLATE,
    PROP_APPLICATION, PROP_CREATED, PROP_MODIFIED, PROP_DATE, PROP_COUNT
};
using DocProps = std::array<std::string, PROP_COUNT>;

// Keep the first value of a property; keywords and descriptions given in
// several pieces are joined.
inline void add_property(DocProps& props, DocProp key, const std::string& value) {
    std::string& v = props[key];
    if (value.empty()) return;
    if (v.empty()) v = value;
    else if (key == PROP_KEYWORDS) v += ", " + value;
    else if (key == PROP_DESCRIPTION) v += ' ' + value;
}

// Properties of the document in the buffer, found by its signature. Formats
// with no property store give none. Never throws.
DocProps document_properties(const uint8_t* data, size_t size);

// A front-matter block, "---\nkey: \"value\"\n---\n\n", of the properties
// that are set; "" when none is.
std::string properties_block(const DocProps& props);

// Readers that live with their format's parser.
void read_pdf_properties(const uint8_t* data, size_t size, DocProps& props);
void read_hwp3_properties(const uint8_t* data, size_t size, DocProps& props);

} // namespace jdoc
