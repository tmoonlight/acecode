#pragma once

#ifdef __APPLE__
#include "agent_browser_capture.hpp"

@class WKWebView;

namespace acecode::desktop::browser_capture {

using Evaluated = std::function<void(bool, nlohmann::json, std::string)>;
using Evaluate = std::function<void(const std::string&, Evaluated)>;

void capture_webkit(std::shared_ptr<WKWebView> view, std::string page_id,
                    const nlohmann::json& params, Evaluate evaluate,
                    Current current, Complete complete, Validate validate = {});

}
#endif
