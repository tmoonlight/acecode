#pragma once
#include <string>

namespace acecode {
// Value contract consumed by TUI startup; parsing stays in the CLI adapter.
struct InteractiveCliOptions {
    bool dangerous_mode = false;
    bool run_configure_cmd = false;
    bool validate_models_registry_cmd = false;
    bool resume_latest = false;
    bool resume_picker_on_startup = false;
    bool force_alt_screen = false;
    std::string resume_session_id;

    // --worktree [name] / -w [name] / --worktree=<name>:启动时先创建
    // (或复用)一个隔离 worktree,整个会话在里面工作。name 缺省时随机
    // 生成;name 也可以是 PR 引用(#123 / GitHub PR URL)→ 基于 PR head
    // 建 worktree,slug 记为 pr-<N>。
    bool worktree_enabled = false;
    std::string worktree_name;

    // --question-policy <ask|deny|timeout[:秒数]>(add-ask-question-policy)。
    // question_policy 为空 = 未指定;question_policy_error 非空 = 解析失败,
    // 调用方应报错退出(fail fast,CLI 是显式意志不做静默归一化)。
    // question_timeout_seconds 仅在 "timeout:N" 冒号语法给出且合法时非 0。
    std::string question_policy;
    int question_timeout_seconds = 0;
    std::string question_policy_error;

    bool direct_resume_requested() const {
        return resume_latest || !resume_session_id.empty();
    }
};

} // namespace acecode
