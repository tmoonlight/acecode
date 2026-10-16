// acecode-ios-sim: drives Xcode iOS Simulators for the ACECode daemon.
//
// Usage: acecode-ios-sim --developer-dir <Xcode.app/Contents/Developer> [--hid indigo|dtuhid]
// Speaks the framed protocol in tool/ios_simulator/wire.hpp over stdin/stdout.
// Exits when its parent exits or stdin closes.

#import "sim_ax.h"
#import "sim_core.h"
#import "sim_output.h"
#import "sim_display.h"
#import "sim_hid.h"
#import "private_api.h"

#include "tool/ios_simulator/wire.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>

using namespace acecode::ios_simulator::helper;
namespace wire = acecode::ios_simulator::wire;

namespace {

struct DeviceContext {
    id device = nil;
    std::string udid;
    double point_width = 0;
    double point_height = 0;
    dispatch_queue_t input_queue = nil;
    dispatch_queue_t display_queue = nil;
    std::unique_ptr<HidSession> hid;   // input_queue only
    std::shared_ptr<Display> display;  // display_queue only
};

std::string g_requested_hid;
std::mutex g_devices_mu;
std::map<std::string, std::shared_ptr<DeviceContext>> g_devices;
std::map<std::uint64_t, std::string> g_stream_devices;
dispatch_queue_t g_control_queue = nil;
dispatch_queue_t g_ax_queue = nil;
std::string g_environment_error;

void reply_ok(const json& request, json result = json::object(), const std::string& binary = {}) {
    emit({{"id", request.value("id", 0)}, {"ok", true}, {"result", std::move(result)}}, binary);
}

void reply_error(const json& request, const std::string& code, const std::string& message) {
    emit({{"id", request.value("id", 0)}, {"ok", false}, {"error", {{"code", code}, {"message", message}}}});
}

std::string required_string(const json& request, const char* key) {
    if (!request.contains(key) || !request[key].is_string() || request[key].get<std::string>().empty())
        throw SimError("BAD_REQUEST", std::string("Missing ") + key + ".");
    return request[key].get<std::string>();
}

double number(const json& request, const char* key, double fallback) {
    return request.contains(key) && request[key].is_number() ? request[key].get<double>() : fallback;
}

std::shared_ptr<DeviceContext> context_for(const std::string& udid) {
    if (!g_environment_error.empty()) throw SimError("CORESIMULATOR_UNAVAILABLE", g_environment_error);
    {
        std::lock_guard<std::mutex> lock(g_devices_mu);
        auto found = g_devices.find(udid);
        if (found != g_devices.end()) return found->second;
    }
    id device = device_for_udid(udid);
    auto description = describe_device(device);
    auto context = std::make_shared<DeviceContext>();
    context->device = device;
    context->udid = udid;
    context->point_width = description.value("point_width", 0.0);
    context->point_height = description.value("point_height", 0.0);
    const std::string label = "dev.acecode.ios-sim.device." + udid;
    context->input_queue = dispatch_queue_create((label + ".input").c_str(), DISPATCH_QUEUE_SERIAL);
    context->display_queue = dispatch_queue_create((label + ".display").c_str(), DISPATCH_QUEUE_SERIAL);
    std::lock_guard<std::mutex> lock(g_devices_mu);
    auto [it, inserted] = g_devices.emplace(udid, context);
    return it->second;
}

void forget_device(const std::string& udid) {
    std::shared_ptr<DeviceContext> context;
    {
        std::lock_guard<std::mutex> lock(g_devices_mu);
        auto found = g_devices.find(udid);
        if (found == g_devices.end()) return;
        context = found->second;
        g_devices.erase(found);
        for (auto it = g_stream_devices.begin(); it != g_stream_devices.end();) {
            if (it->second == udid) it = g_stream_devices.erase(it); else ++it;
        }
    }
    dispatch_async(context->display_queue, ^{
        if (context->display) context->display->stop_all();
        context->display.reset();
    });
    dispatch_async(context->input_queue, ^{ context->hid.reset(); });
}

HidSession& hid_for(DeviceContext& context) {
    if (!device_booted(context.device)) {
        context.hid.reset();
        throw SimError("DEVICE_NOT_BOOTED", "The simulator is not booted.");
    }
    if (!context.hid)
        context.hid = open_hid_session(context.device, context.point_width, context.point_height, g_requested_hid);
    return *context.hid;
}

Display& display_for(DeviceContext& context) {
    if (!device_booted(context.device)) {
        if (context.display) context.display->stop_all();
        context.display.reset();
        throw SimError("DEVICE_NOT_BOOTED", "The simulator is not booted.");
    }
    if (!context.display) context.display = open_display(context.device, context.udid);
    return *context.display;
}

Direction direction_of(const json& value) {
    const auto phase = value.value("phase", value.value("direction", std::string("down")));
    return phase == "up" ? Direction::Up : Direction::Down;
}

unsigned edge_of(const json& value) {
    const auto edge = value.value("edge", std::string("none"));
    if (edge == "top") return 1;
    if (edge == "left") return 2;
    if (edge == "bottom") return 3;
    if (edge == "right") return 4;
    return 0;
}

// Interface-orientation points -> native panel points (idb geometry rules).
void unrotate(int rotation, const DeviceContext& context, double& x, double& y) {
    const double width = context.point_width;
    const double height = context.point_height;
    const double ix = x;
    const double iy = y;
    if (rotation == 90) { x = iy; y = height - ix; }
    else if (rotation == 180) { x = width - ix; y = height - iy; }
    else if (rotation == 270) { x = width - iy; y = ix; }
}

unsigned unrotate_edge(int rotation, unsigned edge) {
    // edges in clockwise order: top(1), right(4), bottom(3), left(2)
    static const unsigned order[] = {1, 4, 3, 2};
    if (edge == 0) return 0;
    int index = 0;
    while (index < 4 && order[index] != edge) ++index;
    if (index == 4) return 0;
    const int turns = rotation == 90 ? 3 : rotation == 180 ? 2 : rotation == 270 ? 1 : 0;
    return order[(index + turns) % 4];
}

json oriented_size(const DeviceContext& context, int rotation) {
    const bool quarter = rotation == 90 || rotation == 270;
    return {{"point_width", quarter ? context.point_height : context.point_width},
            {"point_height", quarter ? context.point_width : context.point_height},
            {"rotation", rotation}};
}

void sleep_ms(double ms) {
    if (ms > 0) usleep(static_cast<useconds_t>(std::min(ms, 10000.0) * 1000));
}

// Runs body on queue, translating SimError into an error reply.
void run_on(dispatch_queue_t queue, const json& request, void (^body)(void)) {
    json copy = request;
    dispatch_async(queue, ^{
        try {
            body();
        } catch (const SimError& error) {
            reply_error(copy, error.code, error.what());
        } catch (const std::exception& error) {
            reply_error(copy, "INTERNAL_ERROR", error.what());
        }
    });
}

// Parameters are taken by value: the blocks below capture copies.
void handle_input(const json request, const std::string op, const std::shared_ptr<DeviceContext> context) {
    run_on(context->input_queue, request, ^{
        try {
            HidSession& hid = hid_for(*context);
            const bool touches = op == "touch" || op == "touch_sequence";
            const int rotation = touches ? display_rotation(context->device, context->udid) : 0;
            const auto touch_at = ^(HidSession& target, const json& event) {
                double x = number(event, "x", 0);
                double y = number(event, "y", 0);
                unrotate(rotation, *context, x, y);
                target.touch(direction_of(event), x, y, unrotate_edge(rotation, edge_of(event)));
            };
            if (op == "touch") {
                touch_at(hid, request);
                if (request.value("flush", false)) hid.flush();
            } else if (op == "touch_sequence") {
                const auto& events = request.at("events");
                for (const auto& event : events) {
                    const auto type = event.value("type", std::string());
                    if (type == "delay") sleep_ms(number(event, "ms", 0));
                    else if (type == "touch") touch_at(hid, event);
                    else if (type == "touch2") {
                        double x1 = number(event, "x1", 0), y1 = number(event, "y1", 0);
                        double x2 = number(event, "x2", 0), y2 = number(event, "y2", 0);
                        unrotate(rotation, *context, x1, y1);
                        unrotate(rotation, *context, x2, y2);
                        hid.touch2(direction_of(event), x1, y1, x2, y2);
                    }
                    else throw SimError("BAD_REQUEST", "Unknown touch event type: " + type);
                }
                hid.flush();
            } else if (op == "button") {
                HidButton button;
                const auto name = required_string(request, "name");
                if (!lookup_button(name, button)) throw SimError("BAD_REQUEST", "Unknown button: " + name);
                const auto action = request.value("action", std::string("press"));
                if (action == "down" || action == "press") hid.button(button, Direction::Down);
                if (action == "press") sleep_ms(number(request, "duration_ms", 80));
                if (action == "up" || action == "press") hid.button(button, Direction::Up);
                hid.flush();
            } else if (op == "keys") {
                for (const auto& event : request.at("events")) {
                    if (event.contains("delay_ms")) { sleep_ms(number(event, "delay_ms", 0)); continue; }
                    const auto usage = event.value("usage", 0u);
                    if (usage == 0 || usage > 0xFFFF) throw SimError("BAD_REQUEST", "Invalid HID keyboard usage.");
                    hid.key(usage, direction_of(event));
                }
                hid.flush();
            }
            reply_ok(request, {{"transport", hid.transport()}});
        } catch (const SimError& error) {
            if (error.code == "HID_FAILED" || error.code == "HID_TIMEOUT") context->hid.reset();
            throw;
        }
    });
}

void handle(const json& incoming) {
    // Blocks capture this copy; the decoded frame dies when handle returns.
    const json request = incoming;
    const auto op = request.value("op", std::string());
    if (op == "hello") {
        const auto& env = environment();
        reply_ok(request, {{"protocol", wire::kProtocolVersion}, {"pid", getpid()},
            {"developer_dir", env.developer_dir}, {"core_simulator_version", env.core_simulator_version},
            {"simulator_kit_loaded", env.simulator_kit_loaded}, {"simulator_kit_path", env.simulator_kit_path},
            {"simulator_kit_error", env.load_error}, {"environment_error", g_environment_error},
            {"default_hid_transport", g_environment_error.empty() ? default_transport() : ""}});
        return;
    }
    if (op == "list_devices") {
        run_on(g_control_queue, request, ^{
            if (!g_environment_error.empty()) throw SimError("CORESIMULATOR_UNAVAILABLE", g_environment_error);
            reply_ok(request, {{"devices", list_devices()}});
        });
        return;
    }
    if (op == "stop_stream") {
        const auto stream = request.value("stream", 0ull);
        std::shared_ptr<DeviceContext> context;
        {
            std::lock_guard<std::mutex> lock(g_devices_mu);
            auto found = g_stream_devices.find(stream);
            if (found != g_stream_devices.end()) {
                auto device = g_devices.find(found->second);
                if (device != g_devices.end()) context = device->second;
                g_stream_devices.erase(found);
            }
        }
        if (!context) { reply_ok(request); return; }
        run_on(context->display_queue, request, ^{
            if (context->display) context->display->stop_stream(stream);
            reply_ok(request);
        });
        return;
    }
    const auto udid = request.value("udid", std::string());
    if (udid.empty()) { reply_error(request, "BAD_REQUEST", "Missing udid."); return; }
    if (op == "release") { forget_device(udid); reply_ok(request); return; }
    std::shared_ptr<DeviceContext> context;
    try {
        context = context_for(udid);
    } catch (const SimError& error) {
        reply_error(request, error.code, error.what());
        return;
    }
    if (op == "touch" || op == "touch_sequence" || op == "button" || op == "keys") {
        handle_input(request, op, context);
    } else if (op == "describe") {
        run_on(context->display_queue, request, ^{
            auto out = describe_device(context->device);
            out["default_hid_transport"] = default_transport();
            out["native_point_width"] = context->point_width;
            out["native_point_height"] = context->point_height;
            if (out.value("state", std::string()) == "booted") {
                out.update(oriented_size(*context, display_rotation(context->device, context->udid)));
                try {
                    Display& display = display_for(*context);
                    out["framebuffer_width"] = display.pixel_width();
                    out["framebuffer_height"] = display.pixel_height();
                } catch (const SimError& error) {
                    out["display_error"] = error.what();
                }
            }
            reply_ok(request, out);
        });
    } else if (op == "screenshot") {
        run_on(context->display_queue, request, ^{
            const auto image = display_for(*context).capture(request.value("format", std::string("png")),
                number(request, "scale", 1.0), number(request, "quality", 0.8));
            json result = {{"format", image.format}, {"width", image.width}, {"height", image.height}};
            result.update(oriented_size(*context, image.rotation));
            reply_ok(request, result, image.bytes);
        });
    } else if (op == "start_stream") {
        const auto stream = request.value("stream", 0ull);
        {
            std::lock_guard<std::mutex> lock(g_devices_mu);
            g_stream_devices[stream] = udid;
        }
        run_on(context->display_queue, request, ^{
            Display& display = display_for(*context);
            display.start_stream(stream, number(request, "fps", 30), number(request, "scale", 0.5),
                                 number(request, "quality", 0.7));
            reply_ok(request, {{"framebuffer_width", display.pixel_width()},
                               {"framebuffer_height", display.pixel_height()}});
        });
    } else if (op == "ui_describe") {
        run_on(g_ax_queue, request, ^{
            UiQuery query;
            query.mode = request.value("mode", std::string("tree"));
            query.x = number(request, "x", 0);
            query.y = number(request, "y", 0);
            query.max_depth = static_cast<int>(number(request, "max_depth", 40));
            query.max_nodes = static_cast<int>(number(request, "max_nodes", 600));
            reply_ok(request, describe_ui(context->device, udid, query));
        });
    } else if (op == "display_info") {
        run_on(context->display_queue, request, ^{ reply_ok(request, display_info(context->device, udid)); });
    } else if (op == "set_hardware_keyboard") {
        run_on(context->input_queue, request, ^{
            __block BOOL ok = NO;
            __block NSError* failure = nil;
            const BOOL enabled = request.value("enabled", true) ? YES : NO;
            guarded("KEYBOARD_FAILED", ^{
                id device = context->device;
                if (![device respondsToSelector:@selector(setHardwareKeyboardEnabled:keyboardType:error:)]) return;
                ok = [(id<ACESimDevice>)device setHardwareKeyboardEnabled:enabled keyboardType:0 error:&failure];
            });
            if (!ok) throw SimError("KEYBOARD_FAILED", "Could not change the hardware keyboard: " +
                describe_error(failure));
            reply_ok(request);
        });
    } else {
        reply_error(request, "BAD_REQUEST", "Unknown op: " + op);
    }
}

void read_loop() {
    wire::Decoder decoder;
    char buffer[65536];
    while (true) {
        const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count == 0) _exit(0);
        if (count < 0) {
            if (errno == EINTR) continue;
            _exit(0);
        }
        decoder.feed(buffer, static_cast<std::size_t>(count));
        while (auto frame = decoder.next()) handle(frame->header);
        if (!decoder.error().empty()) _exit(3);
    }
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        std::string developer_dir;
        for (int index = 1; index < argc; ++index) {
            const std::string arg = argv[index];
            if (arg == "--developer-dir" && index + 1 < argc) developer_dir = argv[++index];
            else if (arg == "--hid" && index + 1 < argc) g_requested_hid = argv[++index];
            else return 2;
        }
        if (developer_dir.empty()) return 2;
        signal(SIGPIPE, SIG_IGN);
        const pid_t parent = getppid();
        if (parent <= 1) return 2;
        dispatch_queue_t watchdog = dispatch_queue_create("dev.acecode.ios-sim.lifetime", DISPATCH_QUEUE_SERIAL);
        dispatch_source_t parent_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC,
            static_cast<uintptr_t>(parent), DISPATCH_PROC_EXIT, watchdog);
        if (!parent_source) return 2;
        dispatch_source_set_event_handler(parent_source, ^{ _exit(0); });
        dispatch_resume(parent_source);
        if (getppid() != parent) return 2;
        try {
            load_frameworks(developer_dir);
        } catch (const SimError& error) {
            g_environment_error = error.what();
        }
        g_control_queue = dispatch_queue_create("dev.acecode.ios-sim.control", DISPATCH_QUEUE_SERIAL);
        g_ax_queue = dispatch_queue_create("dev.acecode.ios-sim.ax", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_t reader = dispatch_queue_create("dev.acecode.ios-sim.reader", DISPATCH_QUEUE_SERIAL);
        dispatch_async(reader, ^{ read_loop(); });
        dispatch_main();
    }
}
