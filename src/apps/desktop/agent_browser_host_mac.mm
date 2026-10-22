#include "agent_browser_host.hpp"

#ifdef __APPLE__

#include "agent_browser_cookie_jars.hpp"
#include "agent_browser_macos_helpers.hpp"
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

namespace acecode::desktop {
using namespace agent_browser_macos;
namespace {

class MacAgentBrowserDelegateSink {
public:
    virtual ~MacAgentBrowserDelegateSink() = default;
    virtual void navigation_started(const std::string&, WKWebView*,
                                    std::uint64_t) = 0;
    virtual void navigation_redirected(const std::string&, WKWebView*) = 0;
    virtual void navigation_committed(const std::string&, WKWebView*) = 0;
    virtual void navigation_finished(const std::string&, WKWebView*,
                                     std::uint64_t) = 0;
    virtual void navigation_failed(const std::string&, WKWebView*, NSError*,
                                   const std::string&, std::uint64_t) = 0;
    virtual void navigation_policy_decided(
        const std::string&, WKWebView*, WKNavigationAction*, bool, bool) = 0;
    // 把系统已注册的外部 URI/SSO scheme 交给操作系统。返回 true 表示已交接,
    // 随后 WebView 的取消回调按「已处理」对待而不是导航失败。
    virtual bool handoff_external_url(const std::string&, WKWebView*,
                                      NSURL*) = 0;
    virtual void authentication_challenge(
        const std::string&, WKWebView*, NSURLAuthenticationChallenge*) = 0;
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

@interface ACECodeAgentBrowserSurfaceView : NSView {
@private
    NSArray<NSValue*>* occlusion_rects_;
}
- (BOOL)setOcclusionRects:(NSArray<NSValue*>*)rects;
@end

@implementation ACECodeAgentBrowserSurfaceView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        occlusion_rects_ = [[NSArray alloc] init];
        [self setWantsLayer:YES];
    }
    return self;
}

- (void)dealloc {
    [occlusion_rects_ release];
    [super dealloc];
}

- (BOOL)isFlipped {
    return YES;
}

- (CAShapeLayer*)newOcclusionMaskForView:(NSView*)target_view {
    CALayer* target_layer = [target_view layer];
    const NSRect bounds = [target_view bounds];
    const CGFloat width = NSWidth(bounds);
    const CGFloat height = NSHeight(bounds);
    CGMutablePathRef path = CGPathCreateMutable();
    CGPathAddRect(path, nullptr, CGRectMake(0, 0, width, height));
    for (NSValue* value in occlusion_rects_) {
        const NSRect rect = [value rectValue];
        // Occlusion rectangles use the DOM's top-left Browser contract.
        // Backing layers for flipped AppKit views already use that orientation;
        // converting them again mirrors top overlays to the bottom. Convert Y
        // only when a target layer actually uses bottom-left coordinates.
        const CGFloat x = NSMinX(rect) - NSMinX(bounds);
        const CGFloat top = NSMinY(rect) - NSMinY(bounds);
        const CGFloat y = [target_view isFlipped]
            ? top
            : height - top - NSHeight(rect);
        CGPathAddRect(path, nullptr,
                      CGRectMake(x, y, NSWidth(rect), NSHeight(rect)));
    }

    CAShapeLayer* mask = [[CAShapeLayer alloc] init];
    [mask setFrame:CGRectMake(0, 0, width, height)];
    [mask setPath:path];
    [mask setFillRule:kCAFillRuleEvenOdd];
    [mask setFillColor:CGColorGetConstantColor(kCGColorBlack)];
    [mask setAllowsEdgeAntialiasing:NO];
    CGPathRelease(path);
    return mask;
}

- (BOOL)updateOcclusionMask {
    CALayer* surface_layer = [self layer];
    if (!surface_layer) return [occlusion_rects_ count] == 0;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if ([occlusion_rects_ count] == 0) {
        [surface_layer setMask:nil];
        for (NSView* subview in [self subviews]) {
            [[subview layer] setMask:nil];
        }
        [CATransaction commit];
        return YES;
    }

    CAShapeLayer* surface_mask =
        [self newOcclusionMaskForView:self];
    [surface_layer setMask:surface_mask];
    [surface_mask release];

    BOOL applied = [surface_layer mask] != nil;
    // WKWebView content is hosted by WebKit's remote layer tree. Mask both the
    // wrapper and the concrete child layer so future compositor changes cannot
    // let Browser pixels escape the wrapper's local occlusion contract.
    for (NSView* subview in [self subviews]) {
        [subview setWantsLayer:YES];
        CALayer* subview_layer = [subview layer];
        if (!subview_layer) {
            applied = NO;
            continue;
        }
        CAShapeLayer* subview_mask =
            [self newOcclusionMaskForView:subview];
        [subview_layer setMask:subview_mask];
        [subview_mask release];
        applied = applied && [subview_layer mask] != nil;
    }
    [CATransaction commit];
    return applied;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self updateOcclusionMask];
}

- (BOOL)setOcclusionRects:(NSArray<NSValue*>*)rects {
    NSArray<NSValue*>* next = rects ? [rects copy] : [[NSArray alloc] init];
    [occlusion_rects_ release];
    occlusion_rects_ = next;
    return [self updateOcclusionMask];
}

- (NSView*)hitTest:(NSPoint)point {
    NSView* superview = [self superview];
    const NSPoint local = superview
        ? [self convertPoint:point fromView:superview]
        : point;
    for (NSValue* value in occlusion_rects_) {
        if (NSPointInRect(local, [value rectValue])) return nil;
    }
    return [super hitTest:point];
}

@end

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
    if (owner_) {
        owner_->navigation_started([self pageIDValue], webView,
                                   acecode::desktop::navigation_identity(navigation));
    }
}

- (void)webView:(WKWebView*)webView
    didReceiveServerRedirectForProvisionalNavigation:(WKNavigation*)navigation {
    (void)navigation;
    if (owner_) owner_->navigation_redirected([self pageIDValue], webView);
}

- (void)webView:(WKWebView*)webView didCommitNavigation:(WKNavigation*)navigation {
    (void)navigation;
    if (owner_) owner_->navigation_committed([self pageIDValue], webView);
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    if (owner_) {
        owner_->navigation_finished([self pageIDValue], webView,
                                    acecode::desktop::navigation_identity(navigation));
    }
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                      withError:(NSError*)error {
    if (owner_) {
        owner_->navigation_failed(
            [self pageIDValue], webView, error, "provisional",
            acecode::desktop::navigation_identity(navigation));
    }
}

- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
             withError:(NSError*)error {
    if (owner_) {
        owner_->navigation_failed(
            [self pageIDValue], webView, error, "committed",
            acecode::desktop::navigation_identity(navigation));
    }
}

- (void)webView:(WKWebView*)webView
    didReceiveAuthenticationChallenge:(NSURLAuthenticationChallenge*)challenge
                   completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition disposition,
                                                NSURLCredential* credential))completionHandler {
    // Agent Browser 是开发用浏览器:服务器信任挑战一律放行(自签名、过期、主机名
    // 不匹配、内部 CA 都能打开)。凭据类挑战(Basic/Digest/NTLM/Negotiate/Kerberos/
    // 代理/客户端证书)继续交给系统默认处理 —— ACECode 不编造、也不保存任何凭据。
    NSURLProtectionSpace* protection_space = [challenge protectionSpace];
    const bool server_trust = protection_space &&
        [[protection_space authenticationMethod]
            isEqualToString:NSURLAuthenticationMethodServerTrust] &&
        [protection_space serverTrust] != NULL;
    if (owner_) {
        owner_->authentication_challenge(
            [self pageIDValue], webView, challenge);
    }
    if (server_trust) {
        NSURLCredential* credential = [NSURLCredential
            credentialForTrust:[protection_space serverTrust]];
        completionHandler(NSURLSessionAuthChallengeUseCredential, credential);
        return;
    }
    completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
}

