// macOS 版桌面 agent 办公室:与 desktop_pet.cpp(Windows)共用同一份页面
// (assets/desktop_pet/agent_office_pet.html,构建期嵌进可执行文件)、同一套桥协议
// 和 desktop_pet_layout 的几何计算。
//
// 窗口是透明、置顶、不抢焦点的 NSPanel(非激活面板,不能成为 key window),
// 里面放一个 WKWebView。页面按 WebView2 的 window.chrome.webview 写成,这里在
// 文档开始前注入一个同名垫片:postMessage 转给 WKScriptMessageHandler,宿主经
// window.__acePetDeliver 把消息派给页面的 message 监听器,页面代码无需区分平台。
//
// macOS 没有 SetWindowRgn:透明区域的点击穿透靠定时检测鼠标是否落在房间轮廓、
// 控制条或页面上报的浮层里,再切换 ignoresMouseEvents。拖动 / 把手缩放同样由
// 定时器跟随鼠标,不依赖 WebKit 把鼠标事件交还给窗口。
//
// 坐标:Cocoa 屏幕坐标原点在左下;desktop_pet_layout 按左上原点、设备像素计算。
// 这里统一换成左上原点的「点」,并把 dpi 固定为 96(1 点 = 1 个逻辑像素,
// Retina 的倍率由 WebKit 按 devicePixelRatio 处理)。
//
// 本文件用 ARC 编译(cmake/acecode_desktop.cmake 单独加 -fobjc-arc)。

#include "desktop_pet.hpp"
#include "web_host.hpp"

#include "desktop_pet_layout.hpp"
#include "desktop_office_service.hpp"
#include "version.hpp"
#include "platform/native_ui/strings.hpp"

#include "utils/logger.hpp"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if !__has_feature(objc_arc)
#error "desktop_pet_mac.mm must be compiled with -fobjc-arc"
#endif

@class ACECodeDesktopPetBridge;
@class ACECodeDesktopPetMenuTarget;

namespace acecode {
// cmake/acecode_desktop.cmake 用 acecode_bin2cpp.cmake 把页面嵌进可执行文件。
const unsigned char* desktop_pet_page_data();
std::size_t desktop_pet_page_size();
} // namespace acecode

namespace acecode::desktop {
namespace {

constexpr int kLogicalDpi = 96;
constexpr double kDefaultLogicalScale = 1.25;   // 约 430×315 点,与 Windows 150% 下的逻辑大小相当
constexpr double kZoomStep = 1.1;
constexpr NSTimeInterval kHoverPollSeconds = 1.0 / 30.0;
constexpr NSTimeInterval kGesturePollSeconds = 1.0 / 60.0;
constexpr NSTimeInterval kRevealFallbackSeconds = 3.0;
constexpr std::size_t kMaxActionBytes = 16384;

enum MenuCommand : NSInteger { kMenuDock = 1, kMenuHide, kMenuZoomIn, kMenuZoomOut, kMenuResetSize };

// 页面以 WebView2 的接口写成;这里在文档开始前补一个同名实现。
constexpr const char* kBridgeShim = R"JS((() => {
  const listeners = [];
  const handler = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.acePet;
  if (!handler) return;
  window.chrome = window.chrome || {};
  window.chrome.webview = {
    postMessage: value => handler.postMessage(typeof value === 'string' ? value : JSON.stringify(value)),
    addEventListener: (type, listener) => { if (type === 'message') listeners.push(listener); },
    removeEventListener: (type, listener) => { const i = listeners.indexOf(listener); if (i >= 0) listeners.splice(i, 1); },
  };
  window.__acePetDeliver = data => { for (const listener of listeners.slice()) { try { listener({ data }); } catch (e) {} } };
})();)JS";

bool desktop_pet_disabled_by_env() {
    const char* value = std::getenv("ACECODE_DESKTOP_PET");
    if (!value) return false;
    const std::string text(value);
    return text == "0" || text == "off" || text == "false" || text == "no";
}

bool chinese_ui() {
    return native_locale().rfind("zh", 0) == 0;
}

