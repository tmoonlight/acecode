#pragma once

#include "platform/process/os_process.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace acecode {

struct SessionWriterLeaseInfo {
    daemon::pid_t_compat pid = 0;
    std::string cwd;
    std::string project_dir;
    std::string session_id;
    std::string surface;
    std::int64_t updated_at_ms = 0;
};

struct SessionWriterLeaseResult {
    enum class Status {
        Acquired,
        Conflict,
        Error,
    };

    Status status = Status::Error;
    SessionWriterLeaseInfo owner;
    std::string path;
    std::string error;
    bool stale_recovered = false;
};

class SessionWriterLease {
public:
    static constexpr std::int64_t kDefaultStaleMs = 30000;

    static std::string lease_path(const std::string& project_dir,
                                  const std::string& session_id);

    static std::optional<SessionWriterLeaseInfo> read(const std::string& project_dir,
                                                       const std::string& session_id);

    static SessionWriterLeaseResult acquire(const std::string& project_dir,
                                            const std::string& session_id,
                                            const std::string& cwd,
                                            const std::string& surface,
                                            daemon::pid_t_compat pid = 0,
                                            std::int64_t now_ms = 0,
                                            std::int64_t stale_ms = kDefaultStaleMs);

    static bool refresh(const std::string& project_dir,
                        const std::string& session_id,
                        daemon::pid_t_compat pid = 0,
                        std::int64_t now_ms = 0);

    static bool release(const std::string& project_dir,
                        const std::string& session_id,
                        daemon::pid_t_compat pid = 0);

    static void remove(const std::string& project_dir,
                       const std::string& session_id);

    static std::int64_t now_ms();
};

// Move-only ownership of a writer lease. Reacquisition of the same session
// reuses this object; releasing it never writes transcript/metadata timestamps.
class WriterLease {
public:
    WriterLease(std::string project_dir, std::string session_id);
    ~WriterLease();
    WriterLease(const WriterLease&) = delete;
    WriterLease& operator=(const WriterLease&) = delete;
    WriterLease(WriterLease&& other) noexcept;
    WriterLease& operator=(WriterLease&& other) noexcept;
    SessionWriterLeaseResult acquire(const std::string& cwd, const std::string& surface);
    bool refresh();
    bool matches(const std::string& project_dir, const std::string& session_id) const;
    void reset() noexcept;

private:
    std::string project_dir_;
    std::string session_id_;
    daemon::pid_t_compat pid_;
    bool active_ = false;
};

} // namespace acecode
