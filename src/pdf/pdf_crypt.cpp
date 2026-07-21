// pdf_crypt.cpp — MD5/SHA-2, RC4 and AES for the PDF standard security handler
// License: MIT

#include "pdf_crypt.h"

#include <algorithm>
#include <cstring>

namespace jdoc { namespace pdf_crypt {
namespace {

inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

// Appends the Merkle-Damgård padding: 0x80, zeros, then the bit length in
// big-endian over `len_bytes`. Used by both SHA-2 variants.
std::vector<uint8_t> sha_pad(const uint8_t* data, size_t len,
                             size_t block, size_t len_bytes) {
    size_t total = ((len + len_bytes + 1 + block - 1) / block) * block;
    std::vector<uint8_t> msg(total, 0);
    if (len) std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (size_t i = 0; i < 8; i++)
        msg[total - 1 - i] = static_cast<uint8_t>(bits >> (i * 8));
    return msg;
}

// ── SHA-256 ───────────────────────────────────────────────────

const uint32_t kSha256K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

// ── SHA-512 / SHA-384 ─────────────────────────────────────────

const uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

// SHA-384 and SHA-512 differ only in the initial state and the digest length.
void sha512_core(const uint8_t* data, size_t len, const uint64_t iv[8],
                 uint8_t* out, size_t out_len) {
    uint64_t h[8];
    std::memcpy(h, iv, sizeof(h));
    std::vector<uint8_t> msg = sha_pad(data, len, 128, 16);

    uint64_t w[80];
    for (size_t off = 0; off < msg.size(); off += 128) {
        const uint8_t* p = msg.data() + off;
        for (int i = 0; i < 16; i++) {
            w[i] = 0;
            for (int b = 0; b < 8; b++) w[i] = (w[i] << 8) | p[i * 8 + b];
        }
        for (int i = 16; i < 80; i++) {
            uint64_t s0 = rotr64(w[i-15], 1) ^ rotr64(w[i-15], 8) ^ (w[i-15] >> 7);
            uint64_t s1 = rotr64(w[i-2], 19) ^ rotr64(w[i-2], 61) ^ (w[i-2] >> 6);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint64_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint64_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; i++) {
            uint64_t s1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
            uint64_t ch = (e & f) ^ (~e & g);
            uint64_t t1 = hh + s1 + ch + kSha512K[i] + w[i];
            uint64_t s0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
            uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint64_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (size_t i = 0; i < out_len; i++)
        out[i] = static_cast<uint8_t>(h[i / 8] >> (56 - (i % 8) * 8));
}

// ── AES tables ────────────────────────────────────────────────

const uint8_t kSBox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

const uint8_t kInvSBox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

inline uint8_t xtime(uint8_t x) {
    return static_cast<uint8_t>((x << 1) ^ ((x >> 7) * 0x1b));
}

// Multiplication in GF(2^8); compact rather than table-driven.
inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

// The PDF 1.x password padding string (ISO 32000-1, Algorithm 2).
constexpr uint8_t kPasswordPad[32] = {
    0x28,0xBF,0x4E,0x5E,0x4E,0x75,0x8A,0x41,0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,
    0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A
};

// Algorithm 2.A step (a): the password hash. Revision 5 uses a plain SHA-256;
// revision 6 hardens it with the iterated construction of Algorithm 2.B.
void password_hash(int revision, const std::string& password,
                   const uint8_t salt[8], const uint8_t* udata, size_t udata_len,
                   uint8_t out[32]) {
    std::vector<uint8_t> seed(password.begin(), password.end());
    seed.insert(seed.end(), salt, salt + 8);
    if (udata) seed.insert(seed.end(), udata, udata + udata_len);

    uint8_t k[64];
    size_t k_len = 32;
    sha256(seed.data(), seed.size(), k);
    if (revision < 6) {
        std::memcpy(out, k, 32);
        return;
    }

    // Algorithm 2.B: at least 64 rounds, then until the last byte of the
    // round's ciphertext drops to or below (round number − 32).
    std::vector<uint8_t> block;
    for (int round = 0;; round++) {
        std::vector<uint8_t> unit(password.begin(), password.end());
        unit.insert(unit.end(), k, k + k_len);
        if (udata) unit.insert(unit.end(), udata, udata + udata_len);

        block.clear();
        block.reserve(unit.size() * 64);
        for (int i = 0; i < 64; i++)
            block.insert(block.end(), unit.begin(), unit.end());

        Aes(k, 16).cbc_encrypt(k + 16, block.data(), block.size());

        unsigned sum = 0;
        for (int i = 0; i < 16; i++) sum += block[i];
        switch (sum % 3) {
            case 0: sha256(block.data(), block.size(), k); k_len = 32; break;
            case 1: sha384(block.data(), block.size(), k); k_len = 48; break;
            default: sha512(block.data(), block.size(), k); k_len = 64; break;
        }
        if (round >= 63 && block.back() <= round - 31) break;
    }
    std::memcpy(out, k, 32);
}

// Algorithm 2.A step (e): unwrap /UE or /OE with the intermediate key.
void unwrap_file_key(const uint8_t intermediate[32], const std::string& wrapped,
                     uint8_t out_key[32]) {
    const uint8_t iv[16] = {};
    std::memcpy(out_key, wrapped.data(), 32);
    Aes(intermediate, 32).cbc_decrypt(iv, out_key, 32);
}

}  // namespace

// ── MD5 ───────────────────────────────────────────────────────

void md5(const uint8_t* data, size_t len, uint8_t out[16]) {
    static const uint32_t T[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    size_t pad_len = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(pad_len, 0);
    if (len) std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    uint64_t bit_len = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; i++)
        msg[pad_len - 8 + i] = static_cast<uint8_t>(bit_len >> (i * 8));

    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (size_t off = 0; off < pad_len; off += 64) {
        const uint8_t* p = msg.data() + off;
        uint32_t M[16];
        for (int i = 0; i < 16; i++)
            M[i] = uint32_t(p[i*4]) | (uint32_t(p[i*4+1]) << 8)
                 | (uint32_t(p[i*4+2]) << 16) | (uint32_t(p[i*4+3]) << 24);
        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16)      { f = (b & c) | (~b & d); g = static_cast<uint32_t>(i); }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5*i+1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;          g = (3*i+5) % 16; }
            else             { f = c ^ (b | ~d);       g = (7*i) % 16; }
            uint32_t tmp = d; d = c; c = b;
            uint32_t x = a + f + T[i] + M[g];
            b = b + ((x << S[i]) | (x >> (32 - S[i])));
            a = tmp;
        }
        a0 += a; b0 += b; c0 += c; d0 += d;
    }
    const uint32_t h[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 16; i++)
        out[i] = static_cast<uint8_t>(h[i / 4] >> ((i % 4) * 8));
}

// ── SHA-2 ─────────────────────────────────────────────────────

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::vector<uint8_t> msg = sha_pad(data, len, 64, 8);

