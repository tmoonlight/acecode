#include <gtest/gtest.h>
#include "security/audit_log.hpp"
#include "test_support/sandbox/test_support.hpp"

#include <algorithm>
#include <filesystem>

using namespace acecode::security;

namespace {

AuditEntry make_entry(const char* category, const char* decision, const std::string& target,
                      std::int64_t ts_ms = 0) {
    AuditEntry entry;
    entry.category = category;
    entry.decision = decision;
    entry.source = kAuditSourceAuto;
    entry.reason = "known_safe";
    entry.tool = "bash";
    entry.target = target;
    entry.session_id = "s1";
    entry.cwd = "C:/work";
    entry.sandbox = "workspace-write";
    entry.ts_ms = ts_ms;
    entry.detail = {{"mode", "auto"}};
    return entry;
}

struct TempLog {
    acecode::sandbox::test::TempTree tree;
    AuditLog log;
    TempLog() {
        std::string error;
        EXPECT_TRUE(log.open_file(acecode::path_to_utf8(tree.root / "audit.sqlite3"), &error)) << error;
    }
};

} // namespace

// 场景:未 configure 的存储。期望:record 返回 false 且不抛,query 报「未配置」,
// 与 AgentLoop 里「审计失败不影响工具执行」的约定一致。
TEST(AuditLog, UnconfiguredLogIsNoop) {
    AuditLog log;
    EXPECT_FALSE(log.available());
    AuditEntry entry = make_entry("command", "allow", "git status");
    EXPECT_FALSE(log.record(entry));
    std::string error;
    const auto page = log.query({}, &error);
    EXPECT_TRUE(page.entries.empty());
    EXPECT_FALSE(error.empty());
}

