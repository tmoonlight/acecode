#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace acecode {

struct ConfigRecoveryPaths {
    std::string root_dir;
    std::string last_good_path;
    std::string invalid_dir;
    std::string staging_dir;
    std::string notice_path;
};

struct ConfigRecoveryNotice {
    bool pending = false;
    std::int64_t recovered_at_ms = 0;
    std::string config_path;
    std::string invalid_backup_path;
    std::string invalid_backup_dir;
};

// Recovery artifacts are scoped beside the explicit config file so service,
// user, and test data roots never share snapshots or notices.
ConfigRecoveryPaths config_recovery_paths(const std::string& config_path);

// Persist/read the exact proven-valid JSON bytes. Snapshot writes are atomic
// and request restrictive permissions because config can contain credentials.
bool write_last_good_config(const std::string& config_path,
                            const std::string& bytes,
                            std::string* error = nullptr);
std::optional<std::string> read_last_good_config(
    const std::string& config_path,
    std::string* error = nullptr);

// Archive exact invalid bytes under config-backups/invalid. The returned path
// is collision-safe across processes and repeated failures.
std::optional<std::string> archive_invalid_config(
    const std::string& config_path,
    const std::string& bytes,
    std::string* error = nullptr);

// Write a uniquely named staging candidate used to validate last-good bytes
// before replacing the active config. Callers remove it best-effort.
std::optional<std::string> stage_config_recovery_candidate(
    const std::string& config_path,
    const std::string& bytes,
    std::string* error = nullptr);

bool write_config_recovery_notice(
    const std::string& config_path,
    const std::string& invalid_backup_path,
    std::string* error = nullptr);
ConfigRecoveryNotice read_config_recovery_notice(
    const std::string& config_path);
bool acknowledge_config_recovery_notice(
    const std::string& config_path,
    std::string* error = nullptr);

} // namespace acecode
