#import "sim_display.h"
#import "sim_core.h"
#import "sim_framebuffer.h"
#import "sim_output.h"

#import <Accelerate/Accelerate.h>
#import <ImageIO/ImageIO.h>
#import <IOSurface/IOSurface.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace acecode::ios_simulator::helper {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

CGColorSpaceRef srgb() {
    static CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    return space;
}

void release_bytes(void* info, const void* data, size_t size) {
    (void)data;
    (void)size;
    free(info);
}

// Copies the surface into an owned CGImage, scaled and turned clockwise by
// rotation degrees (the interface orientation of the display).
//
// The read must synchronize with the simulator's GPU writes: an unsynchronized
// read (kIOSurfaceLockAvoidSync fails with kIOReturnCannotLock) returns a stale
// CPU copy. On Intel Macs the synchronizing lock can wait for a whole
// animation (seconds); `simctl io screenshot` stalls the same way. Callers run
// this on their own queue so input is never blocked behind it. Scaling and
// rotation run on the private copy with vImage, off the GPU.
CGImageRef copy_surface_image(IOSurfaceRef surface, double scale, int rotation, int& out_width, int& out_height,
                              bool high_quality = false) {
    const size_t width = IOSurfaceGetWidth(surface);
    const size_t height = IOSurfaceGetHeight(surface);
    if (width == 0 || height == 0 || IOSurfaceGetPixelFormat(surface) != 'BGRA') return nullptr;
    const size_t source_row = IOSurfaceGetBytesPerRow(surface);
    void* source = malloc(source_row * height);
    if (!source) return nullptr;
    const IOSurfaceLockOptions lock = kIOSurfaceLockReadOnly;
    if (IOSurfaceLock(surface, lock, nullptr) != kIOReturnSuccess) { free(source); return nullptr; }
    memcpy(source, IOSurfaceGetBaseAddress(surface), source_row * height);
    IOSurfaceUnlock(surface, lock, nullptr);

    const double clamped = std::clamp(scale, 0.05, 1.0);
    const size_t scaled_width = std::max<size_t>(1, static_cast<size_t>(std::lround(width * clamped)));
    const size_t scaled_height = std::max<size_t>(1, static_cast<size_t>(std::lround(height * clamped)));
    vImage_Buffer src{source, height, width, source_row};
    vImage_Buffer scaled = src;
    void* scaled_bytes = nullptr;
    if (scaled_width != width || scaled_height != height) {
        scaled_bytes = malloc(scaled_width * scaled_height * 4);
        if (!scaled_bytes) { free(source); return nullptr; }
        scaled = {scaled_bytes, scaled_height, scaled_width, scaled_width * 4};
        if (vImageScale_ARGB8888(&src, &scaled, nullptr,
                                 high_quality ? kvImageHighQualityResampling : kvImageNoFlags) != kvImageNoError) {
            free(source);
            free(scaled_bytes);
            return nullptr;
        }
        free(source);
        source = nullptr;
    }
    vImage_Buffer result = scaled;
    void* owned = scaled_bytes ? scaled_bytes : source;
    if (rotation == 90 || rotation == 180 || rotation == 270) {
        const bool quarter = rotation != 180;
        const size_t rotated_width = quarter ? scaled.height : scaled.width;
        const size_t rotated_height = quarter ? scaled.width : scaled.height;
        void* rotated_bytes = malloc(rotated_width * rotated_height * 4);
        if (!rotated_bytes) { free(owned); return nullptr; }
        vImage_Buffer rotated{rotated_bytes, rotated_height, rotated_width, rotated_width * 4};
        const uint8_t rotate = rotation == 90 ? kRotate90DegreesClockwise
            : rotation == 180 ? kRotate180DegreesClockwise : kRotate270DegreesClockwise;
        const Pixel_8888 black = {0, 0, 0, 255};
        if (vImageRotate90_ARGB8888(&scaled, &rotated, rotate, black, kvImageNoFlags) != kvImageNoError) {
            free(owned);
            free(rotated_bytes);
            return nullptr;
        }
        free(owned);
        owned = rotated_bytes;
        result = rotated;
    }
    CGDataProviderRef provider = CGDataProviderCreateWithData(owned, result.data, result.rowBytes * result.height,
                                                              release_bytes);
    if (!provider) { free(owned); return nullptr; }
    CGImageRef image = CGImageCreate(result.width, result.height, 8, 32, result.rowBytes, srgb(),
        kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, provider, nullptr, false,
        kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    out_width = static_cast<int>(result.width);
    out_height = static_cast<int>(result.height);
    return image;
}

std::string encode_image(CGImageRef image, const std::string& format, double quality) {
    NSMutableData* data = [NSMutableData data];
    CFStringRef type = format == "png" ? CFSTR("public.png") : CFSTR("public.jpeg");
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, type, 1, nullptr);
    if (!destination) return {};
    NSDictionary* options = format == "png" ? @{} :
        @{(__bridge NSString*)kCGImageDestinationLossyCompressionQuality: @(std::clamp(quality, 0.1, 1.0))};
    CGImageDestinationAddImage(destination, image, (__bridge CFDictionaryRef)options);
    const bool ok = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    if (!ok) return {};
    return std::string(static_cast<const char*>(data.bytes), data.length);
}

