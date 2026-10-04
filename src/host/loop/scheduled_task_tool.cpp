#include "scheduled_task_tool.hpp"

#include "loop_request.hpp"
#include "loop_scheduler.hpp"
#include "config/config.hpp"
#include "workspace/workspace_registry.hpp"

#include <chrono>
#include <algorithm>

namespace acecode::loop {
namespace {
using nlohmann::json;

ToolResult failure(const std::string& code, const std::string& message) {
    return ToolResult{json{{"error", code}, {"message", message}}.dump(), false};
}

json property(const char* type, const char* description) {
    return {{"type", type}, {"description", description}};
}
}

ScheduledTaskService::ScheduledTaskService(
    LoopStore& store, const AppConfig& config, std::shared_mutex& config_mutex,
    desktop::WorkspaceRegistry& workspaces, std::string projects_dir,
    LoopScheduler* scheduler, Clock clock)
    : store_(store), config_(config), config_mutex_(config_mutex),
      workspaces_(workspaces), projects_dir_(std::move(projects_dir)),
      scheduler_(scheduler), clock_(std::move(clock)) {
    if (!clock_) clock_ = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };
}

std::optional<LoopDefinition> ScheduledTaskService::prepare(json body, ValidationError& error) const {
    if (!body.is_object()) {
        error = {"BAD_JSON", "", "scheduled task arguments must be an object"};
        return std::nullopt;
    }
    std::vector<std::string> models;
    {
        std::shared_lock<std::shared_mutex> lock(config_mutex_);
        for (const auto& model : config_.saved_models) models.push_back(model.name);
        if (!body.contains("model_name")) {
            body["model_name"] = !config_.default_model_name.empty()
                ? config_.default_model_name : (models.empty() ? "" : models.front());
        }
    }
    const auto now = clock_();
    if (body.contains("schedule") && body["schedule"].is_object() &&
        body["schedule"].contains("kind") && body["schedule"]["kind"] == "interval" &&
        !body["schedule"].contains("anchor_ms")) body["schedule"]["anchor_ms"] = now;
    LoopDefinition definition;
    const auto resolve = [workspaces = workspaces_.list(), projects_dir = projects_dir_](
        const std::string& hash) -> std::optional<std::string> {
        for (const auto& workspace : workspaces) {
            if (workspace.hash == hash) return workspace.cwd;
        }
        if (hash.size() != 16 || !std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        })) return std::nullopt;
        if (const auto workspace = desktop::load_workspace_metadata(projects_dir, hash)) {
            if (desktop::workspace_hash_matches_cwd(hash, workspace->cwd)) return workspace->cwd;
        }
        return std::nullopt;
    };
    if (!parse_loop_request(body, now, models, resolve, definition, error)) return std::nullopt;
    ScheduleCompilation compiled;
    if (!compile_schedule(definition.schedule, now, compiled, &error)) return std::nullopt;
    return definition;
}

ToolResult ScheduledTaskService::create(const LoopDefinition& definition) {
    ValidationError validation;
    auto checked = prepare(loop_to_json(definition), validation);
    if (!checked) return failure(validation.code, validation.message);
    StoreError error;
    const auto created = store_.create_loop(std::move(*checked), clock_(), &error);
    if (!created) {
        json result{{"error", error.code}, {"message", error.message}};
        if (error.conflict) result["conflict"] = {
            {"loop_id", error.conflict->loop_id}, {"loop_name", error.conflict->loop_name},
            {"first_conflict_at_ms", error.conflict->first_conflict_at_ms}};
        return ToolResult{result.dump(), false};
    }
    if (scheduler_) scheduler_->notify_changed();
    return ToolResult{json{{"created", true}, {"task", loop_to_json(*created)},
        {"schedule_summary", schedule_summary(created->schedule)},
        {"message", "定时任务已创建。可在定时任务页面管理，无需等待执行。"}}.dump(), true};
}

