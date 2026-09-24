#pragma once

#include "desktop_close_behavior.hpp"
#include "saved_models.hpp"
#include "../computer_use/pointer_appearance.hpp"
#include "../utils/constants.hpp"

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode {

struct OpenAiConfig {
    static constexpr int kDefaultStreamTimeoutMs = 666000;

    std::string base_url = "http://localhost:1234/v1";
    std::string api_key;
    std::string model = "local-model";
    int stream_timeout_ms = kDefaultStreamTimeoutMs;
    // Legacy/global OpenAI-compatible request header templates. Named saved
    // model entries should prefer ModelProfile::request_headers; provider
    // construction uses this map as a fallback when an OpenAI entry has none.
    std::map<std::string, std::string> request_headers;
    // Optional provider id from the bundled models.dev registry (e.g. "anthropic",
    // "openrouter"). Lets resolve_model_context_window() and other catalog-aware
    // call sites pick the correct provider entry even when base_url is a proxy.
    std::optional<std::string> models_dev_provider_id;
};

struct CopilotConfig {
    std::string model = "gpt-4o";
};

struct CodexConfig {
    std::string model = "gpt-5.5";
};

enum class McpTransport {
    Stdio = 0, // launch a child process and talk over its stdio pipes
    Sse,       // HTTP + text/event-stream via mcp::sse_client
    Http,      // MCP Streamable HTTP (currently routed through sse_client)
};

struct McpServerConfig {
    McpTransport transport = McpTransport::Stdio;

    // Stdio-only fields.
    std::string command;                         // required for stdio: executable to launch
    std::vector<std::string> args;               // optional: CLI arguments
    std::map<std::string, std::string> env;      // optional: environment variables

    // SSE / HTTP fields.
    std::string url;                             // required for sse/http: scheme://host[:port]
    std::string sse_endpoint = "/sse";           // path portion of SSE/HTTP endpoint
    std::map<std::string, std::string> headers;  // optional extra request headers
    std::string auth_token;                      // optional bearer token (never logged)
    int timeout_seconds = 30;

    // true = 用户在设置页关掉了这个 server:全 app(所有会话)不连接、不注册它的
    // 工具。持久化到 config.json(仅 true 时写出),daemon 启动时 connect_all 把它
    // 建成 Disabled 态、start_async 跳过;运行时经 /api/mcp/toggle 热切换。
    bool disabled = false;
};

struct SkillsConfig {
    std::vector<std::string> disabled;       // skill names to hide even if present on disk
    std::vector<std::string> external_dirs;  // extra directories to scan (supports ~ and ${ENV})
    bool reuse_opencode = true;              // reuse opencode-compatible skill roots by default
    // Days without use before a skill is treated as dormant (hidden from the
    // automatic <available_skills> list). 0 disables dormancy entirely.
    int idle_days = 30;
    // Runtime-only exact allowlist. nullopt keeps normal discovery behavior;
    // an engaged empty vector hides every skill. This field is intentionally
    // not loaded from or saved to config.json — headless mode uses it on its
    // local AppConfig copy to constrain all cwd-scoped registries.
    std::optional<std::vector<std::string>> allowed;
};

struct MemoryConfig {
    bool enabled = true;
    // Hard cap on MEMORY.md size for system-prompt injection. Oversized indexes
    // are truncated in-memory with a marker; the on-disk file is untouched.
    std::size_t max_index_bytes = 32 * 1024;
};

struct ProjectInstructionsConfig {
    bool enabled = true;
    int max_depth = 8;                         // max dirs walked from cwd towards HOME
    std::size_t max_bytes = 256 * 1024;        // per-file cap
    std::size_t max_total_bytes = 1024 * 1024; // aggregate cap for merged text
    // Priority order. Each directory contributes at most the first filename that
    // exists. AGENT.md is native; AGENTS.md is the plural sibling convention;
    // CLAUDE.md is compat.
    std::vector<std::string> filenames = {"AGENT.md", "AGENTS.md", "CLAUDE.md"};
    // Per-filename gate. Setting this to false removes CLAUDE.md from the
    // effective search list at runtime (overriding its presence in filenames).
    bool read_claude_md = true;
};

inline constexpr std::size_t kCustomInstructionsMaxBytes = 64 * 1024;

struct CustomInstructionsConfig {
    std::string text;

    CustomInstructionsConfig() = default;
    CustomInstructionsConfig(const CustomInstructionsConfig& other) {
        std::lock_guard<std::mutex> lock(other.mu_);
        text = other.text;
    }
    CustomInstructionsConfig& operator=(const CustomInstructionsConfig& other) {
        if (this == &other) return *this;
        std::scoped_lock lock(mu_, other.mu_);
        text = other.text;
        return *this;
    }

    std::string text_snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return text;
    }
    void set_text(std::string value) {
        std::lock_guard<std::mutex> lock(mu_);
        text = std::move(value);
    }

private:
    mutable std::mutex mu_;
};

