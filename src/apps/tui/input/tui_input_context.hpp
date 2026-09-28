#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"
#include "tui/render/frame_geometry.hpp"
#include <atomic>
namespace acecode {
class CommandRegistry; class SessionManager; class PermissionManager;
struct AppConfig;
}

namespace acecode::tui {
// All references are fixed constructor dependencies owned by the TUI app.
// App implementations enter through ports; input code never stores AgentLoop.
struct TuiInputContext {
    TuiState& state;
    IScreenPort& screen;
    ChatViewport& viewport;
    FrameGeometry& geometry;
    CommandRegistry& commands;
    ITurnSubmitter& turn;
    ICommandContextFactory& command_contexts;
    IClipboard& clipboard;
    AppConfig& config;
    PermissionManager& permissions;
    SessionManager& session;
    std::atomic<bool>& auth_done;
    const std::string& working_dir;
    std::atomic<std::int64_t>& last_keyboard_input_at_ms;
    PermissionResponder respond_remote;
};
}
