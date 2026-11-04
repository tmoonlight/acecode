// 覆盖 openspec add-inline-visuals 的请求组装(ApiRequestBuilder / RequestContextFactory):
//   1. 画图说明追加在静态系统提示末尾:网页会话开关打开含 mermaid + visual 两部分,
//      关闭只含 mermaid,终端只含 mermaid,None 不含;同一组输入逐字节稳定
//   2. visual 部分不生效时,技能索引隐藏 inline-visuals,目录身份随之变化
//   3. 「工具重写」把 skill_view 改名后,说明里引用改名后的名字
//   4. 子会话(带父会话 id)与群聊代答会话降为不加说明,普通会话照常
// 画图说明正文本身的取舍规则在 tests/prompt/diagram_guidance_test.cpp。

#include <gtest/gtest.h>

#include "test_support/agent_loop/characterization_fixture.hpp"
#include "agent/request/api_request_builder.hpp"
#include "agent/request/prompt_context_cache.hpp"
#include "llm/tool_protocol_names.hpp"
#include "session/channel_delegate.hpp"
#include "skills/skill_registry.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {
namespace fs = std::filesystem;

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// 技能根里放 inline-visuals 与另一个普通技能,用来观察索引里谁被隐藏。
std::shared_ptr<acecode::SkillRegistry> visual_skills(const std::string& leaf) {
    const fs::path root = fs::path(testing::TempDir()) / leaf;
    fs::remove_all(root);
    for (const char* name : {"inline-visuals", "other-skill"}) {
        fs::create_directories(root / name);
        std::ofstream(root / name / "SKILL.md", std::ios::binary)
            << "---\nname: " << name << "\ndescription: " << name
            << " rules\n---\n\n# body\n";
    }
    auto registry = std::make_shared<acecode::SkillRegistry>();
    registry->set_scan_roots({root});
    registry->scan();
    return registry;
}

acecode::agent::RequestContextOptions base_options(
    std::shared_ptr<acecode::SkillRegistry> skills, acecode::DiagramSurface surface,
    bool enabled) {
    acecode::agent::RequestContextOptions options;
    options.cwd = testing::TempDir();
    options.git_config.emplace();
    options.git_config->enabled = false;
    options.project_config.emplace();
    options.project_config->enabled = false;
    options.memory_config.emplace();
    options.memory_config->enabled = false;
    options.skills = std::move(skills);
    options.diagram_surface = surface;
    options.inline_visuals_enabled = enabled;
    return options;
}

void register_skill_view(acecode::ToolExecutor& tools) {
    acecode::ToolImpl skill_view;
    skill_view.definition.name = "skill_view";
    ASSERT_TRUE(tools.register_tool(skill_view));
}

std::string system_prompt_of(const std::vector<acecode::ChatMessage>& messages) {
    const auto found = std::find_if(messages.begin(), messages.end(),
        [](const acecode::ChatMessage& message) { return message.role == "system"; });
    return found == messages.end() ? std::string{} : found->content;
}

} // namespace

// 场景:Web 会话,「可视化图表」为默认开启,技能快照里有 inline-visuals,skill_view 可用。
// 期望:静态系统提示含 mermaid 与 visual 两部分并点名加载技能;正常请求、压缩用的初始
// 上下文与再次组装得到逐字节相同的系统提示(同一回合多次请求共享缓存前缀)。
TEST(DiagramGuidanceRequest, WebSessionCarriesBothPartsAndStaysByteStable) {
    acecode::ToolExecutor tools;
    register_skill_view(tools);
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    const auto options = base_options(visual_skills("acecode-diagram-web"),
                                      acecode::DiagramSurface::Web, true);

    const auto prompt = builder.static_system_prompt(options);
    EXPECT_TRUE(has(prompt, "\n\n# Diagrams\n\n"));
    EXPECT_TRUE(has(prompt, "## Inline visuals"));
    EXPECT_TRUE(has(prompt, "load the `inline-visuals` skill with the `skill_view` tool"));
    EXPECT_EQ(prompt, builder.static_system_prompt(options));
    EXPECT_EQ(builder.capture(options, {}, false).system_prompt, prompt);
    EXPECT_EQ(system_prompt_of(builder.initial_context(options)), prompt);
}

// 场景:用户关闭开关,随后又打开。期望:关闭期间只剩 mermaid 部分;打开后与最初的
// 系统提示逐字节相同 —— 只有切换后的那次请求变化,之后再次稳定。
TEST(DiagramGuidanceRequest, SwitchOffKeepsMermaidAndSwitchBackRestoresBytes) {
    acecode::ToolExecutor tools;
    register_skill_view(tools);
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    auto options = base_options(visual_skills("acecode-diagram-switch"),
                                acecode::DiagramSurface::Web, true);
    const auto on = builder.static_system_prompt(options);

    options.inline_visuals_enabled = false;
    const auto off = builder.static_system_prompt(options);
    EXPECT_TRUE(has(off, "# Diagrams"));
    EXPECT_FALSE(has(off, "Inline visuals"));
    EXPECT_FALSE(has(off, "```visual"));
    EXPECT_EQ(off, builder.static_system_prompt(options));

    options.inline_visuals_enabled = true;
    EXPECT_EQ(builder.static_system_prompt(options), on);
}

