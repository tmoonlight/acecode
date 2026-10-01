#include "session_registry_memory.hpp"

#include "memory_command.hpp"
#include "session_registry.hpp"

#include "session/system_notice.hpp"

namespace acecode {

BuiltinCommandResult execute_memory_builtin(SessionEntry& entry,
                                            const BuiltinCommandRequest& request,
                                            MemoryRuntime* runtime) {
    if (!entry.loop) return {BuiltinCommandStatus::Failed, "session unavailable"};
    MemoryCommandContext ctx;
    ctx.runtime = runtime;
    ctx.session = entry.sm.get();
    ctx.web = true;
    const MemoryCommandResult result = dispatch_memory_command(request.args, ctx);
    entry.loop->emit_system_message(result.text, make_system_notice_metadata("memory_status"));
    return {BuiltinCommandStatus::Accepted, "ok"};
}

} // namespace acecode
