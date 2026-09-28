#pragma once
#include "runtime_files.hpp"
#include <utility>

namespace acecode::daemon {
// Owns only this PID/GUID generation, including partial startup publication.
// A replacement generation and the Desktop-owned owner record are preserved.
class RuntimeFilesGuard {
public:
    RuntimeFilesGuard(std::int64_t pid, std::string guid, std::string run_dir = {},
                      bool remove_guid = false)
        : pid_(pid), guid_(std::move(guid)), run_dir_(std::move(run_dir)),
          remove_guid_(remove_guid) {}
    ~RuntimeFilesGuard() { cleanup(); }
    RuntimeFilesGuard(const RuntimeFilesGuard&) = delete;
    RuntimeFilesGuard& operator=(const RuntimeFilesGuard&) = delete;
    RuntimeFilesGuard(RuntimeFilesGuard&& other) noexcept
        : pid_(other.pid_), guid_(std::move(other.guid_)), run_dir_(std::move(other.run_dir_)),
          remove_guid_(other.remove_guid_), active_(std::exchange(other.active_, false)) {}
    RuntimeFilesGuard& operator=(RuntimeFilesGuard&& other) noexcept {
        if (this != &other) {
            cleanup();
            pid_ = other.pid_;
            guid_ = std::move(other.guid_);
            run_dir_ = std::move(other.run_dir_);
            remove_guid_ = other.remove_guid_;
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }
    void cleanup() noexcept {
        if (!std::exchange(active_, false)) return;
        try { cleanup_runtime_files_if_owned(pid_, guid_, run_dir_, remove_guid_); }
        catch (...) {} // Best-effort cleanup must not mask startup/exit errors.
    }
private:
    std::int64_t pid_;
    std::string guid_;
    std::string run_dir_;
    bool remove_guid_;
    bool active_ = true;
};
} // namespace acecode::daemon
