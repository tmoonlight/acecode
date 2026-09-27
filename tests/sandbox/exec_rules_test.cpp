#include <gtest/gtest.h>
#include "sandbox/exec_rules.hpp"
#include "test_support/sandbox/test_support.hpp"
#include <fstream>
#include <iterator>

using namespace acecode::sandbox;

// 场景:全局 allow 了 `bash`,模型执行 `bash -c 'echo $UNTRUSTED'`;另有
// `curl` forbidden,模型用 `sudo curl` 包一层。期望:不透明脚本不能借外层
// allow 放行;forbidden 穿透 sudo 包装仍生效。
TEST(ExecRules, OpaqueNestedScriptCannotBorrowWrapperAllow) {
    ExecRules rules;
    auto parsed = parse_rules_text("prefix_rule(pattern=[\"bash\"], decision=\"allow\")",
                                  RuleScope::Global, "global.rules");
    ASSERT_TRUE(parsed.error.empty());
    rules.add_rule(parsed.rules.front());
    const auto classification = classify_command("bash -c 'echo $UNTRUSTED'", CommandPlatform::Posix);
    EXPECT_FALSE(classification.split_safely);
    EXPECT_EQ(rules.evaluate(classification).decision, RuleDecision::NoMatch);
    auto denied = parse_rules_text("prefix_rule(pattern=[\"curl\"], decision=\"forbidden\")",
                                  RuleScope::Global, "global.rules");
    rules.add_rule(denied.rules.front());
    EXPECT_EQ(rules.evaluate(classify_command("sudo curl url", CommandPlatform::Posix)).decision, RuleDecision::Forbidden);
}

// 场景:Codex 语法的 pattern 并集(`["status","diff"]`)+ match / not_match
// 校验。期望:解析成功,`git.exe diff` 经 basename 回退命中,`git commit` 不命中。
TEST(ExecRules, ParsesAlternativesAndChecksExamples) {
    const auto parsed = parse_rules_text(R"(
        # Codex-compatible prefix rules
        prefix_rule(pattern=["git", ["status", "diff"]], match=["git status"], not_match=["git push"])
    )", RuleScope::Global, "example.rules");
    ASSERT_TRUE(parsed.error.empty()) << parsed.error;
    ASSERT_EQ(parsed.rules.size(), 1u);
    EXPECT_TRUE(prefix_rule_matches(parsed.rules[0], {{"git.exe", "diff"}}));
    EXPECT_FALSE(prefix_rule_matches(parsed.rules[0], {{"git", "commit"}}));
}

// 场景:同一文件中存在非法规则。期望:不能只保留前面的 allow。
TEST(ExecRules, RejectsWholeFileOnInvalidSyntaxOrExamples) {
    for (const char* tail : {"prefix_rule(pattern=['b'], decision='maybe')",
         "prefix_rule(pattern=['git'], match=['curl x'])",
         "prefix_rule(pattern=['git'], not_match=['git status'])",
         "prefix_rule(pattern=['b'], decision='forbidden', decision='allow')"}) {
        const auto parsed = parse_rules_text(std::string("prefix_rule(pattern=['a'])\n") + tail,
            RuleScope::Global, "bad.rules");
        EXPECT_FALSE(parsed.error.empty());
        EXPECT_TRUE(parsed.rules.empty());
    }
}

