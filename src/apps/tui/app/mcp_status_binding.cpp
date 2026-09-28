#include "tui/app/mcp_status_binding.hpp"
#include "tui/tui_state.hpp"
#include "tui/model/mcp_sidebar_model.hpp"
#include "tool/mcp_manager.hpp"
#include "tool/mcp_startup_coordination.hpp"
namespace acecode::tui {
McpStatusBinding::McpStatusBinding(McpManager& mcp, ToolExecutor& tools,
    TuiState& state, IScreenPort& screen) : mcp_(mcp), state_(state), screen_(screen) {
    if (mcp_.configured_server_count() == 0) return;
    mcp_.set_status_callback([ref = lifetime_.ref(*this)](const McpServerInfo& info) {
        ref.with([&](McpStatusBinding& owner) { owner.changed(info); });
    });
    registered_ = true;
    mcp_.start_async(tools);
}
McpStatusBinding::~McpStatusBinding() { stop(); }
void McpStatusBinding::stop() {
    if (registered_) { mcp_.set_status_callback({}); registered_ = false; }
    lifetime_.revoke();
}
void McpStatusBinding::changed(const McpServerInfo& info) {
    auto servers = build_mcp_sidebar_servers(mcp_);
    auto message = mcp_status_message(info);
    {
        std::lock_guard<std::mutex> lock(state_.mu);
        set_mcp_sidebar_servers_locked(state_, std::move(servers));
        if (message) state_.conversation.push_back({"system", *message, false});
    }
    screen_.post_event(ftxui::Event::Custom);
}
}
