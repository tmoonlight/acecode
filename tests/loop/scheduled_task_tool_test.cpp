#include <gtest/gtest.h>

#include "loop/loop_request.hpp"
#include "loop/scheduled_task_tool.hpp"
#include "config/config.hpp"
#include "utils/uuid.hpp"
#include "workspace/workspace_registry.hpp"

#include <filesystem>

namespace {
using namespace acecode;
using namespace acecode::loop;
using nlohmann::json;

class ScheduledTaskToolTest : public testing::Test {
protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("acecode-scheduled-tool-" + generate_uuid());
    AppConfig config;
    std::shared_mutex config_mutex;
    desktop::WorkspaceRegistry workspaces;
    std::unique_ptr<LoopStore> store;
    std::shared_ptr<ScheduledTaskService> service;
    std::int64_t now = 1'800'000'000'000;

    void SetUp() override {
        std::filesystem::create_directories(root);
        store = std::make_unique<LoopStore>(root / "loops.sqlite3");
        ASSERT_TRUE(store->initialize());
        ModelProfile model;
        model.name = "test-model";
        config.saved_models.push_back(model);
        config.default_model_name = model.name;
        service = std::make_shared<ScheduledTaskService>(
            *store, config, config_mutex, workspaces, (root / "projects").string(),
            nullptr, [this] { return now; });
    }

