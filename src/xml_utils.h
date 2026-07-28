#pragma once
// XML utility helpers for OOXML parsing with pugixml
// License: MIT

#include "pugixml/pugixml.hpp"
#include "zip_reader.h"
#include <cstring>
#include <string>
#include <vector>

namespace jdoc {

// Element nesting is attacker-controlled — pugixml's parser is iterative and
// imposes no limit — so the recursive walkers below cap their depth to defend
// against stack overflow from malicious input.
constexpr int kXmlMaxDepth = 256;

// Return the namespace-independent part of an XML name. Keeping this in one
// helper avoids each parser spelling out the same strchr/colon handling.
inline const char* xml_local_name(const char* qualified_name) {
    const char* colon = strchr(qualified_name, ':');
    return colon ? colon + 1 : qualified_name;
}

inline const char* xml_local_name(const pugi::xml_node& node) {
    return xml_local_name(node.name());
}

// Read and parse one XML package part in place. `storage` owns the mutable
// buffer for as long as `doc` is in use; parsing in place avoids a second copy.
inline bool xml_load_part(ZipReader& zip, const std::string& path,
                          pugi::xml_document& doc,
                          std::vector<char>& storage,
                          unsigned int options = pugi::parse_default) {
    storage = zip.read_entry(path);
    if (storage.empty()) return false;
    return doc.load_buffer_inplace(storage.data(), storage.size(), options);
}

// Collect all text content from a node and its descendants recursively.
// Skips mc:Fallback branches to avoid duplicate text.
inline std::string xml_text_content(const pugi::xml_node& node, int depth = 0) {
    std::string result;
    if (depth > kXmlMaxDepth) return result;
    for (auto child = node.first_child(); child; child = child.next_sibling()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            result += child.value();
        } else {
            const char* local = xml_local_name(child);
            if (strcmp(local, "Fallback") == 0) continue;
            result += xml_text_content(child, depth + 1);
        }
    }
    return result;
}

// Find all descendant nodes with a given name (ignoring namespace prefixes).
// Skips mc:Fallback branches inside mc:AlternateContent to avoid duplicates.
inline void xml_find_all(const pugi::xml_node& node, const char* local_name,
                          std::vector<pugi::xml_node>& results, int depth = 0) {
    if (depth > kXmlMaxDepth) return;
    for (auto child = node.first_child(); child; child = child.next_sibling()) {
        const char* local = xml_local_name(child);
        // Skip mc:Fallback — mc:Choice content is preferred
        if (strcmp(local, "Fallback") == 0) continue;
        if (strcmp(local, local_name) == 0) {
            results.push_back(child);
        }
        xml_find_all(child, local_name, results, depth + 1);
    }
}

// Get child by local name (ignoring namespace prefix)
inline pugi::xml_node xml_child(const pugi::xml_node& node, const char* local_name) {
    for (auto child = node.first_child(); child; child = child.next_sibling()) {
        const char* local = xml_local_name(child);
        if (strcmp(local, local_name) == 0) return child;
    }
    return {};
}

// Get attribute value by local name
inline const char* xml_attr(const pugi::xml_node& node, const char* attr_local) {
    for (auto attr = node.first_attribute(); attr; attr = attr.next_attribute()) {
        const char* local = xml_local_name(attr.name());
        if (strcmp(local, attr_local) == 0) return attr.value();
    }
    return "";
}

} // namespace jdoc
