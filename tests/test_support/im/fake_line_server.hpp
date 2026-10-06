#pragma once

// 测试用:本机假 LINE Messaging API(api.line.me 与 api-data.line.me 共用一个地址)
// 以及假 cloudflared(指标服务 /quicktunnel、/ready + 可注入的假子进程)。
// 只监听 127.0.0.1,任何测试都不连真实平台。
//
// 假 LINE 服务覆盖:无状态令牌换取、/v2/bot/info、webhook 地址读写与测试(真的向登记地址
// 发一个带签名的空事件请求)、回复(校验回复令牌只能用一次)、推送(记录重试键)、
// 正在输入动画、配额、用户资料、消息内容下载(含 202 转码、410 撤回)。

#include "im/http.hpp"
#include "im/line/line_protocol.hpp"
#include "im/line/line_tunnel.hpp"
#include "test_support/network/crow_test_server.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace acecode::test {

class FakeLineServer {
public:
    struct Call {
        std::string method;
        std::string path;
        nlohmann::json body;  // JSON 请求体;换令牌的表单字段也解析成对象
        std::string authorization;
        std::string retry_key;
    };
    struct Reply {
        int status = 200;
        nlohmann::json body = nlohmann::json::object();
        std::optional<std::string> raw;  // 非空时按二进制原样返回(消息内容下载)
    };

    static constexpr const char* kChannelId = "1650000001";
    // LINE 官方文档签名示例里的 secret。
    static constexpr const char* kSecret = "8c570fa6dd201bb328f1c1eac23a96d8";
    static constexpr const char* kBotId = "U0123456789abcdef0123456789abcdef";

    FakeLineServer() {
        CROW_CATCHALL_ROUTE(app_)([this](const crow::request& req) { return handle(req); });
        server_ = std::make_unique<CrowTestServer>(app_);
    }
    ~FakeLineServer() { server_.reset(); }

    std::string base() const { return server_->http_base(); }

    // 可选:返回值非空时用它应答(在内置处理之前调用)。测试开始前设置。
    std::function<std::optional<Reply>(const Call&)> override;

    std::string long_lived_token;           // 非空时也接受它作为 Bearer
    std::atomic<bool> webhook_active{true}; // “Use webhook” 开关
    std::string test_signing_secret = kSecret;  // webhook 测试请求用的签名密钥
    int token_expires_in = 900;

    // 发一个回复令牌(模拟 LINE 随 webhook 下发)。
    std::string issue_reply_token() {
        std::lock_guard<std::mutex> lock(mu_);
        const auto token = "rt-" + std::to_string(++next_reply_token_);
        reply_tokens_.insert(token);
        return token;
    }
    // 让当前所有已发放的无状态令牌失效(模拟平台提前作废)。
    void revoke_tokens() {
        std::lock_guard<std::mutex> lock(mu_);
        tokens_.clear();
    }
    void set_content(const std::string& message_id, std::string bytes) {
        std::lock_guard<std::mutex> lock(mu_);
        content_[message_id] = std::move(bytes);
    }
    void set_endpoint(const std::string& endpoint) {
        std::lock_guard<std::mutex> lock(mu_);
        endpoint_ = endpoint;
    }
    std::string endpoint() {
        std::lock_guard<std::mutex> lock(mu_);
        return endpoint_;
    }
    int mint_count() {
        std::lock_guard<std::mutex> lock(mu_);
        return mints_;
    }
    std::vector<Call> calls() {
        std::lock_guard<std::mutex> lock(mu_);
        return calls_;
    }
    std::vector<Call> calls_to(const std::string& method, const std::string& path) {
        std::vector<Call> out;
        for (const auto& call : calls())
            if (call.method == method && call.path == path) out.push_back(call);
        return out;
    }

    // ---- 构造 webhook ----

