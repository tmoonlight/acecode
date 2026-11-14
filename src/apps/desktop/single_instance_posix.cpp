// POSIX 实现:`flock` 在 `~/.acecode/run/acecode-desktop.lock` 拿独占锁。
//
// 设计见 single_instance.hpp 头注。macOS 通过 distributed notification 唤醒
// 已有窗口;Linux 用每用户 Unix datagram，由 GTK 主线程消费。
//
// 只在非 Windows 平台编译;_WIN32 走 single_instance_win.cpp。

#include "single_instance.hpp"

#ifndef _WIN32

#include "utils/logger.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/file.h>
#include <unistd.h>
#include <utility>
#ifndef __APPLE__
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#endif

#ifdef __APPLE__
#  include <CoreFoundation/CoreFoundation.h>
#endif

namespace fs = std::filesystem;

namespace acecode::desktop {

namespace {

std::string default_lock_path() {
    fs::path run_dir = acecode::path_from_utf8(acecode::get_run_dir());
    std::error_code ec;
    fs::create_directories(run_dir, ec);
    return acecode::path_to_utf8(run_dir / "acecode-desktop.lock");
}

} // namespace

#ifndef __APPLE__
namespace {
sockaddr_un focus_socket_address() {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    // Linux abstract sockets disappear with the owning fd and require no
    // unlink in a shared temporary directory. UID isolates user sessions.
    const auto name = "acecode-focus-" + std::to_string(::geteuid());
    std::memcpy(address.sun_path + 1, name.c_str(), name.size());
    return address;
}
}
platform::UniqueFd open_desktop_focus_socket() {
    platform::UniqueFd fd(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!fd) return {};
    const auto address = focus_socket_address();
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) return {};
    return fd;
}
#endif

SingleInstance::SingleInstance(std::string lock_name)
    : lock_name_(std::move(lock_name)) {}

SingleInstance::~SingleInstance() {
    release();
}

bool SingleInstance::try_acquire() {
    if (acquired_) return true;
    lock_path_ = lock_name_.empty() ? default_lock_path() : lock_name_;
    platform::UniqueFd fd(::open(lock_path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
    if (!fd) {
        LOG_WARN(std::string("[desktop] single_instance: open(") + lock_path_ +
                 ") failed: " + std::strerror(errno));
        return false;
    }
    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        // EWOULDBLOCK / EAGAIN = 别的进程持有锁。其它 errno 视为环境异常,同样
        // 返回 false 让上层走 focus_existing_instance / exit 路径。
        LOG_INFO(std::string("[desktop] single_instance: another instance holds ") +
                 lock_path_ + " (errno=" + std::to_string(errno) + ")");
        return false;
    }
    // flock 成功 — 我是第一个。fd 持续打开,close 时锁自动释放(POSIX flock 语义)。
    native_handle_ = std::move(fd);
    acquired_ = true;
    LOG_INFO("[desktop] single_instance: lock acquired at " + lock_path_);
    return true;
}

void SingleInstance::release() {
    if (!acquired_) return;
    const int fd = native_handle_.get();
    if (fd >= 0) {
        ::flock(fd, LOCK_UN);
    }
    native_handle_.reset();
    acquired_ = false;
    // 故意不 unlink lock 文件 — 留下让下次启动复用,避免 TOCTOU。
}

bool focus_existing_instance() {
#ifdef __APPLE__
    CFNotificationCenterPostNotification(
        CFNotificationCenterGetDistributedCenter(),
        CFSTR("dev.acecode.desktop.focusExisting.v1"),
        nullptr,
        nullptr,
        true);
    LOG_INFO("[desktop] focus_existing_instance: posted macOS distributed focus notification");
    return true;
#else
    platform::UniqueFd fd(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (!fd) return false;
    const auto address = focus_socket_address();
    return ::sendto(fd.get(), "1", 1, 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 1;
#endif
}

} // namespace acecode::desktop

#endif // !_WIN32
