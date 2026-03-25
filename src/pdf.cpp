// pdf.cpp — PDF→Markdown with custom parser (no PDFium dependency)
// Features: text, headings, bold/italic, images, table detection, page rendering
// Thread-safe: no global state, fully reentrant

#include "jdoc/pdf.h"
#include "common/string_utils.h"
#include "common/file_utils.h"

#include <jpeglib.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <zlib.h>

namespace jdoc {
namespace {

// ── PDF Object Model ─────────────────────────────────────

enum class ObjType { NONE, BOOL, INT, REAL, STRING, NAME, ARRAY, DICT, STREAM, REF };

struct PdfObj;
using PdfArray = std::vector<PdfObj>;
using PdfDict  = std::vector<std::pair<std::string, PdfObj>>;

struct PdfObj {
    ObjType type = ObjType::NONE;
    bool    bool_val = false;
    int64_t int_val  = 0;
    double  real_val = 0;
    std::string str_val;
    PdfArray arr;
    PdfDict  dict;
    std::vector<uint8_t> stream_data;
    const uint8_t* stream_ptr = nullptr;  // zero-copy: points into file data
    size_t stream_len = 0;
    int ref_num = 0, ref_gen = 0;

    bool is_none() const { return type == ObjType::NONE; }
    bool is_int()  const { return type == ObjType::INT; }
    bool is_real() const { return type == ObjType::REAL; }
    bool is_num()  const { return type == ObjType::INT || type == ObjType::REAL; }
    bool is_name() const { return type == ObjType::NAME; }
    bool is_str()  const { return type == ObjType::STRING; }
    bool is_arr()  const { return type == ObjType::ARRAY; }
    bool is_dict() const { return type == ObjType::DICT || type == ObjType::STREAM; }
    bool is_ref()  const { return type == ObjType::REF; }
    bool is_stream() const { return type == ObjType::STREAM; }
    bool is_bool() const { return type == ObjType::BOOL; }

    const uint8_t* raw_stream_data() const {
        if (stream_ptr) return stream_ptr;
        if (!stream_data.empty()) return stream_data.data();
        return nullptr;
    }
    size_t raw_stream_size() const {
        if (stream_ptr) return stream_len;
        return stream_data.size();
    }

    double as_num() const {
        if (type == ObjType::INT) return static_cast<double>(int_val);
        if (type == ObjType::REAL) return real_val;
        return 0;
    }
    int as_int() const {
        if (type == ObjType::INT) return static_cast<int>(int_val);
        if (type == ObjType::REAL) return static_cast<int>(real_val);
        return 0;
    }

    const PdfObj& get(const std::string& key) const {
        static PdfObj none;
        for (auto& [k, v] : dict)
            if (k == key) return v;
        return none;
    }
    bool has(const std::string& key) const {
        for (auto& [k, v] : dict)
            if (k == key) return true;
        return false;
    }

    static PdfObj make_int(int64_t v) { PdfObj o; o.type = ObjType::INT; o.int_val = v; return o; }
    static PdfObj make_real(double v) { PdfObj o; o.type = ObjType::REAL; o.real_val = v; return o; }
    static PdfObj make_name(const std::string& s) { PdfObj o; o.type = ObjType::NAME; o.str_val = s; return o; }
    static PdfObj make_str(const std::string& s) { PdfObj o; o.type = ObjType::STRING; o.str_val = s; return o; }
    static PdfObj make_ref(int num, int gen) { PdfObj o; o.type = ObjType::REF; o.ref_num = num; o.ref_gen = gen; return o; }
    static PdfObj make_bool(bool v) { PdfObj o; o.type = ObjType::BOOL; o.bool_val = v; return o; }
    static PdfObj make_arr() { PdfObj o; o.type = ObjType::ARRAY; return o; }
    static PdfObj make_dict() { PdfObj o; o.type = ObjType::DICT; return o; }
};

// ── PDF Lexer / Tokenizer ────────────────────────────────

struct PdfLexer {
    const uint8_t* data;
    size_t len;
    size_t pos;

    PdfLexer(const uint8_t* d, size_t l, size_t p = 0) : data(d), len(l), pos(p) {}

    int peek() const { return pos < len ? data[pos] : -1; }
    int get() { return pos < len ? data[pos++] : -1; }
    void skip_ws() {
        while (pos < len) {
            uint8_t c = data[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0' || c == '\f') { pos++; continue; }
            if (c == '%') { while (pos < len && data[pos] != '\r' && data[pos] != '\n') pos++; continue; }
            break;
        }
    }

    static bool is_delim(int c) {
        return c == '(' || c == ')' || c == '<' || c == '>' ||
               c == '[' || c == ']' || c == '{' || c == '}' ||
               c == '/' || c == '%';
    }
    static bool is_ws(int c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0' || c == '\f';
    }

    std::string read_token() {
        skip_ws();
        if (pos >= len) return "";
        std::string tok;
        uint8_t c = data[pos];
        if (c == '/' ) {
            pos++;
            while (pos < len && !is_ws(data[pos]) && !is_delim(data[pos]))
                tok += static_cast<char>(data[pos++]);
            return "/" + tok;
        }
        if (is_delim(c)) { pos++; return std::string(1, static_cast<char>(c)); }
        while (pos < len && !is_ws(data[pos]) && !is_delim(data[pos]))
            tok += static_cast<char>(data[pos++]);
        return tok;
    }

    std::string read_hex_string() {
        std::string hex;
        while (pos < len && data[pos] != '>') {
            uint8_t c = data[pos++];
            if (!is_ws(c)) hex += static_cast<char>(c);
        }
        if (pos < len) pos++; // skip >
        if (hex.size() & 1) hex += '0';
        std::string result;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            auto hex_val = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return 0;
            };
            result += static_cast<char>((hex_val(hex[i]) << 4) | hex_val(hex[i + 1]));
        }
        return result;
    }

    std::string read_literal_string() {
        std::string result;
        int depth = 1;
        while (pos < len && depth > 0) {
            uint8_t c = data[pos++];
            if (c == '(') { depth++; result += '('; }
            else if (c == ')') { depth--; if (depth > 0) result += ')'; }
            else if (c == '\\' && pos < len) {
                c = data[pos++];
                switch (c) {
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case '(': result += '(';  break;
                    case ')': result += ')';  break;
                    case '\\': result += '\\'; break;
                    case '\r':
                        if (pos < len && data[pos] == '\n') pos++;
                        break;
                    case '\n': break;
                    default:
                        if (c >= '0' && c <= '7') {
                            int oct = c - '0';
                            for (int i = 0; i < 2 && pos < len && data[pos] >= '0' && data[pos] <= '7'; i++)
                                oct = oct * 8 + (data[pos++] - '0');
                            result += static_cast<char>(oct);
                        } else {
                            result += static_cast<char>(c);
                        }
                }
            } else {
                result += static_cast<char>(c);
            }
        }
        return result;
    }

    PdfObj parse_object();
};

PdfObj PdfLexer::parse_object() {
    skip_ws();
    if (pos >= len) return {};

    uint8_t c = data[pos];

    if (c == '(') { pos++; return PdfObj::make_str(read_literal_string()); }

    if (c == '<') {
        pos++;
        if (pos < len && data[pos] == '<') {
            pos++;
            PdfObj obj; obj.type = ObjType::DICT;
            while (true) {
                skip_ws();
                if (pos + 1 < len && data[pos] == '>' && data[pos + 1] == '>') { pos += 2; break; }
                if (pos >= len) break;
                auto key_tok = read_token();
                if (key_tok.empty() || key_tok[0] != '/') break;
                std::string key = key_tok.substr(1);
                auto val = parse_object();
                obj.dict.push_back({key, std::move(val)});
            }
            return obj;
        }
        return PdfObj::make_str(read_hex_string());
    }

    if (c == '[') {
        pos++;
        PdfObj obj; obj.type = ObjType::ARRAY;
        while (true) {
            skip_ws();
            if (pos < len && data[pos] == ']') { pos++; break; }
            if (pos >= len) break;
            obj.arr.push_back(parse_object());
        }
        return obj;
    }

    if (c == '/') {
        auto tok = read_token();
        return PdfObj::make_name(tok.substr(1));
    }

    auto tok = read_token();
    if (tok.empty()) return {};

    if (tok == "true")  return PdfObj::make_bool(true);
    if (tok == "false") return PdfObj::make_bool(false);
    if (tok == "null")  return {};
    if (tok == "R")     return {}; // stray R

    // Check for "num gen R" pattern
    bool all_digit = true;
    bool has_dot = false;
    bool has_sign = false;
    for (size_t i = 0; i < tok.size(); i++) {
        char ch = tok[i];
        if (ch == '-' || ch == '+') { if (i == 0) has_sign = true; else { all_digit = false; break; } }
        else if (ch == '.') { has_dot = true; }
        else if (ch < '0' || ch > '9') { all_digit = false; break; }
    }

    if (all_digit || has_sign) {
        if (has_dot) {
            return PdfObj::make_real(std::strtod(tok.c_str(), nullptr));
        }
        int64_t num = std::strtoll(tok.c_str(), nullptr, 10);
        // Look ahead for "gen R"
        size_t saved = pos;
        skip_ws();
        size_t t2_start = pos;
        std::string tok2;
        while (pos < len && !is_ws(data[pos]) && !is_delim(data[pos]))
            tok2 += static_cast<char>(data[pos++]);
        if (!tok2.empty()) {
            bool t2_num = true;
            for (char ch : tok2) if (ch < '0' || ch > '9') { t2_num = false; break; }
            if (t2_num) {
                skip_ws();
                if (pos < len && data[pos] == 'R') {
                    pos++;
                    return PdfObj::make_ref(static_cast<int>(num), std::atoi(tok2.c_str()));
                }
            }
        }
        pos = saved;
        return PdfObj::make_int(num);
    }

    // Unknown token — return as name
    PdfObj o;
    o.type = ObjType::NAME;
    o.str_val = tok;
    return o;
}

// ── Stream Decoders ──────────────────────────────────────

std::vector<uint8_t> decode_flate(const uint8_t* src, size_t src_len) {
    std::vector<uint8_t> out;
    if (src_len == 0) return out;
    out.reserve(src_len * 3);

    z_stream zs = {};
    if (inflateInit(&zs) != Z_OK) return {};
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = static_cast<uInt>(src_len);

    uint8_t buf[8192];
    int ret;
    do {
        zs.next_out = buf;
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&zs);
            return out; // return partial
        }
        size_t have = sizeof(buf) - zs.avail_out;
        out.insert(out.end(), buf, buf + have);
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

std::vector<uint8_t> decode_ascii85(const uint8_t* src, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(len);
    size_t i = 0;
    while (i < len) {
        if (src[i] == '~' && i + 1 < len && src[i + 1] == '>') break;
        if (src[i] <= ' ') { i++; continue; }
        if (src[i] == 'z') { out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0); i++; continue; }
        uint32_t acc = 0;
        int cnt = 0;
        while (cnt < 5 && i < len) {
            if (src[i] == '~') break;
            if (src[i] <= ' ') { i++; continue; }
            acc = acc * 85 + (src[i] - 33);
            cnt++; i++;
        }
        if (cnt < 2) break;
        for (int j = cnt; j < 5; j++) acc = acc * 85 + 84;
        for (int j = 0; j < cnt - 1; j++)
            out.push_back(static_cast<uint8_t>((acc >> (24 - j * 8)) & 0xFF));
    }
    return out;
}

std::vector<uint8_t> decode_lzw(const uint8_t* src, size_t src_len, int early_change = 1) {
    std::vector<uint8_t> out;
    if (src_len == 0) return out;

    struct Entry { std::vector<uint8_t> data; };
    std::vector<Entry> table;
    auto reset_table = [&]() {
        table.clear();
        table.resize(258);
        for (int i = 0; i < 256; i++) table[i].data = {static_cast<uint8_t>(i)};
    };
    reset_table();

    int bits = 9;
    size_t bit_pos = 0;

    auto read_code = [&]() -> int {
        if (bit_pos + bits > src_len * 8) return -1;
        int code = 0;
        for (int b = 0; b < bits; b++) {
            size_t byte_idx = (bit_pos + b) / 8;
            int bit_idx = 7 - ((bit_pos + b) % 8);
            if (src[byte_idx] & (1 << bit_idx)) code |= (1 << (bits - 1 - b));
        }
        bit_pos += bits;
        return code;
    };

    int prev = -1;
    while (true) {
        int code = read_code();
        if (code < 0 || code == 257) break;
        if (code == 256) { reset_table(); bits = 9; prev = -1; continue; }

        if (code < (int)table.size() && !table[code].data.empty()) {
            out.insert(out.end(), table[code].data.begin(), table[code].data.end());
            if (prev >= 0 && prev < (int)table.size() && !table[prev].data.empty()) {
                Entry e;
                e.data = table[prev].data;
                e.data.push_back(table[code].data[0]);
                table.push_back(std::move(e));
            }
        } else if (prev >= 0 && prev < (int)table.size()) {
            Entry e;
            e.data = table[prev].data;
            e.data.push_back(e.data[0]);
            out.insert(out.end(), e.data.begin(), e.data.end());
            table.push_back(std::move(e));
        }
        prev = code;

        int sz = (int)table.size() + (early_change ? 0 : 1);
        if (sz >= (1 << bits) && bits < 12) bits++;
    }
    return out;
}

void apply_predictor(std::vector<uint8_t>& data, int predictor, int columns, int colors, int bpc) {
    if (predictor <= 1) return;
    int bytes_per_pixel = (colors * bpc + 7) / 8;
    int row_bytes = (columns * colors * bpc + 7) / 8;

    if (predictor == 2) {
        // TIFF predictor
        for (size_t i = 0; i + row_bytes <= data.size(); i += row_bytes) {
            for (int j = bytes_per_pixel; j < row_bytes; j++)
                data[i + j] += data[i + j - bytes_per_pixel];
        }
        return;
    }

    // PNG predictors (10-15)
    if (predictor >= 10) {
        int stride = 1 + row_bytes; // filter byte + row
        size_t n_rows = data.size() / stride;
        std::vector<uint8_t> out;
        out.reserve(n_rows * row_bytes);
        for (size_t r = 0; r < n_rows; r++) {
            uint8_t filter = data[r * stride];
            const uint8_t* row = data.data() + r * stride + 1;
            size_t prev_start = out.size() - row_bytes;
            for (int j = 0; j < row_bytes; j++) {
                uint8_t a = (j >= bytes_per_pixel) ? out[out.size() - bytes_per_pixel] : 0;
                uint8_t b = (r > 0) ? out[prev_start + j] : 0;
                uint8_t c_val = (r > 0 && j >= bytes_per_pixel) ? out[prev_start + j - bytes_per_pixel] : 0;
                uint8_t x = row[j];
                switch (filter) {
                    case 0: break;
                    case 1: x += a; break;
                    case 2: x += b; break;
                    case 3: x += (a + b) / 2; break;
                    case 4: {
                        int p = (int)a + (int)b - (int)c_val;
                        int pa = std::abs(p - (int)a);
                        int pb = std::abs(p - (int)b);
                        int pc = std::abs(p - (int)c_val);
                        x += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c_val);
                        break;
                    }
                }
                out.push_back(x);
            }
        }
        data = std::move(out);
    }
}

// ── PDF Document ─────────────────────────────────────────

struct XrefEntry {
    int64_t offset = 0;
    int gen = 0;
    bool in_use = false;
    // For compressed objects in object streams
    int stream_obj = -1;
    int stream_idx = -1;
};

struct PdfDoc {
    const uint8_t* data;
    size_t len;
    std::map<int, XrefEntry> xref;
    PdfObj trailer;
    std::map<int, PdfObj> obj_cache;

    PdfDoc(const uint8_t* d, size_t l) : data(d), len(l) {}

    bool parse();
    PdfObj resolve(const PdfObj& obj);
    PdfObj get_obj(int num);
    std::vector<uint8_t> decode_stream(const PdfObj& obj);

private:
    void parse_xref_table(size_t offset);
    void parse_xref_stream(size_t offset);
    int64_t find_startxref();
    void parse_obj_stream(int stream_num);
};

int64_t PdfDoc::find_startxref() {
    // Search last 1024 bytes for "startxref"
    size_t search_start = (len > 1024) ? len - 1024 : 0;
    for (size_t i = len; i > search_start; ) {
        i--;
        if (i + 9 <= len && std::memcmp(data + i, "startxref", 9) == 0) {
            PdfLexer lex(data, len, i + 9);
            lex.skip_ws();
            auto tok = lex.read_token();
            return std::strtoll(tok.c_str(), nullptr, 10);
        }
    }
    return -1;
}

void PdfDoc::parse_xref_table(size_t offset) {
    if (offset >= len) return;
    PdfLexer lex(data, len, offset);
    auto tok = lex.read_token();
    if (tok != "xref") {
        // Might be an xref stream object
        parse_xref_stream(offset);
        return;
    }

    while (true) {
        lex.skip_ws();
        if (lex.pos >= len) break;

        // Check for "trailer"
        size_t saved = lex.pos;
        auto t = lex.read_token();
        if (t == "trailer") break;
        lex.pos = saved;

        auto start_tok = lex.read_token();
        auto count_tok = lex.read_token();
        int start = std::atoi(start_tok.c_str());
        int count = std::atoi(count_tok.c_str());

        for (int i = 0; i < count && lex.pos < len; i++) {
            lex.skip_ws();
            auto off_tok = lex.read_token();
            auto gen_tok = lex.read_token();
            auto use_tok = lex.read_token();
            int obj_num = start + i;
            if (xref.find(obj_num) != xref.end()) continue; // keep first (newest)
            XrefEntry e;
            e.offset = std::strtoll(off_tok.c_str(), nullptr, 10);
            e.gen = std::atoi(gen_tok.c_str());
            e.in_use = (use_tok == "n");
            xref[obj_num] = e;
        }
    }

    // Parse trailer dict
    lex.skip_ws();
    PdfObj trl = lex.parse_object();
    if (trailer.is_none()) trailer = trl;

    // Follow /Prev chain
    auto& prev = trl.get("Prev");
    if (prev.is_int()) {
        parse_xref_table(static_cast<size_t>(prev.int_val));
    }
}

void PdfDoc::parse_xref_stream(size_t offset) {
    if (offset >= len) return;
    PdfLexer lex(data, len, offset);

    // "num gen obj"
    auto num_tok = lex.read_token();
    lex.read_token(); // gen
    auto obj_tok = lex.read_token();
    if (obj_tok != "obj") return;

    PdfObj stream_dict = lex.parse_object();
    if (!stream_dict.is_dict()) return;

    // Read stream data
    lex.skip_ws();
    size_t saved = lex.pos;
    auto s_tok = lex.read_token();
    if (s_tok != "stream") { lex.pos = saved; return; }
    if (lex.pos < len && data[lex.pos] == '\r') lex.pos++;
    if (lex.pos < len && data[lex.pos] == '\n') lex.pos++;

    int64_t slen = stream_dict.get("Length").as_int();
    if (slen <= 0 || lex.pos + slen > (int64_t)len) return;

    stream_dict.type = ObjType::STREAM;
    stream_dict.stream_ptr = data + lex.pos;
    stream_dict.stream_len = static_cast<size_t>(slen);

    if (trailer.is_none()) {
        trailer = PdfObj::make_dict();
        for (auto& [k, v] : stream_dict.dict)
            trailer.dict.push_back({k, v});
    }

    auto decoded = decode_stream(stream_dict);
    if (decoded.empty()) return;

    // Parse W array
    auto& w_arr = stream_dict.get("W");
    if (!w_arr.is_arr() || w_arr.arr.size() < 3) return;
    int w0 = w_arr.arr[0].as_int();
    int w1 = w_arr.arr[1].as_int();
    int w2 = w_arr.arr[2].as_int();
    int entry_size = w0 + w1 + w2;
    if (entry_size <= 0) return;

    // Parse Index array (default: [0 Size])
    std::vector<std::pair<int, int>> index_pairs;
    auto& idx = stream_dict.get("Index");
    if (idx.is_arr() && idx.arr.size() >= 2) {
        for (size_t i = 0; i + 1 < idx.arr.size(); i += 2)
            index_pairs.push_back({idx.arr[i].as_int(), idx.arr[i + 1].as_int()});
    } else {
        index_pairs.push_back({0, stream_dict.get("Size").as_int()});
    }

    auto read_field = [&](const uint8_t* p, int width) -> int64_t {
        int64_t val = 0;
        for (int b = 0; b < width; b++)
            val = (val << 8) | p[b];
        return val;
    };

    size_t dpos = 0;
    for (auto& [start, count] : index_pairs) {
        for (int i = 0; i < count && dpos + entry_size <= decoded.size(); i++) {
            const uint8_t* ep = decoded.data() + dpos;
            int64_t type_val = (w0 > 0) ? read_field(ep, w0) : 1;
            int64_t field1 = (w1 > 0) ? read_field(ep + w0, w1) : 0;
            int64_t field2 = (w2 > 0) ? read_field(ep + w0 + w1, w2) : 0;
            dpos += entry_size;

            int obj_num = start + i;
            if (xref.find(obj_num) != xref.end()) continue;

            XrefEntry e;
            if (type_val == 0) {
                e.in_use = false;
            } else if (type_val == 1) {
                e.offset = field1;
                e.gen = static_cast<int>(field2);
                e.in_use = true;
            } else if (type_val == 2) {
                e.stream_obj = static_cast<int>(field1);
                e.stream_idx = static_cast<int>(field2);
                e.in_use = true;
            }
            xref[obj_num] = e;
        }
    }

    auto& prev = stream_dict.get("Prev");
    if (prev.is_int()) {
        size_t prev_off = static_cast<size_t>(prev.int_val);
        if (prev_off < len) parse_xref_stream(prev_off);
    }
}

bool PdfDoc::parse() {
    if (len < 10) return false;

    int64_t startxref = find_startxref();
    if (startxref < 0 || startxref >= (int64_t)len) return false;

    // Detect if xref is a table or a stream
    PdfLexer probe(data, len, static_cast<size_t>(startxref));
    probe.skip_ws();
    size_t probe_start = probe.pos;
    std::string first_tok;
    while (probe.pos < len && !PdfLexer::is_ws(data[probe.pos]) && !PdfLexer::is_delim(data[probe.pos])) {
        first_tok += static_cast<char>(data[probe.pos++]);
    }

    if (first_tok == "xref") {
        parse_xref_table(static_cast<size_t>(startxref));
    } else {
        parse_xref_stream(static_cast<size_t>(startxref));
    }

    return !xref.empty();
}

PdfObj PdfDoc::get_obj(int num) {
    static const PdfObj empty;
    auto cached = obj_cache.find(num);
    if (cached != obj_cache.end()) return cached->second;

    auto it = xref.find(num);
    if (it == xref.end() || !it->second.in_use) return empty;

    auto& entry = it->second;

    // Compressed object in object stream
    if (entry.stream_obj >= 0) {
        parse_obj_stream(entry.stream_obj);
        auto cached2 = obj_cache.find(num);
        if (cached2 != obj_cache.end()) return cached2->second;
        return empty;
    }

    if (entry.offset <= 0 || entry.offset >= (int64_t)len) return empty;

    PdfLexer lex(data, len, static_cast<size_t>(entry.offset));
    lex.read_token(); // obj num
    lex.read_token(); // gen num
    auto obj_kw = lex.read_token(); // "obj"
    if (obj_kw != "obj") return empty;

    PdfObj obj = lex.parse_object();

    // Check for stream
    lex.skip_ws();
    size_t saved = lex.pos;
    std::string maybe_stream;
    while (lex.pos < len && lex.pos - saved < 10 && !PdfLexer::is_ws(data[lex.pos]) && !PdfLexer::is_delim(data[lex.pos]))
        maybe_stream += static_cast<char>(data[lex.pos++]);

    if (maybe_stream == "stream" && obj.is_dict()) {
        if (lex.pos < len && data[lex.pos] == '\r') lex.pos++;
        if (lex.pos < len && data[lex.pos] == '\n') lex.pos++;

        int64_t slen = resolve(obj.get("Length")).as_int();
        if (slen > 0 && lex.pos + slen <= (int64_t)len) {
            obj.type = ObjType::STREAM;
            obj.stream_ptr = data + lex.pos;
            obj.stream_len = static_cast<size_t>(slen);
        }
    }

    obj_cache[num] = std::move(obj);
    return obj_cache[num];
}

void PdfDoc::parse_obj_stream(int stream_num) {
    PdfObj stm_obj = get_obj(stream_num);
    if (!stm_obj.is_stream()) return;

    auto decoded = decode_stream(stm_obj);
    if (decoded.empty()) return;

    int n = stm_obj.get("N").as_int();
    int first = stm_obj.get("First").as_int();
    if (n <= 0 || first <= 0) return;

    // Parse header: pairs of (obj_num, offset_within_stream)
    PdfLexer hdr(decoded.data(), decoded.size());
    std::vector<std::pair<int, int>> entries;
    for (int i = 0; i < n; i++) {
        auto num_tok = hdr.read_token();
        auto off_tok = hdr.read_token();
        entries.push_back({std::atoi(num_tok.c_str()), std::atoi(off_tok.c_str())});
    }

    for (auto& [obj_num, obj_off] : entries) {
        if (obj_cache.find(obj_num) != obj_cache.end()) continue;
        size_t abs_off = static_cast<size_t>(first + obj_off);
        if (abs_off >= decoded.size()) continue;
        PdfLexer olex(decoded.data(), decoded.size(), abs_off);
        obj_cache[obj_num] = olex.parse_object();
    }
}

PdfObj PdfDoc::resolve(const PdfObj& obj) {
    if (obj.is_ref()) return get_obj(obj.ref_num);
    return obj;
}

