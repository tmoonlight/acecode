#pragma once

// 测试用:本机假 Discord。REST 挂在 /api/v10 下(/users/@me、/applications/@me、/gateway/bot、
// 打开私聊、发消息(JSON 与 multipart)、输入状态、取消息),CDN 挂在 /cdn/attachments/...,
// 网关是两个 WebSocket 路由:/gateway/(首次登录)与 /resume/(READY 给出的恢复地址)。
// 各环节可注入失败:关闭码、不回 ACK、Resume 被拒、发消息返回 429 / 10008 等。只监听 127.0.0.1。

#include "test_support/network/crow_test_server.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace acecode::test {

class FakeDiscordServer {
public:
    struct Request {
        std::string method;
        std::string path;           // 去掉 /api/v10 前缀;CDN 请求为 /cdn/...
        std::string authorization;
        std::string user_agent;
        nlohmann::json body = nlohmann::json::object();  // JSON 请求体,或 multipart 的 payload_json
        std::vector<std::string> part_names;              // multipart 字段名
        std::string file_name;                            // multipart 文件字段的文件名
        std::string file_body;                            // multipart 文件内容
    };

    struct Frame {
        std::string route;  // "gateway" / "resume"
        nlohmann::json payload;
    };

    // 形如真实 token(三段 base64url),只用于测试。
    std::string token = "OTAwMDAwMDAwMDAwMDAwMDAx.GfakeX.test-token-abcdefghijklmnopqrstuvwxyz0123";
    std::string bot_id = "900000000000000001";
    std::string bot_name = "AceBot";
    std::string application_id = "900000000000000001";
    std::int64_t application_flags = std::int64_t{1} << 19;  // Message Content Intent(<100 个服务器的开关)
    int heartbeat_interval_ms = 1000;
    int gateway_bot_status = 200;           // /gateway/bot 的状态码(可模拟 401)
    int session_start_remaining = 999;
    std::atomic<bool> ack_heartbeats{true};
    std::atomic<int> identify_close_code{0};  // 非 0:收到 Identify 后用这个关闭码断开
    std::atomic<bool> resume_ok{true};        // false:收到 Resume 时回 op 9(d=false)
    // 发消息接口的应答;返回 {0, ...} 表示用默认应答。
    std::function<std::pair<int, nlohmann::json>(const Request&)> message_handler;
    // 签名“已过期”的附件 id:CDN 对不带 fresh=1 的请求回 404。
    std::set<std::string> expired_attachments;

