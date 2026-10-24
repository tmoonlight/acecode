#include <gtest/gtest.h>
#include "session_host/home_intent_prompts.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "test_support/memory/memory_test_home.hpp"

#include <filesystem>

using namespace acecode;
using nlohmann::json;
namespace {
GlobalSessionCatalogEntry source(int i) {
    GlobalSessionCatalogEntry entry;
    entry.meta.id = "session_" + std::to_string(i);
    entry.meta.last_user_message_at = "2026-10-24T10:0" + std::to_string(i) + ":00Z";
    entry.meta.model_preset = "last";
    entry.meta.model_activated_at_ms = i;
    entry.workspace_hash = "ws_" + std::to_string(i);
    entry.workspace_cwd = "N:/project_" + std::to_string(i);
    entry.workspace_name = "Project " + std::to_string(i);
    return entry;
}
json draft(int index = 0) {
    return {{"category", "execution"}, {"title", "Continue implementation"},
            {"description", "Verify the remaining changes"}, {"prompt", "Implement the next step and run tests."},
            {"source_index", index}};
}
AppConfig config() {
    AppConfig result;
    for (const auto* name : {"summary", "last", "default"}) {
        ModelProfile profile;
        profile.name = name; profile.provider = "openai"; profile.model = name;
        result.saved_models.push_back(profile);
    }
    result.default_model_name = "default";
    return result;
}
class HomeIntentFixture : public testing::Test {
protected:
    acecode_test::MemoryTestHome home{"home_intents"};
};
}

TEST(HomeIntentPrompts, FiltersBusyArchivedChildrenAndEmptyBeforeTakingFive) {
    GlobalSessionCatalog catalog;
    for (int i = 0; i < 9; ++i) catalog.entries.push_back(source(i));
    catalog.entries[8].active = SessionInfo{};
    catalog.entries[8].active->busy = true;
    catalog.entries[7].meta.archived = true;
    catalog.entries[6].meta.parent_session_id = "parent";
    catalog.entries[0].meta.last_user_message_at.clear();
    catalog.entries[1].meta.updated_at = "2099-01-01T00:00:00Z";
    const auto selected = select_home_intent_sessions(catalog);
    ASSERT_EQ(selected.size(), 5u);
    EXPECT_EQ(selected.front().meta.id, "session_5");
    EXPECT_EQ(selected.back().meta.id, "session_1");
}

TEST(HomeIntentPrompts, SummarySelectionWinsEvenWhenAutoTitleDisabled) {
    auto cfg = config();
    cfg.summary_generation.enabled = false;
    cfg.summary_generation.model_name = "summary";
    GlobalSessionCatalog catalog; catalog.entries.push_back(source(1));
    ASSERT_TRUE(resolve_home_intent_model(cfg, catalog));
    EXPECT_EQ(resolve_home_intent_model(cfg, catalog)->name, "summary");
    cfg.summary_generation.model_name = "deleted";
    EXPECT_FALSE(resolve_home_intent_model(cfg, catalog));
}

TEST(HomeIntentPrompts, FallbackUsesActualActivationNotDefaultOrRenameOrBusyFilter) {
    auto cfg = config();
    GlobalSessionCatalog catalog;
    auto older = source(1); older.meta.model_preset = "summary";
    older.meta.updated_at = "2099-01-01T00:00:00Z";
    auto recent = source(2); recent.active = SessionInfo{};
    recent.active->busy = true; recent.active->model_name = "last";
    recent.active->model_activated_at_ms = 10;
    catalog.entries = {older, recent};
    ASSERT_TRUE(resolve_home_intent_model(cfg, catalog));
    EXPECT_EQ(resolve_home_intent_model(cfg, catalog)->name, "last");
    EXPECT_FALSE(resolve_home_intent_model(cfg, {}));
}

TEST(HomeIntentPrompts, RejectsInvalidCategorySourceCountAndShape) {
    const auto sources = json::array({home_intent_source(source(1))});
    auto row = draft();
    for (const auto& category : {"writing", "execution", "exploration", "thinking"}) {
        row["category"] = category;
        EXPECT_TRUE(parse_home_intent_prompts(json{{"suggestions", json::array({row})}}.dump(), sources));
    }
    row["category"] = "invalid";
    EXPECT_FALSE(parse_home_intent_prompts(json{{"suggestions", json::array({row})}}.dump(), sources));
    EXPECT_FALSE(parse_home_intent_prompts(json{{"suggestions", json::array({draft(-1)})}}.dump(), sources));
    EXPECT_FALSE(parse_home_intent_prompts(json{{"suggestions", json::array({draft(1)})}}.dump(), sources));
    EXPECT_FALSE(parse_home_intent_prompts(json{{"suggestions", json::array({draft(), draft(), draft(), draft(), draft()})}}.dump(), sources));
    EXPECT_FALSE(parse_home_intent_prompts("not json", sources));
    EXPECT_TRUE(parse_home_intent_prompts("{\"suggestions\":[]}", sources));
}

