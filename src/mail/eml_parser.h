#pragma once
// EML (RFC 5322 / MIME) parser: decodes headers and MIME parts to markdown.

#include "jdoc/types.h"
#include <map>
#include <string>
#include <vector>

namespace jdoc {

// One MIME entity (headers + body). A multipart entity fills `children`; a leaf
// fills `decoded_body` (CTE-decoded and converted to UTF-8).
struct MimePart {
    std::string content_type;       // lower-case, no params ("text/plain")
    std::string charset;            // lower-case ("utf-8", "ks_c_5601-1987"…)
    std::string boundary;           // multipart boundary
    std::string transfer_encoding;  // lower-case ("base64"/"quoted-printable"…)
    std::string disposition;        // "inline" / "attachment"
    std::string filename;           // decoded name= / filename=
    std::string decoded_body;       // leaf: UTF-8 body
    std::vector<MimePart> children; // multipart sub-parts
    bool is_multipart() const { return content_type.rfind("multipart/", 0) == 0; }
};

class EmlParser {
public:
    explicit EmlParser(const std::string& file_path);
    EmlParser(const uint8_t* data, size_t size);

    std::string to_markdown(const ConvertOptions& opts);
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    std::string raw_;

    // Parse headers starting at `start` into `out` (lower-cased names, unfolded,
    // first occurrence wins). Returns the body offset (past the blank line).
    size_t parse_headers(size_t start,
                         std::map<std::string, std::string>& out) const;

    // Parse one MIME entity spanning [body_begin, body_end) with its headers.
    MimePart parse_part(size_t body_begin, size_t body_end,
                        const std::map<std::string, std::string>& headers,
                        int depth) const;

    // Split a multipart body by boundary into child parts.
    void split_multipart(MimePart& parent, size_t body_begin, size_t body_end,
                         int depth) const;

    // Pick the body text to display; collect attachment descriptions.
    std::string select_body(const MimePart& part,
                            std::vector<std::string>& attachments,
                            const ConvertOptions& opts, bool in_alternative) const;
};

} // namespace jdoc