std::vector<uint8_t> PdfDoc::decode_stream(const PdfObj& obj) {
    if (!obj.is_stream()) return {};
    // Use zero-copy pointer if available, otherwise use stored data
    std::vector<uint8_t> result;
    if (obj.stream_ptr && obj.stream_len > 0) {
        result.assign(obj.stream_ptr, obj.stream_ptr + obj.stream_len);
    } else {
        result = obj.stream_data;
    }

    auto filter_obj = resolve(obj.get("Filter"));
    auto parms_obj = resolve(obj.get("DecodeParms"));

    std::vector<std::string> filters;
    std::vector<PdfObj> parms_list;

    if (filter_obj.is_name()) {
        filters.push_back(filter_obj.str_val);
        parms_list.push_back(parms_obj);
    } else if (filter_obj.is_arr()) {
        for (auto& f : filter_obj.arr) {
            auto rf = resolve(f);
            filters.push_back(rf.str_val);
        }
        if (parms_obj.is_arr()) {
            for (auto& p : parms_obj.arr) parms_list.push_back(resolve(p));
        }
    }
    while (parms_list.size() < filters.size()) parms_list.push_back({});

    for (size_t fi = 0; fi < filters.size(); fi++) {
        auto& fname = filters[fi];
        auto& fparm = parms_list[fi];

        if (fname == "FlateDecode") {
            result = decode_flate(result.data(), result.size());
            int pred = fparm.get("Predictor").as_int();
            if (pred > 1) {
                int cols = fparm.get("Columns").as_int();
                int colors_v = fparm.get("Colors").as_int();
                int bpc = fparm.get("BitsPerComponent").as_int();
                if (cols <= 0) cols = 1;
                if (colors_v <= 0) colors_v = 1;
                if (bpc <= 0) bpc = 8;
                apply_predictor(result, pred, cols, colors_v, bpc);
            }
        } else if (fname == "ASCII85Decode") {
            result = decode_ascii85(result.data(), result.size());
        } else if (fname == "LZWDecode") {
            int early = 1;
            if (fparm.has("EarlyChange")) early = fparm.get("EarlyChange").as_int();
            result = decode_lzw(result.data(), result.size(), early);
            int pred = fparm.get("Predictor").as_int();
            if (pred > 1) {
                int cols = fparm.get("Columns").as_int();
                int colors_v = fparm.get("Colors").as_int();
                int bpc = fparm.get("BitsPerComponent").as_int();
                if (cols <= 0) cols = 1;
                if (colors_v <= 0) colors_v = 1;
                if (bpc <= 0) bpc = 8;
                apply_predictor(result, pred, cols, colors_v, bpc);
            }
        } else if (fname == "ASCIIHexDecode") {
            std::vector<uint8_t> out;
            int hi = -1;
            for (uint8_t c : result) {
                if (c == '>') break;
                int nibble = -1;
                if (c >= '0' && c <= '9') nibble = c - '0';
                else if (c >= 'a' && c <= 'f') nibble = 10 + c - 'a';
                else if (c >= 'A' && c <= 'F') nibble = 10 + c - 'A';
                if (nibble < 0) continue;
                if (hi < 0) { hi = nibble; } else { out.push_back(static_cast<uint8_t>((hi << 4) | nibble)); hi = -1; }
            }
            if (hi >= 0) out.push_back(static_cast<uint8_t>(hi << 4));
            result = std::move(out);
        }
        // DCTDecode, CCITTFaxDecode: leave raw data for caller to handle
    }
    return result;
}

// ── Font / Encoding ──────────────────────────────────────

// Standard glyph name → Unicode mapping (commonly used subset)
static uint32_t glyph_name_to_unicode(const std::string& name) {
    static const std::unordered_map<std::string, uint32_t> table = {
        {"space", 0x20}, {"exclam", 0x21}, {"quotedbl", 0x22}, {"numbersign", 0x23},
        {"dollar", 0x24}, {"percent", 0x25}, {"ampersand", 0x26}, {"quotesingle", 0x27},
        {"parenleft", 0x28}, {"parenright", 0x29}, {"asterisk", 0x2A}, {"plus", 0x2B},
        {"comma", 0x2C}, {"hyphen", 0x2D}, {"period", 0x2E}, {"slash", 0x2F},
        {"zero", 0x30}, {"one", 0x31}, {"two", 0x32}, {"three", 0x33},
        {"four", 0x34}, {"five", 0x35}, {"six", 0x36}, {"seven", 0x37},
        {"eight", 0x38}, {"nine", 0x39}, {"colon", 0x3A}, {"semicolon", 0x3B},
        {"less", 0x3C}, {"equal", 0x3D}, {"greater", 0x3E}, {"question", 0x3F},
        {"at", 0x40}, {"bracketleft", 0x5B}, {"backslash", 0x5C}, {"bracketright", 0x5D},
        {"asciicircum", 0x5E}, {"underscore", 0x5F}, {"grave", 0x60},
        {"braceleft", 0x7B}, {"bar", 0x7C}, {"braceright", 0x7D}, {"asciitilde", 0x7E},
        {"bullet", 0x2022}, {"endash", 0x2013}, {"emdash", 0x2014},
        {"quoteleft", 0x2018}, {"quoteright", 0x2019},
        {"quotedblleft", 0x201C}, {"quotedblright", 0x201D},
        {"fi", 0xFB01}, {"fl", 0xFB02}, {"ff", 0xFB00},
        {"ffi", 0xFB03}, {"ffl", 0xFB04},
        {"ellipsis", 0x2026}, {"degree", 0x00B0}, {"copyright", 0x00A9},
        {"registered", 0x00AE}, {"trademark", 0x2122}, {"section", 0x00A7},
        {"paragraph", 0x00B6}, {"dagger", 0x2020}, {"daggerdbl", 0x2021},
        {"guillemotleft", 0x00AB}, {"guillemotright", 0x00BB},
        {"Euro", 0x20AC}, {"mu", 0x03BC}, {"multiply", 0x00D7}, {"divide", 0x00F7},
        {"minus", 0x2212}, {"plusminus", 0x00B1}, {"infinity", 0x221E},
        {"notequal", 0x2260}, {"lessequal", 0x2264}, {"greaterequal", 0x2265},
        {"approxequal", 0x2248}, {"summation", 0x2211}, {"product", 0x220F},
        {"radical", 0x221A}, {"integral", 0x222B},
        {"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acircumflex", 0xC2}, {"Atilde", 0xC3},
        {"Adieresis", 0xC4}, {"Aring", 0xC5}, {"AE", 0xC6}, {"Ccedilla", 0xC7},
        {"Egrave", 0xC8}, {"Eacute", 0xC9}, {"Ecircumflex", 0xCA}, {"Edieresis", 0xCB},
        {"Igrave", 0xCC}, {"Iacute", 0xCD}, {"Icircumflex", 0xCE}, {"Idieresis", 0xCF},
        {"Eth", 0xD0}, {"Ntilde", 0xD1}, {"Ograve", 0xD2}, {"Oacute", 0xD3},
        {"Ocircumflex", 0xD4}, {"Otilde", 0xD5}, {"Odieresis", 0xD6},
        {"Oslash", 0xD8}, {"Ugrave", 0xD9}, {"Uacute", 0xDA},
        {"Ucircumflex", 0xDB}, {"Udieresis", 0xDC}, {"Yacute", 0xDD}, {"Thorn", 0xDE},
        {"germandbls", 0xDF}, {"agrave", 0xE0}, {"aacute", 0xE1}, {"acircumflex", 0xE2},
        {"atilde", 0xE3}, {"adieresis", 0xE4}, {"aring", 0xE5}, {"ae", 0xE6},
        {"ccedilla", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9}, {"ecircumflex", 0xEA},
        {"edieresis", 0xEB}, {"igrave", 0xEC}, {"iacute", 0xED}, {"icircumflex", 0xEE},
        {"idieresis", 0xEF}, {"eth", 0xF0}, {"ntilde", 0xF1}, {"ograve", 0xF2},
        {"oacute", 0xF3}, {"ocircumflex", 0xF4}, {"otilde", 0xF5}, {"odieresis", 0xF6},
        {"oslash", 0xF8}, {"ugrave", 0xF9}, {"uacute", 0xFA}, {"ucircumflex", 0xFB},
        {"udieresis", 0xFC}, {"yacute", 0xFD}, {"thorn", 0xFE}, {"ydieresis", 0xFF},
        {"nbspace", 0xA0}, {"exclamdown", 0xA1}, {"cent", 0xA2}, {"sterling", 0xA3},
        {"currency", 0xA4}, {"yen", 0xA5}, {"brokenbar", 0xA6},
        {"ordfeminine", 0xAA}, {"ordmasculine", 0xBA},
        {"onequarter", 0xBC}, {"onehalf", 0xBD}, {"threequarters", 0xBE},
        {"questiondown", 0xBF},
        {"Lslash", 0x141}, {"lslash", 0x142}, {"OE", 0x152}, {"oe", 0x153},
        {"Scaron", 0x160}, {"scaron", 0x161}, {"Zcaron", 0x17D}, {"zcaron", 0x17E},
        {"circumflex", 0x2C6}, {"tilde", 0x2DC}, {"caron", 0x2C7},
        {"dotaccent", 0x2D9}, {"ring", 0x2DA}, {"ogonek", 0x2DB},
        {"hungarumlaut", 0x2DD}, {"cedilla", 0xB8}, {"acute", 0xB4},
        {"dieresis", 0xA8}, {"macron", 0xAF}, {"breve", 0x2D8},
    };
    // Single letter names: A-Z, a-z
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z')))
        return static_cast<uint32_t>(name[0]);
    auto it = table.find(name);
    return (it != table.end()) ? it->second : 0;
}

// WinAnsiEncoding (PDF spec, Appendix D)
static const uint32_t kWinAnsi[256] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
    64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,
    96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,
    0x20AC,0x2022,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0x2022,0x017D,0x2022,
    0x2022,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0x2022,0x017E,0x0178,
    0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
    0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
    0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,
    0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF,
};

// MacRomanEncoding (subset of differences from ASCII)
static const uint32_t kMacRoman[256] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
    64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,
    96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,
    0xC4,0xC5,0xC7,0xC9,0xD1,0xD6,0xDC,0xE1,0xE0,0xE2,0xE4,0xE3,0xE5,0xE7,0xE9,0xE8,
    0xEA,0xEB,0xED,0xEC,0xEE,0xEF,0xF1,0xF3,0xF2,0xF4,0xF6,0xF5,0xFA,0xF9,0xFB,0xFC,
    0x2020,0xB0,0xA2,0xA3,0xA7,0x2022,0xB6,0xDF,0xAE,0xA9,0x2122,0xB4,0xA8,0x2260,0xC6,0xD8,
    0x221E,0xB1,0x2264,0x2265,0xA5,0xB5,0x2202,0x2211,0x220F,0x3C0,0x222B,0xAA,0xBA,0x2126,0xE6,0xF8,
    0xBF,0xA1,0xAC,0x221A,0x192,0x2248,0x2206,0xAB,0xBB,0x2026,0xA0,0xC0,0xC3,0xD5,0x152,0x153,
    0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0xF7,0x25CA,0xFF,0x178,0x2044,0x20AC,0x2039,0x203A,0xFB01,0xFB02,
    0x2021,0xB7,0x201A,0x201E,0x2030,0xC2,0xCA,0xC1,0xCB,0xC8,0xCD,0xCE,0xCF,0xCC,0xD3,0xD4,
    0xF8FF,0xD2,0xDA,0xDB,0xD9,0x131,0x2C6,0x2DC,0xAF,0x2D8,0x2D9,0x2DA,0xB8,0x2DD,0x2DB,0x2C7,
};

struct PdfFont {
    std::string name;
    bool is_bold = false;
    bool is_italic = false;
    bool is_identity = false;    // Identity-H/V (CID font)
    bool is_type0 = false;       // Type0 composite font
    std::unordered_map<uint32_t, uint32_t> to_unicode;  // char code → Unicode
    std::unordered_map<uint32_t, uint32_t> cid_to_unicode; // CID → Unicode from ToUnicode
    const uint32_t* encoding_table = nullptr; // WinAnsi, MacRoman, etc.
    std::unordered_map<int, std::string> differences; // /Differences array
    std::unordered_map<uint32_t, double> widths; // char code → width in 1/1000 of text space
    double default_width = 1000; // default glyph width in 1/1000 units
    double missing_width = 0;    // /MissingWidth from FontDescriptor
    int cmap_code_bytes = 0;     // 0=auto, 1 or 2 from codespacerange

    double get_width(uint32_t code) const {
        auto it = widths.find(code);
        if (it != widths.end()) return it->second;
        if (missing_width > 0) return missing_width;
        if (is_identity || is_type0) return default_width;
        return 0; // unknown — let caller use default
    }

    uint32_t decode_char(uint32_t code) const {
        // 1. ToUnicode CMap lookup
        auto tu = to_unicode.find(code);
        if (tu != to_unicode.end()) return tu->second;

        // 2. CID fonts (Identity-H/V)
        if (is_identity || is_type0) {
            auto cu = cid_to_unicode.find(code);
            if (cu != cid_to_unicode.end()) return cu->second;
            return code; // fallback: assume code is Unicode
        }

        // 3. Differences array
        auto diff = differences.find(static_cast<int>(code));
        if (diff != differences.end()) {
            uint32_t u = glyph_name_to_unicode(diff->second);
            if (u) return u;
        }

        // 4. Encoding table
        if (encoding_table && code < 256)
            return encoding_table[code];

        // 5. Fallback
        return (code < 128) ? code : 0xFFFD;
    }
};

void parse_tounicode_cmap(PdfDoc& doc, const PdfObj& tu_obj, PdfFont& font) {
    auto resolved = doc.resolve(tu_obj);
    std::vector<uint8_t> cmap_data;
    if (resolved.is_stream()) {
        cmap_data = doc.decode_stream(resolved);
    } else if (resolved.is_str()) {
        cmap_data.assign(resolved.str_val.begin(), resolved.str_val.end());
    }
    if (cmap_data.empty()) return;

    std::string cmap(cmap_data.begin(), cmap_data.end());

    // Detect codespace range to determine byte width
    auto csr = cmap.find("begincodespacerange");
    if (csr != std::string::npos) {
        auto csr_end = cmap.find("endcodespacerange", csr);
        if (csr_end != std::string::npos) {
            auto s1 = cmap.find('<', csr + 19);
            auto e1 = cmap.find('>', s1);
            if (s1 < csr_end && e1 < csr_end) {
                int hex_len = 0;
                for (size_t ci = s1 + 1; ci < e1; ci++) {
                    char ch = cmap[ci];
                    if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))
                        hex_len++;
                }
                font.cmap_code_bytes = (hex_len <= 2) ? 1 : 2;
            }
        }
    }

    auto parse_hex = [](const std::string& s) -> uint32_t {
        uint32_t val = 0;
        for (char c : s) {
            val <<= 4;
            if (c >= '0' && c <= '9') val |= c - '0';
            else if (c >= 'a' && c <= 'f') val |= 10 + c - 'a';
            else if (c >= 'A' && c <= 'F') val |= 10 + c - 'A';
        }
        return val;
    };

    auto extract_hex = [](const std::string& s, size_t start, size_t end) -> std::string {
        std::string h;
        for (size_t i = start; i < end; i++) {
            char c = s[i];
            if (c != '<' && c != '>' && c != ' ' && c != '\t' && c != '\r' && c != '\n')
                h += c;
        }
        return h;
    };

    // Parse bfchar sections
    size_t p = 0;
    while (p < cmap.size()) {
        auto bfc = cmap.find("beginbfchar", p);
        if (bfc == std::string::npos) break;
        auto ebc = cmap.find("endbfchar", bfc);
        if (ebc == std::string::npos) break;

        size_t cp = bfc + 11;
        while (cp < ebc) {
            auto s1 = cmap.find('<', cp);
            if (s1 >= ebc) break;
            auto e1 = cmap.find('>', s1);
            if (e1 >= ebc) break;
            auto s2 = cmap.find('<', e1);
            if (s2 >= ebc) break;
            auto e2 = cmap.find('>', s2);
            if (e2 >= ebc) break;

            std::string src_hex = extract_hex(cmap, s1 + 1, e1);
            std::string dst_hex = extract_hex(cmap, s2 + 1, e2);

            uint32_t src = parse_hex(src_hex);
            if (dst_hex.size() <= 4) {
                font.to_unicode[src] = parse_hex(dst_hex);
            } else {
                // Multi-byte: decode as UTF-16BE sequence, store first char
                uint32_t u = parse_hex(dst_hex.substr(0, 4));
                font.to_unicode[src] = u;
            }
            cp = e2 + 1;
        }
        p = ebc + 9;
    }

    // Parse bfrange sections
    p = 0;
    while (p < cmap.size()) {
        auto bfr = cmap.find("beginbfrange", p);
        if (bfr == std::string::npos) break;
        auto ebr = cmap.find("endbfrange", bfr);
        if (ebr == std::string::npos) break;

        size_t cp = bfr + 12;
        while (cp < ebr) {
            auto s1 = cmap.find('<', cp);
            if (s1 >= ebr) break;
            auto e1 = cmap.find('>', s1);
            if (e1 >= ebr) break;
            auto s2 = cmap.find('<', e1);
            if (s2 >= ebr) break;
            auto e2 = cmap.find('>', s2);
            if (e2 >= ebr) break;

            std::string lo_hex = extract_hex(cmap, s1 + 1, e1);
            std::string hi_hex = extract_hex(cmap, s2 + 1, e2);
            uint32_t lo = parse_hex(lo_hex);
            uint32_t hi = parse_hex(hi_hex);

            // Next element: either <hex> or [<hex> <hex> ...]
            size_t next = e2 + 1;
            while (next < ebr && (cmap[next] == ' ' || cmap[next] == '\t' || cmap[next] == '\r' || cmap[next] == '\n')) next++;

            if (next < ebr && cmap[next] == '[') {
                // Array of destination values
                size_t arr_end = cmap.find(']', next);
                if (arr_end == std::string::npos) arr_end = ebr;
                size_t ap = next + 1;
                uint32_t code = lo;
                while (ap < arr_end && code <= hi) {
                    auto as = cmap.find('<', ap);
                    if (as >= arr_end) break;
                    auto ae = cmap.find('>', as);
                    if (ae >= arr_end) break;
                    std::string dh = extract_hex(cmap, as + 1, ae);
                    font.to_unicode[code] = parse_hex(dh);
                    code++;
                    ap = ae + 1;
                }
                cp = (arr_end < ebr) ? arr_end + 1 : ebr;
            } else {
                auto s3 = cmap.find('<', next);
                if (s3 >= ebr) break;
                auto e3 = cmap.find('>', s3);
                if (e3 >= ebr) break;
                std::string dst_hex = extract_hex(cmap, s3 + 1, e3);
                uint32_t dst = parse_hex(dst_hex);
                for (uint32_t code = lo; code <= hi; code++)
                    font.to_unicode[code] = dst + (code - lo);
                cp = e3 + 1;
            }
        }
        p = ebr + 10;
    }
}

PdfFont load_font(PdfDoc& doc, const PdfObj& font_ref) {
    PdfFont font;
    auto fobj = doc.resolve(font_ref);
    if (!fobj.is_dict()) return font;

    // Font name
    auto& base_font = fobj.get("BaseFont");
    if (base_font.is_name()) font.name = base_font.str_val;

    // Detect bold/italic from name
    {
        std::string lower;
        for (char c : font.name) lower += std::tolower(static_cast<unsigned char>(c));
        font.is_bold = lower.find("bold") != std::string::npos ||
                       lower.find("heavy") != std::string::npos ||
                       lower.find("black") != std::string::npos;
        font.is_italic = lower.find("italic") != std::string::npos ||
                         lower.find("oblique") != std::string::npos;
    }

    // Also check FontDescriptor flags
    auto desc = doc.resolve(fobj.get("FontDescriptor"));
    if (desc.is_dict()) {
        int flags = desc.get("Flags").as_int();
        if (flags & (1 << 18)) font.is_bold = true;   // bit 19 (0-indexed 18)
        if (flags & (1 << 6))  font.is_italic = true;  // bit 7 (Italic)
        double mw = desc.get("MissingWidth").as_num();
        if (mw > 0) font.missing_width = mw;
    }

    // Font type
    auto& subtype = fobj.get("Subtype");
    std::string font_type = subtype.is_name() ? subtype.str_val : "";

    // Parse /Widths array for simple fonts (Type1, TrueType)
    auto& widths_arr = fobj.get("Widths");
    if (widths_arr.is_arr()) {
        int first_char = fobj.get("FirstChar").as_int();
        for (size_t i = 0; i < widths_arr.arr.size(); i++) {
            double w = widths_arr.arr[i].as_num();
            font.widths[static_cast<uint32_t>(first_char + i)] = w;
        }
    }

    if (font_type == "Type0") {
        font.is_type0 = true;
        // Check encoding
        auto& enc = fobj.get("Encoding");
        if (enc.is_name()) {
            if (enc.str_val == "Identity-H" || enc.str_val == "Identity-V")
                font.is_identity = true;
        }
        // CIDFont descendant — parse /W (widths) and /DW (default width)
        auto& descendants = fobj.get("DescendantFonts");
        if (descendants.is_arr() && !descendants.arr.empty()) {
            auto cid_font = doc.resolve(descendants.arr[0]);
            if (cid_font.is_dict()) {
                double dw = cid_font.get("DW").as_num();
                if (dw > 0) font.default_width = dw;

                // /W array: [cid [w1 w2 ...] cid_start cid_end w ...]
                auto w_arr = doc.resolve(cid_font.get("W"));
                if (w_arr.is_arr()) {
                    size_t wi = 0;
                    while (wi < w_arr.arr.size()) {
                        if (!w_arr.arr[wi].is_int()) { wi++; continue; }
                        int cid_start = w_arr.arr[wi].as_int();
                        wi++;
                        if (wi >= w_arr.arr.size()) break;
                        if (w_arr.arr[wi].is_arr()) {
                            // [cid [w1 w2 ...]]
                            auto& warr = w_arr.arr[wi].arr;
                            for (size_t j = 0; j < warr.size(); j++)
                                font.widths[static_cast<uint32_t>(cid_start + j)] = warr[j].as_num();
                            wi++;
                        } else if (w_arr.arr[wi].is_int() && wi + 1 < w_arr.arr.size()) {
                            // [cid_start cid_end w]
                            int cid_end = w_arr.arr[wi].as_int();
                            wi++;
                            double w = w_arr.arr[wi].as_num();
                            for (int cid = cid_start; cid <= cid_end; cid++)
                                font.widths[static_cast<uint32_t>(cid)] = w;
                            wi++;
                        } else {
                            wi++;
                        }
                    }
                }

                // Also parse CID FontDescriptor for MissingWidth
                auto cid_desc = doc.resolve(cid_font.get("FontDescriptor"));
                if (cid_desc.is_dict()) {
                    double mw = cid_desc.get("MissingWidth").as_num();
                    if (mw > 0) font.missing_width = mw;
                }
            }
        }
    }

    // ToUnicode CMap
    auto& tu = fobj.get("ToUnicode");
    if (!tu.is_none()) parse_tounicode_cmap(doc, tu, font);

    // Encoding
    auto& enc_obj = fobj.get("Encoding");
    if (enc_obj.is_name()) {
        if (enc_obj.str_val == "WinAnsiEncoding") font.encoding_table = kWinAnsi;
        else if (enc_obj.str_val == "MacRomanEncoding") font.encoding_table = kMacRoman;
    } else if (enc_obj.is_dict()) {
        auto& base_enc = enc_obj.get("BaseEncoding");
        if (base_enc.is_name()) {
            if (base_enc.str_val == "WinAnsiEncoding") font.encoding_table = kWinAnsi;
            else if (base_enc.str_val == "MacRomanEncoding") font.encoding_table = kMacRoman;
        }
        // Differences array
        auto& diffs = enc_obj.get("Differences");
        if (diffs.is_arr()) {
            int code = 0;
            for (auto& item : diffs.arr) {
                if (item.is_int()) { code = item.as_int(); }
                else if (item.is_name()) { font.differences[code++] = item.str_val; }
            }
        }
    }

    // Default encoding if nothing set
    if (!font.encoding_table && font.to_unicode.empty() && !font.is_identity) {
        font.encoding_table = kWinAnsi;
    }

    return font;
}

// ── Content Stream Text Extraction ───────────────────────

struct TextChar {
    double x, y;
    double left, right, top, bot;
    double font_size;
    uint32_t unicode;
    bool is_bold;
    bool is_italic;
};

struct GfxState {
    double ctm[6] = {1, 0, 0, 1, 0, 0};  // a b c d e f
    double text_mat[6] = {1, 0, 0, 1, 0, 0};
    double line_mat[6] = {1, 0, 0, 1, 0, 0};
    double font_size = 12;
    double word_spacing = 0;
    double char_spacing = 0;
    double h_scaling = 100;
    double text_rise = 0;
    double text_leading = 0;
    PdfFont* font = nullptr;

    // Graphics state for paths
    double stroke_r = 0, stroke_g = 0, stroke_b = 0;
    double fill_r = 0, fill_g = 0, fill_b = 0;
    double line_width = 1;
    int line_cap = 0, line_join = 0;
    double miter_limit = 10;
};

static void mat_multiply(double* out, const double* a, const double* b) {
    double r[6];
    r[0] = a[0]*b[0] + a[1]*b[2];
    r[1] = a[0]*b[1] + a[1]*b[3];
    r[2] = a[2]*b[0] + a[3]*b[2];
    r[3] = a[2]*b[1] + a[3]*b[3];
    r[4] = a[4]*b[0] + a[5]*b[2] + b[4];
    r[5] = a[4]*b[1] + a[5]*b[3] + b[5];
    std::memcpy(out, r, sizeof(r));
}

