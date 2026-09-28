#include "append_file.hpp"

#include <limits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace acecode {

AppendFile::~AppendFile() { close(); }

bool AppendFile::open(const std::filesystem::path& path) {
    close();
#ifdef _WIN32
    // Do not request GENERIC_WRITE: CRT append streams seek before WriteFile,
    // allowing another process to write into the same offset between the two.
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    handle_ = reinterpret_cast<std::intptr_t>(file);
#else
    handle_ = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
#endif
    return is_open();
}

void AppendFile::close() {
    if (!is_open()) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    ::close(static_cast<int>(handle_));
#endif
    handle_ = -1;
}

bool AppendFile::append(std::string_view record) {
    if (!is_open()) return false;
    if (record.empty()) return true;
#ifdef _WIN32
    if (record.size() > (std::numeric_limits<DWORD>::max)()) return false;
    DWORD written = 0;
    return WriteFile(reinterpret_cast<HANDLE>(handle_), record.data(),
        static_cast<DWORD>(record.size()), &written, nullptr) != FALSE &&
        written == record.size();
#else
    if (record.size() > static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)())) {
        return false;
    }
    ssize_t written;
    do {
        written = ::write(static_cast<int>(handle_), record.data(), record.size());
    } while (written < 0 && errno == EINTR);
    // A short write (e.g. disk full) must not become separately appended pieces
    // that can be interleaved with another process's complete log record.
    return written >= 0 && static_cast<std::size_t>(written) == record.size();
#endif
}

} // namespace acecode
