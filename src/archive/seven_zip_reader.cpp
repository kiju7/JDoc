// seven_zip_reader.cpp - 7z reader backed by the vendored LZMA SDK
//
// The SDK pulls bytes through an ILookInStream; we adapt DataSource with a
// small ISeekInStream (read/seek delegate to read_at) and let the SDK's
// CLookToRead2 provide the look-ahead semantics on top of it.
// License: MIT

#include "archive/seven_zip_reader.h"
#include "archive/data_source.h"
#include "common/string_utils.h"

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"

#include <algorithm>
#include <cstring>

namespace jdoc {
namespace {

// ISeekInStream over DataSource. vt must stay the first member so the
// SDK's interface pointer can be cast back to the adapter.
struct SeekAdapter {
    ISeekInStream vt;
    const DataSource* src = nullptr;
    uint64_t pos = 0;
};

SRes adapter_read(ISeekInStreamPtr p, void* buf, size_t* size) {
    auto* a = const_cast<SeekAdapter*>(reinterpret_cast<const SeekAdapter*>(p));
    if (*size == 0) return SZ_OK;
    size_t n = a->src->read_at(a->pos, buf, *size);
    a->pos += n;
    *size = n;  // 0 means end of stream
    return SZ_OK;
}

SRes adapter_seek(ISeekInStreamPtr p, Int64* pos, ESzSeek origin) {
    auto* a = const_cast<SeekAdapter*>(reinterpret_cast<const SeekAdapter*>(p));
    Int64 base = 0;
    switch (origin) {
        case SZ_SEEK_SET: base = 0; break;
        case SZ_SEEK_CUR: base = static_cast<Int64>(a->pos); break;
        case SZ_SEEK_END: base = static_cast<Int64>(a->src->size()); break;
        default: return SZ_ERROR_PARAM;
    }
    Int64 target = base + *pos;
    if (target < 0) return SZ_ERROR_FAIL;
    a->pos = static_cast<uint64_t>(target);
    *pos = target;
    return SZ_OK;
}

const char* decode_error_message(SRes res) {
    switch (res) {
        case SZ_ERROR_UNSUPPORTED:
            return "unsupported 7z coder (encrypted or unknown method)";
        case SZ_ERROR_CRC:        return "member crc mismatch";
        case SZ_ERROR_INPUT_EOF:  return "truncated 7z archive";
        case SZ_ERROR_MEM:        return "out of memory decoding solid block";
        default:                  return "corrupt 7z data";
    }
}

} // anonymous namespace

struct SevenZipReader::Impl {
    Impl() { SzArEx_Init(&db); }  // keep SzArEx_Free in ~SevenZipReader safe
                                  // even when the source never opened

    SeekAdapter seek;
    CLookToRead2 look = {};
    std::vector<Byte> look_buf;
    CSzArEx db;
    ISzAlloc alloc = {SzAlloc, SzFree};
    ISzAlloc alloc_temp = {SzAllocTemp, SzFreeTemp};

    // Solid-block cache: consecutive members of one folder decode once.
    UInt32 cached_block = 0xFFFFFFFFu;
    Byte* block_buf = nullptr;
    size_t block_buf_size = 0;
};

SevenZipReader::SevenZipReader(const std::string& path)
    : impl_(new Impl) {
    auto file = std::make_unique<FileSource>(path);
    if (!file->is_open()) return;
    src_ = std::move(file);
    open_archive();
}

SevenZipReader::SevenZipReader(const uint8_t* data, size_t size)
    : impl_(new Impl) {
    src_ = std::make_unique<MemorySource>(data, size);
    open_archive();
}

SevenZipReader::~SevenZipReader() {
    release_cache();
    SzArEx_Free(&impl_->db, &impl_->alloc);
}

void SevenZipReader::open_archive() {
    static const bool crc_ready = [] { CrcGenerateTable(); return true; }();
    (void)crc_ready;

    Impl& im = *impl_;
    im.seek.vt.Read = adapter_read;
    im.seek.vt.Seek = adapter_seek;
    im.seek.src = src_.get();

    im.look_buf.resize(1 << 18);
    LookToRead2_CreateVTable(&im.look, 0 /* no direct look-ahead reads */);
    im.look.realStream = &im.seek.vt;
    im.look.buf = im.look_buf.data();
    im.look.bufSize = im.look_buf.size();
    LookToRead2_INIT(&im.look);

    if (SzArEx_Open(&im.db, &im.look.vt, &im.alloc, &im.alloc_temp) != SZ_OK)
        return;

    entries_.reserve(im.db.NumFiles);
    std::vector<UInt16> name16;
    for (UInt32 i = 0; i < im.db.NumFiles; i++) {
        if (SzArEx_IsDir(&im.db, i)) continue;

        Entry e;
        e.file_index = i;
        e.uncompressed_size = SzArEx_GetFileSize(&im.db, i);
        e.folder_index = im.db.FileToFolder[i];  // kNoFolder for empty files
        if (e.folder_index != kNoFolder)
            e.folder_unpack_size = SzAr_GetFolderUnpackSize(&im.db.db, e.folder_index);

        size_t len16 = SzArEx_GetFileNameUtf16(&im.db, i, nullptr);
        name16.resize(len16);
        SzArEx_GetFileNameUtf16(&im.db, i, name16.data());
        // len16 includes the terminating NUL; 7z stores '/' separators
        e.name = util::utf16le_to_utf8(
            reinterpret_cast<const char*>(name16.data()),
            len16 > 0 ? (len16 - 1) * 2 : 0);

        entries_.push_back(std::move(e));
    }
    open_ = true;
}

bool SevenZipReader::read_entry_streamed(const Entry& entry, const WriteFn& sink,
                                         std::string* err) const {
    if (!open_) {
        if (err) *err = "archive not open";
        return false;
    }
    Impl& im = *impl_;
    size_t offset = 0, out_size = 0;
    SRes res = SzArEx_Extract(&im.db, &im.look.vt, entry.file_index,
                              &im.cached_block, &im.block_buf, &im.block_buf_size,
                              &offset, &out_size, &im.alloc, &im.alloc_temp);
    if (res != SZ_OK) {
        if (err) *err = decode_error_message(res);
        return false;
    }

    const char* data = reinterpret_cast<const char*>(im.block_buf) + offset;
    size_t remaining = out_size;
    while (remaining > 0) {
        size_t n = std::min<size_t>(remaining, 64 * 1024);
        if (!sink(data, n)) return false;  // sink abort: err stays empty
        data += n;
        remaining -= n;
    }
    return true;
}

void SevenZipReader::release_cache() const {
    Impl& im = *impl_;
    if (im.block_buf) {
        ISzAlloc_Free(&im.alloc, im.block_buf);
        im.block_buf = nullptr;
        im.block_buf_size = 0;
        im.cached_block = 0xFFFFFFFFu;
    }
}

} // namespace jdoc
