// test_eml.cpp - Tests for EML (RFC 5322 / MIME) email conversion
// License: MIT

#include "jdoc/eml.h"
#include "common/string_utils.h"
#include <iostream>
#include <string>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    std::cerr << "  Testing: " << #name << "... "; \
    try {

#define TEST_END \
        tests_passed++; \
        std::cerr << "OK\n"; \
    } catch (const std::exception& e) { \
        tests_failed++; \
        std::cerr << "FAILED: " << e.what() << "\n"; \
    }

#define ASSERT(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);

static std::string conv(const std::string& eml) {
    return jdoc::eml_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(eml.data()), eml.size());
}

void test_mime_codecs() {
    std::cerr << "MIME codecs:\n";

    TEST(base64_basic)
        ASSERT(jdoc::util::decode_base64("aGVsbG8=") == "hello");
        ASSERT(jdoc::util::decode_base64("aGVsbG8gd29ybGQ=") == "hello world");
    TEST_END

    TEST(base64_ignores_newlines)
        ASSERT(jdoc::util::decode_base64("aGVs\r\nbG8=") == "hello");
    TEST_END

    TEST(quoted_printable_hex_and_softbreak)
        ASSERT(jdoc::util::decode_quoted_printable("a=3Db") == "a=b");
        ASSERT(jdoc::util::decode_quoted_printable("line=\r\nwrap") == "linewrap");
    TEST_END

    TEST(quoted_printable_q_underscore)
        // RFC 2047 'Q' encoding: '_' is a space
        ASSERT(jdoc::util::decode_quoted_printable("a_b", true) == "a b");
    TEST_END

    TEST(charset_utf8_passthrough)
        std::string s = "\xEC\x95\x88\xEB\x85\x95";  // "안녕" UTF-8
        ASSERT(jdoc::util::charset_to_utf8(s, "utf-8") == s);
    TEST_END

    TEST(charset_euckr_to_utf8)
        std::string euckr = "\xC1\xD6\xB9\xCE\xB9\xF8\xC8\xA3";  // "주민번호"
        std::string utf8  = "\xEC\xA3\xBC\xEB\xAF\xBC\xEB\xB2\x88\xED\x98\xB8";
        ASSERT(jdoc::util::charset_to_utf8(euckr, "ks_c_5601-1987") == utf8);
    TEST_END
}

void test_eml_messages() {
    std::cerr << "\nEML messages:\n";

    TEST(rfc2047_subject_decoded)
        // Subject is a base64 EUC-KR encoded word for "주민번호".
        std::string eml =
            "Subject: =?ks_c_5601-1987?B?wda5zrn4yKM=?=\r\n"
            "From: a@b.com\r\n"
            "\r\n"
            "body\r\n";
        auto md = conv(eml);
        std::string utf8 = "\xEC\xA3\xBC\xEB\xAF\xBC\xEB\xB2\x88\xED\x98\xB8";
        ASSERT(md.find(utf8) != std::string::npos);
        ASSERT(md.find("=?ks_c") == std::string::npos);  // encoded word gone
    TEST_END

    TEST(base64_body_decoded)
        // Body is base64 of "hello mail".
        std::string eml =
            "Subject: hi\r\n"
            "Content-Transfer-Encoding: base64\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "\r\n"
            "aGVsbG8gbWFpbA==\r\n";
        auto md = conv(eml);
        ASSERT(md.find("hello mail") != std::string::npos);
        ASSERT(md.find("aGVsbG8") == std::string::npos);  // no raw base64
    TEST_END

    TEST(multipart_alternative_prefers_plain)
        std::string eml =
            "Subject: multi\r\n"
            "Content-Type: multipart/alternative; boundary=\"BND\"\r\n"
            "\r\n"
            "preamble\r\n"
            "--BND\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "\r\n"
            "PLAIN_BODY\r\n"
            "--BND\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "\r\n"
            "<p>HTML_BODY</p>\r\n"
            "--BND--\r\n";
        auto md = conv(eml);
        ASSERT(md.find("PLAIN_BODY") != std::string::npos);
        ASSERT(md.find("HTML_BODY") == std::string::npos);   // html branch skipped
        ASSERT(md.find("preamble") == std::string::npos);     // preamble ignored
    TEST_END

    TEST(headers_rendered)
        std::string eml =
            "Subject: Report\r\n"
            "From: alice@example.com\r\n"
            "To: bob@example.com\r\n"
            "Date: Thu, 1 Dec 2011 21:43:08 +0900\r\n"
            "\r\n"
            "the body text\r\n";
        auto md = conv(eml);
        ASSERT(md.find("# Report") != std::string::npos);
        ASSERT(md.find("**From:** alice@example.com") != std::string::npos);
        ASSERT(md.find("**To:** bob@example.com") != std::string::npos);
        ASSERT(md.find("the body text") != std::string::npos);
    TEST_END

    TEST(folded_content_type_header)
        // A Content-Type folded across two lines must still yield the boundary.
        std::string eml =
            "Subject: fold\r\n"
            "Content-Type: multipart/mixed;\r\n"
            "\tboundary=\"XY\"\r\n"
            "\r\n"
            "--XY\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "folded works\r\n"
            "--XY--\r\n";
        auto md = conv(eml);
        ASSERT(md.find("folded works") != std::string::npos);
    TEST_END
}

int main() {
    std::cerr << "=== jdoc eml tests ===\n\n";

    test_mime_codecs();
    test_eml_messages();

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";
    return tests_failed > 0 ? 1 : 0;
}
