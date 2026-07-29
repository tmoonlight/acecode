#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode::feedback {

constexpr std::size_t kDefaultLogTailBytes = 512 * 1024;

// 一个待打包的日志文件。诊断一次 desktop 问题通常要同时看 desktop 壳日志与
// 处理该请求的 daemon 日志,所以打包层接受任意多个来源而不是单个文件。
struct FeedbackLogSource {
    std::filesystem::path path;
    // zip 内条目名;留空时按文件名推导为 "logs/<filename>.tail.txt"。
    std::string entry_name;
    // 0 = 沿用 FeedbackPackageRequest::max_log_bytes。
    std::size_t max_bytes = 0;
};

struct FeedbackLogInclusion {
    std::string entry_name;
    std::string source_path;
    bool included = false;
    std::size_t tail_bytes = 0;
};

struct FeedbackPackageRequest {
    std::string source;
    std::string feedback_text;
    std::string session_id;
    std::filesystem::path session_jsonl_path;
    std::string workspace_hash;
    std::vector<FeedbackLogSource> logs;
    std::filesystem::path output_dir;
    std::string created_at;
    std::string acecode_version;
    std::string platform;
    std::string computer_name;
    std::string login_name;
    std::size_t max_log_bytes = kDefaultLogTailBytes;
};

struct FeedbackPackageResult {
    bool ok = false;
    std::filesystem::path package_path;
    std::string package_filename;
    std::string error;
    // 任意一个日志被打进包即为 true;log_tail_bytes 是所有日志尾巴的字节和。
    bool log_included = false;
    std::size_t log_tail_bytes = 0;
    std::vector<FeedbackLogInclusion> logs;
    std::vector<std::string> included_files;
};

struct FeedbackUploadRequest {
    std::string upload_url;
    std::filesystem::path package_path;
    std::string package_filename;
    int timeout_ms = 30000;
};

struct FeedbackUploadResult {
    bool ok = false;
    long status_code = 0;
    std::string response_body;
    std::string error;
};

std::string sanitize_feedback_filename_component(const std::string& value);
std::string make_feedback_package_filename(const std::string& session_id,
                                           const std::string& created_at,
                                           const std::string& platform = {},
                                           const std::string& computer_name = {},
                                           const std::string& login_name = {});
// 找 logs_dir 下 "<base_name>-<date>.log" 里最近修改的一个(Logger 的滚动命名)。
std::optional<std::filesystem::path> latest_rotated_log_path(
    const std::filesystem::path& logs_dir, const std::string& base_name);
std::optional<std::filesystem::path> latest_desktop_log_path(
    const std::filesystem::path& logs_dir);

// daemon / desktop 反馈默认附带的运行时日志:desktop 壳日志 + daemon 日志,
// 各取最近一个滚动文件,缺失的静默跳过。
std::vector<FeedbackLogSource> collect_runtime_log_sources(
    const std::filesystem::path& logs_dir);

FeedbackPackageResult build_feedback_package(const FeedbackPackageRequest& request);
FeedbackUploadResult upload_feedback_package(const FeedbackUploadRequest& request);

} // namespace acecode::feedback
