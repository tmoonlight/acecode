#include <gtest/gtest.h>
#include "llm/tool_protocol_names.hpp"

#include "hooks/hook_manager.hpp"
#include "hooks/hook_runtime.hpp"
#include "utils/joining_thread.hpp"
#include "utils/uuid.hpp"

#include <atomic>
#include <filesystem>
#include <thread>

#include <string>
#include <vector>

namespace {

acecode::NormalizedHook make_hook(std::string id,
                                  std::string event,
                                  std::string matcher,
                                  acecode::HookTrustStatus trust =
                                      acecode::HookTrustStatus::Trusted) {
    acecode::NormalizedHook hook;
    hook.id = std::move(id);
    hook.source_id = "source";
    hook.event_name = std::move(event);
    hook.matcher = std::move(matcher);
    hook.kind = acecode::HookHandlerKind::Command;
    hook.command.command = "hook-command";
    hook.command.timeout_seconds = 1;
    hook.trust_status = trust;
    return hook;
}

acecode::HookProcessResult ok_json_result(const std::string& stdout_text) {
    acecode::HookProcessResult result;
    result.started = true;
    result.exit_code = 0;
    result.stdout_text = stdout_text;
    result.output = stdout_text;
    return result;
}

} // namespace

TEST(HookRuntime, CancelledDispatchSkipsAllSynchronousHooks) {
    acecode::HookRegistrySnapshot registry;
    registry.feature_enabled = true;
    registry.hooks = {make_hook("first", acecode::kCodexHookEventPreToolUse, "*")};
    int calls = 0;
    acecode::HookManager manager(registry, {}, acecode::HookShellRunner{
        [&calls](const auto&, const auto&, int, const auto&) {
            ++calls;
            return ok_json_result("{}");
        }});
    std::atomic<bool> abort{true};
    acecode::HookDispatchRequest request;
    request.event_name = acecode::kCodexHookEventPreToolUse;
    request.matcher_value = "bash";
    request.abort_flag = &abort;
    const auto outcome = manager.dispatch_codex(request);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(outcome.invoked_count, 0);
}

TEST(HookRuntime, NativeSynchronousHookCancelsProcessTreeAndSkipsNextHook) {
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;
    const auto marker = fs::temp_directory_path() / ("hook-cancel-" + acecode::generate_uuid());
    acecode::HookRegistrySnapshot registry;
    registry.feature_enabled = true;
    auto hook = make_hook("blocking", acecode::kCodexHookEventPreToolUse, "*");
    hook.command.timeout_seconds = 10;
#ifdef _WIN32
    hook.command.command_windows = "echo ready > \"" + marker.string() + "\" & ping -n 20 127.0.0.1 > nul";
#else
    hook.command.command = "printf ready > '" + marker.string() + "'; sleep 20";
#endif
    registry.hooks = {hook, make_hook("must-not-run", acecode::kCodexHookEventPreToolUse, "*")};
    acecode::HookManager manager(registry);
    std::atomic<bool> abort{false}, saw_marker{false};
    acecode::JoiningThread cancel([&] {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (fs::exists(marker)) { saw_marker = true; break; }
            std::this_thread::sleep_for(2ms);
        }
        abort = true;
    });
    acecode::HookDispatchRequest request;
    request.event_name = acecode::kCodexHookEventPreToolUse;
    request.matcher_value = "bash";
    request.abort_flag = &abort;
    const auto start = std::chrono::steady_clock::now();
    // A hook that does not read stdin must not block the caller while writing
    // a large tool payload, before the cancellation loop even starts.
    request.payload = {{"tool_output", std::string(1024 * 1024, 'x')}};
    const auto outcome = manager.dispatch_codex(request);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);
    cancel.join();
    EXPECT_TRUE(saw_marker);
    EXPECT_EQ(outcome.invoked_count, 1);
    EXPECT_TRUE(outcome.no_decision);
    EXPECT_FALSE(outcome.blocked);
    EXPECT_FALSE(outcome.replacement_output);
    fs::remove(marker);
}

