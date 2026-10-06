#pragma once

// 测试用:本机假钉钉开放平台(只监听 127.0.0.1)。一个端口同时扮演三个角色:
//   - api.dingtalk.com:/v1.0/oauth2/accessToken、/v1.0/gateway/connections/open、
//     /v1.0/robot/oToMessages/batchSend、/v1.0/robot/groupMessages/send、/v1.0/robot/messageFiles/download;
//   - oapi.dingtalk.com:/media/upload(multipart)、会话 webhook /robot/sendBySession?session=…;
//   - Stream 网关:WebSocket /connect?ticket=…(ticket 一次性),以及 OSS 下载 /oss/<code>
//     (带 Content-Type 头的请求一律 403,模拟签名校验失败)。
// 各接口都可以注入应答,用于钉钉传输层与 API 客户端的端到端测试。

#include "test_support/network/crow_test_server.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace acecode::test {

class FakeDingTalkServer {
public:
    struct Request {
        std::string path;
        std::string token;           // x-acs-dingtalk-access-token 头或 access_token 查询参数
        std::string query;           // 关心的查询参数(webhook 的 session、上传的 type)
        std::string content_type;    // 请求的 Content-Type 头
        nlohmann::json body;         // JSON 请求体;multipart 时为普通字段
        std::string file_name;       // multipart 文件字段的文件名
        std::string file_body;       // multipart 文件内容
    };

    using Reply = std::pair<int, nlohmann::json>;

    std::string client_id = "dingTESTCLIENT";
    std::string client_secret = "client-secret-value-123";

    // 以下应答钩子可选;返回 status=0 表示走默认应答。
    std::function<Reply(const Request&)> token_handler;
    std::function<Reply(const Request&)> open_handler;
    std::function<Reply(const Request&)> webhook_handler;
    std::function<Reply(const Request&)> oto_handler;
    std::function<Reply(const Request&)> group_handler;
    std::function<Reply(const Request&)> upload_handler;
    std::function<Reply(const Request&)> download_handler;

