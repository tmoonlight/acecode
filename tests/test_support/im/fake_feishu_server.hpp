#pragma once

// 测试用:本机假飞书开放平台(只监听 127.0.0.1)。提供 tenant_access_token、bot/v3/info、
// 长连接地址(/callback/ws/endpoint)、二进制 WebSocket 长连接(pbbp2 帧)、发消息 / 回复、
// 图片与文件上传、消息资源下载,并允许测试注入失败,用于飞书传输层与 API 的端到端测试。

#include "im/feishu/feishu_frame.hpp"
#include "test_support/network/crow_test_server.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace acecode::test {

class FakeFeishuServer {
public:
    struct Request {
        std::string path;           // 不含查询串
        std::string query;          // receive_id_type / type 查询参数
        std::string authorization;
        std::string user_agent;
        std::string locale;
        std::string content_type;
        std::string raw;            // 原始请求体(multipart 时用来检查字段)
        nlohmann::json body;        // JSON 请求体(解析失败为空对象)
    };

    // 应答钩子:返回 (HTTP 状态码, 响应体)。未设置时一律成功。钩子在服务端锁外调用。
    std::function<std::pair<int, nlohmann::json>(const Request&)> message_handler;
    std::function<std::pair<int, nlohmann::json>(const Request&)> token_handler;
    std::function<std::pair<int, nlohmann::json>(const Request&)> endpoint_handler;
    std::function<std::pair<int, nlohmann::json>(const Request&)> bot_handler;
    std::function<std::pair<int, nlohmann::json>(const Request&)> upload_handler;

    // 长连接参数:默认重连无抖动、无间隔(测试里由本地退避决定节奏)。
    nlohmann::json client_config{{"ReconnectCount", -1}, {"ReconnectInterval", 0}, {"ReconnectNonce", 0},
                                 {"PingInterval", 120}};
    std::atomic<bool> respond_pings{true};
    std::string pong_payload;  // 非空时随 pong 下发(新的 ClientConfig)
    // 加到发消息类响应上的额外响应头(如 x-ogw-ratelimit-reset、X-Tt-Logid);请求开始前设置。
    std::vector<std::pair<std::string, std::string>> message_headers;
    std::int32_t service_id = 1234;