TEST(HookRuntime, MatcherAliasesMapCodexNamesToAceCodeTools) {
    EXPECT_TRUE(acecode::hook_matcher_matches(
        make_hook("h1", acecode::kCodexHookEventPreToolUse, "Bash"),
        acecode::kCodexHookEventPreToolUse,
        "bash"));
    EXPECT_TRUE(acecode::hook_matcher_matches(
        make_hook("h2", acecode::kCodexHookEventPreToolUse, "Edit"),
        acecode::kCodexHookEventPreToolUse,
        "file_edit"));
    EXPECT_TRUE(acecode::hook_matcher_matches(
        make_hook("h3", acecode::kCodexHookEventPreToolUse, "Write"),
        acecode::kCodexHookEventPreToolUse,
        "file_write"));
    EXPECT_TRUE(acecode::hook_matcher_matches(
        make_hook("h4", acecode::kCodexHookEventPreToolUse, "apply_patch"),
        acecode::kCodexHookEventPreToolUse,
        "file_write"));
    // apply_patch 也是 GPT / Codex 系模型实际调用的原生工具名:同一个 matcher
    // 必须同时命中它,否则用户为 Codex 写的 hooks 在 GPT 模型下静默失效。
    EXPECT_TRUE(acecode::hook_matcher_matches(
        make_hook("h4b", acecode::kCodexHookEventPreToolUse, "apply_patch"),
        acecode::kCodexHookEventPreToolUse,
        "apply_patch"));
}

// 场景:「工具重写」生效(file_write → write),用户照模型的叫法写 matcher "write"。
// 期望:命中原生 file_write;映射关闭时 "write" 只是个不存在的工具名,不命中。
// 回归:改动前 hooks 只认 Claude Code 风格的 Write / Edit 别名,不认模型侧名,
// 三套命名词汇并存时用户写的 matcher 静默失效。
TEST(HookRuntime, MatcherAcceptsModelFacingAliasWhenRewriteActive) {
    {
        acecode::ScopedModelToolNameMappings scoped(
            acecode::ToolProtocolNameMappings{{"file_write", "write"}});
        EXPECT_TRUE(acecode::hook_matcher_matches(
            make_hook("h5", acecode::kCodexHookEventPreToolUse, "write"),
            acecode::kCodexHookEventPreToolUse,
            "file_write"));
        EXPECT_EQ(acecode::canonical_hook_match_value("write"), "file_write");
        EXPECT_EQ(acecode::canonical_hook_match_value("Write"), "file_write");
    }
    // 映射关闭时 "write" 不再是别名:canonical 值原样返回(matcher 本身仍可能
    // 按正则子串命中 file_write,那是既有的正则语义,不在本用例范围内)。
    acecode::ScopedModelToolNameMappings none(acecode::ToolProtocolNameMappings{});
    EXPECT_EQ(acecode::canonical_hook_match_value("write"), "write");
    EXPECT_EQ(acecode::canonical_hook_match_value("Write"), "file_write");
}

TEST(HookRuntime, InvalidRegexProducesDiagnostic) {
    std::vector<acecode::HookDiagnostic> diagnostics;
    EXPECT_FALSE(acecode::hook_matcher_matches(
        make_hook("h1", acecode::kCodexHookEventPreToolUse, "["),
        acecode::kCodexHookEventPreToolUse,
        "bash",
        &diagnostics));
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "HOOK_MATCHER_INVALID_REGEX");
}

TEST(HookRuntime, CommonPayloadIncludesCodexFields) {
    acecode::HookCommonPayloadFields fields;
    fields.session_id = "sid";
    fields.transcript_path = "session.jsonl";
    fields.cwd = "C:/repo";
    fields.hook_event_name = acecode::kCodexHookEventSessionStart;
    fields.model = "gpt-test";
    fields.permission_mode = "default";
    fields.turn_id = "turn";

    auto payload = acecode::build_session_start_hook_payload(fields, "startup");

    EXPECT_EQ(payload["session_id"], "sid");
    EXPECT_EQ(payload["transcript_path"], "session.jsonl");
    EXPECT_EQ(payload["cwd"], "C:/repo");
    EXPECT_EQ(payload["hook_event_name"], acecode::kCodexHookEventSessionStart);
    EXPECT_EQ(payload["model"], "gpt-test");
    EXPECT_EQ(payload["permission_mode"], "default");
    EXPECT_EQ(payload["turn_id"], "turn");
    EXPECT_EQ(payload["source"], "startup");
}

