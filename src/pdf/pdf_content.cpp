#include "pdf_content.h"
#include "common/image_utils.h"
#include "common/string_utils.h"
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

// Content-stream operators are one to three bytes. Packing them once turns the
// long dispatch chain into comparisons of one integer; the compiler can then
// lower the chain to a jump table instead of repeating length checks and byte
// comparisons for every candidate operator.
class ContentOperator {
public:
    ContentOperator(const char* token, size_t length) {
        if (length == 0 || length > 3) return;
        code_ = static_cast<uint32_t>(length) << 24;
        code_ |= static_cast<uint32_t>(
            static_cast<unsigned char>(token[0])) << 16;
        if (length > 1)
            code_ |= static_cast<uint32_t>(
                static_cast<unsigned char>(token[1])) << 8;
        if (length > 2)
            code_ |= static_cast<uint32_t>(
                static_cast<unsigned char>(token[2]));
    }

    template <size_t N>
    bool is(const char (&literal)[N]) const {
        static_assert(N >= 2 && N <= 4,
                      "PDF operators are one to three bytes");
        uint32_t expected = static_cast<uint32_t>(N - 1) << 24;
        expected |= static_cast<uint32_t>(
            static_cast<unsigned char>(literal[0])) << 16;
        if constexpr (N > 2)
            expected |= static_cast<uint32_t>(
                static_cast<unsigned char>(literal[1])) << 8;
        if constexpr (N > 3)
            expected |= static_cast<uint32_t>(
                static_cast<unsigned char>(literal[2]));
        return code_ == expected;
    }

private:
    uint32_t code_ = 0;
};

// ── Colorspace resolution for cs/CS + sc/scn ─────────────
// sc/scn operands mean different things per selected colorspace: a Separation
// tint of 1.0 is solid ink (usually black), not white — misreading it as
// DeviceGray blanks text. Tint ramps and palettes are precomputed to RGB.

static void cmyk_to_rgb8(double c, double m, double y, double k,
                         std::array<uint8_t, 3>& out) {
    util::cmyk_to_rgb(c, m, y, k, out[0], out[1], out[2]);
}

// Evaluate a 1-in/N-out PDF function at t ∈ [0,1]. Supports sampled (type 0),
// exponential (type 2) and stitching (type 3); empty result = unsupported
// (e.g. type 4 PostScript calculator) — caller falls back to an ink ramp.
static std::vector<double> eval_function_1d(PdfDoc& doc, const PdfObj& fn_ref,
                                            double t, int depth = 0) {
    if (depth > 4) return {};
    auto fn = doc.resolve(fn_ref);
    if (!fn.is_dict() && !fn.is_stream()) return {};
    int type = fn.get("FunctionType").as_int();
    auto domain = doc.resolve(fn.get("Domain"));
    double d0 = 0, d1 = 1;
    if (domain.is_arr() && domain.arr.size() >= 2) {
        d0 = domain.arr[0].as_num();
        d1 = domain.arr[1].as_num();
    }
    if (t < d0) t = d0;
    if (t > d1) t = d1;

    if (type == 2) {
        auto c0 = doc.resolve(fn.get("C0"));
        auto c1 = doc.resolve(fn.get("C1"));
        double n = fn.get("N").as_num();
        if (n <= 0) n = 1;
        size_t outs = std::max(c0.is_arr() ? c0.arr.size() : 1,
                               c1.is_arr() ? c1.arr.size() : 1);
        std::vector<double> out(outs);
        double x = (d1 > d0) ? (t - d0) / (d1 - d0) : 0;
        double f = std::pow(x, n);
        for (size_t i = 0; i < outs; i++) {
            double a = (c0.is_arr() && i < c0.arr.size()) ? c0.arr[i].as_num() : 0.0;
            double b = (c1.is_arr() && i < c1.arr.size()) ? c1.arr[i].as_num() : 1.0;
            out[i] = a + f * (b - a);
        }
        return out;
    }

    if (type == 3) {
        auto funcs = doc.resolve(fn.get("Functions"));
        auto bounds = doc.resolve(fn.get("Bounds"));
        auto encode = doc.resolve(fn.get("Encode"));
        if (!funcs.is_arr() || funcs.arr.empty()) return {};
        size_t k = 0;
        double lo = d0, hi = d1;
        while (k < funcs.arr.size() - 1 && bounds.is_arr() && k < bounds.arr.size() &&
               t >= bounds.arr[k].as_num())
            k++;
        if (bounds.is_arr()) {
            lo = (k == 0) ? d0 : bounds.arr[k - 1].as_num();
            hi = (k < bounds.arr.size()) ? bounds.arr[k].as_num() : d1;
        }
        double e0 = 0, e1 = 1;
        if (encode.is_arr() && encode.arr.size() >= (k + 1) * 2) {
            e0 = encode.arr[k * 2].as_num();
            e1 = encode.arr[k * 2 + 1].as_num();
        }
        double x = (hi > lo) ? (t - lo) / (hi - lo) : 0;
        return eval_function_1d(doc, funcs.arr[k], e0 + x * (e1 - e0), depth + 1);
    }

    if (type == 0) {
        if (!fn.is_stream()) return {};
        auto range = doc.resolve(fn.get("Range"));
        auto size_arr = doc.resolve(fn.get("Size"));
        int bps = fn.get("BitsPerSample").as_int();
        if (!range.is_arr() || range.arr.size() < 2 || !size_arr.is_arr() ||
            size_arr.arr.empty() || (bps != 8 && bps != 16))
            return {};
        int outs = static_cast<int>(range.arr.size() / 2);
        int size0 = size_arr.arr[0].as_int();
        if (size0 <= 0 || outs <= 0) return {};
        auto samples = doc.decode_stream(fn);
        double e0 = 0, e1 = size0 - 1;
        auto encode = doc.resolve(fn.get("Encode"));
        if (encode.is_arr() && encode.arr.size() >= 2) {
            e0 = encode.arr[0].as_num();
            e1 = encode.arr[1].as_num();
        }
        double x = (d1 > d0) ? (t - d0) / (d1 - d0) : 0;
        long idx = std::lround(e0 + x * (e1 - e0));
        if (idx < 0) idx = 0;
        if (idx >= size0) idx = size0 - 1;
        std::vector<double> out(outs);
        for (int i = 0; i < outs; i++) {
            double r0 = range.arr[i * 2].as_num();
            double r1 = range.arr[i * 2 + 1].as_num();
            double v = 0;
            if (bps == 8) {
                size_t off = static_cast<size_t>(idx) * outs + i;
                if (off >= samples.size()) return {};
                v = samples[off] / 255.0;
            } else {
                size_t off = (static_cast<size_t>(idx) * outs + i) * 2;
                if (off + 1 >= samples.size()) return {};
                v = ((samples[off] << 8) | samples[off + 1]) / 65535.0;
            }
            out[i] = r0 + v * (r1 - r0);
        }
        return out;
    }

    return {};
}

// Map N alternate-space components to RGB (N=1 gray, 3 RGB, 4 CMYK).
static void alt_components_to_rgb(const std::vector<double>& comps,
                                  std::array<uint8_t, 3>& out) {
    auto b = [](double v) {
        return static_cast<uint8_t>(
            std::min(255.0, std::max(0.0, v * 255.0 + 0.5)));
    };
    if (comps.size() >= 4) cmyk_to_rgb8(comps[0], comps[1], comps[2], comps[3], out);
    else if (comps.size() == 3) out = {b(comps[0]), b(comps[1]), b(comps[2])};
    else if (comps.size() == 1) out = {b(comps[0]), b(comps[0]), b(comps[0])};
    else out = {0, 0, 0};
}

static int colorspace_component_count(PdfDoc& doc, const PdfObj& cs_resolved) {
    if (cs_resolved.is_name()) {
        const std::string& n = cs_resolved.str_val;
        if (n == "DeviceGray" || n == "CalGray" || n == "G") return 1;
        if (n == "DeviceCMYK" || n == "CMYK") return 4;
        return 3;
    }
    if (cs_resolved.is_arr() && !cs_resolved.arr.empty()) {
        auto head = doc.resolve(cs_resolved.arr[0]);
        if (head.is_name()) {
            if (head.str_val == "ICCBased" && cs_resolved.arr.size() >= 2) {
                int n = doc.resolve(cs_resolved.arr[1]).get("N").as_int();
                if (n == 1 || n == 3 || n == 4) return n;
            }
            if (head.str_val == "CalGray") return 1;
            if (head.str_val == "CalRGB" || head.str_val == "Lab") return 3;
        }
    }
    return 3;
}