// 连接器首次启动认证钩子:一次性外部进程,由 config.json 数据完全描述。
// on_startup —— 每个 ACECode 安装仅在首次 daemon 启动时自动执行一次。
// on_enable / on_auth_error / auth_error_scope 仍为兼容旧配置而解析、序列化,
// 但运行时不再自动执行或匹配它们。
struct ConnectorHookConfig {
    std::string command;                 // 可执行文件路径(安装脚本写绝对路径)
    std::vector<std::string> args;
    int timeout_ms = 300000;             // 等待钩子进程退出的上限
};

struct ConnectorConfig {
    std::string id;
    std::string name;
    std::string description;
    bool enabled = true;
    std::optional<ConnectorHookConfig> on_enable;      // 兼容旧 JSON: hooks.on_enable,运行时不执行
    std::optional<ConnectorHookConfig> on_auth_error;  // 兼容旧 JSON: hooks.on_auth_error,运行时不执行
    std::optional<ConnectorHookConfig> on_startup;     // JSON: hooks.on_startup,首次 daemon 启动认证
    std::string auth_error_base_url_prefix;            // 兼容旧 JSON: auth_error_scope.base_url_prefix
};

struct DaemonConfig {
    bool auto_start_on_double_click = false;
    std::string service_name = "ACECodeDaemon";
    int heartbeat_interval_ms = 2000;
    int heartbeat_timeout_ms = 15000;
};

struct WebConfig {
    bool enabled = true;
    std::string bind = "127.0.0.1";
    int port = constants::DEFAULT_WEB_PORT;
    // Remote access is provided by a separately supervised reverse proxy.
    // The daemon listener itself remains on the canonical loopback bind.
    bool remote_enabled = false;
    // 0 = choose an available external port at runtime. A non-zero value is
    // fixed and enablement fails rather than silently substituting another.
    int remote_port = 0;
    // Empty = serve embedded assets; non-empty = serve from this filesystem path
    // (development mode for the front-end change).
    std::string static_dir;
};

struct WebUiPreferencesConfig {
    bool show_acecode_avatar = false;
    // Stable Desktop/WebUI appearance preferences. `system` is resolved by
    // the frontend so a legacy first launch keeps following the OS mode.
    std::string theme = "system";       // system | light | dark
    std::string color_theme = "blue";   // Built-in colors, downloadable themes, or local ai-* themes.
    std::string font_size = "medium";   // small | medium | large
    // Sidebar session rows show a relative timestamp. Product default is on;
    // turning it off leaves the time visible only in the row hover card.
    bool sidebar_session_time = true;
    // Collapse conversation activity; false keeps only individual tools foldable.
    bool message_auto_collapse = true;
};

struct ModelsDevConfig {
    bool allow_network = false;                          // permit any HTTP request to models.dev
    std::optional<std::string> user_override_path;       // local api.json that beats the bundled snapshot
    bool refresh_on_command_only = true;                 // suppress all startup-time network refresh
};

struct InputHistoryConfig {
    bool enabled = true;        // disable to fall back to pure in-memory history
    int max_entries = 10;       // hard cap on persisted entries per working directory
};

// agent_loop.* control AgentLoop's safety bounds. See
// openspec/changes/align-loop-with-hermes for the termination protocol.
//
// A text-only assistant reply (zero tool calls) ends the loop. This matches
// hermes-agent (`run_agent.py:9823`) and claudecodehaha. ACECode models
// (GPT / Copilot / local LMs) sometimes hedge mid-task with "Would you like
// me to continue?" — when they do, the loop ends and the user manually
// re-prompts (e.g. "继续"). Earlier auto-continue / nudge machinery was
// removed for being more disruptive than helpful (chit-chat regressions,
// model-give-up risk; see hermes-agent #7915).
//
// `task_complete` remains an OPTIONAL explicit terminator: the model can
// call it with a `summary` to render a compact "Done: <summary>" row. It is
// not required — a plain text reply also ends the loop cleanly.
//
// `AskUserQuestion` is NEVER a terminator (its tool_result feeds back to
// the model and the loop continues, exactly like any other tool).

