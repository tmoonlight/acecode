// 覆盖 src/host/session_host/memory_scheduler.{hpp,cpp}(openspec unify-memory-system
// 第 8 / 9 组)。用注入的假模型(host.complete)与假时钟(host.now_ms)驱动 tick(),
// 会话是用 SessionManager 真实落盘的 meta / JSONL:
// - 8.1 每轮重读 config.json 的 memory 段;每轮最多提炼 2 个会话
// - 8.2 待提炼筛选:子会话 / headless / 关闭记忆 / 无工作区 / 正在进行 / 未闲置 / 太旧 都排除
// - 8.4 摘要模型:memory.summary.model_name 优先,否则用会话最后使用的模型
// - 8.5 输出不合规作废,同一范围最多 3 次,出现新活动后重新计数
// - 8.6 观察脱敏后写入收件箱,写成功才推进提炼位置;只提炼新增部分
// - 8.7 进行中关闭记忆摘要,结果丢弃
// - 9.1 / 9.3 满 20 条整合、按计划写条目并归档;应用失败整批回滚;租约被占则跳过
// - 9.5 /memory flush 忽略闲置立即提炼并强制整合,返回报告
// - 9.6 状态摘要

#include <gtest/gtest.h>

#include "memory/memory_frontmatter.hpp"
#include "memory/memory_inbox.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_service.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session_host/memory_scheduler.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace {

using acecode_test::MemoryTestHome;

constexpr std::int64_t kMinute = 60LL * 1000;

struct ModelCall {
    std::string model;
    bool extraction = false;
    std::string user;
};

// 从提示词里把「- id: xxx」都摘出来,假模型据此在计划里引用依据。
std::vector<std::string> observation_ids(const std::string& prompt) {
    std::vector<std::string> ids;
    std::size_t pos = 0;
    while ((pos = prompt.find("- id: ", pos)) != std::string::npos) {
        pos += 6;
        const std::size_t end = prompt.find_first_of(" |\n", pos);
        ids.push_back(prompt.substr(pos, end - pos));
    }
    return ids;
}

std::string observations_json(int count, const std::string& scope, const std::string& statement = "s") {
    nlohmann::json j = {{"outcome", "observations"}, {"observations", nlohmann::json::array()}};
    for (int i = 0; i < count; ++i) {
        j["observations"].push_back({{"scope", scope}, {"type", scope == "global" ? "feedback" : "project"},
                                     {"title", "title " + std::to_string(i)}, {"statement", statement}});
    }
    return j.dump();
}

class SchedulerHarness {
public:
    explicit SchedulerHarness(MemoryTestHome& home)
        : cwd(home.workspace_cwd("ws")), project_dir(MemoryTestHome::project_dir(cwd)) {
        acecode::MemoryConfig config;
        config.summary.enabled = true;
        memory = home.service(config);
        now = acecode::memory_now_ms() + 60 * kMinute;  // 默认:会话都已闲置 1 小时
        extraction_reply = R"({"outcome":"noop","observations":[]})";
        plan_reply = R"({"operations":[]})";
    }

    acecode::MemorySchedulerHost host() {
        acecode::MemorySchedulerHost h;
        h.project_dirs = [this] { return std::vector<std::string>{project_dir}; };
        h.session_busy = [this](const std::string& id) { return busy.count(id) > 0; };
        h.app_config = [] { return acecode::AppConfig{}; };
        h.config_path = config_path;
        h.now_ms = [this] { return now; };
        h.complete = [this](const std::string& model, const std::string& system, const std::string& user,
                            std::string& error, bool& overflow) -> std::string {
            const bool extraction = system.find("extract durable memories") != std::string::npos;
            calls.push_back({model, extraction, user});
            if (on_call) on_call(extraction);
            if (!fail_error.empty()) {
                error = fail_error;
                overflow = false;
                return {};
            }
            if (extraction) return extraction_reply;
            if (plan_builder) return plan_builder(observation_ids(user));
            return plan_reply;
        };
        return h;
    }

    // 落盘一个会话(一问一答),返回 id。tweak 在第一条消息前调整会话属性。
    std::string make_session(const std::function<void(acecode::SessionManager&)>& tweak = {},
                             const std::string& surface = "daemon", bool no_workspace = false,
                             const std::string& text = "please remember to use pnpm") {
        acecode::SessionManager sm;
        sm.start_session(cwd, "stub", "stub-model", acecode::SessionStorage::generate_session_id(),
                         "model-m", surface, no_workspace);
        if (tweak) tweak(sm);
        append(sm, text);
        const std::string id = sm.current_session_id();
        sm.finalize();
        return id;
    }