// 场景:全局 allow git、项目 forbidden git push、项目 allow pnpm。期望:多规则
// 取最严格;多段命令须每段都 allow,含项目 allow 的整体降级为 AllowSandboxed,
// 有一段没命中就是 NoMatch;forbidden 穿透 `bash -c` 包装。
TEST(ExecRules, AppliesStrictestDecisionAndProjectScope) {
    ExecRules rules;
    rules.add_rule({{{"git"}}, RuleDecision::Allow, "", RuleScope::Global, ""});
    rules.add_rule({{{"git"}, {"push"}}, RuleDecision::Forbidden, "no push", RuleScope::Project, ""});
    rules.add_rule({{{"pnpm"}}, RuleDecision::Allow, "", RuleScope::Project, ""});
    EXPECT_EQ(rules.evaluate(classify_command("git status")).decision, RuleDecision::Allow);
    EXPECT_EQ(rules.evaluate(classify_command("git status && pnpm test")).decision, RuleDecision::AllowSandboxed);
    EXPECT_EQ(rules.evaluate(classify_command("git status && unknown-tool")).decision, RuleDecision::NoMatch);
    EXPECT_EQ(rules.evaluate(classify_command("git push")).decision, RuleDecision::Forbidden);
    EXPECT_EQ(rules.evaluate(classify_command("bash -c 'git push'", CommandPlatform::Posix)).decision, RuleDecision::Forbidden);
}

// 场景:禁用前缀名单(align-codex-sandboxing D6)。期望:解释器 / shell / rm /
// sudo / cmd /c / powershell -Command / `git` 单独 都禁;`git commit`、`pnpm install`
// 不禁;首 token 按 basename 比(`C:\...\node.exe` = node),大小写不敏感;空列表禁。
TEST(ExecRules, BannedPrefixListCoversInterpretersAndDestructiveTools) {
    for (const auto& banned : std::vector<std::vector<std::string>>{
             {"bash"}, {"sh", "-c"}, {"python3", "-c"}, {"node", "-e"}, {"rm"}, {"sudo"}, {"git"},
             {"cmd", "/c"}, {"powershell", "-Command"}, {"PowerShell.exe", "-command"},
             {"C:\\Program Files\\nodejs\\node.exe"}, {"del"}, {"Remove-Item"}, {"npx"}}) {
        EXPECT_TRUE(is_banned_prefix(banned)) << banned.front();
    }
    for (const auto& ok : std::vector<std::vector<std::string>>{
             {"git", "commit"}, {"pnpm", "install"}, {"cargo", "test"}, {"ls"}, {"node", "script.js"}}) {
        EXPECT_FALSE(is_banned_prefix(ok)) << ok.front();
    }
    EXPECT_TRUE(is_banned_prefix({}));
}

// 场景:推导可写回规则文件的 pattern。期望:模型给的 prefix_rule 覆盖每一段时
// 作为唯一 pattern;只覆盖一段 / 命中禁用名单时退回逐段推导;逐段推导取
// `always_allow_prefix_tokens`(多级 CLI 带子命令),任一段推不出(解释器)则
// 整体不提供;不可安全拆段的命令不提供。
TEST(ExecRules, DerivesRememberPatternsFromProposalOrSegments) {
    const auto multi = classify_command("git status && pnpm test", CommandPlatform::Posix);
    EXPECT_EQ(derive_remember_patterns(multi, {}),
              (std::vector<std::vector<std::string>>{{"git", "status"}, {"pnpm", "test"}}));
    EXPECT_EQ(derive_remember_patterns(multi, {"git"}), std::vector<std::vector<std::string>>{})
        << "`git` 单独在禁用名单";
    EXPECT_EQ(derive_remember_patterns(multi, {"pnpm"}), std::vector<std::vector<std::string>>{})
        << "建议前缀没覆盖每一段(git status 不以 pnpm 开头)→ 不提供";
    const auto single = classify_command("pnpm install lodash", CommandPlatform::Posix);
    EXPECT_EQ(derive_remember_patterns(single, {"pnpm", "install"}),
              (std::vector<std::vector<std::string>>{{"pnpm", "install"}}));
    EXPECT_EQ(derive_remember_patterns(single, {"pnpm", "test"}), std::vector<std::vector<std::string>>{})
        << "建议前缀与命令不符,也不用逐段推导兜底(模型明确给了错的)";
    EXPECT_TRUE(derive_remember_patterns(classify_command("python -c 'x'", CommandPlatform::Posix), {}).empty());
    EXPECT_TRUE(derive_remember_patterns(classify_command("git status > out.txt", CommandPlatform::Posix), {}).empty());
}

