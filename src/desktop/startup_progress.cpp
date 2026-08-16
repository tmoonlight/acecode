#include "startup_progress.hpp"

#include "locale.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>

namespace acecode::desktop {
namespace {

constexpr std::size_t kMaxStartupHistory = 32;
constexpr double kMaxFrontendPerformanceMs = 24.0 * 60.0 * 60.0 * 1000.0;

std::uint64_t steady_now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

nlohmann::json event_json(const DesktopStartupEvent& event) {
    nlohmann::json out{
        {"sequence", event.sequence},
        {"stage", event.stage},
        {"message", event.message},
        {"source", event.source},
        {"elapsed_ms", event.elapsed_ms},
        {"terminal", event.terminal},
    };
    if (event.frontend_ms.has_value()) {
        out["frontend_ms"] = *event.frontend_ms;
    }
    return out;
}

void set_parse_error(std::string* error, std::string value) {
    if (error) *error = std::move(value);
}

const nlohmann::json* first_bridge_argument(const nlohmann::json& value) {
    if (value.is_array()) {
        if (value.empty()) return nullptr;
        return &value.front();
    }
    return &value;
}

const std::array<const char*, 6> kFrontendStages{
    "web_bootstrap",
    "first_contentful_paint",
    "daemon_connecting",
    "daemon_connected",
    "daemon_connection_failed",
    "ui_ready",
};

} // namespace

DesktopStartupTimeline::DesktopStartupTimeline(NowMsFn now_ms)
    : now_ms_(now_ms ? std::move(now_ms) : NowMsFn(steady_now_ms)) {
    started_ms_ = now_ms_();
}

DesktopStartupEvent DesktopStartupTimeline::record(
    std::string stage,
    std::string message,
    std::string source,
    bool terminal,
    std::optional<double> frontend_ms) {
    const std::uint64_t now = now_ms_();
    const std::uint64_t measured = now >= started_ms_ ? now - started_ms_ : 0;
    const std::uint64_t elapsed = std::max(last_elapsed_ms_, measured);
    last_elapsed_ms_ = elapsed;

    DesktopStartupEvent event;
    event.sequence = next_sequence_++;
    event.stage = std::move(stage);
    event.message = std::move(message);
    event.source = std::move(source);
    event.elapsed_ms = elapsed;
    event.frontend_ms = frontend_ms;
    event.terminal = terminal;

    if (history_.size() == kMaxStartupHistory) {
        history_.erase(history_.begin());
    }
    history_.push_back(event);
    return event;
}

bool DesktopStartupTimeline::empty() const {
    return history_.empty();
}

const DesktopStartupEvent* DesktopStartupTimeline::latest() const {
    return history_.empty() ? nullptr : &history_.back();
}

const std::vector<DesktopStartupEvent>& DesktopStartupTimeline::history() const {
    return history_;
}

std::string DesktopStartupTimeline::snapshot_json() const {
    nlohmann::json history = nlohmann::json::array();
    for (const auto& event : history_) history.push_back(event_json(event));
    nlohmann::json snapshot{
        {"version", kDesktopStartupProgressVersion},
        {"history", std::move(history)},
    };
    const DesktopStartupEvent* current = latest();
    if (current && !current->terminal) {
        const auto terminal = std::find_if(
            history_.rbegin(), history_.rend(),
            [](const DesktopStartupEvent& event) { return event.terminal; });
        if (terminal != history_.rend()) current = &*terminal;
    }
    if (current) {
        snapshot["current"] = event_json(*current);
    } else {
        snapshot["current"] = nullptr;
    }
    return snapshot.dump();
}

bool is_frontend_startup_stage(const std::string& stage) {
    return std::find(kFrontendStages.begin(), kFrontendStages.end(), stage) !=
           kFrontendStages.end();
}

bool is_terminal_startup_stage(const std::string& stage) {
    return stage == "ui_ready";
}

std::optional<FrontendStartupMilestone> parse_frontend_startup_milestone(
    const std::string& args_json,
    std::string* error) {
    try {
        nlohmann::json value = nlohmann::json::parse(args_json);
        const nlohmann::json* payload = first_bridge_argument(value);
        if (!payload || !payload->is_object()) {
            set_parse_error(error, "startup milestone object required");
            return std::nullopt;
        }

        const auto stage_it = payload->find("stage");
        if (stage_it == payload->end() || !stage_it->is_string()) {
            set_parse_error(error, "startup milestone stage required");
            return std::nullopt;
        }
        const std::string stage = stage_it->get<std::string>();
        if (!is_frontend_startup_stage(stage)) {
            set_parse_error(error, "startup milestone stage is not allowed");
            return std::nullopt;
        }

        std::optional<double> performance_ms;
        const auto performance_it = payload->find("performance_ms");
        if (performance_it != payload->end() && !performance_it->is_null()) {
            if (!performance_it->is_number()) {
                set_parse_error(error, "startup milestone performance_ms must be numeric");
                return std::nullopt;
            }
            const double parsed = performance_it->get<double>();
            if (!std::isfinite(parsed) || parsed < 0.0 ||
                parsed > kMaxFrontendPerformanceMs) {
                set_parse_error(error, "startup milestone performance_ms out of range");
                return std::nullopt;
            }
            performance_ms = parsed;
        }

        return FrontendStartupMilestone{stage, performance_ms};
    } catch (const std::exception& e) {
        set_parse_error(error, std::string("startup milestone parse failed: ") + e.what());
        return std::nullopt;
    }
}

std::string desktop_startup_stage_message(
    const std::string& stage,
    const std::string& locale) {
    const bool zh = locale != kLocaleEnUs;
    if (zh) {
        if (stage == "desktop_starting") return u8"正在启动 ACECode…";
        if (stage == "config_load_begin") return u8"正在读取配置…";
        if (stage == "config_load_end") return u8"配置已加载";
        if (stage == "workspace_scan_begin") return u8"正在扫描工作区…";
        if (stage == "workspace_scan_end") return u8"工作区扫描完成";
        if (stage == "daemon_activate_begin") return u8"正在启动后台服务…";
        if (stage == "daemon_activate_end") return u8"后台服务已就绪";
        if (stage == "daemon_activate_failed") return u8"后台服务启动失败";
        if (stage == "workspace_register_begin") return u8"正在注册工作区…";
        if (stage == "workspace_register_end") return u8"工作区已就绪";
        if (stage == "workspace_register_failed") return u8"工作区注册失败";
        if (stage == "webhost_create_begin") return u8"正在初始化界面…";
        if (stage == "webhost_create_end") return u8"界面环境已就绪";
        if (stage == "webhost_create_failed") return u8"界面环境初始化失败";
        if (stage == "native_shell_ready") return u8"正在准备桌面组件…";
        if (stage == "webview_navigate_begin") return u8"正在加载首屏…";
        if (stage == "dom_ready") return u8"正在显示首屏…";
        if (stage == "web_bootstrap") return u8"正在初始化前端…";
        if (stage == "first_contentful_paint") return u8"正在绘制首屏…";
        if (stage == "daemon_connecting") return u8"正在连接后台服务…";
        if (stage == "daemon_connected") return u8"后台服务已连接";
        if (stage == "daemon_connection_failed") return u8"无法连接后台服务";
        if (stage == "ui_ready") return u8"启动完成";
        return u8"正在启动…";
    }

    if (stage == "desktop_starting") return "Starting ACECode...";
    if (stage == "config_load_begin") return "Loading configuration...";
    if (stage == "config_load_end") return "Configuration loaded";
    if (stage == "workspace_scan_begin") return "Scanning workspaces...";
    if (stage == "workspace_scan_end") return "Workspace scan complete";
    if (stage == "daemon_activate_begin") return "Starting background service...";
    if (stage == "daemon_activate_end") return "Background service ready";
    if (stage == "daemon_activate_failed") return "Background service failed to start";
    if (stage == "workspace_register_begin") return "Registering workspace...";
    if (stage == "workspace_register_end") return "Workspace ready";
    if (stage == "workspace_register_failed") return "Workspace registration failed";
    if (stage == "webhost_create_begin") return "Initializing interface...";
    if (stage == "webhost_create_end") return "Interface environment ready";
    if (stage == "webhost_create_failed") return "Interface initialization failed";
    if (stage == "native_shell_ready") return "Preparing desktop components...";
    if (stage == "webview_navigate_begin") return "Loading first screen...";
    if (stage == "dom_ready") return "Showing first screen...";
    if (stage == "web_bootstrap") return "Initializing frontend...";
    if (stage == "first_contentful_paint") return "Drawing first screen...";
    if (stage == "daemon_connecting") return "Connecting to background service...";
    if (stage == "daemon_connected") return "Background service connected";
    if (stage == "daemon_connection_failed") return "Unable to connect to background service";
    if (stage == "ui_ready") return "Startup complete";
    return "Starting...";
}

} // namespace acecode::desktop
