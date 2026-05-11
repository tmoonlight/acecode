#pragma once

#include "../daemon/platform.hpp"

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

} // namespace acecode
