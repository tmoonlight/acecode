#pragma once

// 沙盒策略模型(openspec add-auto-mode-sandbox + align-codex-sandboxing,对齐
// Codex protocol.rs 的 SandboxPolicy / WritableRoot / FileSystemSandboxEntry):
// 一次 bash 执行允许写哪些根、根下哪些子路径只读、哪些路径可读 / 禁止读写、
// 是否放行网络。三平台后端共用这一份,后端只负责把它翻译成令牌 ACE /
// seatbelt 子句 / bwrap 参数。
//
// 清单模型(D2):策略由三类条目派生 —— read(可读根;为空 = 全盘可读)、
// write(可写根)、deny(读写都拒,含子树;可带 glob)。判定「最长前缀命中
// 优先,同深度 deny > write > read」。

#include "sandbox_types.hpp"

#include <string>
#include <vector>

namespace acecode::sandbox {

struct WritableRoot {
    std::string root;                              // 绝对路径(已 weakly_canonical)
    std::vector<std::string> read_only_subpaths;   // 根下必须保持只读的子路径
};

// 模型单次申请(additional_permissions)或会话授权(SessionGrants)的额外权限。
// 路径已展开 `~` 且为绝对路径;network=true 表示申请放行网络。
struct AdditionalPermissions {
    std::vector<std::string> read;
    std::vector<std::string> write;
    bool network = false;

    bool empty() const { return read.empty() && write.empty() && !network; }
    // 把 other 并进来(去重)。
    void merge(const AdditionalPermissions& other);
    // this 是否已经覆盖 other 的全部申请(会话授权是否已包含本次申请)。
    bool covers(const AdditionalPermissions& other) const;
};

struct SandboxPolicy {
    SandboxMode mode = SandboxMode::FullAccess;
    std::vector<WritableRoot> writable_roots;      // 仅 WorkspaceWrite 非空
    bool network_access = false;
    // Windows WorkspaceWrite 的专用临时目录;空 = 不覆盖子进程临时环境。
    std::string temporary_directory;
    // 受限读时的可读根(绝对路径);空 = 全盘可读。
    std::vector<std::string> readable_roots;
    // 读写都拒绝的绝对路径(含子树)。
    std::vector<std::string> denied_paths;
    // 读写都拒绝的 glob(`**` / `*` / `?`,`/` 分隔,已展开记号)。
    std::vector<std::string> denied_globs;

    bool full_disk_read() const { return readable_roots.empty(); }
    bool has_deny_entries() const { return !denied_paths.empty() || !denied_globs.empty(); }

    // 「最长前缀命中优先,同深度 deny > write > read」。返回 Deny 表示既不可读
    // 也不可写(受限读且未命中可读根时同样返回 Deny)。FullAccess 恒 Write。
    FsAccess resolve_access(const std::string& absolute_path) const;
    bool can_write(const std::string& absolute_path) const;
    bool can_read(const std::string& absolute_path) const;
};

// AgentLoop 注入到 ToolContext 的"这次 bash 怎么跑":策略 + 用哪个后端 +
// 该后端能否断网(决定提示文案)。policy.mode == FullAccess = 不沙盒。
struct ExecSandboxRequest {
    SandboxPolicy policy;
    BackendKind backend = BackendKind::None;
    bool network_enforced = false;
    // Windows 受限令牌:不能真断网,但会套用准断网环境(D7)。
    bool network_best_effort = false;
    std::string backend_executable;
    // Windows 准断网的 ssh / scp 桩目录(prepare_request 确保存在);空 = 不注入 PATH。
    std::string denybin_dir;
};

struct SandboxPolicyOptions {
    std::vector<std::string> extra_writable_roots; // config.sandbox.writable_roots + filesystem.write
    std::vector<std::string> readable_roots;       // config.sandbox.filesystem.read(原文,可含记号)
    std::vector<std::string> denied_entries;       // config.sandbox.filesystem.deny + 默认名单(原文)
    bool include_tmpdir = true;                    // !config.sandbox.exclude_tmpdir
    bool network_access = false;
    std::string tmpdir_override;                   // 测试用的系统临时根;空 = 系统临时目录
    std::string home_override;                     // 测试用的家目录;空 = 当前用户家目录
    std::string acecode_home;                      // 数据目录(`:acecode_home` 记号);空 = 记号不展开
    AdditionalPermissions grants;                  // 会话授权 + 本次申请(已合并)
};

// 内置默认 deny 名单(原文,`~/` 开头):秘密存储目录 / 凭据文件。
std::vector<std::string> default_denied_entries();

// 展开条目里的记号:`~` / `~/x` → 家目录;`:workspace_roots[/sub]` → 写边界根
// (可能多个:主根 + linked git dirs);`:tmpdir` → 专用临时根。未知记号返回空
// (调用方跳过并记日志)。普通路径原样返回(仍需调用方 canonical 化)。
struct EntryContext {
    std::string home;
    std::vector<std::string> workspace_roots;
    std::string tmpdir;
    std::string acecode_home;   // `:acecode_home` → 数据目录(可能被 data-dir.redirect 重定向)
};
std::vector<std::string> expand_entry(const std::string& raw, const EntryContext& ctx);

// 条目是否含 glob 元字符(`*` `?` `[`)。
bool is_glob_entry(const std::string& entry);

// glob 匹配(`**` 跨目录、`*` `?` 不跨 `/`);两边都按 `/` 归一,大小写按平台。
bool glob_matches_path(const std::string& pattern, const std::string& path);

// 计算 WorkspaceWrite 的可写根:write_root(会话写边界根 / cwd)+ 额外根 +
// 临时目录 + 链接 worktree 的 gitdir 与 common dir + 会话授权的写路径。每个根下的
// `.git/hooks`、`.git/config`、`.git/config.worktree`、`.git/modules`、
// `.git/worktrees/*/config.worktree`、`.acecode/rules` 列为只读子路径。
std::vector<WritableRoot> compute_writable_roots(const std::string& write_root,
                                                 const SandboxPolicyOptions& options);

SandboxPolicy make_sandbox_policy(SandboxMode mode, const std::string& write_root,
                                  const SandboxPolicyOptions& options);

// 受保护子路径名单(相对某个 .git 目录 / 工作区根)。
std::vector<std::string> protected_subpaths_under(const std::string& root);

// 解析 `<root>/.git` 是文件时的 `gitdir:` 指针,返回 gitdir 与 common dir 的
// 绝对路径(都不存在时返回空)。
struct LinkedGitDirs {
    std::string gitdir;      // <main>/.git/worktrees/<name>
    std::string common_dir;  // <main>/.git
};
LinkedGitDirs resolve_linked_git_dirs(const std::string& root);

std::string system_temp_dir();
std::string user_home_dir();

// 供 system prompt / /sandbox 用的一行摘要:
// "workspace-write; writable: a, b; denied: 3 entries; network: blocked"。
std::string describe_policy(const SandboxPolicy& policy, bool network_enforced,
                            bool network_best_effort = false);

// 路径归一(weakly_canonical + 去尾分隔符),空输入返回空。
std::string canonical_policy_path(const std::string& path);

// 路径存在时同 canonical_policy_path;不存在时只做词法归一(generic 分隔符),
// 不让 Windows 给 POSIX 形态的申请路径(`/home/u/x`)凭空加上当前盘符。
std::string normalize_policy_path(const std::string& path);

// 绝对路径,或 POSIX 形态的根路径(`/x`,在 Windows 上 is_absolute() 为 false)。
bool is_rooted_path(const std::string& path);

} // namespace acecode::sandbox
