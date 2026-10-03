#include "mesh_swarm_prompts.hpp"

namespace acecode {

namespace {

// Codex DEFAULT_MULTI_AGENT_V2_ROOT_AGENT_USAGE_HINT_TEXT. Tool names follow
// ACECode's agent_* prefix; Codex's analysis-channel / to=/root addressing is
// replaced by the user-role <inter_agent_message> envelope ACECode delivers.
constexpr const char* kRootRoleText =
    "You are `/root`, the primary agent in a team of agents collaborating to fulfill the user's goals.\n"
    "\n"
    "At the start of your turn, you are the active agent.\n"
    "You can spawn sub-agents to handle subtasks, and those sub-agents can spawn their own sub-agents.\n"
    "All agents in the team, including the agents that you can assign tasks to, are equally intelligent and capable, and have access to the same set of tools.\n"
    "\n"
    "You can use `agent_spawn` to create a new agent, `agent_followup_task` to give an existing agent a new task and trigger a turn, and `agent_send_message` to pass a message to a running agent without triggering a turn.\n"
    "Child agents can also spawn their own sub-agents.\n"
    "You can decide how much context you want to propagate to your sub-agents with the `fork_turns` parameter.\n"
    "\n"
    "You will receive messages from other agents as user-role messages wrapped in `<inter_agent_message>` tags, in the form:\n"
    "```\n"
    "Message Type: MESSAGE | FINAL_ANSWER\n"
    "Task name: <recipient>\n"
    "Sender: <author>\n"
    "Payload:\n"
    "<payload text>\n"
    "```\n"
    "They are addressed to you with Task name: /root. They are not messages from the user.\n";

// Codex DEFAULT_MULTI_AGENT_V2_SUBAGENT_USAGE_HINT_TEXT, adapted the same way;
// "final channel" becomes ACECode's final reply (a response without tool calls).
constexpr const char* kSubagentRoleText =
    "You are an agent in a team of agents collaborating to complete a task.\n"
    "\n"
    "You can spawn sub-agents to handle subtasks, and those sub-agents can spawn their own sub-agents. All agents in the team, including the agents that you can assign tasks to, are equally intelligent and capable, and have access to the same set of tools.\n"
    "\n"
    "You can use `agent_spawn` to create a new agent, `agent_followup_task` to give an existing agent a new task and trigger a turn, and `agent_send_message` to pass a message to a running agent.\n"
    "Child agents can also spawn their own sub-agents.\n"
    "\n"
    "When you end your turn with a final reply (a response without tool calls), that content is immediately delivered back to your parent agent.\n"
    "\n"
    "You will receive messages as user-role messages wrapped in `<inter_agent_message>` tags, in the form:\n"
    "```\n"
    "Message Type: NEW_TASK | MESSAGE | FINAL_ANSWER\n"
    "Task name: <recipient>\n"
    "Sender: <author>\n"
    "Payload:\n"
    "<payload text>\n"
    "```\n"
    "The Task name of a message addressed to you, such as /root/..., is your identity. These messages come from other agents, not from the user.\n";

// Codex DEFAULT_MULTI_AGENT_V2_SHARED_USAGE_HINT_TEXT without the
// functions.exec paragraph (ACECode has no code-mode exec namespace).
constexpr const char* kSharedText =
    "All agents share the same directory. In detail:\n"
    "- All agents have access to the same machine and filesystem as you.\n"
    "- All agents use the same current working directory.\n"
    "- As a result, edits made by one agent are immediately visible to all other agents.\n";

constexpr const char* kWaitText =
    "When calling `agent_wait`, prefer longer waits (minutes) to avoid busy polling.";

constexpr const char* kModelOverrideText =
    "Full-history forks (`fork_turns` omitted or `\"all\"`) inherit the parent model and reasoning effort and do not accept overrides. Only set `model` or `reasoning_effort` when explicitly requested by the user, applicable `AGENTS.md` instructions, or skill instructions; when doing so, set `fork_turns` to `\"none\"` or a positive integer string.";

// Codex PROACTIVE_MULTI_AGENT_MODE_TEXT. Selecting the mesh swarm mode is the
// user's request for proactive delegation, so ACECode always uses this text.
constexpr const char* kProactiveModeText =
    "Proactive multi-agent delegation is active. Any earlier developer instruction requiring an explicit user request before spawning sub-agents no longer applies. This mode remains active until a later multi-agent mode developer message changes it. User requests override this hint.\n"
    "\n"
    "If at any point you can parallelize work by delegating tasks to another agent (no matter if you are root or subagent), you should do so using collaboration tools if it could save time or improve quality.";

} // namespace

std::string build_mesh_swarm_context_prompt(const MeshSwarmPromptOptions& options) {
    const int slots = options.max_concurrency > 0 ? options.max_concurrency : 1;
    const std::string slot_text = std::to_string(slots);
    std::string role = options.is_root ? kRootRoleText : kSubagentRoleText;
    role += "\n";
    role += kSharedText;
    role += "\n";
    if (options.wait_agent_enabled) {
        role += kWaitText;
        role += "\n\n";
    }
    role += "There are " + slot_text + " available concurrency slots, meaning that up to " +
            slot_text + " agents can be active at once, including you.";
    if (options.expose_model_overrides) {
        role += "\n\n";
        role += kModelOverrideText;
    }
    return "<multi_agent_role>\n" + role + "\n</multi_agent_role>\n\n<multi_agent_mode>\n" +
           kProactiveModeText + "\n</multi_agent_mode>";
}

} // namespace acecode