    void continue_session(const std::string& id, const std::string& text) {
        acecode::SessionManager sm;
        sm.start_session(cwd, "stub", "stub-model");
        sm.resume_session(id);
        append(sm, text);
        sm.finalize();
    }

    std::optional<acecode::MemoryExtractionProgress> progress(const std::string& id) {
        return memory->state().extraction(id);
    }

    std::size_t extraction_calls() const {
        std::size_t n = 0;
        for (const auto& call : calls) n += call.extraction ? 1 : 0;
        return n;
    }

    std::string cwd;
    std::string project_dir;
    std::string config_path;
    std::shared_ptr<acecode::MemoryService> memory;
    std::int64_t now = 0;
    std::set<std::string> busy;
    std::vector<ModelCall> calls;
    std::string extraction_reply;
    std::string plan_reply;
    std::string fail_error;
    std::function<std::string(const std::vector<std::string>&)> plan_builder;
    std::function<void(bool)> on_call;

private:
    static void append(acecode::SessionManager& sm, const std::string& text) {
        acecode::ChatMessage user;
        user.role = "user";
        user.content = text;
        sm.on_message(user);
        acecode::ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content = "ok, noted";
        sm.on_message(assistant);
    }
};

} // namespace

// 场景:同一工作区里有普通会话、子会话、headless 会话、关闭了记忆的会话、无工作区会话、
// 正在进行回合的会话,都已闲置。
// 期望:一轮调度只提炼普通会话,其余全部排除(没有进度记录、没有模型调用)。
TEST(MemorySchedulerTest, SelectsOnlyEligibleSessions) {
    MemoryTestHome home("memory-sched-select");
    SchedulerHarness h(home);
    const std::string eligible = h.make_session();
    const std::string child = h.make_session([](acecode::SessionManager& sm) { sm.set_parent_session_id("parent"); });
    const std::string headless = h.make_session({}, "headless");
    const std::string off = h.make_session([](acecode::SessionManager& sm) { sm.set_memory_enabled(false); });
    const std::string no_ws = h.make_session({}, "daemon", /*no_workspace=*/true);
    const std::string busy = h.make_session();
    h.busy.insert(busy);

    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    scheduler.tick();

    EXPECT_TRUE(h.progress(eligible).has_value());
    for (const auto& id : {child, headless, off, no_ws, busy}) {
        EXPECT_FALSE(h.progress(id).has_value()) << id;
    }
    EXPECT_EQ(h.extraction_calls(), 1u);
}

