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
// 控制条或页面上报的浮层里,再切换 ignoresMouseEvents。原生按下时即保留鼠标
// 接收和起点,页面决定拖动 / 缩放后由定时器从原始起点跟随。
//
// 坐标:Cocoa 屏幕坐标原点在左下;desktop_pet_layout 按左上原点、设备像素计算。
// 这里统一换成左上原点的「点」,并把 dpi 固定为 96(1 点 = 1 个逻辑像素,
// Retina 的倍率由 WebKit 按 devicePixelRatio 处理)。
//
// 本文件用 ARC 编译(cmake/acecode_desktop.cmake 单独加 -fobjc-arc)。

#include "desktop_pet.hpp"
#include "web_host.hpp"

#include "desktop_pet_layout.hpp"
#include "desktop_pet_mac_support.hpp"
#include "desktop_office_service.hpp"
#include "desktop_pet_resources.hpp"
#include "version.hpp"
#include "platform/native_ui/strings.hpp"
#include "tray_icon_win.hpp"

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

enum MenuCommand : NSInteger { kMenuDock = 1, kMenuHide, kMenuZoomIn, kMenuZoomOut, kMenuResetSize, kMenuPin,
                               kMenuPetKindBase = 100 /* + index into kDesktopPetKinds */ };

} // namespace

// GUI 线程上的全部状态;ObjC 回调只持有它的 weak_ptr。
class PetController : public std::enable_shared_from_this<PetController> {
public:
    explicit PetController(WebHost& owner) : host_(owner) {}
    virtual ~PetController() { close(); }

    std::function<void()> on_closed;
    // 右键菜单选了别的宠物:交给服务保存并切换(服务再回调 set_pet_kind)。
    std::function<void(const std::string&)> on_pet_kind;
    void update_snapshot(const nlohmann::json& value);
    void set_pet_kind(const std::string& kind);
    bool start();
    void close();

    void on_web_message(const std::string& message);
    void navigation_finished(bool success);
    void web_process_terminated();
    void poll_hover();
    void poll_gesture();
    void mouse_event(NSEvent* event);
    void update_cursor();
    void menu_command(NSInteger command);
    void screens_changed();

private:
    void publish_snapshot();
    void publish_character();
    void deliver(const nlohmann::json& value);
    void publish_drag(const char* phase, const char* direction = nullptr);
    void publish_dock();
    void settle_dock();
    DesktopPetPlacement keep_docked(DesktopPetPlacement next, const DesktopPetRect& work) const;
    void set_pinned(bool pinned);
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
    void move_gesture(NSPoint mouse);
    bool resize_grip_hit(NSPoint mouse) const;
    void release_cursor();
    void show_menu();
    void load_page();
    double current_scale() const;

    WebHost& host_;   // DesktopPet is destroyed before this host.
    nlohmann::json office_snapshot_ = {{"follow", true}};
    std::string pet_kind_ = "office";
    std::vector<DesktopPetOverlay> overlays_;
    std::vector<DesktopPetOverlay> foreground_overlays_;
    NSPanel* panel_ = nil;
    WKWebView* webview_ = nil;
    WKUserContentController* content_ = nil;
    ACECodeDesktopPetBridge* bridge_ = nil;
    ACECodeDesktopPetMenuTarget* menu_target_ = nil;
    id screen_observer_ = nil;
    NSTimer* hover_timer_ = nil;
    NSTimer* gesture_timer_ = nil;
    NSTimer* reveal_timer_ = nil;
    NSCursor* resize_cursor_ = nil;
    double logical_scale_ = 0.0;   // 用户调过的大小,0 = 默认
    bool page_ready_ = false;
    bool revealed_ = false;
    bool user_moved_ = false;
    bool closed_ = false;
    bool resizing_ = false;
    bool dragging_ = false;
    DesktopPetDragTracker drag_tracker_;   // 陪伴宠物按拖动方向播放左右跑动画
    int docked_side_ = 0;                  // 鲸鱼娘吸附在工作区左(-1)/右(+1)边,0 = 未吸附
    bool mouse_pressed_ = false;
    bool owns_cursor_ = false;
    bool pinned_ = true;
    NSPoint gesture_mouse_ = NSZeroPoint;
    DesktopPetRect gesture_start_{};
};

} // namespace acecode::desktop

using acecode::desktop::PetController;

// 非激活面板:点击不会把 ACECode 主窗口的键盘焦点抢走。
@interface ACECodeDesktopPetPanel : NSPanel
- (void)bindController:(const std::weak_ptr<PetController>&)controller;
@end

