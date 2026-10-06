#include "utils/abandonable_call.hpp"
#include "worker.hpp"
#include "daemon_shutdown_sequence.hpp"
#include "im_channels.hpp"
#include "rc_session_catalog.hpp"
#include "platform/termination_signal.hpp"
#include "ipc/runtime_files_guard.hpp"
#include "utils/lifetime_token.hpp"
#include "utils/scope_exit.hpp"
#include "channels/runtime.hpp"
#include "session/session_serializer.hpp"

#include "platform/native_ui/folder_picker.hpp"
#include "platform/native_ui/context_picker.hpp"
#include "environment/terminal_runtime.hpp"
#include "ipc/daemon_protocol.hpp"
#include "platform/native_ui/open_in_explorer.hpp"
#include "version.hpp"
#include "session_host/tools/spawn_subagent_tool.hpp"
#include "session_host/tools/thread_tools.hpp"
#include "session_host/tools/mesh_agent_tools.hpp"
#include "session_host/mesh/mesh_agent_service.hpp"
#include "tool/workspace_tools.hpp"
#include "workspace/workspace_registry.hpp"
#include "experts/expert_registry.hpp"
#include "connectors/connector_first_start_auth.hpp"
#include "ipc/guid.hpp"
#include "heartbeat.hpp"
#include "tool/mcp_runtime.hpp"
#include "platform/process/os_process.hpp"
#include "ipc/runtime_files.hpp"
#include "provider/cwd_model_override.hpp"
#include "provider/copilot_provider.hpp"
#include "provider/model_pool_status.hpp"
#include "provider/model_resolver.hpp"
#include "provider/models_dev_registry.hpp"
#include "provider/provider_factory.hpp"
#include "hooks/hook_config.hpp"
#include "hooks/hook_manager.hpp"
#include "agent/hook_bridge/hook_events.hpp"
#include "platform/process/process_runner.hpp"
#include "loop/loop_scheduler.hpp"
#include "loop/loop_store.hpp"
#include "loop/scheduled_task_tool.hpp"
#include "session_host/local_session_client.hpp"
#include "session_host/session_registry.hpp"
#include "session_host/thread_service.hpp"
#include "session_host/task_suggestion_service.hpp"
#include "session_host/tools/task_suggestion_tools.hpp"
#include "skills/skill_registry.hpp"
#include "skills/skill_init.hpp"
#include "skills/skill_usage_store.hpp"
#include "tool/ask_user_question_tool.hpp"
#include "tool/bash_tool.hpp"
#include "tool/builtin_tool_registry.hpp"
#include "tool/tool_rewrites.hpp"
#include "security/audit_log.hpp"
#include "tool/file_read_tool.hpp"
#include "tool/file_write_tool.hpp"
#include "tool/file_edit_tool.hpp"
#include "tool/grep_tool.hpp"
#include "tool/glob_tool.hpp"
#include "tool/goal_tool.hpp"
#include "daemon_memory.hpp"
#include "session_host/memory_runtime.hpp"
#include "tool/skill_view_tool.hpp"
#include "tool/skills_tool.hpp"
#include "tool/task_complete_tool.hpp"
#include "tool/tool_executor.hpp"
#include "lsp/lsp_service.hpp"
#include "tool/web_search/runtime.hpp"
#include "tool/web_search/backend_router.hpp"
#include "tool/web_search/region_detector.hpp"
#include "tool/web_search/web_search_tool.hpp"
#include "network/proxy_resolver.hpp"
#include "remote_control/session_channel_binder.hpp"
#include "utils/logger.hpp"
#include "utils/joining_thread.hpp"
#include "utils/paths.hpp"
#include "platform/power_inhibitor.hpp"
#include "utils/token.hpp"
#include "utils/utf8_path.hpp"
#include "web/auth.hpp"
#include "pty/pty_session_registry.hpp"
#include "web/remote_web.hpp"
#include "web/remote_web_proxy.hpp"
#include "web/server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace acecode::daemon {

namespace {

// Windows service control and owner-monitor requests share the process bridge.
void request_terminate() {
    platform::TerminationSignal::request_process_termination();
}

bool apply_cwd_override(const std::string& raw, bool foreground) {
    if (raw.empty()) return true;

    namespace fs = std::filesystem;

    fs::path requested = acecode::path_from_utf8(acecode::expand_path(raw));
    std::error_code ec;
    fs::path effective = requested.is_absolute()
        ? requested
        : fs::absolute(requested, ec);
    if (ec) effective = requested;

    auto canonical = fs::weakly_canonical(effective, ec);
    if (!ec && !canonical.empty()) {
        effective = canonical;
    }

    std::error_code dir_ec;
    if (!fs::exists(effective, dir_ec) || !fs::is_directory(effective, dir_ec)) {
        std::string msg = "[daemon] --cwd path is not a directory: " + acecode::path_to_utf8(effective);
        LOG_ERROR(msg);
        if (foreground) std::cerr << msg << "\n";
        return false;
    }

    fs::current_path(effective, dir_ec);
    if (dir_ec) {
        std::string msg = "[daemon] failed to switch --cwd to " + acecode::path_to_utf8(effective)
            + ": " + dir_ec.message();
        LOG_ERROR(msg);
        if (foreground) std::cerr << msg << "\n";
        return false;
    }

    std::string msg = "[daemon] cwd=" + acecode::path_to_utf8(fs::current_path())
        + " (from --cwd=" + raw + ")";
    LOG_INFO(msg);
    if (foreground) std::cerr << msg << "\n";
    return true;
}

acecode::PermissionMode permission_mode_from_config(const std::string& mode) {
    return acecode::PermissionManager::parse_mode_name(mode)
        .value_or(acecode::PermissionMode::Default);
}

} // namespace