struct StreamState {
    double interval_ms = 33;
    double scale = 0.5;
    double quality = 0.7;
    std::int64_t last_sent_ms = 0;
    std::uint64_t last_seed = 0;
    bool have_seed = false;
    std::uint64_t seq = 0;
};

class FramebufferDisplay final : public Display, public std::enable_shared_from_this<FramebufferDisplay> {
public:
    FramebufferDisplay(id device, std::string udid) : device_(device), udid_(std::move(udid)) {
        encode_queue_ = dispatch_queue_create("dev.acecode.ios-sim.display.encode", DISPATCH_QUEUE_SERIAL);
    }

    void start() {
        std::weak_ptr<FramebufferDisplay> surface_owner = weak_from_this();
        std::weak_ptr<FramebufferDisplay> frame_owner = surface_owner;
        // Callbacks can fire before the constructor returns; they reach the
        // source only through current_surface(), which waits for this store.
        auto source = std::make_unique<FramebufferSource>(
            device_,
            [surface_owner](id surface) {
                if (auto self = surface_owner.lock()) self->surface_changed(surface);
            },
            [frame_owner]() {
                if (auto self = frame_owner.lock()) self->schedule(0);
            });
        std::lock_guard<std::mutex> lock(mu_);
        source_ = std::move(source);
    }

    EncodedImage capture(const std::string& format, double scale, double quality) override {
        id surface = current_surface();
        if (!surface) throw SimError("DISPLAY_UNAVAILABLE", "The simulator display has no picture yet.");
        EncodedImage out;
        out.rotation = display_rotation(device_, udid_);
        out.format = format == "png" ? "png" : "jpeg";
        CGImageRef image = copy_surface_image((__bridge IOSurfaceRef)surface, scale, out.rotation, out.width,
                                              out.height, true);
        if (!image) throw SimError("CAPTURE_FAILED", "Could not read the simulator framebuffer.");
        out.bytes = encode_image(image, out.format, quality);
        CGImageRelease(image);
        if (out.bytes.empty()) throw SimError("CAPTURE_FAILED", "Could not encode the simulator screenshot.");
        return out;
    }

    void start_stream(std::uint64_t stream, double fps, double scale, double quality) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            StreamState state;
            state.interval_ms = 1000.0 / std::clamp(fps, 1.0, 60.0);
            state.scale = std::clamp(scale, 0.1, 1.0);
            state.quality = std::clamp(quality, 0.2, 0.95);
            streams_[stream] = state;
        }
        schedule(0);
    }

    void stop_stream(std::uint64_t stream) override {
        std::lock_guard<std::mutex> lock(mu_);
        streams_.erase(stream);
    }

    void stop_all() override {
        std::map<std::uint64_t, StreamState> ended;
        {
            std::lock_guard<std::mutex> lock(mu_);
            ended.swap(streams_);
        }
        for (const auto& [stream, state] : ended)
            emit({{"event", "stream_ended"}, {"stream", stream}, {"reason", "stopped"}});
    }

    int pixel_width() override {
        id surface = current_surface();
        return surface ? static_cast<int>(IOSurfaceGetWidth((__bridge IOSurfaceRef)surface)) : 0;
    }

    int pixel_height() override {
        id surface = current_surface();
        return surface ? static_cast<int>(IOSurfaceGetHeight((__bridge IOSurfaceRef)surface)) : 0;
    }

