#include "agent_browser_host.hpp"

#ifdef __APPLE__

#include "agent_browser_runtime.hpp"

#include "daemon/platform.hpp"
#include "utils/atomic_file.hpp"
#include "utils/base64.hpp"
#include "utils/logger.hpp"
#include "utils/token.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
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
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::desktop {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxConsoleEntries = 1000;
constexpr std::size_t kMaxConsoleEntryBytes = 16 * 1024;
constexpr std::size_t kMaxFaviconBytes = 256 * 1024;
constexpr char kConsoleHandlerName[] = "__acecodeAgentBrowserConsoleV1";
constexpr char kAutomationWorldName[] = "dev.acecode.agent-browser.automation";
constexpr char kElementPickerKey[] = "__acecodeAgentBrowserElementPickerV1";

std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void assign_error(std::string* target, const std::string& value) {
    if (target) *target = value;
}

NSString* ns_string(const std::string& value, NSString* fallback = @"") {
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

bool valid_favicon(const std::string& value) {
    if (value.empty() || value.size() > kMaxFaviconBytes) return false;
    return value.rfind("https://", 0) == 0 ||
        value.rfind("http://", 0) == 0 ||
        value.rfind("data:image/", 0) == 0;
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

std::string navigation_failure_kind(NSError* error) {
    if (!error) return "unexpected";
    switch ([error code]) {
    case NSURLErrorTimedOut: return "timeout";
    case NSURLErrorCannotFindHost: return "name_not_resolved";
    case NSURLErrorCannotConnectToHost: return "cannot_connect";
    case NSURLErrorNetworkConnectionLost: return "connection_reset";
    case NSURLErrorNotConnectedToInternet: return "disconnected";
    case NSURLErrorCancelled: return "cancelled";
    case NSURLErrorServerCertificateUntrusted:
    case NSURLErrorServerCertificateHasBadDate:
    case NSURLErrorServerCertificateHasUnknownRoot:
    case NSURLErrorServerCertificateNotYetValid:
        return "certificate";
    default: return "unexpected";
    }
}

bool safe_navigation_url(NSURL* url) {
    if (!url) return true;
    NSString* scheme_value = [[url scheme] lowercaseString];
    if (!scheme_value) return true;
    return [scheme_value isEqualToString:@"http"] ||
        [scheme_value isEqualToString:@"https"] ||
        ([scheme_value isEqualToString:@"about"] &&
         [[[url absoluteString] lowercaseString] isEqualToString:@"about:blank"]);
}

class MacAgentBrowserDelegateSink {
public:
    virtual ~MacAgentBrowserDelegateSink() = default;
    virtual void navigation_started(const std::string&, WKWebView*) = 0;
    virtual void navigation_finished(const std::string&, WKWebView*) = 0;
    virtual void navigation_failed(const std::string&, WKWebView*, NSError*) = 0;
    virtual void page_identity_changed(const std::string&, WKWebView*) = 0;
    virtual void process_terminated(const std::string&) = 0;
    virtual void console_message(const std::string&, id) = 0;
    virtual void load_popup_in_page(const std::string&, NSURLRequest*) = 0;
    virtual void store_alert(const std::string&, NSString*, void (^)(void)) = 0;
    virtual void store_confirm(const std::string&, NSString*, void (^)(BOOL)) = 0;
    virtual void store_prompt(const std::string&, NSString*, NSString*,
                              void (^)(NSString*)) = 0;
};

} // namespace
} // namespace acecode::desktop

@interface ACECodeAgentBrowserDelegate
    : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler> {
@public
    acecode::desktop::MacAgentBrowserDelegateSink* owner_;
    NSString* page_id_;
}
- (instancetype)initWithOwner:(acecode::desktop::MacAgentBrowserDelegateSink*)owner
                        pageID:(NSString*)pageID;
@end

@implementation ACECodeAgentBrowserDelegate

static void* kACECodeAgentBrowserStateObservation =
    &kACECodeAgentBrowserStateObservation;

- (instancetype)initWithOwner:(acecode::desktop::MacAgentBrowserDelegateSink*)owner
                        pageID:(NSString*)pageID {
    self = [super init];
    if (self) {
        owner_ = owner;
        page_id_ = [pageID copy];
    }
    return self;
}

- (void)dealloc {
    owner_ = nullptr;
    [page_id_ release];
    [super dealloc];
}

- (std::string)pageIDValue {
    return acecode::desktop::utf8_string(page_id_);
}

- (void)webView:(WKWebView*)webView didStartProvisionalNavigation:(WKNavigation*)navigation {
    (void)navigation;
    if (owner_) owner_->navigation_started([self pageIDValue], webView);
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    (void)navigation;
    if (owner_) owner_->navigation_finished([self pageIDValue], webView);
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                      withError:(NSError*)error {
    (void)navigation;
    if (owner_) owner_->navigation_failed([self pageIDValue], webView, error);
}

- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
             withError:(NSError*)error {
    (void)navigation;
    if (owner_) owner_->navigation_failed([self pageIDValue], webView, error);
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView {
    (void)webView;
    if (owner_) owner_->process_terminated([self pageIDValue]);
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context {
    (void)keyPath;
    (void)change;
    if (context == kACECodeAgentBrowserStateObservation && owner_ &&
        [object isKindOfClass:[WKWebView class]]) {
        owner_->page_identity_changed(
            [self pageIDValue], static_cast<WKWebView*>(object));
        return;
    }
    [super observeValueForKeyPath:keyPath
                         ofObject:object
                           change:change
                          context:context];
}

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)navigationAction
                   decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    (void)webView;
    NSURL* url = navigationAction.request.URL;
    decisionHandler(acecode::desktop::safe_navigation_url(url)
        ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}

- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)navigationAction
                    windowFeatures:(WKWindowFeatures*)windowFeatures {
    (void)webView;
    (void)configuration;
    (void)windowFeatures;
    if (!navigationAction.targetFrame && owner_) {
        owner_->load_popup_in_page([self pageIDValue], navigationAction.request);
    }
    return nil;
}

- (void)userContentController:(WKUserContentController*)userContentController
      didReceiveScriptMessage:(WKScriptMessage*)message {
    (void)userContentController;
    if (owner_) owner_->console_message([self pageIDValue], message.body);
}

- (void)webView:(WKWebView*)webView
    runJavaScriptAlertPanelWithMessage:(NSString*)message
                      initiatedByFrame:(WKFrameInfo*)frame
                     completionHandler:(void (^)(void))completionHandler {
    (void)webView;
    (void)frame;
    if (owner_) owner_->store_alert([self pageIDValue], message, completionHandler);
    else completionHandler();
}

- (void)webView:(WKWebView*)webView
    runJavaScriptConfirmPanelWithMessage:(NSString*)message
                        initiatedByFrame:(WKFrameInfo*)frame
                       completionHandler:(void (^)(BOOL))completionHandler {
    (void)webView;
    (void)frame;
    if (owner_) owner_->store_confirm([self pageIDValue], message, completionHandler);
    else completionHandler(NO);
}

- (void)webView:(WKWebView*)webView
    runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt
                              defaultText:(NSString*)defaultText
                         initiatedByFrame:(WKFrameInfo*)frame
                        completionHandler:(void (^)(NSString*))completionHandler {
    (void)webView;
    (void)frame;
    if (owner_) {
        owner_->store_prompt([self pageIDValue], prompt, defaultText,
                             completionHandler);
    } else {
        completionHandler(nil);
    }
}

@end

namespace acecode::desktop {
namespace {

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

} // namespace

struct AgentBrowserHost::Impl final
    : public MacAgentBrowserDelegateSink,
      public std::enable_shared_from_this<AgentBrowserHost::Impl> {
    bool parent_surface_visible = true;

    struct PendingProxyCall {
        std::mutex mutex;
        std::condition_variable ready;
        bool completed = false;
        json response;
    };

    struct Page {
        std::string id;
        AgentBrowserState state;
        AgentBrowserBounds requested_bounds;
        WKWebView* webview = nil;
        ACECodeAgentBrowserDelegate* delegate = nil;
        std::vector<std::string> console_logs;
        std::vector<WKBackForwardListItem*> history_items;
        std::uint64_t element_selection_generation = 0;
        bool closing = false;
        bool observing_state = false;
        std::string dialog_kind;
        std::string dialog_message;
        void (^alert_completion)(void) = nil;
        void (^confirm_completion)(BOOL) = nil;
        void (^prompt_completion)(NSString*) = nil;
    };

    void* parent_window = nullptr;
    std::int64_t desktop_pid = 0;
    std::string desktop_instance_id;
    std::string acecode_dir;
    StateHandler state_handler;
    DispatchHandler dispatch_handler;
    mutable std::mutex state_mutex;
    AgentBrowserState host_state;
    std::unordered_map<std::string, std::shared_ptr<Page>> pages;
    std::vector<std::string> page_order;
    std::string active_page;
    std::uint64_t next_page_sequence = 0;
    WKWebsiteDataStore* data_store = nil;
    WKContentWorld* automation_world = nil;
    std::string proxy_socket_path;
    std::string proxy_auth_token;
    std::atomic<bool> proxy_stopping{false};
    std::atomic<int> listener_fd{-1};
    std::thread proxy_thread;

    Impl(void* parent,
         std::int64_t pid,
         std::string instance_id,
         StateHandler handler,
         DispatchHandler dispatcher,
         std::string data_dir)
        : parent_window(parent),
          desktop_pid(pid),
          desktop_instance_id(std::move(instance_id)),
          acecode_dir(std::move(data_dir)),
          state_handler(std::move(handler)),
          dispatch_handler(std::move(dispatcher)) {
        host_state.supported = false;
        if (@available(macOS 14.0, *)) {
            host_state.supported = parent_window != nullptr;
        } else {
            host_state.error = "Agent Browser requires macOS 14 or newer";
        }
    }

    ~Impl() override {
        proxy_stopping.store(true);
        const int listening = listener_fd.exchange(-1);
        if (listening >= 0) {
            ::shutdown(listening, SHUT_RDWR);
            ::close(listening);
        }
        if (proxy_thread.joinable()) proxy_thread.join();
        if (!proxy_socket_path.empty()) ::unlink(proxy_socket_path.c_str());

        std::vector<std::shared_ptr<Page>> remaining;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            for (const auto& item : pages) remaining.push_back(item.second);
            pages.clear();
            page_order.clear();
            active_page.clear();
        }
        for (const auto& page : remaining) teardown_page(page);
        [automation_world release];
        [data_store release];
        cleanup_agent_browser_runtime_manifest(desktop_instance_id, acecode_dir);
    }

    void emit_state(const AgentBrowserState& state) const {
        if (state_handler) state_handler(state);
    }

    AgentBrowserState state(const std::string& requested_page = {}) const {
        std::lock_guard<std::mutex> lock(state_mutex);
        const std::string id = requested_page.empty() ? active_page : requested_page;
        const auto found = pages.find(id);
        return found == pages.end() ? host_state : found->second->state;
    }

    std::vector<AgentBrowserState> states() const {
        std::vector<AgentBrowserState> result;
        std::lock_guard<std::mutex> lock(state_mutex);
        result.reserve(page_order.size());
        for (const auto& id : page_order) {
            const auto found = pages.find(id);
            if (found != pages.end()) result.push_back(found->second->state);
        }
        return result;
    }

    std::string active_page_id() const {
        std::lock_guard<std::mutex> lock(state_mutex);
        return active_page;
    }

    std::shared_ptr<Page> find_page(const std::string& requested_page) const {
        std::lock_guard<std::mutex> lock(state_mutex);
        const std::string id = requested_page.empty() ? active_page : requested_page;
        const auto found = pages.find(id);
        return found == pages.end() ? nullptr : found->second;
    }

    void update_host_state(const std::function<void(AgentBrowserState&)>& update) {
        AgentBrowserState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            update(host_state);
            snapshot = host_state;
        }
        emit_state(snapshot);
    }

    void update_page(const std::shared_ptr<Page>& page,
                     const std::function<void(AgentBrowserState&)>& update) {
        if (!page) return;
        AgentBrowserState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->closing && !page->state.closed) return;
            update(page->state);
            snapshot = page->state;
        }
        emit_state(snapshot);
    }

    void fail(const std::string& message) {
        LOG_ERROR("[agent-browser] " + message);
        update_host_state([&](AgentBrowserState& value) {
            value.ready = false;
            value.error = message;
        });
    }

    static void finish_proxy_call(
        const std::shared_ptr<PendingProxyCall>& pending,
        json response) {
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            if (pending->completed) return;
            pending->completed = true;
            pending->response = std::move(response);
        }
        pending->ready.notify_all();
    }

    void start() {
        if (!host_state.supported) return;
        NSWindow* window = static_cast<NSWindow*>(parent_window);
        if (!window || ![window contentView]) {
            fail("Agent Browser parent window is unavailable");
            return;
        }
        if (!dispatch_handler) {
            fail("Agent Browser UI dispatcher is unavailable");
            return;
        }

        const std::string identifier_value = profile_identifier(acecode_dir);
        if (identifier_value.empty()) {
            fail("failed to create Agent Browser macOS profile identifier");
            return;
        }
        NSUUID* identifier = [[[NSUUID alloc]
            initWithUUIDString:ns_string(identifier_value)] autorelease];
        if (!identifier) {
            fail("Agent Browser macOS profile identifier is invalid");
            return;
        }
        data_store = [[WKWebsiteDataStore dataStoreForIdentifier:identifier] retain];
        automation_world = [[WKContentWorld worldWithName:
            ns_string(kAutomationWorldName)] retain];
        if (!data_store || !automation_world) {
            fail("failed to initialize Agent Browser WKWebView resources");
            return;
        }
        if (!publish_proxy()) return;
        update_host_state([](AgentBrowserState& value) {
            value.supported = true;
            value.ready = true;
            value.error.clear();
        });
        LOG_INFO("[agent-browser] WKWebView host ready; macOS proxy published");
    }

    void clear_dialog(const std::shared_ptr<Page>& page, bool dismiss) {
        if (!page) return;
        if (page->alert_completion) {
            auto block = page->alert_completion;
            page->alert_completion = nil;
            if (dismiss) block();
            [block release];
        }
        if (page->confirm_completion) {
            auto block = page->confirm_completion;
            page->confirm_completion = nil;
            if (dismiss) block(NO);
            [block release];
        }
        if (page->prompt_completion) {
            auto block = page->prompt_completion;
            page->prompt_completion = nil;
            if (dismiss) block(nil);
            [block release];
        }
        page->dialog_kind.clear();
        page->dialog_message.clear();
    }

    void clear_history(const std::shared_ptr<Page>& page) {
        for (WKBackForwardListItem* item : page->history_items) [item release];
        page->history_items.clear();
    }

    void teardown_page(const std::shared_ptr<Page>& page) {
        if (!page) return;
        clear_dialog(page, true);
        clear_history(page);
        if (page->webview) {
            if (page->observing_state && page->delegate) {
                for (NSString* key_path in
                     @[@"URL", @"title", @"canGoBack", @"canGoForward"]) {
                    [page->webview removeObserver:page->delegate
                                       forKeyPath:key_path
                                          context:kACECodeAgentBrowserStateObservation];
                }
                page->observing_state = false;
            }
            WKUserContentController* controller =
                page->webview.configuration.userContentController;
            [controller removeScriptMessageHandlerForName:
                ns_string(kConsoleHandlerName)
                                           contentWorld:[WKContentWorld pageWorld]];
            page->webview.navigationDelegate = nil;
            page->webview.UIDelegate = nil;
            [page->webview stopLoading];
            [page->webview removeFromSuperview];
            [page->webview release];
            page->webview = nil;
        }
        if (page->delegate) {
            page->delegate->owner_ = nullptr;
            [page->delegate release];
            page->delegate = nil;
        }
    }

    std::string create_page_on_ui(bool shared_with_agent) {
        auto page = std::make_shared<Page>();
        page->id = "browser-" + std::to_string(desktop_pid) + "-" +
                   std::to_string(++next_page_sequence);
        page->state.page_id = page->id;
        page->state.supported = true;
        page->state.shared_with_agent = shared_with_agent;

        WKWebViewConfiguration* configuration =
            [[[WKWebViewConfiguration alloc] init] autorelease];
        configuration.websiteDataStore = data_store;
        configuration.preferences.javaScriptCanOpenWindowsAutomatically = YES;
        WKUserContentController* content =
            [[[WKUserContentController alloc] init] autorelease];
        configuration.userContentController = content;

        page->delegate = [[ACECodeAgentBrowserDelegate alloc]
            initWithOwner:this pageID:ns_string(page->id)];
        WKUserScript* console_script = [[[WKUserScript alloc]
            initWithSource:ns_string(console_capture_script())
             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
          forMainFrameOnly:NO] autorelease];
        [content addUserScript:console_script];
        [content addScriptMessageHandler:page->delegate
                            contentWorld:[WKContentWorld pageWorld]
                                    name:ns_string(kConsoleHandlerName)];

        page->webview = [[WKWebView alloc]
            initWithFrame:NSZeroRect configuration:configuration];
        page->webview.navigationDelegate = page->delegate;
        page->webview.UIDelegate = page->delegate;
        for (NSString* key_path in
             @[@"URL", @"title", @"canGoBack", @"canGoForward"]) {
            [page->webview addObserver:page->delegate
                            forKeyPath:key_path
                               options:NSKeyValueObservingOptionNew
                               context:kACECodeAgentBrowserStateObservation];
        }
        page->observing_state = true;
        [page->webview setHidden:YES];
        [page->webview setAutoresizingMask:NSViewNotSizable];
        [[static_cast<NSWindow*>(parent_window) contentView]
            addSubview:page->webview positioned:NSWindowAbove relativeTo:nil];
        page->state.ready = page->webview != nil;
        if (!page->webview) page->state.error = "failed to create WKWebView page";

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            pages.emplace(page->id, page);
            page_order.push_back(page->id);
        }
        emit_state(page->state);
        std::string ignored;
        select_page_on_ui(page->id, &ignored);
        return page->id;
    }

    void apply_bounds(const std::shared_ptr<Page>& page) {
        if (!page || !page->webview) return;
        NSView* content = [static_cast<NSWindow*>(parent_window) contentView];
        const auto bounds = page->requested_bounds;
        // The Desktop shell's contentView is itself a WKWebView, whose native
        // coordinate system is flipped (top-left origin), matching DOM
        // getBoundingClientRect(). Other embedders may still provide an
        // ordinary bottom-left NSView. Respect the actual parent coordinate
        // system so a live page does not jump upward when it replaces the
        // React placeholder.
        const CGFloat y = [content isFlipped]
            ? bounds.y
            : NSHeight([content bounds]) - bounds.y - bounds.height;
        [page->webview setFrame:NSMakeRect(
            bounds.x, y, bounds.width, bounds.height)];
        const AgentBrowserState snapshot = state(page->id);
        const bool show = parent_surface_visible && bounds.visible &&
            snapshot.active &&
            snapshot.content_state == kAgentBrowserContentStateLive &&
            bounds.width > 0 && bounds.height > 0;
        [page->webview setHidden:show ? NO : YES];
        if (show) {
            [content addSubview:page->webview
                     positioned:NSWindowAbove relativeTo:nil];
        }
        update_page(page, [&](AgentBrowserState& value) {
            value.visible = show;
        });
    }

    bool select_page_on_ui(const std::string& page_id, std::string* error) {
        auto page = find_page(page_id);
        if (!page || page->closing) {
            assign_error(error, "Agent Browser page was not found");
            return false;
        }
        std::vector<std::shared_ptr<Page>> changed;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            active_page = page_id;
            for (const auto& item : pages) {
                const bool active = item.first == page_id;
                if (item.second->state.active != active) {
                    item.second->state.active = active;
                    changed.push_back(item.second);
                }
            }
        }
        for (const auto& changed_page : changed) {
            emit_state(state(changed_page->id));
            apply_bounds(changed_page);
        }
        return true;
    }

    bool close_page_on_ui(const std::string& requested_page,
                          std::string* closed_page,
                          std::string* error) {
        std::shared_ptr<Page> page;
        std::string next_page;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            const std::string id = requested_page.empty()
                ? active_page : requested_page;
            const auto found = pages.find(id);
            if (found == pages.end()) {
                assign_error(error, "Agent Browser page was not found");
                return false;
            }
            page = found->second;
            page->closing = true;
            auto ordered = std::find(page_order.begin(), page_order.end(), id);
            const std::size_t index = ordered == page_order.end()
                ? 0 : static_cast<std::size_t>(ordered - page_order.begin());
            pages.erase(found);
            if (ordered != page_order.end()) page_order.erase(ordered);
            if (active_page == id) {
                if (!page_order.empty()) {
                    next_page = page_order[(std::min)(index, page_order.size() - 1)];
                }
                active_page = next_page;
            }
        }
        teardown_page(page);
        page->state.closed = true;
        page->state.ready = false;
        page->state.visible = false;
        page->state.active = false;
        emit_state(page->state);
        if (closed_page) *closed_page = page->id;
        if (!next_page.empty()) {
            std::string ignored;
            select_page_on_ui(next_page, &ignored);
        }
        return true;
    }

    bool set_shared_with_agent_on_ui(const std::string& page_id,
                                     bool shared,
                                     std::string* error) {
        auto page = find_page(page_id);
        if (!page || page->closing) {
            assign_error(error, "Agent Browser page was not found");
            return false;
        }
        update_page(page, [&](AgentBrowserState& value) {
            value.shared_with_agent = shared;
        });
        return true;
    }

    bool navigate_on_ui(const std::string& page_id,
                        const std::string& input,
                        std::string* error) {
        auto page = find_page(page_id);
        if (!page || !page->webview || page->closing) {
            assign_error(error, "Agent Browser page is still starting");
            return false;
        }
        auto normalized = normalize_agent_browser_url(input, error);
        if (!normalized) return false;
        if (*normalized == "about:blank") {
            [page->webview loadHTMLString:@"" baseURL:nil];
        } else {
            NSURL* url = [NSURL URLWithString:ns_string(*normalized)];
            if (!url) {
                assign_error(error, "Agent Browser URL is invalid");
                return false;
            }
            [page->webview loadRequest:[NSURLRequest requestWithURL:url]];
        }
        return true;
    }

    bool page_shared_with_agent(const std::shared_ptr<Page>& page) const {
        if (!page) return false;
        std::lock_guard<std::mutex> lock(state_mutex);
        return !page->closing && page->state.shared_with_agent;
    }

    bool require_shared(const std::shared_ptr<Page>& page,
                        const std::string& page_id,
                        const std::shared_ptr<PendingProxyCall>& pending) const {
        if (page_shared_with_agent(page)) return true;
        finish_proxy_call(pending,
            {{"ok", false}, {"page_id", page_id},
             {"error", "page_not_shared_with_agent"}});
        return false;
    }

    // Delegate callbacks run on the WebKit/UI thread.
    void navigation_started(const std::string& page_id,
                            WKWebView* webview) override {
        auto page = find_page(page_id);
        if (!page || page->webview != webview) return;
        page->console_logs.clear();
        const std::string url = utf8_string(webview.URL.absoluteString);
        update_page(page, [&](AgentBrowserState& value) {
            value.loading = true;
            value.visible = false;
            value.content_state = kAgentBrowserContentStateLoading;
            value.failure_kind.clear();
            value.error.clear();
            value.favicon.clear();
            if (!url.empty()) value.url = url;
        });
        apply_bounds(page);
    }

    void refresh_page_identity(const std::shared_ptr<Page>& page) {
        if (!page || !page->webview) return;
        const std::string url = utf8_string(page->webview.URL.absoluteString);
        std::string title = utf8_string(page->webview.title);
        if (title.find_first_not_of(" \t\r\n") == std::string::npos) {
            title = kAgentBrowserDefaultTitle;
        }
        update_page(page, [&](AgentBrowserState& value) {
            value.url = url.empty() ? "about:blank" : url;
            value.title = title;
            value.can_go_back = page->webview.canGoBack == YES;
            value.can_go_forward = page->webview.canGoForward == YES;
        });
    }

    void evaluate_value(const std::shared_ptr<Page>& page,
                        const std::string& expression,
                        std::function<void(bool, json, std::string)> completion) {
        if (!page || !page->webview) {
            completion(false, nullptr, "Agent Browser page is still starting");
            return;
        }
        NSString* body = @"try {\n"
            "  let value = await (0, eval)(expression);\n"
            "  const replacer = (_, item) => typeof item === 'bigint' ? String(item) : item;\n"
            "  return JSON.stringify({ok:true,value:value === undefined ? null : value}, replacer);\n"
            "} catch (error) {\n"
            "  return JSON.stringify({ok:false,error:String(error?.message || error),stack:String(error?.stack || '')});\n"
            "}";
        [page->webview
            callAsyncJavaScript:body
                      arguments:@{@"expression": ns_string(expression)}
                         inFrame:nil
                  inContentWorld:automation_world
               completionHandler:^(id result, NSError* native_error) {
            if (native_error) {
                completion(false, nullptr,
                    utf8_string([native_error localizedDescription]));
                return;
            }
            const std::string text = utf8_string(result);
            json parsed = json::parse(text, nullptr, false);
            if (!parsed.is_object()) {
                completion(false, nullptr,
                    "WKWebView returned malformed automation JSON");
                return;
            }
            if (!parsed.value("ok", false)) {
                completion(false, nullptr,
                    parsed.value("error", "WKWebView JavaScript evaluation failed"));
                return;
            }
            completion(true, parsed.value("value", json(nullptr)), {});
        }];
    }

    void refresh_favicon(const std::shared_ptr<Page>& page) {
        const auto weak = weak_from_this();
        evaluate_value(page, favicon_expression(),
            [weak, page](bool ok, json value, std::string) {
                if (!ok || !value.is_string()) return;
                const auto self = weak.lock();
                if (!self) return;
                const std::string favicon = value.get<std::string>();
                if (!valid_favicon(favicon)) return;
                self->update_page(page, [&](AgentBrowserState& state) {
                    state.favicon = favicon;
                });
            });
    }

    void navigation_finished(const std::string& page_id,
                             WKWebView* webview) override {
        auto page = find_page(page_id);
        if (!page || page->webview != webview) return;
        refresh_page_identity(page);
        update_page(page, [](AgentBrowserState& value) {
            value.loading = false;
            value.content_state = kAgentBrowserContentStateLive;
            value.failure_kind.clear();
            value.error.clear();
        });
        apply_bounds(page);
        refresh_favicon(page);
        // Warm the isolated world after cold WKWebView startup.
        evaluate_value(page, "true", [](bool, json, std::string) {});
    }

    void navigation_failed(const std::string& page_id,
                           WKWebView* webview,
                           NSError* native_error) override {
        auto page = find_page(page_id);
        if (!page || page->webview != webview) return;
        const std::string message = utf8_string(
            [native_error localizedDescription]);
        refresh_page_identity(page);
        update_page(page, [&](AgentBrowserState& value) {
            value.loading = false;
            value.visible = false;
            value.content_state = kAgentBrowserContentStateNavigationError;
            value.failure_kind = navigation_failure_kind(native_error);
            value.error = message.empty() ? "WKWebView navigation failed" : message;
        });
        apply_bounds(page);
    }

    void page_identity_changed(const std::string& page_id,
                               WKWebView* webview) override {
        auto page = find_page(page_id);
        if (page && page->webview == webview && !page->closing) {
            refresh_page_identity(page);
        }
    }

    void process_terminated(const std::string& page_id) override {
        auto page = find_page(page_id);
        if (!page) return;
        update_page(page, [](AgentBrowserState& value) {
            value.loading = false;
            value.visible = false;
            value.content_state = kAgentBrowserContentStateProcessFailed;
            value.failure_kind = "web_content_process_terminated";
            value.error = "WKWebView content process terminated";
        });
        apply_bounds(page);
    }

    void console_message(const std::string& page_id, id body) override {
        auto page = find_page(page_id);
        if (!page || ![body isKindOfClass:[NSDictionary class]]) return;
        NSString* level = [(NSDictionary*)body objectForKey:@"level"];
        NSString* text = [(NSDictionary*)body objectForKey:@"text"];
        std::string entry = "[" + utf8_string(level) + "] " + utf8_string(text);
        entry = clipped(std::move(entry), kMaxConsoleEntryBytes);
        page->console_logs.push_back(std::move(entry));
        if (page->console_logs.size() > kMaxConsoleEntries) {
            page->console_logs.erase(
                page->console_logs.begin(),
                page->console_logs.begin() +
                    static_cast<std::ptrdiff_t>(
                        page->console_logs.size() - kMaxConsoleEntries));
        }
    }

    void load_popup_in_page(const std::string& page_id,
                            NSURLRequest* request) override {
        auto page = find_page(page_id);
        if (page && request.URL && safe_navigation_url(request.URL)) {
            [page->webview loadRequest:request];
        }
    }

    void store_alert(const std::string& page_id,
                     NSString* message,
                     void (^completion)(void)) override {
        auto page = find_page(page_id);
        if (!page) { completion(); return; }
        clear_dialog(page, true);
        page->dialog_kind = "alert";
        page->dialog_message = utf8_string(message);
        page->alert_completion = [completion copy];
    }

    void store_confirm(const std::string& page_id,
                       NSString* message,
                       void (^completion)(BOOL)) override {
        auto page = find_page(page_id);
        if (!page) { completion(NO); return; }
        clear_dialog(page, true);
        page->dialog_kind = "confirm";
        page->dialog_message = utf8_string(message);
        page->confirm_completion = [completion copy];
    }

    void store_prompt(const std::string& page_id,
                      NSString* message,
                      NSString* default_text,
                      void (^completion)(NSString*)) override {
        (void)default_text;
        auto page = find_page(page_id);
        if (!page) { completion(nil); return; }
        clear_dialog(page, true);
        page->dialog_kind = "prompt";
        page->dialog_message = utf8_string(message);
        page->prompt_completion = [completion copy];
    }

    // Remaining proxy, command, and public-operation methods are below.
    void call_command_on_ui(
        const std::string&, const std::string&, const json&,
        const std::shared_ptr<PendingProxyCall>&);
    void run_synthetic_input(
        const std::shared_ptr<Page>&, const std::string&, const json&,
        const std::shared_ptr<PendingProxyCall>&);
    void run_native_input(
        const std::shared_ptr<Page>&, const std::string&, const json&,
        const std::shared_ptr<PendingProxyCall>&);
    void capture_screenshot(
        const std::shared_ptr<Page>&, const json&,
        const std::shared_ptr<PendingProxyCall>&);
    bool prepare_native_input(
        const std::shared_ptr<Page>&, std::string&);
    CGPoint native_screen_point(
        const std::shared_ptr<Page>&, double, double) const;
    bool handle_dialog_on_ui(
        const std::shared_ptr<Page>&, const json&, std::string&);
    bool toggle_element_selection_on_ui(
        const std::string&, std::string*);
    bool publish_proxy();
    void execute_proxy_request_on_ui(
        const json&, const std::shared_ptr<PendingProxyCall>&);
    json execute_proxy_request(const json&);
    void handle_proxy_connection(int);
    void proxy_loop();
};

