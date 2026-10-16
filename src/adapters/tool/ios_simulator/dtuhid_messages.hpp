#pragma once

// Messages for the dtuhidd digitizer service that CoreSimulator 1155.4+
// (Xcode 27) exposes inside a booted simulator. Header-only so the
// acecode-ios-sim helper (Objective-C++, ARC) and the unit tests (plain C++)
// build the same XPC dictionaries. Each builder returns a new dictionary: ARC
// releases it in the helper, plain C++ callers own it (xpc_release).
//
// Wire shape, all keys required:
//   envelope  {messageType: string, isBarrier: bool,
//              featureIdentifier: kDigitizerService, payload: dictionary}
//   IndigoDigitizerEvent       {pointOne: {x, y}, [pointTwo: {x, y}],
//                               eventType: 0 began | 1 moved | 2 ended,
//                               edge: 0 none | 1 top | 2 left | 3 bottom | 4 right,
//                               target: 0}
//   IndigoButtonEvent          {usagePage, usageCode, state: 1 down | 2 up}
//   IndigoKeyboardButtonEvent  {usageCode, state: 1 down | 2 up}
// Points are fractions of the device's portrait point size (0…1).

#include <cstdint>

#ifdef __APPLE__
#include <xpc/xpc.h>
#endif

namespace acecode::ios_simulator::dtuhid {

inline constexpr const char* kDigitizerService = "com.apple.coredevice.feature.remote.hid.digitizer";
inline constexpr const char* kFirstCoreSimulatorVersion = "1155.4";
inline constexpr const char* kDigitizerEvent = "IndigoDigitizerEvent";
inline constexpr const char* kButtonEvent = "IndigoButtonEvent";
inline constexpr const char* kKeyboardEvent = "IndigoKeyboardButtonEvent";

enum class Phase : std::uint64_t { Began = 0, Moved = 1, Ended = 2 };

// Down events start a contact and continue it; Up ends it. One tracker per
// contact kind (one finger, two fingers).
class ContactTracker {
public:
    Phase next(bool up) {
        if (up) {
            active_ = false;
            return Phase::Ended;
        }
        if (active_) return Phase::Moved;
        active_ = true;
        return Phase::Began;
    }
    bool active() const { return active_; }

private:
    bool active_ = false;
};

struct UnitPoint {
    double x = 0;
    double y = 0;
};

inline UnitPoint unit_point(double x, double y, double point_width, double point_height) {
    return {point_width > 0 ? x / point_width : 0, point_height > 0 ? y / point_height : 0};
}

inline std::uint64_t button_state(bool down) { return down ? 1 : 2; }

#ifdef __APPLE__

inline xpc_object_t point_dictionary(UnitPoint point) {
    xpc_object_t value = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_double(value, "x", point.x);
    xpc_dictionary_set_double(value, "y", point.y);
    return value;
}

// Takes the payload by reference: the envelope retains it.
inline xpc_object_t envelope(const char* message_type, xpc_object_t payload, bool barrier) {
    xpc_object_t message = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(message, "messageType", message_type);
    xpc_dictionary_set_bool(message, "isBarrier", barrier);
    xpc_dictionary_set_string(message, "featureIdentifier", kDigitizerService);
    xpc_dictionary_set_value(message, "payload", payload);
    return message;
}

inline xpc_object_t digitizer_payload(UnitPoint one, const UnitPoint* two, Phase phase, unsigned edge) {
    xpc_object_t payload = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_object_t first = point_dictionary(one);
    xpc_dictionary_set_value(payload, "pointOne", first);
#if !__has_feature(objc_arc)
    xpc_release(first);
#endif
    if (two) {
        xpc_object_t second = point_dictionary(*two);
        xpc_dictionary_set_value(payload, "pointTwo", second);
#if !__has_feature(objc_arc)
        xpc_release(second);
#endif
    }
    xpc_dictionary_set_uint64(payload, "eventType", static_cast<std::uint64_t>(phase));
    xpc_dictionary_set_uint64(payload, "edge", two ? 0 : edge);
    xpc_dictionary_set_uint64(payload, "target", 0);
    return payload;
}

inline xpc_object_t button_payload(std::uint16_t usage_page, std::uint16_t usage, bool down) {
    xpc_object_t payload = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_uint64(payload, "usagePage", usage_page);
    xpc_dictionary_set_uint64(payload, "usageCode", usage);
    xpc_dictionary_set_uint64(payload, "state", button_state(down));
    return payload;
}

inline xpc_object_t keyboard_payload(std::uint32_t usage, bool down) {
    xpc_object_t payload = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_uint64(payload, "usageCode", usage);
    xpc_dictionary_set_uint64(payload, "state", button_state(down));
    return payload;
}

// A key-up of usage 0 sent as a barrier: harmless, and dtuhidd replies to it,
// which proves the endpoint is live before real input is sent.
inline xpc_object_t liveness_probe() {
    xpc_object_t payload = keyboard_payload(0, false);
    xpc_object_t message = envelope(kKeyboardEvent, payload, true);
#if !__has_feature(objc_arc)
    xpc_release(payload);
#endif
    return message;
}

#endif // __APPLE__

} // namespace acecode::ios_simulator::dtuhid