static std::shared_ptr<const CsInfo> load_colorspace(PdfDoc& doc,
                                                     const PdfObj& cs_ref) {
    auto cs = doc.resolve(cs_ref);
    auto info = std::make_shared<CsInfo>();

    if (cs.is_name()) {
        info->kind = (cs.str_val == "DeviceCMYK") ? CsInfo::CMYK4 : CsInfo::NUMERIC;
        return info;
    }
    if (!cs.is_arr() || cs.arr.empty()) return info;
    auto head = doc.resolve(cs.arr[0]);
    if (!head.is_name()) return info;
    const std::string& kindname = head.str_val;

    if (kindname == "ICCBased") {
        info->kind = (colorspace_component_count(doc, cs) == 4) ? CsInfo::CMYK4
                                                                : CsInfo::NUMERIC;
        return info;
    }

    if (kindname == "Separation" || kindname == "DeviceN") {
        info->kind = CsInfo::TINT;
        info->lut.resize(256);
        std::string colorant;
        if (kindname == "Separation" && cs.arr.size() >= 2) {
            auto cn = doc.resolve(cs.arr[1]);
            if (cn.is_name()) colorant = cn.str_val;
        }
        bool have_fn = false;
        // /All and /Black are defined by name; others via the tint transform.
        if (colorant != "All" && colorant != "Black" &&
            kindname == "Separation" && cs.arr.size() >= 4) {
            have_fn = true;
            for (int v = 0; v < 256; v++) {
                auto comps = eval_function_1d(doc, cs.arr[3], v / 255.0);
                if (comps.empty()) { have_fn = false; break; }
                alt_components_to_rgb(comps, info->lut[v]);
            }
        }
        if (!have_fn) {
            // Ink-coverage ramp: tint 0 = paper (white), tint 1 = solid ink.
            for (int v = 0; v < 256; v++) {
                uint8_t g = static_cast<uint8_t>(255 - v);
                info->lut[v] = {g, g, g};
            }
        }
        return info;
    }

    if (kindname == "Indexed" || kindname == "I") {
        if (cs.arr.size() < 4) return info;
        int base_comps = colorspace_component_count(doc, doc.resolve(cs.arr[1]));
        int hival = doc.resolve(cs.arr[2]).as_int();
        if (hival < 0) return info;
        auto lut_obj = doc.resolve(cs.arr[3]);
        std::vector<uint8_t> raw;
        if (lut_obj.is_str()) raw.assign(lut_obj.str_val.begin(), lut_obj.str_val.end());
        else if (lut_obj.is_stream()) raw = doc.decode_stream(lut_obj);
        if (raw.empty()) return info;
        info->kind = CsInfo::INDEXED;
        info->lut.resize(static_cast<size_t>(hival) + 1);
        for (int i = 0; i <= hival; i++) {
            size_t off = static_cast<size_t>(i) * base_comps;
            std::vector<double> comps;
            for (int c = 0; c < base_comps; c++)
                comps.push_back(off + c < raw.size() ? raw[off + c] / 255.0 : 0.0);
            alt_components_to_rgb(comps, info->lut[i]);
        }
        return info;
    }

    if (kindname == "DeviceCMYK") info->kind = CsInfo::CMYK4;
    return info;
}

