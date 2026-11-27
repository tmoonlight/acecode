#pragma once

#ifdef _WIN32
#include "agent_browser_windows_helpers.hpp"
#include "agent_browser_capture_cdp.hpp"

namespace acecode::desktop::agent_browser_windows {

using NativeView = Microsoft::WRL::ComPtr<ICoreWebView2>;
using AnnotationResult = std::function<void(bool, nlohmann::json)>;

void capture_command(const NativeView& webview, const std::string& method,
                     const nlohmann::json& params, browser_capture::CommandResult completion);
void evaluate_annotation(NativeView webview, browser_capture::Current current,
                         std::string expression, AnnotationResult completion);
void proxy_command(const NativeView& webview, std::string page_id,
                   const std::string& method, const nlohmann::json& params,
                   browser_capture::Complete completion);
nlohmann::json selected_element(HRESULT result, LPCWSTR raw);
bool open_developer_tools(const NativeView& webview, std::string* error);

}
#endif
