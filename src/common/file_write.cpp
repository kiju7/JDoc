// Atomic file creation — the one place <windows.h> is allowed near the parsers.
// License: MIT

#include "common/file_write.h"

#include "common/file_utils.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace jdoc { namespace util {

namespace {
// One write() call never carries more than this. macOS rejects a write larger
// than INT_MAX outright (EINVAL, and it does not attempt a partial write),
// while Linux silently shortens it and Windows accepts it; a fixed cap makes
// the loop below behave identically on all three, at one extra call per GiB.
constexpr size_t kMaxWriteChunk = static_cast<size_t>(1) << 30;
}  // namespace

ExclusiveWriteResult write_exclusive_file(const std::string& path,
                                          const void* data, size_t size) {
#ifdef _WIN32
    const auto wide_path = io_path(path);
    HANDLE file = CreateFileW(wide_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            return ExclusiveWriteResult::exists;
        if (error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND)
            return ExclusiveWriteResult::missing_dir;
        // POSIX reports every taken name as EEXIST, whatever is sitting there.
        // Windows is narrower: only a file in the way is a collision, while a
        // directory of that name comes back as ACCESS_DENIED and a locked file
        // as a sharing violation. Ask what is actually there — anything at all
        // means the name is taken and the writer should move to the next one,
        // which is what POSIX does. A genuine failure has nothing there.
        if (GetFileAttributesW(wide_path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return ExclusiveWriteResult::exists;
        return ExclusiveWriteResult::failed;
    }

    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    bool ok = true;
    while (remaining != 0) {
        const DWORD chunk =
            static_cast<DWORD>((std::min)(remaining, kMaxWriteChunk));
        DWORD written = 0;
        if (!WriteFile(file, bytes, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        bytes += written;
        remaining -= written;
    }
    if (!CloseHandle(file)) ok = false;
    if (!ok) {
        DeleteFileW(wide_path.c_str());
        return ExclusiveWriteResult::failed;
    }
#else
    int file;
    do {
        file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    } while (file < 0 && errno == EINTR);
    if (file < 0) {
        if (errno == EEXIST) return ExclusiveWriteResult::exists;
        if (errno == ENOENT) return ExclusiveWriteResult::missing_dir;
        return ExclusiveWriteResult::failed;
    }

    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    bool ok = true;
    while (remaining != 0) {
        const size_t chunk = std::min<size_t>(remaining, kMaxWriteChunk);
        const ssize_t written = ::write(file, bytes, chunk);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            ok = false;
            break;
        }
        bytes += written;
        remaining -= static_cast<size_t>(written);
    }
    if (::close(file) != 0) ok = false;
    if (!ok) {
        ::unlink(path.c_str());
        return ExclusiveWriteResult::failed;
    }
#endif
    return ExclusiveWriteResult::written;
}

void ensure_output_dir(const std::string& dir) {
    static std::mutex dir_mutex;
    std::lock_guard<std::mutex> lock(dir_mutex);
    ensure_dirs(dir);
}

}} // namespace jdoc::util
