// Outlook .msg (MS-OXMSG) parser implementation.
// License: MIT

#include "mail/msg_parser.h"
#include "common/string_utils.h"
#include "html/html_parser.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace jdoc {

namespace {

std::string trim_nul(std::string s) {
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

} // namespace

MsgParser::MsgParser(OleReader& ole) : ole_(ole) {}

std::string MsgParser::substg_name(uint16_t prop_id, uint16_t prop_type) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "__substg1.0_%04X%04X", prop_id, prop_type);
    return buf;
}

std::string MsgParser::read_prop_string(uint16_t prop_id, const std::string& storage) {
    const std::string base = storage.empty() ? "" : storage + "/";

    // PT_UNICODE (001F): UTF-16LE.
    std::string uni = base + substg_name(prop_id, 0x001F);
    if (ole_.has_stream(uni)) {
        auto raw = ole_.read_stream(uni);
        return trim_nul(util::utf16le_to_utf8(raw.data(), raw.size()));
    }
    // PT_STRING8 (001E): ANSI code page (CP949 in Korean files).
    std::string a8 = base + substg_name(prop_id, 0x001E);
    if (ole_.has_stream(a8)) {
        auto raw = ole_.read_stream(a8);
        return trim_nul(util::plain_text_to_utf8(std::string(raw.begin(), raw.end())));
    }
    return {};
}

std::string MsgParser::build_header() {
    std::string subject = read_prop_string(0x0037);
    if (subject.empty()) subject = read_prop_string(0x0070);  // conversation topic

    std::string from_name  = read_prop_string(0x0C1A);
    std::string from_email = read_prop_string(0x0C1F);
    if (from_name.empty())  from_name  = read_prop_string(0x0042);
    if (from_email.empty()) from_email = read_prop_string(0x0065);

    // Prefer per-recipient details (name + email); fall back to the display
    // string PR_DISPLAY_TO / PR_DISPLAY_CC.
    std::vector<std::string> recips = read_recipients();
    std::string to;
    if (!recips.empty()) {
        for (size_t i = 0; i < recips.size(); i++) {
            if (i) to += "; ";
            to += recips[i];
        }
    } else {
        to = read_prop_string(0x0E04);  // PR_DISPLAY_TO
    }
    std::string cc = read_prop_string(0x0E03);  // PR_DISPLAY_CC

    std::string h;
    h += "# " + (subject.empty() ? std::string("(제목 없음)") : subject) + "\n\n";
    if (!from_name.empty() || !from_email.empty()) {
        h += "- **From:** " + from_name;
        if (!from_email.empty()) {
            if (!from_name.empty()) h += " ";
            h += "<" + from_email + ">";
        }
        h += "\n";
    }
    if (!to.empty()) h += "- **To:** " + to + "\n";
    if (!cc.empty()) h += "- **Cc:** " + cc + "\n";
    h += "\n---\n\n";
    return h;
}

std::vector<std::string> MsgParser::read_recipients() {
    std::vector<std::string> out;
    for (const auto& st : storages_with_prefix("__recip_version1.0_#")) {
        std::string name = read_prop_string(0x3001, st);   // display name
        std::string email = read_prop_string(0x3003, st);  // email address
        std::string entry;
        if (!name.empty()) entry = name;
        if (!email.empty()) {
            if (!entry.empty()) entry += " ";
            entry += "<" + email + ">";
        }
        if (!entry.empty()) out.push_back(entry);
    }
    return out;
}

std::vector<std::string> MsgParser::read_attachment_names() {
    std::vector<std::string> out;
    for (const auto& st : storages_with_prefix("__attach_version1.0_#")) {
        std::string nm = read_prop_string(0x3707, st);   // long file name
        if (nm.empty()) nm = read_prop_string(0x3704, st);  // short file name
        if (!nm.empty()) out.push_back(nm);
    }
    return out;
}

std::string MsgParser::extract_body(const ConvertOptions& opts) {
    // Plain-text body (PR_BODY, tag 1000) is present in virtually all messages
    // and needs no rendering, so it is preferred.
    std::string plain = read_prop_string(0x1000);
    if (!plain.empty()) return plain;

    // Otherwise fall back to an HTML body (PR_HTML, tag 1013) stored as a text
    // property. The binary (0102) variant and compressed RTF (1009, LZFu) are
    // not handled here.
    for (uint16_t t : {uint16_t(0x001F), uint16_t(0x001E)}) {
        std::string nm = substg_name(0x1013, t);
        if (ole_.has_stream(nm)) {
            auto raw = ole_.read_stream(nm);
            std::string html = (t == 0x001F)
                ? util::utf16le_to_utf8(raw.data(), raw.size())
                : util::plain_text_to_utf8(std::string(raw.begin(), raw.end()));
            HtmlParser p(html.data(), html.size());
            return p.to_markdown(opts);
        }
    }
    return {};
}

std::vector<std::string> MsgParser::storages_with_prefix(const std::string& prefix) {
    std::set<std::string> seen;
    std::vector<std::string> out;
    for (const auto& s : ole_.list_streams()) {
        if (s.rfind(prefix, 0) != 0) continue;
        std::string top = s.substr(0, s.find('/'));
        if (seen.insert(top).second) out.push_back(top);
    }
    return out;
}

std::string MsgParser::to_markdown(const ConvertOptions& opts) {
    std::string out = build_header();
    std::string body = extract_body(opts);
    out += body.empty() ? "*(본문 없음)*\n" : body;

    auto att_storages = storages_with_prefix("__attach_version1.0_#");
    if (!att_storages.empty()) {
        out += "\n\n---\n\n**첨부(" + std::to_string(att_storages.size()) + ")**";
        auto names = read_attachment_names();
        if (!names.empty()) {
            out += ": ";
            for (size_t i = 0; i < names.size(); i++) {
                if (i) out += ", ";
                out += names[i];
            }
        }
        out += "\n";
    }
    return util::sanitize_utf8(out);
}

std::vector<PageChunk> MsgParser::to_chunks(const ConvertOptions& opts) {
    PageChunk c;
    c.page_number = 1;
    c.text = to_markdown(opts);
    return {c};
}

} // namespace jdoc
