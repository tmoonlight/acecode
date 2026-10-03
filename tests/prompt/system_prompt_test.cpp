// 覆盖 src/prompt/system_prompt.{hpp,cpp} 的 prompt cache 分区逻辑:
// - 静态 system prompt 不包含每次请求/会话可能变化的 context
// - memory / project_instructions 进入 provider-facing session context
// - full tool schema 只走 provider tools array,不重复塞进 system prompt

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_registry.hpp"
#include "prompt/memory_prompt.hpp"
#include "memory/memory_types.hpp"
#include "prompt/system_prompt.hpp"
#include "llm/tool_protocol_names.hpp"
#include "tool/tool_executor.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

TEST(SystemPromptSandbox, StableStateAndExplicitEscalationInstructions) {
    acecode::ToolExecutor tools;
    acecode::SystemPromptSandboxState state{"workspace-write; writable: C:/work; network: not enforced"};
    const auto build = [&] {
        return acecode::build_system_prompt(tools, "C:/work", nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, true, nullptr, &state);
    };
    const auto first = build();
    EXPECT_EQ(first, build());
    EXPECT_NE(first.find("Shell sandbox:"), std::string::npos);
    EXPECT_NE(first.find("network: not enforced"), std::string::npos);
    // align-codex-sandboxing:指引先讲最小申请(with_additional_permissions),
    // 再讲沙盒外执行(require_escalated),两者都要 justification。
    EXPECT_NE(first.find("with_additional_permissions"), std::string::npos);
    EXPECT_NE(first.find("require_escalated"), std::string::npos);
    EXPECT_LT(first.find("with_additional_permissions"), first.find("require_escalated"));
    EXPECT_NE(first.find("justification"), std::string::npos);
}

namespace {

#ifdef _WIN32
constexpr const char* kHomeEnvName = "USERPROFILE";
#else
constexpr const char* kHomeEnvName = "HOME";
#endif

void set_env(const char* n, const std::string& v) {
#ifdef _WIN32
    _putenv_s(n, v.c_str());
#else
    setenv(n, v.c_str(), 1);
#endif
}

class SystemPromptTest : public ::testing::Test {
protected:
    fs::path temp_home;
    std::string prev_home;

    void SetUp() override {
        const char* e = std::getenv(kHomeEnvName);
        prev_home = e ? e : "";
        temp_home = fs::temp_directory_path() /
                    fs::path("acecode-sysprompt-" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(temp_home, ec);
        fs::create_directories(temp_home);
        set_env(kHomeEnvName, temp_home.string());
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_home, ec);
        set_env(kHomeEnvName, prev_home);
    }
};

void write_file(const fs::path& p, const std::string& c) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary);
    ofs << c;
}

} // namespace

// 场景:memory / project_instructions 都不提供,prompt 里无对应段头
TEST_F(SystemPromptTest, EmptyInputsOmitSections) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_EQ(out.find("# User Memory"), std::string::npos);
    EXPECT_EQ(out.find("# Project Instructions"), std::string::npos);
}

// 场景:# Environment 只携带不会随时间变化的环境事实。当前日期不得动态
// 注入 system prompt,避免跨日期改变缓存前缀。
TEST_F(SystemPromptTest, EnvironmentOmitsDynamicDate) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());

    EXPECT_NE(out.find("# Environment"), std::string::npos);
    EXPECT_NE(out.find("- OS: "), std::string::npos);
    EXPECT_NE(out.find("- Shell: "), std::string::npos);
    EXPECT_NE(out.find("- Working directory: " + temp_home.string()),
              std::string::npos);
    EXPECT_EQ(out.find("- Today's date: "), std::string::npos);
    EXPECT_NE(out.find("- Session worktree: inactive"), std::string::npos);
    EXPECT_NE(out.find("Worktree session switches are exclusive"),
              std::string::npos);
    EXPECT_NE(out.find("Merging a worktree branch into master/main does not"),
              std::string::npos);
}

TEST_F(SystemPromptTest, ActiveWorktreeRequiresExitToolToReturn) {
    acecode::ToolExecutor tools;
    acecode::SystemPromptWorktreeState worktree;
    worktree.active = true;
    worktree.worktree_path = (temp_home / "wt").string();
    worktree.worktree_branch = "worktree-demo";
    worktree.original_cwd = temp_home.string();

    std::string out = acecode::build_system_prompt(
        tools, worktree.worktree_path,
        /*skills=*/nullptr, /*memory=*/nullptr, /*memory_cfg=*/nullptr,
        /*project_instructions_cfg=*/nullptr, /*effective_tool_policy=*/nullptr,
        &worktree);

    EXPECT_NE(out.find("- Session worktree: active on branch worktree-demo"),
              std::string::npos);
    EXPECT_NE(out.find("- Session worktree path: " + worktree.worktree_path),
              std::string::npos);
    EXPECT_NE(out.find("- Session worktree return cwd: " + worktree.original_cwd),
              std::string::npos);
    EXPECT_NE(out.find("Returning this session to the main checkout requires `ExitWorktree`"),
              std::string::npos);
    EXPECT_EQ(out.find("- Session worktree: inactive"), std::string::npos);
}

// 场景:静态 system prompt 不能包含每次请求都会变化的内容,否则 prompt
// cache 前缀会被从最前面打穿。日期只精确到天,时分秒一律不进 prompt。
TEST_F(SystemPromptTest, StaticSystemPromptIsByteStableAcrossCalls) {
    acecode::ToolExecutor tools;
    const std::string first =
        acecode::build_system_prompt(tools, temp_home.string());
    const std::string second =
        acecode::build_system_prompt(tools, temp_home.string());

    EXPECT_EQ(first, second);
    EXPECT_EQ(first.find("[当前环境状态]"), std::string::npos);
    EXPECT_EQ(first.find("Current local date/time"), std::string::npos);
}

