#pragma once

#include "global_session_catalog.hpp"
#include <filesystem>

namespace acecode::detail {

struct ProjectScanResult {
    std::vector<GlobalSessionCatalogEntry> entries;
    std::vector<std::string> archived_identities;
    std::vector<GlobalSessionCatalogError> errors;
    bool cancelled = false;
};

inline GlobalSessionCatalogError make_error(const std::string& project_dir,
                                     const std::string& workspace_hash,
                                     const std::string& stage,
                                     const std::string& message) {
    return GlobalSessionCatalogError{project_dir, workspace_hash, stage, message};
}

ProjectScanResult scan_project(
    const std::filesystem::path& project_path,
    const GlobalSessionCatalogOptions& options);

} // namespace acecode::detail