    FakeDiscordServer() {
        CROW_ROUTE(app_, "/api/v10/users/@me")
        ([this](const crow::request& req) {
            const auto request = record_request(req, "GET", "/users/@me");
            if (!authorized(request)) return unauthorized();
            return json_response(200, {{"id", bot_id}, {"username", bot_name}, {"discriminator", "0"},
                                       {"global_name", nullptr}, {"bot", true}});
        });
        CROW_ROUTE(app_, "/api/v10/applications/@me")
        ([this](const crow::request& req) {
            const auto request = record_request(req, "GET", "/applications/@me");
            if (!authorized(request)) return unauthorized();
            return json_response(200, {{"id", application_id}, {"name", "Ace App"}, {"bot_public", false},
                                       {"flags", application_flags}});
        });
        CROW_ROUTE(app_, "/api/v10/gateway/bot")
        ([this](const crow::request& req) {
            const auto request = record_request(req, "GET", "/gateway/bot");
            if (!authorized(request)) return unauthorized();
            if (gateway_bot_status != 200)
                return json_response(gateway_bot_status, {{"message", "gateway failed"}, {"code", 0}});
            return json_response(200, {{"url", ws_url("/gateway")},
                                       {"shards", 1},
                                       {"session_start_limit",
                                        {{"total", 1000}, {"remaining", session_start_remaining},
                                         {"reset_after", 14400000}, {"max_concurrency", 1}}}});
        });
        CROW_ROUTE(app_, "/api/v10/users/@me/channels").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto request = record_request(req, "POST", "/users/@me/channels");
            if (!authorized(request)) return unauthorized();
            const auto recipient = request.body.value("recipient_id", std::string{});
            return json_response(200, {{"id", dm_channel_for(recipient)}, {"type", 1},
                                       {"recipients", nlohmann::json::array({{{"id", recipient}}})}});
        });
        CROW_ROUTE(app_, "/api/v10/channels/<string>/messages").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, const std::string& channel) {
            const auto request = record_request(req, "POST", "/channels/" + channel + "/messages");
            if (!authorized(request)) return unauthorized();
            if (message_handler) {
                auto reply = message_handler(request);
                if (reply.first != 0) return json_response(reply.first, reply.second);
            }
            nlohmann::json message{{"id", std::to_string(++message_seq_ + 990000000000000000LL)},
                                   {"channel_id", channel},
                                   {"content", request.body.value("content", std::string{})},
                                   {"attachments", nlohmann::json::array()}};
            if (!request.file_name.empty()) {
                const auto& declared = request.body.contains("attachments") ? request.body["attachments"]
                                                                            : nlohmann::json::array();
                const auto filename = declared.is_array() && !declared.empty()
                                          ? declared[0].value("filename", request.file_name)
                                          : request.file_name;
                message["attachments"].push_back({{"id", "991000000000000001"}, {"filename", filename},
                                                  {"size", request.file_body.size()}});
            }
            return json_response(200, message);
        });
        CROW_ROUTE(app_, "/api/v10/channels/<string>/typing").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, const std::string& channel) {
            const auto request = record_request(req, "POST", "/channels/" + channel + "/typing");
            if (!authorized(request)) return unauthorized();
            return crow::response(204);
        });
        CROW_ROUTE(app_, "/api/v10/channels/<string>/messages/<string>")
        ([this](const crow::request& req, const std::string& channel, const std::string& message) {
            const auto request = record_request(req, "GET", "/channels/" + channel + "/messages/" + message);
            if (!authorized(request)) return unauthorized();
            nlohmann::json attachments = nlohmann::json::array();
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& id : expired_attachments)
                attachments.push_back({{"id", id}, {"filename", "notes.txt"},
                                       {"url", cdn_url(channel, id, "notes.txt", true)}});
            return json_response(200, {{"id", message}, {"channel_id", channel}, {"attachments", attachments}});
        });
        CROW_ROUTE(app_, "/cdn/attachments/<string>/<string>/<string>")
        ([this](const crow::request& req, const std::string& channel, const std::string& id,
                const std::string& name) {
            record_request(req, "GET", "/cdn/attachments/" + channel + "/" + id + "/" + name);
            const bool fresh = req.url_params.get("fresh") != nullptr;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (expired_attachments.count(id) && !fresh) return crow::response(404);
            }
            return crow::response(200, std::string(name == "big.bin" ? 4096 : 32, 'd'));
        });
        CROW_WEBSOCKET_ROUTE(app_, "/gateway/")
            .onopen([this](crow::websocket::connection& conn) { opened(conn, "gateway"); })
            .onclose([this](crow::websocket::connection& conn, const std::string&, std::uint16_t code) {
                closed(conn, code);
            })
            .onmessage([this](crow::websocket::connection& conn, const std::string& data, bool) {
                message(conn, "gateway", data);
            });
        CROW_WEBSOCKET_ROUTE(app_, "/resume/")
            .onopen([this](crow::websocket::connection& conn) { opened(conn, "resume"); })
            .onclose([this](crow::websocket::connection& conn, const std::string&, std::uint16_t code) {
                closed(conn, code);
            })
            .onmessage([this](crow::websocket::connection& conn, const std::string& data, bool) {
                message(conn, "resume", data);
            });
        server_ = std::make_unique<CrowTestServer>(app_);
        port_ = server_->port();
    }

    ~FakeDiscordServer() { server_.reset(); }

    std::string api_base() const { return server_->http_base() + "/api/v10"; }
    std::string http_base() const { return server_->http_base(); }

    // 假服务给某个用户分配的私聊频道 id(数字串,形如真实频道 id)。
    static std::string dm_channel_for(const std::string& user_id) { return "8" + user_id; }

    std::string cdn_url(const std::string& channel, const std::string& id, const std::string& name,
                        bool fresh = false) const {
        return server_->http_base() + "/cdn/attachments/" + channel + "/" + id + "/" + name +
               (fresh ? "?fresh=1&ex=1&is=1&hm=sig" : "?ex=1&is=1&hm=sig");
    }

    // 推送一条 Dispatch(op 0),seq 自动递增。
    void push_dispatch(const std::string& type, const nlohmann::json& d) {
        push({{"op", 0}, {"t", type}, {"s", ++seq_}, {"d", d}});
    }
    // 私信:没有 guild_id,channel_type 1。
    void push_dm(const std::string& id, const std::string& user_id, const std::string& content,
                 nlohmann::json attachments = nlohmann::json::array()) {
        push_dispatch("MESSAGE_CREATE",
                      {{"id", id}, {"channel_id", dm_channel_for(user_id)}, {"channel_type", 1}, {"type", 0},
                       {"author", {{"id", user_id}, {"username", "alice"}, {"global_name", "Alice"}}},
                       {"content", content}, {"mentions", nlohmann::json::array()},
                       {"mention_roles", nlohmann::json::array()}, {"attachments", attachments}});
    }
    // 服务器频道消息;extra 里的字段覆盖默认值(mentions、type、referenced_message、author…)。
    void push_guild(const std::string& id, const std::string& channel, const std::string& user_id,
                    const std::string& content, const nlohmann::json& extra = nlohmann::json::object()) {
        nlohmann::json d{{"id", id}, {"channel_id", channel}, {"guild_id", "700000000000000001"},
                         {"channel_type", 0}, {"type", 0},
                         {"author", {{"id", user_id}, {"username", "bob"}, {"global_name", "Bob"}}},
                         {"member", {{"nick", nullptr}, {"roles", nlohmann::json::array()}}},
                         {"content", content}, {"mentions", nlohmann::json::array()},
                         {"mention_roles", nlohmann::json::array()}, {"attachments", nlohmann::json::array()}};
        for (auto it = extra.begin(); it != extra.end(); ++it) d[it.key()] = it.value();
        push_dispatch("MESSAGE_CREATE", d);
    }
    void push(const nlohmann::json& frame) {
        std::lock_guard<std::mutex> lock(mu_);
        if (conn_) conn_->send_text(frame.dump());
    }
    void close_connection(std::uint16_t code, const std::string& reason) {
        std::lock_guard<std::mutex> lock(mu_);
        if (conn_) conn_->close(reason, code);
    }

    std::vector<Request> requests() {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_;
    }
    std::vector<Request> requests_to(const std::string& method, const std::string& path_prefix) {
        std::vector<Request> out;
        for (const auto& r : requests())
            if (r.method == method && r.path.rfind(path_prefix, 0) == 0) out.push_back(r);
        return out;
    }
    std::vector<Frame> frames() {
        std::lock_guard<std::mutex> lock(mu_);
        return frames_;
    }
    std::vector<nlohmann::json> frames_with_op(int op) {
        std::vector<nlohmann::json> out;
        for (const auto& f : frames())
            if (f.payload.value("op", -1) == op) out.push_back(f.payload);
        return out;
    }
    int connections(const std::string& route) {
        std::lock_guard<std::mutex> lock(mu_);
        return route == "resume" ? resume_connections_ : gateway_connections_;
    }
    int identify_count() { return static_cast<int>(frames_with_op(2).size()); }
    std::vector<int> client_close_codes() {
        std::lock_guard<std::mutex> lock(mu_);
        return close_codes_;
    }
    std::int64_t last_seq() const { return seq_.load(); }

    // 等待条件成立(最多 timeout);条件在锁外求值。
    static bool wait_until(const std::function<bool()>& predicate,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return predicate();
    }

