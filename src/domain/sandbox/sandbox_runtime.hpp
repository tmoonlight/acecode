#pragma once

// 沙盒运行时(openspec add-auto-mode-sandbox / align-codex-sandboxing):会话持有配置,
// 进程共享后端探测,按模式构造策略、合并会话授权、生成 /sandbox 与 system prompt
// 用的状态文本。
//
// 探测结果缓存的意义不只是省开销:system prompt 的 `Shell sandbox:` 行读它,
// 同一回合内不能翻转,否则打穿 prompt cache 前缀。configure() 后重置缓存。

#include "exec_decision.hpp"
#include "sandbox_backend.hpp"
#include "sandbox_policy.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace acecode::sandbox {

struct SandboxRuntimeConfig {
    bool enabled = true;
    bool network_access = false;
    std::vector<std::string> writable_roots;      // config.sandbox.writable_roots + filesystem.write
    bool exclude_tmpdir = false;
    std::vector<std::string> readable_roots;      // config.sandbox.filesystem.read(原文)
    std::vector<std::string> denied_entries;      // config.sandbox.filesystem.deny(原文)
    bool deny_defaults = true;                    // 追加内置默认 deny 名单
    WindowsBackendChoice windows_backend = WindowsBackendChoice::RestrictedToken;
    std::string acecode_home;                     // 数据目录(`:acecode_home` 与 denybin 落点)
};

class SandboxRuntime {
public:
    static SandboxRuntime& instance();

    void configure(SandboxRuntimeConfig cfg);
    SandboxRuntimeConfig config() const;

    // 缓存的后端探测(首次调用真探测)。
    BackendProbe probe();
    // config.enabled && 后端可用(测试覆盖优先)。
    bool available();
    bool network_enforced();
    bool network_best_effort();

    // 测试用:强制可用 / 不可用(nullopt = 取消覆盖)。
    void set_availability_override_for_tests(std::optional<bool> value);

    // 会话授权(用户批准的 additional_permissions / 「只放行该目录」):后续所有
    // 命令的策略都带上它。切换模式 / cwd / 沙盒开关时清空。
    void grant_for_session(const AdditionalPermissions& grants);
    AdditionalPermissions session_grants() const;
    void clear_session_grants();

    // 「编辑项目」附加文件夹:与配置里的 writable_roots 一样进可写根。AgentLoop 每回合
    // 开头与切 cwd 时刷新;configure() 不清它(配置刷新与项目设置是两份来源)。
    void set_workspace_writable_roots(std::vector<std::string> roots);

    // 策略选项:配置 + 会话授权(+ 本次申请)。
    SandboxPolicyOptions policy_options(const AdditionalPermissions* extra = nullptr) const;
    SandboxPolicy policy_for(SandboxMode mode, const std::string& write_root,
                             const AdditionalPermissions* extra = nullptr) const;
    ExecSandboxRequest request_for(SandboxMode mode, const std::string& write_root,
                                   const AdditionalPermissions* extra = nullptr);
    // 决策之后、执行之前准备保护路径。失败只报告不可用,绝不在这里执行未沙盒命令。
    std::string prepare_request(ExecSandboxRequest& request);
    void mark_unavailable(const std::string& reason);
    // 丢掉本会话缓存的探测结论(含 mark_unavailable 的粘性标记),下次 probe()
    // 重新读取进程级探测结果。/sandbox on 用它恢复。
    void reset_probe();

    // Windows 准断网桩目录(`<acecode_home>/sandbox/denybin`);空 = 不注入 PATH。
    std::string denybin_dir() const;

    // 给 /sandbox 用的多行状态;给 system prompt 用的单行摘要在
    // system_prompt.cpp 里按 SystemPromptSandboxState 拼。
    std::string status_text(PermissionMode mode, const std::string& write_root,
                            bool session_disabled);

private:
    mutable std::mutex mu_;
    SandboxRuntimeConfig cfg_;
    std::optional<BackendProbe> probe_;
    std::optional<bool> override_;
    AdditionalPermissions session_grants_;
    std::vector<std::string> workspace_writable_roots_;
};

inline SandboxRuntime& runtime() { return SandboxRuntime::instance(); }

} // namespace acecode::sandbox