// 工具前言(openspec add-tool-preamble,设置 > 开发者模式 > 工具前言)。
// 默认关闭。开启后每个工具调用批次配一条短标题,来源由 mode 决定:
//   "prompt"    提示驱动:系统提示要求模型在工具调用前先写一句 8~12 词的前言。
//   "reasoning" 推理服务内置摘要:从 provider 推理摘要里抠第一对 **加粗**,
//               没有则取推理首句。不改提示词、不多花 token。
//   "sidecar"   旁路模型摘要:用 sidecar_model(空 = 沿用会话模型)对本步材料
//               单独发一次小请求出标签;落盘前最多等 sidecar_wait_ms。
// 非法 mode 在 load_config 归一化为 "prompt";sidecar_wait_ms clamp [0, 15000]。
struct ToolPreambleConfig {
    bool enabled = false;
    std::string mode = "prompt";
    std::string sidecar_model;
    int sidecar_wait_ms = 2000;

    bool operator==(const ToolPreambleConfig& other) const {
        return enabled == other.enabled && mode == other.mode &&
               sidecar_model == other.sidecar_model &&
               sidecar_wait_ms == other.sidecar_wait_ms;
    }
    bool operator!=(const ToolPreambleConfig& other) const { return !(*this == other); }
};

struct AgentLoopConfig {
    int max_iterations = 0; // 0 = unlimited; positive values cap total LLM turns per run()

    ToolPreambleConfig tool_preamble;

    // 开发者模式 JB 开关。只影响系统提示词里的拒绝/停顿说明,不改权限、沙箱或工具上限。
    // 默认关闭;关闭时 build_system_prompt 与未传该标志逐字节一致。
    bool jb_mode = false;

    // AskUserQuestion 应答策略(openspec/changes/add-ask-question-policy)。
    //   "ask"     = 默认。正常弹 UI 无限期等用户回答。
    //   "deny"    = 不弹 UI,立即返回自动应答让模型自行决策并继续。
    //   "timeout" = 弹 UI 等 question_timeout_seconds 秒,无人回答则自动
    //               采纳每个 question 的第一个选项(工具约定推荐项排第一)。
    // 优先级:active goal 固定 Timeout(30) > 显式配置(config/CLI)
    // > 默认 ask。YOLO 只放行工具权限,不改变提问策略。
    std::string question_policy = "ask";
    int question_timeout_seconds = 60; // 仅 policy=timeout 时读取;clamp [5, 3600]

    // 运行时标记,不序列化:config JSON 显式含 question_policy 键。
    // sparse-on-write 下无法从值区分「默认 ask」与「用户写了 ask」,
    // 因此保留显式意图标记供策略解析与后续扩展使用。
    bool question_policy_explicit = false;

    // CLI --question-policy 覆盖,运行时字段,永不序列化。独立于
    // question_policy 存放:若直接覆写上面的配置值,后续任何 save_config
    // (如 /model --default)都会把 CLI 会话级意志意外落盘。
    // 空 = 无覆盖;question_timeout_seconds_cli 仅在 cli 值为 "timeout"
    // 且用户给了冒号秒数时非 0。
    std::string question_policy_cli;
    int question_timeout_seconds_cli = 0;
};

// TUI 渲染策略。绕开 Win10 < 1809 的 conhost / Cmder/ConEmu 在密集 cursor-up
// 序列下产生的画面跳动 —— 详细背景见
// openspec/changes/add-legacy-terminal-fallback/。
//
//   "auto"   = 默认。走 alt-screen,让 TUI 启动时直接撑满终端。
//   "always" = 始终走 alt-screen(\033[?1049h)。
//   "never"  = 始终走 TerminalOutput。
//
// 非法值会在 load_config 中被规范化为 "auto" 并 LOG_WARN。
struct AskConfig {
    // 单次 AskUserQuestion 调用允许的题目数量；所有运行端共享。
    // load_config clamp 到 [1, 50]，默认 10。
    int max_questions = 10;
    // 单个问题允许的选项数量上限；所有运行端共享。
    // load_config clamp 到 [4, 8]，默认 6。
    int max_options = 6;
};

