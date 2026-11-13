#pragma once
// pdf_cid_unicode.h — internal: Unicode values of the Adobe character
// collections' CIDs.
//
// A CID font shown through Identity-H/V with no /ToUnicode (often not even
// embedded) still names the character collection its CIDs number in
// /CIDSystemInfo: Adobe-GB1, -CNS1, -Japan1 or -Korea1. Every conforming
// reader maps those CIDs to text through Adobe's published tables, compiled
// in here from the mapping-resources-pdf CMaps (tools/gen_cid_unicode.py).

#include <cstdint>
#include <string_view>

namespace jdoc { namespace pdf_detail {

// Index of the Adobe collection with this /Ordering ("GB1", "CNS1",
// "Japan1", "Korea1"), or -1.
int adobe_cid_collection(std::string_view ordering);

// The Unicode value of cid in a collection from adobe_cid_collection, or 0
// when the collection does not map it.
uint32_t adobe_cid_to_unicode(int collection, uint32_t cid);

}} // namespace jdoc::pdf_detail
