// routes_ios_simulator.cpp — iOS Simulator settings, devices and the live
// panel stream (WS /ws/ios-simulator/<udid>). macOS only at runtime; other
// platforms answer "supported": false.
#include "web/server_impl.hpp"
#include "config/config_mutation.hpp"
#include "config/saved_models_revision.hpp"
#include "tool/ios_simulator/device_consent.hpp"
#include "tool/ios_simulator/input_plan.hpp"
#include "tool/ios_simulator/runtime.hpp"
#include "tool/ios_simulator/xcode_locator.hpp"
#include "tool/ios_simulator_tool.hpp"

#include <cmath>
#include <thread>

namespace acecode::web {
using nlohmann::json;
namespace sim = acecode::ios_simulator;

namespace {

struct SimulatorWsState {
    std::string udid;
    std::mutex mu;
    crow::websocket::connection* conn = nullptr;  // null once closed; guarded by mu
    std::uint64_t subscription = 0;
    // False while the Desktop shell draws the screen natively: no JPEG
    // stream, the socket only carries input and status.
    bool frames_enabled = true;
    double point_width = 0;
    double point_height = 0;
};

// Binary frame for the browser: u32le json_length | json | jpeg.
std::string browser_frame(const sim::StreamFrame& frame) {
    const auto header = json({{"type", "frame"}, {"seq", frame.seq}, {"width", frame.width},
                              {"height", frame.height}, {"rotation", frame.rotation}}).dump();
    std::string out;
    out.reserve(4 + header.size() + (frame.jpeg ? frame.jpeg->size() : 0));
    const auto length = static_cast<std::uint32_t>(header.size());
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>((length >> shift) & 0xff));
    out += header;
    if (frame.jpeg) out += *frame.jpeg;
    return out;
}

// Subscribes the socket to the device's JPEG stream (off Crow's IO thread:
// the first start can take seconds for Xcode discovery and helper start).
void subscribe_frames(const std::shared_ptr<SimulatorWsState>& state) {
    std::thread([state] {
        auto& runtime = sim::Runtime::instance();
        std::string error;
        const auto subscription = runtime.subscribe(state->udid, [state](const sim::StreamFrame& frame) {
            std::lock_guard<std::mutex> lock(state->mu);
            if (!state->conn) return;
            if (frame.ended) {
                state->conn->send_text(json({{"type", "ended"}, {"reason", frame.reason}}).dump());
                return;
            }
            if (frame.rotation == 90 || frame.rotation == 270) {
                if (state->point_width < state->point_height) std::swap(state->point_width, state->point_height);
            } else if (state->point_width > state->point_height) {
                std::swap(state->point_width, state->point_height);
            }
            if (state->frames_enabled) state->conn->send_binary(browser_frame(frame));
        }, &error);
        bool keep = false;
        {
            std::lock_guard<std::mutex> lock(state->mu);
            keep = state->conn != nullptr && state->frames_enabled && state->subscription == 0;
            if (keep) state->subscription = subscription;
            if (state->conn && !subscription)
                state->conn->send_text(json({{"type", "error"}, {"message", error}}).dump());
        }
        if (!keep && subscription) runtime.unsubscribe(subscription);
    }).detach();
}

json devices_payload(std::string* error) {
    auto& runtime = sim::Runtime::instance();
    json devices = json::array();
    const auto consent = sim::list_device_consent();
    for (const auto& device : runtime.devices(error)) {
        if (!device.available) continue;
        auto item = sim::to_json(device);
        item["allowed"] = consent.contains(device.udid);
        item["sessions"] = runtime.sessions_for(device.udid);
        devices.push_back(std::move(item));
    }
    return devices;
}

} // namespace

