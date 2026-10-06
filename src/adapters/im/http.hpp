#pragma once

// IM 传输层共用的 HTTP 调用(cpr)。代理与 TLS 选项与仓库其它 cpr 调用点同源
// (ProxyResolver + NoRevoke);测试连本机假服务时可关闭代理。

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace acecode::im {

struct HttpPart {
    std::string name;
    std::string value;              // 普通字段的值
    std::filesystem::path file;     // 非空时作为文件字段上传(整读进内存)
    std::string filename;           // 文件字段对外显示的文件名
    std::string content_type;
};

struct HttpRequest {
    std::string method = "GET";     // GET / POST / PUT / PATCH / DELETE
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;               // 原始请求体(JSON)
    std::vector<HttpPart> parts;    // 非空时按 multipart/form-data 发送
    std::chrono::milliseconds timeout{std::chrono::seconds(30)};
    bool use_proxy = true;
    std::filesystem::path download_to;   // 非空时把响应体写入该文件
    std::uint64_t max_download_bytes = 0;  // 0 = 不限
    // 返回 true 时中止请求(长轮询停机用);libcurl 大约每秒检查一次。
    std::function<bool()> cancel;
};

struct HttpResponse {
    long status = 0;        // 0 = 没拿到 HTTP 响应(连接失败、超时、被取消)
    std::string body;
    std::map<std::string, std::string> headers;  // 响应头,键为小写(如微信 CDN 的 x-encrypted-param)
    std::string error;      // 传输层错误;已脱敏
    bool too_large = false; // 下载超过 max_download_bytes,文件已删除
    bool cancelled = false; // 被 HttpRequest::cancel 中止
};

HttpResponse http_send(const HttpRequest& request);

// "ACECode/<版本>",所有 IM 平台请求都带上,方便平台识别来源。
std::string user_agent();

} // namespace acecode::im