    void TearDown() override {
        service.reset();
        store.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    json arguments() const {
        return {{"name", "Meeting"}, {"prompt", "Remind me about the meeting"},
                {"schedule", {{"kind", "once"}, {"once_at_ms", now + 60'000}}}};
    }

    json response(const std::string& mode) const {
        return {{"answers", json::array({{
            {"question_id", "scheduled-task-permission"}, {"selected", json::array({mode})}}})}};
    }

    ToolContext context(const std::string& mode = "yolo") {
        ToolContext ctx;
        ctx.ask_user_questions = [this, mode](const json& questions) {
            EXPECT_TRUE(store->list_loops().empty());
            EXPECT_EQ(questions[0]["options"][0]["value"], "yolo");
            EXPECT_TRUE(questions[0]["options"][0]["recommended"].get<bool>());
            return response(mode);
        };
        return ctx;
    }
};

TEST_F(ScheduledTaskToolTest, UserSelectionOverridesModelArgumentsAndPersistsExistingDefinition) {
    auto args = arguments();
    args["permission_mode"] = "yolo";
    const auto result = create_scheduled_task_tool(service).execute(args.dump(), context("default"));
    ASSERT_TRUE(result.success) << result.output;
    const auto data = json::parse(result.output);
    ASSERT_TRUE(data["created"].get<bool>());
    const auto saved = store->get_loop(data["task"]["id"]);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->permission_mode, "default");
    EXPECT_EQ(saved->model_name, "test-model");
    EXPECT_EQ(saved->next_run_at_ms, now + 60'000);
    EXPECT_FALSE(saved->use_worktree);
    EXPECT_TRUE(saved->workspace_cwd.empty());
    EXPECT_TRUE(store->list_runs(saved->id).empty());
}

TEST_F(ScheduledTaskToolTest, CancellationInterjectionTimeoutAndMissingAnswersNeverCreate) {
    auto tool = create_scheduled_task_tool(service);
    auto ctx = context();
    std::vector<json> replies = {{{"cancelled", true}}, {{"interjected", true}},
        {{"timed_out", true}}, json::object(), response("unknown")};
    auto automatic = response("yolo");
    automatic["answers"][0]["auto_selected"] = true;
    replies.push_back(automatic);
    for (const auto& reply : replies) {
        ctx.ask_user_questions = [reply](const json&) { return reply; };
        EXPECT_FALSE(tool.execute(arguments().dump(), ctx).success) << reply;
        EXPECT_TRUE(store->list_loops().empty());
    }
    EXPECT_FALSE(tool.execute(arguments().dump(), {}).success);
}

TEST_F(ScheduledTaskToolTest, ValidatesBeforeAskingAndRechecksAfterUserAnswers) {
    auto args = arguments();
    args["model_name"] = "missing";
    int questions = 0;
    auto ctx = context();
    ctx.ask_user_questions = [this, &questions](const json&) {
        ++questions;
        config.saved_models.clear();
        return response("yolo");
    };
    auto tool = create_scheduled_task_tool(service);
    EXPECT_FALSE(tool.execute(args.dump(), ctx).success);
    EXPECT_EQ(questions, 0);
    EXPECT_FALSE(tool.execute(arguments().dump(), ctx).success);
    EXPECT_EQ(questions, 1);
    EXPECT_TRUE(store->list_loops().empty());
}

TEST_F(ScheduledTaskToolTest, SharedValidationAndConflictSemanticsMatchManualCreation) {
    const auto workspace = workspaces.register_new((root / "projects").string(), root.string());
    auto args = arguments();
    args["workspace_hash"] = workspace.hash;
    args["workspace_cwd"] = workspace.cwd;
    args["model_name"] = "test-model";
    LoopDefinition manual;
    ValidationError error;
    ASSERT_TRUE(parse_loop_request(args, now, {"test-model"},
        [&workspace](const std::string&) { return std::optional<std::string>(workspace.cwd); }, manual, error));
    ASSERT_TRUE(store->create_loop(manual, now));
    auto ctx = context();
    ctx.ask_user_questions = [this](const json&) { return response("yolo"); };
    auto tool = create_scheduled_task_tool(service);
    const auto result = tool.execute(args.dump(), ctx);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(json::parse(result.output)["error"], "SCHEDULE_CONFLICT");
    EXPECT_EQ(store->list_loops().size(), 1u);
    args["workspace_cwd"] = "wrong-directory";
    EXPECT_EQ(json::parse(tool.execute(args.dump(), ctx).output)["error"], "INVALID_WORKSPACE");
}

TEST_F(ScheduledTaskToolTest, ExpiredTimeAbortAndServiceShutdownDoNotCreate) {
    auto tool = create_scheduled_task_tool(service);
    auto args = arguments();
    args["schedule"]["once_at_ms"] = now - 1;
    EXPECT_FALSE(tool.execute(args.dump(), context()).success);
    std::atomic<bool> abort{true};
    auto ctx = context();
    ctx.abort_flag = &abort;
    EXPECT_FALSE(tool.execute(arguments().dump(), ctx).success);
    abort.store(false);
    ctx.ask_user_questions = [this](const json&) {
        auto reply = response("yolo");
        service.reset();
        return reply;
    };
    EXPECT_FALSE(tool.execute(arguments().dump(), ctx).success);
    EXPECT_TRUE(store->list_loops().empty());
}

TEST_F(ScheduledTaskToolTest, IntervalDefaultAndInclusiveDateRangeUseExistingSchedule) {
    auto args = arguments();
    args["schedule"] = {{"kind", "interval"}, {"interval_value", 1}, {"interval_unit", "days"},
                        {"valid_from_ms", now + kLoopDayMs}, {"valid_until_ms", now + 3 * kLoopDayMs}};
    const auto result = create_scheduled_task_tool(service).execute(args.dump(), context());
    ASSERT_TRUE(result.success) << result.output;
    const auto saved = store->list_loops().front();
    EXPECT_EQ(saved.schedule.anchor_ms, now);
    auto occurrence = saved.next_run_at_ms;
    for (int day = 1; day <= 3; ++day) {
        ASSERT_TRUE(occurrence);
        EXPECT_EQ(*occurrence, now + day * kLoopDayMs);
        occurrence = next_occurrence_ms(saved.schedule, *occurrence);
    }
    EXPECT_FALSE(occurrence);
}

// 触发场景:用户说「建立定时任务，每天帮我生成代码」,模型把原话整句写进 prompt。
// bug 表现:到点执行时会话把「建立定时任务」当成新请求,又建出一个子定时任务。
// 期望:AI 创建路径在弹权限提问之前就拒绝(错误码 PROMPT_CONTAINS_CREATION_REQUEST,
// 提示改写为单次要做的事),不弹窗、不落库。
TEST_F(ScheduledTaskToolTest, PromptCarryingCreationRequestIsRejectedBeforeAsking) {
    int questions = 0;
    auto ctx = context();
    ctx.ask_user_questions = [this, &questions](const json&) {
        ++questions;
        return response("yolo");
    };
    auto tool = create_scheduled_task_tool(service);
    for (const char* prompt : {"建立定时任务，每天帮我生成代码",
                               "/scheduled-task 每天提醒我喝水",
                               "Create a scheduled task that backs up the repo"}) {
        auto args = arguments();
        args["prompt"] = prompt;
        const auto result = tool.execute(args.dump(), ctx);
        ASSERT_FALSE(result.success) << prompt;
        const auto data = json::parse(result.output);
        EXPECT_EQ(data["error"], "PROMPT_CONTAINS_CREATION_REQUEST") << prompt;
        EXPECT_NE(data["message"].get<std::string>().find("只写单次要做的事"), std::string::npos);
    }
    EXPECT_EQ(questions, 0);
    EXPECT_TRUE(store->list_loops().empty());

    // 改写成单次执行内容后照常走权限提问并创建。
    auto args = arguments();
    args["prompt"] = "生成代码：每天的数据导出脚本";
    ASSERT_TRUE(tool.execute(args.dump(), ctx).success);
    EXPECT_EQ(questions, 1);
    EXPECT_EQ(store->list_loops().size(), 1u);
}

// 触发场景:判定「prompt 是否仍带着创建定时任务的请求」。
// 期望:动词(+量词)紧挨「定时任务」等名词、斜杠技能标记、英文 create a scheduled
// task 一律命中;提到定时任务但不是要新建的监控 / 汇报 / 提醒类内容不能误伤 ——
// 例如「检查昨天新增的定时任务」中间隔着「的」,「假设定时任务失败」没有建任务的动词。
TEST(ScheduledTaskPromptCheck, FlagsCreationRequestsButKeepsOrdinaryTaskText) {
    for (const char* prompt : {"建立定时任务，每天帮我生成代码", "帮我设置一个定时任务",
                               "请 创建 定时提醒：早上9点起床", "新建一条计划任务",
                               "加个定时任务提醒我开会", "$scheduled-task 每周汇总",
                               "call create_scheduled_task again", "Set up a cron job for backups",
                               "Schedule another scheduled task"}) {
        EXPECT_TRUE(prompt_requests_scheduled_task_creation(prompt)) << prompt;
    }
    for (const char* prompt : {"生成代码：每天的数据导出脚本", "提醒用户：参加会议",
                               "检查昨天新增的定时任务运行情况并汇报", "假设定时任务失败就通知我",
                               "汇总定时任务页面上的失败记录", "Summarize the scheduled task runs",
                               "Remind me about the meeting"}) {
        EXPECT_FALSE(prompt_requests_scheduled_task_creation(prompt)) << prompt;
    }
}

// 触发场景:create_scheduled_task 是按技能延迟开放的工具。
// 期望:工具声明了技能索引用的触发说明(含中文触发词),并声明在定时任务执行
// 会话中隐藏(拒绝文案要求「只做一次底层工作、不再建任务」)。
TEST(ScheduledTaskPromptCheck, ToolDeclaresActivationHintAndLoopExecutionRefusal) {
    const auto tool = create_scheduled_task_tool({});
    EXPECT_EQ(tool.activation_skill, "scheduled-task");
    EXPECT_NE(tool.activation_hint.find("建立定时任务"), std::string::npos);
    EXPECT_NE(tool.loop_execution_refusal.find("do not schedule anything"), std::string::npos);
}
} // namespace
