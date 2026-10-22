#include "agent_browser_macos_helpers.hpp"

#include "agent_browser_host.hpp"

#ifdef __APPLE__

#include "agent_browser_cookie_jars.hpp"
#include "agent_browser_navigation_state.hpp"
#include "ipc/agent_browser_runtime.hpp"
#include "ipc/agent_browser_store_state.hpp"

#include "platform/process/os_process.hpp"
#include "utils/atomic_file.hpp"
#include "utils/base64.hpp"
#include "utils/logger.hpp"
#include "utils/token.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <QuartzCore/QuartzCore.h>
#import <WebKit/WebKit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "utils/joining_thread.hpp"
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>


namespace acecode::desktop::agent_browser_macos {

std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void assign_error(std::string* target, const std::string& value) {
    if (target) *target = value;
}

NSString* ns_string(const std::string& value, NSString* fallback) {
    if (value.empty()) return fallback;
    NSString* converted = [[[NSString alloc]
        initWithBytes:value.data()
               length:value.size()
             encoding:NSUTF8StringEncoding] autorelease];
    return converted ?: fallback;
}

std::string utf8_string(id value) {
    if (![value isKindOfClass:[NSString class]]) return {};
    const char* text = [(NSString*)value UTF8String];
    return text ? std::string(text) : std::string();
}

std::string clipped(std::string value, std::size_t max_bytes) {
    if (value.size() <= max_bytes) return value;
    value.resize(max_bytes);
    value += "\n[truncated]";
    return value;
}

std::string native_description(id value) {
    if (!value) return {};
    NSString* description = [value isKindOfClass:[NSString class]]
        ? static_cast<NSString*>(value)
        : [value description];
    return clipped(utf8_string(description), kMaxNativeErrorFieldBytes);
}

json native_error_details(NSError* error, std::size_t depth) {
    if (!error) {
        return {
            {"domain", "unknown"},
            {"code", 0},
            {"description", "WKWebView navigation failed"},
        };
    }

    json details{
        {"domain", native_description([error domain])},
        {"code", static_cast<long long>([error code])},
        {"description", native_description([error localizedDescription])},
    };
    const std::string failure_reason =
        native_description([error localizedFailureReason]);
    const std::string recovery_suggestion =
        native_description([error localizedRecoverySuggestion]);
    if (!failure_reason.empty()) details["failure_reason"] = failure_reason;
    if (!recovery_suggestion.empty()) {
        details["recovery_suggestion"] = recovery_suggestion;
    }

    NSDictionary* user_info = [error userInfo];
    id failing_url = [user_info objectForKey:@"NSErrorFailingURLKey"];
    if (!failing_url) {
        failing_url = [user_info objectForKey:@"NSErrorFailingURLStringKey"];
    }
    const std::string failing_url_text = native_description(failing_url);
    if (!failing_url_text.empty()) details["failing_url"] = failing_url_text;

    std::vector<std::string> user_info_keys;
    for (id key in user_info) {
        const std::string key_text = native_description(key);
        if (!key_text.empty()) user_info_keys.push_back(key_text);
    }
    std::sort(user_info_keys.begin(), user_info_keys.end());
    if (!user_info_keys.empty()) details["user_info_keys"] = user_info_keys;
    details["peer_trust_present"] =
        [user_info objectForKey:@"NSURLErrorFailingURLPeerTrustErrorKey"] != nil;

    id underlying = [user_info objectForKey:NSUnderlyingErrorKey];
    if (depth + 1 < kMaxUnderlyingErrorDepth &&
        [underlying isKindOfClass:[NSError class]] && underlying != error) {
        details["underlying"] = native_error_details(
            static_cast<NSError*>(underlying), depth + 1);
    }
    return details;
}

void append_native_error_diagnostic(std::ostringstream& output,
                                    const json& details,
                                    std::size_t depth) {
    output << (depth == 0 ? "NSError" : "Underlying NSError")
           << " domain=" << details.value("domain", "unknown")
           << " code=" << details.value("code", 0LL) << '\n';
    output << "Description: "
           << details.value("description", "WKWebView navigation failed")
           << '\n';
    for (const auto* field : {"failing_url", "failure_reason",
                              "recovery_suggestion"}) {
        const auto found = details.find(field);
        if (found == details.end() || !found->is_string() || found->empty()) {
            continue;
        }
        if (std::strcmp(field, "failing_url") == 0) output << "Failing URL: ";
        else if (std::strcmp(field, "failure_reason") == 0) {
            output << "Failure reason: ";
        } else {
            output << "Recovery suggestion: ";
        }
        output << found->get<std::string>() << '\n';
    }
    const auto keys = details.find("user_info_keys");
    if (keys != details.end() && keys->is_array() && !keys->empty()) {
        output << "User info keys: ";
        bool first = true;
        for (const auto& key : *keys) {
            if (!key.is_string()) continue;
            if (!first) output << ", ";
            output << key.get<std::string>();
            first = false;
        }
        output << '\n';
    }
    if (details.value("peer_trust_present", false)) {
        output << "Peer trust: present\n";
    }
    const auto underlying = details.find("underlying");
    if (underlying != details.end() && underlying->is_object()) {
        append_native_error_diagnostic(output, *underlying, depth + 1);
    }
}

std::string native_error_diagnostic(NSError* error) {
    std::ostringstream output;
    append_native_error_diagnostic(output, native_error_details(error));
    return clipped(output.str(), kMaxNativeErrorDiagnosticBytes);
}

NSError* url_loading_error(NSError* error) {
    NSError* current = error;
    for (std::size_t depth;
         current && depth < kMaxUnderlyingErrorDepth;
         ++depth) {
        NSString* domain = [current domain];
        if ([domain isEqualToString:NSURLErrorDomain] ||
            [domain isEqualToString:@"kCFErrorDomainCFNetwork"]) {
            return current;
        }
        id underlying = [[current userInfo] objectForKey:NSUnderlyingErrorKey];
        current = [underlying isKindOfClass:[NSError class]]
            ? static_cast<NSError*>(underlying) : nil;
    }
    return error;
}

bool valid_favicon(const std::string& value) {
    if (value.empty() || value.size() > kMaxFaviconBytes) return false;
    return value.rfind("https://", 0) == 0 ||
        value.rfind("http://", 0) == 0 ||
        value.rfind("data:image/", 0) == 0;
}

NSString* safari_compatible_application_name() {
    NSString* safari_version = nil;
    NSURL* safari_url = [[NSWorkspace sharedWorkspace]
        URLForApplicationWithBundleIdentifier:@"com.apple.Safari"];
    if (safari_url) {
        NSBundle* safari_bundle = [NSBundle bundleWithURL:safari_url];
        id version = [safari_bundle
            objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
        if ([version isKindOfClass:[NSString class]] &&
            [(NSString*)version length] > 0) {
            NSCharacterSet* invalid = [[NSCharacterSet
                characterSetWithCharactersInString:@"0123456789."]
                invertedSet];
            if ([(NSString*)version rangeOfCharacterFromSet:invalid].location ==
                NSNotFound) {
                safari_version = (NSString*)version;
            }
        }
    }

    // WKWebView uses the system WebKit engine but omits Safari product tokens
    // from its default user agent. Some sites consequently serve legacy
    // downgrade pages that can loop under WebKit's HTTPS upgrade policy.
    // Preserve WebKit's generated platform/engine fields and append the local
    // Safari identity through the public configuration API.
    return safari_version
        ? [NSString stringWithFormat:@"Version/%@ Safari/605.1.15",
                                     safari_version]
        : @"Safari/605.1.15";
}

const char* favicon_expression() {
    return R"JS((() => {
  const links = [...document.querySelectorAll('link[rel][href]')];
  const icon = links.find((link) => String(link.rel || '').toLowerCase()
    .split(/\s+/).includes('icon'));
  let href = icon?.href || '';
  if (!href && (location.protocol === 'http:' || location.protocol === 'https:')) {
    href = new URL('/favicon.ico', location.href).href;
  }
  if (!href) return '';
  try {
    const parsed = new URL(href, location.href);
    return ['http:', 'https:', 'data:'].includes(parsed.protocol)
      ? parsed.href.slice(0, 262144) : '';
  } catch (_) { return ''; }
})())JS";
}

