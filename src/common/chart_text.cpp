// License: MIT
#include "common/chart_text.h"
#include "xml_utils.h"

#include <vector>

namespace jdoc {

namespace {

void add(std::string& out, const std::string& s) {
    if (s.empty()) return;
    if (!out.empty()) out += ' ';
    out += s;
}

// The text of a rich title (c:title/c:tx/c:rich, a:t runs joined).
std::string title_text(const pugi::xml_node& title) {
    std::vector<pugi::xml_node> runs;
    xml_find_all(title, "t", runs);
    std::string s;
    for (auto& t : runs) s += xml_text_content(t);
    return s;
}

// Every c:pt/c:v of a string reference or literal (strRef/strCache, strLit,
// multiLvlStrRef), or a bare c:v as Hancom writes a series name.
void add_values(std::string& out, const pugi::xml_node& node) {
    if (!node) return;
    std::vector<pugi::xml_node> pts;
    xml_find_all(node, "pt", pts);
    if (pts.empty()) {
        auto v = xml_child(node, "v");
        if (v) add(out, xml_text_content(v));
        return;
    }
    for (auto& pt : pts) {
        auto v = xml_child(pt, "v");
        if (v) add(out, xml_text_content(v));
    }
}

} // namespace

std::string drawingml_chart_text(const char* xml, size_t size) {
    if (!xml || size == 0) return "";
    pugi::xml_document doc;
    if (!doc.load_buffer(xml, size, pugi::parse_default | pugi::parse_ws_pcdata))
        return "";
    pugi::xml_node space = doc.document_element();
    if (!space || std::string(xml_local_name(space)) != "chartSpace") return "";
    pugi::xml_node chart = xml_child(space, "chart");
    if (!chart) return "";

    std::string out;
    if (auto title = xml_child(chart, "title")) add(out, title_text(title));

    pugi::xml_node plot = xml_child(chart, "plotArea");
    if (!plot) return out;
    // Axis titles (c:catAx, c:valAx, c:dateAx, c:serAx).
    for (auto ax : plot.children()) {
        std::string n = xml_local_name(ax);
        if (n.size() > 2 && n.compare(n.size() - 2, 2, "Ax") == 0)
            if (auto title = xml_child(ax, "title")) add(out, title_text(title));
    }
    // Series: name, then category labels. Values are numbers and stay out.
    for (auto group : plot.children()) {
        for (auto ser : group.children()) {
            if (std::string(xml_local_name(ser)) != "ser") continue;
            add_values(out, xml_child(ser, "tx"));
            add_values(out, xml_child(ser, "cat"));
            add_values(out, xml_child(ser, "xVal"));
        }
    }
    return out;
}

} // namespace jdoc
