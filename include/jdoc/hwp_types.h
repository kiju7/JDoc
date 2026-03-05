#pragma once
// jdoc - HWP/HWPX specific data structures
// License: MIT

#include "common/binary_utils.h"
#include "common/string_utils.h"
#include <string>
#include <vector>
#include <cstdint>

namespace jdoc { namespace hwp {

// ── Character Shape Info (from DocInfo / header.xml) ────────
struct CharShapeInfo {
    int id = 0;
    int font_ref[7] = {};
    int32_t height = 1000;       // font size in HWP units (1/100 pt)
    bool bold = false;
    bool italic = false;
    std::string text_color = "#000000";

    double font_size_pt() const { return height / 100.0; }
};

// ── Paragraph Shape Info ────────────────────────────────────
struct ParaShapeInfo {
    int id = 0;
    int outline_level = 0;       // 0 = body, 1-7 = heading levels
    std::string alignment;       // "JUSTIFY", "LEFT", "CENTER", "RIGHT"
};

// ── Font Face Info ──────────────────────────────────────────
struct FaceNameInfo {
    int id = 0;
    std::string name;
    std::string lang;
};

// ── BinData Reference ───────────────────────────────────────
struct BinDataRef {
    std::string id;
    std::string href;
    std::string media_type;
};

// ── HWP Record Tag Constants (for HWP binary format) ───────
enum HWPTag : uint16_t {
    BEGIN               = 0x10,
    DOCUMENT_PROPERTIES = 0x10,
    ID_MAPPINGS         = 0x11,
    BIN_DATA            = 0x12,
    FACE_NAME           = 0x13,
    BORDER_FILL         = 0x14,
    CHAR_SHAPE          = 0x15,
    TAB_DEF             = 0x16,
    NUMBERING           = 0x17,
    BULLET              = 0x18,
    PARA_SHAPE          = 0x19,
    STYLE               = 0x1A,

    PARA_HEADER         = 0x42,
    PARA_TEXT           = 0x43,
    PARA_CHAR_SHAPE     = 0x44,
    PARA_LINE_SEG       = 0x45,
    PARA_RANGE_TAG      = 0x46,
    CTRL_HEADER         = 0x47,
    LIST_HEADER         = 0x48,
    PAGE_DEF            = 0x49,

    SHAPE_COMPONENT         = 0x4C,
    TABLE                   = 0x4D,
    SHAPE_COMPONENT_LINE    = 0x4E,
    SHAPE_COMPONENT_RECT    = 0x4F,
    SHAPE_COMPONENT_ELLIPSE = 0x50,
    SHAPE_COMPONENT_ARC     = 0x51,
    SHAPE_COMPONENT_POLYGON = 0x52,
    SHAPE_COMPONENT_CURVE   = 0x53,
    SHAPE_COMPONENT_OLE     = 0x55,
    SHAPE_COMPONENT_PICTURE = 0x56,
    SHAPE_COMPONENT_CONTAINER = 0x57,
    CTRL_DATA               = 0x58,
    EQEDIT                  = 0x59,
    SHAPE_COMPONENT_TEXTART = 0x5B,
    MEMO_LIST               = 0x5D,
};

// ── HWP Record Header (binary format) ──────────────────────
struct RecordHeader {
    uint16_t tag_id = 0;
    uint16_t level = 0;
    uint32_t size = 0;
};

inline RecordHeader parse_record_header(const uint8_t* data, size_t& offset) {
    uint32_t val = util::read_u32_le(data + offset);
    offset += 4;
    RecordHeader h;
    h.tag_id = val & 0x3FF;
    h.level  = (val >> 10) & 0x3FF;
    h.size   = (val >> 20) & 0xFFF;
    if (h.size == 0xFFF) {
        h.size = util::read_u32_le(data + offset);
        offset += 4;
    }
    return h;
}

// ── HWP Character Type Classification ──────────────────────
enum class HWPCharType { Normal, ControlChar, ControlExtend, ControlInline };

inline HWPCharType classify_hwp_char(uint16_t code) {
    if (code > 31) return HWPCharType::Normal;
    switch (code) {
        case 0: case 1: case 2: case 3:
        case 11: case 12:
        case 14: case 15: case 16: case 17: case 18:
        case 21: case 22: case 23:
            return HWPCharType::ControlExtend;
        case 4: case 5: case 6: case 7: case 8:
            return HWPCharType::ControlInline;
        default:
            return HWPCharType::ControlChar;
    }
}

}} // namespace jdoc::hwp
