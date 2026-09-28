#pragma once
#include <ftxui/component/event.hpp>
#include <functional>
#include <string>

namespace acecode::tui {
// Helpers depend on this port; only the screen host owns ScreenInteractive.
// Posted tasks retain the same deferred Post ordering.
class IScreenPort {
public:
    virtual ~IScreenPort() = default;
    virtual void post_event(ftxui::Event event) = 0;
    virtual void post_task(std::function<void()> task) = 0;
    virtual void exit() = 0;
    virtual std::string get_selection() = 0;
    virtual void shift_selection(int dx, int dy) = 0;
    virtual int dimx() const = 0;
};
} // namespace acecode::tui