ContentParseResult parse_content_stream(PdfDoc& doc, const std::vector<uint8_t>& stream,
                                         const PdfObj& resources, double page_height,
                                         FontCache* font_cache,
                                         const ContentParseOptions& options,
                                         const double* initial_ctm,
                                         int depth,
                                         const GfxState* inherit_gs) {
    ContentParseResult result;
    const bool skip_graphics =
        options.graphics == GraphicsCollection::None;
    const bool collect_render_paths =
        options.graphics == GraphicsCollection::RenderPaths;

    // Load fonts from resources, using cross-page cache when available.
    // The lock covers only lookup/insert; load_font runs outside it.
    std::unordered_map<std::string, PdfFont> fonts;
    auto res = doc.resolve(resources);
    auto& font_dict = res.get("Font");
    if (!font_dict.is_none()) {
        auto fd = doc.resolve(font_dict);
        if (fd.is_dict()) {
            for (auto& [name, ref] : fd.dict) {
                int rn = ref.is_ref() ? ref.ref_num : -1;
                if (font_cache && rn >= 0) {
                    std::lock_guard<std::mutex> lock(font_cache->mu);
                    auto it = font_cache->map.find(rn);
                    if (it != font_cache->map.end()) {
                        fonts[name] = it->second;
                        continue;
                    }
                }
                fonts[name] = load_font(doc, ref);
                if (font_cache && rn >= 0) {
                    std::lock_guard<std::mutex> lock(font_cache->mu);
                    font_cache->map.emplace(rn, fonts[name]);
                }
            }
        }
    }

    std::vector<GfxState> state_stack;
    GfxState gs;
    if (inherit_gs) {
        // Form XObjects inherit the caller's full graphics state (PDF 8.10.1):
        // fill/stroke color, /ca /CA alpha, clip. The text machinery restarts.
        gs = *inherit_gs;
        gs.in_text = false;
        gs.font = nullptr;
    }
    if (initial_ctm) std::memcpy(gs.ctm, initial_ctm, sizeof(gs.ctm));
    std::vector<PathPoint> current_path;
    std::vector<PathPoint> pending_clip; // path named by W, active after next paint op
    bool has_pending_clip = false;
    int draw_seq = 0; // shared z-order counter for recorded paths and images

    auto path_bbox = [](const std::vector<PathPoint>& pts, double& x0, double& y0,
                        double& x1, double& y1) {
        x0 = y0 = 1e18; x1 = y1 = -1e18;
        for (auto& pt : pts) {
            if (pt.type == PathPoint::CLOSE) continue;
            x0 = std::min(x0, pt.x); x1 = std::max(x1, pt.x);
            y0 = std::min(y0, pt.y); y1 = std::max(y1, pt.y);
            if (pt.type == PathPoint::CURVE) {
                x0 = std::min({x0, pt.cx1, pt.cx2}); x1 = std::max({x1, pt.cx1, pt.cx2});
                y0 = std::min({y0, pt.cy1, pt.cy2}); y1 = std::max({y1, pt.cy1, pt.cy2});
            }
        }
    };

    auto commit_pending_clip = [&](GfxState& g) {
        if (has_pending_clip) {
            g.clip_path = std::make_shared<const std::vector<PathPoint>>(
                std::move(pending_clip));
            pending_clip.clear();
            has_pending_clip = false;
        }
    };

    // "Clip to the shape, then paint a covering rect" draws the clip shape;
    // without a clip stack, substituting the clip path is the faithful read.
    auto apply_clip_substitution = [&](GfxState& g) {
        if (!g.clip_path || g.clip_path->empty() || current_path.empty()) return;
        double fx0, fy0, fx1, fy1, cx0, cy0, cx1, cy1;
        path_bbox(current_path, fx0, fy0, fx1, fy1);
        path_bbox(*g.clip_path, cx0, cy0, cx1, cy1);
        if (fx0 <= cx0 + 0.1 && fy0 <= cy0 + 0.1 &&
            fx1 >= cx1 - 0.1 && fy1 >= cy1 - 0.1 &&
            (fx1 - fx0) * (fy1 - fy0) > (cx1 - cx0) * (cy1 - cy0) + 0.01)
            current_path = *g.clip_path;
    };

    std::unordered_map<std::string, std::shared_ptr<const CsInfo>> cs_cache;
    auto lookup_colorspace =
        [&](const std::string& name) -> std::shared_ptr<const CsInfo> {
        auto it = cs_cache.find(name);
        if (it != cs_cache.end()) return it->second;
        std::shared_ptr<const CsInfo> info;
        if (name == "DeviceRGB" || name == "DeviceGray" || name == "CalRGB" ||
            name == "CalGray" || name == "Pattern") {
            info = std::make_shared<CsInfo>();
        } else if (name == "DeviceCMYK") {
            auto ci = std::make_shared<CsInfo>();
            ci->kind = CsInfo::CMYK4;
            info = ci;
        } else {
            auto cs_dict = doc.resolve(res.get("ColorSpace"));
            info = cs_dict.is_dict() ? load_colorspace(doc, cs_dict.get(name))
                                     : std::make_shared<CsInfo>();
        }
        cs_cache.emplace(name, info);
        return info;
    };

    PdfLexer lex(stream.data(), stream.size());
    // Content streams are overwhelmingly numeric: coordinates, matrices and
    // colors account for tens of millions of operands in vector-heavy PDFs.
    // Keeping every number in a full PdfObj constructs and destroys its string,
    // array and dictionary members even though operators only need a double.
    // Preserve the exact mixed operand order in a compact reference, while
    // retaining PdfObj only for names, strings and arrays.
    struct ContentOperand {
        double number = 0;
        uint32_t object_index = 0;
        bool is_number = true;
    };
    std::vector<ContentOperand> operands;
    std::vector<PdfObj> object_operands;
    operands.reserve(8);
    object_operands.reserve(2);

    auto push_object = [&](PdfObj&& obj) {
        const uint32_t index =
            static_cast<uint32_t>(object_operands.size());
        object_operands.push_back(std::move(obj));
        operands.push_back({0, index, false});
    };

    auto operand_object = [&](size_t index) -> const PdfObj* {
        if (index >= operands.size() || operands[index].is_number)
            return nullptr;
        const uint32_t object_index = operands[index].object_index;
        return object_index < object_operands.size()
            ? &object_operands[object_index] : nullptr;
    };

    auto operand_num = [&](size_t index) -> double {
        if (index >= operands.size()) return 0;
        const auto& operand = operands[index];
        if (operand.is_number) return operand.number;
        const PdfObj* obj = operand_object(index);
        return obj ? obj->as_num() : 0;
    };

    auto pop_num = [&](int idx_from_end = 0) -> double {
        int i = static_cast<int>(operands.size()) - 1 - idx_from_end;
        if (i < 0) return 0;
        return operand_num(static_cast<size_t>(i));
    };

    auto flush_path_segments = [&]() {
        // Extract line segments from path
        double px = 0, py = 0;
        bool has_move = false;
        double move_x = 0, move_y = 0;

        for (auto& pt : current_path) {
            switch (pt.type) {
                case PathPoint::MOVE:
                    px = pt.x; py = pt.y;
                    move_x = px; move_y = py;
                    has_move = true;
                    break;
                case PathPoint::LINE: {
                    PdfLineSegment seg;
                    seg.x0 = static_cast<float>(px);
                    seg.y0 = static_cast<float>(py);
                    seg.x1 = static_cast<float>(pt.x);
                    seg.y1 = static_cast<float>(pt.y);
                    if (seg.is_horizontal() || seg.is_vertical())
                        result.segments.push_back(seg);
                    px = pt.x; py = pt.y;
                    break;
                }
                case PathPoint::CURVE:
                    px = pt.x; py = pt.y;
                    break;
                case PathPoint::CLOSE:
                    if (has_move) {
                        PdfLineSegment seg;
                        seg.x0 = static_cast<float>(px);
                        seg.y0 = static_cast<float>(py);
                        seg.x1 = static_cast<float>(move_x);
                        seg.y1 = static_cast<float>(move_y);
                        if (seg.is_horizontal() || seg.is_vertical())
                            result.segments.push_back(seg);
                        px = move_x; py = move_y;
                    }
                    break;
            }
        }
    };

    auto filter_white_stroke = [&]() -> bool {
        return (gs.stroke_r >= 0.94 && gs.stroke_g >= 0.94 && gs.stroke_b >= 0.94);
    };

    auto filter_small_rect = [&]() -> bool {
        if (current_path.size() < 4 || current_path.size() > 6) return false;
        double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
        double first_x = 0, first_y = 0, last_x = 0, last_y = 0;
        bool has_start = false;
        for (auto& pt : current_path) {
            if (pt.type == PathPoint::MOVE || pt.type == PathPoint::LINE) {
                if (!has_start) { first_x = pt.x; first_y = pt.y; has_start = true; }
                last_x = pt.x; last_y = pt.y;
                if (pt.x < min_x) min_x = pt.x;
                if (pt.x > max_x) max_x = pt.x;
                if (pt.y < min_y) min_y = pt.y;
                if (pt.y > max_y) max_y = pt.y;
            }
        }
        if (std::abs(first_x - last_x) < 2 && std::abs(first_y - last_y) < 2) {
            double w = max_x - min_x, h = max_y - min_y;
            // Thin horizontal rect (Word table border) → emit as h-line.
            if (h < 3.0 && w >= 20.0) {
                float cy = static_cast<float>((min_y + max_y) / 2.0);
                result.segments.push_back({static_cast<float>(min_x), cy,
                                           static_cast<float>(max_x), cy});
                return true;
            }
            // Thin vertical rect → emit as v-line.
            if (w < 3.0 && h >= 5.0) {
                float cx = static_cast<float>((min_x + max_x) / 2.0);
                result.segments.push_back({cx, static_cast<float>(min_y),
                                           cx, static_cast<float>(max_y)});
                return true;
            }
            if (h < 20.0) return true;
        }
        return false;
    };

    // Pure-fill (f/F/f*) rect handling: thin rects become rules, sizable
    // rects are cell shading recorded as PdfFillRect — their edges stay OUT
    // of the segment pool (a shading edge is not a drawn rule; the table
    // pass weighs it separately). Word emits several `re` subpaths under a
    // single fill, so the path is decomposed subpath by subpath; any
    // non-rect subpath bails out to the generic edge flush.
    auto capture_fill_rect = [&]() -> bool {
        struct Rect { double x0, y0, x1, y1; };
        std::vector<Rect> rs;
        size_t i = 0, n = current_path.size();
        while (i < n) {
            if (current_path[i].type != PathPoint::MOVE) return false;
            double px[5], py[5];
            px[0] = current_path[i].x;
            py[0] = current_path[i].y;
            size_t j = i + 1;
            int pts = 1;
            while (j < n && current_path[j].type == PathPoint::LINE && pts < 5) {
                px[pts] = current_path[j].x;
                py[pts] = current_path[j].y;
                pts++;
                j++;
            }
            if (pts == 5) {
                // explicit return to the start corner
                if (std::abs(px[4] - px[0]) >= 2 || std::abs(py[4] - py[0]) >= 2)
                    return false;
                pts = 4;
            }
            if (pts != 4) return false;
            if (j < n && current_path[j].type == PathPoint::CLOSE) j++;
            double min_x = px[0], max_x = px[0], min_y = py[0], max_y = py[0];
            for (int k = 1; k < 4; k++) {
                min_x = std::min(min_x, px[k]);
                max_x = std::max(max_x, px[k]);
                min_y = std::min(min_y, py[k]);
                max_y = std::max(max_y, py[k]);
            }
            for (int k = 0; k < 4; k++) {
                bool on_x = std::abs(px[k] - min_x) < 2 || std::abs(px[k] - max_x) < 2;
                bool on_y = std::abs(py[k] - min_y) < 2 || std::abs(py[k] - max_y) < 2;
                if (!on_x || !on_y) return false;   // not axis-aligned
            }
            rs.push_back({min_x, min_y, max_x, max_y});
            i = j;
        }
        if (rs.empty()) return false;
        for (auto& r : rs) {
            double w = r.x1 - r.x0, h = r.y1 - r.y0;
            if (h < 3.0 && w >= 20.0) {
                float cy = static_cast<float>((r.y0 + r.y1) / 2.0);
                result.segments.push_back({static_cast<float>(r.x0), cy,
                                           static_cast<float>(r.x1), cy});
            } else if (w < 3.0 && h >= 5.0) {
                float cx = static_cast<float>((r.x0 + r.x1) / 2.0);
                result.segments.push_back({cx, static_cast<float>(r.y0),
                                           cx, static_cast<float>(r.y1)});
            } else if (w >= 15.0 && h >= 8.0) {
                result.fill_rects.push_back({static_cast<float>(r.x0),
                                             static_cast<float>(r.y0),
                                             static_cast<float>(r.x1),
                                             static_cast<float>(r.y1),
                                             static_cast<float>(gs.fill_r),
                                             static_cast<float>(gs.fill_g),
                                             static_cast<float>(gs.fill_b)});
            }
            // else: tiny rect (border joints, specks) — swallow
        }
        return true;
    };

    auto record_render_path = [&](bool do_fill, bool do_stroke,
                                  bool even_odd = false) {
        if (collect_render_paths) {
            RenderPath rp;
            rp.points = std::move(current_path);
            rp.fill_r = gs.fill_r; rp.fill_g = gs.fill_g; rp.fill_b = gs.fill_b;
            rp.stroke_r = gs.stroke_r; rp.stroke_g = gs.stroke_g;
            rp.stroke_b = gs.stroke_b;
            rp.fill_alpha = gs.fill_alpha; rp.stroke_alpha = gs.stroke_alpha;
            // Line width is a user-space quantity — the CTM scales the pen
            // along with the geometry, and these points are already in page
            // space. Leaving it raw drew a CAD sheet's hairlines, set to 24
            // units under a 1:2.8 plot matrix, as 50-pixel bars instead of
            // 0.72pt lines.
            rp.line_width = gs.line_width * ctm_pen_scale(gs.ctm);
            rp.do_fill = do_fill; rp.do_stroke = do_stroke;
            rp.even_odd = even_odd;
            rp.seq = draw_seq++;
            result.paths.push_back(std::move(rp));
        }
        current_path.clear();
    };

    // Show one text string: decode codes, map to Unicode, advance the text
    // matrix and emit a TextChar per rendered glyph. Shared by Tj/'/" and by
    // each string element of TJ (TJ handles its numeric adjustments itself), so
    // the two show paths cannot drift apart. Rotated glyphs advance but are not
    // emitted; unmappable/PUA glyphs are skipped without advancing (matching the
    // original per-operator loops).
    // Opaque-Type3 glyph programs, parsed once per (font, code) in glyph
    // space; each show instance transforms a copy to page space.
    std::unordered_map<const PdfFont*, std::unordered_map<uint32_t, std::vector<RenderPath>>> t3_glyph_cache;

    auto expand_type3_glyph = [&](GfxState& gs, uint32_t code) {
        auto& per_font = t3_glyph_cache[gs.font];
        auto it = per_font.find(code);
        if (it == per_font.end()) {
            std::vector<RenderPath> glyph;
            auto name_it = gs.font->differences.find(static_cast<int>(code));
            if (name_it != gs.font->differences.end() && depth < 8) {
                auto proc = doc.resolve(gs.font->charprocs.get(name_it->second));
                if (proc.is_stream()) {
                    auto data = doc.decode_stream(proc);
                    if (!data.empty()) {
                        const PdfObj& sub_res = gs.font->t3_resources.is_none()
                            ? resources : gs.font->t3_resources;
                        ContentParseOptions glyph_options;
                        glyph_options.graphics = GraphicsCollection::RenderPaths;
                        auto sub = parse_content_stream(doc, data, sub_res,
                                                        page_height, font_cache,
                                                        glyph_options, nullptr,
                                                        depth + 1);
                        glyph = std::move(sub.paths);
                    }
                }
            }
            it = per_font.emplace(code, std::move(glyph)).first;
        }
        if (it->second.empty()) return;

        // glyph space → page space: FontMatrix, then the text rendering chain
        double trm[6], final_mat[6], M[6];
        double scale_mat[6] = {gs.font_size * gs.h_scaling / 100.0, 0, 0,
                               gs.font_size, 0, gs.text_rise};
        mat_multiply(trm, scale_mat, gs.text_mat);
        mat_multiply(final_mat, trm, gs.ctm);
        mat_multiply(M, gs.font->font_matrix, final_mat);
        // The glyph program was parsed with an identity CTM, so its widths
        // carry only whatever `cm` the charproc set itself; this adds the
        // show-time chain on top.
        double lw_scale = ctm_pen_scale(M);

        for (auto rp : it->second) {
            for (auto& pt : rp.points) {
                transform_point(M, pt.x, pt.y, pt.x, pt.y);
                if (pt.type == PathPoint::CURVE) {
                    transform_point(M, pt.cx1, pt.cy1, pt.cx1, pt.cy1);
                    transform_point(M, pt.cx2, pt.cy2, pt.cx2, pt.cy2);
                }
            }
            // d1 glyphs (the near-universal case) take the color in force at
            // the show operator, not any color set inside the glyph program.
            rp.fill_r = gs.fill_r; rp.fill_g = gs.fill_g; rp.fill_b = gs.fill_b;
            rp.stroke_r = gs.stroke_r; rp.stroke_g = gs.stroke_g; rp.stroke_b = gs.stroke_b;
            rp.fill_alpha = gs.fill_alpha; rp.stroke_alpha = gs.stroke_alpha;
            rp.line_width *= lw_scale;
            rp.seq = draw_seq++;
            result.paths.push_back(std::move(rp));
        }
    };

    auto show_text_string = [&](GfxState& gs, const std::string& s) {
        double fs = gs.font_size;
        double h_scale = gs.h_scaling / 100.0;
        bool t3_expand = gs.font && gs.font->opaque_type3 &&
                         collect_render_paths;
        double gw_scale = (gs.font && gs.font->is_type3) ? gs.font->glyph_space_scale : 0.001;
        bool use_2byte = gs.font && (gs.font->is_identity || gs.font->is_type0);
        if (gs.font && gs.font->cmap_code_bytes == 1) use_2byte = false;
        if (gs.font && gs.font->cmap_code_bytes == 2) use_2byte = true;

        size_t i = 0;
        while (i < s.size()) {
            uint32_t code;
            if (use_2byte && i + 1 < s.size()) {
                code = (static_cast<uint8_t>(s[i]) << 8) | static_cast<uint8_t>(s[i + 1]);
                i += 2;
            } else {
                code = static_cast<uint8_t>(s[i]);
                i++;
            }

            // Scrambled Type3: draw the glyph program, advance, emit no char.
            if (t3_expand) {
                expand_type3_glyph(gs, code);
                double glyph_w = gs.font->get_width(code);
                if (glyph_w <= 0) glyph_w = 600;
                double advance = glyph_w * gw_scale * fs * h_scale + gs.char_spacing;
                gs.text_mat[4] += advance * gs.text_mat[0];
                gs.text_mat[5] += advance * gs.text_mat[1];
                continue;
            }

            uint32_t unicode = gs.font ? gs.font->decode_char(code) : code;
            if (unicode == 0 || unicode == 0xFFFD) continue;
            // Private-use glyphs have no portable text value. Skip Unicode
            // noncharacters too, but retain valid supplementary characters
            // such as mathematical alphanumerics above U+FFFF.
            const bool private_use =
                (unicode >= 0xE000 && unicode <= 0xF8FF) ||
                (unicode >= 0xF0000 && unicode <= 0xFFFFD) ||
                (unicode >= 0x100000 && unicode <= 0x10FFFD);
            const bool noncharacter =
                (unicode >= 0xFDD0 && unicode <= 0xFDEF) ||
                ((unicode & 0xFFFF) >= 0xFFFE);
            if (private_use || noncharacter) continue;

            // Rendering matrix from the pre-advance text matrix.
            double trm[6];
            double scale_mat[6] = {fs * h_scale, 0, 0, fs, 0, gs.text_rise};
            mat_multiply(trm, scale_mat, gs.text_mat);
            double final_mat[6];
            mat_multiply(final_mat, trm, gs.ctm);

            double glyph_w = gs.font ? gs.font->get_width(code) : 0;
            if (glyph_w <= 0) glyph_w = (gs.font && (gs.font->is_identity || gs.font->is_type0)) ? 1000 : 600;
            double char_w_ts = glyph_w * gw_scale * fs * h_scale;
            double advance = char_w_ts + gs.char_spacing;
            if (unicode == ' ') advance += gs.word_spacing;

            // Writing direction, quantized to 15° buckets. Upright text lands
            // on 0 even when b is rounding noise rather than a true tilt, and
            // takes the no-trig path — every glyph of ordinary prose runs
            // through here, and almost none of them are rotated.
            int16_t rot = 0;
            if (final_mat[1] != 0 || final_mat[0] < 0) {
                double deg = std::atan2(final_mat[1], final_mat[0]) *
                             (180.0 / 3.14159265358979323846);
                int q = static_cast<int>(std::lround(deg / 15.0));
                rot = static_cast<int16_t>((q % 24 + 24) % 24);
            }

            double gx, gy;
            transform_point(final_mat, 0, 0, gx, gy);
            double char_h = std::abs(final_mat[3]);
            if (char_h < 1) char_h = std::abs(final_mat[0]);

            // Advance first; the right edge is the next glyph's origin.
            gs.text_mat[4] += advance * gs.text_mat[0];
            gs.text_mat[5] += advance * gs.text_mat[1];
            double next_gx, next_gy;
            {
                double next_mat[6];
                mat_multiply(next_mat, gs.text_mat, gs.ctm);
                transform_point(next_mat, 0, 0, next_gx, next_gy);
            }

            TextChar tc;
            tc.x = gx;
            tc.y = gy;
            tc.rot = rot;
            if (rot == 0) {
                tc.left = gx;
                tc.right = next_gx;
                tc.top = gy + char_h * 0.8;
                tc.bot = gy - char_h * 0.2;
                tc.font_size = char_h;
            } else {
                // A rotated glyph's baseline does not run along +x, so the box
                // has to be built from the transformed axes: the baseline from
                // this origin to the next, raised by the ascender and dropped
                // by the descender along the "up" vector. Its em height is that
                // vector's length, which for upright text is the |d| read above.
                double ux = final_mat[2], uy = final_mat[3];
                double up_len = std::hypot(ux, uy);
                if (up_len < 1) {
                    // Degenerate up vector: size from the baseline instead and
                    // stand the ascender perpendicular to it.
                    up_len = std::hypot(final_mat[0], final_mat[1]);
                    double bx = -final_mat[1], by = final_mat[0];
                    double bl = std::hypot(bx, by);
                    if (bl > 0) { ux = bx / bl * up_len; uy = by / bl * up_len; }
                    else { ux = 0; uy = up_len; }
                }
                double ax = ux * 0.8, ay = uy * 0.8;
                double dxx = ux * -0.2, dyy = uy * -0.2;
                double cx[4] = {gx + ax, gx + dxx, next_gx + ax, next_gx + dxx};
                double cy[4] = {gy + ay, gy + dyy, next_gy + ay, next_gy + dyy};
                tc.left = *std::min_element(cx, cx + 4);
                tc.right = *std::max_element(cx, cx + 4);
                tc.top = *std::max_element(cy, cy + 4);
                tc.bot = *std::min_element(cy, cy + 4);
                tc.font_size = up_len;
            }
            tc.unicode = unicode;
            tc.is_bold = (gs.font && gs.font->is_bold) ||
                         gs.render_mode == 2 || gs.render_mode == 6;
            tc.is_italic = gs.font ? gs.font->is_italic : false;
            // Tr 3/7 draw nothing: an OCR text layer over a scanned page is
            // all invisible, and the composite classifier must not mistake it
            // for visible body text.
            if (gs.render_mode != 3 && gs.render_mode != 7)
                result.visible_text_chars++;
            result.chars.push_back(tc);
        }
    };

    while (lex.pos < lex.len) {
        lex.skip_ws();
        if (lex.pos >= lex.len) break;

        uint8_t first_byte = lex.data[lex.pos];

        // Fast path: numbers (most common token in content streams)
        if ((first_byte >= '0' && first_byte <= '9') || first_byte == '-' || first_byte == '+' || first_byte == '.') {
            size_t start = lex.pos;
            bool has_dot = (first_byte == '.');
            lex.pos++;
            while (lex.pos < lex.len) {
                uint8_t c = lex.data[lex.pos];
                if (c >= '0' && c <= '9') { lex.pos++; }
                else if (c == '.' && !has_dot) { has_dot = true; lex.pos++; }
                else break;
            }
            // Inline integer parse to avoid strtoll overhead
            const uint8_t* ndata = lex.data + start;
            size_t nlen = lex.pos - start;
            if (!has_dot && nlen <= 10) {
                int64_t val = 0;
                bool neg = false;
                size_t i = 0;
                if (ndata[0] == '-') { neg = true; i = 1; }
                else if (ndata[0] == '+') { i = 1; }
                for (; i < nlen; i++) val = val * 10 + (ndata[i] - '0');
                operands.push_back(
                    {static_cast<double>(neg ? -val : val), 0, true});
            } else {
                operands.push_back({parse_pdf_real(
                    reinterpret_cast<const char*>(ndata), nlen), 0, true});
            }
            continue;
        }

        // /name → operand
        if (first_byte == '/') {
            PdfObj obj = lex.parse_object();
            push_object(std::move(obj));
            continue;
        }

        // String or array or dict → parse as object
        if (first_byte == '(' || first_byte == '<' || first_byte == '[') {
            PdfObj obj = lex.parse_object();
            if (!obj.is_none()) push_object(std::move(obj));
            continue;
        }

        // Bare keyword → operator (zero-copy: compare via pointer+length)
        size_t saved = lex.pos;
        while (lex.pos < lex.len && !PdfLexer::is_ws(lex.data[lex.pos]) && !PdfLexer::is_delim(lex.data[lex.pos]))
            lex.pos++;
        size_t tok_len = lex.pos - saved;
        if (tok_len == 0) { lex.pos++; continue; }
        const char* tok_ptr = reinterpret_cast<const char*>(lex.data + saved);

        if (tok_len == 4 && std::memcmp(tok_ptr, "true", 4) == 0) {
            push_object(PdfObj::make_bool(true));
            continue;
        }
        if (tok_len == 5 && std::memcmp(tok_ptr, "false", 5) == 0) {
            push_object(PdfObj::make_bool(false));
            continue;
        }
        if (tok_len == 4 && std::memcmp(tok_ptr, "null", 4) == 0) continue;

        {
            const ContentOperator op(tok_ptr, tok_len);

            // ── Graphics State ──
            if (op.is("q")) {
                state_stack.push_back(gs);
            } else if (op.is("Q")) {
                if (!state_stack.empty()) { gs = state_stack.back(); state_stack.pop_back(); }
            } else if (op.is("cm")) {
                if (operands.size() >= 6) {
                    double m[6] = {pop_num(5), pop_num(4), pop_num(3), pop_num(2), pop_num(1), pop_num(0)};
                    double r[6];
                    mat_multiply(r, m, gs.ctm);
                    std::memcpy(gs.ctm, r, sizeof(r));
                }
            } else if (op.is("w")) {
                gs.line_width = pop_num(0);
            } else if (op.is("J")) {
                gs.line_cap = static_cast<int>(pop_num(0));
            } else if (op.is("j")) {
                gs.line_join = static_cast<int>(pop_num(0));
            } else if (op.is("M")) {
                gs.miter_limit = pop_num(0);
            }

            // ── Color (skip when graphics not needed) ──
            else if (skip_graphics && (op.is("RG") || op.is("rg") || op.is("G") ||
                     op.is("g") || op.is("K") || op.is("k") || op.is("SC") ||
                     op.is("SCN") || op.is("sc") || op.is("scn") || op.is("CS") ||
                     op.is("cs"))) {
                // skip color ops
            }
            else if (op.is("RG")) {
                if (operands.size() >= 3) { gs.stroke_r = pop_num(2); gs.stroke_g = pop_num(1); gs.stroke_b = pop_num(0); }
            } else if (op.is("rg")) {
                if (operands.size() >= 3) { gs.fill_r = pop_num(2); gs.fill_g = pop_num(1); gs.fill_b = pop_num(0); }
            } else if (op.is("G")) {
                double g = pop_num(0); gs.stroke_r = gs.stroke_g = gs.stroke_b = g;
            } else if (op.is("g")) {
                double g = pop_num(0); gs.fill_r = gs.fill_g = gs.fill_b = g;
            } else if (op.is("K")) {
                if (operands.size() >= 4) {
                    std::array<uint8_t, 3> px;
                    cmyk_to_rgb8(pop_num(3), pop_num(2), pop_num(1), pop_num(0), px);
                    gs.stroke_r = px[0] / 255.0; gs.stroke_g = px[1] / 255.0; gs.stroke_b = px[2] / 255.0;
                }
            } else if (op.is("k")) {
                if (operands.size() >= 4) {
                    std::array<uint8_t, 3> px;
                    cmyk_to_rgb8(pop_num(3), pop_num(2), pop_num(1), pop_num(0), px);
                    gs.fill_r = px[0] / 255.0; gs.fill_g = px[1] / 255.0; gs.fill_b = px[2] / 255.0;
                }
            } else if (op.is("SC") || op.is("SCN") || op.is("sc") || op.is("scn")) {
                bool is_fill = (op.is("sc") || op.is("scn"));
                const auto& cs = is_fill ? gs.fill_cs : gs.stroke_cs;
                double r = -1, g = -1, b = -1;
                if (cs && cs->kind == CsInfo::TINT && operands.size() >= 1 &&
                    !cs->lut.empty()) {
                    double t = std::min(1.0, std::max(0.0, pop_num(0)));
                    auto& px = cs->lut[static_cast<size_t>(t * 255.0 + 0.5)];
                    r = px[0] / 255.0; g = px[1] / 255.0; b = px[2] / 255.0;
                } else if (cs && cs->kind == CsInfo::INDEXED &&
                           operands.size() >= 1 && !cs->lut.empty()) {
                    long idx = std::lround(pop_num(0));
                    if (idx < 0) idx = 0;
                    if (idx >= static_cast<long>(cs->lut.size()))
                        idx = static_cast<long>(cs->lut.size()) - 1;
                    auto& px = cs->lut[idx];
                    r = px[0] / 255.0; g = px[1] / 255.0; b = px[2] / 255.0;
                } else if (operands.size() >= 4 &&
                           (!cs || cs->kind == CsInfo::CMYK4)) {
                    std::array<uint8_t, 3> px;
                    cmyk_to_rgb8(pop_num(3), pop_num(2), pop_num(1), pop_num(0), px);
                    r = px[0] / 255.0; g = px[1] / 255.0; b = px[2] / 255.0;
                } else if (operands.size() >= 3) {
                    r = pop_num(2); g = pop_num(1); b = pop_num(0);
                } else if (operands.size() >= 1 && operands.back().is_number) {
                    r = g = b = pop_num(0);
                }
                if (r >= 0) {
                    if (is_fill) { gs.fill_r = r; gs.fill_g = g; gs.fill_b = b; }
                    else { gs.stroke_r = r; gs.stroke_g = g; gs.stroke_b = b; }
                }
            } else if (op.is("gs")) {
                // ExtGState: constant alpha is how watermarks are faded;
                // ignoring it paints them solid over the page.
                const PdfObj* name = operands.empty()
                    ? nullptr : operand_object(operands.size() - 1);
                if (name && name->is_name()) {
                    auto egs_dict = doc.resolve(res.get("ExtGState"));
                    if (egs_dict.is_dict()) {
                        auto egs = doc.resolve(egs_dict.get(name->str_val));
                        if (egs.is_dict()) {
                            auto& ca = egs.get("ca");
                            if (ca.is_num()) gs.fill_alpha = ca.as_num();
                            auto& CA = egs.get("CA");
                            if (CA.is_num()) gs.stroke_alpha = CA.as_num();
                            auto& lw = egs.get("LW");
                            if (lw.is_num()) gs.line_width = lw.as_num();
                        }
                    }
                }
            } else if (op.is("CS") || op.is("cs")) {
                const PdfObj* name = operands.empty()
                    ? nullptr : operand_object(operands.size() - 1);
                if (name && name->is_name()) {
                    auto info = lookup_colorspace(name->str_val);
                    bool is_fill = op.is("cs");
                    // Spec initial color: black for device spaces, tint 1.0
                    // (solid ink) for Separation/DeviceN, index 0 for Indexed.
                    double r = 0, g = 0, b = 0;
                    if (info && info->kind == CsInfo::TINT && !info->lut.empty()) {
                        auto& px = info->lut.back();
                        r = px[0] / 255.0; g = px[1] / 255.0; b = px[2] / 255.0;
                    } else if (info && info->kind == CsInfo::INDEXED &&
                               !info->lut.empty()) {
                        auto& px = info->lut.front();
                        r = px[0] / 255.0; g = px[1] / 255.0; b = px[2] / 255.0;
                    }
                    if (is_fill) {
                        gs.fill_cs = info;
                        gs.fill_r = r; gs.fill_g = g; gs.fill_b = b;
                    } else {
                        gs.stroke_cs = info;
                        gs.stroke_r = r; gs.stroke_g = g; gs.stroke_b = b;
                    }
                }
            }

            // ── Text ──
            else if (op.is("BT")) {
                double id[6] = {1,0,0,1,0,0};
                std::memcpy(gs.text_mat, id, sizeof(id));
                std::memcpy(gs.line_mat, id, sizeof(id));
                gs.in_text = true;
            } else if (op.is("ET")) {
                gs.in_text = false;
            } else if (op.is("Tf")) {
                if (operands.size() >= 2) {
                    gs.font_size = pop_num(0);
                    const PdfObj* font_name =
                        operand_object(operands.size() - 2);
                    if (font_name) {
                        auto it = fonts.find(font_name->str_val);
                        gs.font = (it != fonts.end()) ? &it->second : nullptr;
                    } else {
                        gs.font = nullptr;
                    }
                }
            } else if (op.is("Td")) {
                if (operands.size() >= 2) {
                    double tx = pop_num(1), ty = pop_num(0);
                    gs.line_mat[4] += tx * gs.line_mat[0] + ty * gs.line_mat[2];
                    gs.line_mat[5] += tx * gs.line_mat[1] + ty * gs.line_mat[3];
                    std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
                }
            } else if (op.is("TD")) {
                if (operands.size() >= 2) {
                    double tx = pop_num(1), ty = pop_num(0);
                    gs.text_leading = -ty;
                    gs.line_mat[4] += tx * gs.line_mat[0] + ty * gs.line_mat[2];
                    gs.line_mat[5] += tx * gs.line_mat[1] + ty * gs.line_mat[3];
                    std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
                }
            } else if (op.is("Tm")) {
                if (operands.size() >= 6) {
                    gs.text_mat[0] = pop_num(5); gs.text_mat[1] = pop_num(4);
                    gs.text_mat[2] = pop_num(3); gs.text_mat[3] = pop_num(2);
                    gs.text_mat[4] = pop_num(1); gs.text_mat[5] = pop_num(0);
                    std::memcpy(gs.line_mat, gs.text_mat, sizeof(gs.line_mat));
                }
            } else if (op.is("T*")) {
                gs.line_mat[4] += -gs.text_leading * gs.line_mat[2];
                gs.line_mat[5] += -gs.text_leading * gs.line_mat[3];
                std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
            } else if (op.is("TL")) {
                gs.text_leading = pop_num(0);
            } else if (op.is("Tc")) {
                gs.char_spacing = pop_num(0);
            } else if (op.is("Tw")) {
                gs.word_spacing = pop_num(0);
            } else if (op.is("Tz")) {
                gs.h_scaling = pop_num(0);
            } else if (op.is("Ts")) {
                gs.text_rise = pop_num(0);
            } else if (op.is("Tr")) {
                gs.render_mode = static_cast<int>(pop_num(0));
            }

            // ── Text Show ──
            else if (op.is("Tj") || op.is("'") || op.is("\"")) {
                if (op.is("'")) {
                    gs.line_mat[4] += -gs.text_leading * gs.line_mat[2];
                    gs.line_mat[5] += -gs.text_leading * gs.line_mat[3];
                    std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
                } else if (op.is("\"")) {
                    if (operands.size() >= 3) {
                        gs.word_spacing = operand_num(0);
                        gs.char_spacing = operand_num(1);
                    }
                    gs.line_mat[4] += -gs.text_leading * gs.line_mat[2];
                    gs.line_mat[5] += -gs.text_leading * gs.line_mat[3];
                    std::memcpy(gs.text_mat, gs.line_mat, sizeof(gs.text_mat));
                }

                const PdfObj* text =
                    operands.empty() ? nullptr
                                     : operand_object(operands.size() - 1);
                if (text && text->is_str()) {
                    show_text_string(gs, text->str_val);
                }
            } else if (op.is("TJ")) {
                const PdfObj* array =
                    operands.empty() ? nullptr
                                     : operand_object(operands.size() - 1);
                if (array && array->is_arr()) {
                    double fs = gs.font_size;
                    double h_scale = gs.h_scaling / 100.0;
                    for (auto& elem : array->arr) {
                        if (elem.is_num()) {
                            double shift = -elem.as_num() / 1000.0 * fs * h_scale;
                            gs.text_mat[4] += shift * gs.text_mat[0];
                            gs.text_mat[5] += shift * gs.text_mat[1];
                        } else if (elem.is_str()) {
                            show_text_string(gs, elem.str_val);
                        }
                    }
                }
            }

            // ── Path Construction (skip when graphics not needed) ──
            else if (skip_graphics && (op.is("m") || op.is("l") || op.is("c") ||
                     op.is("v") || op.is("y") || op.is("h") || op.is("re") ||
                     op.is("S") || op.is("s") || op.is("f") || op.is("F") ||
                     op.is("f*") || op.is("B") || op.is("B*") || op.is("b") ||
                     op.is("b*") || op.is("n") || op.is("W") || op.is("W*"))) {
                // skip path ops entirely
            }
            else if (op.is("m")) {
                if (operands.size() >= 2) {
                    double x = pop_num(1), y = pop_num(0);
                    double tx, ty;
                    transform_point(gs.ctm, x, y, tx, ty);
                    current_path.push_back({tx, ty, PathPoint::MOVE});
                }
            } else if (op.is("l")) {
                if (operands.size() >= 2) {
                    double x = pop_num(1), y = pop_num(0);
                    double tx, ty;
                    transform_point(gs.ctm, x, y, tx, ty);
                    current_path.push_back({tx, ty, PathPoint::LINE});
                }
            } else if (op.is("c")) {
                if (operands.size() >= 6) {
                    double x1 = pop_num(5), y1 = pop_num(4);
                    double x2 = pop_num(3), y2 = pop_num(2);
                    double x3 = pop_num(1), y3 = pop_num(0);
                    PathPoint pp; pp.type = PathPoint::CURVE;
                    transform_point(gs.ctm, x1, y1, pp.cx1, pp.cy1);
                    transform_point(gs.ctm, x2, y2, pp.cx2, pp.cy2);
                    transform_point(gs.ctm, x3, y3, pp.x, pp.y);
                    current_path.push_back(pp);
                }
            } else if (op.is("v")) {
                if (operands.size() >= 4) {
                    double x2 = pop_num(3), y2 = pop_num(2);
                    double x3 = pop_num(1), y3 = pop_num(0);
                    PathPoint pp; pp.type = PathPoint::CURVE;
                    // v: cp1 = current point
                    double prev_x = 0, prev_y = 0;
                    if (!current_path.empty()) {
                        prev_x = current_path.back().x; prev_y = current_path.back().y;
                    }
                    pp.cx1 = prev_x; pp.cy1 = prev_y;
                    transform_point(gs.ctm, x2, y2, pp.cx2, pp.cy2);
                    transform_point(gs.ctm, x3, y3, pp.x, pp.y);
                    current_path.push_back(pp);
                }
            } else if (op.is("y")) {
                if (operands.size() >= 4) {
                    double x1 = pop_num(3), y1 = pop_num(2);
                    double x3 = pop_num(1), y3 = pop_num(0);
                    PathPoint pp; pp.type = PathPoint::CURVE;
                    transform_point(gs.ctm, x1, y1, pp.cx1, pp.cy1);
                    // y: cp2 = endpoint
                    transform_point(gs.ctm, x3, y3, pp.x, pp.y);
                    pp.cx2 = pp.x; pp.cy2 = pp.y;
                    current_path.push_back(pp);
                }
            } else if (op.is("h")) {
                current_path.push_back({0, 0, PathPoint::CLOSE});
            } else if (op.is("re")) {
                if (operands.size() >= 4) {
                    double x = pop_num(3), y = pop_num(2), w = pop_num(1), h = pop_num(0);
                    double tx, ty;
                    transform_point(gs.ctm, x, y, tx, ty);
                    double tx2, ty2; transform_point(gs.ctm, x+w, y, tx2, ty2);
                    double tx3, ty3; transform_point(gs.ctm, x+w, y+h, tx3, ty3);
                    double tx4, ty4; transform_point(gs.ctm, x, y+h, tx4, ty4);
                    current_path.push_back({tx, ty, PathPoint::MOVE});
                    current_path.push_back({tx2, ty2, PathPoint::LINE});
                    current_path.push_back({tx3, ty3, PathPoint::LINE});
                    current_path.push_back({tx4, ty4, PathPoint::LINE});
                    current_path.push_back({0, 0, PathPoint::CLOSE});
                }
            }

            // ── Path Painting ──
            else if (op.is("S")) {
                if (!filter_white_stroke() && !filter_small_rect())
                    flush_path_segments();
                commit_pending_clip(gs);
                record_render_path(false, true);
            } else if (op.is("s")) {
                current_path.push_back({0, 0, PathPoint::CLOSE});
                if (!filter_white_stroke() && !filter_small_rect())
                    flush_path_segments();
                commit_pending_clip(gs);
                record_render_path(false, true);
            } else if (op.is("f") || op.is("F") || op.is("f*")) {
                apply_clip_substitution(gs);
                if (!capture_fill_rect()) flush_path_segments();
                commit_pending_clip(gs);
                record_render_path(true, false, op.is("f*"));
            } else if (op.is("B") || op.is("B*") || op.is("b") || op.is("b*")) {
                if (op.is("b") || op.is("b*"))
                    current_path.push_back({0, 0, PathPoint::CLOSE});
                apply_clip_substitution(gs);
                if (!filter_white_stroke() && !filter_small_rect())
                    flush_path_segments();
                commit_pending_clip(gs);
                record_render_path(true, true,
                                   op.is("B*") || op.is("b*"));
            } else if (op.is("W") || op.is("W*")) {
                pending_clip = current_path;
                has_pending_clip = true;
            } else if (op.is("n")) {
                current_path.clear();
                commit_pending_clip(gs);
            }

            // ── XObject (images) ──
            else if (op.is("Do")) {
                const PdfObj* name =
                    operands.empty() ? nullptr
                                     : operand_object(operands.size() - 1);
                if (name && name->is_name()) {
                    std::string xname = name->str_val;
                    auto& xobjects = res.get("XObject");
                    auto xd = doc.resolve(xobjects);
                    if (xd.is_dict()) {
                        auto& xref = xd.get(xname);
                        auto xobj = doc.resolve(xref);
                        auto& subtype = xobj.get("Subtype");
                        bool is_form = subtype.is_name() && subtype.str_val == "Form";
                        if (is_form && depth < 8) {
                            // Form XObject: parse its content stream so text and
                            // vectors nested inside charts/figures are not lost.
                            auto form_stream = doc.decode_stream(xobj);
                            if (!form_stream.empty()) {
                                double form_ctm[6];
                                std::memcpy(form_ctm, gs.ctm, sizeof(form_ctm));
                                auto& mtx = xobj.get("Matrix");
                                if (mtx.is_arr() && mtx.arr.size() >= 6) {
                                    double fm[6];
                                    for (int k = 0; k < 6; k++) fm[k] = mtx.arr[k].as_num();
                                    mat_multiply(form_ctm, fm, gs.ctm);
                                }
                                auto& form_res = xobj.get("Resources");
                                const PdfObj& sub_res = form_res.is_none() ? res : form_res;
                                auto sub = parse_content_stream(
                                    doc, form_stream, sub_res, page_height,
                                    font_cache, options,
                                    form_ctm, depth + 1,
                                    &gs);
                                // sub is a temporary discarded right after — move
                                // its elements into the parent instead of copying.
                                result.chars.insert(result.chars.end(),
                                    std::make_move_iterator(sub.chars.begin()),
                                    std::make_move_iterator(sub.chars.end()));
                                result.visible_text_chars +=
                                    sub.visible_text_chars;
                                result.segments.insert(result.segments.end(),
                                    std::make_move_iterator(sub.segments.begin()),
                                    std::make_move_iterator(sub.segments.end()));
                                result.fill_rects.insert(result.fill_rects.end(),
                                    std::make_move_iterator(sub.fill_rects.begin()),
                                    std::make_move_iterator(sub.fill_rects.end()));
                                // Rebase the form's draw order to this Do's
                                // position so z-order survives the merge.
                                for (auto& si : sub.images) si.seq += draw_seq;
                                for (auto& sp : sub.paths) sp.seq += draw_seq;
                                draw_seq += sub.draw_ops;
                                result.images.insert(result.images.end(),
                                    std::make_move_iterator(sub.images.begin()),
                                    std::make_move_iterator(sub.images.end()));
                                result.paths.insert(result.paths.end(),
                                    std::make_move_iterator(sub.paths.begin()),
                                    std::make_move_iterator(sub.paths.end()));
                            }
                        } else if (collect_render_paths) {
                            ImagePlacement ip;
                            ip.xobj_name = xname;
                            if (xref.is_ref()) ip.xobj_ref = xref.ref_num;
                            std::memcpy(ip.ctm, gs.ctm, sizeof(gs.ctm));
                            ip.fill_r = gs.fill_r;
                            ip.fill_g = gs.fill_g;
                            ip.fill_b = gs.fill_b;
                            ip.alpha = gs.fill_alpha;
                            ip.seq = draw_seq++;
                            result.images.push_back(ip);
                        }
                    }
                }
            }

            operands.clear();
            object_operands.clear();
        }
    }

    result.draw_ops = draw_seq;
    return result;
}

