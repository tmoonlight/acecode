#pragma once

#include "tui/tui_state.hpp"
#include "agent/agent_loop.hpp"
#include "llm/llm_provider.hpp"
#include "config/config.hpp"
#include "session/token_tracker.hpp"
#include "session/session_manager.hpp"
#include "provider/session_model_binding.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <utility>

namespace acecode {

class McpManager;
class ToolExecutor;
class SkillRegistry;
class MemoryRuntime;
class CommandRegistry;
namespace tui { class SubagentHost; }

struct CommandContext {
    TuiState& state;
    AgentLoop& agent_loop;
    // Shared daemon/TUI model lifecycle owner. Every provider read takes a
    // shared_ptr snapshot; every install/reload goes through the binding.
    SessionModelBinding* model_binding = nullptr;
    AppConfig& config;
    TokenTracker& token_tracker;
    PermissionManager& permissions;
    std::function<void()> request_exit;
    SessionManager* session_manager = nullptr;
    std::function<void()> post_event;  // post a TUI refresh event from any thread
    McpManager* mcp_manager = nullptr; // runtime MCP control surface (optional)
    ToolExecutor* tools = nullptr;     // tool registry for /mcp enable/disable
    SkillRegistry* skills = nullptr;   // skill registry for /skills and /<skill-name> commands
    MemoryRuntime* memory = nullptr;   // memory runtime for /memory commands
    CommandRegistry* command_registry = nullptr; // self-reference for /skills reload
    std::string cwd;                   // working directory for cwd-scoped operations
    tui::SubagentHost* subagent_host = nullptr; // /tasks 的子代理宿主(仅斜杠 dispatch 路径注入)
    std::function<void(const UserInput&)> submit_user_input; // optional TUI submit wrapper
    // Optional observer installed only by direct user-input surfaces. Internal
    // dispatch contexts omit it so programmatic commands do not affect usage.
    std::function<void(const std::string&)> on_command_recognized;
    // Full-screen TUI surface entry points. They are intentionally callbacks
    // instead of FTXUI types so the command layer stays usable by daemon and
    // headless contexts. The string is a stable deep-link slug.
    std::function<bool(const std::string& tab, std::string& error)>
        open_settings_surface;
    std::function<bool(const std::string& tab, std::string& error)>
        open_management_surface;
    std::function<void()> on_command_completed;
    // dispatch() 在执行期间填入用户实际敲的命令名(可能是别名,如 "side"),
    // 供需要回显原样命令的处理函数使用;直接调用 execute 时为空。
    std::string invoked_command_name;
};

inline void submit_user_input(CommandContext& ctx, UserInput input) {
    if (ctx.submit_user_input) {
        ctx.submit_user_input(input);
    } else {
        ctx.agent_loop.submit(input);
    }
}

inline void submit_user_text(CommandContext& ctx,
                             const std::string& text,
                             const std::string& display_text = std::string{}) {
    UserInput input;
    input.text = text;
    input.display_text = display_text;
    submit_user_input(ctx, std::move(input));
}

struct SlashCommand {
    std::string name;
    std::string description;
    std::function<void(CommandContext& ctx, const std::string& args)> execute;
    // 别名与原名指向同一条命令:下拉菜单与 /help 只出一行,
    // 敲别名时显示成 "/原名 (别名)"。原名一般是长名,别名一般是短名。
    std::vector<std::string> aliases;
};

class CommandRegistry {
public:
    // 同名重复注册会整体替换(连同旧别名);别名不覆盖已存在的原名或别名。
    void register_command(const SlashCommand& cmd);

    // Remove a single command (and its aliases) by its canonical name.
    // Returns true when it existed.
    bool unregister_command(const std::string& name);

    // Check whether a command name or alias is registered.
    bool has_command(const std::string& name) const {
        return !resolve_name(name).empty();
    }

    // 原名或别名 → 原名;未注册返回空串。
    std::string resolve_name(const std::string& name) const;

    // 按原名或别名查找命令;未注册返回 nullptr。
    const SlashCommand* find(const std::string& name) const;

    // Dispatch a slash command string (e.g., "/help" or "/model gpt-4").
    // Returns true if a command was found and executed, false if unknown.
    bool dispatch(const std::string& input, CommandContext& ctx);

    // Registered commands keyed by canonical name. Aliases are not separate
    // entries; they live in SlashCommand::aliases.
    const std::map<std::string, SlashCommand>& commands() const { return commands_; }

private:
    std::map<std::string, SlashCommand> commands_;
    std::map<std::string, std::string> alias_to_name_;
};

} // namespace acecode