TEST(HookRuntime, EventPayloadBuildersIncludeEventSpecificFields) {
    acecode::HookCommonPayloadFields fields;
    fields.session_id = "sid";
    fields.cwd = "C:/repo";
    fields.hook_event_name = "event";
    fields.model = "model";
    fields.permission_mode = "default";
    fields.turn_id = "turn";

    auto prompt = acecode::build_user_prompt_submit_hook_payload(fields, "hello");
    EXPECT_EQ(prompt["prompt"], "hello");

    auto tool = acecode::build_tool_hook_payload(
        fields,
        "bash",
        nlohmann::json{{"command", "echo hi"}},
        nlohmann::json{{"output", "hi"}, {"success", true}});
    EXPECT_EQ(tool["tool_name"], "bash");
    EXPECT_EQ(tool["tool_input"]["command"], "echo hi");
    EXPECT_EQ(tool["tool_response"]["output"], "hi");

    fields.hook_event_name = acecode::kCodexHookEventPermissionResolved;
    auto resolved = acecode::build_permission_resolved_hook_payload(
        fields,
        "bash",
        nlohmann::json{{"command", "echo hi"}},
        "allow",
        "interactive");
    EXPECT_EQ(resolved["hook_event_name"], "PermissionResolved");
    EXPECT_EQ(resolved["tool_name"], "bash");
    EXPECT_EQ(resolved["permission_decision"], "allow");
    EXPECT_EQ(resolved["permission_source"], "interactive");

    fields.hook_event_name = acecode::kCodexHookEventSessionTitleChanged;
    auto title = acecode::build_session_title_changed_hook_payload(
        fields,
        "修复 \"登录\" & deploy %PATH%",
        "user",
        "user");
    EXPECT_EQ(title["hook_event_name"], "SessionTitleChanged");
    EXPECT_EQ(title["title"], "修复 \"登录\" & deploy %PATH%");
    EXPECT_EQ(title["source"], "user");
    EXPECT_EQ(title["title_source"], "user");

    auto compact = acecode::build_compact_hook_payload(fields, "manual");
    EXPECT_EQ(compact["trigger"], "manual");

    auto stop = acecode::build_stop_hook_payload(fields, true, "last message");
    EXPECT_EQ(stop["stop_hook_active"], true);
    EXPECT_EQ(stop["last_assistant_message"], "last message");
}

TEST(HookRuntime, SessionTitleChangedMatcherUsesTriggerSource) {
    EXPECT_TRUE(acecode::hook_matcher_matches(
        make_hook("resume", acecode::kCodexHookEventSessionTitleChanged, "resume"),
        acecode::kCodexHookEventSessionTitleChanged,
        "resume"));
    EXPECT_FALSE(acecode::hook_matcher_matches(
        make_hook("generated", acecode::kCodexHookEventSessionTitleChanged,
                  "generated"),
        acecode::kCodexHookEventSessionTitleChanged,
        "resume"));
}

TEST(HookRuntime, ExitCodeTwoBlocksWithStderrReason) {
    acecode::HookProcessResult result;
    result.started = true;
    result.exit_code = 2;
    result.stderr_text = "blocked by policy";

    auto parsed = acecode::parse_hook_process_output(
        result, acecode::kCodexHookEventUserPromptSubmit);
    acecode::HookAggregateOutcome aggregate;
    acecode::merge_hook_output(
        aggregate,
        parsed,
        acecode::kCodexHookEventUserPromptSubmit,
        make_hook("h1", acecode::kCodexHookEventUserPromptSubmit, "*"));

    EXPECT_TRUE(aggregate.blocked);
    EXPECT_TRUE(aggregate.denied);
    EXPECT_EQ(aggregate.reason, "blocked by policy");
}

