#pragma once
// ODF (OpenDocument Format) parser — odt / ods / odp.
// Extracts text, tables, and images from an OpenDocument package to Markdown.

#include "zip_reader.h"
#include "jdoc/office.h"   // DocFormat
#include "jdoc/types.h"
#include "pugixml/pugixml.hpp"

#include <map>
#include <string>
#include <vector>

namespace jdoc {

class OdfParser {
public:
    // kind is one of DocFormat::ODT, ODS, ODP.
    OdfParser(ZipReader& zip, DocFormat kind);

    std::string to_markdown(const ConvertOptions& opts);
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    ZipReader& zip_;
    DocFormat kind_;
    std::map<std::string, std::string> image_name_map_;  // Pictures/x.png -> ref name

    bool load_content(pugi::xml_document& doc, std::vector<char>& buf) const;

    std::vector<PageChunk> parse_text(const pugi::xml_node& body,
                                      const ConvertOptions& opts);
    std::vector<PageChunk> parse_spreadsheet(const pugi::xml_node& body,
                                             const ConvertOptions& opts);
    std::vector<PageChunk> parse_presentation(const pugi::xml_node& body,
                                              const ConvertOptions& opts);

    // Render a text:p / text:h paragraph, honoring text:line-break, text:tab,
    // and text:s (repeated spaces).
    std::string paragraph_text(const pugi::xml_node& p) const;
    // A text:h heading level from text:outline-level (clamped to 1..6).
    int heading_level(const pugi::xml_node& p) const;
    // A table:table rendered as a Markdown table (column/row repeats expanded).
    std::string format_table(const pugi::xml_node& table) const;
    // Markdown image reference for a draw:image, or "" if not extracted.
    std::string image_ref(const pugi::xml_node& image,
                          const ConvertOptions& opts) const;

    std::vector<ImageData> extract_images(const ConvertOptions& opts);
};

} // namespace jdoc
