// 覆盖「一个坏 skill 不能拖垮整个 skills 面板」这条不变量。
//
// 背景 bug:设置页的技能 tab 打开就是 500。GET /api/skills 会对扫描根做一次
// 全量扫描,再把结果 dump() 成 JSON —— 这条链路上任何一处抛异常(nlohmann
// 对非法 UTF-8 抛 type_error.316、文件系统竞态抛 filesystem_error)都会被
// Crow 变成整页 500,而用户拿到的信息只有一个数字,连是哪个 skill 出问题都
// 看不出来。更早的形态更隐蔽:解析不出元数据的 skill 会被 load_skill_from_dir
// 静默丢弃,只在日志里留一行,面板上表现为"技能没装上"。
//
// 现在的约定:
//   - inspect_skill_dir() 永不抛,坏 skill 降级成 SkillLoadIssue;
//   - 致命 issue 不进 SkillRegistry::list()(不污染模型侧的 skill 索引 /
//     斜杠命令),但会出现在 /api/skills 响应里,带 status=error + 原因;
//   - 非致命 issue(frontmatter 缺失 / 未闭合 / 缺 description)挂在正常
//     skill 行上,status=warning。

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "skills/skill_loader.hpp"
#include "skills/skill_registry.hpp"
#include "utils/encoding.hpp"
#include "web/handlers/skills_handler.hpp"
#include "web/json_dump.hpp"

namespace fs = std::filesystem;

namespace {

using acecode::SkillLoadFailure;
using acecode::SkillRegistry;
using acecode::inspect_skill_dir;
using acecode::is_valid_utf8;
using acecode::skill_load_failure_code;

class SkillLoadResilience : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("acecode_skill_resilience_" +
                 std::to_string(::testing::UnitTest::GetInstance()
                                    ->current_test_info()
                                    ->line()));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path write_skill(const std::string& dir, const std::string& content) {
        const fs::path skill_dir = root_ / dir;
        fs::create_directories(skill_dir);
        std::ofstream ofs(skill_dir / "SKILL.md", std::ios::binary);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        return skill_dir;
    }

    SkillRegistry::Snapshot scan() const {
        SkillRegistry registry;
        registry.set_scan_roots({root_});
        registry.set_disabled({});
        return registry.snapshot();
    }

    nlohmann::json payload() const {
        return acecode::web::build_skills_payload_with_roots(
            /*project_roots=*/{}, /*global_roots=*/{root_},
            /*disabled=*/{});
    }

    static const nlohmann::json* find_entry(const nlohmann::json& arr,
                                            const std::string& name) {
        for (const auto& item : arr) {
            if (item.value("name", std::string{}) == name) return &item;
        }
        return nullptr;
    }

    fs::path root_;
};

// 用户报的原始形态:description 是个裸的 `>` 折叠块标量。它必须解析成折叠后的
// 文本,而不是把指示符本身当描述,更不能让整页 500。
TEST_F(SkillLoadResilience, FoldedDescriptionParsesInsteadOfLeakingIndicator) {
    write_skill("folded",
                "---\n"
                "name: folded\n"
                "description: >\n"
                "  Folded description\n"
                "  across two lines.\n"
                "---\n"
                "# body\n");

    const auto snap = scan();
    ASSERT_EQ(snap.skills.size(), 1u);
    EXPECT_EQ(snap.skills[0].description,
              "Folded description across two lines.");
    EXPECT_TRUE(snap.issues.empty());
}

// 整块 frontmatter 被统一缩进(手写 / tab 缩进常见)时,结构化解析器不能整块
// 放弃并降级到只认 `key: value` 的兜底解析器 —— 那条路径会把 `>` 原样存下来,
// 面板上的描述就变成一个孤零零的 ">"。
TEST_F(SkillLoadResilience, UniformlyIndentedFrontmatterStillParses) {
    write_skill("tabbed",
                "---\n"
                "\tname: tabbed\n"
                "\tdescription: >\n"
                "\t  Indented frontmatter description.\n"
                "---\n"
                "body\n");

    const auto snap = scan();
    ASSERT_EQ(snap.skills.size(), 1u);
    EXPECT_EQ(snap.skills[0].name, "tabbed");
    EXPECT_EQ(snap.skills[0].description, "Indented frontmatter description.");
}

