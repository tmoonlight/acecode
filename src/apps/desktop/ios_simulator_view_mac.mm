#include "ios_simulator_view.hpp"

#import "tool/ios_simulator/helper/sim_core.h"
#import "tool/ios_simulator/helper/sim_framebuffer.h"

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sim = acecode::ios_simulator::helper;
using acecode::desktop::IosSimulatorViewRect;

namespace {

constexpr const char* kCoreSimulatorFramework = "/Library/Developer/PrivateFrameworks/CoreSimulator.framework";

} // namespace

// The screen surface. It never takes input: the web panel below keeps
// receiving pointer and keyboard events and forwards them to the daemon.
@interface ACECodeIosSimulatorScreenView : NSView
@property(nonatomic, readonly) CALayer* screenLayer;
- (void)applyOcclusionRects:(const std::vector<IosSimulatorViewRect>&)rects;
@end

@implementation ACECodeIosSimulatorScreenView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        self.layer.masksToBounds = YES;
        _screenLayer = [CALayer layer];
        _screenLayer.magnificationFilter = kCAFilterLinear;
        _screenLayer.minificationFilter = kCAFilterTrilinear;
        _screenLayer.actions = @{@"contents": [NSNull null], @"bounds": [NSNull null],
                                 @"position": [NSNull null], @"transform": [NSNull null]};
        [self.layer addSublayer:_screenLayer];
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (NSView*)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}

- (BOOL)acceptsFirstResponder {
    return NO;
}

// Web content drawn over the screen (menus, dialogs) stays visible: those
// rectangles are cut out of the layer. Rectangles use the DOM's top-left
// coordinates, which a flipped view's backing layer already shares.
- (void)applyOcclusionRects:(const std::vector<IosSimulatorViewRect>&)rects {
    CALayer* layer = self.layer;
    if (!layer) return;
    if (rects.empty()) {
        layer.mask = nil;
        return;
    }
    const NSRect bounds = self.bounds;
    CGMutablePathRef path = CGPathCreateMutable();
    CGPathAddRect(path, nullptr, CGRectMake(0, 0, NSWidth(bounds), NSHeight(bounds)));
    for (const auto& rect : rects) {
        CGPathAddRect(path, nullptr, CGRectMake(rect.x, rect.y, rect.width, rect.height));
    }
    CAShapeLayer* mask = [CAShapeLayer layer];
    mask.frame = CGRectMake(0, 0, NSWidth(bounds), NSHeight(bounds));
    mask.path = path;
    mask.fillRule = kCAFillRuleEvenOdd;
    mask.fillColor = CGColorGetConstantColor(kCGColorBlack);
    CGPathRelease(path);
    layer.mask = mask;
}

@end

namespace acecode::desktop {

struct IosSimulatorView::Impl : std::enable_shared_from_this<Impl> {
    __weak NSWindow* window = nil;
    ACECodeIosSimulatorScreenView* view = nil;
    StateHandler on_state;
    dispatch_queue_t attach_queue = nil;

    // Main thread only.
    IosSimulatorViewLayout layout;
    bool has_layout = false;
    std::string attached_udid;
    std::string attaching_udid;
    std::uint64_t generation = 0;
    std::shared_ptr<sim::FramebufferSource> source;
    id surface = nil;
    int rotation = 0;
    std::string error;
    nlohmann::json last_published;

    // Frame coalescing between the framebuffer queue and the main thread.
    std::atomic<bool> frame_pending{false};
    std::atomic<int> pending_rotation{0};

    bool rendering() const { return view && !view.hidden; }

    nlohmann::json snapshot() const {
        IOSurfaceRef ref = (__bridge IOSurfaceRef)surface;
        return {{"supported", true},
                {"udid", has_layout ? layout.udid : std::string()},
                {"attached", source != nullptr && has_layout && attached_udid == layout.udid},
                {"attaching", !attaching_udid.empty()},
                {"rendering", rendering()},
                {"rotation", rotation},
                {"pixel_width", ref ? IOSurfaceGetWidth(ref) : 0},
                {"pixel_height", ref ? IOSurfaceGetHeight(ref) : 0},
                {"layout_revision", layout.layout_revision},
                {"error", error}};
    }

