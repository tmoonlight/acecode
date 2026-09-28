#pragma once
#include "utils/lifetime_token.hpp"
#include <ftxui/component/event.hpp>
#include <functional>
namespace acecode::tui {
class TuiScreenHost;
class UiPostTarget {
public:
    explicit UiPostTarget(LifetimeRef<TuiScreenHost> host) : host_(std::move(host)) {}
    void post_task(std::function<void()> task) const;
    void post_event(ftxui::Event event) const;
private:
    LifetimeRef<TuiScreenHost> host_;
};
}
