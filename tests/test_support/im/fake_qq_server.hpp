#pragma once

// 测试用:本机假 QQ 开放平台。提供令牌、/gateway、WebSocket 网关、发消息、上传、
// 扫码绑定任务等接口,并允许测试注入失败,用于 QQ 传输层与扫码流程的端到端测试。

#include "test_support/network/crow_test_server.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace acecode::test {

class FakeQqServer {
public:
    struct Request {
        std::string path;
        std::string authorization;
        nlohmann::json body;
    };

    // 发消息接口的应答:返回 HTTP 状态码与响应体;默认一律成功。
    std::function<std::pair<int, nlohmann::json>(const Request&)> message_handler;
    // 令牌接口的应答;默认返回 tok-N(每次递增)。
    std::function<nlohmann::json(const nlohmann::json&)> token_handler;
    // 绑定任务接口的应答;默认 create 返回 task-1,poll 返回 status 1。
    std::function<nlohmann::json(const nlohmann::json&)> create_bind_handler;
    std::function<nlohmann::json(const nlohmann::json&)> poll_bind_handler;
    // 收到 Identify(op 2)后服务端要做的事;默认回 READY。返回 false 表示不回 READY。
    std::function<bool(const nlohmann::json&)> on_identify;
    int heartbeat_interval_ms = 1000;
    int first_gateway_status = 200;  // 可模拟 /gateway 首次失败

