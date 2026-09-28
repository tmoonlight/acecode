// /lsp 命令实现:状态文本组装是纯函数,TUI 与 daemon builtin 共用。

#include "tui/commands/lsp_command.hpp"
#include "lsp/lsp_status_text.hpp"

#include <cctype>
#include <mutex>

namespace acecode {

namespace {

std::string trim(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

void emit(CommandContext& ctx, const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system", text, false});
        ctx.state.chat_follow_tail = true;
    }
    if (ctx.post_event) ctx.post_event();
}

void cmd_lsp(CommandContext& ctx, const std::string& args) {
    emit(ctx, dispatch_lsp_subcommand(trim(args)));
}

} // namespace

void register_lsp_command(CommandRegistry& registry) {
    registry.register_command({
        "lsp",
        "Show LSP server status (connected/broken/not installed)",
        cmd_lsp,
    });
}

} // namespace acecode
