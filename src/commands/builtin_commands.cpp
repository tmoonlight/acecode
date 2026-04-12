#include "builtin_commands.hpp"
#include "compact.hpp"
#include <sstream>
#include <iomanip>

namespace acecode {

static void cmd_help(CommandContext& ctx, const std::string& /*args*/) {
    std::ostringstream oss;
    oss << "Available commands:\n"
        << "  /help     - Show this help message\n"
        << "  /clear    - Clear conversation history\n"
        << "  /compact  - Compress conversation history\n"
        << "  /model    - Show or switch current model\n"
        << "  /config   - Show current configuration\n"
        << "  /cost     - Show token usage and estimated cost\n"
        << "  /resume   - Resume a previous session\n"
        << "  /exit     - Exit acecode";
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_clear(CommandContext& ctx, const std::string& /*args*/) {
    ctx.state.conversation.clear();
    ctx.agent_loop.clear_messages();
    ctx.token_tracker.reset();
    ctx.state.token_status.clear();
    if (ctx.session_manager) {
        ctx.session_manager->end_current_session();
    }
    ctx.state.conversation.push_back({"system", "Conversation cleared.", false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_model(CommandContext& ctx, const std::string& args) {
    if (args.empty()) {
        std::string info = "[" + ctx.provider.name() + "] model: " + ctx.provider.model();
        ctx.state.conversation.push_back({"system", info, false});
    } else {
        ctx.provider.set_model(args);
        ctx.state.status_line = "[" + ctx.provider.name() + "] model: " + args;
        ctx.state.conversation.push_back({"system", "Model switched to: " + args, false});
    }
    ctx.state.chat_follow_tail = true;
}

static void cmd_config(CommandContext& ctx, const std::string& /*args*/) {
    std::ostringstream oss;
    oss << "Current configuration:\n"
        << "  provider:       " << ctx.config.provider << "\n"
        << "  model:          " << ctx.provider.model() << "\n"
        << "  context_window: " << ctx.context_window << "\n"
        << "  permission:     " << PermissionManager::mode_name(ctx.permissions.mode());
    if (ctx.config.provider == "openai") {
        oss << "\n  base_url:       " << ctx.config.openai.base_url;
    }
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_cost(CommandContext& ctx, const std::string& /*args*/) {
    std::ostringstream oss;
    oss << "Session token usage:\n"
        << "  prompt:     " << TokenTracker::format_tokens(ctx.token_tracker.prompt_tokens()) << "\n"
        << "  completion: " << TokenTracker::format_tokens(ctx.token_tracker.completion_tokens()) << "\n"
        << "  total:      " << TokenTracker::format_tokens(ctx.token_tracker.total_tokens()) << "\n";
    oss << std::fixed;
    oss.precision(4);
    oss << "  est. cost:  ~$" << ctx.token_tracker.estimated_cost();
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_compact(CommandContext& ctx, const std::string& /*args*/) {
    ctx.state.conversation.push_back({"system", "Compacting conversation...", false});
    ctx.state.chat_follow_tail = true;

    auto result = compact_context(ctx.provider, ctx.agent_loop, ctx.state);
    if (!result.performed) {
        ctx.state.conversation.push_back({"system", result.error, false});
    } else {
        std::ostringstream oss;
        oss << "Compacted " << result.messages_compressed << " messages, saved ~"
            << TokenTracker::format_tokens(result.estimated_tokens_saved) << " tokens";
        ctx.state.conversation.push_back({"system", oss.str(), false});
    }
    ctx.state.chat_follow_tail = true;
}

static void cmd_exit(CommandContext& ctx, const std::string& /*args*/) {
    if (ctx.request_exit) {
        ctx.request_exit();
    }
}

static void do_resume_session(CommandContext& ctx, const std::string& session_id,
                              const std::vector<SessionMeta>& sessions) {
    // Find the meta for this session
    const SessionMeta* target = nullptr;
    for (const auto& s : sessions) {
        if (s.id == session_id) {
            target = &s;
            break;
        }
    }

    auto messages = ctx.session_manager->resume_session(session_id);
    ctx.agent_loop.clear_messages();
    for (const auto& msg : messages) {
        ctx.agent_loop.push_message(msg);
    }
    ctx.state.conversation.clear();
    for (const auto& msg : messages) {
        bool is_tool = (msg.role == "tool");
        ctx.state.conversation.push_back({msg.role, msg.content, is_tool});
    }
    std::ostringstream oss;
    oss << "Resumed session " << session_id << " (" << messages.size() << " messages)";
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_resume(CommandContext& ctx, const std::string& args) {
    if (!ctx.session_manager) {
        ctx.state.conversation.push_back({"system", "Session persistence is not available.", false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    auto sessions = ctx.session_manager->list_sessions();
    if (sessions.empty()) {
        ctx.state.conversation.push_back({"system", "No previous sessions found for this project.", false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    // If a number argument is provided, resume that session directly
    if (!args.empty()) {
        int choice = 0;
        try {
            choice = std::stoi(args);
        } catch (...) {
            ctx.state.conversation.push_back({"system", "Invalid session number: " + args, false});
            ctx.state.chat_follow_tail = true;
            return;
        }
        if (choice < 1 || choice > static_cast<int>(sessions.size())) {
            ctx.state.conversation.push_back({"system",
                "Session number out of range. Use /resume to see available sessions.", false});
            ctx.state.chat_follow_tail = true;
            return;
        }
        do_resume_session(ctx, sessions[choice - 1].id, sessions);
        return;
    }

    // Build picker items
    int max_show = std::min(static_cast<int>(sessions.size()), 20);
    ctx.state.resume_items.clear();
    for (int i = 0; i < max_show; ++i) {
        const auto& s = sessions[i];
        std::ostringstream line;
        line << "[" << (i + 1) << "] " << s.updated_at
             << "  " << s.message_count << " msgs";
        if (!s.summary.empty()) {
            line << "  " << s.summary;
        }
        ctx.state.resume_items.push_back({s.id, line.str()});
    }
    ctx.state.resume_selected = 0;
    ctx.state.resume_picker_active = true;

    // Capture session list for callback
    auto captured_sessions = sessions;
    auto* sm = ctx.session_manager;
    auto* al = &ctx.agent_loop;
    ctx.state.resume_callback = [&state = ctx.state, sm, al, captured_sessions](const std::string& sid) {
        auto messages = sm->resume_session(sid);
        al->clear_messages();
        for (const auto& msg : messages) {
            al->push_message(msg);
        }
        state.conversation.clear();
        for (const auto& msg : messages) {
            bool is_tool = (msg.role == "tool");
            state.conversation.push_back({msg.role, msg.content, is_tool});
        }
        std::ostringstream oss;
        oss << "Resumed session " << sid << " (" << messages.size() << " messages)";
        state.conversation.push_back({"system", oss.str(), false});
        state.chat_follow_tail = true;
    };
}

void register_builtin_commands(CommandRegistry& registry) {
    registry.register_command({"help", "Show available commands", cmd_help});
    registry.register_command({"clear", "Clear conversation history", cmd_clear});
    registry.register_command({"model", "Show or switch current model", cmd_model});
    registry.register_command({"config", "Show current configuration", cmd_config});
    registry.register_command({"cost", "Show token usage and estimated cost", cmd_cost});
    registry.register_command({"compact", "Compress conversation history", cmd_compact});
    registry.register_command({"resume", "Resume a previous session", cmd_resume});
    registry.register_command({"exit", "Exit acecode", cmd_exit});
}

} // namespace acecode
