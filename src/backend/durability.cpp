#include "backend/durability.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace db::backend {

bool syncFileToDisk(const std::string& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileA(
        path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return ok;
#else
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        return false;
    }
    bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#endif
}

}  // namespace db::backend