namespace {

std::string synthetic_mouse_expression(const json& parameters) {
    json payload = parameters.is_object() ? parameters : json::object();
    payload.erase("acecodeInputMode");
    return "(() => {\n"
        " const p=" + payload.dump() + ";\n"
        " const x=Number(p.x||0),y=Number(p.y||0);\n"
        " const target=document.elementFromPoint(x,y)||document.documentElement;\n"
        " if(!target)return {ok:false,message:'No element at input coordinates'};\n"
        " const key='__acecodeSyntheticPointerV1';const state=globalThis[key]||(globalThis[key]={pressed:null,dragging:false,data:null});\n"
        " const names={mouseMoved:'mousemove',mousePressed:'mousedown',mouseReleased:'mouseup',mouseWheel:'wheel'};\n"
        " const button={left:0,middle:1,right:2}[String(p.button||'left')]??0;\n"
        " const buttons=Number(p.buttons==null?(p.type==='mousePressed'?1:0):p.buttons);\n"
        " const init={bubbles:true,cancelable:true,composed:true,clientX:x,clientY:y,screenX:x,screenY:y,button,buttons,detail:Number(p.clickCount||1)};\n"
        " if(p.type==='mouseWheel'){const event=new WheelEvent('wheel',{...init,deltaX:Number(p.deltaX||0),deltaY:Number(p.deltaY||0),deltaMode:0});target.dispatchEvent(event);if(!event.defaultPrevented)window.scrollBy({left:Number(p.deltaX||0),top:Number(p.deltaY||0),behavior:'instant'});return {ok:true};}\n"
        " if(p.type==='mousePressed'){target.focus?.({preventScroll:true});state.pressed=target;state.dragging=false;state.data=null;target.dispatchEvent(new MouseEvent('mousedown',init));return {ok:true};}\n"
        " if(p.type==='mouseMoved'){target.dispatchEvent(new MouseEvent('mousemove',init));if(state.pressed&&buttons){try{if(!state.dragging){state.data=new DataTransfer();state.pressed.dispatchEvent(new DragEvent('dragstart',{...init,dataTransfer:state.data}));state.dragging=true;}target.dispatchEvent(new DragEvent('dragenter',{...init,dataTransfer:state.data}));target.dispatchEvent(new DragEvent('dragover',{...init,dataTransfer:state.data}));}catch(_){}}return {ok:true};}\n"
        " if(p.type==='mouseReleased'){target.dispatchEvent(new MouseEvent('mouseup',init));if(state.dragging){try{target.dispatchEvent(new DragEvent('drop',{...init,dataTransfer:state.data}));state.pressed?.dispatchEvent(new DragEvent('dragend',{...init,dataTransfer:state.data}));}catch(_){}}else if(state.pressed){const clickTarget=state.pressed.isConnected?state.pressed:target;if(button===0&&typeof clickTarget.click==='function')clickTarget.click();else clickTarget.dispatchEvent(new MouseEvent('contextmenu',init));if(Number(p.clickCount||1)>1)clickTarget.dispatchEvent(new MouseEvent('dblclick',init));}state.pressed=null;state.dragging=false;state.data=null;return {ok:true};}\n"
        " target.dispatchEvent(new MouseEvent(names[p.type]||String(p.type||'mousemove'),init));return {ok:true};\n"
        "})()";
}

std::string synthetic_text_expression(const json& parameters) {
    json payload = parameters.is_object() ? parameters : json::object();
    payload.erase("acecodeInputMode");
    return "(() => {\n"
        " const p=" + payload.dump() + ";const text=String(p.text==null?'':p.text);const el=document.activeElement;\n"
        " if(!el||el===document.body)return {ok:false,message:'No focused editable element'};\n"
        " const before=new InputEvent('beforeinput',{bubbles:true,cancelable:true,composed:true,inputType:'insertText',data:text});if(!el.dispatchEvent(before))return {ok:true,cancelled:true};\n"
        " if(el instanceof HTMLInputElement||el instanceof HTMLTextAreaElement){const start=Number.isInteger(el.selectionStart)?el.selectionStart:el.value.length;const end=Number.isInteger(el.selectionEnd)?el.selectionEnd:start;el.setRangeText(text,start,end,'end');}\n"
        " else if(el.isContentEditable){const selection=getSelection();if(selection&&selection.rangeCount){selection.deleteFromDocument();selection.getRangeAt(0).insertNode(document.createTextNode(text));selection.collapseToEnd();}else el.textContent=(el.textContent||'')+text;}\n"
        " else if('value'in el){el.value=String(el.value||'')+text;}else return {ok:false,message:'Focused element is not editable'};\n"
        " el.dispatchEvent(new InputEvent('input',{bubbles:true,composed:true,inputType:'insertText',data:text}));return {ok:true};\n"
        "})()";
}

std::string synthetic_key_expression(const json& parameters) {
    json payload = parameters.is_object() ? parameters : json::object();
    payload.erase("acecodeInputMode");
    return "(() => {\n"
        " const p=" + payload.dump() + ";const el=document.activeElement||document.body;const key=String(p.key||'');const down=p.type==='keyDown'||p.type==='rawKeyDown';const mods=Number(p.modifiers||0);\n"
        " const init={key,code:String(p.code||key),bubbles:true,cancelable:true,composed:true,altKey:!!(mods&1),ctrlKey:!!(mods&2),metaKey:!!(mods&4),shiftKey:!!(mods&8),repeat:false};\n"
        " const event=new KeyboardEvent(down?'keydown':'keyup',init);const proceed=el.dispatchEvent(event);if(!down||!proceed)return {ok:true};\n"
        " if((init.metaKey||init.ctrlKey)&&key.toLowerCase()==='a'&&typeof el.select==='function'){el.select();return {ok:true};}\n"
        " if(key==='Tab'){const nodes=[...document.querySelectorAll('a[href],button,input,textarea,select,[tabindex]:not([tabindex=\"-1\"])')].filter(n=>!n.disabled&&n.getClientRects().length);const index=nodes.indexOf(el);const delta=init.shiftKey?-1:1;const next=nodes[(index+delta+nodes.length)%nodes.length];next?.focus();return {ok:true};}\n"
        " if(key==='Enter'){if(el instanceof HTMLTextAreaElement){el.setRangeText('\\n',el.selectionStart,el.selectionEnd,'end');el.dispatchEvent(new InputEvent('input',{bubbles:true,inputType:'insertLineBreak',data:'\\n'}));}else if(el.form&&typeof el.form.requestSubmit==='function'){el.form.requestSubmit();}else if(typeof el.click==='function'){el.click();}return {ok:true};}\n"
        " if((key==='Backspace'||key==='Delete')&&(el instanceof HTMLInputElement||el instanceof HTMLTextAreaElement)){const start=el.selectionStart??el.value.length;const end=el.selectionEnd??start;const left=key==='Backspace'&&start===end?Math.max(0,start-1):start;const right=key==='Delete'&&start===end?Math.min(el.value.length,end+1):end;el.setRangeText('',left,right,'end');el.dispatchEvent(new InputEvent('input',{bubbles:true,inputType:'deleteContentBackward',data:null}));}\n"
        " return {ok:true};\n"
        "})()";
}

std::string remote_type(const json& value) {
    if (value.is_null()) return "object";
    if (value.is_boolean()) return "boolean";
    if (value.is_number()) return "number";
    if (value.is_string()) return "string";
    return "object";
}

CGMouseButton cg_mouse_button(const std::string& button) {
    if (button == "right") return kCGMouseButtonRight;
    if (button == "middle") return kCGMouseButtonCenter;
    return kCGMouseButtonLeft;
}

CGEventType cg_mouse_event_type(const std::string& type,
                                CGMouseButton button,
                                bool dragging) {
    if (type == "mousePressed") {
        return button == kCGMouseButtonRight ? kCGEventRightMouseDown
            : button == kCGMouseButtonCenter ? kCGEventOtherMouseDown
            : kCGEventLeftMouseDown;
    }
    if (type == "mouseReleased") {
        return button == kCGMouseButtonRight ? kCGEventRightMouseUp
            : button == kCGMouseButtonCenter ? kCGEventOtherMouseUp
            : kCGEventLeftMouseUp;
    }
    if (dragging) {
        return button == kCGMouseButtonRight ? kCGEventRightMouseDragged
            : button == kCGMouseButtonCenter ? kCGEventOtherMouseDragged
            : kCGEventLeftMouseDragged;
    }
    return kCGEventMouseMoved;
}

} // namespace

