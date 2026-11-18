#include "page_annotation_bridge.hpp"
#include "agent_browser_host.hpp"
#include "web_host.hpp"

#include <nlohmann/json.hpp>

namespace acecode::desktop {

acecode::LifetimeToken install_page_annotation_bridge(WebHost& host, AgentBrowserHost& browser) {
    acecode::LifetimeToken lifetime;
    host.bind_async("aceDesktop_agentBrowserAnnotationCommand",
        [browser = lifetime.ref(browser)](
            const std::string& req, WebHost::BindingReply reply) {
            const auto args = nlohmann::json::parse(req, nullptr, false);
            if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
                !args[0].contains("page_id") || !args[0]["page_id"].is_string() ||
                !args[0].contains("command") || !args[0]["command"].is_object()) {
                reply("{\"ok\":false,\"error\":\"Invalid annotation request\"}");
                return;
            }
            const bool delivered = browser.with([&](AgentBrowserHost& current) {
                current.annotation_command(args[0]["page_id"].get<std::string>(),
                                           args[0]["command"].dump(), reply);
            });
            if (!delivered) reply("{\"ok\":false,\"error\":\"Browser closed\"}");
        });
    return lifetime;
}

} // namespace acecode::desktop