struct TuiConfig {
    std::string alt_screen_mode = "auto";
    // 同步刷新(DEC mode 2026):把每帧输出包在 CSI ?2026h/?2026l 里,终端
    // 整帧一次上屏,消除闪烁。见 openspec/changes/add-synchronized-output/。
    //   "auto"   = 默认。仅对已确认支持 2026 的终端启用(环境变量白名单)。
    //   "always" = 始终启用(包括老 conhost / 未知终端,依赖终端忽略未知序列)。
    //   "never"  = 始终关闭(输出与未实现该特性时完全一致)。
    // 非法值会在 load_config 中被规范化为 "auto" 并 LOG_WARN。
    std::string sync_output_mode = "auto";
    // 把 PgUp / PgDn 当成单行滚动 (等同 Alt+↑/↓). 部分终端 (老 conhost / Cmder /
    // 某些远程 SSH 客户端) 吞掉 Alt+方向键序列, 用户拿不到 Alt+Arrow; 默认打开,
    // 需要整页滚动时可通过 /page-step off 写入 tui.page_keys_single_line=false.
    bool page_keys_single_line = true;
    // TUI 调色板: "auto"(启动时探测终端背景色) / "dark" / "light"。
    std::string theme = "auto";
    // AskUserQuestion 内容区期望保留的最小可见行数;load_config clamp [2,12]。
    int question_min_visible_rows = 4;
    // 预设项提交后保留选中视觉反馈的时长(ms);load_config clamp [0,1000]。
    int question_selection_feedback_ms = 200;
};

// Network / HTTP client tuning. Drives the system-proxy integration —
// see openspec/changes/respect-system-proxy. proxy_mode is the only field
// callers SHOULD branch on at runtime; the rest are passively consumed by
// network::ProxyResolver.
// Web search tool tunables. See openspec/changes/integrate-rss-web-search/.
// `backend = "parallel"` concurrently searches RSS and DuckDuckGo.
// `backend = "rss"` uses AceCode's hosted curated index and falls back per
// request to DuckDuckGo when unavailable or empty. `backend = "auto"` also
// selects DuckDuckGo. Legacy "bing_cn" remains parseable but is disabled and
// resolves to DuckDuckGo at runtime.
struct WebSearchConfig {
    bool enabled = true;
    // "parallel" | "rss" | "auto" | "duckduckgo" | legacy "bing_cn" | ...
    std::string backend = "parallel";
    std::string api_key;        // Reserved for future API backends.
    std::string rss_base_url = "https://ge.bigjuan.xyz/rss-search";
    int max_results = 5;        // Tool limit cap(min(limit, max_results, 10)).
    int timeout_ms = 8000;      // Per-backend HTTP timeout.
};

struct ComputerUseConfig {
    // Desktop control is opt-in, including after loading a legacy config.
    bool enabled = false;
    std::string pointer_style = computer_use::pointer_appearance::kDefaultStyle;
    std::string pointer_color = computer_use::pointer_appearance::kDefaultColor;
};

// 图像生成工具配置(openspec add-image-generation-tool)。
//
// 端点是 OpenAI 兼容的 Images API。三档 quality 对应三个**不同的模型名** ——
// 实测上游对 size / n 参数不生效,分辨率只能靠模型名选,所以档位映射
// 必须可配置,换后端时不用改代码。
//
// source 区分两种凭据来源:同一个网关很可能既提供聊天模型也提供图像
// 模型,让用户把同一个 key 拄两遍是坏体验;但图像端点又不能直接放进
// saved_models(那是聊天模型注册表,会出现在会话模型选择器里被误选)。
struct ImageGenerationConfig {
    bool enabled = true;
    // "saved_model" = 借用 saved_models 里一条同源连接的 base_url + api_key;
    // "inline" = 用本段自己的 base_url + api_key。
    std::string source = "inline";
    std::string saved_model_name;
    std::string base_url = constants::ACEMODEL_API_BASE_URL;
    std::string api_key;
    // quality 档位 → 模型名。
    std::string model_standard = "acemodel-image";
    std::string model_high     = "acemodel-image-2k";
    std::string model_ultra    = "acemodel-image-4k";
    // 模型未传 quality 时的默认档:"standard" | "high" | "ultra"。
    std::string default_quality = "standard";
    // 实测单张 20~60 秒、4k 更久,默认 HTTP 超时会误杀。clamp [30000, 600000]。
    int timeout_ms = 180000;
};

