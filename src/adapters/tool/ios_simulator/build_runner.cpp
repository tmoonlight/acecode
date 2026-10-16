#include "build_runner.hpp"

#include "platform/process/process_runner.hpp"
#include "utils/sha1.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace acecode::ios_simulator {
namespace {

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && text[start] == ' ') ++start;
    return text.substr(start);
}

HookProcessResult run_tool(const std::string& developer_dir, const std::string& tool,
                           const std::vector<std::string>& args, int timeout_ms,
                           const std::atomic<bool>* abort_flag, std::size_t max_bytes) {
    HookProcessOptions options;
    options.timeout_ms = timeout_ms;
    options.abort_flag = abort_flag;
    options.max_stdout_bytes = max_bytes;
    options.max_stderr_bytes = max_bytes;
    options.terminate_process_tree = true;
    options.append_output_truncation_notice = false;
    return run_hook_process({developer_dir + "/usr/bin/" + tool, args}, "", "", options);
}

} // namespace

std::vector<std::string> xcodebuild_target_args(const BuildRequest& request) {
    std::vector<std::string> args;
    if (!request.workspace.empty()) {
        args.push_back("-workspace");
        args.push_back(request.workspace);
    } else if (!request.project.empty()) {
        args.push_back("-project");
        args.push_back(request.project);
    }
    if (!request.scheme.empty()) {
        args.push_back("-scheme");
        args.push_back(request.scheme);
    }
    args.push_back("-configuration");
    args.push_back(request.configuration.empty() ? "Debug" : request.configuration);
    args.push_back("-destination");
    args.push_back(request.udid.empty() ? "generic/platform=iOS Simulator"
                                        : "platform=iOS Simulator,id=" + request.udid);
    if (!request.derived_data.empty()) {
        args.push_back("-derivedDataPath");
        args.push_back(request.derived_data);
    }
    return args;
}

std::vector<std::string> xcodebuild_build_args(const BuildRequest& request) {
    auto args = xcodebuild_target_args(request);
    args.insert(args.end(), {"-quiet", "-skipMacroValidation", "-skipPackagePluginValidation", "build"});
    return args;
}

std::string derived_data_for(const std::string& base_dir, const BuildRequest& request) {
    const auto key = request.workspace.empty() ? request.project : request.workspace;
    return base_dir + "/" + sha1_hex(key).substr(0, 16) + "/DerivedData";
}

std::vector<std::string> extract_build_errors(const std::string& log, std::size_t max_errors) {
    std::vector<std::string> errors;
    std::istringstream lines(log);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.find("error:") == std::string::npos) continue;
        if (line.size() > 600) line = line.substr(0, 600) + "…";
        if (std::find(errors.begin(), errors.end(), line) != errors.end()) continue;
        errors.push_back(line);
        if (errors.size() >= max_errors) break;
    }
    return errors;
}

std::string tail_lines(const std::string& text, std::size_t max_lines) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!trim(line).empty()) lines.push_back(line);
    }
    const auto first = lines.size() > max_lines ? lines.size() - max_lines : 0;
    std::string out;
    for (auto index = first; index < lines.size(); ++index) {
        if (!out.empty()) out.push_back('\n');
        out += lines[index];
    }
    return out;
}

std::optional<BuildProduct> parse_build_settings(const nlohmann::json& settings) {
    if (!settings.is_array()) return std::nullopt;
    std::optional<BuildProduct> fallback;
    for (const auto& target : settings) {
        if (!target.is_object() || !target.contains("buildSettings") || !target["buildSettings"].is_object()) continue;
        const auto& build = target["buildSettings"];
        const auto wrapper = build.value("WRAPPER_EXTENSION", std::string());
        const auto product_type = build.value("PRODUCT_TYPE", std::string());
        const auto dir = build.value("BUILT_PRODUCTS_DIR", std::string());
        const auto name = build.value("FULL_PRODUCT_NAME", std::string());
        if (dir.empty() || name.empty()) continue;
        BuildProduct product{dir + "/" + name, build.value("PRODUCT_BUNDLE_IDENTIFIER", std::string()),
                             build.value("PRODUCT_NAME", std::string())};
        if (product_type == "com.apple.product-type.application") return product;
        if (wrapper == "app" && !fallback) fallback = product;
    }
    return fallback;
}