TEST_F(SystemPromptTest, UserAtPathReferenceContractIsExplicit) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_NE(out.find("an `@path` or `@\"path with spaces\"` token"),
              std::string::npos);
    EXPECT_NE(out.find("Resolve relative paths from the current working directory"),
              std::string::npos);
    EXPECT_NE(out.find("an explicitly selected local file or directory may use an absolute path"),
              std::string::npos);
    EXPECT_NE(out.find("content is not automatically attached"),
              std::string::npos);
    EXPECT_NE(out.find("do not assume a referenced directory was recursively loaded"),
              std::string::npos);
    EXPECT_NE(out.find("Do not reject a referenced file only because it is large, binary"),
              std::string::npos);
    EXPECT_NE(out.find("a safely converted or split working copy"),
              std::string::npos);
    EXPECT_NE(out.find("instead of treating the attachment limit as the end of the task"),
              std::string::npos);
}

// 场景:插入到历史中部的可变上下文块必须内容驱动:输入不变则逐字节
// 不变。这里曾经拼进一个秒级时间戳,导致同一回合内每次工具调用往返都把
// 缓存前缀从插入点截断,整条尾巴全价重算。
TEST_F(SystemPromptTest, SessionContextIsByteStableForUnchangedInputs) {
    acecode::PromptContextBlock memory_snapshot;
    memory_snapshot.content = "# Memory\n- [user] a\n";
    memory_snapshot.cache_key = "memory:fixed";

    const auto first = acecode::build_session_context_prompt(
        temp_home.string(), &memory_snapshot, nullptr, nullptr, 128000);
    const auto second = acecode::build_session_context_prompt(
        temp_home.string(), &memory_snapshot, nullptr, nullptr, 128000);

    EXPECT_EQ(first.content, second.content);
    EXPECT_EQ(first.cache_key, second.cache_key);
    EXPECT_EQ(first.content.find("[当前环境状态]"), std::string::npos);
}

// 场景:记忆快照块非空 -> 原样进入 session context;静态 system prompt 不含记忆条目。
TEST_F(SystemPromptTest, MemorySnapshotGoesIntoSessionContextOnly) {
    acecode::PromptContextBlock memory_snapshot;
    memory_snapshot.content = "# Memory\n\n## Global memory\n- [user] user_profile - senior Go dev\n";
    memory_snapshot.cache_key = "memory:abc";

    acecode::ToolExecutor tools;
    acecode::MemoryConfig mcfg;
    std::string out = acecode::build_system_prompt(
        tools, temp_home.string(), /*skills=*/nullptr, /*memory=*/nullptr, &mcfg, /*project=*/nullptr);
    EXPECT_EQ(out.find("user_profile"), std::string::npos);

    auto context = acecode::build_session_context_prompt(
        temp_home.string(), &memory_snapshot, nullptr, nullptr, 128000);
    EXPECT_NE(context.content.find("## Global memory"), std::string::npos);
    EXPECT_NE(context.content.find("user_profile"), std::string::npos);

    // 空快照(两个作用域都没有条目 / 记忆关闭)不产生任何 session context。
    acecode::PromptContextBlock empty;
    EXPECT_TRUE(acecode::build_session_context_prompt(
        temp_home.string(), &empty, nullptr, nullptr, 128000).content.empty());
}

// 场景:cwd 下有 AGENTS.md -> provider-facing Project Instructions context,
// 静态 system prompt 不包含项目文件内容。
TEST_F(SystemPromptTest, ProjectInstructionsContextWithSource) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENTS.md", "# rules\nuse goroutines\n");

    acecode::ToolExecutor tools;
    acecode::ProjectInstructionsConfig pcfg;
    std::string out = acecode::build_system_prompt(
        tools, repo.string(), /*skills=*/nullptr,
        /*memory=*/nullptr, /*memcfg=*/nullptr, &pcfg);
    EXPECT_EQ(out.find("# Project Instructions"), std::string::npos);
    EXPECT_EQ(out.find("goroutines"), std::string::npos);

    auto context = acecode::build_project_instructions_context_prompt(repo.string(), &pcfg);
    EXPECT_NE(context.content.find("# Project Instructions"), std::string::npos);
    EXPECT_NE(context.content.find("AGENTS.md"), std::string::npos);
    EXPECT_NE(context.content.find("goroutines"), std::string::npos);
    EXPECT_FALSE(context.cache_key.empty());
}

// 场景:有 CLAUDE.md 也会被当作 project instructions context(compat 路径)
TEST_F(SystemPromptTest, ClaudeMdFallback) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "CLAUDE.md", "# legacy claude rules\n");
    acecode::ToolExecutor tools;
    acecode::ProjectInstructionsConfig pcfg;
    std::string out = acecode::build_system_prompt(
        tools, repo.string(), /*skills=*/nullptr,
        /*memory=*/nullptr, /*memcfg=*/nullptr, &pcfg);
    EXPECT_EQ(out.find("# Project Instructions"), std::string::npos);
    EXPECT_EQ(out.find("legacy claude rules"), std::string::npos);

    auto context = acecode::build_project_instructions_context_prompt(repo.string(), &pcfg);
    EXPECT_NE(context.content.find("# Project Instructions"), std::string::npos);
    EXPECT_NE(context.content.find("legacy claude rules"), std::string::npos);
    EXPECT_NE(context.content.find("CLAUDE.md"), std::string::npos);
}

// 场景:cfg.enabled=false 时即便有 AGENTS.md 也不注入
TEST_F(SystemPromptTest, ProjectInstructionsDisabledByCfg) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENTS.md", "content\n");
    acecode::ToolExecutor tools;
    acecode::ProjectInstructionsConfig pcfg;
    pcfg.enabled = false;
    std::string out = acecode::build_system_prompt(
        tools, repo.string(), /*skills=*/nullptr,
        /*memory=*/nullptr, /*memcfg=*/nullptr, &pcfg);
    EXPECT_EQ(out.find("# Project Instructions"), std::string::npos);

    auto context = acecode::build_project_instructions_context_prompt(repo.string(), &pcfg);
    EXPECT_TRUE(context.content.empty());
}

