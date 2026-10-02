#include "swarm_mode.hpp"

#include <cctype>

namespace acecode {

const char* swarm_mode_name(SwarmMode mode) {
    switch (mode) {
    case SwarmMode::Star: return "star";
    case SwarmMode::Mesh: return "mesh";
    case SwarmMode::Off: break;
    }
    return "off";
}

std::optional<SwarmMode> parse_swarm_mode(const std::string& text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char ch : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lowered == "off" || lowered == "false") return SwarmMode::Off;
    if (lowered == "star" || lowered == "true") return SwarmMode::Star;
    if (lowered == "mesh") return SwarmMode::Mesh;
    return std::nullopt;
}

const std::vector<std::string>& mesh_agent_tool_names() {
    static const std::vector<std::string> names = {
        "agent_spawn", "agent_list", "agent_send_message",
        "agent_followup_task", "agent_wait", "agent_interrupt",
    };
    return names;
}

std::unordered_map<std::string, std::string> swarm_mode_hidden_tools(SwarmMode mode) {
    std::unordered_map<std::string, std::string> hidden;
    if (mode != SwarmMode::Mesh) {
        for (const auto& name : mesh_agent_tool_names()) {
            hidden[name] = name + " is only available in mesh swarm mode.";
        }
        return hidden;
    }
    hidden["spawn_subagent"] =
        "spawn_subagent is not available in mesh swarm mode; use agent_spawn instead.";
    hidden["wait_subagent"] =
        "wait_subagent is not available in mesh swarm mode; use agent_wait instead.";
    static const char* const kThreadTools[] = {
        "create_thread", "fork_thread", "list_threads", "read_thread",
        "send_message_to_thread", "wait_threads", "set_thread_title",
        "set_thread_pinned", "set_thread_archived", "delete_thread", "repair_thread",
    };
    for (const char* name : kThreadTools) {
        hidden[name] = std::string(name) +
            " is not available in mesh swarm mode; use the agent_* collaboration tools instead.";
    }
    return hidden;
}

} // namespace acecode