NSString* ns_string(const std::string& value) {
    NSString* text = [[NSString alloc] initWithBytes:value.data()
                                              length:value.size()
                                            encoding:NSUTF8StringEncoding];
    return text ? text : @"";
}

// 不放进 ACECode 数据目录:数据目录迁移会把持续写入的文件当成活文件。
std::filesystem::path pet_root_dir() {
    NSArray<NSURL*>* urls = [[NSFileManager defaultManager]
        URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask];
    NSURL* base = urls.firstObject;
    const char* path = base ? base.fileSystemRepresentation : nullptr;
    std::filesystem::path root = path ? std::filesystem::path(path)
                                      : std::filesystem::temp_directory_path();
    return root / "ACECode" / "desktop-pet";
}

double load_logical_scale() {
    std::ifstream in(pet_root_dir() / "settings.json");
    if (!in) return 0.0;
    const auto json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("scale") || !json["scale"].is_number()) return 0.0;
    const double scale = json["scale"].get<double>();
    return std::isfinite(scale) && scale > 0.0 ? scale : 0.0;
}

void save_logical_scale(double scale) {
    std::error_code ec;
    std::filesystem::create_directories(pet_root_dir(), ec);
    nlohmann::json json = nlohmann::json::object();
    if (scale > 0.0) json["scale"] = std::round(scale * 1000.0) / 1000.0;
    std::ofstream out(pet_root_dir() / "settings.json", std::ios::trunc);
    if (out) out << json.dump();
}

// 全局坐标翻转以主屏(菜单栏所在的屏)高度为准。
CGFloat flip_height() {
    NSScreen* primary = NSScreen.screens.firstObject;
    return primary ? primary.frame.size.height : 0.0;
}

DesktopPetRect top_left_rect(NSRect rect) {
    return {static_cast<int>(std::lround(rect.origin.x)),
            static_cast<int>(std::lround(flip_height() - (rect.origin.y + rect.size.height))),
            static_cast<int>(std::lround(rect.size.width)),
            static_cast<int>(std::lround(rect.size.height))};
}

NSRect cocoa_rect(const DesktopPetRect& rect) {
    return NSMakeRect(rect.x, flip_height() - (rect.y + rect.height), rect.width, rect.height);
}

} // namespace

// GUI 线程上的全部状态;ObjC 回调只持有它的 weak_ptr。
class PetController : public std::enable_shared_from_this<PetController> {
public:
    explicit PetController(WebHost& owner) : host_(owner) {}
    virtual ~PetController() { close(); }

    std::function<void()> on_closed;
    void update_snapshot(const nlohmann::json& value);
    bool start();
    void close();

    void on_web_message(const std::string& message);
    void navigation_finished(bool success);
    void web_process_terminated();
    void poll_hover();
    void poll_gesture();
    void menu_command(NSInteger command);
    void screens_changed();

private:
    void publish_snapshot();
    void deliver(const nlohmann::json& value);
    void office_action(const std::string& message);
    void reveal();
    void apply_placement(const DesktopPetPlacement& next);
    DesktopPetRect window_rect() const;
    DesktopPetRect work_area() const;
    void notify_size();
    void apply_scale(double scale, double fx, double fy);
    void zoom_by(int steps, double fx, double fy);
    void reset_size();
    void dock_to_corner();
    void begin_gesture(bool resize);
    void end_gesture();
    void show_menu();
    void load_page();
    double current_scale() const;

    WebHost& host_;   // DesktopPet is destroyed before this host.
    nlohmann::json office_snapshot_ = {{"follow", true}};
    std::vector<DesktopPetOverlay> overlays_;
    NSPanel* panel_ = nil;
    WKWebView* webview_ = nil;
    WKUserContentController* content_ = nil;
    ACECodeDesktopPetBridge* bridge_ = nil;
    ACECodeDesktopPetMenuTarget* menu_target_ = nil;
    id screen_observer_ = nil;
    NSTimer* hover_timer_ = nil;
    NSTimer* gesture_timer_ = nil;
    NSTimer* reveal_timer_ = nil;
    double logical_scale_ = 0.0;   // 用户调过的大小,0 = 默认
    bool page_ready_ = false;
    bool revealed_ = false;
    bool user_moved_ = false;
    bool closed_ = false;
    bool resizing_ = false;
    bool dragging_ = false;
    bool pinned_ = true;
    NSPoint gesture_mouse_ = NSZeroPoint;
    DesktopPetRect gesture_start_{};
};

} // namespace acecode::desktop

