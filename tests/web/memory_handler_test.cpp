// 覆盖 src/apps/web/handlers/memory_handler.{hpp,cpp}(openspec unify-memory-system 7.2):
// /api/config/memory 与 /api/memory* 的纯函数层 —— 设置 PATCH 解析、响应体形状、
// 条目序列化与编辑请求校验。路由与鉴权在 web smoke 里另测。

#include <gtest/gtest.h>

#include "memory/memory_service.hpp"
#include "session_host/memory_scheduler.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "web/handlers/memory_handler.hpp"

#include <nlohmann/json.hpp>

using nlohmann::json;

// 场景:个性化页只改了记忆摘要开关(PATCH 语义)。
// 期望:只覆盖给出的字段,其余沿用当前值。
TEST(MemoryHandlerTest, SettingsPatchKeepsMissingFields) {
    acecode::MemoryConfig current;
    current.summary.model_name = "fast";
    acecode::MemoryConfig out;
    std::string error;
    ASSERT_TRUE(acecode::web::parse_memory_settings_request(
        json::parse(R"({"summary":{"enabled":true}})"), current, out, error)) << error;
    EXPECT_TRUE(out.enabled);
    EXPECT_TRUE(out.summary.enabled);
    EXPECT_EQ(out.summary.model_name, "fast");
    EXPECT_EQ(out.summary.idle_minutes, 30);
}

// 场景:请求体类型不对(布尔写成字符串、summary 不是对象、整体不是对象、数值字段不是整数)。
// 期望:解析失败并给出可读原因,out 不被修改。
TEST(MemoryHandlerTest, SettingsPatchRejectsWrongTypes) {
    const acecode::MemoryConfig current;
    acecode::MemoryConfig out;
    out.max_index_bytes = 1;
    std::string error;
    EXPECT_FALSE(acecode::web::parse_memory_settings_request(json::parse(R"({"enabled":"yes"})"), current, out, error));
    EXPECT_NE(error.find("enabled"), std::string::npos);
    EXPECT_FALSE(acecode::web::parse_memory_settings_request(json::parse(R"({"summary":true})"), current, out, error));
    EXPECT_FALSE(acecode::web::parse_memory_settings_request(json::parse("[]"), current, out, error));
    EXPECT_FALSE(acecode::web::parse_memory_settings_request(
        json::parse(R"({"summary":{"idle_minutes":"30"}})"), current, out, error));
    EXPECT_EQ(out.max_index_bytes, 1u);
}

// 场景:GET /api/config/memory 的响应体。期望:字段齐全,summary_available 原样透出。
TEST(MemoryHandlerTest, SettingsJsonShape) {
    acecode::MemoryConfig cfg;
    cfg.summary.enabled = true;
    const auto j = acecode::web::memory_settings_json(cfg, false);
    EXPECT_TRUE(j["enabled"].get<bool>());
    EXPECT_EQ(j["max_index_bytes"].get<int>(), 8192);
    EXPECT_TRUE(j["summary"]["enabled"].get<bool>());
    EXPECT_EQ(j["summary"]["idle_minutes"].get<int>(), 30);
    EXPECT_EQ(j["summary"]["max_session_age_days"].get<int>(), 7);
    EXPECT_FALSE(j["summary_available"].get<bool>());
}

// 场景:编辑条目的请求体。期望:description / body 必须是字符串、description 非空,
// type 可选但必须是四类之一。
TEST(MemoryHandlerTest, EntryEditValidation) {
    acecode::web::MemoryEntryEdit edit;
    std::string error;
    EXPECT_TRUE(acecode::web::parse_memory_entry_edit(
        json::parse(R"({"description":"d","body":"b","type":"project"})"), edit, error)) << error;
    EXPECT_EQ(edit.type, acecode::MemoryType::Project);
    EXPECT_FALSE(acecode::web::parse_memory_entry_edit(json::parse(R"({"description":"d"})"), edit, error));
    EXPECT_FALSE(acecode::web::parse_memory_entry_edit(json::parse(R"({"description":"  ","body":"b"})"), edit, error));
    EXPECT_FALSE(acecode::web::parse_memory_entry_edit(
        json::parse(R"({"description":"d","body":"b","type":"notes"})"), edit, error));
}

// 场景:一个没有来源字段的旧条目被列出 / 查看。
// 期望:source 按手写(manual)透出;只有查看单条时才带 body 与 path。
TEST(MemoryHandlerTest, EntryJsonDefaultsLegacySourceToManual) {
    acecode::MemoryEntry entry;
    entry.name = "legacy";
    entry.description = "old";
    entry.body = "body\n";
    const auto summary = acecode::web::memory_entry_json(acecode::MemoryScope::Global, entry, false);
    EXPECT_EQ(summary["source"], "manual");
    EXPECT_EQ(summary["scope"], "global");
    EXPECT_FALSE(summary.contains("body"));
    const auto full = acecode::web::memory_entry_json(acecode::MemoryScope::Workspace, entry, true);
    EXPECT_EQ(full["body"], "body\n");
    EXPECT_TRUE(full.contains("path"));
}

// 场景:没有选工作区与选了工作区时的列表。期望:没选时工作区 available=false;
// 状态字段原样带出。
TEST(MemoryHandlerTest, OverviewListsScopesAndStatus) {
    acecode_test::MemoryTestHome home("memory-handler-overview");
    auto memory = home.service();
    std::string err;
    memory->global().upsert("g", acecode::MemoryType::User, "global", "b\n",
                            acecode::MemoryWriteMode::Upsert, err);
    acecode::MemorySummaryStatus status;
    status.workspace_inbox = 7;
    status.last_error = "plan rejected";

    const auto no_ws = acecode::web::memory_overview_json(*memory, "", status);
    EXPECT_TRUE(no_ws["scopes"]["global"]["available"].get<bool>());
    EXPECT_FALSE(no_ws["scopes"]["workspace"]["available"].get<bool>());
    EXPECT_EQ(no_ws["scopes"]["global"]["entries"].size(), 1u);
    EXPECT_EQ(no_ws["status"]["workspace_inbox"].get<int>(), 7);
    EXPECT_EQ(no_ws["status"]["last_error"], "plan rejected");

    const auto with_ws = acecode::web::memory_overview_json(
        *memory, acecode_test::MemoryTestHome::project_dir(home.workspace_cwd("ws")), status);
    EXPECT_TRUE(with_ws["scopes"]["workspace"]["available"].get<bool>());
    EXPECT_TRUE(with_ws["scopes"]["workspace"]["entries"].empty());
}
