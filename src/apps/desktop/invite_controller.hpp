#pragma once
#include "web_host.hpp"
#include "platform/native_ui/notifications.hpp"
#include "utils/lifetime_token.hpp"
#include <memory>

namespace acecode::desktop {
class DesktopInviteController {
public:
    DesktopInviteController(WebHost& host, bool clipboard_enabled);
    ~DesktopInviteController();
    std::function<void(const std::string&)> opener() const;
    std::function<bool(const NotifyPayload&)> notification_handler() const;
    std::function<void()> activation_handler() const;
private:
    WebHost& host_;
    struct State;
    // Controller owns state; native callbacks hold weak references only.
    std::shared_ptr<State> state_;
    LifetimeToken lifetime_;
};
} // namespace acecode::desktop