static void transform_point(const double* m, double x, double y, double& ox, double& oy) {
    ox = m[0]*x + m[2]*y + m[4];
    oy = m[1]*x + m[3]*y + m[5];
}

struct PathPoint {
    double x, y;
    enum Type { MOVE, LINE, CURVE, CLOSE } type;
    double cx1, cy1, cx2, cy2; // for CURVE
};

struct PdfLineSegment {
    float x0, y0, x1, y1;
    bool is_horizontal() const { return std::abs(y1 - y0) < 2.0f; }
    bool is_vertical()   const { return std::abs(x1 - x0) < 2.0f; }
};

struct ImagePlacement {
    int xobj_ref = -1;
    std::string xobj_name;
    double ctm[6];
    double fill_r = 0, fill_g = 0, fill_b = 0; // fill color for ImageMask
};

struct RenderPath {
    std::vector<PathPoint> points;
    double fill_r, fill_g, fill_b;
    double stroke_r, stroke_g, stroke_b;
    double line_width;
    bool do_fill, do_stroke;
};

struct ContentParseResult {
    std::vector<TextChar> chars;
    std::vector<PdfLineSegment> segments;
    std::vector<ImagePlacement> images;
    std::vector<RenderPath> paths; // for vector rendering
};

ContentParseResult parse_content_stream(PdfDoc& doc, const std::vector<uint8_t>& stream,
                                         const PdfObj& resources, double page_height) {
    ContentParseResult result;

    // Load fonts from resources
    std::unordered_map<std::string, PdfFont> fonts;
    auto res = doc.resolve(resources);
    auto& font_dict = res.get("Font");
    if (!font_dict.is_none()) {
        auto fd = doc.resolve(font_dict);
        if (fd.is_dict()) {
            for (auto& [name, ref] : fd.dict)
                fonts[name] = load_font(doc, ref);
        }
    }

    std::vector<GfxState> state_stack;
    GfxState gs;
    std::vector<PathPoint> current_path;

    PdfLexer lex(stream.data(), stream.size());
    std::vector<PdfObj> operands;

    auto pop_num = [&](int idx_from_end = 0) -> double {
        int i = static_cast<int>(operands.size()) - 1 - idx_from_end;
        if (i < 0) return 0;
        return operands[i].as_num();
    };

    auto flush_path_segments = [&]() {
        // Extract line segments from path
        double px = 0, py = 0;
        bool has_move = false;
        double move_x = 0, move_y = 0;

        for (auto& pt : current_path) {
            switch (pt.type) {
                case PathPoint::MOVE:
                    px = pt.x; py = pt.y;
                    move_x = px; move_y = py;
                    has_move = true;
                    break;
                case PathPoint::LINE: {
                    PdfLineSegment seg;
                    seg.x0 = static_cast<float>(px);
                    seg.y0 = static_cast<float>(py);
                    seg.x1 = static_cast<float>(pt.x);
                    seg.y1 = static_cast<float>(pt.y);
                    if (seg.is_horizontal() || seg.is_vertical())
                        result.segments.push_back(seg);
                    px = pt.x; py = pt.y;
                    break;
                }
                case PathPoint::CURVE:
                    px = pt.x; py = pt.y;
                    break;
                case PathPoint::CLOSE:
                    if (has_move) {
                        PdfLineSegment seg;
                        seg.x0 = static_cast<float>(px);
                        seg.y0 = static_cast<float>(py);
                        seg.x1 = static_cast<float>(move_x);
                        seg.y1 = static_cast<float>(move_y);
                        if (seg.is_horizontal() || seg.is_vertical())
                            result.segments.push_back(seg);
                        px = move_x; py = move_y;
                    }
                    break;
            }
        }
    };

    auto filter_white_stroke = [&]() -> bool {
        return (gs.stroke_r >= 0.94 && gs.stroke_g >= 0.94 && gs.stroke_b >= 0.94);
    };

    auto filter_small_rect = [&]() -> bool {
        if (current_path.size() < 4 || current_path.size() > 6) return false;
        double min_y = 1e9, max_y = -1e9;
        double first_x = 0, first_y = 0, last_x = 0, last_y = 0;
        bool has_start = false;
        for (auto& pt : current_path) {
            if (pt.type == PathPoint::MOVE || pt.type == PathPoint::LINE) {
                if (!has_start) { first_x = pt.x; first_y = pt.y; has_start = true; }
                last_x = pt.x; last_y = pt.y;
                if (pt.y < min_y) min_y = pt.y;
                if (pt.y > max_y) max_y = pt.y;
            }
        }
        if (std::abs(first_x - last_x) < 2 && std::abs(first_y - last_y) < 2) {
            if (max_y - min_y < 20.0) return true;
        }
        return false;
    };

    while (lex.pos < lex.len) {
        lex.skip_ws();
        if (lex.pos >= lex.len) break;

        uint8_t first_byte = lex.data[lex.pos];

        // Fast path: numbers (most common token in content streams)
        if ((first_byte >= '0' && first_byte <= '9') || first_byte == '-' || first_byte == '+' || first_byte == '.') {
            size_t start = lex.pos;
            bool has_dot = (first_byte == '.');
            lex.pos++;
            while (lex.pos < lex.len) {
                uint8_t c = lex.data[lex.pos];
                if (c >= '0' && c <= '9') { lex.pos++; }
                else if (c == '.' && !has_dot) { has_dot = true; lex.pos++; }
                else break;
            }
            const char* nstart = reinterpret_cast<const char*>(lex.data + start);
            if (has_dot) {
                operands.push_back(PdfObj::make_real(std::strtod(nstart, nullptr)));
            } else {
                operands.push_back(PdfObj::make_int(std::strtoll(nstart, nullptr, 10)));
            }
            continue;
        }

        // /name → operand
        if (first_byte == '/') {
            PdfObj obj = lex.parse_object();
            operands.push_back(std::move(obj));
            continue;
        }

        // String or array or dict → parse as object
        if (first_byte == '(' || first_byte == '<' || first_byte == '[') {
            PdfObj obj = lex.parse_object();
            if (!obj.is_none()) operands.push_back(std::move(obj));
            continue;
        }

        // Bare keyword → operator
        size_t saved = lex.pos;
        std::string tok;
        while (lex.pos < lex.len && !PdfLexer::is_ws(lex.data[lex.pos]) && !PdfLexer::is_delim(lex.data[lex.pos]))
            tok += static_cast<char>(lex.data[lex.pos++]);
        if (tok.empty()) { lex.pos++; continue; }
        if (tok == "true") { operands.push_back(PdfObj::make_bool(true)); continue; }
        if (tok == "false") { operands.push_back(PdfObj::make_bool(false)); continue; }
        if (tok == "null") continue;

        {
            std::string op = tok;

            // ── Graphics State ──
            if (op == "q") {
                state_stack.push_back(gs);
            } else if (op == "Q") {
                if (!state_stack.empty()) { gs = state_stack.back(); state_stack.pop_back(); }
            } else if (op == "cm") {
                if (operands.size() >= 6) {
                    double m[6] = {pop_num(5), pop_num(4), pop_num(3), pop_num(2), pop_num(1), pop_num(0)};
                    double r[6];
                    mat_multiply(r, m, gs.ctm);
                    std::memcpy(gs.ctm, r, sizeof(r));
                }
            } else if (op == "w") {
                gs.line_width = pop_num(0);
            } else if (op == "J") {
                gs.line_cap = static_cast<int>(pop_num(0));
            } else if (op == "j") {
                gs.line_join = static_cast<int>(pop_num(0));
            } else if (op == "M") {
                gs.miter_limit = pop_num(0);
            }

            // ── Color ──
            else if (op == "RG") {
                if (operands.size() >= 3) { gs.stroke_r = pop_num(2); gs.stroke_g = pop_num(1); gs.stroke_b = pop_num(0); }
            } else if (op == "rg") {
                if (operands.size() >= 3) { gs.fill_r = pop_num(2); gs.fill_g = pop_num(1); gs.fill_b = pop_num(0); }
            } else if (op == "G") {
                double g = pop_num(0); gs.stroke_r = gs.stroke_g = gs.stroke_b = g;
            } else if (op == "g") {
                double g = pop_num(0); gs.fill_r = gs.fill_g = gs.fill_b = g;
            } else if (op == "K") {
                if (operands.size() >= 4) {
                    double c = pop_num(3), m = pop_num(2), y = pop_num(1), k = pop_num(0);
                    gs.stroke_r = 1 - std::min(1.0, c + k);
                    gs.stroke_g = 1 - std::min(1.0, m + k);
                    gs.stroke_b = 1 - std::min(1.0, y + k);
                }
            } else if (op == "k") {
                if (operands.size() >= 4) {
                    double c = pop_num(3), m = pop_num(2), y = pop_num(1), k = pop_num(0);
                    gs.fill_r = 1 - std::min(1.0, c + k);
                    gs.fill_g = 1 - std::min(1.0, m + k);
                    gs.fill_b = 1 - std::min(1.0, y + k);
                }
            } else if (op == "SC" || op == "SCN") {
                if (operands.size() >= 3) { gs.stroke_r = pop_num(2); gs.stroke_g = pop_num(1); gs.stroke_b = pop_num(0); }
                else if (operands.size() >= 1) { double g = pop_num(0); gs.stroke_r = gs.stroke_g = gs.stroke_b = g; }
            } else if (op == "sc" || op == "scn") {
                if (operands.size() >= 3) { gs.fill_r = pop_num(2); gs.fill_g = pop_num(1); gs.fill_b = pop_num(0); }
                else if (operands.size() >= 1) { double g = pop_num(0); gs.fill_r = gs.fill_g = gs.fill_b = g; }
            } else if (op == "CS" || op == "cs") {
                // Colorspace name — just consume
            }

            // ── Text ──
            else if (op == "BT") {
                double id[6] = {1,0,0,1,0,0};
                std::memcpy(gs.text_mat, id, sizeof(id));
                std::memcpy(gs.line_mat, id, sizeof(id));
            } else if (op == "ET") {
                // nothing
            } else if (op == "Tf") {
                if (operands.size() >= 2) {
                    gs.font_size = pop_num(0);
                    std::string fname = operands[operands.size() - 2].str_val;
                    auto it = fonts.find(fname);
                    gs.font = (it != fonts.end()) ? &it->second : nullptr;
                }
            } else if (op == "Td") {
                if (operands.size() >= 2) {
                    double tx = pop_num(1), ty = pop_num(0);
                    gs.line_mat[4] += tx * gs.line_mat[0] + ty * gs.line_mat[2];
                    gs.line_mat[5] += tx * gs.line_mat[1] + ty * gs.line_mat[3];
                    std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
                }
            } else if (op == "TD") {
                if (operands.size() >= 2) {
                    double tx = pop_num(1), ty = pop_num(0);
                    gs.text_leading = -ty;
                    gs.line_mat[4] += tx * gs.line_mat[0] + ty * gs.line_mat[2];
                    gs.line_mat[5] += tx * gs.line_mat[1] + ty * gs.line_mat[3];
                    std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
                }
            } else if (op == "Tm") {
                if (operands.size() >= 6) {
                    gs.text_mat[0] = pop_num(5); gs.text_mat[1] = pop_num(4);
                    gs.text_mat[2] = pop_num(3); gs.text_mat[3] = pop_num(2);
                    gs.text_mat[4] = pop_num(1); gs.text_mat[5] = pop_num(0);
                    std::memcpy(gs.line_mat, gs.text_mat, sizeof(gs.line_mat));
                }
            } else if (op == "T*") {
                gs.line_mat[4] += -gs.text_leading * gs.line_mat[2];
                gs.line_mat[5] += -gs.text_leading * gs.line_mat[3];
                std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
            } else if (op == "TL") {
                gs.text_leading = pop_num(0);
            } else if (op == "Tc") {
                gs.char_spacing = pop_num(0);
            } else if (op == "Tw") {
                gs.word_spacing = pop_num(0);
            } else if (op == "Tz") {
                gs.h_scaling = pop_num(0);
            } else if (op == "Ts") {
                gs.text_rise = pop_num(0);
            }

            // ── Text Show ──
            else if (op == "Tj" || op == "'" || op == "\"") {
                if (op == "'") {
                    gs.line_mat[4] += -gs.text_leading * gs.line_mat[2];
                    gs.line_mat[5] += -gs.text_leading * gs.line_mat[3];
                    std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
                } else if (op == "\"") {
                    if (operands.size() >= 3) {
                        gs.word_spacing = operands[0].as_num();
                        gs.char_spacing = operands[1].as_num();
                    }
                    gs.line_mat[4] += -gs.text_leading * gs.line_mat[2];
                    gs.line_mat[5] += -gs.text_leading * gs.line_mat[3];
                    std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
                }

                if (!operands.empty() && operands.back().is_str()) {
                    auto& s = operands.back().str_val;
                    double fs = gs.font_size;
                    double h_scale = gs.h_scaling / 100.0;

                    bool use_2byte = gs.font && (gs.font->is_identity || gs.font->is_type0);
                    if (gs.font && gs.font->cmap_code_bytes == 1) use_2byte = false;
                    if (gs.font && gs.font->cmap_code_bytes == 2) use_2byte = true;
                    size_t i = 0;
                    while (i < s.size()) {
                        uint32_t code;
                        if (use_2byte && i + 1 < s.size()) {
                            code = (static_cast<uint8_t>(s[i]) << 8) | static_cast<uint8_t>(s[i + 1]);
                            i += 2;
                        } else {
                            code = static_cast<uint8_t>(s[i]);
                            i++;
                        }

                        uint32_t unicode = gs.font ? gs.font->decode_char(code) : code;
                        if (unicode == 0 || unicode == 0xFFFD) continue;

                        // Recompute rendering matrix for each char (text_mat changes with advances)
                        double trm[6];
                        double scale_mat[6] = {fs * h_scale, 0, 0, fs, 0, gs.text_rise};
                        mat_multiply(trm, scale_mat, gs.text_mat);
                        double final_mat[6];
                        mat_multiply(final_mat, trm, gs.ctm);

                        double gx, gy;
                        transform_point(final_mat, 0, 0, gx, gy);

                        double glyph_w = gs.font ? gs.font->get_width(code) : 0;
                        if (glyph_w <= 0) glyph_w = (gs.font && (gs.font->is_identity || gs.font->is_type0)) ? 1000 : 600;
                        // char_w in text space (used for text matrix advance)
                        double char_w_ts = glyph_w / 1000.0 * fs * h_scale;
                        // char_w in page space (for bounding box)
                        double gx2, gy2;
                        transform_point(final_mat, glyph_w / 1000.0, 0, gx2, gy2);
                        double char_w = std::abs(gx2 - gx);
                        if (char_w < 0.1) char_w = std::abs(final_mat[0]) * glyph_w / 1000.0;
                        double char_h = std::abs(final_mat[3]);
                        if (char_h < 1) char_h = std::abs(final_mat[0]);

                        TextChar tc;
                        tc.x = gx;
                        tc.y = gy;
                        tc.left = gx;
                        tc.right = gx + char_w;
                        tc.top = gy + char_h * 0.8;
                        tc.bot = gy - char_h * 0.2;
                        tc.font_size = char_h;
                        tc.unicode = unicode;
                        tc.is_bold = gs.font ? gs.font->is_bold : false;
                        tc.is_italic = gs.font ? gs.font->is_italic : false;
                        result.chars.push_back(tc);

                        // Advance text position
                        double advance = char_w_ts + gs.char_spacing;
                        if (unicode == ' ') advance += gs.word_spacing;
                        gs.text_mat[4] += advance * gs.text_mat[0];
                        gs.text_mat[5] += advance * gs.text_mat[1];
                    }
                }
            } else if (op == "TJ") {
                if (!operands.empty() && operands.back().is_arr()) {
                    auto& arr = operands.back().arr;
                    double fs = gs.font_size;
                    double h_scale = gs.h_scaling / 100.0;
                    bool use_2byte = gs.font && (gs.font->is_identity || gs.font->is_type0);
                    if (gs.font && gs.font->cmap_code_bytes == 1) use_2byte = false;
                    if (gs.font && gs.font->cmap_code_bytes == 2) use_2byte = true;

                    for (auto& elem : arr) {
                        if (elem.is_num()) {
                            double adjust = elem.as_num();
                            double shift = -adjust / 1000.0 * fs * h_scale;
                            gs.text_mat[4] += shift * gs.text_mat[0];
                            gs.text_mat[5] += shift * gs.text_mat[1];
                        } else if (elem.is_str()) {
                            auto& s = elem.str_val;
                            size_t i = 0;
                            while (i < s.size()) {
                                uint32_t code;
                                if (use_2byte && i + 1 < s.size()) {
                                    code = (static_cast<uint8_t>(s[i]) << 8) | static_cast<uint8_t>(s[i + 1]);
                                    i += 2;
                                } else {
                                    code = static_cast<uint8_t>(s[i]);
                                    i++;
                                }

                                uint32_t unicode = gs.font ? gs.font->decode_char(code) : code;
                                if (unicode == 0 || unicode == 0xFFFD) continue;

                                double trm[6];
                                double scale_mat[6] = {fs * h_scale, 0, 0, fs, 0, gs.text_rise};
                                mat_multiply(trm, scale_mat, gs.text_mat);
                                double final_mat[6];
                                mat_multiply(final_mat, trm, gs.ctm);

                                double gx, gy;
                                transform_point(final_mat, 0, 0, gx, gy);

                                double glyph_w = gs.font ? gs.font->get_width(code) : 0;
                                if (glyph_w <= 0) glyph_w = (gs.font && (gs.font->is_identity || gs.font->is_type0)) ? 1000 : 600;
                                double char_w_ts = glyph_w / 1000.0 * fs * h_scale;
                                double gx2, gy2;
                                transform_point(final_mat, glyph_w / 1000.0, 0, gx2, gy2);
                                double char_w = std::abs(gx2 - gx);
                                if (char_w < 0.1) char_w = std::abs(final_mat[0]) * glyph_w / 1000.0;
                                double char_h = std::abs(final_mat[3]);
                                if (char_h < 1) char_h = std::abs(final_mat[0]);

                                TextChar tc;
                                tc.x = gx; tc.y = gy;
                                tc.left = gx; tc.right = gx + char_w;
                                tc.top = gy + char_h * 0.8;
                                tc.bot = gy - char_h * 0.2;
                                tc.font_size = char_h;
                                tc.unicode = unicode;
                                tc.is_bold = gs.font ? gs.font->is_bold : false;
                                tc.is_italic = gs.font ? gs.font->is_italic : false;
                                result.chars.push_back(tc);

                                double advance = char_w_ts + gs.char_spacing;
                                if (unicode == ' ') advance += gs.word_spacing;
                                gs.text_mat[4] += advance * gs.text_mat[0];
                                gs.text_mat[5] += advance * gs.text_mat[1];
                            }
                        }
                    }
                }
            }

            // ── Path Construction ──
            else if (op == "m") {
                if (operands.size() >= 2) {
                    double x = pop_num(1), y = pop_num(0);
                    double tx, ty;
                    transform_point(gs.ctm, x, y, tx, ty);
                    current_path.push_back({tx, ty, PathPoint::MOVE});
                }
            } else if (op == "l") {
                if (operands.size() >= 2) {
                    double x = pop_num(1), y = pop_num(0);
                    double tx, ty;
                    transform_point(gs.ctm, x, y, tx, ty);
                    current_path.push_back({tx, ty, PathPoint::LINE});
                }
            } else if (op == "c") {
                if (operands.size() >= 6) {
                    double x1 = pop_num(5), y1 = pop_num(4);
                    double x2 = pop_num(3), y2 = pop_num(2);
                    double x3 = pop_num(1), y3 = pop_num(0);
                    PathPoint pp; pp.type = PathPoint::CURVE;
                    transform_point(gs.ctm, x1, y1, pp.cx1, pp.cy1);
                    transform_point(gs.ctm, x2, y2, pp.cx2, pp.cy2);
                    transform_point(gs.ctm, x3, y3, pp.x, pp.y);
                    current_path.push_back(pp);
                }
            } else if (op == "v") {
                if (operands.size() >= 4) {
                    double x2 = pop_num(3), y2 = pop_num(2);
                    double x3 = pop_num(1), y3 = pop_num(0);
                    PathPoint pp; pp.type = PathPoint::CURVE;
                    // v: cp1 = current point
                    double prev_x = 0, prev_y = 0;
                    if (!current_path.empty()) {
                        prev_x = current_path.back().x; prev_y = current_path.back().y;
                    }
                    pp.cx1 = prev_x; pp.cy1 = prev_y;
                    transform_point(gs.ctm, x2, y2, pp.cx2, pp.cy2);
                    transform_point(gs.ctm, x3, y3, pp.x, pp.y);
                    current_path.push_back(pp);
                }
            } else if (op == "y") {
                if (operands.size() >= 4) {
                    double x1 = pop_num(3), y1 = pop_num(2);
                    double x3 = pop_num(1), y3 = pop_num(0);
                    PathPoint pp; pp.type = PathPoint::CURVE;
                    transform_point(gs.ctm, x1, y1, pp.cx1, pp.cy1);
                    // y: cp2 = endpoint
                    transform_point(gs.ctm, x3, y3, pp.x, pp.y);
                    pp.cx2 = pp.x; pp.cy2 = pp.y;
                    current_path.push_back(pp);
                }
            } else if (op == "h") {
                current_path.push_back({0, 0, PathPoint::CLOSE});
            } else if (op == "re") {
                if (operands.size() >= 4) {
                    double x = pop_num(3), y = pop_num(2), w = pop_num(1), h = pop_num(0);
                    double tx, ty;
                    transform_point(gs.ctm, x, y, tx, ty);
                    double tx2, ty2; transform_point(gs.ctm, x+w, y, tx2, ty2);
                    double tx3, ty3; transform_point(gs.ctm, x+w, y+h, tx3, ty3);
                    double tx4, ty4; transform_point(gs.ctm, x, y+h, tx4, ty4);
                    current_path.push_back({tx, ty, PathPoint::MOVE});
                    current_path.push_back({tx2, ty2, PathPoint::LINE});
                    current_path.push_back({tx3, ty3, PathPoint::LINE});
                    current_path.push_back({tx4, ty4, PathPoint::LINE});
                    current_path.push_back({0, 0, PathPoint::CLOSE});
                }
            }

            // ── Path Painting ──
            else if (op == "S") {
                if (!filter_white_stroke() && !filter_small_rect())
                    flush_path_segments();
                { RenderPath rp; rp.points = current_path;
                  rp.stroke_r = gs.stroke_r; rp.stroke_g = gs.stroke_g; rp.stroke_b = gs.stroke_b;
                  rp.fill_r = gs.fill_r; rp.fill_g = gs.fill_g; rp.fill_b = gs.fill_b;
                  rp.line_width = gs.line_width; rp.do_fill = false; rp.do_stroke = true;
                  result.paths.push_back(std::move(rp)); }
                current_path.clear();
            } else if (op == "s") {
                current_path.push_back({0, 0, PathPoint::CLOSE});
                if (!filter_white_stroke() && !filter_small_rect())
                    flush_path_segments();
                { RenderPath rp; rp.points = current_path;
                  rp.stroke_r = gs.stroke_r; rp.stroke_g = gs.stroke_g; rp.stroke_b = gs.stroke_b;
                  rp.fill_r = gs.fill_r; rp.fill_g = gs.fill_g; rp.fill_b = gs.fill_b;
                  rp.line_width = gs.line_width; rp.do_fill = false; rp.do_stroke = true;
                  result.paths.push_back(std::move(rp)); }
                current_path.clear();
            } else if (op == "f" || op == "F" || op == "f*") {
                if (!filter_small_rect()) flush_path_segments();
                { RenderPath rp; rp.points = current_path;
                  rp.fill_r = gs.fill_r; rp.fill_g = gs.fill_g; rp.fill_b = gs.fill_b;
                  rp.stroke_r = gs.stroke_r; rp.stroke_g = gs.stroke_g; rp.stroke_b = gs.stroke_b;
                  rp.line_width = gs.line_width; rp.do_fill = true; rp.do_stroke = false;
                  result.paths.push_back(std::move(rp)); }
                current_path.clear();
            } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
                if (op == "b" || op == "b*")
                    current_path.push_back({0, 0, PathPoint::CLOSE});
                if (!filter_white_stroke() && !filter_small_rect())
                    flush_path_segments();
                { RenderPath rp; rp.points = current_path;
                  rp.fill_r = gs.fill_r; rp.fill_g = gs.fill_g; rp.fill_b = gs.fill_b;
                  rp.stroke_r = gs.stroke_r; rp.stroke_g = gs.stroke_g; rp.stroke_b = gs.stroke_b;
                  rp.line_width = gs.line_width; rp.do_fill = true; rp.do_stroke = true;
                  result.paths.push_back(std::move(rp)); }
                current_path.clear();
            } else if (op == "n") {
                current_path.clear();
            }

            // ── XObject (images) ──
            else if (op == "Do") {
                if (!operands.empty() && operands.back().is_name()) {
                    std::string xname = operands.back().str_val;
                    auto& xobjects = res.get("XObject");
                    auto xd = doc.resolve(xobjects);
                    if (xd.is_dict()) {
                        auto& xref = xd.get(xname);
                        ImagePlacement ip;
                        ip.xobj_name = xname;
                        if (xref.is_ref()) ip.xobj_ref = xref.ref_num;
                        std::memcpy(ip.ctm, gs.ctm, sizeof(gs.ctm));
                        ip.fill_r = gs.fill_r;
                        ip.fill_g = gs.fill_g;
                        ip.fill_b = gs.fill_b;
                        result.images.push_back(ip);
                    }
                }
            }

            operands.clear();
        }
    }

    return result;
}