// 单个 LSP server 的 config 条目(openspec add-lsp-service)。
// 名字命中内置 server(clangd / typescript-language-server / pyright /
// gopls / rust-analyzer)时按字段覆盖内置定义;新名字 = 纯自定义 server,
// 此时 command 必填(argv 形式),extensions 建议提供(空 = 匹配所有文件)。
struct LspServerOverride {
    bool disabled = false;
    std::vector<std::string> command;      // argv;空 = 沿用内置 spawn 逻辑
    std::vector<std::string> extensions;   // 形如 ".rs";空 = 沿用内置定义
    std::map<std::string, std::string> env;
    nlohmann::json initialization;         // initializationOptions 原样透传
};

// LSP 集成总配置。enabled=false 时:lsp 工具不注册、不注入诊断、
// 不 spawn 任何 server 进程 —— 行为与引入 LSP 前完全一致。
struct LspConfig {
    bool enabled = true;
    std::map<std::string, LspServerOverride> servers;
};

// Worktree 隔离配置(enter_worktree 工具 / --worktree 启动),对齐
// Claude Code settings.json 的 worktree 段。两个字段都是显式配置才生效。
struct WorktreeConfig {
    // 从主仓 symlink 进新 worktree 的目录(如 "node_modules" ".cache"),
    // 避免每个 worktree 重复占磁盘。默认不做任何 symlink。
    std::vector<std::string> symlink_directories;
    // 创建 worktree 时走 git sparse-checkout(cone 模式)只落盘这些路径,
    // 大型 monorepo 明显更快。默认空 = 完整 checkout。
    std::vector<std::string> sparse_paths;
};

// bash 沙盒配置(openspec add-auto-mode-sandbox,对齐 Codex 的
// sandbox_workspace_write 段)。默认值不落盘。
struct SandboxConfig {
    // 总开关。false = 后端一律视为不可用:auto 模式退化为"安全命令自动、
    // 其余确认"。
    bool enabled = true;
    // workspace-write 沙盒是否放行网络。只在能断网的后端(macOS Seatbelt /
    // Linux bwrap)生效;Windows unelevated 受限令牌不拦网络。
    bool network_access = false;
    // 会话 cwd 之外额外允许写的绝对路径。
    std::vector<std::string> writable_roots;
    // true = 不把系统临时目录列为可写根。
    bool exclude_tmpdir = false;
    // 权限清单(openspec align-codex-sandboxing D2):条目可用 `~`、
    // `:workspace_roots[/sub]`、`:tmpdir`、`:acecode_home` 记号。
    //   read  非空时启用受限读(只放行这些根 + 可写根);空 = 全盘可读。
    //   write 与 writable_roots 语义相同,一并合并。
    //   deny  读写都拒绝(含子树),允许 glob(`**/.env`)。
    std::vector<std::string> filesystem_read;
    std::vector<std::string> filesystem_write;
    std::vector<std::string> filesystem_deny;
    // true = 追加内置默认 deny 名单(~/.ssh、~/.aws、~/.gnupg、~/.netrc、
    // ~/.docker/config.json、~/.kube、数据目录里的 config.json)。
    bool deny_defaults = true;
    // Windows 后端:"restricted-token"(默认)/ "mxc"(MXC 口子,本构建不可用)。
    std::string windows_backend;
};

// git 感知配置(openspec add-git-context)。enabled=false 时不采集/不注入
// gitStatus 快照,/api/git/* 端点按非仓库处理;系统提示的 git repo 标识行
// 保留(零成本且不泄露仓库状态)。
struct GitContextConfig {
    bool enabled = true;
    int timeout_ms = 3000; // 单条 git 命令超时,clamp [500, 30000]
};

// TUI /remote-control 基座配置(openspec add-remote-control)。
// token 持久化后,channel bridge 跨 ACECode 重启无需重新配对;首次
// /remote-control on 或默认 channel 激活时自动生成并写回。
struct RemoteControlConfig {
    struct ChannelPluginConfig {
        std::string manifest_path;                 // 外部 channel 插件 manifest
        int timeout_ms = 10000;                    // 激活/解除绑定进程超时
        nlohmann::json settings = nlohmann::json::object(); // 透传给插件
    };

