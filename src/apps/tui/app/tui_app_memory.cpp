// TUI 的记忆摘要宿主:扫描启动工作区、按主会话忙碌状态避让、/memory flush 完成后
// 在对话里插一行系统消息。回调都经 bind() 走 LifetimeRef,TuiApp 析构后自动失效。
#include "tui/app/tui_app.hpp"
#include "tui/app/tui_services.hpp"
#include "tui/app/tui_screen_host.hpp"

#include "agent/agent_loop.hpp"
#include "session/session_storage.hpp"
#include "session_host/memory_runtime.hpp"
#include "session_host/memory_scheduler.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

namespace acecode::tui {

void TuiApp::start_memory_scheduler() {
    if (!services_ || !services_->memory) return;
    MemorySchedulerHost host;
    host.project_dirs = bind(&TuiApp::memory_project_dirs);
    host.session_busy = bind(&TuiApp::memory_session_busy);
    host.app_config = bind(&TuiApp::config_snapshot);
    host.notify = bind(&TuiApp::memory_notice);
    host.config_path = path_to_utf8(path_from_utf8(get_acecode_dir()) / "config.json");
    services_->memory->start_summary_scheduler(std::move(host));
}

std::vector<std::string> TuiApp::memory_project_dirs() {
    return {SessionStorage::get_project_dir(environment_.working_dir)};
}

bool TuiApp::memory_session_busy(const std::string& session_id) {
    return agent_loop_ && agent_loop_->is_busy() &&
           session_manager_.current_session_id() == session_id;
}

void TuiApp::memory_notice(const std::string& session_id, const std::string& text) {
    if (session_manager_.current_session_id() != session_id) return;
    {
        std::lock_guard<std::mutex> lock(state_.mu);
        state_.conversation.push_back({"system", text, false});
        state_.chat_follow_tail = true;
    }
    if (screen_host_) screen_host_->post_event(ftxui::Event::Custom);
}

} // namespace acecode::tui
