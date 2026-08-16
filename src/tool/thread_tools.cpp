#include "thread_tools.hpp"

#include "../session/session_manager.hpp"
#include "../session/thread_service.hpp"
#include "../utils/tool_args_parser.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace acecode {

namespace {

using nlohmann::json;

ToolResult tool_error(const std::string& message) {
    return ToolResult{"[Error] " + message, false};
}

ToolResult service_result(ThreadServiceResult result) {
    if (!result.success) return tool_error(result.error);
    ToolResult output;
    output.output = result.value.dump();
    return output;
}

ThreadScope scope_from_context(const ToolContext& ctx) {
    ThreadScope scope;
    scope.cwd = ctx.cwd;
    scope.caller_manager = ctx.session_manager;
    if (!ctx.session_manager) return scope;
    scope.caller_thread_id = ctx.session_manager->current_session_id();
    if (!scope.caller_thread_id.empty()) {
        const auto meta = ctx.session_manager->load_session_meta(
            scope.caller_thread_id);
        if (!meta.cwd.empty()) scope.cwd = meta.cwd;
    }
    return scope;
}

std::optional<ToolArgsParser> parse_args(const std::string& arguments,
                                         ToolResult* error) {
    ToolArgsParser parser(arguments);
    if (parser.has_error()) {
        *error = tool_error(parser.error());
        return std::nullopt;
    }
    return parser;
}

template <typename T>
std::optional<T> required_arg(const ToolArgsParser& parser,
                              const char* key,
                              ToolResult* error) {
    auto value = parser.get<T>(key);
    if (value.has_value()) return value;
    *error = tool_error(std::string(key) +
                        " is required and must have the expected type");
    return std::nullopt;
}

std::shared_ptr<ThreadService> service_or_error(
    const std::shared_ptr<ThreadToolDeps>& deps,
    ToolResult* error) {
    if (!deps || !deps->service) {
        *error = tool_error("thread tools are unavailable in this runtime");
        return {};
    }
    return deps->service;
}

ToolImpl make_tool(std::string name,
                   std::string description,
                   json parameters,
                   bool read_only,
                   std::function<ToolResult(
                       const std::string&, const ToolContext&)> execute) {
    ToolImpl tool;
    tool.definition.name = std::move(name);
    tool.definition.description = std::move(description);
    tool.definition.parameters = std::move(parameters);
    tool.is_read_only = read_only;
    tool.execute = std::move(execute);
    return tool;
}

json object_schema(json properties,
                   json required = json::array()) {
    json schema{
        {"type", "object"},
        {"properties", std::move(properties)},
        {"additionalProperties", false},
    };
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

ToolImpl create_thread_tool(std::shared_ptr<ThreadToolDeps> deps) {
    return make_tool(
        "create_thread",
        "Create a separate ACECode thread in the current workspace and send "
        "its initial prompt. Creation is non-blocking and returns threadId.",
        object_schema(json{
            {"prompt", json{{"type", "string"}, {"minLength", 1}}},
            {"title", json{{"type", "string"}, {"maxLength", 160}}},
            {"model", json{{"type", "string"}}},
        }, json::array({"prompt"})),
        false,
        [deps](const std::string& arguments,
               const ToolContext& ctx) -> ToolResult {
            ToolResult error;
            auto service = service_or_error(deps, &error);
            if (!service) return error;
            auto parser = parse_args(arguments, &error);
            if (!parser) return error;
            auto prompt = required_arg<std::string>(*parser, "prompt", &error);
            if (!prompt) return error;
            return service_result(service->create(
                scope_from_context(ctx),
                *prompt,
                parser->get_or<std::string>("title", {}),
                parser->get_or<std::string>("model", {})));
        });
}

ToolImpl fork_thread_tool(std::shared_ptr<ThreadToolDeps> deps) {
    return make_tool(
        "fork_thread",
        "Fork completed persisted history into a new ACECode thread. Omit "
        "threadId to fork the calling thread.",
        object_schema(json{
            {"threadId", json{{"type", "string"}}},
        }),
        false,
        [deps](const std::string& arguments,
               const ToolContext& ctx) -> ToolResult {
            ToolResult error;
            auto service = service_or_error(deps, &error);
            if (!service) return error;
            auto parser = parse_args(arguments, &error);
            if (!parser) return error;
            return service_result(service->fork(
                scope_from_context(ctx),
                parser->get_or<std::string>("threadId", {})));
        });
}

ToolImpl list_threads_tool(std::shared_ptr<ThreadToolDeps> deps) {
    return make_tool(
        "list_threads",
        "List ACECode threads in the current workspace. pinnedThreads always "
        "contains every pinned thread in UI order; limit applies only to "
        "non-pinned threads.",
        object_schema(json{
            {"limit", json{{"type", "integer"},
                            {"minimum", 1}, {"maximum", 50}}},
        }),
        true,
        [deps](const std::string& arguments,
               const ToolContext& ctx) -> ToolResult {
            ToolResult error;
            auto service = service_or_error(deps, &error);
            if (!service) return error;
            auto parser = parse_args(arguments, &error);
            if (!parser) return error;
            const int limit = (std::max)(1,
                parser->get_or<int>("limit", 20));
            return service_result(service->list(
                scope_from_context(ctx), static_cast<std::size_t>(limit)));
        });
}

ToolImpl read_thread_tool(std::shared_ptr<ThreadToolDeps> deps) {
    return make_tool(
        "read_thread",
        "Read recent bounded turn summaries for one ACECode thread without "
        "opening it. Pass nextCursor to read older turns.",
        object_schema(json{
            {"threadId", json{{"type", "string"}, {"minLength", 1}}},
            {"cursor", json{{"type", "string"}}},
            {"turnLimit", json{{"type", "integer"},
                                {"minimum", 1}, {"maximum", 20}}},
            {"includeOutputs", json{{"type", "boolean"}}},
            {"maxOutputCharsPerItem", json{{"type", "integer"},
                {"minimum", 256}, {"maximum", 8000}}},
        }, json::array({"threadId"})),
        true,
        [deps](const std::string& arguments,
               const ToolContext& ctx) -> ToolResult {
            ToolResult error;
            auto service = service_or_error(deps, &error);
            if (!service) return error;
            auto parser = parse_args(arguments, &error);
            if (!parser) return error;
            auto thread_id = required_arg<std::string>(
                *parser, "threadId", &error);
            if (!thread_id) return error;
            return service_result(service->read(
                scope_from_context(ctx),
                *thread_id,
                parser->get_or<std::string>("cursor", {}),
                static_cast<std::size_t>((std::max)(1,
                    parser->get_or<int>("turnLimit", 8))),
                parser->get_or<bool>("includeOutputs", false),
                static_cast<std::size_t>((std::max)(256,
                    parser->get_or<int>("maxOutputCharsPerItem", 2000)))));
        });
}

ToolImpl send_message_tool(std::shared_ptr<ThreadToolDeps> deps) {
    return make_tool(
        "send_message_to_thread",
        "Send a follow-up prompt to an existing ACECode thread in the "
        "background.",
        object_schema(json{
            {"threadId", json{{"type", "string"}, {"minLength", 1}}},
            {"prompt", json{{"type", "string"}, {"minLength", 1}}},
        }, json::array({"threadId", "prompt"})),
        false,
        [deps](const std::string& arguments,
               const ToolContext& ctx) -> ToolResult {
            ToolResult error;
            auto service = service_or_error(deps, &error);
            if (!service) return error;
            auto parser = parse_args(arguments, &error);
            if (!parser) return error;
            auto thread_id = required_arg<std::string>(
                *parser, "threadId", &error);
            if (!thread_id) return error;
            auto prompt = required_arg<std::string>(*parser, "prompt", &error);
            if (!prompt) return error;
            return service_result(service->send(
                scope_from_context(ctx),
                *thread_id, *prompt));
        });
}

ToolImpl wait_threads_tool(std::shared_ptr<ThreadToolDeps> deps) {
    return make_tool(
        "wait_threads",
        "Wait for the first of up to eight ACECode threads to complete, fail, "
        "or need attention. timeoutMs: 0 returns an immediate snapshot.",
        object_schema(json{
            {"targets", json{
                {"type", "array"}, {"minItems", 1}, {"maxItems", 8},
                {"items", json{
                    {"type", "object"},
                    {"properties", json{
                        {"threadId", json{{"type", "string"},
                                           {"minLength", 1}}},
                        {"afterCursor", json{{"type", "string"}}},
                    }},
                    {"required", json::array({"threadId"})},
                    {"additionalProperties", false},
                }},
            }},
            {"timeoutMs", json{{"type", "integer"},
                                {"minimum", 0}, {"maximum", 120000}}},
        }, json::array({"targets"})),
        true,
        [deps](const std::string& arguments,
               const ToolContext& ctx) -> ToolResult {
            ToolResult error;
            auto service = service_or_error(deps, &error);
            if (!service) return error;
            json args;
            try {
                args = json::parse(arguments);
            } catch (const std::exception& ex) {
                return tool_error(std::string("invalid arguments: ") + ex.what());
            }
            if (!args.is_object() || !args.contains("targets") ||
                !args["targets"].is_array()) {
                return tool_error("targets must be an array");
            }
            std::vector<ThreadWaitTarget> targets;
            for (const auto& raw : args["targets"]) {
                if (!raw.is_object() || !raw.contains("threadId") ||
                    !raw["threadId"].is_string()) {
                    return tool_error("each target requires threadId");
                }
                ThreadWaitTarget target;
                target.thread_id = raw["threadId"].get<std::string>();
                if (raw.contains("afterCursor")) {
                    if (!raw["afterCursor"].is_string()) {
                        return tool_error("afterCursor must be a string");
                    }
                    const std::string value =
                        raw["afterCursor"].get<std::string>();
                    try {
                        std::size_t consumed = 0;
                        target.after_cursor = std::stoull(
                            value, &consumed, 10);
                        if (consumed != value.size()) {
                            return tool_error("invalid afterCursor");
                        }
                    } catch (...) {
                        return tool_error("invalid afterCursor");
                    }
                }
                targets.push_back(std::move(target));
            }
            const int timeout_ms = args.contains("timeoutMs") &&
                args["timeoutMs"].is_number_integer()
                ? args["timeoutMs"].get<int>() : 120000;
            return service_result(service->wait(
                scope_from_context(ctx), targets,
                timeout_ms, ctx.abort_flag));
        });
}

ToolImpl set_title_tool(std::shared_ptr<ThreadToolDeps> deps) {
    return make_tool(
        "set_thread_title",
        "Rename an ACECode thread. Omit threadId to rename the calling thread.",
        object_schema(json{
            {"threadId", json{{"type", "string"}}},
            {"title", json{{"type", "string"}, {"maxLength", 160}}},
        }, json::array({"title"})),
        false,
        [deps](const std::string& arguments,
               const ToolContext& ctx) -> ToolResult {
            ToolResult error;
            auto service = service_or_error(deps, &error);
            if (!service) return error;
            auto parser = parse_args(arguments, &error);
            if (!parser) return error;
            auto title = required_arg<std::string>(*parser, "title", &error);
            if (!title) return error;
            return service_result(service->set_title(
                scope_from_context(ctx),
                parser->get_or<std::string>("threadId", {}),
                *title));
        });
}

ToolImpl set_pinned_tool(std::shared_ptr<ThreadToolDeps> deps) {
    return make_tool(
        "set_thread_pinned",
        "Pin or unpin an ACECode thread.",
        object_schema(json{
            {"threadId", json{{"type", "string"}, {"minLength", 1}}},
            {"pinned", json{{"type", "boolean"}}},
        }, json::array({"threadId", "pinned"})),
        false,
        [deps](const std::string& arguments,
               const ToolContext& ctx) -> ToolResult {
            ToolResult error;
            auto service = service_or_error(deps, &error);
            if (!service) return error;
            auto parser = parse_args(arguments, &error);
            if (!parser) return error;
            auto thread_id = required_arg<std::string>(
                *parser, "threadId", &error);
            if (!thread_id) return error;
            auto pinned = required_arg<bool>(*parser, "pinned", &error);
            if (!pinned) return error;
            return service_result(service->set_pinned(
                scope_from_context(ctx), *thread_id, *pinned));
        });
}

ToolImpl set_archived_tool(std::shared_ptr<ThreadToolDeps> deps) {
    return make_tool(
        "set_thread_archived",
        "Archive or unarchive an ACECode thread. Omit threadId to target the "
        "calling thread.",
        object_schema(json{
            {"threadId", json{{"type", "string"}}},
            {"archived", json{{"type", "boolean"}}},
        }, json::array({"archived"})),
        false,
        [deps](const std::string& arguments,
               const ToolContext& ctx) -> ToolResult {
            ToolResult error;
            auto service = service_or_error(deps, &error);
            if (!service) return error;
            auto parser = parse_args(arguments, &error);
            if (!parser) return error;
            auto archived = required_arg<bool>(*parser, "archived", &error);
            if (!archived) return error;
            return service_result(service->set_archived(
                scope_from_context(ctx),
                parser->get_or<std::string>("threadId", {}),
                *archived));
        });
}

ToolImpl delete_thread_tool(std::shared_ptr<ThreadToolDeps> deps) {
    return make_tool(
        "delete_thread",
        "Permanently delete an ACECode thread and its spawned descendant "
        "threads. The calling thread cannot delete itself.",
        object_schema(json{
            {"threadId", json{{"type", "string"}, {"minLength", 1}}},
        }, json::array({"threadId"})),
        false,
        [deps](const std::string& arguments,
               const ToolContext& ctx) -> ToolResult {
            ToolResult error;
            auto service = service_or_error(deps, &error);
            if (!service) return error;
            auto parser = parse_args(arguments, &error);
            if (!parser) return error;
            auto thread_id = required_arg<std::string>(
                *parser, "threadId", &error);
            if (!thread_id) return error;
            return service_result(service->delete_thread(
                scope_from_context(ctx), *thread_id));
        });
}

ToolImpl repair_thread_tool(std::shared_ptr<ThreadToolDeps> deps) {
    return make_tool(
        "repair_thread",
        "Deterministically diagnose and repair another ACECode thread after "
        "context overflow or history damage. This does not call a model, "
        "replay tools, or rewrite the visible transcript.",
        object_schema(json{
            {"threadId", json{{"type", "string"}, {"minLength", 1}}},
        }, json::array({"threadId"})),
        false,
        [deps](const std::string& arguments,
               const ToolContext& ctx) -> ToolResult {
            ToolResult error;
            auto service = service_or_error(deps, &error);
            if (!service) return error;
            auto parser = parse_args(arguments, &error);
            if (!parser) return error;
            auto thread_id = required_arg<std::string>(
                *parser, "threadId", &error);
            if (!thread_id) return error;
            return service_result(service->repair(
                scope_from_context(ctx), *thread_id));
        });
}

} // namespace

void register_codex_thread_tools(
    ToolExecutor& tools,
    std::shared_ptr<ThreadToolDeps> deps) {
    tools.register_tool(create_thread_tool(deps));
    tools.register_tool(fork_thread_tool(deps));
    tools.register_tool(list_threads_tool(deps));
    tools.register_tool(read_thread_tool(deps));
    tools.register_tool(send_message_tool(deps));
    tools.register_tool(wait_threads_tool(deps));
    tools.register_tool(set_title_tool(deps));
    tools.register_tool(set_pinned_tool(deps));
    tools.register_tool(set_archived_tool(deps));
    tools.register_tool(delete_thread_tool(deps));
    tools.register_tool(repair_thread_tool(deps));
}

} // namespace acecode
