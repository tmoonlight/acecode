// Host-to-guest XPC services inside a booted simulator: connection setup and
// the CoreDevice display-info request used for interface rotation. Protocol
// shapes follow idb (FBSimulatorControl/CoreDevice), MIT License.

#import "sim_core.h"
#import "private_api.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>

namespace acecode::ios_simulator::helper {
namespace {

constexpr const char* kDisplayInfoService = "com.apple.coredevice.feature.getdisplayinfo";
constexpr const char* kDisplayInfoAction = "com.apple.coredevice.action.displayinfo";

// Interface rotation per device. The first lookup answers synchronously;
// later ones return the last answer at once and refresh in the background, so
// input and frame encoding never wait for the guest. The display-info
// connection is kept and reused (connecting is the slow part on a busy Mac).
struct RotationState {
    int rotation = 0;
    bool known = false;        // a lookup has completed
    bool answered = false;     // the provider gave a rotation at least once
    bool unsupported = false;  // runtime without the provider (before Xcode 27)
    bool failed = false;       // provider present, last lookup had no answer
    bool refreshing = false;
    std::chrono::steady_clock::time_point checked;
    xpc_connection_t connection = nullptr;
};

std::mutex g_rotation_mu;
std::map<std::string, RotationState> g_rotations;

xpc_object_t core_device_version() {
    static NSString* version = [NSBundle bundleWithPath:@"/Library/Developer/PrivateFrameworks/CoreDevice.framework"]
        .infoDictionary[@"CFBundleVersion"];
    NSString* text = version.length ? version : @"1.0";
    xpc_object_t components = xpc_array_create(nullptr, 0);
    NSArray<NSString*>* parts = [text componentsSeparatedByString:@"."];
    for (NSString* part in parts) xpc_array_append_value(components, xpc_uint64_create(part.longLongValue));
    xpc_object_t out = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_value(out, "components", components);
    xpc_dictionary_set_int64(out, "originalComponentsCount", static_cast<int64_t>(parts.count));
    xpc_dictionary_set_string(out, "stringValue", text.UTF8String);
    return out;
}

// Xcode 27 reports displays[].currentOrientation as rot0 / rot90 / rot180 /
// rot270 (verified on the xcode-27 runner); anything else is no answer.
int rotation_from(const char* value) {
    if (!value) return -1;
    const std::string text = value;
    if (text == "rot0") return 0;
    if (text == "rot90") return 90;
    if (text == "rot180") return 180;
    if (text == "rot270") return 270;
    return -1;
}

// Asks the display-info provider over an open connection. Returns -1 when it
// did not answer in time or the reply had no orientation; broken is set when
// the connection failed and must be replaced. raw receives a description of
// the reply for diagnostics.
int query_rotation(xpc_connection_t xpc, const std::string& udid, double timeout_seconds, bool& broken,
                   std::string* raw) {
    xpc_object_t request = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(request, "CoreDevice.actionIdentifier", kDisplayInfoAction);
    xpc_dictionary_set_string(request, "CoreDevice.deviceIdentifier", udid.c_str());
    xpc_dictionary_set_string(request, "CoreDevice.invocationIdentifier", [NSUUID UUID].UUIDString.UTF8String);
    xpc_dictionary_set_int64(request, "CoreDevice.CoreDeviceDDIProtocolVersion", 1);
    xpc_dictionary_set_value(request, "CoreDevice.coreDeviceVersion", core_device_version());
    xpc_dictionary_set_value(request, "CoreDevice.input", xpc_dictionary_create(nullptr, nullptr, 0));
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    __block int rotation = -1;
    __block bool failed_connection = false;
    __block std::string description;
    xpc_connection_send_message_with_reply(xpc, request, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
        ^(xpc_object_t reply) {
            if (char* text = xpc_copy_description(reply)) {
                description.assign(text, strnlen(text, 4000));
                free(text);
            }
            if (xpc_get_type(reply) == XPC_TYPE_ERROR) failed_connection = true;
            if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY) {
                xpc_object_t output = xpc_dictionary_get_value(reply, "CoreDevice.output");
                xpc_object_t displays = output ? xpc_dictionary_get_value(output, "displays") : nullptr;
                if (displays && xpc_get_type(displays) == XPC_TYPE_ARRAY) {
                    const size_t count = xpc_array_get_count(displays);
                    for (size_t index = 0; index < count; ++index) {
                        xpc_object_t display = xpc_array_get_value(displays, index);
                        if (xpc_get_type(display) != XPC_TYPE_DICTIONARY) continue;
                        const int value = rotation_from(xpc_dictionary_get_string(display, "currentOrientation"));
                        if (value < 0) continue;
                        if (xpc_dictionary_get_bool(display, "primary") || rotation < 0) rotation = value;
                        if (xpc_dictionary_get_bool(display, "primary")) break;
                    }
                }
            }
            dispatch_semaphore_signal(answered);
        });
    const bool timed_out = dispatch_semaphore_wait(
        answered, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(timeout_seconds * NSEC_PER_SEC))) != 0;
    broken = !timed_out && failed_connection;
    if (raw) *raw = timed_out ? std::string("timed out") : description;
    return timed_out ? -1 : rotation;
}