std::string validate_can_start(const WorkerOptions& opts,
                               int heartbeat_timeout_ms) {
    auto existing_guid = read_guid_file();

    if (opts.supervised) {
        // launcher 派 GUID 进来。如果磁盘已有 guid 但跟 launcher 派的不一致,
        // 视为另一个 launcher 已抢占了 daemon.guid,拒启。
        if (existing_guid.has_value() && !existing_guid->empty() &&
            *existing_guid != opts.guid) {
            return "another supervised worker already owns daemon.guid (expected="
                   + opts.guid + " actual=" + *existing_guid + ")";
        }
        return {};
    }

    RuntimeValidationOptions validation_options;
    validation_options.heartbeat_timeout_ms = heartbeat_timeout_ms;
    RuntimeSnapshot snapshot = read_runtime_snapshot();
    RuntimeReuseCheck reuse = validate_runtime_snapshot_for_reuse(snapshot, validation_options);

    // standalone: 只有当 runtime file bundle 仍描述一个当前可用 daemon 时拒启。
    if (existing_guid.has_value() && reuse.reusable && snapshot.pid.has_value()) {
        std::ostringstream oss;
        oss << "another daemon already running (pid=" << *snapshot.pid
            << " guid=" << *existing_guid << ")";
        return oss.str();
    }
    if ((existing_guid.has_value() || snapshot.pid.has_value()) && !reuse.reusable) {
        LOG_WARN("[daemon] ignoring stale runtime files during startup: " + reuse.reason);
    }
    return {};
}

