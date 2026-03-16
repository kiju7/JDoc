// Thread-safety & benchmark test for JDoc C API.
// Tests: correctness under concurrency + throughput at scale (1000 docs).
#include "jdoc/jdoc_c_api.h"

#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <functional>

// ---------------------------------------------------------------------------
// Test file generators
// ---------------------------------------------------------------------------

static void create_test_pdf(const std::string& path, int id) {
    std::string content = "Document " + std::to_string(id) +
        " - thread safety benchmark. "
        "Name: Hong Gildong, Phone: 010-1234-5678, Email: test@example.com, "
        "ID: 901231-1234567, Account: 110-123-456789. "
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 repeat content for realistic size. "
        "Lorem ipsum dolor sit amet consectetur adipiscing elit.";

    std::string stream_content = "BT /F1 12 Tf 72 720 Td (" + content + ") Tj ET";
    std::string len = std::to_string(stream_content.size());

    std::ostringstream pdf;
    pdf << "%PDF-1.4\n"
        << "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
        << "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
        << "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]"
        << "/Contents 4 0 R/Resources<</Font<</F1 5 0 R>>>>>>endobj\n"
        << "5 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n"
        << "4 0 obj<</Length " << len << ">>\nstream\n"
        << stream_content << "\nendstream\nendobj\n"
        << "xref\n0 6\n"
        << "0000000000 65535 f \n"
        << "0000000009 00000 n \n"
        << "0000000058 00000 n \n"
        << "0000000115 00000 n \n"
        << "0000000306 00000 n \n"
        << "0000000266 00000 n \n"
        << "trailer<</Size 6/Root 1 0 R>>\nstartxref\n500\n%%EOF\n";

    std::ofstream f(path, std::ios::binary);
    f << pdf.str();
}

static void create_test_html(const std::string& path, int id) {
    std::ostringstream html;
    html << "<html><head><title>Test " << id << "</title></head><body>"
         << "<h1>Thread Safety Test Document " << id << "</h1>"
         << "<p>Name: Hong Gildong, Phone: 010-1234-5678</p>"
         << "<p>Email: test" << id << "@example.com</p>"
         << "<table><tr><th>ID</th><th>Name</th></tr>"
         << "<tr><td>" << id << "</td><td>User " << id << "</td></tr></table>"
         << "<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
         << "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.</p>"
         << "</body></html>";

    std::ofstream f(path, std::ios::binary);
    f << html.str();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct BenchResult {
    std::string label;
    int total;
    int success;
    int fail;
    long elapsed_ms;
    double docs_per_sec;
};

static void print_result(const BenchResult& r) {
    std::cout << "  " << std::left << std::setw(36) << r.label
              << " | " << std::right << std::setw(5) << r.success << "/" << std::setw(5) << r.total
              << " | " << std::setw(6) << r.elapsed_ms << "ms"
              << " | " << std::fixed << std::setprecision(1) << std::setw(8) << r.docs_per_sec << " docs/s"
              << " | " << (r.fail == 0 ? "PASS" : "FAIL")
              << std::endl;
}

using ExtractFn = std::function<bool(const std::string& path)>;

static BenchResult run_bench(const std::string& label,
                              const std::vector<std::string>& paths,
                              int num_threads, int total_ops,
                              ExtractFn fn) {
    BenchResult res{label, total_ops, 0, 0, 0, 0};
    std::atomic<int> success{0}, fail{0};
    std::atomic<int> next_op{0};

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&]() {
            while (true) {
                int op = next_op.fetch_add(1);
                if (op >= total_ops) break;
                if (fn(paths[op % paths.size()])) {
                    success++;
                } else {
                    fail++;
                }
            }
        });
    }

    for (auto& th : threads) th.join();

    auto elapsed = std::chrono::steady_clock::now() - start;
    res.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    res.success = success;
    res.fail = fail;
    res.docs_per_sec = res.elapsed_ms > 0 ? (double)res.success / res.elapsed_ms * 1000.0 : 0;
    return res;
}

// ---------------------------------------------------------------------------
// Extract functions
// ---------------------------------------------------------------------------

static bool extract_text(const std::string& path) {
    char err[256] = {};
    char* text = jdoc_extract_text(path.c_str(), err, sizeof(err));
    if (text && strlen(text) > 0) {
        jdoc_free_string(text);
        return true;
    }
    if (text) jdoc_free_string(text);
    return false;
}

