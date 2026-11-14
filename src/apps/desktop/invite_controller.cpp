#include "invite_controller.hpp"
#include "platform/clipboard.hpp"
#include "utils/sha256.hpp"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace acecode::desktop {
struct DesktopInviteController::State {
    std::atomic<bool> enabled{true};
    std::mutex mu;
    std::unordered_set<std::string> seen;
    std::unordered_map<std::string, std::string> pending;
    std::function<void(const std::string&)> open;
};

DesktopInviteController::DesktopInviteController(WebHost& host, bool enabled)
    : host_(host), state_(std::make_shared<State>()) {
    state_->enabled = enabled;
    const auto host_ref = lifetime_.ref(host);
    state_->open = [host_ref](const std::string& link) {
        const auto encoded = nlohmann::json(link).dump();
        host_ref.with([encoded](WebHost& current) {
            current.eval("(function(){const link=" + encoded + ";if(window.__aceInviteReceiverReady){"
                "window.dispatchEvent(new CustomEvent('acecode:open-invite',{detail:{link}}));}else{"
                "(window.__acePendingInvites=window.__acePendingInvites||[]).push(link);}})();");
        });
    };
    const std::weak_ptr<State> weak = state_;
    host.set_open_invite_handler(state_->open);
    host.bind("aceDesktop_getInviteClipboardPreference", [weak](const std::string&) {
        auto state = weak.lock();
        return nlohmann::json{{"ok", !!state}, {"enabled", state && state->enabled.load()}}.dump();
    });
    host.bind("aceDesktop_applyInviteClipboardPreference", [weak](const std::string& request) {
        auto state = weak.lock();
        const auto args = nlohmann::json::parse(request, nullptr, false);
        if (!state || !args.is_array() || args.empty() || !args[0].is_boolean())
            return std::string("{\"ok\":false}");
        state->enabled = args[0].get<bool>();
        return std::string("{\"ok\":true}");
    });
    host.bind("aceDesktop_markInviteSeen", [weak](const std::string& request) {
        const auto args = nlohmann::json::parse(request, nullptr, false);
        auto state = weak.lock();
        if (state && args.is_array() && !args.empty() && args[0].is_string()) {
            const auto link = extract_clipboard_invitation(args[0].get<std::string>());
            if (!link.empty()) { std::lock_guard<std::mutex> lock(state->mu); state->seen.insert(sha256_hex(link)); }
        }
        return std::string("{\"ok\":true}");
    });
    host.set_clipboard_changed_handler([weak](bool foreground, const std::string& invitation) {
        auto state = weak.lock();
        if (!state || !state->enabled.load()) return;
        const auto link = invitation.empty() ? read_clipboard_invitation() : invitation;
        if (link.empty()) return;
        const auto hash = sha256_hex(link);
        { std::lock_guard<std::mutex> lock(state->mu); if (!state->seen.insert(hash).second) return; }
        if (foreground) { state->open(link); return; }
        NotifyPayload notification;
        notification.id = "invite-" + hash;
        notification.title = "ACECode invitation";
        notification.body = "Open ACECode to review the session invitation.";
        { std::lock_guard<std::mutex> lock(state->mu); state->pending[notification.id] = link; }
        if (!show_notification(notification)) {
            { std::lock_guard<std::mutex> lock(state->mu); state->pending.erase(notification.id); }
            state->open(link);
        }
    });
}

DesktopInviteController::~DesktopInviteController() {
    host_.set_clipboard_changed_handler({});
    host_.set_open_invite_handler({});
    lifetime_.revoke();
}

std::function<void(const std::string&)> DesktopInviteController::opener() const { return state_->open; }

std::function<bool(const NotifyPayload&)> DesktopInviteController::notification_handler() const {
    return [weak = std::weak_ptr<State>(state_)](const NotifyPayload& payload) {
        auto state = weak.lock();
        if (!state) return false;
        std::string link;
        { std::lock_guard<std::mutex> lock(state->mu);
          const auto found = state->pending.find(payload.id);
          if (found == state->pending.end()) return false;
          link = found->second; state->pending.erase(found); }
        state->open(link);
        return true;
    };
}

std::function<void()> DesktopInviteController::activation_handler() const {
    return [weak = std::weak_ptr<State>(state_), host_ref = lifetime_.ref(host_)] {
        auto state = weak.lock();
        if (state && state->enabled.load()) host_ref.with([](WebHost& host) { host.check_clipboard_invitation(); });
    };
}
} // namespace acecode::desktop