// ── Layout Engine: TextChar → TextLine ───────────────────

struct TextLine {
    std::string text;
    double font_size = 0;
    bool is_bold = false;
    bool is_italic = false;
    double y_center = 0;
    double x_left = 1e9;
    double x_right = 0;
};

struct PageCharCache {
    struct CharInfo {
        double x, y;
        double left, right, top, bot;
        unsigned int unicode;
    };
    std::vector<CharInfo> chars;
    std::vector<size_t> y_sorted;

    void build(const std::vector<TextChar>& text_chars) {
        chars.reserve(text_chars.size());
        for (auto& tc : text_chars) {
            if (tc.unicode == 0 || tc.unicode == '\r' || tc.unicode == '\n' || tc.unicode == 0xFFFD) continue;
            chars.push_back({tc.x, tc.y, tc.left, tc.right, tc.top, tc.bot, tc.unicode});
        }
        y_sorted.resize(chars.size());
        for (size_t i = 0; i < chars.size(); i++) y_sorted[i] = i;
        std::stable_sort(y_sorted.begin(), y_sorted.end(),
            [this](size_t a, size_t b) { return chars[a].y < chars[b].y; });
    }

    std::string get_text_in_rect(double left, double top, double right, double bottom) const {
        double rect_top = std::max(top, bottom);
        double rect_bot = std::min(top, bottom);
        double y_lo = rect_bot + 1, y_hi = rect_top - 1;
        auto lo_it = std::lower_bound(y_sorted.begin(), y_sorted.end(), y_lo,
            [this](size_t idx, double val) { return chars[idx].y < val; });
        auto hi_it = std::upper_bound(lo_it, y_sorted.end(), y_hi,
            [this](double val, size_t idx) { return val < chars[idx].y; });
        std::vector<size_t> matches;
        for (auto it = lo_it; it != hi_it; ++it) {
            auto& ch = chars[*it];
            if (ch.x >= left + 1 && ch.x <= right - 1)
                matches.push_back(*it);
        }
        std::sort(matches.begin(), matches.end());
        std::string text;
        for (size_t idx : matches) {
            auto& ch = chars[idx];
            if (ch.unicode == ' ' || ch.unicode == 0xA0)
                text += ' ';
            else
                util::append_utf8(text, ch.unicode);
        }
        size_t s = text.find_first_not_of(" ");
        size_t e = text.find_last_not_of(" ");
        if (s != std::string::npos) return text.substr(s, e - s + 1);
        return "";
    }

    std::vector<std::pair<double,double>> get_char_ranges_in_row(
            double y_center, double y_tol, double x_min, double x_max) const {
        std::vector<std::pair<double,double>> ranges;
        for (auto& ch : chars) {
            if (ch.unicode == ' ' || ch.unicode == '\t' || ch.unicode == 0xA0) continue;
            if (std::abs(ch.y - y_center) > y_tol) continue;
            if (ch.x < x_min - 5 || ch.x > x_max + 5) continue;
            ranges.push_back({ch.left, ch.right});
        }
        return ranges;
    }
};

std::vector<TextLine> chars_to_lines(const std::vector<TextChar>& chars) {
    if (chars.empty()) return {};

    // Sort by y (descending, top-first) then x (left-to-right)
    std::vector<size_t> idx(chars.size());
    std::iota(idx.begin(), idx.end(), 0);

    // Compute median font size for line clustering tolerance
    std::vector<double> font_sizes;
    for (auto& ch : chars) if (ch.font_size > 1) font_sizes.push_back(ch.font_size);
    double median_fs = 12;
    if (!font_sizes.empty()) {
        std::sort(font_sizes.begin(), font_sizes.end());
        median_fs = font_sizes[font_sizes.size() / 2];
    }
    double y_tol = median_fs * 0.4;
    if (y_tol < 2) y_tol = 2;

    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        if (std::abs(chars[a].y - chars[b].y) > y_tol) return chars[a].y > chars[b].y;
        return chars[a].x < chars[b].x;
    });

    std::vector<TextLine> lines;
    double cur_y = chars[idx[0]].y;
    TextLine cur;
    double total_fs = 0;
    int fs_count = 0;
    double prev_right = -1e9;

    auto flush = [&]() {
        if (cur.text.empty()) return;
        size_t end = cur.text.find_last_not_of(" \t");
        if (end != std::string::npos) cur.text.resize(end + 1);
        if (fs_count > 0) cur.font_size = total_fs / fs_count;
        if (!cur.text.empty()) lines.push_back(std::move(cur));
        cur = TextLine{};
        total_fs = 0;
        fs_count = 0;
        prev_right = -1e9;
    };

    for (size_t ii = 0; ii < idx.size(); ii++) {
        auto& ch = chars[idx[ii]];
        if (std::abs(ch.y - cur_y) > y_tol) {
            flush();
            cur_y = ch.y;
        }
        cur.y_center = ch.y;
        if (ch.left < cur.x_left) cur.x_left = ch.left;
        if (ch.right > cur.x_right) cur.x_right = ch.right;

        // Detect word spacing using gap between this char's left and previous char's right
        if (!cur.text.empty() && ch.unicode != ' ' && ch.unicode != 0xA0 && prev_right > -1e8) {
            double gap = ch.left - prev_right;
            // Use font-size-relative threshold for word spacing
            double word_gap = ch.font_size * 0.25;
            if (word_gap < 2) word_gap = 2;
            if (gap > word_gap && gap < ch.font_size * 8)
                cur.text += ' ';
        }

        if (ch.unicode != ' ' && ch.unicode != 0xA0) {
            cur.is_bold = ch.is_bold;
            cur.is_italic = ch.is_italic;
            total_fs += ch.font_size;
            fs_count++;
        }
        util::append_utf8(cur.text, ch.unicode);
        prev_right = ch.right;
    }
    flush();
    return lines;
}

// ── PDF-specific types ───────────────────────────────────

struct TableData {
    std::vector<std::vector<std::string>> rows;
    double x0, y0, x1, y1;
    int page = 0;
};

struct FontStats {
    double body_size = 12.0;

    void compute(const std::vector<std::vector<TextLine>>& all_lines) {
        std::map<int, int> counts;
        for (auto& pl : all_lines)
            for (auto& l : pl)
                if (l.font_size > 1.0)
                    counts[(int)(l.font_size * 10)]++;

        int max_c = 0, max_k = 120;
        for (auto& [k, c] : counts)
            if (c > max_c) { max_c = c; max_k = k; }
        body_size = max_k / 10.0;
        if (body_size < 4.0) body_size = 12.0;
    }

    int heading_level(double fs) const {
        if (fs <= 0) return 0;
        double r = fs / body_size;
        if (r >= 1.8) return 1;
        if (r >= 1.5) return 2;
        if (r >= 1.3) return 3;
        return 0;
    }
};

// ── Table detection helpers ──────────────────────────────

std::vector<std::pair<double,double>> merge_char_ranges(
        std::vector<std::pair<double,double>>& ranges, double merge_gap = 8.0) {
    std::vector<std::pair<double,double>> spans;
    if (ranges.empty()) return spans;
    std::sort(ranges.begin(), ranges.end());
    auto cur = ranges[0];
    for (size_t i = 1; i < ranges.size(); i++) {
        if (ranges[i].first - cur.second < merge_gap) {
            cur.second = std::max(cur.second, ranges[i].second);
        } else {
            spans.push_back(cur);
            cur = ranges[i];
        }
    }
    spans.push_back(cur);
    return spans;
}

std::vector<double> cluster_values(std::vector<double>& vals, double tol) {
    if (vals.empty()) return {};
    std::sort(vals.begin(), vals.end());
    std::vector<double> clusters;
    clusters.push_back(vals[0]);
    for (size_t i = 1; i < vals.size(); i++) {
        if (vals[i] - clusters.back() > tol)
            clusters.push_back(vals[i]);
    }
    return clusters;
}

std::vector<double> detect_response_boundaries(const PageCharCache& cache,
                                                double left, double right,
                                                const std::vector<double>& row_ys) {
    double width = right - left;
    if (width < 150) return {};

    int n_rows = (int)row_ys.size() - 1;
    if (n_rows < 2) return {};

    struct RowChars { std::vector<double> xs; };
    std::vector<RowChars> per_row(n_rows);

    for (auto& ch : cache.chars) {
        if (ch.unicode == ' ' || ch.unicode == 0xA0 || ch.unicode == '\t') continue;
        if (ch.x < left + 1 || ch.x > right - 1) continue;

        for (int r = 0; r < n_rows; r++) {
            double bot = std::min(row_ys[r], row_ys[r+1]);
            double top = std::max(row_ys[r], row_ys[r+1]);
            if (ch.y >= bot + 1 && ch.y <= top - 1) {
                per_row[r].xs.push_back(ch.x);
                break;
            }
        }
    }

    std::vector<double> all_centers;
    int rows_with_clusters = 0;

    for (int r = 0; r < n_rows; r++) {
        auto& xs = per_row[r].xs;
        if (xs.size() < 3) continue;
        std::sort(xs.begin(), xs.end());

        std::vector<double> centers;
        double sum = xs[0];
        int cnt = 1;
        for (size_t i = 1; i < xs.size(); i++) {
            if (xs[i] - xs[i-1] > 20.0) {
                centers.push_back(sum / cnt);
                sum = xs[i];
                cnt = 1;
            } else {
                sum += xs[i];
                cnt++;
            }
        }
        centers.push_back(sum / cnt);

        if ((int)centers.size() >= 3) {
            rows_with_clusters++;
            for (double c : centers)
                all_centers.push_back(c);
        }
    }

    if (rows_with_clusters < 2) return {};

    auto stable_xs = cluster_values(all_centers, 15.0);

    if ((int)stable_xs.size() > 7) {
        int n = (int)stable_xs.size();
        std::vector<int> hits(n, 0);
        for (int i = 0; i < n; i++)
            for (double c : all_centers)
                if (std::abs(c - stable_xs[i]) < 15.0) hits[i]++;

        std::vector<int> order(n);
        for (int i = 0; i < n; i++) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return hits[a] > hits[b]; });

        int top = std::min(n, 8);
        std::vector<double> cands;
        for (int i = 0; i < top; i++) cands.push_back(stable_xs[order[i]]);
        std::sort(cands.begin(), cands.end());

        if ((int)cands.size() >= 5) {
            double best_var = 1e9;
            std::vector<double> best_set;
            int cn = (int)cands.size();
            for (int a = 0; a < cn-4; a++)
            for (int b = a+1; b < cn-3; b++)
            for (int c = b+1; c < cn-2; c++)
            for (int d = c+1; d < cn-1; d++)
            for (int e = d+1; e < cn; e++) {
                double xs[5] = {cands[a], cands[b], cands[c], cands[d], cands[e]};
                double avg = (xs[4] - xs[0]) / 4.0;
                double var = 0;
                for (int i = 0; i < 4; i++) {
                    double g = xs[i+1] - xs[i] - avg;
                    var += g * g;
                }
                if (var < best_var) { best_var = var; best_set = {xs[0], xs[1], xs[2], xs[3], xs[4]}; }
            }
            if (!best_set.empty() && best_var < 200.0) {
                stable_xs = best_set;
            } else {
                return {};
            }
        } else {
            return {};
        }
    }

    if ((int)stable_xs.size() < 3 || (int)stable_xs.size() > 7) return {};

    int n_sub = (int)stable_xs.size();
    std::vector<double> boundaries;
    boundaries.push_back(left);
    for (int i = 0; i < n_sub - 1; i++) {
        boundaries.push_back((stable_xs[i] + stable_xs[i+1]) / 2.0);
    }
    boundaries.push_back(right);

    return boundaries;
}

bool is_scale_row(const PageCharCache& cache, double left, double right,
                   double bot, double top, const std::vector<double>& boundaries) {
    int n_sub = (int)boundaries.size() - 1;
    if (n_sub < 3) return false;

    int filled = 0, total_len = 0, max_len = 0;
    for (int sc = 0; sc < n_sub; sc++) {
        std::string t = cache.get_text_in_rect(boundaries[sc], top, boundaries[sc+1], bot);
        int len = (int)t.size();
        if (len > 0) { filled++; total_len += len; }
        if (len > max_len) max_len = len;
    }

    return filled >= 3 && max_len <= 40 && total_len <= 80;
}

std::vector<double> find_column_boundaries(
        const std::vector<PdfLineSegment>& v_lines,
        double table_left, double table_right,
        double table_bot, double table_top) {
    double table_height = table_top - table_bot;

    std::vector<double> vx_vals;
    for (auto& vl : v_lines) {
        double vx = (vl.x0 + vl.x1) / 2.0;
        double vy_lo = std::min((double)vl.y0, (double)vl.y1);
        double vy_hi = std::max((double)vl.y0, (double)vl.y1);
        if (vy_hi < table_bot - 5 || vy_lo > table_top + 5) continue;
        if (vx < table_left - 5 || vx > table_right + 5) continue;
        vx_vals.push_back(vx);
    }
    auto vx_clusters = cluster_values(vx_vals, 5.0);

    struct VLineInfo { double x, coverage; };
    std::vector<VLineInfo> vline_infos;
    for (double cx : vx_clusters) {
        std::vector<std::pair<double,double>> intervals;
        for (auto& vl : v_lines) {
            double vx = (vl.x0 + vl.x1) / 2.0;
            if (std::abs(vx - cx) > 6.0) continue;
            double vy_lo = std::min((double)vl.y0, (double)vl.y1);
            double vy_hi = std::max((double)vl.y0, (double)vl.y1);
            if (vy_hi < table_bot - 5 || vy_lo > table_top + 5) continue;
            intervals.push_back({vy_lo, vy_hi});
        }
        if (intervals.empty()) continue;
        std::sort(intervals.begin(), intervals.end());
        double total = 0, cur_lo = intervals[0].first, cur_hi = intervals[0].second;
        for (size_t i = 1; i < intervals.size(); i++) {
            if (intervals[i].first <= cur_hi + 3.0)
                cur_hi = std::max(cur_hi, intervals[i].second);
            else { total += cur_hi - cur_lo; cur_lo = intervals[i].first; cur_hi = intervals[i].second; }
        }
        total += cur_hi - cur_lo;
        vline_infos.push_back({cx, total});
    }

    std::vector<double> col_xs;
    col_xs.push_back(table_left);
    for (auto& vi : vline_infos)
        if (vi.coverage >= table_height * 0.4 &&
            vi.x > table_left + 5 && vi.x < table_right - 5)
            col_xs.push_back(vi.x);
    col_xs.push_back(table_right);

    std::sort(col_xs.begin(), col_xs.end());
    col_xs.erase(std::unique(col_xs.begin(), col_xs.end(),
        [](double a, double b) { return std::abs(a - b) < 5.0; }), col_xs.end());

    if (col_xs.size() > 3) {
        std::vector<double> merged;
        merged.push_back(col_xs[0]);
        for (size_t i = 1; i < col_xs.size() - 1; i++) {
            if (col_xs[i] - merged.back() < 25.0) {
                double cov = 0;
                for (auto& vi : vline_infos)
                    if (std::abs(vi.x - col_xs[i]) < 6.0) { cov = vi.coverage; break; }
                if (cov >= table_height * 0.8)
                    merged.push_back(col_xs[i]);
            } else {
                merged.push_back(col_xs[i]);
            }
        }
        merged.push_back(col_xs.back());
        col_xs = std::move(merged);
    }

    while (col_xs.size() > 9) {
        double min_gap = 1e9;
        size_t min_idx = 1;
        for (size_t i = 1; i < col_xs.size() - 1; i++) {
            double gap = col_xs[i + 1] - col_xs[i];
            if (gap < min_gap) { min_gap = gap; min_idx = i; }
        }
        col_xs.erase(col_xs.begin() + min_idx);
    }
    return col_xs;
}

void trim_table(TableData& table) {
    auto row_empty = [](const std::vector<std::string>& row) {
        for (auto& c : row) if (!c.empty()) return false;
        return true;
    };
    while (!table.rows.empty() && row_empty(table.rows.back()))
        table.rows.pop_back();
    while (!table.rows.empty() && row_empty(table.rows.front()))
        table.rows.erase(table.rows.begin());

    while (!table.rows.empty() && !table.rows[0].empty()) {
        int last = (int)table.rows[0].size() - 1;
        bool empty = true;
        for (auto& row : table.rows)
            if (last < (int)row.size() && !row[last].empty()) { empty = false; break; }
        if (empty) { for (auto& row : table.rows) if (!row.empty()) row.pop_back(); }
        else break;
    }
}

// Forward declaration
std::vector<double> infer_columns_from_text(const PageCharCache& cache,
                                             double left, double right,
                                             const std::vector<double>& row_ys);

TableData build_table(const std::vector<double>& row_ys,
                       const std::vector<PdfLineSegment>& h_lines,
                       const std::vector<PdfLineSegment>& v_lines,
                       const PageCharCache& cache) {
    TableData table;
    double table_top = row_ys.back();
    double table_bot = row_ys.front();

    double table_left = 1e9, table_right = 0;
    for (auto& hl : h_lines) {
        double hy = (hl.y0 + hl.y1) / 2.0;
        bool in_table = false;
        for (auto& ry : row_ys)
            if (std::abs(hy - ry) < 4.0) { in_table = true; break; }
        if (!in_table) continue;
        double lx = std::min((double)hl.x0, (double)hl.x1);
        double rx = std::max((double)hl.x0, (double)hl.x1);
        if (lx < table_left) table_left = lx;
        if (rx > table_right) table_right = rx;
    }

    auto col_xs = find_column_boundaries(v_lines, table_left, table_right,
                                          table_bot, table_top);

    int internal_vline_count = 0;
    for (auto& vl : v_lines) {
        double vx = (vl.x0 + vl.x1) / 2.0;
        if (vx > table_left + 10 && vx < table_right - 10) {
            double vy_lo = std::min((double)vl.y0, (double)vl.y1);
            double vy_hi = std::max((double)vl.y0, (double)vl.y1);
            if (vy_hi > table_bot - 5 && vy_lo < table_top + 5)
                internal_vline_count++;
        }
    }

    if (col_xs.size() < 3 && internal_vline_count == 0) {
        col_xs = infer_columns_from_text(cache, table_left, table_right, row_ys);
    }

    if (col_xs.size() < 3) {
        table.rows.clear();
        return table;
    }

    std::vector<double> actual_ys;
    {
        double tl = col_xs.front(), tr = col_xs.back();
        double row_h = (row_ys.size() >= 2) ? (row_ys[1] - row_ys[0]) : 18.0;
        int n_cols_found = (int)col_xs.size() - 1;

        struct CharPos { double x, y; };
        std::vector<CharPos> cchars;
        for (auto& ch : cache.chars) {
            if (ch.unicode == ' ' || ch.unicode == 0xA0 || ch.unicode == '\t') continue;
            if (ch.x < tl - 5 || ch.x > tr + 5) continue;
            cchars.push_back({ch.x, ch.y});
        }

        std::vector<double> cy_vals;
        for (auto& cp : cchars) cy_vals.push_back(cp.y);
        auto text_row_ys = cluster_values(cy_vals, row_h * 0.4);

        std::vector<std::vector<double>> col_char_ys(n_cols_found);
        for (auto& cp : cchars) {
            for (int c = 0; c < n_cols_found; c++) {
                if (cp.x >= col_xs[c] && cp.x <= col_xs[c + 1]) {
                    col_char_ys[c].push_back(cp.y);
                    break;
                }
            }
        }

        std::vector<double> table_row_centers;
        double tol = row_h * 0.4;
        for (double ry : text_row_ys) {
            int cols_hit = 0;
            for (int c = 0; c < n_cols_found; c++) {
                for (double cy : col_char_ys[c]) {
                    if (std::abs(cy - ry) < tol) { cols_hit++; break; }
                }
            }
            if (cols_hit >= 2)
                table_row_centers.push_back(ry);
        }

        double tb = row_ys.front(), tt = row_ys.back();
        int rows_in_grid = 0;
        for (double tc : table_row_centers)
            if (tc >= tb - row_h * 0.5 && tc <= tt + row_h * 0.5)
                rows_in_grid++;

        int n_rows_expected = (int)row_ys.size() - 1;
        bool use_text_rows = (rows_in_grid < n_rows_expected * 0.9) &&
                             ((int)table_row_centers.size() >= n_rows_expected * 0.8);

        if (use_text_rows && !table_row_centers.empty()) {
            std::sort(table_row_centers.begin(), table_row_centers.end());
            double half = row_h / 2.0;
            actual_ys.push_back(table_row_centers.front() - half);
            for (size_t i = 0; i < table_row_centers.size() - 1; i++)
                actual_ys.push_back((table_row_centers[i] + table_row_centers[i + 1]) / 2.0);
            actual_ys.push_back(table_row_centers.back() + half);
        } else {
            actual_ys = row_ys;
        }
    }

    int n_rows = (int)actual_ys.size() - 1;
    int n_cols = (int)col_xs.size() - 1;

    table.x0 = col_xs.front();
    table.y0 = actual_ys.front();
    table.x1 = col_xs.back();
    table.y1 = actual_ys.back();

    int last_col_idx = n_cols - 1;
    auto sub_boundaries = detect_response_boundaries(cache,
        col_xs[last_col_idx], col_xs[last_col_idx + 1], actual_ys);
    int n_sub = sub_boundaries.empty() ? 0 : (int)sub_boundaries.size() - 1;
    int total_cols = (n_sub > 1) ? (n_cols - 1 + n_sub) : n_cols;

    table.rows.resize(n_rows);
    for (int r = 0; r < n_rows; r++) {
        table.rows[r].resize(total_cols);
        for (int c = 0; c < n_cols; c++) {
            double left   = col_xs[c];
            double right  = col_xs[c + 1];
            double bottom = actual_ys[r];
            double top    = actual_ys[r + 1];

            if (c == last_col_idx && n_sub > 1) {
                if (is_scale_row(cache, left, right, bottom, top, sub_boundaries)) {
                    for (int sc = 0; sc < n_sub; sc++) {
                        table.rows[r][n_cols - 1 + sc] = cache.get_text_in_rect(
                            sub_boundaries[sc], top, sub_boundaries[sc+1], bottom);
                    }
                } else {
                    table.rows[r][n_cols - 1] = cache.get_text_in_rect(left, top, right, bottom);
                }
            } else {
                table.rows[r][c] = cache.get_text_in_rect(left, top, right, bottom);
            }
        }
    }

    std::reverse(table.rows.begin(), table.rows.end());
    trim_table(table);

    int meaningful_rows = 0;
    for (auto& row : table.rows) {
        int filled_cols = 0;
        for (auto& cell : row) if (!cell.empty()) filled_cols++;
        if (filled_cols >= 2) meaningful_rows++;
    }
    if (meaningful_rows < 3) table.rows.clear();

    if (!table.rows.empty()) {
        int n_cols_t = (int)table.rows[0].size();
        int total_cells = 0;
        int empty_cells = 0;
        for (auto& row : table.rows) {
            for (int c = 0; c < n_cols_t && c < (int)row.size(); c++) {
                total_cells++;
                if (row[c].empty()) empty_cells++;
            }
        }
        if (total_cells > 0 && empty_cells > total_cells * 0.6) {
            table.rows.clear();
            return table;
        }
    }

    // Reject list-like tables
    if (!table.rows.empty() && (int)table.rows[0].size() >= 2) {
        int n_cols_t = (int)table.rows[0].size();
        int rows_with_list_marker = 0;
        int valid_rows_t = 0;
        for (auto& row : table.rows) {
            bool has_content = false;
            for (auto& c : row) if (!c.empty()) { has_content = true; break; }
            if (!has_content) continue;
            valid_rows_t++;
            std::string first_cell;
            for (auto& c : row) if (!c.empty()) { first_cell = c; break; }
            if (first_cell.empty()) continue;
            bool is_marker = false;
            if (first_cell[0] >= '0' && first_cell[0] <= '9') {
                for (size_t k = 1; k < first_cell.size() && k < 4; k++) {
                    if (first_cell[k] == ')') { is_marker = true; break; }
                    if (first_cell[k] < '0' || first_cell[k] > '9') break;
                }
            }
            if (is_marker) rows_with_list_marker++;
        }
        if (valid_rows_t >= 2 && rows_with_list_marker >= valid_rows_t * 0.6) {
            table.rows.clear();
            return table;
        }
    }

    if (!table.rows.empty() && (int)table.rows[0].size() == 2) {
        int short_label_rows = 0;
        int long_content_rows = 0;
        for (auto& row : table.rows) {
            if (row.size() >= 2 && row[0].size() <= 6 && !row[0].empty())
                short_label_rows++;
            if (row.size() >= 2 && row[1].size() > 15)
                long_content_rows++;
        }
        int valid_rows = (int)table.rows.size();
        if (valid_rows >= 2 &&
            short_label_rows >= valid_rows * 0.5 &&
            long_content_rows >= valid_rows * 0.5) {
            table.rows.clear();
        }
    }

    // Reject tables where text continues across column boundaries
    // (body text split by vertical lines — not real tabular data)
    if (!table.rows.empty()) {
        int n_cols_t = (int)table.rows[0].size();
        auto is_content = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80; };

        if (n_cols_t <= 3) {
            // 2-3 column: check if most rows have text continuation
            int cont_rows = 0, checked = 0;
            for (auto& row : table.rows) {
                if ((int)row.size() < 2 || row[0].empty() || row[1].empty()) continue;
                checked++;
                if (is_content((unsigned char)row[0].back()) && is_content((unsigned char)row[1][0]))
                    cont_rows++;
            }
            if (checked >= 3 && cont_rows >= checked * 0.15)
                table.rows.clear();
            // Also reject if first column cells are very long (body text, not table data)
            if (!table.rows.empty() && n_cols_t == 2) {
                int long_first = 0;
                for (auto& row : table.rows) {
                    if (row.size() >= 2 && row[0].size() > 100) long_first++;
                }
                if (long_first >= 2) table.rows.clear();
            }
        } else if (n_cols_t >= 8) {
            // High column count (8+): likely body text split by many vertical lines
            // Check if majority of non-empty cells are text fragments (short, continuation)
            int cont_rows = 0, checked = 0;
            for (auto& row : table.rows) {
                if ((int)row.size() < n_cols_t) continue;
                int pairs_ok = 0, pairs_cont = 0;
                for (int c = 0; c + 1 < n_cols_t; c++) {
                    if (row[c].empty() || row[c+1].empty()) continue;
                    pairs_ok++;
                    if (is_content((unsigned char)row[c].back()) && is_content((unsigned char)row[c+1][0]))
                        pairs_cont++;
                }
                checked++;
                // Most adjacent cell pairs show text continuation
                if (pairs_ok > 0 && pairs_cont >= pairs_ok * 0.6) cont_rows++;
            }
            if (checked >= 3 && cont_rows >= checked * 0.4)
                table.rows.clear();
        }
    }

    return table;
}