// 场景:把 pattern 追加进规则文件并重新加载。期望:文件与父目录自动创建,格式与
// Codex amend.rs 一致;同 pattern 不重复追加;`default.sandboxed.rules` 加载后 allow
// 降级为 AllowSandboxed,`default.rules` 仍是 Allow;已有内容末尾无换行时补换行。
TEST(ExecRules, AppendsRulesWithoutDuplicatesAndLoadsSandboxedScope) {
    acecode::sandbox::test::TempTree tree;
    const auto dir = tree.root / "rules";
    const std::string bypass_file = acecode::path_to_utf8(dir / kRememberedRulesFile);
    const std::string sandboxed_file = acecode::path_to_utf8(dir / kRememberedSandboxedRulesFile);
    EXPECT_EQ(format_prefix_rule({"git", "com\"mit"}), "prefix_rule(pattern=[\"git\", \"com\\\"mit\"], decision=\"allow\")");
    ASSERT_TRUE(append_prefix_rules(bypass_file, {{"pnpm", "install"}}).empty());
    ASSERT_TRUE(append_prefix_rules(bypass_file, {{"pnpm", "install"}, {"cargo", "test"}}).empty());
    tree.write(dir / "manual.rules", "prefix_rule(pattern=[\"ls\"])");   // 无换行结尾
    ASSERT_TRUE(append_prefix_rules(acecode::path_to_utf8(dir / "manual.rules"), {{"pwd"}}).empty());
    ASSERT_TRUE(append_prefix_rules(sandboxed_file, {{"pnpm", "test"}}).empty());
    std::ifstream in(dir / kRememberedRulesFile);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "prefix_rule(pattern=[\"pnpm\", \"install\"], decision=\"allow\")\n"
                       "prefix_rule(pattern=[\"cargo\", \"test\"], decision=\"allow\")\n");
    std::ifstream manual(dir / "manual.rules");
    std::string manual_content((std::istreambuf_iterator<char>(manual)), std::istreambuf_iterator<char>());
    EXPECT_EQ(manual_content, "prefix_rule(pattern=[\"ls\"])\nprefix_rule(pattern=[\"pwd\"], decision=\"allow\")\n");
    const auto rules = ExecRules::load(acecode::path_to_utf8(dir), "");
    EXPECT_TRUE(rules.skipped_files().empty());
    EXPECT_EQ(rules.evaluate(classify_command("pnpm install x", CommandPlatform::Posix)).decision, RuleDecision::Allow);
    EXPECT_EQ(rules.evaluate(classify_command("pnpm test", CommandPlatform::Posix)).decision, RuleDecision::AllowSandboxed);
    EXPECT_EQ(rules.evaluate(classify_command("pwd", CommandPlatform::Posix)).decision, RuleDecision::Allow);
    EXPECT_FALSE(append_prefix_rules("", {{"x"}}).empty());
    EXPECT_FALSE(append_prefix_rules(bypass_file, {}).empty());
}

// 场景:allow 前缀后追加重定向或命令替换。期望:不可凭前缀授权整段不透明脚本。
TEST(ExecRules, DoesNotAllowOpaqueScriptsByPrefix) {
    ExecRules rules;
    rules.add_rule({{{"echo"}}, RuleDecision::Allow, "", RuleScope::Global, ""});
    EXPECT_EQ(rules.evaluate(classify_command("echo x > outside")).decision, RuleDecision::NoMatch);
    EXPECT_EQ(rules.evaluate(classify_command("echo $(custom-script)", CommandPlatform::Posix)).decision, RuleDecision::NoMatch);
}

