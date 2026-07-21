// test_pdf_crypt.cpp — Standard test vectors for the PDF crypto primitives
// MD5 (RFC 1321), SHA-2 (FIPS 180-4), AES (FIPS 197), RC4.
#include "pdf/pdf_crypt.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

using namespace jdoc::pdf_crypt;

namespace {

int failures = 0;

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 0xF]; }
    return s;
}

void expect(const char* name, const std::string& got, const std::string& want) {
    if (got == want) {
        std::cout << "    ok   " << name << "\n";
    } else {
        std::cout << "    FAIL " << name << "\n         got  " << got
                  << "\n         want " << want << "\n";
        failures++;
    }
}

const uint8_t kAbc[3] = {'a', 'b', 'c'};

void test_hashes() {
    std::cout << "[1] Hashes\n";
    uint8_t h[64];
    md5(kAbc, 3, h);
    expect("MD5(abc)", hex(h, 16), "900150983cd24fb0d6963f7d28e17f72");
    md5(nullptr, 0, h);
    expect("MD5(empty)", hex(h, 16), "d41d8cd98f00b204e9800998ecf8427e");

    sha256(kAbc, 3, h);
    expect("SHA-256(abc)", hex(h, 32),
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256(nullptr, 0, h);
    expect("SHA-256(empty)", hex(h, 32),
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    sha384(kAbc, 3, h);
    expect("SHA-384(abc)", hex(h, 48),
           "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
           "8086072ba1e7cc2358baeca134c825a7");

    sha512(kAbc, 3, h);
    expect("SHA-512(abc)", hex(h, 64),
           "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
           "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    sha512(nullptr, 0, h);
    expect("SHA-512(empty)", hex(h, 64),
           "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
           "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");

    // A multi-block message exercises the padding boundary of both variants.
    const std::string long_msg(200, 'a');
    const auto* lm = reinterpret_cast<const uint8_t*>(long_msg.data());
    sha256(lm, long_msg.size(), h);
    expect("SHA-256(200 x 'a')", hex(h, 32),
           "c2a908d98f5df987ade41b5fce213067efbcc21ef2240212a41e54b5e7c28ae5");
    sha512(lm, long_msg.size(), h);
    expect("SHA-512(200 x 'a')", hex(h, 64),
           "4b11459c33f52a22ee8236782714c150a3b2c60994e9acee17fe68947a3e6789"
           "f31e7668394592da7bef827cddca88c4e6f86e4df7ed1ae6cba71f3e98faee9f");
}

void test_aes() {
    std::cout << "[2] AES (FIPS 197 appendix C)\n";
    uint8_t block[16], key[32];
    for (int i = 0; i < 32; i++) key[i] = static_cast<uint8_t>(i);

    for (int i = 0; i < 16; i++) block[i] = static_cast<uint8_t>(i * 0x11);
    Aes aes128(key, 16);
    aes128.encrypt_block(block);
    expect("AES-128 encrypt", hex(block, 16), "69c4e0d86a7b0430d8cdb78070b4c55a");
    aes128.decrypt_block(block);
    expect("AES-128 decrypt", hex(block, 16), "00112233445566778899aabbccddeeff");

    for (int i = 0; i < 16; i++) block[i] = static_cast<uint8_t>(i * 0x11);
    Aes aes256(key, 32);
    aes256.encrypt_block(block);
    expect("AES-256 encrypt", hex(block, 16), "8ea2b7ca516745bfeafc49904b496089");
    aes256.decrypt_block(block);
    expect("AES-256 decrypt", hex(block, 16), "00112233445566778899aabbccddeeff");

    // CBC round trip over several blocks (the mode used by Algorithm 2.B).
    uint8_t iv[16] = {};
    uint8_t data[48], original[48];
    for (int i = 0; i < 48; i++) data[i] = original[i] = static_cast<uint8_t>(i);
    aes256.cbc_encrypt(iv, data, sizeof(data));
    bool changed = std::memcmp(data, original, sizeof(data)) != 0;
    expect("AES-256-CBC alters data", changed ? "yes" : "no", "yes");
    aes256.cbc_decrypt(iv, data, sizeof(data));
    expect("AES-256-CBC round trip", hex(data, 48), hex(original, 48));
}

void test_rc4() {
    std::cout << "[3] RC4\n";
    uint8_t buf[9];
    std::memcpy(buf, "Plaintext", 9);
    rc4(reinterpret_cast<const uint8_t*>("Key"), 3, buf, 9);
    expect("RC4(Key, Plaintext)", hex(buf, 9), "bbf316e8d940af0ad3");
    rc4(reinterpret_cast<const uint8_t*>("Key"), 3, buf, 9);
    expect("RC4 round trip", std::string(reinterpret_cast<char*>(buf), 9), "Plaintext");
}

}  // namespace

int main() {
    std::cout << "=== jdoc PDF crypto test ===\n\n";
    test_hashes();
    test_aes();
    test_rc4();
    std::cout << "\n" << (failures ? "FAILED" : "All vectors pass") << "\n";
    return failures ? 1 : 0;
}
