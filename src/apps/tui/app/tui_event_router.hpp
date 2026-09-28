#pragma once
#include "tui/input/tui_input_context.hpp"
#include "utils/lifetime_token.hpp"
#include <ftxui/component/component_base.hpp>
#include <array>

namespace acecode::tui {
using InputHandler = InputDisposition (*)(TuiInputContext&, const ftxui::Event&);
struct InputRoute {
    const char* name;
    int original_line;  // 7942011b:src/main.cpp, before layer relocation.
    InputHandler handler;
};
class TuiEventRouter {
public:
    explicit TuiEventRouter(TuiInputContext& context) : context_(context) {}
    TuiEventRouter(const TuiEventRouter&) = delete;
    TuiEventRouter& operator=(const TuiEventRouter&) = delete;
    bool handle(const ftxui::Event& event);
    ftxui::Component wrap(ftxui::Component child);
    static const std::array<InputRoute, 38> & routes();
private:
    TuiInputContext& context_;
    LifetimeToken lifetime_;  // Revokes CatchEvent closures before dependencies.
};
}
