#include "session_file_reader.hpp"
#include "session_load_metrics.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace acecode {

SessionFileReader::SessionFileReader(std::string path) : path_(std::move(path)) {
#ifdef _WIN32
    handle_.reset(::CreateFileW(path_from_utf8(path_).c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle_) return;
    ++session_read_metrics().files;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle_.get(), &info)) {
        handle_.reset();
        return;
    }
    size_ = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    identity_ = std::to_string(info.dwVolumeSerialNumber) + ":" +
                std::to_string(info.nFileIndexHigh) + ":" + std::to_string(info.nFileIndexLow) + ":" +
                std::to_string(info.ftCreationTime.dwHighDateTime) + ":" +
                std::to_string(info.ftCreationTime.dwLowDateTime);
#else
    handle_.reset(::open(path_.c_str(), O_RDONLY | O_CLOEXEC));
    if (!handle_) return;
    ++session_read_metrics().files;
    struct stat info{};
    if (::fstat(handle_.get(), &info) != 0 || info.st_size < 0) {
        handle_.reset();
        return;
    }
    size_ = static_cast<std::uint64_t>(info.st_size);
    identity_ = std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
#endif
}

bool SessionFileReader::valid() const { return static_cast<bool>(handle_); }

std::string SessionFileReader::read(std::uint64_t offset, std::uint64_t count) const {
    if (!valid() || offset > size_) throw HistorySnapshotChanged();
    count = (std::min)(count, size_ - offset);
    if (count > std::numeric_limits<std::size_t>::max()) throw std::length_error("session read too large");
    std::string result(static_cast<std::size_t>(count), '\0');
    std::uint64_t read = 0;
    while (read < count) {
        const auto wanted = static_cast<unsigned long>((std::min)(count - read, std::uint64_t{1024 * 1024}));
#ifdef _WIN32
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset + read);
        DWORD received = 0;
        if (!::SetFilePointerEx(handle_.get(), position, nullptr, FILE_BEGIN) ||
            !::ReadFile(handle_.get(), result.data() + read, wanted, &received, nullptr) ||
            received == 0) throw HistorySnapshotChanged();
#else
        const auto received = ::pread(handle_.get(), result.data() + read, wanted, static_cast<off_t>(offset + read));
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) throw HistorySnapshotChanged();
#endif
        read += received;
        session_read_metrics().bytes += received;
    }
    return result;
}

std::string SessionFileReader::prefix() const {
    return read(0, (std::min)(size_, std::uint64_t{4096}));
}

bool SessionFileReader::unchanged(const std::string& original_prefix) const {
    SessionFileReader current(path_);
    return current.valid() && current.identity() == identity_ && current.size() >= size_ &&
           current.read(0, original_prefix.size()) == original_prefix;
}

} // namespace acecode