// 场景:安全中心把规则表整体写回托管文件(openspec add-security-center D3):候选
// 并集、prompt / forbidden 决策、justification 里带引号与反斜杠。期望:渲染 → 解析
// 往返得到相同 pattern / decision / justification;文件头是注释;sandboxed 文件按
// 文件名判定作用域。
TEST(ExecRules, ManagedFileRoundTripsThroughRenderAndParse) {
    test::TempTree tree;
    PrefixRule a;
    a.pattern = {{"git"}, {"status", "diff"}};
    a.decision = RuleDecision::Allow;
    a.justification = "read-only \"safe\" \ ok";
    PrefixRule b;
    b.pattern = {{"git"}, {"push"}};
    b.decision = RuleDecision::Prompt;
    PrefixRule c;
    c.pattern = {{"curl"}};
    c.decision = RuleDecision::Forbidden;
    const auto file = tree.root / "rules" / kRememberedRulesFile;
    ASSERT_TRUE(write_rules_file(acecode::path_to_utf8(file), {a, b, c}).empty());
    std::ifstream in(file, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content.rfind("# Managed by ACECode", 0), 0u) << content;
    const auto parsed = parse_rules_text(content, RuleScope::Global, kRememberedRulesFile);
    ASSERT_TRUE(parsed.error.empty()) << parsed.error;
    ASSERT_EQ(parsed.rules.size(), 3u);
    EXPECT_EQ(parsed.rules[0].pattern, a.pattern);
    EXPECT_EQ(parsed.rules[0].justification, a.justification);
    EXPECT_EQ(parsed.rules[1].decision, RuleDecision::Prompt);
    EXPECT_EQ(parsed.rules[2].decision, RuleDecision::Forbidden);
    EXPECT_TRUE(is_managed_rules_file(kRememberedRulesFile));
    EXPECT_TRUE(is_managed_rules_file(kRememberedSandboxedRulesFile));
    EXPECT_FALSE(is_managed_rules_file("custom.rules"));
    EXPECT_EQ(rules_file_scope(kRememberedSandboxedRulesFile), RuleScope::Sandboxed);
    EXPECT_EQ(rules_file_scope(kRememberedRulesFile), RuleScope::Global);
    // 加载器读回:沙盒外 allow 保持 Allow。
    const auto loaded = ExecRules::load(acecode::path_to_utf8(tree.root / "rules"), "");
    EXPECT_EQ(loaded.evaluate(classify_command("git status", CommandPlatform::Posix)).decision, RuleDecision::Allow);
    EXPECT_EQ(loaded.evaluate(classify_command("curl x", CommandPlatform::Posix)).decision, RuleDecision::Forbidden);
}

// 场景:空表写回、非法规则(空 token)写回。期望:空表只剩注释头,加载后无规则无错误;
// 非法规则不落盘(文件保持原内容)。
TEST(ExecRules, EmptyManagedFileLoadsCleanAndInvalidRulesAreNotWritten) {
    test::TempTree tree;
    const auto file = tree.root / "rules" / kRememberedSandboxedRulesFile;
    ASSERT_TRUE(write_rules_file(acecode::path_to_utf8(file), {}).empty());
    const auto loaded = ExecRules::load(acecode::path_to_utf8(tree.root / "rules"), "");
    EXPECT_TRUE(loaded.empty());
    EXPECT_TRUE(loaded.skipped_files().empty());
    std::ifstream in(file, std::ios::binary);
    const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    PrefixRule bad;
    bad.pattern = {{"git"}, {""}};
    EXPECT_FALSE(write_rules_file(acecode::path_to_utf8(file), {bad}).empty());
    PrefixRule no_pattern;
    EXPECT_FALSE(write_rules_file(acecode::path_to_utf8(file), {no_pattern}).empty());
    std::ifstream again(file, std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, before);
    EXPECT_EQ(format_prefix_rule_full(PrefixRule{{{"a", "b"}, {"c"}}, RuleDecision::AllowSandboxed, "", RuleScope::Sandboxed, ""}),
              "prefix_rule(pattern=[[\"a\", \"b\"], \"c\"], decision=\"allow\")");
}