TEST_F(SystemPromptTest, CustomInstructionsContextAppearsWhenNonEmpty) {
    acecode::ToolExecutor tools;
    acecode::CustomInstructionsConfig custom_cfg;
    custom_cfg.text = "Always answer in concise Chinese.";

    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_EQ(out.find("# Custom Instructions"), std::string::npos);
    EXPECT_EQ(out.find("concise Chinese"), std::string::npos);

    auto context = acecode::build_custom_instructions_context_prompt(&custom_cfg);
    EXPECT_NE(context.content.find("# Custom Instructions"), std::string::npos);
    EXPECT_NE(context.content.find("concise Chinese"), std::string::npos);
    EXPECT_NE(context.content.find("do not override"), std::string::npos);
    EXPECT_FALSE(context.cache_key.empty());
}

TEST_F(SystemPromptTest, CustomInstructionsOmittedWhenWhitespaceOnly) {
    acecode::CustomInstructionsConfig custom_cfg;
    custom_cfg.text = " \n\t ";

    auto context = acecode::build_custom_instructions_context_prompt(&custom_cfg);
    EXPECT_TRUE(context.content.empty());
    EXPECT_TRUE(context.cache_key.empty());
}

TEST_F(SystemPromptTest, SessionContextIncludesCustomInstructions) {
    acecode::CustomInstructionsConfig custom_cfg;
    custom_cfg.text = "Use repository-specific wording.";
    acecode::PromptContextCategoryBytes category_bytes;

    auto context = acecode::build_session_context_prompt(
        temp_home.string(),
        /*memory_snapshot=*/nullptr,
        /*project_instructions_cfg=*/nullptr,
        /*skills=*/nullptr,
        /*context_window_tokens=*/0,
        &custom_cfg,
        /*git_status_snapshot=*/{},
        /*expert=*/nullptr,
        /*expert_member_id=*/{},
        &category_bytes);

    EXPECT_NE(context.content.find("<system-reminder>"), std::string::npos);
    EXPECT_NE(context.content.find("# Custom Instructions"), std::string::npos);
    EXPECT_NE(context.content.find("repository-specific wording"), std::string::npos);
    EXPECT_FALSE(context.cache_key.empty());
    EXPECT_GT(category_bytes.project_rules, 0u);
    EXPECT_EQ(category_bytes.skills, 0u);
    EXPECT_GT(category_bytes.dynamic_context, 0u);
    EXPECT_EQ(
        category_bytes.project_rules + category_bytes.skills +
            category_bytes.dynamic_context,
        context.content.size());
}

// 场景:full tool schema 不再重复塞进静态 system prompt,避免工具 schema 变化
// 打穿前缀缓存。结构化 schema 仍由 provider tools array 发送。
TEST_F(SystemPromptTest, StaticPromptDoesNotDuplicateToolSchemas) {
    acecode::ToolExecutor tools;
    acecode::ToolDef def;
    def.name = "example_tool";
    def.description = "Example tool description.";
    def.parameters = {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "Path to read"}}}
        }},
    };
    acecode::ToolImpl impl;
    impl.definition = def;
    impl.execute = [](const std::string&, const acecode::ToolContext&) {
        return acecode::ToolResult{"ok", true};
    };
    tools.register_tool(impl);

    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_NE(out.find("# Tool Schemas"), std::string::npos);
    EXPECT_EQ(out.find("## example_tool"), std::string::npos);
    EXPECT_EQ(out.find("Parameters:"), std::string::npos);
    EXPECT_EQ(out.find("\"properties\""), std::string::npos);
}

// 场景:hash helper 稳定区分静态 prompt / mutable context / tools 三类变化。
TEST_F(SystemPromptTest, PromptCacheDiagnosticsSeparatePromptContextAndTools) {
    acecode::ToolDef tool_a;
    tool_a.name = "a";
    tool_a.description = "A";
    tool_a.parameters = {{"type", "object"}};
    acecode::ToolDef tool_b = tool_a;
    tool_b.description = "B";

    std::string static_prompt = "stable";
    auto d1 = acecode::build_prompt_cache_diagnostics(static_prompt, "ctx1", {tool_a});
    auto d2 = acecode::build_prompt_cache_diagnostics(static_prompt, "ctx2", {tool_a});
    auto d3 = acecode::build_prompt_cache_diagnostics(static_prompt, "ctx1", {tool_b});

    EXPECT_EQ(d1.static_system_prompt_hash, d2.static_system_prompt_hash);
    EXPECT_NE(d1.mutable_context_hash, d2.mutable_context_hash);
    EXPECT_EQ(d1.tool_schema_hash, d2.tool_schema_hash);

    EXPECT_EQ(d1.static_system_prompt_hash, d3.static_system_prompt_hash);
    EXPECT_EQ(d1.mutable_context_hash, d3.mutable_context_hash);
    EXPECT_NE(d1.tool_schema_hash, d3.tool_schema_hash);
}

// 场景:prompt 必须包含 "# Task completion protocol" 段 + 工具名,
// 并且明确 AskUserQuestion 不是"交还控制权"的工具。
TEST_F(SystemPromptTest, TaskCompletionProtocolAppears) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_NE(out.find("# Task completion protocol"), std::string::npos);
    EXPECT_NE(out.find("task_complete"), std::string::npos);
    EXPECT_NE(out.find("AskUserQuestion"), std::string::npos);
    EXPECT_NE(out.find("The summary is rendered as Markdown"), std::string::npos);
    EXPECT_NE(out.find("collapsing everything into one long line"), std::string::npos);
    // 反对 "should I proceed?" 类 prose 问题
    EXPECT_NE(out.find("Should I proceed"), std::string::npos);
    // 必须说清 AskUserQuestion 不是终止器,是辅助决策工具
    EXPECT_NE(out.find("NOT a way to hand control back"),
              std::string::npos);
}

