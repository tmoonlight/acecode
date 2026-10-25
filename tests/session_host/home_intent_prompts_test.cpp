#include <gtest/gtest.h>
#include "session_host/home_intent_prompts.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "skills/skill_usage_store.hpp"
#include "test_support/memory/memory_test_home.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <random>

using namespace acecode;
using nlohmann::json;
namespace {
// Metadata written before last_user_message_at existed: only the transcript knows it.
GlobalSessionCatalogEntry legacy(const std::string& id, const std::string& updated_at) {
    GlobalSessionCatalogEntry entry;
    entry.meta.id = id;
    entry.meta.updated_at = updated_at;
    entry.workspace_hash = "legacy";
    return entry;
}
std::string at_minute(int minute) {
    char text[32];
    std::snprintf(text, sizeof(text), "2026-10-24T%02d:%02d:00Z", minute / 60, minute % 60);
    return text;
}
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

// 场景：数据目录里绝大多数会话是旧格式元数据(没有 last_user_message_at)，只有少数近期会话
// 已持久化活动时间。旧实现在刷新请求里逐个打开全部旧会话的 JSONL 补算，实测 2.8 万个会话
// 在 60 秒超时内读不完，模型请求根本发不出去，首页提示永远停在默认卡片。
// 期望：更新时间(加容差)早于已知前 5 名的旧会话一个都不读；更新时间足够新的旧会话才读，
// 读出的活动时间照常参与排序；读不出用户消息的(只改过名)不入选。
TEST(HomeIntentPrompts, LegacyActivityIsReadOnlyWhileItCanStillRank) {
    GlobalSessionCatalog catalog;
    for (int i = 0; i < 5; ++i) {
        auto entry = source(i);
        entry.meta.updated_at = entry.meta.last_user_message_at;
        catalog.entries.push_back(entry);
    }
    for (int i = 0; i < 1000; ++i) catalog.entries.push_back(legacy("old_" + std::to_string(i), "2026-09-01T00:00:00Z"));
    catalog.entries.push_back(legacy("recent_legacy", "2026-10-24T10:30:00Z"));
    catalog.entries.push_back(legacy("renamed_only", "2026-10-24T11:00:00Z"));
    std::vector<std::string> reads;
    const HomeIntentActivityReader read = [&](const GlobalSessionCatalogEntry& entry) -> std::optional<std::string> {
        reads.push_back(entry.meta.id);
        return entry.meta.id == "recent_legacy" ? "2026-10-24T10:20:00Z" : "";
    };
    const auto selected = select_home_intent_sessions(catalog, read);
    ASSERT_EQ(selected.size(), 5u);
    EXPECT_EQ(selected[0].meta.id, "recent_legacy");
    EXPECT_EQ(selected[0].meta.last_user_message_at, "2026-10-24T10:20:00Z");
    EXPECT_EQ(selected[1].meta.id, "session_4");
    EXPECT_EQ(selected[4].meta.id, "session_1");
    EXPECT_EQ(reads, (std::vector<std::string>{"renamed_only", "recent_legacy"}));
}

// 场景：活动时间与更新时间随机分布，其中一部分会话的最后用户消息比元数据更新时间晚 1 秒
// (真实数据里出现过的取整误差)，还有更新时间缺失的旧会话。
// 期望：按需读取的结果与「先把所有旧会话都补算再排序」逐项相同，且读取次数明显更少。
TEST(HomeIntentPrompts, LazyActivityRankingMatchesFullBackfill) {
    std::mt19937 random(20261025);
    GlobalSessionCatalog catalog;
    std::map<std::string, std::string> transcript;
    for (int i = 0; i < 400; ++i) {
        const int updated = static_cast<int>(random() % 600);
        std::string real;
        const auto kind = random() % 10;
        if (kind == 1) real = at_minute(updated).substr(0, 17) + "01Z"; // 比更新时间晚 1 秒
        else if (kind != 0) real = at_minute((std::max)(0, updated - static_cast<int>(random() % 120)));
        auto entry = legacy("s" + std::to_string(i), random() % 50 == 0 ? std::string{} : at_minute(updated));
        entry.workspace_hash = "ws" + std::to_string(random() % 3);
        transcript[entry.meta.id] = real;
        if (random() % 4 == 0) entry.meta.last_user_message_at = real; // 新格式元数据
        if (random() % 9 == 0) entry.meta.archived = true;
        catalog.entries.push_back(entry);
    }
    std::size_t lazy_reads = 0;
    const HomeIntentActivityReader read = [&](const GlobalSessionCatalogEntry& entry) -> std::optional<std::string> {
        ++lazy_reads;
        return transcript.at(entry.meta.id);
    };
    auto backfilled = catalog;
    for (auto& entry : backfilled.entries) {
        if (entry.meta.last_user_message_at.empty()) entry.meta.last_user_message_at = transcript[entry.meta.id];
    }
    const auto lazy = select_home_intent_sessions(catalog, read);
    const auto full = select_home_intent_sessions(backfilled);
    ASSERT_EQ(lazy.size(), full.size());
    for (std::size_t i = 0; i < full.size(); ++i) {
        EXPECT_EQ(lazy[i].meta.id, full[i].meta.id) << i;
        EXPECT_EQ(lazy[i].meta.last_user_message_at, full[i].meta.last_user_message_at) << i;
    }
    EXPECT_LT(lazy_reads, transcript.size() / 2);
}

// 场景：刷新请求的 60 秒预算用完，读取器开始返回取消。
// 期望：不再读取任何会话，已确定的结果原样返回，由路由按 TIMEOUT 收尾。
TEST(HomeIntentPrompts, CancelledActivityReaderStopsReading) {
    GlobalSessionCatalog catalog;
    for (int i = 0; i < 50; ++i) catalog.entries.push_back(legacy("s" + std::to_string(i), at_minute(i)));
    int reads = 0;
    const HomeIntentActivityReader read = [&](const GlobalSessionCatalogEntry&) -> std::optional<std::string> {
        ++reads;
        return std::nullopt;
    };
    EXPECT_TRUE(select_home_intent_sessions(catalog, read).empty());
    EXPECT_EQ(reads, 1);
}

// 场景：未指定摘要模型，需要按「最近使用的会话模型」兜底，而最近的会话都是旧格式元数据。
// 期望：兜底只读取足以确定顺序的少数会话，选中最近有用户消息的那个会话的模型；
// 没有用户消息的会话(只改名或空会话)不参与兜底。
TEST(HomeIntentPrompts, ModelFallbackReadsLegacyActivityLazily) {
    auto cfg = config();
    GlobalSessionCatalog catalog;
    for (int i = 0; i < 500; ++i) {
        auto entry = legacy("old_" + std::to_string(i), "2026-09-01T00:00:00Z");
        entry.meta.model_preset = "default";
        catalog.entries.push_back(entry);
    }
    auto latest = legacy("latest", "2026-10-24T10:30:00Z"); latest.meta.model_preset = "last";
    auto empty = legacy("empty", "2026-10-24T11:00:00Z"); empty.meta.model_preset = "summary";
    catalog.entries.push_back(latest);
    catalog.entries.push_back(empty);
    int reads = 0;
    const HomeIntentActivityReader read = [&](const GlobalSessionCatalogEntry& entry) -> std::optional<std::string> {
        ++reads;
        return entry.meta.id == "latest" ? "2026-10-24T10:20:00Z" : "";
    };
    const auto profile = resolve_home_intent_model(cfg, catalog, read);
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->name, "last");
    EXPECT_EQ(reads, 2);
}