// ── Layout Engine: TextChar → TextLine ───────────────────


double detect_column_boundary(const std::vector<TextChar>& chars,
                              double median_fs, double y_tol) {
    double page_left = 1e9, page_right = 0;
    for (auto& ch : chars) {
        if (ch.left < page_left) page_left = ch.left;
        if (ch.right > page_right) page_right = ch.right;
    }
    double page_width = page_right - page_left;
    if (page_width < median_fs * 30) return 0;

    // Group chars into Y-rows
    std::vector<size_t> y_sorted(chars.size());
    std::iota(y_sorted.begin(), y_sorted.end(), 0);
    std::sort(y_sorted.begin(), y_sorted.end(), [&](size_t a, size_t b) {
        return chars[a].y > chars[b].y;
    });

    constexpr int NUM_BINS = 200;
    int row_count[NUM_BINS] = {};
    int total_rows = 0;

    size_t ri = 0;
    while (ri < y_sorted.size()) {
        double row_y = chars[y_sorted[ri]].y;
        bool bins_hit[NUM_BINS] = {};
        while (ri < y_sorted.size() && std::abs(chars[y_sorted[ri]].y - row_y) <= y_tol) {
            auto& ch = chars[y_sorted[ri]];
            if (ch.unicode != ' ' && ch.unicode != 0xA0) {
                int b0 = static_cast<int>((ch.left - page_left) / page_width * NUM_BINS);
                int b1 = static_cast<int>((ch.right - page_left) / page_width * NUM_BINS);
                if (b0 < 0) b0 = 0;
                if (b1 >= NUM_BINS) b1 = NUM_BINS - 1;
                for (int b = b0; b <= b1; b++) bins_hit[b] = true;
            }
            ri++;
        }
        for (int b = 0; b < NUM_BINS; b++)
            if (bins_hit[b]) row_count[b]++;
        total_rows++;
    }

    if (total_rows < 10) return 0;

    // Find the deepest dip in row_count within center 50% of page
    int center_start = NUM_BINS / 4;
    int center_end = NUM_BINS * 3 / 4;

    double left_avg = 0, right_avg = 0;
    int lc = 0, rc = 0;
    for (int b = NUM_BINS / 10; b < center_start; b++) { left_avg += row_count[b]; lc++; }
    for (int b = center_end; b < NUM_BINS * 9 / 10; b++) { right_avg += row_count[b]; rc++; }
    if (lc > 0) left_avg /= lc;
    if (rc > 0) right_avg /= rc;
    double body_avg = (left_avg + right_avg) / 2.0;
    if (body_avg < 5) return 0;

    // Find the minimum row_count in center region (smoothed over 3 bins)
    int best_bin = -1;
    double best_val = 1e9;
    for (int b = center_start + 1; b < center_end - 1; b++) {
        double val = (row_count[b - 1] + row_count[b] + row_count[b + 1]) / 3.0;
        if (val < best_val) { best_val = val; best_bin = b; }
    }

    // The dip must be significantly lower than body average (at least 30% lower)
    if (best_val > body_avg * 0.7) return 0;

    return page_left + (best_bin + 0.5) / NUM_BINS * page_width;
}

