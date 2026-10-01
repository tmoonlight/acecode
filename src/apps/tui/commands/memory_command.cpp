#include "memory_command.hpp"

#include "session_host/memory_command.hpp"
#include "session_host/memory_runtime.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <cstdlib>
#include <mutex>

namespace acecode {

namespace {

void emit(CommandContext& ctx, const std::string& msg) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    ctx.state.conversation.push_back({"system", msg, false});
    ctx.state.chat_follow_tail = true;
}

int run_editor(const std::string& target) {
    std::string editor = getenv_utf8("EDITOR");
#ifdef _WIN32
    if (editor.empty()) editor = "notepad";
#else
    if (editor.empty()) editor = "vim";
#endif
    const std::string cmd = editor + " \"" + target + "\"";
#ifdef _WIN32
    return _wsystem(utf8_to_wide(cmd).c_str());
#else
    return std::system(cmd.c_str());
#endif
}

void cmd_memory(CommandContext& ctx, const std::string& args) {
    MemoryCommandContext memory_ctx;
    memory_ctx.runtime = ctx.memory;
    memory_ctx.session = ctx.session_manager;
    const MemoryCommandResult result = dispatch_memory_command(args, memory_ctx);
    if (result.edit_path.empty()) {
        emit(ctx, result.text);
        return;
    }
    // rc 因编辑器而异;无论结果都按磁盘重扫,并告诉用户改的是哪个文件。
    (void)run_editor(result.edit_path);
    if (ctx.memory && ctx.memory->service()) ctx.memory->service()->global().reload();
    emit(ctx, "Reloaded memory from disk (edited " +
                  path_to_utf8_generic(path_from_utf8(result.edit_path)) + ").");
}

} // namespace

void register_memory_command(CommandRegistry& registry) {
    registry.register_command(
        {"memory", "List, view, edit or forget memory; flush memory summarization; turn memory off/on for this session",
         cmd_memory});
}

} // namespace acecode
