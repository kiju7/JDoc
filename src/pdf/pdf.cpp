// pdf.cpp — facade: orchestration + public API (PDF -> Markdown, no PDFium).
#include "pdf_extract.h"
#include "common/string_utils.h"
#include "common/file_utils.h"
#include <fstream>
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

std::vector<uint8_t> get_page_content(PdfDoc& doc, const PdfObj& page_obj) {
    auto contents = doc.resolve(page_obj.get("Contents"));
    if (contents.is_stream()) {
        return doc.decode_stream(contents);
    }
    if (contents.is_arr()) {
        std::vector<uint8_t> combined;
        for (auto& ref : contents.arr) {
            auto stm = doc.resolve(ref);
            if (stm.is_stream()) {
                auto decoded = doc.decode_stream(stm);
                combined.insert(combined.end(), decoded.begin(), decoded.end());
                combined.push_back(' ');
            }
        }
        return combined;
    }
    return {};
}

// Extract from an in-memory buffer; pdf_path is used for error messages only.
static ExtractResult extract_pdf_buffer(const uint8_t* data, size_t size,
                                        const std::string& pdf_path,
                                        const ConvertOptions& opts) {
    ExtractResult result;

    // Check PDF header
    if (size < 5 || std::memcmp(data, "%PDF-", 5) != 0)
        throw std::runtime_error("Not a valid PDF file: " + pdf_path);

    PdfDoc doc(data, size);
    if (!doc.parse())
        throw std::runtime_error("Failed to parse PDF structure: " + pdf_path);

    // Handle encryption (supports Standard Security Handler with empty password)
    if (doc.trailer.has("Encrypt")) {
        if (!doc.init_encryption(""))
            throw std::runtime_error("Encrypted PDF requires a password: " + pdf_path);
    }

    // Get page tree
    auto root = doc.resolve(doc.trailer.get("Root"));
    auto pages = doc.resolve(root.get("Pages"));

    // Collect all page objects
    std::vector<PdfObj> page_objs;
    std::vector<int> page_obj_nums;
    // The page tree is attacker-controlled and its /Kids can form a cycle or
    // nest arbitrarily deep, so cap the recursion and skip page nodes already
    // seen by object number. The recovery scan below still finds every page if
    // this bails out early on a malformed tree.
    std::unordered_set<int> seen_pages;
    std::function<void(const PdfObj&, int)> collect_pages;
    collect_pages = [&](const PdfObj& node, int depth) {
        if (depth > 256) return;
        if (node.is_ref() && !seen_pages.insert(node.ref_num).second) return;
        auto n = doc.resolve(node);
        if (!n.is_dict()) return;
        auto& type = n.get("Type");
        if (type.is_name() && type.str_val == "Page") {
            page_objs.push_back(n);
            if (node.is_ref()) page_obj_nums.push_back(node.ref_num);
            else page_obj_nums.push_back(-1);
            return;
        }
        auto& kids = n.get("Kids");
        if (kids.is_arr()) {
            for (auto& kid : kids.arr) collect_pages(kid, depth + 1);
        }
    };
    if (pages.is_dict()) collect_pages(pages, 0);

    // Recovery: page tree missing or broken (truncated PDFs) — scan every
    // known object for /Type /Page and use them in object-number order.
    if (page_objs.empty()) {
        for (auto& [num, e] : doc.xref) {
            PdfObj obj = doc.get_obj(num);
            if (!obj.is_dict()) continue;
            auto& type = obj.get("Type");
            if (type.is_name() && type.str_val == "Page") {
                page_objs.push_back(obj);
                page_obj_nums.push_back(num);
            }
        }
    }
    if (page_objs.empty())
        throw std::runtime_error("Invalid PDF page tree: " + pdf_path);

    int tp = static_cast<int>(page_objs.size());
    result.total_pages = tp;
    result.all_lines.resize(tp);
    result.all_images.resize(tp);
    result.all_image_y.resize(tp);
    result.all_image_x.resize(tp);
    result.col_boundaries.resize(tp, 0);
    result.all_tables.resize(tp);
    result.all_annots.resize(tp);
    result.page_widths.resize(tp, 0);
    result.page_heights.resize(tp, 0);

    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < tp; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    // Always extract images for markdown references; only save to disk if dir is set
    std::string image_dir;
    ConvertOptions img_opts = opts;
    img_opts.images = true;
    if (!opts.image_dir.empty()) {
        image_dir = opts.image_dir;
        util::ensure_dir(image_dir);
    }

    std::unordered_map<int, PdfFont> font_cache;

    for (int p : page_indices) {
        if (p < 0 || p >= tp) continue;
        auto& page_obj = page_objs[p];

        // Get page dimensions from MediaBox
        auto mediabox = doc.resolve(page_obj.get("MediaBox"));
        double page_w = 612, page_h = 792; // default letter
        if (mediabox.is_arr() && mediabox.arr.size() >= 4) {
            page_w = mediabox.arr[2].as_num() - mediabox.arr[0].as_num();
            page_h = mediabox.arr[3].as_num() - mediabox.arr[1].as_num();
        }
        result.page_widths[p] = page_w;
        result.page_heights[p] = page_h;

        // Get resources (inherit from parent)
        auto resources = doc.resolve(page_obj.get("Resources"));
        if (!resources.is_dict()) {
            auto parent = doc.resolve(page_obj.get("Parent"));
            if (parent.is_dict()) resources = doc.resolve(parent.get("Resources"));
        }

        // Quick check: skip pages with no fonts and no extractable images
        bool has_fonts = false;
        {
            auto& font_res = resources.get("Font");
            if (!font_res.is_none()) {
                auto fd = doc.resolve(font_res);
                has_fonts = fd.is_dict() && !fd.dict.empty();
            }
        }
        if (!has_fonts && !img_opts.images) continue;

        // Parse content stream
        auto content_data = get_page_content(doc, page_obj);
        if (content_data.empty()) continue;

        // Extract text lines
        bool plaintext = (opts.format == OutputFormat::PLAINTEXT);
        bool need_tables = opts.tables && !plaintext;
        bool need_graphics = need_tables || img_opts.images;

        auto parse_result = parse_content_stream(doc, content_data, resources, page_h,
                                                  &font_cache, !need_graphics);

        result.all_lines[p] = chars_to_lines(parse_result.chars, &result.col_boundaries[p]);

        // Extract annotations (text notes, links)
        result.all_annots[p] = extract_annotations(doc, page_obj, page_h);

        if (need_tables) {
            PageCharCache cache;
            cache.build(parse_result.chars);

            result.all_tables[p] = detect_tables(parse_result.segments, cache,
                page_w, page_h);
            auto text_tables = detect_text_tables(cache, result.all_tables[p],
                page_w, page_h);
            for (auto& tt : text_tables)
                result.all_tables[p].push_back(std::move(tt));
        }

        // Image extraction
        if (img_opts.images) {
            // Check for layered page
            bool has_regular = false, has_mask = false;
            for (auto& ip : parse_result.images) {
                PdfObj xobj;
                if (ip.xobj_ref >= 0) xobj = doc.get_obj(ip.xobj_ref);
                if (!xobj.is_stream()) continue;
                int bpc = xobj.get("BitsPerComponent").as_int();
                if (bpc == 1) has_mask = true;
                else has_regular = true;
            }

            if (has_regular && has_mask) {
                // Layered: render as composite
                auto rendered = render_page_composite(doc, page_obj, parse_result,
                                                      p, page_w, page_h, image_dir);
                if (!rendered.data.empty() || !rendered.pixels.empty() || !rendered.saved_path.empty()) {
                    result.all_images[p].push_back(std::move(rendered));
                    result.all_image_y[p].push_back(page_h);
                    result.all_image_x[p].push_back(0);
                }
            } else {
                auto extracted = extract_page_images(doc, page_obj, parse_result, p, image_dir, opts.min_image_size);
                for (auto& ei : extracted) {
                    // ctm[5] is the Y translation in PDF coordinates (origin bottom-left)
                    // ctm[3] is vertical scale; y_top = ctm[5] + abs(ctm[3])
                    double y_top = ei.ctm[5] + std::abs(ei.ctm[3]);
                    result.all_image_y[p].push_back(y_top);
                    result.all_image_x[p].push_back(ei.ctm[4]); // X position
                    result.all_images[p].push_back(std::move(ei.img));
                }
            }

            // Fallback: render page for scanned/vector-only pages
            if (result.all_images[p].empty() && result.all_lines[p].empty()) {
                if (!parse_result.images.empty() || !parse_result.segments.empty()) {
                    auto rendered = render_page_composite(doc, page_obj, parse_result,
                                                          p, page_w, page_h, image_dir);
                    if (!rendered.data.empty() || !rendered.pixels.empty() || !rendered.saved_path.empty()) {
                        result.all_images[p].push_back(std::move(rendered));
                        result.all_image_y[p].push_back(page_h);
                        result.all_image_x[p].push_back(0);
                    }
                }
            }

            // Release pixel data after writing to disk
            if (!image_dir.empty()) {
                for (auto& img : result.all_images[p]) {
                    if (!img.saved_path.empty()) {
                        img.data.clear();
                        img.data.shrink_to_fit();
                        img.pixels.clear();
                        img.pixels.shrink_to_fit();
                    }
                }
            }
        }
    }

    // Extract bookmarks
    auto outlines = doc.resolve(root.get("Outlines"));
    if (outlines.is_dict()) {
        collect_bookmarks(doc, outlines, 0, result.bookmarks);
        // Remap bookmark page references (obj num → page index)
        for (auto& bm : result.bookmarks) {
            if (bm.page >= 0) {
                bool found = false;
                for (int i = 0; i < (int)page_obj_nums.size(); i++) {
                    if (page_obj_nums[i] == bm.page) {
                        bm.page = i;
                        found = true;
                        break;
                    }
                }
                if (!found) bm.page = -1;
            }
        }
    }

    result.stats.compute(result.all_lines);
    return result;
}

