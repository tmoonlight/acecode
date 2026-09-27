#include "runtime.hpp"
#include "config/config.hpp"
#include "daemon/platform.hpp"
#include "remote_control/remote_control_service.hpp"
#include "utils/atomic_file.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <crow.h>
#include <cpr/cpr.h>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>

namespace acecode::channels {
namespace {
constexpr const char* kTokenHeader = "X-ACECode-Channels-Token";
std::optional<Json> endpoint(const std::filesystem::path& directory) {
    try {
        const auto path = directory / "owner.json";
        if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 4096) return std::nullopt;
        std::ifstream file(path, std::ios::binary);
        auto value = Json::parse(file);
        const int port = value.at("port").get<int>();
        const auto token = value.at("token").get<std::string>();
        if (port < 1 || port > 65535 || token.size() != 32 || value.at("protocol") != 1 ||
            !daemon::is_pid_alive(value.at("pid").get<std::int64_t>())) return std::nullopt;
        return value;
    } catch (...) { return std::nullopt; }
}
Json call(const Json& owner, const Json& command, int timeout = 35000) {
    const auto response = cpr::Post(
        cpr::Url{"http://127.0.0.1:" + std::to_string(owner.at("port").get<int>()) + "/channels"},
        cpr::Header{{"Content-Type", "application/json"}, {kTokenHeader, owner.at("token").get<std::string>()}},
        cpr::Body{command.dump()}, cpr::Proxies{{"http", ""}, {"https", ""}},
        cpr::ConnectTimeout{500}, cpr::Timeout{timeout});
    if (response.error.code != cpr::ErrorCode::OK) throw std::runtime_error("Channel owner unavailable; request outcome may be unknown");
    auto result = Json::parse(response.text);
    if (response.status_code != 200) throw std::runtime_error(result.value("error", "Channel request failed"));
    return result;
}
} // namespace

std::filesystem::path channel_directory() { return path_from_utf8(get_acecode_dir()) / "channels" / "whatsapp"; }

struct Runtime::Impl {
    GatewayDeps deps;
    std::filesystem::path directory;
    SpawnOptions spawn;
    State state;
    std::shared_ptr<Bridge> bridge = std::make_shared<Bridge>();
    std::unique_ptr<Gateway> gateway;
    std::unique_ptr<crow::SimpleApp> app;
    std::thread worker, server;
    std::atomic<bool> stopping{false}, server_exited{false};
    std::mutex mu, wait_mu;
    std::condition_variable wake;
    Json connection{{"state", "disabled"}, {"account", ""}};
    std::string last_error, account;
    std::chrono::steady_clock::time_point retry{};
    OwnerLock ownership;
    bool owns_account = false;

