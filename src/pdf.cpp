// pdf.cpp — PDF→Markdown with custom parser (no PDFium dependency)
// Features: text, headings, bold/italic, images, table detection, page rendering
// Thread-safe: no global state, fully reentrant

#include "jdoc/pdf.h"
#include "common/string_utils.h"
#include "common/file_utils.h"
#include "common/png_encode.h"
#include "pdf_crypt.h"

#include <jpeglib.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <zlib.h>

namespace jdoc {
namespace {

// Portable memmem (not available on Windows)
static const void* pdf_memmem(const void* hay, size_t hay_len,
                               const void* needle, size_t needle_len) {
    if (needle_len == 0) return hay;
    if (needle_len > hay_len) return nullptr;
    const uint8_t* h = static_cast<const uint8_t*>(hay);
    const uint8_t* n = static_cast<const uint8_t*>(needle);
    size_t limit = hay_len - needle_len;
    for (size_t i = 0; i <= limit; i++)
        if (h[i] == n[0] && std::memcmp(h + i, n, needle_len) == 0)
            return h + i;
    return nullptr;
}

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
    int src_num = -1, src_gen = 0;  // owning object number (decryption key input)

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
        uint8_t c = data[pos];
        if (c == '/') {
            pos++;
            size_t start = pos;
            while (pos < len && !is_ws(data[pos]) && !is_delim(data[pos])) pos++;
            std::string tok("/", 1);
            tok.append(reinterpret_cast<const char*>(data + start), pos - start);
            return tok;
        }
        if (is_delim(c)) { pos++; return std::string(1, static_cast<char>(c)); }
        size_t start = pos;
        while (pos < len && !is_ws(data[pos]) && !is_delim(data[pos])) pos++;
        return std::string(reinterpret_cast<const char*>(data + start), pos - start);
    }

    std::string read_hex_string() {
        auto hex_val = [](uint8_t c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return -1;
        };
        std::string result;
        int hi = -1;
        while (pos < len && data[pos] != '>') {
            int v = hex_val(data[pos++]);
            if (v < 0) continue;
            if (hi < 0) { hi = v; }
            else { result += static_cast<char>((hi << 4) | v); hi = -1; }
        }
        if (hi >= 0) result += static_cast<char>(hi << 4);
        if (pos < len) pos++;
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
        uInt prev_in = zs.avail_in;
        ret = inflate(&zs, Z_NO_FLUSH);
        size_t have = sizeof(buf) - zs.avail_out;
        out.insert(out.end(), buf, buf + have);
        if (ret == Z_STREAM_END) break;
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR
            || ret == Z_NEED_DICT) {
            inflateEnd(&zs);
            return out; // return partial
        }
        // Z_BUF_ERROR or Z_OK with no progress (e.g. truncated stream missing
        // adler32 trailer) — bail out instead of looping forever.
        if (have == 0 && zs.avail_in == prev_in) {
            inflateEnd(&zs);
            return out;
        }
    } while (true);
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

// ── PDF Encryption (Standard Security Handler) ──────────
// Primitives live in pdf_crypt.h; this layer maps the /Encrypt dictionary
// onto them. Supports V1/V2 (RC4), V4 (AES-128) and V5/R6 (AES-256).

namespace pc = pdf_crypt;

// Crypt filter method selected by /CF /StdCF /CFM.
enum class CryptMethod { RC4, AESV2, AESV3 };

struct PdfCrypt {
    bool active = false;
    int version = 0;                       // /V
    int revision = 0;                      // /R
    int key_len = 5;                       // file key length in bytes
    uint8_t file_key[32] = {};             // up to 256 bits (V5)
    bool meta_encrypted = true;
    CryptMethod method = CryptMethod::RC4;

    bool init(const PdfObj& trailer_dict, const PdfObj& encrypt_obj,
              const std::string& password = "") {
        version = encrypt_obj.get("V").as_int();
        revision = encrypt_obj.get("R").as_int();

        auto& o_obj = encrypt_obj.get("O");
        auto& u_obj = encrypt_obj.get("U");
        if (!o_obj.is_str() || !u_obj.is_str()) return false;

        read_crypt_filter(encrypt_obj);
        auto& em = encrypt_obj.get("EncryptMetadata");
        if (em.is_bool() && !em.bool_val) meta_encrypted = false;

        active = (version >= 5) ? init_aes256(encrypt_obj, o_obj, u_obj, password)
                                : init_legacy(trailer_dict, encrypt_obj,
                                              o_obj, u_obj, password);
        return active;
    }

    // Decrypt string or stream data in place.
    void decrypt_data(std::vector<uint8_t>& data, int obj_num, int gen_num) const {
        if (!active || data.empty()) return;
        if (method == CryptMethod::AESV3) {
            // V5 uses the file key directly — there is no per-object key.
            pc::aes_cbc_decrypt_stream(file_key, key_len, data);
            return;
        }
        uint8_t obj_key[16];
        int obj_key_len = object_key(obj_num, gen_num, obj_key);
        if (method == CryptMethod::AESV2)
            pc::aes_cbc_decrypt_stream(obj_key, obj_key_len, data);
        else
            pc::rc4(obj_key, obj_key_len, data.data(), data.size());
    }

    std::string decrypt_string(const std::string& s, int obj_num, int gen_num) const {
        if (!active || s.empty()) return s;
        std::vector<uint8_t> buf(s.begin(), s.end());
        decrypt_data(buf, obj_num, gen_num);
        return std::string(buf.begin(), buf.end());
    }

private:
    void read_crypt_filter(const PdfObj& encrypt_obj) {
        method = (version >= 5) ? CryptMethod::AESV3 : CryptMethod::RC4;
        auto& stdcf = encrypt_obj.get("CF").get("StdCF");
        if (!stdcf.is_dict()) return;
        auto& cfm = stdcf.get("CFM");
        if (!cfm.is_name()) return;
        if (cfm.str_val == "AESV2")      method = CryptMethod::AESV2;
        else if (cfm.str_val == "AESV3") method = CryptMethod::AESV3;
        else if (cfm.str_val == "V2")    method = CryptMethod::RC4;
    }

    // Revisions 2-4: MD5-derived key (Algorithm 2), verified against /U.
    bool init_legacy(const PdfObj& trailer_dict, const PdfObj& encrypt_obj,
                     const PdfObj& o_obj, const PdfObj& u_obj,
                     const std::string& password) {
        int length_bits = encrypt_obj.get("Length").as_int();
        if (length_bits <= 0) length_bits = 40;
        key_len = std::clamp(length_bits / 8, 5, 16);

        auto& id_arr = trailer_dict.get("ID");
        std::string file_id;
        if (id_arr.is_arr() && !id_arr.arr.empty() && id_arr.arr[0].is_str())
            file_id = id_arr.arr[0].str_val;

        // MD5(padded_pw || O || P(4 bytes LE) || FileID [|| FFFFFFFF])
        uint8_t padded[32];
        pc::pad_password(password, padded);
        std::vector<uint8_t> input(padded, padded + 32);
        input.insert(input.end(), o_obj.str_val.begin(), o_obj.str_val.end());
        const int p_val = encrypt_obj.get("P").as_int();
        for (int i = 0; i < 4; i++)
            input.push_back(static_cast<uint8_t>(p_val >> (i * 8)));
        input.insert(input.end(), file_id.begin(), file_id.end());
        if (revision >= 4 && !meta_encrypted)
            input.insert(input.end(), 4, 0xFF);

        uint8_t hash[16];
        pc::md5(input.data(), input.size(), hash);
        if (revision >= 3)
            for (int i = 0; i < 50; i++) pc::md5(hash, key_len, hash);
        std::memcpy(file_key, hash, key_len);

        return verify_user_password(u_obj.str_val, file_id);
    }

    // Revisions 5-6: the key is unwrapped from /UE or /OE (Algorithm 2.A).
    bool init_aes256(const PdfObj& encrypt_obj, const PdfObj& o_obj,
                     const PdfObj& u_obj, const std::string& password) {
        key_len = 32;
        return pc::aes256_file_key(revision, password,
                                   o_obj.str_val, u_obj.str_val,
                                   encrypt_obj.get("OE").str_val,
                                   encrypt_obj.get("UE").str_val,
                                   file_key);
    }

    bool verify_user_password(const std::string& u_value, const std::string& file_id) const {
        if (revision <= 2) {
            // Algorithm 4: RC4-encrypt the padding string.
            uint8_t result[32];
            std::memcpy(result, kEmptyPassword, 32);
            pc::rc4(file_key, key_len, result, 32);
            return std::memcmp(result, u_value.data(),
                               std::min<size_t>(u_value.size(), 32)) == 0;
        }
        // Algorithm 5: MD5(pad || FileID), then 20 RC4 rounds with the key
        // XOR'd by the round number.
        std::vector<uint8_t> input(kEmptyPassword, kEmptyPassword + 32);
        input.insert(input.end(), file_id.begin(), file_id.end());
        uint8_t hash[16];
        pc::md5(input.data(), input.size(), hash);
        for (int i = 0; i < 20; i++) {
            uint8_t round_key[16];
            for (int j = 0; j < key_len; j++)
                round_key[j] = file_key[j] ^ static_cast<uint8_t>(i);
            pc::rc4(round_key, key_len, hash, 16);
        }
        return u_value.size() >= 16 && std::memcmp(hash, u_value.data(), 16) == 0;
    }

    // Algorithm 1: MD5(file_key || obj(3) || gen(2) [|| "sAlT"]).
    int object_key(int obj_num, int gen_num, uint8_t out_key[16]) const {
        uint8_t buf[25];
        std::memcpy(buf, file_key, key_len);
        buf[key_len]     = static_cast<uint8_t>(obj_num);
        buf[key_len + 1] = static_cast<uint8_t>(obj_num >> 8);
        buf[key_len + 2] = static_cast<uint8_t>(obj_num >> 16);
        buf[key_len + 3] = static_cast<uint8_t>(gen_num);
        buf[key_len + 4] = static_cast<uint8_t>(gen_num >> 8);
        int input_len = key_len + 5;
        if (method == CryptMethod::AESV2) {
            static const uint8_t kAesSalt[4] = {0x73, 0x41, 0x6C, 0x54};  // "sAlT"
            std::memcpy(buf + input_len, kAesSalt, 4);
            input_len += 4;
        }
        uint8_t hash[16];
        pc::md5(buf, input_len, hash);
        const int eff_len = std::min(key_len + 5, 16);  // the key is clamped, not the input
        std::memcpy(out_key, hash, eff_len);
        return eff_len;
    }

    // The empty user password after padding (ISO 32000-1, Algorithm 2).
    static const uint8_t kEmptyPassword[32];
};

const uint8_t PdfCrypt::kEmptyPassword[32] = {
    0x28,0xBF,0x4E,0x5E,0x4E,0x75,0x8A,0x41,0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,
    0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A
};

struct PdfDoc {
    const uint8_t* data;
    size_t len;
    std::map<int, XrefEntry> xref;
    PdfObj trailer;
    std::map<int, PdfObj> obj_cache;
    PdfCrypt crypt;
    int encrypt_ref_num = -1;  // /Encrypt object number (never decrypted)

    PdfDoc(const uint8_t* d, size_t l) : data(d), len(l) {}

    // Recursively decrypt all string values within an object (annotations,
    // bookmarks, metadata). Streams are decrypted separately in decode_stream.
    void decrypt_strings(PdfObj& obj, int num, int gen) {
        if (obj.type == ObjType::STRING) {
            obj.str_val = crypt.decrypt_string(obj.str_val, num, gen);
        } else if (obj.is_arr()) {
            for (auto& e : obj.arr) decrypt_strings(e, num, gen);
        } else if (obj.type == ObjType::DICT || obj.type == ObjType::STREAM) {
            for (auto& [k, v] : obj.dict) decrypt_strings(v, num, gen);
        }
    }

    bool parse();
    bool init_encryption(const std::string& password = "");
    PdfObj resolve(const PdfObj& obj);
    PdfObj get_obj(int num);
    std::vector<uint8_t> decode_stream(const PdfObj& obj, int obj_num = -1, int gen_num = 0);

private:
    void parse_xref_table(size_t offset);
    void parse_xref_stream(size_t offset);
    void rebuild_xref();
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

// Rebuild xref by scanning for "N 0 obj" patterns (repair mode).
// Used when the xref table is missing, corrupted, or has wrong offsets.
void PdfDoc::rebuild_xref() {
    for (size_t i = 0; i + 5 < len; i++) {
        // Match digit(s) followed by " 0 obj"
        if (data[i] < '0' || data[i] > '9') continue;
        size_t j = i + 1;
        while (j < len && data[j] >= '0' && data[j] <= '9') j++;
        if (j + 5 >= len) continue;
        if (data[j] != ' ' || data[j+1] != '0' || data[j+2] != ' '
            || data[j+3] != 'o' || data[j+4] != 'b' || data[j+5] != 'j') continue;
        // Verify next char after "obj" is whitespace or delimiter
        if (j + 6 < len && !PdfLexer::is_ws(data[j+6])
            && !PdfLexer::is_delim(data[j+6])) continue;
        int num = 0;
        for (size_t k = i; k < j; k++) num = num * 10 + (data[k] - '0');
        if (xref.find(num) == xref.end()) {
            XrefEntry e;
            e.offset = static_cast<int64_t>(i);
            e.gen = 0;
            e.in_use = true;
            xref[num] = e;
        }
        i = j + 5;
    }
    // Scan for trailer dict
    if (trailer.is_none()) {
        const char* needle = "trailer";
        for (size_t i = 0; i + 7 < len; i++) {
            if (std::memcmp(data + i, needle, 7) == 0) {
                PdfLexer lex(data, len, i + 7);
                lex.skip_ws();
                trailer = lex.parse_object();
                break;
            }
        }
    }

    // Expand object streams found by the scan: pages/catalog of PDF 1.5+
    // files live inside compressed ObjStm containers, invisible to the
    // "N 0 obj" scan above.
    {
        std::vector<int> stm_nums;
        for (auto& [num, entry] : xref) {
            if (!entry.in_use || entry.offset <= 0) continue;
            PdfObj obj = get_obj(num);
            if (!obj.is_stream()) continue;
            auto& t = obj.get("Type");
            if (t.is_name() && t.str_val == "ObjStm") stm_nums.push_back(num);
        }
        for (int num : stm_nums) parse_obj_stream(num);
    }

    // If no trailer or trailer lacks /Root (e.g. file truncated past xref/trailer/%%EOF),
    // synthesize one by scanning reconstructed objects for /Type /Catalog.
    if (trailer.is_none() || !trailer.has("Root")) {
        int catalog_num = -1, catalog_gen = 0;
        for (auto& [num, entry] : xref) {
            if (!entry.in_use || (entry.offset <= 0 && entry.stream_obj < 0)) continue;
            PdfObj obj = get_obj(num);
            if (!obj.is_dict()) continue;
            auto& t = obj.get("Type");
            if (t.is_name() && t.str_val == "Catalog") {
                catalog_num = num;
                catalog_gen = entry.gen;
                break;
            }
        }
        if (catalog_num >= 0) {
            if (trailer.is_none()) trailer = PdfObj::make_dict();
            trailer.dict.push_back({"Root", PdfObj::make_ref(catalog_num, catalog_gen)});
        }
    }
}

bool PdfDoc::parse() {
    if (len < 10) return false;

    int64_t startxref = find_startxref();
    if (startxref >= 0 && startxref < (int64_t)len) {
        // Detect if xref is a table or a stream
        PdfLexer probe(data, len, static_cast<size_t>(startxref));
        probe.skip_ws();
        std::string first_tok;
        while (probe.pos < len && !PdfLexer::is_ws(data[probe.pos])
               && !PdfLexer::is_delim(data[probe.pos]))
            first_tok += static_cast<char>(data[probe.pos++]);

        if (first_tok == "xref")
            parse_xref_table(static_cast<size_t>(startxref));
        else
            parse_xref_stream(static_cast<size_t>(startxref));
    }

    // Fallback: rebuild xref by scanning for objects
    if (xref.empty())
        rebuild_xref();

    return !xref.empty();
}

bool PdfDoc::init_encryption(const std::string& password) {
    if (!trailer.has("Encrypt")) return true; // not encrypted
    auto& enc_ref = trailer.get("Encrypt");
    if (enc_ref.is_ref()) encrypt_ref_num = enc_ref.ref_num;
    auto encrypt_obj = resolve(enc_ref);
    if (!encrypt_obj.is_dict()) return false;

    // Check /Filter = /Standard
    auto& filter = encrypt_obj.get("Filter");
    if (filter.is_name() && filter.str_val != "Standard") return false;

    return crypt.init(trailer, encrypt_obj, password);
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
        size_t stream_start = lex.pos;

        // Validate Length: scan for "endstream" to find actual extent
        if (slen <= 0 || stream_start + slen > len) {
            // Length missing or out of bounds — find endstream
            const uint8_t* es = (const uint8_t*)pdf_memmem(
                data + stream_start, len - stream_start, "endstream", 9);
            if (es) {
                slen = es - (data + stream_start);
                while (slen > 0 && (data[stream_start + slen - 1] == '\n'
                       || data[stream_start + slen - 1] == '\r'))
                    slen--;
            }
        } else {
            // Length present — verify endstream is nearby
            size_t expected_end = stream_start + (size_t)slen;
            bool found_marker = false;
            for (size_t scan = expected_end; scan < len && scan < expected_end + 4; scan++) {
                if (scan + 9 <= len && std::memcmp(data + scan, "endstream", 9) == 0) {
                    found_marker = true;
                    break;
                }
            }
            if (!found_marker) {
                // Length is wrong — find actual endstream
                const uint8_t* es = (const uint8_t*)pdf_memmem(
                    data + stream_start, len - stream_start, "endstream", 9);
                if (es) {
                    slen = es - (data + stream_start);
                    while (slen > 0 && (data[stream_start + slen - 1] == '\n'
                           || data[stream_start + slen - 1] == '\r'))
                        slen--;
                }
            }
        }

        if (slen > 0 && stream_start + slen <= len) {
            obj.type = ObjType::STREAM;
            obj.stream_ptr = data + stream_start;
            obj.stream_len = static_cast<size_t>(slen);
        }
    }

    obj.src_num = num;
    obj.src_gen = entry.gen;
    // Decrypt string values held directly in top-level objects. Objects living
    // inside object streams are already plaintext (the container was decrypted),
    // and the /Encrypt dictionary itself is never encrypted.
    if (crypt.active && num != encrypt_ref_num && entry.stream_obj < 0)
        decrypt_strings(obj, num, entry.gen);
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