// 场景:写入 3 条后按 limit=2 查询,再用 before_id 续页。期望:第一页是最新 2 条
// (id 倒序)且 has_more;第二页剩 1 条;total 恒为 3;detail 原样回读;id 回填。
TEST(AuditLog, RecordsAndPagesNewestFirst) {
    TempLog fx;
    AuditEntry a = make_entry("command", "allow", "git status", 1000);
    AuditEntry b = make_entry("file", "allow", "C:/work/a.txt", 2000);
    AuditEntry c = make_entry("sandbox", "blocked", "C:/secret", 3000);
    ASSERT_TRUE(fx.log.record(a));
    ASSERT_TRUE(fx.log.record(b));
    ASSERT_TRUE(fx.log.record(c));
    EXPECT_GT(a.id, 0);
    EXPECT_GT(c.id, b.id);

    AuditQuery query;
    query.limit = 2;
    std::string error;
    const auto first = fx.log.query(query, &error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(first.entries.size(), 2u);
    EXPECT_EQ(first.entries[0].target, "C:/secret");
    EXPECT_EQ(first.entries[1].target, "C:/work/a.txt");
    EXPECT_TRUE(first.has_more);
    EXPECT_EQ(first.total, 3);
    EXPECT_EQ(first.entries[0].detail["mode"], "auto");
    EXPECT_EQ(first.entries[0].sandbox, "workspace-write");

    query.before_id = first.entries.back().id;
    const auto second = fx.log.query(query, &error);
    ASSERT_EQ(second.entries.size(), 1u);
    EXPECT_EQ(second.entries[0].target, "git status");
    EXPECT_FALSE(second.has_more);
    EXPECT_EQ(second.total, 3);
}

// 场景:按 category / decision / since_ms / 关键字组合筛选。期望:只返回同时满足
// 全部条件的记录;关键字里的 `%` 不是通配符(LIKE 转义)。
TEST(AuditLog, FiltersCombine) {
    TempLog fx;
    ASSERT_TRUE(fx.log.record(make_entry("command", "allow", "git status", 1000)));
    ASSERT_TRUE(fx.log.record(make_entry("command", "deny", "rm -rf build", 2000)));
    ASSERT_TRUE(fx.log.record(make_entry("sandbox", "blocked", "C:/secret/key", 3000)));
    ASSERT_TRUE(fx.log.record(make_entry("command", "allow", "echo 100%", 4000)));

    AuditQuery query;
    query.category = "command";
    query.decision = "allow";
    EXPECT_EQ(fx.log.query(query).entries.size(), 2u);
    query.since_ms = 1500;
    EXPECT_EQ(fx.log.query(query).entries.size(), 1u);
    query = {};
    query.text = "secret";
    ASSERT_EQ(fx.log.query(query).entries.size(), 1u);
    EXPECT_EQ(fx.log.query(query).entries[0].category, "sandbox");
    query.text = "100%";
    ASSERT_EQ(fx.log.query(query).entries.size(), 1u);
    query.text = "%";
    EXPECT_EQ(fx.log.query(query).entries.size(), 1u) << "字面量 % 只命中含 % 的那条";
}

// 场景:汇总。期望:total / by_decision / by_category / last_ts_ms 正确;被拦路径
// 按 target 聚合计数、最近优先、空 target 不进列表。
TEST(AuditLog, SummaryAggregatesBlockedPaths) {
    TempLog fx;
    ASSERT_TRUE(fx.log.record(make_entry("command", "allow", "git status", 1000)));
    ASSERT_TRUE(fx.log.record(make_entry("sandbox", "blocked", "C:/secret", 2000)));
    ASSERT_TRUE(fx.log.record(make_entry("sandbox", "blocked", "C:/secret", 3000)));
    ASSERT_TRUE(fx.log.record(make_entry("sandbox", "blocked", "D:/data", 4000)));
    ASSERT_TRUE(fx.log.record(make_entry("sandbox", "blocked", "", 5000)));
    const auto summary = fx.log.summary();
    EXPECT_EQ(summary.total, 5);
    EXPECT_EQ(summary.by_decision.at("blocked"), 4);
    EXPECT_EQ(summary.by_category.at("command"), 1);
    EXPECT_EQ(summary.last_ts_ms, 5000);
    ASSERT_EQ(summary.blocked_paths.size(), 2u);
    EXPECT_EQ(summary.blocked_paths[0].path, "D:/data");
    EXPECT_EQ(summary.blocked_paths[1].path, "C:/secret");
    EXPECT_EQ(summary.blocked_paths[1].count, 2);
    EXPECT_EQ(summary.blocked_paths[1].last_ts_ms, 3000);
}

// 场景:上限设为 10,写入 300 条(跨过 256 条一次的修剪节奏),再清空。期望:
// 修剪后总数不超过上限 + 一个修剪周期,留下的是最新的;clear 后为 0 且 id 重新计数。
TEST(AuditLog, PrunesOldestBeyondCapAndClears) {
    TempLog fx;
    fx.log.set_max_entries(10);
    for (int i = 1; i <= 300; ++i) {
        ASSERT_TRUE(fx.log.record(make_entry("command", "allow", "cmd " + std::to_string(i), i)));
    }
    EXPECT_LE(fx.log.count(), 10 + 256);
    const auto page = fx.log.query({});
    ASSERT_FALSE(page.entries.empty());
    EXPECT_EQ(page.entries[0].target, "cmd 300");
    EXPECT_EQ(fx.log.query({{}, {}, 0, 0, "cmd 1", 5}).entries.size(), 0u)
        << "cmd 1 (最旧) 已被修剪(cmd 1x / cmd 1xx 也都在被修剪的范围内)";
    ASSERT_TRUE(fx.log.clear());
    EXPECT_EQ(fx.log.count(), 0);
    AuditEntry fresh = make_entry("command", "allow", "after clear", 1);
    ASSERT_TRUE(fx.log.record(fresh));
    EXPECT_EQ(fresh.id, 1) << "清空后自增 id 重新开始";
}

// 场景:导出。期望:JSONL 每行一个对象;CSV 带表头、含逗号 / 引号 / 换行的字段被
// 引号包裹并把内部引号加倍。
TEST(AuditLog, RendersJsonlAndCsv) {
    AuditEntry entry = make_entry("command", "allow", "echo \"a,b\"\nls", 1234);
    entry.id = 7;
    const std::string jsonl = render_audit_jsonl({entry});
    EXPECT_EQ(std::count(jsonl.begin(), jsonl.end(), '\n'), 1);
    const auto parsed = nlohmann::json::parse(jsonl.substr(0, jsonl.size() - 1));
    EXPECT_EQ(parsed["id"], 7);
    EXPECT_EQ(parsed["target"], "echo \"a,b\"\nls");
    const std::string csv = render_audit_csv({entry});
    EXPECT_EQ(csv.rfind("id,ts_ms,category,", 0), 0u);
    EXPECT_NE(csv.find("\"echo \"\"a,b\"\"\nls\""), std::string::npos) << csv;
}

// 场景:configure 到数据目录。期望:自动创建 security/ 子目录与数据库文件;
// 空目录名报错且保持未配置。
TEST(AuditLog, ConfigureCreatesSecurityDirectory) {
    acecode::sandbox::test::TempTree tree;
    AuditLog log;
    std::string error;
    ASSERT_TRUE(log.configure(acecode::path_to_utf8(tree.root), &error)) << error;
    EXPECT_TRUE(log.available());
    EXPECT_TRUE(std::filesystem::exists(tree.root / "security" / "audit.sqlite3"));
    EXPECT_EQ(log.path(), AuditLog::database_path_for(acecode::path_to_utf8(tree.root)));
    AuditLog empty;
    EXPECT_FALSE(empty.configure("", &error));
    EXPECT_FALSE(empty.available());
}