@implementation ACECodeDesktopPetPanel {
    std::weak_ptr<PetController> _controller;
}
- (void)bindController:(const std::weak_ptr<PetController>&)controller {
    _controller = controller;
}
- (void)sendEvent:(NSEvent*)event {
    const auto controller = _controller.lock();
    if (controller) controller->mouse_event(event);
    [super sendEvent:event];
    if (controller) controller->poll_hover();
}
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

// 面板不会成为 key window,第一下点击就要交给页面,而不是只用来激活窗口。
@interface ACECodeDesktopPetWebView : WKWebView
- (void)bindController:(const std::weak_ptr<PetController>&)controller;
@end

@implementation ACECodeDesktopPetWebView {
    std::weak_ptr<PetController> _controller;
    NSTrackingArea* _cursorTracking;
}
- (void)bindController:(const std::weak_ptr<PetController>&)controller {
    _controller = controller;
}
- (void)updateTrackingAreas {
    if (_cursorTracking) [self removeTrackingArea:_cursorTracking];
    [super updateTrackingAreas];
    // ActiveAlways does not deliver cursorUpdate. Use ordinary tracking events
    // so the non-key panel also updates its cursor while another app is active.
    _cursorTracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
        options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                NSTrackingActiveAlways | NSTrackingInVisibleRect | NSTrackingEnabledDuringMouseDrag
        owner:self userInfo:nil];
    [self addTrackingArea:_cursorTracking];
}
- (void)mouseEntered:(NSEvent*)event {
    [super mouseEntered:event];
    if (const auto controller = _controller.lock()) controller->poll_hover();
}
- (void)mouseExited:(NSEvent*)event {
    [super mouseExited:event];
    if (const auto controller = _controller.lock()) controller->poll_hover();
}
- (void)mouseMoved:(NSEvent*)event {
    [super mouseMoved:event];
    if (const auto controller = _controller.lock()) controller->update_cursor();
}
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
    [(ACECodeDesktopPetPanel*)panel_ bindController:weak];
    resize_cursor_ = make_resize_cursor();
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
    [(ACECodeDesktopPetWebView*)webview_ bindController:weak];
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
    const std::string page = desktop_pet_html(pet_kind_);
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
    end_gesture();
    overlays_.clear();
    foreground_overlays_.clear();
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

void PetController::set_pet_kind(const std::string& kind) {
    if (pet_kind_ == kind) return;
    const bool same_page = pet_kind_ != "office" && kind != "office";
    pet_kind_ = kind;
    if (kind != "whale") docked_side_ = 0;   // 只有鲸鱼娘有趴边偷看的样子
    if (!webview_ || closed_) return;
    end_gesture();
    overlays_.clear();
    foreground_overlays_.clear();
    if (same_page) publish_character();
    else {
        page_ready_ = false;
        load_page();
    }
}

void PetController::set_pinned(bool pinned) {
    pinned_ = pinned;
    panel_.floatingPanel = pinned_;
    panel_.level = pinned_ ? NSFloatingWindowLevel : NSNormalWindowLevel;
    deliver({{"type", "pet-window-state"}, {"pinned", pinned_}});
}

void PetController::publish_dock() {
    if (closed_ || !page_ready_ || pet_kind_ == "office") return;
    deliver({{"type", "pet-dock"}, {"side", docked_side_ < 0 ? "left" : docked_side_ > 0 ? "right" : ""}});
}

// 拖动松手后:鲸鱼娘离左/右边足够近就贴到那条边,页面换成趴边偷看;
// 其它情况把窗口收回可见区域,头顶的会话卡片不会被屏幕边截掉。
void PetController::settle_dock() {
    const DesktopPetRect work = work_area();
    const DesktopPetRect current = window_rect();
    docked_side_ = pet_kind_ == "whale" ? desktop_pet_dock_side(current, work) : 0;
    const DesktopPetRect next = docked_side_ ? dock_desktop_pet(current, work, docked_side_)
                                             : keep_desktop_pet_in_work_area(current, work);
    if (next.x != current.x || next.y != current.y) apply_placement({next, current_scale()});
    publish_dock();
}

// 缩放后仍然贴着原来那条边。
DesktopPetPlacement PetController::keep_docked(DesktopPetPlacement next, const DesktopPetRect& work) const {
    if (docked_side_) next.window = dock_desktop_pet(next.window, work, docked_side_);
    return next;
}

