#pragma once
// mq_decoder.h — internal: MQ arithmetic decoder shared by the JBIG2 and
// JPEG2000 entropy coders (ITU-T T.88 Annex E == T.800 Annex C).
#include <cstddef>
#include <cstdint>

namespace jdoc { namespace pdf_detail {

// Context state is packed per slot as (Qe-table index << 1) | MPS.
class MQDecoder {
public:
    MQDecoder(const uint8_t* data, size_t len) : data_(data), len_(len) {
        c_ = static_cast<uint32_t>(byte_at(bp_)) << 16;
        byte_in();
        c_ <<= 7;
        ct_ -= 7;
        a_ = 0x8000;
    }

    int decode(uint8_t* cx) {
        int icx = *cx >> 1;
        int mps = *cx & 1;
        const QeEntry& qe = kQeTable[icx];
        int d;
        a_ -= qe.qe;
        if ((c_ >> 16) < qe.qe) {
            // LPS interval selected
            if (a_ < qe.qe) {
                a_ = qe.qe;
                d = mps;
                icx = qe.nmps;
            } else {
                a_ = qe.qe;
                d = 1 - mps;
                if (qe.sw) mps = 1 - mps;
                icx = qe.nlps;
            }
        } else {
            c_ -= static_cast<uint32_t>(qe.qe) << 16;
            if (a_ & 0x8000) return mps; // no renormalization needed
            if (a_ < qe.qe) {
                d = 1 - mps;
                if (qe.sw) mps = 1 - mps;
                icx = qe.nlps;
            } else {
                d = mps;
                icx = qe.nmps;
            }
        }
        do {
            if (ct_ == 0) byte_in();
            a_ <<= 1;
            c_ <<= 1;
            ct_--;
        } while (!(a_ & 0x8000));
        *cx = static_cast<uint8_t>((icx << 1) | mps);
        return d;
    }

private:
    struct QeEntry { uint16_t qe; uint8_t nmps, nlps, sw; };
    static constexpr QeEntry kQeTable[47] = {
        {0x5601,  1,  1, 1}, {0x3401,  2,  6, 0}, {0x1801,  3,  9, 0},
        {0x0AC1,  4, 12, 0}, {0x0521,  5, 29, 0}, {0x0221, 38, 33, 0},
        {0x5601,  7,  6, 1}, {0x5401,  8, 14, 0}, {0x4801,  9, 14, 0},
        {0x3801, 10, 14, 0}, {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0},
        {0x1C01, 13, 20, 0}, {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1},
        {0x5401, 16, 14, 0}, {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0},
        {0x3801, 19, 17, 0}, {0x3401, 20, 18, 0}, {0x3001, 21, 19, 0},
        {0x2801, 22, 19, 0}, {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0},
        {0x1C01, 25, 22, 0}, {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0},
        {0x1401, 28, 25, 0}, {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0},
        {0x0AC1, 31, 28, 0}, {0x09C1, 32, 29, 0}, {0x08A1, 33, 30, 0},
        {0x0521, 34, 31, 0}, {0x0441, 35, 32, 0}, {0x02A1, 36, 33, 0},
        {0x0221, 37, 34, 0}, {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0},
        {0x0085, 40, 37, 0}, {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0},
        {0x0015, 43, 40, 0}, {0x0009, 44, 41, 0}, {0x0005, 45, 42, 0},
        {0x0001, 45, 43, 0}, {0x5601, 46, 46, 0},
    };

    uint8_t byte_at(size_t i) const { return i < len_ ? data_[i] : 0xFF; }

    void byte_in() {
        if (byte_at(bp_) == 0xFF) {
            if (byte_at(bp_ + 1) > 0x8F) {
                // Marker or end of data: feed 1-bits forever
                c_ += 0xFF00;
                ct_ = 8;
            } else {
                bp_++;
                c_ += static_cast<uint32_t>(byte_at(bp_)) << 9;
                ct_ = 7;
            }
        } else {
            bp_++;
            c_ += static_cast<uint32_t>(byte_at(bp_)) << 8;
            ct_ = 8;
        }
    }

    const uint8_t* data_;
    size_t len_;
    size_t bp_ = 0;
    uint32_t c_ = 0, a_ = 0;
    int ct_ = 0;
};

}} // namespace jdoc::pdf_detail
