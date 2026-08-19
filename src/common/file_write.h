#pragma once
// Atomic file creation.
//
// Deliberately a declaration-only header: the Win32 implementation needs
// <windows.h>, which defines macros for ordinary words — RT_STRING, RT_FONT,
// RGB, DELETE, ERROR — that collide with identifiers the parsers use for their
// own record types. Every parser includes the image writer, so the platform
// header stays in file_write.cpp where nothing else can see it.
// License: MIT

#include <cstddef>
#include <string>

namespace jdoc { namespace util {

enum class ExclusiveWriteResult { written, exists, missing_dir, failed };

// Atomically create a new file and write `size` bytes to it. CREATE_NEW and
// O_EXCL make filename selection safe across both threads and processes; a
// separate exists-then-open sequence cannot provide that guarantee.
//
// `exists` means the name is taken — the caller picks another. `missing_dir`
// means the parent directory is not there, so a caller that can create it may
// retry. A partial write is cleaned up rather than left behind.
ExclusiveWriteResult write_exclusive_file(const std::string& path,
                                          const void* data, size_t size);

// Create the output directory under a process-wide lock. Concurrent
// create_directories() calls for the same missing path are not reliable on
// every Windows CRT, and the atomic file creation above guards the filename
// only — not the directory that has to exist first.
void ensure_output_dir(const std::string& dir);

}} // namespace jdoc::util
