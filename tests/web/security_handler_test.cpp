#include <gtest/gtest.h>

#include "web/handlers/security_handler.hpp"
#include "test_support/sandbox/test_support.hpp"

#include <fstream>
#include <map>

using namespace acecode;
using namespace acecode::web;
using nlohmann::json;

// 场景:GET /api/config/sandbox 的快照。期望:三张清单、开关、只读透传字段、内置
// 默认 deny 名单与平台探测(受限令牌 → read_isolation=false)都在。
TEST(SecurityHandler, SandboxSnapshotCarriesListsDefaultsAndPlatform) {
    SandboxConfig sandbox;
    sandbox.filesystem_deny = {"~/.ssh"};
    sandbox.filesystem_write = {"D:/out"};
    sandbox.network_access = true;
    sandbox::BackendProbe probe;
    probe.kind = sandbox::BackendKind::WindowsRestrictedToken;
    probe.available = true;
    probe.network_best_effort = true;
    const json snapshot = sandbox_settings_snapshot(sandbox, probe);
    EXPECT_EQ(snapshot["enabled"], true);
    EXPECT_EQ(snapshot["network_access"], true);
    EXPECT_EQ(snapshot["deny_defaults"], true);
    EXPECT_EQ(snapshot["filesystem"]["deny"], json::array({"~/.ssh"}));
    EXPECT_EQ(snapshot["filesystem"]["write"], json::array({"D:/out"}));
    EXPECT_EQ(snapshot["filesystem"]["read"], json::array());
    EXPECT_EQ(snapshot["windows_backend"], "restricted-token");
    EXPECT_EQ(snapshot["defaults"]["deny"][0], "~/.ssh");
    EXPECT_EQ(snapshot["platform"]["backend"], "restricted-token");
    EXPECT_EQ(snapshot["platform"]["read_isolation"], false);
    EXPECT_EQ(snapshot["platform"]["network_best_effort"], true);

    probe.kind = sandbox::BackendKind::MacosSeatbelt;
    EXPECT_EQ(sandbox_settings_snapshot(sandbox, probe)["platform"]["read_isolation"], true);
}

// 场景:清单条目校验。期望:记号与绝对路径通过;相对路径、`~user`、未知记号、
// 非 deny 清单里的通配、多行条目被拒。
TEST(SecurityHandler, ValidatesSandboxEntries) {
    EXPECT_TRUE(validate_sandbox_entry("~", false).empty());
    EXPECT_TRUE(validate_sandbox_entry("~/.ssh", true).empty());
    EXPECT_TRUE(validate_sandbox_entry(":workspace_roots/build", false).empty());
    EXPECT_TRUE(validate_sandbox_entry(":tmpdir", false).empty());
    EXPECT_TRUE(validate_sandbox_entry(":acecode_home/config.json", true).empty());
    EXPECT_TRUE(validate_sandbox_entry("/var/tmp", false).empty());
#ifdef _WIN32
    EXPECT_TRUE(validate_sandbox_entry("C:\\data\\out", false).empty());
#else
    EXPECT_FALSE(validate_sandbox_entry("C:\\data\\out", false).empty());
#endif
    EXPECT_TRUE(validate_sandbox_entry("**/.env", true).empty());
    EXPECT_TRUE(validate_sandbox_entry("~/*.pem", true).empty());
    EXPECT_FALSE(validate_sandbox_entry("src", false).empty());
    EXPECT_FALSE(validate_sandbox_entry("~bob/x", false).empty());
    EXPECT_FALSE(validate_sandbox_entry(":home/x", false).empty());
    EXPECT_FALSE(validate_sandbox_entry("**/.env", false).empty()) << "通配只允许出现在 deny";
    EXPECT_FALSE(validate_sandbox_entry("/a\n/b", false).empty());
    EXPECT_FALSE(validate_sandbox_entry("   ", false).empty());
}

// 场景:PUT body 只带部分字段。期望:出现的字段才改,清单整体替换并去空白 / 去重;
// 非法条目报错时 out 保持原样并指出字段。
TEST(SecurityHandler, ParsesPartialSandboxRequestAndRejectsBadEntries) {
    SandboxConfig out;
    out.filesystem_read = {"/keep"};
    std::string error;
    std::string field;
    ASSERT_TRUE(parse_sandbox_settings_request(
        json{{"enabled", false}, {"filesystem", json{{"deny", json::array({" ~/.ssh ", "~/.ssh", "", "**/.env"})}}}},
        out, error, field)) << error;
    EXPECT_FALSE(out.enabled);
    EXPECT_EQ(out.filesystem_read, std::vector<std::string>{"/keep"}) << "未出现的清单不动";
    EXPECT_EQ(out.filesystem_deny, (std::vector<std::string>{"~/.ssh", "**/.env"}));

    SandboxConfig untouched = out;
    EXPECT_FALSE(parse_sandbox_settings_request(
        json{{"filesystem", json{{"write", json::array({"src"})}}}}, out, error, field));
    EXPECT_EQ(field, "filesystem.write");
    EXPECT_NE(error.find("absolute"), std::string::npos);
    EXPECT_EQ(out.filesystem_write, untouched.filesystem_write);
    EXPECT_FALSE(parse_sandbox_settings_request(json{{"enabled", "yes"}}, out, error, field));
    EXPECT_EQ(field, "enabled");
    EXPECT_FALSE(parse_sandbox_settings_request(json::array(), out, error, field));
}

