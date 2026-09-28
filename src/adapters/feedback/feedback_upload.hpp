#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode::feedback {

constexpr std::size_t kDefaultLogTailBytes = 512 * 1024;

// 反馈附带的升级记录窗口:「最近三天」按文件最后修改时间算,72 小时。
// 用修改时间而不是文件名里的日期,是因为文件名日期是 UTC 起始日,跨午夜的操作
// 与东八区用户的「今天」对不上;修改时间只看这份日志最近有没有写过。
constexpr std::chrono::hours kRecentUpgradeLogWindow{72};

// 一个待打包的日志文件。诊断一次 desktop 问题通常要同时看 desktop 壳日志与
// 处理该请求的 daemon 日志,所以打包层接受任意多个来源而不是单个文件。
struct FeedbackLogSource {
    std::filesystem::path path;
    // zip 内条目名;留空时按文件名推导为 "logs/<filename>.tail.txt"。
    std::string entry_name;
    // 0 = 沿用 FeedbackPackageRequest::max_log_bytes。
    std::size_t max_bytes = 0;
};

// 一组按顺序拼接进同一个 zip 条目的日志文件。升级诊断日志每个进程一个文件
// (upgrade-<date>-<pid>.log),三天里能攒几十个,逐个打包读起来很碎;合并后
// 按时间顺序读就是一条完整的升级时间线(每条记录自带 operation_id = pid + 起始时间,
// 不需要额外的分隔行也能对回原文件)。
struct FeedbackLogBundle {
    // 拼接顺序。调用方按时间从旧到新排,超过字节上限时先裁掉的就是最老的记录。
    std::vector<std::filesystem::path> paths;
    std::string entry_name;
    // 整个合并结果的上限(取尾巴);0 = 沿用 FeedbackPackageRequest::max_log_bytes。
    std::size_t max_bytes = 0;
};

struct FeedbackLogInclusion {
    std::string entry_name;
    std::string source_path;
    bool included = false;
    // 单文件来源:进包的尾巴字节数。合并包成员:该文件在合并结果里实际保留的字节数,
    // 被整体上限整个裁掉时为 0(included 仍为 true,表示文件本身可读)。
    std::size_t tail_bytes = 0;
};

struct FeedbackPackageRequest {
    std::string source;
    std::string feedback_text;
    std::string session_id;
    std::filesystem::path session_jsonl_path;
    std::string workspace_hash;
    std::vector<FeedbackLogSource> logs;
    // 单文件来源之后处理;每个 bundle 产出一个条目、按成员逐个记 metadata。
    std::vector<FeedbackLogBundle> log_bundles;
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

// GUI/Desktop 反馈默认附带的运行时日志:Desktop 壳日志 + daemon 日志,
// 各取最近一个滚动文件,缺失时静默跳过。升级日志不在这里 —— 它按窗口取多个,
// 走 collect_recent_upgrade_log_bundle。
std::vector<FeedbackLogSource> collect_runtime_log_sources(
    const std::filesystem::path& logs_dir);

// TUI 反馈默认附带的运行时日志:TUI + daemon 日志。它刻意不附带 Desktop 壳日志,
// 因为反馈包只携带发起界面的表面日志。
std::vector<FeedbackLogSource> collect_tui_runtime_log_sources(
    const std::filesystem::path& logs_dir);

// 找 logs_dir 下最近 window 内写过的全部 "upgrade-<date>-<pid>.log",按修改时间
// 从旧到新排成一个合并包(条目名固定 logs/upgrade.log.tail.txt)。窗口内一个都
// 没有返回 nullopt:更早的升级记录不做兜底,反馈里没有升级条目就意味着这三天
// 没跑过升级。
std::optional<FeedbackLogBundle> collect_recent_upgrade_log_bundle(
    const std::filesystem::path& logs_dir,
    std::chrono::hours window = kRecentUpgradeLogWindow);

FeedbackPackageResult build_feedback_package(const FeedbackPackageRequest& request);
FeedbackUploadResult upload_feedback_package(const FeedbackUploadRequest& request);

} // namespace acecode::feedback
