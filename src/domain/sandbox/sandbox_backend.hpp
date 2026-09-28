#pragma once

// 沙盒后端(openspec add-auto-mode-sandbox / align-codex-sandboxing)。三平台各一个:
//   - Windows:WRITE_RESTRICTED 受限令牌 + 工作区 ACE(对齐 Codex unelevated 档),
//             另留 MXC(微软 AppContainer 库)口子,本构建探测恒不可用
//   - macOS:/usr/bin/sandbox-exec + Seatbelt policy(对齐 Codex seatbelt.rs)
//   - Linux:bubblewrap(对齐 Codex bwrap.rs;只用系统已装的,绝不下载)
//
// 纯字符串的 argv / policy / 环境组装函数在所有平台都编译,单测跨平台跑;真正碰
// OS 的探测与令牌 / ACE / Job 操作按平台 #ifdef。

#include "sandbox_policy.hpp"
#include "sandbox_types.hpp"

#include <string>
#include <utility>
#include <vector>

namespace acecode::sandbox {

struct BackendProbe {
    BackendKind kind = BackendKind::None;
    bool available = false;
    bool network_enforced = false;    // 该后端能否真的断网(Windows unelevated 不能)
    bool network_best_effort = false; // 不能断网但会套用准断网环境(Windows 受限令牌)
    std::string reason;               // 不可用时的原因;可用时可为空
    std::string executable_path;      // POSIX: 探测过的绝对后端路径,避免在 cwd 重新找程序
};

// 真探测(不缓存;缓存在 SandboxRuntime)。Windows 按配置选择后端;其它平台忽略参数。
BackendProbe probe_backend(WindowsBackendChoice windows_backend = WindowsBackendChoice::RestrictedToken);

// ---- 纯逻辑(所有平台编译)----

// Seatbelt policy 文本与 sandbox-exec argv 前缀(不含 shell 与命令)。
std::string build_seatbelt_policy(const SandboxPolicy& policy);
std::vector<std::string> build_seatbelt_argv(const SandboxPolicy& policy);

// bubblewrap argv 前缀(不含 shell 与命令),以 "--" 结尾。
std::vector<std::string> build_bwrap_argv(const SandboxPolicy& policy);

// 子进程环境变量:ACECODE_SANDBOX=<backend>,断网时另加 ACECODE_SANDBOX_NETWORK_DISABLED=1。
// Windows 受限令牌在 network_access=false 时另套用准断网环境(D7):代理指向死端口、
// pip / npm / cargo 离线、git ssh 失败、`denybin_dir`(ssh / scp 桩)插到 PATH 最前。
// `denybin_dir` 为空则不动 PATH。`base_path` / `base_pathext` 为空时取当前进程环境。
std::vector<std::pair<std::string, std::string>> sandbox_environment(
    BackendKind kind, const SandboxPolicy& policy, bool network_enforced,
    const std::string& denybin_dir = {}, const std::string& base_path = {},
    const std::string& base_pathext = {});

// 准断网环境本身(纯逻辑,Windows 之外也编译以便单测)。
std::vector<std::pair<std::string, std::string>> offline_environment_overrides(
    const std::string& denybin_dir, const std::string& base_path, const std::string& base_pathext);

// 确保 `<dir>/ssh.cmd` `<dir>/scp.cmd` 桩存在;失败返回 false(调用方跳过 PATH 注入)。
bool ensure_denybin_stubs(const std::string& dir, std::string* error);

// Seatbelt deny glob → 正则(移植 Codex seatbelt_regex_for_glob,`**` 跨组件、
// `*` `?` 不跨 `/`、字面量匹配自身与子树)。空 pattern 返回空串。
std::string seatbelt_regex_for_glob(const std::string& pattern, bool subtree);

#ifdef _WIN32
// 合成 SID(S-1-5-80-… 服务 SID 派生规则)的字符串形式。
std::string synthetic_sid_string(const SandboxPolicy& policy);

// 创建受限主令牌。返回 HANDLE(void*),失败返回 nullptr 并填 error。
void* create_restricted_token(const SandboxPolicy& policy, std::string* error);

// 每次核对实际 ACL,幂等地补齐策略里的可写根 / 只读子路径 ACE。
// ReadOnly 策略什么都不打,直接 true。
bool ensure_windows_acl_grants(const SandboxPolicy& policy, std::string* error);

// 撤掉指定路径上属于这一策略 SID 的 ACE(内部辅助,未暴露为命令)。
bool remove_windows_acl_grants(const std::string& path, const SandboxPolicy& policy, std::string* error);

// Job Object:超时 / 中止时杀整棵进程树(D7)。不设 KILL_ON_JOB_CLOSE,正常结束
// 时后台孙进程照旧存活。create 失败返回 nullptr;assign 失败返回 false(嵌套
// Job 被拒时调用方退回 TerminateProcess)。
void* create_process_tree_job();
bool assign_process_to_job(void* job, void* process);
void terminate_job_tree(void* job);
void close_job(void* job);

// MXC 口子(D9):本构建未捆绑 MXC,探测恒不可用并说明原因。
BackendProbe probe_windows_mxc();
#endif

} // namespace acecode::sandbox
