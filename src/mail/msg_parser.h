#pragma once
// Outlook .msg (MS-OXMSG) parser. Reads the mail headers and body from the
// __substg1.0_* property streams of the OLE/CFB container.

#include "legacy/ole_reader.h"
#include "jdoc/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jdoc {

class MsgParser {
public:
    explicit MsgParser(OleReader& ole);

    std::string to_markdown(const ConvertOptions& opts);
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    OleReader& ole_;

    // "__substg1.0_" + 4-hex prop id + 4-hex prop type.
    static std::string substg_name(uint16_t prop_id, uint16_t prop_type);

    // Read a string property, trying PT_UNICODE (001F) then PT_STRING8 (001E).
    // A non-empty `storage` scopes the lookup to that sub-storage.
    std::string read_prop_string(uint16_t prop_id, const std::string& storage = "");

    std::string build_header();
    std::string extract_body(const ConvertOptions& opts);

    // Distinct sub-storage names (before the first '/') whose path starts with
    // `prefix`, e.g. "__recip_version1.0_#" or "__attach_version1.0_#".
    std::vector<std::string> storages_with_prefix(const std::string& prefix);

    // Per-recipient "name <email>" strings from __recip_version1.0_#NNNN.
    std::vector<std::string> read_recipients();
    // Per-attachment file names from __attach_version1.0_#NNNN.
    std::vector<std::string> read_attachment_names();
};

} // namespace jdoc