private:
    static nlohmann::json parse(const std::string& text) {
        try {
            return text.empty() ? nlohmann::json::object() : nlohmann::json::parse(text);
        } catch (...) {
            return nlohmann::json::object();
        }
    }
    static crow::response json_response(int status, const nlohmann::json& body) {
        crow::response res(status, body.dump());
        res.set_header("Content-Type", "application/json");
        return res;
    }
    static crow::response unauthorized() { return json_response(401, {{"message", "401: Unauthorized"}, {"code", 0}}); }

    bool authorized(const Request& request) const { return request.authorization == "Bot " + token; }

    std::string ws_url(const std::string& path) const { return "ws://127.0.0.1:" + std::to_string(port_) + path; }

    Request record_request(const crow::request& req, const std::string& method, const std::string& path) {
        Request request;
        request.method = method;
        request.path = path;
        request.authorization = req.get_header_value("Authorization");
        request.user_agent = req.get_header_value("User-Agent");
        const auto type = req.get_header_value("Content-Type");
        if (type.find("multipart/form-data") != std::string::npos) {
            crow::multipart::message message(req);
            for (const auto& part : message.parts) {
                const auto disposition = part.get_header_object("Content-Disposition");
                const auto name = disposition.params.count("name") ? disposition.params.at("name") : std::string{};
                request.part_names.push_back(name);
                if (disposition.params.count("filename")) {
                    request.file_name = disposition.params.at("filename");
                    request.file_body = part.body;
                } else if (name == "payload_json") {
                    request.body = parse(part.body);
                }
            }
        } else {
            request.body = parse(req.body);
        }
        std::lock_guard<std::mutex> lock(mu_);
        requests_.push_back(request);
        return request;
    }

    void opened(crow::websocket::connection& conn, const std::string& route) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            conn_ = &conn;
            if (route == "resume") ++resume_connections_;
            else ++gateway_connections_;
        }
        conn.send_text(nlohmann::json{{"op", 10}, {"d", {{"heartbeat_interval", heartbeat_interval_ms}}}}.dump());
    }

    void closed(crow::websocket::connection& conn, std::uint16_t code) {
        std::lock_guard<std::mutex> lock(mu_);
        close_codes_.push_back(code);
        if (conn_ == &conn) conn_ = nullptr;
    }

    void message(crow::websocket::connection& conn, const std::string& route, const std::string& data) {
        const auto frame = parse(data);
        const int op = frame.value("op", -1);
        int sessions = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            frames_.push_back({route, frame});
            if (op == 2) sessions = ++sessions_;
        }
        if (op == 1) {
            if (ack_heartbeats) conn.send_text(R"({"op":11})");
        } else if (op == 2) {
            if (const int code = identify_close_code.load(); code != 0) {
                conn.close("identify rejected", static_cast<std::uint16_t>(code));
                return;
            }
            conn.send_text(nlohmann::json{
                {"op", 0}, {"t", "READY"}, {"s", ++seq_},
                {"d",
                 {{"v", 10},
                  {"user", {{"id", bot_id}, {"username", bot_name}, {"bot", true}}},
                  {"guilds", nlohmann::json::array({{{"id", "700000000000000001"}, {"unavailable", true}}})},
                  {"session_id", "S" + std::to_string(sessions)},
                  {"resume_gateway_url", ws_url("/resume")},
                  {"application", {{"id", application_id}, {"flags", application_flags}}}}}}.dump());
        } else if (op == 6) {
            if (resume_ok) conn.send_text(nlohmann::json{{"op", 0}, {"t", "RESUMED"}, {"s", ++seq_}, {"d", nullptr}}.dump());
            else conn.send_text(R"({"op":9,"d":false})");
        }
    }

    crow::SimpleApp app_;
    std::unique_ptr<CrowTestServer> server_;
    std::uint16_t port_ = 0;
    std::mutex mu_;
    crow::websocket::connection* conn_ = nullptr;
    std::atomic<std::int64_t> seq_{0};
    std::atomic<long long> message_seq_{0};
    int sessions_ = 0;
    int gateway_connections_ = 0;
    int resume_connections_ = 0;
    std::vector<int> close_codes_;
    std::vector<Request> requests_;
    std::vector<Frame> frames_;
};

} // namespace acecode::test
