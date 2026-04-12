#include "builtin_commands.hpp"
#include "compact.hpp"
#include <sstream>

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
        << "  /exit     - Exit acecode";
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_clear(CommandContext& ctx, const std::string& /*args*/) {
    ctx.state.conversation.clear();
    ctx.agent_loop.clear_messages();
    ctx.token_tracker.reset();
    ctx.state.token_status.clear();
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

void register_builtin_commands(CommandRegistry& registry) {
    registry.register_command({"help", "Show available commands", cmd_help});
    registry.register_command({"clear", "Clear conversation history", cmd_clear});
    registry.register_command({"model", "Show or switch current model", cmd_model});
    registry.register_command({"config", "Show current configuration", cmd_config});
    registry.register_command({"cost", "Show token usage and estimated cost", cmd_cost});
    registry.register_command({"compact", "Compress conversation history", cmd_compact});
    registry.register_command({"exit", "Exit acecode", cmd_exit});
}

} // namespace acecode
