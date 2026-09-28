#pragma once
#include "tui/tui_state.hpp"
#include "tui/screen_port.hpp"
#include "tui/chat/chat_viewport.hpp"
#include "tui/render/frame_geometry.hpp"
#include "permissions/permissions.hpp"
#include <ftxui/component/component_base.hpp>
#include <atomic>
#include <functional>

namespace acecode::tui {
struct FrameTerminalSize { int width; int height; };
class TuiFrameRenderer {
public:
    using TerminalSize = std::function<FrameTerminalSize()>;
    TuiFrameRenderer(TuiState& state, IScreenPort& screen,
        const std::string& version_str, const std::string& cwd_display,
        ChatViewport& viewport, FrameGeometry& geometry, std::atomic<int>& anim_tick,
        ftxui::Component input_component, PermissionManager& permissions,
        bool dangerous_mode, bool conhost_compat_layout, bool hover_supported,
        TerminalSize terminal_size = {});
    ftxui::Element render();
private:
    TuiState& state_;
    IScreenPort& screen_;
    const std::string& version_str_;
    const std::string& cwd_display_;
    ChatViewport& viewport_;
    FrameGeometry& geometry_;
    std::atomic<int>& anim_tick_;
    // Shared with the component tree: rendering retains the editor for the same lifetime.
    ftxui::Component input_component_;
    PermissionManager& permissions_;
    bool dangerous_mode_, conhost_compat_layout_, hover_supported_;
    TerminalSize terminal_size_;
};
}