CGPoint AgentBrowserHost::Impl::native_screen_point(
    const std::shared_ptr<Page>& page,
    double x,
    double y) const {
    if (!page || !page->webview) return CGPointZero;
    const NSRect bounds = [page->webview bounds];
    const CGFloat local_y = [page->webview isFlipped]
        ? y
        : NSHeight(bounds) - y;
    const NSPoint local = NSMakePoint(x, local_y);
    const NSPoint window_point = [page->webview convertPoint:local toView:nil];
    NSWindow* window = [page->webview window];
    const NSPoint screen_point = [window convertPointToScreen:window_point];
    NSScreen* primary = [[NSScreen screens] firstObject];
    const CGFloat primary_top = primary ? NSMaxY([primary frame]) : 0;
    return CGPointMake(screen_point.x, primary_top - screen_point.y);
}

bool AgentBrowserHost::Impl::prepare_native_input(
    const std::shared_ptr<Page>& page,
    std::string& error) {
    if (!page || !page->webview || page->closing) {
        error = "Agent Browser page is still starting";
        return false;
    }
    const AgentBrowserState snapshot = state(page->id);
    if (!snapshot.active || !snapshot.visible) {
        error = "native_input_requires_visible_active_page";
        return false;
    }
    if (!request_native_input_permission()) {
        error = "native_input_permission_required: allow ACECode in System Settings > Privacy & Security > Accessibility, then retry or use input_mode=synthetic";
        return false;
    }
    NSWindow* window = [page->webview window];
    [NSApp activateIgnoringOtherApps:YES];
    [window makeKeyAndOrderFront:nil];
    [window makeFirstResponder:page->webview];
    return true;
}

