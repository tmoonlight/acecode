#pragma once

// 飞书 / Lark OpenAPI 客户端:tenant_access_token 缓存、机器人信息、长连接地址、
// 发消息 / 回复、图片与文件上传、消息资源下载。
// 线程安全:令牌刷新在锁内串行(单飞),其余调用可以并发。
// App Secret、令牌、长连接地址(带票据)都不会出现在返回的错误文本里。

#include "im/feishu/feishu_protocol.hpp"
#include "im/http.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace acecode::im::feishu {

struct ApiOptions {
    std::string app_id;
    std::string app_secret;
    std::string base = kFeishuBase;  // base_for_domain("feishu" / "lark")
    bool use_proxy = true;
};

// 所有请求的 User-Agent:"ACECode/<版本> source/acecode channel"。末尾独立的 channel 标记
// 照官方渠道 SDK 发送(hermes #50656 称缺它时长连接收不到群 @ 事件;未证实,但发送无害)。
std::string feishu_user_agent();

class Api {
public:
    explicit Api(ApiOptions options);

    // 有效的 tenant_access_token;距到期不足 10 分钟(或剩余寿命一半)时刷新。
    // 失败返回空串,error 为中文原因(不含密钥)。cancel 置位时尽快放弃。
    std::string tenant_token(std::string* error, const std::atomic<bool>* cancel = nullptr);
    void invalidate_token();
    // 最近一次换令牌失败是否因为凭据被拒(这种错误不会自己好,不应继续重连)。
    bool last_token_auth_failed() const;

    // 带令牌的 JSON 调用(method 为 GET / POST;body 为 null 时不发请求体)。
    // 令牌失效类错误码(99991661/3/4/5)自动换新令牌并重试一次。
    ApiResult call(const std::string& method, const std::string& path, const nlohmann::json& body = nullptr,
                   const std::atomic<bool>* cancel = nullptr,
                   std::chrono::milliseconds timeout = std::chrono::seconds(30));
    // multipart 上传(图片 / 文件),令牌处理同 call。
    ApiResult upload(const std::string& path, const std::vector<HttpPart>& parts);

    // GET /open-apis/bot/v3/info。失败时 error 为中文原因。
    BotInfo bot_info(std::string* error, const std::atomic<bool>* cancel = nullptr);
    // POST /callback/ws/endpoint(用 App ID / App Secret 鉴权,不用令牌)。
    EndpointInfo ws_endpoint(const std::atomic<bool>* cancel = nullptr);

    // GET /open-apis/im/v1/messages/{message_id}/resources/{key}?type=image|file 下载到 dest。
    // 超过 max_bytes(0 = 不限)失败并删除半截文件;失败时 error 为中文原因。
    bool download_resource(const ResourceRef& ref, const std::filesystem::path& dest, std::uint64_t max_bytes,
                           std::string* error);

    const ApiOptions& options() const { return options_; }

private:
    std::string url(const std::string& path) const;
    std::string fetch_token_locked(std::string* error, const std::atomic<bool>* cancel);
    std::string redact(const std::string& text) const;

    ApiOptions options_;
    mutable std::mutex mu_;
    std::string token_;
    std::chrono::steady_clock::time_point refresh_at_{};
    bool auth_failed_ = false;
};

// 保存凭据前的联网校验:换 tenant_access_token,再取机器人信息。
struct VerifyResult {
    bool ok = false;             // 凭据有效(换到了令牌)
    bool auth_failed = false;    // 凭据被拒:App ID / App Secret 不对,或飞书 / Lark 选错
    bool network_error = false;  // 网络不通或平台暂时不可用,无法判断凭据
    std::string error;           // 失败时给用户看的中文原因(不含密钥)
    std::string bot_open_id;     // 机器人自己的 open_id(非密钥)
    std::string bot_name;        // 机器人名称(应用名)
    int activate_status = -1;    // bot/v3/info 的 activate_status;-1 = 没取到
    bool bot_ready = false;      // activate_status == 2
    // 凭据有效但机器人还没就绪(未添加机器人能力 / 未发布版本 / 被停用)时的提示。
    // 长连接本身不依赖它:飞书后台要求先有在线的长连接才能保存“使用长连接接收事件”,
    // 而发布版本往往在那之后,所以这里只提示、不判失败。
    std::string bot_warning;
};

VerifyResult verify_credentials(const ApiOptions& options);

} // namespace acecode::im::feishu
