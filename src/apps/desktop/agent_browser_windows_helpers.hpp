#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent_browser_cookie_jars.hpp"
#include "agent_browser_host.hpp"

namespace acecode::desktop::agent_browser_windows {
inline constexpr std::size_t kAgentBrowserMaxConsoleEntries = 1000;
inline constexpr std::size_t kAgentBrowserMaxConsoleEntryBytes = 16 * 1024;
inline constexpr std::size_t kAgentBrowserMaxFaviconBytes = 256 * 1024;
inline constexpr char kAgentBrowserElementPickerKey[] =
    "__acecodeAgentBrowserElementPickerV1";

// 隐藏的临时 controller:没有页面时借它拿到某个 Profile(清空、删除、复制
// cookie);最后一个引用释放时关闭。
struct ProfileLease {
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview;
    ProfileLease() = default;
    ProfileLease(const ProfileLease&) = delete;
    ProfileLease& operator=(const ProfileLease&) = delete;
    ~ProfileLease() {
        webview.Reset();
        if (controller) controller->Close();
    }
};
using ProfileCallback = std::function<void(
    const Microsoft::WRL::ComPtr<ICoreWebView2Profile>& profile,
    const std::string& error,
    const std::shared_ptr<ProfileLease>& lease)>;
using StoreDone = std::function<void(bool ok, const std::string& error)>;


bool valid_agent_browser_favicon(const std::string& value);
const char* agent_browser_favicon_expression();
bool agent_browser_external_handoff_candidate(const std::string& uri);
const char* agent_browser_web_error_kind(
    COREWEBVIEW2_WEB_ERROR_STATUS status);
const char* agent_browser_process_failure_kind(
    COREWEBVIEW2_PROCESS_FAILED_KIND kind,
    COREWEBVIEW2_PROCESS_FAILED_REASON reason);
std::string clip_agent_browser_text(std::string value,
                                    std::size_t max_bytes);
std::string cdp_remote_object_text(const nlohmann::json& object);
const char* agent_browser_element_picker_expression();
HWND create_agent_browser_widget(HWND parent);
void park_agent_browser_widget(HWND widget);
void hide_agent_browser_widget(HWND widget);
POINT main_web_content_origin(HWND parent);
bool apply_agent_browser_widget_region(
    HWND widget,
    int width,
    int height,
    const std::vector<AgentBrowserOcclusionRect>& occlusions);
std::string hresult_text(HRESULT result);
bool proxy_aborted(const std::atomic<bool>& stopping,
                   std::chrono::steady_clock::time_point deadline);
bool pipe_transfer(HANDLE pipe,
                   void* buffer,
                   std::size_t size,
                   bool write,
                   const std::atomic<bool>& stopping,
                   std::chrono::steady_clock::time_point deadline);
bool connect_pipe(HANDLE pipe,
                  const std::atomic<bool>& stopping);
bool remove_session_profile_directory(
    const std::filesystem::path& user_data_path,
    const std::string& session_id);
HRESULT create_controller_for_store(
    const Microsoft::WRL::ComPtr<ICoreWebView2Environment>& environment,
    HWND browser_widget,
    const AgentBrowserCookieStoreKey& key,
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler);
Microsoft::WRL::ComPtr<ICoreWebView2Profile> profile_of(
    const Microsoft::WRL::ComPtr<ICoreWebView2>& webview);
void clear_profile(const Microsoft::WRL::ComPtr<ICoreWebView2Profile>& profile,
                          const std::shared_ptr<ProfileLease>& lease,
                          const StoreDone& done);
Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> cookie_manager_of(
    const Microsoft::WRL::ComPtr<ICoreWebView2Profile>& profile);

void copy_profile_cookies(
    const Microsoft::WRL::ComPtr<ICoreWebView2CookieManager>& source,
    const Microsoft::WRL::ComPtr<ICoreWebView2CookieManager>& target,
    const std::shared_ptr<ProfileLease>& lease,
    const std::string& parent_session_id,
    const std::function<void()>& done);

} // namespace acecode::desktop::agent_browser_windows
#endif