    int port = 28190;          // loopback listener 端口(避开 daemon 默认 12399)
    std::string token;         // 空 = 首次启用时生成并持久化
    std::string outbound_url;  // manual 出站 webhook;空 = 仅入站/等待插件
    std::string default_channel;
    // daemon 托管模式:/rc 绑定的 Web 会话 id。非空且该会话存在时,daemon
    // worker 启动即自动 start 服务 + 激活默认 channel + 重建绑定。TUI 不读写。
    std::string bound_session_id;
    std::map<std::string, ChannelPluginConfig> channels;
};

struct UpgradeConfig {
    std::string base_url = "http://2017studio.imwork.net:82/aupdate/";
    int timeout_ms = 30000;
};

// Native session notification settings shared by supported application
// surfaces. Windows supports TUI + Desktop; macOS supports ACECode.app.
// Other surfaces retain parsing and safely degrade to no-op.
struct DesktopNotificationsConfig {
    bool enabled = true;                 // 总开关
    bool on_permission = true;           // 权限确认触发通知
    bool on_question = true;             // AskUserQuestion 触发通知
    bool on_completion = true;           // 回合完成触发通知
    bool suppress_when_focused = true;   // 当前 session 已可见且窗口聚焦时不弹
};

struct DesktopConfig {
    DesktopNotificationsConfig notifications;
    // Developer preference, shared by all installations for the current user.
    // Only new Desktop processes consult it; existing instances keep running.
    bool allow_multiple_instances = false;
    // Windows 关窗(× / Alt+F4 / aceDesktop_closeWindow)默认隐藏到托盘。
    // false 时回到关窗即退出。macOS 始终将关窗与真正退出分开。
    bool close_to_tray = true;
    // Windows 关闭窗口时的三态行为。新配置默认询问；显式 legacy
    // close_to_tray 会在加载时映射为对应的记住选择。
    DesktopCloseBehavior close_behavior = DesktopCloseBehavior::Ask;
    // Desktop 真正退出后是否保留其管理的后台进程。默认关闭;关窗口不属于退出。
    bool continue_background_process = false;
};

// Fixed GUI copy for Desktop/WebUI. A missing ui.locale belongs to legacy
// configurations and intentionally keeps the historical Simplified Chinese
// UI. Freshly generated configurations explicitly write "auto".
struct UiConfig {
    // Canonical values: auto | zh-CN | en-US.
    std::string locale = "zh-CN";
};

// 终端配置:控制台停靠区、Agent bash 工具与 system prompt 共用同一份解析结果
// (见 openspec/changes/redesign-settings-config-section 的 agent-default-terminal)。
struct ConsoleConfig {
    // 终端 shell 覆盖(legacy 原始命令行)。default_shell 为空时保留此选择。
    // 例:"pwsh" / "powershell" / "/usr/bin/fish"。
    std::string shell;
    // 选中的终端类型 id(Windows: powershell / git-bash / cmd;POSIX: shell / bash / zsh / fish)。
    // 空 = 未选择,首次启动自动探测后落盘。见 environment::resolve_terminal。
    std::string default_shell;
    // 各终端类型显式指定的程序路径(type id → 绝对路径)。空 = 用探测到的路径。
    // legacy 字段 console.git_bash_path 在加载时并入 "git-bash" 项,不再单独落盘。
    std::map<std::string, std::string> shell_paths;

    // 取某类型的显式路径;没有则返回空串。
    std::string shell_path_for(const std::string& id) const {
        auto it = shell_paths.find(id);
        return it == shell_paths.end() ? std::string{} : it->second;
    }
};

// Agent 工具链目录(Settings → 配置 → 工作空间依赖项)。每项是一个目录的绝对路径,
// 启动时按 python → node → csharp 顺序前插到进程 PATH;空 = 使用系统 PATH。
// 见 openspec/changes/redesign-settings-config-section 的 agent-toolchain-directories。
struct ToolchainsConfig {
    std::string python;
    std::string node;
    std::string csharp;
};

struct SessionTitleConfig {
    bool enabled = true;
    // Empty = use the current session model. Non-empty = saved_models.name.
    std::string model_name;
    int max_input_bytes = 1000;
    int timeout_ms = 15000;
};