// 场景:会话刚结束 5 分钟(闲置阈值 30 分钟);另一个会话最后活动在 8 天前。
// 期望:5 分钟时不提炼,31 分钟后提炼;8 天前的会话始终不提炼(不回溯旧会话)。
TEST(MemorySchedulerTest, RespectsIdleTimeAndMaxAge) {
    MemoryTestHome home("memory-sched-idle");
    SchedulerHarness h(home);
    const std::string fresh = h.make_session();
    const std::string old = h.make_session();
    auto meta_path = acecode::SessionStorage::meta_path(h.project_dir, old);
    auto meta = acecode::SessionStorage::read_meta(meta_path);
    const std::int64_t real_now = acecode::memory_now_ms();
    const std::time_t eight_days_ago = static_cast<std::time_t>((real_now - 8 * 24 * 60 * kMinute) / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &eight_days_ago);
#else
    gmtime_r(&eight_days_ago, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    meta.updated_at = buf;
    ASSERT_TRUE(acecode::SessionStorage::write_meta(meta_path, meta));

    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    h.now = real_now + 5 * kMinute;
    scheduler.tick();
    EXPECT_FALSE(h.progress(fresh).has_value());
    h.now = real_now + 31 * kMinute;
    scheduler.tick();
    EXPECT_TRUE(h.progress(fresh).has_value());
    EXPECT_FALSE(h.progress(old).has_value());
}

// 场景:开启记忆摘要时有 4 个会话同时满足条件。
// 期望:每轮最多提炼 2 个,第二轮处理剩下 2 个;没有新活动的会话不会被重复提炼。
TEST(MemorySchedulerTest, ProcessesAtMostTwoSessionsPerTick) {
    MemoryTestHome home("memory-sched-limit");
    SchedulerHarness h(home);
    for (int i = 0; i < 4; ++i) h.make_session();
    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    scheduler.tick();
    EXPECT_EQ(h.extraction_calls(), 2u);
    scheduler.tick();
    EXPECT_EQ(h.extraction_calls(), 4u);
    scheduler.tick();
    EXPECT_EQ(h.extraction_calls(), 4u);
}

// 场景:本进程启动时记忆摘要关闭;另一个进程把 config.json 改成开启。
// 期望:下一轮调度按 config.json 修改时间重读 memory 段,开始提炼。
TEST(MemorySchedulerTest, ReloadsSettingsFromConfigFile) {
    MemoryTestHome home("memory-sched-reload");
    SchedulerHarness h(home);
    acecode::MemoryConfig off;
    h.memory->update_config(off);
    h.config_path = acecode::path_to_utf8(home.root() / "config.json");
    h.make_session();
    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    scheduler.tick();
    EXPECT_EQ(h.extraction_calls(), 0u);

    std::ofstream(acecode::path_from_utf8(h.config_path))
        << R"({"memory":{"summary":{"enabled":true,"model_name":""}}})";
    scheduler.tick();
    EXPECT_TRUE(h.memory->config().summary.enabled);
    EXPECT_EQ(h.extraction_calls(), 1u);
}

// 场景:摘要模型保持「当前模型」与指定为已保存的模型 S。
// 期望:前者发给会话最后使用的模型(model-m),后者发给 S。
TEST(MemorySchedulerTest, ModelFollowsSummarySetting) {
    MemoryTestHome home("memory-sched-model");
    SchedulerHarness h(home);
    h.make_session();
    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    scheduler.tick();
    ASSERT_EQ(h.calls.size(), 1u);
    EXPECT_EQ(h.calls[0].model, "model-m");

    auto config = h.memory->config();
    config.summary.model_name = "S";
    h.memory->update_config(config);
    h.make_session();
    scheduler.tick();
    ASSERT_EQ(h.calls.size(), 2u);
    EXPECT_EQ(h.calls[1].model, "S");
}

// 场景:模型输出的观察里含令牌;之后会话继续了新的对话并再次闲置。
// 期望:收件箱文件里令牌变成 [REDACTED];提炼位置推进到最后一条消息;没有新活动时
// 不再调用模型;继续对话后只把新增部分送去提炼。
TEST(MemorySchedulerTest, WritesRedactedObservationsAndOnlySendsNewMessages) {
    MemoryTestHome home("memory-sched-write");
    SchedulerHarness h(home);
    const std::string id = h.make_session({}, "daemon", false, "first topic: keep logs in L:/logs");
    h.extraction_reply = observations_json(2, "workspace", "deploy with token=abc123def");
    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    scheduler.tick();

    const fs::path ws_dir = acecode::workspace_memory_dir(h.project_dir);
    const auto inbox = acecode::list_memory_inbox(ws_dir);
    ASSERT_EQ(inbox.size(), 1u);
    EXPECT_EQ(inbox[0].session_id, id);
    EXPECT_EQ(inbox[0].observations.size(), 2u);
    EXPECT_EQ(inbox[0].observations[0].statement, "deploy with token=[REDACTED]");
    ASSERT_TRUE(h.progress(id).has_value());
    EXPECT_EQ(h.progress(id)->next_message_index, inbox[0].to);

    scheduler.tick();
    EXPECT_EQ(h.extraction_calls(), 1u);

    h.continue_session(id, "second topic: release on fridays");
    h.extraction_reply = R"({"outcome":"noop","observations":[]})";
    h.now = acecode::memory_now_ms() + 60 * kMinute;
    scheduler.tick();
    ASSERT_EQ(h.extraction_calls(), 2u);
    EXPECT_NE(h.calls.back().user.find("second topic"), std::string::npos);
    EXPECT_EQ(h.calls.back().user.find("first topic"), std::string::npos);
}

// 场景:模型连续返回不合规的输出。
// 期望:同一范围最多尝试 3 次,之后不再重试;会话出现新活动后重新开始计数。
TEST(MemorySchedulerTest, InvalidOutputStopsAfterThreeAttempts) {
    MemoryTestHome home("memory-sched-retry");
    SchedulerHarness h(home);
    const std::string id = h.make_session();
    h.extraction_reply = R"({"outcome":"observations","observations":[{"scope":"global","type":"user","title":"t","statement":"s","priority":1}]})";
    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    for (int i = 0; i < 5; ++i) scheduler.tick();
    EXPECT_EQ(h.extraction_calls(), 3u);
    EXPECT_EQ(h.progress(id)->attempts, 3);

    h.continue_session(id, "new activity");
    h.now = acecode::memory_now_ms() + 60 * kMinute;
    scheduler.tick();
    EXPECT_EQ(h.extraction_calls(), 4u);
}

// 场景:提炼请求进行中,用户关闭了记忆摘要。
// 期望:返回的观察被丢弃、不落盘,提炼位置不推进。
TEST(MemorySchedulerTest, TurningOffDuringExtractionDiscardsResult) {
    MemoryTestHome home("memory-sched-discard");
    SchedulerHarness h(home);
    const std::string id = h.make_session();
    h.extraction_reply = observations_json(1, "global");
    h.on_call = [&h](bool) {
        auto config = h.memory->config();
        config.summary.enabled = false;
        h.memory->update_config(config);
    };
    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    scheduler.tick();
    EXPECT_EQ(acecode::count_memory_inbox_observations(acecode::get_memory_dir()), 0u);
    EXPECT_FALSE(h.progress(id).has_value() && h.progress(id)->next_message_index > 0);
}

// 场景:一次提炼产出 20 条工作区观察,达到整合阈值;整合计划 create 一条条目并引用观察。
// 期望:条目写入工作区(source: summary,来源会话为该会话)、出现在 MEMORY.md;本批观察
// 全部移进 archive/<日期>/,收件箱清空;状态里记下最近整合时间。
TEST(MemorySchedulerTest, ConsolidatesAtTwentyObservationsAndArchives) {
    MemoryTestHome home("memory-sched-consolidate");
    SchedulerHarness h(home);
    const std::string id = h.make_session();
    h.extraction_reply = observations_json(20, "workspace");
    h.plan_builder = [](const std::vector<std::string>& ids) {
        nlohmann::json plan = {{"operations", {{{"op", "create"}, {"name", "deploy_rules"}, {"type", "project"},
                                                {"description", "Deploy only from main"},
                                                {"body", "Details about deploys."},
                                                {"evidence", {ids.at(0), ids.at(1), ids.at(2)}}}}}};
        return plan.dump();
    };
    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    scheduler.tick();

    auto workspace = h.memory->workspace(h.project_dir);
    workspace->reload();
    const auto created = workspace->find("deploy_rules");
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->source, "summary");
    EXPECT_EQ(created->source_sessions, std::vector<std::string>{id});
    EXPECT_EQ(acecode::count_memory_inbox_observations(workspace->dir()), 0u);
    EXPECT_TRUE(fs::exists(acecode::memory_archive_dir(workspace->dir())));
    std::ifstream index(workspace->dir() / "MEMORY.md");
    std::string index_text((std::istreambuf_iterator<char>(index)), std::istreambuf_iterator<char>());
    EXPECT_NE(index_text.find("deploy_rules.md"), std::string::npos);
    const auto status = acecode::read_memory_summary_status(*h.memory, h.project_dir);
    EXPECT_GT(status.last_consolidation_ms, 0);
    EXPECT_GT(status.last_extraction_ms, 0);
}

