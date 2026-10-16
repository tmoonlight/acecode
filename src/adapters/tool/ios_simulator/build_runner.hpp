#pragma once

// xcodebuild for the iOS Simulator using the selected Xcode (no xcode-select
// dependency). Builds into a per-project DerivedData under the ACECode data
// directory and resolves the produced .app from -showBuildSettings.

#include <nlohmann/json.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace acecode::ios_simulator {

struct BuildRequest {
    std::string project;        // absolute .xcodeproj (exclusive with workspace)
    std::string workspace;      // absolute .xcworkspace
    std::string scheme;
    std::string configuration = "Debug";
    std::string udid;           // destination simulator; empty = generic iOS Simulator
    std::string derived_data;   // absolute DerivedData directory
};

struct BuildProduct {
    std::string app_path;
    std::string bundle_id;
    std::string product_name;
};

struct BuildResult {
    bool ok = false;
    bool cancelled = false;
    bool timed_out = false;
    long long duration_ms = 0;
    std::optional<BuildProduct> product;
    std::vector<std::string> errors;   // "error:" lines, deduplicated
    std::string log_tail;              // last lines of the build log
    std::string message;               // top-level failure description
};

// --- Pure helpers -------------------------------------------------------------
std::vector<std::string> xcodebuild_target_args(const BuildRequest& request);
std::vector<std::string> xcodebuild_build_args(const BuildRequest& request);
std::string derived_data_for(const std::string& base_dir, const BuildRequest& request);
std::vector<std::string> extract_build_errors(const std::string& log, std::size_t max_errors = 20);
std::string tail_lines(const std::string& text, std::size_t max_lines);
// Picks the application target from `xcodebuild -showBuildSettings -json`.
std::optional<BuildProduct> parse_build_settings(const nlohmann::json& settings);
// Schemes from `xcodebuild -list -json`.
std::vector<std::string> parse_scheme_list(const nlohmann::json& list);

// --- Execution ---------------------------------------------------------------
BuildResult run_xcodebuild(const std::string& developer_dir, const BuildRequest& request,
                           const std::atomic<bool>* abort_flag);
std::vector<std::string> list_schemes(const std::string& developer_dir, const BuildRequest& request,
                                      std::string* error);
// CFBundleIdentifier of a built .app (reads its Info.plist with plutil).
std::string bundle_id_of_app(const std::string& app_path);

nlohmann::json to_json(const BuildResult& result);

} // namespace acecode::ios_simulator