    Impl(GatewayDeps deps, std::filesystem::path directory, SpawnOptions spawn)
        : deps(std::move(deps)), directory(std::move(directory)), spawn(std::move(spawn)), state(this->directory) {
        if (!this->spawn) this->spawn = whatsapp_bridge_options;
        this->deps.request = [transport = bridge](const std::string& method, const Json& params) {
            return transport->request(method, params);
        };
    }
    void connect() {
        if (!bridge->running()) {
            bridge->start(spawn(state.transport_directory()));
            bridge->request("status", Json::object(), std::chrono::seconds(10));
        }
        connection = bridge->request("connect", Json::object(), std::chrono::seconds(10));
    }
    Json control(const Json& command) {
        const auto op = command.value("op", "status");
        if (op == "ping") return {{"protocol", 1}};
        std::lock_guard<std::mutex> lock(mu);
        if (op.compare(0, 6, "setup_") == 0) {
            throw std::runtime_error("Setup is local only. Run acecode channels.");
        }
        if (op == "status" || op == "qr") {
            auto result = connection;
            result["enabled"] = state.enabled(); result["host_running"] = true; result["error"] = last_error;
            result["access"] = state.snapshot().at("access");
            result.erase("qr"); // The rendered code is sufficient; never expose the raw secret in diagnostics.
            if (op != "qr") result.erase("qr_text");
            return result;
        }
        if (op == "on" || op == "reconnect") {
            state.set_enabled(true); last_error.clear();
            if (op == "reconnect") { bridge->stop(); }
            try { connect(); } catch (const std::exception& e) { last_error = e.what(); throw; }
            return {{"text", "WhatsApp enabled in the running daemon/Desktop."}};
        }
        if (op == "off") {
            state.set_enabled(false);
            bridge->stop();
            gateway->shutdown(); gateway = std::make_unique<Gateway>(state, deps);
            connection = {{"state", "disabled"}, {"account", account}};
            last_error.clear();
            return {{"text", "WhatsApp disconnected. Credentials and sessions retained."}};
        }
        auto request = command;
        if ((op == "allow" || op == "revoke") && !request.contains("account")) request["account"] = account;
        return gateway->control(request);
    }
    void host() {
        state.reload_history();
        gateway = std::make_unique<Gateway>(state, deps);
        const auto token = rc::generate_remote_control_token();
        app = std::make_unique<crow::SimpleApp>();
        CROW_ROUTE((*app), "/channels").methods(crow::HTTPMethod::Post)
            ([this, token](const crow::request& request) {
                crow::response response;
                response.set_header("Content-Type", "application/json");
                response.set_header("Cache-Control", "no-store");
                if (request.get_header_value(kTokenHeader) != token || !request.get_header_value("Origin").empty()) {
                    response.code = 403; response.body = R"({"error":"Unauthorized"})"; return response;
                }
                if (request.body.size() > kMaxTextBytes + 4096) {
                    response.code = 413; response.body = R"({"error":"Request too large"})"; return response;
                }
                try { response.body = control(Json::parse(request.body)).dump(); }
                catch (const std::exception& e) { response.code = 400; response.body = Json{{"error", e.what()}}.dump(); }
                return response;
            });
        app->bindaddr("127.0.0.1").port(0).concurrency(2).signal_clear();
        server_exited = false;
        server = std::thread([this] {
            try { app->run(); } catch (...) {}
            server_exited = true; wake.notify_all();
        });
        if (app->wait_for_server_start(std::chrono::seconds(3)) == std::cv_status::timeout || server_exited)
            throw std::runtime_error("Cannot start channel control listener");
        const Json owner{{"protocol", 1}, {"pid", daemon::current_pid()}, {"port", app->port()}, {"token", token}};
        // Authenticated readiness probe before publishing the owner descriptor.
        call(owner, {{"op", "ping"}}, 1000);
        if (!atomic_write_file(path_to_utf8(directory / "owner.json"), owner.dump(), true))
            throw std::runtime_error("Cannot publish channel owner");
        while (!stopping && !server_exited) {
            {
                std::lock_guard<std::mutex> lock(mu);
                if (state.enabled()) {
                    if (!bridge->running()) {
                        connection["state"] = "error";
                        if (!bridge->error().empty()) last_error = bridge->error();
                    }
                    if (!bridge->running() && std::chrono::steady_clock::now() >= retry) {
                        retry = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                        try { connect(); last_error.clear(); }
                        catch (const std::exception& e) { last_error = e.what(); connection["state"] = "error"; }
                    }
                    for (const auto& event : bridge->take_events()) {
                        try {
                            const auto type = event.value("event", "");
                            if (type == "status") {
                                connection = event;
                                account = event.value("account", std::string{});
                                if (event.value("state", "") == "connected") {
                                    last_error.clear();
                                    if (state.enabled()) gateway->restore(account);
                                }
                            } else if (type == "message") {
                                if (state.enabled() && event.value("account", "") == account && connection.value("state", "") == "connected")
                                    gateway->receive(event);
                            } else if (type == "error") last_error = event.value("error", "Bridge error");
                        } catch (const std::exception& e) { last_error = e.what(); }
                    }
                }
            }
            std::unique_lock<std::mutex> lock(wait_mu);
            wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping.load(); });
        }
    }
    void cleanup() {
        if (app) app->stop();
        if (server.joinable()) server.join();
        // Stop transport before joining hubs, so pending sends unblock immediately.
        bridge->stop();
        if (gateway) gateway->shutdown();
        gateway.reset(); app.reset();
        std::error_code ec; std::filesystem::remove(directory / "owner.json", ec);
    }
    void run() {
        while (!stopping) {
            try {
                if (owns_account || (owns_account = ownership.acquire(directory / "owner.lock"))) {
                    try { host(); }
                    catch (const std::exception& e) { LOG_ERROR(std::string("[channels] ") + e.what()); }
                    cleanup(); ownership.release(); owns_account = false;
                }
            } catch (const std::exception& e) { LOG_ERROR(std::string("[channels] ") + e.what()); }
            std::unique_lock<std::mutex> lock(wait_mu);
            wake.wait_for(lock, std::chrono::seconds(1), [&] { return stopping.load(); });
        }
        ownership.release(); owns_account = false;
    }
};
Runtime::Runtime(GatewayDeps deps, std::filesystem::path directory, SpawnOptions spawn)
    : impl_(std::make_unique<Impl>(std::move(deps), directory.empty() ? channel_directory() : std::move(directory), std::move(spawn))) {}
Runtime::~Runtime() { stop(); }
void Runtime::start() {
    if (impl_->worker.joinable()) return;
    impl_->stopping = false;
    try { impl_->state.load(); }
    catch (const std::exception& e) { LOG_ERROR(std::string("[channels] ") + e.what()); return; }
    if (!impl_->state.enabled()) return;
    // Claim before launching the worker so a later instance cannot win its race.
    try { impl_->owns_account = impl_->ownership.acquire(impl_->directory / "owner.lock"); }
    catch (const std::exception& e) { LOG_ERROR(std::string("[channels] ") + e.what()); }
    impl_->worker = std::thread([this] { impl_->run(); });
}
void Runtime::stop() {
    impl_->stopping = true; impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

Json request_control(const Json& command, const std::filesystem::path& supplied) {
    const auto directory = supplied.empty() ? channel_directory() : supplied;
    auto owner = endpoint(directory);
    if (owner) {
        try { call(*owner, {{"op", "ping"}}, 1000); }
        catch (...) { owner.reset(); }
    }
    if (!owner) {
        if (command.value("op", "status") == "status") {
            State state(directory); state.load();
            return {{"state", state.enabled() ? "configured; waiting for daemon/Desktop" : "disabled"},
                    {"enabled", state.enabled()}, {"host_running", false}, {"access", state.snapshot().at("access")}};
        }
        throw std::runtime_error("No channel host is running. Start acecode daemon or Desktop first; this command does not start one.");
    }
    return call(*owner, command);
}
} // namespace acecode::channels
