#pragma once

#include "config/config.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acecode::upgrade {

class DiagnosticLog;

enum class UpdateCheckStatus {
    UpdateAvailable,
    UpToDate,
    StoreManaged,
    NoCompatiblePackage,
    InvalidConfig,
    UnsupportedTarget,
    ManifestUnavailable,
    ManifestInvalid,
};

struct UpdateReleaseSummary {
    std::string version;
    std::string published_at;
    std::string notes;
    // rollback_package_for_release 判定可以回滚到这个版本(见 manifest.hpp)。
    bool rollback_available = false;
    // 可回滚时,本平台回滚包在清单里声明的大小。
    std::optional<std::uintmax_t> rollback_package_size;
};

struct UpdateCheckResult {
    UpdateCheckStatus status = UpdateCheckStatus::ManifestUnavailable;
    std::string current_version;
    std::string latest_version;
    std::string target;
    std::string manifest_url;
    std::string package_file;
    std::string package_url;
    std::optional<std::uintmax_t> package_size;
    std::vector<UpdateReleaseSummary> releases;
    long http_status = 0;
    std::string error;
    std::string log_path;
    std::string log_error;

    bool update_available() const {
        return status == UpdateCheckStatus::UpdateAvailable;
    }
};

const char* update_check_status_name(UpdateCheckStatus status);

UpdateCheckResult check_for_update(const AppConfig& config,
                                   const std::string& current_version,
                                   DiagnosticLog* diagnostics = nullptr);

} // namespace acecode::upgrade
