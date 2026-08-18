// pdf.cpp — facade: orchestration + public API (PDF -> Markdown, no PDFium).
#include "pdf_extract.h"
#include "pdf_limits.h"
#include "common/cpu_budget.h"
#include "common/string_utils.h"
#include "common/file_utils.h"
#include "common/mapped_file.h"
#include <fstream>
#include <algorithm>
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

        // Page geometry: MediaBox and /Rotate are inheritable, so climb the
        // page tree for both (CAD exports often keep them on the Pages node).
        double page_w = 612, page_h = 792; // default letter
        double mb_llx = 0, mb_lly = 0;
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
            bool vector_text_page = no_text &&
                                    parse_result.paths.size() >= kVectorTextMinPaths;

            struct PlacementInfo {
                size_t idx;             // index into parse_result.images
                double x0, y0, x1, y1;  // device bbox, viewing coords
                int obj_num;            // resolved ref; -1 for name/inline
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
                if (bx1 - bx0 < 0.1 || by1 - by0 < 0.1) continue;
                if (bx1 < 0 || bx0 > page_w || by1 < 0 || by0 > page_h)
                    continue;
                infos.push_back({i, bx0, by0, bx1, by1, ip.xobj_ref});
            }

            const size_t n_inf = infos.size();
            std::vector<size_t> parent(n_inf);
            for (size_t i = 0; i < n_inf; i++) parent[i] = i;
            auto find = [&](size_t a) {
                while (parent[a] != a) {
                    parent[a] = parent[parent[a]];
                    a = parent[a];
                }
                return a;
            };

            // Print-driver strips abut within sub-point rounding; distinct
            // assets sit tens of points apart in real layouts.
            const double eps = std::max(2.0, 0.003 * std::max(page_w, page_h));
            if (n_inf > 2000) {
                // Thousands of placements on one page IS a shredded raster;
                // pairwise adjacency adds nothing at that count.
                for (size_t i = 1; i < n_inf; i++) parent[i] = 0;
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
                            size_t ra = find(a), rb = find(b);
                            if (ra != rb) parent[rb] = ra;
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
                    size_t r = find(i);
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

            double frag_area = 0;
            for (auto& c : clusters) {
                if (!qualifies(c)) continue;
                double cx0 = std::max(0.0, c.x0), cy0 = std::max(0.0, c.y0);
                double cx1 = std::min(page_w, c.x1);
                double cy1 = std::min(page_h, c.y1);
                if (cx1 > cx0 && cy1 > cy0)
                    frag_area += (cx1 - cx0) * (cy1 - cy0);
            }
            bool shredded_page =
                no_text && frag_area >= 0.5 * page_w * page_h;

            bool composited = false;
            if (vector_text_page || shredded_page) {
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
                for (auto& c : clusters) {
                    if (!qualifies(c)) continue;
                    double rgn[4] = {std::max(0.0, c.x0), std::max(0.0, c.y0),
                                     std::min(page_w, c.x1),
                                     std::min(page_h, c.y1)};
                    if (rgn[2] - rgn[0] < 1.0 || rgn[3] - rgn[1] < 1.0)
                        continue;
                    std::vector<size_t> members;
                    members.reserve(c.members.size());
                    for (size_t mi : c.members)
                        members.push_back(infos[mi].idx);
                    std::sort(members.begin(), members.end());
                    ImageData rendered;
                    {
                        CompositeMemoryLease lease(
                            composite_memory,
                            composite_memory_cost(rgn[2] - rgn[0],
                                                  rgn[3] - rgn[1]));
                        rendered = render_region_composite(
                            doc, resources, parse_result, members, p, rgn,
                            image_dir, img_idx, &result.page_diags[p]);
                    }
                    if (rendered.data.empty() && rendered.pixels.empty() &&
                        rendered.saved_path.empty())
                        continue; // members fall back to individual export
                    for (size_t mi : members) handled[mi] = 1;
                    result.all_images[p].push_back(std::move(rendered));
                    // Cluster top/left keeps the markdown insert in reading
                    // order; only whole-page composites pin to top-of-page.
                    result.all_image_y[p].push_back(rgn[3]);
                    result.all_image_x[p].push_back(rgn[0]);
                    img_idx++;
                }

                std::vector<size_t> remaining;
                for (size_t i = 0; i < parse_result.images.size(); i++)
                    if (!handled[i]) remaining.push_back(i);
                if (!remaining.empty()) {
                    auto extracted = extract_page_images(
                        doc, resources, parse_result, p, image_dir,
                        opts.min_image_size, &result.page_diags[p],
                        &remaining, img_idx);
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
        // not wipe the parse-time inline counters.
        result.page_diags[p].inline_images = parse_result.inline_images;
        result.page_diags[p].inline_scan_bailouts =
            parse_result.inline_scan_bailouts;
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
