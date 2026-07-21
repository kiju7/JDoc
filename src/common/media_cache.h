#pragma once
// Deduplicating cache for embedded media parts.
// License: MIT
//
// One media part — ppt/media/image7.png, BinData/bin0003.png — is routinely
// referenced from many places in a single document: a logo repeated on every
// slide, a figure reused across sections, a background inherited by a run of
// pages. Extracting it once per reference multiplies the disk writes, the
// decode work and (downstream) the image inspections, all for bytes that are
// identical every time.
//
// The cache keys on the part path, so the first reference does the read, the
// size measurement and the write, and every later reference reuses that
// result: the same file on disk, the same name in the markdown reference.
// Rejections are remembered too, so a part filtered out by the minimum-size
// rule is not re-read and re-measured at each later reference.
//
// The cache deliberately holds no payload. In the memory-return mode the
// bytes travel with the first reference's ImageData; a repeat reference
// carries the identifying metadata and an empty buffer, because the payload
// has already been handed to the caller under that same name.

#include "jdoc/types.h"
#include <map>
#include <string>

namespace jdoc { namespace util {

class MediaCache {
public:
    struct Lookup {
        bool known = false;      // part has been resolved before
        bool skipped = false;    // resolved, and deliberately not emitted
        ImageData image;         // canonical record (metadata only)
        std::string ref_name;    // filename used in markdown references
    };

    Lookup find(const std::string& part) const {
        auto it = entries_.find(part);
        if (it == entries_.end()) return {};
        Lookup r;
        r.known = true;
        r.skipped = it->second.skipped;
        if (!r.skipped) {
            r.image = it->second.image;
            r.ref_name = it->second.ref_name;
        }
        return r;
    }

    // Record the first successful extraction of `part`. The payload is
    // stripped: the caller keeps the only copy of the bytes.
    void insert(const std::string& part, const ImageData& img,
                const std::string& ref_name) {
        Entry e;
        e.skipped = false;
        e.image = img;
        e.image.data.clear();
        e.image.data.shrink_to_fit();
        e.image.pixels.clear();
        e.image.pixels.shrink_to_fit();
        e.ref_name = ref_name;
        entries_[part] = std::move(e);
    }

    // Record that `part` was examined and will not be emitted — missing from
    // the package, unreadable, or below the minimum size.
    void insert_skipped(const std::string& part) {
        Entry e;
        e.skipped = true;
        entries_[part] = std::move(e);
    }

    // A repeat reference to an already-extracted part, attributed to `page`.
    // Carries the shared name and on-disk path so the markdown reference
    // resolves to the one extracted file, but no second copy of the bytes.
    static ImageData reference(const ImageData& canonical, int page) {
        ImageData ref = canonical;
        ref.page_number = page;
        return ref;
    }

private:
    struct Entry {
        bool skipped = false;
        ImageData image;
        std::string ref_name;
    };
    std::map<std::string, Entry> entries_;
};

}} // namespace jdoc::util
