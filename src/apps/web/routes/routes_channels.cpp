// routes_channels.cpp — 消息通道(QQ、微信、飞书、钉钉、Telegram、Discord、LINE;openspec add-desktop-im-channels design D12)
//
// 所有接口只接受本机直连(远程 Web 代理转发进来的 127.0.0.2 也拒绝,返回 403),
// 并照常鉴权。凭据只回显尾号;扫码二维码内容只经本机 WS 推送,不写日志。
#include "web/server_impl.hpp"
#include "web/handlers/channels_handler.hpp"

#include "channels/core/host.hpp"

#include <chrono>
#include <functional>
#include <stdexcept>

namespace acecode::web {
using nlohmann::json;

void WebServer::Impl::broadcast_local_event(const std::string& type, const json& payload) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto text = json{{"type", type}, {"timestamp_ms", now}, {"payload", payload}}.dump(
        -1, ' ', false, json::error_handler_t::replace);
    std::lock_guard<std::mutex> lk(ws_mu);
    for (const auto& [conn, state] : ws_connections) {
        if (!state || !state->trusted_local) continue;
        try {
            conn->send_text(text);
        } catch (...) {
        }
    }
}

void WebServer::Impl::register_channels() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
        return with_cors(req, std::move(response));
    };
    // 本机直连 + 鉴权 + 宿主可用;不满足时返回要回给客户端的响应。
    const auto gate = [this, respond](const crow::request& req) -> std::optional<crow::response> {
        if (!is_trusted_local_client_address(req.remote_ip_address)) {
            log_unauthorized(req.url, req.remote_ip_address, "channels non-local");
            return respond(req, 403, channel_error_body("LOCAL_ONLY", "消息通道只能在本机设置"));
        }
        if (auto rejected = require_auth(req)) return rejected;
        if (!deps.channel_host) return respond(req, 503, channel_error_body("UNAVAILABLE", "消息通道不可用"));
        return std::nullopt;
    };
    // 宿主操作失败时抛的异常转成响应:未知平台 404,其余 400(消息可直接展示,不含凭据)。
    const auto run = [respond](const crow::request& req, const std::function<json()>& action) {
        try {
            return respond(req, 200, action());
        } catch (const std::invalid_argument& e) {
            return respond(req, 404, channel_error_body("UNKNOWN_PLATFORM", e.what()));
        } catch (const std::exception& e) {
            return respond(req, 400, channel_error_body("CHANNEL_ERROR", e.what()));
        }
    };
    const auto parse_body = [](const crow::request& req) { return json::parse(req.body, nullptr, false); };

    CROW_ROUTE(app, "/api/channels").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/channels").methods(crow::HTTPMethod::GET)
    ([this, gate, run](const crow::request& req) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        return run(req, [this] { return deps.channel_host->snapshot(); });
    });

    CROW_ROUTE(app, "/api/channels/<string>/enabled").methods(crow::HTTPMethod::POST)
    ([this, gate, run, respond, parse_body](const crow::request& req, const std::string& platform) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        bool enabled = false;
        std::string error;
        if (!parse_channel_enabled_request(parse_body(req), enabled, error))
            return respond(req, 400, channel_error_body("BAD_REQUEST", error));
        return run(req, [this, &platform, enabled] {
            deps.channel_host->platform(platform).set_enabled(enabled);
            return deps.channel_host->snapshot();
        });
    });

    CROW_ROUTE(app, "/api/channels/<string>/credentials").methods(crow::HTTPMethod::PUT)
    ([this, gate, run, respond, parse_body](const crow::request& req, const std::string& platform) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        if (!is_channel_platform(platform))
            return respond(req, 404, channel_error_body("UNKNOWN_PLATFORM", "未知的消息通道平台"));
        std::string error;
        const auto credentials = parse_channel_credentials_request(platform, parse_body(req), error);
        if (!credentials) return respond(req, 400, channel_error_body("BAD_REQUEST", error));
        return run(req, [this, &platform, &credentials] {
            deps.channel_host->platform(platform).set_credentials(*credentials);
            return deps.channel_host->snapshot();
        });
    });

    // 扫码绑定(QQ、微信);其他平台返回 404。
    CROW_ROUTE(app, "/api/channels/<string>/bind").methods(crow::HTTPMethod::POST)
    ([this, gate, run](const crow::request& req, const std::string& platform) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        return run(req, [this, &platform] { return deps.channel_host->start_bind(platform); });
    });
    CROW_ROUTE(app, "/api/channels/<string>/bind").methods(crow::HTTPMethod::DELETE)
    ([this, gate, run](const crow::request& req, const std::string& platform) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        return run(req, [this, &platform] {
            if (!deps.channel_host->has_platform(platform)) throw std::invalid_argument("未知的消息通道平台");
            deps.channel_host->cancel_bind(platform);
            return deps.channel_host->bind_state(platform);
        });
    });

    // 机主绑定:Telegram 一次性链接,飞书 / 钉钉 / Discord / LINE 一次性 6 位绑定码。
    CROW_ROUTE(app, "/api/channels/<string>/owner-link").methods(crow::HTTPMethod::POST)
    ([this, gate, run](const crow::request& req, const std::string& platform) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        return run(req, [this, &platform] { return deps.channel_host->owner_link(platform); });
    });

    CROW_ROUTE(app, "/api/channels/telegram/remove-webhook").methods(crow::HTTPMethod::POST)
    ([this, gate, run](const crow::request& req) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        return run(req, [this] {
            deps.channel_host->platform("telegram").action("remove_webhook", json::object());
            return deps.channel_host->snapshot();
        });
    });

    CROW_ROUTE(app, "/api/channels/<string>/requests/<string>/<string>").methods(crow::HTTPMethod::POST)
    ([this, gate, run, respond](const crow::request& req, const std::string& platform, const std::string& id,
                                 const std::string& decision) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        if (decision != "approve" && decision != "reject")
            return respond(req, 404, channel_error_body("NOT_FOUND", "未知的操作"));
        return run(req, [this, &platform, &id, &decision] {
            deps.channel_host->platform(platform).approve(id, decision == "approve");
            return deps.channel_host->snapshot();
        });
    });

    CROW_ROUTE(app, "/api/channels/<string>/access/<string>").methods(crow::HTTPMethod::DELETE)
    ([this, gate, run, respond](const crow::request& req, const std::string& platform, const std::string& raw) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        const auto principal = decode_channel_principal(raw);
        if (!principal) return respond(req, 400, channel_error_body("BAD_REQUEST", "授权身份格式不对"));
        return run(req, [this, &platform, &principal] {
            deps.channel_host->platform(platform).revoke(*principal);
            return deps.channel_host->snapshot();
        });
    });
}

} // namespace acecode::web