namespace {
std::int64_t ms(const std::string& iso) { return parse_iso8601_to_epoch_ms(iso); }

GlobalSessionCatalogEntry active_at(const std::string& id, const std::string& activity) {
    auto entry = legacy(id, activity);
    entry.workspace_hash = "ws_" + id;
    entry.meta.last_user_message_at = activity;
    entry.meta.model_preset = "default";
    return entry;
}

// 内存里的项目目录(行已知，相当于命中预热索引)：modified_ms 是目录修改时间，
// open 记录被打开的顺序。
HomeIntentProject project_at(std::int64_t modified_ms, std::vector<GlobalSessionCatalogEntry> rows,
                             std::vector<std::int64_t>& loads) {
    HomeIntentProject project;
    project.modified_ms = modified_ms;
    project.open = [rows, modified_ms, &loads] {
        loads.push_back(modified_ms);
        return HomeIntentProjectContents{rows, {}};
    };
    return project;
}

std::vector<HomeIntentProject> old_projects(int count, std::vector<std::int64_t>& loads) {
    std::vector<HomeIntentProject> projects;
    for (int i = 0; i < count; ++i) {
        projects.push_back(project_at(ms("2026-09-01T00:00:00Z") - i, {active_at("old_" + std::to_string(i), "2026-08-31T23:00:00Z")}, loads));
    }
    return projects;
}
} // namespace