using acecode::desktop::PetController;

// 非激活面板:点击不会把 ACECode 主窗口的键盘焦点抢走。
@interface ACECodeDesktopPetPanel : NSPanel
@end

@implementation ACECodeDesktopPetPanel
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

// 面板不会成为 key window,第一下点击就要交给页面,而不是只用来激活窗口。
@interface ACECodeDesktopPetWebView : WKWebView
@end

@implementation ACECodeDesktopPetWebView
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}
@end

@interface ACECodeDesktopPetBridge : NSObject <WKScriptMessageHandler, WKNavigationDelegate>
- (instancetype)initWithController:(const std::weak_ptr<PetController>&)controller;
@end

@implementation ACECodeDesktopPetBridge {
    std::weak_ptr<PetController> _controller;
}

- (instancetype)initWithController:(const std::weak_ptr<PetController>&)controller {
    if ((self = [super init])) _controller = controller;
    return self;
}

- (void)userContentController:(WKUserContentController*)userContentController
      didReceiveScriptMessage:(WKScriptMessage*)message {
    (void)userContentController;
    if (![message.body isKindOfClass:[NSString class]]) return;
    const char* utf8 = [(NSString*)message.body UTF8String];
    if (!utf8) return;
    if (const auto controller = _controller.lock()) controller->on_web_message(utf8);
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    (void)webView;
    (void)navigation;
    if (const auto controller = _controller.lock()) controller->navigation_finished(true);
}

- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    (void)webView;
    (void)navigation;
    (void)error;
    if (const auto controller = _controller.lock()) controller->navigation_finished(false);
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
    (void)webView;
    (void)navigation;
    (void)error;
    if (const auto controller = _controller.lock()) controller->navigation_finished(false);
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView {
    (void)webView;
    if (const auto controller = _controller.lock()) controller->web_process_terminated();
}
@end

@interface ACECodeDesktopPetMenuTarget : NSObject
- (instancetype)initWithController:(const std::weak_ptr<PetController>&)controller;
- (void)choose:(NSMenuItem*)item;
@end

@implementation ACECodeDesktopPetMenuTarget {
    std::weak_ptr<PetController> _controller;
}

- (instancetype)initWithController:(const std::weak_ptr<PetController>&)controller {
    if ((self = [super init])) _controller = controller;
    return self;
}

- (void)choose:(NSMenuItem*)item {
    if (const auto controller = _controller.lock()) controller->menu_command(item.tag);
}
@end

namespace acecode::desktop {

void PetController::update_snapshot(const nlohmann::json& value) {
    office_snapshot_ = value;
    publish_snapshot();
}

bool PetController::start() {
    logical_scale_ = load_logical_scale();
    const DesktopPetPlacement placement = place_desktop_pet(
        work_area(), kLogicalDpi, logical_scale_ > 0.0 ? logical_scale_ : kDefaultLogicalScale);

    panel_ = [[ACECodeDesktopPetPanel alloc]
        initWithContentRect:cocoa_rect(placement.window)
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered
                      defer:NO];
    if (!panel_) {
        LOG_WARN("[desktop-pet] failed to create the macOS panel");
        return false;
    }
    panel_.releasedWhenClosed = NO;
    panel_.opaque = NO;
    panel_.backgroundColor = NSColor.clearColor;
    panel_.hasShadow = NO;
    panel_.level = NSFloatingWindowLevel;
    panel_.floatingPanel = YES;
    panel_.hidesOnDeactivate = NO;
    panel_.becomesKeyOnlyIfNeeded = YES;
    panel_.movableByWindowBackground = NO;
    panel_.acceptsMouseMovedEvents = YES;
    panel_.ignoresMouseEvents = YES;   // 首帧之前完全透明,也不挡点击
    panel_.alphaValue = 0.0;
    panel_.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                NSWindowCollectionBehaviorFullScreenAuxiliary |
                                NSWindowCollectionBehaviorStationary |
                                NSWindowCollectionBehaviorIgnoresCycle;

    const std::weak_ptr<PetController> weak = weak_from_this();
    bridge_ = [[ACECodeDesktopPetBridge alloc] initWithController:weak];
    menu_target_ = [[ACECodeDesktopPetMenuTarget alloc] initWithController:weak];

    content_ = [[WKUserContentController alloc] init];
    WKUserScript* shim = [[WKUserScript alloc] initWithSource:ns_string(kBridgeShim)
                                                injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                             forMainFrameOnly:YES];
    [content_ addUserScript:shim];
    [content_ addScriptMessageHandler:bridge_ name:@"acePet"];

    WKWebViewConfiguration* configuration = [[WKWebViewConfiguration alloc] init];
    configuration.userContentController = content_;
    // 页面是嵌入的离线资源,不需要持久的网站数据。
    configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];

