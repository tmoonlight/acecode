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

void ApiRequestBuilder::invalidate_memory_snapshot() { cache_.invalidate_memory(); }

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
    std::string system_prompt = build_system_prompt(
        tools_, options.cwd, options.skills.get(), /*memory=*/nullptr,
        ptr(options.memory_config), ptr(options.project_config),
        &options.tool_policy,
        &options.worktree,
        options.can_read_images,
        &options.environment, &options.sandbox, &options.model,
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
    auto api_messages = detail::model_facing_provider_messages(inputs.history, "provider-request");
    auto context_category_bytes = inputs.category_bytes;
    const std::string skill_context = inputs.emergency_profile ? std::string{} : cache_.skills(inputs.skills);
    const std::string session_context = inputs.emergency_profile ? std::string{} : cache_.session(inputs.session);
    const std::string& swarm_mode_context = inputs.swarm_context;
    const std::string& hook_context = inputs.hook_context;
    const std::string& plan_mode_context = inputs.plan_context;
    const auto& todo_context_items = inputs.todos;
    context_category_bytes.skills = skill_context.size();
    std::vector<ChatMessage> mutable_context_messages;
    detail::append_request_context_for_api(mutable_context_messages, session_context);
    detail::append_request_context_for_api(mutable_context_messages, swarm_mode_context);
    detail::append_request_context_for_api(mutable_context_messages, hook_context);
    detail::append_plan_mode_context_for_api(mutable_context_messages, plan_mode_context);
    detail::append_todo_context_for_api(mutable_context_messages, todo_context_items);

    ChatMessage skill_system_message;
    if (!skill_context.empty()) {
        skill_system_message.role = "system";
        skill_system_message.content = skill_context;
        skill_system_message.metadata =
            nlohmann::json{{"request_local_skill_context", true}};
    }
    std::vector<ChatMessage> estimated_context_messages =
        mutable_context_messages;
    if (!skill_system_message.content.empty()) {
        estimated_context_messages.insert(
            estimated_context_messages.begin(), skill_system_message);
    }

    bundle.context_usage_estimate = estimate_context_usage_breakdown(
        inputs.system_prompt,
        api_messages,
        estimated_context_messages,
        context_category_bytes.project_rules,
        context_category_bytes.skills,
        inputs.builtin_tool_defs,
        inputs.mcp_tool_defs);

    insert_context_before_last_real_user_or_summary(
        api_messages, std::move(mutable_context_messages));

    ChatMessage sys_msg;
    sys_msg.role = "system";
    sys_msg.content = inputs.system_prompt;
    bundle.messages_with_system.push_back(sys_msg);
    if (!skill_system_message.content.empty()) {
        bundle.messages_with_system.push_back(std::move(skill_system_message));
    }
    bundle.messages_with_system.insert(bundle.messages_with_system.end(),
                                       api_messages.begin(), api_messages.end());

    auto prompt_diag = build_prompt_cache_diagnostics(
        inputs.system_prompt,
        skill_context + "\n" + session_context + "\n" + swarm_mode_context + "\n" +
            plan_mode_context + "\n" + hook_context + "\n" +
            format_todo_injection(todo_context_items),
        bundle.tool_defs);
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


} // namespace acecode::agent
