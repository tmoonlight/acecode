#include "agent_browser_native_commands_win.hpp"
#include "agent_browser_host_common.hpp"

#ifdef _WIN32
#include "utils/encoding.hpp"

#include <cwchar>

namespace acecode::desktop::agent_browser_windows {
bool open_developer_tools(const NativeView& webview, std::string* error) {
    const HRESULT result = webview->OpenDevToolsWindow();
    if (FAILED(result)) {
        agent_browser_common::assign_error(error, "failed to open WebView2 developer tools (" +
                                hresult_text(result) + ")");
        return false;
    }
    return true;
}

namespace {

void annotation_cdp(const NativeView& webview, const browser_capture::Current& current,
                    const wchar_t* method, const nlohmann::json& params,
                    AnnotationResult completion) {
    const auto serialized = acecode::utf8_to_wide(params.dump());
    auto callback = Microsoft::WRL::Callback<
        ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
        [current, completion](HRESULT status, LPCWSTR raw) -> HRESULT {
            if (!current()) {
                completion(false, nullptr);
                return S_OK;
            }
            const auto result = raw ? nlohmann::json::parse(
                acecode::wide_to_utf8(raw), nullptr, false) : nlohmann::json();
            completion(SUCCEEDED(status) && result.is_object(), result);
            return S_OK;
        });
    const auto status = webview->CallDevToolsProtocolMethod(
        method, serialized.c_str(), callback.Get());
    if (FAILED(status)) completion(false, nullptr);
}

}

void evaluate_annotation(NativeView webview, browser_capture::Current current,
                         std::string expression, AnnotationResult completion) {
    annotation_cdp(webview, current, L"Page.getFrameTree", nlohmann::json::object(),
        [webview, current, expression, completion](bool ok, nlohmann::json result) {
            if (!ok || !current() ||
                !result.contains("frameTree") || !result["frameTree"].is_object() ||
                !result["frameTree"].contains("frame") || !result["frameTree"]["frame"].is_object() ||
                !result["frameTree"]["frame"].contains("id") ||
                !result["frameTree"]["frame"]["id"].is_string()) {
                completion(false, nullptr);
                return;
            }
            const nlohmann::json params{
                {"frameId", result["frameTree"]["frame"]["id"]},
                {"worldName", "acecode-page-annotations-v1"},
                {"grantUniveralAccess", false},
            };
            annotation_cdp(webview, current, L"Page.createIsolatedWorld", params,
                [webview, current, expression, completion](bool created, nlohmann::json world) {
                    if (!created || !current() ||
                        !world.contains("executionContextId") ||
                        !world["executionContextId"].is_number_integer()) {
                        completion(false, nullptr);
                        return;
                    }
                    annotation_cdp(webview, current, L"Runtime.evaluate", {
                        {"expression", expression}, {"contextId", world["executionContextId"]},
                        {"returnByValue", true}, {"awaitPromise", true},
                    }, [completion](bool evaluated, nlohmann::json response) {
                        if (!evaluated || response.contains("exceptionDetails") ||
                            !response.contains("result") || !response["result"].is_object()) {
                            completion(false, nullptr);
                            return;
                        }
                        completion(true, response["result"].value("value", nlohmann::json()));
                    });
                });
        });
}

void capture_command(const NativeView& webview, const std::string& method,
                     const nlohmann::json& params, browser_capture::CommandResult completion) {
    const bool screenshot = method == "Page.captureScreenshot";
    const auto callback = Microsoft::WRL::Callback<
        ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
        [completion, screenshot](HRESULT result, LPCWSTR raw) -> HRESULT {
            if (FAILED(result)) {
                completion({}, "capture_native_failed: " + hresult_text(result));
                return S_OK;
            }
            constexpr auto max_response = ((browser_capture::kMaxPngBytes + 2) / 3) * 4 + 1024;
            if (screenshot && raw && wcsnlen(raw, max_response + 1) > max_response) {
                completion({}, "capture_resource_limit");
                return S_OK;
            }
            try {
                auto value = nlohmann::json::parse(
                    raw ? acecode::wide_to_utf8(raw) : "", nullptr, false);
                if (!value.is_object() || value.contains("error")) {
                    completion({}, "capture_native_failed");
                } else {
                    completion(std::move(value), {});
                }
            } catch (const std::bad_alloc&) {
                completion({}, "capture_resource_limit");
            }
            return S_OK;
        });
    const auto wide_method = acecode::utf8_to_wide(method);
    const auto wide_params = acecode::utf8_to_wide(params.dump());
    const HRESULT started = webview->CallDevToolsProtocolMethod(
        wide_method.c_str(), wide_params.c_str(), callback.Get());
    if (FAILED(started)) completion({}, "capture_dispatch_failed");
}

void proxy_command(const NativeView& webview, std::string page_id,
                   const std::string& method, const nlohmann::json& params,
                   browser_capture::Complete completion) {
    const std::wstring wide_method = acecode::utf8_to_wide(method);
    const std::wstring wide_params = acecode::utf8_to_wide(
        (params.is_object() ? params : nlohmann::json::object()).dump());
    auto completed = Microsoft::WRL::Callback<
        ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
        [completion, page_id](HRESULT result, LPCWSTR raw) -> HRESULT {
            if (FAILED(result)) {
                completion({{"ok", false}, {"page_id", page_id},
                    {"error", "WebView2 CDP call failed (" + hresult_text(result) + ")"}});
                return S_OK;
            }
            const std::string text = raw ? acecode::wide_to_utf8(raw) : std::string("{}");
            auto value = nlohmann::json::parse(text, nullptr, false);
            if (value.is_discarded()) {
                completion({{"ok", false}, {"page_id", page_id},
                    {"error", "WebView2 returned malformed CDP JSON"}});
                return S_OK;
            }
            completion({{"ok", true}, {"page_id", page_id}, {"result", std::move(value)}});
            return S_OK;
        });
    const HRESULT started = webview->CallDevToolsProtocolMethod(
        wide_method.c_str(), wide_params.c_str(), completed.Get());
    if (FAILED(started)) {
        completion({{"ok", false}, {"page_id", page_id},
            {"error", "failed to dispatch WebView2 CDP call (" + hresult_text(started) + ")"}});
    }
}

nlohmann::json selected_element(HRESULT result, LPCWSTR raw) {
    nlohmann::json selected;
    if (SUCCEEDED(result) && raw) {
        auto response = nlohmann::json::parse(acecode::wide_to_utf8(raw), nullptr, false);
        if (!response.is_discarded() && response.is_object()) {
            const auto remote = response.value("result", nlohmann::json::object());
            if (remote.is_object() && remote.contains("value") && remote["value"].is_object()) {
                const auto& value = remote["value"];
                if (!value.value("cancelled", true) && value.contains("element") &&
                    value["element"].is_object()) {
                    selected = value["element"];
                }
            }
        }
    }
    return selected;
}

}
#endif