TEST(HookRuntime, PlainStdoutBecomesContextOnlyForSupportedEvents) {
    auto session_parsed = acecode::parse_hook_process_output(
        ok_json_result("plain context"), acecode::kCodexHookEventSessionStart);
    acecode::HookAggregateOutcome session_out;
    acecode::merge_hook_output(
        session_out,
        session_parsed,
        acecode::kCodexHookEventSessionStart,
        make_hook("h1", acecode::kCodexHookEventSessionStart, "*"));
    ASSERT_EQ(session_out.additional_context.size(), 1u);
    EXPECT_EQ(session_out.additional_context[0], "plain context");

    auto tool_parsed = acecode::parse_hook_process_output(
        ok_json_result("plain ignored"), acecode::kCodexHookEventPreToolUse);
    acecode::HookAggregateOutcome tool_out;
    acecode::merge_hook_output(
        tool_out,
        tool_parsed,
        acecode::kCodexHookEventPreToolUse,
        make_hook("h2", acecode::kCodexHookEventPreToolUse, "*"));
    EXPECT_TRUE(tool_out.additional_context.empty());
}

TEST(HookRuntime, JsonOutputMergesPermissionDecisionAndAdditionalContext) {
    auto parsed = acecode::parse_hook_process_output(
        ok_json_result(R"({
            "hookSpecificOutput": {
                "permissionDecision": "deny",
                "permissionDecisionReason": "no rm",
                "additionalContext": "remember this"
            },
            "systemMessage": "visible warning"
        })"),
        acecode::kCodexHookEventPermissionRequest);

    acecode::HookAggregateOutcome aggregate;
    acecode::merge_hook_output(
        aggregate,
        parsed,
        acecode::kCodexHookEventPermissionRequest,
        make_hook("h1", acecode::kCodexHookEventPermissionRequest, "Bash"));

    EXPECT_TRUE(aggregate.denied);
    EXPECT_FALSE(aggregate.allowed);
    EXPECT_EQ(aggregate.reason, "no rm");
    ASSERT_EQ(aggregate.additional_context.size(), 1u);
    EXPECT_EQ(aggregate.additional_context[0], "remember this");
    ASSERT_EQ(aggregate.system_messages.size(), 1u);
    EXPECT_EQ(aggregate.system_messages[0], "visible warning");
}

TEST(HookRuntime, ContinueFalseTakesPrecedenceOverAllowAndCapturesReason) {
    auto parsed = acecode::parse_hook_process_output(
        ok_json_result(R"({
            "continue": false,
            "decision": "allow",
            "reason": "stop now"
        })"),
        acecode::kCodexHookEventStop);

    acecode::HookAggregateOutcome aggregate;
    acecode::merge_hook_output(
        aggregate,
        parsed,
        acecode::kCodexHookEventStop,
        make_hook("h1", acecode::kCodexHookEventStop, "*"));

    EXPECT_TRUE(aggregate.continue_false);
    EXPECT_TRUE(aggregate.allowed);
    EXPECT_EQ(aggregate.reason, "stop now");
}

TEST(HookRuntime, UnsupportedOutputFieldsProduceDiagnostics) {
    auto parsed = acecode::parse_hook_process_output(
        ok_json_result(R"({
            "unknownTop": true,
            "hookSpecificOutput": {
                "unknownNested": "x"
            }
        })"),
        acecode::kCodexHookEventPreToolUse);

    acecode::HookAggregateOutcome aggregate;
    acecode::merge_hook_output(
        aggregate,
        parsed,
        acecode::kCodexHookEventPreToolUse,
        make_hook("h1", acecode::kCodexHookEventPreToolUse, "*"));

    ASSERT_EQ(aggregate.diagnostics.size(), 2u);
    EXPECT_EQ(aggregate.diagnostics[0].code, "HOOK_OUTPUT_UNSUPPORTED_FIELD");
    EXPECT_NE(aggregate.diagnostics[0].message.find("unknownTop"), std::string::npos);
    EXPECT_NE(aggregate.diagnostics[1].message.find("hookSpecificOutput.unknownNested"),
              std::string::npos);
}

