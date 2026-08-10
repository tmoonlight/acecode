// 覆盖 src/prompt/system_prompt.{hpp,cpp} 的 skill 索引注入逻辑
// (openspec/changes/adopt-codex-skill-catalog):
// - 索引格式化:Codex 单行来源定位 / 单条 1024 字符 UTF-8 安全截断
// - Codex 预算:已知窗口 2% token / 未知窗口 8000 字符
// - 公平退化:先保留所有名称+路径,再 round-robin 分配描述,极端时才省略条目
// - PromptContextBlock 装配:空 registry 不发块,cache_key 跟随 skill 集变化
// - session context 隔离:skills 块不再进入 user-role system-reminder
// - 静态 system prompt 措辞:指向索引而非 skills_list 枚举
//
// 背景 bug:此前 skill 清单从不进入模型上下文,模型对已安装 skill 零可见,
// 永远不会主动调用 skills_list / skill_view —— skill 只能靠用户显式 /<name>
// 触发。修复后索引每请求注入 session-context system-reminder。

#include <gtest/gtest.h>

#include "prompt/system_prompt.hpp"
#include "skills/skill_metadata.hpp"
#include "skills/skill_registry.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

acecode::SkillMetadata make_skill(const std::string& name,
                                  const std::string& description,
                                  const std::string& category = "",
                                  const std::string& when_to_use = "") {
    acecode::SkillMetadata meta;
    meta.name = name;
    meta.command_key = name;
    meta.description = description;
    meta.category = category;
    meta.when_to_use = when_to_use;
    meta.skill_md_path = fs::path("C:/skills") /
                         acecode::path_from_utf8(name) / "SKILL.md";
    return meta;
}

acecode::SkillMetadataBudget char_budget(std::size_t limit) {
    return {acecode::SkillMetadataBudgetUnit::Characters, limit};
}

std::size_t utf8_chars(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) ++count;
    }
    return count;
}

void write_skill_md(const fs::path& root,
                    const std::string& category,
                    const std::string& name,
                    const std::string& description,
                    const std::string& when_to_use = "") {
    fs::path dir = root;
    if (!category.empty()) dir /= acecode::path_from_utf8(category);
    dir /= acecode::path_from_utf8(name);
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n";
    if (!when_to_use.empty()) ofs << "whenToUse: " << when_to_use << "\n";
    ofs << "---\n\n# " << name << "\n";
}

class SkillsIndexRegistryTest : public ::testing::Test {
protected:
    fs::path temp_root;