struct SummaryGenerationConfig {
    bool enabled = false;
    // A saved_models.name, independent of the current conversation model.
    std::string model_name;
};

struct NetworkConfig {
    // "auto"   = Windows: WinHTTP-IE → registry → env → direct;
    //            POSIX: env (HTTPS_PROXY/HTTP_PROXY/ALL_PROXY/NO_PROXY).
    // "off"    = force direct, ignore all sources.
    // "manual" = use proxy_url verbatim.
    std::string proxy_mode = "auto";
    std::string proxy_url;        // required when proxy_mode == "manual"
    std::string proxy_no_proxy;   // comma-separated; merged with env NO_PROXY

    // openspec/changes/proxy-fallback-on-unreachable:启动时对解析出的代理做
    // 一次同步 TCP probe,connect 失败就把进程级 fallback flag 置位、横幅显示
    // `auto-fallback`、所有 cpr 走直连。`/proxy refresh` 重跑探测。
    int  proxy_probe_timeout_ms = 1500;  // load_config 时 clamp 到 [200, 10000]
    bool proxy_probe_enabled = true;     // 总开关:false = 完全跳过探测,等价旧行为
};

struct FeaturesConfig {
    // Codex-compatible hooks are enabled by default. Users can set
    // features.hooks=false to disable discovery and execution.
    bool hooks = true;
    // Static, global switch for Web/Desktop completed-turn transcript self-heal.
    // Missing or invalid config keeps this enabled; users can set it false.
    bool completed_turn_self_heal = true;
};

struct AppConfig {
    std::string provider; // active runtime provider; empty = not configured
    OpenAiConfig openai;
    CopilotConfig copilot;
    CodexConfig codex;
    int context_window = 128000; // model context window size in tokens
    int max_sessions = 50;       // max saved sessions per project
    // Successful summary compactions before suggesting a fresh conversation.
    // Zero disables automatic continuation suggestions.
    int task_suggestion_compact_threshold = 3;
    // Default permission mode for newly-created daemon/Web/Desktop sessions.
    // Canonical values: default | auto | plan | yolo (accept-edits is read as auto).
    std::string default_permission_mode = "default";
    SandboxConfig sandbox;                       // bash 沙盒(openspec add-auto-mode-sandbox)
    // Fixed one-time startup migration; keep separate from editable sandbox settings.
    bool sandbox_disable_migration_completed = false;
    std::map<std::string, McpServerConfig> mcp_servers; // MCP stdio servers (optional)
    SkillsConfig skills;                         // skill system configuration (optional)
    MemoryConfig memory;                         // persistent user memory settings
    ProjectInstructionsConfig project_instructions; // AGENT.md / AGENTS.md / CLAUDE.md loader
    CustomInstructionsConfig custom_instructions; // Desktop/Web user-authored prompt context
    std::vector<ConnectorConfig> connectors;      // user-configured desktop connectors
    DaemonConfig daemon;                         // daemon process supervision settings
    WebConfig web;                               // HTTP/WebSocket server settings
    WebUiPreferencesConfig web_ui;               // Web/Desktop UI-only preferences
    ModelsDevConfig models_dev;                  // bundled models.dev registry behaviour
    InputHistoryConfig input_history;            // per-cwd persistent ↑/↓ history
    AgentLoopConfig agent_loop;                  // agent-loop termination tunables
    NetworkConfig network;                       // proxy / TLS / abort-debug knobs
    FeaturesConfig features;                     // feature flags
    WebSearchConfig web_search;                  // 联网搜索工具配置(参见 add-web-search-tool)
    LspConfig lsp;                               // LSP 集成(参见 add-lsp-service)
    WorktreeConfig worktree;                     // worktree 隔离(enter_worktree / --worktree)
    ImageGenerationConfig image_generation;      // 图像生成工具(参见 add-image-generation-tool)
    ComputerUseConfig computer_use;              // Windows desktop control, explicitly enabled
    GitContextConfig git_context;                // git 感知(参见 add-git-context)
    RemoteControlConfig remote_control;          // TUI /remote-control channel 托管
    UpgradeConfig upgrade;                       // explicit self-upgrade command config
    AskConfig ask;                               // AskUserQuestion 跨端题目数量
    TuiConfig tui;                               // 终端渲染策略(legacy fallback 等)
    DesktopConfig desktop;                       // desktop shell 配置(系统通知等)
    UiConfig ui;                                 // Desktop/WebUI locale preference
    ConsoleConfig console;                       // 终端类型 / 程序路径(控制台 + bash 工具共用)
    ToolchainsConfig toolchains;                 // Agent 工具链目录(进程 PATH 前缀)
    SessionTitleConfig session_title;            // hidden auto session title generation
    SummaryGenerationConfig summary_generation; // optional background title model

