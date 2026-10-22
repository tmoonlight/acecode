#pragma once
namespace acecode::desktop {
class WebHost;
class AgentBrowserHost;
// Host and browser are owned by main and outlive their registered callbacks.
void install_agent_browser_cookie_bridge(WebHost& host, AgentBrowserHost& agent_browser);
} // namespace acecode::desktop
