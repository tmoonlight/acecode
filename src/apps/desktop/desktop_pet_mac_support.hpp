#pragma once

// macOS 桌宠宿主(desktop_pet_mac.mm)的辅助部分:注入页面的 chrome.webview 桥垫片、
// 环境开关与界面语言、缩放偏好读写、Cocoa(左下原点)与左上原点坐标换算、缩放把手光标。
// 只由 desktop_pet_mac.mm 包含(ObjC++,ARC);从那里拆出来是为了守住单文件 1000 行上限。

#include "desktop_pet_layout.hpp"
#include "platform/native_ui/strings.hpp"

#import <AppKit/AppKit.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace acecode::desktop {
namespace {

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
  // Keep the threshold-crossing move and release on the original element even
  // when the pointer leaves the canvas before WebKit posts the drag request.
  let captured = null;
  let suppressClick = false;
  document.addEventListener('pointerdown', event => {
    if (event.button !== 0) return;
    const target = event.target.closest?.('#cv, #grip');
    if (!target) return;
    target.setPointerCapture(event.pointerId);
    captured = { target, id: event.pointerId, x: event.clientX, y: event.clientY, dragged: false };
    suppressClick = false;
    // macOS owns the canvas threshold here. WebKit can omit screen coordinates;
    // client coordinates suffice until the first native window movement.
    if (target.id === 'cv') event.stopPropagation();
  }, true);
  document.addEventListener('pointermove', event => {
    if (captured?.id !== event.pointerId || captured.target.id !== 'cv') return;
    event.stopPropagation();
    if (!(event.buttons & 1) || captured.dragged) return;
    if (Math.abs(event.clientX - captured.x) + Math.abs(event.clientY - captured.y) < 4) return;
    captured.dragged = true;
    suppressClick = true;
    handler.postMessage('drag');
  }, true);
  document.addEventListener('pointerup', event => {
    if (captured?.id === event.pointerId) captured = null;
  }, true);
  document.addEventListener('click', event => {
    if (!suppressClick || event.target.id !== 'cv') return;
    event.preventDefault();
    event.stopImmediatePropagation();
    suppressClick = false;
  }, true);
  const cancel = event => {
    if (captured?.id !== event.pointerId) return;
    captured = null;
    suppressClick = true;
    handler.postMessage('gesture-cancel');
  };
  document.addEventListener('pointercancel', cancel, true);
  document.addEventListener('lostpointercapture', cancel, true);
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

NSCursor* make_resize_cursor() {
    // Public AppKit APIs on every supported macOS version. A custom image avoids
    // relying on WebKit's platform-dependent mapping of CSS nwse-resize.
    NSImage* image = [NSImage imageWithSize:NSMakeSize(24, 24) flipped:NO
        drawingHandler:^BOOL(NSRect rect) {
            (void)rect;
            NSBezierPath* path = [NSBezierPath bezierPath];
            [path moveToPoint:NSMakePoint(5, 19)];
            [path lineToPoint:NSMakePoint(19, 5)];
            [path moveToPoint:NSMakePoint(5, 12)];
            [path lineToPoint:NSMakePoint(5, 19)];
            [path lineToPoint:NSMakePoint(12, 19)];
            [path moveToPoint:NSMakePoint(12, 5)];
            [path lineToPoint:NSMakePoint(19, 5)];
            [path lineToPoint:NSMakePoint(19, 12)];
            path.lineJoinStyle = NSRoundLineJoinStyle;
            path.lineCapStyle = NSRoundLineCapStyle;
            [NSColor.whiteColor setStroke];
            path.lineWidth = 4;
            [path stroke];
            [NSColor.blackColor setStroke];
            path.lineWidth = 2;
            [path stroke];
            return YES;
        }];
    return [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(12, 12)];
}

} // namespace
} // namespace acecode::desktop