const char* console_capture_script() {
    return R"JS((() => {
  if (globalThis.__acecodeConsoleCaptureV1) return;
  globalThis.__acecodeConsoleCaptureV1 = true;
  const post = (level, values) => {
    try {
      const text = values.map((value) => {
        if (typeof value === 'string') return value;
        if (value instanceof Error) return `${value.name}: ${value.message}\n${value.stack || ''}`;
        try { return JSON.stringify(value); } catch (_) { return String(value); }
      }).join(' ').slice(0, 16384);
      globalThis.webkit?.messageHandlers?.__acecodeAgentBrowserConsoleV1
        ?.postMessage({ level, text, timestamp: Date.now() });
    } catch (_) {}
  };
  for (const level of ['debug', 'info', 'log', 'warn', 'error']) {
    const original = console[level];
    console[level] = function(...values) {
      post(level, values);
      return original.apply(this, values);
    };
  }
  addEventListener('error', (event) => post('error', [event.error || event.message]));
  addEventListener('unhandledrejection', (event) => post('error', [event.reason]));
})())JS";
}

const char* element_picker_expression() {
    return R"JS((() => {
  const key = '__acecodeAgentBrowserElementPickerV1';
  const previous = globalThis[key];
  if (previous && typeof previous.cancel === 'function') previous.cancel();
  return new Promise((resolve) => {
    const root = document.documentElement || document.body;
    if (!root) { resolve({cancelled:true}); return; }
    let current = null;
    let finished = false;
    const overlay = document.createElement('div');
    overlay.style.cssText = 'display:none;position:fixed;box-sizing:border-box;border:2px solid #0e70c0;background:rgba(14,112,192,.14);pointer-events:none;z-index:2147483647';
    root.appendChild(overlay);
    const elementAt = (x, y) => document.elementsFromPoint(x, y)
      .find((value) => value !== overlay && !overlay.contains(value));
    const render = (element) => {
      current = element || null;
      if (!current) { overlay.style.display = 'none'; return; }
      const rect = current.getBoundingClientRect();
      Object.assign(overlay.style, {display:'block',left:`${rect.left}px`,top:`${rect.top}px`,width:`${rect.width}px`,height:`${rect.height}px`});
    };
    const collect = (element) => {
      const rect = element.getBoundingClientRect();
      const attributes = {};
      [...element.attributes].slice(0, 100).forEach((item) => {
        attributes[item.name] = String(item.value || '').slice(0, 2048);
      });
      const tag = String(element.tagName || '').toLowerCase();
      const name = `${tag}${element.id ? `#${element.id}` : ''}` +
        [...element.classList].slice(0, 4).map((value) => `.${value}`).join('');
      return {
        url: location.href,
        title: document.title,
        name,
        tagName: tag,
        outerHTML: String(element.outerHTML || '').slice(0, 40000),
        innerText: String(element.innerText || element.textContent || '').slice(0, 20000),
        attributes,
        bounds: {x:rect.left,y:rect.top,width:rect.width,height:rect.height},
        dimensions: {top:rect.top,left:rect.left,width:rect.width,height:rect.height},
      };
    };
    const suppress = (event) => { event.preventDefault(); event.stopImmediatePropagation(); };
    const onMove = (event) => { suppress(event); render(elementAt(event.clientX, event.clientY)); };
    const onClick = (event) => {
      suppress(event);
      const target = current || elementAt(event.clientX, event.clientY);
      finish(target ? {cancelled:false,element:collect(target)} : {cancelled:true});
    };
    const onKey = (event) => {
      suppress(event);
      if (event.key === 'Escape') finish({cancelled:true});
    };
    const cleanup = () => {
      removeEventListener('pointermove', onMove, true);
      removeEventListener('pointerdown', suppress, true);
      removeEventListener('click', onClick, true);
      removeEventListener('contextmenu', suppress, true);
      removeEventListener('keydown', onKey, true);
      overlay.remove();
      try { delete globalThis[key]; } catch (_) { globalThis[key] = undefined; }
    };
    const finish = (value) => {
      if (finished) return;
      finished = true;
      cleanup();
      resolve(value);
    };
    addEventListener('pointermove', onMove, true);
    addEventListener('pointerdown', suppress, true);
    addEventListener('click', onClick, true);
    addEventListener('contextmenu', suppress, true);
    addEventListener('keydown', onKey, true);
    globalThis[key] = {cancel:() => finish({cancelled:true})};
  });
})())JS";
}