int run_worker(const WorkerOptions& opts, const AppConfig& cfg) {
    // Declared before all work producers; also covers partial startup failures.
    ScopeExit wait_background([] {
        acecode::web_search::shutdown();
        wait_for_abandoned_work(std::chrono::seconds(2));
    });
    // daemon 模式日志切换(spec 12.1-12.3): 写到 ~/.acecode/logs/daemon-{date}.log,
    // 跨午夜自动滚动文件;foreground=true 时同时镜像到 stderr。必须放在 preflight
    // 之前,否则启动期校验失败时不会留下任何日志记录。
    Logger::instance().init_with_rotation(get_logs_dir(), "daemon", opts.foreground);
    // 数据目录重定向的解析告警发生在日志初始化之前(被 Logger 丢掉),这里补记。
    acecode::log_deferred_data_dir_resolution_warning();
    Logger::instance().set_level(LogLevel::Dbg);

    if (!apply_cwd_override(opts.cwd_override, opts.foreground)) {
        return 13;
    }

    auto reject = validate_can_start(opts, cfg.daemon.heartbeat_timeout_ms);
    if (!reject.empty()) {
        std::cerr << "[daemon] refuse to start: " << reject << "\n";
        return 3;
    }

    // 代理解析器初始化 —— 必须在第一个 cpr 调用前完成。daemon 路径无 TUI,
    // 横幅写到日志(LOG_INFO)。
    network::proxy_resolver().init(cfg.network);
    network::proxy_resolver().probe_and_maybe_fallback();
    {
        auto resolved = network::proxy_resolver().effective("https://example.com");
        std::ostringstream oss;
        if (resolved.source == "auto-fallback") {
            auto fb = network::proxy_resolver().fallback_info_snapshot();
            oss << "[proxy] effective=direct (auto-fallback: " << fb.original_url
                << " from " << fb.original_source << " unreachable; reason=" << fb.reason << ")";
        } else {
            oss << "[proxy] effective="
                << (resolved.url.empty() ? "direct" : network::redact_credentials(resolved.url))
                << " source=" << resolved.source
                << " mode=" << cfg.network.proxy_mode;
        }
        LOG_INFO(oss.str());
        if (opts.foreground) std::cerr << oss.str() << "\n";
    }

    // Daemon/WebUI 不经过 TUI 的启动初始化路径，因此必须在这里装载本地
    // models.dev 快照。否则模型目录处理器只能看到进程级空注册表，Provider
    // 与模型查询在实际构建启动后不可用，尽管源码级处理器测试可以通过。
    std::string executable_dir;
    if (const std::string executable = current_executable_path();
        !executable.empty()) {
        executable_dir = path_to_utf8(path_from_utf8(executable).parent_path());
    }
    initialize_registry(cfg, executable_dir);
    if (cfg.models_dev.allow_network &&
        !cfg.models_dev.refresh_on_command_only) {
        refresh_registry_in_background();
    }

    // 模型池负载监控只服务 wizard-ai 地址,避免普通模型配置访问企业接口。
    if (acecode::should_start_model_pool_monitor(cfg.saved_models)) {
        LOG_INFO("[model_pool] wizard-ai model configured; starting 30s load monitor");
        acecode::model_pool_status_service().start();
    }

    const std::string runtime_dir = ensure_run_dir();

    // GUID: supervised 用 launcher 派的;standalone 自己生成。
    std::string guid = opts.supervised ? opts.guid : generate_daemon_guid();
    std::int64_t pid = current_pid();

    // 整个 daemon 路径只用 cfg_mut 一份本地可变副本: /api/mcp PUT 要写、
    // desktop 父进程注入的 port_override 也要写。原本只服务前者,现在两用合一,
    // 后续所有引用都从 cfg_mut 取(原代码部分位置仍引用 const cfg,见底部 web_deps)。
    AppConfig cfg_mut = cfg;
    // Remote access is owned by a separate proxy process. The daemon itself
    // never inherits a legacy or manually supplied wildcard bind.
    cfg_mut.web.bind = acecode::web::kRemoteWebLoopbackBind;
    if (opts.port_override > 0) {
        cfg_mut.web.port = opts.port_override;
    }
    if (!opts.static_dir_override.empty()) {
        cfg_mut.web.static_dir = opts.static_dir_override;
    }

    RuntimeFilesGuard runtime_files(pid, guid, runtime_dir, opts.desktop_managed);

    // 写运行时产物。顺序: guid → pid → port → token,失败立刻退出。
    if (!write_guid_file(guid))                 { std::cerr << "write guid failed\n"; return 4; }
    if (!write_pid_file(pid))                   { std::cerr << "write pid failed\n"; return 4; }
    if (!write_port_file(cfg_mut.web.port))     { std::cerr << "write port failed\n"; return 4; }

    std::string token = !opts.token_override.empty()
        ? opts.token_override
        : acecode::generate_auth_token();
    if (token.empty() || !write_token(token)) {
        std::cerr << "write token failed\n";
        return 4;
    }
    if (opts.desktop_managed) {
        DesktopManagedRuntime managed;
        managed.pid = pid;
        managed.guid = guid;
        managed.kind = acecode::desktop::kDesktopManagedRuntimeKind;
        managed.protocol_version = opts.desktop_protocol_version;
        managed.acecode_version = ACECODE_VERSION;
        if (!write_desktop_managed_runtime(managed)) {
            std::cerr << "write desktop managed runtime failed\n";
            return 4;
        }
        DesktopOwnerRecord owner;
        owner.pid = opts.desktop_owner_pid;
        owner.instance_id = opts.desktop_owner_instance;
        owner.timestamp_ms = now_unix_ms();
        // A new Desktop can acquire the singleton and publish its owner while
        // this worker is still starting after the original Desktop crashed.
        // Preserve that live handoff instead of overwriting it with stale
        // launch arguments.
        if (auto published = read_desktop_owner_record();
            published && published->instance_id != owner.instance_id &&
            is_pid_alive(published->pid)) {
            owner = *published;
        }
        if (!write_desktop_owner_record(std::string(), owner)) {
            std::cerr << "write desktop owner record failed\n";
            return 4;
        }
    }

    {
        std::ostringstream oss;
        oss << "[daemon] worker started pid=" << pid
            << " guid=" << guid
            << " bind=" << cfg_mut.web.bind << ":" << cfg_mut.web.port
            << (opts.supervised ? " mode=supervised" : " mode=standalone");
        LOG_INFO(oss.str());
        if (opts.foreground) std::cerr << oss.str() << "\n";
    }

    // 心跳
    HeartbeatWriter heartbeat(pid, guid, cfg.daemon.heartbeat_interval_ms);
    heartbeat.start();

    platform::TerminationSignal termination;
    termination.install_process_handlers();

    // ----- 装配 daemon-side 的 Provider / Tools / SessionRegistry -----
    // 这一段重现了 main.cpp 在 TUI 路径下的初始化,但缩到 daemon 必要项:
    //   - LlmProvider (与 TUI 等价的三层解析)
    //   - 7 个内置工具 + skills 工具; project instructions 通过 SessionRegistry
    //     接到每个 AgentLoop,与 TUI 保持一致
    //   - PermissionManager (template,SessionRegistry 给每个 session 复制 mode)
    //   - SessionRegistry + LocalSessionClient
    //   - WebServer (HTTP + WebSocket)
    std::string cwd = acecode::current_path_utf8();
    std::string projects_dir =
        acecode::path_to_utf8(acecode::path_from_utf8(acecode::get_acecode_dir()) / "projects");
    acecode::desktop::ensure_workspace_metadata(projects_dir, cwd);
    acecode::desktop::WorkspaceRegistry workspace_registry;
    workspace_registry.scan(projects_dir);

    std::string hook_config_error;
    acecode::HookConfig hook_config = acecode::load_hook_config(&hook_config_error);
    if (!hook_config_error.empty()) {
        LOG_WARN("[hooks] " + hook_config_error);
    }
    acecode::HookManager hook_manager(std::move(hook_config));
    {
        std::string trust_error;
        acecode::HookTrustStore trust_store =
            acecode::load_hook_trust_store_from_path(
                acecode::default_hook_trust_state_path(), &trust_error);
        if (!trust_error.empty()) {
            LOG_WARN("[hooks] " + trust_error);
        }
        acecode::HookLoadOptions hook_load;
        hook_load.feature_enabled = cfg.features.hooks;
        hook_load.cwd = cwd;
        hook_load.project_trusted = true;
        hook_manager.refresh_registry(
            acecode::load_hook_registry(hook_load, &trust_store));
    }

    // cfg_mut 已在前面创建(承接 port_override),这里只继续使用,不再重复声明。
    auto cwd_override = acecode::load_cwd_model_override(cwd);
    auto effective_entry = acecode::resolve_effective_model(cfg_mut, cwd_override, std::nullopt);
    auto provider = acecode::create_provider_from_entry(effective_entry, &cfg_mut);
    if (!provider) {
        LOG_ERROR("[daemon] failed to create LLM provider — daemon will start but new sessions cannot run agent loop until provider is configured");
    } else if (effective_entry.provider == "copilot") {
        auto copilot = std::dynamic_pointer_cast<acecode::CopilotProvider>(provider);
        if (copilot && !copilot->try_silent_auth()) {
            LOG_WARN("[daemon] Copilot silent auth failed; run `acecode configure` to re-authenticate before using Copilot in Web UI");
        }
    }
    {
        auto payload = acecode::build_startup_models_loaded_payload(cwd, effective_entry, provider);
        hook_manager.dispatch(acecode::kHookEventStartupModelsLoaded, payload, cwd);
    }
    // The root provider is immutable after startup. Sessions publish their own
    // provider snapshots through SessionModelBinding.
    auto provider_accessor = [provider] { return provider; };

    // ---- Init LSP runtime (daemon path, openspec add-lsp-service) ----
    // 惰性子系统:init 本身不 spawn 任何进程,首个匹配文件的编辑/查询才会。
    acecode::lsp::init(cfg.lsp, cwd);

    // ---- Init web search runtime (daemon path) ----
    // 与 TUI 共用同一份 state.json 的 region 缓存,所以两侧探测结果互通。
    acecode::web_search::init(cfg.web_search);
    acecode::web_search::register_default_backends(
        acecode::web_search::runtime().router(), cfg.web_search);
    {
        auto cached = acecode::web_search::runtime().detector().cached_region();
        acecode::web_search::runtime().router().resolve_active(cached);
        if (cached == acecode::web_search::Region::Unknown) {
            acecode::web_search::runtime().detect_region_async();
        }
    }

    // SkillRegistry 必须在 register_tool 之前创建,因为 skills_list / skill_view
    // tools 持有 registry 引用。与 TUI 走同一份扫描根逻辑(see src/skills/skill_init.cpp),
    // 让 GET /api/skills 与 GET /api/commands 看到的 skill 集合与 TUI `/skills` 一致。
    acecode::SkillRegistry skill_registry;
    acecode::initialize_skill_registry(skill_registry, cfg, cwd);
    // Skill usage/dormancy state shared with the Web UI. Same state file the
    // TUI process writes, so counts stay consistent across surfaces. Best-
    // effort: read/write failures never break the daemon.
    acecode::SkillUsageStore skill_usage_store(
        acecode::get_acecode_dir() + "/.skill_usage_state.json");
    acecode::ExpertRegistry expert_registry;

    // 「工具重写」(<data_dir>/tool-rewrites.json)必须先于任何 register_tool
    // 发布到进程,注册期的模型侧名冲突检查才拿得到真实映射。
    acecode::tool_rewrites::load_and_apply(acecode::get_acecode_dir());
    // 安全审计存储(openspec add-security-center):失败只记日志,record 退化为 no-op。
    acecode::security::audit_log().configure(acecode::get_acecode_dir());

    // 记忆:三入口共用 create_memory_runtime(openspec unify-memory-system),
    // 先于 ToolExecutor 构造 —— 工具持有它的服务,析构顺序要反过来。
    auto memory_runtime = acecode::create_memory_runtime(
        cfg_mut, acecode::get_acecode_dir(), acecode::MemorySurface::Daemon);

    acecode::ToolExecutor tools;
    acecode::register_session_builtin_tools(tools, cfg_mut);
    // daemon 用 async 版本(走 ToolContext::ask_user_questions → AskUserQuestionPrompter
    // → WS question_request)。TUI 工厂版需要 TuiState/ScreenInteractive,这里没有。
    tools.register_tool(acecode::create_ask_user_question_tool_async(
        cfg_mut.ask.max_questions, cfg_mut.ask.max_options));

    // skills_list / skill_view 让 LLM 按需加载 SKILL.md(配合 expand-webui-skill-commands
    // 的轻量提示策略 — daemon expander 不再 inject SKILL.md body,LLM 看到提示后用
    // 这两个 tool 自己取)。
    tools.register_tool(acecode::create_skills_list_tool(skill_registry, &cfg_mut));
    tools.register_tool(acecode::create_skill_view_tool(skill_registry, &cfg_mut));
    memory_runtime->register_tools(tools);

    // spawn_subagent / wait_subagent:daemon 专属(TUI 无 SessionRegistry 不注册)。
    // ToolExecutor 先于 SessionRegistry 构造,deps 用 shared_ptr 延迟回填 —— 回填
    // 发生在 server 启动前,不存在工具已被调用的并发窗口。
    auto subagent_deps = std::make_shared<acecode::SubagentToolDeps>();
    tools.register_tool(acecode::create_spawn_subagent_tool(subagent_deps));
    tools.register_tool(acecode::create_wait_subagent_tool(subagent_deps));
    auto thread_tool_deps = std::make_shared<acecode::ThreadToolDeps>();
    acecode::register_codex_thread_tools(tools, thread_tool_deps);
    auto workspace_tool_deps =
        std::make_shared<acecode::WorkspaceToolDeps>();
    workspace_tool_deps->registry = &workspace_registry;
    workspace_tool_deps->projects_dir = projects_dir;
    acecode::register_workspace_tools(tools, workspace_tool_deps);

    acecode::daemon::DaemonMcpRuntime mcp_runtime;
    mcp_runtime.start(cfg_mut, tools);

    acecode::PermissionManager template_perm;
    template_perm.set_mode(permission_mode_from_config(cfg_mut.default_permission_mode));
    if (opts.dangerous) template_perm.set_dangerous(true);

    std::shared_mutex app_config_mu;
    acecode::SessionRegistryDeps reg_deps;
    reg_deps.provider_accessor    = provider_accessor;
    reg_deps.tools                = &tools;
    reg_deps.cwd                  = cwd;
    reg_deps.config               = &cfg_mut;
    reg_deps.config_mutex         = &app_config_mu;
    reg_deps.skill_registry       = &skill_registry;
    reg_deps.expert_registry      = &expert_registry;
    reg_deps.memory               = memory_runtime;
    reg_deps.project_instructions_cfg = &cfg_mut.project_instructions;
    reg_deps.custom_instructions_cfg = &cfg_mut.custom_instructions;
    reg_deps.hook_manager         = &hook_manager;
    reg_deps.mcp_manager          = &mcp_runtime.manager();
    reg_deps.template_permissions = &template_perm;
    reg_deps.power_guard          = &acecode::process_power_guard();

    acecode::SessionRegistry registry(std::move(reg_deps));
    acecode::LocalSessionClient client(registry);
    acecode::channels::GatewayDeps channel_deps{client};
    channel_deps.permissions = [&registry](const std::string& id) {
        auto entry = registry.acquire(id);
        return entry && entry->prompter ? entry->prompter->snapshot_pending_requests()
                                        : std::vector<nlohmann::json>{};
    };
    channel_deps.session_cwd = [&registry](const std::string& id) {
        auto entry = registry.acquire(id);
        if (!entry) throw std::runtime_error("Unknown channel session");
        return entry->cwd;
    };
    channel_deps.transcript = [&registry](const std::string& id) {
        nlohmann::json result = nlohmann::json::array();
        auto entry = registry.acquire(id);
        if (!entry || !entry->sm) return result;
        const auto messages = entry->sm->load_active_messages();
        const auto begin = messages.size() > 100 ? messages.size() - 100 : 0;
        for (std::size_t i = begin; i < messages.size(); ++i)
            result.push_back(nlohmann::json::parse(acecode::serialize_message(messages[i])));
        return result;
    };
    acecode::channels::Runtime channel_runtime(std::move(channel_deps));
    // 消息通道(QQ / Telegram):WS 推送等 WebServer 建好后再接上(宿主声明在 server 之前)。
    auto im_broadcast = std::make_shared<acecode::channels::core::Broadcast>();
    auto im_channels = make_im_channel_host(
        {&registry, &client, &cfg_mut, &app_config_mu,
         [projects_dir, &client](const std::optional<std::string>& query) {
             return build_rc_session_catalog(projects_dir, client, query);
         },
         [im_broadcast](const std::string& type, const nlohmann::json& payload) {
             if (*im_broadcast) (*im_broadcast)(type, payload);
         }});
    subagent_deps->registry = &registry;
    subagent_deps->client   = &client;
    subagent_deps->config   = &cfg_mut;
    thread_tool_deps->service = std::make_shared<acecode::ThreadService>(
        acecode::ThreadService::Deps{&registry, &client});
    auto task_suggestions = std::make_shared<acecode::TaskSuggestionService>(
        acecode::TaskSuggestionService::Deps{&registry, &client, &cfg_mut, &app_config_mu});
    acecode::register_task_suggestion_tools(tools, task_suggestions);
    // 蜂群模式（网状）:agent_* 工具在 registry 之后注册,闭包只捕获 weak_ptr(C12)。
    auto mesh_service = std::make_shared<acecode::mesh::MeshAgentService>(
        acecode::mesh::MeshAgentService::Deps{&registry, &expert_registry, &cfg_mut, &app_config_mu, {}, {}});
    mesh_service->attach();
    acecode::register_mesh_agent_tools(tools, mesh_service, cfg_mut);

    // LOOP is daemon-owned and independent of browser connections. SQLite is
    // initialized before HTTP routes are exposed; scheduler shutdown happens
    // explicitly before the session/MCP stack begins tearing down.
    acecode::loop::LoopStore loop_store(acecode::loop::LoopStore::default_database_path());
    acecode::loop::StoreError loop_error;
    const bool loop_store_ready = loop_store.initialize(&loop_error);
    if (!loop_store_ready) {
        LOG_ERROR("[loop] scheduler unavailable: " + loop_error.message);
    }
    acecode::loop::LoopScheduler loop_scheduler(
        loop_store, registry, client, cfg_mut);
    if (loop_store_ready && !loop_scheduler.start(&loop_error)) {
        LOG_ERROR("[loop] scheduler failed to start: " + loop_error.message);
    }
    // Shared ownership: this composition root owns the service; tool calls
    // temporarily lock its weak reference. Sessions stop before dependencies.
    auto scheduled_task_service = std::make_shared<acecode::loop::ScheduledTaskService>(
        loop_store, cfg_mut, app_config_mu, workspace_registry, projects_dir, &loop_scheduler);
    if (loop_store_ready && loop_scheduler.running()) {
        tools.register_tool(acecode::loop::create_scheduled_task_tool(scheduled_task_service));
    }

    const std::string config_path =
        acecode::path_to_utf8(acecode::path_from_utf8(acecode::get_acecode_dir()) / "config.json");

    // 控制台 PTY 注册表(add-console-dock):启动期探测一次 backend,
    // 析构时 stop_all 杀掉全部 shell(栈对象,server.run() 返回后回收)。
    // 默认 shell:+ 旁下拉框选中的 default_shell(探测可用)→ 平台默认 → legacy
    // console.shell。per-create 覆盖由 REST /api/pty 的 shell 参数注入。
    const auto resolved_terminal = acecode::environment::terminal().current();
    const std::string default_shell_cmd = resolved_terminal
        ? resolved_terminal->console_command : acecode::resolve_console_shell(cfg_mut.console.shell);
    acecode::PtySessionRegistry pty_registry(
        acecode::detect_pty_backend(), cwd, default_shell_cmd);
    LOG_INFO(std::string("[daemon] console backend=") +
             acecode::pty_backend_kind_name(pty_registry.backend()) +
             " shell=" + pty_registry.shell());

    acecode::web::WebServerDeps web_deps;
    web_deps.web_cfg            = &cfg_mut.web;   // 含 port_override 后的 effective port
    web_deps.daemon_cfg         = &cfg_mut.daemon;
    web_deps.app_config         = &cfg_mut;
    web_deps.app_config_mutex   = &app_config_mu;
    web_deps.config_path        = config_path;
    web_deps.cwd                = cwd;
    web_deps.projects_dir       = projects_dir;
    web_deps.token              = token;
    web_deps.guid               = guid;
    web_deps.pid                = pid;
    web_deps.start_time_unix_ms = now_unix_ms();
    web_deps.desktop_managed = opts.desktop_managed;
    web_deps.desktop_protocol_version = opts.desktop_protocol_version;
    web_deps.session_client     = &client;
    web_deps.session_registry   = &registry;
    web_deps.task_suggestions   = task_suggestions;
    web_deps.memory             = memory_runtime;
    web_deps.before_data_dir_copy = [&] { loop_scheduler.stop(); };
    web_deps.on_data_dir_copy_failure = [&] { if (loop_store_ready) loop_scheduler.start(); };
    web_deps.expert_registry    = &expert_registry;
    web_deps.hook_manager       = &hook_manager;
    web_deps.tools              = &tools;
    web_deps.mcp_manager        = &mcp_runtime.manager();
    web_deps.workspace_registry = &workspace_registry;
    web_deps.native_folder_picker_enabled = opts.native_folder_picker_enabled;
    if (opts.native_folder_picker_enabled) {
        web_deps.native_folder_picker = [] {
            return acecode::desktop::pick_folder(nullptr);
        };
        web_deps.native_open_file_picker = [] {
            const auto picked = acecode::desktop::pick_single_file(nullptr);
            return acecode::web::NativeSaveFilePickResult{picked.file_path, picked.error};
        };
        web_deps.native_save_file_picker = [](const std::string& suggested_filename) {
            const auto outcome = acecode::desktop::pick_save_file_outcome(
                nullptr, suggested_filename);
            return acecode::web::NativeSaveFilePickResult{
                outcome.path,
                outcome.error,
            };
        };
        // webapp 兼容模式右键菜单的「在资源管理器中打开」。显式的现存绝对
        // 文件或目录可直接交给系统文件管理器，不在这里维护独立路径白名单。
        web_deps.open_in_explorer =
            [](const std::string& path) -> std::optional<std::string> {
            auto result = acecode::desktop::open_path_in_file_manager(path);
            if (result.ok) return std::nullopt;
            return result.error.empty()
                ? std::string("failed to open path in file manager")
                : result.error;
        };
    }
    web_deps.skill_registry     = &skill_registry;
    web_deps.skill_usage_store  = &skill_usage_store;
    web_deps.provider           = nullptr;
    web_deps.provider_mu        = nullptr;
    web_deps.dangerous          = opts.dangerous;
    web_deps.pty_registry       = &pty_registry;
    web_deps.loop_store         = loop_store_ready ? &loop_store : nullptr;
    web_deps.on_loops_changed   = [&loop_scheduler] { loop_scheduler.notify_changed(); };

    acecode::web::ManagedRemoteWebProxyController remote_web_proxy(
        current_executable_path(), runtime_dir, pid);
    if (cfg_mut.web.remote_enabled) {
        if (opts.dangerous) {
            LOG_WARN(
                "[remote-web] configured proxy was not started because "
                "dangerous mode is active; daemon remains loopback-only");
        } else {
            const auto proxy = remote_web_proxy.start(
                cfg_mut.web.remote_port,
                cfg_mut.web.port);
            if (proxy.running) {
                LOG_INFO(
                    "[remote-web] proxy ready pid=" +
                    std::to_string(proxy.pid) + " bind=" +
                    acecode::web::kRemoteWebProxyBind + ":" +
                    std::to_string(proxy.port));
            } else {
                LOG_ERROR(
                    "[remote-web] proxy failed to start; daemon remains "
                    "loopback-only: " + proxy.error);
            }
        }
    }
    web_deps.remote_web_proxy = &remote_web_proxy;
    web_deps.channel_host = im_channels.get();

    acecode::web::WebServer server(std::move(web_deps));
    *im_broadcast = [&server](const std::string& type, const nlohmann::json& payload) {
        server.broadcast_local_event(type, payload);
    };
    LifetimeToken server_lifetime;
    const auto server_ref = server_lifetime.ref(server);

    // ---- daemon 托管 remote control(/rc 绑定 Web 会话到 channel 插件)----
    // 声明在 registry/client 之后(析构先于两者,退订时 AgentLoop 仍活着)、
    // server 之后(注入其 app_config 锁;shutdown() 在 run() 返回后第一时间
    // 显式调用,析构期不再触碰 server)。行为契约见
    // session_channel_binder.hpp 文件头 ①-⑥。
    acecode::rc::SessionChannelBinderDeps rc_binder_deps;
    rc_binder_deps.service     = &acecode::rc::remote_control_service();
    rc_binder_deps.client      = &client;
    rc_binder_deps.config      = &cfg_mut;
    rc_binder_deps.config_path = config_path;
    // binder 与全部 HTTP 路由 / 连接器钩子刷新共用 WebServer 的 app_config
    // 锁 —— cfg_mut 是全进程共享可变对象,binder 曾是唯一不持锁的读写方
    //(Crow 线程上 /rc 与 config PUT / refresh_saved_models_from_disk 并发
    // 即数据竞争 + config.json 交错写坏)。
    rc_binder_deps.with_config_lock = [&server](const std::function<void()>& fn) {
        server.with_app_config_lock(fn);
    };
    // persist 前 reload-merge 用的磁盘读取(config_path 即默认路径,
    // 读写同一份 config.json)。
    rc_binder_deps.load_disk_config = []() { return acecode::load_config(); };
    rc_binder_deps.session_active = [&registry](const std::string& id) {
        return registry.acquire(id) != nullptr;
    };
    const std::string rc_no_workspace_root = acecode::default_no_workspace_cache_root();
    rc_binder_deps.session_resumable = [&client, rc_no_workspace_root](const std::string& id) {
        // 常规 resume 失败后按 no-workspace 缓存目录兜底(与 HTTP resume
        // 路由一致)—— 绑定的是「不使用工作区」会话时,默认 SessionOptions
        // 会把 cwd 解析成 daemon 自身 cwd,重启重建永远找不到该会话。
        return acecode::rc::resume_session_with_no_workspace_fallback(
            client, id, rc_no_workspace_root);
    };
    rc_binder_deps.session_catalog =
        [projects_dir, &client](const std::optional<std::string>& query) {
            return build_rc_session_catalog(projects_dir, client, query);
        };
    rc_binder_deps.resume_session_target = [&client](const acecode::rc::RcSessionTarget& target) {
        return acecode::rc::resume_session_target_exact(client, target);
    };
    rc_binder_deps.on_session_selected = [&server](const acecode::rc::RcSessionTarget& target) {
        server.broadcast_remote_control_session_selected(
            target.session_id,
            target.workspace_hash,
            target.cwd,
            target.no_workspace,
            target.title,
            target.updated_at);
    };
    acecode::rc::SessionChannelBinder rc_binder(std::move(rc_binder_deps));

    // /rc 与 /remote-control:HTTP 命令网关放行后经 registry 的兜底处理器
    // 到达 binder;执行结果同时以 system message 透出到该会话的聊天流
    // (与 /lsp 的反馈方式一致)。
    registry.set_external_command_handler(
        [&rc_binder, &registry](const std::string& id,
                                const acecode::BuiltinCommandRequest& request)
            -> acecode::BuiltinCommandResult {
            if (request.name != "rc" && request.name != "remote-control") {
                return {acecode::BuiltinCommandStatus::UnsupportedCommand,
                        "unsupported command"};
            }
            auto entry = registry.acquire(id);
            if (!entry || !entry->loop) {
                return {acecode::BuiltinCommandStatus::UnknownSession,
                        "unknown session"};
            }
            auto outcome = rc_binder.execute_command(id, request.args);
            entry->loop->emit_system_message(outcome.message, outcome.notice_metadata);
            return {outcome.ok ? acecode::BuiltinCommandStatus::Accepted
                               : acecode::BuiltinCommandStatus::Failed,
                    outcome.message};
        });

    // Connector 自动认证只允许在这个 ACECode home 的第一次 daemon 启动
    // 执行。先持久化 at-most-once claim,再启动任何外部进程;落盘失败时宁可
    // 跳过,避免每次启动都反复弹登录器。
    ReapingThreadSet connector_first_start_threads;
    JoiningThread desktop_owner_monitor;
    JoiningThread watcher;
    DaemonShutdownSequence shutdown;
    const DaemonShutdownSequence::Action shutdown_step = [&](DaemonShutdownStep step) {
        switch (step) {
        case DaemonShutdownStep::Channels: channel_runtime.stop(); break;
        case DaemonShutdownStep::ImChannels: im_channels->stop(); break;
        case DaemonShutdownStep::RemoteWeb: remote_web_proxy.stop(); break;
        case DaemonShutdownStep::RemoteControl:
            rc_binder.shutdown();
            registry.set_external_command_handler({});
            break;
        case DaemonShutdownStep::ConnectorWorkers:
            connector_first_start_threads.join_all();
            break;
        case DaemonShutdownStep::Watchers:
            termination.request();
            watcher.request_stop();
            watcher.join();
            desktop_owner_monitor.request_stop();
            desktop_owner_monitor.join();
            break;
        case DaemonShutdownStep::LoopScheduler:
            loop_scheduler.stop();
            memory_runtime->stop_summary_scheduler();  // 调度线程引用注册表,先于会话停
            break;
        case DaemonShutdownStep::TaskSuggestions: task_suggestions->shutdown(); break;
        case DaemonShutdownStep::Sessions: registry.shutdown_all(); break;
        case DaemonShutdownStep::SpawnListener:
            subagent_deps->on_spawn = {};
            mesh_service->shutdown();
            break;
        case DaemonShutdownStep::Mcp: mcp_runtime.shutdown(); break;
        case DaemonShutdownStep::Lsp: acecode::lsp::shutdown(); break;
        case DaemonShutdownStep::ModelPool: acecode::model_pool_status_service().stop(); break;
        case DaemonShutdownStep::Heartbeat: heartbeat.stop(); break;
        case DaemonShutdownStep::RuntimeFiles: runtime_files.cleanup(); break;
        }
    };
    // Synchronous cleanup while all referenced stack resources are still alive.
    ScopeExit shutdown_on_exit([&] { shutdown.run(shutdown_step); });
    // 记忆摘要调度器引用注册表:在停机守卫建立之后再启动,任何退出路径都先停它。
    memory_runtime->start_summary_scheduler(acecode::daemon::make_memory_scheduler_host(
        registry, cwd, cfg_mut, app_config_mu, config_path));
    const auto first_start_auth =
        acecode::plan_connector_first_start_auth(cfg_mut.connectors);
    if (!first_start_auth.persisted) {
        LOG_ERROR(
            "connector first-start authentication skipped: "
            "failed to persist durable claim");
    } else if (!first_start_auth.claimed) {
        LOG_INFO(
            "connector first-start authentication already claimed; "
            "skipping automatic hooks");
    }
    for (const auto& connector : first_start_auth.connectors) {
        const acecode::ConnectorHookConfig hook = *connector.on_startup;
        const std::string connector_id = connector.id;
        connector_first_start_threads.spawn(
            [hook, connector_id, server_ref]() {
                acecode::platform::ProcessSpec cmd;
                cmd.command = hook.command;
                cmd.args = hook.args;
                const acecode::HookProcessResult result =
                    acecode::run_hook_process(
                        cmd,
                        std::string{},
                        hook.timeout_ms,
                        std::string{});
                LOG_INFO(
                    "connector first-start auth finished; id=" +
                    connector_id +
                    " started=" +
                    (result.started ? "true" : "false") +
                    " timed_out=" +
                    (result.timed_out ? "true" : "false") +
                    " exit=" + std::to_string(result.exit_code));
                if (result.started && !result.timed_out &&
                    result.exit_code == 0) {
                    server_ref.with([](acecode::web::WebServer& active) {
                        active.refresh_saved_models_from_disk();
                    });
                }
            });
    }

    // 子会话 spawn 后登记到 WebServer,给它挂常驻状态监听器,使其 busy 能广播
    // session_status(否则未被 WS 订阅的子会话永不广播,父会话前端在 wait=true
    // 阻塞期间发现不了它,子代理的权限请求冒泡不到主会话)。on_spawn 只在 turn
    // 内被调;退出时先 join registry,再清除回调,异常路径另有 LifetimeRef 兜底。
    subagent_deps->on_spawn =
        [server_ref](const std::string& child_id, const std::string& /*prompt*/) {
            server_ref.with([&](acecode::web::WebServer& active) {
                active.track_subagent(child_id);
            });
        };
    mesh_service->set_on_agent_loaded(
        [server_ref](const std::string& child_id, const std::string& /*root_id*/) {
            server_ref.with([&](acecode::web::WebServer& active) {
                active.track_subagent(child_id);
            });
        });

    // 行为①:持久化的 bound_session_id 非空且会话存在(active 或可从磁盘
    // resume)→ 自动 start rc 服务 + 激活默认 channel + 重建绑定。失败只记
    // 日志,不阻塞 daemon 启动。
    rc_binder.rebuild_from_config();

    // POSIX has no Windows Job Object. A managed daemon therefore watches the
    // Desktop owner record and applies the persisted exit preference when that
    // owner disappears. Start only after all throwable initialization has
    // completed so an early setup exception cannot strand a joinable thread.
    if (opts.desktop_managed) {
        const bool initial_continue_background =
            cfg.desktop.continue_background_process;
        desktop_owner_monitor = JoiningThread(
            [initial_continue_background, owner_pid = opts.desktop_owner_pid,
             owner_instance = opts.desktop_owner_instance, &termination](StopToken stop) {
                DesktopOwnerRecord current{
                    owner_pid, owner_instance, now_unix_ms(),
                };
                std::string handled_dead_instance;
                while (!stop.stop_requested() && !termination.requested()) {
                    auto disk = read_desktop_owner_record();
                    if (disk && disk->instance_id != handled_dead_instance &&
                        disk->instance_id != current.instance_id &&
                        is_pid_alive(disk->pid)) {
                        current = *disk;
                    }

                    if (current.pid > 0 && is_pid_alive(current.pid)) {
                        if (stop.wait_for(std::chrono::milliseconds(200))) break;
                        continue;
                    }

                    // Give a replacement Desktop enough time to acquire the
                    // singleton and publish its new owner generation.
                    if (stop.wait_for(std::chrono::milliseconds(600))) break;
                    disk = read_desktop_owner_record();
                    if (disk && disk->instance_id != handled_dead_instance &&
                        disk->instance_id != current.instance_id &&
                        is_pid_alive(disk->pid)) {
                        current = *disk;
                        continue;
                    }

                    bool continue_background = initial_continue_background;
                    try {
                        continue_background =
                            load_config().desktop.continue_background_process;
                    } catch (const std::exception& e) {
                        LOG_WARN(std::string("[daemon] failed to reload Desktop "
                                             "background preference: ") + e.what());
                    }
                    if (!continue_background) {
                        LOG_INFO("[daemon] Desktop owner exited; stopping coupled "
                                 "managed process");
                        request_terminate();
                        break;
                    }

                    handled_dead_instance = current.instance_id;
                    current = {};
                    if (stop.wait_for(std::chrono::milliseconds(200))) break;
                }
            });
    }

    // 信号 / 终止 → 主循环退出。Crow app.run() 阻塞跑;另起个观察线程在
    // term 信号时调 server.stop() 让 Crow 退出。这样我们就在主线程上 join。
    watcher = JoiningThread([server_ref, &termination](StopToken stop) {
        while (!stop.stop_requested()) {
            if (termination.wait_for(std::chrono::milliseconds(100))) {
                server_ref.with([](acecode::web::WebServer& active) { active.stop(); });
                return;
            }
        }
    });

    channel_runtime.start();
    im_channels->start();
    const int rc = server.run();
    LOG_INFO("[daemon] worker shutting down");
    if (opts.foreground) std::cerr << "[daemon] shutting down\n";
    shutdown.run(shutdown_step);
    return rc;
}

void request_worker_termination() {
    request_terminate();
}

} // namespace acecode::daemon
