// tar_reader.cpp - Sequential tar reader (ustar/pax/GNU longname)
// License: MIT

#include "archive/tar_reader.h"
#include <cstring>

namespace jdoc {

static constexpr size_t BLOCK = 512;

// Parse a NUL/space-terminated octal field. GNU base-256 (high bit set on
// first byte) is used for sizes > 8 GiB.
static uint64_t parse_octal(const unsigned char* p, size_t len) {
    if (len > 0 && (p[0] & 0x80)) {
        uint64_t v = p[0] & 0x7F;
        for (size_t i = 1; i < len; i++) v = (v << 8) | p[i];
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '0')) i++;
    for (; i < len && p[i] >= '0' && p[i] <= '7'; i++)
        v = v * 8 + (p[i] - '0');
    return v;
}

bool TarReader::read_full(void* buf, size_t len) {
    auto* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < len) {
        size_t n = src_.read(p + got, len - got);
        if (n == 0) return false;
        got += n;
    }
    return true;
}

bool TarReader::discard(uint64_t len) {
    char buf[4096];
    while (len > 0) {
        size_t chunk = len < sizeof(buf) ? static_cast<size_t>(len) : sizeof(buf);
        size_t n = src_.read(buf, chunk);
        if (n == 0) return false;
        len -= n;
    }
    return true;
}

// Extract "path=" value from a pax extended header record set.
// Records are "<len> <key>=<value>\n".
static std::string pax_path(const std::string& data) {
    size_t pos = 0;
    while (pos < data.size()) {
        size_t sp = data.find(' ', pos);
        if (sp == std::string::npos) break;
        unsigned long rec_len = strtoul(data.c_str() + pos, nullptr, 10);
        if (rec_len < 2 || pos + rec_len > data.size()) break;
        std::string rec = data.substr(sp + 1, pos + rec_len - sp - 2);  // strip trailing \n
        if (rec.compare(0, 5, "path=") == 0)
            return rec.substr(5);
        pos += rec_len;
    }
    return "";
}

bool TarReader::next(Member& out) {
    // Ensure the previous member's data was consumed
    if (data_remaining_ > 0 || pad_remaining_ > 0) {
        if (!discard(data_remaining_ + pad_remaining_)) return false;
        data_remaining_ = pad_remaining_ = 0;
    }

    unsigned char hdr[BLOCK];
    for (;;) {
        if (!read_full(hdr, BLOCK)) return false;

        // End of archive: block of zeros
        bool all_zero = true;
        for (size_t i = 0; i < BLOCK; i++)
            if (hdr[i] != 0) { all_zero = false; break; }
        if (all_zero) return false;

        uint64_t size = parse_octal(&hdr[124], 12);
        char typeflag = static_cast<char>(hdr[156]);
        uint64_t padded = (size + BLOCK - 1) / BLOCK * BLOCK;

        // Metadata records that carry a long name for the NEXT entry
        if (typeflag == 'x' || typeflag == 'L') {
            if (size > 1024 * 1024) return false;  // sane bound for metadata
            std::string data(static_cast<size_t>(size), '\0');
            if (!read_full(&data[0], static_cast<size_t>(size))) return false;
            if (!discard(padded - size)) return false;
            if (typeflag == 'L') {
                // GNU longname: NUL-terminated
                pending_longname_ = data.c_str();
            } else {
                std::string p = pax_path(data);
                if (!p.empty()) pending_longname_ = p;
            }
            continue;
        }
        // Global pax header or other special record: skip
        if (typeflag == 'g') {
            if (!discard(padded)) return false;
            continue;
        }

        // Regular header
        std::string name;
        if (!pending_longname_.empty()) {
            name = pending_longname_;
            pending_longname_.clear();
        } else {
            char name_buf[101] = {};
            memcpy(name_buf, hdr, 100);
            name = name_buf;
            // ustar prefix field
            if (memcmp(&hdr[257], "ustar", 5) == 0 && hdr[345] != 0) {
                char prefix[156] = {};
                memcpy(prefix, &hdr[345], 155);
                name = std::string(prefix) + "/" + name;
            }
        }

        out.name = std::move(name);
        out.size = size;
        out.is_file = (typeflag == '0' || typeflag == '\0');
        data_remaining_ = size;
        pad_remaining_ = padded - size;
        return true;
    }
}

bool TarReader::read_data(const WriteFn& sink) {
    char buf[65536];
    while (data_remaining_ > 0) {
        size_t chunk = data_remaining_ < sizeof(buf)
                           ? static_cast<size_t>(data_remaining_) : sizeof(buf);
        size_t n = src_.read(buf, chunk);
        if (n == 0) return false;
        data_remaining_ -= n;
        if (!sink(buf, n)) return false;
    }
    if (pad_remaining_ > 0) {
        if (!discard(pad_remaining_)) return false;
        pad_remaining_ = 0;
    }
    return true;
}

bool TarReader::skip_data() {
    if (!discard(data_remaining_ + pad_remaining_)) return false;
    data_remaining_ = pad_remaining_ = 0;
    return true;
}

} // namespace jdoc
