#include "jdoc/jdoc_c_api.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) \
            throw std::runtime_error("Check failed: " #condition); \
    } while (false)

int count_page(const JDocPage* page, void* userdata) {
    CHECK(page != nullptr);
    CHECK(page->text != nullptr);
    ++*static_cast<int*>(userdata);
    return 1;
}

std::vector<char> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

} // namespace

int main() {
    const char* path = "test/fixtures/pdf/sample.pdf";
    char error[1024] = {};
    JDocOptions opts = jdoc_default_options();
    opts.images = 0;

    int count = 0;
    JDocPage* pages = jdoc_convert_pages(
        path, &opts, &count, error, static_cast<int>(sizeof(error)));
    CHECK(pages != nullptr);
    CHECK(count > 0);
    CHECK(pages[0].text != nullptr);
    CHECK(pages[0].page_width > 0);
    CHECK(pages[0].page_height > 0);
    jdoc_free_pages(pages, count);

    const std::vector<char> data = read_file(path);
    count = 0;
    pages = jdoc_convert_pages_mem(
        data.data(), static_cast<int>(data.size()), "sample.pdf", &opts,
        &count, error, static_cast<int>(sizeof(error)));
    CHECK(pages != nullptr);
    CHECK(count > 0);
    jdoc_free_pages(pages, count);

    int streamed = 0;
    CHECK(jdoc_convert_pages_mem_stream(
              data.data(), static_cast<int>(data.size()), "sample.pdf", &opts,
              count_page, &streamed, error,
              static_cast<int>(sizeof(error))) == 0);
    CHECK(streamed == count);

    char* empty = jdoc_convert_mem(
        nullptr, 0, "empty.txt", &opts, error,
        static_cast<int>(sizeof(error)));
    CHECK(empty != nullptr);
    CHECK(std::strcmp(empty, "") == 0);
    jdoc_free_string(empty);

    opts.format = "html";
    char* invalid = jdoc_convert(
        path, &opts, error, static_cast<int>(sizeof(error)));
    CHECK(invalid == nullptr);
    CHECK(error[0] != '\0');

    std::cout << "C API regression tests passed\n";
}
