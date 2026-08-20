// pdf.cpp — facade: orchestration + public API (PDF -> Markdown, no PDFium).
#include "pdf_extract.h"
#include "pdf_limits.h"
#include "common/cpu_budget.h"
#include "common/string_utils.h"
#include "common/file_utils.h"
#include "common/mapped_file.h"
#include <fstream>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jdoc { namespace pdf_detail {

namespace {

class DisjointSet {
public:
    explicit DisjointSet(size_t count) : parent_(count), rank_(count, 0) {
        std::iota(parent_.begin(), parent_.end(), size_t{0});
    }

    size_t find(size_t node) {
        while (parent_[node] != node) {
            parent_[node] = parent_[parent_[node]];
            node = parent_[node];
        }
        return node;
    }

    void unite(size_t a, size_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (rank_[a] < rank_[b]) std::swap(a, b);
        parent_[b] = a;
        if (rank_[a] == rank_[b]) rank_[a]++;
    }

private:
    std::vector<size_t> parent_;
    std::vector<unsigned char> rank_;
};

// Limit simultaneous page-composite working sets process-wide. An A4 RGB
// canvas at 300 DPI is about 26 MiB, while compression and decoded source
// rasters add further transient allocations. Text-only pages do not take a
// lease and can still use the full CPU worker pool.
class CompositeMemoryGate {
public:
    void acquire(size_t bytes) {
        bytes = std::min(bytes, limits::kCompositeMemoryBudget);
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] {
            return bytes <= limits::kCompositeMemoryBudget - used_;
        });
        used_ += bytes;
    }

    void release(size_t bytes) {
        bytes = std::min(bytes, limits::kCompositeMemoryBudget);
        {
            std::lock_guard<std::mutex> lock(mu_);
            assert(used_ >= bytes);
            used_ -= bytes;
        }
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    size_t used_ = 0;
};

class CompositeMemoryLease {
public:
    CompositeMemoryLease(CompositeMemoryGate& gate, size_t bytes)
        : gate_(gate), bytes_(bytes) {
        gate_.acquire(bytes_);
    }

    ~CompositeMemoryLease() { gate_.release(bytes_); }

    CompositeMemoryLease(const CompositeMemoryLease&) = delete;
    CompositeMemoryLease& operator=(const CompositeMemoryLease&) = delete;

private:
    CompositeMemoryGate& gate_;
    size_t bytes_;
};

size_t composite_memory_cost(double page_w, double page_h) {
    constexpr long double kScale = 300.0L / 72.0L;
    constexpr long double kMaxPixels =
        static_cast<long double>(limits::kMaxDecodedPixels);
    long double pixels = std::max(0.0L,
        static_cast<long double>(page_w) * page_h * kScale * kScale);
    pixels = std::min(pixels, kMaxPixels);
    // RGB canvas + DEFLATE destination + decoded/mask scratch, with a small
    // fixed allowance for renderer vectors and compression state.
    long double bytes =
        pixels * limits::kCompositeBytesPerPixel +
        static_cast<long double>(limits::kCompositeFixedBytes);
    return static_cast<size_t>(std::min<long double>(
        bytes, limits::kCompositeMemoryBudget));
}

enum class ImagePayloadPolicy { MetadataOnly, Retain };

} // namespace

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

// Base CTM that maps the page's unrotated coordinate space (origin shifted
// to the MediaBox corner) into viewing coordinates. Landscape drawings are
// routinely stored as portrait pages with /Rotate 90; without this, every
// glyph on such a page looks vertical and is dropped by the rotated-text
// filter, and rendered composites come out sideways.
static void page_view_ctm(int rotate, double w, double h,
                          double llx, double lly, double out[6]) {
    const double T[6] = {1, 0, 0, 1, -llx, -lly};
    const double R90[6]  = {0, -1, 1, 0, 0, w};
    const double R180[6] = {-1, 0, 0, -1, w, h};
    const double R270[6] = {0, 1, -1, 0, h, 0};
    const double RID[6]  = {1, 0, 0, 1, 0, 0};
    const double* R = rotate == 90 ? R90 : rotate == 180 ? R180 :
                      rotate == 270 ? R270 : RID;
    mat_multiply(out, T, R);
}