- (void)webView:(WKWebView*)webView
    authenticationChallenge:(NSURLAuthenticationChallenge*)challenge
       shouldAllowDeprecatedTLS:(void (^)(BOOL))decisionHandler {
    (void)webView;
    (void)challenge;
    // 操作系统仍能协商的旧 TLS 一律继续,老旧内网服务不应成为挡板。系统已经移除
    // 的协议/密码套件不在此列 —— 那种连接根本走不到这个回调。
    decisionHandler(YES);
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
    NSURL* url = navigationAction.request.URL;
    const bool allowed = acecode::desktop::safe_navigation_url(url);
    bool handed_off = false;
    if (!allowed && owner_ &&
        acecode::desktop::external_handoff_candidate_url(url)) {
        handed_off = owner_->handoff_external_url(
            [self pageIDValue], webView, url);
    }
    if (owner_) {
        owner_->navigation_policy_decided(
            [self pageIDValue], webView, navigationAction, allowed, handed_off);
    }
    decisionHandler(allowed
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
        ACECodeAgentBrowserSurfaceView* surface = nil;
        WKWebView* webview = nil;
        ACECodeAgentBrowserDelegate* delegate = nil;
        std::vector<std::string> console_logs;
        std::vector<WKBackForwardListItem*> history_items;
        std::uint64_t element_selection_generation = 0;
        AgentBrowserNavigationTracker navigation;
        // 取消 / 外部交接后要恢复的导航前快照。
        std::string content_state_before_navigation =
            kAgentBrowserContentStateEmpty;
        std::string favicon_before_navigation;
        bool last_authentication_was_proxy = false;
        bool closing = false;
        bool observing_state = false;
        std::string dialog_kind;
        std::string dialog_message;
        void (^alert_completion)(void) = nil;
        void (^confirm_completion)(BOOL) = nil;
        void (^prompt_completion)(NSString*) = nil;
        // 当前 webview 所在的 cookie 存储(建 webview 时按档位与 owner 决定)。
        AgentBrowserCookieStoreKey store;
        // 按会话存储第一次被用到时要先清空 / 从父会话复制;做完之前命令排队、
        // 地址栏导航拒绝(同 Windows host)。
        bool store_prepared = true;
        std::vector<std::function<void(bool, const std::string&)>> ready_waiters;
        // 切档重建时记下的地址,新 webview 就绪后导航回去。
        std::string restore_url;
    };
    using StoreDone = std::function<void(bool ok, const std::string& error)>;

    void* parent_window = nullptr;
    std::int64_t desktop_pid = 0;
    std::string desktop_instance_id;
    std::string acecode_dir;
    StateHandler state_handler;
    DispatchHandler dispatch_handler;
    mutable std::mutex state_mutex;
    AgentBrowserState host_state;
    std::unordered_map<std::string, std::shared_ptr<Page>> pages;
    // 页序、显示页与每个会话的 Agent 默认目标(跨平台纯逻辑,与 Windows host
    // 共用,见 agent_browser_page_directory.hpp)。所有读写都在 state_mutex 下。
    AgentBrowserPageDirectory directory;
    std::uint64_t next_page_sequence = 0;
    // 共享档的存储:固定 UUID 的 identifier store(升级前就是它,登录数据都在这里)。
    WKWebsiteDataStore* data_store = nil;
    // 退出即清档:进程内持有的非持久存储,切离该档或 Desktop 退出时释放。
    WKWebsiteDataStore* until_quit_store = nil;
    // 本次运行打开过的按会话存储(retain),key 是根会话 id。
    std::unordered_map<std::string, WKWebsiteDataStore*> session_stores;
    std::unordered_set<std::string> prepared_session_stores;
    std::unordered_map<std::string, std::vector<StoreDone>> session_store_waiters;
    StorageEventHandler storage_handler;
    AgentBrowserCookieMode cookie_mode = AgentBrowserCookieMode::Shared;
    WKContentWorld* automation_world = nil;
    std::string proxy_socket_path;
    std::string proxy_auth_token;
    std::atomic<bool> proxy_stopping{false};
    std::atomic<int> listener_fd{-1};
    acecode::JoiningThread proxy_thread;

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
            directory.clear();
        }
        for (const auto& page : remaining) teardown_page(page);
        [automation_world release];
        [data_store release];
        [until_quit_store release];
        for (auto& item : session_stores) [item.second release];
        session_stores.clear();
        cleanup_agent_browser_runtime_manifest(desktop_instance_id, acecode_dir);
    }

    void emit_state(const AgentBrowserState& state) const {
        if (state_handler) state_handler(state);
    }

    AgentBrowserState state(const std::string& requested_page = {}) const {
        std::lock_guard<std::mutex> lock(state_mutex);
        const std::string id = requested_page.empty()
            ? directory.displayed_page_id() : requested_page;
        const auto found = pages.find(id);
        return found == pages.end() ? host_state : found->second->state;
    }

    std::vector<AgentBrowserState> states(
        const std::string& owner_session_id = {}) const {
        std::vector<AgentBrowserState> result;
        std::lock_guard<std::mutex> lock(state_mutex);
        result.reserve(directory.size());
        for (const auto& id : directory.ordered_page_ids()) {
            const auto found = pages.find(id);
            if (found == pages.end()) continue;
            if (!owner_session_id.empty() &&
                found->second->state.owner.session_id != owner_session_id) {
                continue;
            }
            result.push_back(found->second->state);
        }
        return result;
    }

    std::string active_page_id() const {
        std::lock_guard<std::mutex> lock(state_mutex);
        return directory.displayed_page_id();
    }

    std::shared_ptr<Page> find_page(const std::string& requested_page) const {
        std::lock_guard<std::mutex> lock(state_mutex);
        const std::string id = requested_page.empty()
            ? directory.displayed_page_id() : requested_page;
        const auto found = pages.find(id);
        return found == pages.end() ? nullptr : found->second;
    }

    // 目录里的 agent target 变化后,把每页状态上的 agent_target 位对齐并返回需要
    // 广播的快照。调用方必须持有 state_mutex。
    std::vector<AgentBrowserState> sync_agent_target_flags_locked() {
        std::vector<AgentBrowserState> changed;
        for (const auto& item : pages) {
            const bool target = directory.is_agent_target(item.first);
            if (item.second->state.agent_target != target) {
                item.second->state.agent_target = target;
                changed.push_back(item.second->state);
            }
        }
        return changed;
    }

    // 代理请求落到哪一页(见 AgentBrowserPageDirectory::resolve_agent_page)。
    // allow_create=false 时没有现成页面就返回空,调用方按「未找到」处理。
    std::string resolve_agent_page_id(const std::string& requested_page,
                                      const AgentBrowserPageOwner& owner,
                                      bool allow_create) {
        AgentBrowserPageResolution resolution;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            resolution = directory.resolve_agent_page(requested_page, owner);
        }
        if (!resolution.create) return resolution.page_id;
        if (!allow_create) return {};
        return create_page_on_ui(true, owner, true);
    }

    // 显式 select 或隐式 claim 之后把该会话的默认目标钉在这一页上。
    void set_agent_target_on_ui(const AgentBrowserPageOwner& owner,
                                const std::string& page_id) {
        if (owner.empty()) return;
        std::vector<AgentBrowserState> changed;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!directory.set_agent_target(owner, page_id)) return;
            changed = sync_agent_target_flags_locked();
        }
        for (const auto& snapshot : changed) emit_state(snapshot);
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
        run_startup_store_tasks();
        if (!publish_proxy()) return;
        update_host_state([](AgentBrowserState& value) {
            value.supported = true;
            value.ready = true;
            value.error.clear();
        });
        LOG_INFO("[agent-browser] WKWebView host ready; macOS proxy published");
    }

    // ---- cookie 存储(openspec add-agent-browser-cookie-modes D5) ----

    void emit_storage_event(AgentBrowserStorageEvent event) const {
        if (event.cookie_mode.empty()) {
            event.cookie_mode = agent_browser_cookie_mode_name(cookie_mode);
        }
        if (storage_handler) storage_handler(event);
    }

    static NSUUID* session_store_identifier(const std::string& session_id) {
        return [[[NSUUID alloc] initWithUUIDString:
            ns_string(agent_browser_session_store_uuid(session_id))] autorelease];
    }

    // 存储键 -> data store。共享 = 固定 UUID 的 identifier store;退出即清 = 进程内
    // 非持久存储(持有到切离该档或退出,所以关掉所有页面也不丢);按会话 =
    // 由会话 id 派生 UUID 的 identifier store。
    WKWebsiteDataStore* store_for_key(const AgentBrowserCookieStoreKey& key) {
        if (key.kind == AgentBrowserCookieStoreKind::UntilQuit) {
            if (!until_quit_store) {
                until_quit_store = [[WKWebsiteDataStore nonPersistentDataStore] retain];
            }
            return until_quit_store;
        }
        if (key.kind == AgentBrowserCookieStoreKind::Session) {
            const auto found = session_stores.find(key.session_id);
            if (found != session_stores.end()) return found->second;
            NSUUID* identifier = session_store_identifier(key.session_id);
            WKWebsiteDataStore* store = nil;
            if (@available(macOS 14.0, *)) {
                if (identifier) {
                    store = [[WKWebsiteDataStore dataStoreForIdentifier:identifier] retain];
                }
            }
            if (!store) {
                LOG_WARN("[agent-browser][macos] cannot open session cookie store " +
                         key.session_id + "; using the shared store");
                return data_store;
            }
            session_stores.emplace(key.session_id, store);
            return store;
        }
        return data_store;
    }

    void release_session_store(const std::string& session_id) {
        const auto found = session_stores.find(session_id);
        if (found == session_stores.end()) return;
        [found->second release];
        session_stores.erase(found);
    }

    static void clear_data_store(WKWebsiteDataStore* store, StoreDone done) {
        if (!store) {
            done(false, "Agent Browser cookie store is unavailable");
            return;
        }
        [store removeDataOfTypes:[WKWebsiteDataStore allWebsiteDataTypes]
                   modifiedSince:[NSDate distantPast]
               completionHandler:^{
            done(true, std::string());
        }];
    }

    // 从磁盘删除一份按会话存储(macOS 14+)。存储还被 WebKit 占用时失败,记录留到
    // 下次启动再删。
    static void remove_session_store_from_disk(const std::string& session_id,
                                               StoreDone done) {
        NSUUID* identifier = session_store_identifier(session_id);
        if (!identifier) {
            done(false, "session cookie store identifier is invalid");
            return;
        }
        if (@available(macOS 14.0, *)) {
            [WKWebsiteDataStore removeDataStoreForIdentifier:identifier
                                           completionHandler:^(NSError* error) {
                if (error) {
                    done(false, utf8_string(error.localizedDescription));
                } else {
                    done(true, std::string());
                }
            }];
            return;
        }
        done(false, "removing cookie stores requires macOS 14");
    }

    static void drop_session_from_store_lists(const std::string& session_id) {
        update_agent_browser_store_state([&session_id](AgentBrowserStoreState& current) {
            const auto drop = [&session_id](std::vector<std::string>& values) {
                values.erase(std::remove(values.begin(), values.end(), session_id),
                             values.end());
            };
            drop(current.pending_wipe);
            drop(current.pending_deletions);
            drop(current.session_stores);
            return true;
        });
    }

    // 启动时:待清空 / 待删除的按会话存储直接从磁盘删除(此时还没有页面在用);
    // 共享存储欠一次清空的,补清。都是异步完成,完成后才撤掉状态里的记录。
    void run_startup_store_tasks() {
        const AgentBrowserStoreState state = read_agent_browser_store_state();
        cookie_mode = effective_agent_browser_cookie_mode(state.cookie_mode);
        std::vector<std::string> ids = state.pending_wipe;
        for (const auto& id : state.pending_deletions) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        for (const auto& session_id : ids) {
            const std::string id = session_id;
            remove_session_store_from_disk(id, [id](bool ok, const std::string& error) {
                if (ok) {
                    drop_session_from_store_lists(id);
                } else {
                    LOG_WARN("[agent-browser][macos] failed to remove session cookie store " +
                             id + ": " + error);
                }
            });
        }
        if (state.shared_wipe_pending) clear_shared_store_and_mark_done();
        LOG_INFO(std::string("[agent-browser][macos] cookie mode: ") +
                 agent_browser_cookie_mode_name(cookie_mode));
    }

    void clear_shared_store_and_mark_done() {
        clear_data_store(data_store, [](bool ok, const std::string& error) {
            if (!ok) {
                LOG_WARN("[agent-browser][macos] failed to clear shared cookie store: " + error);
                return;
            }
            update_agent_browser_store_state([](AgentBrowserStoreState& current) {
                if (!current.shared_wipe_pending) return false;
                current.shared_wipe_pending = false;
                return true;
            });
            LOG_INFO("[agent-browser][macos] shared cookie store cleared");
        });
    }

    // 分叉会话第一次建存储:把父会话存储里的全部 cookie 复制进来;失败不阻断页面。
    void seed_store_cookies(const std::string& parent_session_id,
                            WKWebsiteDataStore* target,
                            const std::function<void()>& done) {
        AgentBrowserCookieStoreKey parent;
        parent.kind = AgentBrowserCookieStoreKind::Session;
        parent.session_id = parent_session_id;
        WKWebsiteDataStore* source = store_for_key(parent);
        if (!source || !target || source == target) {
            done();
            return;
        }
        WKHTTPCookieStore* target_cookies = target.httpCookieStore;
        const std::string parent_id = parent_session_id;
        const std::function<void()> finished = done;
        [source.httpCookieStore getAllCookies:^(NSArray<NSHTTPCookie*>* cookies) {
            dispatch_group_t group = dispatch_group_create();
            for (NSHTTPCookie* cookie in cookies) {
                dispatch_group_enter(group);
                [target_cookies setCookie:cookie completionHandler:^{
                    dispatch_group_leave(group);
                }];
            }
            const std::size_t count = static_cast<std::size_t>(cookies.count);
            dispatch_group_notify(group, dispatch_get_main_queue(), ^{
                LOG_INFO("[agent-browser][macos] seeded forked cookie store from " +
                         parent_id + ": copied=" + std::to_string(count));
                finished();
            });
            dispatch_release(group);
        }];
    }

    // 页面的 webview 建好之后:按会话存储第一次被用到时按状态文件先清空 / 从父会话
    // 复制,再登记;同一存储并发建多页时只做一次,其余页面等它完成。
    void prepare_page_store(const std::shared_ptr<Page>& page) {
        if (!page || page->store_prepared) return;
        const std::string session_id = page->store.session_id;
        const std::string page_id = page->id;
        const auto weak = weak_from_this();
        auto& waiters = session_store_waiters[session_id];
        waiters.push_back([weak, page_id](bool ok, const std::string& error) {
            const auto self = weak.lock();
            if (!self) return;
            const auto ready_page = self->find_page(page_id);
            if (!ready_page || ready_page->closing) return;
            self->finish_page_store(ready_page, ok, error);
        });
        if (waiters.size() > 1) return;

        const AgentBrowserSessionStorePreparation preparation =
            prepare_agent_browser_session_store(session_id, read_agent_browser_store_state());
        WKWebsiteDataStore* store = store_for_key(page->store);
        const StoreDone finish = [weak, session_id](bool ok, const std::string& error) {
            const auto self = weak.lock();
            if (!self) return;
            if (ok) {
                self->prepared_session_stores.insert(session_id);
                if (!update_agent_browser_store_state([&session_id](AgentBrowserStoreState& state) {
                        apply_agent_browser_session_store_preparation(session_id, state);
                        return true;
                    })) {
                    LOG_WARN("[agent-browser][macos] failed to record cookie store " + session_id);
                }
            }
            auto pending = std::move(self->session_store_waiters[session_id]);
            self->session_store_waiters.erase(session_id);
            for (auto& waiter : pending) waiter(ok, error);
        };
        const std::string seed_from = preparation.seed_from;
        const std::function<void()> seed_then_finish = [weak, seed_from, store, finish]() {
            const auto self = weak.lock();
            if (!self) return;
            if (seed_from.empty()) {
                finish(true, std::string());
                return;
            }
            self->seed_store_cookies(seed_from, store, [finish] { finish(true, std::string()); });
        };
        if (!preparation.wipe_first) {
            seed_then_finish();
            return;
        }
        clear_data_store(store, [seed_then_finish, finish](bool ok, const std::string& error) {
            if (!ok) {
                finish(false, error);
                return;
            }
            seed_then_finish();
        });
    }

    void finish_page_store(const std::shared_ptr<Page>& page,
                           bool ok,
                           const std::string& error) {
        auto waiters = std::move(page->ready_waiters);
        page->ready_waiters.clear();
        if (!ok) {
            update_page(page, [&error](AgentBrowserState& value) {
                value.ready = false;
                value.error = error;
            });
            for (auto& waiter : waiters) waiter(false, error);
            return;
        }
        page->store_prepared = true;
        update_page(page, [](AgentBrowserState& value) {
            value.ready = true;
            value.error.clear();
        });
        for (auto& waiter : waiters) waiter(true, std::string());
        if (!page->restore_url.empty()) {
            const std::string url = page->restore_url;
            page->restore_url.clear();
            std::string navigate_error;
            if (!navigate_on_ui(page->id, url, &navigate_error)) {
                LOG_WARN("[agent-browser][macos] failed to restore " + page->id +
                         " after cookie store change: " + navigate_error);
            }
        }
        apply_bounds(page);
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
        if (page->surface) {
            [page->surface removeFromSuperview];
            [page->surface release];
            page->surface = nil;
        }
        if (page->delegate) {
            page->delegate->owner_ = nullptr;
            [page->delegate release];
            page->delegate = nil;
        }
    }

    // 为页面建 WKWebView 与承载它的 surface。data store 按当前档位与页面 owner 选
    // (agent_browser_cookie_store_key);新建页面与切档重建共用这一段。
    void attach_webview(const std::shared_ptr<Page>& page) {
        page->store = agent_browser_cookie_store_key(cookie_mode, page->state.owner);
        page->store_prepared =
            page->store.kind != AgentBrowserCookieStoreKind::Session ||
            prepared_session_stores.count(page->store.session_id) != 0;

        WKWebViewConfiguration* configuration =
            [[[WKWebViewConfiguration alloc] init] autorelease];
        configuration.websiteDataStore = store_for_key(page->store);
        configuration.applicationNameForUserAgent =
            safari_compatible_application_name();
        configuration.preferences.javaScriptCanOpenWindowsAutomatically = YES;
        // 开发浏览器不使用 WebKit 的欺诈/恶意网站拦截页 —— 它会在开发与内网站点
        // 上变成又一块挡板,而 Agent Browser 的定位是尽力打开。
        if ([configuration.preferences
                respondsToSelector:@selector(setFraudulentWebsiteWarningEnabled:)]) {
            configuration.preferences.fraudulentWebsiteWarningEnabled = NO;
        }
        [configuration.preferences setValue:@YES
                                     forKey:@"allowFileAccessFromFileURLs"];
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
        page->surface = [[ACECodeAgentBrowserSurfaceView alloc]
            initWithFrame:NSZeroRect];
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
        [page->webview setAutoresizingMask:
            NSViewWidthSizable | NSViewHeightSizable];
        [page->surface setHidden:YES];
        [page->surface setAutoresizingMask:NSViewNotSizable];
        if (page->surface && page->webview) {
            [page->surface addSubview:page->webview];
            [[static_cast<NSWindow*>(parent_window) contentView]
                addSubview:page->surface
                positioned:NSWindowAbove
                relativeTo:nil];
        }
        const bool created_view = page->webview != nil && page->surface != nil;
        // 按会话存储还没准备好(先清空 / 从父会话复制)时先不报就绪。
        page->state.ready = created_view && page->store_prepared;
        if (!created_view) {
            page->state.error = "failed to create WKWebView page surface";
        }
    }

    std::string create_page_on_ui(bool shared_with_agent,
                                  const AgentBrowserPageOwner& owner = {},
                                  bool agent_created = false) {
        auto page = std::make_shared<Page>();
        page->id = "browser-" + std::to_string(desktop_pid) + "-" +
                   std::to_string(++next_page_sequence);
        page->state.page_id = page->id;
        page->state.supported = true;
        page->state.shared_with_agent = shared_with_agent;
        page->state.owner = owner;
        attach_webview(page);

        std::vector<AgentBrowserState> changed;
        AgentBrowserState created;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            pages.emplace(page->id, page);
            directory.add_page(page->id, owner, agent_created);
            changed = sync_agent_target_flags_locked();
            created = page->state;
        }
        emit_state(created);
        for (const auto& snapshot : changed) {
            if (snapshot.page_id != page->id) emit_state(snapshot);
        }
        LOG_INFO(("[agent-browser][macos] " + json{
            {"event", "page_created"},
            {"page_id", page->id},
            {"shared_with_agent", shared_with_agent},
            {"owner_session_id", owner.session_id},
            {"ready", page->state.ready},
        }.dump()));
        // 有归属的 Agent 建页不抢显示:显示页由 Web UI 按当前会话决定。旧协议
        // (无 owner)与 UI 自建页沿用「新页即显示」。
        if (!agent_created || owner.empty()) {
            std::string ignored;
            select_page_on_ui(page->id, &ignored);
        }
        if (!page->store_prepared && page->webview) prepare_page_store(page);
        return page->id;
    }

    bool apply_bounds(const std::shared_ptr<Page>& page) {
        if (!page || !page->surface || !page->webview) return false;
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
        [page->surface setFrame:NSMakeRect(
            bounds.x, y, bounds.width, bounds.height)];
        [page->webview setFrame:[page->surface bounds]];

        NSMutableArray<NSValue*>* occlusion_rects =
            [NSMutableArray arrayWithCapacity:bounds.occlusion_rects.size()];
        for (const auto& occlusion : bounds.occlusion_rects) {
            const int left = std::max(0, std::min(bounds.width, occlusion.x));
            const int top = std::max(0, std::min(bounds.height, occlusion.y));
            const int right = std::max(left, std::min(
                bounds.width, occlusion.x + occlusion.width));
            const int bottom = std::max(top, std::min(
                bounds.height, occlusion.y + occlusion.height));
            if (right > left && bottom > top) {
                [occlusion_rects addObject:[NSValue valueWithRect:NSMakeRect(
                    left, top, right - left, bottom - top)]];
            }
        }
        const bool occlusion_ready =
            [page->surface setOcclusionRects:occlusion_rects] == YES;
        const AgentBrowserState snapshot = state(page->id);
        const bool show = parent_surface_visible && bounds.visible &&
            snapshot.active &&
            snapshot.content_state == kAgentBrowserContentStateLive &&
            occlusion_ready &&
            bounds.width > 0 && bounds.height > 0;
        [page->surface setHidden:show ? NO : YES];
        [page->webview setHidden:show ? NO : YES];
        if (show) {
            [content addSubview:page->surface
                     positioned:NSWindowAbove relativeTo:nil];
        }
        update_page(page, [&](AgentBrowserState& value) {
            value.visible = show;
        });
        return occlusion_ready;
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
            directory.set_displayed_page(page_id);
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
        std::vector<AgentBrowserState> changed;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            const std::string id = requested_page.empty()
                ? directory.displayed_page_id() : requested_page;
            const auto found = pages.find(id);
            if (found == pages.end()) {
                assign_error(error, "Agent Browser page was not found");
                return false;
            }
            page = found->second;
            page->closing = true;
            // 页面关闭后仍会有在途回调到达 delegate,标记之后一律忽略。
            page->navigation.close();
            if (directory.displayed_page_id() == id) {
                next_page = directory.next_displayed_after_close(id);
            }
            pages.erase(found);
            directory.remove_page(id);
            changed = sync_agent_target_flags_locked();
        }
        teardown_page(page);
        page->state.closed = true;
        page->state.ready = false;
        page->state.visible = false;
        page->state.active = false;
        page->state.agent_target = false;
        emit_state(page->state);
        for (const auto& snapshot : changed) emit_state(snapshot);
        LOG_INFO(("[agent-browser][macos] " + json{
            {"event", "page_closed"},
            {"page_id", page->id},
            {"url", page->state.url},
        }.dump()));
        if (closed_page) *closed_page = page->id;
        if (!next_page.empty()) {
            std::string ignored;
            select_page_on_ui(next_page, &ignored);
        }
        return true;
    }

    std::vector<AgentBrowserPageStoreBinding> page_store_bindings() const {
        std::vector<AgentBrowserPageStoreBinding> bindings;
        std::lock_guard<std::mutex> lock(state_mutex);
        for (const std::string& id : directory.ordered_page_ids()) {
            const auto found = pages.find(id);
            if (found == pages.end() || found->second->closing) continue;
            bindings.push_back({id, found->second->state.owner, found->second->store});
        }
        return bindings;
    }

    // 切档后页面换存储:拆掉旧 webview,用同一个 page id、owner 与共享标记在新存储
    // 里重建并导航回原地址。期间的命令排队,就绪后照常执行。
    void rebuild_page_on_ui(const std::shared_ptr<Page>& page) {
        if (!page || page->closing) return;
        std::string url;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            url = page->state.url;
        }
        teardown_page(page);
        page->navigation = AgentBrowserNavigationTracker{};
        ++page->element_selection_generation;
        page->console_logs.clear();
        page->restore_url = (url.empty() || url == "about:blank") ? std::string() : url;
        const bool restoring = !page->restore_url.empty();
        update_page(page, [restoring](AgentBrowserState& value) {
            value.ready = false;
            value.loading = restoring;
            value.visible = false;
            value.can_go_back = false;
            value.can_go_forward = false;
            value.element_selection_active = false;
            value.failure_kind.clear();
            value.error.clear();
            value.diagnostic.clear();
            value.content_state = restoring ? kAgentBrowserContentStateLoading
                                            : kAgentBrowserContentStateEmpty;
        });
        attach_webview(page);
        if (!page->webview) {
            update_page(page, [](AgentBrowserState& value) {
                value.error = "failed to create WKWebView page surface";
            });
            return;
        }
        if (page->store_prepared) {
            finish_page_store(page, true, std::string());
        } else {
            prepare_page_store(page);
        }
    }

    bool set_cookie_mode_on_ui(const std::string& name, std::string* error) {
        const auto mode = agent_browser_cookie_mode_from_name(name);
        if (!mode) {
            assign_error(error, "unknown Agent Browser cookie mode: " + name);
            return false;
        }
        const AgentBrowserCookieModeSwitch plan =
            plan_agent_browser_cookie_mode_switch(cookie_mode, *mode);
        const bool persisted = update_agent_browser_store_state(
            [&mode, &plan](AgentBrowserStoreState& state) {
                state.cookie_mode = agent_browser_cookie_mode_name(*mode);
                if (plan.mark_session_stores_for_wipe) {
                    state.pending_wipe.insert(state.pending_wipe.end(),
                                              state.session_stores.begin(),
                                              state.session_stores.end());
                }
                if (plan.clear_shared_now) state.shared_wipe_pending = true;
                return true;
            });
        if (!persisted) {
            assign_error(error, "failed to save the Agent Browser cookie setting");
            return false;
        }
        if (!plan.changed) return true;

        const AgentBrowserCookieMode previous = cookie_mode;
        cookie_mode = *mode;
        if (plan.mark_session_stores_for_wipe) prepared_session_stores.clear();
        const auto rebuild = agent_browser_pages_to_rebuild(page_store_bindings(), cookie_mode);
        for (const auto& page_id : rebuild) rebuild_page_on_ui(find_page(page_id));
        if (plan.clear_shared_now) clear_shared_store_and_mark_done();
        if (plan.release_until_quit_store) {
            [until_quit_store release];
            until_quit_store = nil;
        }
        LOG_INFO(std::string("[agent-browser][macos] cookie mode ") +
                 agent_browser_cookie_mode_name(previous) + " -> " +
                 agent_browser_cookie_mode_name(cookie_mode) + "; rebuilt " +
                 std::to_string(rebuild.size()) + " page(s)");
        AgentBrowserStorageEvent event;
        event.type = "cookie_mode_changed";
        emit_storage_event(std::move(event));
        return true;
    }

    // 清除该页当前所用存储的全部网站数据,完成后重新加载同一存储里的页面。
    bool clear_browsing_data_on_ui(const std::string& page_id, std::string* error) {
        const auto page = find_page(page_id);
        if (!page || page->closing) {
            assign_error(error, "Agent Browser page was not found");
            return false;
        }
        const AgentBrowserCookieStoreKey key = page->store;
        const auto weak = weak_from_this();
        const std::string requested_page = page_id;
        clear_data_store(store_for_key(key),
                         [weak, key, requested_page](bool ok, const std::string& clear_error) {
            const auto self = weak.lock();
            if (!self) return;
            if (ok) {
                std::vector<std::shared_ptr<Page>> reload;
                {
                    std::lock_guard<std::mutex> lock(self->state_mutex);
                    for (const auto& item : self->pages) {
                        if (!item.second->closing && item.second->webview &&
                            item.second->store == key) {
                            reload.push_back(item.second);
                        }
                    }
                }
                for (const auto& candidate : reload) [candidate->webview reload];
                LOG_INFO("[agent-browser][macos] browsing data cleared: " + key.id());
            } else {
                LOG_WARN("[agent-browser][macos] failed to clear browsing data for " +
                         key.id() + ": " + clear_error);
            }
            AgentBrowserStorageEvent event;
            event.type = "browsing_data_cleared";
            event.ok = ok;
            event.store = key.id();
            event.page_id = requested_page;
            event.error = clear_error;
            self->emit_storage_event(std::move(event));
        });
        return true;
    }

    // daemon 永久删除会话后:关闭该根会话的页面,释放并从磁盘删除它的按会话存储。
    // WebKit 仍占用时删除失败,记录留到下次启动处理。
    void process_store_tasks_on_ui() {
        const AgentBrowserStoreState state = read_agent_browser_store_state();
        for (const auto& session_id : state.pending_deletions) {
            for (const auto& page_id :
                 agent_browser_pages_of_root_session(page_store_bindings(), session_id)) {
                close_page_on_ui(page_id, nullptr, nullptr);
            }
            prepared_session_stores.erase(session_id);
            release_session_store(session_id);
            const auto weak = weak_from_this();
            const std::string id = session_id;
            remove_session_store_from_disk(id, [weak, id](bool ok, const std::string& error) {
                if (ok) drop_session_from_store_lists(id);
                const auto self = weak.lock();
                if (!self) return;
                if (!ok) {
                    LOG_WARN("[agent-browser][macos] session cookie store " + id +
                             " stays pending: " + error);
                }
                AgentBrowserStorageEvent event;
                event.type = "session_store_deleted";
                event.ok = ok;
                event.store = "session:" + id;
                event.error = error;
                self->emit_storage_event(std::move(event));
            });
        }
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
        if (!page || !page->webview || page->closing || !page->store_prepared) {
            assign_error(error, "Agent Browser page is still starting");
            return false;
        }
        auto normalized = normalize_agent_browser_url(input, error);
        if (!normalized) {
            LOG_WARN(("[agent-browser][macos] " + json{
                {"event", "navigation_rejected"},
                {"page_id", page_id},
                {"input", clipped(input, kMaxNativeErrorFieldBytes)},
                {"error", error ? *error : std::string{}},
            }.dump()));
            return false;
        }
        LOG_INFO(("[agent-browser][macos] " + json{
            {"event", "navigation_requested"},
            {"page_id", page_id},
            {"url", *normalized},
        }.dump()));
        if (*normalized == "about:blank") {
            [page->webview loadHTMLString:@"" baseURL:nil];
        } else {
            NSURL* url = [NSURL URLWithString:ns_string(*normalized)];
            if (!url) {
                assign_error(error, "Agent Browser URL is invalid");
                return false;
            }
            load_agent_browser_request(
                page->webview, [NSURLRequest requestWithURL:url]);
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
                            WKWebView* webview,
                            std::uint64_t navigation_id) override {
        auto page = find_page(page_id);
        if (!page || page->webview != webview) return;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->state.content_state !=
                kAgentBrowserContentStateLoading) {
                page->content_state_before_navigation =
                    page->state.content_state;
                page->favicon_before_navigation = page->state.favicon;
            }
        }
        page->navigation.begin_navigation(navigation_id);
        page->console_logs.clear();
        page->last_authentication_was_proxy = false;
        const std::string url = utf8_string(webview.URL.absoluteString);
        LOG_INFO(("[agent-browser][macos] " + json{
            {"event", "navigation_started"},
            {"page_id", page_id},
            {"url", url},
        }.dump()));
        update_page(page, [&](AgentBrowserState& value) {
            value.loading = true;
            value.visible = false;
            value.content_state = kAgentBrowserContentStateLoading;
            value.failure_kind.clear();
            value.error.clear();
            value.diagnostic.clear();
            value.favicon.clear();
            if (!url.empty()) value.url = url;
        });
        apply_bounds(page);
    }

    void navigation_redirected(const std::string& page_id,
                               WKWebView* webview) override {
        auto page = find_page(page_id);
        if (!page || page->webview != webview) return;
        LOG_INFO(("[agent-browser][macos] " + json{
            {"event", "navigation_redirected"},
            {"page_id", page_id},
            {"url", native_description(webview.URL.absoluteString)},
        }.dump()));
    }

    void navigation_committed(const std::string& page_id,
                              WKWebView* webview) override {
        auto page = find_page(page_id);
        if (!page || page->webview != webview) return;
        LOG_INFO(("[agent-browser][macos] " + json{
            {"event", "navigation_committed"},
            {"page_id", page_id},
            {"url", native_description(webview.URL.absoluteString)},
        }.dump()));
    }

    // 把系统已注册的外部 URI/SSO scheme 交给操作系统。没有注册 handler 时如实
    // 记录一次外部启动失败,绝不谎称受保护的 HTTP(S) 页面本身出了证书问题。
    bool handoff_external_url(const std::string& page_id,
                              WKWebView* webview,
                              NSURL* url) override {
        auto page = find_page(page_id);
        if (!page || page->webview != webview || !url) return false;
        NSWorkspace* workspace = [NSWorkspace sharedWorkspace];
        NSURL* handler = [workspace URLForApplicationToOpenURL:url];
        if (!handler) {
            LOG_WARN(("[agent-browser][macos] " + json{
                {"event", "external_uri_no_handler"},
                {"page_id", page_id},
                {"scheme", native_description([url scheme])},
            }.dump()));
            return false;
        }
        const bool launched = [workspace openURL:url] == YES;
        if (launched) page->navigation.note_external_handoff(0);
        json event{
            {"event", launched ? "external_uri_launched"
                               : "external_uri_launch_failed"},
            {"page_id", page_id},
            {"scheme", native_description([url scheme])},
        };
        if (launched) LOG_INFO("[agent-browser][macos] " + event.dump());
        else LOG_WARN("[agent-browser][macos] " + event.dump());
        return launched;
    }

    void navigation_policy_decided(const std::string& page_id,
                                   WKWebView* webview,
                                   WKNavigationAction* action,
                                   bool allowed,
                                   bool handed_off) override {
        auto page = find_page(page_id);
        if (!page || page->webview != webview || !action) return;
        WKFrameInfo* source_frame = [action sourceFrame];
        WKFrameInfo* target_frame = [action targetFrame];
        NSURL* url = action.request.URL;
        json event{
            {"event", "navigation_policy"},
            {"page_id", page_id},
            {"url", native_description(url.absoluteString)},
            {"scheme", native_description([url scheme])},
            {"navigation_type", navigation_type_name([action navigationType])},
            {"source_main_frame", source_frame &&
                [source_frame isMainFrame] == YES},
            {"target_frame_present", target_frame != nil},
            {"target_main_frame", target_frame &&
                [target_frame isMainFrame] == YES},
            {"decision", allowed ? "allow" : "cancel"},
            {"external_handoff", handed_off},
        };
        const std::string log_line =
            "[agent-browser][macos] " + event.dump();
        if (allowed || handed_off) LOG_INFO(log_line);
        else LOG_WARN(log_line);
    }

    void authentication_challenge(
        const std::string& page_id,
        WKWebView* webview,
        NSURLAuthenticationChallenge* challenge) override {
        auto page = find_page(page_id);
        if (!page || page->webview != webview || !challenge) return;

        NSURLProtectionSpace* protection_space = [challenge protectionSpace];
        const bool is_proxy = protection_space &&
            [protection_space isProxy] == YES;
        const bool accepts_server_trust = protection_space &&
            [[protection_space authenticationMethod]
                isEqualToString:NSURLAuthenticationMethodServerTrust] &&
            [protection_space serverTrust] != NULL;
        page->last_authentication_was_proxy = is_proxy;

        json event{
            {"event", "authentication_challenge"},
            {"page_id", page_id},
            {"url", native_description(webview.URL.absoluteString)},
            {"authentication_method", protection_space
                ? native_description([protection_space authenticationMethod])
                : std::string{}},
            {"host", protection_space
                ? native_description([protection_space host])
                : std::string{}},
            {"port", protection_space
                ? static_cast<long long>([protection_space port]) : 0LL},
            {"protocol", protection_space
                ? native_description([protection_space protocol])
                : std::string{}},
            {"realm", protection_space
                ? native_description([protection_space realm])
                : std::string{}},
            {"is_proxy", is_proxy},
            {"receives_credential_securely", protection_space &&
                [protection_space receivesCredentialSecurely] == YES},
            {"server_trust_present", protection_space &&
                [protection_space serverTrust] != nullptr},
            {"client_certificate_issuer_count", protection_space
                ? static_cast<std::size_t>(
                    [[protection_space distinguishedNames] count]) : 0U},
            {"previous_failure_count",
                static_cast<long long>([challenge previousFailureCount])},
            {"proposed_credential_present",
                [challenge proposedCredential] != nil},
            // 服务器信任挑战被开发宽松策略直接接受;凭据类挑战仍走系统默认。
            // 日志只记协议元数据 —— 不记用户名、密码、Cookie、Authorization 头
            // 或客户端证书私钥。
            {"disposition", accepts_server_trust
                ? "use_server_trust_credential"
                : "perform_default_handling"},
            {"development_permissive_trust", accepts_server_trust},
        };
        NSURLResponse* response = [challenge failureResponse];
        if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
            event["failure_response_status"] = static_cast<long long>(
                [(NSHTTPURLResponse*)response statusCode]);
        }
        // 信任放行在开发浏览器里是常态,记 INFO;需要凭据的挑战才值得 WARN。
        if (accepts_server_trust) {
            LOG_INFO("[agent-browser][macos] " + event.dump());
        } else {
            LOG_WARN("[agent-browser][macos] " + event.dump());
        }
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
                             WKWebView* webview,
                             std::uint64_t navigation_id) override {
        auto page = find_page(page_id);
        if (!page || page->webview != webview) return;
        if (page->navigation.on_navigation_completed(
                navigation_id, BrowserNavigationFailure::kNone) !=
            BrowserNavigationDecision::kPublishSuccess) {
            return;
        }
        refresh_page_identity(page);
        update_page(page, [](AgentBrowserState& value) {
            value.loading = false;
            value.content_state = kAgentBrowserContentStateLive;
            value.failure_kind.clear();
            value.error.clear();
            value.diagnostic.clear();
        });
        apply_bounds(page);
        refresh_favicon(page);
        const AgentBrowserState snapshot = state(page_id);
        LOG_INFO(("[agent-browser][macos] " + json{
            {"event", "navigation_finished"},
            {"page_id", page_id},
            {"url", snapshot.url},
            {"title", snapshot.title},
        }.dump()));
        // Warm the isolated world after cold WKWebView startup.
        evaluate_value(page, "true", [](bool, json, std::string) {});
    }

    void navigation_failed(const std::string& page_id,
                           WKWebView* webview,
                           NSError* native_error,
                           const std::string& phase,
                           std::uint64_t navigation_id) override {
        auto page = find_page(page_id);
        if (!page || page->webview != webview) return;
        const std::string message = utf8_string(
            [native_error localizedDescription]);
        const std::string diagnostic = native_error_diagnostic(native_error);
        const std::string failure_kind = navigation_failure_kind(
            native_error, page->last_authentication_was_proxy);
        NSError* url_error = url_loading_error(native_error);
        const long long url_error_code = url_error
            ? static_cast<long long>([url_error code]) : 0LL;
        const bool handoff_pending = page->navigation.external_handoff_pending();
        const BrowserNavigationDecision decision =
            page->navigation.on_navigation_completed(
                navigation_id,
                classify_darwin_navigation_failure(
                    url_error_code, handoff_pending));

        if (decision == BrowserNavigationDecision::kIgnore ||
            decision == BrowserNavigationDecision::kHoldPending) {
            // 陈旧回调、已关闭的页面或仍在等待继续:当前可见状态保持不动。
            LOG_INFO(("[agent-browser][macos] " + json{
                {"event", "navigation_failure_ignored"},
                {"phase", phase},
                {"page_id", page_id},
                {"failure_kind", failure_kind},
                {"reason", decision == BrowserNavigationDecision::kIgnore
                    ? "stale_or_closed" : "pending_continuation"},
                {"native_error", native_error_details(native_error)},
            }.dump()));
            return;
        }

        if (decision == BrowserNavigationDecision::kRestorePrevious) {
            // 被新导航取代、重定向替换或已交给操作系统的外部 scheme。这些是引擎
            // 内部的中间取消,不是这次浏览的最终结果 —— 发布错误页只会造成录屏里
            // 那种「闪一下白屏再显示无法打开此页面」。
            const std::string restored_state =
                page->content_state_before_navigation;
            const std::string restored_favicon =
                page->favicon_before_navigation;
            refresh_page_identity(page);
            update_page(page, [&](AgentBrowserState& value) {
                value.loading = false;
                value.content_state = restored_state;
                value.favicon = restored_favicon;
                value.failure_kind.clear();
                value.error.clear();
                value.diagnostic.clear();
            });
            LOG_INFO(("[agent-browser][macos] " + json{
                {"event", "navigation_cancelled"},
                {"phase", phase},
                {"page_id", page_id},
                {"external_handoff", handoff_pending},
                {"restored_content_state", restored_state},
                {"native_error", native_error_details(native_error)},
            }.dump()));
            apply_bounds(page);
            return;
        }

        refresh_page_identity(page);
        update_page(page, [&](AgentBrowserState& value) {
            value.loading = false;
            value.visible = false;
            value.content_state = kAgentBrowserContentStateNavigationError;
            value.failure_kind = failure_kind;
            value.error = message.empty() ? "WKWebView navigation failed" : message;
            value.diagnostic = diagnostic;
        });
        const AgentBrowserState snapshot = state(page_id);
        LOG_ERROR(("[agent-browser][macos] " + json{
            {"event", "navigation_failed"},
            {"phase", phase},
            {"page_id", page_id},
            {"url", snapshot.url},
            {"failure_kind", failure_kind},
            {"proxy_authentication_challenge",
                page->last_authentication_was_proxy},
            {"native_error", native_error_details(native_error)},
        }.dump()));
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
        LOG_ERROR(("[agent-browser][macos] " + json{
            {"event", "web_content_process_terminated"},
            {"page_id", page_id},
            {"url", page->state.url},
        }.dump()));
        update_page(page, [](AgentBrowserState& value) {
            value.loading = false;
            value.visible = false;
            value.content_state = kAgentBrowserContentStateProcessFailed;
            value.failure_kind = "web_content_process_terminated";
            value.error = "WKWebView content process terminated";
            value.diagnostic.clear();
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
        const bool allowed = page && request.URL &&
            safe_navigation_url(request.URL);
        const std::string log_line = "[agent-browser][macos] " + json{
            {"event", "popup_navigation"},
            {"page_id", page_id},
            {"url", request.URL
                ? native_description(request.URL.absoluteString)
                : std::string{}},
            {"decision", allowed ? "load_in_page" : "cancel"},
        }.dump();
        if (allowed) {
            LOG_INFO(log_line);
            load_agent_browser_request(page->webview, request);
        } else {
            LOG_WARN(log_line);
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
        const std::string&, const AgentBrowserPageOwner&, const std::string&,
        const json&, const std::shared_ptr<PendingProxyCall>&);
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
    const AgentBrowserPageOwner& owner,
    const std::string& method,
    const json& params,
    const std::shared_ptr<PendingProxyCall>& pending) {
    const std::string page_id =
        resolve_agent_page_id(requested_page, owner, true);
    auto page = find_page(page_id);
    if (!page || page->closing || !page->webview) {
        finish_proxy_call(pending,
            {{"ok", false}, {"page_id", page_id},
             {"error", "Agent Browser page was not found"}});
        return;
    }
    if (!require_shared(page, page_id, pending)) return;
    if (!page->store_prepared) {
        // 按会话存储还在准备(先清空 / 从父会话复制):就绪后按显式 page id 重放。
        const auto weak = weak_from_this();
        const std::string queued_page = page_id;
        const std::string queued_method = method;
        const json queued_params = params;
        page->ready_waiters.push_back(
            [weak, queued_page, queued_method, queued_params, pending](
                bool ok, const std::string& error) {
                const auto self = weak.lock();
                if (!self || !ok) {
                    finish_proxy_call(pending,
                        {{"ok", false}, {"page_id", queued_page},
                         {"error", ok ? std::string("Agent Browser stopped") : error}});
                    return;
                }
                self->call_command_on_ui(queued_page, {}, queued_method,
                                         queued_params, pending);
            });
        return;
    }

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
    const AgentBrowserPageOwner owner = parse_agent_browser_page_owner(
        request.contains("owner") ? request["owner"] : json());
    if (operation == "drain_store_tasks") {
        // 提醒本身不带数据,要做的事都在状态文件里(daemon 写入的待删除会话)。
        process_store_tasks_on_ui();
        finish_proxy_call(pending, {{"ok", true}, {"result", json::object()}});
        return;
    }
    if (operation == "create_page") {
        const std::string page_id = create_page_on_ui(true, owner, true);
        finish_proxy_call(pending,
            {{"ok", true}, {"page_id", page_id},
             {"result", {{"page_id", page_id}}}});
        return;
    }
    if (operation == "claim_page") {
        const std::string page_id =
            resolve_agent_page_id(requested_page, owner, true);
        auto page = find_page(page_id);
        if (!page) {
            finish_proxy_call(pending,
                {{"ok", false}, {"page_id", page_id},
                 {"error", "Agent Browser page was not found"}});
        } else if (require_shared(page, page_id, pending)) {
            set_agent_target_on_ui(owner, page_id);
            finish_proxy_call(pending,
                {{"ok", true}, {"page_id", page_id},
                 {"result", {{"page_id", page_id}}}});
        }
        return;
    }
    if (operation == "close_page") {
        const std::string page_id =
            resolve_agent_page_id(requested_page, owner, false);
        auto page = page_id.empty() ? nullptr : find_page(page_id);
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
        // 有归属的显式选页只钉该会话的默认目标,不改变显示页;旧协议沿用
        // 「选页即显示」。
        const bool selected = owner.empty()
            ? select_page_on_ui(requested_page, &error)
            : (set_agent_target_on_ui(owner, requested_page), true);
        if (!selected) {
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
        owner,
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
        operation != "select_page" && operation != "drain_store_tasks") {
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
        proxy_thread = acecode::JoiningThread([this] { proxy_loop(); });
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

std::vector<AgentBrowserState> AgentBrowserHost::states(
    const std::string& owner_session_id) const {
    return impl_ ? impl_->states(owner_session_id)
                 : std::vector<AgentBrowserState>{};
}

std::string AgentBrowserHost::active_page_id() const {
    return impl_ ? impl_->active_page_id() : std::string{};
}

std::string AgentBrowserHost::create_page(std::string* error,
                                          const AgentBrowserPageOwner& owner) {
    if (impl_ && impl_->state().supported && impl_->state().ready) {
        return impl_->create_page_on_ui(false, owner, false);
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
    if (!impl_->apply_bounds(page)) {
        assign_error(error, "Agent Browser failed to apply overlay occlusion");
        return false;
    }
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
        if (page && page->surface && page->webview) {
            [page->surface setHidden:YES];
            [page->webview setHidden:YES];
            impl_->update_page(page, [](AgentBrowserState& value) {
                value.visible = false;
            });
        }
        return;
    }
    for (const auto& state : impl_->states()) {
        auto page = impl_->find_page(state.page_id);
        if (page && page->surface && page->webview) {
            [page->surface setHidden:YES];
            [page->webview setHidden:YES];
            impl_->update_page(page, [](AgentBrowserState& value) {
                value.visible = false;
            });
        }
    }
}

std::string AgentBrowserHost::cookie_mode() const {
    return agent_browser_cookie_mode_name(
        impl_ ? impl_->cookie_mode : AgentBrowserCookieMode::Shared);
}

bool AgentBrowserHost::set_cookie_mode(const std::string& mode, std::string* error) {
    if (!impl_ || !impl_->state().supported) {
        assign_error(error, "Agent Browser is unavailable on this platform");
        return false;
    }
    return impl_->set_cookie_mode_on_ui(mode, error);
}

bool AgentBrowserHost::clear_browsing_data(const std::string& page_id,
                                           std::string* error) {
    if (!impl_ || !impl_->state().supported) {
        assign_error(error, "Agent Browser is unavailable on this platform");
        return false;
    }
    return impl_->clear_browsing_data_on_ui(page_id, error);
}

void AgentBrowserHost::process_store_tasks() {
    if (impl_ && impl_->state().supported) impl_->process_store_tasks_on_ui();
}

void AgentBrowserHost::set_storage_event_handler(StorageEventHandler handler) {
    if (impl_) impl_->storage_handler = std::move(handler);
}

} // namespace acecode::desktop

#endif // __APPLE__
