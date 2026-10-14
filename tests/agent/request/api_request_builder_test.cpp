#include <gtest/gtest.h>
#include "agent/request/api_request_builder.hpp"
#include "agent/request/prompt_context_cache.hpp"
#include "skills/skill_activation.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/request_context_record.hpp"
#include "session/session_serializer.hpp"
#include "skills/skill_registry.hpp"

#include <filesystem>
#include <fstream>
#include <memory>

TEST(PromptContextCache, CwdChangePreservesSkillAndSessionPins) {
    // cwd 失效只针对 Git;相同内容键下重采的动态文本仍钉住原字节。
    acecode::agent::PromptContextCache cache;
    EXPECT_EQ(cache.skills({"skill-first", "skill-key", ""}), "skill-first");
    EXPECT_EQ(cache.session({"session-first", "session-key", ""}), "session-first");
    cache.reset_on_cwd_change();
    EXPECT_EQ(cache.skills({"skill-new", "skill-key", ""}), "skill-first");
    EXPECT_EQ(cache.session({"session-new", "session-key", ""}), "session-first");
    EXPECT_EQ(cache.session({"session-new", "next-key", ""}), "session-new");
}

TEST(ApiRequestBuilder, MainAndCompactionUseIdenticalStaticPrefixAndCapturedInputs) {
    acecode::ToolExecutor tools;
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    acecode::agent::RequestContextOptions options;
    options.cwd = testing::TempDir();
    options.git_config.emplace();
    options.git_config->enabled = false;
    options.project_config.emplace();
    options.project_config->enabled = false;
    options.memory_config.emplace();
    options.memory_config->enabled = false;
    options.loop_active = true;
    options.loop_context = "fixed loop rule";
    const auto compact = builder.initial_context(options);
    acecode::ChatMessage user;
    user.role = "user";
    user.content = "request";
    auto inputs = builder.capture(options, {user}, false);
    options.loop_context = "changed after capture";
    inputs.hook_context = "hook snapshot";
    const auto request = builder.build(std::move(inputs));
    ASSERT_FALSE(compact.empty());
    ASSERT_FALSE(request.messages_with_system.empty());
    EXPECT_EQ(request.messages_with_system.front().content, compact.front().content);
    EXPECT_NE(request.messages_with_system.front().content.find("fixed loop rule"), std::string::npos);
    EXPECT_EQ(request.messages_with_system.front().content.find("changed after capture"), std::string::npos);
}

TEST(ApiRequestBuilder, DeferredSkillToolsStaySessionLocalAndRespectPolicyAfterReload) {
    acecode::ToolExecutor tools;
    acecode::ToolImpl tool;
    tool.definition.name = "create_scheduled_task";
    tool.activation_skill = "scheduled-task";
    ASSERT_TRUE(tools.register_tool(tool));
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    acecode::agent::RequestContextOptions options;
    options.cwd = testing::TempDir();
    options.git_config.emplace();
    options.git_config->enabled = false;
    options.project_config.emplace();
    options.project_config->enabled = false;
    EXPECT_TRUE(builder.capture(options, {}, false).tool_defs.empty());
    EXPECT_EQ(tools.get_registered_tools().size(), 1u);

    acecode::ToolResult loaded{"skill instructions", true};
    loaded.metadata[acecode::kLoadedSkillsMetadata] = {"scheduled-task"};
    auto message = acecode::ToolExecutor::format_tool_result("load-skill", loaded);
    EXPECT_EQ(builder.capture(options, {message}, false).tool_defs.size(), 1u);
    EXPECT_EQ(builder.capture(options, {message}, false).builtin_tool_defs.size(), 1u);
    // A shared registry does not activate another session or a compacted history.
    EXPECT_TRUE(builder.capture(options, {}, false).tool_defs.empty());
    EXPECT_TRUE(tools.get_model_tool_definitions().empty());
    EXPECT_EQ(builder.capture(options, {message}, false).tool_defs.size(), 1u);

    message.metadata["tool_success"] = false;
    EXPECT_TRUE(builder.capture(options, {message}, false).tool_defs.empty());
    // Explicit Skill expansion uses a user-message load record.
    message.role = "user";
    EXPECT_EQ(builder.capture(options, {message}, false).tool_defs.size(), 1u);
    options.tool_policy.builtin_tools = std::unordered_set<std::string>{};
    EXPECT_TRUE(builder.capture(options, {message}, false).tool_defs.empty());
}