static ExtractResult extract_pdf(const std::string& pdf_path, const ConvertOptions& opts) {
    std::ifstream file(pdf_path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open PDF: " + pdf_path);

    std::streamsize fsize = file.tellg();
    if (fsize <= 0) throw std::runtime_error("Empty PDF file: " + pdf_path);
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> file_data(static_cast<size_t>(fsize));
    if (!file.read(reinterpret_cast<char*>(file_data.data()), fsize))
        throw std::runtime_error("Cannot read PDF: " + pdf_path);
    file.close();

    return extract_pdf_buffer(file_data.data(), file_data.size(), pdf_path, opts);
}


}} // namespace jdoc::pdf_detail

namespace jdoc {
using namespace pdf_detail;

std::string pdf_to_markdown(const std::string& pdf_path, ConvertOptions opts) {
    auto r = extract_pdf(pdf_path, opts);
    return result_to_markdown(r, opts);
}

std::vector<PageChunk> pdf_to_markdown_chunks(const std::string& pdf_path,
                                              ConvertOptions opts) {
    auto r = extract_pdf(pdf_path, opts);
    return result_to_chunks(r, opts);
}

std::string pdf_to_markdown_mem(const uint8_t* data, size_t size,
                                ConvertOptions opts) {
    auto r = extract_pdf_buffer(data, size, "<memory>", opts);
    return result_to_markdown(r, opts);
}

std::vector<PageChunk> pdf_to_markdown_chunks_mem(const uint8_t* data, size_t size,
                                                  ConvertOptions opts) {
    auto r = extract_pdf_buffer(data, size, "<memory>", opts);
    return result_to_chunks(r, opts);
}
} // namespace jdoc