    const NSRect bounds = NSMakeRect(0, 0, placement.window.width, placement.window.height);
    webview_ = [[ACECodeDesktopPetWebView alloc] initWithFrame:bounds configuration:configuration];
    if (!webview_) {
        LOG_WARN("[desktop-pet] failed to create the macOS WKWebView");
        return false;
    }
    webview_.navigationDelegate = bridge_;
    webview_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    webview_.allowsMagnification = NO;
    webview_.allowsBackForwardNavigationGestures = NO;
    // 透明背景:页面里没画东西的地方直接透出桌面。
    [webview_ setValue:@NO forKey:@"drawsBackground"];
    if (@available(macOS 12.0, *)) webview_.underPageBackgroundColor = NSColor.clearColor;
    panel_.contentView = webview_;

    screen_observer_ = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationDidChangeScreenParametersNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* note) {
                    (void)note;
                    if (const auto self = weak.lock()) self->screens_changed();
                }];

    hover_timer_ = [NSTimer timerWithTimeInterval:kHoverPollSeconds
                                          repeats:YES
                                            block:^(NSTimer* timer) {
                                                const auto self = weak.lock();
                                                if (self) self->poll_hover();
                                                else [timer invalidate];
                                            }];
    [[NSRunLoop mainRunLoop] addTimer:hover_timer_ forMode:NSRunLoopCommonModes];

    // 透明地挂上屏幕,让 WebKit 按可见窗口渲染首帧;页面发 ready 后再显形。
    [panel_ orderFrontRegardless];
    load_page();
    return true;
}

void PetController::load_page() {
    const std::string page(reinterpret_cast<const char*>(acecode::desktop_pet_page_data()),
                           acecode::desktop_pet_page_size());
    [webview_ loadHTMLString:ns_string(page) baseURL:nil];
}

void PetController::navigation_finished(bool success) {
    if (closed_ || revealed_) return;
    if (!success) {
        LOG_WARN("[desktop-pet] page navigation failed");
        close();
        return;
    }
    if (reveal_timer_) return;
    const std::weak_ptr<PetController> weak = weak_from_this();
    reveal_timer_ = [NSTimer scheduledTimerWithTimeInterval:kRevealFallbackSeconds
                                                    repeats:NO
                                                      block:^(NSTimer* timer) {
                                                          (void)timer;
                                                          const auto self = weak.lock();
                                                          if (!self || self->revealed_) return;
                                                          LOG_WARN("[desktop-pet] page ready signal missing; revealing after timeout");
                                                          self->reveal();
                                                      }];
}

void PetController::web_process_terminated() {
    if (closed_) return;
    // WebContent 进程被系统回收后页面是空白的:重新加载,ready 后重发快照。
    LOG_WARN("[desktop-pet] web content process terminated; reloading");
    page_ready_ = false;
    load_page();
}

void PetController::reveal() {
    if (revealed_ || closed_ || !panel_) return;
    revealed_ = true;
    [reveal_timer_ invalidate];
    reveal_timer_ = nil;
    panel_.alphaValue = 1.0;
    [panel_ orderFrontRegardless];
    const auto rect = window_rect();
    LOG_INFO("[desktop-pet] shown at " + std::to_string(rect.x) + "," + std::to_string(rect.y) +
             " size " + std::to_string(rect.width) + "x" + std::to_string(rect.height));
}