static bool extract_all_paged(const std::string& path) {
    char err[256] = {};
    JDocImage* images = nullptr;
    int img_count = 0;
    JDocPageText* pages = nullptr;
    int page_count = 0;
    char* text = jdoc_extract_all_paged(path.c_str(),
        &images, &img_count, &pages, &page_count, err, sizeof(err));
    if (text) {
        jdoc_free_string(text);
        if (images) jdoc_free_images(images, img_count);
        if (pages) jdoc_free_page_texts(pages, page_count);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    const int SMALL_OPS = 200;
    const int BENCH_OPS = 1000;
    const int THREADS[] = {1, 2, 4, 8};

    std::cout << "==========================================================" << std::endl;
    std::cout << "  JDoc Thread Safety & Benchmark Test" << std::endl;
    std::cout << "==========================================================" << std::endl;

    // --- Generate test files ---
    const int NUM_PDF = 100;
    const int NUM_HTML = 100;
    std::vector<std::string> pdf_paths, html_paths, mixed_paths;

    std::cout << "\nGenerating " << NUM_PDF << " PDFs + " << NUM_HTML << " HTMLs..." << std::endl;
    for (int i = 0; i < NUM_PDF; i++) {
        std::string p = "/tmp/bench_" + std::to_string(i) + ".pdf";
        create_test_pdf(p, i);
        pdf_paths.push_back(p);
        mixed_paths.push_back(p);
    }
    for (int i = 0; i < NUM_HTML; i++) {
        std::string p = "/tmp/bench_" + std::to_string(i) + ".html";
        create_test_html(p, i);
        html_paths.push_back(p);
        mixed_paths.push_back(p);
    }

    // --- 1. Correctness: single-threaded baseline ---
    std::cout << "\n--- [1] Single-threaded correctness ---" << std::endl;
    int baseline_ok = 0;
    for (int i = 0; i < 10; i++) {
        if (extract_text(pdf_paths[i])) baseline_ok++;
        if (extract_text(html_paths[i])) baseline_ok++;
    }
    std::cout << "  Baseline: " << baseline_ok << "/20 extracted OK "
              << (baseline_ok == 20 ? "PASS" : "FAIL") << std::endl;

    // --- 2. Thread safety: extract_text ---
    std::cout << "\n--- [2] Thread safety: jdoc_extract_text ---" << std::endl;
    std::cout << "  " << std::left << std::setw(36) << "Test"
              << " | " << std::right << std::setw(11) << "OK/Total"
              << " |  Time   | Throughput | Result" << std::endl;
    std::cout << "  " << std::string(85, '-') << std::endl;

    for (int t : THREADS) {
        auto r = run_bench("PDF " + std::to_string(t) + "T extract_text",
                           pdf_paths, t, SMALL_OPS, extract_text);
        print_result(r);
    }
    for (int t : THREADS) {
        auto r = run_bench("HTML " + std::to_string(t) + "T extract_text",
                           html_paths, t, SMALL_OPS, extract_text);
        print_result(r);
    }
    for (int t : THREADS) {
        auto r = run_bench("Mixed " + std::to_string(t) + "T extract_text",
                           mixed_paths, t, SMALL_OPS, extract_text);
        print_result(r);
    }

    // --- 3. Thread safety: extract_all_paged ---
    std::cout << "\n--- [3] Thread safety: jdoc_extract_all_paged ---" << std::endl;
    std::cout << "  " << std::left << std::setw(36) << "Test"
              << " | " << std::right << std::setw(11) << "OK/Total"
              << " |  Time   | Throughput | Result" << std::endl;
    std::cout << "  " << std::string(85, '-') << std::endl;

    for (int t : THREADS) {
        auto r = run_bench("PDF " + std::to_string(t) + "T extract_all_paged",
                           pdf_paths, t, SMALL_OPS, extract_all_paged);
        print_result(r);
    }
    for (int t : THREADS) {
        auto r = run_bench("HTML " + std::to_string(t) + "T extract_all_paged",
                           html_paths, t, SMALL_OPS, extract_all_paged);
        print_result(r);
    }

    // --- 4. Benchmark: 1000 documents ---
    std::cout << "\n--- [4] Benchmark: " << BENCH_OPS << " documents ---" << std::endl;
    std::cout << "  " << std::left << std::setw(36) << "Test"
              << " | " << std::right << std::setw(11) << "OK/Total"
              << " |  Time   | Throughput | Result" << std::endl;
    std::cout << "  " << std::string(85, '-') << std::endl;

    // PDF 1000 at various thread counts
    for (int t : THREADS) {
        auto r = run_bench("PDF " + std::to_string(t) + "T x" + std::to_string(BENCH_OPS),
                           pdf_paths, t, BENCH_OPS, extract_text);
        print_result(r);
    }

    // HTML 1000 at various thread counts
    for (int t : THREADS) {
        auto r = run_bench("HTML " + std::to_string(t) + "T x" + std::to_string(BENCH_OPS),
                           html_paths, t, BENCH_OPS, extract_text);
        print_result(r);
    }

    // Mixed 1000 at 8 threads
    {
        auto r = run_bench("Mixed 8T x" + std::to_string(BENCH_OPS),
                           mixed_paths, 8, BENCH_OPS, extract_text);
        print_result(r);
    }

    // --- 5. Stress test: extract_all_paged 1000 mixed ---
    std::cout << "\n--- [5] Stress: extract_all_paged 1000 mixed 8T ---" << std::endl;
    {
        auto r = run_bench("Mixed 8T x1000 extract_all_paged",
                           mixed_paths, 8, BENCH_OPS, extract_all_paged);
        std::cout << "  " << std::left << std::setw(36) << r.label
                  << " | " << r.success << "/" << r.total
                  << " | " << r.elapsed_ms << "ms"
                  << " | " << std::fixed << std::setprecision(1) << r.docs_per_sec << " docs/s"
                  << " | " << (r.fail == 0 ? "PASS" : "FAIL") << std::endl;
    }

    // Cleanup
    for (auto& p : pdf_paths) std::remove(p.c_str());
    for (auto& p : html_paths) std::remove(p.c_str());

    std::cout << "\n==========================================================" << std::endl;
    std::cout << "  All tests complete." << std::endl;
    std::cout << "==========================================================" << std::endl;
    return 0;
}