private:
    id current_surface() {
        std::lock_guard<std::mutex> lock(mu_);
        return source_ ? source_->surface() : nil;
    }

    void surface_changed(id surface) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [stream, state] : streams_) state.have_seed = false;
        }
        if (!surface) {
            std::map<std::uint64_t, StreamState> ended;
            {
                std::lock_guard<std::mutex> lock(mu_);
                ended.swap(streams_);
            }
            for (const auto& [stream, state] : ended)
                emit({{"event", "stream_ended"}, {"stream", stream}, {"reason", "display_gone"}});
            return;
        }
        schedule(0);
    }

    void schedule(double delay_ms) {
        bool expected = false;
        if (!scheduled_.compare_exchange_strong(expected, true)) return;
        std::weak_ptr<FramebufferDisplay> weak = weak_from_this();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(delay_ms * NSEC_PER_MSEC)),
            encode_queue_, ^{
                if (auto self = weak.lock()) self->pump();
            });
    }

    // Runs on encode_queue_. Sends a frame to every stream whose interval has
    // elapsed and whose surface content changed; reschedules the rest.
    void pump() {
        scheduled_.store(false);
        id surface = current_surface();
        std::map<std::uint64_t, StreamState> snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            snapshot = streams_;
        }
        if (!surface || snapshot.empty()) return;
        IOSurfaceRef ref = (__bridge IOSurfaceRef)surface;
        const int rotation = display_rotation(device_, udid_);
        if (rotation != last_rotation_) {
            last_rotation_ = rotation;
            for (auto& [stream, state] : snapshot) state.have_seed = false;
        }
        const std::uint64_t seed = IOSurfaceGetSeed(ref);
        const auto now = now_ms();
        double next_delay = -1;
        for (auto& [stream, state] : snapshot) {
            if (state.have_seed && state.last_seed == seed) continue;
            const double elapsed = static_cast<double>(now - state.last_sent_ms);
            if (state.last_sent_ms && elapsed < state.interval_ms) {
                const double wait = state.interval_ms - elapsed;
                next_delay = next_delay < 0 ? wait : std::min(next_delay, wait);
                continue;
            }
            int width = 0;
            int height = 0;
            const auto started = now_ms();
            CGImageRef image = copy_surface_image(ref, state.scale, rotation, width, height);
            if (!image) continue;
            const auto bytes = encode_image(image, "jpeg", state.quality);
            CGImageRelease(image);
            const auto encode_ms = now_ms() - started;
            if (bytes.empty()) continue;
            state.last_sent_ms = now;
            state.last_seed = seed;
            state.have_seed = true;
            ++state.seq;
            emit({{"event", "frame"}, {"stream", stream}, {"seq", state.seq}, {"format", "jpeg"},
                  {"width", width}, {"height", height}, {"rotation", rotation}, {"udid", udid_},
                  {"encode_ms", encode_ms}}, bytes, true);
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& [stream, state] : snapshot) {
                auto found = streams_.find(stream);
                if (found == streams_.end()) continue;
                found->second.last_sent_ms = state.last_sent_ms;
                found->second.last_seed = state.last_seed;
                found->second.have_seed = state.have_seed;
                found->second.seq = state.seq;
            }
        }
        if (next_delay >= 0) schedule(next_delay);
    }

    id device_;
    std::string udid_;
    int last_rotation_ = 0;  // encode_queue_ only
    dispatch_queue_t encode_queue_ = nil;
    std::atomic<bool> scheduled_{false};
    std::mutex mu_;
    std::map<std::uint64_t, StreamState> streams_;
    // Last: destroyed (callbacks unregistered and drained) before the rest.
    std::unique_ptr<FramebufferSource> source_;
};

} // namespace

std::shared_ptr<Display> open_display(id device, const std::string& udid) {
    auto display = std::make_shared<FramebufferDisplay>(device, udid);
    display->start();
    return display;
}

} // namespace acecode::ios_simulator::helper