    void publish() {
        auto state = snapshot();
        if (state == last_published) return;
        last_published = state;
        if (on_state) on_state(state);
    }

    void place() {
        NSWindow* parent = window;
        NSView* content = parent.contentView;
        if (!view || !content) return;
        const auto& frame = layout.frame;
        const bool show = has_layout && layout.visible && surface != nil && frame.width > 0 &&
                          frame.height > 0 && attached_udid == layout.udid;
        if (!show) {
            view.hidden = YES;
            return;
        }
        const CGFloat y = content.isFlipped ? frame.y : NSHeight(content.bounds) - frame.y - frame.height;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        view.frame = NSMakeRect(frame.x, y, frame.width, frame.height);
        view.layer.cornerRadius = layout.corner_radius;
        IOSurfaceRef ref = (__bridge IOSurfaceRef)surface;
        const auto geometry = fit_simulator_layer(frame.width, frame.height, IOSurfaceGetWidth(ref),
                                                  IOSurfaceGetHeight(ref), rotation);
        CALayer* screen = view.screenLayer;
        screen.bounds = CGRectMake(0, 0, geometry.width, geometry.height);
        screen.position = CGPointMake(geometry.center_x, geometry.center_y);
        screen.transform = CATransform3DMakeRotation(
            simulator_layer_angle_radians(geometry.rotation, view.isFlipped), 0, 0, 1);
        [view applyOcclusionRects:clipped_occlusion_rects(layout)];
        [CATransaction commit];
        if (view.superview != content || content.subviews.lastObject != view)
            [content addSubview:view positioned:NSWindowAbove relativeTo:nil];
        view.hidden = NO;
    }

    void release_device() {
        ++generation;
        attaching_udid.clear();
        attached_udid.clear();
        surface = nil;
        view.screenLayer.contents = nil;
        if (source) {
            // Unregistering talks to CoreSimulator and waits for a running
            // callback; keep that off the main thread.
            __block std::shared_ptr<sim::FramebufferSource> retired = std::move(source);
            dispatch_async(attach_queue, ^{ retired.reset(); });
        }
    }

    void attach(const std::string& requested_udid, const std::string& requested_developer_dir) {
        release_device();
        error.clear();
        attaching_udid = requested_udid;
        const std::uint64_t attempt = generation;
        std::weak_ptr<Impl> weak = weak_from_this();
        // Blocks capture C++ reference parameters by reference; copy them.
        const std::string udid = requested_udid;
        const std::string developer_dir = requested_developer_dir;
        dispatch_async(attach_queue, ^{
            std::string failure;
            std::shared_ptr<sim::FramebufferSource> opened;
            int initial_rotation = 0;
            try {
                // CoreSimulator only: SimulatorKit (input) stays in the helper.
                sim::load_frameworks(developer_dir, false);
                id device = sim::device_for_udid(udid);
                std::weak_ptr<Impl> surface_owner = weak;
                std::weak_ptr<Impl> frame_owner = weak;
                opened = std::make_shared<sim::FramebufferSource>(
                    device,
                    [surface_owner, attempt](id changed) {
                        dispatch_async(dispatch_get_main_queue(), ^{
                            if (auto self = surface_owner.lock()) self->surface_changed(attempt, changed);
                        });
                    },
                    [frame_owner, attempt, device, udid]() {
                        // The rotation lookup is cached; keep it off the main thread.
                        const int turned = sim::display_rotation(device, udid);
                        auto self = frame_owner.lock();
                        if (!self) return;
                        self->pending_rotation.store(turned);
                        if (self->frame_pending.exchange(true)) return;
                        dispatch_async(dispatch_get_main_queue(), ^{
                            self->frame_pending.store(false);
                            self->frame_arrived(attempt, self->pending_rotation.load());
                        });
                    });
                initial_rotation = sim::display_rotation(device, udid);
            } catch (const sim::SimError& failed) {
                failure = failed.code + ": " + failed.what();
            } catch (const std::exception& failed) {
                failure = failed.what();
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                auto self = weak.lock();
                if (!self) return;
                self->attached(attempt, udid, opened, initial_rotation, failure);
            });
        });
    }

