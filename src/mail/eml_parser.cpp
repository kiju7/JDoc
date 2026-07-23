// EML (RFC 5322 / MIME) parser implementation.
// License: MIT

#include "eml_parser.h"
#include "jdoc/eml.h"
#include "html/html_parser.h"
#include "common/string_utils.h"
#include "common/file_utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>

namespace jdoc {

namespace {

constexpr int kMaxDepth = 32;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' ||
                     s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

// Parse "; key=value; key2=\"quoted value\"" parameters starting at `from`.
std::map<std::string, std::string> parse_params(const std::string& s, size_t from) {
    std::map<std::string, std::string> params;
    size_t i = from;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ';' || s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= s.size()) break;
        size_t eq = s.find('=', i);
        if (eq == std::string::npos) break;
        std::string key = lower(trim(s.substr(i, eq - i)));
        size_t v = eq + 1;
        while (v < s.size() && (s[v] == ' ' || s[v] == '\t')) v++;
        std::string val;
        if (v < s.size() && s[v] == '"') {
            v++;
            size_t start = v;
            while (v < s.size() && s[v] != '"') v++;
            val = s.substr(start, v - start);
            if (v < s.size()) v++;  // skip closing quote
        } else {
            size_t start = v;
            while (v < s.size() && s[v] != ';') v++;
            val = trim(s.substr(start, v - start));
        }
        if (!key.empty()) params[key] = val;
        i = v;
    }
    return params;
}

// Decode RFC 2047 encoded words (=?charset?B/Q?text?=) embedded in a header.
std::string decode_encoded_words(const std::string& in) {
    std::string out;
    size_t i = 0;
    bool prev_was_word = false;
    while (i < in.size()) {
        size_t start = in.find("=?", i);
        if (start == std::string::npos) {
            out += in.substr(i);
            break;
        }
        size_t c1 = start + 2;
        size_t q1 = in.find('?', c1);
        size_t q2 = (q1 == std::string::npos) ? std::string::npos : in.find('?', q1 + 1);
        size_t end = (q2 == std::string::npos) ? std::string::npos : in.find("?=", q2 + 1);
        if (end == std::string::npos) {  // not a well-formed encoded word
            out += in.substr(i);
            break;
        }

        std::string between = in.substr(i, start - i);
        bool between_ws_only = !between.empty() &&
            between.find_first_not_of(" \t\r\n") == std::string::npos;
        // Linear whitespace between two adjacent encoded words is dropped.
        if (!(prev_was_word && between_ws_only)) out += between;

        std::string charset = in.substr(c1, q1 - c1);
        std::string enc = in.substr(q1 + 1, q2 - (q1 + 1));
        std::string text = in.substr(q2 + 1, end - (q2 + 1));

        std::string decoded;
        if (enc == "B" || enc == "b") decoded = util::decode_base64(text);
        else if (enc == "Q" || enc == "q") decoded = util::decode_quoted_printable(text, true);
        else decoded = text;
        out += util::charset_to_utf8(decoded, charset);

        prev_was_word = true;
        i = end + 2;
    }
    return out;
}

} // namespace

// ── Constructors ────────────────────────────────────────────

EmlParser::EmlParser(const std::string& file_path) {
    std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
    if (!ifs) return;
    std::streamsize size = ifs.tellg();
    if (size <= 0) return;
    ifs.seekg(0, std::ios::beg);
    raw_.resize(static_cast<size_t>(size));
    if (!ifs.read(&raw_[0], size)) raw_.clear();
}

EmlParser::EmlParser(const uint8_t* data, size_t size)
    : raw_(reinterpret_cast<const char*>(data), size) {}

// ── Header parsing ──────────────────────────────────────────

size_t EmlParser::parse_headers(size_t start,
                                std::map<std::string, std::string>& out) const {
    size_t pos = start;
    std::string cur_name, cur_val;
    auto flush = [&]() {
        if (!cur_name.empty() && out.find(cur_name) == out.end())
            out[cur_name] = cur_val;
        cur_name.clear();
        cur_val.clear();
    };

    while (pos < raw_.size()) {
        size_t nl = raw_.find('\n', pos);
        size_t line_end = (nl == std::string::npos) ? raw_.size() : nl;
        size_t content_end = line_end;
        if (content_end > pos && raw_[content_end - 1] == '\r') content_end--;
        std::string line = raw_.substr(pos, content_end - pos);
        size_t next = (nl == std::string::npos) ? raw_.size() : nl + 1;

        if (line.empty()) {          // blank line ends the header block
            flush();
            return next;
        }
        if (line[0] == ' ' || line[0] == '\t') {  // folded continuation
            size_t s = 0;
            while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) s++;
            if (!cur_name.empty()) {
                cur_val += " ";
                cur_val += line.substr(s);
            }
        } else {
            flush();
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                cur_name = lower(line.substr(0, colon));
                size_t vs = colon + 1;
                while (vs < line.size() && (line[vs] == ' ' || line[vs] == '\t')) vs++;
                cur_val = line.substr(vs);
            }
        }
        pos = next;
    }
    flush();
    return raw_.size();
}