void WebServer::Impl::refresh_ios_simulator_tool_locked() {
    if (!deps.app_config) return;
    if (deps.tools) {
        refresh_ios_simulator_tools(*deps.tools, *deps.app_config);
        return;
    }
    sim::RuntimeConfig config;
    config.enabled = deps.app_config->ios_simulator.enabled;
    config.xcode_path = deps.app_config->ios_simulator.xcode_path;
    config.stream_fps = deps.app_config->ios_simulator.stream_fps;
    config.stream_scale = deps.app_config->ios_simulator.stream_scale;
    config.stream_quality = deps.app_config->ios_simulator.stream_quality;
    sim::Runtime::instance().configure(config);
}

void WebServer::Impl::register_ios_simulator() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
        return with_cors(req, std::move(response));
    };

    CROW_ROUTE(app, "/api/ios-simulator/status").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/config/ios-simulator").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/ios-simulator/xcode/inspect").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/ios-simulator/devices").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/ios-simulator/status").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        const bool refresh = req.url_params.get("refresh") != nullptr;
        return respond(req, 200, sim::Runtime::instance().status(refresh));
    });

    CROW_ROUTE(app, "/api/config/ios-simulator").methods(crow::HTTPMethod::PUT)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) return respond(req, 400, {{"error", "BAD_JSON"}});
        if (body.contains("xcode_path")) {
            if (!body["xcode_path"].is_string()) return respond(req, 400, {{"error", "BAD_REQUEST"}});
            const auto path = body["xcode_path"].get<std::string>();
            std::string why;
            if (!path.empty() && !sim::inspect_xcode(path, &why))
                return respond(req, 400, {{"error", "XCODE_INVALID"}, {"message", why}});
        }
        std::unique_lock<std::shared_mutex> lock(app_config_mu);
        const auto result = mutate_config([&body](AppConfig& candidate, std::string& error) {
            try {
                json section = ios_simulator_config_to_json(candidate.ios_simulator);
                for (const char* key : {"enabled", "xcode_path", "stream_fps", "stream_scale", "stream_quality"}) {
                    if (body.contains(key)) section[key] = body[key];
                }
                IosSimulatorConfig next;
                load_ios_simulator_config_json(section, next);
                candidate.ios_simulator = next;
                return true;
            } catch (const std::exception& e) {
                error = e.what();
                return false;
            }
        }, deps.config_path, deps.app_config);
        if (!result.ok) {
            const bool invalid = result.error_kind == ConfigMutationErrorKind::Validation;
            return respond(req, invalid ? 400 : 500, {{"error", invalid ? "BAD_REQUEST" : "PERSIST_FAILED"},
                                                      {"message", result.error}});
        }
        deps.app_config->ios_simulator = result.config.ios_simulator;
        refresh_ios_simulator_tool_locked();
        publish_live_saved_models(*deps.app_config, result.config.saved_models);
        lock.unlock();
        return respond(req, 200, sim::Runtime::instance().status(false));
    });

    CROW_ROUTE(app, "/api/ios-simulator/xcode/inspect").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("path") || !body["path"].is_string())
            return respond(req, 400, {{"error", "BAD_REQUEST"}});
        std::string why;
        auto install = sim::inspect_xcode(body["path"].get<std::string>(), &why);
        if (!install) return respond(req, 200, {{"ok", false}, {"message", why}});
        return respond(req, 200, {{"ok", true}, {"install", sim::to_json(*install)}});
    });

    CROW_ROUTE(app, "/api/ios-simulator/devices").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        std::string error;
        auto devices = devices_payload(&error);
        json body = {{"devices", devices}, {"error", devices.empty() ? error : std::string()}};
        if (auto session = req.url_params.get("session"))
            body["session_device"] = sim::Runtime::instance().session_device(session);
        return respond(req, 200, body);
    });

    CROW_ROUTE(app, "/api/ios-simulator/devices/<string>/<string>").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&, const std::string&) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/ios-simulator/devices/<string>/<string>").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req, const std::string& udid, const std::string& action) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        auto& runtime = sim::Runtime::instance();
        std::string error;
        auto tool = runtime.simctl(&error);
        if (!tool) return respond(req, 409, {{"error", "XCODE_UNAVAILABLE"}, {"message", error}});
        if (action == "boot") {
            auto result = tool->boot(udid);
            if (!result.ok) return respond(req, 500, {{"error", "BOOT_FAILED"}, {"message", result.message()}});
        } else if (action == "shutdown") {
            auto result = tool->shutdown(udid);
            if (!result.ok) return respond(req, 500, {{"error", "SHUTDOWN_FAILED"}, {"message", result.message()}});
            runtime.post({{"op", "release"}, {"udid", udid}});
        } else if (action == "consent") {
            const auto body = json::parse(req.body, nullptr, false);
            const bool allowed = body.is_object() && body.value("allowed", false);
            if (allowed) sim::grant_device_consent(udid, body.value("name", std::string()));
            else sim::revoke_device_consent(udid);
        } else {
            return respond(req, 404, {{"error", "UNKNOWN_ACTION"}});
        }
        auto devices = devices_payload(&error);
        return respond(req, 200, {{"devices", devices}});
    });

    CROW_ROUTE(app, "/api/ios-simulator/devices/<string>/screenshot").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req, const std::string& udid) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        auto response = sim::Runtime::instance().call(
            {{"op", "screenshot"}, {"udid", udid}, {"format", "png"}, {"scale", 1.0}}, 30000);
        if (!response.ok) {
            crow::response failed(500, json({{"error", response.error_code}, {"message", response.message}}).dump());
            failed.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(failed));
        }
        crow::response image(200);
        image.add_header("Content-Type", "image/png");
        image.add_header("Cache-Control", "no-store");
        image.body = std::move(response.binary);
        return with_cors(req, std::move(image));
    });

    // WS /ws/ios-simulator/<udid>?token=…[&frames=0] — server → client: binary
    // frames (u32le json_length | json | jpeg) and JSON status texts; client →
    // server: {type:"touch",phase,x,y} | {type:"button",name} |
    // {type:"key",name} | {type:"type",text} | {type:"text",text} |
    // {type:"frames",enabled}. Points are in the interface orientation of the
    // last frame. frames=0 / enabled:false stop the JPEG stream while the
    // Desktop shell draws the screen natively.
    CROW_WEBSOCKET_ROUTE(app, "/ws/ios-simulator/<string>")
        .onaccept([this](const crow::request& req, void** userdata) -> bool {
            std::string header_token = req.get_header_value("X-ACECode-Token");
            std::string query_token;
            if (auto t = req.url_params.get("token")) query_token = t;
            if (auth_result_for_request(req, header_token, query_token) != AuthResult::Allowed) {
                log_unauthorized(req.url, req.remote_ip_address, "ios simulator ws");
                return false;
            }
            std::string udid = req.url;
            const std::string prefix = "/ws/ios-simulator/";
            const auto pos = udid.find(prefix);
            if (pos == std::string::npos) return false;
            udid = udid.substr(pos + prefix.size());
            if (auto q = udid.find('?'); q != std::string::npos) udid = udid.substr(0, q);
            if (udid.empty() || udid.size() > 64) return false;
            auto* holder = new std::shared_ptr<SimulatorWsState>(std::make_shared<SimulatorWsState>());
            (*holder)->udid = udid;
            if (auto frames = req.url_params.get("frames")) (*holder)->frames_enabled = std::string(frames) != "0";
            *userdata = holder;
            return true;
        })
        .onopen([](crow::websocket::connection& conn) {
            auto* holder = static_cast<std::shared_ptr<SimulatorWsState>*>(conn.userdata());
            if (!holder) { conn.close("no state"); return; }
            auto state = *holder;
            {
                std::lock_guard<std::mutex> lock(state->mu);
                state->conn = &conn;
            }
            // Device discovery can take seconds the first time (Xcode
            // discovery, helper start); keep it off Crow's IO thread.
            std::thread([state] {
                std::string error;
                if (auto info = sim::Runtime::instance().describe(state->udid, &error)) {
                    std::lock_guard<std::mutex> lock(state->mu);
                    state->point_width = info->point_width;
                    state->point_height = info->point_height;
                    if (state->conn)
                        state->conn->send_text(json({{"type", "device"}, {"device", sim::to_json(*info)}}).dump());
                }
                bool frames = false;
                {
                    std::lock_guard<std::mutex> lock(state->mu);
                    frames = state->conn != nullptr && state->frames_enabled;
                }
                if (frames) subscribe_frames(state);
            }).detach();
        })
        .onmessage([](crow::websocket::connection& conn, const std::string& data, bool is_binary) {
            auto* holder = static_cast<std::shared_ptr<SimulatorWsState>*>(conn.userdata());
            if (!holder || is_binary) return;
            auto state = *holder;
            const auto msg = json::parse(data, nullptr, false);
            if (!msg.is_object()) return;
            const auto type = msg.value("type", std::string());
            auto& runtime = sim::Runtime::instance();
            if (type == "touch") {
                double x = msg.value("x", 0.0);
                double y = msg.value("y", 0.0);
                if (!std::isfinite(x) || !std::isfinite(y)) return;
                {
                    std::lock_guard<std::mutex> lock(state->mu);
                    // Without frames the orientation is not tracked here; the
                    // helper turns touches itself, so only bound them loosely.
                    const double bound = std::max(state->point_width, state->point_height);
                    const double width = state->frames_enabled ? state->point_width : bound;
                    const double height = state->frames_enabled ? state->point_height : bound;
                    if (width > 0) x = std::clamp(x, 0.0, width);
                    if (height > 0) y = std::clamp(y, 0.0, height);
                }
                const auto phase = msg.value("phase", std::string("down"));
                json request = {{"op", "touch"}, {"udid", state->udid}, {"phase", phase == "up" ? "up" : "down"},
                                {"x", x}, {"y", y}};
                if (msg.contains("edge") && msg["edge"].is_string()) request["edge"] = msg["edge"];
                runtime.post(request);
            } else if (type == "frames") {
                const bool enabled = msg.value("enabled", true);
                std::uint64_t dropped = 0;
                {
                    std::lock_guard<std::mutex> lock(state->mu);
                    if (state->frames_enabled == enabled) return;
                    state->frames_enabled = enabled;
                    if (!enabled) std::swap(dropped, state->subscription);
                }
                if (dropped) runtime.unsubscribe(dropped);
                if (enabled) subscribe_frames(state);
            } else if (type == "button") {
                runtime.post({{"op", "button"}, {"udid", state->udid}, {"name", msg.value("name", std::string())}});
            } else if (type == "key") {
                if (auto chord = sim::chord_for_name(msg.value("name", std::string())))
                    runtime.post({{"op", "keys"}, {"udid", state->udid}, {"events", sim::plan_chord(*chord)}});
            } else if (type == "type" || type == "text") {
                const auto text = msg.value("text", std::string());
                if (text.empty() || text.size() > 8000) return;
                if (auto strokes = sim::keys_for_text(text)) {
                    runtime.post({{"op", "keys"}, {"udid", state->udid}, {"events", sim::plan_keystrokes(*strokes, 4)}});
                } else {
                    std::string error;
                    if (auto tool = runtime.simctl(&error); tool && tool->pbcopy(state->udid, text).ok)
                        runtime.post({{"op", "keys"}, {"udid", state->udid},
                                      {"events", sim::plan_chord({sim::kUsageLeftCommand, sim::kUsageV})}});
                }
            }
        })
        .onclose([](crow::websocket::connection& conn, const std::string&, uint16_t) {
            auto* holder = static_cast<std::shared_ptr<SimulatorWsState>*>(conn.userdata());
            if (!holder) return;
            auto state = *holder;
            std::uint64_t subscription = 0;
            {
                std::lock_guard<std::mutex> lock(state->mu);
                state->conn = nullptr;
                subscription = state->subscription;
                state->subscription = 0;
            }
            if (subscription) sim::Runtime::instance().unsubscribe(subscription);
            conn.userdata(nullptr);
            delete holder;
        });
}

} // namespace acecode::web
