#import "sim_core.h"
#import "private_api.h"

#include <atomic>
#include <cerrno>
#include <dlfcn.h>
#include <mutex>
#include <unistd.h>

namespace acecode::ios_simulator::helper {
namespace {

std::mutex g_env_mu;
Environment g_env;
bool g_loaded = false;
void* g_simulator_kit_image = nullptr;
id g_service_context = nil;
id g_device_set = nil;

NSString* const kCoreSimulatorPath = @"/Library/Developer/PrivateFrameworks/CoreSimulator.framework";
NSString* const kAccessibilityPath =
    @"/System/Library/PrivateFrameworks/AccessibilityPlatformTranslation.framework";

bool load_bundle(NSString* path, std::string& error) {
    NSBundle* bundle = [NSBundle bundleWithPath:path];
    if (!bundle) {
        error = "missing " + utf8(path);
        return false;
    }
    if (bundle.loaded) return true;
    NSError* failure = nil;
    if (![bundle loadAndReturnError:&failure]) {
        error = "could not load " + utf8(path) + ": " + describe_error(failure);
        return false;
    }
    return true;
}

id service_device_set() {
    if (g_device_set) return g_device_set;
    Class factory = NSClassFromString(@"SimServiceContext");
    if (!factory) throw SimError("CORESIMULATOR_UNAVAILABLE", "SimServiceContext is unavailable.");
    __block id context = nil;
    __block NSError* failure = nil;
    guarded("CORESIMULATOR_ERROR", ^{
        context = [(id<ACESimServiceContextFactory>)factory
            sharedServiceContextForDeveloperDir:ns(g_env.developer_dir) error:&failure];
    });
    if (!context)
        throw SimError("CORESIMULATOR_ERROR", "Could not open the CoreSimulator service: " + describe_error(failure));
    __block id set = nil;
    guarded("CORESIMULATOR_ERROR", ^{
        set = [(id<ACESimServiceContext>)context defaultDeviceSetWithError:&failure];
    });
    if (!set)
        throw SimError("CORESIMULATOR_ERROR", "Could not open the default simulator set: " + describe_error(failure));
    g_service_context = context;
    g_device_set = set;
    return set;
}

std::string family_name(int family) {
    switch (family) {
        case 1: return "iphone";
        case 2: return "ipad";
        case 3: return "appletv";
        case 4: return "watch";
        case 6: return "homepod";
        case 7: return "vision";
        default: return "unknown";
    }
}

} // namespace

std::string utf8(NSString* value) {
    if (!value) return {};
    const char* bytes = value.UTF8String;
    return bytes ? std::string(bytes) : std::string{};
}

NSString* ns(const std::string& value) {
    return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding] ?: @"";
}

std::string describe_error(NSError* error) {
    if (!error) return "unknown error";
    return utf8(error.localizedDescription) + " (" + utf8(error.domain) + " " + std::to_string(error.code) + ")";
}

void guarded(const char* code, void (^body)(void)) {
    @try {
        body();
    } @catch (NSException* exception) {
        throw SimError(code, utf8(exception.name) + ": " + utf8(exception.reason));
    }
}

const Environment& load_frameworks(const std::string& developer_dir, bool simulator_kit) {
    std::lock_guard<std::mutex> lock(g_env_mu);
    if (g_loaded) return g_env;
    g_env.developer_dir = developer_dir;
    std::string error;
    if (!NSClassFromString(@"SimDevice") && !load_bundle(kCoreSimulatorPath, error))
        throw SimError("CORESIMULATOR_UNAVAILABLE", error +
            ". Open Xcode once (or run xcodebuild -runFirstLaunch) to install simulator components.");
    Class device_class = NSClassFromString(@"SimDevice");
    if (!device_class) throw SimError("CORESIMULATOR_UNAVAILABLE", "CoreSimulator did not provide SimDevice.");
    g_env.core_simulator_version =
        utf8([NSBundle bundleForClass:device_class].infoDictionary[@"CFBundleVersion"]);

    g_loaded = true;
    if (!simulator_kit) return g_env;
    NSString* developer = ns(developer_dir);
    NSArray<NSString*>* candidates = @[
        [[developer stringByAppendingPathComponent:@"../SharedFrameworks/SimulatorKit.framework"]
            stringByStandardizingPath],
        [developer stringByAppendingPathComponent:@"Library/PrivateFrameworks/SimulatorKit.framework"],
    ];
    for (NSString* path in candidates) {
        if (![[NSFileManager defaultManager] fileExistsAtPath:path]) continue;
        std::string kit_error;
        if (!load_bundle(path, kit_error)) { g_env.load_error = kit_error; continue; }
        NSString* binary = [NSBundle bundleWithPath:path].executablePath;
        g_simulator_kit_image = binary ? dlopen(binary.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL) : nullptr;
        g_env.simulator_kit_loaded = g_simulator_kit_image != nullptr;
        g_env.simulator_kit_path = utf8(path);
        if (g_env.simulator_kit_loaded) break;
    }
    if (!g_env.simulator_kit_loaded && g_env.load_error.empty())
        g_env.load_error = "SimulatorKit.framework was not found in the selected Xcode.";
    return g_env;
}

