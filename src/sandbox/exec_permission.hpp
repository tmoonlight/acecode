#pragma once

#include "exec_decision.hpp"
#include "permissions/permissions.hpp"
#include "sandbox_policy.hpp"
#include <nlohmann/json.hpp>

namespace acecode::sandbox {

// bash 参数 `sandbox_permissions`(对齐 Codex shell 工具;align-codex-sandboxing D3):
//   use_default                 按模式沙盒跑
//   with_additional_permissions 留在沙盒里,但临时加上 additional_permissions 里的路径 / 网络
//   require_escalated           申请在沙盒外执行(旧参数 with_escalated_permissions=true 等价)
enum class SandboxPermissionsRequest { UseDefault, WithAdditional, RequireEscalated };
const char* sandbox_permissions_name(SandboxPermissionsRequest request);

// 权限门和独立 bash 调用共享参数校验,确保错误请求不会先弹确认。
std::string validate_escalation_arguments(const nlohmann::json& args);
SandboxPermissionsRequest requested_sandbox_permissions(const nlohmann::json& args);
// 解析 additional_permissions:展开 `~`(home 空则取当前用户家目录)并归一成绝对路径。
AdditionalPermissions parse_additional_permissions(const nlohmann::json& args, const std::string& home = {});
// 模型建议的可记住前缀(`prefix_rule`),空 = 未提供。
std::vector<std::string> requested_prefix_rule(const nlohmann::json& args);

struct ExecPermissionOptions {
    bool unattended = false;                 // active goal 无人值守
    AdditionalPermissions session_grants;    // 已批准的会话授权
    std::string home;                        // 测试用家目录;空 = 当前用户
};

struct ExecPermission {
    ExecDecisionInput input;
    ExecDecision decision;
    CommandClassification classification;
    std::vector<std::string> prefixes;
    // 本次申请的额外权限(已归一);use_default / require_escalated 时为空。
    AdditionalPermissions additional;
    // 「批准并记住」可写回规则文件的 pattern;空 = 不提供该选项。
    std::vector<std::vector<std::string>> remember_patterns;
    nlohmann::json arguments;
    std::string error;

    void set_availability(bool available);
    // remember_patterns 的展示形式:"git commit" / "git commit; pnpm test"。
    std::string remember_display() const;
};

ExecPermission evaluate_exec_permission(const std::string& arguments,
                                        const PermissionManager& permissions,
                                        const ExecRules& rules, bool sandbox_available,
                                        CommandPlatform platform = host_command_platform(),
                                        const ExecPermissionOptions& options = {});

// 不依赖 cwd 的规则目录名称检测;调用者同时检查解析后的绝对路径。
bool is_exec_rules_path(const std::string& path);

} // namespace acecode::sandbox
