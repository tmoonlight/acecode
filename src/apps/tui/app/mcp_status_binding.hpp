#pragma once
#include "tui/screen_port.hpp"
#include "utils/lifetime_token.hpp"
namespace acecode { class McpManager; class ToolExecutor; struct TuiState; struct McpServerInfo; }
namespace acecode::tui {
class McpStatusBinding {
public:
    McpStatusBinding(McpManager& mcp, ToolExecutor& tools, TuiState& state, IScreenPort& screen);
    ~McpStatusBinding();
    void stop();
private:
    void changed(const McpServerInfo& info);
    McpManager& mcp_;
    TuiState& state_;
    IScreenPort& screen_;
    bool registered_ = false;
    LifetimeToken lifetime_;
};
}