const Environment& environment() {
    std::lock_guard<std::mutex> lock(g_env_mu);
    return g_env;
}

void* simulator_kit_symbol(const char* name) {
    return g_simulator_kit_image ? dlsym(g_simulator_kit_image, name) : nullptr;
}

bool load_accessibility_framework(std::string& error) {
    if (NSClassFromString(@"AXPTranslator")) return true;
    return load_bundle(kAccessibilityPath, error) && NSClassFromString(@"AXPTranslator") != nil;
}

std::string state_name(unsigned long long state) {
    switch (state) {
        case 0: return "creating";
        case 1: return "shutdown";
        case 2: return "booting";
        case 3: return "booted";
        case 4: return "shutting_down";
        default: return "unknown";
    }
}

bool device_booted(id device) {
    __block unsigned long long state = 0;
    guarded("DEVICE_GONE", ^{ state = [(id<ACESimDevice>)device state]; });
    return state == 3;
}

id device_for_udid(const std::string& udid) {
    std::lock_guard<std::mutex> lock(g_env_mu);
    id set = service_device_set();
    NSUUID* wanted = [[NSUUID alloc] initWithUUIDString:ns(udid)];
    if (!wanted) throw SimError("BAD_UDID", "Invalid simulator UDID: " + udid);
    __block id found = nil;
    guarded("CORESIMULATOR_ERROR", ^{
        for (id device in [(id<ACESimDeviceSet>)set devices]) {
            if ([[(id<ACESimDevice>)device UDID] isEqual:wanted]) { found = device; break; }
        }
    });
    if (!found) throw SimError("DEVICE_NOT_FOUND", "No simulator with UDID " + udid + ".");
    return found;
}

json describe_device(id device) {
    __block json out = json::object();
    guarded("DEVICE_GONE", ^{
        id<ACESimDevice> sim = (id<ACESimDevice>)device;
        id<ACESimDeviceType> type = (id<ACESimDeviceType>)sim.deviceType;
        id<ACESimRuntime> runtime = (id<ACESimRuntime>)sim.runtime;
        const CGSize pixels = type.mainScreenSize;
        const double scale = type.mainScreenScale > 0 ? type.mainScreenScale : 1.0;
        out["udid"] = utf8(sim.UDID.UUIDString);
        out["name"] = utf8(sim.name);
        out["state"] = state_name(sim.state);
        out["device_type"] = utf8(type.name);
        out["product_family"] = family_name(type.productFamilyID);
        out["runtime"] = utf8(runtime.name);
        out["runtime_version"] = utf8(runtime.versionString);
        out["runtime_identifier"] = utf8(runtime.identifier);
        out["pixel_width"] = pixels.width;
        out["pixel_height"] = pixels.height;
        out["scale"] = scale;
        out["point_width"] = pixels.width / scale;
        out["point_height"] = pixels.height / scale;
    });
    return out;
}

json list_devices() {
    id set = nil;
    {
        std::lock_guard<std::mutex> lock(g_env_mu);
        set = service_device_set();
    }
    __block NSArray* devices = nil;
    guarded("CORESIMULATOR_ERROR", ^{ devices = [(id<ACESimDeviceSet>)set devices]; });
    json out = json::array();
    for (id device in devices) {
        try { out.push_back(describe_device(device)); } catch (const SimError&) {}
    }
    return out;
}

} // namespace acecode::ios_simulator::helper