void PetController::publish_drag(const char* phase, const char* direction) {
    if (closed_ || !page_ready_ || pet_kind_ == "office") return;
    nlohmann::json event = {{"type", "pet-drag"}, {"phase", phase}};
    if (direction) event["direction"] = direction;
    deliver(event);
}

void PetController::publish_character() {
    if (!closed_ && page_ready_ && pet_kind_ != "office") deliver(desktop_pet_character(pet_kind_));
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
        publish_character();
        publish_snapshot();
        deliver({{"type", "pet-window-state"}, {"pinned", pinned_}});
        publish_dock();
    } else if (!message.empty() && message.front() == '{') {
        office_action(message);
    } else if (message == "drag") {
        begin_gesture(false);
    } else if (message == "resize") {
        begin_gesture(true);
    } else if (message == "gesture-cancel") {
        end_gesture();
        poll_hover();
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
        set_pinned(field->get<bool>());
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
        // Bubbles are below the handle and pointer-transparent. Controls and
        // the member list/notice are above it and must retain their own cursor.
        auto foreground = value;
        foreground.erase("bubbles");
        foreground_overlays_ = desktop_pet_overlays_from_message(foreground);
        return;
    }
    // 双击任何宠物都激活主窗口(与系统通知点击相同);带会话时再交给主页面切换到该会话。
    if (type == "activate" || type == "open") mac_bring_window_foreground(host_.native_window());
    // stop / read 来自陪伴宠物的会话卡片(停止该会话的回合、把完成结果标为已读)。
    if (type != "select" && type != "follow" && type != "open" && type != "stop" && type != "read") return;
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
    apply_placement(keep_docked(scale_desktop_pet(window_rect(), clamped, fx, fy, work), work));
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
    else apply_placement(keep_docked(scale_desktop_pet(window_rect(), kDefaultLogicalScale, 1.0, 1.0, work_area()), work_area()));
    notify_size();
}

