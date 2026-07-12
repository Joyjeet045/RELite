#pragma once

#include <string>

namespace db::backend {

// Forces the operating system's file cache for `path` to stable storage
// (fsync on POSIX, FlushFileBuffers on Windows). Best-effort: returns true on
// success. The caller must have already flushed its own userspace buffer
// (e.g. std::fstream::flush()) before calling this. Because the OS page cache
// is keyed by file, syncing through a freshly opened handle still flushes the
// data written through another handle to the same path.
bool syncFileToDisk(const std::string& path);

}  // namespace db::backend
