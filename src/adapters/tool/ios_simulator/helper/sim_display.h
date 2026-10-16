// Main-display framebuffer access for one simulator: still captures and
// rate-limited JPEG streams fed by CoreSimulator frame callbacks.
#pragma once

#import <Foundation/Foundation.h>

#include <cstdint>
#include <memory>
#include <string>

namespace acecode::ios_simulator::helper {

struct EncodedImage {
    std::string bytes;
    std::string format;  // "png" or "jpeg"
    int width = 0;
    int height = 0;
    int rotation = 0;    // interface rotation applied to the framebuffer
};

class Display {
public:
    virtual ~Display() = default;
    // scale in (0, 1] relative to the framebuffer; quality in (0, 1] for JPEG.
    // The image is turned to the current interface orientation.
    virtual EncodedImage capture(const std::string& format, double scale, double quality) = 0;
    // Emits {"event":"frame"} frames until stopped or the surface disappears.
    virtual void start_stream(std::uint64_t stream, double fps, double scale, double quality) = 0;
    virtual void stop_stream(std::uint64_t stream) = 0;
    virtual void stop_all() = 0;
    virtual int pixel_width() = 0;
    virtual int pixel_height() = 0;
};

// Throws SimError when the device has no renderable main display.
std::shared_ptr<Display> open_display(id device, const std::string& udid);

} // namespace acecode::ios_simulator::helper
