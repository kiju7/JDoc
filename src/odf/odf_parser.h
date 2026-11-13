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
    // kind is one of DocFormat::ODT, ODS, ODP (it only picks the page
    // separators of to_markdown; the body's own element decides the layout).
    // prefix roots the parser at a sub-document ("Object 1/").
    OdfParser(ZipReader& zip, DocFormat kind, std::string prefix = "");

    std::string to_markdown(const ConvertOptions& opts);
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    ZipReader& zip_;
    DocFormat kind_;
    std::string prefix_;
    std::map<std::string, std::string> image_name_map_;  // part path (Pictures/x.png) -> ref name
    pugi::xml_node skip_;     // already rendered (a slide's title frame)
    bool in_list_ = false;    // paragraphs are list items
    bool flat_ = false;       // inside a table cell: tables read as text

    bool load_part(const char* part, pugi::xml_document& doc,
                   std::vector<char>& buf) const;

    std::vector<PageChunk> parse_spreadsheet(const pugi::xml_node& body,
                                             const ConvertOptions& opts);
    std::vector<PageChunk> parse_presentation(const pugi::xml_node& body,
                                              const ConvertOptions& opts);

    // Every block under n in document order, as markdown.
    void walk(const pugi::xml_node& n, std::string& md, const ConvertOptions& opts,
              int depth);
    void walk_one(const pugi::xml_node& c, std::string& md, const ConvertOptions& opts,
                  int depth);
    std::string flat_text(const pugi::xml_node& n, const ConvertOptions& opts, int depth);
    int heading_level(const pugi::xml_node& p) const;
    std::string format_table(const pugi::xml_node& table, const ConvertOptions& opts,
                             int depth);
    std::string object_text(const pugi::xml_node& obj, const ConvertOptions& opts,
                            int depth);
    std::string header_footer_text(const ConvertOptions& opts);
    std::string image_ref(const pugi::xml_node& image, const ConvertOptions& opts) const;
    void extract_images(const pugi::xml_node& root, const ConvertOptions& opts);
};

} // namespace jdoc
