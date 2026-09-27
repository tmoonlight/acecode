#include "task_suggestion_tools.hpp"

#include "session/session_manager.hpp"
#include "session/task_suggestion_service.hpp"
#include "utils/tool_args_parser.hpp"

namespace acecode {
namespace {

using nlohmann::json;

ToolResult suggestion_result(TaskSuggestionServiceResult result) {
    return result.ok ? ToolResult{result.value.dump(), true}
                     : ToolResult{"[Error] " + result.error, false};
}

} // namespace

void register_task_suggestion_tools(
    ToolExecutor& tools, std::shared_ptr<TaskSuggestionService> service) {
    ToolImpl suggest;
    suggest.definition.name = "suggest_task";
    suggest.definition.description =
        "Offer a useful, evidence-backed side task outside the current request. "
        "Shows a persistent suggestion card; does NOT start work or create a "
        "worktree. The user chooses whether to start it in a worktree or the "
        "current directory. Continue the main task after proposing. Use only "
        "for independent actionable findings, not blockers required by the "
        "current request or speculative cleanup. Include the evidence, scope, "
        "dependencies and verification in a self-contained prompt. Avoid "
        "duplicate or excessive suggestions. Use dismiss_task_suggestion when "
        "the finding is resolved or no longer applies.";
    suggest.definition.parameters = json{
        {"type", "object"}, {"additionalProperties", false},
        {"properties", {
            {"title", {{"type", "string"}, {"minLength", 1}, {"maxLength", 160}}},
            {"description", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2000}}},
            {"prompt", {{"type", "string"}, {"minLength", 1}, {"maxLength", 16000}}},
        }},
        {"required", json::array({"title", "description", "prompt"})},
    };
    // It changes only the suggestion inbox, never project files or execution.
    suggest.is_read_only = true;
    suggest.execute = [service](const std::string& arguments,
                                const ToolContext& ctx) -> ToolResult {
        if (!service || !ctx.session_manager) {
            return {"[Error] Task suggestions require an active host session", false};
        }
        ToolArgsParser parser(arguments);
        if (parser.has_error()) return {"[Error] " + parser.error(), false};
        const auto title = parser.get<std::string>("title");
        const auto description = parser.get<std::string>("description");
        const auto prompt = parser.get<std::string>("prompt");
        if (!title || !description || !prompt) {
            return {"[Error] title, description and prompt are required strings", false};
        }
        return suggestion_result(service->propose(
            ctx.session_manager->current_session_id(),
            json{{"kind", "side_task"}, {"title", *title},
                 {"description", *description}, {"prompt", *prompt}}));
    };
    tools.register_tool(std::move(suggest));

    ToolImpl dismiss;
    dismiss.definition.name = "dismiss_task_suggestion";
    dismiss.definition.description =
        "Withdraw a suggestion from this session when its finding is resolved "
        "or stale. Use the suggestion id returned by suggest_task. This never "
        "stops an already started task.";
    dismiss.definition.parameters = json{
        {"type", "object"}, {"additionalProperties", false},
        {"properties", {{"suggestion_id", {{"type", "string"}, {"minLength", 1}}}}},
        {"required", json::array({"suggestion_id"})},
    };
    dismiss.is_read_only = true;
    dismiss.execute = [service](const std::string& arguments,
                                const ToolContext& ctx) -> ToolResult {
        if (!service || !ctx.session_manager) {
            return {"[Error] Task suggestions require an active host session", false};
        }
        ToolArgsParser parser(arguments);
        if (parser.has_error()) return {"[Error] " + parser.error(), false};
        const auto id = parser.get<std::string>("suggestion_id");
        if (!id) return {"[Error] suggestion_id is required", false};
        return suggestion_result(service->dismiss(
            ctx.session_manager->current_session_id(), *id));
    };
    tools.register_tool(std::move(dismiss));
}

} // namespace acecode
