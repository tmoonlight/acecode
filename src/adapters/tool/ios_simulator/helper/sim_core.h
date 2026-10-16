// Shared state for the acecode-ios-sim helper: private framework loading,
// device lookup, ObjC exception containment and the framed stdout writer.
#pragma once

#import <Foundation/Foundation.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <stdexcept>
#include <string>

namespace acecode::ios_simulator::helper {

using json = nlohmann::json;

// Error carried back to the daemon as {"ok":false,"error":{code,message}}.
struct SimError : std::runtime_error {
    SimError(std::string code_value, const std::string& message)
        : std::runtime_error(message), code(std::move(code_value)) {}
    std::string code;
};

std::string utf8(NSString* value);
NSString* ns(const std::string& value);
std::string describe_error(NSError* error);

// Runs body, converting an escaping NSException into SimError(code). Private
// CoreSimulator proxies raise when a device goes away mid-call.
void guarded(const char* code, void (^body)(void));

struct Environment {
    std::string developer_dir;
    std::string core_simulator_version;
    bool simulator_kit_loaded = false;
    std::string simulator_kit_path;
    std::string load_error;
};

// Loads CoreSimulator (required) and SimulatorKit (input) once. Throws SimError
// when CoreSimulator cannot be loaded.
const Environment& load_frameworks(const std::string& developer_dir);
const Environment& environment();
void* simulator_kit_symbol(const char* name);
bool load_accessibility_framework(std::string& error);

// Device lookup through the shared service context for the selected Xcode.
id device_for_udid(const std::string& udid);
json list_devices();
json describe_device(id device);
std::string state_name(unsigned long long state);
bool device_booted(id device);

// Opens an XPC connection to a service inside the booted simulator (host to
// guest bridge). Returns nil and fills error when the service is missing.
// unsupported is set when CoreSimulator reports the service as unknown.
id connect_guest_service(id device, const char* service, std::string& error, bool* unsupported = nullptr);

// Interface rotation of the main display: 0, 90, 180 or 270 degrees, as
// reported by the guest CoreDevice display-info provider (0 when unknown).
int display_rotation(id device, const std::string& udid);
// Uncached display-info probe with the raw provider reply, for diagnostics.
json display_info(id device, const std::string& udid);

// Framed writer (serialized, safe from any queue). Frames with drop_if_busy
// are skipped while more than a couple of droppable frames are pending.
void emit(const json& header, const std::string& binary = {}, bool drop_if_busy = false);
void emit_log(const std::string& level, const std::string& message);

} // namespace acecode::ios_simulator::helper
