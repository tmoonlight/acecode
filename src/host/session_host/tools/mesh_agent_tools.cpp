#include "mesh_agent_tools.hpp"

#include "config/config.hpp"
#include "session_host/mesh/mesh_agent_service.hpp"
#include "utils/encoding.hpp"

#include <algorithm>
#include <optional>
#include <set>

namespace acecode {

namespace {

using nlohmann::json;
using mesh::MeshAgentService;

constexpr const char* kUnavailable = "collab manager unavailable";

ToolResult failure(const std::string& message) {
    ToolResult result;
    result.success = false;
    result.output = message;
    return result;
}

ToolSummary summary(const std::string& verb, const std::string& object) {
    ToolSummary s;
    s.icon = "agent";
    s.verb = verb;
    s.object = truncate_utf8_prefix(object, 80);
    return s;
}

// Codex deserializes tool arguments with deny_unknown_fields.
std::string parse_object(const std::string& arguments, const std::set<std::string>& allowed,
                         json& out) {
    try {
        out = arguments.empty() ? json::object() : json::parse(arguments);
    } catch (const std::exception& e) {
        return std::string("failed to parse function arguments: ") + e.what();
    }
    if (!out.is_object()) return "failed to parse function arguments: expected an object";
    for (const auto& [key, value] : out.items()) {
        if (allowed.count(key) == 0) return "unknown field `" + key + "`";
    }
    return {};
}

bool read_string(const json& args, const char* key, std::string& out, std::string& error,
                 bool required) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        if (required) error = std::string("missing field `") + key + "`";
        return !required;
    }
    if (!it->is_string()) {
        error = std::string("invalid type for `") + key + "`: expected a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

json string_property(const std::string& description) {
    return json{{"type", "string"}, {"description", description}};
}

json object_schema(json properties, json required) {
    json schema = {{"type", "object"}, {"properties", std::move(properties)},
                   {"additionalProperties", false}};
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

ToolImpl make_tool(std::string name, std::string description, json parameters) {
    ToolImpl tool;
    tool.definition.name = std::move(name);
    tool.definition.description = std::move(description);
    tool.definition.parameters = std::move(parameters);
    // Like spawn_subagent: coordination itself touches no files; each agent's
    // own permission gate governs what it does.
    tool.is_read_only = true;
    // Codex runs collaboration tools under the exclusive tool lock, never in a
    // parallel batch.
    tool.requires_serial_execution = true;
    return tool;
}

// Codex spawn_agent_models_description (at most 5 picker-visible models).
std::string models_description(const AppConfig& config) {
    std::string lines;
    std::size_t count = 0;
    for (const auto& profile : config.saved_models) {
        if (count++ == 5) break;
        lines += "\n- `" + profile.name + "`: " + profile.provider + " " + profile.model;
    }
    if (lines.empty()) return "No picker-visible model overrides are currently loaded.";
    return "Available model overrides (optional; inherited parent model is preferred):" + lines;
}

std::string spawn_description(const AppConfig& config) {
    std::string text;
    if (config.swarm.mesh.expose_model_overrides) text = models_description(config) + "\n";
    text +=
        "Spawns an agent to work on the specified task. If your current task is `/root/task1` and "
        "you agent_spawn with task_name \"task_3\" the agent will have canonical task name "
        "`/root/task1/task_3`.\n"
        "You are then able to refer to this agent as `task_3` or `/root/task1/task_3` "
        "interchangeably. However an agent `/root/task2/task_3` would only be able to communicate "
        "with this agent via its canonical name `/root/task1/task_3`.\n"
        "The spawned agent will have the same tools as you and the ability to spawn its own "
        "subagents.\n";
    text +=
        "It will be able to send you and other running agents messages, and its final answer will "
        "be provided to you when it finishes.\n"
        "The new agent's canonical task name will be provided to it along with the message.\n"
        "\n"
        "Note that passing `fork_turns=\"none\"` will not pass any surrounding context to the "
        "spawned subagent, which may cause the agent to lack the context it needs to complete its "
        "task, whereas `fork_turns=\"all\"` will provide the subagent with all surrounding context.";
    return text;
}

ToolImpl create_spawn_tool(std::weak_ptr<MeshAgentService> weak, const AppConfig& config) {
    const bool expose_model_overrides = config.swarm.mesh.expose_model_overrides;
    json properties = {
        {"message", string_property("Initial plain-text task for the new agent.")},
        {"task_name", string_property(
            "Task name for the new agent. Use lowercase letters, digits, and underscores.")},
        {"agent_type", string_property(
            "Agent type override for the new agent. Omit unless explicitly asked. The selected "
            "role applies regardless of how much parent history is inherited.\n"
            "An agent type is a member id of this session's team expert; it is only accepted when "
            "the session is bound to a team expert.")},
        {"fork_turns", string_property(
            "Optional number of turns to fork. Defaults to `all`. Use `none`, `all`, or a positive "
            "integer string such as `3` to fork only the most recent turns.")},
    };
    if (expose_model_overrides) {
        properties["model"] = string_property(
            "Model override for the new agent: a saved model name. Omit unless an explicit "
            "override is needed.");
        properties["reasoning_effort"] = string_property(
            "Reasoning effort override for the new agent. Omit to inherit the parent effort.");
    }
    auto tool = make_tool("agent_spawn", spawn_description(config),
                          object_schema(std::move(properties), json::array({"task_name", "message"})));
    tool.execute = [weak, expose_model_overrides](const std::string& arguments,
                                                  const ToolContext& ctx) -> ToolResult {
        auto service = weak.lock();
        if (!service) return failure(kUnavailable);
        std::set<std::string> allowed = {"message", "task_name", "agent_type", "fork_turns",
                                         "fork_context"};
        if (expose_model_overrides) allowed.insert({"model", "reasoning_effort"});
        json args;
        if (auto error = parse_object(arguments, allowed, args); !error.empty()) {
            return failure(error);
        }
        if (args.contains("fork_context")) {
            return failure("fork_context is not supported in MultiAgentV2; use fork_turns instead");
        }
        mesh::SpawnArgs spawn;
        std::string fork_turns;
        std::string effort;
        std::string error;
        if (!read_string(args, "task_name", spawn.task_name, error, true) ||
            !read_string(args, "message", spawn.message, error, true) ||
            !read_string(args, "agent_type", spawn.agent_type, error, false) ||
            !read_string(args, "fork_turns", fork_turns, error, false) ||
            !read_string(args, "model", spawn.model, error, false) ||
            !read_string(args, "reasoning_effort", effort, error, false)) {
            return failure(error);
        }
        const auto fork = mesh::parse_fork_turns(fork_turns, &error);
        if (!fork) return failure(error);
        spawn.fork = *fork;
        if (!effort.empty()) spawn.reasoning_effort = effort;
        const auto spawned = service->spawn(ctx, spawn);
        if (!spawned.error.empty()) return failure(spawned.error);
        ToolResult result;
        result.success = true;
        result.output = json{{"task_name", spawned.path}}.dump();
        result.metadata["subagent_session_id"] = spawned.session_id;
        result.metadata["agent_path"] = spawned.path;
        result.summary = summary("agent_spawn", spawned.path);
        return result;
    };
    return tool;
}

ToolImpl create_message_tool(std::weak_ptr<MeshAgentService> weak, bool trigger_turn) {
    const std::string name = trigger_turn ? "agent_followup_task" : "agent_send_message";
    const std::string description = trigger_turn
        ? "Send a follow-up task to an existing non-root target agent and trigger a turn if it is "
          "idle. If the target is already running, deliver the task promptly at message "
          "boundaries while sampling, or after the pending tool call completes."
        : "Send a message to an existing agent. The message will be delivered promptly. Does not "
          "trigger a new turn.";
    const json properties = trigger_turn
        ? json{{"target", string_property("Agent id or canonical task name to send a follow-up "
                                          "task to (from agent_spawn).")},
               {"message", string_property("Message text to send to the target agent.")}}
        : json{{"target", string_property(
                   "Relative or canonical task name to message (from agent_spawn).")},
               {"message", string_property("Message text to queue on the target agent.")}};
    auto tool = make_tool(name, description,
                          object_schema(properties, json::array({"target", "message"})));
    tool.execute = [weak, trigger_turn, name](const std::string& arguments,
                                              const ToolContext& ctx) -> ToolResult {
        auto service = weak.lock();
        if (!service) return failure(kUnavailable);
        json args;
        if (auto error = parse_object(arguments, {"target", "message"}, args); !error.empty()) {
            return failure(error);
        }
        std::string target;
        std::string message;
        std::string error;
        if (!read_string(args, "target", target, error, true) ||
            !read_string(args, "message", message, error, true)) {
            return failure(error);
        }
        if (auto delivery_error = service->deliver(ctx, target, message, trigger_turn);
            !delivery_error.empty()) {
            return failure(delivery_error);
        }
        // Codex returns an empty output on success.
        ToolResult result;
        result.success = true;
        result.metadata["agent_target"] = target;
        result.summary = summary(name, target);
        return result;
    };
    return tool;
}

ToolImpl create_wait_tool(std::weak_ptr<MeshAgentService> weak, const MeshSwarmConfig& config) {
    const int min_ms = config.min_wait_timeout_ms;
    const int default_ms = config.default_wait_timeout_ms;
    const int max_ms = config.max_wait_timeout_ms;
    auto tool = make_tool("agent_wait",
        "Wait for a mailbox update from any live agent, including queued messages and "
        "final-status notifications. The wait also ends early when new user input is steered "
        "into the active turn. Does not return the content; returns either a summary of which "
        "agents have updates (if any), an interruption summary for steered input, or a timeout "
        "summary if no activity arrives before the deadline.",
        object_schema(json{{"timeout_ms", json{{"type", "number"},
            {"description", "Timeout in milliseconds. Defaults to " + std::to_string(default_ms) +
                            ", min " + std::to_string(min_ms) + ", max " +
                            std::to_string(max_ms) + "."}}}},
                      json::array()));
    tool.execute = [weak, min_ms, default_ms, max_ms](const std::string& arguments,
                                                      const ToolContext& ctx) -> ToolResult {
        auto service = weak.lock();
        if (!service) return failure(kUnavailable);
        json args;
        if (auto error = parse_object(arguments, {"timeout_ms"}, args); !error.empty()) {
            return failure(error);
        }
        // Codex: above max is an error, anything else (even negative) clamps up to min.
        std::optional<long long> requested;
        if (args.contains("timeout_ms") && !args["timeout_ms"].is_null()) {
            if (!args["timeout_ms"].is_number()) {
                return failure("invalid type for `timeout_ms`: expected a number");
            }
            requested = args["timeout_ms"].get<long long>();
            if (*requested > max_ms) {
                return failure("timeout_ms must be at most " + std::to_string(max_ms));
            }
        }
        const long long timeout =
            requested ? std::max<long long>(*requested, min_ms) : default_ms;
        mesh::WaitResult outcome = mesh::WaitResult::TimedOut;
        if (auto error = service->wait(ctx, std::chrono::milliseconds(timeout), outcome);
            !error.empty()) {
            return failure(error);
        }
        std::string message;
        switch (outcome) {
        case mesh::WaitResult::Mailbox: message = "Wait completed."; break;
        case mesh::WaitResult::Steered: message = "Wait interrupted by new input."; break;
        case mesh::WaitResult::TimedOut: message = "Wait timed out."; break;
        case mesh::WaitResult::Aborted: message = "Wait aborted."; break;
        }
        if (requested && *requested < timeout) {
            message += "\n\nRequested timeout of " + std::to_string(*requested) +
                       "ms was clamped to the minimum of " + std::to_string(timeout) + "ms.";
        }
        ToolResult result;
        result.success = true;
        result.output = json{{"message", message},
                             {"timed_out", outcome == mesh::WaitResult::TimedOut}}.dump();
        result.summary = summary("agent_wait", "");
        return result;
    };
    return tool;
}

ToolImpl create_list_tool(std::weak_ptr<MeshAgentService> weak) {
    auto tool = make_tool("agent_list",
        "List live agents in the current root thread tree. Optionally filter by task-path prefix.",
        object_schema(json{{"path_prefix", string_property(
            "Task-path prefix filter without a trailing slash. Omit to list all live agents.")}},
                      json::array()));
    tool.execute = [weak](const std::string& arguments, const ToolContext& ctx) -> ToolResult {
        auto service = weak.lock();
        if (!service) return failure(kUnavailable);
        json args;
        if (auto error = parse_object(arguments, {"path_prefix"}, args); !error.empty()) {
            return failure(error);
        }
        std::string prefix;
        std::string error;
        if (!read_string(args, "path_prefix", prefix, error, false)) return failure(error);
        std::vector<mesh::ListedAgent> agents;
        if (auto list_error = service->list(ctx, prefix, agents); !list_error.empty()) {
            return failure(list_error);
        }
        json listed = json::array();
        for (const auto& agent : agents) {
            listed.push_back({{"agent_name", agent.path},
                              {"agent_status", mesh::agent_status_to_json(agent.status)}});
        }
        ToolResult result;
        result.success = true;
        result.output = json{{"agents", std::move(listed)}}.dump();
        result.summary = summary("agent_list", prefix);
        return result;
    };
    return tool;
}

ToolImpl create_interrupt_tool(std::weak_ptr<MeshAgentService> weak) {
    auto tool = make_tool("agent_interrupt",
        "Interrupt an agent's current turn, if any, and return its previous status. The agent "
        "remains available for messages and follow-up tasks.",
        object_schema(json{{"target", string_property(
            "Agent id or canonical task name to interrupt (from agent_spawn).")}},
                      json::array({"target"})));
    tool.execute = [weak](const std::string& arguments, const ToolContext& ctx) -> ToolResult {
        auto service = weak.lock();
        if (!service) return failure(kUnavailable);
        json args;
        if (auto error = parse_object(arguments, {"target"}, args); !error.empty()) {
            return failure(error);
        }
        std::string target;
        std::string error;
        if (!read_string(args, "target", target, error, true)) return failure(error);
        mesh::AgentStatus previous;
        if (auto interrupt_error = service->interrupt(ctx, target, previous);
            !interrupt_error.empty()) {
            return failure(interrupt_error);
        }
        ToolResult result;
        result.success = true;
        result.output = json{{"previous_status", mesh::agent_status_to_json(previous)}}.dump();
        result.metadata["agent_target"] = target;
        result.summary = summary("agent_interrupt", target);
        return result;
    };
    return tool;
}

} // namespace

void rebind_mesh_agent_tools(ToolExecutor& tools, std::weak_ptr<mesh::MeshAgentService> service,
                             const AppConfig& config) {
    const std::vector<ToolImpl> fresh = {
        create_spawn_tool(service, config),
        create_list_tool(service),
        create_message_tool(service, false),
        create_message_tool(service, true),
        create_wait_tool(service, config.swarm.mesh),
        create_interrupt_tool(service),
    };
    for (const auto& tool : fresh) {
        if (!tools.has_tool(tool.definition.name)) continue;
        tools.unregister_tool(tool.definition.name);
        tools.register_tool(tool);
    }
}

void register_mesh_agent_tools(ToolExecutor& tools, std::weak_ptr<mesh::MeshAgentService> service,
                               const AppConfig& config) {
    tools.register_tool(create_spawn_tool(service, config));
    tools.register_tool(create_list_tool(service));
    tools.register_tool(create_message_tool(service, false));
    tools.register_tool(create_message_tool(service, true));
    tools.register_tool(create_wait_tool(service, config.swarm.mesh));
    tools.register_tool(create_interrupt_tool(service));
}

} // namespace acecode