TEST_F(SystemPromptTest, EffectiveToolPolicyOmitsDisabledToolGuidance) {
    acecode::ToolExecutor tools;
    auto register_tool = [&](const std::string& name) {
        acecode::ToolImpl impl;
        impl.definition.name = name;
        impl.definition.description = "test tool";
        impl.definition.parameters = nlohmann::json::object();
        impl.execute = [](const std::string&, const acecode::ToolContext&) {
            return acecode::ToolResult{"ok", true};
        };
        ASSERT_TRUE(tools.register_tool(impl));
    };
    for (const char* name : {
             "file_read", "file_edit", "file_write", "grep", "glob", "bash",
             "AskUserQuestion", "task_complete", "skill_view", "skills_list"}) {
        register_tool(name);
    }

    // 模型侧名只在「工具重写」生效时出现,这里显式启用内置种子映射。
    acecode::ScopedModelToolNameMappings scoped(
        acecode::default_model_tool_name_mappings());
    acecode::ToolCapabilityPolicy policy;
    policy.builtin_tools =
        std::unordered_set<std::string>{"file_read"};
    policy.mcp_servers = std::unordered_set<std::string>{};
    const std::string out = acecode::build_system_prompt(
        tools, temp_home.string(), nullptr, nullptr, nullptr, nullptr, &policy);

    EXPECT_NE(out.find("`read`"), std::string::npos);
    EXPECT_EQ(out.find("file_read"), std::string::npos);
    for (const char* denied : {
             "file_edit", "file_write", "grep", "glob",
             "AskUserQuestion", "task_complete", "skill_view", "skills_list"}) {
        EXPECT_EQ(out.find(denied), std::string::npos) << denied;
    }
    // On POSIX the environment section may legitimately report /bin/bash as
    // the user's shell even when the bash tool is disabled. Assert on the
    // tool-specific guidance instead of the ambient shell name.
    EXPECT_EQ(out.find("# User Shell Mode"), std::string::npos);
    EXPECT_EQ(out.find("<bash-input>"), std::string::npos);
}

// 场景:工具使用与进度更新文案应鼓励同一 assistant turn 中批量发出独立工具调用,
// 而不是形成一句旁白配一个工具调用的低密度交替模式。
TEST_F(SystemPromptTest, PromptEncouragesBatchedToolCallsWithoutPerCallNarration) {
    acecode::ScopedModelToolNameMappings scoped(
        acecode::default_model_tool_name_mappings());
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());

    EXPECT_NE(out.find("batch them in the same assistant message"),
              std::string::npos);
    EXPECT_NE(out.find("Do not add a progress sentence before each individual tool call"),
              std::string::npos);
    EXPECT_NE(out.find("Do not narrate every tool call"),
              std::string::npos);
    EXPECT_NE(out.find("prefer silent batches of tool calls"),
              std::string::npos);
    EXPECT_NE(out.find("\"Let me read this file.\" followed by one `read`"),
              std::string::npos);
    EXPECT_EQ(out.find("you will produce many assistant messages between tool calls"),
              std::string::npos);
}

TEST_F(SystemPromptTest, PromptUsesClaudeStyleReadFailureGuidanceAndGuidesScratchScripts) {
    acecode::ScopedModelToolNameMappings scoped(
        acecode::default_model_tool_name_mappings());
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());

    EXPECT_NE(out.find("`edit` will error if you attempt an edit without reading the file"), std::string::npos);
    EXPECT_NE(out.find("`write` will fail if you did not read the file first"), std::string::npos);
    EXPECT_NE(out.find("Do not call `read` again for the same file/range"), std::string::npos);
    EXPECT_NE(out.find("Do not re-read a file only to verify a successful edit/write"), std::string::npos);
    EXPECT_NE(out.find("ACECODE_TMPDIR"), std::string::npos);
    EXPECT_NE(out.find("only as the leading path component"), std::string::npos);
    EXPECT_NE(out.find("Never embed the alias inside another path"), std::string::npos);
    EXPECT_EQ(out.find("Before editing or overwriting an existing non-empty file, read the target file first"), std::string::npos);
    EXPECT_EQ(out.find("partial reads are only enough for range edits"), std::string::npos);
    EXPECT_EQ(out.find("start_line/end_line/expected_hash"), std::string::npos);
    EXPECT_EQ(out.find("metadata/range edit"), std::string::npos);
    EXPECT_EQ(out.find("file_read"), std::string::npos);
    EXPECT_EQ(out.find("file_edit"), std::string::npos);
    EXPECT_EQ(out.find("file_write"), std::string::npos);
}

// 场景:「工具重写」未启用(进程默认)。
// 期望:system prompt 里的工具指引直接使用原生名 file_read / file_edit /
// file_write,与发给模型的工具表一致;不出现任何 OpenCode 别名。
TEST_F(SystemPromptTest, PromptUsesNativeToolNamesWhenNoRewriteIsActive) {
    acecode::ScopedModelToolNameMappings none({});
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());

    EXPECT_NE(out.find("`file_edit` will error if you attempt an edit without reading the file"), std::string::npos);
    EXPECT_NE(out.find("`file_write` will fail if you did not read the file first"), std::string::npos);
    EXPECT_NE(out.find("Do not call `file_read` again for the same file/range"), std::string::npos);
    EXPECT_EQ(out.find("`read`"), std::string::npos);
    EXPECT_EQ(out.find("`edit`"), std::string::npos);
    EXPECT_EQ(out.find("`write`"), std::string::npos);
}

// 场景:Windows 平台 build prompt 必须注入 "# Shell Command Guidance (Windows)" 段。
// 回归测试:用户在 acecode 里让 LLM 跑 `mkdir -p testfolder1`,因为 bash_tool
// 在 Windows 上走 cmd.exe /c,cmd.exe 的 mkdir 不认 -p,把 -p 当成第二个目录名,
// 结果创建出 "-p/" 和 "testfolder1/" 两个目录(claudecodehaha 没这问题是因为它
// 强制走 git-bash)。方案 C:不换 shell,改提示词把高频 cmd.exe vs POSIX 分歧
// 写进 system prompt。这里特意 assert "mkdir -p" 反例本身,防止有人把 guidance
// 段瘦身时把这条最关键的反例删掉。
TEST_F(SystemPromptTest, WindowsShellGuidanceInjected) {
#ifdef _WIN32
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_NE(out.find("# Shell Command Guidance (Windows)"), std::string::npos);
    EXPECT_NE(out.find("mkdir -p"), std::string::npos);
    EXPECT_NE(out.find("rd /s /q"), std::string::npos);
    EXPECT_NE(out.find("%VAR%"), std::string::npos);
#else
    GTEST_SKIP() << "Windows-only guidance";
#endif
}

