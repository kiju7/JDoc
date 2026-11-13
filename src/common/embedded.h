#pragma once
// Text of a document embedded in another one: an OLE object in HWP/HWPX or a
// legacy Office file, an OOXML embeddings/ part, an ODF sub-document. A
// spreadsheet pasted into a report holds text a reader of the report sees, so
// it is converted like any other document rather than listed by name.
// License: MIT

#include "jdoc/types.h"

#include <cstdint>
#include <string>

namespace jdoc {

class OleReader;

// The embedded payload converted in opts.format, or "" when it is not a
// document jdoc reads (an equation, a chart's cache, an unknown object), it
// fails to parse, or nesting goes deeper than opts.archive.max_depth.
// Never throws.
std::string embedded_document_text(const uint8_t* data, size_t size,
                                   const std::string& name_hint,
                                   const ConvertOptions& opts);

// The same for an object a legacy Office file keeps as one of its storages
// (ObjectPool/_123 in .doc, MBD0001 in .xls).
std::string embedded_storage_text(const OleReader& host, const std::string& storage,
                                  const ConvertOptions& opts);

// "## Attachments" with the text of each embedded object storage of a .doc
// (ObjectPool/*) or .xls (MBD* at the root); "" when none has text.
std::string ole_storage_attachments(const OleReader& host, const ConvertOptions& opts);

} // namespace jdoc