std::string navigation_failure_kind(NSError* error,
                                    bool proxy_authentication_challenge) {
    error = url_loading_error(error);
    if (!error) return "unexpected";
    switch ([error code]) {
    case NSURLErrorTimedOut: return "timeout";
    case NSURLErrorCannotFindHost:
    case NSURLErrorDNSLookupFailed:
        return "name_not_resolved";
    case NSURLErrorCannotConnectToHost: return "cannot_connect";
    case NSURLErrorResourceUnavailable:
    case NSURLErrorCannotLoadFromNetwork:
        return "server_unreachable";
    case NSURLErrorNetworkConnectionLost: return "connection_reset";
    case NSURLErrorNotConnectedToInternet: return "disconnected";
    case NSURLErrorCancelled: return "connection_aborted";
    case NSURLErrorHTTPTooManyRedirects:
    case NSURLErrorRedirectToNonExistentLocation:
        return "redirect_failed";
    case NSURLErrorBadServerResponse:
    case NSURLErrorCannotDecodeRawData:
    case NSURLErrorCannotDecodeContentData:
    case NSURLErrorCannotParseResponse:
    case NSURLErrorZeroByteResource:
        return "invalid_response";
    case NSURLErrorUserCancelledAuthentication:
    case NSURLErrorUserAuthenticationRequired:
        return proxy_authentication_challenge
            ? "proxy_authentication_required" : "authentication_required";
    case NSURLErrorAppTransportSecurityRequiresSecureConnection:
        return "app_transport_security";
    case NSURLErrorSecureConnectionFailed:
        return "secure_connection_failed";
    case NSURLErrorClientCertificateRejected:
    case NSURLErrorClientCertificateRequired:
        return "authentication_required";
    case NSURLErrorServerCertificateUntrusted:
    case NSURLErrorServerCertificateHasBadDate:
    case NSURLErrorServerCertificateHasUnknownRoot:
    case NSURLErrorServerCertificateNotYetValid:
        return "certificate";
    default: return "unexpected";
    }
}

