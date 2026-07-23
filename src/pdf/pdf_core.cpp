#include "pdf_core.h"
#include "common/string_utils.h"
#include "common/file_utils.h"
#include "common/inflate.h"
#include <fstream>
#include <zlib.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jdoc { namespace pdf_detail {

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

// parse_pdf_real() lives inline in pdf_core.h (hot path, shared across TUs).

// ── PDF Object Model ─────────────────────────────────────


PdfObj PdfLexer::parse_object(int depth) {
    // Dicts and arrays nest arbitrarily in a malformed file; cap the recursion
    // so a hostile PDF cannot overflow the stack. 64 is far deeper than any
    // legitimate object nesting yet leaves ample headroom on any thread stack.
    // Consume a byte before bailing: the enclosing array/dict loops only stop
    // on a closing delimiter or end of input, so returning without advancing
    // would spin forever.
    if (depth > 64) { pos++; return {}; }

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
                auto val = parse_object(depth + 1);
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
            obj.arr.push_back(parse_object(depth + 1));
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
            return PdfObj::make_real(parse_pdf_real(tok.data(), tok.size()));
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

    // Fast path: libdeflate decodes a well-formed FlateDecode stream whole.
    auto whole = inflate_zlib(src, src_len, src_len * 3);
    if (!whole.empty()) return whole;

    // Fallback: many real PDFs have truncated/malformed streams (missing the
    // adler32 trailer, wrong /Length). zlib's streaming inflate recovers the
    // PARTIAL output that precedes the damage, where libdeflate returns nothing.
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


const uint8_t PdfCrypt::kEmptyPassword[32] = {
    0x28,0xBF,0x4E,0x5E,0x4E,0x75,0x8A,0x41,0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,
    0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A
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

uint32_t glyph_name_to_unicode(const std::string& name) {
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

    auto parse_unicode = [&](const std::string& hex) -> uint32_t {
        if (hex.size() < 4)
            return parse_hex(hex);

        const uint32_t high = parse_hex(hex.substr(0, 4));
        if (high < 0xD800 || high > 0xDFFF)
            return high;
        if (high > 0xDBFF || hex.size() < 8)
            return 0xFFFD;

        const uint32_t low = parse_hex(hex.substr(4, 4));
        if (low < 0xDC00 || low > 0xDFFF)
            return 0xFFFD;
        return 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
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
            font.to_unicode[src] = parse_unicode(dst_hex);
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
                    font.to_unicode[code] = parse_unicode(dh);
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
                uint32_t dst = parse_unicode(dst_hex);
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
        // Check encoding (may be an indirect reference)
        auto enc = doc.resolve(fobj.get("Encoding"));
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

    // ToUnicode CMap (may be an indirect reference)
    auto tu = doc.resolve(fobj.get("ToUnicode"));
    if (!tu.is_none()) parse_tounicode_cmap(doc, tu, font);

    // Simple fonts (everything but Type0 composite fonts) index glyphs with
    // single-byte codes, whatever the ToUnicode CMap's codespacerange claims.
    // Some producers ship a Type1/TrueType font with a 2-byte <0000><FFFF>
    // codespace (and HWP does the same for Type3); honoring it would make the
    // string decoder read one-byte text two bytes at a time and drop the body.
    if (!font.is_type0) font.cmap_code_bytes = 1;

    // Encoding (may be an indirect reference to an encoding dictionary)
    auto enc_obj = doc.resolve(fobj.get("Encoding"));
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


}} // namespace jdoc::pdf_detail