// 场景:POSIX 平台(Linux/macOS)build prompt 时,Windows-only 段必须不出现,
// 避免污染普通用户的 prompt 浪费 token + 误导 LLM。
TEST_F(SystemPromptTest, PosixPromptStaysCleanOfWindowsGuidance) {
#ifndef _WIN32
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_EQ(out.find("# Shell Command Guidance (Windows)"), std::string::npos);
    EXPECT_EQ(out.find("cmd.exe"), std::string::npos);
#else
    GTEST_SKIP() << "POSIX-only assertion";
#endif
}

// 场景:acecode 以软件工程为主能力,但不应把"非代码"当作拒绝理由
TEST_F(SystemPromptTest, GeneralNonCodeRequestsAreAllowed) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());

    EXPECT_NE(out.find("primary product capability"), std::string::npos);
    EXPECT_NE(out.find("not a restriction"), std::string::npos);
    EXPECT_NE(out.find("not about code"), std::string::npos);
    EXPECT_NE(out.find("not tied to the current project"), std::string::npos);
    EXPECT_NE(out.find("non-code help"), std::string::npos);
    EXPECT_NE(out.find("forcing them into a codebase frame"), std::string::npos);
}

TEST_F(SystemPromptTest, SwarmModeContextIsRequestLocalAndPolicyGated) {
    EXPECT_TRUE(
        acecode::build_swarm_mode_context_prompt(false, true).empty());
    EXPECT_TRUE(
        acecode::build_swarm_mode_context_prompt(true, false).empty());

    const std::string swarm =
        acecode::build_swarm_mode_context_prompt(true, true);
    EXPECT_NE(swarm.find("# Swarm Mode"), std::string::npos);
    EXPECT_NE(swarm.find("two or three"), std::string::npos);
    EXPECT_NE(swarm.find("spawn_subagent"), std::string::npos);
    EXPECT_NE(swarm.find("wait=false"), std::string::npos);
    EXPECT_NE(swarm.find("wait_subagent"), std::string::npos);
    EXPECT_NE(swarm.find("read-heavy"), std::string::npos);
    EXPECT_NE(swarm.find("same-file"), std::string::npos);
    EXPECT_NE(swarm.find("final verification"), std::string::npos);

    acecode::ToolExecutor tools;
    const std::string static_prompt =
        acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_EQ(static_prompt.find("# Swarm Mode"), std::string::npos);
}

// ---------------------------------------------------------------------------
// # Environment 的视觉能力行(回归会话 20260830-024351-9599)
//
// bug 表现:主模型自己带 vision 能力、图片也确实发到了它手上,它却先 skill_view
// 了 vision-image-reader、再调 vision_analyze 绕道看图。根因之一是 system prompt
// 从头到尾没有一行告诉模型"你自己能不能看图" —— 而工具与 skill 的触发条件写的是
// "当模型不能可靠看图时",这个"可靠"要模型自我评估,它评估不了,于是保守调用。
// 这里把它变成模型可直接读取的硬事实。
// ---------------------------------------------------------------------------

// 场景:当前模型能看图(默认 fail-open 值)。
// 期望行为:Environment 里写明 Yes,并显式禁止绕道调 vision_analyze / 加载
//   vision-image-reader skill。
TEST_F(SystemPromptTest, EnvironmentDeclaresActiveModelCanReadImages) {
    acecode::ToolExecutor tools;
    const std::string out = acecode::build_system_prompt(
        tools, temp_home.string(),
        /*skills=*/nullptr, /*memory=*/nullptr, /*memory_cfg=*/nullptr,
        /*project_instructions_cfg=*/nullptr, /*effective_tool_policy=*/nullptr,
        /*worktree=*/nullptr, /*active_model_can_read_images=*/true);

    EXPECT_NE(out.find("- Active model can read images directly: Yes"),
              std::string::npos);
    EXPECT_NE(out.find("do NOT call `vision_analyze`"), std::string::npos);
    EXPECT_EQ(out.find("- Active model can read images directly: No"),
              std::string::npos);
}

// 场景:当前模型看不见图。
// 期望行为:Environment 写明 No,并指向 vision_analyze —— 这条是该工具存在的
//   本来用途,不能被上面的禁止文案连坐。
TEST_F(SystemPromptTest, EnvironmentDeclaresActiveModelCannotReadImages) {
    acecode::ToolExecutor tools;
    const std::string out = acecode::build_system_prompt(
        tools, temp_home.string(),
        /*skills=*/nullptr, /*memory=*/nullptr, /*memory_cfg=*/nullptr,
        /*project_instructions_cfg=*/nullptr, /*effective_tool_policy=*/nullptr,
        /*worktree=*/nullptr, /*active_model_can_read_images=*/false);

    EXPECT_NE(out.find("- Active model can read images directly: No"),
              std::string::npos);
    EXPECT_NE(out.find("use `vision_analyze` to inspect them"), std::string::npos);
    EXPECT_EQ(out.find("do NOT call `vision_analyze`"), std::string::npos);
}

// 场景:同一模型能力下重复构建 system prompt。
// 期望行为:逐字节相同。这一位只随模型切换变化,留在可缓存的静态前缀里才不会
//   每回合打穿 prompt cache —— 与 StaticSystemPromptIsByteStableAcrossCalls
//   守的是同一条不变量。
TEST_F(SystemPromptTest, VisionEnvironmentLineIsByteStableForSameCapability) {
    acecode::ToolExecutor tools;
    const auto build = [&](bool can_read_images) {
        return acecode::build_system_prompt(
            tools, temp_home.string(),
            /*skills=*/nullptr, /*memory=*/nullptr, /*memory_cfg=*/nullptr,
            /*project_instructions_cfg=*/nullptr,
            /*effective_tool_policy=*/nullptr,
            /*worktree=*/nullptr, can_read_images);
    };

    EXPECT_EQ(build(true), build(true));
    EXPECT_EQ(build(false), build(false));
    // 能力不同必须产出不同前缀,否则这一位等于没注入。
    EXPECT_NE(build(true), build(false));
}