    uint32_t w[64];
    for (size_t off = 0; off < msg.size(); off += 64) {
        const uint8_t* p = msg.data() + off;
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t(p[i*4]) << 24) | (uint32_t(p[i*4+1]) << 16)
                 | (uint32_t(p[i*4+2]) << 8) | uint32_t(p[i*4+3]);
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
            uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 32; i++)
        out[i] = static_cast<uint8_t>(h[i / 4] >> (24 - (i % 4) * 8));
}

void sha384(const uint8_t* data, size_t len, uint8_t out[48]) {
    static const uint64_t kIv[8] = {
        0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL};
    sha512_core(data, len, kIv, out, 48);
}

void sha512(const uint8_t* data, size_t len, uint8_t out[64]) {
    static const uint64_t kIv[8] = {
        0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    sha512_core(data, len, kIv, out, 64);
}

// ── RC4 ───────────────────────────────────────────────────────

void rc4(const uint8_t* key, int key_len, uint8_t* data, size_t len) {
    if (key_len <= 0) return;
    uint8_t s[256];
    for (int i = 0; i < 256; i++) s[i] = static_cast<uint8_t>(i);
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + s[i] + key[i % key_len]) & 0xFF;
        std::swap(s[i], s[j]);
    }
    int si = 0;
    j = 0;
    for (size_t k = 0; k < len; k++) {
        si = (si + 1) & 0xFF;
        j = (j + s[si]) & 0xFF;
        std::swap(s[si], s[j]);
        data[k] ^= s[(s[si] + s[j]) & 0xFF];
    }
}

// ── AES ───────────────────────────────────────────────────────

Aes::Aes(const uint8_t* key, int key_len) {
    static const uint8_t rcon[14] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,
                                     0x1b,0x36,0x6c,0xd8,0xab,0x4d};
    if (key_len != 32) key_len = 16;
    rounds_ = (key_len == 32) ? 14 : 10;

    const int nk = key_len / 4;             // key words
    const int total = (rounds_ + 1) * 16;   // schedule bytes
    std::memcpy(rk_, key, key_len);
    for (int i = key_len, r = 0; i < total; i += 4) {
        uint8_t t[4] = {rk_[i-4], rk_[i-3], rk_[i-2], rk_[i-1]};
        const int word = i / 4;
        if (word % nk == 0) {
            uint8_t first = t[0];
            t[0] = kSBox[t[1]] ^ rcon[r++];
            t[1] = kSBox[t[2]];
            t[2] = kSBox[t[3]];
            t[3] = kSBox[first];
        } else if (nk > 6 && word % nk == 4) {
            for (int k = 0; k < 4; k++) t[k] = kSBox[t[k]];
        }
        for (int k = 0; k < 4; k++) rk_[i + k] = rk_[i - key_len + k] ^ t[k];
    }
}