// 场景:规则目录里有一个手写文件(带解析错误)与一个托管文件,另一个托管文件不存在。
// 期望:托管文件排前面且即使不存在也列出(exists=false、空规则);手写文件 managed=false
// 且 error 非空;规则 JSON 的 pattern 单候选是字符串、多候选是数组。
TEST(SecurityHandler, ReadsRulesDirectoryWithManagedFilesFirst) {
    sandbox::test::TempTree tree;
    const auto dir = tree.dir("rules");
    tree.write(dir / "default.rules",
               "prefix_rule(pattern=[\"git\", [\"status\", \"diff\"]], decision=\"allow\", justification=\"safe\")\n"
               "prefix_rule(pattern=[\"git\", \"push\"], decision=\"prompt\")\n");
    tree.write(dir / "custom.rules", "prefix_rule(pattern=['x'], decision='maybe')\n");
    tree.write(dir / "notes.txt", "ignored");
    const auto files = read_exec_rules_dir(path_to_utf8(dir));
    ASSERT_EQ(files.size(), 3u);
    EXPECT_EQ(files[0].name, "default.rules");
    EXPECT_TRUE(files[0].managed);
    EXPECT_TRUE(files[0].exists);
    ASSERT_EQ(files[0].rules.size(), 2u);
    EXPECT_EQ(files[1].name, "default.sandboxed.rules");
    EXPECT_TRUE(files[1].managed);
    EXPECT_FALSE(files[1].exists);
    EXPECT_EQ(files[1].scope, sandbox::RuleScope::Sandboxed);
    EXPECT_EQ(files[2].name, "custom.rules");
    EXPECT_FALSE(files[2].managed);
    EXPECT_FALSE(files[2].error.empty());

    const json snapshot = exec_rules_snapshot(path_to_utf8(dir), files);
    EXPECT_EQ(snapshot["dir"], path_to_utf8(dir));
    EXPECT_EQ(snapshot["managed"], json::array({"default.rules", "default.sandboxed.rules"}));
    const auto& rules = snapshot["files"][0]["rules"];
    EXPECT_EQ(rules[0]["pattern"], json::array({"git", json::array({"status", "diff"})}));
    EXPECT_EQ(rules[0]["display"], "git status|diff");
    EXPECT_EQ(rules[0]["decision"], "allow");
    EXPECT_EQ(rules[0]["justification"], "safe");
    EXPECT_EQ(rules[1]["decision"], "prompt");
    EXPECT_EQ(snapshot["files"][1]["scope"], "sandboxed");
}

// 场景:PUT /api/security/exec-rules 的 body 解析。期望:合法 body 得到两份规则表
// (sandboxed 文件的 allow 规则带 Sandboxed 作用域);非托管文件、非法 decision、
// sandboxed 文件里的 prompt、`rm` 放行(禁用前缀)、空 pattern 都被拒且 out 不变。
TEST(SecurityHandler, ParsesExecRulesRequestAndRejectsBannedPrefixes) {
    std::map<std::string, std::vector<sandbox::PrefixRule>> out;
    std::string error;
    ASSERT_TRUE(parse_exec_rules_request(json{{"files", json{
        {"default.rules", json::array({
            json{{"pattern", json::array({"pnpm", "test"})}, {"decision", "allow"}, {"justification", " ci "}},
            json{{"pattern", json::array({"git", json::array({"push", "fetch"})})}, {"decision", "prompt"}},
            json{{"pattern", json::array({"curl"})}, {"decision", "forbidden"}}})},
        {"default.sandboxed.rules", json::array({
            json{{"pattern", json::array({"git", "push", "--force"})}}})}}}}, out, error)) << error;
    ASSERT_EQ(out.at("default.rules").size(), 3u);
    EXPECT_EQ(out.at("default.rules")[0].justification, "ci");
    EXPECT_EQ(out.at("default.rules")[1].pattern[1], (std::vector<std::string>{"push", "fetch"}));
    EXPECT_EQ(out.at("default.rules")[2].decision, sandbox::RuleDecision::Forbidden);
    ASSERT_EQ(out.at("default.sandboxed.rules").size(), 1u);
    EXPECT_EQ(out.at("default.sandboxed.rules")[0].scope, sandbox::RuleScope::Sandboxed);

    const auto before = out;
    const auto reject = [&](const json& body) {
        EXPECT_FALSE(parse_exec_rules_request(body, out, error)) << body.dump();
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(out.size(), before.size());
    };
    reject(json{{"files", json{{"custom.rules", json::array()}}}});
    reject(json{{"files", json{{"default.rules", json::array({json{{"pattern", json::array({"git"})}, {"decision", "maybe"}}})}}}});
    reject(json{{"files", json{{"default.sandboxed.rules", json::array({json{{"pattern", json::array({"git"})}, {"decision", "prompt"}}})}}}});
    reject(json{{"files", json{{"default.rules", json::array({json{{"pattern", json::array({"rm", "-rf"})}, {"decision", "allow"}}})}}}});
    reject(json{{"files", json{{"default.rules", json::array({json{{"pattern", json::array({json::array({"python", "git"})})}, {"decision", "allow"}}})}}}});
    reject(json{{"files", json{{"default.rules", json::array({json{{"pattern", json::array()}}})}}}});
    reject(json{{"files", json{{"default.rules", json::array({json{{"pattern", json::array({""})}}})}}}});
    reject(json{{"nope", 1}});
    // 禁止 / 询问规则不受禁用名单限制:用户就是要拦 rm。
    ASSERT_TRUE(parse_exec_rules_request(json{{"files", json{{"default.rules", json::array({
        json{{"pattern", json::array({"rm"})}, {"decision", "forbidden"}}})}}}}, out, error)) << error;
}

