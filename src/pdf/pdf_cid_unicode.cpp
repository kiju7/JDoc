#include "pdf_cid_unicode.h"
#include <algorithm>

namespace jdoc { namespace pdf_detail {

namespace {

struct CidAstral {
    uint16_t cid;
    uint32_t unicode;
};

struct CidCollection {
    const char* ordering;
    const uint16_t* bmp;        // indexed by CID; 0 = not in the BMP or unmapped
    uint32_t count;
    const CidAstral* astral;    // CIDs mapping outside the BMP, sorted by CID
    uint32_t astral_count;
};

#include "pdf_cid_unicode_data.inc"

} // namespace

int adobe_cid_collection(std::string_view ordering) {
    for (int i = 0; i < (int)(sizeof(kCollections) / sizeof(kCollections[0])); i++)
        if (ordering == kCollections[i].ordering) return i;
    return -1;
}

uint32_t adobe_cid_to_unicode(int collection, uint32_t cid) {
    if (collection < 0 || collection >= (int)(sizeof(kCollections) / sizeof(kCollections[0])))
        return 0;
    const CidCollection& c = kCollections[collection];
    if (cid < c.count && c.bmp[cid]) return c.bmp[cid];
    const CidAstral* end = c.astral + c.astral_count;
    const CidAstral* it = std::lower_bound(c.astral, end, cid,
        [](const CidAstral& a, uint32_t v) { return a.cid < v; });
    return it != end && it->cid == cid ? it->unicode : 0;
}

}} // namespace jdoc::pdf_detail