    void SetUp() override {
        temp_root = fs::temp_directory_path() /
                    fs::path("acecode-skills-index-test-" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(temp_root, ec);
        fs::create_directories(temp_root);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }
};

// ---------------------------------------------------------------------------
// skills_index_budget
// ---------------------------------------------------------------------------

// 触发场景:已知 context window(128k tokens)。
// 期望:预算 = 2% tokens = 2560 tokens。
TEST(SkillsIndexBudgetTest, KnownWindowGivesTwoPercentInTokens) {
    const auto budget128k = acecode::skills_index_budget(128000);
    EXPECT_EQ(budget128k.unit, acecode::SkillMetadataBudgetUnit::Tokens);
    EXPECT_EQ(budget128k.limit, 2560u);

    const auto budget200k = acecode::skills_index_budget(200000);
    EXPECT_EQ(budget200k.unit, acecode::SkillMetadataBudgetUnit::Tokens);
    EXPECT_EQ(budget200k.limit, 4000u);
}

// 触发场景:context window 未知(0 或负数,如 provider 探测失败)。
// 期望:退回 Codex 的 8000 Unicode 字符兜底。
TEST(SkillsIndexBudgetTest, UnknownWindowFallsBackTo8000Characters) {
    for (int window : {0, -1}) {
        const auto budget = acecode::skills_index_budget(window);
        EXPECT_EQ(budget.unit, acecode::SkillMetadataBudgetUnit::Characters);
        EXPECT_EQ(budget.limit, 8000u);
    }
}

// ---------------------------------------------------------------------------
// format_skills_index_within_budget
// ---------------------------------------------------------------------------

// 触发场景:无 skill。期望:空串(调用方据此跳过整个块)。
TEST(SkillsIndexFormatTest, EmptyListRendersEmpty) {
    const auto result =
        acecode::format_skills_index_within_budget({}, char_budget(8000));
    EXPECT_TRUE(result.content.empty());
    EXPECT_EQ(result.report.total_count, 0u);
}

// 触发场景:带/不带旧 category 元数据的 Skill 混合。
// 期望:Codex 目录保持 registry 输入顺序,每条扁平显示绝对来源定位。
TEST(SkillsIndexFormatTest, UsesFlatCodexEntriesWithSourceLocators) {
    std::vector<acecode::SkillMetadata> skills = {
        make_skill("review-pr", "Review pull requests", "review"),
        make_skill("standalone", "A flat skill"),
    };
    auto rendered = acecode::format_skills_index_within_budget(
        skills, char_budget(8000));
    EXPECT_EQ(rendered.content,
              "- review-pr: Review pull requests (file: C:/skills/review-pr/SKILL.md)\n"
              "- standalone: A flat skill (file: C:/skills/standalone/SKILL.md)");
    EXPECT_EQ(rendered.report.included_count, 2u);
    EXPECT_EQ(rendered.report.omitted_count, 0u);
}

// 触发场景:兼容 loader 仍保留旧 whenToUse 元数据。
// 期望:Codex 生产目录只使用标准 description,不拼接私有字段。
TEST(SkillsIndexFormatTest, UsesStandardDescriptionWithoutWhenToUseSuffix) {
    std::vector<acecode::SkillMetadata> skills = {
        make_skill("commit", "Create a git commit", "", "Use when the user asks to commit changes"),
    };
    auto rendered = acecode::format_skills_index_within_budget(
        skills, char_budget(8000));
    EXPECT_EQ(rendered.content,
              "- commit: Create a git commit (file: C:/skills/commit/SKILL.md)");
}

// 触发场景:描述(含 whenToUse)超过 Codex 的 1024 Unicode 字符上限。
// 期望:UTF-8 边界安全截断 + "..." 收尾,描述部分恰好不超过 1024 字符。
TEST(SkillsIndexFormatTest, TruncatesLongDescriptionOnUtf8Boundary) {
    // 用 3 字节中文字符验证上限按字符而不是 UTF-8 byte 计算。
    std::string long_desc;
    for (int i = 0; i < 1100; ++i) long_desc += "\xE4\xB8\xAD"; // "中" × 1100
    std::vector<acecode::SkillMetadata> skills = {make_skill("s", long_desc)};
    auto rendered = acecode::format_skills_index_within_budget(
        skills, char_budget(2000));
    const std::string& out = rendered.content;

    ASSERT_TRUE(out.rfind("- s: ", 0) == 0);
    const std::string locator = " (file: C:/skills/s/SKILL.md)";
    ASSERT_GT(out.size(), locator.size() + 5);
    ASSERT_EQ(out.substr(out.size() - locator.size()), locator);
    std::string desc_part = out.substr(5, out.size() - 5 - locator.size());
    EXPECT_EQ(utf8_chars(desc_part), 1024u);
    EXPECT_EQ(desc_part.substr(desc_part.size() - 3), "...");
    // 截断后仍是合法 UTF-8:省略号前每个 "中" 完整保留。
    std::string body = desc_part.substr(0, desc_part.size() - 3);
    EXPECT_EQ(body.size() % 3, 0u);
}

// 触发场景:全量描述超预算,但三个最小名称条目都放得下。
// 期望:剩余预算 round-robin 分给全部 Skill,而不是第一个条目吃完。
TEST(SkillsIndexFormatTest, OverBudgetDistributesDescriptionsRoundRobin) {
    std::vector<acecode::SkillMetadata> skills;
    for (int i = 0; i < 3; ++i) {
        skills.push_back(make_skill("skill-" + std::to_string(i),
                                    std::string(100, 'd')));
    }
    // 来源定位提高了最小成本;200 字符仍低于完整目录,但足以让三条
    // 都得到 description 字符。
    auto rendered = acecode::format_skills_index_within_budget(
        skills, char_budget(200));
    EXPECT_NE(rendered.content.find("- skill-0: d"), std::string::npos);
    EXPECT_NE(rendered.content.find("- skill-1: d"), std::string::npos);
    EXPECT_NE(rendered.content.find("- skill-2: d"), std::string::npos);
    EXPECT_EQ(rendered.report.included_count, 3u);
    EXPECT_EQ(rendered.report.omitted_count, 0u);
    EXPECT_EQ(rendered.report.truncated_description_count, 3u);
}

// 触发场景:token 预算下只剩一个近似 token 可分配给描述。
// 期望:成本按 ceil(UTF-8 bytes / 4)计算,零成本的同 token 字符仍可保留。
TEST(SkillsIndexFormatTest, TokenBudgetUsesApproximateUtf8ByteCost) {
    auto skill = make_skill("alpha", std::string(20, 'd'));
    skill.skill_md_path = fs::path("x");
    std::vector<acecode::SkillMetadata> skills = {skill};
    auto rendered = acecode::format_skills_index_within_budget(
        skills, {acecode::SkillMetadataBudgetUnit::Tokens, 6});

    EXPECT_EQ(rendered.content, "- alpha: dddd (file: x)");
    EXPECT_EQ(rendered.report.included_count, 1u);
    EXPECT_EQ(rendered.report.omitted_count, 0u);
}

// 触发场景:全部名称可容纳,但每个描述平均要截掉超过 100 字符。
// 期望:不遗漏任何 Skill,并生成 Codex 风格的描述缩短警告。
TEST(SkillsIndexFormatTest, MaterialDescriptionShorteningProducesWarning) {
    std::vector<acecode::SkillMetadata> skills = {
        make_skill("a", std::string(300, 'a')),
        make_skill("b", std::string(300, 'b')),
    };
    auto rendered = acecode::format_skills_index_within_budget(
        skills, char_budget(120));

    EXPECT_EQ(rendered.report.included_count, 2u);
    EXPECT_EQ(rendered.report.omitted_count, 0u);
    EXPECT_GT(rendered.report.truncated_description_chars / 2, 100u);
    EXPECT_NE(rendered.report.warning_message().find("descriptions were shortened"),
              std::string::npos);
}

// 触发场景:最小名称条目仍超预算(极端:海量 skill + 极小窗口)。
// 期望:按 Codex host 目录语义只保留放得下的最小条目,诊断记录省略数,
// 模型可见目录不伪造 omission marker。
TEST(SkillsIndexFormatTest, ExtremeOverflowOmitsWithoutModelVisibleMarker) {
    std::vector<acecode::SkillMetadata> skills;
    for (int i = 0; i < 50; ++i) {
        skills.push_back(make_skill("very-long-skill-name-" + std::to_string(i),
                                    "desc"));
    }
    auto rendered = acecode::format_skills_index_within_budget(
        skills, char_budget(150));
    EXPECT_LE(utf8_chars(rendered.content), 150u);
    EXPECT_GT(rendered.report.omitted_count, 0u);
    EXPECT_EQ(rendered.content.find("additional skills omitted"), std::string::npos);
    EXPECT_EQ(rendered.content.find("skills_list"), std::string::npos);
    EXPECT_FALSE(rendered.report.warning_message().empty());
}

// 触发场景:前一条最小记录过长放不下,后一条较短记录仍可容纳。
// 期望:Codex 的逐条 greedy 策略不会因为前一条被省略就截断整个尾部。
TEST(SkillsIndexFormatTest, ExtremeOverflowStillKeepsLaterShortEntry) {
    auto long_skill = make_skill(std::string(100, 'l'), "long");
    long_skill.skill_md_path = fs::path(std::string(100, 'p'));
    auto short_skill = make_skill("s", "short");
    short_skill.skill_md_path = fs::path("x");

    auto rendered = acecode::format_skills_index_within_budget(
        {long_skill, short_skill}, char_budget(20));

    EXPECT_EQ(rendered.report.included_count, 1u);
    EXPECT_EQ(rendered.report.omitted_count, 1u);
    EXPECT_NE(rendered.content.find("- s: (file: x)"), std::string::npos);
}

// 触发场景:多个 Skill 共享很长的扫描根,绝对路径目录在预算内会省略。
// 期望:root alias 表成本计入预算后仍能改善结果,因此选择 r0 短路径。
TEST(SkillsIndexFormatTest, UsesRootAliasesWhenTheyImproveBoundedCatalog) {
    const fs::path root = fs::path("C:/") / std::string(120, 'r');
    std::vector<acecode::SkillMetadata> skills;
    for (const std::string& name : {"alpha", "beta", "gamma"}) {
        auto skill = make_skill(name, "A useful skill description");
        skill.scan_root = root;
        skill.skill_md_path = root / name / "SKILL.md";
        skills.push_back(std::move(skill));
    }

    auto rendered = acecode::format_skills_index_within_budget(
        skills, char_budget(300));

    ASSERT_FALSE(rendered.skill_root_lines.empty());
    EXPECT_EQ(rendered.report.included_count, 3u);
    EXPECT_NE(rendered.skill_root_lines.front().find("`r0`"), std::string::npos);
    EXPECT_NE(rendered.content.find("(file: r0/alpha/SKILL.md)"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// build_skills_index_context_prompt
// ---------------------------------------------------------------------------

// 触发场景:registry 为 nullptr(skill 功能未初始化)。
// 期望:空块 —— content/cache_key 都为空,session context 不包含 skill 段。
TEST(SkillsIndexBlockTest, NullRegistryYieldsEmptyBlock) {
    auto block = acecode::build_skills_index_context_prompt(nullptr, 128000);
    EXPECT_TRUE(block.content.empty());
    EXPECT_TRUE(block.cache_key.empty());
}

// 触发场景:registry 扫描了一个空目录(用户没装任何 skill)。
// 期望:空块,不发 "### Available skills" 标题。
TEST_F(SkillsIndexRegistryTest, EmptyRegistryYieldsEmptyBlock) {
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();
    auto block = acecode::build_skills_index_context_prompt(&registry, 128000);
    EXPECT_TRUE(block.content.empty());
}

// 触发场景:registry 有 skill(含 whenToUse)。
// 期望:块含 "### Available skills" 标题、skill_view 指引、来源定位与标准描述;
// cache_key 非空且带 "skills:" 前缀(进 session context 复合 key)。
TEST_F(SkillsIndexRegistryTest, PopulatedRegistryRendersBlock) {
    write_skill_md(temp_root, "review", "review-pr", "Review pull requests",
                   "Use when the user mentions a PR");
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();

    auto block = acecode::build_skills_index_context_prompt(&registry, 128000);
    EXPECT_EQ(block.content.rfind("<skills_instructions>", 0), 0u);
    EXPECT_NE(block.content.find("### Available skills"), std::string::npos);
    EXPECT_NE(block.content.find("skill_view"), std::string::npos);
    EXPECT_NE(block.content.find("task clearly matches"), std::string::npos);
    EXPECT_NE(block.content.find("Multiple mentions mean use them all"),
              std::string::npos);
    EXPECT_NE(block.content.find("review-pr: Review pull requests"), std::string::npos);
    EXPECT_NE(block.content.find("(file: "), std::string::npos);
    EXPECT_EQ(block.content.find("Use when the user mentions a PR"), std::string::npos);
    EXPECT_EQ(block.cache_key.rfind("skills:", 0), 0u);
}

// Codex 的目录发现不依赖专用 skill_view 工具；有 file locator 时即使该
// 工具被策略隐藏,目录仍应进入模型上下文并指导使用普通文件读取能力。
TEST_F(SkillsIndexRegistryTest, CatalogRemainsVisibleWithoutSkillViewTool) {
    write_skill_md(temp_root, "", "alpha", "First description");
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();

    auto block = acecode::build_skills_index_context_prompt(
        &registry, 128000, /*skill_view_available=*/false,
        /*skills_list_available=*/false);
    EXPECT_NE(block.content.find("alpha: First description"), std::string::npos);
    EXPECT_NE(block.content.find("open the listed `SKILL.md` path"),
              std::string::npos);
}

// 触发场景:同一 skill 集合构建两次 / 改动描述后再构建。
// 期望:集合不变 → cache_key 逐字节一致(cached_context_for_api 直接复用);
// 描述变化 → cache_key 变化(缓存失效,新索引上行)。
TEST_F(SkillsIndexRegistryTest, CacheKeyTracksSkillSetChanges) {
    write_skill_md(temp_root, "", "alpha", "First description");
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();

    auto block1 = acecode::build_skills_index_context_prompt(&registry, 128000);
    auto block2 = acecode::build_skills_index_context_prompt(&registry, 128000);
    EXPECT_EQ(block1.cache_key, block2.cache_key);

    write_skill_md(temp_root, "", "alpha", "Changed description");
    registry.reload();
    auto block3 = acecode::build_skills_index_context_prompt(&registry, 128000);
    EXPECT_NE(block1.cache_key, block3.cache_key);
}

// ---------------------------------------------------------------------------
// build_session_context_prompt 隔离
// ---------------------------------------------------------------------------

// 触发场景:带 skill registry 构建通用 session context(memory/project 均关闭)。
// 期望:默认不再把 Skill 目录塞进 user-role <system-reminder>。
TEST_F(SkillsIndexRegistryTest, SessionContextExcludesSkillsBlockByDefault) {
    write_skill_md(temp_root, "", "alpha", "First skill");
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();

    auto block = acecode::build_session_context_prompt(
        temp_root.string(), nullptr, nullptr, nullptr, &registry, 128000);
    EXPECT_TRUE(block.content.empty());
}

// 触发场景:不传 registry(旧调用方 / skill 功能关闭)。
// 期望:行为与改动前完全一致 —— 无其它 context 时返回空块。向后兼容回归。
TEST_F(SkillsIndexRegistryTest, SessionContextWithoutRegistryUnchanged) {
    auto block = acecode::build_session_context_prompt(
        temp_root.string(), nullptr, nullptr, nullptr);
    EXPECT_TRUE(block.content.empty());
}

// ---------------------------------------------------------------------------
// 静态 system prompt 措辞
// ---------------------------------------------------------------------------

// 触发场景:构建静态 system prompt。
// 期望:# Skills 段指向独立高优先级 <skills_instructions> system 消息,
// 含 "err on the side of loading" 强化措辞;不再把 skills_list 当作主发现
// 路径("Call `skills_list` to enumerate" 旧文案应删除)。
TEST(SkillsIndexSystemPromptTest, SkillsSectionPointsAtInContextIndex) {
    acecode::ToolExecutor tools;
    std::string prompt = acecode::build_system_prompt(tools, ".");

    EXPECT_NE(prompt.find("<skills_instructions>"), std::string::npos);
    EXPECT_NE(prompt.find("err on the side of loading"), std::string::npos);
    EXPECT_NE(prompt.find("BLOCKING REQUIREMENT"), std::string::npos);
    EXPECT_EQ(prompt.find("Call `skills_list` to enumerate"), std::string::npos);
}

// ---------------------------------------------------------------------------
// skill_loader: whenToUse frontmatter
// ---------------------------------------------------------------------------

// 触发场景:SKILL.md frontmatter 带 whenToUse(camelCase,claude-code 约定)。
// 期望:SkillMetadata.when_to_use 持有该值。
TEST_F(SkillsIndexRegistryTest, LoaderParsesWhenToUseCamelCase) {
    write_skill_md(temp_root, "", "alpha", "Desc", "Use for X");
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();
    auto meta = registry.find("alpha");
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->when_to_use, "Use for X");
}

// 触发场景:frontmatter 用 snake_case 别名 when_to_use。
// 期望:同样被解析(两种写法等价,camelCase 优先)。
TEST_F(SkillsIndexRegistryTest, LoaderParsesWhenToUseSnakeCaseAlias) {
    fs::path dir = temp_root / "beta";
    fs::create_directories(dir);
    {
        std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
        ofs << "---\n"
            << "name: beta\n"
            << "description: Desc\n"
            << "when_to_use: Use for Y\n"
            << "---\n\n# beta\n";
    }
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();
    auto meta = registry.find("beta");
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->when_to_use, "Use for Y");
}

} // namespace
