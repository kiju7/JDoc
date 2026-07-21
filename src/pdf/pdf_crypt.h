#pragma once
// Cryptographic primitives for the PDF standard security handler
// Knows nothing about the PDF object model — it operates on bytes only.
// License: MIT

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jdoc { namespace pdf_crypt {

// ── Hashes ────────────────────────────────────────────────────

void md5(const uint8_t* data, size_t len, uint8_t out[16]);
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void sha384(const uint8_t* data, size_t len, uint8_t out[48]);
void sha512(const uint8_t* data, size_t len, uint8_t out[64]);

// ── RC4 ───────────────────────────────────────────────────────

// Encrypts or decrypts `data` in place (the cipher is its own inverse).
void rc4(const uint8_t* key, int key_len, uint8_t* data, size_t len);

// ── AES ───────────────────────────────────────────────────────

// Both cipher directions for 128- and 256-bit keys. The key schedule is
// computed once per instance, so reuse the object across blocks.
class Aes {
public:
    // `key_len` is 16 or 32 bytes; any other value yields AES-128.
    Aes(const uint8_t* key, int key_len);

    void encrypt_block(uint8_t block[16]) const;
    void decrypt_block(uint8_t block[16]) const;

    // CBC without padding: `len` must be a multiple of 16.
    void cbc_encrypt(const uint8_t iv[16], uint8_t* data, size_t len) const;
    void cbc_decrypt(const uint8_t iv[16], uint8_t* data, size_t len) const;

private:
    void add_round_key(uint8_t s[16], int round) const;

    uint8_t rk_[240] = {};  // up to 15 round keys (AES-256)
    int rounds_ = 10;
};

// Decrypts a PDF stream or string: the IV is the leading 16 bytes and PKCS#7
// padding is stripped. `data` is cleared when the input is malformed.
void aes_cbc_decrypt_stream(const uint8_t* key, int key_len,
                            std::vector<uint8_t>& data);

// ── Standard security handler ─────────────────────────────────

// Pads or truncates a password to the fixed 32 bytes that revisions 2-4
// require (ISO 32000-1, Algorithm 2).
void pad_password(const std::string& password, uint8_t out[32]);

// Revisions 5 and 6 (AES-256).
// Recovers the 32-byte file encryption key from the /Encrypt entries, trying
// the user password first and then the owner password (ISO 32000-2 Algorithms
// 2.A, 11 and 12). Returns false when neither authenticates.
bool aes256_file_key(int revision, const std::string& password,
                     const std::string& o, const std::string& u,
                     const std::string& oe, const std::string& ue,
                     uint8_t out_key[32]);

}}  // namespace jdoc::pdf_crypt