// 场景:审计查询参数解析。期望:默认 limit 100;非法 category / decision / 数字 / 越界
// limit 报错;`q` 去空白。
TEST(SecurityHandler, ParsesAuditQueryParams) {
    security::AuditQuery query;
    std::string error;
    ASSERT_TRUE(parse_audit_query({}, query, error)) << error;
    EXPECT_EQ(query.limit, 100);
    EXPECT_TRUE(query.category.empty());
    ASSERT_TRUE(parse_audit_query({{"category", "sandbox"}, {"decision", "blocked"}, {"since_ms", "1700000000000"},
                                   {"before_id", "42"}, {"q", "  ssh "}, {"limit", "500"}}, query, error)) << error;
    EXPECT_EQ(query.category, "sandbox");
    EXPECT_EQ(query.decision, "blocked");
    EXPECT_EQ(query.since_ms, 1700000000000LL);
    EXPECT_EQ(query.before_id, 42);
    EXPECT_EQ(query.text, "ssh");
    EXPECT_EQ(query.limit, 500);
    EXPECT_FALSE(parse_audit_query({{"category", "network"}}, query, error));
    EXPECT_FALSE(parse_audit_query({{"decision", "maybe"}}, query, error));
    EXPECT_FALSE(parse_audit_query({{"since_ms", "abc"}}, query, error));
    EXPECT_FALSE(parse_audit_query({{"limit", "0"}}, query, error));
    EXPECT_FALSE(parse_audit_query({{"limit", "501"}}, query, error));
    EXPECT_FALSE(parse_audit_query({{"before_id", "-1"}}, query, error));
}

// 场景:分页与汇总的 JSON 形状。期望:has_more 时带 next_before_id(最后一条 id);
// 汇总把 map 与被拦路径列表原样序列化;导出文件名按格式带扩展名。
TEST(SecurityHandler, SerializesAuditPageAndSummary) {
    security::AuditPage page;
    security::AuditEntry a;
    a.id = 9;
    a.category = "command";
    security::AuditEntry b;
    b.id = 8;
    b.category = "file";
    page.entries = {a, b};
    page.has_more = true;
    page.total = 20;
    const json out = audit_page_to_json(page);
    EXPECT_EQ(out["entries"].size(), 2u);
    EXPECT_EQ(out["entries"][0]["id"], 9);
    EXPECT_EQ(out["next_before_id"], 8);
    EXPECT_EQ(out["total"], 20);
    page.has_more = false;
    EXPECT_FALSE(audit_page_to_json(page).contains("next_before_id"));

    security::AuditSummary summary;
    summary.total = 3;
    summary.by_decision["allow"] = 2;
    summary.by_category["sandbox"] = 1;
    summary.last_ts_ms = 5;
    summary.blocked_paths.push_back({"C:/secret", 1, 5});
    const json s = audit_summary_to_json(summary);
    EXPECT_EQ(s["by_decision"]["allow"], 2);
    EXPECT_EQ(s["blocked_paths"][0]["path"], "C:/secret");
    EXPECT_EQ(s["blocked_paths"][0]["count"], 1);

    EXPECT_EQ(audit_export_filename("csv", 0).substr(0, 14), "acecode-audit-");
    EXPECT_NE(audit_export_filename("csv", 0).find(".csv"), std::string::npos);
    EXPECT_NE(audit_export_filename("jsonl", 0).find(".jsonl"), std::string::npos);
}