const char* navigation_type_name(WKNavigationType type) {
    switch (type) {
    case WKNavigationTypeLinkActivated: return "link_activated";
    case WKNavigationTypeFormSubmitted: return "form_submitted";
    case WKNavigationTypeBackForward: return "back_forward";
    case WKNavigationTypeReload: return "reload";
    case WKNavigationTypeFormResubmitted: return "form_resubmitted";
    case WKNavigationTypeOther: return "other";
    default: return "unknown";
    }
}

bool safe_navigation_url(NSURL* url) {
    if (!url) return true;
    NSString* scheme_value = [[url scheme] lowercaseString];
    if (!scheme_value) return true;
    return [scheme_value isEqualToString:@"http"] ||
        [scheme_value isEqualToString:@"https"] ||
        [scheme_value isEqualToString:@"file"] ||
        ([scheme_value isEqualToString:@"about"] &&
         [[[url absoluteString] lowercaseString] isEqualToString:@"about:blank"]);
}

// WKNavigation 对象指针即导航身份:同一次导航的 start/finish/fail 回调拿到的是
// 同一个对象,而被取代的旧导航拿到的是另一个。nil 折算成 0,由纯逻辑层按「当前
// 导航」处理。
std::uint64_t navigation_identity(WKNavigation* navigation) {
    return static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(navigation));
}

// 可以交给操作系统打开的外部 scheme。http/https/file/about 由 WebView 自己承载;
// javascript/data/blob 不代表外部应用,交给系统既无意义也不安全。
bool external_handoff_candidate_url(NSURL* url) {
    if (!url) return false;
    NSString* scheme_value = [[url scheme] lowercaseString];
    if (!scheme_value || [scheme_value length] == 0) return false;
    for (NSString* blocked in @[@"javascript", @"data", @"about", @"blob",
                                @"http", @"https", @"file"]) {
        if ([scheme_value isEqualToString:blocked]) return false;
    }
    return true;
}

void load_agent_browser_request(WKWebView* webview, NSURLRequest* request) {
    if (!webview || !request.URL) return;
    if ([request.URL isFileURL]) {
        NSURL* root_read_access =
            [NSURL fileURLWithPath:@"/" isDirectory:YES];
        [webview loadFileURL:request.URL
            allowingReadAccessToURL:root_read_access];
        return;
    }
    [webview loadRequest:request];
}