// 触发场景:create_scheduled_task 带 activation_skill + activation_hint 注册,
// 会话里装着 scheduled-task 技能,但还没加载它(工具表里看不到该工具)。
// 期望:每次请求的技能索引都单列门控工具和触发说明,模型据此先加载技能;
// 工具被会话策略隐藏(定时任务执行会话 / 专家收走)时,这一节同步消失,
// 且 skills_catalog_key 跟着变化,不会复用旧提示。
TEST(ApiRequestBuilder, SkillIndexAdvertisesGatedToolUntilPolicyHidesIt) {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(testing::TempDir()) / "acecode-gated-tool-index";
    fs::remove_all(root);
    fs::create_directories(root / "scheduled-task");
    {
        std::ofstream skill(root / "scheduled-task" / "SKILL.md", std::ios::binary);
        skill << "---\nname: scheduled-task\ndescription: Create scheduled tasks\n---\n\n# body\n";
    }
    auto registry = std::make_shared<acecode::SkillRegistry>();
    registry->set_scan_roots({root});
    registry->scan();

    acecode::ToolExecutor tools;
    acecode::ToolImpl gated;
    gated.definition.name = "create_scheduled_task";
    gated.activation_skill = "scheduled-task";
    gated.activation_hint = "run later or repeatedly (每天 / 建立定时任务)";
    ASSERT_TRUE(tools.register_tool(gated));
    acecode::ToolImpl skill_view;
    skill_view.definition.name = "skill_view";
    ASSERT_TRUE(tools.register_tool(skill_view));

    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    acecode::agent::RequestContextOptions options;
    options.cwd = testing::TempDir();
    options.git_config.emplace();
    options.git_config->enabled = false;
    options.project_config.emplace();
    options.project_config->enabled = false;
    options.skills = registry;

    const auto visible = builder.capture(options, {}, false);
    EXPECT_NE(visible.skills.content.find(
        "- `create_scheduled_task` (load skill `scheduled-task`): run later or repeatedly"),
        std::string::npos);
    for (const auto& def : visible.tool_defs) EXPECT_NE(def.name, "create_scheduled_task");

    options.tool_policy.hidden_builtin_tools.emplace("create_scheduled_task", "hidden");
    const auto hidden = builder.capture(options, {}, false);
    EXPECT_EQ(hidden.skills.content.find("Skill-gated"), std::string::npos);
    EXPECT_NE(hidden.skills_catalog_key, visible.skills_catalog_key);
    fs::remove_all(root);
}

namespace {
acecode::ChatMessage message(const std::string& role, const std::string& text) {
    acecode::ChatMessage result;
    result.role = role;
    result.content = text;
    return result;
}

void expect_prefix(const std::vector<acecode::ChatMessage>& before,
                   const std::vector<acecode::ChatMessage>& after) {
    ASSERT_GE(after.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(acecode::serialize_message_json(before[i]),
                  acecode::serialize_message_json(after[i])) << "message " << i;
    }
}

acecode::agent::RequestBuildInputs context_inputs() {
    acecode::agent::RequestBuildInputs inputs;
    inputs.system_prompt = "fixed system";
    inputs.skills = {"frozen skills", "skills-v1", ""};
    inputs.session = {"project rules v1", "session-v1", ""};
    inputs.history = {message("user", "first request")};
    return inputs;
}

void commit(acecode::agent::RequestBuildInputs& inputs,
            const acecode::agent::ApiRequestBundle& request) {
    inputs.history.insert(inputs.history.end(), request.context_records.begin(), request.context_records.end());
    inputs.hook_context.clear();
}
}

TEST(ApiRequestBuilder, PrefixSurvivesNewTurnsSkillActivityAndReload) {
    acecode::ToolExecutor tools;
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    auto inputs = context_inputs();
    const auto first = builder.build(inputs);
    ASSERT_EQ(first.context_records.size(), 1u);
    EXPECT_TRUE(acecode::is_request_context_snapshot(first.context_records.front()));
    commit(inputs, first);
    inputs.history.push_back(message("assistant", "answer"));
    inputs.history.push_back(message("user", "second request"));
    inputs.skills = {"changed by another session", "skills-v2", ""};
    const auto second = builder.build(inputs);
    expect_prefix(first.messages_with_system, second.messages_with_system);
    EXPECT_TRUE(second.context_records.empty());

    // Reload actual serialized records into a new builder/cache.
    for (auto& item : inputs.history) {
        item = acecode::deserialize_message_json(acecode::serialize_message_json(item));
    }
    inputs.history = acecode::reconstruct_effective_model_history(inputs.history);
    acecode::agent::PromptContextCache reloaded_cache;
    acecode::agent::ApiRequestBuilder reloaded(tools, reloaded_cache);
    const auto resumed = reloaded.build(inputs);
    expect_prefix(second.messages_with_system, resumed.messages_with_system);
    EXPECT_EQ(second.messages_with_system.size(), resumed.messages_with_system.size());
    EXPECT_TRUE(resumed.context_records.empty());
}