double h_line_coverage_at_y(const std::vector<PdfLineSegment>& h_lines,
                             double y, double tol) {
    std::vector<std::pair<double,double>> intervals;
    for (auto& hl : h_lines) {
        double hy = (hl.y0 + hl.y1) / 2.0;
        if (std::abs(hy - y) > tol) continue;
        double lx = std::min((double)hl.x0, (double)hl.x1);
        double rx = std::max((double)hl.x0, (double)hl.x1);
        intervals.push_back({lx, rx});
    }
    if (intervals.empty()) return 0;
    std::sort(intervals.begin(), intervals.end());
    double total = 0, cur_l = intervals[0].first, cur_r = intervals[0].second;
    for (size_t i = 1; i < intervals.size(); i++) {
        if (intervals[i].first <= cur_r + 2.0)
            cur_r = std::max(cur_r, intervals[i].second);
        else { total += cur_r - cur_l; cur_l = intervals[i].first; cur_r = intervals[i].second; }
    }
    total += cur_r - cur_l;
    return total;
}

bool h_lines_share_full_span(const std::vector<PdfLineSegment>& h_lines,
                              double y1, double y2, double tol) {
    double min1 = 1e9, max1 = 0, min2 = 1e9, max2 = 0;
    for (auto& hl : h_lines) {
        double hy = (hl.y0 + hl.y1) / 2.0;
        double lx = std::min((double)hl.x0, (double)hl.x1);
        double rx = std::max((double)hl.x0, (double)hl.x1);
        if (std::abs(hy - y1) < tol) { if (lx < min1) min1 = lx; if (rx > max1) max1 = rx; }
        if (std::abs(hy - y2) < tol) { if (lx < min2) min2 = lx; if (rx > max2) max2 = rx; }
    }
    if (max1 == 0 || max2 == 0) return false;

    double extent1 = max1 - min1;
    double extent2 = max2 - min2;
    if (extent1 < 50 || extent2 < 50) return false;

    double cov1 = h_line_coverage_at_y(h_lines, y1, tol);
    double cov2 = h_line_coverage_at_y(h_lines, y2, tol);
    if (cov1 < extent1 * 0.4 || cov2 < extent2 * 0.4) return false;

    double overlap_l = std::max(min1, min2);
    double overlap_r = std::min(max1, max2);
    if (overlap_r <= overlap_l) return false;
    double overlap = overlap_r - overlap_l;
    double extent = std::max(extent1, extent2);
    double ratio = std::min(extent1, extent2) / extent;
    return overlap >= extent * 0.7 && ratio >= 0.5;
}

std::vector<double> infer_columns_from_text(const PageCharCache& cache,
                                             double left, double right,
                                             const std::vector<double>& row_ys) {
    int n_rows = (int)row_ys.size() - 1;
    if (n_rows < 1) return {};

    std::vector<std::vector<std::pair<double,double>>> per_row(n_rows);

    for (auto& ch : cache.chars) {
        if (ch.unicode == ' ' || ch.unicode == 0xA0 || ch.unicode == '\t') continue;
        double cx = ch.x;
        double cy = ch.y;
        if (cx < left - 5 || cx > right + 5) continue;
        for (int r = 0; r < n_rows; r++) {
            double bot = std::min(row_ys[r], row_ys[r+1]);
            double top = std::max(row_ys[r], row_ys[r+1]);
            if (cy >= bot + 1 && cy <= top - 1) {
                per_row[r].push_back({ch.left, ch.right});
                break;
            }
        }
    }
    for (int r = 0; r < n_rows; r++) {
        per_row[r] = merge_char_ranges(per_row[r]);
    }

    double width = right - left;
    int n_bins = std::max(20, (int)(width / 5.0));
    double bin_w = width / n_bins;
    std::vector<int> gap_counts(n_bins, 0);

    for (int r = 0; r < n_rows; r++) {
        if (per_row[r].empty()) continue;
        for (int b = 0; b < n_bins; b++) {
            double bx = left + b * bin_w + bin_w / 2.0;
            bool in_text = false;
            for (auto& sp : per_row[r]) {
                if (bx >= sp.first - 2 && bx <= sp.second + 2) {
                    in_text = true;
                    break;
                }
            }
            if (!in_text) gap_counts[b]++;
        }
    }

    int non_empty_rows = 0;
    for (int r = 0; r < n_rows; r++)
        if (!per_row[r].empty()) non_empty_rows++;
    int threshold = std::max(1, (int)(non_empty_rows * 0.4));

    std::vector<double> boundaries;
    boundaries.push_back(left);
    bool in_gap = false;
    double gap_start = 0;
    for (int b = 0; b < n_bins; b++) {
        double bx = left + b * bin_w + bin_w / 2.0;
        if (gap_counts[b] >= threshold) {
            if (!in_gap) { gap_start = bx; in_gap = true; }
        } else {
            if (in_gap) {
                double gap_center = (gap_start + bx) / 2.0;
                if (gap_center > left + 15 && gap_center < right - 15)
                    boundaries.push_back(gap_center);
                in_gap = false;
            }
        }
    }
    boundaries.push_back(right);

    if (boundaries.size() < 3) return {};

    int n_cols_inferred = (int)boundaries.size() - 1;
    int rows_with_multi_cols = 0;
    for (int r = 0; r < n_rows; r++) {
        if (per_row[r].empty()) continue;
        int cols_hit = 0;
        for (int c = 0; c < n_cols_inferred; c++) {
            double col_l = boundaries[c];
            double col_r = boundaries[c + 1];
            for (auto& sp : per_row[r]) {
                double sp_mid = (sp.first + sp.second) / 2.0;
                if (sp_mid >= col_l && sp_mid <= col_r) { cols_hit++; break; }
            }
        }
        if (cols_hit >= 2) rows_with_multi_cols++;
    }
    if (rows_with_multi_cols < non_empty_rows * 0.5) return {};

    return boundaries;
}

std::vector<TableData> detect_tables(const std::vector<PdfLineSegment>& lines,
                                      const PageCharCache& cache,
                                      double page_width, double page_height) {
    if (lines.size() < 4) return {};

    std::vector<PdfLineSegment> h_lines, v_lines;
    for (auto& l : lines) {
        if (l.is_horizontal()) {
            double y = (l.y0 + l.y1) / 2.0;
            if (y < 0 || y > page_height) continue;
            double lx = std::min((double)l.x0, (double)l.x1);
            double rx = std::max((double)l.x0, (double)l.x1);
            if (lx < -10 || rx > page_width + 10) continue;
            if (rx - lx < 50.0) continue;
            h_lines.push_back(l);
        } else if (l.is_vertical()) {
            double x = (l.x0 + l.x1) / 2.0;
            if (x < 0 || x > page_width) continue;
            double ly = std::min((double)l.y0, (double)l.y1);
            double ry = std::max((double)l.y0, (double)l.y1);
            if (ly < -10 || ry > page_height + 10) continue;
            v_lines.push_back(l);
        }
    }

    std::vector<double> h_ys;
    for (auto& hl : h_lines) h_ys.push_back((hl.y0 + hl.y1) / 2.0);
    auto row_ys = cluster_values(h_ys, 3.0);
    if (row_ys.size() < 3) return {};

    int n_levels = (int)row_ys.size();

    std::vector<bool> connected(n_levels - 1, false);
    for (int i = 0; i < n_levels - 1; i++) {
        double y_lo = row_ys[i];
        double y_hi = row_ys[i + 1];
        for (auto& vl : v_lines) {
            double vy_lo = std::min((double)vl.y0, (double)vl.y1);
            double vy_hi = std::max((double)vl.y0, (double)vl.y1);
            if (vy_lo <= y_lo + 5.0 && vy_hi >= y_hi - 5.0) {
                connected[i] = true;
                break;
            }
        }
    }

    std::vector<bool> x_overlap(n_levels - 1, false);
    for (int i = 0; i < n_levels - 1; i++) {
        x_overlap[i] = h_lines_share_full_span(h_lines, row_ys[i], row_ys[i+1], 3.0);
    }

    std::vector<std::vector<double>> table_groups;
    std::vector<double> current_group;
    int group_vline_connections = 0;
    current_group.push_back(row_ys[0]);
    for (int i = 0; i < n_levels - 1; i++) {
        double gap = row_ys[i + 1] - row_ys[i];
        bool close_enough = gap < 200.0;
        if (connected[i] || (x_overlap[i] && close_enough)) {
            current_group.push_back(row_ys[i + 1]);
            if (connected[i]) group_vline_connections++;
        } else {
            if (current_group.size() >= 3 &&
                (group_vline_connections > 0 || current_group.size() >= 7))
                table_groups.push_back(current_group);
            current_group.clear();
            group_vline_connections = 0;
            current_group.push_back(row_ys[i + 1]);
        }
    }
    if (current_group.size() >= 3 &&
        (group_vline_connections > 0 || current_group.size() >= 7))
        table_groups.push_back(current_group);

    if (table_groups.empty()) return {};

    std::vector<TableData> result;
    for (auto& group : table_groups) {
        TableData t = build_table(group, h_lines, v_lines, cache);
        if (!t.rows.empty()) {
            result.push_back(std::move(t));
        }
    }
    return result;
}

// ── Pure text-based table detection ──────────────────────

std::vector<TableData> detect_text_tables(const PageCharCache& cache,
                                           const std::vector<TableData>& existing_tables,
                                           double page_width, double page_height) {
    if (cache.chars.size() < 10) return {};

    struct CharInfo { double x, y, left, right, top, bot; int idx; unsigned int unicode; };
    std::vector<CharInfo> chars;
    chars.reserve(cache.chars.size());
    for (size_t i = 0; i < cache.chars.size(); i++) {
        auto& ch = cache.chars[i];
        if (ch.unicode == ' ' || ch.unicode == '\t' || ch.unicode == 0xA0) continue;
        if (ch.x < 0 || ch.x > page_width || ch.y < 0 || ch.y > page_height) continue;
        chars.push_back({ch.x, ch.y, ch.left, ch.right, ch.top, ch.bot, (int)i, ch.unicode});
    }
    if (chars.empty()) return {};

    std::sort(chars.begin(), chars.end(), [](const CharInfo& a, const CharInfo& b) {
        return a.y > b.y;
    });

    struct TextRow {
        double y_center;
        double y_top, y_bot;
        std::vector<std::pair<double,double>> char_ranges;
        std::vector<size_t> char_indices;
    };
    std::vector<TextRow> text_rows;
    {
        TextRow cur;
        cur.y_center = chars[0].y;
        cur.y_top = chars[0].top;
        cur.y_bot = chars[0].bot;
        cur.char_ranges.push_back({chars[0].left, chars[0].right});
        cur.char_indices.push_back(0);

        for (size_t i = 1; i < chars.size(); i++) {
            if (std::abs(chars[i].y - cur.y_center) < 3.0) {
                cur.char_ranges.push_back({chars[i].left, chars[i].right});
                cur.char_indices.push_back(i);
                cur.y_top = std::max(cur.y_top, chars[i].top);
                cur.y_bot = std::min(cur.y_bot, chars[i].bot);
                cur.y_center = (cur.y_center * (cur.char_ranges.size() - 1) + chars[i].y)
                               / cur.char_ranges.size();
            } else {
                if (!cur.char_ranges.empty()) text_rows.push_back(std::move(cur));
                cur = TextRow();
                cur.y_center = chars[i].y;
                cur.y_top = chars[i].top;
                cur.y_bot = chars[i].bot;
                cur.char_ranges.push_back({chars[i].left, chars[i].right});
                cur.char_indices.push_back(i);
            }
        }
        if (!cur.char_ranges.empty()) text_rows.push_back(std::move(cur));
    }

    if (text_rows.size() < 3) return {};

    auto row_in_existing_table = [&](const TextRow& row) -> bool {
        for (auto& t : existing_tables) {
            double t_bottom = std::min(t.y0, t.y1) - 5.0;
            double t_top = std::max(t.y0, t.y1) + 5.0;
            if (row.y_center >= t_bottom && row.y_center <= t_top)
                return true;
        }
        return false;
    };

    struct RowSpans {
        double y_center, y_top, y_bot;
        double x_min, x_max;
        std::vector<std::pair<double,double>> spans;
        std::vector<size_t> char_indices;
    };

    std::vector<RowSpans> row_spans;
    for (size_t ri = 0; ri < text_rows.size(); ri++) {
        auto& tr = text_rows[ri];
        if (row_in_existing_table(tr)) continue;

        RowSpans rs;
        rs.y_center = tr.y_center;
        rs.y_top = tr.y_top;
        rs.y_bot = tr.y_bot;
        rs.char_indices = std::move(tr.char_indices);

        auto ranges = tr.char_ranges;
        rs.spans = merge_char_ranges(ranges);

        rs.x_min = rs.spans.front().first;
        rs.x_max = rs.spans.back().second;
        row_spans.push_back(std::move(rs));
    }

    if (row_spans.size() < 3) return {};

    auto get_gaps = [](const RowSpans& rs) -> std::vector<double> {
        std::vector<double> gaps;
        for (size_t i = 1; i < rs.spans.size(); i++) {
            double gap_mid = (rs.spans[i-1].second + rs.spans[i].first) / 2.0;
            gaps.push_back(gap_mid);
        }
        return gaps;
    };

    std::vector<TableData> result;
    size_t start = 0;
    while (start < row_spans.size()) {
        if (row_spans[start].spans.size() < 2) { start++; continue; }

        std::vector<size_t> group_indices;
        group_indices.push_back(start);

        auto ref_gaps = get_gaps(row_spans[start]);
        double ref_xmin = row_spans[start].x_min;
        double ref_xmax = row_spans[start].x_max;

        for (size_t j = start + 1; j < row_spans.size(); j++) {
            auto& rs = row_spans[j];

            double y_gap = std::abs(row_spans[group_indices.back()].y_center - rs.y_center);
            if (y_gap > 40.0) break;

            if (rs.spans.size() == 1) {
                double sp_mid = (rs.spans[0].first + rs.spans[0].second) / 2.0;
                if (sp_mid >= ref_xmin - 5 && sp_mid <= ref_xmax + 5) {
                    group_indices.push_back(j);
                    continue;
                }
                break;
            }

            double overlap_l = std::max(ref_xmin, rs.x_min);
            double overlap_r = std::min(ref_xmax, rs.x_max);
            double extent = std::max(ref_xmax - ref_xmin, rs.x_max - rs.x_min);
            if (extent < 50) break;
            if (overlap_r - overlap_l < extent * 0.5) break;

            auto cur_gaps = get_gaps(rs);
            if (cur_gaps.size() > 0) {
                int matching = 0;
                for (auto& cg : cur_gaps) {
                    for (auto& rg : ref_gaps) {
                        if (std::abs(cg - rg) < 15.0) { matching++; break; }
                    }
                }
                int min_gaps = (int)std::min(cur_gaps.size(), ref_gaps.size());
                if (min_gaps > 0 && matching >= (min_gaps + 1) / 2) {
                    group_indices.push_back(j);
                    ref_xmin = std::min(ref_xmin, rs.x_min);
                    ref_xmax = std::max(ref_xmax, rs.x_max);
                    continue;
                }
            }

            break;
        }

        if (group_indices.size() < 3) { start++; continue; }

        std::vector<double> all_gaps;
        int multi_span_rows = 0;
        for (auto idx : group_indices) {
            auto& rs = row_spans[idx];
            if (rs.spans.size() < 2) continue;
            multi_span_rows++;
            for (size_t s = 1; s < rs.spans.size(); s++) {
                double gap_width = rs.spans[s].first - rs.spans[s-1].second;
                if (gap_width >= 20.0) {
                    double gap_mid = (rs.spans[s-1].second + rs.spans[s].first) / 2.0;
                    all_gaps.push_back(gap_mid);
                }
            }
        }

        if (all_gaps.empty() || multi_span_rows < 2) {
            start = group_indices.back() + 1; continue;
        }

        std::sort(all_gaps.begin(), all_gaps.end());
        std::vector<double> col_boundaries;
        col_boundaries.push_back(ref_xmin);

        double cluster_sum = all_gaps[0];
        int cluster_count = 1;
        for (size_t i = 1; i < all_gaps.size(); i++) {
            if (all_gaps[i] - (cluster_sum / cluster_count) < 15.0) {
                cluster_sum += all_gaps[i];
                cluster_count++;
            } else {
                if (cluster_count >= std::max(2, (int)(multi_span_rows * 0.5))) {
                    col_boundaries.push_back(cluster_sum / cluster_count);
                }
                cluster_sum = all_gaps[i];
                cluster_count = 1;
            }
        }
        if (cluster_count >= std::max(2, (int)(multi_span_rows * 0.5))) {
            col_boundaries.push_back(cluster_sum / cluster_count);
        }
        col_boundaries.push_back(ref_xmax);

        int n_cols = (int)col_boundaries.size() - 1;
        if (n_cols < 2) { start = group_indices.back() + 1; continue; }

        if (n_cols == 2 && multi_span_rows >= 3) {
            if (cluster_count < (int)(multi_span_rows * 0.7)) {
                start = group_indices.back() + 1; continue;
            }
        }

        TableData table;
        table.y0 = row_spans[group_indices.back()].y_bot;
        table.y1 = row_spans[group_indices.front()].y_top;
        table.x0 = ref_xmin;
        table.x1 = ref_xmax;

        for (auto idx : group_indices) {
            auto& rs = row_spans[idx];
            std::vector<std::string> row(n_cols);

            auto sorted_ci = rs.char_indices;
            std::sort(sorted_ci.begin(), sorted_ci.end(), [&](size_t a, size_t b) {
                return chars[a].x < chars[b].x;
            });

            for (size_t ci : sorted_ci) {
                auto& ch = chars[ci];
                int col = -1;
                for (int c = 0; c < n_cols; c++) {
                    if (ch.x >= col_boundaries[c] - 2 && ch.x <= col_boundaries[c+1] + 2) {
                        col = c;
                        break;
                    }
                }
                if (col < 0) continue;
                util::append_utf8(row[col], ch.unicode);
            }

            for (int c = 0; c < n_cols; c++) {
                size_t s = row[c].find_first_not_of(" \t");
                size_t e = row[c].find_last_not_of(" \t");
                if (s != std::string::npos)
                    row[c] = row[c].substr(s, e - s + 1);
                else
                    row[c].clear();
            }
            table.rows.push_back(std::move(row));
        }

        // Early reject: check text continuity across columns
        {
            auto is_content_byte = [](unsigned char c) -> bool {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c >= 0x80;
            };
            int continuation_rows = 0;
            int checked_rows = 0;
            for (auto& r : table.rows) {
                if ((int)r.size() < n_cols) continue;
                int pairs_checked = 0;
                int pairs_continued = 0;
                for (int c = 0; c + 1 < n_cols; c++) {
                    if (r[c].empty() || r[c+1].empty()) continue;
                    pairs_checked++;
                    if (is_content_byte((unsigned char)r[c].back()) &&
                        is_content_byte((unsigned char)r[c+1][0]))
                        pairs_continued++;
                }
                checked_rows++;
                if (pairs_checked > 0 && pairs_continued > pairs_checked / 2)
                    continuation_rows++;
            }
            // For 2-column tables, even a single continuation row is suspicious
            double cont_threshold = (n_cols == 2) ? 0.15 : 0.3;
            if (checked_rows >= 2 && continuation_rows >= checked_rows * cont_threshold) {
                start = group_indices.back() + 1;
                continue;
            }
        }

        // Merge continuation rows
        for (size_t r = 1; r < table.rows.size(); r++) {
            int filled = 0;
            int filled_col = -1;
            for (int c = 0; c < n_cols; c++) {
                if (!table.rows[r][c].empty()) { filled++; filled_col = c; }
            }
            if (filled == 1 && filled_col >= 0 && r > 0) {
                if (!table.rows[r-1][filled_col].empty())
                    table.rows[r-1][filled_col] += " ";
                table.rows[r-1][filled_col] += table.rows[r][filled_col];
                table.rows.erase(table.rows.begin() + r);
                r--;
            }
        }

        int meaningful_rows = 0;
        for (auto& row : table.rows) {
            int filled = 0;
            for (auto& c : row) if (!c.empty()) filled++;
            if (filled >= 2) meaningful_rows++;
        }

        int min_rows = (n_cols == 2) ? 5 : 3;
        if (meaningful_rows >= min_rows && n_cols <= 4) {
            bool looks_like_list = false;

            {
                int marker_rows = 0;
                int content_rows = 0;
                for (auto& row : table.rows) {
                    bool has_content = false;
                    for (auto& c : row) if (!c.empty()) { has_content = true; break; }
                    if (!has_content) continue;
                    content_rows++;
                    std::string first;
                    for (auto& c : row) if (!c.empty()) { first = c; break; }
                    if (!first.empty() && first[0] >= '0' && first[0] <= '9') {
                        for (size_t k = 1; k < first.size() && k < 4; k++) {
                            if (first[k] == ')') { marker_rows++; break; }
                            if (first[k] < '0' || first[k] > '9') break;
                        }
                    }
                }
                if (content_rows >= 2 && marker_rows >= content_rows * 0.6)
                    looks_like_list = true;
            }

            if (!looks_like_list && n_cols == 2) {
                int short_label_rows = 0;
                int long_content_rows = 0;
                for (auto& row : table.rows) {
                    if (row.size() >= 2 && row[0].size() <= 6 && !row[0].empty())
                        short_label_rows++;
                    if (row.size() >= 2 && row[1].size() > 15)
                        long_content_rows++;
                }
                int valid_rows = (int)table.rows.size();
                if (valid_rows >= 2 &&
                    short_label_rows >= valid_rows * 0.5 &&
                    long_content_rows >= valid_rows * 0.5) {
                    looks_like_list = true;
                }
            }

            if (!looks_like_list && n_cols == 2) {
                int korean_label_rows = 0;
                for (auto& row : table.rows) {
                    if (row.size() < 2 || row[0].empty()) continue;
                    if (row[0].size() <= 6) {
                        unsigned char c0 = (unsigned char)row[0][0];
                        bool is_hangul = (c0 >= 0xEA && c0 <= 0xED);
                        bool ends_period = (row[0].back() == '.');
                        if (is_hangul && ends_period) korean_label_rows++;
                    }
                }
                int valid_rows = (int)table.rows.size();
                if (valid_rows >= 2 && korean_label_rows >= valid_rows * 0.4)
                    looks_like_list = true;
            }

            if (!looks_like_list && n_cols == 2) {
                int empty_first_col = 0;
                for (auto& row : table.rows)
                    if (row.size() >= 2 && row[0].empty() && !row[1].empty())
                        empty_first_col++;
                if ((int)table.rows.size() >= 3 &&
                    empty_first_col >= (int)table.rows.size() * 0.4)
                    looks_like_list = true;
            }

            if (!looks_like_list && !table.rows.empty()) {
                bool header_only_punct = true;
                for (auto& cell : table.rows[0]) {
                    for (char c : cell) {
                        if (c != '.' && c != ',' && c != ' ' && c != '\t' &&
                            c != ';' && c != ':' && c != '-')
                            { header_only_punct = false; break; }
                    }
                    if (!header_only_punct) break;
                }
                if (header_only_punct) looks_like_list = true;
            }

            if (!looks_like_list) {
                auto is_content_char = [](unsigned char c) -> bool {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c >= 0x80;
                };
                auto is_filler_only = [](const std::string& s) -> bool {
                    for (char c : s)
                        if (c != '.' && c != ',' && c != ' ' && c != '\t' &&
                            c != ';' && c != ':' && c != '-')
                            return false;
                    return true;
                };

                int continuation_rows = 0;
                int filler_cells = 0;
                int total_cells = 0;
                int checked_rows = 0;

                for (auto& row : table.rows) {
                    if ((int)row.size() < n_cols) continue;

                    for (auto& cell : row) {
                        if (cell.empty()) continue;
                        total_cells++;
                        if (is_filler_only(cell)) filler_cells++;
                    }

                    bool row_is_continuation = false;
                    int pairs_checked = 0;
                    int pairs_continued = 0;
                    for (int c = 0; c + 1 < n_cols; c++) {
                        if (row[c].empty() || row[c+1].empty()) continue;
                        unsigned char left_last = (unsigned char)row[c].back();
                        unsigned char right_first = (unsigned char)row[c+1][0];
                        pairs_checked++;
                        if (is_content_char(left_last) && is_content_char(right_first))
                            pairs_continued++;
                    }
                    if (pairs_checked > 0 && pairs_continued > pairs_checked / 2)
                        row_is_continuation = true;

                    if (row_is_continuation) continuation_rows++;
                    checked_rows++;
                }

                double ct = (n_cols == 2) ? 0.15 : 0.3;
                if (checked_rows >= 2 && continuation_rows >= checked_rows * ct)
                    looks_like_list = true;

                if (!looks_like_list && n_cols >= 3) {
                    int tiny_cells = 0;
                    int non_empty_cells = 0;
                    for (auto& row : table.rows) {
                        for (auto& cell : row) {
                            if (cell.empty()) continue;
                            if (is_filler_only(cell)) continue;
                            non_empty_cells++;
                            if (cell.size() <= 6) tiny_cells++;
                        }
                    }
                    if (non_empty_cells >= 4 && tiny_cells >= non_empty_cells * 0.4)
                        looks_like_list = true;
                }

                if (total_cells > 0 && filler_cells >= total_cells * 0.15)
                    looks_like_list = true;
            }

            // Reject 2-3 column "tables" where first column is much longer than second
            // (body text split at page margin)
            if (!looks_like_list && n_cols <= 3) {
                int total_rows = 0;
                double sum_first = 0, sum_second = 0;
                int unbalanced = 0;
                for (auto& row : table.rows) {
                    if (row.empty() || row[0].empty()) continue;
                    total_rows++;
                    sum_first += row[0].size();
                    if (n_cols >= 2 && row.size() >= 2) sum_second += row[1].size();
                    // First column > 3x second column
                    if (n_cols == 2 && row.size() >= 2 && !row[1].empty() &&
                        row[0].size() > row[1].size() * 3)
                        unbalanced++;
                }
                if (total_rows >= 3) {
                    double avg_first = sum_first / total_rows;
                    double avg_second = (n_cols >= 2) ? sum_second / total_rows : 0;
                    // First column avg > 30 chars and 3x larger than second → body text
                    if (avg_first > 30 && avg_second > 0 && avg_first > avg_second * 2.5)
                        looks_like_list = true;
                    // >40% rows have heavily unbalanced columns
                    if (unbalanced >= total_rows * 0.4)
                        looks_like_list = true;
                }
            }

            if (!looks_like_list) {
                trim_table(table);
                result.push_back(std::move(table));
            }
        }

        start = group_indices.back() + 1;
    }

    return result;
}