void PetController::deliver(const nlohmann::json& value) {
    if (closed_ || !webview_) return;
    const std::string script = "window.__acePetDeliver&&window.__acePetDeliver(" + value.dump() + ");";
    [webview_ evaluateJavaScript:ns_string(script) completionHandler:nil];
}

void PetController::publish_snapshot() {
    if (closed_ || !page_ready_ || !office_snapshot_.contains("version")) return;
    deliver(office_snapshot_);
}

void PetController::on_web_message(const std::string& message) {
    if (closed_ || !panel_) return;
    if (message == "ready") {
        page_ready_ = true;
        reveal();
        publish_snapshot();
        deliver({{"type", "pet-window-state"}, {"pinned", pinned_}});
    } else if (!message.empty() && message.front() == '{') {
        office_action(message);
    } else if (message == "drag") {
        begin_gesture(false);
    } else if (message == "resize") {
        begin_gesture(true);
    } else if (message == "menu") {
        // 回到下一轮事件循环再弹菜单,不在 WebKit 的回调里进入模态菜单循环。
        const std::weak_ptr<PetController> weak = weak_from_this();
        dispatch_async(dispatch_get_main_queue(), ^{
            if (const auto self = weak.lock()) self->show_menu();
        });
    } else if (message == "size-reset") {
        reset_size();
    } else if (message.rfind("zoom ", 0) == 0) {
        int steps = 0;
        double fx = 1.0, fy = 1.0;
        if (std::sscanf(message.c_str() + 5, "%d %lf %lf", &steps, &fx, &fy) >= 1) zoom_by(steps, fx, fy);
    }
}

void PetController::office_action(const std::string& message) {
    if (message.size() > kMaxActionBytes) return;
    const auto value = nlohmann::json::parse(message, nullptr, false);
    if (!value.is_object()) return;
    const auto type_field = value.find("type");
    if (type_field == value.end() || !type_field->is_string()) return;
    const auto type = type_field->get<std::string>();
    if (type == "pin") {
        const auto field = value.find("pinned");
        if (field == value.end() || !field->is_boolean()) return;
        pinned_ = field->get<bool>();
        panel_.floatingPanel = pinned_;
        panel_.level = pinned_ ? NSFloatingWindowLevel : NSNormalWindowLevel;
        deliver({{"type", "pet-window-state"}, {"pinned", pinned_}});
        return;
    }
    if (type == "close") {
        // Leave the WebKit message callback before releasing the view and bridge.
        const std::weak_ptr<PetController> weak = weak_from_this();
        dispatch_async(dispatch_get_main_queue(), ^{
            if (const auto self = weak.lock()) self->close();
        });
        return;
    }
    if (type == "overlay") {
        overlays_ = desktop_pet_overlays_from_message(value);
        return;
    }
    if (type != "select" && type != "follow" && type != "open") return;
    host_.eval("window.dispatchEvent(new CustomEvent('ace-desktop-office-action',{detail:" +
               value.dump() + "}));");
}

DesktopPetRect PetController::window_rect() const {
    return panel_ ? top_left_rect(panel_.frame) : DesktopPetRect{};
}

DesktopPetRect PetController::work_area() const {
    NSScreen* screen = panel_ && panel_.screen ? panel_.screen : NSScreen.mainScreen;
    if (!screen) screen = NSScreen.screens.firstObject;
    return screen ? top_left_rect(screen.visibleFrame) : DesktopPetRect{0, 0, 1440, 900};
}

double PetController::current_scale() const {
    const auto rect = window_rect();
    return rect.width > 0 ? rect.width / static_cast<double>(kDesktopPetSceneWidth) : 1.0;
}

void PetController::apply_placement(const DesktopPetPlacement& next) {
    if (!panel_) return;
    [panel_ setFrame:cocoa_rect(next.window) display:YES];
}

void PetController::notify_size() {
    const double base = kDefaultLogicalScale;
    const int percent = static_cast<int>(std::lround(current_scale() / base * 100.0));
    deliver("size " + std::to_string(percent));
}