    static nlohmann::json source(const std::string& type, const std::string& chat, const std::string& user) {
        nlohmann::json value{{"type", type}};
        if (type == "user") value["userId"] = user;
        if (type == "group") value["groupId"] = chat;
        if (type == "room") value["roomId"] = chat;
        if (type != "user" && !user.empty()) value["userId"] = user;
        return value;
    }
    static nlohmann::json text_event(const nlohmann::json& src, const std::string& text, const std::string& message_id,
                                     const std::string& reply_token, const std::string& event_id,
                                     const nlohmann::json& extra = nlohmann::json::object()) {
        nlohmann::json message{{"id", message_id}, {"type", "text"}, {"text", text}, {"quoteToken", "q-" + message_id}};
        for (auto it = extra.begin(); it != extra.end(); ++it) message[it.key()] = it.value();
        nlohmann::json event{{"type", "message"},
                             {"mode", "active"},
                             {"timestamp", now_ms()},
                             {"source", src},
                             {"webhookEventId", event_id},
                             {"deliveryContext", {{"isRedelivery", false}}},
                             {"message", message}};
        if (!reply_token.empty()) event["replyToken"] = reply_token;
        return event;
    }
    static std::string webhook_body(const std::vector<nlohmann::json>& events, const std::string& destination = kBotId) {
        return nlohmann::json{{"destination", destination}, {"events", events}}.dump();
    }
    // 向回调端口发一个带签名的 webhook;secret 为空时不带签名头。
    static im::HttpResponse post_webhook(const std::string& url, const std::string& body,
                                         const std::string& secret = kSecret) {
        im::HttpRequest request;
        request.method = "POST";
        request.url = url;
        request.headers = {{"Content-Type", "application/json; charset=utf-8"}};
        if (!secret.empty()) request.headers.emplace_back("x-line-signature", im::line::sign_body(body, secret));
        request.body = body;
        request.timeout = std::chrono::seconds(10);
        request.use_proxy = false;
        return im::http_send(request);
    }
    static im::HttpResponse http_get(const std::string& url) {
        im::HttpRequest request;
        request.url = url;
        request.timeout = std::chrono::seconds(10);
        request.use_proxy = false;
        return im::http_send(request);
    }

    static std::int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    static bool wait_until(const std::function<bool()>& predicate,
                           std::chrono::milliseconds timeout = std::chrono::seconds(8)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return predicate();
    }

private:
    static crow::response reply(const Reply& r) {
        if (r.raw) {
            crow::response res(r.status, *r.raw);
            res.set_header("Content-Type", "image/jpeg");
            return res;
        }
        crow::response res(r.status, r.body.dump());
        res.set_header("Content-Type", "application/json");
        return res;
    }
    static Reply error(int status, const std::string& message) { return {status, {{"message", message}}, {}}; }