// Reorder lines so that within each column band, left-column lines
// come before right-column lines. Spanning lines stay in place.
std::vector<TextLine> reorder_column_lines(std::vector<TextLine>& lines,
                                           double col_boundary) {
    enum Type { LEFT, RIGHT, SPANNING };

    // Compute content width for SPANNING minimum width threshold
    double min_left = 1e9, max_right = 0;
    for (auto& l : lines) {
        if (l.x_left < min_left) min_left = l.x_left;
        if (l.x_right > max_right) max_right = l.x_right;
    }
    double content_width = max_right - min_left;
    double span_min_width = content_width * 0.6;
    double page_center = (min_left + max_right) / 2.0;

    std::vector<Type> types(lines.size());
    for (size_t i = 0; i < lines.size(); i++) {
        auto& l = lines[i];
        double line_width = l.x_right - l.x_left;
        double line_center = (l.x_left + l.x_right) / 2.0;
        bool straddles = l.x_left < col_boundary - 5 && l.x_right > col_boundary + 5;
        bool is_wide = line_width > span_min_width;
        bool is_centered = straddles && std::abs(line_center - page_center) < content_width * 0.15;
        if (straddles && (is_wide || is_centered))
            types[i] = SPANNING;
        else if ((l.x_left + l.x_right) / 2.0 < col_boundary)
            types[i] = LEFT;
        else
            types[i] = RIGHT;
    }

    std::vector<TextLine> result;
    result.reserve(lines.size());
    size_t i = 0;
    while (i < lines.size()) {
        if (types[i] == SPANNING) {
            result.push_back(std::move(lines[i]));
            i++;
            continue;
        }
        size_t band_end = i + 1;
        while (band_end < lines.size() && types[band_end] != SPANNING)
            band_end++;
        for (size_t j = i; j < band_end; j++)
            if (types[j] == LEFT) {
                lines[j].is_column_split = true;
                result.push_back(std::move(lines[j]));
            }
        for (size_t j = i; j < band_end; j++)
            if (types[j] == RIGHT) {
                lines[j].is_column_split = true;
                result.push_back(std::move(lines[j]));
            }
        i = band_end;
    }
    return result;
}

