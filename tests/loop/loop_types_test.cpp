#include <gtest/gtest.h>

#include "loop/loop_types.hpp"

using namespace acecode::loop;

namespace {

LoopDefinition valid_loop() {
    LoopDefinition value;
    value.name = " 每日巡检 ";
    value.prompt = " 检查当前代码并报告问题。 ";
    value.model_name = "default-model";
    value.permission_mode = "yolo";
    value.schedule.kind = ScheduleKind::Period;
    value.schedule.period = PeriodKind::Workdays;
    value.schedule.hour = 9;
    value.schedule.minute = 30;
    return value;
}

} // namespace

TEST(LoopTypes, ParsesAndNormalizesDefinition) {
    nlohmann::json body{
        {"name", " 每日代码健康巡检 "},
        {"prompt", " 检查仓库状态。 "},
        {"model_name", "model-a"},
        {"permission_mode", "yolo"},
        {"workspace_hash", ""},
        {"workspace_cwd", ""},
        {"use_worktree", true},
        {"schedule", {
            {"kind", "period"},
            {"period", "weekly"},
            {"weekdays", {5, 1, 1}},
            {"hour", 9},
            {"minute", 0},
            {"timezone_offset_minutes", 480},
        }},
    };
    LoopDefinition parsed;
    ValidationError error;
    ASSERT_TRUE(loop_from_json(body, parsed, &error)) << error.message;
    EXPECT_EQ(parsed.name, "每日代码健康巡检");
    EXPECT_EQ(parsed.prompt, "检查仓库状态。");
    EXPECT_TRUE(parsed.use_worktree);
    EXPECT_EQ(parsed.schedule.weekdays, (std::vector<int>{1, 5}));
    EXPECT_EQ(parsed.schedule.timezone_offset_minutes, 480);

    auto json = loop_to_json(parsed);
    EXPECT_TRUE(json["use_worktree"].get<bool>());
    EXPECT_EQ(json["schedule"]["kind"], "period");
    EXPECT_FALSE(json.contains("schedule_expr"));
}

TEST(LoopTypes, WorktreeDefaultsOffAndRequiresBoolean) {
    auto body = loop_to_json(valid_loop());
    EXPECT_FALSE(body["use_worktree"].get<bool>());

    body["use_worktree"] = "yes";
    LoopDefinition parsed;
    ValidationError error;
    EXPECT_FALSE(loop_from_json(body, parsed, &error));
    EXPECT_EQ(error.code, "INVALID_USE_WORKTREE");
    EXPECT_EQ(error.field, "use_worktree");
}

TEST(LoopTypes, RejectsMissingPromptAndInvalidPermission) {
    auto value = valid_loop();
    value.prompt = "  ";
    normalize_loop_definition(value);
    ValidationError error;
    EXPECT_FALSE(validate_loop_definition(value, &error));
    EXPECT_EQ(error.code, "MISSING_PROMPT");

    value = valid_loop();
    value.permission_mode = "accept-edits";
    EXPECT_FALSE(validate_loop_definition(value, &error));
    EXPECT_EQ(error.code, "INVALID_PERMISSION_MODE");
}

TEST(LoopTypes, RequiresCompleteWorkspacePair) {
    auto value = valid_loop();
    value.workspace_hash = "hash";
    ValidationError error;
    EXPECT_FALSE(validate_loop_definition(value, &error));
    EXPECT_EQ(error.code, "INVALID_WORKSPACE");
}

TEST(LoopTypes, ValidatesScheduleVariants) {
    ValidationError error;
    LoopSchedule weekly;
    weekly.kind = ScheduleKind::Period;
    weekly.period = PeriodKind::Weekly;
    EXPECT_FALSE(validate_loop_schedule(weekly, &error));
    EXPECT_EQ(error.code, "MISSING_WEEKDAY");

    LoopSchedule interval;
    interval.kind = ScheduleKind::Interval;
    interval.interval_value = 90;
    interval.interval_unit = IntervalUnit::Minutes;
    interval.anchor_ms = 1000;
    EXPECT_TRUE(validate_loop_schedule(interval, &error));

    LoopSchedule once;
    once.kind = ScheduleKind::Once;
    once.once_at_ms = 2000;
    once.valid_from_ms = 3000;
    once.valid_until_ms = 1000;
    EXPECT_FALSE(validate_loop_schedule(once, &error));
    EXPECT_EQ(error.code, "INVALID_VALIDITY_WINDOW");
}