// 空 SKILL.md:以前静默消失,现在是一条致命 issue。
TEST_F(SkillLoadResilience, EmptySkillFileReportsFatalIssue) {
    write_skill("blank", "");

    const auto snap = scan();
    EXPECT_TRUE(snap.skills.empty());
    ASSERT_EQ(snap.issues.size(), 1u);
    EXPECT_TRUE(snap.issues[0].fatal());
    EXPECT_EQ(snap.issues[0].failure, SkillLoadFailure::Unreadable);
    EXPECT_FALSE(snap.issues[0].detail.empty());
}

// 名字里没有一个能进斜杠命令的字符 → 无法注册,但要说清楚原因。
TEST_F(SkillLoadResilience, UnusableSkillNameReportsFatalIssue) {
    write_skill("symbols",
                "---\n"
                "name: \"!!!\"\n"
                "description: has a name that cannot be slugged\n"
                "---\n"
                "body\n");

    const auto snap = scan();
    EXPECT_TRUE(snap.skills.empty());
    ASSERT_EQ(snap.issues.size(), 1u);
    EXPECT_EQ(snap.issues[0].failure, SkillLoadFailure::UnusableName);
    EXPECT_TRUE(snap.issues[0].fatal());
}

// frontmatter 开了 `---` 却没闭合:skill 仍然可用(名字回退目录名),但要挂一条
// 非致命告警,否则用户只会看到一个描述莫名其妙的 skill。
TEST_F(SkillLoadResilience, UnterminatedFrontmatterWarnsButKeepsSkill) {
    write_skill("unterminated",
                "---\n"
                "name: unterminated\n"
                "description: never closed\n"
                "still inside the unclosed block\n");

    const auto snap = scan();
    ASSERT_EQ(snap.skills.size(), 1u);
    EXPECT_EQ(snap.skills[0].name, "unterminated");
    ASSERT_EQ(snap.issues.size(), 1u);
    EXPECT_FALSE(snap.issues[0].fatal());
    EXPECT_EQ(snap.issues[0].failure,
              SkillLoadFailure::UnterminatedFrontmatter);
    // 未闭合时整份文件落进 body,描述回退取正文首行 —— 不能取到那行分隔符。
    EXPECT_NE(snap.skills[0].description, "---");
}

// 完全没有 frontmatter 的 SKILL.md 历史上是可用的(名字取目录、描述取正文
// 首行),这个行为不能因为新增告警而回退。
TEST_F(SkillLoadResilience, MissingFrontmatterKeepsSkillUsable) {
    write_skill("plain", "a plain body line\n");

    const auto snap = scan();
    ASSERT_EQ(snap.skills.size(), 1u);
    EXPECT_EQ(snap.skills[0].name, "plain");
    EXPECT_EQ(snap.skills[0].description, "a plain body line");
    ASSERT_EQ(snap.issues.size(), 1u);
    EXPECT_FALSE(snap.issues[0].fatal());
    EXPECT_EQ(snap.issues[0].failure, SkillLoadFailure::MissingFrontmatter);
}

// 坏 skill 不能进 list():模型侧的 skill 索引、斜杠命令、skill_view 都读这个
// 列表,把一个连名字都注册不了的条目塞进去只会制造幻觉。
TEST_F(SkillLoadResilience, FatalIssuesNeverEnterTheUsableSkillList) {
    write_skill("ok-skill",
                "---\nname: ok-skill\ndescription: fine\n---\nbody\n");
    write_skill("broken", "");

    const auto snap = scan();
    ASSERT_EQ(snap.skills.size(), 1u);
    EXPECT_EQ(snap.skills[0].name, "ok-skill");
    ASSERT_EQ(snap.issues.size(), 1u);
    EXPECT_TRUE(snap.issues[0].fatal());
}