// Group chars that all share one writing direction into lines. Callers reach
// this through chars_to_lines(), which rotates each non-upright direction into
// this function's frame first.
static std::vector<TextLine> lines_from_upright_chars(
    const std::vector<TextChar>& chars, double* out_col_boundary) {
    if (chars.empty()) return {};

    // Sort by y (descending, top-first) then x (left-to-right)
    std::vector<size_t> idx(chars.size());
    std::iota(idx.begin(), idx.end(), 0);

    // Compute median font size for line clustering tolerance
    std::vector<double> font_sizes;
    for (auto& ch : chars) if (ch.font_size > 1) font_sizes.push_back(ch.font_size);
    double median_fs = 12;
    if (!font_sizes.empty()) {
        std::sort(font_sizes.begin(), font_sizes.end());
        median_fs = font_sizes[font_sizes.size() / 2];
    }
    double y_tol = median_fs * 0.4;
    if (y_tol < 2) y_tol = 2;

    double col_boundary = detect_column_boundary(chars, median_fs, y_tol);
    if (out_col_boundary) *out_col_boundary = col_boundary;

    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        if (std::abs(chars[a].y - chars[b].y) > y_tol) return chars[a].y > chars[b].y;
        return chars[a].x < chars[b].x;
    });

    std::vector<TextLine> lines;
    double cur_y = chars[idx[0]].y;
    TextLine cur;
    double total_fs = 0;
    int fs_count = 0;
    double prev_right = -1e9;

    auto flush = [&]() {
        if (cur.text.empty()) return;
        size_t end = cur.text.find_last_not_of(" \t");
        if (end != std::string::npos) cur.text.resize(end + 1);
        if (fs_count > 0) cur.font_size = total_fs / fs_count;
        if (!cur.text.empty()) lines.push_back(std::move(cur));
        cur = TextLine{};
        total_fs = 0;
        fs_count = 0;
        prev_right = -1e9;
    };

    // Column-gutter gap threshold: large enough to skip word spaces (~0.15×fs)
    // but small enough to catch tight body-text gutters (~1.2×fs).
    double col_gap_thresh = std::max(median_fs * 1.2, 8.0);

    // Peek-ahead helper: count distinct span clusters in the chars of the
    // current y-row that lie strictly to the right of col_boundary, starting
    // from index start. Two chars belong to the same cluster if their gap is
    // smaller than col_gap_thresh (same as the split threshold below).
    auto right_clusters = [&](size_t start, double cur_y_val) -> int {
        if (col_boundary <= 0) return 0;
        int clusters = 0;
        double last_right = -1e9;
        for (size_t k = start; k < idx.size(); k++) {
            auto& c = chars[idx[k]];
            if (std::abs(c.y - cur_y_val) > y_tol) break;
            if (c.unicode == ' ' || c.unicode == 0xA0) continue;
            if (c.left <= col_boundary) continue;
            if (last_right < -1e8 || c.left - last_right > col_gap_thresh) {
                clusters++;
                if (clusters >= 2) return clusters;
            }
            last_right = std::max(last_right, (double)c.right);
        }
        return clusters;
    };

    for (size_t ii = 0; ii < idx.size(); ii++) {
        auto& ch = chars[idx[ii]];
        if (std::abs(ch.y - cur_y) > y_tol) {
            flush();
            cur_y = ch.y;
        }

        // Split line at column boundary when a large gap crosses it.
        // Skip the split when the right side has multiple distinct cell
        // clusters — that signals a wide table row spanning the page, not
        // two body-text columns sharing a y coordinate. A *very* wide gap
        // (≥ 2×median_fs, ~20pt for 10pt body text) is always treated as a
        // page-gutter split, even when both sides have cell-like content,
        // so two tables sitting side-by-side at the same y get separated.
        if (col_boundary > 0 && !cur.text.empty() && prev_right > -1e8) {
            double gap = ch.left - prev_right;
            if (gap > col_gap_thresh &&
                prev_right < col_boundary && ch.left > col_boundary) {
                bool gutter = gap > std::max(median_fs * 2.0, 18.0);
                if (gutter || right_clusters(ii, cur_y) < 2) {
                    flush();
                    cur_y = ch.y;
                }
            }
        }

        cur.y_center = ch.y;
        if (ch.left < cur.x_left) cur.x_left = ch.left;
        if (ch.right > cur.x_right) cur.x_right = ch.right;

        // Detect word spacing using gap between this char's left and previous char's right
        if (!cur.text.empty() && ch.unicode != ' ' && ch.unicode != 0xA0 && prev_right > -1e8) {
            double gap = ch.left - prev_right;
            // Use font-size-relative threshold for word spacing
            double word_gap = ch.font_size * 0.15;
            if (word_gap < 1) word_gap = 1;
            if (gap > word_gap && gap < ch.font_size * 8 && cur.text.back() != ' ')
                cur.text += ' ';
        }

        if (ch.unicode != ' ' && ch.unicode != 0xA0) {
            cur.is_bold = ch.is_bold;
            cur.is_italic = ch.is_italic;
            total_fs += ch.font_size;
            fs_count++;
        }
        util::append_utf8(cur.text, ch.unicode);
        prev_right = ch.right;
    }
    flush();

    if (col_boundary > 0)
        lines = reorder_column_lines(lines, col_boundary);

    return lines;
}