// Extract from an in-memory buffer; pdf_path is used for error messages only.
static ExtractResult extract_pdf_buffer(const uint8_t* data, size_t size,
                                        const std::string& pdf_path,
                                        const ConvertOptions& opts,
                                        ImagePayloadPolicy payload_policy) {
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
    result.page_diags.resize(tp);
    result.page_widths.resize(tp, 0);
    result.page_heights.resize(tp, 0);

    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < tp; i++) page_indices.push_back(i);
    } else {
        // Two workers must never own the same result slot, so duplicate
        // page requests collapse to their first occurrence.
        std::unordered_set<int> requested;
        for (int p : opts.pages)
            if (requested.insert(p).second) page_indices.push_back(p);
    }

    std::string image_dir;
    if (opts.images && !opts.image_dir.empty()) {
        image_dir = opts.image_dir;
        util::ensure_dir(image_dir);
    }

    FontCache font_cache;
    // Shared across simultaneous conversions so document-level parallelism
    // cannot multiply every conversion's page-canvas budget.
    static CompositeMemoryGate composite_memory;

    // Parallelize independent pages, while respecting affinity/container CPU
    // limits. CompositeMemoryGate separately bounds concurrent raster working
    // sets; text-only pages do not pay that serialization cost.
    constexpr size_t kMaxPageWorkers = 8;
    const size_t hw = util::available_cpus();
    const size_t n_workers = std::min({page_indices.size(), hw, kMaxPageWorkers});

    auto process_page = [&](int p) {
        if (p < 0 || p >= tp) return;
        auto& page_obj = page_objs[p];

        // Diagnostics for the figure/decoration gates: per-cluster features
        // and per-raster placements, printed to stderr when JDOC_FIG_DEBUG=1.
        static const bool fig_debug = [] {
            const char* e = std::getenv("JDOC_FIG_DEBUG");
            return e && *e && *e != '0';
        }();

        // Page geometry: MediaBox and /Rotate are inheritable, so climb the
        // page tree for both (CAD exports often keep them on the Pages node).
        double page_w = 612, page_h = 792; // default letter
        double mb_llx = 0, mb_lly = 0;
        // CropBox bounds in page space (MediaBox origin). Content outside the
        // CropBox is trim-margin material (printer marks, bleed) that no
        // viewer shows; it must not seed figures or export as an image.
        double crop_x0 = 0, crop_y0 = 0, crop_x1 = 0, crop_y1 = 0;
        bool have_crop = false;
        int page_rotate = 0;
        PdfObj resources; // /Resources is inheritable like MediaBox and /Rotate
        {
            bool have_box = false, have_rot = false, have_res = false;
            PdfObj node = page_obj;
            for (int hop = 0; hop < 64 && (!have_box || !have_rot || !have_res);
                 hop++) {
                if (!have_box) {
                    auto mediabox = doc.resolve(node.get("MediaBox"));
                    if (mediabox.is_arr() && mediabox.arr.size() >= 4) {
                        mb_llx = mediabox.arr[0].as_num();
                        mb_lly = mediabox.arr[1].as_num();
                        page_w = mediabox.arr[2].as_num() - mb_llx;
                        page_h = mediabox.arr[3].as_num() - mb_lly;
                        have_box = true;
                    }
                }
                if (!have_crop) {
                    auto cropbox = doc.resolve(node.get("CropBox"));
                    if (cropbox.is_arr() && cropbox.arr.size() >= 4) {
                        crop_x0 = cropbox.arr[0].as_num();
                        crop_y0 = cropbox.arr[1].as_num();
                        crop_x1 = cropbox.arr[2].as_num();
                        crop_y1 = cropbox.arr[3].as_num();
                        have_crop = true;
                    }
                }
                if (!have_rot) {
                    auto rot = doc.resolve(node.get("Rotate"));
                    if (rot.is_num()) {
                        page_rotate = ((rot.as_int() % 360) + 360) % 360;
                        page_rotate -= page_rotate % 90;
                        have_rot = true;
                    }
                }
                if (!have_res) {
                    auto r = doc.resolve(node.get("Resources"));
                    if (r.is_dict()) {
                        resources = std::move(r);
                        have_res = true;
                    }
                }
                auto parent = doc.resolve(node.get("Parent"));
                if (!parent.is_dict()) break;
                node = std::move(parent);
            }
            if (have_crop) {
                if (crop_x0 > crop_x1) std::swap(crop_x0, crop_x1);
                if (crop_y0 > crop_y1) std::swap(crop_y0, crop_y1);
                crop_x0 = std::max(0.0, crop_x0 - mb_llx);
                crop_y0 = std::max(0.0, crop_y0 - mb_lly);
                crop_x1 = std::min(page_w, crop_x1 - mb_llx);
                crop_y1 = std::min(page_h, crop_y1 - mb_lly);
                if (crop_x1 - crop_x0 < 1.0 || crop_y1 - crop_y0 < 1.0)
                    have_crop = false; // degenerate: fall back to MediaBox
            }
            if (!have_crop) {
                crop_x0 = 0; crop_y0 = 0;
                crop_x1 = page_w; crop_y1 = page_h;
            }
            if (fig_debug)
                fprintf(stderr,
                        "[figdbg] p=%d media=%.1fx%.1f crop=(%.1f,%.1f)-(%.1f,%.1f)%s\n",
                        p + 1, page_w, page_h, crop_x0, crop_y0, crop_x1,
                        crop_y1, have_crop ? "" : " (=media)");
        }

        // Normalize into viewing coordinates so downstream consumers (text
        // filter, tables, composites, image placement) never see the raw
        // rotated space. Identity pages keep the historical fast path.
        double base_ctm[6];
        const double* initial_ctm = nullptr;
        if (page_rotate != 0 || mb_llx != 0 || mb_lly != 0) {
            page_view_ctm(page_rotate, page_w, page_h, mb_llx, mb_lly, base_ctm);
            initial_ctm = base_ctm;
        }
        if (page_rotate == 90 || page_rotate == 270)
            std::swap(page_w, page_h);
        result.page_widths[p] = page_w;
        result.page_heights[p] = page_h;

        // Quick check: skip pages with no fonts and no extractable images
        bool has_fonts = false;
        {
            auto& font_res = resources.get("Font");
            if (!font_res.is_none()) {
                auto fd = doc.resolve(font_res);
                has_fonts = fd.is_dict() && !fd.dict.empty();
            }
        }
        if (!has_fonts && !opts.images) return;

        // Parse content stream
        auto content_data = get_page_content(doc, page_obj);
        if (content_data.empty()) return;

        // Extract text lines
        bool plaintext = (opts.format == OutputFormat::PLAINTEXT);
        bool need_tables = opts.tables && !plaintext;
        bool need_graphics = need_tables || opts.images;

        ContentParseOptions parse_options;
        if (!need_graphics) {
            parse_options.graphics = GraphicsCollection::None;
        } else if (opts.images) {
            parse_options.graphics = GraphicsCollection::RenderPaths;
        } else {
            parse_options.graphics = GraphicsCollection::TableGeometry;
        }
        parse_options.page_width = page_w;
        auto parse_result = parse_content_stream(
            doc, content_data, resources, page_h, &font_cache, parse_options,
            initial_ctm);

        result.all_lines[p] = chars_to_lines(parse_result.chars, &result.col_boundaries[p]);

        // Extract annotations (text notes, links)
        result.all_annots[p] = extract_annotations(doc, page_obj, page_h, initial_ctm);

        if (need_tables) {
            PageCharCache cache;
            cache.build(parse_result.chars);

            result.all_tables[p] = detect_tables(parse_result.segments, cache,
                page_w, page_h);
            auto shade_tables = detect_shading_tables(parse_result.fill_rects,
                cache, result.all_tables[p], page_w, page_h);
            for (auto& st : shade_tables)
                result.all_tables[p].push_back(std::move(st));
            auto text_tables = detect_text_tables(cache, result.all_tables[p],
                page_w, page_h, result.col_boundaries[p]);
            for (auto& tt : text_tables)
                result.all_tables[p].push_back(std::move(tt));
        }

        // Image extraction
        if (opts.images) {
            auto render_composite = [&]() {
                const size_t memory_cost = composite_memory_cost(page_w, page_h);
                CompositeMemoryLease lease(composite_memory, memory_cost);
                return render_page_composite(
                    doc, resources, parse_result, p, page_w, page_h, image_dir,
                    0, &result.page_diags[p]);
            };

            // Decide whether image XObjects are independent assets or drawing
            // primitives that only make sense composited. Fragmented rasters
            // (print-driver strips, scanner tiles, layered stamp+photo pairs)
            // arrive as placements that abut or overlap in device space, so
            // the placements are clustered by bbox adjacency and each cluster
            // composites once — into a cropped region on text-bearing pages,
            // whose text the compositor cannot draw and must never lose.
            constexpr size_t kVectorTextMinPaths = 50;
            // An OCR text layer drawn with Tr 3 (invisible) must not count as
            // page text: the page is visually a scan and composites like one.
            bool no_text = result.all_lines[p].size() <= 2 ||
                           parse_result.visible_text_chars == 0;
            // Gradient strips are synthesized geometry, not evidence of
            // vectorized glyphs — a page with one shaded banner must not
            // count as a vector-text page.
            size_t drawn_paths = parse_result.paths.size() -
                                 static_cast<size_t>(parse_result.shading_paths);
            bool vector_text_page = no_text &&
                                    drawn_paths >= kVectorTextMinPaths;

            struct PlacementInfo {
                size_t idx;             // index into parse_result.images
                double x0, y0, x1, y1;  // device bbox, viewing coords
                int obj_num;            // resolved ref; -1 for name/inline
                bool masked;            // stencil/1-bit or /SMask//Mask-backed
                int px_w, px_h;         // source raster size, /Width x /Height
            };
            std::vector<PlacementInfo> infos;
            auto xobjects = doc.resolve(resources.get("XObject"));
            for (size_t i = 0; i < parse_result.images.size(); i++) {
                auto& ip = parse_result.images[i];
                PdfObj xobj_res;
                if (ip.inline_img) {
                    // GDI drivers emit inline strips; they cluster like any
                    // other placement.
                } else if (ip.xobj_ref >= 0) {
                    xobj_res = doc.get_obj(ip.xobj_ref);
                } else if (xobjects.is_dict() && !ip.xobj_name.empty()) {
                    xobj_res = doc.resolve(xobjects.get(ip.xobj_name));
                }
                const PdfObj& xobj = ip.inline_img ? *ip.inline_img : xobj_res;
                if (!xobj.is_stream()) continue;
                auto& st = xobj.get("Subtype");
                if (!ip.inline_img &&
                    (!st.is_name() || st.str_val != "Image")) continue;

                // Exact device bbox: the unit square's corners through the
                // CTM, correct for rotated placements too.
                const double* m = ip.ctm;
                double xs[4] = {m[4], m[4] + m[0], m[4] + m[2],
                                m[4] + m[0] + m[2]};
                double ys[4] = {m[5], m[5] + m[1], m[5] + m[3],
                                m[5] + m[1] + m[3]};
                double bx0 = std::min({xs[0], xs[1], xs[2], xs[3]});
                double bx1 = std::max({xs[0], xs[1], xs[2], xs[3]});
                double by0 = std::min({ys[0], ys[1], ys[2], ys[3]});
                double by1 = std::max({ys[0], ys[1], ys[2], ys[3]});
                if (!std::isfinite(bx0) || !std::isfinite(bx1) ||
                    !std::isfinite(by0) || !std::isfinite(by1))
                    continue;
                // The visual extent is the placement clipped by W/W*: a big
                // image behind a small window must cluster by the window.
                bx0 = std::max(bx0, static_cast<double>(ip.clip[0]));
                by0 = std::max(by0, static_cast<double>(ip.clip[1]));
                bx1 = std::min(bx1, static_cast<double>(ip.clip[2]));
                by1 = std::min(by1, static_cast<double>(ip.clip[3]));
                if (bx1 - bx0 < 0.1 || by1 - by0 < 0.1) continue;
                if (bx1 < 0 || bx0 > page_w || by1 < 0 || by0 > page_h)
                    continue;
                // Rasterized glyph strips commonly store a solid RGB image
                // plus a soft mask holding the actual glyphs; /Mask,
                // ImageMask and 1-bit layers carry the same role.
                bool masked = xobj.get("ImageMask").bool_val ||
                              xobj.get("BitsPerComponent").as_int() == 1 ||
                              !xobj.get("SMask").is_none() ||
                              !xobj.get("Mask").is_none();
                infos.push_back({i, bx0, by0, bx1, by1, ip.xobj_ref, masked,
                                 xobj.get("Width").as_int(),
                                 xobj.get("Height").as_int()});
            }

            const size_t n_inf = infos.size();
            DisjointSet image_sets(n_inf);

            // Print-driver strips abut within sub-point rounding; distinct
            // assets sit tens of points apart in real layouts.
            const double eps = std::max(2.0, 0.003 * std::max(page_w, page_h));
            if (n_inf > 2000) {
                // Thousands of placements on one page IS a shredded raster;
                // pairwise adjacency adds nothing at that count.
                for (size_t i = 1; i < n_inf; i++) image_sets.unite(0, i);
            } else {
                for (size_t a = 0; a < n_inf; a++) {
                    // Same object drawn again at the same spot (producer
                    // quirk): weld so it cannot form a phantom layered pair.
                    for (size_t b = a + 1; b < n_inf; b++) {
                        double gx = std::max(infos[a].x0, infos[b].x0) -
                                    std::min(infos[a].x1, infos[b].x1);
                        double gy = std::max(infos[a].y0, infos[b].y0) -
                                    std::min(infos[a].y1, infos[b].y1);
                        double eps_y = eps;
                        // Page-wide bands tolerate a bigger vertical gap
                        // (blank rows a driver skipped), gated on real x
                        // overlap so a nearby logo does not merge sideways.
                        double wa = infos[a].x1 - infos[a].x0;
                        double wb = infos[b].x1 - infos[b].x0;
                        if (std::min(wa, wb) >= 0.30 * page_w) {
                            double xov = std::min(infos[a].x1, infos[b].x1) -
                                         std::max(infos[a].x0, infos[b].x0);
                            if (xov >= 0.5 * std::min(wa, wb))
                                eps_y = std::max(eps, 0.02 * page_h);
                        }
                        if (gx <= eps && gy <= eps_y) {
                            image_sets.unite(a, b);
                        }
                    }
                }
            }

            struct Cluster {
                std::vector<size_t> members; // indices into infos
                double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
                double area_sum = 0;
            };
            std::vector<Cluster> clusters;
            {
                std::unordered_map<size_t, size_t> root_to_cluster;
                for (size_t i = 0; i < n_inf; i++) {
                    size_t r = image_sets.find(i);
                    auto [it, fresh] =
                        root_to_cluster.try_emplace(r, clusters.size());
                    if (fresh) clusters.emplace_back();
                    auto& c = clusters[it->second];
                    c.members.push_back(i);
                    c.x0 = std::min(c.x0, infos[i].x0);
                    c.y0 = std::min(c.y0, infos[i].y0);
                    c.x1 = std::max(c.x1, infos[i].x1);
                    c.y1 = std::max(c.y1, infos[i].y1);
                    c.area_sum += (infos[i].x1 - infos[i].x0) *
                                  (infos[i].y1 - infos[i].y0);
                }
            }

            auto qualifies = [&](const Cluster& c) {
                size_t n = c.members.size();
                if (n < 2) return false;
                double ua = (c.x1 - c.x0) * (c.y1 - c.y0);
                if (ua <= 0) return false;
                // Three or more images butted to sub-3pt gaps are fragments;
                // the coverage floor rejects sparse corner-to-corner chains.
                if (n >= 3) return c.area_sum / ua >= 0.4;
                const auto& A = infos[c.members[0]];
                const auto& B = infos[c.members[1]];
                double xov = std::min(A.x1, B.x1) - std::max(A.x0, B.x0);
                double yov = std::min(A.y1, B.y1) - std::max(A.y0, B.y0);
                double area_a = (A.x1 - A.x0) * (A.y1 - A.y0);
                double area_b = (B.x1 - B.x0) * (B.y1 - B.y0);
                // Layered pair: a stencil stamped over its base image. A
                // photo here and a signature there stay separate assets.
                if (xov > 0 && yov > 0 &&
                    xov * yov >= 0.3 * std::min(area_a, area_b))
                    return true;
                // Stacked pair: two halves of one raster, butted along one
                // axis with the perpendicular extents mostly aligned.
                if (-yov <= eps &&
                    xov >= 0.7 * std::min(A.x1 - A.x0, B.x1 - B.x0))
                    return true;
                if (-xov <= eps &&
                    yov >= 0.7 * std::min(A.y1 - A.y0, B.y1 - B.y0))
                    return true;
                return false;
            };

            // ── Fragment classification ─────────────────────────
            // A fragment is a drawing primitive that only makes sense
            // composited: a member of an abutting/overlapping cluster
            // (strips, tiles, layered stamp pairs), or a thin glyph strip
            // (mask-backed on any page; bare rasters count once the page has
            // no visible text). Any two fragments turn the page into one
            // whole-page composite; standalone images (photos, logos,
            // scans) always export as original assets besides it.
            std::vector<char> is_fragment(parse_result.images.size(), 0);
            size_t fragment_count = 0;
            for (auto& c : clusters) {
                if (!qualifies(c)) continue;
                for (size_t mi : c.members)
                    if (!is_fragment[infos[mi].idx]) {
                        is_fragment[infos[mi].idx] = 1;
                        fragment_count++;
                    }
            }
            for (auto& info : infos) {
                if (is_fragment[info.idx]) continue;
                double w = info.x1 - info.x0, h = info.y1 - info.y0;
                bool thin = h <= 0.04 * page_h && w >= 2.0 * h;
                if (thin && (info.masked || no_text)) {
                    is_fragment[info.idx] = 1;
                    fragment_count++;
                }
            }
            bool fragment_page = fragment_count >= 2;

            bool composited = false;
            if (vector_text_page || fragment_page) {
                // The composite draws every placement, standalone images
                // included; exporting those separately would store the same
                // content twice, so a composited page emits exactly one file.
                auto rendered = render_composite();
                if (!rendered.data.empty() || !rendered.pixels.empty() || !rendered.saved_path.empty()) {
                    result.all_images[p].push_back(std::move(rendered));
                    result.all_image_y[p].push_back(page_h);
                    result.all_image_x[p].push_back(0);
                    composited = true;
                }
            }
            if (!composited) {
                // The diag records the attempt that produced the page's final
                // images; a failed composite re-attempts everything below, so
                // its counts must not double up with the passes here.
                result.page_diags[p] = {};
                int img_idx = 0;
                std::vector<char> handled(parse_result.images.size(), 0);
                std::vector<std::array<double, 4>> done_regions;

                // ── Vector figures ──────────────────────────────
                // A schematic or chart drawn as paths (often with a few small
                // glyph images sprinkled in) has no representation at all
                // outside a whole-page composite. Cluster author-drawn path
                // bboxes the same way and composite dense regions that are
                // not tables and hold no body text.
                constexpr size_t kFigureMinPaths = 10;
                constexpr size_t kFigureMaxPaths = 1500;
                const size_t author_paths =
                    parse_result.paths.size() -
                    static_cast<size_t>(parse_result.shading_paths);
                if (author_paths >= kFigureMinPaths &&
                    author_paths <= kFigureMaxPaths) {
                    struct PathBox {
                        double x0, y0, x1, y1;
                        bool dark;
                        // Ruling is a hairline: one bbox dimension is a
                        // stroke wide and the other much longer. A drawn
                        // shape has two real dimensions.
                        bool rule;
                        // No curve and no diagonal segment. Rules, boxes
                        // and cell shading are axis-aligned; charts and
                        // schematics are not.
                        bool ortho;
                        // Index into parse_result.paths, for the coordinate
                        // rank computed per accepted cluster below.
                        size_t src;
                    };
                    std::vector<PathBox> pb;
                    pb.reserve(author_paths);
                    size_t rp_idx = static_cast<size_t>(-1);
                    for (auto& rp : parse_result.paths) {
                        rp_idx++;
                        if (rp.synthetic) continue;
                        // Real figures carry ink; a cluster of nothing but
                        // pastel fills is a decorated text background.
                        auto lum = [](double r, double g, double b) {
                            return 0.299 * r + 0.587 * g + 0.114 * b;
                        };
                        bool dark =
                            (rp.do_stroke &&
                             lum(rp.stroke_r, rp.stroke_g, rp.stroke_b) <
                                 0.7) ||
                            (rp.do_fill &&
                             lum(rp.fill_r, rp.fill_g, rp.fill_b) < 0.7);
                        double bx0 = 1e300, by0 = 1e300;
                        double bx1 = -1e300, by1 = -1e300;
                        for (auto& pt : rp.points) {
                            if (pt.type == PathPoint::CLOSE) continue;
                            bx0 = std::min(bx0, pt.x);
                            bx1 = std::max(bx1, pt.x);
                            by0 = std::min(by0, pt.y);
                            by1 = std::max(by1, pt.y);
                            if (pt.type == PathPoint::CURVE) {
                                bx0 = std::min({bx0, pt.cx1, pt.cx2});
                                bx1 = std::max({bx1, pt.cx1, pt.cx2});
                                by0 = std::min({by0, pt.cy1, pt.cy2});
                                by1 = std::max({by1, pt.cy1, pt.cy2});
                            }
                        }
                        if (bx0 > bx1 || !std::isfinite(bx0) ||
                            !std::isfinite(bx1) || !std::isfinite(by0) ||
                            !std::isfinite(by1))
                            continue;
                        if (rp.do_stroke) {
                            double pad = rp.line_width * 0.5;
                            bx0 -= pad; by0 -= pad;
                            bx1 += pad; by1 += pad;
                        }
                        bx0 = std::max(bx0, static_cast<double>(rp.clip[0]));
                        by0 = std::max(by0, static_cast<double>(rp.clip[1]));
                        bx1 = std::min(bx1, static_cast<double>(rp.clip[2]));
                        by1 = std::min(by1, static_cast<double>(rp.clip[3]));
                        double w = bx1 - bx0, h = by1 - by0;
                        if (w < 0.5 && h < 0.5) continue;
                        if (bx1 < 0 || bx0 > page_w || by1 < 0 || by0 > page_h)
                            continue;
                        // Page borders/backgrounds join everything and mean
                        // nothing; long rules are separators, not figures.
                        if (w > 0.7 * page_w && h > 0.7 * page_h) continue;
                        double longd = std::max(w, h);
                        double shortd = std::max(std::min(w, h), 0.04);
                        if (longd > 25.0 * shortd &&
                            longd > 0.3 * std::max(page_w, page_h))
                            continue;
                        bool has_curve = false, ortho = true;
                        {
                            double px = 0, py = 0, sx = 0, sy = 0;
                            bool have = false;
                            for (auto& pt : rp.points) {
                                if (pt.type == PathPoint::CURVE) {
                                    has_curve = true;
                                    ortho = false;
                                    break;
                                }
                                if (pt.type == PathPoint::CLOSE) {
                                    if (have &&
                                        std::abs(sx - px) > 0.05 &&
                                        std::abs(sy - py) > 0.05)
                                        ortho = false;
                                    continue;
                                }
                                if (pt.type == PathPoint::MOVE) {
                                    sx = px = pt.x;
                                    sy = py = pt.y;
                                    have = true;
                                    continue;
                                }
                                if (have && std::abs(pt.x - px) > 0.05 &&
                                    std::abs(pt.y - py) > 0.05)
                                    ortho = false;
                                px = pt.x;
                                py = pt.y;
                            }
                        }
                        double mind = std::min(w, h);
                        bool rule = !has_curve && mind <= 2.0 &&
                                    std::max(w, h) >=
                                        4.0 * std::max(mind, 0.05);
                        pb.push_back(
                            {bx0, by0, bx1, by1, dark, rule, ortho, rp_idx});
                    }

                    const size_t np = pb.size();
                    DisjointSet figure_sets(np);
                    for (size_t a = 0; a < np; a++)
                        for (size_t b = a + 1; b < np; b++) {
                            double gx = std::max(pb[a].x0, pb[b].x0) -
                                        std::min(pb[a].x1, pb[b].x1);
                            double gy = std::max(pb[a].y0, pb[b].y0) -
                                        std::min(pb[a].y1, pb[b].y1);
                            if (gx <= eps && gy <= eps) {
                                figure_sets.unite(a, b);
                            }
                        }

                    struct FigCluster {
                        size_t n = 0, dark = 0, rules = 0, orthos = 0;
                        double x0 = 1e300, y0 = 1e300;
                        double x1 = -1e300, y1 = -1e300;
                        std::vector<size_t> members; // indices into pb
                    };
                    std::vector<FigCluster> figs;
                    {
                        std::unordered_map<size_t, size_t> root_to_fig;
                        for (size_t i = 0; i < np; i++) {
                            auto [it, fresh] = root_to_fig.try_emplace(
                                figure_sets.find(i), figs.size());
                            if (fresh) figs.emplace_back();
                            auto& fc = figs[it->second];
                            fc.n++;
                            if (pb[i].dark) fc.dark++;
                            if (pb[i].rule) fc.rules++;
                            if (pb[i].ortho) fc.orthos++;
                            fc.x0 = std::min(fc.x0, pb[i].x0);
                            fc.y0 = std::min(fc.y0, pb[i].y0);
                            fc.x1 = std::max(fc.x1, pb[i].x1);
                            fc.y1 = std::max(fc.y1, pb[i].y1);
                            fc.members.push_back(i);
                        }
                    }
                    // Two clusters can interleave — union boxes overlapping
                    // while no member pair touches (a wire bus crossing a
                    // component row). Merge until the boxes are disjoint.
                    for (bool merged = true; merged;) {
                        merged = false;
                        for (size_t a = 0; a < figs.size() && !merged; a++)
                            for (size_t b = a + 1; b < figs.size(); b++) {
                                if (std::max(figs[a].x0, figs[b].x0) -
                                        std::min(figs[a].x1, figs[b].x1) >
                                    eps)
                                    continue;
                                if (std::max(figs[a].y0, figs[b].y0) -
                                        std::min(figs[a].y1, figs[b].y1) >
                                    eps)
                                    continue;
                                figs[a].n += figs[b].n;
                                figs[a].dark += figs[b].dark;
                                figs[a].rules += figs[b].rules;
                                figs[a].orthos += figs[b].orthos;
                                figs[a].x0 = std::min(figs[a].x0, figs[b].x0);
                                figs[a].y0 = std::min(figs[a].y0, figs[b].y0);
                                figs[a].x1 = std::max(figs[a].x1, figs[b].x1);
                                figs[a].y1 = std::max(figs[a].y1, figs[b].y1);
                                figs[a].members.insert(
                                    figs[a].members.end(),
                                    figs[b].members.begin(),
                                    figs[b].members.end());
                                figs.erase(figs.begin() + b);
                                merged = true;
                                break;
                            }
                    }

                    // Coordinate rank: the number of distinct vertex
                    // positions along each axis, quantized to 2pt. Data
                    // spreads vertices across both axes; a band is one
                    // rectangle (rank 2 each way) and a ruled grid starts
                    // and ends every rule on the same few x positions.
                    auto coord_rank = [&](const FigCluster& fc, int& rx,
                                          int& ry) {
                        std::unordered_set<long> xs, ys;
                        for (size_t m : fc.members) {
                            auto& rp = parse_result.paths[pb[m].src];
                            for (auto& pt : rp.points) {
                                if (pt.type == PathPoint::CLOSE) continue;
                                xs.insert(std::lround(pt.x / 2.0));
                                ys.insert(std::lround(pt.y / 2.0));
                            }
                        }
                        rx = static_cast<int>(xs.size());
                        ry = static_cast<int>(ys.size());
                    };
                    auto figlog = [&](const FigCluster& fc,
                                      const char* verdict) {
                        if (!fig_debug) return;
                        int rx = 0, ry = 0;
                        coord_rank(fc, rx, ry);
                        fprintf(stderr,
                                "[figdbg] p=%d cand n=%zu dark=%zu rules=%zu"
                                " orthos=%zu bbox=(%.1f,%.1f)-(%.1f,%.1f)"
                                " rank=%d/%d %s\n",
                                p + 1, fc.n, fc.dark, fc.rules, fc.orthos,
                                fc.x0, fc.y0, fc.x1, fc.y1, rx, ry, verdict);
                    };
                    for (auto& fc : figs) {
                        if (fc.n < kFigureMinPaths) {
                            if (fc.n >= 4) figlog(fc, "reject:minpaths");
                            continue;
                        }
                        if (fc.dark < 2) {
                            figlog(fc, "reject:dark");
                            continue;
                        }
                        // Nothing but hairlines is ruling, not a drawing:
                        // a table's rules, a separator band, a box around
                        // text. Rasterising it yields an empty grid, and
                        // the words it frames are already in the markdown.
                        if (fc.rules == fc.n) {
                            figlog(fc, "reject:all-rules");
                            continue;
                        }
                        double rgn[4] = {std::max(0.0, fc.x0),
                                         std::max(0.0, fc.y0),
                                         std::min(page_w, fc.x1),
                                         std::min(page_h, fc.y1)};
                        double fw = rgn[2] - rgn[0], fh = rgn[3] - rgn[1];
                        if (fw < 1.0 || fh < 1.0) {
                            figlog(fc, "reject:degenerate");
                            continue;
                        }
                        double farea = fw * fh;
                        double parea = page_w * page_h;
                        if (farea < 0.006 * parea || farea > 0.65 * parea) {
                            figlog(fc, "reject:area");
                            continue;
                        }
                        // Bands and decorative strips: one long rectangle
                        // (plus its edge hairlines) collapses to a handful
                        // of distinct vertex positions on one axis, and is
                        // far wider than tall. Either signal alone kills
                        // real content — sparse box diagrams sit at rank 3
                        // (aspect <= 5.2 measured) and heatmap grids reach
                        // rank 11 — so both must agree. Measured margins:
                        // decorations rank <= 6 / aspect >= 12.6; figures
                        // rank >= 11 or aspect <= 5.2.
                        {
                            double aspect =
                                std::max(fw, fh) / std::max(std::min(fw, fh),
                                                            1.0);
                            if (aspect >= 6.0) {
                                int rx = 0, ry = 0;
                                coord_rank(fc, rx, ry);
                                if (std::min(rx, ry) <= 8) {
                                    figlog(fc, "reject:band");
                                    continue;
                                }
                            }
                        }
                        auto overlap = [&](double x0, double y0, double x1,
                                           double y1) {
                            double ox = std::min(rgn[2], x1) -
                                        std::max(rgn[0], x0);
                            double oy = std::min(rgn[3], y1) -
                                        std::max(rgn[1], y0);
                            return ox > 0 && oy > 0 ? ox * oy : 0.0;
                        };
                        // Ruled/shaded tables cluster densely too, and text
                        // blocks carry decorations; both must stay text.
                        int veto_code = 0;
                        // A booktabs table is found by alignment, not by
                        // its geometry, yet it does own the rules it draws.
                        // Ruling-only geometry under a text table is that
                        // table; anything carrying a curve or a diagonal is
                        // a drawing the detector merely straddled.
                        bool ruling_only = fc.orthos == fc.n &&
                                           fc.rules * 10 >= fc.n * 7;
                        for (auto& t : result.all_tables[p]) {
                            // Text-detected tables usually own no drawn
                            // geometry; paths under them are a figure unless
                            // they are the table's own ruling.
                            if (t.kind == TableData::TEXT && !ruling_only)
                                continue;
                            if (overlap(t.x0, t.y0, t.x1, t.y1) >
                                0.3 * farea) {
                                veto_code = 1;
                                break;
                            }
                        }
                        if (!veto_code) {
                            // Body text vetoes; a figure's own short labels
                            // (pin names, axis ticks) do not — the region is
                            // still a drawing, and the label characters stay
                            // in the markdown text either way.
                            int body_lines = 0;
                            for (auto& ln : result.all_lines[p]) {
                                if (ln.y_center < rgn[1] ||
                                    ln.y_center > rgn[3])
                                    continue;
                                double lw = ln.x_right - ln.x_left;
                                if (lw <= std::max(0.5 * fw, 60.0)) continue;
                                // Sparse label rows (pin names merged onto
                                // one baseline) span width with few glyphs;
                                // body lines carry real character mass.
                                if (ln.text.size() < 30) continue;
                                // A line the region merely crosses belongs
                                // to a page-wide row, not to this box: chart
                                // panels standing side by side share their
                                // baselines, so the axis ticks and legends of
                                // every panel merge into one line spanning
                                // them all. Only a line the region actually
                                // contains is text the box swallowed.
                                double inside =
                                    std::min(static_cast<double>(ln.x_right),
                                             rgn[2]) -
                                    std::max(static_cast<double>(ln.x_left),
                                             rgn[0]);
                                if (inside >= 0.7 * lw) body_lines++;
                            }
                            if (body_lines >= 2) veto_code = 2;
                        }
                        bool veto = veto_code != 0;
                        if (!veto)
                            for (auto& dr : done_regions)
                                if (overlap(dr[0], dr[1], dr[2], dr[3]) >
                                    0.3 * farea) {
                                    veto = true;
                                    veto_code = 3;
                                    break;
                                }
                        if (veto) {
                            figlog(fc, veto_code == 1   ? "reject:table"
                                       : veto_code == 2 ? "reject:bodytext"
                                                        : "reject:overlap");
                            continue;
                        }
                        figlog(fc, "ACCEPT");

                        // Glyph images inside the figure render with it.
                        std::vector<size_t> members;
                        for (auto& info : infos)
                            if (!handled[info.idx] &&
                                overlap(info.x0, info.y0, info.x1, info.y1) >
                                    0)
                                members.push_back(info.idx);
                        std::sort(members.begin(), members.end());
                        ImageData rendered;
                        {
                            CompositeMemoryLease lease(
                                composite_memory,
                                composite_memory_cost(fw, fh));
                            rendered = render_region_composite(
                                doc, resources, parse_result, members, p,
                                rgn, image_dir, img_idx,
                                &result.page_diags[p]);
                        }
                        if (rendered.data.empty() &&
                            rendered.pixels.empty() &&
                            rendered.saved_path.empty())
                            continue;
                        for (size_t mi : members) handled[mi] = 1;
                        result.all_images[p].push_back(std::move(rendered));
                        result.all_image_y[p].push_back(rgn[3]);
                        result.all_image_x[p].push_back(rgn[0]);
                        done_regions.push_back(
                            {rgn[0], rgn[1], rgn[2], rgn[3]});
                        img_idx++;
                    }
                }

                std::vector<size_t> remaining;
                for (size_t i = 0; i < parse_result.images.size(); i++)
                    if (!handled[i]) remaining.push_back(i);
                // Standalone rasters that are page furniture, not figures.
                //  - Inline icons: a placement under 24pt on both axes is a
                //    bullet or stamp inside a text line (measured: icons
                //    <= 17.7pt; the smallest genuine standalone figure
                //    placement is >= 64pt) — unless the page is tiled with
                //    them, which is how a figure built from many small
                //    rasters looks. See the tile_peers count below.
                //  - Trim bleed: decoration is deliberately drawn past the
                //    trim edge so the guillotine cannot leave a white sliver;
                //    a figure never risks its own data. Crossing the CropBox
                //    by more than 2pt on two or more sides marks a cover or
                //    chapter banner (measured: 15/16 IMF WEO banners, 0/50
                //    genuine placements in the HWP thesis).
                if (!remaining.empty() && !infos.empty()) {
                // A figure can be tiled out of many small rasters — an
                // attention-map grid, a sheet of glyph samples — and each
                // tile is placed as small as a bullet. What separates them is
                // not the placement but how many siblings share the page:
                // measured, decoration comes at most 4 to a page while a
                // tiled figure starts at 8. Fragments shredded by a print
                // driver also come in the hundreds, so only siblings with
                // real pixels behind them count (their raster is 2x1 or so,
                // against 107px and up for a genuine tile).
                const int kTilePixels = 100;   // short side of the raster
                size_t tile_peers = 0;
                for (auto& o : infos)
                    if (std::max(o.x1 - o.x0, o.y1 - o.y0) < 24.0 &&
                        std::min(o.px_w, o.px_h) >= kTilePixels)
                        tile_peers++;
                    std::vector<size_t> kept;
                    kept.reserve(remaining.size());
                    for (size_t idx : remaining) {
                        const PlacementInfo* pi = nullptr;
                        for (auto& info : infos)
                            if (info.idx == idx) { pi = &info; break; }
                        if (pi) {
                            double w = pi->x1 - pi->x0, h = pi->y1 - pi->y0;
                            const bool tile =
                                tile_peers >= 6 &&
                                std::min(pi->px_w, pi->px_h) >= kTilePixels;
                            if (std::max(w, h) < 24.0 && !tile) {
                                if (fig_debug)
                                    fprintf(stderr,
                                            "[figdbg] p=%d raster skip:icon"
                                            " (%.1fx%.1fpt px=%dx%d"
                                            " tiles=%zu)\n",
                                            p + 1, w, h, pi->px_w,
                                            pi->px_h, tile_peers);
                                continue;
                            }
                            // Bleed is judged on the raw placement, not the
                            // clipped window: a bleeding banner is usually
                            // clipped to the page box, which erases the very
                            // evidence looked for here.
                            const double* m = parse_result.images[idx].ctm;
                            double rxs[4] = {m[4], m[4] + m[0], m[4] + m[2],
                                             m[4] + m[0] + m[2]};
                            double rys[4] = {m[5], m[5] + m[1], m[5] + m[3],
                                             m[5] + m[1] + m[3]};
                            double rx0 = std::min({rxs[0], rxs[1], rxs[2],
                                                   rxs[3]});
                            double rx1 = std::max({rxs[0], rxs[1], rxs[2],
                                                   rxs[3]});
                            double ry0 = std::min({rys[0], rys[1], rys[2],
                                                   rys[3]});
                            double ry1 = std::max({rys[0], rys[1], rys[2],
                                                   rys[3]});
                            int out_sides = (rx0 < crop_x0 - 2.0) +
                                            (ry0 < crop_y0 - 2.0) +
                                            (rx1 > crop_x1 + 2.0) +
                                            (ry1 > crop_y1 + 2.0);
                            if (out_sides >= 2) {
                                if (fig_debug)
                                    fprintf(stderr,
                                            "[figdbg] p=%d raster skip:bleed"
                                            " (%d sides)\n",
                                            p + 1, out_sides);
                                continue;
                            }
                        }
                        kept.push_back(idx);
                    }
                    remaining.swap(kept);
                }
                if (!remaining.empty()) {
                    auto extracted = extract_page_images(
                        doc, resources, parse_result, p, image_dir,
                        opts.min_image_size, &result.page_diags[p],
                        &remaining, img_idx);
                    for (auto& ei : extracted) {
                        if (fig_debug) {
                            double ix0 = std::min(ei.ctm[4],
                                                  ei.ctm[0] + ei.ctm[4]);
                            double ix1 = std::max(ei.ctm[4],
                                                  ei.ctm[0] + ei.ctm[4]);
                            double iy0 = std::min(ei.ctm[5],
                                                  ei.ctm[3] + ei.ctm[5]);
                            double iy1 = std::max(ei.ctm[5],
                                                  ei.ctm[3] + ei.ctm[5]);
                            fprintf(stderr,
                                    "[figdbg] p=%d raster bbox=(%.1f,%.1f)-"
                                    "(%.1f,%.1f) crop=(%.1f,%.1f)-(%.1f,%.1f)"
                                    " %dx%d\n",
                                    p + 1, ix0, iy0, ix1, iy1, crop_x0,
                                    crop_y0, crop_x1, crop_y1,
                                    static_cast<int>(ei.img.width),
                                    static_cast<int>(ei.img.height));
                        }
                        // ctm[5] is the Y translation in PDF coordinates (origin bottom-left)
                        // ctm[3] is vertical scale; y_top = ctm[5] + abs(ctm[3])
                        double y_top = ei.ctm[5] + std::abs(ei.ctm[3]);
                        result.all_image_y[p].push_back(y_top);
                        result.all_image_x[p].push_back(ei.ctm[4]); // X position
                        result.all_images[p].push_back(std::move(ei.img));
                    }
                }

                // Fallback: render page for scanned/vector-only pages.
                // Paths count too: gradient strips and curved vector art
                // never produce segments, and such a page has no other
                // representation than a composite.
                if (result.all_images[p].empty() && result.all_lines[p].empty()) {
                    if (!parse_result.images.empty() ||
                        !parse_result.segments.empty() ||
                        !parse_result.paths.empty()) {
                        result.page_diags[p] = {};
                        auto rendered = render_composite();
                        if (!rendered.data.empty() || !rendered.pixels.empty() || !rendered.saved_path.empty()) {
                            result.all_images[p].push_back(std::move(rendered));
                            result.all_image_y[p].push_back(page_h);
                            result.all_image_x[p].push_back(0);
                        }
                    }
                }
            }

            // The string-only API emits image references but cannot expose the
            // encoded bytes. Drop them per page instead of retaining an entire
            // scanned document until Markdown assembly finishes. Chunk APIs
            // keep ownership unless the image was successfully written out.
            for (auto& img : result.all_images[p]) {
                if (payload_policy == ImagePayloadPolicy::MetadataOnly ||
                    (!image_dir.empty() && !img.saved_path.empty())) {
                    discard_image_payload(img);
                }
            }
        }

        // Set after the image pipeline: the per-attempt diag resets above must
        // not wipe the parse-time counters.
        result.page_diags[p].inline_images = parse_result.inline_images;
        result.page_diags[p].inline_scan_bailouts =
            parse_result.inline_scan_bailouts;
        result.page_diags[p].shading_unsupported =
            parse_result.shading_unsupported;
    };

    // Each worker owns one pre-sized result slot. Shared object/font caches
    // synchronize internally, preserving deterministic output order.
    if (n_workers <= 1) {
        for (int p : page_indices) process_page(p);
    } else {
        std::atomic<size_t> next_page{0};
        std::atomic<bool> failed{false};
        std::mutex err_mu;
        std::exception_ptr first_error;
        std::vector<std::thread> workers;
        workers.reserve(n_workers);
        try {
            for (size_t t = 0; t < n_workers; t++) {
                workers.emplace_back([&]() {
                    size_t i;
                    // Match the sequential loop's abort semantics: once a page
                    // throws, no worker starts another page (in-flight pages
                    // finish), instead of rendering the whole rest of the
                    // document for a result the rethrow below discards.
                    while (!failed.load(std::memory_order_relaxed) &&
                           (i = next_page.fetch_add(1)) < page_indices.size()) {
                        try {
                            process_page(page_indices[i]);
                        } catch (...) {
                            failed.store(true, std::memory_order_relaxed);
                            std::lock_guard<std::mutex> lock(err_mu);
                            if (!first_error)
                                first_error = std::current_exception();
                        }
                    }
                });
            }
        } catch (...) {
            failed.store(true, std::memory_order_relaxed);
            for (auto& w : workers) w.join();
            throw;
        }
        for (auto& w : workers) w.join();
        if (first_error) std::rethrow_exception(first_error);
    }

    collect_attachments(doc, root, result.attachments);

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

