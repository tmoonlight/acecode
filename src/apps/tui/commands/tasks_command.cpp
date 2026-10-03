#include "tasks_command.hpp"

#include "command_registry.hpp"
#include "session/session_storage.hpp"
#include "tui/subagent_host.hpp"

#include <mutex>
#include <sstream>

namespace acecode {
namespace {

// /tasks — 子代理(spawn_subagent)后台任务的操作入口。右侧栏只展示
// 运行中任务且不可交互;list/abort 由本命令承担(与 Web 的后台
// 任务面板同一数据:parent_session_id 归属当前主会话)。
static void cmd_tasks(CommandContext& ctx, const std::string& args) {
    auto push_system = [&](const std::string& text) {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system", text, false});
        ctx.state.chat_follow_tail = true;
    };
    if (!ctx.subagent_host) {
        push_system("/tasks is unavailable in this context.");
        return;
    }
    const std::string project_dir = SessionStorage::get_project_dir(ctx.cwd);
    std::istringstream iss(args);
    std::string sub;
    iss >> sub;

    if (sub.empty() || sub == "list") {
        auto entries = ctx.subagent_host->list_tasks(project_dir);
        if (entries.empty()) {
            push_system("No subagent tasks for this session.");
            return;
        }
        std::ostringstream oss;
        oss << "Subagent tasks (" << entries.size() << "):";
        for (const auto& e : entries) {
            oss << "\n  " << (e.running ? "\xE2\x97\x8F running " : "\xE2\x9C\x93 settled ")
                << e.id;
            if (!e.title.empty()) oss << "  " << e.title;
        }
        oss << "\n\nUse /tasks abort <id> to stop a running task. Subagent tasks "
               "stay with this session and are deleted only together with it.";
        push_system(oss.str());
        return;
    }
    if (sub == "abort") {
        std::string id;
        iss >> id;
        if (id.empty()) {
            push_system("Usage: /tasks abort <session-id>");
            return;
        }
        // 支持 id 前缀:唯一命中时展开。
        if (!ctx.subagent_host->abort_task(id)) {
            std::string matched;
            for (const auto& t : ctx.subagent_host->running_tasks()) {
                if (t.id.rfind(id, 0) == 0) {
                    if (!matched.empty()) { matched.clear(); break; }
                    matched = t.id;
                }
            }
            if (matched.empty() || !ctx.subagent_host->abort_task(matched)) {
                push_system("No running subagent task matches: " + id);
                return;
            }
            id = matched;
        }
        push_system("Abort requested for subagent task " + id + ".");
        return;
    }
    push_system("Usage: /tasks [list|abort <id>]");
}

} // namespace

void register_tasks_command(CommandRegistry& registry) {
    registry.register_command({"tasks", "List or abort subagent background tasks", cmd_tasks});
}

} // namespace acecode