std::string format_table(const TableData& table) {
    if (table.rows.empty()) return "";

    std::vector<std::vector<std::string>> filtered;
    for (size_t r = 0; r < table.rows.size(); r++) {
        bool all_empty = true;
        for (auto& cell : table.rows[r])
            if (!cell.empty()) { all_empty = false; break; }
        if (!all_empty || r == 0)
            filtered.push_back(table.rows[r]);
    }
    if (filtered.empty()) return "";

    int n_cols = filtered[0].size();
    if (n_cols == 0) return "";

    std::vector<size_t> widths(n_cols, 3);
    for (auto& row : filtered)
        for (int c = 0; c < n_cols && c < (int)row.size(); c++)
            widths[c] = std::max(widths[c], row[c].size());

    std::string md;
    for (size_t r = 0; r < filtered.size(); r++) {
        md += "|";
        for (int c = 0; c < n_cols; c++) {
            std::string cell = (c < (int)filtered[r].size()) ? filtered[r][c] : "";
            md += " " + cell;
            for (size_t p = cell.size(); p < widths[c]; p++) md += ' ';
            md += " |";
        }
        md += '\n';

        if (r == 0) {
            md += "|";
            for (int c = 0; c < n_cols; c++) {
                md += " ";
                for (size_t p = 0; p < widths[c]; p++) md += '-';
                md += " |";
            }
            md += '\n';
        }
    }
    return md;
}

// ── CCITTFax Decoder (lookup-table based, algorithm from ITU-T T.4/T.6) ──
// Huffman lookup tables and algorithms derived from the ITU-T T.4/T.6 standards.

struct HuffNode { short val; short nbits; };

// 2D mode codes
enum { PASS = -4, HORIZ = -5, VR3 = 0, VR2 = 1, VR1 = 2, V0 = 3, VL1 = 4, VL2 = 5, VL3 = 6, HUFF_ERROR = -1, HUFF_ZEROS = -2 };

// White Huffman table (8-bit initial lookup)
static const HuffNode kWhiteHuff[] = {
    {256,12},{272,12},{29,8},{30,8},{45,8},{46,8},{22,7},{22,7},
    {23,7},{23,7},{47,8},{48,8},{13,6},{13,6},{13,6},{13,6},{20,7},
    {20,7},{33,8},{34,8},{35,8},{36,8},{37,8},{38,8},{19,7},{19,7},
    {31,8},{32,8},{1,6},{1,6},{1,6},{1,6},{12,6},{12,6},{12,6},{12,6},
    {53,8},{54,8},{26,7},{26,7},{39,8},{40,8},{41,8},{42,8},{43,8},
    {44,8},{21,7},{21,7},{28,7},{28,7},{61,8},{62,8},{63,8},{0,8},
    {320,8},{384,8},{10,5},{10,5},{10,5},{10,5},{10,5},{10,5},{10,5},
    {10,5},{11,5},{11,5},{11,5},{11,5},{11,5},{11,5},{11,5},{11,5},
    {27,7},{27,7},{59,8},{60,8},{288,9},{290,9},{18,7},{18,7},{24,7},
    {24,7},{49,8},{50,8},{51,8},{52,8},{25,7},{25,7},{55,8},{56,8},
    {57,8},{58,8},{192,6},{192,6},{192,6},{192,6},{1664,6},{1664,6},
    {1664,6},{1664,6},{448,8},{512,8},{292,9},{640,8},{576,8},{294,9},
    {296,9},{298,9},{300,9},{302,9},{256,7},{256,7},{2,4},{2,4},{2,4},
    {2,4},{2,4},{2,4},{2,4},{2,4},{2,4},{2,4},{2,4},{2,4},{2,4},{2,4},
    {2,4},{2,4},{3,4},{3,4},{3,4},{3,4},{3,4},{3,4},{3,4},{3,4},{3,4},
    {3,4},{3,4},{3,4},{3,4},{3,4},{3,4},{3,4},{128,5},{128,5},{128,5},
    {128,5},{128,5},{128,5},{128,5},{128,5},{8,5},{8,5},{8,5},{8,5},
    {8,5},{8,5},{8,5},{8,5},{9,5},{9,5},{9,5},{9,5},{9,5},{9,5},{9,5},
    {9,5},{16,6},{16,6},{16,6},{16,6},{17,6},{17,6},{17,6},{17,6},
    {4,4},{4,4},{4,4},{4,4},{4,4},{4,4},{4,4},{4,4},{4,4},{4,4},{4,4},
    {4,4},{4,4},{4,4},{4,4},{4,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},
    {5,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},
    {14,6},{14,6},{14,6},{14,6},{15,6},{15,6},{15,6},{15,6},{64,5},
    {64,5},{64,5},{64,5},{64,5},{64,5},{64,5},{64,5},{6,4},{6,4},
    {6,4},{6,4},{6,4},{6,4},{6,4},{6,4},{6,4},{6,4},{6,4},{6,4},{6,4},
    {6,4},{6,4},{6,4},{7,4},{7,4},{7,4},{7,4},{7,4},{7,4},{7,4},{7,4},
    {7,4},{7,4},{7,4},{7,4},{7,4},{7,4},{7,4},{7,4},{-2,3},{-2,3},
    {-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},
    {-1,0},{-1,0},{-1,0},{-1,0},{-3,4},{1792,3},{1792,3},{1984,4},
    {2048,4},{2112,4},{2176,4},{2240,4},{2304,4},{1856,3},{1856,3},
    {1920,3},{1920,3},{2368,4},{2432,4},{2496,4},{2560,4},{1472,1},
    {1536,1},{1600,1},{1728,1},{704,1},{768,1},{832,1},{896,1},
    {960,1},{1024,1},{1088,1},{1152,1},{1216,1},{1280,1},{1344,1},
    {1408,1}
};

// Black Huffman table (7-bit initial lookup)
static const HuffNode kBlackHuff[] = {
    {128,12},{160,13},{224,12},{256,12},{10,7},{11,7},{288,12},{12,7},
    {9,6},{9,6},{8,6},{8,6},{7,5},{7,5},{7,5},{7,5},{6,4},{6,4},{6,4},
    {6,4},{6,4},{6,4},{6,4},{6,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},
    {5,4},{5,4},{1,3},{1,3},{1,3},{1,3},{1,3},{1,3},{1,3},{1,3},{1,3},
    {1,3},{1,3},{1,3},{1,3},{1,3},{1,3},{1,3},{4,3},{4,3},{4,3},{4,3},
    {4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},
    {4,3},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},
    {3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},
    {3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},
    {2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},
    {2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},
    {2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},
    {-2,4},{-2,4},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},
    {-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-3,5},{1792,4},
    {1792,4},{1984,5},{2048,5},{2112,5},{2176,5},{2240,5},{2304,5},
    {1856,4},{1856,4},{1920,4},{1920,4},{2368,5},{2432,5},{2496,5},
    {2560,5},{18,3},{18,3},{18,3},{18,3},{18,3},{18,3},{18,3},{18,3},
    {52,5},{52,5},{640,6},{704,6},{768,6},{832,6},{55,5},{55,5},
    {56,5},{56,5},{1280,6},{1344,6},{1408,6},{1472,6},{59,5},{59,5},
    {60,5},{60,5},{1536,6},{1600,6},{24,4},{24,4},{24,4},{24,4},
    {25,4},{25,4},{25,4},{25,4},{1664,6},{1728,6},{320,5},{320,5},
    {384,5},{384,5},{448,5},{448,5},{512,6},{576,6},{53,5},{53,5},
    {54,5},{54,5},{896,6},{960,6},{1024,6},{1088,6},{1152,6},{1216,6},
    {64,3},{64,3},{64,3},{64,3},{64,3},{64,3},{64,3},{64,3},{13,1},
    {13,1},{13,1},{13,1},{13,1},{13,1},{13,1},{13,1},{13,1},{13,1},
    {13,1},{13,1},{13,1},{13,1},{13,1},{13,1},{23,4},{23,4},{50,5},
    {51,5},{44,5},{45,5},{46,5},{47,5},{57,5},{58,5},{61,5},{256,5},
    {16,3},{16,3},{16,3},{16,3},{17,3},{17,3},{17,3},{17,3},{48,5},
    {49,5},{62,5},{63,5},{30,5},{31,5},{32,5},{33,5},{40,5},{41,5},
    {22,4},{22,4},{14,1},{14,1},{14,1},{14,1},{14,1},{14,1},{14,1},
    {14,1},{14,1},{14,1},{14,1},{14,1},{14,1},{14,1},{14,1},{14,1},
    {15,2},{15,2},{15,2},{15,2},{15,2},{15,2},{15,2},{15,2},{128,5},
    {192,5},{26,5},{27,5},{28,5},{29,5},{19,4},{19,4},{20,4},{20,4},
    {34,5},{35,5},{36,5},{37,5},{38,5},{39,5},{21,4},{21,4},{42,5},
    {43,5},{0,3},{0,3},{0,3},{0,3}
};

// 2D mode Huffman table (7-bit initial lookup)
static const HuffNode k2dHuff[] = {
    {128,11},{144,10},{6,7},{0,7},{5,6},{5,6},{1,6},{1,6},{-4,4},
    {-4,4},{-4,4},{-4,4},{-4,4},{-4,4},{-4,4},{-4,4},{-5,3},{-5,3},
    {-5,3},{-5,3},{-5,3},{-5,3},{-5,3},{-5,3},{-5,3},{-5,3},{-5,3},
    {-5,3},{-5,3},{-5,3},{-5,3},{-5,3},{4,3},{4,3},{4,3},{4,3},{4,3},
    {4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},
    {2,3},{2,3},{2,3},{2,3},{2,3},{2,3},{2,3},{2,3},{2,3},{2,3},{2,3},
    {2,3},{2,3},{2,3},{2,3},{2,3},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{-2,4},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},
    {-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},
    {-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-3,3}
};

static const uint8_t kClzTable[256] = {
    8,7,6,6,5,5,5,5,4,4,4,4,4,4,4,4,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static const uint8_t kTailMask[8] = {0x7F,0x3F,0x1F,0x0F,0x07,0x03,0x01,0x00};
static const uint8_t kLmask[8] = {0xFF,0x7F,0x3F,0x1F,0x0F,0x07,0x03,0x01};
static const uint8_t kRmask[8] = {0x00,0x80,0xC0,0xE0,0xF0,0xF8,0xFC,0xFE};

static inline int get_bit(const uint8_t* buf, int x) {
    return (buf[x >> 3] >> (7 - (x & 7))) & 1;
}

static inline void set_bits(uint8_t* line, int x0, int x1) {
    if (x1 <= x0) return;
    int a0 = x0 >> 3, a1 = x1 >> 3, b0 = x0 & 7, b1 = x1 & 7;
    if (a0 == a1) { if (b1) line[a0] |= kLmask[b0] & kRmask[b1]; }
    else {
        line[a0] |= kLmask[b0];
        for (int a = a0 + 1; a < a1; a++) line[a] = 0xFF;
        if (b1) line[a1] |= kRmask[b1];
    }
}

static int next_edge(const uint8_t* line, int x, int w) {
    if (!line) return w;
    int m;
    if (x < 0) { x = 0; m = 0xFF; }
    else { m = kTailMask[x & 7]; }
    int W = w >> 3;
    int xb = x >> 3;
    int a = line[xb];
    int b = (a ^ (a >> 1)) & m;
    if (xb >= W) { int r = (xb << 3) + kClzTable[b]; return r > w ? w : r; }
    while (b == 0) {
        if (++xb >= W) goto nearend;
        int prev_lsb = a & 1;
        a = line[xb];
        b = (prev_lsb << 7) ^ a ^ (a >> 1);
    }
    return (xb << 3) + kClzTable[b];
nearend:
    if ((xb << 3) == w) return w;
    { int prev_lsb = a & 1; a = line[xb]; b = (prev_lsb << 7) ^ a ^ (a >> 1); }
    { int r = (xb << 3) + kClzTable[b]; return r > w ? w : r; }
}

static int next_color_edge(const uint8_t* line, int x, int w, int color) {
    if (!line || x >= w) return w;
    x = next_edge(line, (x > 0 || !color) ? x : -1, w);
    if (x < w && get_bit(line, x) != color)
        x = next_edge(line, x, w);
    return x;
}

// ── New CCITTFax decoder using lookup tables ──

struct BitStream {
    const uint8_t* src;
    size_t src_len;
    uint32_t word;
    int bidx; // bits consumed from word (32-bidx = bits available)
    size_t byte_pos;

    BitStream(const uint8_t* s, size_t l) : src(s), src_len(l), word(0), bidx(32), byte_pos(0) {
        fill();
    }

    void fill() {
        while (bidx > (32 - 13) && byte_pos < src_len) {
            bidx -= 8;
            word |= static_cast<uint32_t>(src[byte_pos++]) << bidx;
        }
    }

    void eat(int n) { word <<= n; bidx += n; }

    int get_code(const HuffNode* table, int initial_bits) {
        fill();
        int tidx = word >> (32 - initial_bits);
        int val = table[tidx].val;
        int nbits = table[tidx].nbits;
        if (nbits > initial_bits) {
            uint32_t wordmask = (1u << (32 - initial_bits)) - 1;
            tidx = val + ((word & wordmask) >> (32 - nbits));
            val = table[tidx].val;
            nbits = initial_bits + table[tidx].nbits;
        }
        eat(nbits);
        return val;
    }

    int get_run(int color) {
        // Decode one 1D run (makeup + terminating)
        int total = 0;
        for (;;) {
            int code;
            if (color == 0) // white
                code = get_code(kWhiteHuff, 8);
            else
                code = get_code(kBlackHuff, 7);
            if (code < 0) return total; // error
            total += code;
            if (code < 64) break; // terminating code
        }
        return total;
    }
};

std::vector<uint8_t> decode_ccitt(const uint8_t* src, size_t src_len,
                                    int k_param, int columns, bool black_is_1) {
    if (columns <= 0) columns = 1728;
    int stride = (columns + 7) / 8;
    std::vector<uint8_t> out;
    BitStream st(src, src_len);

    std::vector<uint8_t> ref(stride, 0);  // reference line (all white)
    std::vector<uint8_t> dst(stride, 0);  // current line

    int max_rows = 100000;

    if (k_param == 0) {
        // Group 3, 1D
        while (max_rows-- > 0 && st.byte_pos < st.src_len) {
            std::memset(dst.data(), 0, stride);
            int a = 0, c = 0; // position, color (0=white)
            while (a < columns) {
                int run = st.get_run(c);
                if (run < 0) break;
                if (c) set_bits(dst.data(), a, std::min(a + run, columns));
                a += run;
                c = !c;
            }
            out.insert(out.end(), dst.begin(), dst.end());
        }
    } else if (k_param < 0) {
        // Group 4, 2D
        while (max_rows-- > 0 && st.byte_pos < st.src_len) {
            std::memset(dst.data(), 0, stride);
            int a = 0, c = 0; // position, color (0=white, 1=black)

            while (a < columns) {
                st.fill();

                int code = st.get_code(k2dHuff, 7);

                if (code == HORIZ) {
                    // Horizontal mode: read two 1D runs
                    if (a < 0) a = 0;
                    int run1 = st.get_run(c);
                    if (c) set_bits(dst.data(), a, std::min(a + run1, columns));
                    a += run1;
                    if (run1 < 64 || (run1 >= 64 && a <= columns)) c = !c;
                    else continue;

                    int run2 = st.get_run(c);
                    if (c) set_bits(dst.data(), a, std::min(a + run2, columns));
                    a += run2;
                    if (run2 < 64 || (run2 >= 64 && a <= columns)) c = !c;
                    else continue;
                    // After H mode: color is back to original
                    // (toggled twice = same as start)
                    continue; // don't toggle again below
                }

                if (code == PASS) {
                    // Pass mode
                    int b1 = next_color_edge(ref.data(), a, columns, !c);
                    int b2 = (b1 >= columns) ? columns : next_edge(ref.data(), b1, columns);
                    if (c) set_bits(dst.data(), a, b2);
                    a = b2;
                    continue;
                }

                // Vertical modes: V0, VR1-3, VL1-3
                int offset = 0;
                switch (code) {
                    case V0:  offset = 0; break;
                    case VR1: offset = 1; break;
                    case VR2: offset = 2; break;
                    case VR3: offset = 3; break;
                    case VL1: offset = -1; break;
                    case VL2: offset = -2; break;
                    case VL3: offset = -3; break;
                    default: goto done_line; // error/EOL
                }

                {
                    int b1 = next_color_edge(ref.data(), a, columns, !c) + offset;
                    if (b1 < 0) b1 = 0;
                    if (b1 > columns) b1 = columns;
                    if (c) set_bits(dst.data(), a, b1);
                    a = b1;
                    c = !c;
                }
            }

            done_line:
            out.insert(out.end(), dst.begin(), dst.end());
            std::memcpy(ref.data(), dst.data(), stride);
        }
    }

    // Output convention: 1-bits = black pixels (ITU-T standard).
    // Caller handles BlackIs1 and ImageMask interpretation.
    return out;
}

// ── JPEG Decoder ─────────────────────────────────────────
// (moved here — old CCITTFax tables removed)

// Marker: OLD_CCITT_START — everything below until OLD_CCITT_END was removed
// ── JPEG Decoder ─────────────────────────────────────────

struct JpegResult {
    std::vector<uint8_t> pixels;
    int width = 0, height = 0, components = 0;
};

struct JpegErrorMgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    char buf[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buf);
    longjmp(err->setjmp_buffer, 1);
}

JpegResult jpeg_decode(const uint8_t* data, size_t len) {
    JpegResult result;
    struct jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return result;
    }

    jpeg_create_decompress(&cinfo);
    // Custom memory source manager for all libjpeg versions
    {
        auto* src = static_cast<struct jpeg_source_mgr*>(
            (*cinfo.mem->alloc_small)(reinterpret_cast<j_common_ptr>(&cinfo),
                                       JPOOL_PERMANENT, sizeof(struct jpeg_source_mgr)));
        cinfo.src = src;
        src->next_input_byte = data;
        src->bytes_in_buffer = len;
        src->init_source = [](j_decompress_ptr) {};
        src->fill_input_buffer = [](j_decompress_ptr cinfo) -> boolean {
            // Insert fake EOI marker
            static const JOCTET eoi[2] = {0xFF, JPEG_EOI};
            cinfo->src->next_input_byte = eoi;
            cinfo->src->bytes_in_buffer = 2;
            return TRUE;
        };
        src->skip_input_data = [](j_decompress_ptr cinfo, long num_bytes) {
            if (num_bytes > 0) {
                size_t skip = static_cast<size_t>(num_bytes);
                if (skip > cinfo->src->bytes_in_buffer) skip = cinfo->src->bytes_in_buffer;
                cinfo->src->next_input_byte += skip;
                cinfo->src->bytes_in_buffer -= skip;
            }
        };
        src->resync_to_restart = jpeg_resync_to_restart;
        src->term_source = [](j_decompress_ptr) {};
    }
    jpeg_read_header(&cinfo, TRUE);

    if (cinfo.num_components == 4)
        cinfo.out_color_space = JCS_CMYK;
    else
        cinfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&cinfo);

    result.width = cinfo.output_width;
    result.height = cinfo.output_height;
    result.components = cinfo.output_components;
    int row_stride = result.width * result.components;
    result.pixels.resize(static_cast<size_t>(row_stride) * result.height);

    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* row_ptr = result.pixels.data() + cinfo.output_scanline * row_stride;
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return result;
}

// ── PNG Writer ───────────────────────────────────────────

static void png_put32(std::vector<char>& v, uint32_t val) {
    char b[4] = {static_cast<char>((val >> 24) & 0xFF),
                 static_cast<char>((val >> 16) & 0xFF),
                 static_cast<char>((val >> 8) & 0xFF),
                 static_cast<char>(val & 0xFF)};
    v.insert(v.end(), b, b + 4);
}

static void png_write_chunk(std::vector<char>& out, const char type[4],
                             const uint8_t* data, uint32_t len) {
    png_put32(out, len);
    size_t type_pos = out.size();
    out.insert(out.end(), type, type + 4);
    if (data && len > 0)
        out.insert(out.end(), reinterpret_cast<const char*>(data),
                   reinterpret_cast<const char*>(data) + len);
    uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(&out[type_pos]),
                         4 + len);
    png_put32(out, crc);
}

static std::vector<char> pixels_to_png(const uint8_t* pixels, int w, int h,
                                        int components, int level = Z_BEST_SPEED) {
    if (!pixels || w <= 0 || h <= 0) return {};

    size_t row_bytes = 1 + static_cast<size_t>(w) * 3;
    std::vector<uint8_t> raw(row_bytes * h);
    for (int y = 0; y < h; y++) {
        const uint8_t* sr = pixels + y * w * components;
        uint8_t* dr = raw.data() + y * row_bytes;
        dr[0] = 0; // filter: none
        for (int x = 0; x < w; x++) {
            if (components >= 3) {
                dr[1 + x*3]     = sr[x*components];     // R
                dr[1 + x*3 + 1] = sr[x*components + 1]; // G
                dr[1 + x*3 + 2] = sr[x*components + 2]; // B
            } else if (components == 1) {
                dr[1 + x*3] = dr[1 + x*3 + 1] = dr[1 + x*3 + 2] = sr[x];
            } else if (components == 4) {
                // CMYK to RGB
                int c = sr[x*4], m = sr[x*4+1], yy = sr[x*4+2], k = sr[x*4+3];
                dr[1 + x*3]     = static_cast<uint8_t>(255 - std::min(255, c + k));
                dr[1 + x*3 + 1] = static_cast<uint8_t>(255 - std::min(255, m + k));
                dr[1 + x*3 + 2] = static_cast<uint8_t>(255 - std::min(255, yy + k));
            }
        }
    }

    uLong bound = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> deflated(bound);
    uLong deflated_size = bound;
    if (compress2(deflated.data(), &deflated_size, raw.data(),
                  static_cast<uLong>(raw.size()), level) != Z_OK)
        return {};

    raw.clear(); raw.shrink_to_fit();

    std::vector<char> png;
    png.reserve(8 + 25 + deflated_size + 24);

    const uint8_t sig[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    png.insert(png.end(), sig, sig + 8);

    uint8_t ihdr[13] = {};
    ihdr[0] = (w >> 24); ihdr[1] = (w >> 16); ihdr[2] = (w >> 8); ihdr[3] = w;
    ihdr[4] = (h >> 24); ihdr[5] = (h >> 16); ihdr[6] = (h >> 8); ihdr[7] = h;
    ihdr[8] = 8; ihdr[9] = 2; // 8-bit RGB
    png_write_chunk(png, "IHDR", ihdr, 13);
    png_write_chunk(png, "IDAT", deflated.data(), static_cast<uint32_t>(deflated_size));
    png_write_chunk(png, "IEND", nullptr, 0);

    return png;
}

// ── BMP Writer ───────────────────────────────────────────

static std::vector<char> pixels_to_bmp(const uint8_t* pixels, int w, int h,
                                        int components) {
    if (!pixels || w <= 0 || h <= 0) return {};

    int out_stride = ((w * 3 + 3) / 4) * 4;
    int pixel_data_size = out_stride * h;
    int file_size = 14 + 40 + pixel_data_size;

    std::vector<char> bmp(file_size, 0);
    auto write16 = [&](int off, uint16_t v) { memcpy(&bmp[off], &v, 2); };
    auto write32 = [&](int off, uint32_t v) { memcpy(&bmp[off], &v, 4); };

    bmp[0] = 'B'; bmp[1] = 'M';
    write32(2, static_cast<uint32_t>(file_size));
    write32(10, 14 + 40);
    write32(14, 40);
    write32(18, static_cast<uint32_t>(w));
    write32(22, static_cast<uint32_t>(h));
    write16(26, 1);
    write16(28, 24);
    write32(34, static_cast<uint32_t>(pixel_data_size));

    for (int y = 0; y < h; y++) {
        const uint8_t* src_row = pixels + (h - 1 - y) * w * components;
        char* dst_row = &bmp[14 + 40 + y * out_stride];
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            if (components == 1) {
                r = g = b = src_row[x];
            } else if (components == 3) {
                r = src_row[x * 3]; g = src_row[x * 3 + 1]; b = src_row[x * 3 + 2];
            } else if (components == 4) {
                int c = src_row[x*4], m = src_row[x*4+1], yy = src_row[x*4+2], k = src_row[x*4+3];
                r = static_cast<uint8_t>(255 - std::min(255, c + k));
                g = static_cast<uint8_t>(255 - std::min(255, m + k));
                b = static_cast<uint8_t>(255 - std::min(255, yy + k));
            } else {
                r = g = b = src_row[x * components];
            }
            dst_row[x * 3 + 0] = static_cast<char>(b);
            dst_row[x * 3 + 1] = static_cast<char>(g);
            dst_row[x * 3 + 2] = static_cast<char>(r);
        }
    }
    return bmp;
}