    FakeDingTalkServer() {
        CROW_ROUTE(app_, "/v1.0/oauth2/accessToken").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto request = make_request("/v1.0/oauth2/accessToken", req);
            record(request);
            int calls = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                calls = ++token_calls_;
            }
            if (auto custom = call(token_handler, request); custom.first) return json_response(custom);
            if (request.body.value("appKey", "") != client_id || request.body.value("appSecret", "") != client_secret)
                return json_response({400, {{"code", "invalidClientIdOrSecret"}, {"requestid", "R-T"},
                                            {"message", "无效的clientId或者clientSecret"}}});
            return json_response({200, {{"accessToken", "tok-" + std::to_string(calls)}, {"expireIn", 7200}}});
        });
        CROW_ROUTE(app_, "/v1.0/gateway/connections/open").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto request = make_request("/v1.0/gateway/connections/open", req);
            record(request);
            if (auto custom = call(open_handler, request); custom.first) return json_response(custom);
            if (request.body.value("clientId", "") != client_id ||
                request.body.value("clientSecret", "") != client_secret)
                return json_response({401, {{"code", "authFailed"}, {"requestid", "R-O"}, {"message", "鉴权失败"}}});
            std::string ticket;
            {
                std::lock_guard<std::mutex> lock(mu_);
                ticket = "ticket-" + std::to_string(++ticket_seq_);
                tickets_.insert(ticket);
                issued_.push_back(ticket);
            }
            return json_response({200, {{"endpoint", "ws://127.0.0.1:" + std::to_string(port_) + "/connect"},
                                        {"ticket", ticket}}});
        });
        CROW_ROUTE(app_, "/robot/sendBySession").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            auto request = make_request("/robot/sendBySession", req);
            request.query = param(req, "session");
            record(request);
            if (auto custom = call(webhook_handler, request); custom.first) return json_response(custom);
            return json_response({200, {{"errcode", 0}, {"errmsg", "ok"}}});
        });
        CROW_ROUTE(app_, "/v1.0/robot/oToMessages/batchSend").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto request = make_request("/v1.0/robot/oToMessages/batchSend", req);
            record(request);
            if (auto auth = check_token(request); auth.first) return json_response(auth);
            if (auto custom = call(oto_handler, request); custom.first) return json_response(custom);
            return json_response({200, {{"processQueryKey", "PQK-OTO"},
                                        {"invalidStaffIdList", nlohmann::json::array()},
                                        {"flowControlledStaffIdList", nlohmann::json::array()}}});
        });
        CROW_ROUTE(app_, "/v1.0/robot/groupMessages/send").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto request = make_request("/v1.0/robot/groupMessages/send", req);
            record(request);
            if (auto auth = check_token(request); auth.first) return json_response(auth);
            if (auto custom = call(group_handler, request); custom.first) return json_response(custom);
            return json_response({200, {{"processQueryKey", "PQK-GROUP"}}});
        });
        CROW_ROUTE(app_, "/v1.0/robot/messageFiles/download").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto request = make_request("/v1.0/robot/messageFiles/download", req);
            record(request);
            if (auto auth = check_token(request); auth.first) return json_response(auth);
            if (auto custom = call(download_handler, request); custom.first) return json_response(custom);
            return json_response({200, {{"downloadUrl", base() + "/oss/" + request.body.value("downloadCode", "")}}});
        });
        CROW_ROUTE(app_, "/oss/<string>")
        ([this](const crow::request& req, const std::string& code) {
            auto request = make_request("/oss/" + code, req);
            record(request);
            if (!request.content_type.empty()) return crow::response(403, "SignatureDoesNotMatch");
            return crow::response(200, std::string(code == "big" ? 4096 : 16, 'z'));
        });
        CROW_ROUTE(app_, "/media/upload").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            auto request = make_request("/media/upload", req);
            request.token = param(req, "access_token");
            request.query = param(req, "type");
            record(request);
            if (auto custom = call(upload_handler, request); custom.first) return json_response(custom);
            if (!valid_token(request.token))
                return json_response({200, {{"errcode", 40014}, {"errmsg", "不合法的access_token"}}});
            int seq = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                seq = ++media_seq_;
            }
            return json_response({200, {{"errcode", 0}, {"errmsg", "ok"}, {"type", request.query},
                                        {"media_id", "@media-" + std::to_string(seq)}}});
        });
        CROW_WEBSOCKET_ROUTE(app_, "/connect")
            .onaccept([this](const crow::request& req, void**) -> bool {
                const auto ticket = param(req, "ticket");
                std::lock_guard<std::mutex> lock(mu_);
                used_.push_back(ticket);
                // ticket 一次性:用过即作废,没发过的一律拒绝握手。
                return tickets_.erase(ticket) > 0;
            })
            .onopen([this](crow::websocket::connection& conn) {
                std::lock_guard<std::mutex> lock(mu_);
                conn_ = &conn;
                ++connections_;
            })
            .onclose([this](crow::websocket::connection& conn, const std::string&, std::uint16_t) {
                std::lock_guard<std::mutex> lock(mu_);
                if (conn_ == &conn) conn_ = nullptr;
            })
            .onmessage([this](crow::websocket::connection&, const std::string& data, bool) {
                std::lock_guard<std::mutex> lock(mu_);
                acks_.push_back(parse(data));
            });
        server_ = std::make_unique<CrowTestServer>(app_);
        port_ = server_->port();
    }

    ~FakeDingTalkServer() { server_.reset(); }

    std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::string webhook(const std::string& session) const { return base() + "/robot/sendBySession?session=" + session; }

    static std::int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // 单聊消息的 data(staff 为空时模拟组织外用户:只有加密 senderId)。
    nlohmann::json private_text(const std::string& msg_id, const std::string& staff, const std::string& text,
                                const std::string& session = "S1", std::int64_t expires_ms = 0) const {
        nlohmann::json data{{"msgId", msg_id},
                            {"msgtype", "text"},
                            {"text", {{"content", text}}},
                            {"conversationType", "1"},
                            {"conversationId", "cidPRIVATE"},
                            {"senderId", "$:LWCP_v1:$" + (staff.empty() ? std::string("ext") : staff)},
                            {"senderNick", "Ann"},
                            {"chatbotUserId", "$:LWCP_v1:$bot"},
                            {"robotCode", client_id},
                            {"sessionWebhook", webhook(session)},
                            {"sessionWebhookExpiredTime", expires_ms ? expires_ms : now_ms() + 90 * 60 * 1000},
                            {"createAt", now_ms()}};
        if (!staff.empty()) data["senderStaffId"] = staff;
        return data;
    }

    nlohmann::json group_text(const std::string& msg_id, const std::string& cid, const std::string& staff,
                              const std::string& text, bool at_bot = true) const {
        return {{"msgId", msg_id},
                {"msgtype", "text"},
                {"text", {{"content", text}}},
                {"conversationType", "2"},
                {"conversationId", cid},
                {"conversationTitle", "Team"},
                {"senderId", "$:LWCP_v1:$" + staff},
                {"senderStaffId", staff},
                {"senderNick", "Bob"},
                {"chatbotUserId", "$:LWCP_v1:$bot"},
                {"isInAtList", at_bot},
                {"atUsers", at_bot ? nlohmann::json::array({{{"dingtalkId", "$:LWCP_v1:$bot"}}})
                                   : nlohmann::json::array()},
                {"robotCode", client_id},
                {"sessionWebhook", webhook("G-" + cid)},
                {"sessionWebhookExpiredTime", now_ms() + 90 * 60 * 1000}};
    }

    // 推一条机器人消息回调;header_id 为帧头 messageId(重投时换新)。
    void push_callback(const std::string& header_id, const nlohmann::json& data) {
        push({{"specVersion", "1.0"},
              {"type", "CALLBACK"},
              {"headers", {{"appId", "app"}, {"connectionId", "c1"}, {"contentType", "application/json"},
                           {"messageId", header_id}, {"time", "1690106592000"},
                           {"topic", "/v1.0/im/bot/messages/get"}}},
              {"data", data.dump()}});
    }
    void push_ping(const std::string& header_id, const std::string& opaque) {
        push({{"specVersion", "1.0"},
              {"type", "SYSTEM"},
              {"headers", {{"contentType", "application/json"}, {"messageId", header_id}, {"topic", "ping"},
                           {"time", 1690106592000LL}}},
              {"data", nlohmann::json{{"opaque", opaque}}.dump()}});
    }
    void push_disconnect(const std::string& header_id) {
        nlohmann::json headers{{"contentType", "application/json"}, {"topic", "disconnect"}};
        if (!header_id.empty()) headers["messageId"] = header_id;
        push({{"specVersion", "1.0"}, {"type", "SYSTEM"}, {"headers", headers},
              {"data", R"({"reason": "connection is expired"})"}});
    }
    void push(const nlohmann::json& frame) {
        std::lock_guard<std::mutex> lock(mu_);
        if (conn_) conn_->send_text(frame.dump());
    }
    void close_connection(std::uint16_t code = 1000) {
        std::lock_guard<std::mutex> lock(mu_);
        if (conn_) conn_->close("bye", code);
    }

    std::vector<Request> requests() {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_;
    }
    std::vector<Request> requests_to(const std::string& path) {
        std::vector<Request> out;
        for (const auto& r : requests()) if (r.path == path) out.push_back(r);
        return out;
    }
    std::vector<nlohmann::json> acks() {
        std::lock_guard<std::mutex> lock(mu_);
        return acks_;
    }
    bool has_ack(const std::string& message_id) {
        for (const auto& ack : acks()) {
            if (ack.contains("headers") && ack["headers"].value("messageId", "") == message_id) return true;
        }
        return false;
    }
    int connections() {
        std::lock_guard<std::mutex> lock(mu_);
        return connections_;
    }
    int token_calls() {
        std::lock_guard<std::mutex> lock(mu_);
        return token_calls_;
    }
    std::vector<std::string> issued_tickets() {
        std::lock_guard<std::mutex> lock(mu_);
        return issued_;
    }
    std::vector<std::string> used_tickets() {
        std::lock_guard<std::mutex> lock(mu_);
        return used_;
    }
    // 令牌作废:之后这些令牌调用 OpenAPI 一律 InvalidAuthentication。
    void revoke_token(const std::string& token) {
        std::lock_guard<std::mutex> lock(mu_);
        revoked_.insert(token);
    }

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
            return nlohmann::json::parse(text);
        } catch (...) {
            return nlohmann::json::object();
        }
    }
    static std::string param(const crow::request& req, const char* name) {
        const char* value = req.url_params.get(name);
        return value ? std::string(value) : std::string{};
    }
    static crow::response json_response(const Reply& reply) {
        crow::response res(reply.first, reply.second.dump());
        res.set_header("Content-Type", "application/json");
        return res;
    }
    static Reply call(const std::function<Reply(const Request&)>& handler, const Request& request) {
        return handler ? handler(request) : Reply{0, nullptr};
    }
    static Request make_request(const std::string& path, const crow::request& req) {
        Request request;
        request.path = path;
        request.token = req.get_header_value("x-acs-dingtalk-access-token");
        request.content_type = req.get_header_value("Content-Type");
        if (request.content_type.find("multipart/form-data") != std::string::npos) {
            request.body = nlohmann::json::object();
            crow::multipart::message message(req);
            for (const auto& part : message.parts) {
                const auto disposition = part.get_header_object("Content-Disposition");
                const auto name = disposition.params.count("name") ? disposition.params.at("name") : std::string{};
                if (disposition.params.count("filename")) {
                    request.file_name = disposition.params.at("filename");
                    request.file_body = part.body;
                } else {
                    request.body[name] = part.body;
                }
            }
        } else {
            request.body = req.body.empty() ? nlohmann::json::object() : parse(req.body);
        }
        return request;
    }
    bool valid_token(const std::string& token) {
        std::lock_guard<std::mutex> lock(mu_);
        return token.rfind("tok-", 0) == 0 && !revoked_.count(token);
    }
    Reply check_token(const Request& request) {
        if (valid_token(request.token)) return {0, nullptr};
        return {400, {{"code", "InvalidAuthentication"}, {"requestid", "R-A"}, {"message", "不合法的access_token"}}};
    }
    void record(const Request& request) {
        std::lock_guard<std::mutex> lock(mu_);
        requests_.push_back(request);
    }

    crow::SimpleApp app_;
    std::unique_ptr<CrowTestServer> server_;
    std::uint16_t port_ = 0;
    std::mutex mu_;
    crow::websocket::connection* conn_ = nullptr;
    int connections_ = 0;
    int token_calls_ = 0;
    int ticket_seq_ = 0;
    int media_seq_ = 0;
    std::set<std::string> tickets_;
    std::set<std::string> revoked_;
    std::vector<std::string> issued_;
    std::vector<std::string> used_;
    std::vector<Request> requests_;
    std::vector<nlohmann::json> acks_;
};

} // namespace acecode::test