TEST(LoopTypes, RunJsonUsesCanonicalStatus) {
    LoopRun run;
    run.id = "run-1";
    run.loop_id = "loop-1";
    run.status = RunStatus::WaitingUser;
    run.scheduled_at_ms = 42;
    auto json = run_to_json(run);
    EXPECT_EQ(json["status"], "waiting_user");
    EXPECT_TRUE(json["started_at_ms"].is_null());
    EXPECT_EQ(parse_run_status("missed"), RunStatus::Missed);
}

// 场景:定时任务的旧通知清理三个字段(自动归档 / 自动删除 / 保留个数)。
// 期望:不传时默认两个开关都关、保留 3 个;传入后原样解析并在 loop_to_json 里
// 回显(AI 创建路径会 loop_to_json → prepare 再校验一遍,丢字段就等于没设)。
TEST(LoopTypes, SessionCleanupFieldsDefaultOffAndRoundTrip) {
    auto body = loop_to_json(valid_loop());
    EXPECT_FALSE(body["auto_archive_old_sessions"].get<bool>());
    EXPECT_FALSE(body["auto_delete_old_sessions"].get<bool>());
    EXPECT_EQ(body["keep_recent_sessions"].get<int>(), 3);

    body.erase("auto_archive_old_sessions");
    body.erase("auto_delete_old_sessions");
    body.erase("keep_recent_sessions");
    LoopDefinition parsed;
    ValidationError error;
    ASSERT_TRUE(loop_from_json(body, parsed, &error)) << error.message;
    EXPECT_FALSE(parsed.auto_archive_old_sessions);
    EXPECT_FALSE(parsed.auto_delete_old_sessions);
    EXPECT_EQ(parsed.keep_recent_sessions, 3);

    body["auto_archive_old_sessions"] = true;
    body["keep_recent_sessions"] = 7;
    ASSERT_TRUE(loop_from_json(body, parsed, &error)) << error.message;
    EXPECT_TRUE(parsed.auto_archive_old_sessions);
    EXPECT_FALSE(parsed.auto_delete_old_sessions);
    EXPECT_EQ(parsed.keep_recent_sessions, 7);
    const auto echoed = loop_to_json(parsed);
    EXPECT_TRUE(echoed["auto_archive_old_sessions"].get<bool>());
    EXPECT_EQ(echoed["keep_recent_sessions"].get<int>(), 7);
}

// 场景:两个开关同时为 true、保留个数为 0 / 101 / 小数、开关不是布尔。
// 期望:分别报 CONFLICTING_SESSION_CLEANUP / INVALID_KEEP_RECENT_SESSIONS /
// INVALID_SESSION_CLEANUP,field 指向出错字段;100 是上限本身,可以通过。
TEST(LoopTypes, SessionCleanupRejectsConflictsAndOutOfRangeCounts) {
    auto body = loop_to_json(valid_loop());
    LoopDefinition parsed;
    ValidationError error;

    auto both = body;
    both["auto_archive_old_sessions"] = true;
    both["auto_delete_old_sessions"] = true;
    EXPECT_FALSE(loop_from_json(both, parsed, &error));
    EXPECT_EQ(error.code, "CONFLICTING_SESSION_CLEANUP");
    EXPECT_EQ(error.field, "auto_delete_old_sessions");

    for (const int keep : {0, 101}) {
        auto out_of_range = body;
        out_of_range["keep_recent_sessions"] = keep;
        error = {};
        EXPECT_FALSE(loop_from_json(out_of_range, parsed, &error)) << keep;
        EXPECT_EQ(error.code, "INVALID_KEEP_RECENT_SESSIONS") << keep;
    }
    auto fractional = body;
    fractional["keep_recent_sessions"] = 2.5;
    EXPECT_FALSE(loop_from_json(fractional, parsed, &error));
    EXPECT_EQ(error.code, "INVALID_KEEP_RECENT_SESSIONS");

    auto not_bool = body;
    not_bool["auto_delete_old_sessions"] = "yes";
    EXPECT_FALSE(loop_from_json(not_bool, parsed, &error));
    EXPECT_EQ(error.code, "INVALID_SESSION_CLEANUP");
    EXPECT_EQ(error.field, "auto_delete_old_sessions");

    auto max_keep = body;
    max_keep["auto_delete_old_sessions"] = true;
    max_keep["keep_recent_sessions"] = 100;
    EXPECT_TRUE(loop_from_json(max_keep, parsed, &error)) << error.message;
}