// ── Image Extraction ─────────────────────────────────────

struct ExtractedImage {
    ImageData img;
    double ctm[6];
};

std::vector<ExtractedImage> extract_page_images(PdfDoc& doc, const PdfObj& page_obj,
                                                 const ContentParseResult& parse_result,
                                                 int page_num,
                                                 const std::string& output_dir) {
    std::vector<ExtractedImage> images;
    int img_idx = 0;

    auto res = doc.resolve(page_obj.get("Resources"));

    for (auto& ip : parse_result.images) {
        PdfObj xobj;
        if (ip.xobj_ref >= 0) {
            xobj = doc.get_obj(ip.xobj_ref);
        } else if (!ip.xobj_name.empty()) {
            auto& xobjects = res.get("XObject");
            auto xd = doc.resolve(xobjects);
            if (xd.is_dict()) xobj = doc.resolve(xd.get(ip.xobj_name));
        }

        if (!xobj.is_stream()) continue;

        auto& subtype = xobj.get("Subtype");
        if (!subtype.is_name() || subtype.str_val != "Image") continue;

        int w = xobj.get("Width").as_int();
        int h = xobj.get("Height").as_int();
        if (w <= 0 || h <= 0) continue;

        ImageData img;
        img.page_number = page_num;
        img.name = "page" + std::to_string(page_num + 1) + "_img" + std::to_string(img_idx);
        img.width = w;
        img.height = h;

        // Determine filter chain
        auto filter_obj = doc.resolve(xobj.get("Filter"));
        std::string last_filter;
        bool single_filter = false;
        if (filter_obj.is_name()) {
            last_filter = filter_obj.str_val;
            single_filter = true;
        } else if (filter_obj.is_arr() && !filter_obj.arr.empty()) {
            auto last = doc.resolve(filter_obj.arr.back());
            if (last.is_name()) last_filter = last.str_val;
            single_filter = (filter_obj.arr.size() == 1);
        }

        if (last_filter == "DCTDecode") {
            if (single_filter) {
                // JPEG passthrough — raw bytes are valid JPEG
                if (!xobj.raw_stream_data() || xobj.raw_stream_size() == 0) continue;
                img.format = "jpeg";
                img.data.assign(reinterpret_cast<const char*>(xobj.raw_stream_data()),
                               reinterpret_cast<const char*>(xobj.raw_stream_data()) + xobj.raw_stream_size());
            } else {
                // Multi-stage: decode preceding filters, result is JPEG bytes
                auto decoded = doc.decode_stream(xobj);
                if (decoded.empty()) continue;
                // Check if result is valid JPEG
                if (decoded.size() >= 2 && decoded[0] == 0xFF && decoded[1] == 0xD8) {
                    img.format = "jpeg";
                    img.data.assign(reinterpret_cast<const char*>(decoded.data()),
                                   reinterpret_cast<const char*>(decoded.data()) + decoded.size());
                } else {
                    // Decode JPEG to raw pixels
                    auto jr = jpeg_decode(decoded.data(), decoded.size());
                    if (jr.pixels.empty()) continue;
                    img.format = "raw";
                    img.width = jr.width; img.height = jr.height;
                    img.components = jr.components;
                    img.pixels = std::move(jr.pixels);
                }
            }
        } else if (last_filter == "JPXDecode" && single_filter) {
            if (!xobj.raw_stream_data() || xobj.raw_stream_size() == 0) continue;
            img.format = "jp2";
            img.data.assign(reinterpret_cast<const char*>(xobj.raw_stream_data()),
                           reinterpret_cast<const char*>(xobj.raw_stream_data()) + xobj.raw_stream_size());
        } else if (last_filter == "CCITTFaxDecode") {
            auto parms = doc.resolve(xobj.get("DecodeParms"));
            int k = parms.get("K").as_int();
            int cols = parms.get("Columns").as_int();
            if (cols <= 0) cols = w;
            bool black_is_1 = parms.get("BlackIs1").bool_val;

            auto decoded = decode_ccitt(xobj.raw_stream_data(), xobj.raw_stream_size(),
                                         k, cols, black_is_1);
            // Convert 1-bit to grayscale
            int row_bytes = (cols + 7) / 8;
            int expected_rows = (int)decoded.size() / row_bytes;
            if (expected_rows <= 0) continue;

            std::vector<uint8_t> gray(static_cast<size_t>(cols) * expected_rows);
            for (int y = 0; y < expected_rows; y++) {
                for (int x = 0; x < cols; x++) {
                    int byte_idx = y * row_bytes + x / 8;
                    int bit_idx = 7 - (x % 8);
                    bool bit_set = (decoded[byte_idx] >> bit_idx) & 1;
                    // Decoder uses 1=black convention; if BlackIs1=false, invert
                    bool is_black = black_is_1 ? bit_set : !bit_set;
                    gray[y * cols + x] = is_black ? 0 : 255;
                }
            }

            img.format = "raw";
            img.width = cols;
            img.height = expected_rows;
            img.components = 1;
            img.pixels = std::move(gray);
        } else {
            // FlateDecode or other — decode to raw pixels
            auto decoded = doc.decode_stream(xobj);
            if (decoded.empty()) continue;

            int bpc = xobj.get("BitsPerComponent").as_int();
            if (bpc <= 0) bpc = 8;

            auto cs_obj = doc.resolve(xobj.get("ColorSpace"));
            std::string cs_name;
            if (cs_obj.is_name()) cs_name = cs_obj.str_val;
            else if (cs_obj.is_arr() && !cs_obj.arr.empty()) {
                auto first = doc.resolve(cs_obj.arr[0]);
                if (first.is_name()) cs_name = first.str_val;
            }

            int components = 3;
            if (cs_name == "DeviceGray" || cs_name == "CalGray") components = 1;
            else if (cs_name == "DeviceCMYK") components = 4;
            else if (cs_name == "DeviceRGB" || cs_name == "CalRGB") components = 3;
            else if (cs_name == "ICCBased") {
                if (cs_obj.is_arr() && cs_obj.arr.size() >= 2) {
                    auto icc_stream = doc.resolve(cs_obj.arr[1]);
                    int n = icc_stream.get("N").as_int();
                    if (n > 0) components = n;
                }
            }

            size_t expected = static_cast<size_t>(w) * h * components * bpc / 8;
            if (decoded.size() < expected && decoded.size() > 0) {
                // Try to infer components
                size_t total = static_cast<size_t>(w) * h;
                if (total > 0 && decoded.size() % total == 0)
                    components = static_cast<int>(decoded.size() / total);
            }

            img.format = "raw";
            img.components = components;
            img.pixels = std::move(decoded);
        }

        if (!img.data.empty() || !img.pixels.empty()) {
            if (!output_dir.empty()) {
                std::string ext, path;
                if (img.format == "jpeg") ext = ".jpg";
                else if (img.format == "jp2") ext = ".jp2";
                else ext = ".png";
                path = output_dir + "/" + img.name + ext;

                std::ofstream ofs(path, std::ios::binary);
                if (ofs) {
                    if (!img.data.empty()) {
                        // JPEG/JP2 passthrough
                        ofs.write(img.data.data(), img.data.size());
                    } else if (!img.pixels.empty()) {
                        // Raw pixels → PNG
                        auto png = pixels_to_png(img.pixels.data(), img.width, img.height, img.components);
                        ofs.write(png.data(), png.size());
                    }
                    img.saved_path = path;
                }
            }

            ExtractedImage ei;
            ei.img = std::move(img);
            std::memcpy(ei.ctm, ip.ctm, sizeof(ip.ctm));
            images.push_back(std::move(ei));
            img_idx++;
        }
    }

    return images;
}

// ── Canvas / Image Compositing ───────────────────────────

struct Canvas {
    int width, height;
    std::vector<uint8_t> pixels; // RGBA

    Canvas(int w, int h) : width(w), height(h), pixels(static_cast<size_t>(w) * h * 4, 255) {
        // Initialize to white, opaque
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = 255;     // R
            pixels[i + 1] = 255; // G
            pixels[i + 2] = 255; // B
            pixels[i + 3] = 255; // A
        }
    }

    void blend_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (x < 0 || x >= width || y < 0 || y >= height) return;
        size_t off = (static_cast<size_t>(y) * width + x) * 4;
        if (a == 255) {
            pixels[off] = r;
            pixels[off + 1] = g;
            pixels[off + 2] = b;
            pixels[off + 3] = 255;
        } else if (a > 0) {
            float sa = a / 255.0f;
            float da = pixels[off + 3] / 255.0f;
            float oa = sa + da * (1 - sa);
            if (oa > 0) {
                pixels[off]     = static_cast<uint8_t>((r * sa + pixels[off] * da * (1 - sa)) / oa);
                pixels[off + 1] = static_cast<uint8_t>((g * sa + pixels[off + 1] * da * (1 - sa)) / oa);
                pixels[off + 2] = static_cast<uint8_t>((b * sa + pixels[off + 2] * da * (1 - sa)) / oa);
                pixels[off + 3] = static_cast<uint8_t>(oa * 255);
            }
        }
    }

    void blit_image(const uint8_t* src, int sw, int sh, int scomp,
                     const double ctm[6], double page_h, double scale) {
        // CTM maps image space [0,1]×[0,1] to page space
        // Scale converts page space to canvas space
        bool axis_aligned = (std::abs(ctm[1]) < 0.001 && std::abs(ctm[2]) < 0.001);

        if (axis_aligned) {
            // Fast path: direct pixel copy
            double px = ctm[4] * scale;
            double py = (page_h - ctm[5] - ctm[3]) * scale;
            double pw = ctm[0] * scale;
            double ph = ctm[3] * scale;

            int dx = static_cast<int>(px);
            int dy = static_cast<int>(py);
            int dw = static_cast<int>(std::abs(pw));
            int dh = static_cast<int>(std::abs(ph));
            if (dw <= 0 || dh <= 0) return;

            for (int y = 0; y < dh; y++) {
                int sy = y * sh / dh;
                if (sy >= sh) sy = sh - 1;
                for (int x = 0; x < dw; x++) {
                    int sx = x * sw / dw;
                    if (sx >= sw) sx = sw - 1;
                    const uint8_t* sp = src + (sy * sw + sx) * scomp;
                    uint8_t r, g, b;
                    if (scomp >= 3) { r = sp[0]; g = sp[1]; b = sp[2]; }
                    else { r = g = b = sp[0]; }
                    blend_pixel(dx + x, dy + y, r, g, b, 255);
                }
            }
        } else {
            // General case: inverse transform
            // For each destination pixel, find source pixel
            double inv_det = ctm[0]*ctm[3] - ctm[1]*ctm[2];
            if (std::abs(inv_det) < 1e-10) return;

            // Bounding box in canvas space
            double corners[4][2];
            for (int i = 0; i < 4; i++) {
                double ix = (i & 1) ? 1.0 : 0.0;
                double iy = (i & 2) ? 1.0 : 0.0;
                double pgx = ctm[0]*ix + ctm[2]*iy + ctm[4];
                double pgy = ctm[1]*ix + ctm[3]*iy + ctm[5];
                corners[i][0] = pgx * scale;
                corners[i][1] = (page_h - pgy) * scale;
            }
            double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
            for (auto& c : corners) {
                if (c[0] < min_x) min_x = c[0];
                if (c[0] > max_x) max_x = c[0];
                if (c[1] < min_y) min_y = c[1];
                if (c[1] > max_y) max_y = c[1];
            }

            int x0 = std::max(0, static_cast<int>(min_x));
            int x1 = std::min(width - 1, static_cast<int>(max_x) + 1);
            int y0 = std::max(0, static_cast<int>(min_y));
            int y1 = std::min(height - 1, static_cast<int>(max_y) + 1);

            for (int dy = y0; dy <= y1; dy++) {
                for (int dx = x0; dx <= x1; dx++) {
                    double pgx = dx / scale;
                    double pgy = page_h - dy / scale;
                    double lx = pgx - ctm[4];
                    double ly = pgy - ctm[5];
                    double ix = (ctm[3]*lx - ctm[2]*ly) / inv_det;
                    double iy = (-ctm[1]*lx + ctm[0]*ly) / inv_det;
                    if (ix < 0 || ix > 1 || iy < 0 || iy > 1) continue;
                    int sx = static_cast<int>(ix * (sw - 1));
                    int sy = static_cast<int>((1 - iy) * (sh - 1));
                    if (sx < 0) sx = 0; if (sx >= sw) sx = sw - 1;
                    if (sy < 0) sy = 0; if (sy >= sh) sy = sh - 1;
                    const uint8_t* sp = src + (sy * sw + sx) * scomp;
                    uint8_t r, g, b;
                    if (scomp >= 3) { r = sp[0]; g = sp[1]; b = sp[2]; }
                    else { r = g = b = sp[0]; }
                    blend_pixel(dx, dy, r, g, b, 255);
                }
            }
        }
    }

    std::vector<char> to_png(int level = Z_BEST_SPEED) const {
        // Convert RGBA to RGB for PNG output
        std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
        for (int i = 0; i < width * height; i++) {
            rgb[i*3]   = pixels[i*4];
            rgb[i*3+1] = pixels[i*4+1];
            rgb[i*3+2] = pixels[i*4+2];
        }
        return pixels_to_png(rgb.data(), width, height, 3, level);
    }
};

// ── Page Rendering ───────────────────────────────────────

ImageData render_page_composite(PdfDoc& doc, const PdfObj& page_obj,
                                 const ContentParseResult& parse_result,
                                 int page_num, double page_w, double page_h,
                                 const std::string& output_dir) {
    constexpr double kDPI = 200.0;
    constexpr double kBase = 72.0;
    double scale = kDPI / kBase;
    int rw = static_cast<int>(page_w * scale);
    int rh = static_cast<int>(page_h * scale);
    if (rw <= 0 || rh <= 0) return {};

    Canvas canvas(rw, rh);

    // ── Rasterize vector paths (anti-aliased scanline fill) ──
    constexpr int AA_H = 8;  // horizontal subsamples per pixel
    constexpr int AA_V = 8;  // vertical subsamples per pixel

    // Bezier flattening
    std::function<void(double,double,double,double,double,double,double,double,
                        std::vector<std::pair<double,double>>&,double,int)> flatten_bezier;
    flatten_bezier = [&flatten_bezier](double x0, double y0, double cx1, double cy1,
                              double cx2, double cy2, double x3, double y3,
                              std::vector<std::pair<double,double>>& pts, double tol, int depth) {
        if (depth > 10) { pts.push_back({x3, y3}); return; }
        double dmax = std::max({std::abs(cx1-x0), std::abs(cy1-y0),
                                std::abs(cx2-x3), std::abs(cy2-y3)});
        if (dmax < tol) { pts.push_back({x3, y3}); return; }
        double m01x=(x0+cx1)/2, m01y=(y0+cy1)/2;
        double m12x=(cx1+cx2)/2, m12y=(cy1+cy2)/2;
        double m23x=(cx2+x3)/2, m23y=(cy2+y3)/2;
        double m012x=(m01x+m12x)/2, m012y=(m01y+m12y)/2;
        double m123x=(m12x+m23x)/2, m123y=(m12y+m23y)/2;
        double mx=(m012x+m123x)/2, my=(m012y+m123y)/2;
        flatten_bezier(x0,y0,m01x,m01y,m012x,m012y,mx,my,pts,tol,depth+1);
        flatten_bezier(mx,my,m123x,m123y,m23x,m23y,x3,y3,pts,tol,depth+1);
    };

    for (auto& rp : parse_result.paths) {
        if (rp.points.empty()) continue;

        // Flatten path to line segments
        std::vector<std::vector<std::pair<double,double>>> subpaths;
        std::vector<std::pair<double,double>> cur_sub;
        double px = 0, py = 0;
        for (auto& pt : rp.points) {
            switch (pt.type) {
                case PathPoint::MOVE:
                    if (!cur_sub.empty()) subpaths.push_back(std::move(cur_sub));
                    cur_sub.clear();
                    cur_sub.push_back({pt.x, pt.y});
                    px = pt.x; py = pt.y; break;
                case PathPoint::LINE:
                    cur_sub.push_back({pt.x, pt.y});
                    px = pt.x; py = pt.y; break;
                case PathPoint::CURVE:
                    flatten_bezier(px, py, pt.cx1, pt.cy1, pt.cx2, pt.cy2, pt.x, pt.y, cur_sub, 0.25, 0);
                    px = pt.x; py = pt.y; break;
                case PathPoint::CLOSE:
                    if (!cur_sub.empty()) { cur_sub.push_back(cur_sub[0]); px = cur_sub[0].first; py = cur_sub[0].second; }
                    break;
            }
        }
        if (!cur_sub.empty()) subpaths.push_back(std::move(cur_sub));

        // Collect edges in subpixel coordinates
        struct Edge { double x_at_ymin; double inv_slope; int ymin, ymax; int dir; };
        std::vector<Edge> edges;
        int global_ymin = rh * AA_V, global_ymax = 0;

        for (auto& sp : subpaths) {
            for (size_t i = 0; i + 1 < sp.size(); i++) {
                double sx0 = sp[i].first * scale;
                double sy0 = (page_h - sp[i].second) * scale;
                double sx1 = sp[i+1].first * scale;
                double sy1 = (page_h - sp[i+1].second) * scale;
                // Convert to subpixel grid
                int iy0 = static_cast<int>(std::round(sy0 * AA_V));
                int iy1 = static_cast<int>(std::round(sy1 * AA_V));
                if (iy0 == iy1) continue;
                int dir = 1;
                if (iy0 > iy1) { std::swap(sx0, sx1); std::swap(sy0, sy1); std::swap(iy0, iy1); dir = -1; }
                double inv_slope = (sx1 - sx0) / (sy1 - sy0);
                double x_start = sx0 + (iy0 / (double)AA_V - sy0) * inv_slope;
                edges.push_back({x_start * AA_H, inv_slope / AA_V * AA_H, iy0, iy1, dir});
                if (iy0 < global_ymin) global_ymin = iy0;
                if (iy1 > global_ymax) global_ymax = iy1;
            }
        }
        if (edges.empty()) continue;

        global_ymin = std::max(0, global_ymin);
        global_ymax = std::min(rh * AA_V, global_ymax);

        // Fill using scanline with AA hit-count
        if (rp.do_fill) {
            uint8_t fr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_r * 255)));
            uint8_t fg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_g * 255)));
            uint8_t fb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_b * 255)));

            // Hit count per pixel: how many sub-samples are inside the path
            int prev_row = global_ymin / AA_V;
            std::vector<int> hits(rw + 1, 0);

            for (int suby = global_ymin; suby < global_ymax; suby++) {
                int cur_row = suby / AA_V;
                if (cur_row != prev_row) {
                    // Flush: convert hits to alpha and blend
                    for (int x = 0; x < rw; x++) {
                        if (hits[x] > 0) {
                            int alpha = hits[x] * 255 / (AA_H * AA_V);
                            if (alpha > 255) alpha = 255;
                            canvas.blend_pixel(x, prev_row, fr, fg, fb, static_cast<uint8_t>(alpha));
                        }
                    }
                    std::fill(hits.begin(), hits.end(), 0);
                    prev_row = cur_row;
                }

                // Find edge intersections at this sub-scanline
                std::vector<double> xs;
                for (auto& e : edges) {
                    if (suby >= e.ymin && suby < e.ymax) {
                        double x = e.x_at_ymin + (suby - e.ymin) * e.inv_slope;
                        xs.push_back(x);
                    }
                }
                std::sort(xs.begin(), xs.end());

                // Even-odd: fill between pairs of intersections
                for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                    double fx0 = xs[i] / AA_H;
                    double fx1 = xs[i+1] / AA_H;
                    int ix0 = static_cast<int>(fx0);
                    int ix1 = static_cast<int>(fx1);
                    if (ix0 < 0) ix0 = 0;
                    if (ix1 >= rw) ix1 = rw - 1;

                    if (ix0 == ix1) {
                        // Both edges in same pixel
                        hits[ix0] += static_cast<int>((fx1 - fx0) * AA_H);
                    } else {
                        // Left partial pixel
                        hits[ix0] += static_cast<int>((ix0 + 1 - fx0) * AA_H);
                        // Full middle pixels
                        for (int x = ix0 + 1; x < ix1; x++)
                            hits[x] += AA_H;
                        // Right partial pixel
                        if (ix1 < rw)
                            hits[ix1] += static_cast<int>((fx1 - ix1) * AA_H);
                    }
                }
            }
            // Flush last row
            {
                for (int x = 0; x < rw; x++) {
                    if (hits[x] > 0) {
                        int alpha = hits[x] * 255 / (AA_H * AA_V);
                        if (alpha > 255) alpha = 255;
                        canvas.blend_pixel(x, prev_row, fr, fg, fb, static_cast<uint8_t>(alpha));
                    }
                }
            }
        }

        // Stroke: expand line segments into filled quads, then scanline fill
        if (rp.do_stroke) {
            uint8_t sr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_r * 255)));
            uint8_t sg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_g * 255)));
            uint8_t sb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_b * 255)));
            double lw = rp.line_width * scale;
            if (lw < 0.5) lw = 0.5;
            double half = lw / 2.0;

            for (auto& sp : subpaths) {
                for (size_t i = 0; i + 1 < sp.size(); i++) {
                    double sx0 = sp[i].first * scale;
                    double sy0 = (page_h - sp[i].second) * scale;
                    double sx1 = sp[i+1].first * scale;
                    double sy1 = (page_h - sp[i+1].second) * scale;

                    // Compute perpendicular normal
                    double dx = sx1 - sx0, dy = sy1 - sy0;
                    double len = std::sqrt(dx*dx + dy*dy);
                    if (len < 0.01) continue;
                    double nx = -dy / len * half;
                    double ny = dx / len * half;

                    // Expand to quad (4 corners)
                    double qx[4] = {sx0+nx, sx1+nx, sx1-nx, sx0-nx};
                    double qy[4] = {sy0+ny, sy1+ny, sy1-ny, sy0-ny};

                    // Find bounding box
                    int ymin = static_cast<int>(std::min({qy[0],qy[1],qy[2],qy[3]}));
                    int ymax = static_cast<int>(std::max({qy[0],qy[1],qy[2],qy[3]})) + 1;
                    ymin = std::max(0, ymin);
                    ymax = std::min(rh - 1, ymax);

                    // Scanline fill the quad
                    for (int y = ymin; y <= ymax; y++) {
                        double fy = y + 0.5;
                        // Find intersections with quad edges
                        std::vector<double> xs;
                        for (int e = 0; e < 4; e++) {
                            int e2 = (e + 1) % 4;
                            double ey0 = qy[e], ey1 = qy[e2];
                            if ((fy >= ey0 && fy < ey1) || (fy >= ey1 && fy < ey0)) {
                                double t = (fy - ey0) / (ey1 - ey0);
                                xs.push_back(qx[e] + t * (qx[e2] - qx[e]));
                            }
                        }
                        if (xs.size() >= 2) {
                            std::sort(xs.begin(), xs.end());
                            int x0 = std::max(0, static_cast<int>(xs[0]));
                            int x1 = std::min(rw - 1, static_cast<int>(xs.back()));
                            for (int x = x0; x <= x1; x++)
                                canvas.blend_pixel(x, y, sr, sg, sb, 255);
                        }
                    }
                }
            }
        }
    }

    auto res = doc.resolve(page_obj.get("Resources"));

    // Composite images in stream order
    for (auto& ip : parse_result.images) {
        PdfObj xobj;
        if (ip.xobj_ref >= 0) {
            xobj = doc.get_obj(ip.xobj_ref);
        } else if (!ip.xobj_name.empty()) {
            auto& xobjects = res.get("XObject");
            auto xd = doc.resolve(xobjects);
            if (xd.is_dict()) xobj = doc.resolve(xd.get(ip.xobj_name));
        }

        auto& subtype = xobj.get("Subtype");

        int w = xobj.get("Width").as_int();
        int h = xobj.get("Height").as_int();
        if (w <= 0 || h <= 0) continue;

        // Check if this is an ImageMask (1-bit stencil)
        bool is_image_mask = xobj.get("ImageMask").bool_val;

        // Decode image to RGB pixels for compositing
        std::vector<uint8_t> pixels;
        int components = 3;
        {
            // Determine last filter to decide decode strategy
            auto filter_obj = doc.resolve(xobj.get("Filter"));
            std::string last_filter;
            if (filter_obj.is_name()) last_filter = filter_obj.str_val;
            else if (filter_obj.is_arr() && !filter_obj.arr.empty()) {
                auto last = doc.resolve(filter_obj.arr.back());
                if (last.is_name()) last_filter = last.str_val;
            }

            if (last_filter == "CCITTFaxDecode") {
                // Decode preceding filters first, then CCITTFax
                // decode_stream skips CCITTFax, so we need manual handling
                auto parms_obj = doc.resolve(xobj.get("DecodeParms"));
                PdfObj ccitt_parms;
                if (parms_obj.is_dict()) ccitt_parms = parms_obj;
                else if (parms_obj.is_arr() && !parms_obj.arr.empty())
                    ccitt_parms = doc.resolve(parms_obj.arr.back());

                int k = ccitt_parms.get("K").as_int();
                int cols = ccitt_parms.get("Columns").as_int();
                if (cols <= 0) cols = w;
                bool black_is_1 = ccitt_parms.get("BlackIs1").bool_val;

                // Get raw stream data and apply pre-filters manually
                const uint8_t* src = xobj.raw_stream_data();
                size_t src_len = xobj.raw_stream_size();
                if (!src || src_len == 0) continue;

                // Apply preceding filters (e.g. FlateDecode before CCITTFax)
                std::vector<uint8_t> pre_decoded;
                if (filter_obj.is_arr() && filter_obj.arr.size() > 1) {
                    pre_decoded.assign(src, src + src_len);
                    for (size_t fi = 0; fi + 1 < filter_obj.arr.size(); fi++) {
                        auto fname = doc.resolve(filter_obj.arr[fi]);
                        if (fname.str_val == "FlateDecode")
                            pre_decoded = decode_flate(pre_decoded.data(), pre_decoded.size());
                    }
                    src = pre_decoded.data();
                    src_len = pre_decoded.size();
                }

                auto ccitt_data = decode_ccitt(src, src_len, k, cols, black_is_1);
                int row_bytes = (cols + 7) / 8;
                int rows = ccitt_data.empty() ? 0 : (int)ccitt_data.size() / row_bytes;
                if (rows <= 0) continue;

                // Convert 1-bit to grayscale
                pixels.resize(static_cast<size_t>(cols) * rows);
                for (int y = 0; y < rows; y++)
                    for (int x = 0; x < cols; x++) {
                        int bi = y * row_bytes + x / 8;
                        bool bit_set = (ccitt_data[bi] >> (7 - (x % 8))) & 1;
                        if (is_image_mask) {
                            // For ImageMask: store raw bit (1=paint, 0=transparent)
                            pixels[y * cols + x] = bit_set ? 255 : 0;
                        } else {
                            bool is_black = black_is_1 ? bit_set : !bit_set;
                            pixels[y * cols + x] = is_black ? 0 : 255;
                        }
                    }
                w = cols; h = rows; components = 1;
            } else {
                // Use decode_stream for everything else (handles filter chains)
                auto decoded = doc.decode_stream(xobj);

                // Check if result is JPEG (decode_stream leaves DCTDecode raw)
                if (decoded.size() >= 2 && decoded[0] == 0xFF && decoded[1] == 0xD8) {
                    auto jr = jpeg_decode(decoded.data(), decoded.size());
                    pixels = std::move(jr.pixels);
                    w = jr.width; h = jr.height; components = jr.components;
                } else {
                    auto cs_obj = doc.resolve(xobj.get("ColorSpace"));
                    std::string cs_name;
                    if (cs_obj.is_name()) cs_name = cs_obj.str_val;
                    else if (cs_obj.is_arr() && !cs_obj.arr.empty()) {
                        auto first = doc.resolve(cs_obj.arr[0]);
                        if (first.is_name()) cs_name = first.str_val;
                    }
                    if (cs_name == "DeviceGray" || cs_name == "CalGray") components = 1;
                    else if (cs_name == "DeviceCMYK") components = 4;
                    else if (cs_name == "ICCBased") {
                        if (cs_obj.is_arr() && cs_obj.arr.size() >= 2) {
                            auto icc = doc.resolve(cs_obj.arr[1]);
                            int n = icc.get("N").as_int();
                            if (n > 0) components = n;
                        }
                    }
                    pixels = std::move(decoded);
                }
            }
        }
        size_t expected = static_cast<size_t>(w) * h * components;
        if (pixels.size() < expected) continue;

        if (is_image_mask && components == 1) {
            // ImageMask: painted where mask bit is SET (pixel==0 means bit was 1 in decoder)
            // In our grayscale: 0=black(bit was set), 255=white(bit was clear)
            // Paint fill color (black) where bit was set (pixel==0),
            // transparent where bit was clear (pixel==255)
            uint8_t fr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_r * 255)));
            uint8_t fg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_g * 255)));
            uint8_t fb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_b * 255)));
            // Blit with alpha — use Canvas blit for proper CTM handling
            double px = ip.ctm[4] * scale;
            double py = (page_h - ip.ctm[5] - ip.ctm[3]) * scale;
            double pw = std::abs(ip.ctm[0] * scale);
            double ph = std::abs(ip.ctm[3] * scale);
            int dx = static_cast<int>(px), dy = static_cast<int>(py);
            int dw = static_cast<int>(pw), dh = static_cast<int>(ph);
            if (dw <= 0 || dh <= 0) continue;
            for (int y = 0; y < dh && dy + y >= 0 && dy + y < canvas.height; y++) {
                int sy = y * h / dh;
                if (sy >= h) sy = h - 1;
                for (int x = 0; x < dw && dx + x < canvas.width; x++) {
                    if (dx + x < 0) continue;
                    int sx = x * w / dw;
                    if (sx >= w) sx = w - 1;
                    // pixels[sy*w+sx]: 255=bit set(paint), 0=clear(transparent)
                    if (pixels[sy * w + sx] > 128)
                        canvas.blend_pixel(dx + x, dy + y, fr, fg, fb, 255);
                }
            }
        } else {
            canvas.blit_image(pixels.data(), w, h, components, ip.ctm, page_h, scale);
        }
    }

    ImageData img;
    img.page_number = page_num;
    img.name = "page" + std::to_string(page_num + 1) + "_render";
    img.format = "raw";
    img.width = rw;
    img.height = rh;
    img.components = 3;

    // Extract RGB from RGBA canvas
    img.pixels.resize(static_cast<size_t>(rw) * rh * 3);
    for (int i = 0; i < rw * rh; i++) {
        img.pixels[i*3]   = canvas.pixels[i*4];
        img.pixels[i*3+1] = canvas.pixels[i*4+1];
        img.pixels[i*3+2] = canvas.pixels[i*4+2];
    }

    if (!output_dir.empty()) {
        auto png = pixels_to_png(img.pixels.data(), rw, rh, 3, Z_BEST_SPEED);
        if (!png.empty()) {
            std::string path = output_dir + "/" + img.name + ".png";
            std::ofstream f(path, std::ios::binary);
            if (f) {
                f.write(png.data(), static_cast<std::streamsize>(png.size()));
                img.saved_path = path;
            }
        }
    }
    return img;
}

