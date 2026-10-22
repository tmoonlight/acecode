#pragma once
#ifdef __APPLE__
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <WebKit/WebKit.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
namespace acecode::desktop::agent_browser_macos {
using json = nlohmann::json;

inline constexpr std::size_t kMaxConsoleEntries = 1000;
inline constexpr std::size_t kMaxConsoleEntryBytes = 16 * 1024;
inline constexpr std::size_t kMaxFaviconBytes = 256 * 1024;
inline constexpr std::size_t kMaxNativeErrorFieldBytes = 4 * 1024;
inline constexpr std::size_t kMaxNativeErrorDiagnosticBytes = 16 * 1024;
inline constexpr std::size_t kMaxUnderlyingErrorDepth = 4;
inline constexpr char kConsoleHandlerName[] = "__acecodeAgentBrowserConsoleV1";
inline constexpr char kAutomationWorldName[] = "dev.acecode.agent-browser.automation";
inline constexpr char kElementPickerKey[] = "__acecodeAgentBrowserElementPickerV1";

std::int64_t now_unix_ms();
void assign_error(std::string* target, const std::string& value);
NSString* ns_string(const std::string& value, NSString* fallback = @"");
std::string utf8_string(id value);
std::string clipped(std::string value, std::size_t max_bytes);
std::string native_description(id value);
json native_error_details(NSError* error, std::size_t depth = 0);
void append_native_error_diagnostic(std::ostringstream& output,
                                    const json& details,
                                    std::size_t depth = 0);
std::string native_error_diagnostic(NSError* error);
NSError* url_loading_error(NSError* error);
bool valid_favicon(const std::string& value);
NSString* safari_compatible_application_name();
const char* favicon_expression();
const char* console_capture_script();
const char* element_picker_expression();
std::string navigation_failure_kind(NSError* error,
                                    bool proxy_authentication_challenge);
const char* navigation_type_name(WKNavigationType type);
bool safe_navigation_url(NSURL* url);
std::uint64_t navigation_identity(WKNavigation* navigation);
bool external_handoff_candidate_url(NSURL* url);
void load_agent_browser_request(WKWebView* webview, NSURLRequest* request);
std::string profile_identifier(const std::string& acecode_dir);
int bounded_poll_timeout(std::chrono::steady_clock::time_point deadline);
bool socket_transfer(int socket_fd,
                     void* buffer,
                     std::size_t size,
                     bool write,
                     const std::atomic<bool>& stopping,
                     std::chrono::steady_clock::time_point deadline);
CGEventFlags native_modifier_flags(int modifiers);
std::optional<CGKeyCode> native_key_code(std::string key);
bool request_native_input_permission();

} // namespace acecode::desktop::agent_browser_macos
#endif