    static nlohmann::json parse_form(const std::string& body) {
        nlohmann::json out = nlohmann::json::object();
        std::size_t start = 0;
        while (start < body.size()) {
            auto end = body.find('&', start);
            if (end == std::string::npos) end = body.size();
            const auto pair = body.substr(start, end - start);
            const auto eq = pair.find('=');
            if (eq != std::string::npos) out[pair.substr(0, eq)] = decode(pair.substr(eq + 1));
            start = end + 1;
        }
        return out;
    }
    static std::string decode(const std::string& value) {
        std::string out;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                out.push_back(static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(value[i] == '+' ? ' ' : value[i]);
            }
        }
        return out;
    }

    bool authorized(const std::string& header) {
        const std::string prefix = "Bearer ";
        if (header.compare(0, prefix.size(), prefix) != 0) return false;
        const auto token = header.substr(prefix.size());
        std::lock_guard<std::mutex> lock(mu_);
        return tokens_.count(token) > 0 || (!long_lived_token.empty() && token == long_lived_token);
    }

    nlohmann::json sent_messages(std::size_t count) {
        nlohmann::json sent = nlohmann::json::array();
        std::lock_guard<std::mutex> lock(mu_);
        for (std::size_t i = 0; i < count; ++i) {
            const auto id = std::to_string(9000 + ++next_message_);
            sent.push_back({{"id", id}, {"quoteToken", "sq-" + id}});
        }
        return sent;
    }

    crow::response handle(const crow::request& req) {
        Call call;
        call.method = crow::method_name(req.method);
        call.path = req.url.substr(0, req.url.find('?'));
        call.authorization = req.get_header_value("Authorization");
        call.retry_key = req.get_header_value("X-Line-Retry-Key");
        if (call.path == "/oauth2/v3/token") {
            call.body = parse_form(req.body);
        } else {
            try {
                call.body = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
            } catch (...) {
                call.body = nlohmann::json::object();
            }
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            calls_.push_back(call);
        }
        if (override) {
            if (const auto custom = override(call)) return reply(*custom);
        }
        return reply(builtin(call));
    }

    Reply builtin(const Call& call) {
        const auto& path = call.path;
        if (path == "/oauth2/v3/token" && call.method == "POST") {
            if (call.body.value("grant_type", "") != "client_credentials" ||
                call.body.value("client_id", "") != kChannelId || call.body.value("client_secret", "") != kSecret)
                return {400, {{"error", "invalid_client"}, {"error_description", "Invalid 'client_credentials'."}}};
            std::lock_guard<std::mutex> lock(mu_);
            const auto token = "stateless-" + std::to_string(++mints_);
            tokens_.insert(token);
            return {200, {{"token_type", "Bearer"}, {"access_token", token}, {"expires_in", token_expires_in}}};
        }
        if (!authorized(call.authorization)) return error(401, "Authentication failed. Confirm that the access token in the authorization header is valid.");
        if (path == "/v2/bot/info") {
            return {200, {{"userId", kBotId}, {"basicId", "@216ruabc"}, {"displayName", "ACE 测试机器人"},
                          {"chatMode", "bot"}, {"markAsReadMode", "auto"}}};
        }
        if (path == "/v2/bot/channel/webhook/endpoint") {
            std::lock_guard<std::mutex> lock(mu_);
            if (call.method == "PUT") {
                endpoint_ = call.body.value("endpoint", std::string{});
                return {200, nlohmann::json::object()};
            }
            if (endpoint_.empty()) return error(404, "Webhook endpoint not found");
            return {200, {{"endpoint", endpoint_}, {"active", webhook_active.load()}}};
        }
        if (path == "/v2/bot/channel/webhook/test") {
            // 和 LINE 一样:向目标地址发一个带签名的空事件请求,把结果报回来。
            const auto target = call.body.value("endpoint", endpoint());
            const auto body = webhook_body({});
            const auto response = post_webhook(target, body, test_signing_secret);
            const bool ok = response.status == 200;
            return {200, {{"success", ok}, {"timestamp", "2026-10-06T00:00:00Z"}, {"statusCode", response.status},
                          {"reason", ok ? "OK" : (response.status == 0 ? "COULD_NOT_CONNECT" : "ERROR_STATUS_CODE")},
                          {"detail", std::to_string(response.status)}}};
        }
        if (path == "/v2/bot/message/reply") {
            const auto token = call.body.value("replyToken", std::string{});
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (!reply_tokens_.count(token) || used_reply_tokens_.count(token)) return error(400, "Invalid reply token");
                used_reply_tokens_.insert(token);
            }
            return {200, {{"sentMessages", sent_messages(call.body["messages"].size())}}};
        }
        if (path == "/v2/bot/message/push") {
            return {200, {{"sentMessages", sent_messages(call.body["messages"].size())}}};
        }
        if (path == "/v2/bot/chat/loading/start") return {202, nlohmann::json::object()};
        if (path == "/v2/bot/message/quota") return {200, {{"type", "limited"}, {"value", 200}}};
        if (path == "/v2/bot/message/quota/consumption") return {200, {{"totalUsage", 3}}};
        if (path.rfind("/v2/bot/profile/", 0) == 0 || path.find("/member/") != std::string::npos)
            return {200, {{"displayName", "Ann"}, {"userId", path.substr(path.rfind('/') + 1)}}};
        const std::string content_prefix = "/v2/bot/message/";
        if (path.rfind(content_prefix, 0) == 0) {
            const auto rest = path.substr(content_prefix.size());
            const auto slash = rest.find('/');
            const auto id = rest.substr(0, slash);
            const auto tail = slash == std::string::npos ? std::string{} : rest.substr(slash);
            if (tail == "/content/transcoding") return {200, {{"status", "succeeded"}}};
            if (tail == "/content") return content(id);
        }
        return error(404, "Not found");
    }

    Reply content(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu_);
        if (id == "gone") return error(410, "The content is gone");
        if (id.rfind("transcode", 0) == 0 && !transcoded_.count(id)) {
            // 第一次请求:还在转码(202,无正文)。
            transcoded_.insert(id);
            Reply pending;
            pending.status = 202;
            pending.raw = std::string{};
            return pending;
        }
        const auto it = content_.find(id);
        if (it == content_.end()) return error(404, "not found");
        Reply ok;
        ok.raw = it->second;
        return ok;
    }

    crow::SimpleApp app_;
    std::unique_ptr<CrowTestServer> server_;
    std::mutex mu_;
    std::vector<Call> calls_;
    std::set<std::string> tokens_;
    std::set<std::string> reply_tokens_;
    std::set<std::string> used_reply_tokens_;
    std::set<std::string> transcoded_;
    std::map<std::string, std::string> content_;
    std::string endpoint_;
    int mints_ = 0;
    int next_reply_token_ = 0;
    int next_message_ = 0;
};

