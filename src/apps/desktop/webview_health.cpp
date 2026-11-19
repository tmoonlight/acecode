#include "webview_health.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace acecode::desktop {
namespace {

std::string string_field(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::int64_t integer_field(const nlohmann::json& object, const char* key, std::int64_t fallback) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<std::int64_t>() : fallback;
}

} // namespace

PageProbeSample parse_page_probe_reply(const std::string& result_json) {
    PageProbeSample sample;
    const auto reply = nlohmann::json::parse(result_json, nullptr, /*allow_exceptions=*/false);
    if (!reply.is_object() || integer_field(reply, "v", 0) != 1) return sample;
    sample.parsed = true;
    sample.location = string_field(reply, "location");
    sample.ready_state = string_field(reply, "ready");
    sample.visibility = string_field(reply, "visibility");
    const auto focus = reply.find("focus");
    sample.has_focus = focus != reply.end() && focus->is_boolean() && focus->get<bool>();
    sample.root_children = static_cast<int>(integer_field(reply, "root", -1));
    sample.frames = integer_field(reply, "frames", 0);
    return sample;
}

PageHealthVerdict judge_page_health(const PageProbeSample& first,
                                    const PageProbeSample& second,
                                    const HostWindowState& window) {
    if (!first.parsed || !second.parsed || second.root_children < 0) {
        return PageHealthVerdict::NotAppPage;
    }
    if (second.ready_state == "complete" && first.root_children == 0 &&
        second.root_children == 0) {
        return PageHealthVerdict::BlankPage;
    }
    if (window.visible && !window.minimized && second.frames <= first.frames) {
        return PageHealthVerdict::NotDrawing;
    }
    return PageHealthVerdict::Healthy;
}

PageHealthAction action_for_page_health(PageHealthVerdict verdict) {
    switch (verdict) {
    case PageHealthVerdict::BlankPage:
        return PageHealthAction::ReloadPage;
    case PageHealthVerdict::NoReply:
        return PageHealthAction::RestartRenderer;
    case PageHealthVerdict::ControllerUnavailable:
        return PageHealthAction::WebviewLost;
    case PageHealthVerdict::Healthy:
    case PageHealthVerdict::NotDrawing:
    case PageHealthVerdict::NotAppPage:
        return PageHealthAction::None;
    }
    return PageHealthAction::None;
}

const char* page_health_verdict_name(PageHealthVerdict verdict) {
    switch (verdict) {
    case PageHealthVerdict::Healthy: return "healthy";
    case PageHealthVerdict::NotDrawing: return "not-drawing";
    case PageHealthVerdict::BlankPage: return "blank-page";
    case PageHealthVerdict::NotAppPage: return "not-app-page";
    case PageHealthVerdict::NoReply: return "no-reply";
    case PageHealthVerdict::ControllerUnavailable: return "controller-unavailable";
    }
    return "unknown";
}

std::string describe_page_probe(const PageProbeSample& first, const PageProbeSample& second) {
    if (!second.parsed) return "reply=unrecognized";
    return "visibility=" + second.visibility +
           " frames=" + std::to_string(first.frames) + "->" + std::to_string(second.frames) +
           " root=" + std::to_string(second.root_children) +
           " ready=" + second.ready_state +
           " focus=" + (second.has_focus ? "1" : "0") +
           " location=" + second.location;
}

const char* host_wake_event_name(HostWakeEvent event) {
    switch (event) {
    case HostWakeEvent::SessionUnlock: return "session-unlock";
    case HostWakeEvent::RemoteConnect: return "remote-connect";
    case HostWakeEvent::ConsoleConnect: return "console-connect";
    case HostWakeEvent::PowerResume: return "power-resume";
    case HostWakeEvent::RendererUnresponsive: return "renderer-unresponsive";
    }
    return "unknown";
}

SessionChange classify_session_change(std::uint32_t wts_code) {
    // 数值即 WTS_CONSOLE_CONNECT .. WTS_SESSION_UNLOCK(winuser.h),写成字面量
    // 让这份判定在非 Windows 平台的单测里也能编译。
    SessionChange change;
    change.known = true;
    switch (wts_code) {
    case 0x1:
        change.name = "console-connect";
        change.connects = true;
        change.wakes = true;
        change.wake = HostWakeEvent::ConsoleConnect;
        return change;
    case 0x2:
        change.name = "console-disconnect";
        change.disconnects = true;
        return change;
    case 0x3:
        change.name = "remote-connect";
        change.connects = true;
        change.wakes = true;
        change.wake = HostWakeEvent::RemoteConnect;
        return change;
    case 0x4:
        change.name = "remote-disconnect";
        change.disconnects = true;
        return change;
    case 0x5: change.name = "logon"; return change;
    case 0x6: change.name = "logoff"; return change;
    case 0x7:
        change.name = "lock";
        change.locks = true;
        return change;
    case 0x8:
        change.name = "unlock";
        change.unlocks = true;
        change.wakes = true;
        change.wake = HostWakeEvent::SessionUnlock;
        return change;
    default:
        change.known = false;
        return change;
    }
}

bool page_probe_allowed(bool page_loaded, std::int64_t page_age_ms) {
    return page_loaded && page_age_ms >= kMinimumProbedPageAgeMs;
}

WebviewLossAction decide_webview_loss(const WebviewLossContext& context) {
    if (context.session_ending) return WebviewLossAction::Ignore;
    const bool user_at_window = !context.session_locked && context.window_visible &&
                                !context.window_minimized && context.window_foreground;
    return user_at_window ? WebviewLossAction::Recover : WebviewLossAction::Defer;
}

bool admit_recovery_restart(std::vector<std::int64_t>& history,
                            std::int64_t now_ms,
                            RecoveryRestartLimits limits) {
    history.erase(std::remove_if(history.begin(), history.end(),
                                 [now_ms, limits](std::int64_t at_ms) {
                                     return now_ms - at_ms >= limits.window_ms;
                                 }),
                  history.end());
    if (static_cast<int>(history.size()) >= limits.max_restarts) return false;
    history.push_back(now_ms);
    return true;
}

} // namespace acecode::desktop