    void attached(std::uint64_t attempt, const std::string& udid,
                  const std::shared_ptr<sim::FramebufferSource>& opened, int initial_rotation,
                  const std::string& failure) {
        if (attempt != generation) {
            if (opened) {
                __block std::shared_ptr<sim::FramebufferSource> retired = opened;
                dispatch_async(attach_queue, ^{ retired.reset(); });
            }
            return;
        }
        attaching_udid.clear();
        if (!failure.empty()) {
            error = failure;
            place();
            publish();
            return;
        }
        source = opened;
        attached_udid = udid;
        rotation = initial_rotation;
        surface = source->surface();
        view.screenLayer.contents = surface;
        place();
        publish();
    }

    void surface_changed(std::uint64_t attempt, id changed) {
        if (attempt != generation) return;
        surface = changed;
        view.screenLayer.contents = changed;
        if (!changed) error = "DISPLAY_GONE: the simulator display went away";
        place();
        publish();
    }

    void frame_arrived(std::uint64_t attempt, int turned) {
        if (attempt != generation || !surface) return;
        CALayer* screen = view.screenLayer;
        if (turned != rotation) {
            rotation = turned;
            place();
            publish();
        }
        // The IOSurface is updated in place; tell Core Animation to re-read it.
        if ([screen respondsToSelector:@selector(setContentsChanged)]) {
            [screen performSelector:@selector(setContentsChanged)];
        } else {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            screen.contents = nil;
            screen.contents = surface;
            [CATransaction commit];
        }
    }
};

IosSimulatorView::IosSimulatorView(void* parent_window, StateHandler state_handler)
    : impl_(std::make_shared<Impl>()) {
    impl_->window = (__bridge NSWindow*)parent_window;
    impl_->on_state = std::move(state_handler);
    impl_->attach_queue = dispatch_queue_create("dev.acecode.desktop.ios-simulator-view", DISPATCH_QUEUE_SERIAL);
    impl_->view = [[ACECodeIosSimulatorScreenView alloc] initWithFrame:NSZeroRect];
    impl_->view.hidden = YES;
}

IosSimulatorView::~IosSimulatorView() {
    impl_->release_device();
    [impl_->view removeFromSuperview];
}

bool IosSimulatorView::supported() const {
    std::error_code ignored;
    return std::filesystem::exists(kCoreSimulatorFramework, ignored);
}

bool IosSimulatorView::set_layout(const IosSimulatorViewLayout& layout, std::string* error) {
    std::error_code ignored;
    if (!std::filesystem::exists(layout.developer_dir + "/usr/bin/simctl", ignored)) {
        if (error) *error = "developer_dir is not an Xcode developer directory";
        return false;
    }
    auto& impl = *impl_;
    // Attach once per device; after a failure the page remounts to retry.
    const bool device_changed = !impl.has_layout || layout.udid != impl.layout.udid;
    impl.layout = layout;
    impl.has_layout = true;
    if (device_changed) impl.attach(layout.udid, layout.developer_dir);
    impl.place();
    impl.publish();
    return true;
}

void IosSimulatorView::unmount() {
    impl_->has_layout = false;
    impl_->error.clear();
    impl_->release_device();
    impl_->view.hidden = YES;
    [impl_->view removeFromSuperview];
    impl_->publish();
}

nlohmann::json IosSimulatorView::state() const {
    return impl_->snapshot();
}

} // namespace acecode::desktop