// ── Bookmark Extraction ──────────────────────────────────

struct BookmarkEntry {
    std::string title;
    int page = -1;
    int level = 0;
};

void collect_bookmarks(PdfDoc& doc, const PdfObj& node, int depth,
                        std::vector<BookmarkEntry>& out) {
    if (depth > 20) return;

    PdfObj item = doc.resolve(node);
    if (!item.is_dict()) return;

    // Process children
    auto first_ref = item.get("First");
    if (first_ref.is_none()) return;

    PdfObj child = doc.resolve(first_ref);
    while (child.is_dict()) {
        BookmarkEntry entry;
        entry.level = depth;

        auto& title = child.get("Title");
        if (title.is_str()) {
            // Check for UTF-16BE BOM
            if (title.str_val.size() >= 2 &&
                static_cast<uint8_t>(title.str_val[0]) == 0xFE &&
                static_cast<uint8_t>(title.str_val[1]) == 0xFF) {
                // UTF-16BE
                for (size_t i = 2; i + 1 < title.str_val.size(); i += 2) {
                    uint32_t cp = (static_cast<uint8_t>(title.str_val[i]) << 8) |
                                   static_cast<uint8_t>(title.str_val[i + 1]);
                    util::append_utf8(entry.title, cp);
                }
            } else {
                // PDFDocEncoding (similar to Latin-1)
                for (unsigned char c : title.str_val) {
                    util::append_utf8(entry.title, static_cast<uint32_t>(c));
                }
            }
        }

        // Get destination page
        auto dest = doc.resolve(child.get("Dest"));
        if (dest.is_arr() && !dest.arr.empty()) {
            auto page_ref = doc.resolve(dest.arr[0]);
            if (page_ref.is_ref()) {
                // Need to map page object number to page index
                entry.page = page_ref.ref_num; // Will remap later
            } else if (page_ref.is_int()) {
                entry.page = page_ref.as_int();
            }
        }
        if (entry.page < 0) {
            auto action = doc.resolve(child.get("A"));
            if (action.is_dict()) {
                auto& s = action.get("S");
                if (s.is_name() && s.str_val == "GoTo") {
                    auto d = doc.resolve(action.get("D"));
                    if (d.is_arr() && !d.arr.empty()) {
                        auto pr = doc.resolve(d.arr[0]);
                        if (pr.is_ref()) entry.page = pr.ref_num;
                        else if (pr.is_int()) entry.page = pr.as_int();
                    }
                }
            }
        }

        if (!entry.title.empty())
            out.push_back(std::move(entry));

        collect_bookmarks(doc, child, depth + 1, out);

        auto next = child.get("Next");
        if (next.is_none() || next.is_ref()) {
            if (next.is_ref()) child = doc.resolve(next);
            else break;
        } else {
            break;
        }
    }
}

// ── Markdown Formatting ──────────────────────────────────

bool line_in_table(const TextLine& line, const std::vector<TableData>& tables) {
    for (auto& t : tables) {
        double t_bottom = std::min(t.y0, t.y1) - 10.0;
        double t_top = std::max(t.y0, t.y1) + 5.0;
        double t_left = std::min(t.x0, t.x1) - 15.0;
        double t_right = std::max(t.x0, t.x1) + 15.0;
        if (line.y_center >= t_bottom && line.y_center <= t_top) {
            if (line.x_left >= t_left && line.x_right <= t_right) {
                return true;
            }
            double overlap_l = std::max(line.x_left, t_left);
            double overlap_r = std::min(line.x_right, t_right);
            if (overlap_r > overlap_l) {
                double overlap = overlap_r - overlap_l;
                double line_width = line.x_right - line.x_left;
                if (line_width > 0 && overlap >= line_width * 0.6) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::vector<TextLine> merge_colinear_lines(const std::vector<TextLine>& lines) {
    if (lines.size() < 2) return lines;

    std::vector<size_t> idx(lines.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        return lines[a].y_center > lines[b].y_center;
    });

    std::vector<TextLine> merged;
    size_t i = 0;
    while (i < idx.size()) {
        double y = lines[idx[i]].y_center;
        std::vector<size_t> group;
        group.push_back(idx[i]);
        size_t j = i + 1;
        while (j < idx.size() && std::abs(lines[idx[j]].y_center - y) < 5.0) {
            group.push_back(idx[j]);
            j++;
        }

        if (group.size() == 1) {
            merged.push_back(lines[group[0]]);
        } else {
            std::sort(group.begin(), group.end(), [&](size_t a, size_t b) {
                return lines[a].x_left < lines[b].x_left;
            });
            TextLine m;
            m.y_center = lines[group[0]].y_center;
            m.x_left = lines[group[0]].x_left;
            m.font_size = lines[group[0]].font_size;
            m.is_bold = lines[group[0]].is_bold;
            m.is_italic = lines[group[0]].is_italic;
            for (size_t k = 0; k < group.size(); k++) {
                if (k > 0) {
                    double gap = lines[group[k]].x_left - lines[group[k-1]].x_right;
                    double avg_font = (lines[group[k]].font_size +
                                       lines[group[k-1]].font_size) / 2.0;
                    double col_gap = std::max(avg_font * 6.0, 60.0);
                    double word_gap = std::max(avg_font * 0.5, 4.0);
                    if (gap > col_gap)
                        m.text += "\n";
                    else if (gap > word_gap)
                        m.text += " ";
                }
                m.text += lines[group[k]].text;
                if (lines[group[k]].x_right > m.x_right)
                    m.x_right = lines[group[k]].x_right;
            }
            merged.push_back(std::move(m));
        }
        i = j;
    }
    return merged;
}

std::string page_to_markdown(const std::vector<TextLine>& raw_lines,
                              const FontStats& stats,
                              const std::vector<ImageData>& images,
                              const std::vector<TableData>& tables) {
    auto lines = merge_colinear_lines(raw_lines);

    std::string md;
    md.reserve(lines.size() * 80);

    struct TableInsert {
        double y_pos;
        size_t table_idx;
    };
    std::vector<TableInsert> table_inserts;
    for (size_t ti = 0; ti < tables.size(); ti++) {
        table_inserts.push_back({std::max(tables[ti].y0, tables[ti].y1), ti});
    }
    std::sort(table_inserts.begin(), table_inserts.end(),
              [](const TableInsert& a, const TableInsert& b) { return a.y_pos > b.y_pos; });

    size_t next_table = 0;

    for (size_t i = 0; i < lines.size(); i++) {
        const auto& l = lines[i];

        while (next_table < table_inserts.size() &&
               table_inserts[next_table].y_pos >= l.y_center) {
            md += "\n";
            md += format_table(tables[table_inserts[next_table].table_idx]);
            md += "\n";
            next_table++;
        }

        if (line_in_table(l, tables)) continue;

        {
            bool only_filler = true;
            for (char ch : l.text) {
                if (ch != '|' && ch != ' ' && ch != '\t' && ch != '\n') {
                    only_filler = false;
                    break;
                }
            }
            if (only_filler) continue;
        }

        int hlevel = stats.heading_level(l.font_size);

        if (hlevel >= 3 && !l.is_bold && l.text.size() > 60)
            hlevel = 0;

        if (hlevel > 0) {
            if (i > 0) md += '\n';
            for (int h = 0; h < hlevel; h++) md += '#';
            md += ' ';
            md += l.text;
            md += '\n';
        } else if (l.is_bold && l.is_italic) {
            md += "***" + l.text + "***\n";
        } else if (l.is_bold) {
            md += "**" + l.text + "**\n";
        } else if (l.is_italic) {
            md += "*" + l.text + "*\n";
        } else {
            md += l.text;
            md += '\n';
        }
    }

    while (next_table < table_inserts.size()) {
        md += "\n";
        md += format_table(tables[table_inserts[next_table].table_idx]);
        md += "\n";
        next_table++;
    }

    for (auto& img : images) {
        std::string ref = img.saved_path.empty() ? img.name : img.saved_path;
        md += "\n![" + img.name + "](" + ref + ")\n";
    }
    return md;
}

// ── Core Extraction Logic ────────────────────────────────

struct ExtractResult {
    std::vector<std::vector<TextLine>> all_lines;
    std::vector<std::vector<ImageData>> all_images;
    std::vector<std::vector<TableData>> all_tables;
    std::vector<double> page_widths;
    std::vector<double> page_heights;
    std::vector<BookmarkEntry> bookmarks;
    FontStats stats;
    int total_pages = 0;
};

std::vector<uint8_t> get_page_content(PdfDoc& doc, const PdfObj& page_obj) {
    auto contents = doc.resolve(page_obj.get("Contents"));
    if (contents.is_stream()) {
        return doc.decode_stream(contents);
    }
    if (contents.is_arr()) {
        std::vector<uint8_t> combined;
        for (auto& ref : contents.arr) {
            auto stm = doc.resolve(ref);
            if (stm.is_stream()) {
                auto decoded = doc.decode_stream(stm);
                combined.insert(combined.end(), decoded.begin(), decoded.end());
                combined.push_back(' ');
            }
        }
        return combined;
    }
    return {};
}

ExtractResult extract_pdf(const std::string& pdf_path, const ConvertOptions& opts) {
    ExtractResult result;

    // Read file
    std::ifstream file(pdf_path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open PDF: " + pdf_path);

    std::streamsize fsize = file.tellg();
    if (fsize <= 0) throw std::runtime_error("Empty PDF file: " + pdf_path);
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> file_data(static_cast<size_t>(fsize));
    if (!file.read(reinterpret_cast<char*>(file_data.data()), fsize))
        throw std::runtime_error("Cannot read PDF: " + pdf_path);
    file.close();

    // Check PDF header
    if (fsize < 5 || std::memcmp(file_data.data(), "%PDF-", 5) != 0)
        throw std::runtime_error("Not a valid PDF file: " + pdf_path);

    PdfDoc doc(file_data.data(), file_data.size());
    if (!doc.parse())
        throw std::runtime_error("Failed to parse PDF structure: " + pdf_path);

    // Check for encryption
    if (doc.trailer.has("Encrypt"))
        throw std::runtime_error("Encrypted PDF files are not supported: " + pdf_path);

    // Get page tree
    auto root = doc.resolve(doc.trailer.get("Root"));
    auto pages = doc.resolve(root.get("Pages"));
    if (!pages.is_dict())
        throw std::runtime_error("Invalid PDF page tree: " + pdf_path);

    // Collect all page objects
    std::vector<PdfObj> page_objs;
    std::vector<int> page_obj_nums;
    std::function<void(const PdfObj&)> collect_pages;
    collect_pages = [&](const PdfObj& node) {
        auto n = doc.resolve(node);
        if (!n.is_dict()) return;
        auto& type = n.get("Type");
        if (type.is_name() && type.str_val == "Page") {
            page_objs.push_back(n);
            if (node.is_ref()) page_obj_nums.push_back(node.ref_num);
            else page_obj_nums.push_back(-1);
            return;
        }
        auto& kids = n.get("Kids");
        if (kids.is_arr()) {
            for (auto& kid : kids.arr) collect_pages(kid);
        }
    };
    collect_pages(pages);

    int tp = static_cast<int>(page_objs.size());
    result.total_pages = tp;
    result.all_lines.resize(tp);
    result.all_images.resize(tp);
    result.all_tables.resize(tp);
    result.page_widths.resize(tp, 0);
    result.page_heights.resize(tp, 0);

    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < tp; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    std::string image_dir;
    if (opts.extract_images && !opts.image_output_dir.empty()) {
        image_dir = opts.image_output_dir;
        util::ensure_dir(image_dir);
    }

    for (int p : page_indices) {
        if (p < 0 || p >= tp) continue;
        auto& page_obj = page_objs[p];

        // Get page dimensions from MediaBox
        auto mediabox = doc.resolve(page_obj.get("MediaBox"));
        double page_w = 612, page_h = 792; // default letter
        if (mediabox.is_arr() && mediabox.arr.size() >= 4) {
            page_w = mediabox.arr[2].as_num() - mediabox.arr[0].as_num();
            page_h = mediabox.arr[3].as_num() - mediabox.arr[1].as_num();
        }
        result.page_widths[p] = page_w;
        result.page_heights[p] = page_h;

        // Get resources (inherit from parent)
        auto resources = doc.resolve(page_obj.get("Resources"));
        if (!resources.is_dict()) {
            auto parent = doc.resolve(page_obj.get("Parent"));
            if (parent.is_dict()) resources = doc.resolve(parent.get("Resources"));
        }

        // Quick check: if no fonts in resources AND no images needed, skip decompress entirely
        bool has_fonts = false;
        {
            auto& font_res = resources.get("Font");
            if (!font_res.is_none()) {
                auto fd = doc.resolve(font_res);
                has_fonts = fd.is_dict() && !fd.dict.empty();
            }
        }
        if (!has_fonts && !opts.extract_images) continue;

        // Parse content stream
        auto content_data = get_page_content(doc, page_obj);
        if (content_data.empty()) continue;

        auto parse_result = parse_content_stream(doc, content_data, resources, page_h);

        // Extract text lines
        bool plaintext = (opts.output_format == OutputFormat::PLAINTEXT);
        result.all_lines[p] = chars_to_lines(parse_result.chars);

        // Table detection
        bool need_tables = opts.extract_tables && !plaintext;
        if (need_tables) {
            PageCharCache cache;
            cache.build(parse_result.chars);

            result.all_tables[p] = detect_tables(parse_result.segments, cache,
                page_w, page_h);
            auto text_tables = detect_text_tables(cache,
                result.all_tables[p], page_w, page_h);
            for (auto& tt : text_tables)
                result.all_tables[p].push_back(std::move(tt));
        }

        // Image extraction
        if (opts.extract_images) {
            // Check for layered page
            bool has_regular = false, has_mask = false;
            for (auto& ip : parse_result.images) {
                PdfObj xobj;
                if (ip.xobj_ref >= 0) xobj = doc.get_obj(ip.xobj_ref);
                if (!xobj.is_stream()) continue;
                int bpc = xobj.get("BitsPerComponent").as_int();
                if (bpc == 1) has_mask = true;
                else has_regular = true;
            }

            if (has_regular && has_mask) {
                // Layered: render as composite
                auto rendered = render_page_composite(doc, page_obj, parse_result,
                                                      p, page_w, page_h, image_dir);
                if (!rendered.data.empty())
                    result.all_images[p].push_back(std::move(rendered));
            } else {
                auto extracted = extract_page_images(doc, page_obj, parse_result, p, image_dir);
                for (auto& ei : extracted)
                    result.all_images[p].push_back(std::move(ei.img));
            }

            // Fallback: render page for scanned/vector-only pages
            if (result.all_images[p].empty() && result.all_lines[p].empty()) {
                if (!parse_result.images.empty() || !parse_result.segments.empty()) {
                    auto rendered = render_page_composite(doc, page_obj, parse_result,
                                                          p, page_w, page_h, image_dir);
                    if (!rendered.data.empty())
                        result.all_images[p].push_back(std::move(rendered));
                }
            }

            // Release pixel data after writing to disk
            if (!image_dir.empty()) {
                for (auto& img : result.all_images[p]) {
                    if (!img.saved_path.empty()) {
                        img.data.clear();
                        img.data.shrink_to_fit();
                    }
                }
            }
        }
    }

    // Extract bookmarks
    auto outlines = doc.resolve(root.get("Outlines"));
    if (outlines.is_dict()) {
        collect_bookmarks(doc, outlines, 0, result.bookmarks);
        // Remap bookmark page references (obj num → page index)
        for (auto& bm : result.bookmarks) {
            if (bm.page >= 0) {
                bool found = false;
                for (int i = 0; i < (int)page_obj_nums.size(); i++) {
                    if (page_obj_nums[i] == bm.page) {
                        bm.page = i;
                        found = true;
                        break;
                    }
                }
                if (!found) bm.page = -1;
            }
        }
    }

    result.stats.compute(result.all_lines);
    return result;
}

} // anonymous namespace

// ── Public API ───────────────────────────────────────────

static std::string format_bookmarks(const std::vector<BookmarkEntry>& bookmarks,
                                     bool plaintext) {
    if (bookmarks.empty()) return "";
    std::string out;
    for (auto& bm : bookmarks) {
        if (bm.title.empty()) continue;
        if (plaintext) {
            for (int i = 0; i < bm.level; i++) out += "  ";
            out += bm.title;
            if (bm.page >= 0)
                out += " (p." + std::to_string(bm.page + 1) + ")";
            out += "\n";
        } else {
            for (int i = 0; i < bm.level; i++) out += "  ";
            out += "- " + bm.title;
            if (bm.page >= 0)
                out += " *(p." + std::to_string(bm.page + 1) + ")*";
            out += "\n";
        }
    }
    return out;
}

std::string pdf_to_markdown(const std::string& pdf_path, ConvertOptions opts) {
    auto r = extract_pdf(pdf_path, opts);

    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < r.total_pages; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    bool plaintext = (opts.output_format == OutputFormat::PLAINTEXT);

    std::string full_md;
    full_md.reserve(64 * 1024);

    if (!r.bookmarks.empty()) {
        if (!plaintext) full_md += "## Table of Contents\n\n";
        full_md += format_bookmarks(r.bookmarks, plaintext);
        full_md += "\n";
    }

    bool first = true;
    for (int p : page_indices) {
        if (p < 0 || p >= r.total_pages) continue;
        if (!first)
            full_md += "\n--- Page " + std::to_string(p + 1) + " ---\n\n";
        first = false;
        std::string page_md = page_to_markdown(r.all_lines[p], r.stats,
                                                r.all_images[p], r.all_tables[p]);
        if (plaintext)
            full_md += util::strip_markdown(page_md);
        else
            full_md += page_md;
    }
    return full_md;
}

std::vector<PageChunk> pdf_to_markdown_chunks(const std::string& pdf_path,
                                           ConvertOptions opts) {
    auto r = extract_pdf(pdf_path, opts);

    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < r.total_pages; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    bool plaintext = (opts.output_format == OutputFormat::PLAINTEXT);

    std::vector<PageChunk> chunks;
    for (int p : page_indices) {
        if (p < 0 || p >= r.total_pages) continue;
        PageChunk chunk;
        chunk.page_number = p + 1;
        chunk.page_width = r.page_widths[p];
        chunk.page_height = r.page_heights[p];
        chunk.body_font_size = r.stats.body_size;
        std::string page_md = page_to_markdown(r.all_lines[p], r.stats,
                                                r.all_images[p], r.all_tables[p]);
        chunk.text = plaintext ? util::strip_markdown(page_md) : page_md;

        for (auto& td : r.all_tables[p])
            chunk.tables.push_back(td.rows);

        chunk.images = std::move(r.all_images[p]);
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

} // namespace jdoc