// 场景:TUI 会话(Terminal)与 headless(None)。期望:TUI 只有 mermaid 说明、没有 visual;
// headless 不含任何画图说明(-p 的输出是纯文本,画图只会浪费 token)。
TEST(DiagramGuidanceRequest, TerminalHasMermaidOnlyAndNoneHasNothing) {
    acecode::ToolExecutor tools;
    register_skill_view(tools);
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    auto options = base_options(visual_skills("acecode-diagram-surfaces"),
                                acecode::DiagramSurface::Terminal, true);
    const auto terminal = builder.static_system_prompt(options);
    EXPECT_TRUE(has(terminal, "text-art diagrams"));
    EXPECT_FALSE(has(terminal, "```visual"));
    EXPECT_FALSE(has(terminal, "inline-visuals"));

    options.diagram_surface = acecode::DiagramSurface::None;
    EXPECT_FALSE(has(builder.static_system_prompt(options), "# Diagrams"));
}

// 场景:TUI 会话的技能索引里如果列出 inline-visuals,模型会照着去画 visual,用户只看到
// 占位提示。期望:visual 部分不生效(终端 / 开关关闭)时索引不列它但其它技能照常;
// 生效时列出;目录身份(skills_catalog_key)随规则变化,不会复用旧索引。
TEST(DiagramGuidanceRequest, SkillIndexHidesInlineVisualsWhenVisualPartInactive) {
    acecode::ToolExecutor tools;
    register_skill_view(tools);
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    auto options = base_options(visual_skills("acecode-diagram-index"),
                                acecode::DiagramSurface::Web, true);

    const auto active = builder.capture(options, {}, false);
    EXPECT_TRUE(has(active.skills.content, "inline-visuals"));
    EXPECT_TRUE(has(active.skills.content, "other-skill"));

    options.diagram_surface = acecode::DiagramSurface::Terminal;
    const auto terminal = builder.capture(options, {}, false);
    EXPECT_FALSE(has(terminal.skills.content, "inline-visuals"));
    EXPECT_TRUE(has(terminal.skills.content, "other-skill"));
    EXPECT_NE(terminal.skills_catalog_key, active.skills_catalog_key);
    EXPECT_FALSE(has(system_prompt_of(builder.initial_context(options)), "inline-visuals"));

    options.diagram_surface = acecode::DiagramSurface::Web;
    options.inline_visuals_enabled = false;
    EXPECT_FALSE(has(builder.capture(options, {}, false).skills.content, "inline-visuals"));
}

// 场景:「工具重写」把 skill_view 发布成 load_skill;另一个场景是技能被停用(快照里没有)。
// 期望:说明里引用 load_skill;技能不在快照里时 visual 部分仍在但不再要求加载它。
TEST(DiagramGuidanceRequest, UsesMappedToolNameAndDropsLoadWhenSkillMissing) {
    acecode::ToolExecutor tools;
    register_skill_view(tools);
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    auto options = base_options(visual_skills("acecode-diagram-mapped"),
                                acecode::DiagramSurface::Web, true);
    {
        acecode::ScopedModelToolNameMappings mappings{{"skill_view", "load_skill"}};
        const auto mapped = builder.static_system_prompt(options);
        EXPECT_TRUE(has(mapped, "load the `inline-visuals` skill with the `load_skill` tool"));
    }

    options.skills = std::make_shared<acecode::SkillRegistry>();
    const auto missing = builder.static_system_prompt(options);
    EXPECT_TRUE(has(missing, "## Inline visuals"));
    EXPECT_FALSE(has(missing, "inline-visuals"));
}

namespace {
using namespace acecode_test::characterization;

acecode::PromptConfigProvider web_prompt_config() {
    return [] {
        acecode::SessionPromptConfig config;
        config.diagram_surface = acecode::DiagramSurface::Web;
        config.inline_visuals.emplace();
        return config;
    };
}

std::string primed_system_prompt(Harness& harness) {
    harness.loop->prime_side_question_context();
    return system_prompt_of(harness.loop->side_question_context_snapshot());
}
} // namespace

// 场景:daemon 普通会话、spawn_subagent / 网状子 agent(会话带父会话 id)、群聊代答会话,
// 宿主都声明显示面为 Web。期望:普通会话有画图说明;子会话的回复由父会话转述,群聊代答
// 的回复发往 IM 群,两者都不带任何画图说明。
TEST(DiagramGuidanceRequest, ChildAndGroupDelegateSessionsDropTheGuidance) {
    Isolation isolation;
    {
        Harness root(isolation, "diagram-root", {}, true, web_prompt_config());
        EXPECT_TRUE(has(primed_system_prompt(root), "# Diagrams"));
    }
    {
        Harness child(isolation, "diagram-child", {}, true, web_prompt_config());
        child.session->set_parent_session_id("parent-session");
        EXPECT_FALSE(has(primed_system_prompt(child), "# Diagrams"));
    }
    {
        Harness delegate(isolation, "diagram-delegate", {}, true, web_prompt_config());
        acecode::ChannelDelegate group;
        group.platform = "telegram";
        group.chat = "group-1";
        delegate.session->set_channel_delegate(group);
        EXPECT_FALSE(has(primed_system_prompt(delegate), "# Diagrams"));
    }
}
