#pragma once

#include "utils/semver.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acecode::upgrade {

struct PackageInfo {
    std::string target;
    std::string file;
    std::string sha256;
    std::optional<std::uintmax_t> size;
};

struct ReleaseInfo {
    std::string version;
    std::string published_at;
    std::string notes;
    std::vector<PackageInfo> packages;
};

struct UpdateManifest {
    int schema_version = 0;
    std::string latest;
    std::vector<ReleaseInfo> releases;
};

enum class SelectionStatus {
    UpdateAvailable,
    UpToDate,
    NoCompatiblePackage,
    InvalidManifest,
};

struct SelectedPackage {
    std::string version;
    SemVersion parsed_version;
    PackageInfo package;
};

struct SelectionResult {
    SelectionStatus status = SelectionStatus::InvalidManifest;
    std::optional<SelectedPackage> selected;
    std::string latest_version;
    std::string error;
};

std::optional<UpdateManifest> parse_update_manifest(const std::string& text,
                                                    std::string* error);
bool is_valid_sha256_hex(const std::string& s);
bool is_safe_package_file(const std::string& file, std::string* error = nullptr);
std::string normalize_update_base_url(const std::string& base_url);
std::string resolve_package_url(const std::string& base_url, const std::string& file);
std::string manifest_url(const std::string& base_url);
std::string current_target();
std::string manifest_target_for_platform(const std::string& platform_target);
SelectionResult select_update_package(const UpdateManifest& manifest,
                                      const std::string& current_version,
                                      const std::string& target,
                                      bool force = false);

// 回滚只开放给这一天(含当天,按清单 published_at 的 UTC 日期比较)及之后发布的
// 版本,这是产品决定的范围。
inline constexpr const char* kEarliestRollbackPublishedDate = "2026-11-01";

// 回滚资格只有这一处判定:发布日期不早于 kEarliestRollbackPublishedDate(缺日期
// 不算)、版本号严格低于正在运行的版本、并且为 target 发布了文件名安全且带合法
// sha256 的包。检查更新给每个版本打标记、真正执行回滚前拒绝,都用它。
std::optional<PackageInfo> rollback_package_for_release(const ReleaseInfo& release,
                                                        const std::string& current_version,
                                                        const std::string& target);

// 按版本号精确挑一个回滚包(版本号按语义比较,允许带 v 前缀)。找不到该版本或者
// 不满足回滚资格时 status 为 NoCompatiblePackage,error 说明原因。
SelectionResult select_rollback_package(const UpdateManifest& manifest,
                                        const std::string& current_version,
                                        const std::string& requested_version,
                                        const std::string& target);

} // namespace acecode::upgrade