    int idx = 0;
    for (auto& [obj_num, obj_off] : entries) {
        int cur_idx = idx++;
        // Register in xref if absent (rebuilt files lack type-2 entries)
        if (xref.find(obj_num) == xref.end()) {
            XrefEntry e;
            e.in_use = true;
            e.stream_obj = stream_num;
            e.stream_idx = cur_idx;
            xref[obj_num] = e;
        }
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

std::vector<uint8_t> PdfDoc::decode_stream(const PdfObj& obj, int obj_num, int gen_num) {
    if (!obj.is_stream()) return {};
    // Callers rarely pass the owning object number — recover it from the
    // object itself so encrypted streams actually get decrypted.
    if (obj_num < 0 && obj.src_num >= 0) {
        obj_num = obj.src_num;
        gen_num = obj.src_gen;
    }
    // Use zero-copy pointer if available, otherwise use stored data
    std::vector<uint8_t> result;
    if (obj.stream_ptr && obj.stream_len > 0) {
        result.assign(obj.stream_ptr, obj.stream_ptr + obj.stream_len);
    } else {
        result = obj.stream_data;
    }

    // Decrypt stream data if encryption is active
    if (crypt.active && obj_num >= 0) {
        crypt.decrypt_data(result, obj_num, gen_num);
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
    bool is_type3 = false;       // Type3 font (1-byte codes, FontMatrix glyph space)
    bool is_dingbat = false;     // symbol font (Wingdings etc.) — letters are glyph codes, not text
    double glyph_space_scale = 0.001; // glyph-space→text-space width factor (FontMatrix[0] for Type3)
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
        if (tu != to_unicode.end()) {
            uint32_t u = tu->second;
            // Symbol fonts (Wingdings...): a "letter" mapping is really the
            // source glyph code (e.g. ž for a list bullet) — emit a bullet.
            if (is_dingbat && u > 0x20 && u < 0x2000) return 0x2022; // •
            return u;
        }

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
                       lower.find("black") != std::string::npos ||
                       lower.find("-medi") != std::string::npos;
        font.is_italic = lower.find("italic") != std::string::npos ||
                         lower.find("oblique") != std::string::npos;
        font.is_dingbat = lower.find("wingdings") != std::string::npos ||
                          lower.find("webdings") != std::string::npos ||
                          lower.find("dingbat") != std::string::npos;
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
    auto widths_arr = doc.resolve(fobj.get("Widths"));
    if (widths_arr.is_arr()) {
        int first_char = fobj.get("FirstChar").as_int();
        for (size_t i = 0; i < widths_arr.arr.size(); i++) {
            double w = widths_arr.arr[i].as_num();
            font.widths[static_cast<uint32_t>(first_char + i)] = w;
        }
    }

    if (font_type == "Type3") {
        font.is_type3 = true;
        // Glyph space is defined by FontMatrix (not the fixed 1/1000 of other fonts)
        auto fm = doc.resolve(fobj.get("FontMatrix"));
        if (fm.is_arr() && fm.arr.size() >= 4) {
            double a = fm.arr[0].as_num();
            if (a > 0) font.glyph_space_scale = a;
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

    // Simple fonts always use 1-byte codes regardless of the ToUnicode CMap's
    // codespacerange (HWP exports Type3 fonts with a <0000><FFFF> codespace,
    // which must not switch string decoding to 2-byte).
    if (font.is_type3) font.cmap_code_bytes = 1;

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
    int render_mode = 0;   // Tr: 2/6 = fill+stroke (faux bold in HWP exports)
    PdfFont* font = nullptr;

    // Graphics state for paths
    double stroke_r = 0, stroke_g = 0, stroke_b = 0;
    double fill_r = 0, fill_g = 0, fill_b = 0;
    double line_width = 1;
    int line_cap = 0, line_join = 0;
    double miter_limit = 10;
    bool in_text = false;
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
                                         const PdfObj& resources, double page_height,
                                         std::unordered_map<int, PdfFont>* font_cache = nullptr,
                                         bool skip_graphics = false,
                                         const double* initial_ctm = nullptr,
                                         int depth = 0) {
    ContentParseResult result;

    // Load fonts from resources, using cross-page cache when available
    std::unordered_map<std::string, PdfFont> fonts;
    auto res = doc.resolve(resources);
    auto& font_dict = res.get("Font");
    if (!font_dict.is_none()) {
        auto fd = doc.resolve(font_dict);
        if (fd.is_dict()) {
            for (auto& [name, ref] : fd.dict) {
                int rn = ref.is_ref() ? ref.ref_num : -1;
                if (font_cache && rn >= 0) {
                    auto it = font_cache->find(rn);
                    if (it != font_cache->end()) {
                        fonts[name] = it->second;
                        continue;
                    }
                }
                fonts[name] = load_font(doc, ref);
                if (font_cache && rn >= 0)
                    (*font_cache)[rn] = fonts[name];
            }
        }
    }

    std::vector<GfxState> state_stack;
    GfxState gs;
    if (initial_ctm) std::memcpy(gs.ctm, initial_ctm, sizeof(gs.ctm));
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
        double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
        double first_x = 0, first_y = 0, last_x = 0, last_y = 0;
        bool has_start = false;
        for (auto& pt : current_path) {
            if (pt.type == PathPoint::MOVE || pt.type == PathPoint::LINE) {
                if (!has_start) { first_x = pt.x; first_y = pt.y; has_start = true; }
                last_x = pt.x; last_y = pt.y;
                if (pt.x < min_x) min_x = pt.x;
                if (pt.x > max_x) max_x = pt.x;
                if (pt.y < min_y) min_y = pt.y;
                if (pt.y > max_y) max_y = pt.y;
            }
        }
        if (std::abs(first_x - last_x) < 2 && std::abs(first_y - last_y) < 2) {
            double w = max_x - min_x, h = max_y - min_y;
            // Thin horizontal rect (Word table border) → emit as h-line.
            if (h < 3.0 && w >= 20.0) {
                float cy = static_cast<float>((min_y + max_y) / 2.0);
                result.segments.push_back({static_cast<float>(min_x), cy,
                                           static_cast<float>(max_x), cy});
                return true;
            }
            // Thin vertical rect → emit as v-line.
            if (w < 3.0 && h >= 5.0) {
                float cx = static_cast<float>((min_x + max_x) / 2.0);
                result.segments.push_back({cx, static_cast<float>(min_y),
                                           cx, static_cast<float>(max_y)});
                return true;
            }
            if (h < 20.0) return true;
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
            // Inline integer parse to avoid strtoll overhead
            const uint8_t* ndata = lex.data + start;
            size_t nlen = lex.pos - start;
            if (!has_dot && nlen <= 10) {
                int64_t val = 0;
                bool neg = false;
                size_t i = 0;
                if (ndata[0] == '-') { neg = true; i = 1; }
                else if (ndata[0] == '+') { i = 1; }
                for (; i < nlen; i++) val = val * 10 + (ndata[i] - '0');
                operands.push_back(PdfObj::make_int(neg ? -val : val));
            } else {
                operands.push_back(PdfObj::make_real(
                    std::strtod(reinterpret_cast<const char*>(ndata), nullptr)));
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

        // Bare keyword → operator (zero-copy: compare via pointer+length)
        size_t saved = lex.pos;
        while (lex.pos < lex.len && !PdfLexer::is_ws(lex.data[lex.pos]) && !PdfLexer::is_delim(lex.data[lex.pos]))
            lex.pos++;
        size_t tok_len = lex.pos - saved;
        if (tok_len == 0) { lex.pos++; continue; }
        const char* tok_ptr = reinterpret_cast<const char*>(lex.data + saved);

        auto tok_eq = [&](const char* s) {
            size_t sl = std::strlen(s);
            return tok_len == sl && std::memcmp(tok_ptr, s, sl) == 0;
        };

        if (tok_eq("true")) { operands.push_back(PdfObj::make_bool(true)); continue; }
        if (tok_eq("false")) { operands.push_back(PdfObj::make_bool(false)); continue; }
        if (tok_eq("null")) continue;

        {

            // ── Graphics State ──
            if (tok_eq("q")) {
                state_stack.push_back(gs);
            } else if (tok_eq("Q")) {
                if (!state_stack.empty()) { gs = state_stack.back(); state_stack.pop_back(); }
            } else if (tok_eq("cm")) {
                if (operands.size() >= 6) {
                    double m[6] = {pop_num(5), pop_num(4), pop_num(3), pop_num(2), pop_num(1), pop_num(0)};
                    double r[6];
                    mat_multiply(r, m, gs.ctm);
                    std::memcpy(gs.ctm, r, sizeof(r));
                }
            } else if (tok_eq("w")) {
                gs.line_width = pop_num(0);
            } else if (tok_eq("J")) {
                gs.line_cap = static_cast<int>(pop_num(0));
            } else if (tok_eq("j")) {
                gs.line_join = static_cast<int>(pop_num(0));
            } else if (tok_eq("M")) {
                gs.miter_limit = pop_num(0);
            }

            // ── Color (skip when graphics not needed) ──
            else if (skip_graphics && (tok_eq("RG") || tok_eq("rg") || tok_eq("G") ||
                     tok_eq("g") || tok_eq("K") || tok_eq("k") || tok_eq("SC") ||
                     tok_eq("SCN") || tok_eq("sc") || tok_eq("scn") || tok_eq("CS") ||
                     tok_eq("cs"))) {
                // skip color ops
            }
            else if (tok_eq("RG")) {
                if (operands.size() >= 3) { gs.stroke_r = pop_num(2); gs.stroke_g = pop_num(1); gs.stroke_b = pop_num(0); }
            } else if (tok_eq("rg")) {
                if (operands.size() >= 3) { gs.fill_r = pop_num(2); gs.fill_g = pop_num(1); gs.fill_b = pop_num(0); }
            } else if (tok_eq("G")) {
                double g = pop_num(0); gs.stroke_r = gs.stroke_g = gs.stroke_b = g;
            } else if (tok_eq("g")) {
                double g = pop_num(0); gs.fill_r = gs.fill_g = gs.fill_b = g;
            } else if (tok_eq("K")) {
                if (operands.size() >= 4) {
                    double c = pop_num(3), m = pop_num(2), y = pop_num(1), k = pop_num(0);
                    gs.stroke_r = 1 - std::min(1.0, c + k);
                    gs.stroke_g = 1 - std::min(1.0, m + k);
                    gs.stroke_b = 1 - std::min(1.0, y + k);
                }
            } else if (tok_eq("k")) {
                if (operands.size() >= 4) {
                    double c = pop_num(3), m = pop_num(2), y = pop_num(1), k = pop_num(0);
                    gs.fill_r = 1 - std::min(1.0, c + k);
                    gs.fill_g = 1 - std::min(1.0, m + k);
                    gs.fill_b = 1 - std::min(1.0, y + k);
                }
            } else if (tok_eq("SC") || tok_eq("SCN")) {
                if (operands.size() >= 3) { gs.stroke_r = pop_num(2); gs.stroke_g = pop_num(1); gs.stroke_b = pop_num(0); }
                else if (operands.size() >= 1) { double g = pop_num(0); gs.stroke_r = gs.stroke_g = gs.stroke_b = g; }
            } else if (tok_eq("sc") || tok_eq("scn")) {
                if (operands.size() >= 3) { gs.fill_r = pop_num(2); gs.fill_g = pop_num(1); gs.fill_b = pop_num(0); }
                else if (operands.size() >= 1) { double g = pop_num(0); gs.fill_r = gs.fill_g = gs.fill_b = g; }
            } else if (tok_eq("CS") || tok_eq("cs")) {
                // Colorspace name — just consume
            }

            // ── Text ──
            else if (tok_eq("BT")) {
                double id[6] = {1,0,0,1,0,0};
                std::memcpy(gs.text_mat, id, sizeof(id));
                std::memcpy(gs.line_mat, id, sizeof(id));
                gs.in_text = true;
            } else if (tok_eq("ET")) {
                gs.in_text = false;
            } else if (tok_eq("Tf")) {
                if (operands.size() >= 2) {
                    gs.font_size = pop_num(0);
                    std::string fname = operands[operands.size() - 2].str_val;
                    auto it = fonts.find(fname);
                    gs.font = (it != fonts.end()) ? &it->second : nullptr;
                }
            } else if (tok_eq("Td")) {
                if (operands.size() >= 2) {
                    double tx = pop_num(1), ty = pop_num(0);
                    gs.line_mat[4] += tx * gs.line_mat[0] + ty * gs.line_mat[2];
                    gs.line_mat[5] += tx * gs.line_mat[1] + ty * gs.line_mat[3];
                    std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
                }
            } else if (tok_eq("TD")) {
                if (operands.size() >= 2) {
                    double tx = pop_num(1), ty = pop_num(0);
                    gs.text_leading = -ty;
                    gs.line_mat[4] += tx * gs.line_mat[0] + ty * gs.line_mat[2];
                    gs.line_mat[5] += tx * gs.line_mat[1] + ty * gs.line_mat[3];
                    std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
                }
            } else if (tok_eq("Tm")) {
                if (operands.size() >= 6) {
                    gs.text_mat[0] = pop_num(5); gs.text_mat[1] = pop_num(4);
                    gs.text_mat[2] = pop_num(3); gs.text_mat[3] = pop_num(2);
                    gs.text_mat[4] = pop_num(1); gs.text_mat[5] = pop_num(0);
                    std::memcpy(gs.line_mat, gs.text_mat, sizeof(gs.line_mat));
                }
            } else if (tok_eq("T*")) {
                gs.line_mat[4] += -gs.text_leading * gs.line_mat[2];
                gs.line_mat[5] += -gs.text_leading * gs.line_mat[3];
                std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
            } else if (tok_eq("TL")) {
                gs.text_leading = pop_num(0);
            } else if (tok_eq("Tc")) {
                gs.char_spacing = pop_num(0);
            } else if (tok_eq("Tw")) {
                gs.word_spacing = pop_num(0);
            } else if (tok_eq("Tz")) {
                gs.h_scaling = pop_num(0);
            } else if (tok_eq("Ts")) {
                gs.text_rise = pop_num(0);
            } else if (tok_eq("Tr")) {
                gs.render_mode = static_cast<int>(pop_num(0));
            }

            // ── Text Show ──
            else if (tok_eq("Tj") || tok_eq("'") || tok_eq("\"")) {
                if (tok_eq("'")) {
                    gs.line_mat[4] += -gs.text_leading * gs.line_mat[2];
                    gs.line_mat[5] += -gs.text_leading * gs.line_mat[3];
                    std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
                } else if (tok_eq("\"")) {
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
                    double gw_scale = (gs.font && gs.font->is_type3) ? gs.font->glyph_space_scale : 0.001;

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
                        // Private Use Area: unmappable glyphs (e.g. HWP equation fonts) — no text value
                        if ((unicode >= 0xE000 && unicode <= 0xF8FF) || unicode >= 0xFFFE) continue;

                        // Recompute rendering matrix for each char (text_mat changes with advances)
                        double trm[6];
                        double scale_mat[6] = {fs * h_scale, 0, 0, fs, 0, gs.text_rise};
                        mat_multiply(trm, scale_mat, gs.text_mat);
                        double final_mat[6];
                        mat_multiply(final_mat, trm, gs.ctm);

                        // Skip rotated text (vertical > horizontal direction)
                        if (std::abs(final_mat[1]) > std::abs(final_mat[0]) * 2) {
                            double glyph_w_skip = gs.font ? gs.font->get_width(code) : 0;
                            if (glyph_w_skip <= 0) glyph_w_skip = 600;
                            double adv = glyph_w_skip * gw_scale * fs * h_scale + gs.char_spacing;
                            if (unicode == ' ') adv += gs.word_spacing;
                            gs.text_mat[4] += adv * gs.text_mat[0];
                            gs.text_mat[5] += adv * gs.text_mat[1];
                            continue;
                        }

                        double gx, gy;
                        transform_point(final_mat, 0, 0, gx, gy);

                        double glyph_w = gs.font ? gs.font->get_width(code) : 0;
                        if (glyph_w <= 0) glyph_w = (gs.font && (gs.font->is_identity || gs.font->is_type0)) ? 1000 : 600;
                        // char_w in text space (used for text matrix advance)
                        double char_w_ts = glyph_w * gw_scale * fs * h_scale;
                        // char_w in page space (for bounding box)
                        double gx2, gy2;
                        transform_point(final_mat, glyph_w * gw_scale, 0, gx2, gy2);
                        double char_w = std::abs(gx2 - gx);
                        if (char_w < 0.1) char_w = std::abs(final_mat[0]) * glyph_w * gw_scale;
                        double char_h = std::abs(final_mat[3]);
                        if (char_h < 1) char_h = std::abs(final_mat[0]);

                        // Advance text position first, then compute right edge
                        double advance = char_w_ts + gs.char_spacing;
                        if (unicode == ' ') advance += gs.word_spacing;
                        gs.text_mat[4] += advance * gs.text_mat[0];
                        gs.text_mat[5] += advance * gs.text_mat[1];

                        // Right edge = next char position (from advanced text matrix)
                        double next_gx, next_gy;
                        {
                            double next_mat[6];
                            mat_multiply(next_mat, gs.text_mat, gs.ctm);
                            transform_point(next_mat, 0, 0, next_gx, next_gy);
                        }

                        TextChar tc;
                        tc.x = gx;
                        tc.y = gy;
                        tc.left = gx;
                        tc.right = next_gx;
                        tc.top = gy + char_h * 0.8;
                        tc.bot = gy - char_h * 0.2;
                        tc.font_size = char_h;
                        tc.unicode = unicode;
                        tc.is_bold = (gs.font && gs.font->is_bold) ||
                                     gs.render_mode == 2 || gs.render_mode == 6;
                        tc.is_italic = gs.font ? gs.font->is_italic : false;
                        result.chars.push_back(tc);
                    }
                }
            } else if (tok_eq("TJ")) {
                if (!operands.empty() && operands.back().is_arr()) {
                    auto& arr = operands.back().arr;
                    double fs = gs.font_size;
                    double h_scale = gs.h_scaling / 100.0;
                    double gw_scale = (gs.font && gs.font->is_type3) ? gs.font->glyph_space_scale : 0.001;
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
                                // Private Use Area: unmappable glyphs (e.g. HWP equation fonts) — no text value
                                if ((unicode >= 0xE000 && unicode <= 0xF8FF) || unicode >= 0xFFFE) continue;

                                double trm[6];
                                double scale_mat[6] = {fs * h_scale, 0, 0, fs, 0, gs.text_rise};
                                mat_multiply(trm, scale_mat, gs.text_mat);
                                double final_mat[6];
                                mat_multiply(final_mat, trm, gs.ctm);

                                double glyph_w = gs.font ? gs.font->get_width(code) : 0;
                                if (glyph_w <= 0) glyph_w = (gs.font && (gs.font->is_identity || gs.font->is_type0)) ? 1000 : 600;
                                double char_w_ts = glyph_w * gw_scale * fs * h_scale;

                                double advance = char_w_ts + gs.char_spacing;
                                if (unicode == ' ') advance += gs.word_spacing;
                                gs.text_mat[4] += advance * gs.text_mat[0];
                                gs.text_mat[5] += advance * gs.text_mat[1];

                                // Skip rotated text (vertical > horizontal direction)
                                if (std::abs(final_mat[1]) > std::abs(final_mat[0]) * 2)
                                    continue;

                                double gx, gy;
                                transform_point(final_mat, 0, 0, gx, gy);

                                double gx2, gy2;
                                transform_point(final_mat, glyph_w * gw_scale, 0, gx2, gy2);
                                double char_w = std::abs(gx2 - gx);
                                if (char_w < 0.1) char_w = std::abs(final_mat[0]) * glyph_w * gw_scale;
                                double char_h = std::abs(final_mat[3]);
                                if (char_h < 1) char_h = std::abs(final_mat[0]);

                                double next_gx, next_gy;
                                {
                                    double nm[6];
                                    mat_multiply(nm, gs.text_mat, gs.ctm);
                                    transform_point(nm, 0, 0, next_gx, next_gy);
                                }

                                TextChar tc;
                                tc.x = gx; tc.y = gy;
                                tc.left = gx; tc.right = next_gx;
                                tc.top = gy + char_h * 0.8;
                                tc.bot = gy - char_h * 0.2;
                                tc.font_size = char_h;
                                tc.unicode = unicode;
                                tc.is_bold = (gs.font && gs.font->is_bold) ||
                                             gs.render_mode == 2 || gs.render_mode == 6;
                                tc.is_italic = gs.font ? gs.font->is_italic : false;
                                result.chars.push_back(tc);
                            }
                        }
                    }
                }
            }

            // ── Path Construction (skip when graphics not needed) ──
            else if (skip_graphics && (tok_eq("m") || tok_eq("l") || tok_eq("c") ||
                     tok_eq("v") || tok_eq("y") || tok_eq("h") || tok_eq("re") ||
                     tok_eq("S") || tok_eq("s") || tok_eq("f") || tok_eq("F") ||
                     tok_eq("f*") || tok_eq("B") || tok_eq("B*") || tok_eq("b") ||
                     tok_eq("b*") || tok_eq("n") || tok_eq("W") || tok_eq("W*"))) {
                // skip path ops entirely
            }
            else if (tok_eq("m")) {
                if (operands.size() >= 2) {
                    double x = pop_num(1), y = pop_num(0);
                    double tx, ty;
                    transform_point(gs.ctm, x, y, tx, ty);
                    current_path.push_back({tx, ty, PathPoint::MOVE});
                }
            } else if (tok_eq("l")) {
                if (operands.size() >= 2) {
                    double x = pop_num(1), y = pop_num(0);
                    double tx, ty;
                    transform_point(gs.ctm, x, y, tx, ty);
                    current_path.push_back({tx, ty, PathPoint::LINE});
                }
            } else if (tok_eq("c")) {
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
            } else if (tok_eq("v")) {
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
            } else if (tok_eq("y")) {
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
            } else if (tok_eq("h")) {
                current_path.push_back({0, 0, PathPoint::CLOSE});
            } else if (tok_eq("re")) {
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
            else if (tok_eq("S")) {
                if (!filter_white_stroke() && !filter_small_rect())
                    flush_path_segments();
                { RenderPath rp; rp.points = current_path;
                  rp.stroke_r = gs.stroke_r; rp.stroke_g = gs.stroke_g; rp.stroke_b = gs.stroke_b;
                  rp.fill_r = gs.fill_r; rp.fill_g = gs.fill_g; rp.fill_b = gs.fill_b;
                  rp.line_width = gs.line_width; rp.do_fill = false; rp.do_stroke = true;
                  result.paths.push_back(std::move(rp)); }
                current_path.clear();
            } else if (tok_eq("s")) {
                current_path.push_back({0, 0, PathPoint::CLOSE});
                if (!filter_white_stroke() && !filter_small_rect())
                    flush_path_segments();
                { RenderPath rp; rp.points = current_path;
                  rp.stroke_r = gs.stroke_r; rp.stroke_g = gs.stroke_g; rp.stroke_b = gs.stroke_b;
                  rp.fill_r = gs.fill_r; rp.fill_g = gs.fill_g; rp.fill_b = gs.fill_b;
                  rp.line_width = gs.line_width; rp.do_fill = false; rp.do_stroke = true;
                  result.paths.push_back(std::move(rp)); }
                current_path.clear();
            } else if (tok_eq("f") || tok_eq("F") || tok_eq("f*")) {
                if (!filter_small_rect()) flush_path_segments();
                { RenderPath rp; rp.points = current_path;
                  rp.fill_r = gs.fill_r; rp.fill_g = gs.fill_g; rp.fill_b = gs.fill_b;
                  rp.stroke_r = gs.stroke_r; rp.stroke_g = gs.stroke_g; rp.stroke_b = gs.stroke_b;
                  rp.line_width = gs.line_width; rp.do_fill = true; rp.do_stroke = false;
                  result.paths.push_back(std::move(rp)); }
                current_path.clear();
            } else if (tok_eq("B") || tok_eq("B*") || tok_eq("b") || tok_eq("b*")) {
                if (tok_eq("b") || tok_eq("b*"))
                    current_path.push_back({0, 0, PathPoint::CLOSE});
                if (!filter_white_stroke() && !filter_small_rect())
                    flush_path_segments();
                { RenderPath rp; rp.points = current_path;
                  rp.fill_r = gs.fill_r; rp.fill_g = gs.fill_g; rp.fill_b = gs.fill_b;
                  rp.stroke_r = gs.stroke_r; rp.stroke_g = gs.stroke_g; rp.stroke_b = gs.stroke_b;
                  rp.line_width = gs.line_width; rp.do_fill = true; rp.do_stroke = true;
                  result.paths.push_back(std::move(rp)); }
                current_path.clear();
            } else if (tok_eq("n")) {
                current_path.clear();
            }

            // ── XObject (images) ──
            else if (tok_eq("Do")) {
                if (!operands.empty() && operands.back().is_name()) {
                    std::string xname = operands.back().str_val;
                    auto& xobjects = res.get("XObject");
                    auto xd = doc.resolve(xobjects);
                    if (xd.is_dict()) {
                        auto& xref = xd.get(xname);
                        auto xobj = doc.resolve(xref);
                        auto& subtype = xobj.get("Subtype");
                        bool is_form = subtype.is_name() && subtype.str_val == "Form";
                        if (is_form && depth < 8) {
                            // Form XObject: parse its content stream so text and
                            // vectors nested inside charts/figures are not lost.
                            auto form_stream = doc.decode_stream(xobj);
                            if (!form_stream.empty()) {
                                double form_ctm[6];
                                std::memcpy(form_ctm, gs.ctm, sizeof(form_ctm));
                                auto& mtx = xobj.get("Matrix");
                                if (mtx.is_arr() && mtx.arr.size() >= 6) {
                                    double fm[6];
                                    for (int k = 0; k < 6; k++) fm[k] = mtx.arr[k].as_num();
                                    mat_multiply(form_ctm, fm, gs.ctm);
                                }
                                auto& form_res = xobj.get("Resources");
                                const PdfObj& sub_res = form_res.is_none() ? res : form_res;
                                auto sub = parse_content_stream(
                                    doc, form_stream, sub_res, page_height,
                                    font_cache, skip_graphics, form_ctm, depth + 1);
                                result.chars.insert(result.chars.end(),
                                    sub.chars.begin(), sub.chars.end());
                                result.segments.insert(result.segments.end(),
                                    sub.segments.begin(), sub.segments.end());
                                result.images.insert(result.images.end(),
                                    sub.images.begin(), sub.images.end());
                                result.paths.insert(result.paths.end(),
                                    sub.paths.begin(), sub.paths.end());
                            }
                        } else {
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
    bool is_column_split = false;
    double y_center = 0;
    double x_left = 1e9;
    double x_right = 0;
};

struct PageCharCache {
    struct CharInfo {
        double x, y;
        double left, right, top, bot;
        double font_size;
        unsigned int unicode;
    };
    std::vector<CharInfo> chars;
    std::vector<size_t> y_sorted;

    void build(const std::vector<TextChar>& text_chars) {
        chars.reserve(text_chars.size());
        for (auto& tc : text_chars) {
            if (tc.unicode == 0 || tc.unicode == '\r' || tc.unicode == '\n' || tc.unicode == 0xFFFD) continue;
            chars.push_back({tc.x, tc.y, tc.left, tc.right, tc.top, tc.bot, tc.font_size, tc.unicode});
        }
        y_sorted.resize(chars.size());
        for (size_t i = 0; i < chars.size(); i++) y_sorted[i] = i;
        std::stable_sort(y_sorted.begin(), y_sorted.end(),
            [this](size_t a, size_t b) { return chars[a].y < chars[b].y; });
    }

    std::string get_text_in_rect(double left, double top, double right, double bottom) const {
        double rect_top = std::max(top, bottom);
        double rect_bot = std::min(top, bottom);
        double y_lo = rect_bot + 0.5, y_hi = rect_top - 0.5;
        auto lo_it = std::lower_bound(y_sorted.begin(), y_sorted.end(), y_lo,
            [this](size_t idx, double val) { return chars[idx].y < val; });
        auto hi_it = std::upper_bound(lo_it, y_sorted.end(), y_hi,
            [this](double val, size_t idx) { return val < chars[idx].y; });
        // Include a char if its horizontal center falls inside [left, right).
        // Outer cell edges get a small extra tolerance so glyphs that touch
        // the column boundary line are not dropped.
        std::vector<size_t> matches;
        for (auto it = lo_it; it != hi_it; ++it) {
            auto& ch = chars[*it];
            double cx = (ch.left + ch.right) * 0.5;
            if (cx >= left - 1.0 && cx < right + 1.0)
                matches.push_back(*it);
        }
        // Sort by reading order: top-to-bottom, then left-to-right.
        // Single-row cells will fall through to a stable left-to-right order;
        // multi-row cells (merged) read top-to-bottom.
        std::sort(matches.begin(), matches.end(), [this](size_t a, size_t b) {
            const auto& ca = chars[a];
            const auto& cb = chars[b];
            double y_tol = std::max(ca.font_size, cb.font_size) * 0.4;
            if (y_tol < 2.0) y_tol = 2.0;
            if (std::abs(ca.y - cb.y) > y_tol) return ca.y > cb.y;
            return ca.left < cb.left;
        });
        std::string text;
        double prev_right = -1e9;
        double prev_y = 0.0;
        double prev_fs = 12.0;
        bool first = true;
        for (size_t idx : matches) {
            auto& ch = chars[idx];
            double fs = ch.font_size > 1.0 ? ch.font_size : 12.0;
            if (!first) {
                double y_tol = std::max(prev_fs, fs) * 0.4;
                if (y_tol < 2.0) y_tol = 2.0;
                bool new_row = std::abs(ch.y - prev_y) > y_tol;
                if (new_row) {
                    if (!text.empty() && text.back() != ' ') text += ' ';
                } else {
                    // Insert a space when the positional gap exceeds the
                    // word-spacing threshold used by chars_to_lines.
                    double gap = ch.left - prev_right;
                    double word_gap = fs * 0.15;
                    if (word_gap < 1.0) word_gap = 1.0;
                    if (ch.unicode == ' ' || ch.unicode == 0xA0) {
                        if (!text.empty() && text.back() != ' ') text += ' ';
                    } else if (gap > word_gap && !text.empty() && text.back() != ' ') {
                        text += ' ';
                    }
                }
            }
            if (ch.unicode != ' ' && ch.unicode != 0xA0)
                util::append_utf8(text, ch.unicode);
            prev_right = ch.right;
            prev_y = ch.y;
            prev_fs = fs;
            first = false;
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

// Detect vertical column boundary by counting distinct Y-rows per X-bin.
// Full-width rows contribute equally to all bins; column rows only to their
// column's bins, creating a dip at the column gap.
// Returns the x-coordinate of the column separator, or 0 if single-column.
double detect_column_boundary(const std::vector<TextChar>& chars,
                              double median_fs, double y_tol) {
    double page_left = 1e9, page_right = 0;
    for (auto& ch : chars) {
        if (ch.left < page_left) page_left = ch.left;
        if (ch.right > page_right) page_right = ch.right;
    }
    double page_width = page_right - page_left;
    if (page_width < median_fs * 30) return 0;

    // Group chars into Y-rows
    std::vector<size_t> y_sorted(chars.size());
    std::iota(y_sorted.begin(), y_sorted.end(), 0);
    std::sort(y_sorted.begin(), y_sorted.end(), [&](size_t a, size_t b) {
        return chars[a].y > chars[b].y;
    });

    constexpr int NUM_BINS = 200;
    int row_count[NUM_BINS] = {};
    int total_rows = 0;

    size_t ri = 0;
    while (ri < y_sorted.size()) {
        double row_y = chars[y_sorted[ri]].y;
        bool bins_hit[NUM_BINS] = {};
        while (ri < y_sorted.size() && std::abs(chars[y_sorted[ri]].y - row_y) <= y_tol) {
            auto& ch = chars[y_sorted[ri]];
            if (ch.unicode != ' ' && ch.unicode != 0xA0) {
                int b0 = static_cast<int>((ch.left - page_left) / page_width * NUM_BINS);
                int b1 = static_cast<int>((ch.right - page_left) / page_width * NUM_BINS);
                if (b0 < 0) b0 = 0;
                if (b1 >= NUM_BINS) b1 = NUM_BINS - 1;
                for (int b = b0; b <= b1; b++) bins_hit[b] = true;
            }
            ri++;
        }
        for (int b = 0; b < NUM_BINS; b++)
            if (bins_hit[b]) row_count[b]++;
        total_rows++;
    }

    if (total_rows < 10) return 0;

    // Find the deepest dip in row_count within center 50% of page
    int center_start = NUM_BINS / 4;
    int center_end = NUM_BINS * 3 / 4;

    double left_avg = 0, right_avg = 0;
    int lc = 0, rc = 0;
    for (int b = NUM_BINS / 10; b < center_start; b++) { left_avg += row_count[b]; lc++; }
    for (int b = center_end; b < NUM_BINS * 9 / 10; b++) { right_avg += row_count[b]; rc++; }
    if (lc > 0) left_avg /= lc;
    if (rc > 0) right_avg /= rc;
    double body_avg = (left_avg + right_avg) / 2.0;
    if (body_avg < 5) return 0;

    // Find the minimum row_count in center region (smoothed over 3 bins)
    int best_bin = -1;
    double best_val = 1e9;
    for (int b = center_start + 1; b < center_end - 1; b++) {
        double val = (row_count[b - 1] + row_count[b] + row_count[b + 1]) / 3.0;
        if (val < best_val) { best_val = val; best_bin = b; }
    }

    // The dip must be significantly lower than body average (at least 30% lower)
    if (best_val > body_avg * 0.7) return 0;

    return page_left + (best_bin + 0.5) / NUM_BINS * page_width;
}

// Reorder lines so that within each column band, left-column lines
// come before right-column lines. Spanning lines stay in place.
std::vector<TextLine> reorder_column_lines(std::vector<TextLine>& lines,
                                           double col_boundary) {
    enum Type { LEFT, RIGHT, SPANNING };

    // Compute content width for SPANNING minimum width threshold
    double min_left = 1e9, max_right = 0;
    for (auto& l : lines) {
        if (l.x_left < min_left) min_left = l.x_left;
        if (l.x_right > max_right) max_right = l.x_right;
    }
    double content_width = max_right - min_left;
    double span_min_width = content_width * 0.6;
    double page_center = (min_left + max_right) / 2.0;

    std::vector<Type> types(lines.size());
    for (size_t i = 0; i < lines.size(); i++) {
        auto& l = lines[i];
        double line_width = l.x_right - l.x_left;
        double line_center = (l.x_left + l.x_right) / 2.0;
        bool straddles = l.x_left < col_boundary - 5 && l.x_right > col_boundary + 5;
        bool is_wide = line_width > span_min_width;
        bool is_centered = straddles && std::abs(line_center - page_center) < content_width * 0.15;
        if (straddles && (is_wide || is_centered))
            types[i] = SPANNING;
        else if ((l.x_left + l.x_right) / 2.0 < col_boundary)
            types[i] = LEFT;
        else
            types[i] = RIGHT;
    }

    std::vector<TextLine> result;
    result.reserve(lines.size());
    size_t i = 0;
    while (i < lines.size()) {
        if (types[i] == SPANNING) {
            result.push_back(std::move(lines[i]));
            i++;
            continue;
        }
        size_t band_end = i + 1;
        while (band_end < lines.size() && types[band_end] != SPANNING)
            band_end++;
        for (size_t j = i; j < band_end; j++)
            if (types[j] == LEFT) {
                lines[j].is_column_split = true;
                result.push_back(std::move(lines[j]));
            }
        for (size_t j = i; j < band_end; j++)
            if (types[j] == RIGHT) {
                lines[j].is_column_split = true;
                result.push_back(std::move(lines[j]));
            }
        i = band_end;
    }
    return result;
}

std::vector<TextLine> chars_to_lines(const std::vector<TextChar>& chars,
                                    double* out_col_boundary = nullptr) {
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

    double col_boundary = detect_column_boundary(chars, median_fs, y_tol);
    if (out_col_boundary) *out_col_boundary = col_boundary;

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

    // Column-gutter gap threshold: large enough to skip word spaces (~0.15×fs)
    // but small enough to catch tight body-text gutters (~1.2×fs).
    double col_gap_thresh = std::max(median_fs * 1.2, 8.0);

    // Peek-ahead helper: count distinct span clusters in the chars of the
    // current y-row that lie strictly to the right of col_boundary, starting
    // from index start. Two chars belong to the same cluster if their gap is
    // smaller than col_gap_thresh (same as the split threshold below).
    auto right_clusters = [&](size_t start, double cur_y_val) -> int {
        if (col_boundary <= 0) return 0;
        int clusters = 0;
        double last_right = -1e9;
        for (size_t k = start; k < idx.size(); k++) {
            auto& c = chars[idx[k]];
            if (std::abs(c.y - cur_y_val) > y_tol) break;
            if (c.unicode == ' ' || c.unicode == 0xA0) continue;
            if (c.left <= col_boundary) continue;
            if (last_right < -1e8 || c.left - last_right > col_gap_thresh) {
                clusters++;
                if (clusters >= 2) return clusters;
            }
            last_right = std::max(last_right, (double)c.right);
        }
        return clusters;
    };

    for (size_t ii = 0; ii < idx.size(); ii++) {
        auto& ch = chars[idx[ii]];
        if (std::abs(ch.y - cur_y) > y_tol) {
            flush();
            cur_y = ch.y;
        }

        // Split line at column boundary when a large gap crosses it.
        // Skip the split when the right side has multiple distinct cell
        // clusters — that signals a wide table row spanning the page, not
        // two body-text columns sharing a y coordinate. A *very* wide gap
        // (≥ 2×median_fs, ~20pt for 10pt body text) is always treated as a
        // page-gutter split, even when both sides have cell-like content,
        // so two tables sitting side-by-side at the same y get separated.
        if (col_boundary > 0 && !cur.text.empty() && prev_right > -1e8) {
            double gap = ch.left - prev_right;
            if (gap > col_gap_thresh &&
                prev_right < col_boundary && ch.left > col_boundary) {
                bool gutter = gap > std::max(median_fs * 2.0, 18.0);
                if (gutter || right_clusters(ii, cur_y) < 2) {
                    flush();
                    cur_y = ch.y;
                }
            }
        }

        cur.y_center = ch.y;
        if (ch.left < cur.x_left) cur.x_left = ch.left;
        if (ch.right > cur.x_right) cur.x_right = ch.right;

        // Detect word spacing using gap between this char's left and previous char's right
        if (!cur.text.empty() && ch.unicode != ' ' && ch.unicode != 0xA0 && prev_right > -1e8) {
            double gap = ch.left - prev_right;
            // Use font-size-relative threshold for word spacing
            double word_gap = ch.font_size * 0.15;
            if (word_gap < 1) word_gap = 1;
            if (gap > word_gap && gap < ch.font_size * 8 && cur.text.back() != ' ')
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

    if (col_boundary > 0)
        lines = reorder_column_lines(lines, col_boundary);

    return lines;
}

// ── PDF-specific types ───────────────────────────────────

struct TableData {
    std::vector<std::vector<std::string>> rows;
    std::string title;  // full-width title row extracted from top of table
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
                    counts[static_cast<int>(l.font_size * 10)]++;

        int max_c = 0, max_k = 120;
        for (auto& [k, c] : counts)
            if (c > max_c) { max_c = c; max_k = k; }
        body_size = max_k / 10.0;
        if (body_size < 4.0) body_size = 12.0;
    }

    int heading_level(double fs, bool is_bold = false) const {
        if (fs <= 0) return 0;
        double r = fs / body_size;
        if (r >= 1.8) return 1;
        if (r >= 1.5) return 2;
        if (r >= 1.3) return 3;
        if (is_bold && r >= 1.1) return 3;
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
        const std::vector<PdfLineSegment>& h_lines,
        double table_left, double table_right,
        double table_bot, double table_top,
        const std::vector<double>& row_ys) {
    double table_height = table_top - table_bot;

    // Compute average row height for gap tolerance
    double avg_row_h = table_height;
    if (row_ys.size() >= 2) {
        avg_row_h = (row_ys.back() - row_ys.front()) / (double)(row_ys.size() - 1);
    }
    double gap_tol = std::max(3.0, avg_row_h * 0.6);

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

    struct VLineInfo { double x, coverage; int seg_count; };
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
        int seg_count = (int)intervals.size();
        std::sort(intervals.begin(), intervals.end());
        double total = 0, cur_lo = intervals[0].first, cur_hi = intervals[0].second;
        for (size_t i = 1; i < intervals.size(); i++) {
            if (intervals[i].first <= cur_hi + gap_tol)
                cur_hi = std::max(cur_hi, intervals[i].second);
            else { total += cur_hi - cur_lo; cur_lo = intervals[i].first; cur_hi = intervals[i].second; }
        }
        total += cur_hi - cur_lo;
        vline_infos.push_back({cx, total, seg_count});
    }

    // Accept column boundary:
    // - High coverage (≥ 50%): strong continuous v-line
    // - Many segments (Word→PDF cell-unit borders, ≥ 1/3 of rows)
    // - Moderate coverage (≥ 15%) with h-line endpoint evidence at ≥ half of row levels
    int min_segs = std::max(2, static_cast<int>(row_ys.size() / 3));
    std::vector<double> col_xs;
    col_xs.push_back(table_left);
    for (auto& vi : vline_infos) {
        if (vi.x <= table_left + 5 || vi.x >= table_right - 5) continue;
        // Count row levels where h-lines terminate at this v-line x (true grid evidence)
        int rows_with_ep = 0;
        for (double ry : row_ys) {
            for (auto& hl : h_lines) {
                double hy = (hl.y0 + hl.y1) / 2.0;
                if (std::abs(hy - ry) > 4.0) continue;
                double hx_lo = std::min((double)hl.x0, (double)hl.x1);
                double hx_hi = std::max((double)hl.x0, (double)hl.x1);
                if (std::abs(hx_lo - vi.x) < 8.0 || std::abs(hx_hi - vi.x) < 8.0) {
                    rows_with_ep++;
                    break;
                }
            }
        }
        bool accept = vi.coverage >= table_height * 0.5 ||
                      vi.seg_count >= min_segs ||
                      (vi.coverage >= table_height * 0.15 &&
                       rows_with_ep >= (int)row_ys.size() / 2);
        if (accept)
            col_xs.push_back(vi.x);
    }
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
                int segs = 0;
                for (auto& vi : vline_infos)
                    if (std::abs(vi.x - col_xs[i]) < 6.0) { cov = vi.coverage; segs = vi.seg_count; break; }
                if (cov >= table_height * 0.5 || segs >= min_segs)
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

    auto col_xs = find_column_boundaries(v_lines, h_lines, table_left, table_right,
                                          table_bot, table_top, row_ys);

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
            // Clamp to h-line grid boundaries — don't extend beyond the table
            double grid_top = row_ys.back() + half;
            double grid_bot = row_ys.front() - half;
            actual_ys.push_back(std::max(table_row_centers.front() - half, grid_bot));
            for (size_t i = 0; i < table_row_centers.size() - 1; i++)
                actual_ys.push_back((table_row_centers[i] + table_row_centers[i + 1]) / 2.0);
            actual_ys.push_back(std::min(table_row_centers.back() + half, grid_top));
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

    // Detect merged cells: check v-line presence at each internal boundary per row
    // has_vline[r][b] = true if a v-line exists near col_xs[b] spanning row r
    std::vector<std::vector<bool>> has_vline(n_rows, std::vector<bool>(n_cols + 1, true));
    for (int r = 0; r < n_rows; r++) {
        double row_bot = std::min(actual_ys[r], actual_ys[r + 1]);
        double row_top = std::max(actual_ys[r], actual_ys[r + 1]);
        double row_h = row_top - row_bot;
        for (int b = 1; b < n_cols; b++) {
            double cx = col_xs[b];
            bool found = false;
            for (auto& vl : v_lines) {
                double vx = (vl.x0 + vl.x1) / 2.0;
                if (std::abs(vx - cx) > 6.0) continue;
                double vy_lo = std::min((double)vl.y0, (double)vl.y1);
                double vy_hi = std::max((double)vl.y0, (double)vl.y1);
                double overlap = std::min(vy_hi, row_top) - std::max(vy_lo, row_bot);
                if (overlap >= row_h * 0.3) {
                    found = true;
                    break;
                }
            }
            has_vline[r][b] = found;
        }
    }

    // Check v-line grid density: a real table has v-lines in most row/boundary positions.
    // Stray v-lines from body text have sparse coverage.
    // Only skip text continuation rejection for dense grids (real tables with merged cells).
    int vline_present = 0, vline_total = n_rows * (n_cols - 1);
    bool has_merged_cells = false;
    for (int r = 0; r < n_rows; r++)
        for (int b = 1; b < n_cols; b++)
            if (has_vline[r][b]) vline_present++;
    // Dense grid: >= 40% of positions have v-lines AND some are missing (merged cells)
    if (vline_total > 0 && vline_present < vline_total && vline_present >= vline_total * 0.4)
        has_merged_cells = true;

    table.rows.resize(n_rows);
    for (int r = 0; r < n_rows; r++) {
        table.rows[r].resize(total_cols);
        int c = 0;
        while (c < n_cols) {
            // Determine span: extend while no v-line at next boundary
            int span = 1;
            while (c + span < n_cols && !has_vline[r][c + span])
                span++;

            double left   = col_xs[c];
            double right  = col_xs[c + span];
            double bottom = actual_ys[r];
            double top    = actual_ys[r + 1];

            if (c + span - 1 == last_col_idx && n_sub > 1 && span == 1) {
                if (is_scale_row(cache, left, right, bottom, top, sub_boundaries)) {
                    for (int sc = 0; sc < n_sub; sc++) {
                        table.rows[r][n_cols - 1 + sc] = cache.get_text_in_rect(
                            sub_boundaries[sc], top, sub_boundaries[sc+1], bottom);
                    }
                } else {
                    table.rows[r][c] = cache.get_text_in_rect(left, top, right, bottom);
                }
            } else {
                table.rows[r][c] = cache.get_text_in_rect(left, top, right, bottom);
            }
            c += span;
        }
    }

    std::reverse(table.rows.begin(), table.rows.end());
    trim_table(table);

    // Extract title rows: rows at top where only one cell has content,
    // the table has 3+ columns, and the text is long (form titles inside table borders).
    // Skip for 2-column tables where single-fill rows are normal (key-value pairs).
    if (n_cols >= 3) {
        while (table.rows.size() >= 3) {
            auto& row = table.rows.front();
            int filled = 0;
            for (int c = 0; c < (int)row.size(); c++)
                if (!row[c].empty()) filled++;
            if (filled != 1 || row[0].empty()) break;
            // Must be long text (>30 bytes) to look like a title, not a short label
            if (row[0].size() <= 30) break;
            if (!table.title.empty()) table.title += " ";
            table.title += row[0];
            table.rows.erase(table.rows.begin());
        }
    }

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
        if (total_cells > 0 && empty_cells > total_cells * 0.75) {
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

    // Reject tables where text continues across column boundaries
    // (body text split by vertical lines — not real tabular data)
    // SKIP when merged cells detected: v-line grid confirms real table structure.
    if (!table.rows.empty() && !has_merged_cells) {
        int n_cols_t = (int)table.rows[0].size();
        // Detect word continuation: Latin alphanumeric at both boundaries.
        // CJK characters are self-contained units (not word fragments),
        // so only check Latin for cross-boundary continuation.
        // Real CJK tables have independent data in each cell.
        // Body text split in CJK is caught by the empty cell ratio and
        // the long_first checks instead.
        auto is_content = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };

        if (n_cols_t <= 3) {
            int cont_rows = 0, checked = 0;
            for (auto& row : table.rows) {
                if ((int)row.size() < 2 || row[0].empty() || row[1].empty()) continue;
                checked++;
                if (is_content((unsigned char)row[0].back()) && is_content((unsigned char)row[1][0]))
                    cont_rows++;
            }
            if (checked >= 3 && cont_rows >= checked * 0.15)
                table.rows.clear();
            if (!table.rows.empty() && n_cols_t == 2) {
                int long_first = 0;
                for (auto& row : table.rows) {
                    if (row.size() >= 2 && row[0].size() > 100) long_first++;
                }
                if (long_first >= 2) table.rows.clear();
            }
        } else if (n_cols_t >= 4) {
            // 4+ columns: Latin-only continuation check + empty column check
            int cont_rows = 0, checked = 0;
            for (auto& row : table.rows) {
                if ((int)row.size() < n_cols_t) continue;
                int pairs_ok = 0, pairs_cont = 0;
                for (int c = 0; c + 1 < n_cols_t; c++) {
                    if (row[c].empty() || row[c+1].empty()) continue;
                    pairs_ok++;
                    unsigned char lc = (unsigned char)row[c].back();
                    unsigned char rc = (unsigned char)row[c+1][0];
                    // Latin-only: CJK chars are independent units, not word fragments
                    bool latin_cont = is_content(lc) && is_content(rc) && lc < 0x80 && rc < 0x80;
                    if (latin_cont) pairs_cont++;
                }
                checked++;
                // Require ALL non-empty pairs to show Latin continuation.
                // Data tables often have alphanumeric chars at cell boundaries
                // (e.g. phone → email) but not ALL pairs will continue.
                if (pairs_ok >= 2 && pairs_cont == pairs_ok) cont_rows++;
            }
            if (checked >= 3 && cont_rows >= checked * 0.5)
                table.rows.clear();

            // Reject if second column is mostly empty (body text + stray v-lines)
            if (!table.rows.empty() && (int)table.rows.size() >= 15) {
                int col1_trivial = 0;
                for (auto& row : table.rows)
                    if ((int)row.size() >= 2 && row[1].size() <= 2) col1_trivial++;
                if (col1_trivial >= (int)table.rows.size() * 0.7)
                    table.rows.clear();
            }
        }
    }

    // Reject tables where most content concentrates in one column
    // while others are mostly empty — body text split by stray vertical lines.
    if (!table.rows.empty() && (int)table.rows.size() >= 3) {
        int n_cols_t = (int)table.rows[0].size();
        int nr = (int)table.rows.size();
        // Find the column with most content
        int best_col = 0;
        size_t best_len = 0;
        for (int c = 0; c < n_cols_t; c++) {
            size_t total_len = 0;
            for (auto& row : table.rows)
                if (c < (int)row.size()) total_len += row[c].size();
            if (total_len > best_len) { best_len = total_len; best_col = c; }
        }
        // Check if all other columns are mostly empty
        int empty_other_cols = 0;
        for (int c = 0; c < n_cols_t; c++) {
            if (c == best_col) continue;
            int empty = 0;
            for (auto& row : table.rows)
                if (c < (int)row.size() && row[c].empty()) empty++;
            if (empty >= nr / 2) empty_other_cols++;
        }
        // If ALL other columns are mostly empty and the main column has long text
        int long_rows = 0;
        for (auto& row : table.rows)
            if (best_col < (int)row.size() && row[best_col].size() > 30) long_rows++;
        // When all non-primary columns are mostly empty AND their total
        // content is < 5% of the primary column, it's body text split
        // by stray v-lines, not real tabular data.
        if (empty_other_cols == n_cols_t - 1 && best_len > 0) {
            size_t other_len = 0;
            for (int c = 0; c < n_cols_t; c++) {
                if (c == best_col) continue;
                for (auto& row : table.rows)
                    if (c < (int)row.size()) other_len += row[c].size();
            }
            if (other_len * 10 < best_len)
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
    int n_bins = std::max(20, static_cast<int>(width / 5.0));
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
    int threshold = std::max(1, static_cast<int>(non_empty_rows * 0.4));

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
        double row_gap = y_hi - y_lo;
        for (auto& vl : v_lines) {
            double vy_lo = std::min((double)vl.y0, (double)vl.y1);
            double vy_hi = std::max((double)vl.y0, (double)vl.y1);
            // Full span: v-line covers the entire row gap
            if (vy_lo <= y_lo + 5.0 && vy_hi >= y_hi - 5.0) {
                connected[i] = true;
                break;
            }
            // Partial span: v-line overlaps >= 50% of row gap (cell-unit v-lines)
            double overlap = std::min(vy_hi, y_hi) - std::max(vy_lo, y_lo);
            if (overlap >= row_gap * 0.5) {
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
        // Include row if v-line connected, h-lines share span,
        // or h-lines share span with a small gap (merged-cell rows in forms)
        bool h_span_ok = x_overlap[i] && close_enough;
        if (connected[i] || h_span_ok) {
            current_group.push_back(row_ys[i + 1]);
            if (connected[i]) group_vline_connections++;
        } else if (close_enough && group_vline_connections > 0) {
            // No v-line and no x-overlap, but we're already in a connected group.
            // Check if the next row's h-lines share x-range with the group's h-lines.
            double g_left = 1e9, g_right = 0;
            for (auto& hl : h_lines) {
                double hy = (hl.y0 + hl.y1) / 2.0;
                for (auto& gy : current_group) {
                    if (std::abs(hy - gy) < 4.0) {
                        double lx = std::min((double)hl.x0, (double)hl.x1);
                        double rx = std::max((double)hl.x0, (double)hl.x1);
                        if (lx < g_left) g_left = lx;
                        if (rx > g_right) g_right = rx;
                        break;
                    }
                }
            }
            double n_left = 1e9, n_right = 0;
            for (auto& hl : h_lines) {
                double hy = (hl.y0 + hl.y1) / 2.0;
                if (std::abs(hy - row_ys[i + 1]) < 4.0) {
                    double lx = std::min((double)hl.x0, (double)hl.x1);
                    double rx = std::max((double)hl.x0, (double)hl.x1);
                    if (lx < n_left) n_left = lx;
                    if (rx > n_right) n_right = rx;
                }
            }
            double extent = std::max(g_right - g_left, n_right - n_left);
            double overlap = std::min(g_right, n_right) - std::max(g_left, n_left);
            if (extent > 50 && overlap >= extent * 0.7) {
                current_group.push_back(row_ys[i + 1]);
            } else {
                if (current_group.size() >= 3 &&
                    (group_vline_connections > 0 || current_group.size() >= 7))
                    table_groups.push_back(current_group);
                current_group.clear();
                group_vline_connections = 0;
                current_group.push_back(row_ys[i + 1]);
            }
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

    // Merge adjacent groups that share the same h-line x-range.
    // Handles forms where some row intervals lack v-lines (merged cells)
    // but the h-lines clearly continue the same table.
    if (table_groups.size() >= 2) {
        auto h_x_range = [&](const std::vector<double>& group) -> std::pair<double,double> {
            double lo = 1e9, hi = 0;
            for (auto& hl : h_lines) {
                double hy = (hl.y0 + hl.y1) / 2.0;
                for (auto& ry : group) {
                    if (std::abs(hy - ry) < 4.0) {
                        double lx = std::min((double)hl.x0, (double)hl.x1);
                        double rx = std::max((double)hl.x0, (double)hl.x1);
                        if (lx < lo) lo = lx;
                        if (rx > hi) hi = rx;
                        break;
                    }
                }
            }
            return {lo, hi};
        };

        std::vector<std::vector<double>> merged;
        merged.push_back(table_groups[0]);
        for (size_t g = 1; g < table_groups.size(); g++) {
            auto [lo1, hi1] = h_x_range(merged.back());
            auto [lo2, hi2] = h_x_range(table_groups[g]);
            double extent = std::max(hi1 - lo1, hi2 - lo2);
            double overlap = std::min(hi1, hi2) - std::max(lo1, lo2);
            double y_gap = table_groups[g].front() - merged.back().back();
            if (extent > 50 && overlap >= extent * 0.7 && y_gap < 100) {
                for (auto& y : table_groups[g])
                    merged.back().push_back(y);
            } else {
                merged.push_back(table_groups[g]);
            }
        }
        table_groups = std::move(merged);
    }

    if (table_groups.empty()) return {};

    // Split groups where v-line column structure changes significantly
    std::vector<std::vector<double>> final_groups;
    for (auto& group : table_groups) {
        if (group.size() < 5) { final_groups.push_back(group); continue; }
        int n = (int)group.size() - 1;

        // Count internal v-lines for each row interval
        std::vector<int> row_vcount(n, 0);
        double gl = 1e9, gr = 0;
        for (auto& hl : h_lines) {
            double hy = (hl.y0 + hl.y1) / 2.0;
            for (auto& ry : group)
                if (std::abs(hy - ry) < 4.0) {
                    double lx = std::min((double)hl.x0, (double)hl.x1);
                    double rx = std::max((double)hl.x0, (double)hl.x1);
                    if (lx < gl) gl = lx;
                    if (rx > gr) gr = rx;
                    break;
                }
        }
        for (int i = 0; i < n; i++) {
            double y_lo = group[i], y_hi = group[i + 1];
            double row_h = y_hi - y_lo;
            for (auto& vl : v_lines) {
                double vx = (vl.x0 + vl.x1) / 2.0;
                if (vx <= gl + 10 || vx >= gr - 10) continue;
                double vy_lo = std::min((double)vl.y0, (double)vl.y1);
                double vy_hi = std::max((double)vl.y0, (double)vl.y1);
                double overlap = std::min(vy_hi, y_hi) - std::max(vy_lo, y_lo);
                if (overlap >= row_h * 0.3) row_vcount[i]++;
            }
        }

        // Find split points: a row with 0-1 internal v-lines is a split
        // only if BOTH neighbors have >= 3 v-lines (true section boundary).
        // A single transition (many → few) is just a merged-cell area.
        std::vector<int> splits;
        for (int i = 1; i < n - 1; i++) {
            if (row_vcount[i] <= 1 &&
                row_vcount[i - 1] >= 3 && row_vcount[i + 1] >= 3)
                splits.push_back(i);
        }

        if (splits.empty()) { final_groups.push_back(group); continue; }

        int start = 0;
        for (int sp : splits) {
            std::vector<double> sub(group.begin() + start, group.begin() + sp + 1);
            if (sub.size() >= 3) final_groups.push_back(sub);
            start = sp;
        }
        std::vector<double> last(group.begin() + start, group.end());
        if (last.size() >= 3) final_groups.push_back(last);
    }

    std::vector<TableData> result;
    for (auto& group : final_groups) {
        TableData t = build_table(group, h_lines, v_lines, cache);
        if (t.rows.empty()) continue;
        // Reject grids that swallowed page prose (stacked separate tables
        // bridged across body text): no real cell holds a whole paragraph.
        // Rejecting lets the band's lines flow back as normal text.
        size_t max_cell = 0;
        for (auto& row : t.rows)
            for (auto& c : row)
                if (c.size() > max_cell) max_cell = c.size();
        if (max_cell > 300) continue;
        result.push_back(std::move(t));
    }

    // Detach a trailing caption row ("표 4.2 ...", "그림 ...") that was
    // absorbed when stacked tables were bridged across the caption line;
    // it belongs to the table directly below as its title.
    for (auto& t : result) {
        if (t.rows.size() < 2) continue;
        auto& last = t.rows.back();
        int filled = 0;
        std::string text;
        for (auto& c : last)
            if (!c.empty()) { filled++; text = c; }
        if (filled != 1 || text.size() < 8) continue;
        bool is_caption = text.rfind("\xED\x91\x9C ", 0) == 0 ||          // "표 "
                          text.rfind("\xEA\xB7\xB8\xEB\xA6\xBC ", 0) == 0; // "그림 "
        if (!is_caption) continue;
        double bottom = std::min(t.y0, t.y1);
        TableData* below = nullptr;
        for (auto& u : result) {
            if (&u == &t) continue;
            double utop = std::max(u.y0, u.y1);
            if (utop <= bottom + 5.0 &&
                (!below || utop > std::max(below->y0, below->y1)))
                below = &u;
        }
        if (below && below->title.empty()) {
            below->title = text;
            t.rows.pop_back();
        }
    }
    return result;
}

// ── Pure text-based table detection (column-first) ──────────────────
//
// Algorithm overview (see bench/TABLE_DETECTION_REDESIGN.md):
//  S1. group chars into rows; find "y-bands" = runs of multi-cell rows
//      (allow 1-cell rows interleaved; split when ≥N consecutive 1-cell rows)
//  S2. inside each band, build an x histogram of char ranges from multi-cell
//      rows; locate consecutive bins that are empty in ≥70% of those rows
//      and wider than max(median_fs*0.7, 7.0) → column boundary
//  S3. assign chars of each row to columns; word-gap based space recovery
//  S4. rejection: list-like, caption, continuation, numeric-cell ratio
namespace text_tables {

struct CharInfo { double x, y, left, right, top, bot; unsigned int unicode; };

struct TextRow {
    double y_center;
    double y_top, y_bot;
    std::vector<std::pair<double,double>> char_ranges;   // per-char [left,right]
    std::vector<size_t> char_indices;                    // indices into chars
    double x_min, x_max;
    bool is_multi_cell;
};

struct YBand {
    size_t first_row;     // inclusive
    size_t last_row;      // inclusive
    double y_top, y_bot;
    double x_min, x_max;
};

// helper: is a row "multi-cell" given a cell-merge gap (≥ gap → multi-cell)
static bool row_is_multi_cell(const TextRow& tr, double cell_merge_gap) {
    if (tr.char_ranges.size() < 2) return false;
    auto sorted = tr.char_ranges;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); i++) {
        if (sorted[i].first - sorted[i-1].second >= cell_merge_gap)
            return true;
    }
    return false;
}

// S1: find y-bands of consecutive multi-cell rows (with bounded 1-cell rows)
static std::vector<YBand> find_y_bands(const std::vector<TextRow>& rows) {
    std::vector<YBand> bands;
    const int kMaxSingleRunInside = 2;   // ≥3 consecutive 1-cell rows splits a band

    size_t i = 0;
    while (i < rows.size()) {
        // find next multi-cell row
        while (i < rows.size() && !rows[i].is_multi_cell) i++;
        if (i >= rows.size()) break;

        size_t band_start = i;
        size_t band_end = i;          // last multi-cell row in band
        int multi = 1;
        int single_run = 0;
        size_t j = i + 1;
        while (j < rows.size()) {
            // y-gap sanity: if the row is too far below the previous tracked row, stop
            const auto& prev = rows[(band_end > i) ? band_end : i];
            double vgap = std::abs(prev.y_bot - rows[j].y_top);
            // Use the typical line spacing as a guard; allow up to 3x font-size
            double fs_guard = std::max(prev.y_top - prev.y_bot, 8.0) * 3.5;
            if (vgap > fs_guard) break;

            if (rows[j].is_multi_cell) {
                band_end = j;
                multi++;
                single_run = 0;
            } else {
                single_run++;
                if (single_run > kMaxSingleRunInside) break;
            }
            j++;
        }

        if (multi >= 2) {
            YBand b;
            b.first_row = band_start;
            b.last_row  = band_end;
            b.y_top = rows[band_start].y_top;
            b.y_bot = rows[band_end].y_bot;
            // include any 1-cell rows that sit between band_start and band_end
            for (size_t k = band_start; k <= band_end; k++) {
                b.y_top = std::max(b.y_top, rows[k].y_top);
                b.y_bot = std::min(b.y_bot, rows[k].y_bot);
            }

            // x extent from multi-cell rows in the band
            double xl = 1e18, xr = -1e18;
            for (size_t k = band_start; k <= band_end; k++) {
                if (!rows[k].is_multi_cell) continue;
                xl = std::min(xl, rows[k].x_min);
                xr = std::max(xr, rows[k].x_max);
            }
            b.x_min = xl;
            b.x_max = xr;
            bands.push_back(b);
        }
        i = (band_end >= i) ? (band_end + 1) : (i + 1);
    }
    return bands;
}

// S2: column boundaries inside a band by x-bin histogram of multi-cell rows.
//   Returns boundaries (left, inner col-edges, right) — empty on failure.
static std::vector<double> infer_columns_in_band(
        const std::vector<TextRow>& rows, const YBand& band,
        double median_fs) {
    // Collect multi-cell rows in the band
    std::vector<size_t> mc;
    for (size_t k = band.first_row; k <= band.last_row; k++)
        if (rows[k].is_multi_cell) mc.push_back(k);
    if (mc.size() < 2) return {};

    double x_lo = band.x_min, x_hi = band.x_max;
    if (x_hi - x_lo < 30) return {};
    double bin_w = std::max(median_fs * 0.15, 1.0);
    int n_bins = std::max(8, (int)std::ceil((x_hi - x_lo) / bin_w));
    bin_w = (x_hi - x_lo) / n_bins;

    // hit_count[b] = number of multi-cell rows that have a char overlapping bin b
    std::vector<int> hit_count(n_bins, 0);

    auto bin_idx = [&](double x) -> int {
        int b = (int)std::floor((x - x_lo) / bin_w);
        if (b < 0) b = 0;
        if (b >= n_bins) b = n_bins - 1;
        return b;
    };

    for (size_t ri : mc) {
        // mark bins covered by any char range in this row
        std::vector<bool> row_hit(n_bins, false);
        for (auto& cr : rows[ri].char_ranges) {
            int b0 = bin_idx(cr.first);
            int b1 = bin_idx(cr.second);
            for (int b = b0; b <= b1; b++) row_hit[b] = true;
        }
        for (int b = 0; b < n_bins; b++) if (row_hit[b]) hit_count[b]++;
    }

    int total_mc = (int)mc.size();
    // empty bin: ≤ 40% of multi-cell rows have a char there. A bin still has
    // to be a *gap* — adjacent occupied bins on both sides — to be a column
    // boundary, so a slightly looser empty threshold helps catch tables where
    // not every row has every column populated.
    double empty_thresh_frac = 0.40;
    int empty_max = (int)std::floor(total_mc * empty_thresh_frac);

    double col_gap_min = std::max(median_fs * 0.4, 3.0);

    // sweep for runs of empty bins inside [x_lo+, x_hi-]
    std::vector<std::pair<double,double>> empty_runs;   // (start_x, end_x)
    int run_start = -1;
    for (int b = 0; b < n_bins; b++) {
        bool is_empty = hit_count[b] <= empty_max;
        if (is_empty) {
            if (run_start < 0) run_start = b;
        } else {
            if (run_start >= 0) {
                double sx = x_lo + run_start * bin_w;
                double ex = x_lo + b * bin_w;
                empty_runs.push_back({sx, ex});
                run_start = -1;
            }
        }
    }
    if (run_start >= 0) {
        double sx = x_lo + run_start * bin_w;
        double ex = x_lo + n_bins * bin_w;
        empty_runs.push_back({sx, ex});
    }

    std::vector<double> col_edges;
    for (auto& run : empty_runs) {
        double width = run.second - run.first;
        if (width < col_gap_min) continue;
        double mid = (run.first + run.second) / 2.0;
        // skip runs hugging the band edges (those are just margins)
        if (mid <= x_lo + 2.0) continue;
        if (mid >= x_hi - 2.0) continue;
        col_edges.push_back(mid);
    }
    if (col_edges.empty()) return {};

    // Validate: for each candidate boundary, ≥70% of multi-cell rows that
    // overlap a neighborhood of the boundary must "straddle" it (chars on
    // both sides). Rows that have no chars near the boundary are ignored — a
    // row of body text on the opposite side of the page does not invalidate
    // a column boundary inside a data table.
    std::vector<double> kept;
    double neigh = std::max(median_fs * 6.0, 60.0);
    for (double e : col_edges) {
        int agree = 0;
        int relevant = 0;
        for (size_t ri : mc) {
            bool has_left = false, has_right = false;
            bool near = false;
            for (auto& cr : rows[ri].char_ranges) {
                if (cr.second <= e) has_left = true;
                else if (cr.first >= e) has_right = true;
                if (cr.first <= e + neigh && cr.second >= e - neigh) near = true;
                if (has_left && has_right && near) break;
            }
            if (!near) continue;
            relevant++;
            if (has_left && has_right) agree++;
        }
        int needed = std::max(2, (int)std::ceil(relevant * 0.70));
        if (relevant >= 2 && agree >= needed) kept.push_back(e);
    }
    if (kept.empty()) return {};

    std::vector<double> bounds;
    bounds.push_back(x_lo);
    for (double e : kept) bounds.push_back(e);
    bounds.push_back(x_hi);
    return bounds;
}

// S3: build the table from a band + columns.
// For each row, snap each inner column boundary to the nearest natural gap
// in that row (so we don't split words). Falls back to the global boundary if
// no usable gap is nearby.
static TableData build_table_from_band(
        const std::vector<TextRow>& rows, const YBand& band,
        const std::vector<double>& col_bounds,
        const std::vector<CharInfo>& chars,
        double median_fs) {
    TableData table;
    int n_cols = (int)col_bounds.size() - 1;
    if (n_cols < 2) return table;

    table.x0 = col_bounds.front();
    table.x1 = col_bounds.back();
    table.y0 = band.y_bot;
    table.y1 = band.y_top;

    double word_gap   = std::max(median_fs * 0.15, 1.2);
    double snap_tol   = std::max(median_fs * 2.0, 15.0);
    double min_split_gap = std::max(median_fs * 0.5, 4.0);

    for (size_t k = band.first_row; k <= band.last_row; k++) {
        const auto& tr = rows[k];
        // gather chars sorted by x (use char_indices into the per-page chars[])
        std::vector<size_t> ci = tr.char_indices;
        std::sort(ci.begin(), ci.end(), [&](size_t a, size_t b) {
            return chars[a].x < chars[b].x;
        });

        // Find natural gap midpoints in this row (gaps between consecutive chars
        // ≥ min_split_gap), used for snapping column boundaries.
        std::vector<std::pair<double,double>> gap_runs;   // (gap_start, gap_end)
        for (size_t i = 1; i < ci.size(); i++) {
            double prev_r = chars[ci[i-1]].right;
            double cur_l  = chars[ci[i]].left;
            if (cur_l - prev_r >= min_split_gap) {
                gap_runs.push_back({prev_r, cur_l});
            }
        }

        // For each inner boundary, snap to nearest gap midpoint within snap_tol.
        std::vector<double> row_bounds(col_bounds.size());
        row_bounds.front() = col_bounds.front();
        row_bounds.back()  = col_bounds.back();
        for (int c = 1; c < (int)col_bounds.size() - 1; c++) {
            double e = col_bounds[c];
            double best = e;
            double best_d = 1e9;
            for (auto& g : gap_runs) {
                double gm = (g.first + g.second) / 2.0;
                double d = std::abs(gm - e);
                if (d < best_d && d <= snap_tol) {
                    best_d = d;
                    best = gm;
                }
            }
            row_bounds[c] = best;
        }
        // Ensure monotonic
        for (int c = 1; c < (int)row_bounds.size(); c++) {
            if (row_bounds[c] < row_bounds[c-1] + 0.1)
                row_bounds[c] = row_bounds[c-1] + 0.1;
        }

        std::vector<std::string> cells(n_cols);
        std::vector<double> last_right(n_cols, -1e9);

        for (size_t idx : ci) {
            const auto& ch = chars[idx];
            double cmid = (ch.left + ch.right) / 2.0;
            int col = -1;
            for (int c = 0; c < n_cols; c++) {
                // Use strict less-than-or-equal on the right edge so chars that
                // sit exactly on a column boundary fall into the LEFT column —
                // this avoids the first letter of a word being pushed across
                // the boundary in some rows when boundaries snap tightly.
                double lo = row_bounds[c] - 0.5;
                double hi = (c == n_cols - 1) ? row_bounds[c+1] + 1.0
                                              : row_bounds[c+1];
                if (cmid >= lo && cmid < hi) {
                    col = c;
                    break;
                }
            }
            if (col < 0) {
                // fall back: leftmost or rightmost
                if (cmid < row_bounds.front()) col = 0;
                else col = n_cols - 1;
            }
            if (!cells[col].empty() && (ch.left - last_right[col]) >= word_gap)
                cells[col] += ' ';
            util::append_utf8(cells[col], ch.unicode);
            last_right[col] = ch.right;
        }

        for (auto& c : cells) c = util::trim(c);
        table.rows.push_back(std::move(cells));
    }
    return table;
}

// Strip leading/trailing prose columns: if the leftmost or rightmost column
// has very long cells (avg > 40, many > 30 chars) while ≥2 other columns are
// short numeric-style (avg < 12), drop the prose column. Body text alongside
// a real table.
static void strip_prose_columns(TableData& table) {
    size_t stripped_bytes = 0;
    while (!table.rows.empty() && !table.rows[0].empty()) {
        int n_cols = (int)table.rows[0].size();
        if (n_cols < 3) return;
        auto col_stats = [&](int c) -> std::tuple<double,int,int> {
            double sum = 0;
            int cnt = 0;
            int long_n = 0;
            int total_rows = (int)table.rows.size();
            for (auto& row : table.rows) {
                if (c >= (int)row.size()) continue;
                if (row[c].empty()) continue;
                sum += row[c].size();
                cnt++;
                if (row[c].size() > 30) long_n++;
            }
            double avg = cnt > 0 ? sum / cnt : 0.0;
            return {avg, long_n, cnt > 0 ? (cnt * 100 / total_rows) : 0};
        };

        auto [avg_first, long_first, fill_first] = col_stats(0);
        auto [avg_last, long_last, fill_last]    = col_stats(n_cols - 1);

        // Strip a prose edge column iff it is significantly longer (avg > 2x)
        // than any non-edge column, and contains many cells > 30 chars.
        double max_other_avg = 0;
        for (int c = 1; c < n_cols - 1; c++) {
            auto [a, ln, fil] = col_stats(c);
            if (a > max_other_avg) max_other_avg = a;
        }

        bool stripped = false;
        // Left edge prose
        if (avg_first > 35 && long_first >= 3 && fill_first >= 50 &&
            max_other_avg > 0 && avg_first > max_other_avg * 1.8) {
            for (auto& row : table.rows)
                if (!row.empty()) {
                    stripped_bytes += row.front().size();
                    row.erase(row.begin());
                }
            stripped = true;
        }
        // Right edge prose (recompute n_cols if changed)
        else if (avg_last > 35 && long_last >= 3 && fill_last >= 50 &&
                 max_other_avg > 0 && avg_last > max_other_avg * 1.8) {
            for (auto& row : table.rows)
                if (!row.empty()) {
                    stripped_bytes += row.back().size();
                    row.pop_back();
                }
            stripped = true;
        }
        if (!stripped) break;
    }

    // If the stripped prose held the majority of the band's text, this was
    // never a table with body text beside it — it was prose with a column-ish
    // fringe. Reject outright so the text flows back as normal lines.
    if (stripped_bytes > 0) {
        size_t kept_bytes = 0;
        for (auto& row : table.rows)
            for (auto& c : row) kept_bytes += c.size();
        if (stripped_bytes > kept_bytes) table.rows.clear();
    }
}

// S4: rejection / cleanup heuristics.
// Returns true if the table is acceptable (kept).
static bool accept_table(TableData& table) {
    if (table.rows.empty()) return false;
    // pre-step: strip body-text columns adjacent to the table
    strip_prose_columns(table);
    if (table.rows.empty()) return false;
    int n_cols = (int)table.rows[0].size();
    if (n_cols < 2) return false;

    // count rows with ≥2 filled cells
    int meaningful = 0;
    for (auto& row : table.rows) {
        int filled = 0;
        for (auto& c : row) if (!c.empty()) filled++;
        if (filled >= 2) meaningful++;
    }
    // Three rows is the minimum for a real multi-column table — anything
    // smaller is almost always a stray body paragraph that happened to
    // have a short token in a column-like position. (2-col tables stay at 4
    // because key-value lists are easy to mistake otherwise.)
    int min_rows = (n_cols == 2) ? 4 : 3;
    if (meaningful < min_rows) return false;

    // Merge continuation rows: row with a single filled cell in column c, after
    // a row whose column c was already filled → join with " " in same cell.
    for (size_t r = 1; r < table.rows.size(); r++) {
        int filled = 0;
        int filled_col = -1;
        for (int c = 0; c < n_cols; c++) {
            if (!table.rows[r][c].empty()) { filled++; filled_col = c; }
        }
        if (filled == 1 && filled_col >= 0 && r > 0 &&
            !table.rows[r-1][filled_col].empty()) {
            table.rows[r-1][filled_col] += " ";
            table.rows[r-1][filled_col] += table.rows[r][filled_col];
            table.rows.erase(table.rows.begin() + r);
            r--;
        }
    }
    if ((int)table.rows.size() < 2) return false;

    // Reject tables with too many empty cells (likely a degenerate band).
    {
        int total = 0, empty_n = 0;
        for (auto& row : table.rows) {
            for (auto& c : row) {
                total++;
                if (c.empty()) empty_n++;
            }
        }
        if (total > 0 && empty_n > total * 0.65) return false;
    }

    // Reject tables whose cells hold whole sentences — a band of prose that
    // was force-fit into a grid (real table cells are short values/labels).
    {
        size_t sum = 0, filled = 0, max_cell = 0;
        for (auto& row : table.rows) {
            for (auto& c : row) {
                if (c.empty()) continue;
                filled++;
                sum += c.size();
                if (c.size() > max_cell) max_cell = c.size();
            }
        }
        if (max_cell > 250) return false;
        if (filled >= 4 && sum > filled * 60) return false;
    }

    // Reject tables where most cells consist only of non-ASCII junk characters.
    {
        int total = 0, junk = 0;
        for (auto& row : table.rows) {
            for (auto& c : row) {
                if (c.empty()) continue;
                total++;
                int letters = 0, digits = 0;
                for (char ch : c) {
                    if ((ch >= 'a' && ch <= 'z') ||
                        (ch >= 'A' && ch <= 'Z')) letters++;
                    else if (ch >= '0' && ch <= '9') digits++;
                }
                if (letters + digits == 0) junk++;
            }
        }
        if (total >= 4 && junk >= total * 0.4) return false;
    }

    // --- list / prose rejection heuristics (mirrored from v1, tightened) ---
    auto is_lower_or_multibyte = [](unsigned char c) -> bool {
        return (c >= 'a' && c <= 'z') || c >= 0x80;
    };
    auto is_filler_only = [](const std::string& s) -> bool {
        for (char c : s)
            if (c != '.' && c != ',' && c != ' ' && c != '\t' &&
                c != ';' && c != ':' && c != '-')
                return false;
        return !s.empty();
    };

    // numbered-list detection (1) 2) 3) etc)
    {
        int marker_rows = 0, content_rows = 0;
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
        if (content_rows >= 2 && marker_rows >= content_rows * 0.6) return false;
    }

    // continuation rows (lower-letter ↔ lower-letter across column boundary)
    {
        int continuation_rows = 0;
        int checked_rows = 0;
        int filler_cells = 0;
        int total_cells = 0;
        int hyphen_end_cells = 0;
        for (auto& row : table.rows) {
            if ((int)row.size() < n_cols) continue;
            for (auto& cell : row) {
                if (cell.empty()) continue;
                total_cells++;
                if (is_filler_only(cell)) filler_cells++;
                // hyphen-wrap: cell ends with '-' preceded by a letter (text wrap)
                if (cell.size() >= 2 && cell.back() == '-') {
                    unsigned char prev = (unsigned char)cell[cell.size() - 2];
                    if ((prev >= 'a' && prev <= 'z') ||
                        (prev >= 'A' && prev <= 'Z') || prev >= 0x80)
                        hyphen_end_cells++;
                }
            }
            int pairs_checked = 0, pairs_continued = 0;
            for (int c = 0; c + 1 < n_cols; c++) {
                if (row[c].empty() || row[c+1].empty()) continue;
                pairs_checked++;
                if (is_lower_or_multibyte((unsigned char)row[c].back()) &&
                    is_lower_or_multibyte((unsigned char)row[c+1][0]))
                    pairs_continued++;
            }
            if (pairs_checked > 0 && pairs_continued > pairs_checked / 2)
                continuation_rows++;
            checked_rows++;
        }
        double ct = (n_cols == 2) ? 0.15 : 0.30;
        if (checked_rows >= 2 && continuation_rows >= checked_rows * ct)
            return false;
        if (total_cells > 0 && filler_cells >= total_cells * 0.35)
            return false;
        // Hyphen-wrap rejection: prose tends to break words at line ends.
        // Skip this when the table is clearly numeric (>= 30% cells have digits)
        // so that real tables surrounded by prose aren't lost.
        {
            int has_digits = 0;
            for (auto& row : table.rows)
                for (auto& c : row) {
                    if (c.empty()) continue;
                    for (char ch : c) if (ch >= '0' && ch <= '9') { has_digits++; break; }
                }
            bool numeric_table = total_cells > 0 && has_digits >= total_cells * 0.30;
            if (!numeric_table && total_cells >= 4 &&
                hyphen_end_cells >= total_cells * 0.20)
                return false;
        }
    }

    // 2-column body-text heuristic
    if (n_cols <= 3) {
        int total_rows = 0;
        double sum_first = 0, sum_second = 0;
        int unbalanced = 0;
        for (auto& row : table.rows) {
            if (row.empty() || row[0].empty()) continue;
            total_rows++;
            sum_first += row[0].size();
            if (n_cols >= 2 && row.size() >= 2) sum_second += row[1].size();
            if (n_cols == 2 && row.size() >= 2 && !row[1].empty() &&
                row[0].size() > row[1].size() * 5) unbalanced++;
        }
        if (total_rows >= 3) {
            double avg_first = sum_first / total_rows;
            double avg_second = (n_cols >= 2) ? sum_second / total_rows : 0;
            if (avg_first > 30 && avg_second > 0 && avg_first > avg_second * 2.5)
                return false;
            if (n_cols == 2 && avg_first > 30 && avg_second > 30)
                return false;
            if (unbalanced >= total_rows * 0.4) return false;
        }
    }

    // --- S5: numeric-cell ratio sanity check for narrow tables ---
    // If table has very few "tabular" cues (numbers, short tokens), reject —
    // *unless* col 0 is consistently a short label and the rest is description
    // (a definitions table).
    {
        int data_cells = 0;
        int numeric_cells = 0;
        int short_cells = 0;
        for (auto& row : table.rows) {
            for (auto& c : row) {
                if (c.empty()) continue;
                data_cells++;
                bool has_digit = false;
                for (char ch : c) {
                    if (ch >= '0' && ch <= '9') { has_digit = true; break; }
                }
                if (has_digit) numeric_cells++;
                if (c.size() <= 8) short_cells++;
            }
        }
        if (data_cells >= 6 && numeric_cells == 0 && n_cols >= 2) {
            int long_cells = 0;
            int short_col0 = 0, col0_filled = 0;
            for (auto& row : table.rows) {
                for (auto& c : row) if (c.size() > 30) long_cells++;
                if (!row.empty() && !row[0].empty()) {
                    col0_filled++;
                    if (row[0].size() <= 20) short_col0++;
                }
            }
            // definitions table: col 0 short labels in ≥70% of rows
            bool is_definitions = col0_filled >= 3 &&
                                  short_col0 >= col0_filled * 0.70;
            if (!is_definitions && long_cells >= data_cells * 0.3) return false;
        }
    }

    trim_table(table);
    if (table.rows.empty()) return false;
    return true;
}

} // namespace text_tables

std::vector<TableData> detect_text_tables(const PageCharCache& cache,
                                           const std::vector<TableData>& existing_tables,
                                           double page_width, double page_height) {
    using namespace text_tables;
    if (cache.chars.size() < 10) return {};

    std::vector<CharInfo> chars;
    chars.reserve(cache.chars.size());
    for (auto& ch : cache.chars) {
        if (ch.unicode == ' ' || ch.unicode == '\t' || ch.unicode == 0xA0) continue;
        if (ch.x < 0 || ch.x > page_width || ch.y < 0 || ch.y > page_height) continue;
        chars.push_back({ch.x, ch.y, ch.left, ch.right, ch.top, ch.bot,
                         ch.unicode});
    }
    if (chars.size() < 10) return {};

    // sort top-to-bottom (y descending in PDF coords)
    std::sort(chars.begin(), chars.end(), [](const CharInfo& a, const CharInfo& b) {
        return a.y > b.y;
    });

    // median font size (top-bot)
    std::vector<double> fsizes;
    fsizes.reserve(chars.size());
    for (auto& ch : chars) {
        double h = ch.top - ch.bot;
        if (h > 0) fsizes.push_back(h);
    }
    double median_fs = 10.0;
    if (!fsizes.empty()) {
        std::nth_element(fsizes.begin(), fsizes.begin() + fsizes.size() / 2,
                         fsizes.end());
        median_fs = fsizes[fsizes.size() / 2];
    }

    // build TextRows
    std::vector<TextRow> rows;
    {
        TextRow cur;
        cur.y_center = chars[0].y;
        cur.y_top = chars[0].top;
        cur.y_bot = chars[0].bot;
        cur.char_ranges.push_back({chars[0].left, chars[0].right});
        cur.char_indices.push_back(0);
        auto finalize_row = [](TextRow& r) {
            r.x_min = 1e18;
            r.x_max = -1e18;
            for (auto& cr : r.char_ranges) {
                if (cr.first < r.x_min) r.x_min = cr.first;
                if (cr.second > r.x_max) r.x_max = cr.second;
            }
        };
        for (size_t i = 1; i < chars.size(); i++) {
            if (std::abs(chars[i].y - cur.y_center) < std::max(median_fs * 0.4, 3.0)) {
                cur.char_ranges.push_back({chars[i].left, chars[i].right});
                cur.char_indices.push_back(i);
                cur.y_top = std::max(cur.y_top, chars[i].top);
                cur.y_bot = std::min(cur.y_bot, chars[i].bot);
                cur.y_center = (cur.y_center * (cur.char_ranges.size() - 1) +
                                chars[i].y) / cur.char_ranges.size();
            } else {
                if (!cur.char_ranges.empty()) {
                    finalize_row(cur);
                    rows.push_back(std::move(cur));
                }
                cur = TextRow();
                cur.y_center = chars[i].y;
                cur.y_top = chars[i].top;
                cur.y_bot = chars[i].bot;
                cur.char_ranges.push_back({chars[i].left, chars[i].right});
                cur.char_indices.push_back(i);
            }
        }
        if (!cur.char_ranges.empty()) {
            finalize_row(cur);
            rows.push_back(std::move(cur));
        }
    }
    if (rows.size() < 3) return {};

    // multi-cell: at least one gap ≥ cell_merge_gap
    double cell_merge_gap = std::max(median_fs * 0.8, 8.0);
    for (auto& r : rows) {
        r.is_multi_cell = row_is_multi_cell(r, cell_merge_gap);
    }

    // drop rows entirely inside an existing line-based table
    auto row_in_existing = [&](const TextRow& r) {
        for (auto& t : existing_tables) {
            double tb = std::min(t.y0, t.y1) - 5.0;
            double tt = std::max(t.y0, t.y1) + 5.0;
            if (r.y_center >= tb && r.y_center <= tt) return true;
        }
        return false;
    };
    for (auto& r : rows) {
        if (row_in_existing(r)) {
            r.is_multi_cell = false;
        }
    }

    // S1: find y-bands
    auto bands = find_y_bands(rows);
    if (bands.empty()) return {};

    std::vector<TableData> result;
    for (auto& band : bands) {
        // Bibliography bands ("[1] Author, ..." reference lists) are justified
        // prose whose stretched word gaps mimic columns — never tables.
        {
            bool has_biblio_row = false;
            for (size_t k = band.first_row; k <= band.last_row && !has_biblio_row; k++) {
                const auto& tr = rows[k];
                size_t first_idx = SIZE_MAX, second_idx = SIZE_MAX;
                for (size_t idx : tr.char_indices) {
                    if (first_idx == SIZE_MAX || chars[idx].x < chars[first_idx].x) {
                        second_idx = first_idx;
                        first_idx = idx;
                    } else if (second_idx == SIZE_MAX || chars[idx].x < chars[second_idx].x) {
                        second_idx = idx;
                    }
                }
                if (first_idx != SIZE_MAX && second_idx != SIZE_MAX &&
                    chars[first_idx].unicode == '[' &&
                    chars[second_idx].unicode >= '0' && chars[second_idx].unicode <= '9')
                    has_biblio_row = true;
            }
            if (has_biblio_row) continue;
        }

        // S2: infer columns
        auto bounds = infer_columns_in_band(rows, band, median_fs);
        if (bounds.size() < 3) continue;        // need ≥1 inner boundary

        // S3: build cells
        TableData table = build_table_from_band(rows, band, bounds, chars,
                                                median_fs);

        // S4-S5: rejection
        if (!accept_table(table)) continue;

        result.push_back(std::move(table));
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

// pixels_to_png moved to common/png_encode.h
using util::pixels_to_png;

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
                                                 const std::string& output_dir,
                                                 unsigned min_image_size = 0) {
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
        if (min_image_size > 0 &&
            (static_cast<unsigned>(w) < min_image_size ||
             static_cast<unsigned>(h) < min_image_size))
            continue;

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
            bool is_indexed = false;
            int indexed_hival = 0;
            std::vector<uint8_t> indexed_lookup;
            int indexed_base_comps = 3;

            if (cs_name == "DeviceGray" || cs_name == "CalGray") components = 1;
            else if (cs_name == "DeviceCMYK") components = 4;
            else if (cs_name == "DeviceRGB" || cs_name == "CalRGB") components = 3;
            else if (cs_name == "ICCBased") {
                if (cs_obj.is_arr() && cs_obj.arr.size() >= 2) {
                    auto icc_stream = doc.resolve(cs_obj.arr[1]);
                    int n = icc_stream.get("N").as_int();
                    if (n > 0) components = n;
                }
            } else if (cs_name == "Indexed" || cs_name == "I") {
                // Indexed (palette) color space: [/Indexed base hival lookup]
                is_indexed = true;
                components = 1; // index values are single-byte
                if (cs_obj.is_arr() && cs_obj.arr.size() >= 4) {
                    // Base color space
                    auto base_cs = doc.resolve(cs_obj.arr[1]);
                    if (base_cs.is_name()) {
                        if (base_cs.str_val == "DeviceRGB" || base_cs.str_val == "CalRGB")
                            indexed_base_comps = 3;
                        else if (base_cs.str_val == "DeviceCMYK")
                            indexed_base_comps = 4;
                        else if (base_cs.str_val == "DeviceGray" || base_cs.str_val == "CalGray")
                            indexed_base_comps = 1;
                    } else if (base_cs.is_arr() && !base_cs.arr.empty()) {
                        auto bn = doc.resolve(base_cs.arr[0]);
                        if (bn.is_name() && bn.str_val == "ICCBased" && base_cs.arr.size() >= 2) {
                            auto icc = doc.resolve(base_cs.arr[1]);
                            int n = icc.get("N").as_int();
                            if (n > 0) indexed_base_comps = n;
                        }
                    }
                    // hival
                    indexed_hival = doc.resolve(cs_obj.arr[2]).as_int();
                    // lookup table (string or stream)
                    auto lut = doc.resolve(cs_obj.arr[3]);
                    if (lut.is_str()) {
                        indexed_lookup.assign(lut.str_val.begin(), lut.str_val.end());
                    } else if (lut.is_stream()) {
                        indexed_lookup = doc.decode_stream(lut);
                    }
                }
            } else if (cs_name == "Separation") {
                // Separation: treat as grayscale for extraction
                components = 1;
            } else if (cs_name == "DeviceN") {
                // DeviceN: use N parameter if available
                if (cs_obj.is_arr() && cs_obj.arr.size() >= 2) {
                    auto names_arr = doc.resolve(cs_obj.arr[1]);
                    if (names_arr.is_arr())
                        components = static_cast<int>(names_arr.arr.size());
                }
            }

            size_t expected = static_cast<size_t>(w) * h * components * bpc / 8;
            if (decoded.size() < expected && decoded.size() > 0) {
                // Try to infer components
                size_t total = static_cast<size_t>(w) * h;
                if (total > 0 && decoded.size() % total == 0)
                    components = static_cast<int>(decoded.size() / total);
            }

            // Apply Indexed palette expansion
            if (is_indexed && !indexed_lookup.empty() && components == 1) {
                size_t pixel_count = static_cast<size_t>(w) * h;
                std::vector<uint8_t> expanded(pixel_count * indexed_base_comps);
                size_t lut_stride = static_cast<size_t>(indexed_base_comps);
                for (size_t px = 0; px < pixel_count && px < decoded.size(); px++) {
                    int idx = decoded[px];
                    if (idx > indexed_hival) idx = indexed_hival;
                    size_t lut_off = static_cast<size_t>(idx) * lut_stride;
                    for (int c = 0; c < indexed_base_comps; c++) {
                        expanded[px * lut_stride + c] =
                            (lut_off + c < indexed_lookup.size()) ? indexed_lookup[lut_off + c] : 0;
                    }
                }
                decoded = std::move(expanded);
                components = indexed_base_comps;
            }

            img.format = "raw";
            img.components = components;
            img.pixels = std::move(decoded);
        }

        if (!img.data.empty() || !img.pixels.empty()) {
            // Encode raw pixels to PNG for in-memory delivery
            if (img.format == "raw" && img.data.empty() && !img.pixels.empty()) {
                auto png = pixels_to_png(img.pixels.data(), img.width, img.height, img.components);
                img.data.assign(png.begin(), png.end());
                img.format = "png";
                img.pixels.clear();
                img.pixels.shrink_to_fit();
            }

            if (!output_dir.empty()) {
                std::string ext, path;
                if (img.format == "jpeg") ext = ".jpg";
                else if (img.format == "jp2") ext = ".jp2";
                else ext = ".png";
                path = output_dir + "/" + img.name + ext;

                std::ofstream ofs(path, std::ios::binary);
                if (ofs) {
                    ofs.write(img.data.data(), img.data.size());
                    img.saved_path = path;
                }
                if (!img.saved_path.empty()) {
                    img.data.clear();
                    img.data.shrink_to_fit();
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
    std::vector<uint8_t> pixels; // RGB

    Canvas(int w, int h) : width(w), height(h), pixels(static_cast<size_t>(w) * h * 3, 255) {}

    void blend_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (static_cast<unsigned>(x) >= static_cast<unsigned>(width) ||
            static_cast<unsigned>(y) >= static_cast<unsigned>(height)) return;
        uint8_t* p = pixels.data() + (static_cast<size_t>(y) * width + x) * 3;
        if (a >= 255) {
            p[0] = r; p[1] = g; p[2] = b;
        } else if (a > 0) {
            // Alpha blend onto opaque white background (no dst alpha needed)
            unsigned inv = 255 - a;
            p[0] = static_cast<uint8_t>((r * a + p[0] * inv + 127) >> 8);
            p[1] = static_cast<uint8_t>((g * a + p[1] * inv + 127) >> 8);
            p[2] = static_cast<uint8_t>((b * a + p[2] * inv + 127) >> 8);
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

            bool downscale = (dw < sw || dh < sh);
            for (int y = 0; y < dh; y++) {
                for (int x = 0; x < dw; x++) {
                    uint8_t r, g, b;
                    if (downscale) {
                        // Area sampling for downscale
                        int sy0 = y * sh / dh, sy1 = (y + 1) * sh / dh;
                        int sx0 = x * sw / dw, sx1 = (x + 1) * sw / dw;
                        if (sy1 <= sy0) sy1 = sy0 + 1;
                        if (sx1 <= sx0) sx1 = sx0 + 1;
                        if (sy1 > sh) sy1 = sh;
                        if (sx1 > sw) sx1 = sw;
                        int sr = 0, sg = 0, sb = 0, cnt = 0;
                        for (int ry = sy0; ry < sy1; ry++)
                            for (int rx = sx0; rx < sx1; rx++) {
                                const uint8_t* sp = src + (ry * sw + rx) * scomp;
                                if (scomp >= 3) { sr += sp[0]; sg += sp[1]; sb += sp[2]; }
                                else { sr += sp[0]; sg += sp[0]; sb += sp[0]; }
                                cnt++;
                            }
                        r = static_cast<uint8_t>(sr / cnt);
                        g = static_cast<uint8_t>(sg / cnt);
                        b = static_cast<uint8_t>(sb / cnt);
                    } else {
                        // Nearest-neighbor for upscale
                        int sy = y * sh / dh; if (sy >= sh) sy = sh - 1;
                        int sx = x * sw / dw; if (sx >= sw) sx = sw - 1;
                        const uint8_t* sp = src + (sy * sw + sx) * scomp;
                        if (scomp >= 3) { r = sp[0]; g = sp[1]; b = sp[2]; }
                        else { r = g = b = sp[0]; }
                    }
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
        return pixels_to_png(pixels.data(), width, height, 3, level);
    }
};

// ── Page Rendering ───────────────────────────────────────

ImageData render_page_composite(PdfDoc& doc, const PdfObj& page_obj,
                                 const ContentParseResult& parse_result,
                                 int page_num, double page_w, double page_h,
                                 const std::string& output_dir) {
    constexpr double kDPI = 150.0;
    constexpr double kBase = 72.0;
    double scale = kDPI / kBase;
    int rw = static_cast<int>(page_w * scale);
    int rh = static_cast<int>(page_h * scale);
    if (rw <= 0 || rh <= 0) return {};

    Canvas canvas(rw, rh);

    // ── Rasterize vector paths (8× vertical AA + analytic horizontal coverage) ──
    constexpr int AA_V = 8;

    struct ScanEdge { double x_at_ymin; double inv_slope; int ymin, ymax; };
    std::vector<ScanEdge> edge_buf;
    std::vector<double> xs_buf;
    std::vector<int> cov_buf;

    auto rasterize_edges = [&](std::vector<ScanEdge>& edges, int ymin, int ymax,
                               uint8_t cr, uint8_t cg, uint8_t cb) {
        if (edges.empty()) return;
        ymin = std::max(0, ymin);
        ymax = std::min(rh * AA_V, ymax);
        if (ymin >= ymax) return;

        // Find x-bounds
        double xmin_d = 1e9, xmax_d = -1e9;
        for (auto& e : edges) {
            double x0 = e.x_at_ymin;
            double x1 = x0 + (e.ymax - e.ymin) * e.inv_slope;
            if (std::min(x0, x1) < xmin_d) xmin_d = std::min(x0, x1);
            if (std::max(x0, x1) > xmax_d) xmax_d = std::max(x0, x1);
        }
        int xmin = std::max(0, static_cast<int>(xmin_d) - 1);
        int xmax = std::min(rw, static_cast<int>(xmax_d) + 2);
        int xspan = xmax - xmin;
        if (xspan <= 0) return;

        cov_buf.assign(xspan + 1, 0);

        std::sort(edges.begin(), edges.end(),
                  [](const ScanEdge& a, const ScanEdge& b) { return a.ymin < b.ymin; });

        size_t next_edge = 0;
        struct ActiveEdge { double x; double inv_slope; int ymax; };
        std::vector<ActiveEdge> active;
        int prev_row = ymin / AA_V;

        for (int suby = ymin; suby < ymax; suby++) {
            int cur_row = suby / AA_V;
            if (cur_row != prev_row) {
                // Flush row
                for (int x = 0; x < xspan; x++) {
                    if (cov_buf[x] > 0) {
                        int alpha = cov_buf[x] / AA_V;
                        if (alpha > 255) alpha = 255;
                        canvas.blend_pixel(x + xmin, prev_row, cr, cg, cb, static_cast<uint8_t>(alpha));
                        cov_buf[x] = 0;
                    }
                }
                prev_row = cur_row;
            }

            // Add newly active edges
            while (next_edge < edges.size() && edges[next_edge].ymin <= suby) {
                auto& e = edges[next_edge];
                active.push_back({e.x_at_ymin + (suby - e.ymin) * e.inv_slope, e.inv_slope, e.ymax});
                next_edge++;
            }

            // Collect x-intersections, remove expired
            xs_buf.clear();
            size_t write = 0;
            for (size_t i = 0; i < active.size(); i++) {
                if (suby < active[i].ymax) {
                    double xval = active[i].x;
                    size_t pos = xs_buf.size();
                    xs_buf.push_back(xval);
                    while (pos > 0 && xs_buf[pos - 1] > xval) {
                        xs_buf[pos] = xs_buf[pos - 1]; pos--;
                    }
                    xs_buf[pos] = xval;
                    active[i].x += active[i].inv_slope;
                    active[write++] = active[i];
                }
            }
            active.resize(write);

            // Even-odd fill
            for (size_t i = 0; i + 1 < xs_buf.size(); i += 2) {
                double fx0 = xs_buf[i], fx1 = xs_buf[i + 1];
                int ix0 = std::max(xmin, static_cast<int>(fx0));
                int ix1 = std::min(xmax - 1, static_cast<int>(fx1));
                if (ix0 > ix1) continue;
                if (ix0 == ix1) {
                    cov_buf[ix0 - xmin] += static_cast<int>((fx1 - fx0) * 256 + 0.5);
                } else {
                    cov_buf[ix0 - xmin] += static_cast<int>((ix0 + 1 - fx0) * 256 + 0.5);
                    for (int x = ix0 + 1; x < ix1; x++) cov_buf[x - xmin] += 256;
                    cov_buf[ix1 - xmin] += static_cast<int>((fx1 - ix1) * 256 + 0.5);
                }
            }
        }
        // Flush last row
        for (int x = 0; x < xspan; x++) {
            if (cov_buf[x] > 0) {
                int alpha = cov_buf[x] / AA_V;
                if (alpha > 255) alpha = 255;
                canvas.blend_pixel(x + xmin, prev_row, cr, cg, cb, static_cast<uint8_t>(alpha));
            }
        }
    };

    // Bezier flattening (non-recursive with explicit stack)
    struct BezierWork { double x0,y0,cx1,cy1,cx2,cy2,x3,y3; int depth; };
    std::vector<BezierWork> bez_stack;
    auto flatten_bezier = [&](double x0, double y0, double cx1, double cy1,
                              double cx2, double cy2, double x3, double y3,
                              std::vector<std::pair<double,double>>& pts, double tol) {
        bez_stack.clear();
        bez_stack.push_back({x0,y0,cx1,cy1,cx2,cy2,x3,y3,0});
        while (!bez_stack.empty()) {
            auto w = bez_stack.back(); bez_stack.pop_back();
            if (w.depth > 10) { pts.push_back({w.x3, w.y3}); continue; }
            double dmax = std::max({std::abs(w.cx1-w.x0), std::abs(w.cy1-w.y0),
                                    std::abs(w.cx2-w.x3), std::abs(w.cy2-w.y3)});
            if (dmax < tol) { pts.push_back({w.x3, w.y3}); continue; }
            double m01x=(w.x0+w.cx1)/2, m01y=(w.y0+w.cy1)/2;
            double m12x=(w.cx1+w.cx2)/2, m12y=(w.cy1+w.cy2)/2;
            double m23x=(w.cx2+w.x3)/2, m23y=(w.cy2+w.y3)/2;
            double m012x=(m01x+m12x)/2, m012y=(m01y+m12y)/2;
            double m123x=(m12x+m23x)/2, m123y=(m12y+m23y)/2;
            double mx=(m012x+m123x)/2, my=(m012y+m123y)/2;
            // Push right half first (processed second), left half last (processed first)
            bez_stack.push_back({mx,my,m123x,m123y,m23x,m23y,w.x3,w.y3,w.depth+1});
            bez_stack.push_back({w.x0,w.y0,m01x,m01y,m012x,m012y,mx,my,w.depth+1});
        }
    };

    // Reusable buffers for path flattening
    std::vector<std::vector<std::pair<double,double>>> subpaths;
    std::vector<std::pair<double,double>> cur_sub;

    for (auto& rp : parse_result.paths) {
        if (rp.points.empty()) continue;

        // Flatten path to line segments
        subpaths.clear();
        cur_sub.clear();
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
                    flatten_bezier(px, py, pt.cx1, pt.cy1, pt.cx2, pt.cy2, pt.x, pt.y, cur_sub, 0.25);
                    px = pt.x; py = pt.y; break;
                case PathPoint::CLOSE:
                    if (!cur_sub.empty()) { cur_sub.push_back(cur_sub[0]); px = cur_sub[0].first; py = cur_sub[0].second; }
                    break;
            }
        }
        if (!cur_sub.empty()) subpaths.push_back(std::move(cur_sub));

        // Fill
        if (rp.do_fill) {
            edge_buf.clear();
            int ymin = rh * AA_V, ymax = 0;
            for (auto& sp : subpaths) {
                for (size_t i = 0; i + 1 < sp.size(); i++) {
                    double sx0 = sp[i].first * scale;
                    double sy0 = (page_h - sp[i].second) * scale;
                    double sx1 = sp[i+1].first * scale;
                    double sy1 = (page_h - sp[i+1].second) * scale;
                    int iy0 = static_cast<int>(std::round(sy0 * AA_V));
                    int iy1 = static_cast<int>(std::round(sy1 * AA_V));
                    if (iy0 == iy1) continue;
                    if (iy0 > iy1) { std::swap(sx0, sx1); std::swap(sy0, sy1); std::swap(iy0, iy1); }
                    double inv_slope = (sx1 - sx0) / (sy1 - sy0) / AA_V;
                    double x_start = sx0 + (iy0 / (double)AA_V - sy0) * (sx1 - sx0) / (sy1 - sy0);
                    edge_buf.push_back({x_start, inv_slope, iy0, iy1});
                    if (iy0 < ymin) ymin = iy0;
                    if (iy1 > ymax) ymax = iy1;
                }
            }
            uint8_t fr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_r * 255)));
            uint8_t fg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_g * 255)));
            uint8_t fb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_b * 255)));
            rasterize_edges(edge_buf, ymin, ymax, fr, fg, fb);
        }

        // Stroke
        if (rp.do_stroke) {
            double lw = rp.line_width * scale;
            if (lw < 1.0) lw = 1.0;
            double half = lw / 2.0;
            edge_buf.clear();
            int ymin = rh * AA_V, ymax = 0;
            for (auto& sp : subpaths) {
                for (size_t i = 0; i + 1 < sp.size(); i++) {
                    double sx0 = sp[i].first * scale;
                    double sy0 = (page_h - sp[i].second) * scale;
                    double sx1 = sp[i+1].first * scale;
                    double sy1 = (page_h - sp[i+1].second) * scale;
                    double dx = sx1 - sx0, dy = sy1 - sy0;
                    double len = std::sqrt(dx*dx + dy*dy);
                    if (len < 0.01) continue;
                    double nx = -dy / len * half;
                    double ny = dx / len * half;
                    double qx[4] = {sx0+nx, sx1+nx, sx1-nx, sx0-nx};
                    double qy[4] = {sy0+ny, sy1+ny, sy1-ny, sy0-ny};
                    for (int e = 0; e < 4; e++) {
                        int e2 = (e + 1) % 4;
                        double ey0 = qy[e], ey1 = qy[e2];
                        double ex0 = qx[e], ex1 = qx[e2];
                        int iy0 = static_cast<int>(std::round(ey0 * AA_V));
                        int iy1 = static_cast<int>(std::round(ey1 * AA_V));
                        if (iy0 == iy1) continue;
                        if (iy0 > iy1) { std::swap(iy0, iy1); std::swap(ex0, ex1); std::swap(ey0, ey1); }
                        double inv_s = (ex1 - ex0) / (ey1 - ey0) / AA_V;
                        double x_s = ex0 + (iy0 / (double)AA_V - ey0) * (ex1 - ex0) / (ey1 - ey0);
                        edge_buf.push_back({x_s, inv_s, iy0, iy1});
                        if (iy0 < ymin) ymin = iy0;
                        if (iy1 > ymax) ymax = iy1;
                    }
                }
            }
            uint8_t sr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_r * 255)));
            uint8_t sg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_g * 255)));
            uint8_t sb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_b * 255)));
            rasterize_edges(edge_buf, ymin, ymax, sr, sg, sb);
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
                    } else if (cs_name == "Indexed" || cs_name == "I") {
                        // Indexed color space: expand palette
                        components = 1;
                        if (cs_obj.is_arr() && cs_obj.arr.size() >= 4) {
                            auto base_cs = doc.resolve(cs_obj.arr[1]);
                            int base_comps = 3;
                            if (base_cs.is_name()) {
                                if (base_cs.str_val == "DeviceGray" || base_cs.str_val == "CalGray") base_comps = 1;
                                else if (base_cs.str_val == "DeviceCMYK") base_comps = 4;
                            }
                            int hival = doc.resolve(cs_obj.arr[2]).as_int();
                            auto lut_obj = doc.resolve(cs_obj.arr[3]);
                            std::vector<uint8_t> lut;
                            if (lut_obj.is_str()) lut.assign(lut_obj.str_val.begin(), lut_obj.str_val.end());
                            else if (lut_obj.is_stream()) lut = doc.decode_stream(lut_obj);

                            if (!lut.empty()) {
                                size_t px_count = static_cast<size_t>(w) * h;
                                std::vector<uint8_t> expanded(px_count * base_comps);
                                for (size_t pi = 0; pi < px_count && pi < decoded.size(); pi++) {
                                    int idx = decoded[pi];
                                    if (idx > hival) idx = hival;
                                    size_t lo = static_cast<size_t>(idx) * base_comps;
                                    for (int c = 0; c < base_comps; c++)
                                        expanded[pi * base_comps + c] = (lo + c < lut.size()) ? lut[lo + c] : 0;
                                }
                                decoded = std::move(expanded);
                                components = base_comps;
                            }
                        }
                    } else if (cs_name == "Separation") {
                        components = 1;
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
            // Area sampling for ImageMask: compute coverage ratio in source region
            for (int y = 0; y < dh && dy + y >= 0 && dy + y < canvas.height; y++) {
                int sy0 = y * h / dh;
                int sy1 = (y + 1) * h / dh;
                if (sy1 <= sy0) sy1 = sy0 + 1;
                if (sy1 > h) sy1 = h;
                for (int x = 0; x < dw && dx + x < canvas.width; x++) {
                    if (dx + x < 0) continue;
                    int sx0 = x * w / dw;
                    int sx1 = (x + 1) * w / dw;
                    if (sx1 <= sx0) sx1 = sx0 + 1;
                    if (sx1 > w) sx1 = w;
                    // Count set pixels in source region
                    int total = (sy1 - sy0) * (sx1 - sx0);
                    if (total <= 0) continue;
                    int set = 0;
                    for (int ry = sy0; ry < sy1; ry++)
                        for (int rx = sx0; rx < sx1; rx++)
                            if (pixels[ry * w + rx] > 128) set++;
                    if (set > 0) {
                        uint8_t a = static_cast<uint8_t>(set * 255 / total);
                        canvas.blend_pixel(dx + x, dy + y, fr, fg, fb, a);
                    }
                }
            }
        } else {
            canvas.blit_image(pixels.data(), w, h, components, ip.ctm, page_h, scale);
        }
    }

    ImageData img;
    img.page_number = page_num;
    img.name = "page" + std::to_string(page_num + 1) + "_img0";
    img.format = "raw";
    img.width = rw;
    img.height = rh;
    img.components = 3;

    // Canvas is already RGB — encode to PNG for in-memory delivery
    auto png = pixels_to_png(canvas.pixels.data(), rw, rh, 3, Z_BEST_SPEED);
    img.format = "png";
    img.data.assign(png.begin(), png.end());

    if (!output_dir.empty()) {
        std::string path = output_dir + "/" + img.name + ".png";
        std::ofstream f(path, std::ios::binary);
        if (f) {
            f.write(img.data.data(), static_cast<std::streamsize>(img.data.size()));
            img.saved_path = path;
        }
        if (!img.saved_path.empty()) {
            img.data.clear();
            img.data.shrink_to_fit();
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

// ── Annotation Extraction ────────────────────────────────

struct AnnotEntry {
    std::string text;     // annotation body text
    std::string uri;      // for Link annotations
    std::string subtype;  // Text, Link, FreeText, etc.
    double y = 0;         // vertical position on page
};

std::vector<AnnotEntry> extract_annotations(PdfDoc& doc, const PdfObj& page_obj, double page_h) {
    std::vector<AnnotEntry> result;

    auto annots_ref = page_obj.get("Annots");
    if (annots_ref.is_none()) return result;

    auto annots = doc.resolve(annots_ref);
    if (!annots.is_arr()) return result;

    for (auto& aref : annots.arr) {
        auto annot = doc.resolve(aref);
        if (!annot.is_dict()) continue;

        AnnotEntry entry;

        // Get subtype
        auto& subtype = annot.get("Subtype");
        if (subtype.is_name()) entry.subtype = subtype.str_val;

        // Get position from Rect
        auto& rect = annot.get("Rect");
        if (rect.is_arr() && rect.arr.size() >= 4)
            entry.y = rect.arr[3].as_num(); // top y

        // Extract text content (Contents key)
        auto& contents = annot.get("Contents");
        if (contents.is_str() && !contents.str_val.empty()) {
            auto& s = contents.str_val;
            // Detect UTF-16BE BOM
            if (s.size() >= 2 &&
                static_cast<uint8_t>(s[0]) == 0xFE &&
                static_cast<uint8_t>(s[1]) == 0xFF) {
                for (size_t i = 2; i + 1 < s.size(); i += 2) {
                    uint32_t cp = (static_cast<uint8_t>(s[i]) << 8) |
                                   static_cast<uint8_t>(s[i + 1]);
                    util::append_utf8(entry.text, cp);
                }
            } else {
                for (unsigned char c : s)
                    util::append_utf8(entry.text, static_cast<uint32_t>(c));
            }
        }

        // Extract URI for Link annotations
        if (entry.subtype == "Link") {
            auto action = doc.resolve(annot.get("A"));
            if (action.is_dict()) {
                auto& act_s = action.get("S");
                if (act_s.is_name() && act_s.str_val == "URI") {
                    auto& uri = action.get("URI");
                    if (uri.is_str()) entry.uri = uri.str_val;
                }
            }
        }

        // Only include annotations with actual content
        if (!entry.text.empty() || !entry.uri.empty())
            result.push_back(std::move(entry));
    }

    return result;
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

    // Skip merging when lines have been column-reordered
    for (auto& l : lines)
        if (l.is_column_split) return lines;

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

        // Don't merge column-split lines
        bool has_col_split = false;
        for (auto gi : group)
            if (lines[gi].is_column_split) has_col_split = true;

        if (group.size() == 1 || has_col_split) {
            for (auto gi : group)
                merged.push_back(lines[gi]);
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

// Standalone page-number footer lines: "- 3 -", "- ⅰ -", "- iv -"
static bool is_page_number_footer(const std::string& text) {
    std::string s;
    for (char c : text)
        if (c != ' ' && c != '\t') s += c;
    if (s.size() < 3 || s.front() != '-' || s.back() != '-') return false;
    std::string mid = s.substr(1, s.size() - 2);
    if (mid.empty() || mid.size() > 12) return false;

    bool all_digits = true;
    for (char c : mid)
        if (!std::isdigit(static_cast<unsigned char>(c))) { all_digits = false; break; }
    if (all_digits) return true;

    bool all_roman = true;
    for (char c : mid) {
        char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::string("ivxlcdm").find(lc) == std::string::npos) { all_roman = false; break; }
    }
    if (all_roman) return true;

    // Unicode roman numerals U+2160–217F (UTF-8: E2 85 A0..BF)
    if (mid.size() % 3 == 0) {
        bool all_uroman = true;
        for (size_t i = 0; i + 2 < mid.size() + 1; i += 3) {
            if (static_cast<unsigned char>(mid[i]) != 0xE2 ||
                static_cast<unsigned char>(mid[i + 1]) != 0x85 ||
                static_cast<unsigned char>(mid[i + 2]) < 0xA0) { all_uroman = false; break; }
        }
        if (all_uroman) return true;
    }
    return false;
}

// Depth of a leading section number: "2.1 ..." → 2, "4.2.1 ..." → 3, else 0
static int section_number_depth(const std::string& text) {
    size_t i = 0;
    int depth = 0;
    while (i < text.size()) {
        size_t start = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) i++;
        if (i == start) return 0;
        depth++;
        if (i < text.size() && text[i] == '.') { i++; continue; }
        break;
    }
    if (i >= text.size() || (text[i] != ' ' && text[i] != '\t')) return 0;
    return depth;
}

std::string page_to_markdown(const std::vector<TextLine>& raw_lines,
                              const FontStats& stats,
                              const std::vector<ImageData>& images,
                              const std::vector<double>& image_y_pos,
                              const std::vector<double>& image_x_pos,
                              const std::vector<TableData>& tables,
                              const std::vector<AnnotEntry>& annots = {},
                              double col_boundary = 0,
                              const std::string& img_ref_prefix = "") {
    auto lines = merge_colinear_lines(raw_lines);

    // Detect if page has column-split lines (for image placement)
    bool has_columns = false;
    for (auto& l : lines)
        if (l.is_column_split) { has_columns = true; break; }

    // Bottom-most text line Y (page-number footers live there)
    double bottom_y = 1e9;
    for (auto& l : lines)
        if (l.y_center < bottom_y) bottom_y = l.y_center;

    std::string md;
    md.reserve(lines.size() * 80);

    // Build sorted insert lists for tables and images by Y position (top-first in PDF coords)
    struct InlineInsert {
        double y_pos;
        double x_pos;
        size_t idx;
        bool is_image; // false = table, true = image
    };
    std::vector<InlineInsert> inserts;
    for (size_t ti = 0; ti < tables.size(); ti++) {
        double tx = (tables[ti].x0 + tables[ti].x1) / 2.0;
        inserts.push_back({std::max(tables[ti].y0, tables[ti].y1), tx, ti, false});
    }
    for (size_t ii = 0; ii < images.size(); ii++) {
        double y = (ii < image_y_pos.size()) ? image_y_pos[ii] : 0.0;
        double x = (ii < image_x_pos.size()) ? image_x_pos[ii] : 0.0;
        inserts.push_back({y, x, ii, true});
    }
    std::sort(inserts.begin(), inserts.end(),
              [](const InlineInsert& a, const InlineInsert& b) { return a.y_pos > b.y_pos; });

    size_t next_insert = 0;

    // For column-split pages, defer inserts whose X doesn't match current text column
    std::vector<size_t> deferred_inserts;

    auto emit_insert = [&](const InlineInsert& ins) {
        if (ins.is_image) {
            auto& img = images[ins.idx];
            std::string ref = img.name + "." + img.format;
            if (!img.saved_path.empty()) {
                auto slash = img.saved_path.find_last_of('/');
                ref = (slash != std::string::npos)
                    ? img.saved_path.substr(slash + 1)
                    : img.saved_path;
            }
            md += "\n![" + img.name + "](" + img_ref_prefix + ref + ")\n";
        } else {
            auto& tbl = tables[ins.idx];
            if (!tbl.title.empty())
                md += "\n" + tbl.title + "\n";
            md += "\n";
            md += format_table(tbl);
            md += "\n";
        }
    };

    auto flush_inserts = [&](double y_threshold, bool is_left_col = false, bool is_right_col = false) {
        while (next_insert < inserts.size() &&
               inserts[next_insert].y_pos >= y_threshold) {
            auto& ins = inserts[next_insert];
            // On column-split pages, defer inserts from the other column
            if (col_boundary > 0 && ins.x_pos > 0) {
                bool ins_is_left = ins.x_pos < col_boundary;
                if ((is_left_col && !ins_is_left) || (is_right_col && ins_is_left)) {
                    deferred_inserts.push_back(next_insert);
                    next_insert++;
                    continue;
                }
            }
            emit_insert(ins);
            next_insert++;
        }
    };

    for (size_t i = 0; i < lines.size(); i++) {
        const auto& l = lines[i];

        bool is_left = l.is_column_split && (l.x_left + l.x_right) / 2.0 < col_boundary;
        bool is_right = l.is_column_split && !is_left;
        flush_inserts(l.y_center, is_left, is_right);

        // Emit deferred inserts (from other column) when their Y matches current line
        for (auto it = deferred_inserts.begin(); it != deferred_inserts.end(); ) {
            auto& ins = inserts[*it];
            bool ins_is_left = ins.x_pos < col_boundary;
            if (ins.y_pos >= l.y_center &&
                ((is_left && ins_is_left) || (is_right && !ins_is_left) || !l.is_column_split)) {
                emit_insert(ins);
                it = deferred_inserts.erase(it);
            } else {
                ++it;
            }
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

        // Drop standalone page-number footers ("- 3 -") at the page bottom
        if ((i + 2 >= lines.size() || l.y_center <= bottom_y + 5.0) &&
            is_page_number_footer(l.text)) continue;

        int hlevel = stats.heading_level(l.font_size, l.is_bold);

        if (hlevel >= 3 && !l.is_bold && l.text.size() > 60)
            hlevel = 0;

        // Bold numbered section headings at body size ("2.1 ...", "4.2.1 ...")
        if (hlevel == 0 && l.is_bold && l.text.size() < 120) {
            int depth = section_number_depth(l.text);
            if (depth >= 2) hlevel = std::min(depth + 2, 6);
        }

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

    // Flush remaining tables and images
    flush_inserts(-1e9);

    // Emit deferred inserts (images/tables from other column)
    for (auto di : deferred_inserts)
        emit_insert(inserts[di]);

    // Append annotations (links, text notes) at end of page
    if (!annots.empty()) {
        bool has_links = false, has_notes = false;
        for (auto& a : annots) {
            if (!a.uri.empty()) has_links = true;
            if (!a.text.empty() && a.subtype != "Link") has_notes = true;
        }
        if (has_links) {
            md += "\n**Links:**\n";
            for (auto& a : annots) {
                if (a.uri.empty()) continue;
                if (!a.text.empty())
                    md += "- [" + a.text + "](" + a.uri + ")\n";
                else
                    md += "- <" + a.uri + ">\n";
            }
        }
        if (has_notes) {
            md += "\n**Notes:**\n";
            for (auto& a : annots) {
                if (a.text.empty() || a.subtype == "Link") continue;
                md += "> " + a.text + "\n\n";
            }
        }
    }

    return md;
}

// ── Core Extraction Logic ────────────────────────────────

struct ExtractResult {
    std::vector<std::vector<TextLine>> all_lines;
    std::vector<std::vector<ImageData>> all_images;
    std::vector<std::vector<double>> all_image_y;  // per-page image Y positions (PDF coords, top=large)
    std::vector<std::vector<double>> all_image_x;  // per-page image X positions
    std::vector<double> col_boundaries;  // per-page column boundary (0 if single-column)
    std::vector<std::vector<TableData>> all_tables;
    std::vector<std::vector<AnnotEntry>> all_annots;
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

// Extract from an in-memory buffer; pdf_path is used for error messages only.
static ExtractResult extract_pdf_buffer(const uint8_t* data, size_t size,
                                        const std::string& pdf_path,
                                        const ConvertOptions& opts) {
    ExtractResult result;

    // Check PDF header
    if (size < 5 || std::memcmp(data, "%PDF-", 5) != 0)
        throw std::runtime_error("Not a valid PDF file: " + pdf_path);

    PdfDoc doc(data, size);
    if (!doc.parse())
        throw std::runtime_error("Failed to parse PDF structure: " + pdf_path);

    // Handle encryption (supports Standard Security Handler with empty password)
    if (doc.trailer.has("Encrypt")) {
        if (!doc.init_encryption(""))
            throw std::runtime_error("Encrypted PDF requires a password: " + pdf_path);
    }

    // Get page tree
    auto root = doc.resolve(doc.trailer.get("Root"));
    auto pages = doc.resolve(root.get("Pages"));

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
    if (pages.is_dict()) collect_pages(pages);

    // Recovery: page tree missing or broken (truncated PDFs) — scan every
    // known object for /Type /Page and use them in object-number order.
    if (page_objs.empty()) {
        for (auto& [num, e] : doc.xref) {
            PdfObj obj = doc.get_obj(num);
            if (!obj.is_dict()) continue;
            auto& type = obj.get("Type");
            if (type.is_name() && type.str_val == "Page") {
                page_objs.push_back(obj);
                page_obj_nums.push_back(num);
            }
        }
    }
    if (page_objs.empty())
        throw std::runtime_error("Invalid PDF page tree: " + pdf_path);

    int tp = static_cast<int>(page_objs.size());
    result.total_pages = tp;
    result.all_lines.resize(tp);
    result.all_images.resize(tp);
    result.all_image_y.resize(tp);
    result.all_image_x.resize(tp);
    result.col_boundaries.resize(tp, 0);
    result.all_tables.resize(tp);
    result.all_annots.resize(tp);
    result.page_widths.resize(tp, 0);
    result.page_heights.resize(tp, 0);

    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < tp; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    // Always extract images for markdown references; only save to disk if dir is set
    std::string image_dir;
    ConvertOptions img_opts = opts;
    img_opts.images = true;
    if (!opts.image_dir.empty()) {
        image_dir = opts.image_dir;
        util::ensure_dir(image_dir);
    }

    std::unordered_map<int, PdfFont> font_cache;

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

        // Quick check: skip pages with no fonts and no extractable images
        bool has_fonts = false;
        {
            auto& font_res = resources.get("Font");
            if (!font_res.is_none()) {
                auto fd = doc.resolve(font_res);
                has_fonts = fd.is_dict() && !fd.dict.empty();
            }
        }
        if (!has_fonts && !img_opts.images) continue;

        // Parse content stream
        auto content_data = get_page_content(doc, page_obj);
        if (content_data.empty()) continue;

        // Extract text lines
        bool plaintext = (opts.format == OutputFormat::PLAINTEXT);
        bool need_tables = opts.tables && !plaintext;
        bool need_graphics = need_tables || img_opts.images;

        auto parse_result = parse_content_stream(doc, content_data, resources, page_h,
                                                  &font_cache, !need_graphics);

        result.all_lines[p] = chars_to_lines(parse_result.chars, &result.col_boundaries[p]);

        // Extract annotations (text notes, links)
        result.all_annots[p] = extract_annotations(doc, page_obj, page_h);

        if (need_tables) {
            PageCharCache cache;
            cache.build(parse_result.chars);

            result.all_tables[p] = detect_tables(parse_result.segments, cache,
                page_w, page_h);
            auto text_tables = detect_text_tables(cache, result.all_tables[p],
                page_w, page_h);
            for (auto& tt : text_tables)
                result.all_tables[p].push_back(std::move(tt));
        }

        // Image extraction
        if (img_opts.images) {
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
                if (!rendered.data.empty() || !rendered.pixels.empty() || !rendered.saved_path.empty()) {
                    result.all_images[p].push_back(std::move(rendered));
                    result.all_image_y[p].push_back(page_h);
                    result.all_image_x[p].push_back(0);
                }
            } else {
                auto extracted = extract_page_images(doc, page_obj, parse_result, p, image_dir, opts.min_image_size);
                for (auto& ei : extracted) {
                    // ctm[5] is the Y translation in PDF coordinates (origin bottom-left)
                    // ctm[3] is vertical scale; y_top = ctm[5] + abs(ctm[3])
                    double y_top = ei.ctm[5] + std::abs(ei.ctm[3]);
                    result.all_image_y[p].push_back(y_top);
                    result.all_image_x[p].push_back(ei.ctm[4]); // X position
                    result.all_images[p].push_back(std::move(ei.img));
                }
            }

            // Fallback: render page for scanned/vector-only pages
            if (result.all_images[p].empty() && result.all_lines[p].empty()) {
                if (!parse_result.images.empty() || !parse_result.segments.empty()) {
                    auto rendered = render_page_composite(doc, page_obj, parse_result,
                                                          p, page_w, page_h, image_dir);
                    if (!rendered.data.empty() || !rendered.pixels.empty() || !rendered.saved_path.empty()) {
                        result.all_images[p].push_back(std::move(rendered));
                        result.all_image_y[p].push_back(page_h);
                        result.all_image_x[p].push_back(0);
                    }
                }
            }

            // Release pixel data after writing to disk
            if (!image_dir.empty()) {
                for (auto& img : result.all_images[p]) {
                    if (!img.saved_path.empty()) {
                        img.data.clear();
                        img.data.shrink_to_fit();
                        img.pixels.clear();
                        img.pixels.shrink_to_fit();
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

static ExtractResult extract_pdf(const std::string& pdf_path, const ConvertOptions& opts) {
    std::ifstream file(pdf_path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open PDF: " + pdf_path);

    std::streamsize fsize = file.tellg();
    if (fsize <= 0) throw std::runtime_error("Empty PDF file: " + pdf_path);
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> file_data(static_cast<size_t>(fsize));
    if (!file.read(reinterpret_cast<char*>(file_data.data()), fsize))
        throw std::runtime_error("Cannot read PDF: " + pdf_path);
    file.close();

    return extract_pdf_buffer(file_data.data(), file_data.size(), pdf_path, opts);
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

static std::string result_to_markdown(ExtractResult& r, const ConvertOptions& opts) {
    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < r.total_pages; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    bool plaintext = (opts.format == OutputFormat::PLAINTEXT);

    std::string full_md;
    full_md.reserve(64 * 1024);

    if (!r.bookmarks.empty()) {
        if (!plaintext) full_md += "## Table of Contents\n\n";
        full_md += format_bookmarks(r.bookmarks, plaintext);
        full_md += "\n";
    }

    for (int p : page_indices) {
        if (p < 0 || p >= r.total_pages) continue;
        if (!full_md.empty()) full_md += '\n';
        full_md += "--- Page " + std::to_string(p + 1) + " ---\n\n";
        std::string page_md = page_to_markdown(r.all_lines[p], r.stats,
                                                r.all_images[p], r.all_image_y[p], r.all_image_x[p],
                                                r.all_tables[p],
                                                p < (int)r.all_annots.size() ? r.all_annots[p] : std::vector<AnnotEntry>{},
                                                r.col_boundaries[p],
                                                opts.image_ref_prefix);
        if (plaintext)
            full_md += util::strip_markdown(page_md);
        else
            full_md += page_md;
    }
    return full_md;
}

static std::vector<PageChunk> result_to_chunks(ExtractResult& r,
                                               const ConvertOptions& opts) {
    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < r.total_pages; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    bool plaintext = (opts.format == OutputFormat::PLAINTEXT);

    std::vector<PageChunk> chunks;
    for (int p : page_indices) {
        if (p < 0 || p >= r.total_pages) continue;
        PageChunk chunk;
        chunk.page_number = p + 1;
        chunk.page_width = r.page_widths[p];
        chunk.page_height = r.page_heights[p];
        chunk.body_font_size = r.stats.body_size;
        std::string page_md = page_to_markdown(r.all_lines[p], r.stats,
                                                r.all_images[p], r.all_image_y[p], r.all_image_x[p],
                                                r.all_tables[p],
                                                p < (int)r.all_annots.size() ? r.all_annots[p] : std::vector<AnnotEntry>{},
                                                r.col_boundaries[p],
                                                opts.image_ref_prefix);
        chunk.text = plaintext ? util::strip_markdown(page_md) : page_md;

        for (auto& td : r.all_tables[p])
            chunk.tables.push_back(td.rows);

        chunk.images = std::move(r.all_images[p]);
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

std::string pdf_to_markdown(const std::string& pdf_path, ConvertOptions opts) {
    auto r = extract_pdf(pdf_path, opts);
    return result_to_markdown(r, opts);
}

std::vector<PageChunk> pdf_to_markdown_chunks(const std::string& pdf_path,
                                              ConvertOptions opts) {
    auto r = extract_pdf(pdf_path, opts);
    return result_to_chunks(r, opts);
}

std::string pdf_to_markdown_mem(const uint8_t* data, size_t size,
                                ConvertOptions opts) {
    auto r = extract_pdf_buffer(data, size, "<memory>", opts);
    return result_to_markdown(r, opts);
}

std::vector<PageChunk> pdf_to_markdown_chunks_mem(const uint8_t* data, size_t size,
                                                  ConvertOptions opts) {
    auto r = extract_pdf_buffer(data, size, "<memory>", opts);
    return result_to_chunks(r, opts);
}

} // namespace jdoc
