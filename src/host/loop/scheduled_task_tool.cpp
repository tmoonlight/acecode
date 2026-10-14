#include "scheduled_task_tool.hpp"

#include "loop_request.hpp"
#include "loop_scheduler.hpp"
#include "config/config.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace acecode::loop {
namespace {
using nlohmann::json;

ToolResult failure(const std::string& code, const std::string& message) {
    return ToolResult{json{{"error", code}, {"message", message}}.dump(), false};
}

json property(const char* type, const char* description) {
    return {{"type", type}, {"description", description}};
}

// 去掉 ASCII 空白并把 ASCII 字母转小写;中文按原字节保留。UTF-8 自同步,
// 之后按字节找子串不会跨字符误配。
std::string compact_prompt(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') continue;
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c));
    }
    return out;
}

bool starts_with_at(const std::string& text, std::size_t pos, const std::string& prefix) {
    return text.compare(pos, prefix.size(), prefix) == 0;
}

// 「动词 [+ 量词] + 名词」紧挨着才算创建请求:「建立定时任务」「设置一个定时任务」
// 命中,「检查昨天新增的定时任务」因为中间隔着「的」不命中,监控类提示词照常可用。
bool contains_creation_phrase(const std::string& text,
                              const std::vector<std::string>& verbs,
                              const std::vector<std::string>& quantifiers,
                              const std::vector<std::string>& nouns) {
    for (const auto& verb : verbs) {
        for (std::size_t pos = text.find(verb); pos != std::string::npos;
             pos = text.find(verb, pos + 1)) {
            std::vector<std::size_t> starts{pos + verb.size()};
            for (const auto& quantifier : quantifiers) {
                if (starts_with_at(text, starts.front(), quantifier)) {
                    starts.push_back(starts.front() + quantifier.size());
                }
            }
            for (const auto start : starts) {
                for (const auto& noun : nouns) {
                    if (starts_with_at(text, start, noun)) return true;
                }
            }
        }
    }
    return false;
}
}

bool prompt_requests_scheduled_task_creation(const std::string& prompt) {
    const std::string text = compact_prompt(prompt);
    for (const char* marker : {"/scheduled-task", "$scheduled-task", "create_scheduled_task"}) {
        if (text.find(marker) != std::string::npos) return true;
    }
    static const std::vector<std::string> zh_verbs = {
        "创建", "建立", "新建", "添加", "新增", "设置", "设定", "安排",
        "设一个", "设个", "建", "加", "弄", "搞"};
    static const std::vector<std::string> zh_quantifiers = {
        "一个新的", "一个", "一条", "一项", "一下", "新的", "个", "条", "项"};
    static const std::vector<std::string> zh_nouns = {
        "定时任务", "定时提醒", "计划任务", "周期任务"};
    static const std::vector<std::string> en_verbs = {
        "create", "add", "setup", "schedule", "make"};
    static const std::vector<std::string> en_quantifiers = {
        "another", "anew", "new", "an", "a"};
    static const std::vector<std::string> en_nouns = {
        "scheduledtask", "scheduled-task", "recurringtask", "cronjob"};
    return contains_creation_phrase(text, zh_verbs, zh_quantifiers, zh_nouns) ||
           contains_creation_phrase(text, en_verbs, en_quantifiers, en_nouns);
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
    // prompt 是每次到点执行的内容。照抄「建立定时任务,每天帮我生成代码」这类创建
    // 请求,执行时会被当成新的建任务请求。只拦 AI 创建路径,手动表单不经过这里。
    if (prompt_requests_scheduled_task_creation(definition.prompt)) {
        error = {"PROMPT_CONTAINS_CREATION_REQUEST", "prompt",
                 "prompt 是每次到点要执行的内容，不能包含“建立/创建定时任务”这类创建请求，"
                 "否则执行时会被当成新的建任务请求。请去掉创建定时任务和执行时间的描述，"
                 "只写单次要做的事，例如把“建立定时任务，每天帮我生成代码”写成“生成代码：……”。"};
        return std::nullopt;
    }
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
        {"prompt", property("string",
            "What to do in ONE run when the task is due, as a direct instruction. "
            "Never include the request to create a scheduled task or the schedule itself; "
            "the scheduler handles timing. E.g. user '建立定时任务，每天帮我生成代码' -> "
            "prompt '生成代码：<what to generate>'.")},
        {"schedule", schedule},
        {"model_name", property("string", "Saved model name; omitted uses the manual form default.")},
        {"workspace_hash", property("string", "Optional registered workspace hash, paired with workspace_cwd.")},
        {"workspace_cwd", property("string", "Optional registered workspace path; omit both for reminders without a project.")},
        {"use_worktree", property("boolean", "Use an isolated worktree; default false, like manual creation.")}}},
        {"required", json::array({"name", "prompt", "schedule"})}, {"additionalProperties", false}};
    tool.activation_skill = "scheduled-task";
    tool.activation_hint =
        "the user wants something done or a reminder sent later, at a set time, or repeatedly "
        "(定时 / 每天 / 每周 / 工作日 / 每隔 N 分钟 / 明天 9 点 / 建立定时任务). Use it instead of OS "
        "schedulers (cron, Task Scheduler, launchd), sleep loops, or doing the work right away.";
    tool.loop_execution_refusal =
        "create_scheduled_task is not available while a scheduled task is running. This run is "
        "one execution of an existing scheduled task and the scheduler already handles timing. "
        "If the task text asks to create a scheduled task or describes a schedule, that is the "
        "original creation wording: do the underlying work once now and do not schedule anything.";
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