// 场景：冷启动时后台索引还没预热完，首页刷新若重扫全部 2.4 万个项目目录要 30 多秒，
// 和模型调用共用 60 秒预算后经常 TIMEOUT。
// 期望：按目录修改时间从新到旧加载；最新两个目录已凑齐 5 个会话，且它们的活动时间都晚于
// 下一个目录的修改时间(加容差)时立即停止，其余目录一个都不加载。
TEST(HomeIntentPrompts, LoadingStopsOnceNewestProjectsSettleTheSources) {
    auto cfg = config();
    cfg.summary_generation.model_name = "summary";
    std::vector<std::int64_t> loads;
    auto projects = old_projects(100, loads);
    projects.push_back(project_at(ms("2026-10-24T10:58:00Z"),
        {active_at("p1_a", "2026-10-24T10:55:00Z"), active_at("p1_b", "2026-10-24T10:56:00Z")}, loads));
    projects.push_back(project_at(ms("2026-10-24T11:00:00Z"),
        {active_at("p0_a", "2026-10-24T10:50:00Z"), active_at("p0_b", "2026-10-24T10:51:00Z"),
         active_at("p0_c", "2026-10-24T10:52:00Z")}, loads));
    const auto candidates = load_home_intent_candidates(cfg, std::move(projects), {}, {}, 1);
    EXPECT_TRUE(candidates.settled);
    EXPECT_EQ(candidates.loaded_projects, 2u);
    EXPECT_EQ(loads, (std::vector<std::int64_t>{ms("2026-10-24T11:00:00Z"), ms("2026-10-24T10:58:00Z")}));
    const auto selected = select_home_intent_sessions(candidates.catalog);
    ASSERT_EQ(selected.size(), 5u);
    EXPECT_EQ(selected.front().meta.id, "p1_b");
    EXPECT_EQ(selected.back().meta.id, "p0_a");
}

// 场景：某个目录的修改时间读取失败(未知)，它可能装着最新的会话。
// 期望：未知时间的目录最先加载，里面的会话照常参与排序。
TEST(HomeIntentPrompts, ProjectWithUnknownModifiedTimeLoadsFirst) {
    auto cfg = config();
    cfg.summary_generation.model_name = "summary";
    std::vector<std::int64_t> loads;
    auto projects = old_projects(10, loads);
    projects.push_back(project_at(0, {active_at("unknown_time", "2026-10-24T12:00:00Z")}, loads));
    const auto candidates = load_home_intent_candidates(cfg, std::move(projects), {}, {}, 1);
    ASSERT_FALSE(loads.empty());
    EXPECT_EQ(loads.front(), 0);
    const auto selected = select_home_intent_sessions(candidates.catalog);
    ASSERT_FALSE(selected.empty());
    EXPECT_EQ(selected.front().meta.id, "unknown_time");
}

// 场景：一个项目目录里有上千个会话(本机实测 5063 个)，后台索引还没扫到它；把整个目录的
// 元数据都读一遍冷缓存要 20 多秒。
// 期望：先只列出文件名和修改时间，按修改时间从新到旧逐批读取；最新几份已凑齐 5 个来源、
// 剩下文件的修改时间(加容差)更早时立即停止，只读一批(8 个)文件。
TEST(HomeIntentPrompts, LargeProjectReadsOnlyItsNewestMetadataFiles) {
    auto cfg = config();
    cfg.summary_generation.model_name = "summary";
    std::atomic<int> reads{0};
    HomeIntentProject large;
    large.modified_ms = ms("2026-10-24T11:00:00Z");
    large.open = [&reads] {
        HomeIntentProjectContents contents;
        for (int i = 0; i < 1000; ++i) {
            const bool recent = i < 5;
            HomeIntentSessionFile file;
            file.modified_ms = recent ? ms("2026-10-24T10:5" + std::to_string(i) + ":00Z")
                                      : ms("2026-09-01T00:00:00Z") - i;
            const auto id = "file_" + std::to_string(i);
            const auto at = recent ? "2026-10-24T10:4" + std::to_string(i) + ":00Z" : std::string("2026-08-31T00:00:00Z");
            file.read = [&reads, id, at] {
                ++reads;
                return std::optional<GlobalSessionCatalogEntry>(active_at(id, at));
            };
            contents.files.push_back(std::move(file));
        }
        return contents;
    };
    std::vector<std::int64_t> loads;
    std::vector<HomeIntentProject> projects{large};
    const auto candidates = load_home_intent_candidates(cfg, std::move(projects), {}, {}, 8);
    EXPECT_TRUE(candidates.settled);
    EXPECT_EQ(candidates.loaded_projects, 1u);
    EXPECT_EQ(candidates.read_files, 8u);
    EXPECT_EQ(reads.load(), 8);
    const auto selected = select_home_intent_sessions(candidates.catalog);
    ASSERT_EQ(selected.size(), 5u);
    EXPECT_EQ(selected.front().meta.id, "file_4");
    EXPECT_EQ(selected.back().meta.id, "file_0");
}

