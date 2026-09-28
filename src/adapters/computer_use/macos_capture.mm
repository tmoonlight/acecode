#include "macos_native.hpp"
#import <ImageIO/ImageIO.h>

namespace acecode::computer_use::macos {
json capture(Surface& surface, const std::string& observation_id, std::size_t index, json& descriptor) {
    if (!CGPreflightScreenCaptureAccess()) throw Error("screen_recording_permission_required",
        "Allow Screen Recording for ACECode in System Settings > Privacy & Security, then refresh permissions.");
    struct Content {
        dispatch_semaphore_t ready = dispatch_semaphore_create(0);
        __strong SCShareableContent* value = nil;
        __strong NSError* error = nil;
    };
    auto content = std::make_shared<Content>();
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES
        completionHandler:^(SCShareableContent* value, NSError* error) {
            content->value = value; content->error = error; dispatch_semaphore_signal(content->ready);
        }];
    if (dispatch_semaphore_wait(content->ready, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) != 0)
        throw Error("capture_timeout", "ScreenCaptureKit did not enumerate windows in time.");
    if (!content->value) throw Error("capture_unavailable", text(content->error.localizedDescription));
    SCWindow* target = nil;
    for (SCWindow* candidate in content->value.windows) {
        if (candidate.windowID == surface.window.id && candidate.owningApplication.processID == surface.window.pid) { target = candidate; break; }
    }
    if (!target || !same_rect(target.frame, surface.window.rect)) throw Error("stale_window", "Capture window moved or disappeared. Observe again.");
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
    const auto size = capture_size(screen_rect(surface.window.rect), filter.pointPixelScale);
    if (!size.first || !size.second) throw Error("invalid_capture_geometry", "Window has invalid capture dimensions.");
    SCStreamConfiguration* config = [SCStreamConfiguration new];
    config.width = size.first; config.height = size.second;
    config.showsCursor = NO;
    config.ignoreShadowsSingleWindow = YES;
    config.scalesToFit = YES;
    if (@available(macOS 14.2, *)) config.includeChildWindows = NO;
    struct Frame {
        dispatch_semaphore_t ready = dispatch_semaphore_create(0);
        Ref<CGImageRef> image;
        __strong NSError* error = nil;
    };
    auto frame = std::make_shared<Frame>();
    [SCScreenshotManager captureImageWithFilter:filter configuration:config completionHandler:^(CGImageRef image, NSError* error) {
        frame->image = Ref<CGImageRef>::retained(image); frame->error = error; dispatch_semaphore_signal(frame->ready);
    }];
    if (dispatch_semaphore_wait(frame->ready, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) != 0)
        throw Error("capture_timeout", "ScreenCaptureKit did not return a frame in time.");
    if (!frame->image) throw Error("capture_unavailable", text(frame->error.localizedDescription));
    const auto width = CGImageGetWidth(frame->image.get()), height = CGImageGetHeight(frame->image.get());
    if (width == 0 || height == 0 || width > 2560 || height > 2560)
        throw Error("capture_dimensions", "ScreenCaptureKit returned unexpected frame dimensions.");
    surface.width = static_cast<int>(width); surface.height = static_cast<int>(height);
    Ref<CGColorSpaceRef> color(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    Ref<CGContextRef> context(CGBitmapContextCreate(nullptr, width, height, 8, width * 4, color.get(), kCGImageAlphaPremultipliedLast));
    if (!context) throw Error("capture_encoding", "Could not allocate screenshot buffer.");
    CGContextDrawImage(context.get(), CGRectMake(0, 0, width, height), frame->image.get());
    auto cursor = pointer_composite(context.get(), surface);
    Ref<CGImageRef> composed(CGBitmapContextCreateImage(context.get()));
    NSMutableData* data = [NSMutableData data];
    Ref<CGImageDestinationRef> destination(CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, CFSTR("public.png"), 1, nullptr));
    if (!destination) throw Error("capture_encoding", "Could not create PNG encoder.");
    CGImageDestinationAddImage(destination.get(), composed.get(), nullptr);
    if (!CGImageDestinationFinalize(destination.get()) || data.length > 12 * 1024 * 1024)
        throw Error("capture_encoding", "Could not encode screenshot within its byte limit.");
    surface.screenshot_id = observation_id + "-image-" + std::to_string(index);
    descriptor = geometry(surface);
    descriptor.update({{"id", surface.screenshot_id}, {"window", surface.window.id}, {"observation_id", observation_id},
        {"capture_method", "screencapturekit"}, {"relation", surface.relation}, {"cursor", cursor}, {"zIndex", index}});
    return {{"name", "computer-use-" + surface.screenshot_id + ".png"}, {"mime_type", "image/png"},
        {"data_url", "data:image/png;base64," + text([data base64EncodedStringWithOptions:0])},
        {"metadata", {{"computer_use", {{"observation_id", observation_id}, {"screenshot_id", surface.screenshot_id},
            {"window", surface.window.id}, {"geometry", geometry(surface)}, {"cursor", cursor}}}}}};
}
} // namespace acecode::computer_use::macos
