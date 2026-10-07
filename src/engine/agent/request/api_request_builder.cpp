#include "api_request_builder.hpp"
#include "agent/compaction/compact.hpp"
#include "prompt_context_cache.hpp"
#include "provider_history.hpp"
#include "request_context.hpp"
#include "llm/tool_protocol_names.hpp"
#include "prompt/memory_prompt.hpp"
#include "prompt/mesh_swarm_prompts.hpp"
#include "skills/skill_registry.hpp"
#include "skills/skill_activation.hpp"
#include "skills/skill_usage_store.hpp"
#include "session/request_context_record.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <chrono>
#include <utility>

namespace acecode::agent {
namespace {
template<class T> const T* ptr(const std::optional<T>& value) {
    return value ? &*value : nullptr;
}

std::string memory_snapshot_key(const RequestContextOptions& options) {
    return options.memory_session_key + "|" + options.memory_project_dir + "|" +
           (ApiRequestBuilder::memory_active(options) ? "on" : "off");
}

// 本会话关闭记忆(或记忆整体关闭)时,记忆工具不进模型侧工具表。
void remove_memory_tools(std::vector<ToolDef>& defs) {
    const std::string read_name = model_tool_name_for_native("memory_read");
    const std::string write_name = model_tool_name_for_native("memory_write");
    defs.erase(std::remove_if(defs.begin(), defs.end(), [&](const ToolDef& def) {
        return def.name == read_name || def.name == write_name;
    }), defs.end());
}

nlohmann::json context_state(const RequestBuildInputs& inputs) {
    return {{"session", inputs.session.content}, {"swarm", inputs.swarm_context},
            {"plan", inputs.plan_context}, {"execution", inputs.execution_context}};
}

std::string render_context_state(const nlohmann::json& state, bool update) {
    std::string content = update
        ? "<request-context-update>\nThe following sections replace their earlier values. "
          "Unmentioned sections remain in effect.\n"
        : "<request-context>\n";
    for (auto it = state.begin(); it != state.end(); ++it) {
        if (!it.value().is_string()) continue;
        const auto text = it.value().get<std::string>();
        if (text.empty() && !update) continue;
        content += "<" + it.key() + ">\n";
        content += text.empty() ? "This section is no longer active.\n" : text + "\n";
        content += "</" + it.key() + ">\n";
    }
    content += update ? "</request-context-update>" : "</request-context>";
    return content;
}

ChatMessage context_record(const char* subtype, std::string content) {
    ChatMessage message;
    message.role = "user";
    message.content = std::move(content);
    message.is_meta = true;
    message.subtype = subtype;
    message.uuid = generate_uuid();
    message.metadata = {{"request_context_version", 1}};
    return message;
}
}

bool ApiRequestBuilder::memory_active(const RequestContextOptions& options) {
    return options.memory != nullptr && options.memory_config && options.memory_config->enabled;
}

PromptContextBlock ApiRequestBuilder::render_memory_snapshot(const RequestContextOptions& options) {
    if (!memory_active(options)) return {};
    MemorySnapshotSource source;
    source.memory = options.memory;
    source.project_dir = options.memory_project_dir;
    source.max_index_bytes = options.memory_config->max_index_bytes;
    source.now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return build_memory_snapshot_prompt(source);
}

const PromptContextBlock& ApiRequestBuilder::frozen_memory_snapshot(
    const RequestContextOptions& options) {
    const std::string key = memory_snapshot_key(options);
    if (cache_.needs_memory_snapshot(key)) {
        cache_.store_memory_snapshot(key, render_memory_snapshot(options));
    }
    return cache_.memory_snapshot();
}

void ApiRequestBuilder::invalidate_memory_snapshot() {
    cache_.invalidate_memory();
    cache_.invalidate_git();
}

std::set<std::string> ApiRequestBuilder::dormant_skills(
    const SkillRegistry* registry, SkillUsageStore* store, int idle_days) {
    std::set<std::string> out;
    if (!store || idle_days <= 0 || !registry) {
        return out;
    }
    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const std::int64_t idle_ms = static_cast<std::int64_t>(idle_days) *
                                 24LL * 60 * 60 * 1000;
    for (const auto& meta : registry->list()) {
        if (store->is_dormant(meta.name, now_ms, idle_ms)) {
            out.insert(meta.name);
        }
    }
    return out;
}

std::string ApiRequestBuilder::static_system_prompt(const RequestContextOptions& options) const {
    SystemPromptSandboxState stable_sandbox;
    stable_sandbox.description = "see the latest execution context; tool permission checks are authoritative";
    std::string system_prompt = build_system_prompt(
        tools_, options.cwd, options.skills.get(), /*memory=*/nullptr,
        ptr(options.memory_config), ptr(options.project_config),
        &options.tool_policy,
        &options.worktree,
        options.can_read_images,
        &options.environment, &stable_sandbox, &options.model,
        &options.folders);
    if (options.loop_active &&
        !options.loop_context.empty()) {
        system_prompt += "\n\n<loop-execution>\n";
        system_prompt += options.loop_context;
        system_prompt += "\n</loop-execution>";
    }
    return system_prompt;
}

std::vector<ChatMessage> ApiRequestBuilder::initial_context(const RequestContextOptions& options) const {
    std::vector<ChatMessage> context;

    std::string system_prompt = static_system_prompt(options);
    if (!system_prompt.empty()) {
        ChatMessage system;
        system.role = "system";
        system.content = std::move(system_prompt);
        context.push_back(std::move(system));
    }

    const std::string git_snapshot =
        cache_.cached_git();
    const bool skill_view_available =
        tools_.is_allowed("skill_view", &options.tool_policy);
    const bool skills_list_available =
        tools_.is_allowed("skills_list", &options.tool_policy);
    const bool spawn_subagent_available =
        tools_.is_allowed("spawn_subagent", &options.tool_policy);
    const auto dormant = dormant_skills(options.skills.get(), options.skill_usage, options.skill_idle_days);
    PromptContextBlock skill_context = build_skills_index_context_prompt(
        options.skills.get(), options.context_window,
        skill_view_available, skills_list_available, &dormant);
    if (!skill_context.content.empty()) {
        ChatMessage skill_system;
        skill_system.role = "system";
        skill_system.content = std::move(skill_context.content);
        skill_system.metadata = nlohmann::json{
            {"compact_initial_context", true},
            {"request_local_skill_context", true},
        };
        context.push_back(std::move(skill_system));
    }
    // 压缩 / 估算用:有冻结快照就复用,否则现渲染一份(不写回缓存)。
    const PromptContextBlock* frozen = cache_.peek_memory_snapshot(memory_snapshot_key(options));
    const PromptContextBlock memory_block = frozen ? *frozen : render_memory_snapshot(options);
    std::string mutable_context = build_session_context_prompt(
        options.cwd, &memory_block, ptr(options.project_config),
        options.skills.get(), options.context_window,
        ptr(options.custom_config), git_snapshot, ptr(options.expert), options.expert_member,
        /*category_bytes=*/nullptr,
        skill_view_available, skills_list_available,
        spawn_subagent_available,
        /*include_skill_index=*/false).content;
    if (!mutable_context.empty()) {
        ChatMessage user;
        user.role = "user";
        user.content = std::move(mutable_context);
        user.metadata = nlohmann::json{{"compact_initial_context", true}};
        context.push_back(std::move(user));
    }
    return context;
}

RequestBuildInputs ApiRequestBuilder::capture(
    const RequestContextOptions& options, std::vector<ChatMessage> history,
    bool emergency_profile) {
    RequestBuildInputs inputs;
    inputs.emergency_profile = emergency_profile;
    inputs.session_cache_key = options.memory_session_key.empty()
        ? fallback_session_key_ : options.memory_session_key;
    inputs.memory_active = memory_active(options);
    inputs.plan_context = options.plan_context;
    inputs.todos = options.todos;
    inputs.execution_context = "Shell sandbox: " + options.sandbox.description;
    inputs.system_prompt = static_system_prompt(options);
    LOG_DEBUG("System prompt length: " + std::to_string(inputs.system_prompt.size()));
    std::unordered_set<std::string> loaded_skills;
    for (const auto& message : history) {
        if ((message.role != "user" && message.role != "tool") ||
            !message.metadata.is_object()) continue;
        if (message.role == "tool" &&
            (!message.metadata.contains("tool_success") || message.metadata["tool_success"] != true)) continue;
        const auto names = message.metadata.find(kLoadedSkillsMetadata);
        if (names == message.metadata.end() || !names->is_array()) continue;
        for (const auto& name : *names) {
            if (name.is_string()) loaded_skills.insert(name.get<std::string>());
        }
    }
    auto builtin_tool_defs = tools_.get_model_tool_definitions_by_source(
        ToolSource::Builtin, &options.tool_policy, loaded_skills);
    auto mcp_tool_defs = tools_.get_model_tool_definitions_by_source(
        ToolSource::Mcp, &options.tool_policy, loaded_skills);
    if (emergency_profile) {
        // 这里拿到的已是模型侧定义,核心工具名必须经映射取,不能写死 read/write:
        // 「工具重写」关闭时它们叫 file_read / file_write,写死会把核心工具整个滤掉。
        // apply_patch 也算核心:GPT 系模型的编辑工具就是它(下面按模型族再裁)。
        const std::vector<std::string> core_tool_names = {
            model_tool_name_for_native("bash"),
            model_tool_name_for_native("file_read"),
            model_tool_name_for_native("file_write"),
            model_tool_name_for_native("file_edit"),
            model_tool_name_for_native("apply_patch"),
            model_tool_name_for_native("task_complete"),
        };
        const auto is_core_tool = [&core_tool_names](const ToolDef& definition) {
            return std::find(core_tool_names.begin(), core_tool_names.end(),
                             definition.name) != core_tool_names.end();
        };
        builtin_tool_defs.erase(
            std::remove_if(builtin_tool_defs.begin(), builtin_tool_defs.end(),
                           [&](const ToolDef& definition) {
                               return !is_core_tool(definition);
                           }),
            builtin_tool_defs.end());
        mcp_tool_defs.clear();
        LOG_WARN("[thread-repair] using emergency request profile with " +
                 std::to_string(builtin_tool_defs.size()) +
                 " core tool schemas");
        inputs.tool_defs = builtin_tool_defs;
    } else {
        // Preserve the normal model-facing order exactly. The unified helper
        // keeps builtin and MCP tools in registry order, which is also part of
        // prompt-cache stability and expert-switch behavior.
        inputs.tool_defs =
            tools_.get_model_tool_definitions(&options.tool_policy, loaded_skills);
    }
    // GPT / Codex 系模型只看到 apply_patch,其它模型只看到 file_edit / file_write
    // (openspec add-gpt-apply-patch-adaptation)。三个工具始终注册,这里只裁
    // 模型侧定义表;模型在回合内固定,所以裁完的表逐字节稳定,不打穿 prompt cache。
    filter_tool_definitions_for_model(inputs.tool_defs, options.model.prefers_apply_patch);
    if (!memory_active(options)) {
        remove_memory_tools(inputs.tool_defs);
        remove_memory_tools(builtin_tool_defs);
    }
    LOG_DEBUG("Registered tools: " + std::to_string(inputs.tool_defs.size()));


    cache_.prepare_git(options.cwd, ptr(options.git_config), emergency_profile);
    inputs.builtin_tool_defs = std::move(builtin_tool_defs);
    inputs.mcp_tool_defs = std::move(mcp_tool_defs);
    inputs.history = std::move(history);
    if (!emergency_profile) {
        const bool skill_view_available = tools_.is_allowed("skill_view", &options.tool_policy);
        const bool skills_list_available = tools_.is_allowed("skills_list", &options.tool_policy);
        const bool spawn_subagent_available = tools_.is_allowed("spawn_subagent", &options.tool_policy);
        const auto dormant = dormant_skills(options.skills.get(), options.skill_usage, options.skill_idle_days);
        inputs.skills = build_skills_index_context_prompt(
            options.skills.get(), options.context_window, skill_view_available,
            skills_list_available, &dormant);
        const std::set<std::string> no_dormant;
        // Explicit catalog/policy changes must still reach the model. Idle
        // timestamps and other sessions' usage do not change this identity.
        inputs.skills_catalog_key = build_skills_index_context_prompt(
            options.skills.get(), options.context_window, skill_view_available,
            skills_list_available, &no_dormant).cache_key;
        inputs.session = build_session_context_prompt(
            options.cwd, &frozen_memory_snapshot(options), ptr(options.project_config),
            options.skills.get(), options.context_window, ptr(options.custom_config), cache_.cached_git(),
            ptr(options.expert), options.expert_member, &inputs.category_bytes,
            skill_view_available, skills_list_available, spawn_subagent_available,
            /*include_skill_index=*/false);
        if (options.swarm.mode == SwarmMode::Mesh) {
            // 网状:六个 agent_* 工具被专家策略裁掉时不注入,避免提示与工具表矛盾。
            if (tools_.is_allowed("agent_spawn", &options.tool_policy)) {
                MeshSwarmPromptOptions mesh = options.swarm.mesh;
                mesh.wait_agent_enabled = tools_.is_allowed("agent_wait", &options.tool_policy);
                inputs.swarm_context = build_mesh_swarm_context_prompt(mesh);
            }
        } else {
            inputs.swarm_context = build_swarm_mode_context_prompt(
                options.swarm.mode == SwarmMode::Star, spawn_subagent_available);
        }
    }
    return inputs;
}

ApiRequestBundle ApiRequestBuilder::build(RequestBuildInputs inputs) {
    ApiRequestBundle bundle;
    bundle.tool_defs = std::move(inputs.tool_defs);
    bundle.request_options.prompt_cache_key = inputs.session_cache_key;
    auto context_category_bytes = inputs.category_bytes;
    std::string skill_context;
    std::vector<ChatMessage> api_history;
    std::vector<ChatMessage> context_messages;
    if (inputs.emergency_profile) {
        for (const auto& message : inputs.history) {
            if (!is_request_context_record(message)) api_history.push_back(message);
        }
    } else {
        const auto last_snapshot = std::find_if(inputs.history.rbegin(), inputs.history.rend(),
                                               is_request_context_snapshot);
        const auto snapshot_it = last_snapshot == inputs.history.rend()
            ? inputs.history.end() : std::prev(last_snapshot.base());
        // Explicit memory disable must remove previously injected memory from
        // outgoing requests. That privacy boundary starts a fresh context epoch.
        const bool reset_memory = snapshot_it != inputs.history.end() &&
            snapshot_it->metadata.value("memory_active", inputs.memory_active) != inputs.memory_active;
        ChatMessage snapshot;
        nlohmann::json previous = nlohmann::json::object();
        const auto current = context_state(inputs);
        if (snapshot_it == inputs.history.end() || reset_memory) {
            snapshot = context_record(kRequestContextSnapshot, render_context_state(current, false));
            const auto todos = format_todo_injection(inputs.todos);
            if (!todos.empty()) snapshot.content += "\n" + todos;
            snapshot.metadata["context_state"] = current;
            snapshot.metadata["skills"] = inputs.skills.content;
            snapshot.metadata["skills_catalog_key"] = inputs.skills_catalog_key;
            snapshot.metadata["project_rules_bytes"] = inputs.category_bytes.project_rules;
            snapshot.metadata["memory_active"] = inputs.memory_active;
            bundle.context_records.push_back(snapshot);
            previous = current;
        } else {
            snapshot = *snapshot_it;
            previous = snapshot.metadata.value("context_state", nlohmann::json::object());
        }
        skill_context = snapshot.metadata.value("skills", std::string{});
        auto previous_skills_key = snapshot.metadata.value("skills_catalog_key", inputs.skills_catalog_key);
        context_category_bytes.project_rules = snapshot.metadata.value(
            "project_rules_bytes", context_category_bytes.project_rules);
        // The initial snapshot occupies the same position even though its
        // append-only storage record follows the first user message.
        api_history.push_back(snapshot);
        bool current_epoch = snapshot_it == inputs.history.end();
        for (auto it = inputs.history.begin(); it != inputs.history.end(); ++it) {
            const auto& message = *it;
            if (it == snapshot_it) current_epoch = true;
            if (is_request_context_snapshot(message)) continue;
            if (is_request_context_record(message) && message.metadata.contains("context_state") &&
                (reset_memory || !current_epoch)) continue;
            api_history.push_back(message);
            if (is_request_context_record(message) && message.metadata.contains("context_state")) {
                previous.update(message.metadata["context_state"]);
                previous_skills_key = message.metadata.value("skills_catalog_key", previous_skills_key);
            }
        }
        nlohmann::json changed = nlohmann::json::object();
        for (auto it = current.begin(); it != current.end(); ++it) {
            if (!previous.contains(it.key()) || previous[it.key()] != it.value()) {
                changed[it.key()] = it.value();
            }
        }
        if (!inputs.skills_catalog_key.empty() && inputs.skills_catalog_key != previous_skills_key) {
            changed["skills"] = inputs.skills.content;
        }
        if (!changed.empty()) {
            auto update = context_record(kRequestContextUpdate, render_context_state(changed, true));
            update.metadata["context_state"] = std::move(changed);
            update.metadata["skills_catalog_key"] = inputs.skills_catalog_key;
            api_history.push_back(update);
            bundle.context_records.push_back(std::move(update));
        }
        if (!inputs.hook_context.empty()) {
            auto hook = context_record(kRequestContextUpdate, inputs.hook_context);
            api_history.push_back(hook);
            bundle.context_records.push_back(std::move(hook));
        }
    }
    auto api_messages = detail::model_facing_provider_messages(api_history, "provider-request");
    std::vector<ChatMessage> conversation;
    for (const auto& message : api_messages) {
        (is_request_context_record(message) ? context_messages : conversation).push_back(message);
    }

    ChatMessage sys_msg;
    sys_msg.role = "system";
    sys_msg.content = inputs.system_prompt;
    bundle.messages_with_system.push_back(sys_msg);
    if (!skill_context.empty()) {
        ChatMessage skill_system;
        skill_system.role = "system";
        skill_system.content = skill_context;
        skill_system.metadata = {{"request_local_skill_context", true}};
        context_messages.push_back(skill_system);
        bundle.messages_with_system.push_back(std::move(skill_system));
    }
    bundle.messages_with_system.insert(bundle.messages_with_system.end(),
                                       api_messages.begin(), api_messages.end());

    bundle.context_usage_estimate = estimate_context_usage_breakdown(
        inputs.system_prompt, conversation, context_messages,
        context_category_bytes.project_rules, skill_context.size(),
        inputs.builtin_tool_defs, inputs.mcp_tool_defs);
    std::string context_text;
    for (const auto& message : context_messages) context_text += message.content + "\n";
    auto prompt_diag = build_prompt_cache_diagnostics(
        inputs.system_prompt, context_text, bundle.tool_defs);
    bundle.prompt_diag = {
        {"system", prompt_diag.static_system_prompt_hash},
        {"context", prompt_diag.mutable_context_hash},
        {"tools", prompt_diag.tool_schema_hash},
    };
    LOG_DEBUG("Prompt cache hashes: system=" + prompt_diag.static_system_prompt_hash +
              " context=" + prompt_diag.mutable_context_hash +
              " tools=" + prompt_diag.tool_schema_hash);

    return bundle;
}

ApiRequestBundle ApiRequestBuilder::compaction_request(
    const RequestContextOptions& options, const std::vector<ChatMessage>& history) {
    auto inputs = capture(options, history, false);
    nlohmann::json state = nlohmann::json::object();
    bool has_snapshot = false;
    bool snapshot_memory_active = inputs.memory_active;
    auto skills_catalog_key = inputs.skills_catalog_key;
    for (const auto& message : history) {
        if (!is_request_context_record(message)) continue;
        if (is_request_context_snapshot(message)) {
            has_snapshot = true;
            state = nlohmann::json::object();
            snapshot_memory_active = message.metadata.value("memory_active", inputs.memory_active);
        }
        if (message.metadata.contains("context_state")) state.update(message.metadata["context_state"]);
        skills_catalog_key = message.metadata.value("skills_catalog_key", skills_catalog_key);
    }
    if (has_snapshot && snapshot_memory_active == inputs.memory_active) {
        // Summarize exactly the already-sent context, without consuming hooks
        // or inventing uncommitted updates at the compaction boundary.
        inputs.session.content = state.value("session", std::string{});
        inputs.swarm_context = state.value("swarm", std::string{});
        inputs.plan_context = state.value("plan", std::string{});
        inputs.execution_context = state.value("execution", std::string{});
        inputs.skills_catalog_key = skills_catalog_key;
    }
    return build(std::move(inputs));
}

ChatMessage ApiRequestBuilder::fresh_window_snapshot(
    const RequestContextOptions& options, const std::vector<ChatMessage>& replacement_history) {
    // Prepare off to the side: a failed checkpoint must not refresh the live
    // window's memory/git pins before the replacement is committed.
    PromptContextCache window_cache;
    ApiRequestBuilder window_builder(tools_, window_cache);
    auto inputs = window_builder.capture(options, replacement_history, false);
    inputs.history.erase(std::remove_if(inputs.history.begin(), inputs.history.end(),
                                       is_request_context_record), inputs.history.end());
    return window_builder.build(std::move(inputs)).context_records.front();
}

} // namespace acecode::agent