void AgentBrowserHost::Impl::run_synthetic_input(
    const std::shared_ptr<Page>& page,
    const std::string& method,
    const json& params,
    const std::shared_ptr<PendingProxyCall>& pending) {
    std::string expression;
    if (method == "Input.dispatchMouseEvent") {
        expression = synthetic_mouse_expression(params);
    } else if (method == "Input.insertText") {
        expression = synthetic_text_expression(params);
    } else if (method == "Input.dispatchKeyEvent") {
        expression = synthetic_key_expression(params);
    } else {
        finish_proxy_call(pending,
            {{"ok", false}, {"page_id", page->id},
             {"error", "unsupported synthetic input method"}});
        return;
    }
    evaluate_value(page, expression,
        [pending, page](bool ok, json value, std::string error) {
            if (!ok || (value.is_object() && !value.value("ok", true))) {
                if (error.empty() && value.is_object()) {
                    error = value.value("message", "synthetic input failed");
                }
                finish_proxy_call(pending,
                    {{"ok", false}, {"page_id", page->id},
                     {"error", error.empty() ? "synthetic input failed" : error}});
                return;
            }
            finish_proxy_call(pending,
                {{"ok", true}, {"page_id", page->id},
                 {"result", json::object()}});
        });
}

void AgentBrowserHost::Impl::run_native_input(
    const std::shared_ptr<Page>& page,
    const std::string& method,
    const json& params,
    const std::shared_ptr<PendingProxyCall>& pending) {
    std::string error;
    if (!prepare_native_input(page, error)) {
        finish_proxy_call(pending,
            {{"ok", false}, {"page_id", page ? page->id : std::string()},
             {"error", error}});
        return;
    }

    bool posted = false;
    if (method == "Input.dispatchMouseEvent") {
        const std::string type = params.value("type", "mouseMoved");
        const double x = params.value("x", 0.0);
        const double y = params.value("y", 0.0);
        const CGPoint point = native_screen_point(page, x, y);
        if (type == "mouseWheel") {
            CGEventRef move = CGEventCreateMouseEvent(
                nullptr, kCGEventMouseMoved, point, kCGMouseButtonLeft);
            if (move) {
                CGEventPost(kCGHIDEventTap, move);
                CFRelease(move);
            }
            CGEventRef wheel = CGEventCreateScrollWheelEvent(
                nullptr, kCGScrollEventUnitPixel, 2,
                static_cast<int32_t>(-std::lround(params.value("deltaY", 0.0))),
                static_cast<int32_t>(-std::lround(params.value("deltaX", 0.0))));
            if (wheel) {
                CGEventSetLocation(wheel, point);
                CGEventPost(kCGHIDEventTap, wheel);
                CFRelease(wheel);
                posted = true;
            }
        } else {
            const std::string button_name = params.value("button", "left");
            const CGMouseButton button = cg_mouse_button(button_name);
            const bool dragging = params.value("buttons", 0) != 0;
            const CGEventType event_type =
                cg_mouse_event_type(type, button, dragging);
            CGEventRef event = CGEventCreateMouseEvent(
                nullptr, event_type, point, button);
            if (event) {
                CGEventSetIntegerValueField(
                    event, kCGMouseEventClickState,
                    params.value("clickCount", 1));
                CGEventPost(kCGHIDEventTap, event);
                CFRelease(event);
                posted = true;
            }
        }
    } else if (method == "Input.dispatchKeyEvent") {
        const std::string key = params.value("key", "");
        const auto code = native_key_code(key);
        if (!code) {
            error = "native input key is unsupported: " + key;
        } else {
            const bool down = params.value("type", "keyDown") != "keyUp";
            CGEventRef event = CGEventCreateKeyboardEvent(nullptr, *code, down);
            if (event) {
                CGEventSetFlags(
                    event, native_modifier_flags(params.value("modifiers", 0)));
                CGEventPost(kCGHIDEventTap, event);
                CFRelease(event);
                posted = true;
            }
        }
    } else if (method == "Input.insertText") {
        NSString* text = ns_string(params.value("text", ""));
        const NSUInteger length = [text length];
        for (NSUInteger offset = 0; offset < length; offset += 20) {
            const NSUInteger count = (std::min)(NSUInteger{20}, length - offset);
            std::vector<UniChar> characters(count);
            [text getCharacters:characters.data()
                         range:NSMakeRange(offset, count)];
            CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
            CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
            if (!down || !up) {
                if (down) CFRelease(down);
                if (up) CFRelease(up);
                error = "failed to create native text event";
                break;
            }
            CGEventKeyboardSetUnicodeString(down, count, characters.data());
            CGEventPost(kCGHIDEventTap, down);
            CGEventPost(kCGHIDEventTap, up);
            CFRelease(down);
            CFRelease(up);
            posted = true;
        }
        if (length == 0) posted = true;
    }

    if (!posted && error.empty()) error = "failed to post native input event";
    if (!error.empty()) {
        finish_proxy_call(pending,
            {{"ok", false}, {"page_id", page->id}, {"error", error}});
    } else {
        finish_proxy_call(pending,
            {{"ok", true}, {"page_id", page->id},
             {"result", json::object()}});
    }
}

