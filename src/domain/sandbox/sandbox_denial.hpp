#pragma once

// 沙盒拒绝判定(openspec add-auto-mode-sandbox / align-codex-sandboxing,对齐 Codex
// sandboxing/src/denial.rs + violation.rs):命令在沙盒里失败,能否归因到沙盒
// 限制、被拒的是哪条路径。判得出来就给模型一段带路径的升级提示,引导它先申请
// 最小的额外权限(with_additional_permissions),实在不行再申请沙盒外执行,
// 而不是换花样绕。

#include "sandbox_policy.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace acecode::sandbox {

// 归一化的拒绝原因(Codex FileSystemSandboxViolationReason)。
struct SandboxViolation {
    // operation_not_permitted / permission_denied / read_only_file_system /
    // policy_denied / failed_to_write_file / sigsys / access_denied
    std::string reason;
    std::string path;     // 从输出里抽出的被拒路径;抽不到为空
    std::string snippet;  // 输出片段(≤512 字符),给日志与前端
};

// exit_code 0 / 2 / 126 / 127 不算;其余在 output 里大小写不敏感找拒绝特征词。
bool is_likely_sandbox_denied(int exit_code, const std::string& output);

// 分类 + 抽路径;非拒绝返回 nullopt。
std::optional<SandboxViolation> classify_sandbox_violation(int exit_code, const std::string& output);

// 从一段输出里抽被拒路径:`<path>: Permission denied` / `Access to the path 'X' is
// denied` / `Access is denied` 前的盘符路径。抽不到返回空。
std::string extract_denied_path(const std::string& output);

nlohmann::json violation_to_json(const SandboxViolation& violation);

// 附在工具输出末尾的升级提示。network_enforced=false 且非 best-effort 时不提网络。
// violation 非空时带被拒路径,并建议 with_additional_permissions 只加该路径所在目录。
std::string escalation_hint(const SandboxPolicy& policy, bool network_enforced,
                            const SandboxViolation* violation = nullptr,
                            bool network_best_effort = false);

// 被拒路径所在的目录(路径本身是目录则返回自身;抽不到 / 相对路径返回空)。
// 「只放行该目录」选项与模型提示都用它。
std::string suggested_write_root(const SandboxViolation& violation, const SandboxPolicy& policy);

} // namespace acecode::sandbox