std::string profile_identifier(const std::string& acecode_dir) {
    const auto path = agent_browser_macos_profile_identifier_path(acecode_dir);
    std::ifstream input(path, std::ios::binary);
    std::string value;
    if (input) std::getline(input, value);
    NSUUID* parsed = value.empty()
        ? nil : [[[NSUUID alloc] initWithUUIDString:ns_string(value)] autorelease];
    if (parsed) return value;

    value = acecode::generate_uuid();
    if (!acecode::atomic_write_file(
            acecode::path_to_utf8(path), value, true)) {
        return {};
    }
    return value;
}

int bounded_poll_timeout(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return static_cast<int>((std::max)(
        std::int64_t{0}, (std::min)(std::int64_t{50}, remaining)));
}

bool socket_transfer(int socket_fd,
                     void* buffer,
                     std::size_t size,
                     bool write,
                     const std::atomic<bool>& stopping,
                     std::chrono::steady_clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < size && !stopping.load() &&
           std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{socket_fd, static_cast<short>(write ? POLLOUT : POLLIN), 0};
        const int polled = ::poll(
            &descriptor, 1, bounded_poll_timeout(deadline));
        if (polled < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (polled == 0) continue;
        if ((descriptor.revents & descriptor.events) == 0) return false;
        const std::size_t chunk = (std::min)(
            size - offset, static_cast<std::size_t>(1024u * 1024u));
        const ssize_t transferred = write
            ? ::send(socket_fd, static_cast<const char*>(buffer) + offset,
                     chunk, MSG_NOSIGNAL)
            : ::recv(socket_fd, static_cast<char*>(buffer) + offset,
                     chunk, 0);
        if (transferred > 0) {
            offset += static_cast<std::size_t>(transferred);
        } else if (transferred == 0) {
            return false;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return false;
        }
    }
    return offset == size;
}

CGEventFlags native_modifier_flags(int modifiers) {
    CGEventFlags flags = 0;
    if ((modifiers & 1) != 0) flags |= kCGEventFlagMaskAlternate;
    if ((modifiers & 2) != 0) flags |= kCGEventFlagMaskControl;
    if ((modifiers & 4) != 0) flags |= kCGEventFlagMaskCommand;
    if ((modifiers & 8) != 0) flags |= kCGEventFlagMaskShift;
    return flags;
}

std::optional<CGKeyCode> native_key_code(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    static const std::unordered_map<std::string, CGKeyCode> codes{
        {"a", 0}, {"s", 1}, {"d", 2}, {"f", 3}, {"h", 4}, {"g", 5},
        {"z", 6}, {"x", 7}, {"c", 8}, {"v", 9}, {"b", 11}, {"q", 12},
        {"w", 13}, {"e", 14}, {"r", 15}, {"y", 16}, {"t", 17},
        {"1", 18}, {"2", 19}, {"3", 20}, {"4", 21}, {"6", 22},
        {"5", 23}, {"9", 25}, {"7", 26}, {"8", 28}, {"0", 29},
        {"o", 31}, {"u", 32}, {"i", 34}, {"p", 35}, {"l", 37},
        {"j", 38}, {"k", 40}, {"n", 45}, {"m", 46},
        {"enter", 36}, {"return", 36}, {"tab", 48}, {"space", 49},
        {"backspace", 51}, {"delete", 51}, {"escape", 53},
        {"left", 123}, {"arrowleft", 123}, {"right", 124},
        {"arrowright", 124}, {"down", 125}, {"arrowdown", 125},
        {"up", 126}, {"arrowup", 126}, {"home", 115}, {"end", 119},
        {"pageup", 116}, {"pagedown", 121},
    };
    const auto found = codes.find(key);
    return found == codes.end() ? std::nullopt
                                : std::optional<CGKeyCode>(found->second);
}

bool request_native_input_permission() {
    NSDictionary* options = @{
        (NSString*)kAXTrustedCheckOptionPrompt: @YES,
    };
    return AXIsProcessTrustedWithOptions((CFDictionaryRef)options) == TRUE;
}


} // namespace acecode::desktop::agent_browser_macos
#endif