TEST(HookManagerRuntime, InactiveHooksDoNotSerializeInvalidUtf8Payload) {
    acecode::HookRegistrySnapshot snapshot;
    int invocations = 0;
    acecode::HookManager manager(snapshot, acecode::HookProcessRunner{},
        [&](const std::string&, const std::string&, int, const std::string&) {
            ++invocations;
            return ok_json_result("{}");
        });
    acecode::HookDispatchRequest request;
    request.event_name = acecode::kCodexHookEventPostToolUse;
    request.matcher_value = "bash";
    request.payload = {{"output", std::string(58, 'x') +
                                  std::string("\xC0\xB4\xD4\xB4", 4)}};
    ASSERT_THROW(request.payload.dump(), nlohmann::json::type_error);

    acecode::HookAggregateOutcome empty;
    ASSERT_NO_THROW(empty = manager.dispatch_codex(request));
    EXPECT_EQ(empty.matched_count, 0u);
    EXPECT_EQ(empty.invoked_count, 0u);

    snapshot.hooks.push_back(make_hook("other-tool", request.event_name,
                                       "AskUserQuestion"));
    snapshot.hooks.push_back(make_hook("other-event",
                                       acecode::kCodexHookEventStop, "*"));
    snapshot.hooks.push_back(make_hook("disabled", request.event_name, "Bash",
                                       acecode::HookTrustStatus::Disabled));
    snapshot.hooks.push_back(make_hook("pending", request.event_name, "Bash",
                                       acecode::HookTrustStatus::PendingReview));
    auto unsupported = make_hook("unsupported", request.event_name, "Bash");
    unsupported.kind = acecode::HookHandlerKind::UnsupportedPrompt;
    snapshot.hooks.push_back(std::move(unsupported));
    auto skipped = make_hook("skipped", request.event_name, "Bash");
    skipped.skipped = true;
    snapshot.hooks.push_back(std::move(skipped));
    auto legacy = make_hook("legacy", request.event_name, "Bash");
    legacy.legacy_direct = true;
    snapshot.hooks.push_back(std::move(legacy));
    auto no_command = make_hook("no-command", request.event_name, "Bash");
    no_command.command.command.clear();
    snapshot.hooks.push_back(std::move(no_command));
    snapshot.hooks.push_back(make_hook("bad-matcher", request.event_name, "["));
    manager.refresh_registry(std::move(snapshot));

    acecode::HookAggregateOutcome inactive;
    ASSERT_NO_THROW(inactive = manager.dispatch_codex(request));
    EXPECT_EQ(inactive.matched_count, 6u);
    EXPECT_EQ(inactive.skipped_count, 6u);
    EXPECT_EQ(inactive.invoked_count, 0u);
    ASSERT_EQ(inactive.diagnostics.size(), 1u);
    EXPECT_EQ(inactive.diagnostics[0].code, "HOOK_MATCHER_INVALID_REGEX");
    EXPECT_EQ(invocations, 0);
}

TEST(HookManagerRuntime, DispatchSkipsPendingAndRunsTrustedHooks) {
    acecode::HookRegistrySnapshot snapshot;
    snapshot.feature_enabled = true;
    snapshot.hooks.push_back(make_hook(
        "pending", acecode::kCodexHookEventPreToolUse, "Bash",
        acecode::HookTrustStatus::PendingReview));
    snapshot.hooks.push_back(make_hook(
        "trusted", acecode::kCodexHookEventPreToolUse, "Bash",
        acecode::HookTrustStatus::Trusted));

    int invocations = 0;
    acecode::HookManager manager(std::move(snapshot),
        acecode::HookProcessRunner{},
        [&](const std::string& command,
            const std::string& stdin_text,
            int timeout_ms,
            const std::string& cwd) {
            (void)command;
            (void)stdin_text;
            (void)timeout_ms;
            (void)cwd;
            ++invocations;
            return ok_json_result(R"({
                "hookSpecificOutput": {
                    "permissionDecision": "deny",
                    "permissionDecisionReason": "blocked"
                }
            })");
        });

    acecode::HookDispatchRequest request;
    request.event_name = acecode::kCodexHookEventPreToolUse;
    request.matcher_value = "bash";
    request.cwd = ".";
    request.payload = nlohmann::json::object();

    auto outcome = manager.dispatch_codex(request);

    EXPECT_EQ(invocations, 1);
    EXPECT_EQ(outcome.matched_count, 2u);
    EXPECT_EQ(outcome.skipped_count, 1u);
    EXPECT_EQ(outcome.invoked_count, 1u);
    EXPECT_TRUE(outcome.denied);
    EXPECT_EQ(outcome.reason, "blocked");
}