std::vector<std::string> parse_scheme_list(const nlohmann::json& list) {
    std::vector<std::string> schemes;
    for (const char* key : {"workspace", "project"}) {
        if (!list.contains(key) || !list[key].is_object()) continue;
        const auto& entry = list[key];
        if (!entry.contains("schemes") || !entry["schemes"].is_array()) continue;
        for (const auto& scheme : entry["schemes"]) {
            if (scheme.is_string() &&
                std::find(schemes.begin(), schemes.end(), scheme.get<std::string>()) == schemes.end())
                schemes.push_back(scheme.get<std::string>());
        }
    }
    return schemes;
}

BuildResult run_xcodebuild(const std::string& developer_dir, const BuildRequest& request,
                           const std::atomic<bool>* abort_flag) {
    BuildResult result;
    const auto started = std::chrono::steady_clock::now();
    auto build = run_tool(developer_dir, "xcodebuild", xcodebuild_build_args(request), 45 * 60 * 1000, abort_flag,
                          4 * 1024 * 1024);
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    result.cancelled = build.aborted;
    result.timed_out = build.timed_out;
    const auto log = build.stdout_text + "\n" + build.stderr_text;
    result.errors = extract_build_errors(log);
    result.log_tail = tail_lines(log, 40);
    if (!build.started) {
        result.message = "Could not start xcodebuild: " + build.error;
        return result;
    }
    if (build.aborted) { result.message = "Build cancelled."; return result; }
    if (build.timed_out) { result.message = "Build timed out after 45 minutes."; return result; }
    if (build.exit_code != 0) {
        result.message = "xcodebuild failed with exit code " + std::to_string(build.exit_code) + ".";
        return result;
    }
    auto args = xcodebuild_target_args(request);
    args.insert(args.end(), {"-showBuildSettings", "-json"});
    auto settings = run_tool(developer_dir, "xcodebuild", args, 180000, abort_flag, 32 * 1024 * 1024);
    auto json = nlohmann::json::parse(settings.stdout_text, nullptr, false);
    result.product = json.is_discarded() ? std::nullopt : parse_build_settings(json);
    if (!result.product) {
        result.message = "The build succeeded but no application product was found for this scheme.";
        return result;
    }
    if (result.product->bundle_id.empty()) result.product->bundle_id = bundle_id_of_app(result.product->app_path);
    result.ok = true;
    return result;
}

std::vector<std::string> list_schemes(const std::string& developer_dir, const BuildRequest& request,
                                      std::string* error) {
    std::vector<std::string> args;
    if (!request.workspace.empty()) args = {"-list", "-json", "-workspace", request.workspace};
    else args = {"-list", "-json", "-project", request.project};
    auto listed = run_tool(developer_dir, "xcodebuild", args, 120000, nullptr, 4 * 1024 * 1024);
    auto json = nlohmann::json::parse(listed.stdout_text, nullptr, false);
    if (listed.exit_code != 0 || json.is_discarded()) {
        if (error) *error = tail_lines(listed.stderr_text.empty() ? listed.stdout_text : listed.stderr_text, 10);
        return {};
    }
    return parse_scheme_list(json);
}

std::string bundle_id_of_app(const std::string& app_path) {
    HookProcessOptions options;
    options.timeout_ms = 10000;
    auto result = run_hook_process(
        {"/usr/bin/plutil", {"-extract", "CFBundleIdentifier", "raw", "-o", "-", app_path + "/Info.plist"}}, "", "",
        options);
    return result.exit_code == 0 ? trim(result.stdout_text) : std::string();
}

nlohmann::json to_json(const BuildResult& result) {
    nlohmann::json out = {{"ok", result.ok}, {"cancelled", result.cancelled}, {"timed_out", result.timed_out},
                          {"duration_ms", result.duration_ms}, {"errors", result.errors},
                          {"message", result.message}};
    if (result.product)
        out["product"] = {{"app_path", result.product->app_path}, {"bundle_id", result.product->bundle_id},
                          {"name", result.product->product_name}};
    return out;
}

} // namespace acecode::ios_simulator
