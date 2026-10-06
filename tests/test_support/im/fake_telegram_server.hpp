#pragma once

// 测试用:本机假 Telegram Bot API(/bot<token>/<method> 与 /file/bot<token>/<dir>/<name>)。
// getUpdates 支持长轮询等待;各接口可注入失败(409、429、HTML 解析失败等)。

#include "test_support/network/crow_test_server.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace acecode::test {

class FakeTelegramServer {
public:
    struct Call {
        std::string method;
        nlohmann::json body;                       // JSON 请求体
        std::vector<std::string> part_names;       // multipart 字段名
        std::string file_name;                     // multipart 文件字段的文件名
        std::string file_body;                     // multipart 文件内容
    };

    explicit FakeTelegramServer(std::string token = "123456:TEST-TOKEN-abcdefghijklmnopqrstuvwxyz")
        : token_(std::move(token)) {
        CROW_ROUTE(app_, "/<string>/<string>").methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
        ([this](const crow::request& req, const std::string& bot, const std::string& method) {
            if (bot != "bot" + token_) return reply(401, {{"ok", false}, {"error_code", 401}, {"description", "Unauthorized"}});
            return handle(req, method);
        });
        CROW_ROUTE(app_, "/file/<string>/<string>/<string>")
        ([this](const std::string& bot, const std::string& dir, const std::string& name) {
            if (bot != "bot" + token_) return crow::response(401);
            record({"download:" + dir + "/" + name, nlohmann::json::object(), {}, {}, {}});
            return crow::response(200, std::string(name == "big.bin" ? 4096 : 32, 'd'));
        });
        server_ = std::make_unique<CrowTestServer>(app_);
    }

    ~FakeTelegramServer() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            closing_ = true;
        }
        cv_.notify_all();
        server_.reset();
    }

    std::string base() const { return server_->http_base(); }
    const std::string& token() const { return token_; }

    // 可选:返回非空 json 表示用它作为该方法的应答(状态码取 error_code,默认 200)。
    std::function<nlohmann::json(const Call&)> override;
    bool can_read_all_group_messages = false;

    void push_update(nlohmann::json update) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            update["update_id"] = next_update_id_++;
            updates_.push_back(std::move(update));
        }
        cv_.notify_all();
    }
    void push_private_text(std::int64_t user_id, const std::string& text, std::int64_t message_id) {
        push_update({{"message", {{"message_id", message_id},
                                  {"from", {{"id", user_id}, {"is_bot", false}, {"first_name", "Ann"}}},
                                  {"chat", {{"id", user_id}, {"type", "private"}}},
                                  {"text", text}}}});
    }

    std::vector<Call> calls() {
        std::lock_guard<std::mutex> lock(mu_);
        return calls_;
    }
    std::vector<Call> calls_to(const std::string& method) {
        std::vector<Call> out;
        for (const auto& c : calls()) if (c.method == method) out.push_back(c);
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
        res.set_header("Content-Type", "application/json");
        return res;
    }
    static nlohmann::json ok(const nlohmann::json& result) { return {{"ok", true}, {"result", result}}; }

    void record(const Call& call) {
        std::lock_guard<std::mutex> lock(mu_);
        calls_.push_back(call);
    }

    crow::response handle(const crow::request& req, const std::string& method) {
        Call call;
        call.method = method;
        const auto type = req.get_header_value("Content-Type");
        if (type.find("multipart/form-data") != std::string::npos) {
            crow::multipart::message message(req);
            for (const auto& part : message.parts) {
                const auto disposition = part.get_header_object("Content-Disposition");
                const auto name = disposition.params.count("name") ? disposition.params.at("name") : std::string{};
                call.part_names.push_back(name);
                if (disposition.params.count("filename")) {
                    call.file_name = disposition.params.at("filename");
                    call.file_body = part.body;
                } else {
                    call.body[name] = part.body;
                }
            }
        } else {
            try {
                call.body = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
            } catch (...) {
                call.body = nlohmann::json::object();
            }
        }
        if (method != "getUpdates") record(call);
        if (override) {
            const auto custom = override(call);
            if (!custom.is_null()) return reply(custom.value("error_code", 200), custom);
        }
        if (method == "getMe") {
            return reply(200, ok({{"id", 8001}, {"is_bot", true}, {"username", "AceTestBot"},
                                  {"can_read_all_group_messages", can_read_all_group_messages}}));
        }
        if (method == "getUpdates") {
            record(call);
            const auto offset = call.body.value("offset", std::int64_t{0});
            const auto timeout = std::min<int>(call.body.value("timeout", 0), 2);
            std::unique_lock<std::mutex> lock(mu_);
            auto pending = [&] {
                for (const auto& u : updates_) if (u["update_id"].get<std::int64_t>() >= offset) return true;
                return false;
            };
            cv_.wait_for(lock, std::chrono::seconds(timeout), [&] { return closing_ || pending(); });
            nlohmann::json result = nlohmann::json::array();
            for (const auto& u : updates_) if (u["update_id"].get<std::int64_t>() >= offset) result.push_back(u);
            return reply(200, ok(result));
        }
        if (method == "sendMessage") {
            return reply(200, ok({{"message_id", 900 + static_cast<int>(calls_to("sendMessage").size())}}));
        }
        if (method == "sendChatAction" || method == "deleteWebhook") return reply(200, ok(true));
        if (method == "getFile") {
            const auto id = call.body.value("file_id", std::string{});
            if (id == "too-big") return reply(200, ok({{"file_id", id}, {"file_size", 30 * 1024 * 1024}}));
            return reply(200, ok({{"file_id", id}, {"file_path", "documents/" + id + ".bin"}, {"file_size", 32}}));
        }
        if (method == "sendDocument" || method == "sendPhoto") return reply(200, ok({{"message_id", 950}}));
        return reply(404, {{"ok", false}, {"error_code", 404}, {"description", "Not Found: method not found"}});
    }

    std::string token_;
    crow::SimpleApp app_;
    std::unique_ptr<CrowTestServer> server_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool closing_ = false;
    std::int64_t next_update_id_ = 100;
    std::deque<nlohmann::json> updates_;
    std::vector<Call> calls_;
};

} // namespace acecode::test