void Aes::add_round_key(uint8_t s[16], int round) const {
    for (int i = 0; i < 16; i++) s[i] ^= rk_[round * 16 + i];
}

void Aes::encrypt_block(uint8_t s[16]) const {
    add_round_key(s, 0);
    for (int round = 1; round <= rounds_; round++) {
        for (int i = 0; i < 16; i++) s[i] = kSBox[s[i]];
        // ShiftRows
        uint8_t t;
        t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
        t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
        // MixColumns (omitted in the final round)
        if (round < rounds_) {
            for (int c = 0; c < 4; c++) {
                uint8_t* col = s + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
                col[1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
                col[2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
                col[3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
            }
        }
        add_round_key(s, round);
    }
}

void Aes::decrypt_block(uint8_t s[16]) const {
    add_round_key(s, rounds_);
    for (int round = rounds_ - 1; round >= 0; round--) {
        // InvShiftRows
        uint8_t t;
        t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
        t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
        // InvSubBytes
        for (int i = 0; i < 16; i++) s[i] = kInvSBox[s[i]];
        add_round_key(s, round);
        // InvMixColumns (omitted after the final AddRoundKey)
        if (round > 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t* col = s + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3, 9);
                col[1] = gmul(a0, 9) ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13);
                col[2] = gmul(a0,13) ^ gmul(a1, 9) ^ gmul(a2,14) ^ gmul(a3,11);
                col[3] = gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2, 9) ^ gmul(a3,14);
            }
        }
    }
}

void Aes::cbc_encrypt(const uint8_t iv[16], uint8_t* data, size_t len) const {
    uint8_t prev[16];
    std::memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= len; off += 16) {
        for (int i = 0; i < 16; i++) data[off + i] ^= prev[i];
        encrypt_block(data + off);
        std::memcpy(prev, data + off, 16);
    }
}

void Aes::cbc_decrypt(const uint8_t iv[16], uint8_t* data, size_t len) const {
    uint8_t prev[16], cipher[16];
    std::memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= len; off += 16) {
        std::memcpy(cipher, data + off, 16);
        decrypt_block(data + off);
        for (int i = 0; i < 16; i++) data[off + i] ^= prev[i];
        std::memcpy(prev, cipher, 16);
    }
}

void aes_cbc_decrypt_stream(const uint8_t* key, int key_len,
                            std::vector<uint8_t>& data) {
    if (data.size() < 32 || (data.size() % 16) != 0) { data.clear(); return; }
    Aes(key, key_len).cbc_decrypt(data.data(), data.data() + 16, data.size() - 16);
    data.erase(data.begin(), data.begin() + 16);
    // Strip PKCS#7 padding.
    uint8_t pad = data.back();
    if (pad >= 1 && pad <= 16 && pad <= data.size())
        data.resize(data.size() - pad);
}

// ── Standard security handler, revisions 5 and 6 ──────────────

bool aes256_file_key(int revision, const std::string& password,
                     const std::string& o, const std::string& u,
                     const std::string& oe, const std::string& ue,
                     uint8_t out_key[32]) {
    // /U and /O are hash(32) || validation salt(8) || key salt(8).
    if (u.size() < 48) return false;
    const auto* ub = reinterpret_cast<const uint8_t*>(u.data());
    uint8_t hash[32];

    // Algorithm 11: user password.
    password_hash(revision, password, ub + 32, nullptr, 0, hash);
    if (std::memcmp(hash, ub, 32) == 0 && ue.size() >= 32) {
        password_hash(revision, password, ub + 40, nullptr, 0, hash);
        unwrap_file_key(hash, ue, out_key);
        return true;
    }

    // Algorithm 12: owner password, which is validated against /U.
    if (o.size() < 48 || oe.size() < 32) return false;
    const auto* ob = reinterpret_cast<const uint8_t*>(o.data());
    password_hash(revision, password, ob + 32, ub, 48, hash);
    if (std::memcmp(hash, ob, 32) != 0) return false;
    password_hash(revision, password, ob + 40, ub, 48, hash);
    unwrap_file_key(hash, oe, out_key);
    return true;
}

// ── PDF 1.x password padding ──────────────────────────────────

void pad_password(const std::string& password, uint8_t out[32]) {
    size_t n = std::min<size_t>(password.size(), 32);
    if (n) std::memcpy(out, password.data(), n);
    std::memcpy(out + n, kPasswordPad, 32 - n);
}

}}  // namespace jdoc::pdf_crypt