void AgentBrowserHost::Impl::capture_screenshot(
    const std::shared_ptr<Page>& page,
    const json& params,
    const std::shared_ptr<PendingProxyCall>& pending) {
    (void)params;
    // Objective-C blocks retain C++ values, but a reference parameter would
    // otherwise leave the asynchronous callback pointing at the caller's
    // short-lived shared_ptr object.
    const auto target_page = page;
    const auto completion = pending;
    WKSnapshotConfiguration* configuration =
        [[[WKSnapshotConfiguration alloc] init] autorelease];
    configuration.rect = [target_page->webview bounds];
    [target_page->webview takeSnapshotWithConfiguration:configuration
                               completionHandler:^(NSImage* image, NSError* error) {
        if (error || !image) {
            finish_proxy_call(completion,
                {{"ok", false}, {"page_id", target_page->id},
                 {"error", error
                    ? utf8_string([error localizedDescription])
                    : "WKWebView snapshot failed"}});
            return;
        }
        CGImageRef cg_image = [image CGImageForProposedRect:nil
                                                   context:nil
                                                     hints:nil];
        if (!cg_image) {
            finish_proxy_call(completion,
                {{"ok", false}, {"page_id", target_page->id},
                 {"error", "WKWebView snapshot returned no image"}});
            return;
        }
        NSBitmapImageRep* representation = [[[NSBitmapImageRep alloc]
            initWithCGImage:cg_image] autorelease];
        NSData* data = [representation
            representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (!data || [data length] == 0) {
            finish_proxy_call(completion,
                {{"ok", false}, {"page_id", target_page->id},
                 {"error", "failed to encode WKWebView snapshot"}});
            return;
        }
        const std::string bytes(
            static_cast<const char*>([data bytes]), [data length]);
        finish_proxy_call(completion,
            {{"ok", true}, {"page_id", target_page->id},
             {"result", {{"data", acecode::base64_encode(bytes)},
                          {"captureBeyondViewport", false}}}});
    }];
}

bool AgentBrowserHost::Impl::handle_dialog_on_ui(
    const std::shared_ptr<Page>& page,
    const json& params,
    std::string& error) {
    if (!page || page->dialog_kind.empty()) {
        error = "No JavaScript dialog is currently open";
        return false;
    }
    const bool accept = params.value("accept", true);
    if (page->alert_completion) {
        auto block = page->alert_completion;
        page->alert_completion = nil;
        block();
        [block release];
    } else if (page->confirm_completion) {
        auto block = page->confirm_completion;
        page->confirm_completion = nil;
        block(accept ? YES : NO);
        [block release];
    } else if (page->prompt_completion) {
        auto block = page->prompt_completion;
        page->prompt_completion = nil;
        block(accept ? ns_string(params.value("promptText", "")) : nil);
        [block release];
    }
    page->dialog_kind.clear();
    page->dialog_message.clear();
    return true;
}

void AgentBrowserHost::Impl::call_command_on_ui(
    const std::string& requested_page,
    const std::string& method,
    const json& params,
    const std::shared_ptr<PendingProxyCall>& pending) {
    std::string page_id = requested_page;
    if (page_id.empty()) page_id = active_page_id();
    if (page_id.empty()) page_id = create_page_on_ui(true);
    auto page = find_page(page_id);
    if (!page || page->closing || !page->webview) {
        finish_proxy_call(pending,
            {{"ok", false}, {"page_id", page_id},
             {"error", "Agent Browser page was not found"}});
        return;
    }
    if (!require_shared(page, page_id, pending)) return;

    if (method == "Runtime.evaluate") {
        const std::string expression = params.value("expression", "");
        evaluate_value(page, expression,
            [pending, page](bool ok, json value, std::string error) {
                if (!ok) {
                    finish_proxy_call(pending,
                        {{"ok", true}, {"page_id", page->id},
                         {"result", {
                             {"result", {{"type", "undefined"}}},
                             {"exceptionDetails", {{"text", error}}},
                         }}});
                    return;
                }
                finish_proxy_call(pending,
                    {{"ok", true}, {"page_id", page->id},
                     {"result", {{"result", {
                         {"type", remote_type(value)}, {"value", value},
                     }}}}});
            });
        return;
    }

    if (method == "Page.navigate") {
        std::string error;
        if (!navigate_on_ui(page_id, params.value("url", ""), &error)) {
            finish_proxy_call(pending,
                {{"ok", false}, {"page_id", page_id}, {"error", error}});
        } else {
            finish_proxy_call(pending,
                {{"ok", true}, {"page_id", page_id},
                 {"result", {{"frameId", page_id}}}});
        }
        return;
    }
    if (method == "Page.reload") {
        [page->webview reload];
        finish_proxy_call(pending,
            {{"ok", true}, {"page_id", page_id},
             {"result", json::object()}});
        return;
    }
    if (method == "Page.getNavigationHistory") {
        clear_history(page);
        json entries = json::array();
        NSArray<WKBackForwardListItem*>* back =
            page->webview.backForwardList.backList;
        NSArray<WKBackForwardListItem*>* forward =
            page->webview.backForwardList.forwardList;
        for (WKBackForwardListItem* item in back) {
            page->history_items.push_back([item retain]);
        }
        const int current_index = static_cast<int>(page->history_items.size());
        if (page->webview.backForwardList.currentItem) {
            page->history_items.push_back(
                [page->webview.backForwardList.currentItem retain]);
        }
        for (WKBackForwardListItem* item in forward) {
            page->history_items.push_back([item retain]);
        }
        for (std::size_t index = 0; index < page->history_items.size(); ++index) {
            WKBackForwardListItem* item = page->history_items[index];
            entries.push_back({
                {"id", static_cast<int>(index + 1)},
                {"url", utf8_string(item.URL.absoluteString)},
                {"userTypedURL", utf8_string(item.initialURL.absoluteString)},
                {"title", utf8_string(item.title)},
                {"transitionType", "link"},
            });
        }
        finish_proxy_call(pending,
            {{"ok", true}, {"page_id", page_id},
             {"result", {{"currentIndex", current_index},
                          {"entries", std::move(entries)}}}});
        return;
    }
    if (method == "Page.navigateToHistoryEntry") {
        const int entry_id = params.value("entryId", 0);
        if (entry_id <= 0 ||
            static_cast<std::size_t>(entry_id) > page->history_items.size()) {
            finish_proxy_call(pending,
                {{"ok", false}, {"page_id", page_id},
                 {"error", "Browser history entry is invalid"}});
        } else {
            [page->webview goToBackForwardListItem:
                page->history_items[static_cast<std::size_t>(entry_id - 1)]];
            finish_proxy_call(pending,
                {{"ok", true}, {"page_id", page_id},
                 {"result", json::object()}});
        }
        return;
    }
    if (method == "Page.captureScreenshot") {
        capture_screenshot(page, params, pending);
        return;
    }
    if (method == "Page.handleJavaScriptDialog") {
        std::string error;
        if (!handle_dialog_on_ui(page, params, error)) {
            finish_proxy_call(pending,
                {{"ok", false}, {"page_id", page_id}, {"error", error}});
        } else {
            finish_proxy_call(pending,
                {{"ok", true}, {"page_id", page_id},
                 {"result", json::object()}});
        }
        return;
    }
    if (method == "Input.dispatchMouseEvent" ||
        method == "Input.dispatchKeyEvent" ||
        method == "Input.insertText") {
        const std::string mode = params.value("acecodeInputMode", "synthetic");
        if (mode == "native") {
            run_native_input(page, method, params, pending);
        } else if (mode == "synthetic") {
            run_synthetic_input(page, method, params, pending);
        } else {
            finish_proxy_call(pending,
                {{"ok", false}, {"page_id", page_id},
                 {"error", "macOS input_mode must be synthetic or native"}});
        }
        return;
    }

    finish_proxy_call(pending,
        {{"ok", false}, {"page_id", page_id},
         {"error", "WKWebView command is not implemented: " + method}});
}

void AgentBrowserHost::Impl::execute_proxy_request_on_ui(
    const json& request,
    const std::shared_ptr<PendingProxyCall>& pending) {
    const std::string operation = request.value("operation", "cdp");
    const std::string requested_page = request.value("page_id", "");
    if (operation == "create_page") {
        const std::string page_id = create_page_on_ui(true);
        finish_proxy_call(pending,
            {{"ok", true}, {"page_id", page_id},
             {"result", {{"page_id", page_id}}}});
        return;
    }
    if (operation == "claim_page") {
        std::string page_id = requested_page.empty()
            ? active_page_id() : requested_page;
        if (page_id.empty()) page_id = create_page_on_ui(true);
        auto page = find_page(page_id);
        if (!page) {
            finish_proxy_call(pending,
                {{"ok", false}, {"page_id", page_id},
                 {"error", "Agent Browser page was not found"}});
        } else if (require_shared(page, page_id, pending)) {
            finish_proxy_call(pending,
                {{"ok", true}, {"page_id", page_id},
                 {"result", {{"page_id", page_id}}}});
        }
        return;
    }
    if (operation == "close_page") {
        const std::string page_id = requested_page.empty()
            ? active_page_id() : requested_page;
        auto page = find_page(page_id);
        if (!page) {
            finish_proxy_call(pending,
                {{"ok", false}, {"page_id", page_id},
                 {"error", "Agent Browser page was not found"}});
            return;
        }
        if (!require_shared(page, page_id, pending)) return;
        std::string closed_page;
        std::string error;
        if (!close_page_on_ui(page_id, &closed_page, &error)) {
            finish_proxy_call(pending,
                {{"ok", false}, {"page_id", page_id}, {"error", error}});
        } else {
            finish_proxy_call(pending,
                {{"ok", true}, {"page_id", closed_page},
                 {"result", {{"closed", true}, {"page_id", closed_page}}}});
        }
        return;
    }
    if (operation == "select_page") {
        auto page = find_page(requested_page);
        if (!page) {
            finish_proxy_call(pending,
                {{"ok", false}, {"page_id", requested_page},
                 {"error", "Agent Browser page was not found"}});
            return;
        }
        if (!require_shared(page, requested_page, pending)) return;
        std::string error;
        if (!select_page_on_ui(requested_page, &error)) {
            finish_proxy_call(pending,
                {{"ok", false}, {"page_id", requested_page}, {"error", error}});
        } else {
            finish_proxy_call(pending,
                {{"ok", true}, {"page_id", requested_page},
                 {"result", {{"page_id", requested_page}}}});
        }
        return;
    }
    call_command_on_ui(
        requested_page,
        request.value("method", ""),
        request.contains("params") && request["params"].is_object()
            ? request["params"] : json::object(),
        pending);
}

json AgentBrowserHost::Impl::execute_proxy_request(const json& request) {
    if (!request.is_object()) {
        return {{"ok", false}, {"error", "proxy request must be an object"}};
    }
    if (!request.contains("auth_token") ||
        !request["auth_token"].is_string() ||
        request["auth_token"].get_ref<const std::string&>() !=
            proxy_auth_token) {
        return {{"ok", false},
                {"error", "Agent Browser proxy authentication failed"}};
    }
    const std::string operation = request.value("operation", "cdp");
    if (operation != "cdp" && operation != "create_page" &&
        operation != "claim_page" && operation != "close_page" &&
        operation != "select_page") {
        return {{"ok", false},
                {"error", "Agent Browser proxy operation is invalid"}};
    }
    if (operation == "cdp") {
        if (!request.contains("method") || !request["method"].is_string() ||
            request["method"].get_ref<const std::string&>().empty() ||
            request["method"].get_ref<const std::string&>().size() > 256) {
            return {{"ok", false},
                    {"error", "Agent Browser command method is invalid"}};
        }
    }
    const int requested_timeout =
        request.contains("timeout_ms") && request["timeout_ms"].is_number_integer()
        ? request["timeout_ms"].get<int>() : 15000;
    const int timeout_ms = (std::max)(
        100, (std::min)(120000, requested_timeout));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    auto pending = std::make_shared<PendingProxyCall>();
    const auto weak = weak_from_this();
    try {
        dispatch_handler([weak, request, pending] {
            if (const auto self = weak.lock()) {
                self->execute_proxy_request_on_ui(request, pending);
            } else {
                finish_proxy_call(pending,
                    {{"ok", false}, {"error", "Agent Browser stopped"}});
            }
        });
    } catch (const std::exception& error) {
        return {{"ok", false},
                {"error", "Agent Browser dispatch failed: " +
                              std::string(error.what())}};
    }

    std::unique_lock<std::mutex> lock(pending->mutex);
    while (!pending->completed && !proxy_stopping.load() &&
           std::chrono::steady_clock::now() < deadline) {
        pending->ready.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (pending->completed) return pending->response;
    return {{"ok", false},
            {"page_id", request.value("page_id", "")},
            {"error", proxy_stopping.load()
                ? "Agent Browser stopped"
                : "WKWebView command timed out"}};
}

void AgentBrowserHost::Impl::handle_proxy_connection(int connection) {
    const auto request_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
    std::uint32_t request_size = 0;
    if (!socket_transfer(connection, &request_size, sizeof(request_size), false,
                         proxy_stopping, request_deadline) ||
        request_size == 0 || request_size > kAgentBrowserProxyMaxRequestBytes) {
        return;
    }
    std::string payload(request_size, '\0');
    if (!socket_transfer(connection, payload.data(), payload.size(), false,
                         proxy_stopping, request_deadline)) {
        return;
    }
    const json request = json::parse(payload, nullptr, false);
    std::string response_text = execute_proxy_request(request).dump();
    if (response_text.empty() ||
        response_text.size() > kAgentBrowserProxyMaxResponseBytes) {
        return;
    }
    std::uint32_t response_size =
        static_cast<std::uint32_t>(response_text.size());
    const auto response_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(10);
    if (!socket_transfer(connection, &response_size, sizeof(response_size), true,
                         proxy_stopping, response_deadline) ||
        !socket_transfer(connection, response_text.data(), response_text.size(),
                         true, proxy_stopping, response_deadline)) {
        return;
    }
    std::uint8_t acknowledgement = 0;
    const auto acknowledgement_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    socket_transfer(connection, &acknowledgement, sizeof(acknowledgement), false,
                    proxy_stopping, acknowledgement_deadline);
}

void AgentBrowserHost::Impl::proxy_loop() {
    while (!proxy_stopping.load()) {
        const int listening = listener_fd.load();
        if (listening < 0) return;
        pollfd descriptor{listening, POLLIN, 0};
        const int polled = ::poll(&descriptor, 1, 50);
        if (polled < 0) {
            if (errno == EINTR) continue;
            if (!proxy_stopping.load()) {
                LOG_ERROR("[agent-browser] macOS proxy poll failed: " +
                          std::string(std::strerror(errno)));
            }
            return;
        }
        if (polled == 0 || (descriptor.revents & POLLIN) == 0) continue;
        const int connection = ::accept(listening, nullptr, nullptr);
        if (connection < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (!proxy_stopping.load()) {
                LOG_ERROR("[agent-browser] macOS proxy accept failed: " +
                          std::string(std::strerror(errno)));
            }
            continue;
        }
        uid_t peer_uid = static_cast<uid_t>(-1);
        gid_t peer_gid = static_cast<gid_t>(-1);
        if (::getpeereid(connection, &peer_uid, &peer_gid) != 0 ||
            peer_uid != ::geteuid()) {
            ::close(connection);
            continue;
        }
        const int flags = ::fcntl(connection, F_GETFL, 0);
        if (flags >= 0) ::fcntl(connection, F_SETFL, flags | O_NONBLOCK);
        handle_proxy_connection(connection);
        ::close(connection);
    }
}

bool AgentBrowserHost::Impl::publish_proxy() {
    const auto path = agent_browser_proxy_socket_path(acecode_dir);
    proxy_socket_path = acecode::path_to_utf8(path);
    if (proxy_socket_path.empty() ||
        proxy_socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        fail("Agent Browser Unix socket path is too long");
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        fail("failed to create Agent Browser runtime directory: " +
             filesystem_error.message());
        return false;
    }
    ::unlink(proxy_socket_path.c_str());
    const int listening = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listening < 0) {
        fail("failed to create Agent Browser Unix socket: " +
             std::string(std::strerror(errno)));
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    address.sun_len = sizeof(address);
    std::memcpy(address.sun_path, proxy_socket_path.c_str(),
                proxy_socket_path.size() + 1);
    if (::bind(listening, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::chmod(proxy_socket_path.c_str(), S_IRUSR | S_IWUSR) != 0 ||
        ::listen(listening, 8) != 0) {
        const std::string detail = std::strerror(errno);
        ::close(listening);
        ::unlink(proxy_socket_path.c_str());
        fail("failed to bind Agent Browser Unix socket: " + detail);
        return false;
    }
    const int flags = ::fcntl(listening, F_GETFL, 0);
    if (flags >= 0) ::fcntl(listening, F_SETFL, flags | O_NONBLOCK);
    listener_fd.store(listening);

    try {
        proxy_auth_token = acecode::generate_auth_token();
        proxy_thread = std::thread([this] { proxy_loop(); });
    } catch (const std::exception& error) {
        listener_fd.store(-1);
        ::close(listening);
        ::unlink(proxy_socket_path.c_str());
        fail("failed to start Agent Browser proxy: " +
             std::string(error.what()));
        return false;
    }

    AgentBrowserRuntimeManifest manifest;
    manifest.desktop_pid = desktop_pid;
    manifest.desktop_instance_id = desktop_instance_id;
    manifest.user_data_dir = acecode::path_to_utf8(
        agent_browser_root_path(acecode_dir));
    manifest.pipe_name = proxy_socket_path;
    manifest.auth_token = proxy_auth_token;
    manifest.ready_at_ms = now_unix_ms();
    if (!write_agent_browser_runtime_manifest(manifest, acecode_dir)) {
        proxy_stopping.store(true);
        const int active_listener = listener_fd.exchange(-1);
        if (active_listener >= 0) {
            ::shutdown(active_listener, SHUT_RDWR);
            ::close(active_listener);
        }
        if (proxy_thread.joinable()) proxy_thread.join();
        ::unlink(proxy_socket_path.c_str());
        fail("failed to publish Agent Browser runtime endpoint");
        return false;
    }
    return true;
}

bool AgentBrowserHost::Impl::toggle_element_selection_on_ui(
    const std::string& page_id,
    std::string* error) {
    auto page = find_page(page_id);
    if (!page || !page->webview || page->closing) {
        assign_error(error, "Agent Browser page is still starting");
        return false;
    }
    if (state(page_id).element_selection_active) {
        evaluate_value(
            page,
            "(() => { const value=globalThis['" + std::string(kElementPickerKey) +
                "']; if(value&&typeof value.cancel==='function')value.cancel(); return true; })()",
            [](bool, json, std::string) {});
        return true;
    }
    const std::uint64_t generation = ++page->element_selection_generation;
    update_page(page, [](AgentBrowserState& value) {
        value.element_selection_active = true;
    });
    const auto weak = weak_from_this();
    evaluate_value(page, element_picker_expression(),
        [weak, page, generation](bool ok, json result, std::string) {
            const auto self = weak.lock();
            if (!self) return;
            if (page->closing ||
                page->element_selection_generation != generation) return;
            AgentBrowserState snapshot;
            {
                std::lock_guard<std::mutex> lock(self->state_mutex);
                page->state.element_selection_active = false;
                if (ok && result.is_object() &&
                    !result.value("cancelled", true) &&
                    result.contains("element") && result["element"].is_object()) {
                    ++page->state.element_selection_serial;
                    snapshot = page->state;
                    snapshot.selected_element_json = result["element"].dump();
                } else {
                    snapshot = page->state;
                }
            }
            self->emit_state(snapshot);
        });
    return true;
}

AgentBrowserHost::AgentBrowserHost(void* parent_window,
                                   std::int64_t desktop_pid,
                                   std::string desktop_instance_id,
                                   StateHandler state_handler,
                                   DispatchHandler dispatch_handler,
                                   std::string acecode_dir)
    : impl_(std::make_shared<Impl>(
          parent_window, desktop_pid, std::move(desktop_instance_id),
          std::move(state_handler), std::move(dispatch_handler),
          std::move(acecode_dir))) {
    impl_->start();
}

AgentBrowserHost::~AgentBrowserHost() = default;

bool AgentBrowserHost::supported() const {
    return impl_ && impl_->state().supported;
}

AgentBrowserState AgentBrowserHost::state(const std::string& page_id) const {
    return impl_ ? impl_->state(page_id) : AgentBrowserState{};
}

std::vector<AgentBrowserState> AgentBrowserHost::states() const {
    return impl_ ? impl_->states() : std::vector<AgentBrowserState>{};
}

std::string AgentBrowserHost::active_page_id() const {
    return impl_ ? impl_->active_page_id() : std::string{};
}

std::string AgentBrowserHost::create_page(std::string* error) {
    if (impl_ && impl_->state().supported && impl_->state().ready) {
        return impl_->create_page_on_ui(false);
    }
    assign_error(error, impl_ ? impl_->state().error
                              : "Agent Browser is unavailable on this platform");
    return {};
}

bool AgentBrowserHost::close_page(const std::string& page_id,
                                  std::string* error) {
    std::string ignored;
    return impl_ && impl_->close_page_on_ui(page_id, &ignored, error);
}

bool AgentBrowserHost::select_page(const std::string& page_id,
                                   std::string* error) {
    return impl_ && impl_->select_page_on_ui(page_id, error);
}

bool AgentBrowserHost::set_bounds(const std::string& page_id,
                                  const AgentBrowserBounds& bounds,
                                  std::string* error) {
    if (!impl_ || !impl_->state().supported) {
        assign_error(error, "Agent Browser is unavailable on this platform");
        return false;
    }
    if (bounds.x < 0 || bounds.y < 0 || bounds.width < 0 || bounds.height < 0 ||
        bounds.width > 32768 || bounds.height > 32768) {
        assign_error(error, "Agent Browser bounds are invalid");
        return false;
    }
    if (bounds.occlusion_rects.size() > kAgentBrowserMaxOcclusionRects) {
        assign_error(error, "Agent Browser has too many occlusion rectangles");
        return false;
    }
    for (const auto& rect : bounds.occlusion_rects) {
        if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
            rect.x > 32768 || rect.y > 32768 ||
            rect.width > 32768 || rect.height > 32768) {
            assign_error(error, "Agent Browser occlusion rectangle is invalid");
            return false;
        }
    }
    auto page = impl_->find_page(page_id);
    if (!page) {
        assign_error(error, "Agent Browser page was not found");
        return false;
    }
    if (bounds.layout_revision != 0 &&
        page->requested_bounds.layout_revision != 0 &&
        bounds.layout_revision < page->requested_bounds.layout_revision) {
        return true;
    }
    page->requested_bounds = bounds;
    impl_->apply_bounds(page);
    return true;
}

bool AgentBrowserHost::navigate(const std::string& page_id,
                                const std::string& input,
                                std::string* error) {
    return impl_ && impl_->navigate_on_ui(page_id, input, error);
}

bool AgentBrowserHost::go_back(const std::string& page_id,
                               std::string* error) {
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->webview && page->webview.canGoBack) {
        [page->webview goBack];
        return true;
    }
    assign_error(error, "Agent Browser cannot go back");
    return false;
}

bool AgentBrowserHost::go_forward(const std::string& page_id,
                                  std::string* error) {
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->webview && page->webview.canGoForward) {
        [page->webview goForward];
        return true;
    }
    assign_error(error, "Agent Browser cannot go forward");
    return false;
}

