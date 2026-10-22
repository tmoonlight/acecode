#include "agent_browser_cookie_bridge.hpp"
#include "agent_browser_host.hpp"
#include "web_host.hpp"
#include <nlohmann/json.hpp>

namespace acecode::desktop {
void install_agent_browser_cookie_bridge(WebHost& host, AgentBrowserHost& agent_browser) {
    // cookie 存储的异步结果(切档、清除浏览数据、删除会话存储)推给前端;
    // 同步 bridge 只能返回「已开始」。
    agent_browser.set_storage_event_handler(
        [&host](const acecode::desktop::AgentBrowserStorageEvent& event) {
            const nlohmann::json detail{
                {"type", event.type},
                {"ok", event.ok},
                {"cookie_mode", event.cookie_mode},
                {"store", event.store},
                {"page_id", event.page_id},
                {"error", event.error},
            };
            host.eval(
                "(function(){try{window.dispatchEvent(new CustomEvent("
                "'acecode:agent-browser-storage',{detail:" + detail.dump() +
                "}));}catch(e){}})();");
        });
    // 「保留 Cookie」档位:until_quit / shared / per_session(没选过 = shared)。
    host.bind("aceDesktop_agentBrowserGetCookieMode",
              [&agent_browser](const std::string& /*req*/) -> std::string {
        return nlohmann::json{
            {"ok", true},
            {"supported", agent_browser.supported()},
            {"mode", agent_browser.cookie_mode()},
        }.dump();
    });

    // 参数 [mode:string]。切档会按原地址重建存储变了的页面;切到 until_quit 会清空
    // 已保存的 cookie。
    host.bind("aceDesktop_agentBrowserSetCookieMode",
              [&agent_browser](const std::string& req) -> std::string {
        const auto args = nlohmann::json::parse(req, nullptr, false);
        if (!args.is_array() || args.empty() || !args[0].is_string()) {
            return nlohmann::json{
                {"ok", false}, {"error", "expect [mode:string]"}
            }.dump();
        }
        std::string error;
        if (!agent_browser.set_cookie_mode(args[0].get<std::string>(), &error)) {
            return nlohmann::json{{"ok", false}, {"error", error}}.dump();
        }
        return nlohmann::json{
            {"ok", true},
            {"mode", agent_browser.cookie_mode()},
        }.dump();
    });

    // 参数 [page_id:string]:清除该页当前所用存储的全部网站数据。异步完成,结果经
    // acecode:agent-browser-storage 事件(type=browsing_data_cleared)回报。
    host.bind("aceDesktop_agentBrowserClearBrowsingData",
              [&agent_browser](const std::string& req) -> std::string {
        const auto args = nlohmann::json::parse(req, nullptr, false);
        if (!args.is_array() || args.empty() || !args[0].is_string()) {
            return nlohmann::json{
                {"ok", false}, {"error", "expect [page_id:string]"}
            }.dump();
        }
        const std::string page_id = args[0].get<std::string>();
        std::string error;
        if (!agent_browser.clear_browsing_data(page_id, &error)) {
            return nlohmann::json{{"ok", false}, {"error", error}}.dump();
        }
        return nlohmann::json{
            {"ok", true},
            {"started", true},
            {"page_id", page_id},
            {"mode", agent_browser.cookie_mode()},
        }.dump();
    });
}
} // namespace acecode::desktop
