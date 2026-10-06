#include "qq_bind.hpp"

#include "im/http.hpp"
#include "platform/crypto/aes_gcm.hpp"
#include "platform/crypto/secure_random.hpp"
#include "utils/base64.hpp"

#include <nlohmann/json.hpp>

#include <thread>

namespace acecode::im::qqbot {
namespace {

using Clock = std::chrono::steady_clock;

std::string url_encode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') ||
            u == '-' || u == '_' || u == '.' || u == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0x0F]);
        }
    }
    return out;
}

std::string trim_slash(std::string base) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

// 等待 duration;期间每 100ms 检查一次取消。返回 false 表示被取消。
bool wait_or_cancel(std::chrono::milliseconds duration, const std::atomic<bool>& cancelled) {
    const auto until = Clock::now() + duration;
    while (Clock::now() < until) {
        if (cancelled) return false;
        std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(
            std::chrono::milliseconds(100),
            std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now())));
    }
    return !cancelled;
}

struct PortalReply {
    bool ok = false;
    nlohmann::json data;
    std::string error;
};

PortalReply post_portal(const BindOptions& options, const std::string& path, const nlohmann::json& body) {
    HttpRequest request;
    request.method = "POST";
    request.url = trim_slash(options.portal_base) + path;
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body = body.dump();
    request.timeout = options.request_timeout;
    request.use_proxy = options.use_proxy;
    const auto response = http_send(request);
    PortalReply reply;
    if (response.status == 0) {
        reply.error = response.error.empty() ? "无法连接 QQ 开放平台" : response.error;
        return reply;
    }
    if (response.status != 200) {
        reply.error = "QQ 开放平台返回 HTTP " + std::to_string(response.status);
        return reply;
    }
    try {
        const auto json = nlohmann::json::parse(response.body);
        if (!json.is_object() || json.value("retcode", -1) != 0) {
            reply.error = json.is_object() ? json.value("msg", std::string("QQ 开放平台拒绝了请求"))
                                           : std::string("QQ 开放平台返回了无效数据");
            return reply;
        }
        reply.ok = true;
        reply.data = json.contains("data") && json["data"].is_object() ? json["data"] : nlohmann::json::object();
    } catch (...) {
        reply.error = "QQ 开放平台返回了无效数据";
    }
    return reply;
}

std::string id_field(const nlohmann::json& data, const char* key) {
    if (!data.contains(key)) return {};
    const auto& field = data.at(key);
    if (field.is_string()) return field.get<std::string>();
    if (field.is_number_integer()) return std::to_string(field.get<std::int64_t>());
    return {};
}

} // namespace

std::string build_connect_url(const std::string& base, const std::string& task_id,
                              const std::string& source) {
    return trim_slash(base) + "/qqbot/openclaw/connect.html?task_id=" + url_encode(task_id) +
           "&source=" + url_encode(source) + "&_wv=2";
}

bool decrypt_bind_secret(const std::string& encrypted_base64, const std::string& key_base64,
                         std::string& secret, std::string* error) {
    const auto key = base64_decode(key_base64);
    const auto raw = base64_decode(encrypted_base64);
    if (!key || !raw || raw->size() < 12 + 16) {
        if (error) *error = "扫码返回的密钥格式无效";
        return false;
    }
    const std::string iv = raw->substr(0, 12);
    const std::string tag = raw->substr(raw->size() - 16);
    const std::string ciphertext = raw->substr(12, raw->size() - 12 - 16);
    std::string detail;
    if (!platform::aes_256_gcm_decrypt(*key, iv, ciphertext, tag, secret, &detail)) {
        if (error) *error = "扫码返回的密钥无法解密";
        return false;
    }
    return true;
}

BindUpdate run_bind(const BindOptions& options, const std::atomic<bool>& cancelled,
                    const BindProgress& progress) {
    const auto deadline = Clock::now() + options.total_timeout;
    BindUpdate update;
    int refreshes = 0;
    while (true) {
        if (cancelled) return BindUpdate{BindPhase::Cancelled};
        if (Clock::now() >= deadline) return BindUpdate{BindPhase::TimedOut};

        const auto key_bytes = options.key_provider ? options.key_provider() : platform::secure_random_bytes(32);
        if (key_bytes.size() != 32) {
            update.phase = BindPhase::Failed;
            update.error = "无法生成随机密钥";
            return update;
        }
        const auto key = base64_encode(key_bytes);
        const auto created = post_portal(options, "/lite/create_bind_task", {{"key", key}});
        if (!created.ok) {
            update.phase = BindPhase::Failed;
            update.error = "申请绑定任务失败:" + created.error;
            return update;
        }
        const auto task_id = id_field(created.data, "task_id");
        if (task_id.empty()) {
            update.phase = BindPhase::Failed;
            update.error = "申请绑定任务失败:缺少 task_id";
            return update;
        }

        BindUpdate waiting;
        waiting.phase = BindPhase::WaitingScan;
        waiting.qr_url = build_connect_url(options.connect_base, task_id, options.source);
        waiting.refreshes = refreshes;
        if (progress) progress(waiting);

        bool expired = false;
        while (!expired) {
            if (!wait_or_cancel(options.poll_interval, cancelled)) return BindUpdate{BindPhase::Cancelled};
            if (Clock::now() >= deadline) return BindUpdate{BindPhase::TimedOut};
            // 轮询失败(网络抖动、平台临时错误)继续等,与官方 connector 的行为一致。
            const auto polled = post_portal(options, "/lite/poll_bind_result", {{"task_id", task_id}});
            if (!polled.ok) continue;
            const int status = polled.data.value("status", 0);
            if (status == 2) {
                BindUpdate done;
                done.app_id = id_field(polled.data, "bot_appid");
                const auto encrypted = polled.data.value("bot_encrypt_secret", std::string{});
                std::string error;
                if (done.app_id.empty() || !decrypt_bind_secret(encrypted, key, done.app_secret, &error)) {
                    done.phase = BindPhase::Failed;
                    done.error = error.empty() ? "扫码结果缺少机器人信息" : error;
                    done.app_secret.clear();
                    return done;
                }
                done.phase = BindPhase::Completed;
                done.user_openid = id_field(polled.data, "user_openid");
                done.refreshes = refreshes;
                return done;
            }
            if (status == 3) expired = true;
        }
        ++refreshes;
    }
}

} // namespace acecode::im::qqbot