bool AgentBrowserHost::reload(const std::string& page_id,
                              std::string* error) {
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->webview) {
        [page->webview reload];
        return true;
    }
    assign_error(error, "Agent Browser page is still starting");
    return false;
}

bool AgentBrowserHost::focus(const std::string& page_id,
                             std::string* error) {
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->webview && [[page->webview window]
            makeFirstResponder:page->webview]) {
        return true;
    }
    assign_error(error, "Agent Browser page is still starting");
    return false;
}

bool AgentBrowserHost::set_shared_with_agent(const std::string& page_id,
                                              bool shared,
                                              std::string* error) {
    return impl_ && impl_->set_shared_with_agent_on_ui(page_id, shared, error);
}

bool AgentBrowserHost::toggle_element_selection(const std::string& page_id,
                                                 std::string* error) {
    return impl_ && impl_->toggle_element_selection_on_ui(page_id, error);
}

std::string AgentBrowserHost::console_logs(const std::string& page_id,
                                           std::string* error) const {
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (!page) {
        assign_error(error, "Agent Browser page was not found");
        return {};
    }
    std::ostringstream output;
    for (std::size_t index = 0; index < page->console_logs.size(); ++index) {
        if (index != 0) output << '\n';
        output << page->console_logs[index];
    }
    return output.str();
}