TEST(HookManagerRuntime, DispatchSkipsDisabledAndRefreshesRegistrySnapshot) {
    acecode::HookRegistrySnapshot snapshot;
    snapshot.feature_enabled = true;
    snapshot.hooks.push_back(make_hook(
        "disabled", acecode::kCodexHookEventPreToolUse, "Bash",
        acecode::HookTrustStatus::Disabled));

    int invocations = 0;
    acecode::HookManager manager(std::move(snapshot),
        acecode::HookProcessRunner{},
        [&](const std::string&,
            const std::string&,
            int,
            const std::string&) {
            ++invocations;
            return ok_json_result("{}");
        });

    acecode::HookDispatchRequest request;
    request.event_name = acecode::kCodexHookEventPreToolUse;
    request.matcher_value = "bash";
    request.cwd = ".";
    request.payload = nlohmann::json::object();

    auto first = manager.dispatch_codex(request);
    EXPECT_EQ(first.matched_count, 1u);
    EXPECT_EQ(first.skipped_count, 1u);
    EXPECT_EQ(invocations, 0);

    acecode::HookRegistrySnapshot refreshed;
    refreshed.feature_enabled = true;
    refreshed.hooks.push_back(make_hook(
        "trusted", acecode::kCodexHookEventPreToolUse, "Bash",
        acecode::HookTrustStatus::Trusted));
    manager.refresh_registry(std::move(refreshed));

    auto second = manager.dispatch_codex(request);
    EXPECT_EQ(second.invoked_count, 1u);
    EXPECT_EQ(invocations, 1);
    EXPECT_EQ(manager.registry_snapshot().hooks[0].id, "trusted");
}

TEST(HookManagerRuntime, SessionTitleChangedExposesExactTitleToChildEnvironment) {
    acecode::HookRegistrySnapshot snapshot;
    snapshot.feature_enabled = true;
    snapshot.hooks.push_back(make_hook(
        "title", acecode::kCodexHookEventSessionTitleChanged, "user"));

    const std::string expected = "修复 \"登录\" & deploy %PATH%";
    acecode::HookEnvironment captured_environment;
    acecode::HookManager manager(
        std::move(snapshot),
        acecode::HookProcessRunner{},
        [&](const std::string&,
            const std::string& stdin_text,
            int,
            const std::string&,
            const acecode::HookEnvironment& environment) {
            const auto payload = nlohmann::json::parse(stdin_text);
            EXPECT_EQ(payload["title"], expected);
            captured_environment = environment;
            return ok_json_result("{}");
        });

    acecode::HookDispatchRequest request;
    request.event_name = acecode::kCodexHookEventSessionTitleChanged;
    request.matcher_value = "user";
    request.cwd = ".";
    request.payload = nlohmann::json{
        {"hook_event_name", acecode::kCodexHookEventSessionTitleChanged},
        {"title", expected},
        {"source", "user"},
        {"title_source", "user"},
    };

    auto outcome = manager.dispatch_codex(request);

    EXPECT_EQ(outcome.invoked_count, 1u);
    auto it = captured_environment.find("ACECODE_HOOK_SESSION_TITLE");
    ASSERT_NE(it, captured_environment.end());
    EXPECT_EQ(it->second, expected);
}
