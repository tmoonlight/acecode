#pragma once

// 测试用:本机假微信 iLink 服务。
//   /ilink/bot/get_bot_qrcode、get_qrcode_status(扫码登录,未鉴权 GET)
//   /ilink/bot/getupdates(长轮询,游标 "cur-<seq>")、sendmessage、sendtyping、getconfig、getuploadurl
//   /c2c/upload(媒体 CDN 上传,响应头 x-encrypted-param)、/c2c/download(按 encrypted_query_param 取内容)
// 鉴权接口要求 Authorization: Bearer <token>,否则与真实平台一样返回 HTTP 200 + {"errcode":-14}。
// 响应 Content-Type 与真实平台一致是 application/octet-stream。

#include "test_support/network/crow_test_server.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace acecode::test {

class FakeWeixinServer {
public:
    struct Call {
        std::string endpoint;                       // getupdates / sendmessage / cdn_upload / get_bot_qrcode …
        std::string method;                         // GET / POST
        std::map<std::string, std::string> headers; // 关心的几个请求头
        std::map<std::string, std::string> query;   // 关心的几个查询参数(已解码)
        nlohmann::json body = nlohmann::json::object();
        std::string raw;                            // 原始请求体(CDN 上传的密文)
    };

    explicit FakeWeixinServer(std::string token = "wx-bot-token-ABCDEFGHIJKLMNOPQRSTUV")
        : token_(std::move(token)) {
        CROW_ROUTE(app_, "/ilink/bot/<string>").methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)
        ([this](const crow::request& req, const std::string& endpoint) { return handle_api(req, endpoint); });
        CROW_ROUTE(app_, "/c2c/<string>").methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)
        ([this](const crow::request& req, const std::string& endpoint) { return handle_cdn(req, endpoint); });
        server_ = std::make_unique<CrowTestServer>(app_);
    }

    ~FakeWeixinServer() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            closing_ = true;
        }
        cv_.notify_all();
        server_.reset();
    }

    std::string base() const { return server_->http_base(); }
    std::string host() const { return "127.0.0.1:" + std::to_string(server_->port()); }
    std::string cdn_base() const { return base() + "/c2c"; }
    const std::string& token() const { return token_; }

    // 可选:返回非 null 的 json 表示用它作为该接口的应答(HTTP 状态取 "_http",默认 200)。
    std::function<nlohmann::json(const Call&)> override;
    // 扫码:申请二维码与查询状态的应答;为空时用默认(qr-N / 一直 wait)。
    std::function<nlohmann::json(const Call&)> qr_handler;
    std::function<nlohmann::json(const Call&)> qr_status_handler;
    // CDN 上传前几次返回的 HTTP 状态(用来模拟 5xx / 4xx);用完后正常应答。
    std::deque<int> cdn_upload_failures;
    std::chrono::milliseconds max_hold{300};  // getupdates 没有新消息时最多挂起多久
    std::int64_t longpolling_timeout_ms = 300;

    void push_message(nlohmann::json message) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!message.contains("seq")) message["seq"] = ++next_seq_;
            else next_seq_ = std::max<std::int64_t>(next_seq_, message["seq"].get<std::int64_t>());
            messages_.push_back(std::move(message));
        }
        cv_.notify_all();
    }

    static nlohmann::json text_message(const std::string& from, const std::string& text, std::uint64_t message_id,
                                       const std::string& context_token = "ctx-1") {
        return {{"message_id", message_id},
                {"from_user_id", from},
                {"to_user_id", "e06c1ceea05e@im.bot"},
                {"create_time_ms", static_cast<std::int64_t>(1759712345000) + static_cast<std::int64_t>(message_id % 100000)},
                {"message_type", 1},
                {"message_state", 2},
                {"context_token", context_token},
                {"item_list", nlohmann::json::array({{{"type", 1}, {"text_item", {{"text", text}}}}})}};
    }

    void push_text(const std::string& from, const std::string& text, std::uint64_t message_id,
                   const std::string& context_token = "ctx-1") {
        push_message(text_message(from, text, message_id, context_token));
    }

    // CDN 下载内容(按 encrypted_query_param 查找)。
    void put_blob(const std::string& query_param, std::string bytes) {
        std::lock_guard<std::mutex> lock(mu_);
        blobs_[query_param] = std::move(bytes);
    }

    std::vector<Call> calls() {
        std::lock_guard<std::mutex> lock(mu_);
        return calls_;
    }
    std::vector<Call> calls_to(const std::string& endpoint) {
        std::vector<Call> out;
        for (const auto& call : calls())
            if (call.endpoint == endpoint) out.push_back(call);
        return out;
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
    static crow::response reply(int status, const nlohmann::json& body) {
        crow::response res(status, body.dump());
        res.set_header("Content-Type", "application/octet-stream");
        return res;
    }

    static Call describe(const crow::request& req, const std::string& endpoint) {
        Call call;
        call.endpoint = endpoint;
        call.method = req.method == crow::HTTPMethod::Post ? "POST" : "GET";
        for (const char* name : {"Authorization", "AuthorizationType", "X-WECHAT-UIN", "iLink-App-Id",
                                 "iLink-App-ClientVersion", "Content-Type"}) {
            const auto value = req.get_header_value(name);
            if (!value.empty()) call.headers[name] = value;
        }
        for (const char* name : {"qrcode", "bot_type", "encrypted_query_param", "filekey", "verify_code"}) {
            if (const char* value = req.url_params.get(name)) call.query[name] = value;
        }
        call.raw = req.body;
        if (call.method == "POST" && endpoint.rfind("cdn_", 0) != 0) {
            try {
                call.body = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
            } catch (...) {
                call.body = nlohmann::json::object();
            }
        }
        return call;
    }

    void record(const Call& call) {
        std::lock_guard<std::mutex> lock(mu_);
        calls_.push_back(call);
    }

    crow::response handle_api(const crow::request& req, const std::string& endpoint) {
        const auto call = describe(req, endpoint);
        record(call);
        // 测试注入的处理函数在锁外调用,它们可以再查询本服务的记录。
        if (endpoint == "get_bot_qrcode") {
            int count = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                count = ++qr_count_;
            }
            if (qr_handler) return reply(200, qr_handler(call));
            const auto code = "qr-" + std::to_string(count);
            return reply(200, {{"qrcode", code},
                               {"qrcode_img_content",
                                "https://liteapp.weixin.qq.com/q/7GiQu1?qrcode=" + code + "&bot_type=3"},
                               {"ret", 0}});
        }
        if (endpoint == "get_qrcode_status") {
            if (qr_status_handler) return reply(200, qr_status_handler(call));
            return reply(200, {{"ret", 0}, {"status", "wait"}});
        }
        if (call.headers.count("Authorization") == 0 || call.headers.at("Authorization") != "Bearer " + token_)
            return reply(200, {{"errcode", -14}, {"errmsg", "session timeout"}});
        if (override) {
            const auto custom = override(call);
            if (!custom.is_null()) return reply(custom.value("_http", 200), custom);
        }
        if (endpoint == "getupdates") return get_updates(call);
        if (endpoint == "sendmessage") return reply(200, {{"ret", 0}});
        if (endpoint == "sendtyping") return reply(200, {{"ret", 0}, {"errmsg", ""}});
        if (endpoint == "getconfig") return reply(200, {{"ret", 0}, {"errmsg", ""}, {"typing_ticket", "ticket-1"}});
        if (endpoint == "getuploadurl") {
            std::lock_guard<std::mutex> lock(mu_);
            return reply(200, {{"ret", 0}, {"upload_param", "up-" + std::to_string(++upload_count_)},
                               {"thumb_upload_param", ""}});
        }
        return reply(404, {{"ret", -1}, {"errmsg", "not found"}});
    }

    crow::response get_updates(const Call& call) {
        const auto cursor = call.body.value("get_updates_buf", std::string{});
        const std::int64_t after = cursor.rfind("cur-", 0) == 0 ? std::stoll(cursor.substr(4)) : 0;
        std::unique_lock<std::mutex> lock(mu_);
        auto pending = [this, after] {
            for (const auto& m : messages_)
                if (m["seq"].get<std::int64_t>() > after) return true;
            return false;
        };
        cv_.wait_for(lock, max_hold, [this, &pending] { return closing_ || pending(); });
        nlohmann::json msgs = nlohmann::json::array();
        std::int64_t last = after;
        for (const auto& m : messages_) {
            const auto seq = m["seq"].get<std::int64_t>();
            if (seq > after) {
                msgs.push_back(m);
                last = std::max(last, seq);
            }
        }
        return reply(200, {{"ret", 0},
                           {"msgs", msgs},
                           {"get_updates_buf", last > 0 ? "cur-" + std::to_string(last) : cursor},
                           {"longpolling_timeout_ms", longpolling_timeout_ms}});
    }

    crow::response handle_cdn(const crow::request& req, const std::string& name) {
        auto call = describe(req, "cdn_" + name);
        record(call);
        std::lock_guard<std::mutex> lock(mu_);
        if (name == "upload") {
            if (req.method != crow::HTTPMethod::Post) return crow::response(404);
            if (!cdn_upload_failures.empty()) {
                const int status = cdn_upload_failures.front();
                cdn_upload_failures.pop_front();
                crow::response res(status);
                res.set_header("x-error-message", "injected failure");
                return res;
            }
            const auto param = "enc-" + std::to_string(++encrypted_count_);
            blobs_[param] = req.body;
            crow::response res(200);
            res.set_header("x-encrypted-param", param);
            return res;
        }
        if (name == "download") {
            const auto it = blobs_.find(call.query["encrypted_query_param"]);
            if (it == blobs_.end()) return crow::response(404);
            crow::response res(200, it->second);
            res.set_header("Content-Type", "application/octet-stream");
            return res;
        }
        return crow::response(404);
    }

    std::string token_;
    crow::SimpleApp app_;
    std::unique_ptr<CrowTestServer> server_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool closing_ = false;
    std::int64_t next_seq_ = 0;
    int qr_count_ = 0;
    int upload_count_ = 0;
    int encrypted_count_ = 0;
    std::deque<nlohmann::json> messages_;
    std::map<std::string, std::string> blobs_;
    std::vector<Call> calls_;
};

} // namespace acecode::test
