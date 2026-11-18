#pragma once

#include "utils/lifetime_token.hpp"

namespace acecode::desktop {

class WebHost;
class AgentBrowserHost;

[[nodiscard]] acecode::LifetimeToken install_page_annotation_bridge(
    WebHost& host, AgentBrowserHost& browser);

} // namespace acecode::desktop