// 场景：没有指定摘要模型，兜底取「最近切换过的模型」；最新目录里的会话都没切换过模型，
// 切换记录在更早的目录里。
// 期望：来源会话凑齐后仍继续加载，直到最新的切换时间晚于下一个目录的修改时间；
// 选中的是那次切换的模型，而不是按活动时间兜底的默认模型。
TEST(HomeIntentPrompts, FallbackModelKeepsLoadingUntilActivationIsSettled) {
    auto cfg = config();
    std::vector<std::int64_t> loads;
    auto projects = old_projects(50, loads);
    std::vector<GlobalSessionCatalogEntry> newest;
    for (int i = 0; i < 5; ++i) newest.push_back(active_at("new_" + std::to_string(i), "2026-10-24T10:5" + std::to_string(i) + ":00Z"));
    projects.push_back(project_at(ms("2026-10-24T11:00:00Z"), newest, loads));
    auto switched = legacy("switched", "2026-10-23T11:00:00Z");
    switched.workspace_hash = "ws_switched";
    switched.meta.model_preset = "last";
    switched.meta.model_activated_at_ms = ms("2026-10-23T11:00:00Z");
    projects.push_back(project_at(ms("2026-10-23T12:00:00Z"), {switched}, loads));
    auto older_switch = legacy("older_switch", "2026-08-01T00:00:00Z");
    older_switch.meta.model_preset = "summary";
    older_switch.meta.model_activated_at_ms = ms("2026-08-01T00:00:00Z");
    projects.push_back(project_at(ms("2026-08-01T00:00:00Z"), {older_switch}, loads));
    const auto candidates = load_home_intent_candidates(cfg, std::move(projects), {}, {}, 1);
    EXPECT_TRUE(candidates.settled);
    EXPECT_EQ(candidates.loaded_projects, 2u);
    const auto profile = resolve_home_intent_model(cfg, candidates.catalog);
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->name, "last");
}

// 场景：找会话的 30 秒预算在加载途中用完。
// 期望：不再加载新的目录，结果标记为未确定(路由据此记日志，没有来源时报 TIMEOUT)。
TEST(HomeIntentPrompts, StoppedLoadingIsReportedUnsettled) {
    auto cfg = config();
    cfg.summary_generation.model_name = "summary";
    std::vector<std::int64_t> loads;
    auto projects = old_projects(20, loads);
    bool expired = false;
    projects[0].open = [&loads, &expired] {
        loads.push_back(-1);
        expired = true;
        return HomeIntentProjectContents{};
    };
    const auto candidates = load_home_intent_candidates(
        cfg, std::move(projects), {}, [&expired] { return expired; }, 1);
    EXPECT_FALSE(candidates.settled);
    EXPECT_EQ(candidates.loaded_projects, 1u);
    EXPECT_EQ(loads.size(), 1u);
}

// 场景：同一个会话在两个项目目录里都有元数据(旧数据迁移留下的副本)。
// 期望：只保留更新时间较新的那份，不会作为两个来源重复发给模型。
TEST(HomeIntentPrompts, DuplicateSessionAcrossProjectsKeepsNewerCopy) {
    auto cfg = config();
    cfg.summary_generation.model_name = "summary";
    std::vector<std::int64_t> loads;
    auto stale = active_at("dup", "2026-10-24T09:00:00Z");
    auto fresh = active_at("dup", "2026-10-24T10:00:00Z");
    fresh.meta.title = "fresh";
    const auto candidates = load_home_intent_candidates(cfg,
        {project_at(ms("2026-10-24T09:30:00Z"), {stale}, loads), project_at(ms("2026-10-24T10:30:00Z"), {fresh}, loads)},
        {}, {}, 1);
    ASSERT_EQ(candidates.catalog.entries.size(), 1u);
    EXPECT_EQ(candidates.catalog.entries[0].meta.title, "fresh");
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
    const auto first = cache.begin(false, 100, 0);
    ASSERT_TRUE(first);
    EXPECT_FALSE(cache.begin(true, 101, 0));
    cache.finish(first, json::array({draft()}), "last", "", 110);
    EXPECT_FALSE(cache.begin(false, 100 + kHomeIntentRefreshMs - 1, 0));
    const auto retry = cache.begin(true, 111, 0);
    ASSERT_TRUE(retry);
    cache.finish(retry, json::array(), "summary", "INVALID_RESPONSE", 112);
    EXPECT_EQ(cache.snapshot(0)["suggestions"].size(), 1u);
    EXPECT_EQ(cache.snapshot(0)["model"], "last");
    EXPECT_EQ(cache.snapshot(0)["generated"], true);
    const auto running = cache.begin(true, 113, 0);
    ASSERT_TRUE(running); cache.shutdown(); EXPECT_TRUE(running->load());
    cache.finish(running, json::array(), "summary", "", 114);
    EXPECT_EQ(cache.snapshot(0)["suggestions"].size(), 1u);
    EXPECT_FALSE(cache.begin(true, 115, 0));
}

