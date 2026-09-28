#include "owner_lock.hpp"
#include "utils/utf8_path.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace acecode::channels {
struct OwnerLock::Impl {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
};
OwnerLock::OwnerLock() : impl_(std::make_unique<Impl>()) {}
OwnerLock::~OwnerLock() { release(); }
bool OwnerLock::acquire(const std::filesystem::path& path) {
    release();
    std::filesystem::create_directories(path.parent_path());
#ifdef _WIN32
    impl_->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED offset{};
    if (LockFileEx(impl_->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &offset)) return true;
#else
    impl_->fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (impl_->fd >= 0 && ::flock(impl_->fd, LOCK_EX | LOCK_NB) == 0) return true;
#endif
    release(); return false;
}
void OwnerLock::release() {
#ifdef _WIN32
    if (impl_->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->handle); impl_->handle = INVALID_HANDLE_VALUE;
    }
#else
    if (impl_->fd >= 0) { ::close(impl_->fd); impl_->fd = -1; }
#endif
}
} // namespace acecode::channels
