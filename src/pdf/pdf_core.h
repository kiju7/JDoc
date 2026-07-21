#pragma once
// pdf_core.h — internal: PDF object model, lexer, xref, crypt mapping, document, fonts.
#include "jdoc/pdf.h"
#include "pdf_crypt.h"
#include "common/string_utils.h"
#include "common/file_utils.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jdoc { namespace pdf_detail {

// Parse a PDF real number from s[0..n) — the PDF grammar is just
// [+-]? digit* ('.' digit*)?  (no exponent, no locale, no inf/nan), so we skip
// std::strtod entirely. strtod is locale-aware (a localeconv lookup per call on
// both glibc and macOS) and handles forms PDF never uses; avoiding it removes a
// hot cost in content-stream parsing. Digits are accumulated into an integer
// mantissa (capped at 15 significant digits, which stays exact in a double) and
// scaled by a single power of ten, so the result is correctly rounded — and
// thus byte-identical to strtod — for the value ranges PDFs actually contain.
// Kept inline (called per numeric token) so the TU split preserves inlining.
inline double parse_pdf_real(const char* s, size_t n) {
    static const double kPow10[] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    size_t i = 0;
    bool neg = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = (s[i] == '-'); ++i; }
    uint64_t mant = 0;
    int exp10 = 0;   // power-of-ten to apply to mant
    int sig = 0;     // significant digits accumulated (kept exact in double)
    bool dot = false;
    for (; i < n; ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (sig < 15) {
                mant = mant * 10 + static_cast<uint64_t>(c - '0');
                ++sig;
                if (dot) --exp10;      // this digit is fractional
            } else if (!dot) {
                ++exp10;               // extra integer digit: scale up instead
            }                          // extra fractional digits: negligible
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            break;                     // stop at first non-number byte
        }
    }
    double val = static_cast<double>(mant);
    if (exp10 > 0)
        val *= (exp10 <= 22) ? kPow10[exp10] : std::pow(10.0, exp10);
    else if (exp10 < 0)
        val /= (-exp10 <= 22) ? kPow10[-exp10] : std::pow(10.0, -exp10);
    return neg ? -val : val;
}

// Defined in pdf_core.cpp; declared here because PdfFont's inline methods use it.
uint32_t glyph_name_to_unicode(const std::string& name);

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

    PdfObj parse_object(int depth = 0);
};

struct XrefEntry {
    int64_t offset = 0;
    int gen = 0;
    bool in_use = false;
    // For compressed objects in object streams
    int stream_obj = -1;
    int stream_idx = -1;
};

namespace pc = pdf_crypt;

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

// Cross-translation-unit declarations.
std::vector<uint8_t> decode_flate(const uint8_t* src, size_t src_len);
PdfFont load_font(PdfDoc& doc, const PdfObj& font_ref);

}} // namespace jdoc::pdf_detail