void PetController::dock_to_corner() {
    if (docked_side_) {
        docked_side_ = 0;
        publish_dock();
    }
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

void PetController::mouse_event(NSEvent* event) {
    if (closed_ || !panel_ || !revealed_ || !page_ready_) return;
    if (event.type == NSEventTypeLeftMouseDown) {
        end_gesture();
        mouse_pressed_ = true;
        gesture_mouse_ = [panel_ convertPointToScreen:event.locationInWindow];
        gesture_start_ = window_rect();
        panel_.ignoresMouseEvents = NO;
    } else if (event.type == NSEventTypeLeftMouseUp) {
        move_gesture([NSEvent mouseLocation]);
        end_gesture();
    }
}

void PetController::begin_gesture(bool resize) {
    if (!revealed_ || closed_ || !mouse_pressed_ || dragging_ || resizing_) return;
    // The bridge is asynchronous: a request can arrive after the native up.
    if (([NSEvent pressedMouseButtons] & 1) == 0) {
        end_gesture();
        return;
    }
    dragging_ = !resize;
    resizing_ = resize;
    if (dragging_) {
        docked_side_ = 0;   // 页面收到 start 就离开趴边姿势
        drag_tracker_.begin(gesture_mouse_.x);
        publish_drag("start");
    }
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
    poll_gesture();
}

void PetController::end_gesture() {
    [gesture_timer_ invalidate];
    gesture_timer_ = nil;
    const bool dropped = dragging_;
    if (dragging_) {
        user_moved_ = true;
        publish_drag("end");
    }
    if (resizing_) save_logical_scale(logical_scale_);
    dragging_ = false;
    resizing_ = false;
    mouse_pressed_ = false;
    release_cursor();
    if (dropped && !closed_ && panel_) settle_dock();
}

void PetController::poll_gesture() {
    if (closed_ || (!dragging_ && !resizing_)) {
        end_gesture();
        return;
    }
    if (([NSEvent pressedMouseButtons] & 1) == 0) {
        move_gesture([NSEvent mouseLocation]);
        end_gesture();
        poll_hover();
        return;
    }
    move_gesture([NSEvent mouseLocation]);
    update_cursor();
}

void PetController::move_gesture(NSPoint mouse) {
    if (closed_ || !panel_ || (!dragging_ && !resizing_)) return;
    // 屏幕坐标 y 向上;换成左上原点后 y 方向取反。
    const double dx = mouse.x - gesture_mouse_.x;
    const double dy = gesture_mouse_.y - mouse.y;
    if (dragging_) {
        if (const int direction = drag_tracker_.move(mouse.x)) publish_drag("move", direction < 0 ? "left" : "right");
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
    if (!page_ready_) {
        panel_.ignoresMouseEvents = YES;
        release_cursor();
        return;
    }
    // Protect the whole press, including the interval before WebKit reports
    // crossing the drag threshold. Otherwise a fast move enables click-through.
    if (mouse_pressed_ && ([NSEvent pressedMouseButtons] & 1) != 0) {
        panel_.ignoresMouseEvents = NO;
        update_cursor();
        return;
    }
    if (mouse_pressed_) {
        move_gesture([NSEvent mouseLocation]);
        end_gesture();
    }
    const NSPoint mouse = [NSEvent mouseLocation];
    const NSRect frame = panel_.frame;
    const double x = mouse.x - frame.origin.x;
    const double y = frame.origin.y + frame.size.height - mouse.y;
    const bool hit = desktop_pet_hit_test(current_scale(), static_cast<int>(frame.size.width),
                                          static_cast<int>(frame.size.height), overlays_, x, y, pet_kind_ == "office") ||
                     resize_grip_hit(mouse);
    if (panel_.ignoresMouseEvents == hit) panel_.ignoresMouseEvents = !hit;
    update_cursor();
}

bool PetController::resize_grip_hit(NSPoint mouse) const {
    const NSRect frame = panel_.frame;
    const double x = mouse.x - frame.origin.x;
    const double y = frame.origin.y + frame.size.height - mouse.y;
    if (!desktop_pet_resize_grip_hit_test(frame.size.width, frame.size.height, x, y)) return false;
    return std::none_of(foreground_overlays_.begin(), foreground_overlays_.end(), [&](const auto& rect) {
        return x >= rect[0] * frame.size.width && x < rect[2] * frame.size.width &&
               y >= rect[1] * frame.size.height && y < rect[3] * frame.size.height;
    });
}

void PetController::release_cursor() {
    if (owns_cursor_ && NSCursor.currentCursor == resize_cursor_) [NSCursor.arrowCursor set];
    owns_cursor_ = false;
}

void PetController::update_cursor() {
    if (closed_ || !panel_ || !revealed_ || !page_ready_) {
        release_cursor();
        return;
    }
    const NSPoint mouse = [NSEvent mouseLocation];
    const bool hovering = !panel_.ignoresMouseEvents && resize_grip_hit(mouse) &&
        [NSWindow windowNumberAtPoint:mouse belowWindowWithWindowNumber:0] == panel_.windowNumber;
    if (resizing_ || (!dragging_ && hovering)) {
        [resize_cursor_ set];
        owns_cursor_ = true;
    } else {
        release_cursor();
    }
}

void PetController::show_menu() {
    if (closed_ || !panel_) return;
    end_gesture();
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
    // 二级菜单「切换宠物」,当前宠物打勾。
    NSMenu* kinds = [[NSMenu alloc] initWithTitle:@""];
    kinds.autoenablesItems = NO;
    for (NSInteger i = 0; i < static_cast<NSInteger>(kDesktopPetKinds.size()); ++i) {
        const auto& kind = kDesktopPetKinds[static_cast<std::size_t>(i)];
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:ns_string(zh ? kind.zh : kind.en)
                                                      action:@selector(choose:) keyEquivalent:@""];
        item.target = target;
        item.tag = kMenuPetKindBase + i;
        item.state = pet_kind_ == kind.id ? NSControlStateValueOn : NSControlStateValueOff;
        [kinds addItem:item];
    }
    NSMenuItem* switcher = [[NSMenuItem alloc] initWithTitle:(zh ? @"切换宠物" : @"Switch pet") action:nil keyEquivalent:@""];
    switcher.submenu = kinds;
    [menu addItem:switcher];
    [menu addItem:[NSMenuItem separatorItem]];
    add(zh ? @"回到右下角" : @"Move to corner", kMenuDock);
    add(zh ? @"保持置顶" : @"Keep on top", kMenuPin);
    menu.itemArray.lastObject.state = pinned_ ? NSControlStateValueOn : NSControlStateValueOff;
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
    if (command >= kMenuPetKindBase && command < kMenuPetKindBase + static_cast<NSInteger>(kDesktopPetKinds.size())) {
        const std::string kind = kDesktopPetKinds[static_cast<std::size_t>(command - kMenuPetKindBase)].id;
        LOG_INFO("[desktop-pet] switched to " + kind + " from context menu");
        if (kind != pet_kind_ && on_pet_kind) on_pet_kind(kind);
        return;
    }
    switch (command) {
    case kMenuPin: set_pinned(!pinned_); break;
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
    end_gesture();
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
    resize_cursor_ = nil;
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