// ---------------------------------------------------------------------------
// # Environment 的 Shell / Toolchains 行与按终端家族切换的语法指引
// (openspec: agent-default-terminal / agent-toolchain-directories)。
// environment 为空时必须保持改动前输出,上面的老用例即回归哨兵。
// ---------------------------------------------------------------------------

namespace {
std::string build_with_env(const fs::path& cwd, const acecode::SystemPromptEnvironment& env) {
    acecode::ToolExecutor tools;
    return acecode::build_system_prompt(tools, cwd.string(), nullptr, nullptr, nullptr,
                                        nullptr, nullptr, nullptr, true, &env);
}
}  // namespace

// 场景:解析出的终端是 PowerShell。
// 期望:Shell 行写家族与程序路径;出现 PowerShell 指引段,不出现 cmd 指引段。
TEST_F(SystemPromptTest, PowerShellTerminalSwitchesGuidance) {
    acecode::SystemPromptEnvironment env;
    env.terminal_family = "powershell";
    env.terminal_program = "C:\\Program Files\\PowerShell\\7\\pwsh.exe";
    std::string out = build_with_env(temp_home, env);
    EXPECT_NE(out.find("- Shell: powershell (C:\\Program Files\\PowerShell\\7\\pwsh.exe)"),
              std::string::npos);
    EXPECT_NE(out.find("# Shell Command Guidance (PowerShell)"), std::string::npos);
    EXPECT_NE(out.find("$env:ACECODE_TMPDIR"), std::string::npos);
    EXPECT_NE(out.find("Remove-Item -Recurse -Force"), std::string::npos);
    EXPECT_EQ(out.find("# Shell Command Guidance (Windows)"), std::string::npos);
    EXPECT_EQ(out.find("rd /s /q"), std::string::npos)
        << "cmd 专属的删除语法不该出现在 PowerShell 指引里";
}

// 场景:PowerShell 终端下的编码指引(fix-feedback-0924 第 4 条:agent 在 5.1 下用
// Get-Content 读无 BOM 的 UTF-8 源码得到乱码,随后 110 次迭代都在猜原文案)。
// 期望:PowerShell 家族出现编码说明(UTF-8 写法、乱码时重读而不是猜原文);同一环境
// 两次构建逐字节相同(prompt cache 前缀不变量);cmd / Git Bash 家族不出现;
// 新增文案不写死文件工具的原生名(模型侧工具名必须动态取)。
TEST_F(SystemPromptTest, PowerShellGuidanceWarnsAboutEncoding) {
    acecode::SystemPromptEnvironment env;
    env.terminal_family = "powershell";
    env.terminal_program = "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    const std::string out = build_with_env(temp_home, env);
    for (const char* needle : {"- Text encoding:", "[Text.UTF8Encoding]::new($false)",
                               "Join-Path $PWD", "mojibake", "-Encoding Default",
                               "Never guess the original wording"}) {
        EXPECT_NE(out.find(needle), std::string::npos) << needle;
    }
    EXPECT_EQ(out, build_with_env(temp_home, env));
    const auto begin = out.find("- Text encoding:");
    const auto end = out.find('\n', begin);
    ASSERT_NE(begin, std::string::npos);
    const std::string bullet = out.substr(begin, end - begin);
    for (const char* native : {"file_read", "file_edit", "file_write"}) {
        EXPECT_EQ(bullet.find(native), std::string::npos) << native;
    }

    for (const char* family : {"cmd", "bash"}) {
        acecode::SystemPromptEnvironment other;
        other.terminal_family = family;
        other.terminal_program = "C:\\x.exe";
        EXPECT_EQ(build_with_env(temp_home, other).find("mojibake"), std::string::npos)
            << family;
    }
}

// 场景:解析出的终端是 cmd(用户手动选回)。
// 期望:原有的 Windows cmd 指引段完整保留(mkdir -p / rd /s /q / %VAR% 三个哨兵)。
TEST_F(SystemPromptTest, CmdTerminalKeepsLegacyGuidance) {
    acecode::SystemPromptEnvironment env;
    env.terminal_family = "cmd";
    env.terminal_program = "C:\\Windows\\System32\\cmd.exe";
    std::string out = build_with_env(temp_home, env);
    EXPECT_NE(out.find("- Shell: cmd (C:\\Windows\\System32\\cmd.exe)"), std::string::npos);
    EXPECT_NE(out.find("# Shell Command Guidance (Windows)"), std::string::npos);
    EXPECT_NE(out.find("mkdir -p"), std::string::npos);
    EXPECT_NE(out.find("rd /s /q"), std::string::npos);
    EXPECT_NE(out.find("%VAR%"), std::string::npos);
    EXPECT_EQ(out.find("(PowerShell)"), std::string::npos);
    // 同一行 set 的变量取不到、未定义变量原样留成文字(用户项目里出现 `%T%` 目录)。
    EXPECT_NE(out.find("directory literally named `%T%`"), std::string::npos);
}

// 场景:Windows 上的 Git Bash。
// 期望:出现 Git Bash 指引(路径形态提示 + $ACECODE_TMPDIR),没有 cmd / PowerShell 段。
TEST_F(SystemPromptTest, GitBashTerminalGetsPathNote) {
    acecode::SystemPromptEnvironment env;
    env.terminal_family = "bash";
    env.terminal_program = "C:\\Program Files\\Git\\bin\\bash.exe";
    std::string out = build_with_env(temp_home, env);
    EXPECT_NE(out.find("# Shell Command Guidance (Git Bash on Windows)"), std::string::npos);
    EXPECT_NE(out.find("/c/Users/"), std::string::npos);
    EXPECT_EQ(out.find("# Shell Command Guidance (Windows)"), std::string::npos);
    EXPECT_EQ(out.find("(PowerShell)"), std::string::npos);
}

// 场景:POSIX 家族。
// 期望:只有 Shell 行,没有任何 Shell Command Guidance 段。
TEST_F(SystemPromptTest, PosixTerminalHasNoGuidance) {
    acecode::SystemPromptEnvironment env;
    env.terminal_family = "posix";
    env.terminal_program = "/bin/zsh";
    std::string out = build_with_env(temp_home, env);
    EXPECT_NE(out.find("- Shell: posix (/bin/zsh)"), std::string::npos);
    EXPECT_EQ(out.find("# Shell Command Guidance"), std::string::npos);
}

