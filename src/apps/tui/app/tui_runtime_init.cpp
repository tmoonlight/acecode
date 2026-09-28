#include "tui_runtime_init.hpp"
#include "tui/term/terminal_control.hpp"
#include "tui/theme_palette.hpp"
#include "platform/terminal/terminal_theme_detect.hpp"
#include "environment/bootstrap.hpp"
#include "config/mcp_config.hpp"
#include "hooks/hook_config.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_payload.hpp"
#include "agent/hook_bridge/hook_events.hpp"
#include "provider/models_dev_registry.hpp"
#include "provider/model_resolver.hpp"
#include "provider/model_context_resolver.hpp"
#include "provider/session_model_binding.hpp"
#include "session_host/apply_model_to_session.hpp"
#include "network/proxy_resolver.hpp"
#include "skills/default_skill_startup.hpp"
#include "skills/skill_init.hpp"
#include "skills/skill_registry.hpp"
#include "memory/memory_registry.hpp"
#include "memory/memory_paths.hpp"
#include "lsp/lsp_service.hpp"
#include "tool/web_search/runtime.hpp"
#include "tool/web_search/backend_router.hpp"
#include "tool/web_search/region_detector.hpp"
#include "tool/builtin_tool_registry.hpp"
#include "tool/skills_tool.hpp"
#include "tool/skill_view_tool.hpp"
#include "tool/memory_read_tool.hpp"
#include "tool/memory_write_tool.hpp"
#include "tool/mcp_manager.hpp"
#include "tool/tool_executor.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <thread>