// Empty suggestions are still a successful generation. Idle timers and other
// windows must neither launch inference nor spin on an expired deadline.
TEST(HomeIntentPrompts, GeneratedActivitySkipsIdleCyclesUntilNewMessage) {
    HomeIntentPromptCache cache;
    EXPECT_EQ(cache.snapshot(7)["generated"], false);
    const auto first = cache.begin(false, 100, 7);
    ASSERT_TRUE(first);
    cache.finish(first, json::array(), "summary", "", 110);
    for (int cycle = 1; cycle <= 5; ++cycle) {
        const auto now = 100 + cycle * kHomeIntentRefreshMs;
        EXPECT_FALSE(cache.begin(false, now, 7));
        EXPECT_EQ(cache.snapshot(7)["generated"], true);
        EXPECT_EQ(cache.snapshot(7)["next_refresh_at_ms"], now + kHomeIntentRefreshMs);
        EXPECT_FALSE(cache.begin(false, now, 7)); // A second window.
    }
    EXPECT_EQ(cache.snapshot(8)["generated"], false);
    EXPECT_FALSE(cache.begin(false, 100 + 6 * kHomeIntentRefreshMs - 1, 8));
    const auto next = cache.begin(false, 100 + 6 * kHomeIntentRefreshMs, 8);
    ASSERT_TRUE(next);
    cache.finish(next, json::array({draft()}), "summary", "", 111 + 6 * kHomeIntentRefreshMs);
    EXPECT_EQ(cache.snapshot(8)["generated"], true);
}

TEST(HomeIntentPrompts, ActivityDuringGenerationRemainsPendingAndFailureDoesNotCoverIt) {
    HomeIntentPromptCache cache;
    const auto first = cache.begin(false, 100, 1);
    ASSERT_TRUE(first);
    EXPECT_FALSE(cache.begin(true, 105, 2));
    cache.finish(first, json::array({draft()}), "summary", "", 110);
    EXPECT_EQ(cache.snapshot(2)["generated"], false);
    const auto retry = cache.begin(false, 100 + kHomeIntentRefreshMs, 2);
    ASSERT_TRUE(retry);
    cache.finish(retry, json::array(), "summary", "INVALID_RESPONSE", 110 + kHomeIntentRefreshMs);
    EXPECT_EQ(cache.snapshot(2)["generated"], false);
    EXPECT_EQ(cache.snapshot(2)["suggestions"].size(), 1u);
    EXPECT_TRUE(cache.begin(false, 100 + 2 * kHomeIntentRefreshMs, 2));
}

TEST(HomeIntentPrompts, ManualRefreshBypassesGeneratedStateAndCancelledResultDoesNotCoverActivity) {
    HomeIntentPromptCache cache;
    const auto first = cache.begin(false, 100, 0);
    ASSERT_TRUE(first);
    cache.finish(first, json::array({draft()}), "summary", "", 110);
    const auto forced = cache.begin(true, 111, 0);
    ASSERT_TRUE(forced);
    cache.finish(forced, json::array(), "summary", "", 112);
    EXPECT_EQ(cache.snapshot(0)["generated"], true);
    const auto cancelled = cache.begin(true, 113, 1);
    ASSERT_TRUE(cancelled);
    cancelled->store(true);
    cache.finish(cancelled, json::array({draft()}), "summary", "", 114);
    EXPECT_EQ(cache.snapshot(1)["generated"], false);
    EXPECT_TRUE(cache.snapshot(1)["suggestions"].empty());
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
