#import "sim_framebuffer.h"
#import "sim_core.h"
#import "private_api.h"

#include <mutex>

namespace acecode::ios_simulator::helper {

struct FramebufferSource::State {
    std::mutex mu;
    bool closed = false;
    SurfaceHandler on_surface;
    FrameHandler on_frame;
    id surface = nil;
    id renderable = nil;
    NSUUID* token = nil;
    dispatch_queue_t queue = nil;
    bool screen_callbacks = false;
};

namespace {

const void* const kQueueKey = &kQueueKey;

id initial_surface(id renderable) {
    id surface = nil;
    @try { surface = [(id<ACESimDisplayRenderable>)renderable framebufferSurface]; } @catch (NSException*) {}
    if (!surface) {
        @try { surface = [(id<ACESimDisplayRenderable>)renderable ioSurface]; } @catch (NSException*) {}
    }
    return surface;
}

void unregister_callbacks(id renderable, NSUUID* token, bool screen_callbacks) {
    if (screen_callbacks) {
        @try { [(id<ACESimScreen>)renderable unregisterScreenCallbacksWithUUID:token]; } @catch (NSException*) {}
    }
    @try { [(id<ACESimDisplayRenderable>)renderable unregisterIOSurfacesChangeCallbackWithUUID:token]; } @catch (NSException*) {}
    @try { [(id<ACESimDisplayRenderable>)renderable unregisterIOSurfaceChangeCallbackWithUUID:token]; } @catch (NSException*) {}
    @try { [(id<ACESimDisplayRenderable>)renderable unregisterDamageRectanglesCallbackWithUUID:token]; } @catch (NSException*) {}
}

} // namespace

id find_main_renderable(id device) {
    __block id found = nil;
    __block id fallback = nil;
    guarded("DISPLAY_UNAVAILABLE", ^{
        id io = [(id<ACESimDevice>)device io];
        NSArray* ports = io ? [(id<ACESimDeviceIOClient>)io ioPorts] : nil;
        for (id port in ports) {
            id descriptor = [(id<ACESimDeviceIOPort>)port descriptor];
            if (![descriptor respondsToSelector:@selector(framebufferSurface)] &&
                ![descriptor respondsToSelector:@selector(ioSurface)]) continue;
            if (![descriptor respondsToSelector:@selector(state)]) continue;
            id state = [(id<ACESimDisplayRenderable>)descriptor state];
            if (![state respondsToSelector:@selector(displayClass)]) continue;
            if ([(id<ACESimDisplayDescriptorState>)state displayClass] == 0) { found = descriptor; break; }
            if (!fallback) fallback = descriptor;
        }
    });
    return found ?: fallback;
}

FramebufferSource::FramebufferSource(id device, SurfaceHandler on_surface, FrameHandler on_frame)
    : state_(std::make_shared<State>()) {
    if (!device_booted(device)) throw SimError("DEVICE_NOT_BOOTED", "The simulator is not booted.");
    id renderable = find_main_renderable(device);
    if (!renderable) throw SimError("DISPLAY_UNAVAILABLE", "Could not find the simulator's main display.");
    state_->on_surface = std::move(on_surface);
    state_->on_frame = std::move(on_frame);
    state_->renderable = renderable;
    state_->token = [NSUUID UUID];
    state_->queue = dispatch_queue_create("dev.acecode.ios-sim.framebuffer", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(state_->queue, kQueueKey, const_cast<void*>(kQueueKey), nullptr);

    // The blocks hold the state weakly: CoreSimulator may keep them after
    // unregistering, and they must never keep a closed source alive.
    std::weak_ptr<State> weak = state_;
    void (^changed)(id) = ^(id surface) {
        auto state = weak.lock();
        if (!state) return;
        SurfaceHandler handler;
        {
            std::lock_guard<std::mutex> lock(state->mu);
            if (state->closed) return;
            state->surface = surface;
            handler = state->on_surface;
        }
        if (handler) handler(surface);
    };
    void (^frame)(void) = ^{
        auto state = weak.lock();
        if (!state) return;
        FrameHandler handler;
        {
            std::lock_guard<std::mutex> lock(state->mu);
            if (state->closed) return;
            handler = state->on_frame;
        }
        if (handler) handler();
    };

    NSUUID* token = state_->token;
    bool registered = false;
    if ([renderable respondsToSelector:@selector(registerScreenCallbacksWithUUID:callbackQueue:frameCallback:
                                                  surfacesChangedCallback:propertiesChangedCallback:)]) {
        @try {
            [(id<ACESimScreen>)renderable registerScreenCallbacksWithUUID:token callbackQueue:state_->queue
                frameCallback:frame
                surfacesChangedCallback:^(id primary, id masked) { (void)masked; changed(primary); }
                propertiesChangedCallback:^(id properties) { (void)properties; }];
            registered = true;
            state_->screen_callbacks = true;
        } @catch (NSException*) {
            @try { [(id<ACESimScreen>)renderable unregisterScreenCallbacksWithUUID:token]; } @catch (NSException*) {}
        }
    }
    if (!registered) {
        // Legacy callbacks arrive on CoreSimulator's queue; hop to ours so the
        // handlers stay serialized and drainable.
        dispatch_queue_t queue = state_->queue;
        void (^changed_on_queue)(id) = ^(id surface) { dispatch_async(queue, ^{ changed(surface); }); };
        bool surface_ok = false;
        @try {
            [(id<ACESimDisplayRenderable>)renderable registerCallbackWithUUID:token
                                                     ioSurfacesChangeCallback:changed_on_queue];
            surface_ok = true;
        } @catch (NSException*) {}
        @try {
            [(id<ACESimDisplayRenderable>)renderable registerCallbackWithUUID:token
                                                      ioSurfaceChangeCallback:changed_on_queue];
            surface_ok = true;
        } @catch (NSException*) {}
        @try {
            [(id<ACESimDisplayRenderable>)renderable registerCallbackWithUUID:token
                damageRectanglesCallback:^(NSArray* rects) { (void)rects; dispatch_async(queue, frame); }];
        } @catch (NSException*) {
            if (!surface_ok) {
                unregister_callbacks(renderable, token, false);
                throw SimError("DISPLAY_UNAVAILABLE", "Could not subscribe to framebuffer updates.");
            }
        }
    }
    id surface = initial_surface(renderable);
    std::lock_guard<std::mutex> lock(state_->mu);
    if (!state_->surface) state_->surface = surface;
}

FramebufferSource::~FramebufferSource() {
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->closed = true;
        state_->on_surface = nullptr;
        state_->on_frame = nullptr;
    }
    unregister_callbacks(state_->renderable, state_->token, state_->screen_callbacks);
    // Wait for a handler that is already running (unless it is this call).
    if (dispatch_get_specific(kQueueKey) != kQueueKey) dispatch_sync(state_->queue, ^{});
}

id FramebufferSource::surface() {
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (state_->surface) return state_->surface;
    }
    id surface = initial_surface(state_->renderable);
    std::lock_guard<std::mutex> lock(state_->mu);
    if (!state_->surface) state_->surface = surface;
    return state_->surface;
}

} // namespace acecode::ios_simulator::helper