// 假 cloudflared:指标服务 + 假子进程。launcher() 记录参数,按 --url 里的端口给出主机名
// (127.0.0.1:<回调端口>,配合 url_scheme = "http" 让公网自检直达回调端口)。
class FakeCloudflared {
public:
    struct State {
        std::mutex mu;
        std::vector<std::vector<std::string>> launches;
        std::string hostname;
        std::shared_ptr<std::atomic<bool>> current;  // 当前进程是否存活
        std::atomic<bool> ready{true};
        std::atomic<bool> fail_launch{false};
        std::function<std::string(int launch, const std::string& target)> hostname_for;  // 可选
    };

    class Process final : public im::line::TunnelProcess {
    public:
        explicit Process(std::shared_ptr<std::atomic<bool>> alive) : alive_(std::move(alive)) {}
        bool alive() override { return alive_->load(); }
        void stop() override { alive_->store(false); }

    private:
        std::shared_ptr<std::atomic<bool>> alive_;
    };

    FakeCloudflared() {
        CROW_ROUTE(app_, "/quicktunnel")([state = state_] {
            std::lock_guard<std::mutex> lock(state->mu);
            return crow::response(200, nlohmann::json{{"hostname", state->hostname}}.dump());
        });
        CROW_ROUTE(app_, "/ready")([state = state_] {
            std::lock_guard<std::mutex> lock(state->mu);
            const bool up = state->ready.load() && state->current && state->current->load();
            return crow::response(up ? 200 : 503, nlohmann::json{{"status", up ? 200 : 503}}.dump());
        });
        server_ = std::make_unique<CrowTestServer>(app_);
    }

    std::uint16_t metrics_port() const { return server_->port(); }
    std::shared_ptr<State> state() const { return state_; }

    im::line::TunnelLauncher launcher() const {
        return [state = state_](const std::vector<std::string>& argv, std::string* error)
                   -> std::unique_ptr<im::line::TunnelProcess> {
            if (state->fail_launch.load()) {
                if (error) *error = "fake launch failure";
                return nullptr;
            }
            std::string target;
            for (std::size_t i = 0; i + 1 < argv.size(); ++i)
                if (argv[i] == "--url") target = argv[i + 1];
            const auto colon = target.rfind(':');
            const auto port = colon == std::string::npos ? std::string{} : target.substr(colon + 1);
            auto alive = std::make_shared<std::atomic<bool>>(true);
            std::lock_guard<std::mutex> lock(state->mu);
            state->launches.push_back(argv);
            const int n = static_cast<int>(state->launches.size());
            state->hostname = state->hostname_for ? state->hostname_for(n, port) : "127.0.0.1:" + port;
            state->current = alive;
            return std::make_unique<Process>(alive);
        };
    }

    // 模拟 cloudflared 进程意外退出。
    void kill_current() {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (state_->current) state_->current->store(false);
    }
    std::size_t launch_count() {
        std::lock_guard<std::mutex> lock(state_->mu);
        return state_->launches.size();
    }
    std::vector<std::string> last_argv() {
        std::lock_guard<std::mutex> lock(state_->mu);
        return state_->launches.empty() ? std::vector<std::string>{} : state_->launches.back();
    }

private:
    std::shared_ptr<State> state_ = std::make_shared<State>();
    crow::SimpleApp app_;
    std::unique_ptr<CrowTestServer> server_;
};

} // namespace acecode::test