void PetController::apply_scale(double scale, double fx, double fy) {
    const DesktopPetRect work = work_area();
    const double clamped = clamp_desktop_pet_scale(scale, kLogicalDpi, work);
    apply_placement(scale_desktop_pet(window_rect(), clamped, fx, fy, work));
    logical_scale_ = clamped;
    notify_size();
}

void PetController::zoom_by(int steps, double fx, double fy) {
    if (!revealed_ || closed_ || steps == 0) return;
    // 停在右下角时锚在右下角(缩放后还贴着角);拖动过之后以鼠标所在点为中心。
    if (!user_moved_) { fx = 1.0; fy = 1.0; }
    apply_scale(current_scale() * std::pow(kZoomStep, steps), fx, fy);
    save_logical_scale(logical_scale_);
}

void PetController::reset_size() {
    if (!revealed_ || closed_) return;
    logical_scale_ = 0.0;
    save_logical_scale(0.0);
    if (!user_moved_) dock_to_corner();
    else apply_placement(scale_desktop_pet(window_rect(), kDefaultLogicalScale, 1.0, 1.0, work_area()));
    notify_size();
}

void PetController::dock_to_corner() {
    apply_placement(place_desktop_pet(work_area(), kLogicalDpi,
                                      logical_scale_ > 0.0 ? logical_scale_ : kDefaultLogicalScale));
}

void PetController::screens_changed() {
    if (closed_ || !revealed_) return;
    if (!user_moved_) {
        dock_to_corner();
        return;
    }
    // 拖到别处后换了显示器排列:只把窗口夹回当前工作区。
    apply_placement(scale_desktop_pet(window_rect(), current_scale(), 0.0, 0.0, work_area()));
}

void PetController::begin_gesture(bool resize) {
    if (!revealed_ || closed_ || dragging_ || resizing_) return;
    dragging_ = !resize;
    resizing_ = resize;
    gesture_mouse_ = [NSEvent mouseLocation];
    gesture_start_ = window_rect();
    panel_.ignoresMouseEvents = NO;
    const std::weak_ptr<PetController> weak = weak_from_this();
    gesture_timer_ = [NSTimer timerWithTimeInterval:kGesturePollSeconds
                                            repeats:YES
                                              block:^(NSTimer* timer) {
                                                  const auto self = weak.lock();
                                                  if (self) self->poll_gesture();
                                                  else [timer invalidate];
                                              }];
    [[NSRunLoop mainRunLoop] addTimer:gesture_timer_ forMode:NSRunLoopCommonModes];
}

void PetController::end_gesture() {
    [gesture_timer_ invalidate];
    gesture_timer_ = nil;
    if (dragging_) user_moved_ = true;
    if (resizing_) save_logical_scale(logical_scale_);
    dragging_ = false;
    resizing_ = false;
}

void PetController::poll_gesture() {
    if (closed_ || (!dragging_ && !resizing_)) {
        end_gesture();
        return;
    }
    if (([NSEvent pressedMouseButtons] & 1) == 0) {
        end_gesture();
        return;
    }
    const NSPoint mouse = [NSEvent mouseLocation];
    // 屏幕坐标 y 向上;换成左上原点后 y 方向取反。
    const double dx = mouse.x - gesture_mouse_.x;
    const double dy = gesture_mouse_.y - mouse.y;
    if (dragging_) {
        DesktopPetRect next = gesture_start_;
        next.x += static_cast<int>(std::lround(dx));
        next.y += static_cast<int>(std::lround(dy));
        [panel_ setFrameOrigin:cocoa_rect(next).origin];
        return;
    }
    // 把手在左上方:往左上拖变大、右下角不动;横竖两个方向取放大得多的那个。
    const double by_x = gesture_start_.width - dx;
    const double by_y = (gesture_start_.height - dy) * kDesktopPetSceneWidth /
                        static_cast<double>(kDesktopPetSceneHeight);
    const DesktopPetRect work = work_area();
    const double scale = clamp_desktop_pet_scale(std::max(by_x, by_y) / kDesktopPetSceneWidth, kLogicalDpi, work);
    const auto next = scale_desktop_pet(gesture_start_, scale, 1.0, 1.0, work);
    const auto current = window_rect();
    if (next.window.width == current.width && next.window.x == current.x && next.window.y == current.y) return;
    apply_placement(next);
    logical_scale_ = scale;
    notify_size();
}