// 场景:整合计划的第二个操作在应用时失败(目标名字位置被一个目录占着,写不进去)。
// 期望:整批回滚 —— 第一个操作已写的条目被撤回、观察留在收件箱;状态里记下错误。
TEST(MemorySchedulerTest, FailedApplyRollsBackWholeBatch) {
    MemoryTestHome home("memory-sched-rollback");
    SchedulerHarness h(home);
    h.make_session();
    h.extraction_reply = observations_json(20, "workspace");
    const fs::path ws_dir = acecode::workspace_memory_dir(h.project_dir);
    // 非空目录占住 blocked.md:临时文件改名覆盖不了它(空目录在 Windows 上会被直接替换)。
    fs::create_directories(ws_dir / "blocked.md");
    std::ofstream(ws_dir / "blocked.md" / "keep.txt") << "occupied";
    h.plan_builder = [](const std::vector<std::string>& ids) {
        nlohmann::json plan = {{"operations", {
            {{"op", "create"}, {"name", "good"}, {"type", "project"}, {"description", "d"}, {"body", "b"},
             {"evidence", {ids.at(0)}}},
            {{"op", "create"}, {"name", "blocked"}, {"type", "project"}, {"description", "d2"}, {"body", "b"},
             {"evidence", {ids.at(1)}}}}}};
        return plan.dump();
    };
    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    scheduler.tick();

    EXPECT_FALSE(fs::exists(ws_dir / "good.md"));
    EXPECT_EQ(acecode::count_memory_inbox_observations(ws_dir), 20u);
    const auto status = acecode::read_memory_summary_status(*h.memory, h.project_dir);
    EXPECT_NE(status.last_error.find("consolidation"), std::string::npos) << status.last_error;
}

