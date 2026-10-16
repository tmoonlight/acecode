// HID injection for one simulator: the legacy Indigo transport (Xcode 26 and
// earlier) and the dtuhidd digitizer transport (CoreSimulator 1155.4+,
// Xcode 27). Coordinates are device points with a top-left origin.
#pragma once

#import <Foundation/Foundation.h>

#include <cstdint>
#include <memory>
#include <string>

namespace acecode::ios_simulator::helper {

enum class Direction { Down, Up };

struct HidButton {
    int indigo_source = -1;        // -1 when only a consumer usage exists.
    std::uint16_t usage_page = 0;  // 0 when only a legacy source exists.
    std::uint16_t usage = 0;
};

// Returns false for an unknown button name.
bool lookup_button(const std::string& name, HidButton& out);

// "indigo" or "dtuhid" for the loaded CoreSimulator version.
std::string default_transport();

class HidSession {
public:
    virtual ~HidSession() = default;
    virtual std::string transport() const = 0;
    virtual void touch(Direction direction, double x, double y, unsigned edge) = 0;
    virtual void touch2(Direction direction, double x1, double y1, double x2, double y2) = 0;
    virtual void button(const HidButton& button, Direction direction) = 0;
    virtual void key(std::uint32_t usage, Direction direction) = 0;
    // Waits until injected events have been accepted by the guest path.
    virtual void flush() = 0;
};

// requested: "", "indigo" or "dtuhid". Throws SimError on failure.
std::unique_ptr<HidSession> open_hid_session(id device, double point_width, double point_height,
                                             const std::string& requested);

} // namespace acecode::ios_simulator::helper
