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

struct RotationCache {
    int rotation = 0;
    bool unsupported = false;
    bool failed = false;  // the provider exists but gave no usable answer
    std::chrono::steady_clock::time_point checked;
};

std::mutex g_rotation_mu;
std::map<std::string, RotationCache> g_rotations;

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

// Returns -1 when the provider did not answer. raw receives a description of
// the reply (or the failure) for diagnostics.
int query_rotation(id device, const std::string& udid, bool& unsupported, std::string* raw) {
    std::string error;
    id connection = connect_guest_service(device, kDisplayInfoService, error, &unsupported);
    if (!connection) {
        if (raw) *raw = "connect failed: " + error;
        return -1;
    }
    xpc_connection_t xpc = (xpc_connection_t)connection;
    xpc_object_t request = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(request, "CoreDevice.actionIdentifier", kDisplayInfoAction);
    xpc_dictionary_set_string(request, "CoreDevice.deviceIdentifier", udid.c_str());
    xpc_dictionary_set_string(request, "CoreDevice.invocationIdentifier", [NSUUID UUID].UUIDString.UTF8String);
    xpc_dictionary_set_int64(request, "CoreDevice.CoreDeviceDDIProtocolVersion", 1);
    xpc_dictionary_set_value(request, "CoreDevice.coreDeviceVersion", core_device_version());
    xpc_dictionary_set_value(request, "CoreDevice.input", xpc_dictionary_create(nullptr, nullptr, 0));
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    __block int rotation = -1;
    __block std::string description;
    xpc_connection_send_message_with_reply(xpc, request, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
        ^(xpc_object_t reply) {
            if (char* text = xpc_copy_description(reply)) {
                description.assign(text, strnlen(text, 4000));
                free(text);
            }
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
    const bool timed_out =
        dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0;
    xpc_connection_cancel(xpc);
    if (raw) *raw = timed_out ? std::string("timed out") : description;
    return rotation;
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

int display_rotation(id device, const std::string& udid) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_rotation_mu);
        auto found = g_rotations.find(udid);
        if (found != g_rotations.end()) {
            const auto age = now - found->second.checked;
            if (age < std::chrono::milliseconds(600)) return found->second.rotation;
            // A provider that timed out is retried every few seconds, not on
            // every touch: each attempt can block the caller for 2 s.
            if (found->second.failed && age < std::chrono::seconds(5)) return found->second.rotation;
            if (found->second.unsupported && age < std::chrono::seconds(30)) {
                const int rotation = legacy_window_rotation(udid);
                found->second.rotation = rotation;
                return rotation;
            }
        }
    }
    bool unsupported = false;
    const int rotation = query_rotation(device, udid, unsupported, nullptr);
    std::lock_guard<std::mutex> lock(g_rotation_mu);
    auto& entry = g_rotations[udid];
    entry.checked = now;
    // Only "service unknown" is sticky; a slow answer keeps the last value.
    entry.unsupported = unsupported;
    entry.failed = !unsupported && rotation < 0;
    if (rotation >= 0) entry.rotation = rotation;
    // No provider, or a reply without a recognizable orientation: use the
    // orientation Simulator.app applied to the device window.
    else entry.rotation = legacy_window_rotation(udid);
    return entry.rotation;
}

json display_info(id device, const std::string& udid) {
    bool unsupported = false;
    std::string raw;
    const int rotation = query_rotation(device, udid, unsupported, &raw);
    return {{"rotation", rotation}, {"unsupported", unsupported}, {"raw", raw},
            {"window_rotation", legacy_window_rotation(udid)}};
}

} // namespace acecode::ios_simulator::helper