namespace acecode::tui {

static void initialize_proxy_runtime(const AppConfig& config) {
    // 必须在任何 cpr 调用之前完成(下面 initialize_registry 可能触发 models.dev
    // 拉取)。失败也不应阻塞启动 —— ProxyResolver 内部所有探测都是 soft-fail。
    network::proxy_resolver().init(config.network);
    // openspec/changes/proxy-fallback-on-unreachable:启动 TCP probe 检测代理
    // 是否真的在监听,失败时进程级回退直连;所有 cpr 调用站点零变更。
    network::proxy_resolver().probe_and_maybe_fallback();
    auto resolved = network::proxy_resolver().effective("https://example.com");
    std::string banner;
    if (resolved.source == "auto-fallback") {
        auto fb = network::proxy_resolver().fallback_info_snapshot();
        banner = "Proxy: direct (auto-fallback: " + fb.original_url +
                 " from " + fb.original_source + " unreachable)";
    } else {
        std::string url_disp = resolved.url.empty()
                                  ? std::string("direct")
                                  : network::redact_credentials(resolved.url);
        banner = "Proxy: " + url_disp + " (" + resolved.source + ")";
    }
    std::cerr << banner << std::endl;
    LOG_INFO(std::string("[proxy] effective=") +
             (resolved.url.empty() ? "direct" : network::redact_credentials(resolved.url)) +
             " source=" + resolved.source +
             " mode=" + config.network.proxy_mode);
}

static void initialize_models_registry_runtime(const AppConfig& config,
                                               const std::string& argv0_dir) {
    initialize_registry(config, argv0_dir);
    if (config.models_dev.allow_network && !config.models_dev.refresh_on_command_only) {
        std::thread([] {
            refresh_registry_from_network();
        }).detach();
    }
}

static void initialize_web_search_runtime(const AppConfig& config) {
    // Backend router + region detector 单例,异步探测一次后写缓存。失败 / 已有
    // 缓存都不阻塞启动。enabled=false 时仍 init,但下面 register 阶段不挂工具。
    web_search::init(config.web_search);
    web_search::register_default_backends(web_search::runtime().router(),
                                           config.web_search);

    // 启动时先用缓存 region(若有)即时 resolve;无缓存先按 Unknown 走悲观,
    // 再起 detached 线程做实际 HEAD 探测,完成后再 resolve 一次。
    web_search::Region cached = web_search::runtime().detector().cached_region();
    web_search::runtime().router().resolve_active(cached);
    if (cached == web_search::Region::Unknown) {
        std::thread([]{
            auto r = web_search::runtime().detector().detect_now();
            web_search::runtime().router().resolve_active(r);
        }).detach();
    }
}

static MemoryConfig initialize_memory_registry(MemoryRegistry& memory_registry,
                                               const AppConfig& config) {
    // Auto-create ~/.acecode/memory/ if missing; failure disables the memory
    // system for this session without rewriting the user's config.json.
    MemoryConfig runtime_memory_cfg = config.memory;
    std::error_code mkec;
    std::filesystem::create_directories(get_memory_dir(), mkec);
    if (mkec) {
        LOG_ERROR("[memory] failed to create " + get_memory_dir().generic_string() +
                  ": " + mkec.message() + " — memory will be disabled this session");
        runtime_memory_cfg.enabled = false;
    } else if (runtime_memory_cfg.enabled) {
        memory_registry.scan();
    }
    return runtime_memory_cfg;
}

static void initialize_mcp_servers(McpManager& mcp_manager,
                                   const AppConfig& config) {
    const size_t configured = config.mcp_servers.size();
    if (configured == 0) {
        return;
    }

    mcp_manager.connect_all(config);
    LOG_INFO("[mcp] Configured " + std::to_string(configured) +
             " server(s); startup will run in the background");
}

HookConfig load_tui_hook_config() {
    std::string hook_config_error;
    HookConfig hook_config = load_hook_config(&hook_config_error);
    if (!hook_config_error.empty()) {
        LOG_WARN("[hooks] " + hook_config_error);
    }
    return hook_config;
}

AppConfig load_tui_config_and_runtime(HookManager& hook_manager,
                                             const std::string& working_dir,
                                             const std::string& argv0_dir) {
    AppConfig config = load_config();
    acecode::environment::bootstrap(config, {});
    reconcile_default_skills_on_startup(argv0_dir);
    {
        std::string trust_error;
        HookTrustStore trust_store =
            load_hook_trust_store_from_path(default_hook_trust_state_path(),
                                            &trust_error);
        if (!trust_error.empty()) {
            LOG_WARN("[hooks] " + trust_error);
        }
        HookLoadOptions hook_load;
        hook_load.feature_enabled = config.features.hooks;
        hook_load.cwd = working_dir;
        hook_load.project_trusted = true;
        hook_manager.refresh_registry(load_hook_registry(hook_load, &trust_store));
    }
    initialize_proxy_runtime(config);
    initialize_models_registry_runtime(config, argv0_dir);
    return config;
}

ModelProfile initialize_tui_provider_runtime(
    AppConfig& config,
    const std::string& working_dir,
    const std::optional<std::string>& cwd_override,
    SessionModelBinding& model_binding,
    HookManager& hook_manager) {
    ModelProfile effective_entry =
        resolve_effective_model(config, cwd_override, std::nullopt);
    auto snapshot = std::make_shared<AppConfig>(config);
    SessionModelResolvedTarget target;
    target.revision = current_saved_models_revision();
    target.profile = effective_entry;
    target.config = snapshot;
    target.state = session_model_state_from_profile(*snapshot, effective_entry);
    auto resolver = [snapshot, revision = target.revision](
                        const std::string& name) {
        SessionModelResolvedTarget resolved;
        resolved.revision = revision;
        resolved.config = snapshot;
        const auto found = std::find_if(
            snapshot->saved_models.begin(), snapshot->saved_models.end(),
            [&name](const ModelProfile& profile) {
                return profile.name == name;
            });
        if (found != snapshot->saved_models.end()) {
            resolved.profile = *found;
            resolved.state = session_model_state_from_profile(*snapshot, *found);
        }
        return resolved;
    };
    const auto installed = model_binding.install_explicit(
        std::move(target), resolver);
    if (!installed.ok) {
        LOG_WARN("[main] no configured model provider; starting without an active model");
    }
    auto provider = model_binding.provider_snapshot();
    if (provider) {
        // Startup must use the same profile-aware priority as session create,
        // switch, and resume. Calling the provider/model-only resolver here
        // bypassed an explicit saved-model context_window in TUI launches.
        config.context_window = resolve_model_profile_context_window(
            config, effective_entry, config.context_window);
    }
    auto payload =
        build_startup_models_loaded_payload(working_dir, effective_entry, provider);
    hook_manager.dispatch(kHookEventStartupModelsLoaded, payload, working_dir);
    return effective_entry;
}

MemoryConfig initialize_tui_tools_and_registries(
    ToolExecutor& tools,
    SkillRegistry& skill_registry,
    MemoryRegistry& memory_registry,
    McpManager& mcp_manager,
    const AppConfig& config,
    const std::string& working_dir) {
    initialize_web_search_runtime(config);
    // LSP runtime(openspec add-lsp-service):惰性子系统,init 不 spawn 进程。
    lsp::init(config.lsp, working_dir);
    register_session_builtin_tools(tools, config);

    initialize_skill_registry(skill_registry, config, working_dir);
    tools.register_tool(create_skills_list_tool(skill_registry, &config));
    tools.register_tool(create_skill_view_tool(skill_registry, &config));

    MemoryConfig runtime_memory_cfg =
        initialize_memory_registry(memory_registry, config);
    tools.register_tool(create_memory_read_tool(memory_registry,
                                                runtime_memory_cfg.max_index_bytes));
    tools.register_tool(create_memory_write_tool(memory_registry));

    initialize_mcp_servers(mcp_manager, config);
    mcp_manager.reconcile_scope(working_dir, load_project_mcp_config(working_dir), tools);
    return runtime_memory_cfg;
}

acecode::tui::ScreenRenderMode initialize_tui_render_mode(
    AppConfig& config,
    bool force_alt_screen,
    acecode::TerminalCapabilities& term_caps,
    bool& conhost_compat_layout) {
    std::string theme_name = config.tui.theme;
    if (theme_name == "auto") {
        auto detected = acecode::detect_terminal_theme();
        theme_name = (detected == acecode::DetectedTheme::light) ? "light" : "dark";
    }
    acecode::tui::init_theme_palette(theme_name);

    term_caps = acecode::detect_terminal_capabilities();
    if (force_alt_screen) {
        config.tui.alt_screen_mode = "always";
    }
    auto render_mode = acecode::tui::decide_render_mode(config.tui, term_caps);
    conhost_compat_layout =
        acecode::should_use_conhost_compat_layout(term_caps);
    set_ftxui_full_repaint_mode(conhost_compat_layout);
    if (conhost_compat_layout) {
        render_mode = acecode::tui::ScreenRenderMode::AltScreen;
    }
    return render_mode;
}


} // namespace acecode::tui