    FakeQqServer() {
        CROW_ROUTE(app_, "/app/getAppAccessToken").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto body = parse(req.body);
            nlohmann::json reply;
            {
                std::lock_guard<std::mutex> lock(mu_);
                ++token_calls_;
                reply = token_handler ? token_handler(body)
                                      : nlohmann::json{{"access_token", "tok-" + std::to_string(token_calls_)},
                                                       {"expires_in", "7200"}};
            }
            return json_response(200, reply);
        });
        CROW_ROUTE(app_, "/gateway")
        ([this](const crow::request& req) {
            record({"/gateway", req.get_header_value("Authorization"), nlohmann::json::object()});
            std::lock_guard<std::mutex> lock(mu_);
            if (first_gateway_status != 200) {
                const int status = first_gateway_status;
                first_gateway_status = 200;
                return json_response(status, {{"code", status}, {"message", "gateway failed"}});
            }
            return json_response(200, {{"url", "ws://127.0.0.1:" + std::to_string(port_) + "/websocket"}});
        });
        CROW_ROUTE(app_, "/v2/users/<string>/messages").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, const std::string& id) {
            return message("/v2/users/" + id + "/messages", req);
        });
        CROW_ROUTE(app_, "/v2/groups/<string>/messages").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, const std::string& id) {
            return message("/v2/groups/" + id + "/messages", req);
        });
        CROW_ROUTE(app_, "/v2/users/<string>/files").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, const std::string& id) {
            record({"/v2/users/" + id + "/files", req.get_header_value("Authorization"), parse(req.body)});
            return json_response(200, {{"file_uuid", "F1"}, {"file_info", "INFO-1"}, {"ttl", 3600}});
        });
        CROW_ROUTE(app_, "/v2/groups/<string>/files").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, const std::string& id) {
            record({"/v2/groups/" + id + "/files", req.get_header_value("Authorization"), parse(req.body)});
            return json_response(200, {{"file_uuid", "F1"}, {"file_info", "INFO-1"}, {"ttl", 3600}});
        });
        CROW_ROUTE(app_, "/download/<string>")
        ([this](const crow::request& req, const std::string& name) {
            record({"/download/" + name, req.get_header_value("Authorization"), nlohmann::json::object()});
            crow::response res(200, std::string(name == "big" ? 4096 : 16, 'z'));
            return res;
        });
        CROW_ROUTE(app_, "/lite/create_bind_task").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto body = parse(req.body);
            record({"/lite/create_bind_task", "", body});
            std::lock_guard<std::mutex> lock(mu_);
            return json_response(200, create_bind_handler ? create_bind_handler(body)
                                                          : nlohmann::json{{"retcode", 0}, {"data", {{"task_id", "task-1"}}}});
        });
        CROW_ROUTE(app_, "/lite/poll_bind_result").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto body = parse(req.body);
            record({"/lite/poll_bind_result", "", body});
            std::lock_guard<std::mutex> lock(mu_);
            return json_response(200, poll_bind_handler ? poll_bind_handler(body)
                                                        : nlohmann::json{{"retcode", 0}, {"data", {{"status", 1}}}});
        });
        CROW_WEBSOCKET_ROUTE(app_, "/websocket")
            .onopen([this](crow::websocket::connection& conn) {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    conn_ = &conn;
                    ++connections_;
                }
                conn.send_text(nlohmann::json{{"op", 10}, {"d", {{"heartbeat_interval", heartbeat_interval_ms}}}}.dump());
                cv_.notify_all();
            })
            .onclose([this](crow::websocket::connection& conn, const std::string&, std::uint16_t) {
                std::lock_guard<std::mutex> lock(mu_);
                if (conn_ == &conn) conn_ = nullptr;
                cv_.notify_all();
            })
            .onmessage([this](crow::websocket::connection& conn, const std::string& data, bool) {
                const auto frame = parse(data);
                const int op = frame.value("op", -1);
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    frames_.push_back(frame);
                }
                cv_.notify_all();
                if (op == 1) {
                    conn.send_text(R"({"op":11})");
                } else if (op == 2) {
                    if (!on_identify || on_identify(frame)) {
                        conn.send_text(nlohmann::json{{"op", 0}, {"t", "READY"}, {"s", ++seq_},
                                                      {"d", {{"session_id", "S1"}, {"user", {{"id", "B1"}, {"username", "TestBot"}}}}}}.dump());
                    }
                } else if (op == 6) {
                    conn.send_text(nlohmann::json{{"op", 0}, {"t", "RESUMED"}, {"s", ++seq_}, {"d", nullptr}}.dump());
                }
            });
        server_ = std::make_unique<CrowTestServer>(app_);
        port_ = server_->port();
    }

    ~FakeQqServer() { server_.reset(); }

    std::string base() const { return server_->http_base(); }

    // 向当前连接推送一条单聊消息事件。
    void push_c2c(const std::string& id, const std::string& openid, const std::string& content,
                  nlohmann::json attachments = nlohmann::json::array()) {
        push({{"op", 0}, {"t", "C2C_MESSAGE_CREATE"}, {"s", ++seq_},
              {"d", {{"id", id}, {"content", content}, {"author", {{"user_openid", openid}}},
                     {"attachments", attachments}}}});
    }
    void push_group_at(const std::string& id, const std::string& group, const std::string& member,
                       const std::string& content) {
        push({{"op", 0}, {"t", "GROUP_AT_MESSAGE_CREATE"}, {"s", ++seq_},
              {"d", {{"id", id}, {"content", content}, {"group_openid", group},
                     {"author", {{"member_openid", member}}}}}});
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
    std::vector<Request> requests_to(const std::string& prefix) {
        std::vector<Request> out;
        for (const auto& r : requests()) if (r.path.rfind(prefix, 0) == 0) out.push_back(r);
        return out;
    }
    std::vector<nlohmann::json> frames() {
        std::lock_guard<std::mutex> lock(mu_);
        return frames_;
    }
    int token_calls() {
        std::lock_guard<std::mutex> lock(mu_);
        return token_calls_;
    }
    int connections() {
        std::lock_guard<std::mutex> lock(mu_);
        return connections_;
    }

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
            return nlohmann::json::parse(text);
        } catch (...) {
            return nlohmann::json::object();
        }
    }
    static crow::response json_response(int status, const nlohmann::json& body) {
        crow::response res(status, body.dump());
        res.set_header("Content-Type", "application/json");
        return res;
    }
    crow::response message(const std::string& path, const crow::request& req) {
        Request request{path, req.get_header_value("Authorization"), parse(req.body)};
        record(request);
        std::pair<int, nlohmann::json> reply{200, {{"id", "R" + std::to_string(requests().size())}}};
        if (message_handler) reply = message_handler(request);
        return json_response(reply.first, reply.second);
    }
    void record(const Request& request) {
        std::lock_guard<std::mutex> lock(mu_);
        requests_.push_back(request);
    }

    crow::SimpleApp app_;
    std::unique_ptr<CrowTestServer> server_;
    std::uint16_t port_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
    crow::websocket::connection* conn_ = nullptr;
    std::atomic<int> seq_{0};
    int token_calls_ = 0;
    int connections_ = 0;
    std::vector<Request> requests_;
    std::vector<nlohmann::json> frames_;
};

} // namespace acecode::test