// 场景:另一个进程正持有工作区的整合租约(未过期),本进程的收件箱已满 20 条。
// 期望:本进程跳过整合(只有一个进程执行),观察原样留在收件箱。
TEST(MemorySchedulerTest, ConsolidationLeaseHeldElsewhereSkips) {
    MemoryTestHome home("memory-sched-lease");
    SchedulerHarness h(home);
    h.make_session();
    h.extraction_reply = observations_json(20, "workspace");
    const std::string scope_key = acecode::MemoryService::workspace_scope_key(h.project_dir);
    ASSERT_TRUE(h.memory->state().try_acquire_lease(acecode::MemoryLeaseKind::Consolidation, scope_key,
                                                    "other-process", 60 * kMinute, h.now));
    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    scheduler.tick();
    for (const auto& call : h.calls) EXPECT_TRUE(call.extraction);
    EXPECT_EQ(acecode::count_memory_inbox_observations(acecode::workspace_memory_dir(h.project_dir)), 20u);
}

// 场景:记忆摘要开启时,在刚结束一个回合(未闲置)的会话里执行 /memory flush。
// 期望:立即提炼本会话并强制整合(即使只有 1 条观察),报告处理的观察数与改动的条目数。
TEST(MemorySchedulerTest, FlushExtractsImmediatelyAndReports) {
    MemoryTestHome home("memory-sched-flush");
    SchedulerHarness h(home);
    const std::string id = h.make_session();
    h.now = acecode::memory_now_ms() + kMinute;  // 远未到闲置阈值
    h.extraction_reply = observations_json(1, "global");
    h.plan_builder = [](const std::vector<std::string>& ids) {
        nlohmann::json plan = {{"operations", {{{"op", "create"}, {"name", "use_pnpm"}, {"type", "feedback"},
                                                {"description", "Use pnpm"}, {"body", "Always use pnpm."},
                                                {"evidence", {ids.at(0)}}}}}};
        return plan.dump();
    };
    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    const std::string report = scheduler.run_flush(id, h.project_dir);
    EXPECT_NE(report.find("1 new observation(s)"), std::string::npos) << report;
    EXPECT_NE(report.find("1 memory entry changed"), std::string::npos) << report;
    h.memory->global().reload();
    EXPECT_TRUE(h.memory->global().find("use_pnpm").has_value());

    auto config = h.memory->config();
    config.summary.enabled = false;
    h.memory->update_config(config);
    const auto off = scheduler.request_flush(id, h.project_dir);
    EXPECT_FALSE(off.started);
}

// 场景:提炼失败(模型调用报错)后读取状态。
// 期望:状态里有开关、两个作用域的待整合数量与最近错误(带时间)。
TEST(MemorySchedulerTest, StatusReportsInboxCountsAndLastError) {
    MemoryTestHome home("memory-sched-status");
    SchedulerHarness h(home);
    acecode::MemoryObservationFile file;
    file.session_id = "older";
    file.to = 3;
    for (int i = 0; i < 7; ++i) file.observations.push_back({"", acecode::MemoryType::Project, "t", "s"});
    ASSERT_TRUE(acecode::write_memory_observation_file(acecode::workspace_memory_dir(h.project_dir), file));
    h.make_session();
    h.fail_error = "HTTP 500";
    acecode::MemorySummaryScheduler scheduler(h.memory, h.host());
    scheduler.tick();

    const auto status = acecode::read_memory_summary_status(*h.memory, h.project_dir);
    EXPECT_TRUE(status.enabled);
    EXPECT_EQ(status.workspace_inbox, 7u);
    EXPECT_EQ(status.global_inbox, 0u);
    EXPECT_NE(status.last_error.find("HTTP 500"), std::string::npos);
    EXPECT_GT(status.last_error_ms, 0);
}