ToolImpl create_scheduled_task_tool(std::weak_ptr<ScheduledTaskService> service) {
    ToolImpl tool;
    tool.definition.name = "create_scheduled_task";
    tool.definition.description =
        "Create an ACECode scheduled task using the same store and scheduler as manual creation. "
        "Load the scheduled-task skill first. This tool asks the user to choose execution permissions "
        "(YOLO recommended) before saving. On success, report the task and next run; do not wait for execution.";
    auto schedule = property("object", "Same schedule fields as the manual scheduled task form. Epoch values are milliseconds.");
    schedule["properties"] = {
        {"kind", {{"type", "string"}, {"enum", {"period", "interval", "once"}}}},
        {"period", {{"type", "string"}, {"enum", {"daily", "workdays", "weekly"}}}},
        {"weekdays", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}, {"maximum", 6}}}}},
        {"hour", {{"type", "integer"}, {"minimum", 0}, {"maximum", 23}}},
        {"minute", {{"type", "integer"}, {"minimum", 0}, {"maximum", 59}}},
        {"interval_value", property("integer", "Positive interval length.")},
        {"interval_unit", {{"type", "string"}, {"enum", {"minutes", "hours", "days"}}}},
        {"anchor_ms", property("integer", "Interval anchor; defaults to now.")},
        {"once_at_ms", property("integer", "Required for a one-time task.")},
        {"timezone_offset_minutes", property("integer", "Defaults to daemon local UTC offset.")},
        {"valid_from_ms", property("integer", "Optional inclusive start of validity.")},
        {"valid_until_ms", property("integer", "Optional inclusive end of validity.")}};
    schedule["required"] = json::array({"kind"});
    schedule["additionalProperties"] = false;
    tool.definition.parameters = {{"type", "object"}, {"properties", {
        {"name", property("string", "Short user-visible task name.")},
        {"prompt", property("string", "Instructions to execute when the task is due.")},
        {"schedule", schedule},
        {"model_name", property("string", "Saved model name; omitted uses the manual form default.")},
        {"workspace_hash", property("string", "Optional registered workspace hash, paired with workspace_cwd.")},
        {"workspace_cwd", property("string", "Optional registered workspace path; omit both for reminders without a project.")},
        {"use_worktree", property("boolean", "Use an isolated worktree; default false, like manual creation.")}}},
        {"required", json::array({"name", "prompt", "schedule"})}, {"additionalProperties", false}};
    tool.activation_skill = "scheduled-task";
    tool.requires_serial_execution = true;
    tool.execute = [service](const std::string& arguments, const ToolContext& ctx) {
        auto body = json::parse(arguments, nullptr, false);
        if (!body.is_object()) return failure("BAD_JSON", "arguments must be an object");
        // Permission comes only from the actual user response, never model args.
        body.erase("permission_mode");
        body.erase("id");
        body["enabled"] = true;
        ValidationError error;
        std::optional<LoopDefinition> definition;
        {
            const auto owner = service.lock();
            if (!owner) return failure("LOOP_UNAVAILABLE", "定时任务服务不可用。");
            definition = owner->prepare(std::move(body), error);
        }
        if (!definition) return failure(error.code, error.message);
        if (!ctx.ask_user_questions) return failure("PERMISSION_REQUIRED", "请在支持权限提问的聊天中创建定时任务。");
        if (ctx.abort_flag && ctx.abort_flag->load()) return failure("CANCELLED", "已取消创建定时任务。");
        const std::string question = "定时任务“" + definition->name + "”使用哪种执行权限？";
        const auto answer = ctx.ask_user_questions(json::array({{
            {"id", "scheduled-task-permission"}, {"text", question}, {"header", "执行权限"},
            {"multiSelect", false}, {"options", json::array({
                {{"label", "完全访问权限（YOLO，推荐）"}, {"value", "yolo"}, {"recommended", true},
                 {"description", "到点自动执行，工具操作无需逐次批准。"}},
                {{"label", "默认权限"}, {"value", "default"},
                 {"description", "需要授权时等待你确认，可能暂停执行。"}}
            })}
        }}));
        if (!answer.is_object() || answer.value("cancelled", false) ||
            answer.value("interjected", false) || answer.value("timed_out", false) ||
            (ctx.abort_flag && ctx.abort_flag->load())) {
            return failure("CANCELLED", "尚未确认权限，未创建定时任务。");
        }
        std::string permission;
        if (answer.contains("answers") && answer["answers"].is_array()) {
            for (const auto& item : answer["answers"]) {
                if (!item.is_object() || item.value("question_id", "") != "scheduled-task-permission" ||
                    item.value("auto_selected", false) || item.value("not_answered", false) ||
                    !item.contains("selected") || !item["selected"].is_array() || item["selected"].size() != 1 ||
                    !item["selected"][0].is_string() || !item.value("custom_text", std::string{}).empty()) continue;
                permission = item["selected"][0].get<std::string>();
            }
        }
        if (permission != "yolo" && permission != "default") {
            return failure("PERMISSION_REQUIRED", "未选择执行权限，未创建定时任务。请重新选择权限后再试。");
        }
        definition->permission_mode = permission;
        const auto owner = service.lock();
        if (!owner) return failure("LOOP_UNAVAILABLE", "定时任务服务已关闭。");
        return owner->create(*definition);
    };
    return tool;
}

} // namespace acecode::loop
