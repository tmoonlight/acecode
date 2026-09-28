#pragma once

#include "platform/unique_resource.hpp"

namespace acecode::platform {

struct FdTraits {
    using handle_type = int;
    static int invalid() noexcept { return -1; }
    static bool valid(int fd) noexcept { return fd >= 0; }
    static void close(int fd) noexcept;
};
using UniqueFd = UniqueResource<FdTraits>;

#ifdef _WIN32
struct HandleTraits {
    using handle_type = void*;
    static void* invalid() noexcept { return nullptr; }
    static bool valid(void* handle) noexcept;
    static void close(void* handle) noexcept;
};
struct LocalMemTraits {
    using handle_type = void*;
    static void* invalid() noexcept { return nullptr; }
    static bool valid(void* handle) noexcept { return handle != nullptr; }
    static void close(void* handle) noexcept;
};
struct SidTraits : LocalMemTraits {
    static void close(void* handle) noexcept;
};
using UniqueHandle = UniqueResource<HandleTraits>;
using UniqueLocalMem = UniqueResource<LocalMemTraits>;
using UniqueSid = UniqueResource<SidTraits>;
#endif

// Owns a child, including termination/reaping, not just its OS handle.
// The caller must stop concurrent pipe I/O before resetting the process.
class UniqueProcess {
public:
#ifdef _WIN32
    using native_type = void*;
    static native_type invalid() noexcept { return nullptr; }
#else
    using native_type = int;
    static native_type invalid() noexcept { return -1; }
#endif
    UniqueProcess() noexcept = default;
    explicit UniqueProcess(native_type process) noexcept : process_(process) {}
    ~UniqueProcess() { reset(); }
    UniqueProcess(const UniqueProcess&) = delete;
    UniqueProcess& operator=(const UniqueProcess&) = delete;
    UniqueProcess(UniqueProcess&& other) noexcept : process_(other.release()) {}
    UniqueProcess& operator=(UniqueProcess&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    native_type get() const noexcept { return process_; }
    native_type release() noexcept { return std::exchange(process_, invalid()); }
    bool wait_exit(int timeout_ms) noexcept;
    void kill() noexcept;
    void reset(native_type process = invalid()) noexcept;

private:
    native_type process_ = invalid();
};

} // namespace acecode::platform
