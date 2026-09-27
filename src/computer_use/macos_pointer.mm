#include "macos_native.hpp"
#include "config/vocab/pointer_appearance.hpp"

@interface ACEComputerPointerPanel : NSPanel
@end
@implementation ACEComputerPointerPanel
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

namespace acecode::computer_use::macos {
namespace {
struct Pointer {
    __strong ACEComputerPointerPanel* panel = nil;
    __strong NSImageView* view = nil;
    bool visible = false, pressed = false;
    CGPoint point{};
    std::string style = "ace", color = "#2563eb";
};
Pointer& state() { static Pointer pointer; return pointer; }
void main_sync(dispatch_block_t block) {
    if (NSThread.isMainThread) block(); else dispatch_sync(dispatch_get_main_queue(), block);
}
void draw(CGContextRef context, const std::string& style, const std::string& color, bool pressed) {
    const auto component = [&](std::size_t offset) { return std::stoi(color.substr(offset, 2), nullptr, 16) / 255.0; };
    CGContextSetRGBFillColor(context, component(1), component(3), component(5), 1);
    CGContextSetRGBStrokeColor(context, 0.08, 0.08, 0.1, 0.95);
    CGContextSetLineWidth(context, 1.8);
    CGContextSetLineJoin(context, kCGLineJoinRound);
    const CGPoint points[]{{0, 0}, {0, 26}, {7, 20}, {13, 31}, {19, 28}, {13, 18}, {24, 18}};
    CGContextAddLines(context, points, 7); CGContextClosePath(context); CGContextDrawPath(context, kCGPathFillStroke);
    if (pressed) {
        CGContextSetRGBStrokeColor(context, component(1), component(3), component(5), 0.6);
        CGContextStrokeEllipseInRect(context, CGRectMake(-8, -8, 16, 16));
    }
    if (style == "ace") {
        CGContextFillRect(context, CGRectMake(20, 24, 25, 13));
        CGContextStrokeRect(context, CGRectMake(20, 24, 25, 13));
        static const int glyphs[3][7]{{14,17,17,31,17,17,17},{15,16,16,16,16,16,15},{31,16,16,30,16,16,31}};
        CGContextSetRGBFillColor(context, 1, 1, 1, 1);
        for (int letter = 0; letter < 3; ++letter) for (int row = 0; row < 7; ++row)
            for (int column = 0; column < 5; ++column) if (glyphs[letter][row] & (1 << (4 - column)))
                CGContextFillRect(context, CGRectMake(24 + letter * 6 + column, 27 + row, 1, 1));
    }
}
NSImage* image(const Pointer& pointer) {
    // Render in points; NSImage provides backing resolution for the target screen.
    const auto style = pointer.style, color = pointer.color;
    const bool pressed = pointer.pressed;
    return [NSImage imageWithSize:NSMakeSize(64, 56) flipped:YES drawingHandler:^BOOL(NSRect) {
        CGContextRef context = NSGraphicsContext.currentContext.CGContext;
        CGContextTranslateCTM(context, 10, 10);
        draw(context, style, color, pressed);
        return YES;
    }];
}
}
void pointer_configure(const std::string& style, const std::string& color) {
    if (!pointer_appearance::valid_style(style) || !pointer_appearance::normalize_color(color)) return;
    main_sync(^{ auto& pointer = state(); pointer.style = style; pointer.color = color;
        if (pointer.view) pointer.view.image = image(pointer); });
}
void pointer_show(CGPoint point, bool pressed) {
    main_sync(^{
        auto& pointer = state();
        if (!pointer.panel) {
            pointer.panel = [[ACEComputerPointerPanel alloc] initWithContentRect:NSMakeRect(0, 0, 64, 56)
                styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                backing:NSBackingStoreBuffered defer:NO];
            pointer.panel.opaque = NO; pointer.panel.backgroundColor = NSColor.clearColor;
            pointer.panel.hasShadow = NO; pointer.panel.ignoresMouseEvents = YES;
            pointer.panel.hidesOnDeactivate = NO;
            pointer.panel.level = NSStatusWindowLevel;
            pointer.panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
            pointer.view = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, 64, 56)];
            pointer.panel.contentView = pointer.view;
        }
        pointer.point = point; pointer.pressed = pressed; pointer.visible = true;
        pointer.view.image = image(pointer);
        // AppKit has a bottom-left origin; Quartz/AX have a top-left origin.
        const CGFloat top = NSScreen.screens.firstObject.frame.size.height;
        [pointer.panel setFrameOrigin:NSMakePoint(point.x - 10, top - point.y - 46)];
        [pointer.panel orderFrontRegardless];
    });
}
void pointer_hide() { main_sync(^{ state().visible = false; [state().panel orderOut:nil]; }); }
bool pointer_owns(CGWindowID id) {
    __block bool result = false;
    main_sync(^{ result = state().panel && static_cast<CGWindowID>(state().panel.windowNumber) == id; });
    return result;
}
json pointer_composite(CGContextRef context, const Surface& surface) {
    struct Snapshot { bool visible = false, pressed = false; CGPoint point{}; std::string style, color; };
    __block Snapshot snapshot;
    main_sync(^{ const auto& pointer = state(); snapshot = {pointer.visible, pointer.pressed, pointer.point, pointer.style, pointer.color}; });
    if (!snapshot.visible || !CGRectContainsPoint(surface.window.rect, snapshot.point)) return {{"visible", false}};
    // A pointer over an unrelated covering window must not be painted on this capture.
    try { validate_hit(snapshot.point, surface); } catch (const Error&) { return {{"visible", false}}; }
    const double scale_x = surface.width / surface.window.rect.size.width;
    const double scale_y = surface.height / surface.window.rect.size.height;
    const double x = (snapshot.point.x - surface.window.rect.origin.x) * scale_x;
    const double y = (snapshot.point.y - surface.window.rect.origin.y) * scale_y;
    CGContextSaveGState(context);
    CGContextTranslateCTM(context, x, surface.height - y);
    CGContextScaleCTM(context, scale_x, -scale_y);
    draw(context, snapshot.style, snapshot.color, snapshot.pressed);
    CGContextRestoreGState(context);
    return {{"visible", true}, {"source", "agent"}, {"x", x}, {"y", y},
        {"width", (snapshot.style == "ace" ? 45 : 24) * scale_x},
        {"height", (snapshot.style == "ace" ? 37 : 31) * scale_y}, {"hotspot_x", 0}, {"hotspot_y", 0}};
}
} // namespace acecode::computer_use::macos
