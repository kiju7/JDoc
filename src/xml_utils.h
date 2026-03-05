#pragma once
// XML utility helpers for OOXML parsing with pugixml
// License: MIT

#include "pugixml/pugixml.hpp"
#include <cstring>
#include <string>
#include <vector>

namespace jdoc {

// Collect all text content from a node and its descendants recursively
inline std::string xml_text_content(const pugi::xml_node& node) {
    std::string result;
    for (auto child = node.first_child(); child; child = child.next_sibling()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            result += child.value();
        } else {
            result += xml_text_content(child);
        }
    }
    return result;
}

// Find all descendant nodes with a given name (ignoring namespace prefixes)
inline void xml_find_all(const pugi::xml_node& node, const char* local_name,
                          std::vector<pugi::xml_node>& results) {
    for (auto child = node.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        // Match after namespace prefix (e.g. "w:t" matches "t")
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;
        if (strcmp(local, local_name) == 0) {
            results.push_back(child);
        }
        xml_find_all(child, local_name, results);
    }
}

// Get child by local name (ignoring namespace prefix)
inline pugi::xml_node xml_child(const pugi::xml_node& node, const char* local_name) {
    for (auto child = node.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;
        if (strcmp(local, local_name) == 0) return child;
    }
    return {};
}

// Get attribute value by local name
inline const char* xml_attr(const pugi::xml_node& node, const char* attr_local) {
    for (auto attr = node.first_attribute(); attr; attr = attr.next_attribute()) {
        const char* name = attr.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;
        if (strcmp(local, attr_local) == 0) return attr.value();
    }
    return "";
}

} // namespace jdoc