    // --- model profiles (openspec/changes/model-profiles) ---
    // 用户维护的命名模型列表。
    std::vector<ModelProfile> saved_models;
    // 指向 saved_models 中一个 entry 的 name;空字符串 = 未设定。
    std::string default_model_name;
};

// Build a backwards-compatible ModelProfile from the legacy top-level
// provider/openai/copilot fields. Returns an empty profile when no legacy
// provider is configured.
ModelProfile legacy_model_profile_from_config(const AppConfig& cfg);

// Normalize upgrade.base_url by trimming surrounding whitespace and adding a
// trailing slash when non-empty.
std::string normalize_upgrade_base_url(std::string raw);

// Returns true for non-empty http/https URLs after normalization.
bool is_valid_upgrade_base_url(const std::string& raw);

// GUI locale preference accepted by config and the authenticated daemon API.
bool is_valid_ui_locale(const std::string& locale);

// Desktop/WebUI appearance values accepted by config and the authenticated
// UI-preferences API.
bool is_valid_web_ui_theme(const std::string& theme);
bool is_valid_web_ui_color_theme(const std::string& color_theme);
bool is_valid_web_ui_font_size(const std::string& font_size);

nlohmann::json connectors_to_json(const std::vector<ConnectorConfig>& connectors);
bool parse_connectors_json(const nlohmann::json& value,
                           std::vector<ConnectorConfig>& out,
                           std::string* error = nullptr);

// 返回 enabled 且配置了 on_startup 钩子的连接器。调用方只可在首次 daemon
// 启动状态认领成功后执行这些钩子。
std::vector<ConnectorConfig> startup_hook_connectors(
    const std::vector<ConnectorConfig>& connectors);

// Load config from ~/.acecode/config.json, with env var overrides.
// Creates default config if missing.
AppConfig load_config();

// Load config from an explicit path. Environment overrides are disabled by
// default so read-modify-write callers never persist process-only secrets or
// overrides back into config.json.
AppConfig load_config_from_path(
    const std::string& explicit_path,
    bool apply_environment_overrides = false);

// True after load_config() created the ACECode home directory during this
// process. consume_acecode_home_created_by_process() returns that value and
// clears it so first-initialization hooks run once.
bool was_acecode_home_created_by_process();
bool consume_acecode_home_created_by_process();
void reset_acecode_home_created_flag_for_test();

// Save config to ~/.acecode/config.json. Creates the directory if missing,
// overwrites the file, and throws std::runtime_error when writing fails.
void save_config(const AppConfig& cfg);

// Save config to an explicit file path. Creates the parent directory if
// missing and throws std::runtime_error when writing fails.
// Used by daemon/test code paths that must NOT touch the user's real config —
// e.g. PUT /api/mcp under WebServerFixture writes to a per-test temp file.
void save_config(const AppConfig& cfg, const std::string& explicit_path);

// Refresh only the cross-surface defaults used when creating a fresh session.
// This deliberately does not hot-reload the whole AppConfig: long-lived daemon
// state such as proxy, hooks, memory, and runtime services remains unchanged.
// Returns false on parse/validation errors and leaves cfg unchanged.
bool refresh_default_session_preferences_from_config(
    AppConfig& cfg,
    const std::string& explicit_path = {},
    std::string* error = nullptr);

// Get the path to ~/.acecode/ directory
std::string get_acecode_dir();

// Get the path to ~/.acecode/run/ (creates it if missing on first call site —
// callers are responsible for filesystem::create_directories when needed).
std::string get_run_dir();

// Get the path to ~/.acecode/logs/ (callers handle create_directories).
std::string get_logs_dir();

// Validate runtime-affecting config values. Returns an empty vector on success;
// otherwise each entry is a human-readable error message. Daemon mode callers
// should abort on non-empty result.
std::vector<std::string> validate_config(const AppConfig& cfg);

} // namespace acecode
