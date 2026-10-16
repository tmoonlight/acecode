// Subscription to a booted simulator's main-display IOSurface. Shared by the
// helper (JPEG captures and streams) and the Desktop shell (zero-copy layer):
// only CoreSimulator is needed, since the screen descriptors are remote
// proxies served by CoreSimulatorService.
#pragma once

#import <Foundation/Foundation.h>

#include <functional>
#include <memory>

namespace acecode::ios_simulator::helper {

// The main screen's renderable (display class 0), or the first renderable
// display when none reports class 0. nil when the device exposes none.
id find_main_renderable(id device);

class FramebufferSource {
public:
    // surface is nil when the display went away. Both handlers run on a
    // private serial queue and are never called after the destructor returns.
    using SurfaceHandler = std::function<void(id surface)>;
    using FrameHandler = std::function<void()>;

    // Throws SimError (DEVICE_NOT_BOOTED, DISPLAY_UNAVAILABLE).
    FramebufferSource(id device, SurfaceHandler on_surface, FrameHandler on_frame);
    ~FramebufferSource();
    FramebufferSource(const FramebufferSource&) = delete;
    FramebufferSource& operator=(const FramebufferSource&) = delete;

    // The latest surface (nil until the simulator rendered one).
    id surface();

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace acecode::ios_simulator::helper
