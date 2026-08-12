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

// Hardcoded /tmp does not exist on Windows, so every generated file silently
// failed to write there and the whole suite ran on nothing. (Avoids
// std::filesystem: gcc 8 still needs -lstdc++fs for it.)
static std::string temp_dir() {
    for (const char* var : {"TMPDIR", "TEMP", "TMP"}) {
        const char* v = std::getenv(var);
        if (v && *v) return v;
    }
    return "/tmp";
}

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

// Multi-page PDF with computed xref offsets. Odd pages are plain text; even
// pages carry 60 filled rects and no text operator, which trips the
// vector-glyph fallback so every conversion runs the composite renderer and
// PNG encoder inside the parallel page workers.
static void create_multipage_pdf(const std::string& path, int n_pages) {
    std::vector<std::string> objs;
    std::string kids;
    for (int p = 0; p < n_pages; p++)
        kids += std::to_string(4 + 2 * p) + " 0 R ";
    objs.push_back("<</Type/Catalog/Pages 2 0 R>>");
    objs.push_back("<</Type/Pages/Kids[" + kids + "]/Count " +
                   std::to_string(n_pages) + "/MediaBox[0 0 612 792]>>");
    objs.push_back("<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>");
    for (int p = 0; p < n_pages; p++) {
        std::string page =
            "<</Type/Page/Parent 2 0 R/Contents " +
            std::to_string(5 + 2 * p) + " 0 R";
        std::string content;
        if (p % 2 == 0) {
            page += "/Resources<</Font<</F1 3 0 R>>>>>>";
            content = "BT /F1 12 Tf 72 720 Td (Page " + std::to_string(p + 1) +
                      " body: parallel page render check) Tj ET";
        } else {
            page += "/Resources<<>>>>";
            content = "0 0 0 rg\n";
            for (int k = 0; k < 60; k++)
                content += std::to_string(72 + (k % 10) * 45) + " " +
                           std::to_string(120 + (k / 10) * 100) + " 20 20 re f\n";
        }
        objs.push_back(page);
        objs.push_back("<</Length " + std::to_string(content.size()) +
                       ">>\nstream\n" + content + "\nendstream");
    }

    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objs.size(); i++) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj" + objs[i] + "endobj\n";
    }
    size_t xref_pos = pdf.size();
    pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    char line[32];
    for (size_t off : offsets) {
        std::snprintf(line, sizeof(line), "%010zu 00000 n \n", off);
        pdf += line;
    }
    pdf += "trailer<</Size " + std::to_string(objs.size() + 1) +
           "/Root 1 0 R>>\nstartxref\n" + std::to_string(xref_pos) + "\n%%EOF\n";

    std::ofstream f(path, std::ios::binary);
    f << pdf;
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
    char* text = jdoc_convert(path.c_str(), nullptr, err, sizeof(err));
    if (text && strlen(text) > 0) {
        jdoc_free_string(text);
        return true;
    }
    if (text) jdoc_free_string(text);
    return false;
}

// Full per-page conversion collapsed into one comparable string: page texts
// plus every image's name, format and raw bytes. Two conversions of the same
// document must produce identical signatures regardless of thread schedule.
static std::string pages_signature(const std::string& path) {
    char err[256] = {};
    JDocOptions opts = jdoc_default_options();
    opts.images = 1;
    int count = 0;
    JDocPage* pages = jdoc_convert_pages(path.c_str(), &opts, &count, err, sizeof(err));
    if (!pages) return "";
    std::string sig = "pages=" + std::to_string(count) + ";";
    for (int i = 0; i < count; i++) {
        sig += "p" + std::to_string(pages[i].page_number) + ":";
        if (pages[i].text) sig += pages[i].text;
        sig += "|imgs=" + std::to_string(pages[i].image_count) + ";";
        for (int j = 0; j < pages[i].image_count; j++) {
            const JDocImage& im = pages[i].images[j];
            sig += im.name ? im.name : "";
            sig += "/";
            sig += im.format ? im.format : "";
            sig += "/" + std::to_string(im.data_size) + "/";
            if (im.data && im.data_size > 0)
                sig.append(im.data, im.data + im.data_size);
            sig += ";";
        }
    }
    jdoc_free_pages(pages, count);
    return sig;
}

static bool extract_all_paged(const std::string& path) {
    char err[256] = {};
    JDocOptions opts = jdoc_default_options();
    opts.images = 1;
    int count = 0;
    JDocPage* pages = jdoc_convert_pages(path.c_str(), &opts, &count, err, sizeof(err));
    if (pages || count == 0) {
        jdoc_free_pages(pages, count);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    if (jdoc_default_options().images != 1) {
        std::cerr << "default image extraction must be enabled\n";
        return 1;
    }

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
    const std::string tmp = temp_dir();

    std::cout << "\nGenerating " << NUM_PDF << " PDFs + " << NUM_HTML << " HTMLs..." << std::endl;
    for (int i = 0; i < NUM_PDF; i++) {
        std::string p = tmp + "/bench_" + std::to_string(i) + ".pdf";
        create_test_pdf(p, i);
        pdf_paths.push_back(p);
        mixed_paths.push_back(p);
    }
    for (int i = 0; i < NUM_HTML; i++) {
        std::string p = tmp + "/bench_" + std::to_string(i) + ".html";
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

    // --- 6. Same-document parallel page rendering ---
    // One multi-page PDF converted concurrently from many threads. The
    // internal page workers (PdfDoc load_mu, shared font cache, composite
    // render + PNG encode) overlap with the outer threads, and every result
    // must be byte-identical to the single-threaded baseline signature.
    int exit_code = 0;
    std::cout << "\n--- [6] Multi-page parallel render (same document) ---" << std::endl;
    {
        const std::string mp_path = tmp + "/bench_multipage.pdf";
        create_multipage_pdf(mp_path, 8);

        std::string baseline = pages_signature(mp_path);
        bool base_ok = baseline.compare(0, 8, "pages=8;") == 0 &&
                       baseline.find("Page 7 body") != std::string::npos &&
                       baseline.find("/png/") != std::string::npos;
        std::cout << "  Baseline: 8 pages, text + composite PNG "
                  << (base_ok ? "PASS" : "FAIL") << std::endl;
        if (!base_ok) exit_code = 1;

        std::cout << "  " << std::left << std::setw(36) << "Test"
                  << " | " << std::right << std::setw(11) << "OK/Total"
                  << " |  Time   | Throughput | Result" << std::endl;
        std::cout << "  " << std::string(85, '-') << std::endl;

        const int MP_OPS = 64;
        for (int t : THREADS) {
            auto r = run_bench("MP-PDF " + std::to_string(t) + "T signature match",
                               {mp_path}, t, MP_OPS,
                               [&](const std::string& p) {
                                   return pages_signature(p) == baseline;
                               });
            print_result(r);
            if (r.fail != 0) exit_code = 1;
        }
        std::remove(mp_path.c_str());
    }

    // Cleanup
    for (auto& p : pdf_paths) std::remove(p.c_str());
    for (auto& p : html_paths) std::remove(p.c_str());

    std::cout << "\n==========================================================" << std::endl;
    std::cout << "  All tests complete." << std::endl;
    std::cout << "==========================================================" << std::endl;
    return exit_code;
}