void PetController::poll_hover() {
    if (closed_ || !panel_ || !revealed_) return;
    if (dragging_ || resizing_) {
        panel_.ignoresMouseEvents = NO;
        return;
    }
    const NSPoint mouse = [NSEvent mouseLocation];
    const NSRect frame = panel_.frame;
    const double x = mouse.x - frame.origin.x;
    const double y = frame.origin.y + frame.size.height - mouse.y;
    const bool hit = desktop_pet_hit_test(current_scale(), static_cast<int>(frame.size.width),
                                          static_cast<int>(frame.size.height), overlays_, x, y);
    if (panel_.ignoresMouseEvents == hit) panel_.ignoresMouseEvents = !hit;
}

void PetController::show_menu() {
    if (closed_ || !panel_) return;
    const bool zh = chinese_ui();
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;
    ACECodeDesktopPetMenuTarget* target = menu_target_;
    const auto add = [menu, target](NSString* title, NSInteger tag) {
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:@selector(choose:) keyEquivalent:@""];
        item.target = target;
        item.tag = tag;
        [menu addItem:item];
    };
    add(zh ? @"回到右下角" : @"Move to corner", kMenuDock);
    [menu addItem:[NSMenuItem separatorItem]];
    add(zh ? @"放大（也可以滚轮）" : @"Zoom in (or scroll)", kMenuZoomIn);
    add(zh ? @"缩小" : @"Zoom out", kMenuZoomOut);
    add(zh ? @"恢复默认大小" : @"Reset size", kMenuResetSize);
    [menu addItem:[NSMenuItem separatorItem]];
    add(zh ? @"隐藏桌面宠物" : @"Hide desktop pet", kMenuHide);
    [menu popUpMenuPositioningItem:nil atLocation:[NSEvent mouseLocation] inView:nil];
}

void PetController::menu_command(NSInteger command) {
    if (closed_) return;
    switch (command) {
    case kMenuDock:
        user_moved_ = false;
        dock_to_corner();
        break;
    case kMenuZoomIn: zoom_by(1, 0.5, 0.5); break;
    case kMenuZoomOut: zoom_by(-1, 0.5, 0.5); break;
    case kMenuResetSize: reset_size(); break;
    case kMenuHide:
        LOG_INFO("[desktop-pet] hidden from context menu");
        close();
        break;
    default: break;
    }
}

void PetController::close() {
    if (closed_) return;
    closed_ = true;
    [hover_timer_ invalidate];
    [gesture_timer_ invalidate];
    [reveal_timer_ invalidate];
    hover_timer_ = nil;
    gesture_timer_ = nil;
    reveal_timer_ = nil;
    if (screen_observer_) [[NSNotificationCenter defaultCenter] removeObserver:screen_observer_];
    screen_observer_ = nil;
    // WKUserContentController 强引用消息处理器,不移除会连带页面一起泄漏。
    [content_ removeScriptMessageHandlerForName:@"acePet"];
    [content_ removeAllUserScripts];
    webview_.navigationDelegate = nil;
    [webview_ stopLoading];
    [panel_ orderOut:nil];
    [panel_ close];
    webview_ = nil;
    content_ = nil;
    panel_ = nil;
    bridge_ = nil;
    menu_target_ = nil;
    if (on_closed) on_closed();
}

// DesktopPet 持有的实现:只是把控制器的所有权包起来。
struct DesktopPet::Impl : DesktopOfficeService<WebHost, PetController> {
    using DesktopOfficeService::DesktopOfficeService;
};

DesktopPet::DesktopPet(WebHost& host) {
    impl_ = std::make_shared<Impl>(host, pet_root_dir() / "office.json", ACECODE_VERSION,
        !desktop_pet_disabled_by_env(), [] {
            return std::string(reinterpret_cast<const char*>(acecode::desktop_pet_page_data()),
                               acecode::desktop_pet_page_size());
        });
    impl_->bind_bridge();
}

DesktopPet::~DesktopPet() {
    impl_.reset();
}

} // namespace acecode::desktop