static ExtractResult extract_pdf(const std::string& pdf_path,
                                 const ConvertOptions& opts,
                                 ImagePayloadPolicy payload_policy) {
    // Map the file read-only: the parser treats its input as const (the
    // convert_bytes path already feeds it read-only buffers), so mapping avoids
    // a whole-file heap allocation and the read() copy. Falls back to a heap
    // read when mapping is unavailable.
    MappedFile mf(pdf_path);
    if (!mf.valid()) throw std::runtime_error("Cannot open PDF: " + pdf_path);
    if (mf.size() == 0) throw std::runtime_error("Empty PDF file: " + pdf_path);

    return extract_pdf_buffer(mf.data(), mf.size(), pdf_path, opts,
                              payload_policy);
}


}} // namespace jdoc::pdf_detail

namespace jdoc {
using namespace pdf_detail;

std::string pdf_to_markdown(const std::string& pdf_path, ConvertOptions opts) {
    auto r = extract_pdf(pdf_path, opts, ImagePayloadPolicy::MetadataOnly);
    return result_to_markdown(r, opts);
}

std::vector<PageChunk> pdf_to_markdown_chunks(const std::string& pdf_path,
                                              ConvertOptions opts) {
    auto r = extract_pdf(pdf_path, opts, ImagePayloadPolicy::Retain);
    return result_to_chunks(r, opts);
}

std::string pdf_to_markdown_mem(const uint8_t* data, size_t size,
                                ConvertOptions opts) {
    auto r = extract_pdf_buffer(data, size, "<memory>", opts,
                                ImagePayloadPolicy::MetadataOnly);
    return result_to_markdown(r, opts);
}

std::vector<PageChunk> pdf_to_markdown_chunks_mem(const uint8_t* data, size_t size,
                                                  ConvertOptions opts) {
    auto r = extract_pdf_buffer(data, size, "<memory>", opts,
                                ImagePayloadPolicy::Retain);
    return result_to_chunks(r, opts);
}

void pdf_to_markdown_chunks_stream(const std::string& pdf_path,
                                   const ConvertOptions& opts, const PageSink& sink) {
    // Shared document setup (xref, fonts, page tree) is parsed once inside
    // extract_pdf; the per-page emit loop then streams, releasing each page's
    // buffers as the consumer advances (stream_result_chunks).
    //
    // NOTE — PDF is a deliberate exception to producer-side peak-memory
    // reduction. Markdown heading detection needs the document-wide modal body
    // font size, which is only known after every page's text lines are parsed
    // (FontStats::compute over all_lines). Deferring image decode to a second
    // pass to shrink producer peak would force either a content re-parse
    // (throughput regression on text-heavy PDFs) or retaining every page's
    // parse result (memory regression on vector-heavy PDFs) — both violate the
    // "no regression" requirement. So the streaming win here is consumer-side:
    // the sink owns and frees one page at a time. Producer peak is bounded by
    // image_dir (its existing per-page disk flush) when set. Output is
    // byte-identical to pdf_to_markdown_chunks.
    auto r = extract_pdf(pdf_path, opts, ImagePayloadPolicy::Retain);
    stream_result_chunks(r, opts, sink);
}

void pdf_to_markdown_chunks_mem_stream(const uint8_t* data, size_t size,
                                       const ConvertOptions& opts, const PageSink& sink) {
    auto r = extract_pdf_buffer(data, size, "<memory>", opts,
                                ImagePayloadPolicy::Retain);
    stream_result_chunks(r, opts, sink);
}
} // namespace jdoc