std::vector<TextLine> chars_to_lines(const std::vector<TextChar>& chars,
                                    double* out_col_boundary) {
    if (out_col_boundary) *out_col_boundary = 0;
    if (chars.empty()) return {};

    // Which writing directions are present. A page of ordinary prose has only
    // the upright one, and takes the same path it always did.
    bool present[24] = {};
    for (auto& ch : chars)
        if (ch.rot >= 0 && ch.rot < 24) present[ch.rot] = true;
    int directions = 0;
    for (bool p : present) directions += p ? 1 : 0;
    if (directions <= 1 && present[0])
        return lines_from_upright_chars(chars, out_col_boundary);

    // Upright text first and unchanged — it drives the column boundary and
    // keeps the body's reading order. Rotated runs follow, each grouped in its
    // own frame, so a 90° dimension label reads along its baseline.
    std::vector<TextLine> lines;
    if (present[0]) {
        std::vector<TextChar> upright;
        upright.reserve(chars.size());
        for (auto& ch : chars)
            if (ch.rot == 0) upright.push_back(ch);
        lines = lines_from_upright_chars(upright, out_col_boundary);
    }

    const double kDegToRad = 3.14159265358979323846 / 180.0;
    for (int r = 1; r < 24; r++) {
        if (!present[r]) continue;
        const double theta = r * 15.0 * kDegToRad;
        const double cs = std::cos(theta), sn = std::sin(theta);
        // Into the direction's own frame (rotate by -theta), and back out.
        auto fwd = [&](double px, double py, double& ox, double& oy) {
            ox = px * cs + py * sn;
            oy = -px * sn + py * cs;
        };
        auto back = [&](double px, double py, double& ox, double& oy) {
            ox = px * cs - py * sn;
            oy = px * sn + py * cs;
        };

        std::vector<TextChar> rot_chars;
        for (auto& ch : chars) {
            if (ch.rot != r) continue;
            TextChar t = ch;
            fwd(ch.x, ch.y, t.x, t.y);
            // Rotating the page-space AABB is exact on the quarter turns that
            // real documents use, and a slight over-estimate off them.
            double bx[4] = {ch.left, ch.right, ch.left, ch.right};
            double by[4] = {ch.top, ch.top, ch.bot, ch.bot};
            double rx[4], ry[4];
            for (int k = 0; k < 4; k++) fwd(bx[k], by[k], rx[k], ry[k]);
            t.left = *std::min_element(rx, rx + 4);
            t.right = *std::max_element(rx, rx + 4);
            t.top = *std::max_element(ry, ry + 4);
            t.bot = *std::min_element(ry, ry + 4);
            t.rot = 0;
            rot_chars.push_back(t);
        }

        for (auto& ln : lines_from_upright_chars(rot_chars, nullptr)) {
            // Report the line where it sits on the page, not where it sat in
            // the rotated frame: table matching and ordering work in page
            // space. The line's box maps back to a page-space AABB.
            double top = ln.y_center + ln.font_size * 0.8;
            double bot = ln.y_center - ln.font_size * 0.2;
            double bx[4] = {ln.x_left, ln.x_right, ln.x_left, ln.x_right};
            double by[4] = {top, top, bot, bot};
            double px[4], py[4];
            for (int k = 0; k < 4; k++) back(bx[k], by[k], px[k], py[k]);
            ln.x_left = *std::min_element(px, px + 4);
            ln.x_right = *std::max_element(px, px + 4);
            ln.y_center = (*std::min_element(py, py + 4) +
                           *std::max_element(py, py + 4)) / 2;
            // The geometry is page-space now, so the direction is the only
            // thing left saying this line does not belong to a body row it
            // happens to sit beside.
            ln.rot = r;
            lines.push_back(std::move(ln));
        }
    }

    return lines;
}

// ── PDF-specific types ───────────────────────────────────


}} // namespace jdoc::pdf_detail
