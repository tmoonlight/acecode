#include "stop_background_task_tool.hpp"

#include "background_tasks.hpp"

#include <nlohmann/json.hpp>

#include <sstream>

namespace acecode {

namespace {

std::string running_task_list(const BackgroundTaskRegistry& registry) {
    std::ostringstream out;
    std::size_t count = 0;
    for (const auto& info : registry.list()) {
        if (info.state != BackgroundTaskState::Running) continue;
        out << "\n- " << info.id << " ("
            << (info.kind == BackgroundTaskKind::Agent ? "subagent" : "command") << "): " << info.title;
        ++count;
    }
    if (count == 0) return "No background tasks are running.";
    return "Running background tasks:" + out.str();
}

ToolResult stop_background_task(const std::string& arguments_json, const ToolContext& ctx) {
    if (!ctx.background_tasks) {
        return ToolResult{"[Error] Background tasks are not available here.", false};
    }
    std::string task_id;
    try {
        const auto args = nlohmann::json::parse(arguments_json.empty() ? std::string("{}") : arguments_json);
        task_id = args.value("task_id", std::string{});
    } catch (...) {
        return ToolResult{"[Error] Failed to parse tool arguments.", false};
    }
    if (task_id.empty()) {
        return ToolResult{running_task_list(*ctx.background_tasks), true};
    }
    std::string error;
    if (!ctx.background_tasks->stop(task_id, error)) {
        return ToolResult{"[Error] " + error + "\n" + running_task_list(*ctx.background_tasks), false};
    }
    ToolResult result{"Stop requested for background task " + task_id +
                      ". You will be notified when it has stopped.", true};
    result.metadata["background_task_id"] = task_id;
    ToolSummary summary;
    summary.verb = "Stopped";
    summary.object = task_id;
    summary.icon = "stop";
    result.summary = summary;
    return result;
}

} // namespace

ToolImpl create_stop_background_task_tool() {
    ToolDef def;
    def.name = "stop_background_task";
    def.description =
        "Stop one of this session's background tasks: a background command is killed together with its "
        "child processes, and a background subagent has its current turn stopped. Use it when a background "
        "task is no longer needed or is stuck. Call it without task_id to list the running background tasks.";
    def.parameters = nlohmann::json{
        {"type", "object"},
        {"properties", {
            {"task_id", {
                {"type", "string"},
                {"description", "ID of the background task (a command's bg_... ID or a subagent's session ID)."},
            }},
        }},
    };
    // 只作用于本会话自己起的后台任务,不碰文件系统:与 spawn_subagent 一样自动放行。
    return ToolImpl{def, stop_background_task, /*is_read_only=*/true};
}

} // namespace acecode