TEST(ApiRequestBuilder, UpdatesAndHooksAppendOnceAndNeverDisappear) {
    acecode::ToolExecutor tools;
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    auto inputs = context_inputs();
    inputs.plan_context = "plan active";
    inputs.execution_context = "read-only sandbox";
    inputs.hook_context = "hook says preserve this fact";
    const auto first = builder.build(inputs);
    ASSERT_EQ(first.context_records.size(), 2u);
    commit(inputs, first);
    inputs.history.push_back(message("assistant", "answer"));
    inputs.history.push_back(message("user", "continue"));
    inputs.session = {"project rules v2", "session-v2", ""};
    inputs.plan_context.clear();
    inputs.execution_context = "workspace-write sandbox";
    const auto updated = builder.build(inputs);
    expect_prefix(first.messages_with_system, updated.messages_with_system);
    ASSERT_EQ(updated.context_records.size(), 1u);
    const auto& update = updated.context_records.front();
    EXPECT_NE(update.content.find("project rules v2"), std::string::npos);
    EXPECT_NE(update.content.find("no longer active"), std::string::npos);
    EXPECT_NE(update.content.find("workspace-write"), std::string::npos);
    commit(inputs, updated);
    const auto repeated = builder.build(inputs);
    expect_prefix(updated.messages_with_system, repeated.messages_with_system);
    EXPECT_TRUE(repeated.context_records.empty());
}

TEST(ApiRequestBuilder, TodoWriteDoesNotReplaceEarlierChecklist) {
    acecode::ToolExecutor tools;
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    auto inputs = context_inputs();
    inputs.todos = {{"one", "first task", "pending"}};
    const auto first = builder.build(inputs);
    commit(inputs, first);
    auto call = message("assistant", "");
    call.tool_calls = nlohmann::json::array({{{"id", "todo1"}, {"type", "function"},
        {"function", {{"name", "TodoWrite"}, {"arguments", "{}"}}}}});
    inputs.history.push_back(call);
    auto result = message("tool", "first task completed");
    result.tool_call_id = "todo1";
    inputs.history.push_back(result);
    inputs.todos.front().status = "completed";
    const auto next = builder.build(inputs);
    expect_prefix(first.messages_with_system, next.messages_with_system);
    EXPECT_TRUE(next.context_records.empty());
    EXPECT_EQ(next.messages_with_system.back().content, "first task completed");
}

TEST(ApiRequestBuilder, FreshWindowRestoresChecklistBeforeFinalSummary) {
    acecode::ToolExecutor tools;
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    acecode::agent::RequestContextOptions options;
    options.git_config.emplace();
    options.git_config->enabled = false;
    options.project_config.emplace();
    options.project_config->enabled = false;
    options.todos = {{"task", "restored task", "in_progress"}};
    auto summary = message("user", "summary");
    summary.is_compact_summary = true;
    std::vector<acecode::ChatMessage> replacement{summary};
    replacement.insert(replacement.begin(), builder.fresh_window_snapshot(options, replacement));
    const auto first = builder.compaction_request(options, replacement);
    ASSERT_FALSE(first.messages_with_system.empty());
    EXPECT_TRUE(first.context_records.empty());
    EXPECT_EQ(first.messages_with_system.back().content, "summary");
    EXPECT_NE(replacement.front().content.find("restored task"), std::string::npos);
    options.todos.front().status = "completed";
    const auto next = builder.compaction_request(options, replacement);
    expect_prefix(first.messages_with_system, next.messages_with_system);
    EXPECT_TRUE(next.context_records.empty());
}

TEST(ApiRequestBuilder, PermissionChangeLeavesStaticSystemPromptIdentical) {
    acecode::ToolExecutor tools;
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    acecode::agent::RequestContextOptions options;
    options.sandbox.description = "read-only";
    const auto initial = builder.static_system_prompt(options);
    options.sandbox.description = "workspace-write";
    EXPECT_EQ(initial, builder.static_system_prompt(options));
}

TEST(ApiRequestBuilder, SessionCacheKeySurvivesRebuildAndRemainsPerRequest) {
    acecode::ToolExecutor tools;
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    acecode::agent::RequestContextOptions options;
    options.git_config.emplace();
    options.git_config->enabled = false;
    options.project_config.emplace();
    options.project_config->enabled = false;
    options.memory_session_key = "saved-session-a";
    const auto first = builder.build(builder.capture(options, {}, false));
    options.memory_session_key = "saved-session-b";
    const auto other = builder.build(builder.capture(options, {}, false));
    EXPECT_EQ(first.request_options.prompt_cache_key, "saved-session-a");
    EXPECT_EQ(other.request_options.prompt_cache_key, "saved-session-b");
    acecode::agent::PromptContextCache next_cache;
    acecode::agent::ApiRequestBuilder next_builder(tools, next_cache);
    options.memory_session_key = "saved-session-a";
    const auto resumed = next_builder.compaction_request(options, first.context_records);
    EXPECT_EQ(first.request_options.prompt_cache_key, resumed.request_options.prompt_cache_key);
}