    FakeFeishuServer() {
        CROW_ROUTE(app_, "/open-apis/auth/v3/tenant_access_token/internal").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto request = record(req, "");
            int calls = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                calls = ++token_calls_;
            }
            if (token_handler) return reply(token_handler(request));
            return json_response(200, {{"code", 0}, {"msg", "ok"},
                                       {"tenant_access_token", "t-" + std::to_string(calls)}, {"expire", 7200}});
        });
        CROW_ROUTE(app_, "/open-apis/bot/v3/info")
        ([this](const crow::request& req) {
            const auto request = record(req, "");
            if (bot_handler) return reply(bot_handler(request));
            return json_response(200, {{"code", 0}, {"msg", "ok"},
                                       {"bot", {{"activate_status", 2}, {"app_name", "TestBot"},
                                                {"open_id", "ou_bot"}, {"ip_white_list", nlohmann::json::array()}}}});
        });
        CROW_ROUTE(app_, "/callback/ws/endpoint").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto request = record(req, "");
            {
                std::lock_guard<std::mutex> lock(mu_);
                ++endpoint_calls_;
            }
            if (endpoint_handler) return reply(endpoint_handler(request));
            return json_response(200, {{"code", 0}, {"msg", "ok"},
                                       {"data", {{"URL", ws_url()}, {"ClientConfig", client_config}}}});
        });
        CROW_ROUTE(app_, "/open-apis/im/v1/messages").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            return message(record(req, query_of(req, "receive_id_type")));
        });
        CROW_ROUTE(app_, "/open-apis/im/v1/messages/<string>/reply").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, const std::string&) { return message(record(req, "")); });
        CROW_ROUTE(app_, "/open-apis/im/v1/images").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto request = record(req, "");
            if (upload_handler) return reply(upload_handler(request));
            return json_response(200, {{"code", 0}, {"msg", "success"}, {"data", {{"image_key", "img_v3_up"}}}});
        });
        CROW_ROUTE(app_, "/open-apis/im/v1/files").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            const auto request = record(req, "");
            if (upload_handler) return reply(upload_handler(request));
            return json_response(200, {{"code", 0}, {"msg", "success"}, {"data", {{"file_key", "file_v3_up"}}}});
        });
        CROW_ROUTE(app_, "/open-apis/im/v1/messages/<string>/resources/<string>")
        ([this](const crow::request& req, const std::string&, const std::string& key) {
            record(req, query_of(req, "type"));
            if (key == "gone")
                return json_response(400, {{"code", 234043}, {"msg", "unsupported message type"}});
            if (key == "expired") {
                std::lock_guard<std::mutex> lock(mu_);
                if (!expired_once_) {
                    expired_once_ = true;
                    return json_response(400, {{"code", 99991663}, {"msg", "invalid access token"}});
                }
            }
            crow::response res(200, std::string(key == "big" ? 4096 : 16, 'z'));
            res.set_header("Content-Type", "image/png");
            return res;
        });
        CROW_WEBSOCKET_ROUTE(app_, "/ws/v2")
            .onopen([this](crow::websocket::connection& conn) {
                std::lock_guard<std::mutex> lock(mu_);
                conn_ = &conn;
                ++connections_;
            })
            .onclose([this](crow::websocket::connection& conn, const std::string&, std::uint16_t code) {
                std::lock_guard<std::mutex> lock(mu_);
                if (conn_ == &conn) conn_ = nullptr;
                close_codes_.push_back(code);
            })
            .onmessage([this](crow::websocket::connection& conn, const std::string& data, bool binary) {
                im::feishu::Frame frame;
                std::string error;
                const bool ok = binary && im::feishu::decode_frame(data, &frame, &error);
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    if (!binary) ++text_frames_;
                    if (ok) frames_.push_back(frame);
                }
                if (ok && frame.method == im::feishu::kMethodControl && frame.header("type") == "ping" &&
                    respond_pings) {
                    im::feishu::Frame pong = frame;
                    pong.headers = {{"type", "pong"}};
                    if (!pong_payload.empty()) pong.payload = pong_payload;
                    conn.send_binary(im::feishu::encode_frame(pong));
                }
            });
        server_ = std::make_unique<CrowTestServer>(app_);
        port_ = server_->port();
    }

    ~FakeFeishuServer() { server_.reset(); }

    std::string base() const { return server_->http_base(); }
    // 长连接地址:票据放在查询参数里,测试用来确认它不会出现在日志与状态里。
    std::string ws_url() const {
        return "ws://127.0.0.1:" + std::to_string(port_) +
               "/ws/v2?fpid=493&aid=552564&device_id=dev-7&access_key=AK-SECRET&service_id=" +
               std::to_string(service_id) + "&ticket=TICKET-SECRET";
    }

    // ---- 推送事件

    static nlohmann::json message_event(const std::string& message_id, const std::string& chat_type,
                                        const std::string& chat_id, const std::string& sender_open_id,
                                        const std::string& message_type, const nlohmann::json& content,
                                        const nlohmann::json& mentions = nullptr, std::int64_t create_time_ms = 0) {
        if (create_time_ms == 0) {
            create_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
        }
        nlohmann::json message{{"message_id", message_id},
                               {"create_time", std::to_string(create_time_ms)},
                               {"chat_id", chat_id},
                               {"chat_type", chat_type},
                               {"message_type", message_type},
                               {"content", content.dump()}};
        if (!mentions.is_null()) message["mentions"] = mentions;
        return {{"schema", "2.0"},
                {"header", {{"event_id", "ev-" + message_id}, {"event_type", "im.message.receive_v1"},
                            {"create_time", std::to_string(create_time_ms)}, {"app_id", "cli_app"},
                            {"tenant_key", "tk"}}},
                {"event", {{"sender", {{"sender_id", {{"open_id", sender_open_id}, {"union_id", "on_x"}}},
                                       {"sender_type", "user"}, {"tenant_key", "tk"}}},
                           {"message", message}}}};
    }

    // 以一个或多个 DATA type=event 帧推送 payload;parts > 1 时按字节平均切开。
    void push_event(const nlohmann::json& event, int parts = 1) {
        const auto payload = event.dump();
        const auto frame_id = "frame-" + std::to_string(++frame_seq_);
        const auto size = payload.size();
        for (int seq = 0; seq < parts; ++seq) {
            const auto begin = size * static_cast<std::size_t>(seq) / static_cast<std::size_t>(parts);
            const auto end = size * static_cast<std::size_t>(seq + 1) / static_cast<std::size_t>(parts);
            im::feishu::Frame frame;
            frame.seq_id = 0;
            frame.log_id = 0;
            frame.service = service_id;
            frame.method = im::feishu::kMethodData;
            frame.headers = {{"type", "event"},
                             {"message_id", frame_id},
                             {"sum", std::to_string(parts)},
                             {"seq", std::to_string(seq)},
                             {"trace_id", "trace-" + frame_id},
                             {"instance_id", "inst-1"}};
            frame.payload = payload.substr(begin, end - begin);
            push_frame(frame);
        }
    }

    void push_frame(const im::feishu::Frame& frame) {
        std::lock_guard<std::mutex> lock(mu_);
        if (conn_) conn_->send_binary(im::feishu::encode_frame(frame));
    }

    void close_connection(std::uint16_t code = 1000) {
        std::lock_guard<std::mutex> lock(mu_);
        if (conn_) conn_->close("server closing", code);
    }

    // ---- 观察

    std::vector<Request> requests() {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_;
    }
    std::vector<Request> requests_to(const std::string& prefix) {
        std::vector<Request> out;
        for (const auto& r : requests()) {
            if (r.path.rfind(prefix, 0) == 0) out.push_back(r);
        }
        return out;
    }
    // 发消息类请求(创建 + 回复),按到达顺序。
    std::vector<Request> sends() {
        std::vector<Request> out;
        for (const auto& r : requests()) {
            const bool reply = r.path.size() > 6 && r.path.compare(r.path.size() - 6, 6, "/reply") == 0;
            if (r.path == "/open-apis/im/v1/messages" || reply) out.push_back(r);
        }
        return out;
    }
    std::vector<im::feishu::Frame> frames() {
        std::lock_guard<std::mutex> lock(mu_);
        return frames_;
    }
    std::vector<im::feishu::Frame> frames_of_type(const std::string& type) {
        std::vector<im::feishu::Frame> out;
        for (const auto& frame : frames()) {
            if (frame.header("type") == type) out.push_back(frame);
        }
        return out;
    }
    int token_calls() {
        std::lock_guard<std::mutex> lock(mu_);
        return token_calls_;
    }
    int endpoint_calls() {
        std::lock_guard<std::mutex> lock(mu_);
        return endpoint_calls_;
    }
    int connections() {
        std::lock_guard<std::mutex> lock(mu_);
        return connections_;
    }
    int text_frames() {
        std::lock_guard<std::mutex> lock(mu_);
        return text_frames_;
    }
    std::vector<std::uint16_t> close_codes() {
        std::lock_guard<std::mutex> lock(mu_);
        return close_codes_;
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
            const auto json = nlohmann::json::parse(text);
            return json.is_object() ? json : nlohmann::json::object();
        } catch (...) {
            return nlohmann::json::object();
        }
    }
    static std::string query_of(const crow::request& req, const char* key) {
        const char* value = req.url_params.get(key);
        return value ? std::string(value) : std::string{};
    }
    static crow::response json_response(int status, const nlohmann::json& body) {
        crow::response res(status, body.dump());
        res.set_header("Content-Type", "application/json");
        return res;
    }
    static crow::response reply(const std::pair<int, nlohmann::json>& result) {
        return json_response(result.first, result.second);
    }
    Request record(const crow::request& req, const std::string& query) {
        Request request;
        request.path = req.url;
        request.query = query;
        request.authorization = req.get_header_value("Authorization");
        request.user_agent = req.get_header_value("User-Agent");
        request.locale = req.get_header_value("locale");
        request.content_type = req.get_header_value("Content-Type");
        request.raw = req.body;
        request.body = parse(req.body);
        std::lock_guard<std::mutex> lock(mu_);
        requests_.push_back(request);
        return request;
    }
    crow::response message(const Request& request) {
        crow::response res;
        if (message_handler) {
            res = reply(message_handler(request));
        } else {
            int count = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                count = ++sent_;
            }
            res = json_response(200, {{"code", 0}, {"msg", "success"},
                                      {"data", {{"message_id", "om_sent_" + std::to_string(count)}}}});
        }
        for (const auto& [name, value] : message_headers) res.set_header(name, value);
        return res;
    }

    crow::SimpleApp app_;
    std::unique_ptr<CrowTestServer> server_;
    std::uint16_t port_ = 0;
    std::mutex mu_;
    crow::websocket::connection* conn_ = nullptr;
    std::atomic<int> frame_seq_{0};
    int token_calls_ = 0;
    int endpoint_calls_ = 0;
    int connections_ = 0;
    int text_frames_ = 0;
    int sent_ = 0;
    bool expired_once_ = false;
    std::vector<Request> requests_;
    std::vector<im::feishu::Frame> frames_;
    std::vector<std::uint16_t> close_codes_;
};

} // namespace acecode::test