// 场景:配置了两个工具链目录 / 一个都没配。
// 期望:有配置时 Environment 段出现单行 Toolchains 列出两者;没配置时整行不出现。
TEST_F(SystemPromptTest, ToolchainsLinePresentOnlyWhenConfigured) {
    acecode::SystemPromptEnvironment env;
    env.terminal_family = "posix";
    env.terminal_program = "/bin/sh";
    env.toolchains = {{"Python", "D:\\tools\\py"}, {"Node.js", "D:\\tools\\node"}};
    std::string with = build_with_env(temp_home, env);
    EXPECT_NE(with.find("- Toolchains: Python=D:\\tools\\py; Node.js=D:\\tools\\node\n"),
              std::string::npos);

    env.toolchains.clear();
    std::string without = build_with_env(temp_home, env);
    EXPECT_EQ(without.find("Toolchains:"), std::string::npos);
}

// 场景:同一环境连续构建两次。
// 期望:逐字节相同 —— Shell / Toolchains 行只随配置变化,不能打穿 prompt cache 前缀。
TEST_F(SystemPromptTest, EnvironmentLinesAreByteStable) {
    acecode::SystemPromptEnvironment env;
    env.terminal_family = "powershell";
    env.terminal_program = "pwsh";
    env.toolchains = {{"Node.js", "/opt/node/bin"}};
    EXPECT_EQ(build_with_env(temp_home, env), build_with_env(temp_home, env));
}

// 场景:environment 传 nullptr(旧调用方 / 未 bootstrap 的路径)。
// 期望:与改动前一致 —— Windows 标 cmd.exe 且带 cmd 指引;POSIX 标 $SHELL 或 /bin/sh
// 且无指引;两边都没有 Toolchains 行。
TEST_F(SystemPromptTest, NullEnvironmentKeepsLegacyOutput) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_EQ(out.find("Toolchains:"), std::string::npos);
#ifdef _WIN32
    EXPECT_NE(out.find("- Shell: cmd.exe\n"), std::string::npos);
    EXPECT_NE(out.find("# Shell Command Guidance (Windows)"), std::string::npos);
#else
    EXPECT_EQ(out.find("# Shell Command Guidance"), std::string::npos);
#endif
}

// 场景:spawn_subagent 子会话继承父会话的 worktree(inherited=true)。
// 期望:Environment 说明 worktree 归父会话所有、不得 Enter/ExitWorktree、写入
// 必须留在 worktree 内;不再给出 "return cwd" 与 "requires ExitWorktree" 两句
// (它们会引导子代理离开父会话正在用的目录)。
TEST_F(SystemPromptTest, InheritedWorktreeTellsSubagentToStayInside) {
    acecode::ToolExecutor tools;
    acecode::SystemPromptWorktreeState worktree;
    worktree.active = true;
    worktree.inherited = true;
    worktree.worktree_path = (temp_home / "wt").string();
    worktree.worktree_branch = "worktree-ses-parent";
    worktree.original_cwd = temp_home.string();

    std::string out = acecode::build_system_prompt(
        tools, worktree.worktree_path,
        /*skills=*/nullptr, /*memory=*/nullptr, /*memory_cfg=*/nullptr,
        /*project_instructions_cfg=*/nullptr, /*effective_tool_policy=*/nullptr,
        &worktree);

    EXPECT_NE(out.find("- Session worktree: active on branch worktree-ses-parent"),
              std::string::npos);
    EXPECT_NE(out.find("shared with the parent session"), std::string::npos);
    EXPECT_NE(out.find("Do not call `EnterWorktree` or `ExitWorktree`"), std::string::npos);
    EXPECT_EQ(out.find("- Session worktree return cwd:"), std::string::npos);
    EXPECT_EQ(out.find("requires `ExitWorktree`"), std::string::npos);
}

// ---------------------------------------------------------------------------
// GPT 系模型适配(openspec add-gpt-apply-patch-adaptation)。
// 背景:GPT-5 / gpt-5-codex 是用 Codex 的 apply_patch 补丁语言训练的,对
// file_edit 的 old_string 精确匹配不熟,反复失败后退化成 shell heredoc 写文件。
// 现在按模型族分支:GPT 系的工具指引整段换成 apply_patch,并追加模型族行为段;
// 其它模型输出必须逐字节不变。
// ---------------------------------------------------------------------------

namespace {

acecode::SystemPromptModelState gpt_model_state(const char* id = "gpt-5") {
    acecode::SystemPromptModelState state;
    state.model_id = id;
    state.family = acecode::detect_model_family(id);
    state.prefers_apply_patch = acecode::model_prefers_apply_patch(id);
    return state;
}

void register_probe_tools(acecode::ToolExecutor& tools,
                          std::initializer_list<const char*> names) {
    for (const char* name : names) {
        acecode::ToolImpl impl;
        impl.definition.name = name;
        impl.definition.description = "probe";
        impl.definition.parameters = nlohmann::json::object();
        impl.execute = [](const std::string&, const acecode::ToolContext&) {
            return acecode::ToolResult{"ok", true};
        };
        ASSERT_TRUE(tools.register_tool(impl));
    }
}

} // namespace

// 场景:工具表里注册了 memory_write / memory_read;另一份工具表没有。
// 期望:有 memory_write 时静态提示出现「# Memory」段,明确「让我记住」要用
// memory_write、不要另写 MEMORY.md / LESSONS_LEARNED.md / 子目录 CLAUDE.md;
// 没有 memory_write 时整段不出现。
// 回归背景(反馈 LINDANDAN069):用户要求「记住这个处理方式」,记忆目录为空、
// 提示里没有任何记忆指引,模型把经验写进了子目录的 CLAUDE.md 与项目里的
// .acecode/MEMORY.md,ACECode 从不自动加载它们,压缩之后经验就丢了。
TEST_F(SystemPromptTest, MemoryGuidanceRoutesRememberRequestsToMemoryWrite) {
    acecode::ScopedModelToolNameMappings none({});
    acecode::ToolExecutor tools;
    register_probe_tools(tools, {"memory_write", "memory_read"});
    const std::string with_memory =
        acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_NE(with_memory.find("# Memory\n"), std::string::npos);
    EXPECT_NE(with_memory.find("save it with `memory_write`"), std::string::npos);
    EXPECT_NE(with_memory.find("LESSONS_LEARNED.md"), std::string::npos);
    EXPECT_NE(with_memory.find("`memory_read`"), std::string::npos);

    acecode::ToolExecutor bare;
    const std::string without_memory =
        acecode::build_system_prompt(bare, temp_home.string());
    EXPECT_EQ(without_memory.find("# Memory\n"), std::string::npos);
}