// ── MIME part parsing ───────────────────────────────────────

MimePart EmlParser::parse_part(
    size_t body_begin, size_t body_end,
    const std::map<std::string, std::string>& headers, int depth) const {

    MimePart part;
    part.content_type = "text/plain";

    auto ct = headers.find("content-type");
    if (ct != headers.end()) {
        const std::string& v = ct->second;
        size_t semi = v.find(';');
        part.content_type = lower(trim(v.substr(0, semi)));
        auto params = parse_params(v, semi == std::string::npos ? v.size() : semi);
        part.charset = lower(params["charset"]);
        part.boundary = params["boundary"];
        part.filename = params["name"];
    }

    auto te = headers.find("content-transfer-encoding");
    if (te != headers.end()) part.transfer_encoding = lower(trim(te->second));

    auto cd = headers.find("content-disposition");
    if (cd != headers.end()) {
        const std::string& v = cd->second;
        size_t semi = v.find(';');
        part.disposition = lower(trim(v.substr(0, semi)));
        auto params = parse_params(v, semi == std::string::npos ? v.size() : semi);
        if (!params["filename"].empty()) part.filename = params["filename"];
    }
    part.filename = decode_encoded_words(part.filename);

    if (part.is_multipart() && !part.boundary.empty() && depth < kMaxDepth) {
        split_multipart(part, body_begin, body_end, depth);
        return part;
    }

    // Leaf part: decode transfer encoding, then convert text to UTF-8.
    if (body_end < body_begin) body_end = body_begin;
    std::string body = raw_.substr(body_begin, body_end - body_begin);
    std::string decoded;
    if (part.transfer_encoding == "base64") decoded = util::decode_base64(body);
    else if (part.transfer_encoding == "quoted-printable")
        decoded = util::decode_quoted_printable(body);
    else decoded = std::move(body);

    if (part.content_type.rfind("text/", 0) == 0)
        part.decoded_body = util::charset_to_utf8(decoded, part.charset);
    else
        part.decoded_body = std::move(decoded);  // attachment bytes (unused)
    return part;
}

void EmlParser::split_multipart(MimePart& parent, size_t body_begin,
                                size_t body_end, int depth) const {
    const std::string delim = "--" + parent.boundary;

    std::vector<std::pair<size_t, size_t>> segments;  // [body_start, body_end)
    size_t cur_start = std::string::npos;
    size_t pos = body_begin;
    std::string_view all(raw_);
    while (pos < body_end) {
        size_t nl = raw_.find('\n', pos);
        size_t line_end = (nl == std::string::npos || nl >= body_end) ? body_end : nl;
        size_t content_end = line_end;
        if (content_end > pos && raw_[content_end - 1] == '\r') content_end--;
        // A view over the line — the whole body is scanned here, so avoid a
        // per-line allocation just to test the boundary delimiter.
        std::string_view line = all.substr(pos, content_end - pos);
        size_t next = (line_end == body_end) ? body_end : line_end + 1;

        if (line.rfind(delim, 0) == 0) {  // boundary delimiter line
            if (cur_start != std::string::npos)
                segments.emplace_back(cur_start, pos);
            std::string_view rest = line.substr(delim.size());
            if (rest.rfind("--", 0) == 0) break;  // closing delimiter
            cur_start = next;
        }
        if (line_end == body_end) break;
        pos = next;
    }

    for (const auto& [seg_begin, seg_end] : segments) {
        std::map<std::string, std::string> h;
        size_t body = parse_headers(seg_begin, h);
        if (body > seg_end) body = seg_end;
        parent.children.push_back(parse_part(body, seg_end, h, depth + 1));
    }
}

// ── Body selection ──────────────────────────────────────────