// Pre-Xcode-27 runtimes have no display-info provider. There the device
// orientation is whatever Simulator.app last applied to the device window,
// which it records per UDID in its preferences.
int legacy_window_rotation(const std::string& udid) {
    CFPreferencesAppSynchronize(CFSTR("com.apple.iphonesimulator"));
    CFPropertyListRef value = CFPreferencesCopyAppValue(CFSTR("DevicePreferences"), CFSTR("com.apple.iphonesimulator"));
    NSDictionary* devices = CFBridgingRelease(value);
    if (![devices isKindOfClass:[NSDictionary class]]) return 0;
    NSDictionary* device = devices[ns(udid)];
    if (![device isKindOfClass:[NSDictionary class]]) return 0;
    NSString* orientation = device[@"SimulatorWindowOrientation"];
    if ([orientation isKindOfClass:[NSString class]]) {
        if ([orientation isEqualToString:@"LandscapeRight"]) return 270;
        if ([orientation isEqualToString:@"LandscapeLeft"]) return 90;
        if ([orientation isEqualToString:@"PortraitUpsideDown"]) return 180;
        if ([orientation isEqualToString:@"Portrait"]) return 0;
    }
    id angle = device[@"SimulatorWindowRotationAngle"];
    if ([angle respondsToSelector:@selector(doubleValue)]) {
        const long degrees = std::lround([angle doubleValue]);
        return static_cast<int>(((degrees % 360) + 360) % 360) / 90 * 90;
    }
    return 0;
}

} // namespace

id connect_guest_service(id device, const char* service, std::string& error, bool* unsupported) {
    auto endpoint_fn = reinterpret_cast<ACEXPCEndpointFromPortFn>(dlsym(RTLD_DEFAULT, "xpc_endpoint_create_mach_port_4sim"));
    auto connect_fn = reinterpret_cast<ACEXPCConnectionFromEndpointFn>(
        dlsym(RTLD_DEFAULT, "xpc_connection_create_from_endpoint"));
    auto sim2host_fn = reinterpret_cast<ACEXPCEnableSim2HostFn>(dlsym(RTLD_DEFAULT, "xpc_connection_enable_sim2host_4sim"));
    if (!endpoint_fn || !connect_fn || !sim2host_fn) {
        error = "simulator XPC bridging symbols are unavailable";
        return nil;
    }
    __block unsigned int port = 0;
    __block NSError* failure = nil;
    @try {
        port = [(id<ACESimDevice>)device lookup:@(service) error:&failure];
    } @catch (NSException* exception) {
        error = utf8(exception.reason);
        return nil;
    }
    if (port == 0) {
        if (unsupported)
            *unsupported = [failure.domain isEqualToString:@"com.apple.CoreSimulator.SimError"] && failure.code == 405;
        error = describe_error(failure);
        return nil;
    }
    xpc_object_t endpoint = endpoint_fn(port, 0, 0);
    xpc_connection_t connection = endpoint ? connect_fn(endpoint) : nullptr;
    if (!connection) {
        error = "could not create the guest XPC connection";
        return nil;
    }
    sim2host_fn(connection);
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) { (void)event; });
    xpc_connection_resume(connection);
    return connection;
}

namespace {

// Runs one provider lookup for udid and stores the outcome.
void refresh_rotation(id device, const std::string& udid, double timeout_seconds) {
    xpc_connection_t connection = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_rotation_mu);
        connection = g_rotations[udid].connection;
    }
    bool unsupported = false;
    if (!connection) {
        std::string error;
        connection = (xpc_connection_t)connect_guest_service(device, kDisplayInfoService, error, &unsupported);
    }
    bool broken = false;
    const int rotation = connection ? query_rotation(connection, udid, timeout_seconds, broken, nullptr) : -1;
    if (broken) xpc_connection_cancel(connection);
    const int window = rotation < 0 ? legacy_window_rotation(udid) : 0;
    std::lock_guard<std::mutex> lock(g_rotation_mu);
    auto& entry = g_rotations[udid];
    entry.connection = broken ? nullptr : connection;
    entry.known = true;
    entry.refreshing = false;
    entry.checked = std::chrono::steady_clock::now();
    entry.unsupported = unsupported;
    entry.failed = !unsupported && rotation < 0;
    if (rotation >= 0) {
        entry.rotation = rotation;
        entry.answered = true;
    } else if (!entry.answered) {
        // No provider, or it never answered: follow Simulator.app's window.
        entry.rotation = window;
    }
}

} // namespace

int display_rotation(id device, const std::string& udid) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_rotation_mu);
        auto& entry = g_rotations[udid];
        const auto age = now - entry.checked;
        if (entry.known) {
            if (age < std::chrono::milliseconds(600) || entry.refreshing) return entry.rotation;
            if (entry.unsupported && age < std::chrono::seconds(30)) {
                // Before Xcode 27: Simulator.app's window orientation (a cheap
                // preferences read) is the only source.
                entry.rotation = legacy_window_rotation(udid);
                entry.checked = now;
                return entry.rotation;
            }
            if (entry.failed && age < std::chrono::seconds(5)) return entry.rotation;
            entry.refreshing = true;
            const std::string key = udid;
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ refresh_rotation(device, key, 2.0); });
            return entry.rotation;
        }
    }
    // First lookup for this device: answer before the first frame or touch.
    refresh_rotation(device, udid, 5.0);
    std::lock_guard<std::mutex> lock(g_rotation_mu);
    return g_rotations[udid].rotation;
}

json display_info(id device, const std::string& udid) {
    bool unsupported = false;
    bool broken = false;
    std::string raw;
    std::string error;
    int rotation = -1;
    xpc_connection_t connection =
        (xpc_connection_t)connect_guest_service(device, kDisplayInfoService, error, &unsupported);
    if (connection) {
        rotation = query_rotation(connection, udid, 5.0, broken, &raw);
        xpc_connection_cancel(connection);
    } else {
        raw = "connect failed: " + error;
    }
    return {{"rotation", rotation}, {"unsupported", unsupported}, {"raw", raw},
            {"window_rotation", legacy_window_rotation(udid)}};
}

} // namespace acecode::ios_simulator::helper