// 场景:模型态是 gpt-5(偏好 apply_patch),工具表里 apply_patch 可用。
// 期望:出现 apply_patch 指引(相对路径 / 3 行上下文 / @@ 锚点、找不到时重读
// 而不是绕道 shell)与 "# Model-specific guidance" 段(自主推进、最小改动、脏
// 工作区、ASCII 默认);file_edit / file_write 的指引一句都不剩,连 cmd 指引里
// 「多行内容用 file_write」也改指 apply_patch。
TEST_F(SystemPromptTest, GptModelStateSwitchesGuidanceToApplyPatch) {
    acecode::ScopedModelToolNameMappings none({});
    acecode::ToolExecutor tools;
    register_probe_tools(tools, {"file_read", "file_edit", "file_write", "apply_patch", "bash"});
    const acecode::SystemPromptModelState gpt = gpt_model_state("gpt-5");
    acecode::SystemPromptEnvironment env;
    env.terminal_family = "cmd";
    env.terminal_program = "cmd.exe";

    const std::string out = acecode::build_system_prompt(
        tools, temp_home.string(), nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, true, &env, nullptr, &gpt);

    EXPECT_NE(out.find("Use `apply_patch` for every file creation, edit, deletion, or rename"),
              std::string::npos);
    EXPECT_NE(out.find("add an `@@` anchor"), std::string::npos);
    EXPECT_NE(out.find("# Model-specific guidance"), std::string::npos);
    EXPECT_NE(out.find("Always use `apply_patch` for manual code edits"), std::string::npos);
    EXPECT_NE(out.find("NEVER revert, undo, or modify changes you did not make"), std::string::npos);
    EXPECT_NE(out.find("prefer the `apply_patch` tool"), std::string::npos);
    EXPECT_EQ(out.find("file_edit"), std::string::npos);
    EXPECT_EQ(out.find("file_write"), std::string::npos);
}

// 场景:模型态是 claude-sonnet-4,与完全不传模型态各构建一次。
// 期望:两份输出逐字节相同 —— 非 GPT 模型的提示不受本 change 影响。
// 回归:任何把模型族段泄漏到默认分支的改动都会打破这条。
TEST_F(SystemPromptTest, NonGptModelStateIsByteIdenticalToLegacyPrompt) {
    acecode::ScopedModelToolNameMappings none({});
    acecode::ToolExecutor tools;
    register_probe_tools(tools, {"file_read", "file_edit", "file_write", "apply_patch", "bash"});
    const acecode::SystemPromptModelState claude = gpt_model_state("claude-sonnet-4");

    const std::string with_state = acecode::build_system_prompt(
        tools, temp_home.string(), nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, true, nullptr, nullptr, &claude);
    const std::string legacy = acecode::build_system_prompt(
        tools, temp_home.string(), nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, true, nullptr, nullptr, nullptr);

    EXPECT_EQ(with_state, legacy);
    EXPECT_EQ(with_state.find("apply_patch"), std::string::npos);
    EXPECT_NE(with_state.find("`file_edit` will error"), std::string::npos);
}

// 场景:同一 GPT 模型态重复构建。
// 期望:逐字节相同 —— 模型族段只随模型切换变化,留在静态前缀里不打穿 prompt cache
// (与 StaticSystemPromptIsByteStableAcrossCalls 守同一条不变量)。
TEST_F(SystemPromptTest, GptPromptIsByteStableAcrossCalls) {
    acecode::ScopedModelToolNameMappings none({});
    acecode::ToolExecutor tools;
    register_probe_tools(tools, {"file_read", "file_edit", "file_write", "apply_patch", "bash"});
    const acecode::SystemPromptModelState gpt = gpt_model_state("gpt-5-codex");
    const auto build = [&] {
        return acecode::build_system_prompt(
            tools, temp_home.string(), nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, true, nullptr, nullptr, &gpt);
    };
    const std::string first = build();
    EXPECT_EQ(first, build());
    EXPECT_NE(first.find("# Model-specific guidance"), std::string::npos);
}

// 场景:GPT 模型,但 expert 能力策略把 apply_patch 滤掉了(只允许 file_read /
// file_edit / file_write)。
// 期望:回退到 file_edit / file_write 指引 —— 与 AgentLoop 的裁表回退同口径,否则
// 提示说用 apply_patch 而工具表里没有它。模型族行为段仍然给(它不依赖工具)。
TEST_F(SystemPromptTest, GptStateWithoutApplyPatchToolKeepsFileEditGuidance) {
    acecode::ScopedModelToolNameMappings none({});
    acecode::ToolExecutor tools;
    register_probe_tools(tools, {"file_read", "file_edit", "file_write", "apply_patch", "bash"});
    const acecode::SystemPromptModelState gpt = gpt_model_state("gpt-5");
    acecode::ToolCapabilityPolicy policy;
    policy.builtin_tools = std::unordered_set<std::string>{"file_read", "file_edit", "file_write"};
    policy.mcp_servers = std::unordered_set<std::string>{};

    const std::string out = acecode::build_system_prompt(
        tools, temp_home.string(), nullptr, nullptr, nullptr, nullptr, &policy,
        nullptr, true, nullptr, nullptr, &gpt);

    EXPECT_EQ(out.find("Use `apply_patch` for every file creation"), std::string::npos);
    EXPECT_EQ(out.find("Always use `apply_patch`"), std::string::npos);
    EXPECT_NE(out.find("`file_edit` will error"), std::string::npos);
    EXPECT_NE(out.find("# Model-specific guidance"), std::string::npos);
}