TEST(HomeIntentPrompts, WorkspaceComesFromSourceAndNoWorkspaceNeverLeaksCwd) {
    auto a = source(1); auto b = source(2); b.meta.no_workspace = true;
    b.meta.cwd = "N:/cache/private";
    const auto sources = json::array({home_intent_source(a), home_intent_source(b)});
    auto row = draft(); row["workspace"] = {{"cwd", "N:/invented"}};
    const auto result = parse_home_intent_prompts(json{{"suggestions", json::array({row})}}.dump(), sources);
    ASSERT_TRUE(result); EXPECT_EQ((*result)[0]["workspace"]["cwd"], a.workspace_cwd);
    const auto no_workspace = parse_home_intent_prompts(json{{"suggestions", json::array({draft(1)})}}.dump(), sources);
    ASSERT_TRUE(no_workspace);
    EXPECT_EQ((*no_workspace)[0]["workspace"], (json{{"hash", ""}, {"cwd", ""}, {"name", ""}, {"noWorkspace", true}}));
}

TEST(HomeIntentPrompts, CacheDeduplicatesKeepsFailureAndIgnoresShutdownResults) {
    HomeIntentPromptCache cache;
    const auto first = cache.begin(false, 100);
    ASSERT_TRUE(first);
    EXPECT_FALSE(cache.begin(true, 101));
    cache.finish(first, json::array({draft()}), "last", "", 110);
    EXPECT_FALSE(cache.begin(false, 100 + kHomeIntentRefreshMs - 1));
    const auto retry = cache.begin(true, 111);
    ASSERT_TRUE(retry);
    cache.finish(retry, json::array(), "summary", "INVALID_RESPONSE", 112);
    EXPECT_EQ(cache.snapshot()["suggestions"].size(), 1u);
    EXPECT_EQ(cache.snapshot()["model"], "last");
    const auto running = cache.begin(true, 113);
    ASSERT_TRUE(running); cache.shutdown(); EXPECT_TRUE(running->load());
    cache.finish(running, json::array(), "summary", "", 114);
    EXPECT_EQ(cache.snapshot()["suggestions"].size(), 1u);
    EXPECT_FALSE(cache.begin(true, 115));
}

TEST_F(HomeIntentFixture, SnapshotIncludesVisibleTailButNoToolsOrReasoning) {
    auto entry = source(1);
    entry.project_dir = (home.root() / "source").string();
    std::filesystem::create_directories(entry.project_dir);
    const auto path = SessionStorage::session_path(entry.project_dir, entry.meta.id);
    ChatMessage user; user.role = "user"; user.content = "expanded internal context";
    user.metadata = {{"display_text", "Write a report"}};
    ASSERT_TRUE(SessionStorage::append_message(path, user));
    ChatMessage reply; reply.role = "assistant"; reply.content = "Outline ready"; reply.reasoning_content = "private reasoning";
    ASSERT_TRUE(SessionStorage::append_message(path, reply));
    ChatMessage tool; tool.role = "tool"; tool.content = "private tool output";
    ASSERT_TRUE(SessionStorage::append_message(path, tool));
    auto hidden = user; hidden.metadata = {{"hidden_goal_context", true}};
    ASSERT_TRUE(SessionStorage::append_message(path, hidden));
    const auto snapshot = home_intent_source(entry);
    ASSERT_EQ(snapshot["messages"].size(), 2u);
    EXPECT_EQ(snapshot["messages"][0]["text"], "Write a report");
    EXPECT_EQ(snapshot["messages"][1]["text"], "Outline ready");
    EXPECT_EQ(snapshot.dump().find("private"), std::string::npos);
}

TEST_F(HomeIntentFixture, ModelActivationPersistsAndBrowsingDoesNotRenewIt) {
    SessionManager manager;
    manager.start_session(home.root().string(), "openai", "last", "", "last");
    ChatMessage message; message.role = "user"; message.content = "hello";
    manager.on_message(message);
    const auto id = manager.current_session_id();
    const auto path = SessionStorage::meta_path(manager.current_project_dir(), id);
    const auto first = SessionStorage::read_meta(path).model_activated_at_ms;
    ASSERT_GT(first, 0);
    manager.set_session_title("renamed");
    EXPECT_EQ(SessionStorage::read_meta(path).model_activated_at_ms, first);
    manager.resume_session(id);
    EXPECT_EQ(manager.display_snapshot().model_activated_at_ms, first);
    EXPECT_TRUE(manager.set_active_model_state("openai", "last", "last", std::nullopt, true));
    EXPECT_EQ(SessionStorage::read_meta(path).model_activated_at_ms, first);
    EXPECT_TRUE(manager.set_active_model_state("openai", "summary", "summary", std::nullopt, true));
    EXPECT_GE(SessionStorage::read_meta(path).model_activated_at_ms, first);
    EXPECT_EQ(SessionStorage::read_meta(path).model_preset, "summary");
}