// inspect_skill_dir 声明为 noexcept —— 这条断言守住"扫描永不抛"的契约,
// 顺带覆盖不存在的目录。
TEST_F(SkillLoadResilience, InspectingAMissingDirectoryIsSilentAndSafe) {
    static_assert(noexcept(inspect_skill_dir(fs::path{}, fs::path{})),
                  "inspect_skill_dir must be noexcept");
    const auto outcome = inspect_skill_dir(root_ / "does-not-exist", root_);
    EXPECT_FALSE(outcome.meta.has_value());
    EXPECT_FALSE(outcome.issue.has_value());
}

// /api/skills 的载荷:坏 skill 以 status=error 出现在列表里(带原因和路径),
// 好 skill 照常 status=ok,而且整份响应必须能 dump 出来 —— 这正是过去 500 的
// 那一步。
TEST_F(SkillLoadResilience, PayloadListsBrokenSkillsAndStaysSerializable) {
    write_skill("good",
                "---\nname: good\ndescription: a working skill\n---\nbody\n");
    write_skill("blank", "");
    write_skill("no-desc", "---\nname: no-desc\n---\n");

    const auto arr = payload();
    ASSERT_TRUE(arr.is_array());

    const auto* good = find_entry(arr, "good");
    ASSERT_NE(good, nullptr);
    EXPECT_EQ(good->value("status", std::string{}), "ok");
    EXPECT_TRUE(good->value("enabled", false));

    const auto* broken = find_entry(arr, "blank");
    ASSERT_NE(broken, nullptr);
    EXPECT_EQ(broken->value("status", std::string{}), "error");
    EXPECT_EQ(broken->value("error_code", std::string{}),
              skill_load_failure_code(SkillLoadFailure::Unreadable));
    EXPECT_FALSE(broken->value("error", std::string{}).empty());
    EXPECT_FALSE(broken->value("path", std::string{}).empty());
    EXPECT_FALSE(broken->value("enabled", true));

    const auto* warned = find_entry(arr, "no-desc");
    ASSERT_NE(warned, nullptr);
    EXPECT_EQ(warned->value("status", std::string{}), "warning");
    EXPECT_EQ(warned->value("error_code", std::string{}),
              skill_load_failure_code(SkillLoadFailure::MissingDescription));
    // 告警行仍是一个可用技能,启停开关照常工作。
    EXPECT_TRUE(warned->value("enabled", false));

    EXPECT_NO_THROW((void)arr.dump());
}

// 直接锁定原始症状:一个 SKILL.md 里塞进无法解码的字节,响应仍然必须序列化
// 得出来。dump_json_lossy 是最后一道闸 —— 默认 dump() 会 type_error.316。
TEST_F(SkillLoadResilience, HostileBytesDoNotBreakSerialization) {
    std::string content = "---\nname: hostile-";
    content += static_cast<char>(0xE9);   // 残缺的 3 字节序列引导字节
    content += static_cast<char>(0x94);
    content += "\ndescription: >\n  bad ";
    content += static_cast<char>(0xFF);   // 非法引导字节
    content += " bytes\n---\nbody\n";
    write_skill("hostile", content);

    const auto snap = scan();
    for (const auto& s : snap.skills) {
        EXPECT_TRUE(is_valid_utf8(s.name)) << "skill name must be valid UTF-8";
        EXPECT_TRUE(is_valid_utf8(s.description));
        EXPECT_TRUE(is_valid_utf8(s.category));
    }

    const auto arr = payload();
    EXPECT_NO_THROW((void)acecode::web::dump_json_lossy(arr));
    EXPECT_NO_THROW((void)arr.dump());
}

} // namespace
