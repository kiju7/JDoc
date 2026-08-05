#pragma once
// Chunk-fed streaming XML scanner for OOXML parts too large for a DOM.
// Feed arbitrary byte chunks (e.g. from ZipReader::read_entry_streamed);
// the handler receives start/end/text events with entities already decoded.
//
// This is not a general XML parser: it assumes well-formed OOXML output
// (no DTDs, no processing beyond the leading declaration) and reports the
// namespace-local element name only. Constructs that could make the carry
// buffer grow without bound (a tag or comment larger than kMaxCarry) abort
// the scan instead of allocating.
//
// Handler contract (compile-time, no virtual dispatch — the tag loop runs
// ~100M times on a multi-GB part):
//   void on_start(std::string_view local_name, std::string_view tag_body,
//                 bool self_closing);
//   void on_end(std::string_view local_name);
//   void on_text(const char* data, size_t len);   // decoded, may arrive split
//
// License: MIT

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace jdoc {

// Extract an attribute's raw value from a start-tag body slice.
// Matches the namespace-local attribute name (so "r:id" matches query "id"
// only when no plain "id" is present earlier). Returns empty view if absent.
inline std::string_view xml_stream_attr(std::string_view tag_body,
                                        const char* name) {
    const size_t nlen = strlen(name);
    size_t i = 0;
    while (i < tag_body.size()) {
        // skip whitespace
        while (i < tag_body.size() &&
               (tag_body[i] == ' ' || tag_body[i] == '\t' ||
                tag_body[i] == '\n' || tag_body[i] == '\r')) i++;
        size_t name_start = i;
        while (i < tag_body.size() && tag_body[i] != '=' &&
               tag_body[i] != ' ' && tag_body[i] != '\t' &&
               tag_body[i] != '\n' && tag_body[i] != '\r') i++;
        std::string_view aname = tag_body.substr(name_start, i - name_start);
        // namespace-local part
        auto colon = aname.rfind(':');
        std::string_view local = (colon == std::string_view::npos)
            ? aname : aname.substr(colon + 1);
        // skip to '='
        while (i < tag_body.size() && tag_body[i] != '=') i++;
        if (i >= tag_body.size()) return {};
        i++; // '='
        while (i < tag_body.size() &&
               (tag_body[i] == ' ' || tag_body[i] == '\t')) i++;
        if (i >= tag_body.size()) return {};
        char quote = tag_body[i];
        if (quote != '"' && quote != '\'') return {};
        i++;
        size_t val_start = i;
        while (i < tag_body.size() && tag_body[i] != quote) i++;
        if (local.size() == nlen && memcmp(local.data(), name, nlen) == 0)
            return tag_body.substr(val_start, i - val_start);
        if (i < tag_body.size()) i++; // closing quote
    }
    return {};
}

// Decode the five predefined entities and numeric character references in
// `raw`, appending to `out`. Unknown/malformed entities pass through as
// literal text (matching pugixml's lenient behavior).
inline void xml_stream_unescape(std::string_view raw, std::string& out) {
    size_t i = 0;
    while (i < raw.size()) {
        const char* amp = static_cast<const char*>(
            memchr(raw.data() + i, '&', raw.size() - i));
        if (!amp) { out.append(raw.data() + i, raw.size() - i); return; }
        size_t a = static_cast<size_t>(amp - raw.data());
        out.append(raw.data() + i, a - i);
        // find ';' within a bounded window
        size_t end = std::string_view::npos;
        size_t limit = a + 12 < raw.size() ? a + 12 : raw.size();
        for (size_t j = a + 1; j < limit; ++j) {
            if (raw[j] == ';') { end = j; break; }
        }
        if (end == std::string_view::npos) { out += '&'; i = a + 1; continue; }
        std::string_view ent = raw.substr(a + 1, end - a - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            uint32_t cp = 0; bool ok = false;
            if (ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X')) {
                for (size_t j = 2; j < ent.size(); ++j) {
                    char c = ent[j]; uint32_t d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else { ok = false; break; }
                    cp = cp * 16 + d; ok = true;
                }
            } else {
                for (size_t j = 1; j < ent.size(); ++j) {
                    char c = ent[j];
                    if (c < '0' || c > '9') { ok = false; break; }
                    cp = cp * 10 + (c - '0'); ok = true;
                }
            }
            if (ok && cp > 0 && cp <= 0x10FFFF) {
                // UTF-8 encode
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            } else {
                out += '&'; out.append(ent.data(), ent.size()); out += ';';
            }
        } else {
            out += '&'; out.append(ent.data(), ent.size()); out += ';';
        }
        i = end + 1;
    }
}

template <class Handler>
class XmlStreamScanner {
public:
    explicit XmlStreamScanner(Handler& h) : h_(h) {}

    // Feed the next chunk. Returns false if the scan aborted (carry overflow
    // from a pathological tag/comment); error() then describes why.
    bool feed(const char* data, size_t len) {
        if (err_) return false;
        if (!carry_.empty()) {
            // Resume an incomplete construct: append and reprocess the carry.
            // The carry only ever holds one incomplete token, so this stays
            // small except for hostile input (bounded by kMaxCarry).
            if (carry_.size() + len > kMaxCarry)
                return fail("oversized XML token");
            carry_.append(data, len);
            std::string buf;
            buf.swap(carry_);
            return process(buf.data(), buf.size());
        }
        return process(data, len);
    }

    // Flush trailing text after the last chunk.
    bool finish() {
        if (err_) return false;
        if (!carry_.empty()) {
            if (state_ == State::Text) {
                emit_text(carry_.data(), carry_.size());
                carry_.clear();
            } else {
                return fail("truncated XML");
            }
        }
        return true;
    }

    const char* error() const { return err_; }

private:
    enum class State { Text, Tag };
    static constexpr size_t kMaxCarry = 16u << 20;  // 16 MiB token guard

    Handler& h_;
    State state_ = State::Text;
    std::string carry_;
    std::string decode_buf_;
    std::string eol_buf_;
    const char* err_ = nullptr;

    bool fail(const char* why) { err_ = why; return false; }

    bool process(const char* data, size_t len) {
        size_t i = 0;
        while (i < len) {
            if (state_ == State::Text) {
                const char* lt = static_cast<const char*>(
                    memchr(data + i, '<', len - i));
                if (!lt) {
                    hold_text_tail(data + i, len - i);
                    return true;
                }
                size_t at = static_cast<size_t>(lt - data);
                if (at > i) emit_text(data + i, at - i);
                i = at;
                state_ = State::Tag;
            } else {
                // i points at '<'. Find the end of this construct.
                size_t remain = len - i;
                if (remain >= 4 && memcmp(data + i, "<!--", 4) == 0) {
                    const char* close = find_str(data + i, remain, "-->", 3);
                    if (!close) { hold(data + i, remain); return err_ == nullptr; }
                    i = static_cast<size_t>(close - data) + 3;
                    state_ = State::Text;
                    continue;
                }
                if (remain >= 9 && memcmp(data + i, "<![CDATA[", 9) == 0) {
                    const char* close = find_str(data + i, remain, "]]>", 3);
                    if (!close) { hold(data + i, remain); return err_ == nullptr; }
                    size_t body = i + 9;
                    size_t close_at = static_cast<size_t>(close - data);
                    if (close_at > body)   // CDATA: EOL-normalized, no entities
                        emit_text(data + body, close_at - body, false);
                    i = close_at + 3;
                    state_ = State::Text;
                    continue;
                }
                if (remain >= 2 && data[i + 1] == '!') {
                    // Not yet distinguishable from a chunk-truncated <!-- or
                    // <![CDATA[ opener: hold until enough bytes arrive.
                    bool maybe_comment =
                        remain < 4 && memcmp(data + i, "<!--", remain) == 0;
                    bool maybe_cdata =
                        remain < 9 && memcmp(data + i, "<![CDATA[", remain) == 0;
                    if (maybe_comment || maybe_cdata) {
                        hold(data + i, remain); return err_ == nullptr;
                    }
                }
                size_t gt = find_tag_end(data, i, len);
                if (gt == kIncomplete) { hold(data + i, remain); return err_ == nullptr; }
                handle_tag(data + i + 1, gt - i - 1);
                i = gt + 1;
                state_ = State::Text;
            }
        }
        return err_ == nullptr;
    }

    static constexpr size_t kIncomplete = static_cast<size_t>(-1);

    // Find the '>' terminating the tag starting at data[start]=='<',
    // ignoring '>' inside quoted attribute values.
    static size_t find_tag_end(const char* data, size_t start, size_t len) {
        char quote = 0;
        for (size_t j = start + 1; j < len; ++j) {
            char c = data[j];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '>') return j;
        }
        return kIncomplete;
    }

    static const char* find_str(const char* hay, size_t hay_len,
                                const char* needle, size_t nlen) {
        if (hay_len < nlen) return nullptr;
        const char* end = hay + hay_len - nlen + 1;
        for (const char* p = hay; p < end; ++p) {
            p = static_cast<const char*>(memchr(p, needle[0], end - p));
            if (!p) return nullptr;
            if (memcmp(p, needle, nlen) == 0) return p;
        }
        return nullptr;
    }

    void hold(const char* data, size_t len) {
        if (len > kMaxCarry) { fail("oversized XML token"); return; }
        carry_.assign(data, len);
    }

    // Text may end with a partial entity ("&am" | "p;") or a '\r' whose
    // \r\n-collapse depends on the next chunk's first byte; hold those back
    // so decoding and EOL normalization never see a split token.
    void hold_text_tail(const char* data, size_t len) {
        size_t hold_from = len;
        size_t window = len < 12 ? len : 12;
        for (size_t j = len - window; j < len; ++j) {
            if (data[j] == '&') hold_from = j;
            else if (data[j] == ';') hold_from = len;
        }
        if (hold_from == len && len > 0 && data[len - 1] == '\r')
            hold_from = len - 1;
        if (hold_from > 0) emit_text(data, hold_from, true);
        if (hold_from < len) carry_.assign(data + hold_from, len - hold_from);
    }

    void emit_text(const char* data, size_t len, bool decode_entities = true) {
        if (len == 0) return;
        // Fast path: no entities, no carriage returns — raw slice through.
        bool has_amp = decode_entities && memchr(data, '&', len) != nullptr;
        bool has_cr = memchr(data, '\r', len) != nullptr;
        if (!has_amp && !has_cr) { h_.on_text(data, len); return; }
        // XML end-of-line normalization (\r\n → \n, lone \r → \n), applied to
        // raw bytes before entity decoding — a decoded &#13; stays '\r',
        // matching pugixml's parse_eol semantics.
        if (has_cr) {
            eol_buf_.clear();
            for (size_t j = 0; j < len; ++j) {
                if (data[j] == '\r') {
                    eol_buf_ += '\n';
                    if (j + 1 < len && data[j + 1] == '\n') j++;
                } else {
                    eol_buf_ += data[j];
                }
            }
            data = eol_buf_.data();
            len = eol_buf_.size();
        }
        if (!has_amp) { h_.on_text(data, len); return; }
        decode_buf_.clear();
        xml_stream_unescape({data, len}, decode_buf_);
        h_.on_text(decode_buf_.data(), decode_buf_.size());
    }

    void handle_tag(const char* body, size_t len) {
        if (len == 0) return;
        if (body[0] == '?') return;                 // <?xml ...?>
        if (body[0] == '!') return;                 // <!DOCTYPE ...> (no DTDs in OOXML)
        bool is_end = (body[0] == '/');
        size_t p = is_end ? 1 : 0;
        size_t name_start = p;
        while (p < len && body[p] != ' ' && body[p] != '\t' &&
               body[p] != '\n' && body[p] != '\r' && body[p] != '/') p++;
        std::string_view name(body + name_start, p - name_start);
        auto colon = name.rfind(':');
        if (colon != std::string_view::npos) name = name.substr(colon + 1);

        if (is_end) { h_.on_end(name); return; }

        bool self_closing = false;
        size_t body_end = len;
        // trailing '/' (possibly preceded by whitespace)
        size_t q = len;
        while (q > p && (body[q - 1] == ' ' || body[q - 1] == '\t' ||
                         body[q - 1] == '\n' || body[q - 1] == '\r')) q--;
        if (q > p && body[q - 1] == '/') { self_closing = true; body_end = q - 1; }

        std::string_view tag_body(body + p, body_end - p);
        h_.on_start(name, tag_body, self_closing);
        if (self_closing) h_.on_end(name);
    }
};

} // namespace jdoc