bool AgentBrowserHost::open_developer_tools(const std::string& page_id,
                                            std::string* error) {
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (!page || !page->webview) {
        assign_error(error, "Agent Browser page is still starting");
        return false;
    }
    assign_error(error,
        "WKWebView has no public API to open Web Inspector programmatically");
    return false;
}

void AgentBrowserHost::refresh_layout() {
    if (!impl_) return;
    for (const auto& state : impl_->states()) {
        if (auto page = impl_->find_page(state.page_id)) {
            impl_->apply_bounds(page);
        }
    }
}

void AgentBrowserHost::set_parent_visible(bool visible) {
    if (!impl_) return;
    impl_->parent_surface_visible = visible;
    refresh_layout();
}

void AgentBrowserHost::hide(const std::string& page_id) {
    if (!impl_) return;
    if (!page_id.empty()) {
        auto page = impl_->find_page(page_id);
        if (page && page->webview) {
            [page->webview setHidden:YES];
            impl_->update_page(page, [](AgentBrowserState& value) {
                value.visible = false;
            });
        }
        return;
    }
    for (const auto& state : impl_->states()) {
        auto page = impl_->find_page(state.page_id);
        if (page && page->webview) {
            [page->webview setHidden:YES];
            impl_->update_page(page, [](AgentBrowserState& value) {
                value.visible = false;
            });
        }
    }
}

} // namespace acecode::desktop

#endif // __APPLE__