std::string EmlParser::select_body(const MimePart& part,
                                   std::vector<std::string>& attachments,
                                   const ConvertOptions& opts,
                                   bool in_alternative) const {
    (void)in_alternative;

    if (part.content_type == "multipart/alternative") {
        const MimePart* plain = nullptr;
        const MimePart* html = nullptr;
        for (const auto& c : part.children) {
            if (c.content_type == "text/plain" && !plain) plain = &c;
            else if (c.content_type == "text/html" && !html) html = &c;
        }
        if (plain) return select_body(*plain, attachments, opts, true);
        if (html) return select_body(*html, attachments, opts, true);
        if (!part.children.empty())
            return select_body(part.children.front(), attachments, opts, true);
        return "";
    }

    if (part.is_multipart()) {
        std::string out;
        for (const auto& c : part.children) {
            std::string s = select_body(c, attachments, opts, false);
            if (!s.empty()) {
                if (!out.empty()) out += "\n\n";
                out += s;
            }
        }
        return out;
    }

    // Leaf. Treat explicit attachments and named non-text parts as attachments.
    bool is_text = part.content_type.rfind("text/", 0) == 0;
    if (part.disposition == "attachment" || (!part.filename.empty() && !is_text)) {
        std::string desc = part.filename.empty() ? part.content_type : part.filename;
        desc += " (" + part.content_type + ")";
        attachments.push_back(desc);
        return "";
    }

    if (part.content_type == "text/html") {
        HtmlParser hp(part.decoded_body.data(), part.decoded_body.size());
        return hp.to_markdown(opts);
    }
    if (is_text) return part.decoded_body;
    return "";
}

// ── Output ──────────────────────────────────────────────────

std::string EmlParser::to_markdown(const ConvertOptions& opts) {
    std::map<std::string, std::string> headers;
    size_t body = parse_headers(0, headers);
    MimePart root = parse_part(body, raw_.size(), headers, 0);

    std::vector<std::string> attachments;
    std::string body_text = select_body(root, attachments, opts, false);

    auto hv = [&](const char* k) -> std::string {
        auto it = headers.find(k);
        return it == headers.end() ? std::string() : decode_encoded_words(it->second);
    };
    std::string subject = hv("subject");

    std::string md;
    if (!subject.empty()) md += "# " + subject + "\n\n";

    std::string hdr;
    std::string from = hv("from"), to = hv("to"), cc = hv("cc"), date = hv("date");
    if (!from.empty()) hdr += "**From:** " + from + "\n";
    if (!to.empty())   hdr += "**To:** " + to + "\n";
    if (!cc.empty())   hdr += "**Cc:** " + cc + "\n";
    if (!date.empty()) hdr += "**Date:** " + date + "\n";
    md += hdr;
    if (!hdr.empty()) md += "\n---\n\n";

    md += trim(body_text);

    if (!attachments.empty()) {
        md += "\n\n## 첨부파일\n";
        for (const auto& a : attachments) md += "- " + a + "\n";
    }

    if (opts.format == OutputFormat::PLAINTEXT) return util::strip_markdown(md);
    return md;
}

std::vector<PageChunk> EmlParser::to_chunks(const ConvertOptions& opts) {
    PageChunk chunk;
    chunk.page_number = 0;
    chunk.text = to_markdown(opts);
    return {chunk};
}

// ── Public entry points ─────────────────────────────────────

std::string eml_to_markdown(const std::string& file_path, ConvertOptions opts) {
    EmlParser parser(file_path);
    return parser.to_markdown(opts);
}

std::string eml_to_markdown_mem(const uint8_t* data, size_t size, ConvertOptions opts) {
    EmlParser parser(data, size);
    return parser.to_markdown(opts);
}

std::vector<PageChunk> eml_to_markdown_chunks(const std::string& file_path,
                                              ConvertOptions opts) {
    EmlParser parser(file_path);
    return parser.to_chunks(opts);
}

void eml_to_markdown_chunks_stream(const std::string& file_path,
                                   const ConvertOptions& opts, const PageSink& sink) {
    // An EML message is a single page, so this emits exactly one chunk.
    EmlParser parser(file_path);
    PageChunk chunk;
    chunk.page_number = 0;
    chunk.text = parser.to_markdown(opts);
    sink(std::move(chunk));
}

void eml_to_markdown_chunks_mem_stream(const uint8_t* data, size_t size,
                                       const ConvertOptions& opts,
                                       const PageSink& sink) {
    EmlParser parser(data, size);
    PageChunk chunk;
    chunk.page_number = 0;
    chunk.text = parser.to_markdown(opts);
    sink(std::move(chunk));
}

} // namespace jdoc
