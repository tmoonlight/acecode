// aceDesktop_iosSimulatorView* bridges between the web panel and the native
// iOS Simulator view.
#include "ios_simulator_view.hpp"

#include "utils/logger.hpp"
#include "web_host.hpp"

namespace acecode::desktop {
namespace {

std::string failure(const std::string& message) { return nlohmann::json{{"ok", false}, {"error", message}}.dump(); }

std::string with_ok(nlohmann::json state) {
    state["ok"] = true;
    return state.dump();
}

IosSimulatorView::StateHandler state_dispatcher(WebHost& host) {
    auto last_error = std::make_shared<std::string>();
    return [&host, last_error](const nlohmann::json& state) {
        const auto error = state.value("error", std::string());
        if (error != *last_error) {
            *last_error = error;
            if (!error.empty()) LOG_WARN("[ios-sim] native view: " + error);
        }
        host.eval("(function(){try{window.dispatchEvent(new CustomEvent('acecode:ios-simulator-view-state',"
                  "{detail:" + state.dump(-1, ' ', true) + "}));}catch(e){}})();");
    };
}

} // namespace

std::unique_ptr<IosSimulatorView> install_ios_simulator_view(WebHost& host) {
    auto owned = std::make_unique<IosSimulatorView>(host.native_window(), state_dispatcher(host));
    IosSimulatorView& view = *owned;
    host.bind("aceDesktop_iosSimulatorViewGetState",
              [&view](const std::string& /*req*/) -> std::string { return with_ok(view.state()); });
    host.bind("aceDesktop_iosSimulatorViewSetLayout", [&view](const std::string& req) -> std::string {
        const auto args = nlohmann::json::parse(req, nullptr, false);
        if (!args.is_array() || args.empty()) return failure("expect [layout:object]");
        std::string error;
        const auto layout = parse_ios_simulator_view_layout(args[0], &error);
        if (!layout) return failure(error);
        if (!view.set_layout(*layout, &error)) return failure(error);
        return with_ok(view.state());
    });
    host.bind("aceDesktop_iosSimulatorViewUnmount", [&view](const std::string& /*req*/) -> std::string {
        view.unmount();
        return with_ok(view.state());
    });
    return owned;
}

} // namespace acecode::desktop
